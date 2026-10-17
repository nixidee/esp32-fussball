// Repeatable on-device scenarios in the production canonical MatchStore.
// Provider identities/names/events come from recorded public ESPN responses;
// clocks, intermediate states and fixture placement are explicitly synthetic.
// The engine owns no state, setting, route copy, renderer or network operation.
#pragma once

#include <array>
#include <cstdint>

#include "match_store.h"
#include "settings_model.h"

namespace football::demo {
enum class Scenario : uint8_t {
  kOwn,
  kOrdinary,
  kConference,
  kCup,
  kRounds,
  kBreak,
  kEmpty
};
inline constexpr uint8_t kScenarioCount = 7;
inline constexpr uint8_t kFirstCompetition = 249, kSecondCompetition = 250;
inline constexpr uint32_t kKickoff = 1792243800;  // 2026-10-17 13:30 UTC
// Each simulated dataset borrows the exact saved identity in the same list
// position. An absent saved entry has a dedicated synthetic id with incarnation
// zero: policy association resolution then guarantees shared rules only.
struct Binding {
  uint32_t incarnation = 0;
  uint8_t competition = 0;
  bool saved = false;
};
using Bindings = std::array<Binding, 2>;
static_assert(sizeof(Bindings) <= 16);

bool valid(Scenario scenario);
const char* key(Scenario scenario);
const char* title(Scenario scenario);
uint8_t steps(Scenario scenario);
const char* stepTitle(Scenario scenario, uint8_t step);
// Replace only the caller's worker route snapshot, in place. Never save these
// dedicated demo identities or modify the owner's screen policy/settings.
Bindings configure(settings::Competitions& routes,
                   Scenario scenario = Scenario::kOwn,
                   bool saved_competitions = true);
// Rebuild the same canonical store at one deterministic test instant. Mono is
// the real monotonic second, solely for existing goal-flash/event-age behavior.
// An invalid selection leaves both store and routes untouched and returns {}.
Clock apply(MatchStore& store, const settings::Competitions& routes,
            const Bindings& bindings, Scenario scenario, uint8_t step,
            uint32_t realtime_mono_s);
// This is an explicitly synthetic demo proof, never provider coverage evidence
// and never written to the football-cache/coverage files.
bool breakProven(Scenario scenario,
                 uint16_t threshold_days = cfg::kScreenBreakDaysDefault);
}  // namespace football::demo
