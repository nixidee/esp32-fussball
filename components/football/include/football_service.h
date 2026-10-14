#pragma once
#include "esp_err.h"
#include "football_model.h"
#include "football_files.h"
#include "match_store.h"
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
struct Status {
  uint32_t revision, requests[cfg::kProviderCount], generation;
  // Monotonic time of the oldest last successful request over the active
  // competitions (a failed competition keeps earlier data); 0 = none yet.
  int64_t fetched_ms;
  uint8_t matches, rows;
  // Bit i set: the last request of competition i's primary source failed
  // and no fallback route stands in; it shows earlier data (or none) until
  // a later request succeeds.
  uint8_t error_routes;
  bool stale, busy;
  // Not every match fitted the match list; it holds the nearest ones.
  bool matches_truncated;
  Text<96> error;
  Cause cause;  // stable key of 'error'
  Text<96> routing_notice;
  // Monotonic time the next request is due (0 = none scheduled: kNone,
  // kWaiting, kUpdateTest, nothing planned) and why. An owner refresh
  // starts earlier.
  int64_t next_ms;
  Cadence cadence;
};
struct SelectionRequest {
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  bool teams = false;
  // Fixtures of the fallback source of competition 'route' (selectFixtures).
  bool fixtures = false;
  uint8_t route = 0;
  Id competition, season;
  Text<cfg::kSelectionTextMaxBytes> search;
  Text<cfg::kSelectionTextMaxBytes> country;
  uint16_t offset = 0;
};
struct SelectionStatus {
  bool busy, more;
  // The table holds fixtures of competition 'route', not a provider list.
  bool fixtures;
  uint8_t route;
  uint16_t count;
  uint32_t revision;
  Text<96> error;
  Cause cause;  // stable key of 'error'
};
// What the published data says about the club's current or next match and
// about each competition (Web UI overview).
struct OwnOverview {
  bool present = false;
  uint8_t route = 0;
  Phase phase = Phase::kUnknown;
  int64_t kickoff = 0;  // UTC seconds, 0 = unknown
  int16_t home_goals = -1, away_goals = -1, minute = -1;  // -1 = unknown
  Text<cfg::kNameBytes> home, away;
};
struct RouteInfo {
  uint8_t matches = 0;  // matches of the competition in the published data
  cfg::Provider provider = cfg::kProviderDefault;  // source of those matches
};
struct Overview {
  OwnOverview own;
  RouteInfo routes[cfg::kCompetitionMax];
};
esp_err_t init();
void refresh();
Status status();
// The published data: the compact store with the sources it shows
// (store_views.h). 'fetched_ms', 'stale', 'error' and 'retry_ms' as in
// Status; 'names' are the owner's replacements of team short names
// (view::teamName()).
struct Published {
  const MatchStore& store;
  const view::Shown& shown;
  const persist::ShortNames& names;
  uint32_t revision, generation;
  bool configured, stale;
  int64_t fetched_ms, retry_ms;
  const Text<96>& error;
  Cause cause;
};
// Called while holding the short repository read mutex. Callback must copy
// into its own fixed model, with no I/O, LVGL, allocation or blocking calls.
using Reader = void (*)(const Published&, void*);
void read(Reader reader, void* context);
// Own match (a live one first, else the one closest to 'utc') and the match
// count per competition of the published data. No provider request.
Overview overview(int64_t utc);
esp_err_t select(const SelectionRequest& request);
// Starts the job that loads the club's matches at the fallback source of
// competition 'route' into the selection table (pair picker). Entry: id =
// provider fixture id, season = kickoff in UTC seconds as text (empty if
// unknown), name = "home - away". Same busy rule and request limits as
// select(). ESP_ERR_INVALID_ARG: no such competition or no complete
// fallback source.
esp_err_t selectFixtures(uint8_t route);
SelectionStatus selectionStatus();
bool selectionAt(uint16_t index, uint32_t revision, Selection& out);
// The club's matches of competition 'route' in the published data that came
// from 'provider' (the primary source of the competition), in the same entry
// form as selectFixtures(). 'revision' of the published data guards a
// consistent read across count and At: At() fails after a newer publication.
uint16_t publishedFixtureCount(uint8_t route, cfg::Provider provider,
                               uint32_t& revision);
bool publishedFixtureAt(uint8_t route, cfg::Provider provider, uint16_t index,
                        uint32_t revision, Selection& out);
// Teams of competition 'competition' in the published data (its matches
// and table rows, every provider) with the owner's short name ("" = none).
// 'revision' guards a consistent read across count and At, as above.
struct TeamInfo {
  uint32_t id = 0;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  Text<26> short_name, custom;
};
uint16_t teamCount(uint8_t competition, uint32_t& revision);
bool teamAt(uint8_t competition, uint16_t index, uint32_t revision,
            TeamInfo& out);
// Sets the owner's short name of a team (an empty name removes it) and
// writes the short-name file; the caller holds the heavy-operation token.
// ESP_ERR_NO_MEM: the table, the football file budget or the storage is
// full (the previous name stays); ESP_ERR_INVALID_STATE: no storage or
// update test.
esp_err_t setShortName(cfg::Provider provider, uint32_t id,
                       std::string_view name);
// Replaces every short name by 'table' (exchanged: 'table' then holds the
// previous names) and writes the short-name file once; errors and the
// caller's token as in setShortName(), and the previous names stay.
esp_err_t replaceShortNames(persist::ShortNames& table);
// A copy of the owner's short names (settings export). Any task.
void shortNames(persist::ShortNames& out);
// images::format() wiped the files: the competition files and the short
// names are written again from RAM. Any task.
void filesLost();
// Deletes every football file (settings reset before the restart); the
// caller holds the heavy-operation token.
void eraseFiles();
}  // namespace football
