#include "provider_plan.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <new>

#include "civil_time.h"
#include "football_model.h"
#include "matchday.h"

namespace football::plan {
namespace {

using cfg::DataType;
using cfg::Provider;

std::size_t index(DataType type) { return static_cast<std::size_t>(type); }

uint32_t sourceKey(uint8_t id, const settings::Source& s) {
  const char head[2] = {static_cast<char>(id), static_cast<char>(s.provider)};
  uint32_t h = values::fnv1a32({head, 2});
  h = values::fnv1a32(s.competition.view(), h);
  h = values::fnv1a32(s.season.view(), values::fnv1a32("/", h));
  return h == 0 ? 1 : h;
}
// Providers whose URLs need the season: the current one when none is set.
// Events (ESPN's match summary) never do.
// The resolved season is due for a new resolution at 'utc': its time is
// unknown, kSeasonRecheckS old, or after 'utc' (the clock was set back).
bool seasonStale(const SourceState& state, int64_t utc) {
  if (utc <= 0) return false;  // no valid clock: keep it
  return state.season_utc == 0 || utc < state.season_utc ||
         utc - state.season_utc >= cfg::kSeasonRecheckS;
}

bool needsSeason(Provider provider, DataType type) {
  if (type == DataType::kEvents) return false;
  if (provider == Provider::kApiFootball) return true;
  return provider == Provider::kOpenLigaDb;
}
// Expected requests per run of each data type at 'utc' ('known': the state
// belongs to this source).
void costs(const settings::CompetitionRoute& entry, const SourceState& state,
           bool known, int64_t utc,
           std::array<uint8_t, cfg::kDataTypeCount>& cost) {
  const auto& s = entry.primary;
  const bool season = !s.season.empty() || (known && !state.season.empty());
  for (std::size_t t = 0; t < cfg::kDataTypeCount; ++t)
    cost[t] =
        needsSeason(s.provider, static_cast<DataType>(t)) && !season ? 2 : 1;
  // The fixtures run resolves a due season again first (OQ-259, begin()).
  if (s.provider == Provider::kApiFootball && s.season.empty() && known &&
      seasonStale(state, utc))
    cost[index(DataType::kFixtures)] = 2;
  if (s.provider == Provider::kOpenLigaDb) {
    cost[index(DataType::kLive)] = 2;  // change date, round when changed
  }
  if (s.provider == Provider::kEspn) {
    // Match days, then one scoreboard per day (a week before the first).
    cost[index(DataType::kFixtures)] = static_cast<uint8_t>(
        1 + (known && state.fixtures.count ? state.fixtures.count : 7) +
        (entry.club.team ? 2 : 0));
  } else
    cost[index(DataType::kFixtures)] = static_cast<uint8_t>(
        2 + (needsSeason(s.provider, DataType::kFixtures) && !season ? 1 : 0));
  if ((s.provider == Provider::kApiFootball ||
       s.provider == Provider::kOpenLigaDb) &&
      s.season.empty() && known && seasonStale(state, utc))
    cost[index(DataType::kFixtures)] = 3;
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
  current_ = nullptr;
  store_ = nullptr;
  response_details_.clear();
  retention_index_.reset();
}

void Planner::forget(uint8_t slot) {
  if (slot >= cfg::kCompetitionMax) return;
  if (current_ == &states_[slot]) abort();
  states_[slot].clear();
}

retention::Index* Planner::makeRetentionIndex(bool calendar) const {
  if (sink_ != Sink::kIndex || !competitions_)
    return new (std::nothrow)
        retention::Index(map_.club, map_.clock.utc, dates_, calendar);
  // Include the prepared entry even for a caller without the complete route
  // list. Index deduplicates it; an inconsistent ninth distinct club rejects
  // the scope instead of overrunning or silently clipping it.
  std::array<uint32_t, cfg::kCompetitionMax + 1> clubs{};
  std::size_t count = 0;
  if (map_.club) clubs[count++] = map_.club;
  for (const auto& entry : competitions_->entries)
    if (entry.active && entry.primary == source_ && entry.club.team)
      clubs[count++] = entry.club.team;
  return new (std::nothrow)
      retention::Index(std::span<const uint32_t>(clubs.data(), count),
                       map_.clock.utc, dates_, calendar);
}

void Planner::wanted(uint8_t slot, const settings::CompetitionRoute* entry,
                     const MatchStore& store, const Clock& clock,
                     schedule::Wanted& out,
                     const selection::Selection* current_selection) const {
  out = {};
  if (entry == nullptr || !entry->active || entry->id == 0 ||
      entry->primary.competition.empty() || slot >= cfg::kCompetitionMax)
    return;
  const auto& s = entry->primary;
  const auto& state = states_[slot];
  out.id = entry->id;
  out.provider = s.provider;
  // Fixtures and their live updates classify phases even when a sporting
  // screen is disabled. Manual-only entries demand the same usable data.
  out.types = bit(DataType::kFixtures) | bit(DataType::kLive);
  if (eligibility_ != nullptr && competitions_ != nullptr) {
    if (eligibility_->frame) {
      out = {};
      return;
    }
    const selection::Input input{eligibility_->selection,
                                 *competitions_,
                                 store,
                                 clock,
                                 dates_,
                                 break_proven_};
    const auto selected = current_selection != nullptr
                              ? *current_selection
                              : selection::select(input);
    if (selected.phase == selection::ScreenPhase::kOwn &&
        selected.focus.competition == entry->id)
      out.priority_match = selected.focus.id;
    if (eligibility_->allows(selected.phase, screen::Type::kTable, slot))
      out.types |= bit(DataType::kTable) | bit(DataType::kLiveTable);
    if (s.provider == Provider::kEspn &&
        eligibility_->allows(selected.phase, screen::Type::kLiveSingle, slot)) {
      const auto single = selection::single(input, selected, *entry);
      if (const auto* m = selection::find(input, single.match)) {
        out.types |= bit(DataType::kEvents);
        out.event_match = m->id;
        out.event_priority = selected.phase == selection::ScreenPhase::kOwn &&
                             single.match == selected.focus;
        if (!eventReady(*m)) out.held |= bit(DataType::kEvents);
      }
    }
  }
  costs(*entry, state, state.key == sourceKey(entry->id, s), clock.utc,
        out.cost);
}

bool Planner::request(const schedule::Job& job,
                      const settings::CompetitionRoute& entry,
                      MatchStore& store, const Clock& clock, Request& out) {
  current_ = nullptr;
  if (job.slot >= cfg::kCompetitionMax) return false;
  if (job.type == DataType::kEvents && eligibility_ != nullptr &&
      competitions_ != nullptr) {
    schedule::Wanted current;
    wanted(job.slot, &entry, store, clock, current);
    if (current.event_match != job.event_match ||
        (current.held & bit(DataType::kEvents)))
      return false;
  }
  const auto& s = entry.primary;
  if (s.competition.empty()) return false;
  auto& state = states_[job.slot];
  const uint32_t key = sourceKey(entry.id, s);
  if (state.key != key) {
    state.clear();
    state.key = key;
  }
  current_ = &state;
  store_ = &store;
  source_ = s;
  type_ = job.type;
  event_match_ = job.event_match;
  map_ = {};
  map_.competition = entry.id;
  map_.provider = s.provider;
  map_.club = entry.club.team;
  map_.clock = clock;
  return begin(store, clock, out);
}

bool Planner::begin(MatchStore& store, const Clock& clock, Request& out) {
  auto& state = *current_;
  auto& progress = state.progress[index(type_)];
  if (progress.step == 0 && progress.at == 0) {
    // A new run.
    if (type_ == DataType::kFixtures && source_.season.empty() &&
        seasonStale(state, clock.utc))
      state.season = {};  // resolved again once a day (OQ-259)
    if (type_ == DataType::kLive) liveItems(store, clock.utc);
    // A fixtures run stores every match: its notices are settled when it
    // completes (OQ-311).
    if (type_ == DataType::kFixtures) store.beginFixtureRun(map_.competition);
  }
  out = {};
  retain_admission_ = false;
  if (!build(&out)) {
    progress = {};
    current_ = nullptr;
    return false;
  }
  sink_ = out.sink;
  retention_index_.reset();
  retained_rows_ = {};
  response_failure_ = provider::Status::kOk;
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
  if (sink_ == Sink::kTable) store.beginTable(map_.competition);
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
  for (std::size_t i = 0; i < store_->matchCount(); ++i) {
    const auto& m = store_->match(i);
    if (m.id == event_match_ && m.competition == map_.competition &&
        m.provider == source_.provider)
      return &m;
  }
  return nullptr;
}

bool Planner::build(Request* out) {
  using provider::Operation;
  auto& state = *current_;
  auto& progress = state.progress[index(type_)];
  const auto provider = source_.provider;
  const auto season =
      source_.season.empty() ? state.season.view() : source_.season.view();
  auto set = [&](Operation operation, Sink sink) {
    if (out) {
      out->spec = {};
      out->spec.source = {provider, source_.competition.view(), season};
      out->spec.operation = operation;
      out->sink = sink;
    }
    return true;
  };
  discover_ = filter_ = false;
  if (progress.step == 0) {
    if (needsSeason(provider, type_) && season.empty()) {
      discover_ = provider == Provider::kOpenLigaDb;
      set(Operation::kCurrentSeason,
          discover_ ? Sink::kMatches : Sink::kSeason);
      return true;
    }
    progress = {1, 0};
  }
  if (type_ == DataType::kFixtures && provider != Provider::kEspn) {
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
  if (provider == Provider::kOpenLigaDb) {
    if (type_ == DataType::kTable || type_ == DataType::kLiveTable)
      return progress.step == 1 && set(Operation::kTable, Sink::kTable);
    if (type_ != DataType::kLive || progress.at >= state.live_count)
      return false;
    const uint32_t round =
        state.live[progress.at] ? state.live[progress.at] : state.round;
    if (!round) {
      discover_ = true;
      filter_ = !source_.season.empty();
      return set(Operation::kCurrentRound, Sink::kMatches);
    }
    set(progress.step == 1 ? Operation::kLastChange : Operation::kFixtures,
        progress.step == 1 ? Sink::kChange : Sink::kMatches);
    if (out) out->spec.round = round;
    return true;
  }
  if (provider == Provider::kEspn) {
    if (type_ == DataType::kTable || type_ == DataType::kLiveTable)
      return progress.step == 1 && set(Operation::kTable, Sink::kTable);
    if (type_ == DataType::kEvents) {
      if (progress.step != 1 || !event_match_) return false;
      set(Operation::kSummary, Sink::kEvents);
      if (out) out->spec.match = event_match_;
      return true;
    }
    if (type_ == DataType::kFixtures && progress.step == 1) {
      set(Operation::kCalendarDays, Sink::kDays);
      if (out && source_.season.empty()) out->spec.source.season = {};
      return true;
    }
    if (type_ == DataType::kFixtures && progress.step == 2 &&
        progress.at >= state.fixtures.count)
      progress.step = map_.club ? 3 : 5;
    if (type_ == DataType::kFixtures &&
        (progress.step == 3 || progress.step == 4)) {
      filter_ = true;
      set(Operation::kOwnFixtures,
          progress.step == 3 ? Sink::kOwnIndex : Sink::kOwnMatches);
      if (out) out->spec.team = map_.club;
      return true;
    }
    if ((type_ == DataType::kFixtures && progress.step >= 5) ||
        (type_ == DataType::kLive && progress.at >= state.live_count))
      return false;
    set(Operation::kFixtures, Sink::kMatches);
    filter_ = true;
    if (out)
      out->spec.from = type_ == DataType::kLive
                           ? state.live[progress.at]
                           : state.fixtures.calendar_data.days[progress.at];
    return true;
  }
  if (progress.step != 1 || type_ == DataType::kEvents) return false;
  if (type_ == DataType::kTable || type_ == DataType::kLiveTable)
    return set(Operation::kTable, Sink::kTable);
  set(Operation::kFixtures, Sink::kMatches);
  if (out) {
    const int64_t span =
        type_ == DataType::kLive
            ? 0
            : cfg::kFixtureHorizonDays * int64_t{timekeeping::kSecondsPerDay};
    out->spec.from = static_cast<uint32_t>(
        std::max<int64_t>(1, map_.clock.utc - span) /
        timekeeping::kSecondsPerDay * timekeeping::kSecondsPerDay);
    out->spec.to = static_cast<uint32_t>(
        std::min<int64_t>(UINT32_MAX - timekeeping::kSecondsPerDay,
                          map_.clock.utc + span) /
            timekeeping::kSecondsPerDay * timekeeping::kSecondsPerDay +
        timekeeping::kSecondsPerDay - 1);
  }
  return true;
}

void Planner::liveItems(const MatchStore& store, int64_t utc) {
  auto& state = *current_;
  state.live_count = 0;
  auto add = [&](uint32_t item) {
    for (uint8_t i = 0; i < state.live_count; ++i)
      if (state.live[i] == item) return;
    if (state.live_count < kLiveItems) state.live[state.live_count++] = item;
  };
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    uint32_t start = 0, end = 0;
    if (m.competition != map_.competition ||
        !schedule::liveWindow(m, start, end) || utc < start || utc >= end)
      continue;
    if (source_.provider == Provider::kOpenLigaDb) {
      // Round numbers are the provider's own.
      if (m.provider == Provider::kOpenLigaDb && m.round != 0) add(m.round);
    } else if (source_.provider == Provider::kEspn) {
      // The provider day holding the kickoff, else the next match day.
      uint32_t day = 0;
      for (uint8_t d = 0; d < state.fixtures.count; ++d)
        if (state.fixtures.calendar_data.days[d] /
                timekeeping::kSecondsPerDay ==
            m.kickoff / timekeeping::kSecondsPerDay)
          day = state.fixtures.calendar_data.days[d];
      add(day);
    }
  }
  // No round of its own: the current round (0 = still unknown, resolved by
  // the request).
  if (source_.provider == Provider::kOpenLigaDb && state.live_count == 0)
    add(state.round);
  if (source_.provider == Provider::kEspn && state.live_count == 0) add(0);
}

bool Planner::acceptsMatch(const provider::Fixture& row) const {
  return (sink_ == Sink::kMatches || sink_ == Sink::kOwnMatches ||
          sink_ == Sink::kIndex || sink_ == Sink::kOwnIndex) &&
         (!filter_ || row.source_matches);
}
bool Planner::consume(const provider::Fixture& row,
                      std::span<const provider::Event> events) {
  if (!row.source_matches) response_sources_ = false;
  if (sink_ == Sink::kIndex || sink_ == Sink::kOwnIndex) {
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
  if (discover_) {
    found_round_ = std::max<int>(found_round_, row.facts.round);
  }
  if (!acceptsMatch(row)) return true;
  if (retain_admission_ || sink_ == Sink::kOwnMatches) {
    const auto& selected = current_->fixtures;
    const auto own =
        std::find(selected.own.begin(), selected.own.end(), row.facts.id);
    if (own != selected.own.end())
      own_seen_.set(static_cast<std::size_t>(own - selected.own.begin()));
    if (sink_ == Sink::kOwnMatches) {
      if (own == selected.own.end()) return true;
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
  if (current_->fixtures.calendar)
    for (uint8_t i = 0; i < current_->fixtures.calendar_data.stage_count; ++i) {
      const auto& stage = current_->fixtures.calendar_data.stages[i];
      if (row.facts.kickoff >= stage.from && row.facts.kickoff <= stage.to) {
        map_.round = stage.number;
        map_.round_label = stage.label;
        break;
      }
    }
  const bool stored = storeMatch(row, events, *store_, map_);
  if (!stored) response_failure_ = provider::Status::kTooLarge;
  if (!row.source_matches || !row.time_definite) response_round_known_ = false;
  if (auto* match =
          store_->find(map_.competition, map_.provider, row.facts.id)) {
    if (type_ == DataType::kFixtures)
      store_->markFixture(map_.competition, map_.provider, row.facts.id);
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
bool Planner::response(const provider::ParseResult& parsed) {
  if (response_failure_ != provider::Status::kOk) return false;
  const auto& metadata = parsed.metadata;
  response_whole_ = parsed.status == provider::Status::kOk &&
                    parsed.primary_seen && parsed.document_complete &&
                    !parsed.early_stopped && !metadata.invalid &&
                    !metadata.duplicate;
  if (!metadata.season.empty()) found_season_.assign(metadata.season.data);
  found_round_ = std::max<int>(found_round_, metadata.round);
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
    if (!(sink_ == Sink::kIndex ? retention_index_->select(current_->fixtures)
                                : retention_index_->own(current_->fixtures))) {
      response_failure_ = provider::Status::kRejected;
      return false;
    }
  }
  if (retain_admission_ && response_whole_)
    if (!metadata.metadata_complete || !metadata.source_matches ||
        !response_sources_) {
      response_failure_ = provider::Status::kRejected;
      return false;
    }
  if (retain_admission_ && response_whole_)
    for (uint8_t i = 0; i < current_->fixtures.count; ++i)
      if (retained_rows_[i] != current_->fixtures.expected[i]) {
        response_failure_ = provider::Status::kRejected;
        return false;
      }
  if ((sink_ == Sink::kOwnMatches || retain_admission_) && response_whole_)
    for (std::size_t i = 0; i < current_->fixtures.own.size(); ++i)
      if (current_->fixtures.own[i] && !own_seen_.test(i)) {
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
    if (!retention_index_->select(current_->fixtures, store_,
                                  map_.competition)) {
      response_failure_ = provider::Status::kTooLarge;
      store_->flag(map_.competition, kNoticeMatches, true);
      return false;
    }
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
      planner.store_->flag(match.competition, kNoticeEvents, true);
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
      planner.store_->find(match.competition, match.provider, match.id) ==
          nullptr)
    return;
  const auto* competition = planner.competitions_->find(match.competition);
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
  candidate.match = {match.id, match.competition, match.provider};
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
  if (sink_ != Sink::kMatches || eligibility_ == nullptr ||
      competitions_ == nullptr)
    return;
  if (!ok) {
    response_details_.clear();
    return;
  }
  const auto* competition = competitions_->find(map_.competition);
  if (competition == nullptr) {
    response_details_.clear();
    return;
  }
  std::size_t slot = 0;
  for (; slot < competitions_->entries.size(); ++slot)
    if (competitions_->entries[slot].id == competition->id) break;
  const selection::Input input{eligibility_->selection,
                               *competitions_,
                               *store_,
                               clock,
                               dates_,
                               break_proven_};
  const auto selected = selection::select(input);
  if (eligibility_->frame ||
      !eligibility_->allows(selected.phase, screen::Type::kLiveSingle, slot)) {
    response_details_.clear();
    return;
  }
  const auto single = selection::single(input, selected, *competition);
  const auto* match = selection::find(input, single.match);
  if (match == nullptr || store_->isOwn(*match)) {
    response_details_.clear();
    return;
  }
  const ResponseDetails::Candidate* chosen = nullptr;
  uint8_t chosen_index = 0;
  if (response_details_.events && !response_details_.ambiguous)
    for (uint8_t i = 0; i < response_details_.candidates.size(); ++i)
      if (response_details_.candidates[i].match == single.match) {
        chosen = &response_details_.candidates[i];
        chosen_index = i;
      }
  if (chosen != nullptr) {
    auto* target = store_->ensureDetail(match->competition, match->provider,
                                        match->id, clock);
    if (target != nullptr && store_->beginEvents(*target, false, clock.utc)) {
      for (uint8_t i = 0; i < chosen->count; ++i)
        store_->addEvent(response_details_.events->candidates[chosen_index][i]);
      store_->endEvents(true);
      if (chosen->truncated) {
        target->flags |= kMatchEventsTruncated;
        store_->flag(target->competition, kNoticeEvents, true);
      }
    } else {
      store_->flag(match->competition, kNoticeEvents, true);
    }
  } else if (!store_->hasDetail(*match) || response_details_.ambiguous) {
    // Keep the existing loss indication until a complete event replacement.
    // Allocation happens only for the final selected identity, never per row.
    if (auto* target = store_->ensureDetail(match->competition, match->provider,
                                            match->id, clock))
      target->flags |= kMatchEventsTruncated | kMatchEventsPoolCut;
    store_->flag(match->competition, kNoticeEvents, true);
  }
  response_details_.clear();
}

Step Planner::finish(bool ok, const Clock& clock) {
  if (current_ == nullptr) return Step::kDone;
  if (sink_ == Sink::kTable) store_->commitTable(ok);
  if (sink_ == Sink::kMatches || sink_ == Sink::kOwnMatches) {
    store_->datasetResult(map_.competition, MatchStore::Dataset::kFixtures,
                          ok && response_whole_);
    if (response_round_known_ && response_round_)
      store_->roundResult(map_.competition, response_round_, response_label_,
                          response_rows_, ok && response_whole_);
  }
  if (sink_ == Sink::kEvents)
    store_->datasetResult(map_.competition, MatchStore::Dataset::kEvents, ok);
  if (sink_ == Sink::kEvents) store_->endEvents(ok);
  finishDetails(ok, clock);
  retention_index_.reset();
  return advance(ok, clock);
}

void Planner::abort() {
  if (current_ == nullptr) return;
  current_->progress[index(type_)] = {};
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
  if (!ok) return end(Step::kDone);
  const Provider p = source_.provider;
  if (progress.step == 0) {
    // Season request (OpenLigaDB rows also give the current round).
    if (found_season_.empty()) return end(Step::kInvalid);
    state.season = found_season_;
    state.season_utc = clock.utc > 0 && clock.utc < INT64_C(0x100000000)
                           ? static_cast<uint32_t>(clock.utc)
                           : 0;
    if (found_round_ > 0) state.round = static_cast<uint8_t>(found_round_);
    progress = {1, 0};
  } else if (p != Provider::kEspn && type_ == DataType::kFixtures) {
    if (progress.step == 1) {
      if (source_.season.empty() && !found_season_.empty())
        state.season = found_season_;
      if (state.fixtures.current >= 0)
        state.round = state.fixtures.rounds[state.fixtures.current].number;
    }
    ++progress.step;
  } else if (p == Provider::kOpenLigaDb && type_ == DataType::kLive) {
    const uint32_t round =
        state.live[progress.at] != 0 ? state.live[progress.at] : state.round;
    if (discover_) {
      // The current round's matches (the round was unknown).
      if (found_round_ > 0) state.round = static_cast<uint8_t>(found_round_);
      ++progress.at;
    } else if (progress.step == 1) {
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
  } else if (p == Provider::kEspn && type_ == DataType::kFixtures) {
    if (progress.step == 1) {
      if (source_.season.empty() && !found_season_.empty())
        state.season = found_season_;
      progress = {2, 0};
    } else if (progress.step == 2) {
      ++progress.at;
    } else
      ++progress.step;
  } else if (p == Provider::kEspn && type_ == DataType::kLive) {
    ++progress.at;
  } else {
    ++progress.step;
  }
  if (!build(nullptr)) {
    if (type_ == DataType::kFixtures) {
      store_->retain(map_.competition, state.fixtures, clock.utc, dates_);
      if (!state.fixtures.calendar)
        for (uint8_t i = 0; i < state.fixtures.count; ++i) {
          const auto& key = state.fixtures.rounds[i];
          const auto label =
              key.label.empty() ? kNoIndex : store_->label(key.label.data);
          store_->roundResult(map_.competition, key.number, label,
                              state.fixtures.expected[i], true);
        }
      else
        for (uint8_t i = 0; i < state.fixtures.calendar_data.stage_count; ++i) {
          const auto& stage = state.fixtures.calendar_data.stages[i];
          uint16_t count = 0;
          for (std::size_t m = 0; m < store_->matchCount(); ++m) {
            const auto& match = store_->match(m);
            count += match.competition == map_.competition &&
                     match.round == stage.number &&
                     match.label == stage.label &&
                     (match.flags & kMatchRefresh);
          }
          store_->roundResult(map_.competition, stage.number, stage.label,
                              count, true);
        }
      store_->settleRun(map_.competition, kNoticeFixtures);
      store_->collect();
    }
    return end(Step::kDone);
  }
  current_ = nullptr;
  return Step::kMore;
}

}  // namespace football::plan
