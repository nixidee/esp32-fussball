#include "provider_plan.h"

#include <algorithm>
#include <new>

#include "civil_time.h"
#include "football_model.h"
#include "matchday.h"

namespace football::plan {
namespace {

using cfg::DataType;

struct OwnRequest {
  Sink sink;
  bool upcoming;
};
constexpr std::array kOwnRequests{
    OwnRequest{Sink::kOwnIndex, false}, OwnRequest{Sink::kOwnIndex, true},
    OwnRequest{Sink::kOwnMatches, false}, OwnRequest{Sink::kOwnMatches, true}};
constexpr uint8_t kFirstOwnStep = 3;
constexpr uint8_t kOwnEndStep = kFirstOwnStep + kOwnRequests.size();
const OwnRequest* ownRequest(uint8_t step) {
  return step >= kFirstOwnStep && step < kOwnEndStep
             ? &kOwnRequests[step - kFirstOwnStep]
             : nullptr;
}
bool distinctClub(const settings::CompetitionRoutes& routes,
                  const MatchStore& store, SourceSlot source, uint8_t route) {
  const auto& entry = routes.entries[route];
  if (!entry.active || !entry.club.team || !store.includes(entry.id, source))
    return false;
  for (uint8_t prior = 0; prior < route; ++prior) {
    const auto& other = routes.entries[prior];
    if (other.active && other.club.team == entry.club.team &&
        store.includes(other.id, source))
      return false;
  }
  return true;
}

std::size_t index(DataType type) { return static_cast<std::size_t>(type); }

bool seasonStale(const MatchStore& store, SourceSlot source, int64_t utc) {
  if (source == kNoSource) return true;
  if (utc <= 0) return false;
  const auto at = store.resolvedAt(source);
  return at == 0 || utc < at || utc - at >= cfg::kSeasonRecheckS;
}
void applyCapabilities(MatchStore& store, SourceSlot source) {
  if (const auto* key = store.sourceKey(source)) {
    const auto supported = provider::capabilities(key->provider);
    store.datasetAvailable(source, MatchStore::Dataset::kFixtures,
                           supported.fixtures);
    store.datasetAvailable(source, MatchStore::Dataset::kTable,
                           supported.table);
    store.datasetAvailable(source, MatchStore::Dataset::kEvents,
                           supported.events);
  }
}
uint8_t bit(DataType type) { return static_cast<uint8_t>(1u << index(type)); }

// A preview has no summary yet. A confirmed/estimated running row, justified
// suspension or a retained result can supply the same available detail data.
bool eventReady(const StoredMatch& m) {
  return running(m.phase) || m.phase == Phase::kFinished ||
         (m.phase == Phase::kSuspended && m.first_live_observed_utc != 0);
}

uint64_t distance(uint32_t kickoff, int64_t utc) {
  if (kickoff == 0 || utc <= 0) return UINT64_MAX;
  return static_cast<uint64_t>(kickoff > utc ? kickoff - utc : utc - kickoff);
}

SourceState::Mark* findMark(SourceState& state, uint32_t round) {
  for (auto& mark : state.marks)
    if (mark.round != 0 && mark.round == round) return &mark;
  return nullptr;
}

}  // namespace

std::size_t matchdayDays(const uint32_t* days, std::size_t count, int64_t utc,
                         uint32_t* out, std::size_t capacity, bool* complete) {
  return matchday::retainedDays(days, count, utc, out, capacity, complete);
}

uint32_t changeHash(std::string_view text) {
  const uint32_t h = values::fnv1a32(text);
  return h == 0 ? 1 : h;
}

void Planner::context(const settings::CompetitionRoutes& competitions,
                      const settings::screen_policy::Eligibility& eligibility,
                      DateContext dates, bool break_proven) {
  competitions_ = &competitions;
  eligibility_ = &eligibility;
  dates_ = dates;
  break_proven_ = break_proven;
}

void ResponseDetails::clear() {
  events.reset();
  seen_count = 0;
  receiving = -1;
  ambiguous = false;
  for (auto& candidate : candidates) {
    candidate.match = {};
    candidate.kickoff = 0;
    candidate.count = 0;
    candidate.truncated = false;
  }
}

void SourceState::clear() {
  this->~SourceState();
  new (this) SourceState{};
}

void Planner::reset() {
  for (auto& state : states_) state.clear();
  resolving_.clear();
  fixtures_.clear();
  current_ = nullptr;
  store_ = nullptr;
  entry_ = nullptr;
  response_details_.clear();
  retention_index_.reset();
}
void Planner::forget(uint8_t slot) {
  if (slot >= states_.size()) return;
  if (current_ == &states_[slot]) abort();
  if (fixtures_.source == states_[slot].source) fixtures_.clear();
  states_[slot].clear();
}
void Planner::restartFixtures(uint16_t sources) {
  for (SourceSlot slot = 0; slot < states_.size(); ++slot)
    if (sources & (uint16_t{1} << slot)) {
      states_[slot].progress[index(DataType::kFixtures)] = {};
      if (fixtures_.source == states_[slot].source) fixtures_.clear();
    }
}
bool Planner::synchronize(MatchStore& store, const Clock& clock) {
  if (!competitions_) return false;
  bool changed = false;
  for (const auto& entry : competitions_->entries) {
    if (!entry.active || !entry.id) continue;
    if (!store.bind(entry.id, entry.incarnation, entry.club.team, clock))
      continue;
    if (!entry.primary.season.empty()) {
      SourceKey key;
      const auto before = store.currentSource(entry.id);
      if (key.assign(entry.primary.provider, entry.primary.competition.view(),
                     entry.primary.season.view()))
        changed |= store.recordResolution(entry.id, 0,
                                          store.resolve(entry.id, key, clock)
                                              ? Cause::kNone
                                              : Cause::kStoreFull);
      changed |= before != store.currentSource(entry.id);
    }
  }
  // Old sources survive while any configured view can still need a held,
  // active or upcoming match. Current sources are never retired here.
  const selection::Input input{eligibility_
                                   ? eligibility_->selection
                                   : settings::screen_policy::SelectionRules{},
                               *competitions_, store, clock, dates_};
  for (SourceSlot slot = 0; slot < states_.size(); ++slot) {
    const auto id = store.sourceId(slot);
    if (!id) {
      forget(slot);
      continue;
    }
    applyCapabilities(store, slot);
    if (states_[slot].source != id) {
      forget(slot);
      states_[slot].source = id;
    }
    bool current = false, needed = fixtures_.source == id;
    for (const auto& entry : competitions_->entries) {
      if (!entry.active || !store.includes(entry.id, slot)) continue;
      current |= store.currentSource(entry.id) == slot;
      for (std::size_t i = 0; !needed && i < store.matchCount(); ++i) {
        const auto& match = store.match(i);
        if (match.source != slot) continue;
        needed = resultHeld(match, clock.utc, dates_) ||
                 (match.kickoff && match.kickoff > clock.utc) ||
                 selection::window(input, entry, match).active;
      }
    }
    if (!current && !needed && store.retire(id)) {
      forget(slot);
      changed = true;
    }
  }
  if (fixtures_.source && store.sourceSlot(fixtures_.source) == kNoSource)
    fixtures_.clear();
  return changed;
}
uint8_t Planner::nextClub(uint8_t first) const {
  if (competitions_ && store_)
    for (uint8_t i = first; i < competitions_->entries.size(); ++i)
      if (distinctClub(*competitions_, *store_, map_.source, i)) return i;
  return cfg::kCompetitionMax;
}
uint32_t Planner::currentClub() const {
  return competitions_ && fixtures_.club_route < competitions_->entries.size()
             ? competitions_->entries[fixtures_.club_route].club.team
             : 0;
}
retention::Index* Planner::makeRetentionIndex(bool calendar) const {
  if (sink_ != Sink::kIndex) {
    auto* result = new (std::nothrow)
        retention::Index(currentClub(), map_.clock.utc, dates_, calendar);
    const auto* own =
        current_
            ? ownRequest(current_->progress[index(DataType::kFixtures)].step)
            : nullptr;
    if (result && sink_ == Sink::kOwnIndex && own && own->upcoming)
      result->seedOwn(currentClub(), fixtures_.own_candidates);
    return result;
  }
  std::array<uint32_t, cfg::kCompetitionMax> clubs{};
  std::size_t count = 0;
  if (competitions_)
    for (uint8_t i = 0; i < competitions_->entries.size(); ++i)
      if (distinctClub(*competitions_, *store_, map_.source, i))
        clubs[count++] = competitions_->entries[i].club.team;
  return new (std::nothrow)
      retention::Index(std::span<const uint32_t>(clubs.data(), count),
                       map_.clock.utc, dates_, calendar);
}
void Planner::sourceWanted(SourceSlot slot, const MatchStore& store,
                           const Clock& clock, schedule::Wanted& out,
                           const selection::Selection* selected) const {
  out = {};
  const auto* key = store.sourceKey(slot);
  if (!key || !competitions_ || (eligibility_ && eligibility_->frame)) return;
  unsigned clubs = 0;
  bool current = false;
  for (uint8_t i = 0; i < competitions_->entries.size(); ++i) {
    const auto& entry = competitions_->entries[i];
    if (!entry.active || !store.includes(entry.id, slot)) continue;
    if (!out.key.valid()) {
      out.key.source = store.sourceId(slot);
      out.competition = entry.id;
      out.route = i;
      out.provider = key->provider;
    }
    if (distinctClub(*competitions_, store, slot, i)) ++clubs;
    if (store.currentSource(entry.id) != slot) continue;
    current |=
        !entry.primary.season.empty() || !seasonStale(store, slot, clock.utc);
    if (eligibility_ && selected &&
        eligibility_->allows(selected->phase, screen::Type::kTable, i))
      out.types |= bit(DataType::kTable) | bit(DataType::kLiveTable);
  }
  if (!out.key.valid()) return;
  out.types |= bit(DataType::kLive);
  if (current) out.types |= bit(DataType::kFixtures);
  if (fixtures_.source && fixtures_.source != out.key.source)
    out.held |= bit(DataType::kFixtures);
  // Resolve an automatic peer before selecting the shared club union. A failed
  // resolution cannot indefinitely block an independently usable explicit year.
  for (const auto& peer : competitions_->entries)
    if (peer.active && peer.primary.season.empty() &&
        peer.primary.provider == key->provider &&
        peer.primary.competition == key->competition &&
        seasonStale(store, store.currentSource(peer.id), clock.utc) &&
        store.viewDatasetStatus(peer.id, MatchStore::Dataset::kFixtures)
                .cause == Cause::kNone)
      out.held |= bit(DataType::kFixtures);
  if (selected && selected->focus.source == out.key.source)
    out.priority_match = selected->focus.id;
  const auto protocol = provider::profile(key->provider);
  out.cost.fill(1);
  out.cost[index(DataType::kLive)] =
      protocol.live == provider::LiveFlow::kRoundChange ? 2 : 1;
  out.cost[index(DataType::kFixtures)] =
      protocol.fixtures == provider::FixtureFlow::kCalendar
          ? static_cast<uint8_t>(1 +
                                 (states_[slot].day_count
                                      ? states_[slot].day_count
                                      : cfg::kFixtureCalendarEstimatedDays) +
                                 kOwnRequests.size() * clubs)
          : 2;
}
void Planner::wanted(uint8_t slot, const settings::CompetitionRoute* entry,
                     const MatchStore& store, const Clock& clock,
                     schedule::Wanted& out,
                     const selection::Selection* current_selection) const {
  out = {};
  if (!entry || !entry->active || !entry->id || slot >= cfg::kCompetitionMax ||
      entry->primary.competition.empty() ||
      (eligibility_ && eligibility_->frame))
    return;
  out.key.competition = entry->id;
  out.key.incarnation = entry->incarnation;
  out.competition = entry->id;
  out.route = slot;
  out.provider = entry->primary.provider;
  out.cost.fill(1);
  if (entry->primary.season.empty() &&
      seasonStale(store, store.currentSource(entry->id), clock.utc)) {
    bool first = true;
    if (competitions_)
      for (uint8_t i = 0; i < slot; ++i) {
        const auto& prior = competitions_->entries[i];
        if (prior.active && prior.primary == entry->primary) first = false;
      }
    if (first) out.types |= bit(DataType::kFixtures);
    if (fixtures_.source) out.held |= bit(DataType::kFixtures);
  }
  if (!eligibility_ || !competitions_) return;
  const selection::Input input{eligibility_->selection,
                               *competitions_,
                               store,
                               clock,
                               dates_,
                               break_proven_};
  const auto selected =
      current_selection ? *current_selection : selection::select(input);
  if (provider::profile(entry->primary.provider).separate_events &&
      eligibility_->allows(selected.phase, screen::Type::kLiveSingle, slot)) {
    const auto single = selection::single(input, selected, *entry);
    if (const auto* match = selection::find(input, single.match)) {
      out.types |= bit(DataType::kEvents);
      out.event_match = single.match;
      out.event_priority = selected.phase == selection::ScreenPhase::kOwn &&
                           selected.competition == entry->id;
      if (!eventReady(*match)) out.held |= bit(DataType::kEvents);
    }
  }
}
bool Planner::prepareDemand(MatchStore& store, const Clock& clock,
                            schedule::Inputs& out,
                            const selection::Selection* selected) const {
  selection::Selection derived;
  if (!selected && competitions_ && eligibility_) {
    const selection::Input input{eligibility_->selection,
                                 *competitions_,
                                 store,
                                 clock,
                                 dates_,
                                 break_proven_};
    derived = selection::select(input);
    selected = &derived;
  }
  out.scopes = {};
  for (SourceSlot slot = 0; slot < cfg::kStoreSources; ++slot)
    sourceWanted(slot, store, clock, out.scopes[slot], selected);
  if (!competitions_) return false;
  bool changed = false;
  for (uint8_t i = 0; i < competitions_->entries.size(); ++i) {
    auto& view = out.scopes[cfg::kStoreSources + i];
    wanted(i, &competitions_->entries[i], store, clock, view, selected);
    if (!view.event_match.valid()) continue;
    changed |= store.summaryTarget(view.competition, view.event_match);
    for (uint8_t j = 0; j < i; ++j) {
      auto& prior = out.scopes[cfg::kStoreSources + j];
      if (!(prior.types & bit(DataType::kEvents)) ||
          prior.event_match != view.event_match)
        continue;
      prior.event_priority |= view.event_priority;
      view.types &= ~bit(DataType::kEvents);
      break;
    }
  }
  return changed;
}
bool Planner::request(const schedule::Job& job,
                      const settings::CompetitionRoute& entry,
                      MatchStore& store, const Clock& clock, Request& out) {
  current_ = nullptr;
  store_ = &store;
  entry_ = &entry;
  resolving = !job.key.source && job.type == DataType::kFixtures;
  if (!entry.active || entry.primary.competition.empty() ||
      job.competition != entry.id ||
      (!job.key.source && (job.key.competition != entry.id ||
                           job.key.incarnation != entry.incarnation)))
    return false;
  type_ = job.type;
  map_ = {};
  map_.clock = clock;
  source_ = entry.primary;
  event_match_ = job.event_match.id;
  if (resolving) {
    if (fixtures_.source) return false;
    resolving_.clear();
    current_ = &resolving_;
  } else {
    map_.source = store.sourceSlot(job.source());
    const auto* key = store.sourceKey(map_.source);
    if (!key || !store.includes(entry.id, map_.source)) return false;
    source_.provider = key->provider;
    source_.competition = key->competition;
    source_.season = key->season;
    auto& state = states_[map_.source];
    if (state.source != job.source()) {
      state.clear();
      state.source = job.source();
    }
    current_ = &state;
    if (type_ == DataType::kEvents) {
      schedule::Wanted current;
      wanted(job.route, &entry, store, clock, current);
      if (current.event_match != job.event_match ||
          (current.held & bit(DataType::kEvents))) {
        current_ = nullptr;
        return false;
      }
      if (!store.ensureDetail(job.event_match, entry.id, clock)) {
        store.flag(map_.source, kNoticeEvents, true);
        current_ = nullptr;
        return false;
      }
    }
  }
  return begin(store, clock, out);
}

bool Planner::begin(MatchStore& store, const Clock& clock, Request& out) {
  auto& state = *current_;
  auto& progress = state.progress[index(type_)];
  if (progress.step == 0 && progress.at == 0) {
    // A new run.
    if (type_ == DataType::kLive) liveItems(store, clock.utc);
    if (type_ == DataType::kFixtures && !resolving) {
      if (fixtures_.source && fixtures_.source != current_->source) {
        current_ = nullptr;
        return false;
      }
      fixtures_.clear();
      fixtures_.source = current_->source;
      fixtures_.club_route = nextClub(0);
      store.beginFixtureRun(map_.source);
    }
  }
  out = {};
  retain_admission_ = false;
  known_only_ = false;
  if (!build(&out)) {
    progress = {};
    current_ = nullptr;
    return false;
  }
  sink_ = out.sink;
  if (sink_ == Sink::kOwnMatches)
    own_expected_ =
        out.spec.upcoming ? fixtures_.future_expected : fixtures_.past_expected;
  retention_index_.reset();
  retained_rows_ = {};
  response_failure_ = provider::Status::kOk;
  response_cause_ = Cause::kNone;
  response_sources_ = true;
  own_seen_.reset();
  response_rows_ = 0;
  response_round_ = static_cast<uint8_t>(out.spec.round);
  response_label_ = kNoIndex;
  response_round_known_ = true;
  response_whole_ = false;
  found_round_ = 0;
  found_change_ = 0;
  found_season_ = {};
  response_details_.clear();
  map_.observe_match = nullptr;
  map_.observe_events = nullptr;
  if ((sink_ == Sink::kMatches || sink_ == Sink::kOwnMatches) &&
      eligibility_ != nullptr && competitions_ != nullptr) {
    // Allocate only after the first body row arrives. The settings first-send
    // fence has then been released, so its heap cannot overlap this payload.
    map_.observe_match = observeMatch;
    map_.match_context = this;
    map_.observe_events = observeEvent;
    map_.events_context = this;
  }
  if (sink_ == Sink::kTable) store.beginTable(map_.source);
  if (sink_ == Sink::kEvents) {
    // Exact selected target; ownership and special priority are separate.
    const auto* m = eventsRow();
    if (m == nullptr || !eventReady(*m) || !store.hasDetail(*m) ||
        !store.beginEvents(*m, true, map_.clock.utc)) {
      state.progress[index(type_)] = {};
      current_ = nullptr;
      return false;
    }
  }
  return true;
}

const StoredMatch* Planner::eventsRow() const {
  return store_->find(map_.source, event_match_);
}

bool Planner::build(Request* out) {
  using provider::Operation;
  auto& state = *current_;
  auto& progress = state.progress[index(type_)];
  const auto provider = source_.provider;
  const auto protocol = provider::profile(provider);
  const auto season = source_.season.view();
  auto set = [&](Operation operation, Sink sink) {
    if (out) {
      out->spec = {};
      out->spec.source = {provider, source_.competition.view(), season};
      out->spec.operation = operation;
      out->sink = sink;
    }
    return true;
  };
  filter_ = false;
  if (resolving)
    return set(protocol.resolution,
               protocol.resolution == Operation::kCalendarDays ? Sink::kDays
                                                               : Sink::kSeason);
  if (progress.step == 0) progress = {1, 0};
  if (type_ == DataType::kFixtures &&
      protocol.fixtures == provider::FixtureFlow::kSeason) {
    if (progress.step == 1) {
      filter_ = true;
      return set(Operation::kFixtures, Sink::kIndex);
    }
    if (progress.step == 2) {
      filter_ = true;
      retain_admission_ = true;
      return set(Operation::kFixtures, Sink::kMatches);
    }
    return false;
  }
  if (type_ == DataType::kTable || type_ == DataType::kLiveTable)
    return progress.step == 1 && set(Operation::kTable, Sink::kTable);
  if (type_ == DataType::kEvents) {
    if (!protocol.separate_events || progress.step != 1 || !event_match_)
      return false;
    set(Operation::kSummary, Sink::kEvents);
    if (out) out->spec.match = event_match_;
    return true;
  }
  if (protocol.live == provider::LiveFlow::kRoundChange) {
    if (type_ != DataType::kLive || progress.at >= state.live_count)
      return false;
    const uint32_t round =
        state.live[progress.at] ? state.live[progress.at] : state.round;
    if (!round) {
      // A current-round endpoint can have rolled into a different season.
      // Refresh only already-retained identities from this exact season.
      filter_ = known_only_ = true;
      return set(Operation::kFixtures, Sink::kMatches);
    }
    set(progress.step == 1 ? Operation::kLastChange : Operation::kFixtures,
        progress.step == 1 ? Sink::kChange : Sink::kMatches);
    if (out) out->spec.round = round;
    return true;
  }
  if (protocol.fixtures == provider::FixtureFlow::kCalendar) {
    if (type_ == DataType::kFixtures && progress.step == 1) {
      set(Operation::kCalendarDays, Sink::kDays);
      if (out && source_.season.empty()) out->spec.source.season = {};
      return true;
    }
    if (type_ == DataType::kFixtures && progress.step == 2 &&
        progress.at >= fixtures_.selection.count)
      progress.step = currentClub() ? kFirstOwnStep : kOwnEndStep;
    if (type_ == DataType::kFixtures) {
      if (const auto* own = ownRequest(progress.step)) {
        filter_ = true;
        set(Operation::kOwnFixtures, own->sink);
        if (out) {
          out->spec.team = currentClub();
          out->spec.upcoming = own->upcoming;
        }
        return true;
      }
      if (progress.step >= kOwnEndStep) return false;
    } else if (type_ == DataType::kLive && progress.at >= state.live_count)
      return false;
    set(Operation::kFixtures, Sink::kMatches);
    filter_ = true;
    if (out)
      out->spec.from =
          type_ == DataType::kLive
              ? state.live[progress.at]
              : fixtures_.selection.calendar_data.days[progress.at];
    return true;
  }
  if (progress.step != 1) return false;
  set(Operation::kFixtures, Sink::kMatches);
  if (out) {
    out->spec.from = static_cast<uint32_t>(
        std::max<int64_t>(1, map_.clock.utc) / timekeeping::kSecondsPerDay *
        timekeeping::kSecondsPerDay);
    out->spec.to = static_cast<uint32_t>(
        std::min<int64_t>(UINT32_MAX - timekeeping::kSecondsPerDay,
                          map_.clock.utc) /
            timekeeping::kSecondsPerDay * timekeeping::kSecondsPerDay +
        timekeeping::kSecondsPerDay - 1);
  }
  return true;
}

void Planner::liveItems(const MatchStore& store, int64_t utc) {
  auto& state = *current_;
  state.live_count = 0;
  const auto flow = provider::profile(source_.provider).live;
  auto add = [&](uint32_t item) {
    for (uint8_t i = 0; i < state.live_count; ++i)
      if (state.live[i] == item) return;
    if (state.live_count < kLiveItems) state.live[state.live_count++] = item;
  };
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    uint32_t start = 0, end = 0;
    if (m.source != map_.source || !schedule::liveWindow(m, start, end) ||
        utc < start || utc >= end)
      continue;
    if (flow == provider::LiveFlow::kRoundChange) {
      // Round numbers are the provider's own.
      if (m.round != 0) add(m.round);
    } else if (flow == provider::LiveFlow::kCalendarDay) {
      // The provider day holding the kickoff, else the next match day.
      uint32_t day = 0;
      for (uint8_t d = 0; d < state.day_count; ++d)
        if (state.days[d] / timekeeping::kSecondsPerDay ==
            m.kickoff / timekeeping::kSecondsPerDay)
          day = state.days[d];
      add(day);
    }
  }
  // No round of its own: the current round (0 = still unknown, resolved by
  // the request).
  if (flow == provider::LiveFlow::kRoundChange && state.live_count == 0)
    add(state.round);
  if (flow == provider::LiveFlow::kCalendarDay && state.live_count == 0) add(0);
}

bool Planner::acceptsMatch(const provider::Fixture& row) const {
  return (sink_ == Sink::kMatches || sink_ == Sink::kOwnMatches ||
          sink_ == Sink::kIndex || sink_ == Sink::kOwnIndex) &&
         (!filter_ || row.source_matches);
}
bool Planner::consume(const provider::Fixture& row,
                      std::span<const provider::Event> events) {
  if (!row.source_matches) response_sources_ = false;
  if (sink_ == Sink::kSeason) {
    found_round_ = std::max<int>(found_round_, row.facts.round);
    return true;
  }
  if (sink_ == Sink::kIndex || sink_ == Sink::kOwnIndex) {
    if (sink_ == Sink::kOwnIndex) {
      const auto* own =
          ownRequest(current_->progress[index(DataType::kFixtures)].step);
      if (own && own->upcoming)
        for (uint8_t i = 0; i < fixtures_.own_candidates.size(); ++i)
          if (fixtures_.own_candidates[i].id == row.facts.id)
            fixtures_.future_seen |= static_cast<uint8_t>(1u << i);
    }
    if (!retention_index_) retention_index_.reset(makeRetentionIndex());
    if (!retention_index_) {
      response_failure_ = provider::Status::kNoMemory;
      return false;
    }
    if (!retention_index_->observe(row)) {
      response_failure_ = provider::Status::kTooLarge;
      return false;
    }
    return true;
  }
  if (resolving) found_round_ = std::max<int>(found_round_, row.facts.round);
  if (!acceptsMatch(row)) return true;
  if (known_only_ && !store_->find(map_.source, row.facts.id)) return true;
  if (retain_admission_ || sink_ == Sink::kOwnMatches) {
    const auto& selected = fixtures_.selection;
    const auto own =
        std::find(selected.own.begin(), selected.own.end(), row.facts.id);
    if (own != selected.own.end())
      own_seen_.set(static_cast<std::size_t>(own - selected.own.begin()));
    if (sink_ == Sink::kOwnMatches) {
      if (own == selected.own.end() ||
          !own_expected_.test(
              static_cast<std::size_t>(own - selected.own.begin())))
        return true;
    } else {
      const int round =
          selected.roundIndex(row.facts.round, row.round_label.data);
      if (round >= 0) ++retained_rows_[round];
      if (!selected.contains(row.facts.round, row.round_label.data,
                             row.facts.kickoff, row.facts.id))
        return true;
    }
  }
  ++response_rows_;
  if (row.facts.round) {
    if (response_round_ && response_round_ != row.facts.round)
      response_round_known_ = false;
    else
      response_round_ = row.facts.round;
  } else
    response_round_known_ = false;
  map_.round = 0;
  map_.round_label = kNoIndex;
  for (std::size_t i = 0; i < store_->roundCount(); ++i) {
    const auto& stage = store_->round(i);
    if (stage.source == map_.source && stage.kind == RoundKind::kCalendar &&
        row.facts.kickoff >= stage.from && row.facts.kickoff <= stage.to) {
      map_.round = stage.number;
      map_.round_label = stage.label;
      break;
    }
  }
  const bool stored = storeMatch(row, events, *store_, map_);
  if (!stored) response_failure_ = provider::Status::kTooLarge;
  if (!row.source_matches || !row.time_definite) response_round_known_ = false;
  if (auto* match = store_->find(map_.source, row.facts.id)) {
    if (type_ == DataType::kFixtures)
      store_->markFixture(map_.source, row.facts.id);
    if (response_label_ == kNoIndex) response_label_ = match->label;
  }
  return stored;
}
bool Planner::consume(const provider::TableRow& row) {
  const bool stored = storeTableRow(row, *store_, map_);
  if (!stored) response_failure_ = provider::Status::kTooLarge;
  return stored;
}
bool Planner::consume(const provider::TableGroup& group) {
  return store_->appendGroup(group.key.data, group.label.data, group.ordinal);
}
bool Planner::consume(const provider::Event& event) {
  if (const auto* match = eventsRow())
    storeSummaryEvent(event, *store_, *match);
  return true;
}
bool Planner::consumeDay(uint32_t day) {
  if (!retention_index_) retention_index_.reset(makeRetentionIndex(true));
  if (!retention_index_) {
    response_failure_ = provider::Status::kNoMemory;
    return false;
  }
  if (!retention_index_->observeDay(day)) {
    response_failure_ = provider::Status::kTooLarge;
    return false;
  }
  return true;
}
bool Planner::consumeCalendarRound(const provider::CalendarRound& round) {
  if (!retention_index_) retention_index_.reset(makeRetentionIndex(true));
  if (!retention_index_) {
    response_failure_ = provider::Status::kNoMemory;
    return false;
  }
  if (!retention_index_->observeRound(round)) {
    response_failure_ = provider::Status::kTooLarge;
    return false;
  }
  return true;
}
bool Planner::consumeText(std::string_view text) {
  found_change_ = changeHash(text);
  return true;
}
bool Planner::resolveResponse(const Clock& clock) {
  if (!resolving) return true;
  if (!response_whole_ || found_season_.empty() || !entry_ || !competitions_)
    return false;
  SourceKey key;
  if (!key.assign(source_.provider, source_.competition.view(),
                  found_season_.view()))
    return false;
  const auto id = store_->resolve(entry_->id, key, clock);
  if (!id) {
    response_failure_ = provider::Status::kTooLarge;
    response_cause_ = Cause::kStoreFull;
    return false;
  }
  map_.source = store_->sourceSlot(id);
  applyCapabilities(*store_, map_.source);
  auto& state = states_[map_.source];
  if (state.source != id) {
    state.clear();
    state.source = id;
  }
  store_->resolvedAt(map_.source, clock.utc > 0 && clock.utc <= UINT32_MAX
                                      ? static_cast<uint32_t>(clock.utc)
                                      : 0);
  for (const auto& entry : competitions_->entries)
    if (entry.active && entry.primary == entry_->primary) {
      if (!store_->resolve(entry.id, key, clock)) return false;
      store_->recordResolution(entry.id, 0, Cause::kNone);
    }
  if (found_round_ > 0) state.round = static_cast<uint8_t>(found_round_);
  return true;
}
bool Planner::response(const provider::ParseResult& parsed) {
  if (response_failure_ != provider::Status::kOk) return false;
  const auto& metadata = parsed.metadata;
  response_whole_ = parsed.status == provider::Status::kOk &&
                    parsed.primary_seen && parsed.document_complete &&
                    !parsed.early_stopped && !metadata.invalid &&
                    !metadata.duplicate;
  if (!metadata.season.empty()) found_season_.assign(metadata.season.data);
  found_round_ = std::max<int>(found_round_, metadata.round);
  if (resolving && (!metadata.metadata_complete || !metadata.source_matches ||
                    !resolveResponse(map_.clock))) {
    if (response_failure_ == provider::Status::kOk)
      response_failure_ = provider::Status::kRejected;
    return false;
  }
  if (!resolving && response_whole_ &&
      (sink_ == Sink::kMatches || sink_ == Sink::kOwnMatches) &&
      (!metadata.metadata_complete || !metadata.source_matches ||
       !response_sources_)) {
    response_failure_ = provider::Status::kRejected;
    return false;
  }
  if (sink_ == Sink::kIndex || sink_ == Sink::kOwnIndex) {
    if (response_failure_ != provider::Status::kOk) return false;
    if (!response_whole_ || !metadata.metadata_complete ||
        !metadata.source_matches) {
      response_failure_ = provider::Status::kRejected;
      return false;
    }
    if (!retention_index_) retention_index_.reset(makeRetentionIndex());
    if (!retention_index_) response_failure_ = provider::Status::kNoMemory;
    if (response_failure_ != provider::Status::kOk) return false;
    if (sink_ == Sink::kIndex) {
      if (!retention_index_->select(fixtures_.selection)) {
        response_failure_ = provider::Status::kRejected;
        return false;
      }
      store_->acknowledgeFixtureChanges(map_.source);
    } else {
      const auto* own =
          ownRequest(current_->progress[index(DataType::kFixtures)].step);
      if (!own) {
        response_failure_ = provider::Status::kRejected;
        return false;
      }
      if (!own->upcoming) {
        if (!retention_index_->ownCandidates(currentClub(),
                                             fixtures_.own_candidates)) {
          response_failure_ = provider::Status::kRejected;
          return false;
        }
        fixtures_.future_seen = 0;
      } else {
        retention::OwnFixtures chosen;
        if (!retention_index_->ownCandidates(currentClub(), chosen) ||
            !retention_index_->own(fixtures_.selection,
                                   retention::OwnMode::kAppend)) {
          response_failure_ = provider::Status::kRejected;
          return false;
        }
        fixtures_.past_expected.reset();
        fixtures_.future_expected.reset();
        for (const auto& candidate : chosen) {
          if (!candidate.id) continue;
          const auto position =
              std::find(fixtures_.selection.own.begin(),
                        fixtures_.selection.own.end(), candidate.id);
          if (position == fixtures_.selection.own.end()) {
            response_failure_ = provider::Status::kRejected;
            return false;
          }
          const auto bit = static_cast<std::size_t>(
              position - fixtures_.selection.own.begin());
          bool past = false, future = false;
          for (uint8_t i = 0; i < fixtures_.own_candidates.size(); ++i)
            if (fixtures_.own_candidates[i].id == candidate.id) {
              past = true;
              future = fixtures_.future_seen & (1u << i);
            }
          if (past) fixtures_.past_expected.set(bit);
          if (!past || future) fixtures_.future_expected.set(bit);
        }
      }
    }
  }
  if (retain_admission_ && response_whole_)
    if (!metadata.metadata_complete || !metadata.source_matches ||
        !response_sources_) {
      response_failure_ = provider::Status::kRejected;
      return false;
    }
  if (retain_admission_ && response_whole_)
    for (uint8_t i = 0; i < fixtures_.selection.count; ++i)
      if (retained_rows_[i] != fixtures_.selection.expected[i]) {
        response_failure_ = provider::Status::kRejected;
        return false;
      }
  if ((sink_ == Sink::kOwnMatches || retain_admission_) && response_whole_)
    for (std::size_t i = 0; i < fixtures_.selection.own.size(); ++i)
      if (fixtures_.selection.own[i] && !own_seen_.test(i) &&
          (retain_admission_ || own_expected_.test(i))) {
        response_failure_ = provider::Status::kRejected;
        return false;
      }
  if (sink_ == Sink::kDays) {
    if (!response_whole_ || !metadata.metadata_complete ||
        !metadata.source_matches) {
      response_failure_ = provider::Status::kRejected;
      return false;
    }
    if (response_failure_ != provider::Status::kOk) return false;
    if (!retention_index_) retention_index_.reset(makeRetentionIndex(true));
    if (!retention_index_) {
      response_failure_ = provider::Status::kNoMemory;
      return false;
    }
    if (!retention_index_->select(fixtures_.selection, store_, map_.source)) {
      response_failure_ = provider::Status::kTooLarge;
      store_->flag(map_.source, kNoticeMatches, true);
      return false;
    }
    auto& state = states_[map_.source];
    state.days = fixtures_.selection.calendar_data.days;
    state.day_count = fixtures_.selection.count;
    fixtures_.source = store_->sourceId(map_.source);
    fixtures_.club_route = nextClub(0);
    store_->acknowledgeFixtureChanges(map_.source);
  }
  return true;
}

void Planner::observeMatch(const StoredMatch& match, void* context) {
  auto& planner = *static_cast<Planner*>(context);
  auto& response = planner.response_details_;
  response.receiving = -1;
  if (response.ambiguous) return;
  if (!response.events) {
    response.events.reset(new (std::nothrow) ResponseDetails::EventBuffer{});
    if (!response.events) {
      // Scores retain their bounded path; a failed detail allocation stays
      // visible and is not retried for every row in the same response.
      response.ambiguous = true;
      planner.store_->flag(match.source, kNoticeEvents, true);
      return;
    }
  }
  // A provider row repeated with another state can revive an earlier discarded
  // candidate. Such input cannot prove a bounded first-response detail commit.
  if (std::find(response.seen.begin(),
                response.seen.begin() + response.seen_count,
                match.id) != response.seen.begin() + response.seen_count) {
    response.ambiguous = true;
    return;
  }
  if (response.seen_count == response.seen.size()) {
    response.ambiguous = true;
    return;
  }
  response.seen[response.seen_count++] = match.id;
  if (planner.store_->isOwn(match) || response.ambiguous ||
      planner.store_->find(match.source, match.id) == nullptr)
    return;
  const auto* competition = planner.entry_;
  if (competition == nullptr) return;
  const selection::Input input{planner.eligibility_->selection,
                               *planner.competitions_, *planner.store_,
                               planner.map_.clock, planner.dates_};
  const auto window = selection::window(input, *competition, match);
  if (!window.active) return;
  const std::size_t which = window.reported_running ? 0 : 1;
  auto& candidate = response.candidates[which];
  // The closest row survives canonical store pressure longest in this class.
  // Retain a later equal-distance row, matching the store's farthest victim.
  if (candidate.match.valid()) {
    const auto incoming_distance =
        distance(match.kickoff, planner.map_.clock.utc);
    const auto previous_distance =
        distance(candidate.kickoff, planner.map_.clock.utc);
    if (incoming_distance > previous_distance ||
        (incoming_distance == previous_distance &&
         match.kickoff < candidate.kickoff))
      return;
  }
  // Reuse the retained metadata in place; no full metadata stack temporary.
  candidate.count = 0;
  candidate.truncated = false;
  candidate.match = planner.store_->key(match);
  candidate.kickoff = match.kickoff;
  response.receiving = static_cast<int8_t>(which);
}

void Planner::observeEvent(const EventFacts& event, void* context) {
  auto& planner = *static_cast<Planner*>(context);
  if (!planner.response_details_.events) return;
  auto& response = planner.response_details_;
  if (response.ambiguous || response.receiving < 0) return;
  auto& candidate = response.candidates[response.receiving];
  auto& events = response.events->candidates[response.receiving];
  if (candidate.count == events.size()) {
    std::move(events.begin() + 1, events.end(), events.begin());
    --candidate.count;
    candidate.truncated = true;
  }
  events[candidate.count++] = event;
}

void Planner::finishDetails(bool ok, const Clock& clock) {
  if (sink_ != Sink::kMatches || !eligibility_ || !competitions_) return;
  if (!ok || eligibility_->frame) {
    response_details_.clear();
    return;
  }
  const selection::Input input{eligibility_->selection,
                               *competitions_,
                               *store_,
                               clock,
                               dates_,
                               break_proven_};
  const auto selected = selection::select(input);
  std::array<bool, 2> committed{};
  for (uint8_t slot = 0; slot < competitions_->entries.size(); ++slot) {
    const auto& entry = competitions_->entries[slot];
    if (!entry.active || !store_->includes(entry.id, map_.source) ||
        !eligibility_->allows(selected.phase, screen::Type::kLiveSingle, slot))
      continue;
    const auto single = selection::single(input, selected, entry);
    const auto* match = selection::find(input, single.match);
    if (!match || match->source != map_.source) continue;
    const bool had_detail = store_->hasDetail(*match);
    auto* target = store_->ensureDetail(single.match, entry.id, clock);
    if (!target) {
      store_->flag(map_.source, kNoticeEvents, true);
      continue;
    }
    if (store_->isOwn(*target)) continue;
    int chosen = -1;
    if (response_details_.events && !response_details_.ambiguous)
      for (unsigned i = 0; i < response_details_.candidates.size(); ++i)
        if (response_details_.candidates[i].match == single.match) chosen = i;
    if (chosen >= 0) {
      if (committed[chosen]) continue;
      committed[chosen] = true;
      const auto& candidate = response_details_.candidates[chosen];
      if (store_->beginEvents(*target, false, clock.utc)) {
        for (uint8_t i = 0; i < candidate.count; ++i)
          store_->addEvent(response_details_.events->candidates[chosen][i]);
        store_->endEvents(true);
      }
      if (candidate.truncated) {
        target->flags |= kMatchEventsTruncated;
        store_->flag(map_.source, kNoticeEvents, true);
      }
    } else if (response_details_.ambiguous || !had_detail) {
      target->flags |= kMatchEventsTruncated | kMatchEventsPoolCut;
      store_->flag(map_.source, kNoticeEvents, true);
    }
  }
  response_details_.clear();
}

Step Planner::finish(bool ok, const Clock& clock) {
  if (current_ == nullptr) return Step::kDone;
  if (sink_ == Sink::kTable) store_->commitTable(ok);
  if (!resolving && !known_only_ &&
      (sink_ == Sink::kMatches || sink_ == Sink::kOwnMatches)) {
    store_->datasetResult(map_.source, MatchStore::Dataset::kFixtures,
                          ok && response_whole_);
    if (response_round_known_ && response_round_)
      store_->roundResult(map_.source, response_round_, response_label_,
                          response_rows_, ok && response_whole_);
  }
  if (sink_ == Sink::kEvents) store_->endEvents(ok);
  finishDetails(ok, clock);
  retention_index_.reset();
  return advance(ok, clock);
}

void Planner::abort() {
  if (current_ == nullptr) return;
  current_->progress[index(type_)] = {};
  if (type_ == DataType::kFixtures) fixtures_.clear();
  release();
}

void Planner::release() {
  if (current_ == nullptr) return;
  if (sink_ == Sink::kTable) store_->commitTable(false);
  if (sink_ == Sink::kEvents) store_->endEvents(false);
  response_details_.clear();
  retention_index_.reset();
  current_ = nullptr;
}

Completion Planner::complete(Fetch fetch, const Clock& clock) {
  using schedule::Outcome;
  Completion c;
  switch (fetch) {
    case Fetch::kOk:
      c.preserve_scope = known_only_;
      switch (finish(true, clock)) {
        case Step::kDone: break;
        case Step::kMore: c.more = true; break;
        case Step::kInvalid:
          c.outcome = Outcome::kFailed;
          c.invalid = true;
          break;
      }
      return c;
    case Fetch::kFailed:
    case Fetch::kRateLimited:
      finish(false, clock);
      c.outcome =
          fetch == Fetch::kFailed ? Outcome::kFailed : Outcome::kRateLimited;
      return c;
    case Fetch::kBusy:
      release();
      c.outcome = Outcome::kDeferred;
      return c;
    case Fetch::kInterrupted:
    case Fetch::kCapped:
      abort();
      c.outcome = Outcome::kDeferred;
      return c;
    case Fetch::kRefused:
      abort();
      c.outcome = Outcome::kFailed;
      return c;
  }
  return c;
}

Step Planner::advance(bool ok, const Clock& clock) {
  auto& state = *current_;
  auto& progress = state.progress[index(type_)];
  auto end = [&](Step step) {
    progress = {};
    current_ = nullptr;
    return step;
  };
  if (!ok) {
    if (type_ == DataType::kFixtures) fixtures_.clear();
    return end(Step::kDone);
  }
  const auto protocol = provider::profile(source_.provider);
  if (resolving) {
    if (map_.source == kNoSource) return end(Step::kInvalid);
    if (protocol.fixtures == provider::FixtureFlow::kCalendar) {
      auto& resolved = states_[map_.source];
      resolved.progress[index(DataType::kFixtures)] = {2, 0};
      store_->beginFixtureRun(map_.source);
    }
    return end(Step::kDone);
  }
  if (protocol.fixtures == provider::FixtureFlow::kSeason &&
      type_ == DataType::kFixtures) {
    if (progress.step == 1 && fixtures_.selection.current >= 0)
      state.round =
          fixtures_.selection.rounds[fixtures_.selection.current].number;
    ++progress.step;
  } else if (protocol.live == provider::LiveFlow::kRoundChange &&
             type_ == DataType::kLive) {
    if (known_only_) return end(Step::kDone);
    const uint32_t round =
        state.live[progress.at] != 0 ? state.live[progress.at] : state.round;
    if (progress.step == 1) {
      state.checked = found_change_;
      const auto* mark = findMark(state, round);
      const bool full = mark == nullptr || mark->change != state.checked ||
                        clock.mono_s - mark->at >= cfg::kOpenLigaDbFullRefreshS;
      if (full)
        progress.step = 2;
      else
        ++progress.at;
    } else {
      auto* mark = findMark(state, round);
      if (mark == nullptr) {
        // A free mark, else the one with the oldest full request.
        mark = &state.marks[0];
        for (auto& m : state.marks)
          if (mark->round != 0 && (m.round == 0 || m.at < mark->at)) mark = &m;
      }
      *mark = {static_cast<uint8_t>(round), state.checked, clock.mono_s};
      progress = {1, static_cast<uint8_t>(progress.at + 1)};
    }
  } else if (protocol.fixtures == provider::FixtureFlow::kCalendar &&
             type_ == DataType::kFixtures) {
    if (progress.step == 1) {
      progress = {2, 0};
    } else if (progress.step == 2) {
      ++progress.at;
    } else if (ownRequest(progress.step)) {
      if (++progress.step == kOwnEndStep) {
        fixtures_.club_route = nextClub(fixtures_.club_route + 1);
        progress.step = currentClub() ? kFirstOwnStep : kOwnEndStep;
      }
    }
  } else if (protocol.live == provider::LiveFlow::kCalendarDay &&
             type_ == DataType::kLive) {
    ++progress.at;
  } else {
    ++progress.step;
  }
  if (!build(nullptr)) {
    if (type_ == DataType::kFixtures) {
      store_->retain(map_.source, fixtures_.selection, clock.utc, dates_);
      if (!fixtures_.selection.calendar)
        for (uint8_t i = 0; i < fixtures_.selection.count; ++i) {
          const auto& key = fixtures_.selection.rounds[i];
          const auto label =
              key.label.empty() ? kNoIndex : store_->label(key.label.data);
          store_->roundResult(map_.source, key.number, label,
                              fixtures_.selection.expected[i], true);
        }
      else
        for (uint8_t i = 0; i < fixtures_.selection.calendar_data.stage_count;
             ++i) {
          const auto& stage = fixtures_.selection.calendar_data.stages[i];
          uint16_t count = 0;
          for (std::size_t m = 0; m < store_->matchCount(); ++m) {
            const auto& match = store_->match(m);
            count +=
                match.source == map_.source && match.round == stage.number &&
                match.label == stage.label && (match.flags & kMatchRefresh);
          }
          store_->roundResult(map_.source, stage.number, stage.label, count,
                              true, stage.from, stage.to, RoundKind::kCalendar);
        }
      store_->settleRun(map_.source, kNoticeFixtures);
      store_->collect();
      fixtures_.clear();
    }
    return end(Step::kDone);
  }
  current_ = nullptr;
  return Step::kMore;
}

}  // namespace football::plan
