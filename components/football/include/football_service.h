#pragma once
#include <array>

#include "demo_scenarios.h"
#include "esp_err.h"
#include "football_model.h"
#include "match_store.h"
#include "provider_query.h"
#include "store_views.h"
namespace football {
// Why no provider request runs before the scheduler's next wake time (Web
// UI overview). kNone until the first decision.
enum class Cadence : uint8_t {
  kNone,
  kLive,        // a shown match in its live window is running
  kPrematch,    // a live window is open before kickoff
  kPostmatch,   // a match in its live window has finished
  kIdle,        // no live window open; the next data type is due later
  kWindow,      // the next live window opens at the wake time
  kBudget,      // a daily cap stretches or skips data types
  kBudgetUsed,  // a daily cap is used up until the end of the UTC day
  kLimit,       // a window limit (provider or global) delays the request
  kRetry,       // a failed request is retried, or the provider paused us
  kWaiting,     // no network or no valid time: starts once both are there
  kUpdateTest   // paused while a new firmware is in its update test
};
const char* cadenceName(Cadence cadence);
// The state of one competition (index = route = position in the competition
// list) for the display and the Web UI (OQ-278..OQ-282).
struct CompetitionState {
  // Monotonic ms of the last successful request (0 = none yet); a failed
  // competition keeps its earlier data.
  int64_t fetched_ms = 0;
  uint8_t id = 0;  // stable competition id, 0 = unused route
  bool active = false;
  // kNone: the last request succeeded. Otherwise why the last request
  // failed, or kUnmaintained: the provider does not maintain the
  // competition (MatchStore::unmaintained()), or kStoreFull: the device
  // could not hold all its data (OQ-310). A failed request goes first.
  Cause cause = Cause::kNone;
  // MatchStore::notices() of the competition: which data did not fit.
  uint8_t notices = 0;
  // Provider of the failed request (or of the unmaintained matches) and the
  // HTTP status it answered with (0 = no HTTP error status).
  cfg::Provider provider = cfg::kProviderDefault;
  uint16_t http = 0;
  // The last request failed, or a shown match of the competition is in its
  // live window while its last successful match request is older than
  // kDataStaleS.
  bool stale = false;
};
using CompetitionStates = std::array<CompetitionState, cfg::kCompetitionMax>;
// The state of competition 'id' (nullptr = not in the list).
inline const CompetitionState* findState(const CompetitionStates& states,
                                         uint8_t id) {
  for (const auto& state : states)
    if (id != 0 && state.id == id) return &state;
  return nullptr;
}
// Optional REST-only coherent projection. UI/debug keep the narrow summary;
// HTTP never sends while the repository is locked.
using DatasetSnapshots =
    std::array<std::array<DatasetStatus, 3>, cfg::kCompetitionMax>;
struct Status {
  // False until init() ran: no other field is set.
  bool started;
  uint32_t revision, requests[cfg::kProviderCount], generation;
  // Monotonic time of the oldest last successful request over the active
  // competitions (a failed competition keeps earlier data); 0 = none yet.
  int64_t fetched_ms;
  uint16_t matches;
  bool stale, busy;
  CompetitionStates competitions;
  // Monotonic time the next request is due (0 = none scheduled: kNone,
  // kWaiting, kUpdateTest, nothing planned) and why. An owner refresh
  // starts earlier.
  int64_t next_ms;
  Cadence cadence;
};
using SelectionQuery = uint64_t;
struct SelectionStatus {
  SelectionQuery query = 0;
  uint32_t revision;
  uint16_t count;
  // Why the last selection failed (kNone = it did not), the provider asked
  // and its HTTP error status (0 = none), as in CompetitionState.
  Cause cause;
  cfg::Provider provider;
  uint16_t http;
  bool busy, more, teams;
  // While busy: what holds the next request (kLimit: a window limit,
  // kRetry: the provider paused us; kNone: nothing) and the monotonic ms it
  // may run (0 = none).
  Cadence wait;
  int64_t wait_ms;
};
// What the published data says about the club's current or next match and
// about each competition (Web UI overview).
struct OwnOverview {
  bool present = false;
  uint8_t competition = 0;  // canonical identity of the published dataset
  Phase phase = Phase::kUnknown;
  cfg::Provider provider = cfg::kProviderDefault;
  uint32_t home_id = 0, away_id = 0;
  int64_t kickoff = 0;  // UTC seconds, 0 = unknown
  int16_t home_goals = -1, away_goals = -1, minute = -1;  // -1 = unknown
  Text<cfg::kNameBytes> home, away;
};
struct RouteInfo {
  uint8_t id = 0;  // canonical published id; 0 = no dataset in this position
  uint16_t matches = 0;  // matches of the competition in the published data
  cfg::Provider provider = cfg::kProviderDefault;  // source of those matches
};
struct Overview {
  OwnOverview own;
  RouteInfo routes[cfg::kCompetitionMax];
  bool demo = false;
  demo::Bindings demo_bindings;
};
esp_err_t init();
void refresh();
Status status();
struct DemoStatus {
  bool enabled = false, pending = false;
  demo::Scenario scenario = demo::Scenario::kOwn;
  uint8_t step = 0;
  uint32_t revision = 0;
  Clock clock;
  demo::Bindings bindings;
};
// Runtime-only device scenario controls. Enabling Demo remains an explicit
// general setting; these commands never write settings, files or system time.
DemoStatus demoStatus();
esp_err_t demoScenario(demo::Scenario scenario, uint8_t step);
// The published data: the compact store with the sources it shows
// (store_views.h). 'fetched_ms', 'stale' and 'competitions' as in Status;
// 'retry_ms' is the monotonic ms of the next retry of a failed competition
// (0 = none). Team labels in the store are original compact provider fallback.
struct Published {
  const MatchStore& store;
  const view::Shown& shown;
  uint32_t revision, generation;
  bool configured, stale;
  int64_t fetched_ms, retry_ms;
  const CompetitionStates& competitions;
  // Borrowed from the worker while this read holds the repository mutex.
  // Use only with the UI snapshot's matching data generation. These are the
  // effective canonical routes, also used by the device Demo.
  const settings::CompetitionRoutes& routes;
  bool break_proven;
  Clock clock;  // Demo clock when enabled; otherwise ordinary UTC/monotonic
};
// Called while holding the short repository read mutex. Callback must copy
// into its own fixed model, with no I/O, LVGL, allocation or blocking calls.
using Reader = void (*)(const Published&, void*);
void read(Reader reader, void* context);
// Own match (a live one first, else the one closest to 'utc') and the match
// count per competition of the published data. No provider request.
// Optional Status is filled in place under the same repository lock as the
// overview: rows, causes/notices and Demo identity bindings share a revision.
Overview overview(int64_t utc, Status* status_out = nullptr,
                  DatasetSnapshots* datasets_out = nullptr);
// Query identity includes a boot nonce. Only this matching owner can read,
// renew the idle lease or cancel. A cancelled in-flight request keeps the
// single interactive slot until the provider worker acknowledges completion.
esp_err_t select(const SelectionRequest& request, SelectionQuery& query);
// Explicit original-name query; same worker/admission slot, no adoption.
struct RequestOutcome;
namespace provider {
struct OriginalsSink;
}
// Synchronous producer/codec inversion. Callback code has process lifetime;
// context and provider sink are borrowed only during one worker invocation.
using OriginalNamesProducer = RequestOutcome (*)(const provider::OriginalsSink&,
                                                 void*);
using OriginalNamesExport = RequestOutcome (*)(uint32_t,
                                               const settings::Source&,
                                               OriginalNamesProducer, void*);
esp_err_t beginOriginalNames(uint8_t competition, uint32_t& query,
                             OriginalNamesExport encode);
esp_err_t selectionStatus(SelectionQuery query, SelectionStatus& out);
bool selectionAt(SelectionQuery query, uint16_t index, uint32_t revision,
                 CompetitionSelection& out);
bool selectionAt(SelectionQuery query, uint16_t index, uint32_t revision,
                 TeamSelection& out);
esp_err_t cancelSelection(SelectionQuery query);
// Volatile provider fallback labels of teams in the published matches/table.
// Adopted names belong to the separate full collection; no filesystem read.
// 'revision' of the published data guards a consistent read across count
// and At: At() fails after a newer publication.
struct TeamInfo {
  uint32_t id = 0;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  Text<cfg::kTeamNameBytes> name;
  Text<cfg::kTeamShortBytes> short_name;
};
uint16_t teamCount(uint8_t competition, uint32_t& revision);
bool teamAt(uint8_t competition, uint16_t index, uint32_t revision,
            TeamInfo& out);
// Blocks every working-identity mutation through the explicit reset workflow.
esp_err_t beginReset();
void endReset();
}  // namespace football
