#pragma once
#include "esp_err.h"
#include "football_model.h"
namespace football {
// No competition caused the last error.
inline constexpr uint8_t kNoRoute = 0xff;
struct Status {
  uint32_t revision, requests[cfg::kProviderCount], generation;
  int64_t fetched_ms;
  uint8_t matches, rows;
  // Index of the competition whose fetch failed last; kNoRoute after a
  // successful round.
  uint8_t error_route;
  bool stale, busy;
  Text<96> error;
  Text<96> routing_notice;
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
};
// What the published data says about the club's current or next match and
// about each competition (Web UI overview).
struct OwnMatch {
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
  OwnMatch own;
  RouteInfo routes[cfg::kCompetitionMax];
};
esp_err_t init();
void refresh();
Status status();
// Called while holding the short repository read mutex. Callback must copy
// into its own fixed model, with no I/O, LVGL, allocation or blocking calls.
using Reader = void (*)(const Snapshot&, void*);
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
}  // namespace football
