#include "match_store.h"

#include <new>

#include <algorithm>
#include <bitset>

namespace football {
namespace {

// Marks an event added since beginEvents() until endEvents().
constexpr uint8_t kPendingSlot = kNoIndex - 1;
static_assert(cfg::kStoreOwnMatches < kPendingSlot);

// Distance of a kickoff from 'utc'; an unknown kickoff counts as farthest.
uint64_t distance(const StoredMatch& m, int64_t utc) {
  if (m.kickoff == 0) return UINT64_MAX;
  const int64_t d = static_cast<int64_t>(m.kickoff) - utc;
  return static_cast<uint64_t>(d < 0 ? -d : d);
}

// Sort key: kickoff, unknown kickoffs last.
uint32_t sortKey(const StoredMatch& m) {
  return m.kickoff == 0 ? UINT32_MAX : m.kickoff;
}

bool goal(EventType t) {
  return t == EventType::kGoal || t == EventType::kOwnGoal ||
         t == EventType::kPenaltyGoal;
}

Side mirror(Side s) {
  return s == Side::kHome   ? Side::kAway
         : s == Side::kAway ? Side::kHome
                            : s;
}

// Whether event 'e' is shown next to the events 'list' of its slot (see
// MatchStore::eventShown()).
template <class Pool>
bool shownAmong(const StoredEvent& e, const Pool& pool, const uint8_t* list,
                std::size_t count) {
  if (!(e.flags & kEventFallback)) return true;
  const bool scored = goal(e.type);
  const bool missed = e.type == EventType::kPenaltyMissed;
  if (!scored && !missed) return true;
  for (std::size_t n = 0; n < count; ++n) {
    const auto& p = pool[list[n]];
    if (p.flags & (kEventFallback | kEventRemoved)) continue;
    if ((scored && goal(p.type)) ||
        (missed && p.type == EventType::kPenaltyMissed))
      return false;
  }
  return true;
}

// The five scores with their fallback bits.
struct ScoreField {
  Goals StoredMatch::*goals;
  uint8_t bit;
};
constexpr ScoreField kScores[] = {
    {&StoredMatch::principal, kFallbackPrincipal},
    {&StoredMatch::halftime, kFallbackHalftime},
    {&StoredMatch::regulation, kFallbackRegulation},
    {&StoredMatch::extra_time, kFallbackExtraTime},
    {&StoredMatch::shootout, kFallbackShootout},
};

// A phase that stops the clock drops an estimated minute.
void settle(StoredMatch& m) {
  if (!running(m.phase) && (m.estimated & kEstimatedMinute)) {
    m.minute = kNoValue;
    m.estimated &= ~kEstimatedMinute;
  }
}

// Keeps the values 'prior' took from its fallback source where 'm' lacks
// them or only estimated them.
void keepFallback(StoredMatch& m, const StoredMatch& prior) {
  const uint8_t bits = prior.fallback;
  m.fallback = 0;
  if ((bits & kFallbackKickoff) && m.kickoff == 0) {
    m.kickoff = prior.kickoff;
    m.fallback |= kFallbackKickoff;
  }
  if ((bits & kFallbackPhase) &&
      (m.phase == Phase::kUnknown || (m.estimated & kEstimatedPhase))) {
    m.phase = prior.phase;
    m.estimated &= ~kEstimatedPhase;
    m.fallback |= kFallbackPhase;
    settle(m);
  }
  if ((bits & kFallbackMinute) &&
      (m.minute == kNoValue || (m.estimated & kEstimatedMinute))) {
    m.minute = prior.minute;
    m.estimated &= ~kEstimatedMinute;
    m.fallback |= kFallbackMinute;
  }
  for (const auto& f : kScores)
    if ((bits & f.bit) && !(m.*f.goals).valid()) {
      m.*f.goals = prior.*f.goals;
      m.fallback |= f.bit;
    }
}

// A match first seen finished gets 'clock.utc' as estimated final whistle,
// or the expected end when it was never seen running.
void markFinished(StoredMatch& m, bool seen_running, const Clock& clock) {
  if (m.phase != Phase::kFinished || m.finished_at != 0) return;
  int64_t t = clock.utc;
  if (!seen_running && m.kickoff != 0)
    t = std::min<int64_t>(t, m.kickoff + cfg::kExpectedMatchMinutes * 60);
  if (t > 0 && t <= UINT32_MAX) {
    m.finished_at = static_cast<uint32_t>(t);
    m.estimated |= kEstimatedFinish;
  }
}

int64_t localDay(int64_t utc, int32_t offset) {
  const int64_t t = utc + offset;
  return t >= 0 ? t / 86400 : (t - 86399) / 86400;
}

}  // namespace

bool running(Phase p) {
  return p == Phase::kLive || p == Phase::kHalfTime ||
         p == Phase::kExtraTime || p == Phase::kPenalties;
}

void estimate(StoredMatch& m, int64_t utc) {
  if (m.kickoff == 0) return;
  if (m.phase != Phase::kUnknown && !(m.estimated & kEstimatedPhase)) return;
  m.estimated |= kEstimatedPhase;
  m.estimated &= ~kEstimatedMinute;
  m.minute = kNoValue;
  if (utc < m.kickoff) {
    m.phase = Phase::kScheduled;
    return;
  }
  const int64_t elapsed = (utc - m.kickoff) / 60;
  if (elapsed > cfg::kUnknownMatchEndMinutes) {
    m.phase = Phase::kUnknown;
  } else if (elapsed > 45 && elapsed <= 60) {
    // Assumed 15-minute break after the first half.
    m.phase = Phase::kHalfTime;
  } else {
    m.phase = Phase::kLive;
    m.minute = static_cast<uint8_t>(
        std::min<int64_t>(elapsed > 60 ? elapsed - 15 : elapsed, 120));
    m.estimated |= kEstimatedMinute;
  }
}

bool matchdayPassed(uint32_t last_kickoff, int64_t utc,
                    int32_t local_offset_s) {
  return localDay(utc, local_offset_s) >=
         localDay(last_kickoff, local_offset_s) + 2;
}

MatchStore::MatchStore() = default;

// Rebuilt in place: a temporary store would need 22 KB of stack.
void MatchStore::clear() {
  this->~MatchStore();
  new (this) MatchStore();
}

uint8_t MatchStore::team(uint8_t competition, cfg::Provider provider,
                         uint32_t id, std::string_view short_name) {
  if (id == 0) return kNoIndex;
  Text<26> name;
  name.assign(short_name);
  int free = -1;
  for (std::size_t i = 0; i < teams_.size(); ++i) {
    auto& t = teams_[i];
    if (t.id == id && t.provider == provider) {
      if (!name.empty()) t.short_name = name;
      return static_cast<uint8_t>(i);
    }
    if (t.id == 0 && free < 0) free = static_cast<int>(i);
  }
  if (free < 0) {
    note(competition, kNoticeTeams);
    return kNoIndex;
  }
  teams_[free] = {id, provider, name};
  return static_cast<uint8_t>(free);
}

bool MatchStore::rename(cfg::Provider provider, uint32_t id,
                        std::string_view short_name) {
  Text<26> name;
  name.assign(short_name);
  if (id == 0 || name.empty()) return false;
  for (auto& t : teams_)
    if (t.id == id && t.provider == provider) {
      t.short_name = name;
      return true;
    }
  return false;
}

uint8_t MatchStore::label(std::string_view text) {
  Text<31> value;
  value.assign(text);
  if (value.empty()) return kNoIndex;
  int free = -1;
  for (std::size_t i = 0; i < labels_.size(); ++i) {
    if (labels_[i] == value) return static_cast<uint8_t>(i);
    if (labels_[i].empty() && free < 0) free = static_cast<int>(i);
  }
  if (free < 0) return kNoIndex;
  labels_[free] = value;
  return static_cast<uint8_t>(free);
}

int MatchStore::matchIndex(uint8_t competition, cfg::Provider provider,
                           uint32_t id) const {
  for (std::size_t i = 0; i < match_count_; ++i) {
    const auto& m = matches_[i];
    if (m.id == id && m.competition == competition && m.provider == provider)
      return static_cast<int>(i);
  }
  return -1;
}

const StoredMatch* MatchStore::find(uint8_t competition,
                                    cfg::Provider provider,
                                    uint32_t id) const {
  const int i = matchIndex(competition, provider, id);
  return i < 0 ? nullptr : &matches_[i];
}

void MatchStore::freeSlot(uint8_t slot) {
  if (slot >= own_.size()) return;
  for (auto& e : events_)
    if (e.slot == slot) e = {};
  own_[slot] = {};
}

void MatchStore::removeMatch(std::size_t i) {
  freeSlot(matches_[i].own);
  for (std::size_t j = i + 1; j < match_count_; ++j)
    matches_[j - 1] = matches_[j];
  matches_[--match_count_] = {};
}

std::size_t MatchStore::sortedPosition(const StoredMatch& m) const {
  std::size_t at = match_count_;
  while (at > 0 && sortKey(matches_[at - 1]) > sortKey(m)) --at;
  return at;
}

uint8_t MatchStore::ownSlot(const StoredMatch& m, int64_t utc) {
  for (std::size_t i = 0; i < own_.size(); ++i)
    if (own_[i].match == 0) return static_cast<uint8_t>(i);
  // Full: take the slot of the own match farthest from now when 'm' is
  // nearer; a slot whose match is gone is free.
  int victim = -1, victim_row = -1;
  uint64_t farthest = 0;
  for (std::size_t i = 0; i < own_.size(); ++i) {
    int row = -1;
    for (std::size_t j = 0; j < match_count_ && row < 0; ++j)
      if (matches_[j].own == i) row = static_cast<int>(j);
    if (row < 0) {
      freeSlot(static_cast<uint8_t>(i));
      return static_cast<uint8_t>(i);
    }
    const auto d = distance(matches_[row], utc);
    if (victim < 0 || d > farthest) {
      victim = static_cast<int>(i);
      victim_row = row;
      farthest = d;
    }
  }
  if (utc == 0 || distance(m, utc) >= farthest) return kNoIndex;
  auto& row = matches_[victim_row];
  row.own = kNoIndex;
  row.flags &= ~kMatchEventsTruncated;
  freeSlot(static_cast<uint8_t>(victim));
  return static_cast<uint8_t>(victim);
}

StoredMatch* MatchStore::upsert(const StoredMatch& incoming,
                                const Clock& clock,
                                std::string_view home_name,
                                std::string_view away_name) {
  if (incoming.id == 0) return nullptr;
  StoredMatch m = incoming;
  m.own = kNoIndex;
  m.flags &= ~kMatchEventsTruncated;
  uint8_t slot = kNoIndex;
  bool seen_running = false;
  const int at = matchIndex(m.competition, m.provider, m.id);
  if (at >= 0) {
    auto& prior = matches_[at];
    keepFallback(m, prior);
    m.score_changed = prior.score_changed;
    if (m.principal.valid() && prior.principal.valid() &&
        !(m.principal == prior.principal))
      m.score_changed = clock.mono_s ? clock.mono_s : 1;
    seen_running = running(prior.phase);
    if (m.finished_at == 0 && prior.finished_at != 0) {
      m.finished_at = prior.finished_at;
      m.estimated = (m.estimated & ~kEstimatedFinish) |
                    (prior.estimated & kEstimatedFinish);
    }
    slot = prior.own;
    m.flags |= prior.flags & kMatchEventsTruncated;
    prior.own = kNoIndex;  // the slot moves with the match
    removeMatch(static_cast<std::size_t>(at));
  }
  markFinished(m, seen_running, clock);
  if (match_count_ == matches_.size()) {
    // Full: remove the other club's match farthest from now.
    int victim = -1;
    uint64_t farthest = 0;
    for (std::size_t i = 0; i < match_count_; ++i) {
      if (matches_[i].isOwn()) continue;
      const auto d = distance(matches_[i], clock.utc);
      if (victim < 0 || d > farthest) {
        victim = static_cast<int>(i);
        farthest = d;
      }
    }
    if (clock.utc == 0 || victim < 0 ||
        (!m.isOwn() && distance(m, clock.utc) >= farthest)) {
      freeSlot(slot);
      note(m.competition, kNoticeMatches);
      return nullptr;
    }
    note(matches_[victim].competition, kNoticeMatches);
    removeMatch(static_cast<std::size_t>(victim));
  }
  if (m.isOwn()) {
    if (slot == kNoIndex) slot = ownSlot(m, clock.utc);
    if (slot != kNoIndex) {
      auto& own = own_[slot];
      own.match = m.id;
      own.competition = m.competition;
      if (!home_name.empty()) own.home_name.assign(home_name);
      if (!away_name.empty()) own.away_name.assign(away_name);
    }
  } else if (slot != kNoIndex) {
    freeSlot(slot);
    slot = kNoIndex;
  }
  m.own = slot;
  if (slot == kNoIndex) m.flags &= ~kMatchEventsTruncated;
  const std::size_t pos = sortedPosition(m);
  for (std::size_t j = match_count_; j > pos; --j) matches_[j] = matches_[j - 1];
  matches_[pos] = m;
  ++match_count_;
  return &matches_[pos];
}

StoredMatch* MatchStore::place(StoredMatch m, std::size_t from) {
  for (std::size_t j = from + 1; j < match_count_; ++j)
    matches_[j - 1] = matches_[j];
  --match_count_;
  const std::size_t pos = sortedPosition(m);
  for (std::size_t j = match_count_; j > pos; --j) matches_[j] = matches_[j - 1];
  matches_[pos] = m;
  ++match_count_;
  return &matches_[pos];
}

StoredMatch* MatchStore::fill(const StoredMatch& f, cfg::Provider provider,
                              uint32_t id, bool swapped, const Clock& clock) {
  const int at = matchIndex(f.competition, provider, id);
  if (at < 0) return nullptr;
  StoredMatch m = matches_[at];
  const bool seen_running = running(m.phase);
  // Open for the fallback: missing, only estimated, or filled before.
  const auto open = [&m](bool lacks, uint8_t bit) {
    return lacks || (m.fallback & bit);
  };
  if (f.kickoff != 0 && open(m.kickoff == 0, kFallbackKickoff)) {
    m.kickoff = f.kickoff;
    m.fallback |= kFallbackKickoff;
  }
  if (f.phase != Phase::kUnknown && !(f.estimated & kEstimatedPhase) &&
      open(m.phase == Phase::kUnknown || (m.estimated & kEstimatedPhase),
           kFallbackPhase)) {
    m.phase = f.phase;
    m.estimated &= ~kEstimatedPhase;
    m.fallback |= kFallbackPhase;
    settle(m);
  }
  if (f.minute != kNoValue && !(f.estimated & kEstimatedMinute) &&
      open(m.minute == kNoValue || (m.estimated & kEstimatedMinute),
           kFallbackMinute)) {
    m.minute = f.minute;
    m.estimated &= ~kEstimatedMinute;
    m.fallback |= kFallbackMinute;
  }
  const Goals before = m.principal;
  for (const auto& field : kScores) {
    Goals g = f.*field.goals;
    if (swapped) std::swap(g.home, g.away);
    if (g.valid() && open(!(m.*field.goals).valid(), field.bit)) {
      m.*field.goals = g;
      m.fallback |= field.bit;
    }
  }
  if (m.principal.valid() && before.valid() && !(m.principal == before))
    m.score_changed = clock.mono_s ? clock.mono_s : 1;
  if (m.phase == Phase::kFinished && m.finished_at == 0 && f.finished_at) {
    m.finished_at = f.finished_at;
    m.estimated = (m.estimated & ~kEstimatedFinish) |
                  (f.estimated & kEstimatedFinish);
  }
  markFinished(m, seen_running, clock);
  return place(m, static_cast<std::size_t>(at));
}

void MatchStore::removeProvider(uint8_t competition, cfg::Provider provider) {
  for (std::size_t i = match_count_; i-- > 0;)
    if (matches_[i].competition == competition &&
        matches_[i].provider == provider)
      removeMatch(i);
}

void MatchStore::expire(uint8_t competition, int64_t utc,
                        int32_t local_offset_s) {
  // Passed matchdays are always the earliest ones: find the last kickoff of
  // the latest passed matchday, then remove everything up to it.
  uint32_t cutoff = 0, last = 0;
  bool open = false;
  for (std::size_t i = 0; i < match_count_; ++i) {
    const auto& m = matches_[i];
    if (m.competition != competition || m.kickoff == 0) continue;
    if (open && m.kickoff - last >= cfg::kMatchdayGapHours * 3600u) {
      if (!matchdayPassed(last, utc, local_offset_s)) break;
      cutoff = last;
    }
    open = true;
    last = m.kickoff;
  }
  if (open && cutoff != last && matchdayPassed(last, utc, local_offset_s))
    cutoff = last;
  if (cutoff == 0) return;
  for (std::size_t i = match_count_; i-- > 0;) {
    const auto& m = matches_[i];
    if (m.competition == competition && m.kickoff != 0 && m.kickoff <= cutoff)
      removeMatch(i);
  }
}

void MatchStore::estimateAll(int64_t utc) {
  for (std::size_t i = 0; i < match_count_; ++i) estimate(matches_[i], utc);
}

void MatchStore::removeCompetition(uint8_t competition) {
  for (std::size_t i = match_count_; i-- > 0;)
    if (matches_[i].competition == competition) removeMatch(i);
  dropRows(competition);
  for (auto& n : notices_)
    if (n.competition == competition) n = {};
}

void MatchStore::collect() {
  std::bitset<cfg::kStoreTeams> teams;
  std::bitset<cfg::kStoreRoundLabels> labels;
  for (std::size_t i = 0; i < match_count_; ++i) {
    const auto& m = matches_[i];
    if (m.home < teams.size()) teams.set(m.home);
    if (m.away < teams.size()) teams.set(m.away);
    if (m.label < labels.size()) labels.set(m.label);
  }
  for (std::size_t i = 0; i < row_count_; ++i)
    if (rows_[i].team < teams.size()) teams.set(rows_[i].team);
  for (std::size_t i = 0; i < teams_.size(); ++i)
    if (!teams.test(i)) teams_[i] = {};
  for (std::size_t i = 0; i < labels_.size(); ++i)
    if (!labels.test(i)) labels_[i] = {};
}

void MatchStore::dropRows(uint8_t competition) {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < row_count_; ++i)
    if (rows_[i].competition != competition) rows_[kept++] = rows_[i];
  for (std::size_t i = kept; i < row_count_; ++i) rows_[i] = {};
  row_count_ = static_cast<uint16_t>(kept);
}

void MatchStore::beginTable(uint8_t competition) {
  dropRows(0);
  staged_rows_ = 0;
  table_competition_ = competition;
  table_cut_ = table_dropped_ = false;
}

bool MatchStore::appendRow(const StoredRow& row) {
  if (table_competition_ == 0) return false;
  if (row_count_ == rows_.size() && !table_dropped_) {
    dropRows(table_competition_);
    table_dropped_ = true;
  }
  if (row_count_ == rows_.size()) {
    table_cut_ = true;
    note(table_competition_, kNoticeTable);
    return false;
  }
  rows_[row_count_] = row;
  rows_[row_count_++].competition = 0;
  ++staged_rows_;
  return true;
}

bool MatchStore::commitTable(bool complete) {
  const uint8_t competition = table_competition_;
  table_competition_ = 0;
  if (competition == 0) return false;
  const bool commit = complete && !table_cut_ && staged_rows_ > 0;
  if (commit) {
    dropRows(competition);
    for (std::size_t i = 0; i < row_count_; ++i)
      if (rows_[i].competition == 0) rows_[i].competition = competition;
  } else {
    dropRows(0);
    if (table_dropped_) note(competition, kNoticeTable);
  }
  staged_rows_ = 0;
  return commit;
}

bool MatchStore::hasTable(uint8_t competition, cfg::Provider provider) const {
  for (std::size_t i = 0; i < row_count_; ++i)
    if (rows_[i].competition == competition &&
        rows_[i].team < teams_.size() &&
        teams_[rows_[i].team].id != 0 &&
        teams_[rows_[i].team].provider == provider)
      return true;
  return false;
}

bool MatchStore::beginEvents(const StoredMatch& match, bool fallback,
                             bool swapped, bool detail) {
  pending_slot_ = kNoIndex;
  if (match.own >= own_.size() || own_[match.own].match != match.id)
    return false;
  pending_slot_ = match.own;
  pending_count_ = 0;
  pending_cut_ = false;
  pending_fallback_ = fallback;
  pending_swapped_ = fallback && swapped;
  pending_detail_ = !fallback && detail;
  return true;
}

void MatchStore::addEvent(const StoredEvent& incoming) {
  if (pending_slot_ == kNoIndex) return;
  StoredEvent event = incoming;
  event.flags &= ~kEventOrigin;
  if (pending_fallback_) event.flags |= kEventFallback;
  if (pending_detail_) event.flags |= kEventDetail;
  if (pending_swapped_) {
    event.actor = mirror(event.actor);
    event.beneficiary = mirror(event.beneficiary);
    std::swap(event.after.home, event.after.away);
  }
  if (pending_count_ == pending_.size()) {
    // Keep the latest events: reuse the entry of the oldest new one.
    const uint8_t reuse = pending_[0];
    for (std::size_t i = 1; i < pending_count_; ++i)
      pending_[i - 1] = pending_[i];
    pending_[pending_count_ - 1] = reuse;
    events_[reuse] = event;
    events_[reuse].slot = kPendingSlot;
    pending_cut_ = true;
    return;
  }
  // The previous entry of the same event is updated in place, so an update
  // needs pool room only for new events.
  int at = -1;
  for (std::size_t i = 0; i < events_.size() && at < 0; ++i)
    if (events_[i].slot == pending_slot_ &&
        events_[i].identity == event.identity &&
        (events_[i].flags & kEventOrigin) == (event.flags & kEventOrigin))
      at = static_cast<int>(i);
  for (std::size_t i = 0; i < events_.size() && at < 0; ++i)
    if (events_[i].slot == kNoIndex) at = static_cast<int>(i);
  if (at >= 0) {
    events_[at] = event;
    events_[at].slot = kPendingSlot;
    pending_[pending_count_++] = static_cast<uint8_t>(at);
    return;
  }
  pending_cut_ = true;
  note(own_[pending_slot_].competition, kNoticeEvents);
}

bool MatchStore::restoreEvent(const StoredMatch& match,
                              const StoredEvent& event) {
  if (match.own >= own_.size() || own_[match.own].match != match.id)
    return false;
  auto& own = own_[match.own];
  if (own.event_count >= own.events.size()) return false;
  for (std::size_t i = 0; i < events_.size(); ++i) {
    if (events_[i].slot != kNoIndex) continue;
    events_[i] = event;
    events_[i].slot = match.own;
    own.events[own.event_count++] = static_cast<uint8_t>(i);
    return true;
  }
  note(own.competition, kNoticeEvents);
  return false;
}

void MatchStore::endEvents(bool complete) {
  if (pending_slot_ == kNoIndex) return;
  const uint8_t s = pending_slot_;
  pending_slot_ = kNoIndex;
  auto& own = own_[s];
  StoredMatch* row = nullptr;
  for (std::size_t i = 0; i < match_count_ && row == nullptr; ++i)
    if (matches_[i].own == s) row = &matches_[i];
  if (!complete) {
    // Broken off: the previous list (updated in place where seen again)
    // followed by the new events; the oldest go when it overflows.
    std::array<uint8_t, cfg::kMatchEvents + cfg::kMatchEvents> merged{};
    std::size_t count = 0;
    for (std::size_t n = 0; n < own.event_count; ++n)
      merged[count++] = own.events[n];
    for (std::size_t k = 0; k < pending_count_; ++k)
      if (std::find(merged.begin(), merged.begin() + count, pending_[k]) ==
          merged.begin() + count)
        merged[count++] = pending_[k];
    const std::size_t drop =
        count > own.events.size() ? count - own.events.size() : 0;
    for (std::size_t n = 0; n < drop; ++n) events_[merged[n]] = {};
    own.event_count = static_cast<uint8_t>(count - drop);
    for (std::size_t n = 0; n < own.event_count; ++n) {
      own.events[n] = merged[drop + n];
      events_[own.events[n]].slot = s;
    }
    if (row != nullptr && (drop > 0 || pending_cut_))
      row->flags |= kMatchEventsTruncated;
    return;
  }
  const bool whole =
      !pending_cut_ && row != nullptr && !(row->flags & kMatchEventsTruncated);
  const uint8_t origin = pending_fallback_ ? kEventFallback
                         : pending_detail_  ? kEventDetail
                                            : 0;
  // The other origins' events stay ahead of the new list.
  std::array<uint8_t, cfg::kMatchEvents + cfg::kMatchEvents> list{};
  std::size_t count = 0;
  // Previous entries of this origin still held by the slot are missing from
  // the new list (present ones were updated in place by addEvent()).
  for (std::size_t n = 0; n < own.event_count; ++n) {
    auto& previous = events_[own.events[n]];
    if (previous.slot != s) continue;
    if ((previous.flags & kEventOrigin) != origin) {
      list[count++] = own.events[n];
      continue;
    }
    if (whole && !(previous.flags & kEventRemoved)) {
      if (pending_count_ < pending_.size()) {
        previous.flags |= kEventRemoved;
        ++previous.revision;
        previous.slot = kPendingSlot;
        pending_[pending_count_++] = own.events[n];
        continue;
      }
      pending_cut_ = true;
    }
    previous = {};
  }
  for (std::size_t k = 0; k < pending_count_; ++k) {
    events_[pending_[k]].slot = s;
    list[count++] = pending_[k];
  }
  // Too many for the slot: hidden fallback events go first, then other
  // fallback events, then detail events, then the oldest; dropping a shown
  // event cuts the list.
  while (count > own.events.size()) {
    std::size_t victim = count;
    for (std::size_t k = 0; k < count && victim == count; ++k)
      if ((events_[list[k]].flags & kEventFallback) &&
          !shownAmong(events_[list[k]], events_, list.data(), count))
        victim = k;
    for (const uint8_t bit : {kEventFallback, kEventDetail})
      for (std::size_t k = 0; k < count && victim == count; ++k)
        if (events_[list[k]].flags & bit) victim = k;
    if (victim == count) victim = 0;
    if (shownAmong(events_[list[victim]], events_, list.data(), count))
      pending_cut_ = true;
    events_[list[victim]] = {};
    for (std::size_t k = victim + 1; k < count; ++k) list[k - 1] = list[k];
    --count;
  }
  std::copy(list.begin(), list.begin() + count, own.events.begin());
  own.event_count = static_cast<uint8_t>(count);
  if (row != nullptr) {
    if (pending_cut_)
      row->flags |= kMatchEventsTruncated;
    else
      row->flags &= ~kMatchEventsTruncated;
  }
}

bool MatchStore::matchday(uint8_t competition, uint8_t ordinal,
                          Matchday& out) const {
  Matchday current;
  uint8_t index = 0;
  for (std::size_t i = 0; i < match_count_; ++i) {
    const auto& m = matches_[i];
    if (m.competition != competition || m.kickoff == 0) continue;
    if (current.count &&
        m.kickoff - current.last >= cfg::kMatchdayGapHours * 3600u) {
      if (index == ordinal) {
        out = current;
        return true;
      }
      ++index;
      current = {};
    }
    if (current.count == 0) current.first = m.kickoff;
    current.last = m.kickoff;
    if (current.count < UINT8_MAX) ++current.count;
  }
  if (current.count && index == ordinal) {
    out = current;
    return true;
  }
  return false;
}

void MatchStore::note(uint8_t competition, uint8_t bits) {
  Notice* free = nullptr;
  for (auto& n : notices_) {
    if (n.competition == competition) {
      n.bits |= bits;
      return;
    }
    if (n.competition == 0 && free == nullptr) free = &n;
  }
  if (free != nullptr) *free = {competition, bits};
}

void MatchStore::flag(uint8_t competition, uint8_t bits, bool on) {
  if (on) {
    note(competition, bits);
    return;
  }
  for (auto& n : notices_)
    if (n.competition == competition) n.bits &= ~bits;
}

bool MatchStore::eventShown(uint8_t slot, const StoredEvent& e) const {
  if (slot >= own_.size()) return true;
  return shownAmong(e, events_, own_[slot].events.data(),
                    own_[slot].event_count);
}

uint8_t MatchStore::notices(uint8_t competition) const {
  for (const auto& n : notices_)
    if (n.competition == competition) return n.bits;
  return 0;
}

std::size_t MatchStore::teamCount() const {
  return static_cast<std::size_t>(std::count_if(
      teams_.begin(), teams_.end(), [](const auto& t) { return t.id != 0; }));
}

uint32_t decimalId(std::string_view text) {
  if (text.empty() || text.size() > 9) return 0;
  uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return 0;
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  return value;
}

}  // namespace football
