#include "match_store.h"

#include <algorithm>
#include <bitset>
#include <new>

#include "matchday.h"

namespace football {
namespace {

// Marks an event added since beginEvents() until endEvents().
constexpr uint8_t kPendingSlot = kNoIndex - 1;
static_assert(cfg::kStoreDetailMatches < kPendingSlot);

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

std::size_t eventBegin(uint8_t slot) {
  return slot < cfg::kStoreOwnMatches ? 0 : cfg::kStoreOwnEvents;
}
std::size_t eventEnd(uint8_t slot) {
  return slot < cfg::kStoreOwnMatches ? cfg::kStoreOwnEvents
                                      : cfg::kStoreEvents;
}
bool sameDetailBank(uint8_t a, uint8_t b) {
  return (a < cfg::kStoreOwnMatches) == (b < cfg::kStoreOwnMatches);
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

}  // namespace

bool running(Phase p) {
  return p == Phase::kLive || p == Phase::kHalfTime ||
         p == Phase::kExtraTime || p == Phase::kPenalties;
}

int32_t civilDay(int64_t utc, const DateContext& dates) {
  const int32_t offset = dates.offset_at ? dates.offset_at(utc, dates.context)
                                         : dates.fallback_offset_s;
  const int64_t local = utc + offset;
  return static_cast<int32_t>(local >= 0 ? local / 86400
                                         : (local - 86399) / 86400);
}

bool resultHeld(const StoredMatch& m, int64_t utc, const DateContext& dates) {
  return m.phase == Phase::kFinished && m.finished_at != 0 && utc > 0 &&
         civilDay(utc, dates) < civilDay(m.finished_at, dates) + 2;
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

MatchStore::MatchStore() = default;

// Rebuilt in place: never put a temporary complete store on the stack.
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
    note(competition, table_competition_ == competition ? kNoticeTableTeams
                                                        : kNoticeMatchTeams);
    return kNoIndex;
  }
  teams_[free] = {id, provider, name};
  return static_cast<uint8_t>(free);
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
  if (slot >= details_.size()) return;
  for (auto& e : events_)
    if (e.slot == slot) e = {};
  details_[slot] = {};
}

void MatchStore::removeMatch(std::size_t i) {
  freeSlot(matches_[i].detail);
  for (std::size_t j = i + 1; j < match_count_; ++j)
    matches_[j - 1] = matches_[j];
  matches_[--match_count_] = {};
}

std::size_t MatchStore::sortedPosition(const StoredMatch& m) const {
  std::size_t at = match_count_;
  while (at > 0 && sortKey(matches_[at - 1]) > sortKey(m)) --at;
  return at;
}

uint8_t MatchStore::detailSlot(const StoredMatch& m, int64_t utc,
                               bool requested) {
  const std::size_t first = m.isOwn() ? 0 : cfg::kStoreOwnMatches;
  const std::size_t end = m.isOwn() ? cfg::kStoreOwnMatches : details_.size();
  // An ordinary Single has one explicit target per competition. Replacing
  // that target must not displace another competition's requested Single.
  int victim = -1, victim_row = -1;
  uint64_t farthest = 0;
  bool same_competition = false;
  int free = -1;
  for (std::size_t i = first; i < end; ++i) {
    int row = -1;
    for (std::size_t j = 0; j < match_count_ && row < 0; ++j)
      if (matches_[j].detail == i) row = static_cast<int>(j);
    if (row < 0) {
      freeSlot(static_cast<uint8_t>(i));
      if (free < 0) free = static_cast<int>(i);
      continue;
    }
    const auto d = distance(matches_[row], utc);
    const bool same =
        requested && !m.isOwn() && matches_[row].competition == m.competition;
    if (victim < 0 || (same && !same_competition) ||
        (same == same_competition && d > farthest)) {
      victim = static_cast<int>(i);
      victim_row = row;
      farthest = d;
      same_competition = same;
    }
  }
  if (!same_competition && free >= 0) return static_cast<uint8_t>(free);
  if (victim < 0 || (!requested && (utc == 0 || distance(m, utc) >= farthest)))
    return kNoIndex;
  auto& row = matches_[victim_row];
  row.detail = kNoIndex;
  row.flags &= ~(kMatchEventsTruncated | kMatchEventsPoolCut);
  freeSlot(static_cast<uint8_t>(victim));
  return static_cast<uint8_t>(victim);
}

bool MatchStore::hasDetail(const StoredMatch& m) const {
  return m.detail < details_.size() &&
         (m.detail < cfg::kStoreOwnMatches) == m.isOwn() &&
         details_[m.detail].match == m.id &&
         details_[m.detail].competition == m.competition;
}

StoredMatch* MatchStore::ensureDetail(uint8_t competition,
                                      cfg::Provider provider, uint32_t id,
                                      const Clock& clock,
                                      std::string_view home_name,
                                      std::string_view away_name) {
  const int at = matchIndex(competition, provider, id);
  if (at < 0) return nullptr;
  auto& m = matches_[at];
  if (!hasDetail(m)) {
    m.detail = detailSlot(m, clock.utc, !m.isOwn());
    if (m.detail == kNoIndex) return nullptr;
  }
  auto& detail = details_[m.detail];
  detail.match = m.id;
  detail.competition = m.competition;
  if (!home_name.empty()) detail.home_name.assign(home_name);
  if (!away_name.empty()) detail.away_name.assign(away_name);
  return &m;
}

StoredMatch* MatchStore::upsert(const StoredMatch& incoming,
                                const Clock& clock,
                                std::string_view home_name,
                                std::string_view away_name) {
  if (incoming.id == 0) return nullptr;
  StoredMatch m = incoming;
  m.detail = kNoIndex;
  m.flags &= ~(kMatchEventsTruncated | kMatchEventsPoolCut);
  uint8_t slot = kNoIndex;
  bool seen_running = false;
  const int at = matchIndex(m.competition, m.provider, m.id);
  if (at >= 0) {
    auto& prior = matches_[at];
    m.score_changed = prior.score_changed;
    if (m.principal.valid() && prior.principal.valid() &&
        !(m.principal == prior.principal))
      m.score_changed = clock.mono_s ? clock.mono_s : 1;
    seen_running = running(prior.phase) || (prior.phase == Phase::kSuspended &&
                                            prior.first_live_observed_utc != 0);
    if (running(m.phase) || m.phase == Phase::kSuspended) {
      if (prior.first_live_observed_utc != 0)
        m.first_live_observed_utc = prior.first_live_observed_utc;
      if (m.phase == Phase::kSuspended && m.minute == kNoValue) {
        m.minute = prior.minute;
        m.estimated = (m.estimated & ~kEstimatedMinute) |
                      (prior.estimated & kEstimatedMinute);
      }
    }
    // The final whistle belongs to a finished match only: a provider that
    // reports the match running again (a correction or a restart) drops it.
    if (m.phase == Phase::kFinished && m.finished_at == 0 &&
        prior.finished_at != 0) {
      m.finished_at = prior.finished_at;
      m.estimated = (m.estimated & ~kEstimatedFinish) |
                    (prior.estimated & kEstimatedFinish);
    }
    slot = prior.detail;
    m.flags |= prior.flags & (kMatchEventsTruncated | kMatchEventsPoolCut);
    prior.detail = kNoIndex;  // the slot moves with the match
    removeMatch(static_cast<std::size_t>(at));
  }
  if (running(m.phase) && !(m.estimated & kEstimatedPhase) &&
      m.first_live_observed_utc == 0 && clock.utc > 0 &&
      clock.utc <= UINT32_MAX)
    m.first_live_observed_utc = static_cast<uint32_t>(clock.utc);
  if (m.phase == Phase::kScheduled || m.phase == Phase::kPostponed ||
      m.phase == Phase::kCancelled || m.phase == Phase::kAbandoned)
    m.first_live_observed_utc = 0;
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
  // Ownership changes cannot move an ordinary target into the protected own
  // bank or leave a former own target there.
  if (slot != kNoIndex && (slot < cfg::kStoreOwnMatches) != m.isOwn()) {
    freeSlot(slot);
    slot = kNoIndex;
  }
  if (m.isOwn() && slot == kNoIndex) slot = detailSlot(m, clock.utc, false);
  if (slot != kNoIndex) {
    auto& detail = details_[slot];
    detail.match = m.id;
    detail.competition = m.competition;
    if (!home_name.empty()) detail.home_name.assign(home_name);
    if (!away_name.empty()) detail.away_name.assign(away_name);
  }
  m.detail = slot;
  if (slot == kNoIndex)
    m.flags &= ~(kMatchEventsTruncated | kMatchEventsPoolCut);
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

void MatchStore::expire(uint8_t competition, int64_t utc,
                        int32_t local_offset_s, const DateContext* dates) {
  // Passed matchdays are always the earliest ones: find the last kickoff of
  // the latest passed matchday, then remove everything up to it.
  uint32_t cutoff = 0;
  matchday::Span span;
  const auto all = [](const StoredMatch&) { return true; };
  for (std::size_t from = 0;
       matchday::next(*this, competition, all, from, span) &&
       matchday::passed(span.last, utc, local_offset_s);
       from = span.end)
    cutoff = span.last;
  if (cutoff == 0) return;
  const DateContext fallback{nullptr, nullptr, local_offset_s};
  const auto& calendar = dates ? *dates : fallback;
  for (std::size_t i = match_count_; i-- > 0;) {
    const auto& m = matches_[i];
    if (m.competition == competition && m.kickoff != 0 && m.kickoff <= cutoff &&
        !resultHeld(m, utc, calendar))
      removeMatch(i);
  }
}

bool MatchStore::unmaintained(uint8_t competition, int64_t utc) const {
  for (std::size_t i = 0; i < match_count_; ++i) {
    const auto& m = matches_[i];
    if (m.competition != competition || m.kickoff == 0 ||
        utc - m.kickoff <= cfg::kUnmaintainedAfterS || m.principal.valid())
      continue;
    if (m.phase == Phase::kFinished || m.phase == Phase::kPostponed ||
        m.phase == Phase::kCancelled || m.phase == Phase::kAbandoned)
      continue;
    return true;
  }
  return false;
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
  // A table being staged keeps its teams until commitTable().
  for (std::size_t i = 0; table_competition_ != 0 && i < staged_rows_; ++i)
    if (stage_[i].team < teams.size()) teams.set(stage_[i].team);
  for (std::size_t i = 0; i < teams_.size(); ++i)
    if (!teams.test(i)) teams_[i] = {};
  for (std::size_t i = 0; i < labels_.size(); ++i)
    if (!labels.test(i)) labels_[i] = {};
  // Events notices of own matches that left the store.
  for (const auto& n : notices_)
    if (n.bits & kNoticeEvents) settleEvents(n.competition);
}

void MatchStore::dropRows(uint8_t competition) {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < row_count_; ++i)
    if (rows_[i].competition != competition) rows_[kept++] = rows_[i];
  for (std::size_t i = kept; i < row_count_; ++i) rows_[i] = {};
  row_count_ = static_cast<uint16_t>(kept);
}

void MatchStore::beginTable(uint8_t competition) {
  beginRun(competition, kNoticeTable | kNoticeTableTeams);
  staged_rows_ = 0;
  table_competition_ = competition;
  table_cut_ = false;
}

bool MatchStore::appendRow(const StoredRow& row) {
  if (table_competition_ == 0) return false;
  if (staged_rows_ == stage_.size()) {
    table_cut_ = true;
    note(table_competition_, kNoticeTable);
    return false;
  }
  stage_[staged_rows_] = row;
  stage_[staged_rows_++].competition = table_competition_;
  return true;
}

bool MatchStore::commitTable(bool complete) {
  const uint8_t competition = table_competition_;
  const std::size_t staged = staged_rows_;
  table_competition_ = 0;
  staged_rows_ = 0;
  if (competition == 0 || !complete || table_cut_ || staged == 0) return false;
  const auto stored = static_cast<std::size_t>(std::count_if(
      rows_.begin(), rows_.begin() + row_count_,
      [&](const StoredRow& r) { return r.competition == competition; }));
  if (row_count_ - stored + staged > rows_.size()) {
    note(competition, kNoticeTable);
    return false;
  }
  dropRows(competition);
  std::copy_n(stage_.begin(), staged, rows_.begin() + row_count_);
  row_count_ = static_cast<uint16_t>(row_count_ + staged);
  settleRun(competition, kNoticeTable | kNoticeTableTeams);
  return true;
}

bool MatchStore::beginEvents(const StoredMatch& match, bool detail,
                             int64_t utc) {
  pending_slot_ = kNoIndex;
  if (!hasDetail(match)) return false;
  pending_slot_ = match.detail;
  pending_utc_ = utc;
  pending_count_ = 0;
  pending_cut_ = false;
  pending_pool_ = false;
  pending_detail_ = detail;
  return true;
}

void MatchStore::addEvent(const StoredEvent& incoming) {
  if (pending_slot_ == kNoIndex) return;
  StoredEvent event = incoming;
  event.flags &= ~kEventDetail;
  if (pending_detail_) event.flags |= kEventDetail;
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
  for (std::size_t i = eventBegin(pending_slot_);
       i < eventEnd(pending_slot_) && at < 0; ++i)
    if (events_[i].slot == pending_slot_ &&
        events_[i].identity == event.identity &&
        (events_[i].flags & kEventDetail) == (event.flags & kEventDetail))
      at = static_cast<int>(i);
  for (std::size_t i = eventBegin(pending_slot_);
       i < eventEnd(pending_slot_) && at < 0; ++i)
    if (events_[i].slot == kNoIndex) at = static_cast<int>(i);
  if (at < 0) at = takeEvent();
  if (at >= 0) {
    events_[at] = event;
    events_[at].slot = kPendingSlot;
    pending_[pending_count_++] = static_cast<uint8_t>(at);
    return;
  }
  pending_cut_ = pending_pool_ = true;
  note(details_[pending_slot_].competition, kNoticeEvents);
}

int MatchStore::takeEvent() {
  if (pending_utc_ == 0) return -1;
  const StoredMatch* mine = nullptr;
  for (std::size_t j = 0; j < match_count_ && mine == nullptr; ++j)
    if (matches_[j].detail == pending_slot_) mine = &matches_[j];
  if (mine == nullptr) return -1;
  // The farthest match holding an event in this same protected bank.
  int victim = -1;
  StoredMatch* victim_row = nullptr;
  uint64_t farthest = 0;
  for (std::size_t j = 0; j < match_count_; ++j) {
    auto& m = matches_[j];
    if (m.detail >= details_.size() || m.detail == pending_slot_ ||
        !sameDetailBank(m.detail, pending_slot_) ||
        details_[m.detail].event_count == 0)
      continue;
    const auto d = distance(m, pending_utc_);
    if (victim < 0 || d > farthest) {
      victim = m.detail;
      victim_row = &m;
      farthest = d;
    }
  }
  if (victim < 0 || distance(*mine, pending_utc_) >= farthest) return -1;
  auto& own = details_[victim];
  const uint8_t taken = own.events[0];
  for (std::size_t n = 1; n < own.event_count; ++n)
    own.events[n - 1] = own.events[n];
  --own.event_count;
  events_[taken] = {};
  victim_row->flags |= kMatchEventsTruncated | kMatchEventsPoolCut;
  note(own.competition, kNoticeEvents);
  return taken;
}

bool MatchStore::restoreEvent(const StoredMatch& match,
                              const StoredEvent& event) {
  if (!hasDetail(match)) return false;
  auto& own = details_[match.detail];
  if (own.event_count >= own.events.size()) return false;
  for (std::size_t i = eventBegin(match.detail); i < eventEnd(match.detail);
       ++i) {
    if (events_[i].slot != kNoIndex) continue;
    events_[i] = event;
    events_[i].slot = match.detail;
    own.events[own.event_count++] = static_cast<uint8_t>(i);
    return true;
  }
  if (auto* row = detailRow(match.detail)) row->flags |= kMatchEventsPoolCut;
  note(own.competition, kNoticeEvents);
  return false;
}

StoredMatch* MatchStore::detailRow(uint8_t slot) {
  for (std::size_t i = 0; i < match_count_; ++i)
    if (matches_[i].detail == slot) return &matches_[i];
  return nullptr;
}

void MatchStore::settleEvents(uint8_t competition) {
  for (std::size_t i = 0; i < match_count_; ++i) {
    const auto& m = matches_[i];
    if (m.competition == competition && m.detail < details_.size() &&
        (m.flags & (kMatchEventsPoolCut | kMatchEventsTruncated))) {
      flag(competition, kNoticeEvents, true);
      return;
    }
  }
  flag(competition, kNoticeEvents, false);
}

void MatchStore::endEvents(bool complete) {
  if (pending_slot_ == kNoIndex) return;
  const uint8_t s = pending_slot_;
  pending_slot_ = kNoIndex;
  auto& own = details_[s];
  StoredMatch* row = detailRow(s);
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
    if (row != nullptr && pending_pool_) row->flags |= kMatchEventsPoolCut;
    if (row != nullptr) settleEvents(row->competition);
    return;
  }
  const bool whole =
      !pending_cut_ && row != nullptr && !(row->flags & kMatchEventsTruncated);
  const uint8_t origin = pending_detail_ ? kEventDetail : 0;
  // The other origin's events stay ahead of the new list.
  std::array<uint8_t, cfg::kMatchEvents + cfg::kMatchEvents> list{};
  std::size_t count = 0;
  // Previous entries of this origin still held by the slot are missing from
  // the new list (present ones were updated in place by addEvent()).
  for (std::size_t n = 0; n < own.event_count; ++n) {
    auto& previous = events_[own.events[n]];
    if (previous.slot != s) continue;
    if ((previous.flags & kEventDetail) != origin) {
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
  // Too many for the slot: detail events go first, then the oldest; the
  // list is cut.
  while (count > own.events.size()) {
    std::size_t victim = 0;
    for (std::size_t k = 0; k < count; ++k)
      if (events_[list[k]].flags & kEventDetail) {
        victim = k;
        break;
      }
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
    if (pending_pool_)
      row->flags |= kMatchEventsPoolCut;
    else
      row->flags &= ~kMatchEventsPoolCut;
  }
  settleEvents(own.competition);
}

void MatchStore::note(uint8_t competition, uint8_t bits) {
  Notice* free = nullptr;
  for (auto& n : notices_) {
    if (n.competition == competition) {
      n.bits |= bits;
      n.noted |= bits;
      return;
    }
    if (n.competition == 0 && free == nullptr) free = &n;
  }
  if (free != nullptr) *free = {competition, bits, bits};
}

void MatchStore::beginRun(uint8_t competition, uint8_t bits) {
  for (auto& n : notices_)
    if (n.competition == competition) n.noted &= ~bits;
}

void MatchStore::settleRun(uint8_t competition, uint8_t bits) {
  for (auto& n : notices_)
    if (n.competition == competition)
      n.bits = (n.bits & ~bits) | (n.noted & bits);
}

void MatchStore::flag(uint8_t competition, uint8_t bits, bool on) {
  if (on) {
    note(competition, bits);
    return;
  }
  for (auto& n : notices_)
    if (n.competition == competition) n.bits &= ~bits;
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
  if (text.empty()) return 0;
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return 0;
    value = value * 10 + static_cast<uint64_t>(c - '0');
    if (value > UINT32_MAX) return 0;
  }
  return static_cast<uint32_t>(value);
}

}  // namespace football
