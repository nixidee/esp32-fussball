#include "match_store.h"

#include <algorithm>
#include <bitset>
#include <utility>

#include "civil_time.h"
#include "fixture_retention.h"

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

constexpr uint8_t noticeKinds(MatchStore::Dataset dataset) {
  switch (dataset) {
    case MatchStore::Dataset::kFixtures: return kNoticeFixtures;
    case MatchStore::Dataset::kTable: return kNoticeTable | kNoticeTableTeams;
    case MatchStore::Dataset::kEvents: return kNoticeEvents;
  }
  return 0;
}
void summarize(DatasetSummary& out, const DatasetStatus& dataset) {
  out.last_success_ms = std::max(out.last_success_ms, dataset.last_success_ms);
  if (dataset.cause != Cause::kNone &&
      (out.cause == Cause::kNone ||
       dataset.last_attempt_ms > out.failed_attempt_ms)) {
    out.failed_attempt_ms = dataset.last_attempt_ms;
    out.cause = dataset.cause;
    out.http = dataset.http;
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

}  // namespace

bool running(Phase p) {
  return p == Phase::kLive || p == Phase::kHalfTime || p == Phase::kExtraTime ||
         p == Phase::kPenalties;
}

MatchTiming matchTiming(const StoredMatch& m) {
  MatchTiming out;
  if (m.phase == Phase::kPostponed || m.phase == Phase::kCancelled ||
      m.phase == Phase::kAbandoned)
    return out;
  const bool suspended = m.phase == Phase::kSuspended;
  if (suspended && m.first_live_observed_utc == 0) return out;
  out.finished = m.phase == Phase::kFinished;
  if (out.finished) {
    if (m.finished_at == 0) return out;
    out.basis = m.kickoff != 0 ? m.kickoff : m.finished_at;
    out.end = m.finished_at;
  } else {
    out.reported_running =
        (running(m.phase) && !(m.estimated & kEstimatedPhase)) || suspended;
    out.basis = m.kickoff != 0 ? m.kickoff : m.first_live_observed_utc;
    if (out.basis == 0 || (m.kickoff == 0 && !out.reported_running)) return out;
    out.end =
        out.basis + int64_t{out.reported_running ? cfg::kUnknownMatchEndMinutes
                                                 : cfg::kExpectedMatchMinutes} *
                        60;
  }
  out.valid = true;
  return out;
}

int32_t civilDay(int64_t utc, const DateContext& dates) {
  const int32_t offset = dates.offset_at ? dates.offset_at(utc, dates.context)
                                         : dates.fallback_offset_s;
  const int64_t local = utc + offset;
  return static_cast<int32_t>(local >= 0 ? local / timekeeping::kSecondsPerDay
                                         : (local - 86399) /
                                               timekeeping::kSecondsPerDay);
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
  // Clear in place: no store-sized stack temporary, and delayed logical IDs
  // must never alias a later source in the same boot.
  source_index_.clear();
  matches_.fill({});
  teams_.fill({});
  rows_.fill({});
  stage_.fill({});
  details_.fill({});
  events_.fill({});
  labels_.fill({});
  rounds_.fill({});
  groups_.fill({});
  stage_groups_.fill({});
  sources_.fill({});
  bindings_.fill({});
  fixture_changes_ = 0;
  round_count_ = group_count_ = staged_groups_ = 0;
  pending_utc_ = 0;
  pending_.fill(0);
  pending_shots_.fill(0);
  pending_shot_order_ = pending_count_ = 0;
  pending_slot_ = kNoIndex;
  pending_cut_ = pending_pool_ = pending_detail_ = false;
  table_source_ = kNoSource;
  table_cut_ = false;
  match_count_ = row_count_ = staged_rows_ = 0;
}

TeamIndex MatchStore::team(SourceSlot source_slot, uint32_t id,
                           std::string_view name, std::string_view short_name) {
  if (id == 0 || !sourceId(source_slot)) return kNoTeam;
  const auto provider = this->provider(source_slot);
  int free = -1;
  for (std::size_t i = 0; i < teams_.size(); ++i) {
    auto& t = teams_[i];
    if (t.id == id && t.provider == provider) {
      if (!name.empty()) t.name.assign(name);
      if (!name.empty() || !short_name.empty()) t.short_name.assign(short_name);
      return static_cast<TeamIndex>(i);
    }
    if (t.id == 0 && free < 0) free = static_cast<int>(i);
  }
  if (free < 0) {
    note(source_slot,
         table_source_ == source_slot ? kNoticeTableTeams : kNoticeMatchTeams);
    return kNoTeam;
  }
  auto& value = teams_[free];
  value.id = id;
  value.provider = provider;
  value.name.assign(name);
  value.short_name.assign(short_name);
  return static_cast<TeamIndex>(free);
}

MatchStore::Binding* MatchStore::binding(uint8_t competition) {
  return const_cast<Binding*>(std::as_const(*this).binding(competition));
}
const MatchStore::Binding* MatchStore::binding(uint8_t competition) const {
  if (competition)
    for (const auto& item : bindings_)
      if (item.competition == competition) return &item;
  return nullptr;
}
uint8_t MatchStore::bindingMask(uint8_t competition) const {
  const auto* item = binding(competition);
  return item ? static_cast<uint8_t>(1u << (item - bindings_.data())) : 0;
}
cfg::Provider MatchStore::provider(SourceSlot slot) const {
  const auto* key = sourceKey(slot);
  return key ? key->provider : cfg::kProviderDefault;
}
SourceSlot MatchStore::currentSource(uint8_t competition) const {
  const auto* item = binding(competition);
  return item ? item->current : kNoSource;
}
bool MatchStore::includes(uint8_t competition, SourceSlot slot) const {
  return slot < sources_.size() && sourceId(slot) &&
         (sources_[slot].bindings & bindingMask(competition));
}
bool MatchStore::selected(const Binding& current, SourceSlot slot,
                          TeamIndex team) const {
  return current.club && includes(current.competition, slot) &&
         team < teams_.size() && teams_[team].id == current.club &&
         teams_[team].provider == provider(slot);
}
void MatchStore::ownSides(uint8_t competition, const StoredMatch& match,
                          bool& home, bool& away) const {
  const auto* current = binding(competition);
  home = current && selected(*current, match.source, match.home);
  away = current && selected(*current, match.source, match.away);
}
bool MatchStore::isOwn(uint8_t competition, const StoredMatch& match) const {
  bool home, away;
  ownSides(competition, match, home, away);
  return home || away;
}
bool MatchStore::isOwn(uint8_t competition, const StoredRow& row) const {
  const auto* current = binding(competition);
  return current && selected(*current, row.source, row.team);
}
bool MatchStore::isOwn(const StoredMatch& match) const {
  for (const auto& current : bindings_)
    if (current.competition && isOwn(current.competition, match)) return true;
  return false;
}
bool MatchStore::bind(uint8_t competition, uint32_t incarnation, uint32_t club,
                      const Clock& clock) {
  if (!competition) return false;
  auto* current = binding(competition);
  if (current && current->incarnation != incarnation) {
    unbind(competition, clock);
    current = nullptr;
  }
  if (!current)
    for (auto& item : bindings_)
      if (!item.competition) {
        item = {};
        item.incarnation = incarnation;
        item.club = club;
        item.competition = competition;
        return true;
      }
  if (!current) return false;
  if (current->club != club) {
    current->club = club;
    if (current->current != kNoSource) {
      fixture_changes_ |= uint16_t{1} << current->current;
      datasetResult(current->current, Dataset::kFixtures, false);
    }
    reconcileDetails(clock);
  }
  return true;
}
SourceId MatchStore::resolve(uint8_t competition, const SourceKey& key,
                             const Clock& clock) {
  auto* current = binding(competition);
  if (!current) return {};
  const auto id = source_index_.intern(key);
  if (!id) return {};
  const auto slot = sourceSlot(id);
  if (current->current == slot && includes(competition, slot)) return id;
  const bool covered = std::any_of(
      bindings_.begin(), bindings_.end(), [&](const Binding& other) {
        return other.competition && other.club == current->club &&
               includes(other.competition, slot);
      });
  sources_[slot].bindings |= bindingMask(competition);
  if (current->club && !covered) {
    fixture_changes_ |= uint16_t{1} << slot;
    datasetResult(slot, Dataset::kFixtures, false);
  }
  current->current = slot;
  reconcileDetails(clock);
  return id;
}
uint32_t MatchStore::resolvedAt(SourceSlot slot) const {
  const auto* source = status(slot);
  return source ? source->resolved_utc : 0;
}
void MatchStore::resolvedAt(SourceSlot slot, uint32_t utc) {
  if (auto* source = status(slot)) source->resolved_utc = utc;
}
bool MatchStore::retire(SourceId id) {
  const auto slot = sourceSlot(id);
  if (slot == kNoSource) return false;
  for (const auto& view : bindings_)
    if (view.competition && view.current == slot) return false;
  sources_[slot].bindings = 0;
  for (auto& view : bindings_)
    if (view.summary_target.source == id) {
      view.summary_target = {};
      view.summary = {};
    }
  releaseSource(id);
  collect();
  return true;
}
void MatchStore::unbind(uint8_t competition, const Clock& clock) {
  auto* current = binding(competition);
  if (!current) return;
  const auto mask = bindingMask(competition);
  for (auto& source : sources_) source.bindings &= ~mask;
  for (auto& detail : details_) detail.requesters &= ~mask;
  *current = {};
  reconcileDetails(clock);
  for (SourceSlot slot = 0; slot < sources_.size(); ++slot)
    if (!sources_[slot].bindings) releaseSource(sourceId(slot));
}
MatchStore::SourceStatus* MatchStore::status(SourceSlot slot) {
  return slot < sources_.size() && sourceId(slot) ? &sources_[slot] : nullptr;
}
const MatchStore::SourceStatus* MatchStore::status(SourceSlot slot) const {
  return slot < sources_.size() && sourceId(slot) ? &sources_[slot] : nullptr;
}
void MatchStore::reconcileDetails(const Clock& clock) {
  const auto misplaced = [&](uint8_t slot) {
    const auto* match = detailRow(slot);
    return match && (slot < cfg::kStoreOwnMatches) != isOwn(*match);
  };
  const auto exchange = [&](uint8_t first, uint8_t second) {
    // One compact detail temporary; event payloads stay in their sole pool.
    std::swap(details_[first], details_[second]);
    for (std::size_t i = 0; i < match_count_; ++i) {
      auto& slot = matches_[i].detail;
      if (slot == first)
        slot = second;
      else if (slot == second)
        slot = first;
    }
    for (auto& event : events_) {
      if (event.slot == first)
        event.slot = second;
      else if (event.slot == second)
        event.slot = first;
    }
  };
  // Opposite ownership changes can exchange directly, even with full banks.
  for (uint8_t own = 0; own < cfg::kStoreOwnMatches; ++own) {
    if (!misplaced(own)) continue;
    for (uint8_t other = cfg::kStoreOwnMatches; other < details_.size();
         ++other)
      if (misplaced(other)) {
        exchange(own, other);
        break;
      }
  }
  for (uint8_t slot = 0; slot < details_.size(); ++slot) {
    if (!misplaced(slot)) continue;
    auto* match = detailRow(slot);
    const std::size_t first = isOwn(*match) ? 0 : cfg::kStoreOwnMatches;
    const std::size_t end =
        isOwn(*match) ? cfg::kStoreOwnMatches : details_.size();
    uint8_t target = kNoIndex;
    for (std::size_t i = first; i < end; ++i)
      if (detailRow(static_cast<uint8_t>(i)) == nullptr) {
        target = static_cast<uint8_t>(i);
        break;
      }
    if (target == kNoIndex)
      target = detailSlot(*match, clock.utc,
                          isOwn(*match) ? 0 : details_[slot].requesters);
    if (target != kNoIndex)
      exchange(slot, target);
    else {
      freeSlot(slot);
      match->detail = kNoIndex;
    }
  }
  // Detail ownership and physical event banks must agree too. Opposite
  // moves exchange in place; a one-way move consumes only a genuinely free
  // event in the destination bank. Capacity loss stays explicit.
  for (uint8_t i = 0; i < events_.size(); ++i) {
    const uint8_t slot = events_[i].slot;
    if (slot >= details_.size()) continue;
    const bool own = slot < cfg::kStoreOwnMatches;
    if ((i < cfg::kStoreOwnEvents) == own) continue;
    const std::size_t first = own ? 0 : cfg::kStoreOwnEvents;
    const std::size_t end = own ? cfg::kStoreOwnEvents : events_.size();
    int target = -1;
    for (std::size_t j = first; j < end; ++j) {
      const uint8_t other = events_[j].slot;
      if (other == kNoIndex ||
          (other < details_.size() && (other < cfg::kStoreOwnMatches) != own)) {
        target = static_cast<int>(j);
        break;
      }
    }
    if (target >= 0) {
      std::swap(events_[i], events_[target]);
      for (auto& detail : details_)
        for (std::size_t n = 0; n < detail.event_count; ++n) {
          auto& index = detail.events[n];
          if (index == i)
            index = static_cast<uint8_t>(target);
          else if (index == target)
            index = i;
        }
    } else {
      auto& detail = details_[slot];
      for (std::size_t n = 0; n < detail.event_count; ++n)
        if (detail.events[n] == i) {
          for (std::size_t k = n + 1; k < detail.event_count; ++k)
            detail.events[k - 1] = detail.events[k];
          --detail.event_count;
          break;
        }
      events_[i] = {};
      if (auto* match = detailRow(slot))
        match->flags |= kMatchEventsTruncated | kMatchEventsPoolCut;
      note(detail.source, kNoticeEvents);
    }
  }
  for (std::size_t i = 0; i < match_count_; ++i) {
    auto& match = matches_[i];
    if (!isOwn(match) || match.detail != kNoIndex) continue;
    ensureDetail(key(match), 0, clock);
  }
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

int MatchStore::matchIndex(SourceSlot source, uint32_t id) const {
  if (!id || !sourceId(source)) return -1;
  for (std::size_t i = 0; i < match_count_; ++i)
    if (matches_[i].id == id && matches_[i].source == source)
      return static_cast<int>(i);
  return -1;
}
const StoredMatch* MatchStore::find(SourceSlot source, uint32_t id) const {
  const int i = matchIndex(source, id);
  return i < 0 ? nullptr : &matches_[i];
}
const StoredMatch* MatchStore::find(MatchKey match) const {
  return find(sourceSlot(match.source), match.id);
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
                               uint8_t requesters) {
  const std::size_t first = isOwn(m) ? 0 : cfg::kStoreOwnMatches;
  const std::size_t end = isOwn(m) ? cfg::kStoreOwnMatches : details_.size();
  // A configured view owns its interest, even when several views share one
  // source or detail. Never displace another view's requested Single.
  int victim = -1, victim_row = -1;
  uint64_t farthest = 0;
  bool same_source_slot = false;
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
    if (!isOwn(m) && (details_[i].requesters & ~requesters)) continue;
    const auto d = distance(matches_[row], utc);
    const bool same =
        requesters && !isOwn(m) && (details_[i].requesters & ~requesters) == 0;
    if (victim < 0 || (same && !same_source_slot) ||
        (same == same_source_slot && d > farthest)) {
      victim = static_cast<int>(i);
      victim_row = row;
      farthest = d;
      same_source_slot = same;
    }
  }
  if (!same_source_slot && free >= 0) return static_cast<uint8_t>(free);
  if (requesters && !same_source_slot && free < 0) return kNoIndex;
  if (victim < 0 || (!requesters && (utc == 0 || distance(m, utc) >= farthest)))
    return kNoIndex;
  auto& row = matches_[victim_row];
  row.detail = kNoIndex;
  row.flags &= ~(kMatchEventsTruncated | kMatchEventsPoolCut);
  freeSlot(static_cast<uint8_t>(victim));
  return static_cast<uint8_t>(victim);
}

bool MatchStore::hasDetail(const StoredMatch& m) const {
  return m.detail < details_.size() &&
         (m.detail < cfg::kStoreOwnMatches) == isOwn(m) &&
         details_[m.detail].match == m.id &&
         details_[m.detail].source == m.source;
}

StoredMatch* MatchStore::ensureDetail(MatchKey match, uint8_t competition,
                                      const Clock& clock) {
  const auto source = sourceSlot(match.source);
  const int at = matchIndex(source, match.id);
  if (at < 0 || (competition && !includes(competition, source))) return nullptr;
  auto& m = matches_[at];
  const uint8_t requester = bindingMask(competition);
  if (!isOwn(m) && !requester) return nullptr;
  if (!hasDetail(m)) {
    m.detail = detailSlot(m, clock.utc, isOwn(m) ? 0 : requester);
    if (m.detail == kNoIndex) return nullptr;
  }
  // Publish a new target only after it has a slot. Never clear another view.
  if (requester)
    for (auto& detail : details_)
      if (detail.match != m.id || detail.source != m.source)
        detail.requesters &= ~requester;
  auto& detail = details_[m.detail];
  detail.match = m.id;
  detail.source = m.source;
  detail.requesters |= requester;
  return &m;
}

StoredMatch* MatchStore::upsert(const StoredMatch& incoming,
                                const Clock& clock) {
  if (incoming.id == 0 || !sourceId(incoming.source)) return nullptr;
  StoredMatch m = incoming;
  m.detail = kNoIndex;
  m.flags &= ~(kMatchEventsTruncated | kMatchEventsPoolCut);
  uint8_t slot = kNoIndex;
  bool seen_running = false;
  const int at = matchIndex(m.source, m.id);
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
      if (isOwn(matches_[i])) continue;
      const auto d = distance(matches_[i], clock.utc);
      if (victim < 0 || d > farthest) {
        victim = static_cast<int>(i);
        farthest = d;
      }
    }
    if (clock.utc == 0 || victim < 0 ||
        (!isOwn(m) && distance(m, clock.utc) >= farthest)) {
      freeSlot(slot);
      note(m.source, kNoticeMatches);
      return nullptr;
    }
    note(matches_[victim].source, kNoticeMatches);
    removeMatch(static_cast<std::size_t>(victim));
  }
  // A provider correction can change club membership too. Preserve existing
  // requesters/events and let the sole bank reconciler move them after publish.
  const bool moved =
      slot != kNoIndex && (slot < cfg::kStoreOwnMatches) != isOwn(m);
  if (isOwn(m) && slot == kNoIndex) slot = detailSlot(m, clock.utc, false);
  if (slot != kNoIndex) {
    auto& detail = details_[slot];
    detail.match = m.id;
    detail.source = m.source;
  }
  m.detail = slot;
  if (slot == kNoIndex)
    m.flags &= ~(kMatchEventsTruncated | kMatchEventsPoolCut);
  const std::size_t pos = sortedPosition(m);
  for (std::size_t j = match_count_; j > pos; --j)
    matches_[j] = matches_[j - 1];
  matches_[pos] = m;
  ++match_count_;
  if (moved) reconcileDetails(clock);
  return &matches_[pos];
}

StoredMatch* MatchStore::place(StoredMatch m, std::size_t from) {
  for (std::size_t j = from + 1; j < match_count_; ++j)
    matches_[j - 1] = matches_[j];
  --match_count_;
  const std::size_t pos = sortedPosition(m);
  for (std::size_t j = match_count_; j > pos; --j)
    matches_[j] = matches_[j - 1];
  matches_[pos] = m;
  ++match_count_;
  return &matches_[pos];
}

bool MatchStore::unmaintained(SourceSlot source_slot, int64_t utc) const {
  for (std::size_t i = 0; i < match_count_; ++i) {
    const auto& m = matches_[i];
    if (m.source != source_slot || m.kickoff == 0 ||
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

void MatchStore::releaseSource(SourceId id) {
  const auto slot = sourceSlot(id);
  if (slot == kNoSource || sources_[slot].bindings) return;
  if (table_source_ == slot) commitTable(false);
  for (std::size_t i = match_count_; i-- > 0;)
    if (matches_[i].source == slot) removeMatch(i);
  dropRows(slot);
  dropGroups(slot);
  std::size_t kept = 0;
  for (std::size_t i = 0; i < round_count_; ++i)
    if (rounds_[i].source != slot) rounds_[kept++] = rounds_[i];
  round_count_ = static_cast<uint8_t>(kept);
  sources_[slot] = {};
  fixture_changes_ &= ~(uint16_t{1} << slot);
  source_index_.release(id);
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
  for (std::size_t i = 0; table_source_ != kNoSource && i < staged_rows_; ++i)
    if (stage_[i].team < teams.size()) teams.set(stage_[i].team);
  for (std::size_t i = 0; i < teams_.size(); ++i)
    if (!teams.test(i)) teams_[i] = {};
  for (std::size_t i = 0; i < round_count_; ++i)
    if (rounds_[i].label < labels.size()) labels.set(rounds_[i].label);
  for (std::size_t i = 0; i < group_count_; ++i)
    if (groups_[i].label < labels.size()) labels.set(groups_[i].label);
  for (std::size_t i = 0; table_source_ != kNoSource && i < staged_groups_; ++i)
    if (stage_groups_[i].label < labels.size())
      labels.set(stage_groups_[i].label);
  for (std::size_t i = 0; i < labels_.size(); ++i)
    if (!labels.test(i)) labels_[i] = {};
  // Events notices of own matches that left the store.
  for (SourceSlot slot = 0; slot < sources_.size(); ++slot)
    if (sources_[slot].bits & kNoticeEvents) settleEvents(slot);
}

void MatchStore::dropRows(SourceSlot source_slot) {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < row_count_; ++i)
    if (rows_[i].source != source_slot) rows_[kept++] = rows_[i];
  for (std::size_t i = kept; i < row_count_; ++i) rows_[i] = {};
  row_count_ = static_cast<uint16_t>(kept);
}

void MatchStore::beginTable(SourceSlot source_slot) {
  beginRun(source_slot, kNoticeTable | kNoticeTableTeams);
  staged_rows_ = 0;
  staged_groups_ = 0;
  table_source_ = source_slot;
  table_cut_ = false;
}

bool MatchStore::appendRow(const StoredRow& row) {
  if (table_source_ == kNoSource) return false;
  if (staged_rows_ == stage_.size()) {
    table_cut_ = true;
    note(table_source_, kNoticeTable);
    return false;
  }
  stage_[staged_rows_] = row;
  stage_[staged_rows_].source = table_source_;
  if (row.group == kNoIndex && staged_groups_)
    stage_[staged_rows_].group = staged_groups_ - 1;
  ++staged_rows_;
  return true;
}

void MatchStore::dropGroups(SourceSlot source_slot) {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < group_count_; ++i)
    if (groups_[i].source != source_slot) groups_[kept++] = groups_[i];
  group_count_ = static_cast<uint8_t>(kept);
}
bool MatchStore::appendGroup(std::string_view key, std::string_view caption,
                             uint8_t ordinal) {
  if (table_source_ == kNoSource) return false;
  if (ordinal == kNoIndex) ordinal = staged_groups_;
  if (ordinal > staged_groups_ || ordinal >= stage_groups_.size()) {
    table_cut_ = true;
    note(table_source_, kNoticeTable);
    return false;
  }
  auto& group = stage_groups_[ordinal];
  group = {};
  group.source = table_source_;
  group.ordinal = ordinal;
  if (ordinal == staged_groups_) ++staged_groups_;
  group.label = label(caption.empty() ? key : caption);
  if (!(caption.empty() && key.empty()) && group.label == kNoIndex) {
    table_cut_ = true;
    note(table_source_, kNoticeTable);
    return false;
  }
  return true;
}
void MatchStore::datasetResult(SourceSlot source_slot, Dataset dataset,
                               bool complete) {
  if (auto* item = status(source_slot))
    item->datasets[static_cast<unsigned>(dataset)].state =
        complete && !(notices(source_slot) & noticeKinds(dataset))
            ? DatasetState::kComplete
            : DatasetState::kPartial;
}
void MatchStore::datasetAvailable(SourceSlot source_slot, Dataset dataset,
                                  bool available) {
  if (auto* item = status(source_slot)) {
    auto& status = item->datasets[static_cast<unsigned>(dataset)];
    if (!available) {
      status = {};
      status.state = DatasetState::kUnavailable;
    } else if (status.state == DatasetState::kUnavailable) {
      status = {};
    }
  }
}
DatasetState MatchStore::datasetState(SourceSlot source_slot,
                                      Dataset dataset) const {
  return datasetStatus(source_slot, dataset).state;
}
DatasetStatus MatchStore::datasetStatus(SourceSlot source_slot,
                                        Dataset dataset) const {
  const auto* item = status(source_slot);
  return item ? item->datasets[static_cast<unsigned>(dataset)]
              : DatasetStatus{};
}
Cause MatchStore::datasetNotice(SourceSlot source_slot, Dataset dataset,
                                int64_t utc) const {
  if (dataset == Dataset::kFixtures && unmaintained(source_slot, utc))
    return Cause::kUnmaintained;
  return notices(source_slot) & noticeKinds(dataset) ? Cause::kStoreFull
                                                     : Cause::kNone;
}
DatasetSummary MatchStore::datasetSummary(SourceSlot slot) const {
  DatasetSummary out;
  if (const auto* source = status(slot))
    for (const auto& dataset : source->datasets) summarize(out, dataset);
  return out;
}
bool MatchStore::recordResolution(uint8_t competition, int64_t attempt_ms,
                                  Cause cause, uint16_t http) {
  if (auto* view = binding(competition)) {
    const auto status = cause == Cause::kNone ? uint16_t{0} : http;
    const bool changed =
        (attempt_ms > 0 && view->resolution_attempt_ms != attempt_ms) ||
        view->resolution_cause != cause || view->resolution_http != status;
    if (attempt_ms > 0) view->resolution_attempt_ms = attempt_ms;
    view->resolution_cause = cause;
    view->resolution_http = status;
    return changed;
  }
  return false;
}
bool MatchStore::summaryTarget(uint8_t competition, MatchKey target) {
  auto* view = binding(competition);
  if (!view || !target.valid() || target == view->summary_target ||
      !includes(competition, sourceSlot(target.source)))
    return false;
  view->summary_target = target;
  view->summary = {};
  return true;
}
void MatchStore::recordSummary(uint8_t competition, MatchKey expected,
                               int64_t attempt_ms, int64_t completed_ms,
                               bool complete, Cause cause, uint16_t http) {
  auto* view = binding(competition);
  if (!view || !expected.valid() || view->summary_target != expected) return;
  auto& status = view->summary;
  if (attempt_ms > 0) status.last_attempt_ms = attempt_ms;
  status.state =
      complete && !(notices(sourceSlot(expected.source)) & kNoticeEvents)
          ? DatasetState::kComplete
          : DatasetState::kPartial;
  if (complete) status.last_success_ms = completed_ms;
  status.cause = complete ? Cause::kNone : cause;
  status.http = complete || cause == Cause::kNone ? 0 : http;
}
DatasetStatus MatchStore::viewDatasetStatus(uint8_t competition,
                                            Dataset dataset) const {
  const auto* view = binding(competition);
  if (!view) return {};
  auto out = dataset == Dataset::kEvents && view->summary_target.valid()
                 ? view->summary
                 : datasetStatus(view->current, dataset);
  int64_t failure_at = out.cause == Cause::kNone ? 0 : out.last_attempt_ms;
  if (dataset != Dataset::kTable)
    for (SourceSlot slot = 0; slot < sources_.size(); ++slot) {
      if (!includes(competition, slot) ||
          (slot == view->current &&
           !(dataset == Dataset::kEvents && view->summary_target.valid())))
        continue;
      const auto old = datasetStatus(slot, dataset);
      out.last_success_ms = std::max(out.last_success_ms, old.last_success_ms);
      if (old.cause != Cause::kNone &&
          (out.cause == Cause::kNone || old.last_attempt_ms > failure_at)) {
        out.cause = old.cause;
        out.http = old.http;
        failure_at = old.last_attempt_ms;
      }
      out.last_attempt_ms = std::max(out.last_attempt_ms, old.last_attempt_ms);
      if (old.state == DatasetState::kPartial &&
          out.state == DatasetState::kComplete)
        out.state = DatasetState::kPartial;
    }
  if (dataset == Dataset::kFixtures && view->resolution_cause != Cause::kNone) {
    out.cause = view->resolution_cause;
    out.http = view->resolution_http;
    out.last_attempt_ms =
        std::max(out.last_attempt_ms, view->resolution_attempt_ms);
  }
  return out;
}
Cause MatchStore::viewDatasetNotice(uint8_t competition, Dataset dataset,
                                    int64_t utc) const {
  Cause out = Cause::kNone;
  for (SourceSlot slot = 0; slot < sources_.size(); ++slot) {
    if (!includes(competition, slot) ||
        (dataset == Dataset::kTable && slot != currentSource(competition)))
      continue;
    const auto notice = datasetNotice(slot, dataset, utc);
    if (notice == Cause::kStoreFull) return notice;
    if (notice != Cause::kNone) out = notice;
  }
  return out;
}
DatasetSummary MatchStore::viewSummary(uint8_t competition) const {
  DatasetSummary out;
  const auto* view = binding(competition);
  if (!view) return out;
  for (SourceSlot slot = 0; slot < sources_.size(); ++slot) {
    if (!includes(competition, slot)) continue;
    for (const auto dataset :
         {Dataset::kFixtures, Dataset::kTable, Dataset::kEvents})
      if (dataset != Dataset::kTable || slot == view->current)
        summarize(out, sources_[slot].datasets[static_cast<unsigned>(dataset)]);
  }
  summarize(out, view->summary);
  DatasetStatus resolution;
  resolution.last_attempt_ms = view->resolution_attempt_ms;
  resolution.cause = view->resolution_cause;
  resolution.http = view->resolution_http;
  summarize(out, resolution);
  return out;
}
void MatchStore::recordDataset(SourceSlot source_slot, Dataset dataset,
                               int64_t attempt_ms, int64_t completed_ms,
                               bool complete, Cause cause, uint16_t http) {
  auto* item = status(source_slot);
  if (!item) return;
  auto& status = item->datasets[static_cast<unsigned>(dataset)];
  if (attempt_ms > 0) status.last_attempt_ms = attempt_ms;
  if (complete || (completed_ms > 0 && cause == Cause::kNone)) {
    status.last_success_ms = completed_ms;
    status.cause = Cause::kNone;
    status.http = 0;
  } else {
    if (cause != Cause::kNone) {
      status.cause = cause;
      status.http = http;
    }
  }
  datasetResult(source_slot, dataset, complete);
}

DatasetState MatchStore::tableState(SourceSlot source_slot) const {
  return datasetState(source_slot, Dataset::kTable);
}
void MatchStore::roundResult(SourceSlot source_slot, uint8_t number,
                             uint8_t label, uint16_t matches, bool complete,
                             uint32_t from, uint32_t to, RoundKind kind) {
  if (!sourceId(source_slot) || (!number && label == kNoIndex)) return;
  StoredRound* destination = nullptr;
  for (std::size_t i = 0; i < round_count_; ++i)
    if (rounds_[i].source == source_slot && rounds_[i].number == number &&
        rounds_[i].label == label && rounds_[i].kind == kind)
      destination = &rounds_[i];
  if (!destination) {
    if (round_count_ < rounds_.size())
      destination = &rounds_[round_count_++];
    else {
      // A new scope cannot erase an unrelated verified round silently.
      note(source_slot, kNoticeMatches);
      datasetResult(source_slot, Dataset::kFixtures, false);
      return;
    }
  }
  *destination = {from,
                  to,
                  matches,
                  source_slot,
                  number,
                  label,
                  complete ? DatasetState::kComplete : DatasetState::kPartial,
                  kind};
}
void MatchStore::retain(SourceSlot source_slot,
                        const retention::Selection& selected, int64_t utc,
                        DateContext dates) {
  for (std::size_t i = match_count_; i-- > 0;) {
    const auto& match = matches_[i];
    if (match.source != source_slot) continue;
    const auto label = match.label < labels_.size()
                           ? std::string_view(labels_[match.label].data)
                           : std::string_view{};
    if (((match.flags & kMatchRefresh) &&
         selected.contains(match.round, label, match.kickoff, match.id)) ||
        resultHeld(match, utc, dates))
      continue;
    removeMatch(i);
  }
  uint8_t kept = 0;
  for (uint8_t i = 0; i < round_count_; ++i) {
    auto round = rounds_[i];
    if (round.source != source_slot) {
      rounds_[kept++] = round;
      continue;
    }
    const auto label = round.label < labels_.size()
                           ? std::string_view(labels_[round.label].data)
                           : std::string_view{};
    if (selected.roundIndex(round.number, label) >= 0) {
      rounds_[kept++] = round;
      continue;
    }
    uint16_t retained = 0;
    for (std::size_t m = 0; m < match_count_; ++m)
      if (matches_[m].source == source_slot &&
          matches_[m].round == round.number && matches_[m].label == round.label)
        ++retained;
    if (retained) {
      round.matches = retained;
      round.state = DatasetState::kPartial;
      rounds_[kept++] = round;
    }
  }
  round_count_ = kept;
}
void MatchStore::beginFixtureRun(SourceSlot source_slot) {
  for (std::size_t i = 0; i < match_count_; ++i)
    if (matches_[i].source == source_slot) matches_[i].flags &= ~kMatchRefresh;
  beginRun(source_slot, kNoticeFixtures);
}
void MatchStore::markFixture(SourceSlot source_slot, uint32_t match) {
  const int index = matchIndex(source_slot, match);
  if (index >= 0) matches_[index].flags |= kMatchRefresh;
}
bool MatchStore::commitTable(bool complete) {
  const SourceSlot source_slot = table_source_;
  const std::size_t staged = staged_rows_, staged_groups = staged_groups_;
  table_source_ = kNoSource;
  staged_rows_ = 0;
  staged_groups_ = 0;
  if (!sourceId(source_slot)) return false;
  if (complete || staged || staged_groups || table_cut_)
    datasetResult(source_slot, Dataset::kTable, false);
  if (!complete || table_cut_) return false;
  const auto stored = static_cast<std::size_t>(std::count_if(
      rows_.begin(), rows_.begin() + row_count_,
      [&](const StoredRow& r) { return r.source == source_slot; }));
  const auto old_groups = static_cast<std::size_t>(
      std::count_if(groups_.begin(), groups_.begin() + group_count_,
                    [&](const StoredTableGroup& group) {
                      return group.source == source_slot;
                    }));
  if (row_count_ - stored + staged > rows_.size() ||
      group_count_ - old_groups + staged_groups > groups_.size()) {
    note(source_slot, kNoticeTable);
    return false;
  }
  dropRows(source_slot);
  dropGroups(source_slot);
  std::copy_n(stage_.begin(), staged, rows_.begin() + row_count_);
  row_count_ = static_cast<uint16_t>(row_count_ + staged);
  std::copy_n(stage_groups_.begin(), staged_groups,
              groups_.begin() + group_count_);
  group_count_ = static_cast<uint8_t>(group_count_ + staged_groups);
  settleRun(source_slot, kNoticeTable | kNoticeTableTeams);
  datasetResult(source_slot, Dataset::kTable, true);
  return true;
}

bool MatchStore::beginEvents(const StoredMatch& match, bool detail,
                             int64_t utc) {
  pending_slot_ = kNoIndex;
  if (!hasDetail(match)) return false;
  pending_slot_ = match.detail;
  pending_utc_ = utc;
  pending_count_ = 0;
  pending_shots_.fill(0);
  pending_shot_order_ = 0;
  pending_cut_ = false;
  pending_pool_ = false;
  pending_detail_ = detail;
  return true;
}

void MatchStore::addEvent(const EventFacts& incoming) {
  if (pending_slot_ == kNoIndex) return;
  EventFacts event = incoming;
  event.flags &= ~kEventDetail;
  if (pending_detail_) event.flags |= kEventDetail;
  const bool shot = (event.flags & kEventShootout) != 0;
  if (shot) event.ordinal = pending_shot_order_++;
  if (!shot && pending_count_ == pending_.size()) {
    // Keep the latest events: reuse the entry of the oldest new one.
    const uint8_t reuse = pending_[0];
    for (std::size_t i = 1; i < pending_count_; ++i)
      pending_[i - 1] = pending_[i];
    pending_[pending_count_ - 1] = reuse;
    static_cast<EventFacts&>(events_[reuse]) = event;
    events_[reuse].slot = kPendingSlot;
    pending_cut_ = true;
    return;
  }
  // The previous entry of the same event is updated in place, so an update
  // needs pool room only for new events.
  int at = -1;
  for (std::size_t i = eventBegin(pending_slot_);
       i < eventEnd(pending_slot_) && at < 0; ++i)
    if ((events_[i].slot == pending_slot_ ||
         (shot && (pending_shots_[i / 32] & (uint32_t{1} << (i % 32))))) &&
        events_[i].identity == event.identity &&
        (events_[i].flags & kEventDetail) == (event.flags & kEventDetail))
      at = static_cast<int>(i);
  for (std::size_t i = eventBegin(pending_slot_);
       i < eventEnd(pending_slot_) && at < 0; ++i)
    if (events_[i].slot == kNoIndex) at = static_cast<int>(i);
  if (at < 0) at = takeEvent();
  if (at >= 0) {
    static_cast<EventFacts&>(events_[at]) = event;
    events_[at].slot = kPendingSlot;
    if (shot)
      pending_shots_[at / 32] |= uint32_t{1} << (at % 32);
    else
      pending_[pending_count_++] = static_cast<uint8_t>(at);
    return;
  }
  pending_cut_ = pending_pool_ = true;
  note(details_[pending_slot_].source, kNoticeEvents);
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
        !sameDetailBank(m.detail, pending_slot_))
      continue;
    bool has_event = false;
    for (std::size_t e = eventBegin(m.detail); e < eventEnd(m.detail); ++e)
      has_event |= events_[e].slot == m.detail;
    if (!has_event) continue;
    const auto d = distance(m, pending_utc_);
    if (victim < 0 || d > farthest) {
      victim = m.detail;
      victim_row = &m;
      farthest = d;
    }
  }
  if (victim < 0 || distance(*mine, pending_utc_) >= farthest) return -1;
  auto& own = details_[victim];
  uint8_t taken = kNoIndex;
  if (own.event_count) {
    taken = own.events[0];
    for (std::size_t n = 1; n < own.event_count; ++n)
      own.events[n - 1] = own.events[n];
    --own.event_count;
  } else {
    for (std::size_t e = eventBegin(static_cast<uint8_t>(victim));
         e < eventEnd(static_cast<uint8_t>(victim)); ++e)
      if (events_[e].slot == victim &&
          (taken == kNoIndex || events_[e].ordinal < events_[taken].ordinal))
        taken = static_cast<uint8_t>(e);
  }
  if (taken == kNoIndex) return -1;
  events_[taken] = {};
  victim_row->flags |= kMatchEventsTruncated | kMatchEventsPoolCut;
  note(own.source, kNoticeEvents);
  return taken;
}

StoredMatch* MatchStore::detailRow(uint8_t slot) {
  for (std::size_t i = 0; i < match_count_; ++i)
    if (matches_[i].detail == slot) return &matches_[i];
  return nullptr;
}

void MatchStore::settleEvents(SourceSlot source_slot) {
  for (std::size_t i = 0; i < match_count_; ++i) {
    const auto& m = matches_[i];
    if (m.source == source_slot && m.detail < details_.size() &&
        (m.flags & (kMatchEventsPoolCut | kMatchEventsTruncated))) {
      flag(source_slot, kNoticeEvents, true);
      return;
    }
  }
  flag(source_slot, kNoticeEvents, false);
}

void MatchStore::endEvents(bool complete) {
  if (pending_slot_ == kNoIndex) return;
  const uint8_t s = pending_slot_;
  pending_slot_ = kNoIndex;
  auto& own = details_[s];
  StoredMatch* row = detailRow(s);
  const uint8_t origin = pending_detail_ ? kEventDetail : 0;
  const bool whole_shots = complete && !pending_cut_ && row != nullptr &&
                           !(row->flags & kMatchEventsTruncated);
  for (std::size_t at = eventBegin(s); at < eventEnd(s); ++at) {
    auto& event = events_[at];
    const bool seen =
        (pending_shots_[at / 32] & (uint32_t{1} << (at % 32))) != 0;
    if (seen) {
      event.slot = s;
      continue;
    }
    if (event.slot != s || !(event.flags & kEventShootout) ||
        (event.flags & kEventDetail) != origin)
      continue;
    if (!complete) continue;
    if (whole_shots && !(event.flags & kEventRemoved))
      event.flags |= kEventRemoved;
    else
      event = {};
  }
  if (!complete) {
    // Broken off: the previous list (updated in place where seen again)
    // followed by the new events; the oldest go when it overflows.
    std::array<uint8_t, cfg::kMatchEvents + cfg::kMatchEvents> merged{};
    std::size_t count = 0;
    for (std::size_t n = 0; n < own.event_count; ++n)
      if (!(events_[own.events[n]].flags & kEventShootout))
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
    if (row != nullptr) settleEvents(row->source);
    return;
  }
  const bool whole =
      !pending_cut_ && row != nullptr && !(row->flags & kMatchEventsTruncated);
  // The other origin's events stay ahead of the new list.
  std::array<uint8_t, cfg::kMatchEvents + cfg::kMatchEvents> list{};
  std::size_t count = 0;
  // Previous entries of this origin still held by the slot are missing from
  // the new list (present ones were updated in place by addEvent()).
  for (std::size_t n = 0; n < own.event_count; ++n) {
    auto& previous = events_[own.events[n]];
    if (previous.flags & kEventShootout) continue;
    if (previous.slot != s) continue;
    if ((previous.flags & kEventDetail) != origin) {
      list[count++] = own.events[n];
      continue;
    }
    if (whole && !(previous.flags & kEventRemoved)) {
      if (pending_count_ < pending_.size()) {
        previous.flags |= kEventRemoved;
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
  settleEvents(own.source);
}

void MatchStore::note(SourceSlot slot, uint8_t bits) {
  if (auto* source = status(slot)) {
    source->bits |= bits;
    source->noted |= bits;
  }
}
void MatchStore::beginRun(SourceSlot slot, uint8_t bits) {
  if (auto* source = status(slot)) source->noted &= ~bits;
}
void MatchStore::settleRun(SourceSlot slot, uint8_t bits) {
  if (auto* source = status(slot))
    source->bits = (source->bits & ~bits) | (source->noted & bits);
}
void MatchStore::flag(SourceSlot slot, uint8_t bits, bool on) {
  if (on)
    note(slot, bits);
  else if (auto* source = status(slot))
    source->bits &= ~bits;
}
uint8_t MatchStore::notices(SourceSlot slot) const {
  const auto* source = status(slot);
  return source ? source->bits : 0;
}

std::size_t MatchStore::teamCount() const {
  return static_cast<std::size_t>(std::count_if(
      teams_.begin(), teams_.end(), [](const auto& t) { return t.id != 0; }));
}

uint32_t decimalId(std::string_view text) {
  return values::positiveUint32(text);
}

}  // namespace football
