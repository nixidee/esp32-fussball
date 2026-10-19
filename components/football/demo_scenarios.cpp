#include "demo_scenarios.h"

#include <algorithm>
#include <new>

#include "civil_time.h"

namespace football::demo {
namespace {
constexpr auto kProvider = cfg::Provider::kEspn;
constexpr int64_t kDay = timekeeping::kSecondsPerDay;
struct Team {
  uint32_t id;
  const char* name;
  const char* short_name;
};
struct Fixture {
  uint32_t id;
  Team home, away;
};
// Original source identities/names: recorded ESPN league rows (2026-10-14)
// and public provider probes (2026-10-17). Their placement/status/timing below
// deliberately form synthetic test situations, not historical match claims.
constexpr Fixture kUnion{401884775,
                         {598, "1. FC Union Berlin", "Union Berlin"},
                         {10388, "SV Elversberg", "Elversberg"}};
constexpr Fixture kAugsburg{401884777,
                            {3841, "FC Augsburg", "Augsburg"},
                            {132, "Bayern Munich", "Bayern"}};
constexpr Fixture kConference[] = {
    {401884767,
     {127, "Hamburg SV", "Hamburg"},
     {134, "VfB Stuttgart", "Stuttgart"}},
    {401884765, {133, "Schalke 04", "Schalke"}, {2950, "Mainz", "Mainz"}},
    {401884764,
     {10388, "SV Elversberg", "Elversberg"},
     {3841, "FC Augsburg", "Augsburg"}},
    {401884766,
     {137, "Werder Bremen", "Bremen"},
     {3307, "SC Paderborn 07", "Paderborn"}},
    {401884772,
     {132, "Bayern Munich", "Bayern"},
     {11420, "RB Leipzig", "RB Leipzig"}}};
constexpr Fixture kCup{401875181,
                       {18741, "Saarbrücken", "Saarbrücken"},
                       {129, "Hertha Berlin", "Hertha"}};
struct Event {
  EventType type;
  Side side;
  uint8_t minute;
  const char* player;
  uint8_t added = kNoValue;
  bool shootout = false;
};
constexpr Event kUnionEvents[] = {
    {EventType::kYellow, Side::kHome, 7, "Janik Haberer"},
    {EventType::kYellow, Side::kHome, 21, "Tim Skarke"},
    {EventType::kGoal, Side::kHome, 81, "Livan Burcu"},
    {EventType::kYellow, Side::kAway, 90, "Maurice Krattenmacher", 2}};
constexpr Event kAugsburgEvents[] = {
    {EventType::kGoal, Side::kHome, 1, "Robin Fellhauer"},
    {EventType::kYellow, Side::kHome, 42, "Chrislain Matsima"},
    {EventType::kYellow, Side::kHome, 43, "Rodrigo Ribeiro"},
    {EventType::kGoal, Side::kAway, 55, "Ismael Saibari"},
    {EventType::kYellow, Side::kHome, 62, "Hennes Behrens"},
    {EventType::kGoal, Side::kHome, 64, "Rodrigo Ribeiro"},
    {EventType::kGoal, Side::kAway, 80, "Nathaniel Brown"}};
// Source order and all 14 reported events of the public cup row are retained.
// The canonical 12-event limit therefore produces its ordinary visible cutoff.
// ESPN listed scored shootout attempts, not a complete scored/missed sequence.
constexpr Event kCupEvents[] = {
    {EventType::kPenaltyGoal, Side::kAway, 121, "Josip Brekalo", kNoValue,
     true},
    {EventType::kPenaltyGoal, Side::kHome, 122, "Mladen Cvjetinovic", kNoValue,
     true},
    {EventType::kPenaltyGoal, Side::kAway, 122, "Linus Gechter", kNoValue,
     true},
    {EventType::kPenaltyGoal, Side::kAway, 124, "Jan-Luca Schuler", kNoValue,
     true},
    {EventType::kPenaltyGoal, Side::kHome, 125, "Calogero Rizzuto", kNoValue,
     true},
    {EventType::kPenaltyGoal, Side::kAway, 125, "Kevin Sessa", kNoValue, true},
    {EventType::kYellow, Side::kAway, 65, "Linus Gechter"},
    {EventType::kYellow, Side::kHome, 65, "Kai Brunker"},
    {EventType::kYellow, Side::kAway, 66, ""},
    {EventType::kGoal, Side::kAway, 69, "Paul Seguin"},
    {EventType::kGoal, Side::kHome, 90, "Philip Fahrner", 2},
    {EventType::kYellow, Side::kAway, 98, "Oluwaseun Adewumi"},
    {EventType::kYellow, Side::kAway, 117, "Kevin Sessa"},
    {EventType::kYellow, Side::kHome, 118, "Sebastian Vasiliadis"}};

const settings::CompetitionRoute* competition(
    const settings::CompetitionRoutes& routes, const Binding& binding) {
  const auto* entry = routes.find(binding.competition);
  return entry && entry->active && entry->primary.provider == kProvider &&
                 binding.competition != 0 &&
                 (binding.saved == (binding.incarnation != 0)) &&
                 entry->incarnation == binding.incarnation
             ? entry
             : nullptr;
}
SourceSlot source(MatchStore& store,
                  const settings::CompetitionRoute& competition,
                  const Clock& clock) {
  if (!store.bind(competition.id, competition.incarnation,
                  competition.club.team, clock))
    return kNoSource;
  SourceKey key;
  if (!key.assign(competition.primary.provider,
                  competition.primary.competition.view(),
                  competition.primary.season.view()))
    return kNoSource;
  return store.sourceSlot(store.resolve(competition.id, key, clock));
}
StoredMatch* put(MatchStore& store,
                 const settings::CompetitionRoute& competition,
                 const Fixture& fixture, Clock clock, int64_t kickoff,
                 Phase phase, uint8_t minute = kNoValue, Goals score = {},
                 uint8_t round = 6, bool flash = false, bool details = true) {
  const auto slot = source(store, competition, clock);
  StoredMatch row;
  row.id = fixture.id;
  row.source = slot;
  row.kickoff = static_cast<uint32_t>(kickoff);
  row.phase = phase;
  row.minute = minute;
  row.principal = score;
  row.round = round;
  row.home = store.team(slot, fixture.home.id, fixture.home.name,
                        fixture.home.short_name);
  row.away = store.team(slot, fixture.away.id, fixture.away.name,
                        fixture.away.short_name);
  if (running(phase) || phase == Phase::kSuspended)
    row.first_live_observed_utc = kKickoff;
  if (phase == Phase::kFinished) row.finished_at = row.kickoff + 95 * 60;
  if (flash) row.score_changed = std::max<uint32_t>(1, clock.mono_s);
  auto* stored = store.upsert(row, clock);
  if (stored != nullptr && details)
    stored = store.ensureDetail(store.key(*stored), competition.id, clock);
  return stored;
}
void events(MatchStore& store, const StoredMatch& match, const Event* input,
            unsigned count, uint8_t through, Clock clock,
            unsigned shot_limit = 255) {
  if (!store.beginEvents(match, false, clock.utc)) return;
  unsigned shots = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (input[i].minute > through) continue;
    if (input[i].shootout && shots++ >= shot_limit) continue;
    StoredEvent event;
    // Stable demo event identity, deliberately not a provider event-id claim.
    event.identity = match.id + i + 1;
    event.type = input[i].type;
    event.actor = input[i].side;
    event.minute = input[i].minute;
    event.added = input[i].added;
    event.player.assign(input[i].player);
    if (input[i].shootout) event.flags |= kEventShootout;
    store.addEvent(event);
  }
  store.endEvents(true);
}
void table(MatchStore& store, const settings::CompetitionRoute& competition) {
  const auto slot = source(store, competition, {});
  if (store.tableState(slot) == DatasetState::kComplete) return;
  store.beginTable(slot);
  for (unsigned i = 0; i < std::size(kConference); ++i) {
    StoredRow row;
    const auto club = competition.club.team;
    const auto& team = i == 0 && club == kUnion.home.id ? kUnion.home
                       : i == 0 && club == kAugsburg.away.id
                           ? kAugsburg.away
                           : kConference[i].home;
    row.team = store.team(slot, team.id, team.short_name);
    row.position = i + 1;
    row.played = 6;
    row.points = static_cast<int16_t>(15 - i * 2);
    row.difference = static_cast<int16_t>(6 - i * 2);
    store.appendRow(row);
  }
  store.commitTable(true);
}
}  // namespace

bool valid(Scenario scenario) {
  return static_cast<unsigned>(scenario) < kScenarioCount;
}
const char* key(Scenario scenario) {
  switch (scenario) {
    case Scenario::kOwn: return "own";
    case Scenario::kOrdinary: return "ordinary";
    case Scenario::kConference: return "conference";
    case Scenario::kCup: return "cup";
    case Scenario::kRounds: return "rounds";
    case Scenario::kBreak: return "break";
    case Scenario::kEmpty: return "empty";
  }
  return "";
}
const char* title(Scenario scenario) {
  switch (scenario) {
    case Scenario::kOwn: return "Own match";
    case Scenario::kOrdinary: return "Ordinary match";
    case Scenario::kConference: return "Five-match conference";
    case Scenario::kCup: return "Extra time and shootout";
    case Scenario::kRounds: return "Rounds and rescheduled fixture";
    case Scenario::kBreak: return "Proven synthetic break";
    case Scenario::kEmpty: return "Empty successful response";
  }
  return "";
}
uint8_t steps(Scenario scenario) {
  switch (scenario) {
    case Scenario::kOwn: return 9;
    case Scenario::kOrdinary: return 6;
    case Scenario::kConference: return 3;
    case Scenario::kCup: return 5;
    case Scenario::kRounds: return 3;
    case Scenario::kBreak:
    case Scenario::kEmpty: return 1;
  }
  return 0;
}
const char* stepTitle(Scenario scenario, uint8_t step) {
  if (step >= steps(scenario)) return "";
  switch (scenario) {
    case Scenario::kOwn: {
      constexpr const char* labels[]{
          "Before kickoff",           "First booking",
          "Second booking",           "Half time",
          "Goal and two own matches", "Justified interruption",
          "Live without kickoff",     "Final whistle",
          "Result on following day"};
      return labels[step];
    }
    case Scenario::kOrdinary: {
      constexpr const char* labels[]{
          "Before kickoff",       "First goal",
          "Live without kickoff", "Justified interruption",
          "Finished post-window", "Foreign result outside window"};
      return labels[step];
    }
    case Scenario::kConference:
      return step == 0   ? "Before kickoff"
             : step == 1 ? "Five live matches"
                         : "Mixed live and finished";
    case Scenario::kCup: {
      constexpr const char* labels[]{"Before kickoff", "Extra time",
                                     "Shootout begins",
                                     "Reported scored attempts",
                                     "Final shootout with visible event limit"};
      return labels[step];
    }
    case Scenario::kRounds:
      return step == 0   ? "Current round with later rescheduled fixture"
             : step == 1 ? "Held round and next round"
                         : "Next cluster of an earlier round";
    case Scenario::kBreak: return "Two competitions with a gap over 21 days";
    case Scenario::kEmpty: return "No matches and no table";
  }
  return "";
}
Bindings configure(settings::CompetitionRoutes& routes, Scenario scenario,
                   bool saved_competitions) {
  Bindings bindings;
  if (!valid(scenario)) return bindings;
  struct Preferences {
    bool own_special = true;
    uint8_t group = 0;
  };
  std::array<Preferences, 2> preferences{};
  for (unsigned i = 0; i < bindings.size(); ++i) {
    if (saved_competitions && i < routes.entries.size()) {
      const auto& saved = routes.entries[i];
      if (saved.id != 0 && saved.incarnation != 0) {
        bindings[i] = {saved.incarnation, saved.id, true};
        preferences[i] = {saved.club.own_special, saved.club.group};
      }
    }
  }
  for (unsigned i = 0; i < bindings.size(); ++i) {
    if (bindings[i].saved) continue;
    uint8_t id = kFirstCompetition + i;
    while (std::any_of(
        bindings.begin(), bindings.end(),
        [id](const Binding& binding) { return binding.competition == id; }))
      --id;
    bindings[i] = {0, id, false};
  }
  routes.clear();
  for (unsigned i = 0; i < 2; ++i) {
    // Construct directly in the caller's existing storage, not a 2.5 KiB copy.
    auto& entry = routes.entries.items[i];
    new (&entry) settings::CompetitionRoute{};
    entry.id = bindings[i].competition;
    entry.incarnation = bindings[i].incarnation;
    entry.active = true;
    if (bindings[i].saved) {
      entry.club.own_special = preferences[i].own_special;
      entry.club.group = preferences[i].group;
    }
    entry.primary.provider = kProvider;
    entry.primary.competition.assign(
        i && scenario == Scenario::kCup ? "ger.dfb_pokal" : "ger.1");
    entry.primary.season.assign("2026");
    entry.club.team = i ? (scenario == Scenario::kOwn ? 132 : 0) : 598;
    entry.competition_name.assign(i ? "Demo second competition"
                                    : "Demo competition");
    entry.club.name.assign(
        i ? (scenario == Scenario::kOwn ? "Bayern Munich" : "")
          : "1. FC Union Berlin");
  }
  routes.entries.count = 2;
  return bindings;
}

Clock apply(MatchStore& store, const settings::CompetitionRoutes& routes,
            const Bindings& bindings, Scenario scenario, uint8_t step,
            uint32_t realtime_mono_s) {
  const auto* first = competition(routes, bindings[0]);
  const auto* second = competition(routes, bindings[1]);
  if (!valid(scenario) || step >= steps(scenario) || !first || !second)
    return {};
  Clock clock{kKickoff, realtime_mono_s};
  if (scenario == Scenario::kOwn) {
    constexpr int32_t offsets[]{-900,    7 * 60,   21 * 60,
                                45 * 60, 81 * 60,  82 * 60,
                                85 * 60, 100 * 60, timekeeping::kSecondsPerDay};
    clock.utc += offsets[step];
  } else if (scenario == Scenario::kOrdinary) {
    constexpr int32_t offsets[]{-900,    30 * 60,  31 * 60,
                                32 * 60, 100 * 60, timekeeping::kSecondsPerDay};
    clock.utc += offsets[step];
  } else if (scenario == Scenario::kConference)
    clock.utc += step == 0 ? -900 : step == 1 ? 30 * 60 : 100 * 60;
  else if (scenario == Scenario::kCup) {
    constexpr int32_t offsets[]{-900, 105 * 60, 151 * 60, 155 * 60, 160 * 60};
    clock.utc += offsets[step];
  } else if (scenario == Scenario::kRounds)
    clock.utc += step == 0 ? 0 : step == 1 ? 3 * kDay : 8 * kDay;
  store.clear();
  if (scenario == Scenario::kEmpty) return clock;
  table(store, *first);
  table(store, *second);
  if (scenario == Scenario::kOwn || scenario == Scenario::kOrdinary) {
    const bool own = scenario == Scenario::kOwn;
    const auto& fixture = own ? kUnion : kAugsburg;
    Phase phase = step == 0               ? Phase::kScheduled
                  : own && step == 3      ? Phase::kHalfTime
                  : step == (own ? 5 : 3) ? Phase::kSuspended
                  : step >= (own ? 7 : 4) ? Phase::kFinished
                                          : Phase::kLive;
    uint8_t minute = phase == Phase::kScheduled  ? kNoValue
                     : phase == Phase::kFinished ? 90
                     : phase == Phase::kHalfTime
                         ? 45
                         : static_cast<uint8_t>((clock.utc - kKickoff) / 60);
    Goals score = step == 0   ? Goals{}
                  : own       ? (step >= 4 ? Goals{1, 0} : Goals{0, 0})
                  : step >= 4 ? Goals{2, 2}
                              : Goals{1, 0};
    auto* match = put(store, own ? *first : *second, fixture, clock,
                      step == (own ? 6 : 2) ? 0 : kKickoff, phase, minute,
                      score, 6, step == (own ? 4 : 1));
    if (match)
      events(store, *match, own ? kUnionEvents : kAugsburgEvents,
             own ? std::size(kUnionEvents) : std::size(kAugsburgEvents),
             minute == kNoValue ? 0 : minute, clock);
    if (own && (step == 4 || step == 5))
      put(store, *second, kAugsburg, clock, kKickoff + 3600, Phase::kLive,
          step == 4 ? 21 : 22, {1, 0});
  } else if (scenario == Scenario::kConference) {
    for (unsigned i = 0; i < std::size(kConference); ++i)
      put(store, *first, kConference[i], clock, kKickoff,
          step == 0             ? Phase::kScheduled
          : step == 2 && i == 0 ? Phase::kFinished
                                : Phase::kLive,
          step == 0   ? kNoValue
          : step == 1 ? 30
                      : 100,
          step == 0
              ? Goals{}
              : Goals{static_cast<uint8_t>(i % 3), static_cast<uint8_t>(i % 2)},
          6, step == 1, false);
  } else if (scenario == Scenario::kCup) {
    auto* match = put(store, *second, kCup, clock, kKickoff,
                      step == 0   ? Phase::kScheduled
                      : step == 1 ? Phase::kExtraTime
                      : step < 4  ? Phase::kPenalties
                                  : Phase::kFinished,
                      step == 0   ? kNoValue
                      : step == 1 ? 105
                                  : 120,
                      step == 0 ? Goals{} : Goals{1, 1}, 1);
    if (match) {
      if (step >= 2) match->shootout = step == 2 ? Goals{1, 1} : Goals{2, 4};
      if (step == 4) match->finished_at = kKickoff + 156 * 60;
      // Stage two uses only the first two reported scored attempts. No missed
      // attempt, winner sequence or full shootout coverage is fabricated.
      events(store, *match, kCupEvents, std::size(kCupEvents),
             step == 0   ? 0
             : step == 1 ? 105
                         : 125,
             clock, step == 2 ? 2 : 255);
    }
  } else if (scenario == Scenario::kRounds) {
    put(store, *first, kUnion, clock, kKickoff - 7 * kDay, Phase::kFinished, 90,
        {1, 0}, 5);
    for (unsigned i = 0; i < 4; ++i) {
      constexpr int32_t offsets[]{3600, 2 * kDay, 10 * kDay, 7 * kDay};
      const int64_t kickoff = kKickoff + offsets[i];
      const bool finished = clock.utc >= kickoff + 95 * 60;
      put(store, *first, kConference[i], clock, kickoff,
          finished ? Phase::kFinished : Phase::kScheduled,
          finished ? 90 : kNoValue, finished ? Goals{0, 0} : Goals{},
          i == 3 ? 7 : 6, false, false);
    }
  } else if (scenario == Scenario::kBreak) {
    put(store, *first, kUnion, clock, kKickoff - 20 * kDay, Phase::kFinished,
        90, {1, 0}, 1);
    put(store, *first, kConference[0], clock, kKickoff + 30 * kDay,
        Phase::kScheduled, kNoValue, {}, 2, false, false);
    put(store, *second, kAugsburg, clock, kKickoff - 25 * kDay,
        Phase::kFinished, 90, {2, 2}, 1);
    put(store, *second, kConference[4], clock, kKickoff + 35 * kDay,
        Phase::kScheduled, kNoValue, {}, 2, false, false);
  }
  return clock;
}
bool breakProven(Scenario scenario, uint16_t threshold_days) {
  // Latest previous kickoff is -20 days; earliest next kickoff is +30 days.
  // The owner's threshold uses strict elapsed kickoff time, not civil dates.
  return scenario == Scenario::kBreak && threshold_days < 50;
}
}  // namespace football::demo
