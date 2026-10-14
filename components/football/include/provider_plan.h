// Provider request plans (pure logic): which provider requests one run of a
// scheduled job makes, their URLs, and where each streamed element goes in
// the match store. A run can take several requests (OpenLigaDB: current and
// next round plus the club's fixtures; ESPN: match days, then one scoreboard
// per day); the scheduler keeps such a job due between them
// (Scheduler::finished(..., more)). No I/O and no ESP-IDF, so the native
// tests drive it with recorded responses. Not thread-safe: the provider
// worker owns it.
//
// Per provider and data type (OQ-216, OQ-218):
//   OpenLigaDB  core       getavailableteams/{league}/{season}
//               fixtures   getmatchdata/{league} (current round, also the
//                          current season), getmatchdata/…/{round + 1},
//                          getmatchesbyteamid/{club}/… (club only)
//               table      getbltable/{league}/{season} (also live table)
//               live       per round with a match in its live window:
//                          getlastchangedate/…/{round}, then
//                          getmatchdata/…/{round} when the date changed or
//                          the last full request is kOpenLigaDbFullRefreshS
//                          old; goals come with the match data
//   ESPN        core       {slug}/teams
//               fixtures   calendar/ondays (match days), then the scoreboard
//                          of every day of the current and the next matchday
//               table      site.web.api standings (also live table; cups
//                          have none, an absent list is an empty table)
//               live       the scoreboard of each match day with a match in
//                          its live window; goals and cards come with it
//               events     the match summary of the own match (its
//                          substitutions, OQ-207): the latest own match
//                          whose live window has opened, once it has
//                          kicked off; body cap kProviderSummaryBodyBytes
//   API-Football, football-data.org: generic test sources (OQ-205): teams,
//   fixtures within kFixtureHorizonDays, standings, today's fixtures.
// Other providers make no own-match events request: OpenLigaDB delivers
// goals with the live data, API-Football events are removed (OQ-206).
//
// Data types per competition by its screen switches (OQ-192): core data and
// fixtures while it is shown on a screen with match or table data (own
// match only with a club); live for Konferenz, matchday and the own match
// (with a club); table and live table for the table screen; own-match
// events for the own match with an ESPN primary source and a club. The
// fallback's data types are limited the same way.
//
// Fallback source (fallback_active): its rows fill the paired primary
// matches (MatchStore::fill, confirmed values only). Wanted per data type:
//   live         while the competition has active fixture pairs or the
//                primary's last match request failed
//   fixtures     while the primary's last match request failed
//   table, live table
//                while the primary has no table (a cup, or a failed or
//                empty table run); skipped while the store holds the
//                primary's table
//   core data    never (the match rows carry the team names)
// While the primary's last match request (fixtures or live) failed, every
// fallback row is stored on its own (none fills) and the store notice
// kNoticeFallbackRoute is set; the primary's next successful match request
// clears it and removes the fallback rows. A failed fallback request sets
// kNoticeFallbackFailed, a successful one clears it.

#pragma once

#include <ArduinoJson.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_config.h"
#include "match_store.h"
#include "request_scheduler.h"
#include "settings_model.h"
#include "store_mappers.h"

namespace football::plan {

inline constexpr std::size_t kUrlBytes = 320;
// ESPN match days kept per source: the current and the next matchday.
inline constexpr std::size_t kEspnDays = 8;
// Rounds or match days one live run checks.
inline constexpr std::size_t kLiveItems = 3;

// Where the elements of a response go.
enum class Sink : uint8_t {
  kMatches,  // storeMatch(); OpenLigaDB rows also give season and round
  kTable,    // storeTableRow() between beginTable() and commitTable()
  kTeams,    // storeTeam()
  kChange,   // OpenLigaDB change date (one JSON string)
  kDays,     // ESPN match days (calendar/ondays "dates")
  kSeason,   // API-Football current season (/leagues "response")
  kEvents,   // storeSummaryEvent() between beginEvents() and endEvents()
};

struct Request {
  char url[kUrlBytes]{};
  // Array whose elements are streamed (fetchJson): "" a top-level array,
  // nullptr the whole document.
  const char* array = "";
  // The array may be absent (a cup without table): not a failure.
  bool optional = false;
  Sink sink = Sink::kMatches;
  // Most body bytes read (fetchJson body_limit).
  std::size_t body = cfg::kProviderBodyBytes;
};

// Result of finish().
enum class Step : uint8_t {
  kDone,     // the run is complete
  kMore,     // the run continues with another request
  kInvalid,  // the response lacked what the run needs (no season): failed
};

// What one competition source keeps between requests (RAM only).
struct SourceState {
  uint32_t key = 0;  // hash of the competition id and its source settings
  settings::Bytes<cfg::kSeasonMaxBytes> season;  // resolved current season
  uint8_t round = 0;  // OpenLigaDB current round, 0 = unknown
  // OpenLigaDB change date per checked round, with the time of the last
  // full request of that round.
  struct Mark {
    uint8_t round = 0;
    uint32_t change = 0;  // hash of the change date
    uint32_t at = 0;      // monotonic seconds
  };
  std::array<Mark, kLiveItems> marks{};
  // ESPN match days (UTC start of each provider day) of the current and the
  // next matchday, ascending.
  std::array<uint32_t, kEspnDays> days{};
  uint8_t day_count = 0;
  // The live run in progress: its rounds (OpenLigaDB) or match days (ESPN,
  // 0 = the scoreboard without date), and the change date just checked.
  std::array<uint32_t, kLiveItems> live{};
  uint8_t live_count = 0;
  uint32_t checked = 0;
  // Position of each data type's run: step and item within the step.
  struct Progress {
    uint8_t step = 0, at = 0;
  };
  std::array<Progress, cfg::kDataTypeCount> progress{};
  // Primary source only: the last match request (fixtures or live) failed,
  // the last table run failed, the last complete table run left no table.
  bool failing : 1 = false;
  bool table_failing : 1 = false;
  bool no_table : 1 = false;
};

// What a source resolved that a competition file keeps (football_files.h):
// its current season, OpenLigaDB round, ESPN match days and whether its
// last complete table run left no table.
struct Resolved {
  settings::Bytes<cfg::kSeasonMaxBytes> season;
  uint8_t round = 0;
  std::array<uint32_t, kEspnDays> days{};
  uint8_t day_count = 0;
  bool no_table = false;
};

class Planner {
 public:
  void reset();
  // The source states of slot 'slot' start afresh (its data was dropped).
  void forget(uint8_t slot);
  // The resolved part of a source state; empty when the state does not
  // belong to the current sources of 'entry'.
  Resolved saved(uint8_t slot, schedule::Source source,
                 const settings::Competition& entry) const;
  // Restores a saved source state for the current sources of 'entry' (as
  // after its requests); a run in progress starts over.
  void restore(uint8_t slot, schedule::Source source,
               const settings::Competition& entry, const Resolved& resolved);
  // The fixture pairs of all competitions (the competition store's list);
  // pairs with an id that is not a decimal number are ignored.
  template <class Pairs>
  void setPairs(const Pairs& pairs) {
    pairing_.count = 0;
    for (const auto& p : pairs) addPair(p);
  }
  // Scheduler inputs of slot 'slot' (entry null, inactive or without
  // competition: unused); the fallback source as described above.
  void wanted(uint8_t slot, const settings::Competition* entry,
              schedule::Wanted& out) const;
  // The next request of 'job' for competition 'entry' (its slot and id);
  // prepares the streaming into 'store' (a table request begins the
  // staging). False when the run has nothing (more) to request: the run is
  // complete without a request.
  bool request(const schedule::Job& job, const settings::Competition& entry,
               MatchStore& store, const Clock& clock, Request& out);
  // Fixture picker: the fixtures run of the fallback source of 'entry' in
  // pick mode (MapContext::pick): the club's matches go to 'pick', nothing
  // is stored and no source state or notice changes. beginPick() starts a
  // run; pickRequest() is then called until it returns false or finish()
  // returns kDone.
  void beginPick() { pick_state_ = {}; }
  bool pickRequest(const settings::Competition& entry, MatchStore& store,
                   const Clock& clock, Request& out, MatchPick pick,
                   void* context);
  // fetchJson consumer for the prepared request; 'planner' is this.
  static bool consume(ArduinoJson::JsonVariantConst element, void* planner);
  // Ends the prepared request; 'ok' = the response was complete. A failure
  // ends the run (the next run starts from its first request).
  Step finish(bool ok, const Clock& clock);
  // Ends the prepared request and its run without an outcome (settings or
  // network changed, the request was not sent): a staged table is dropped,
  // no source state or notice changes; the next run starts from its first
  // request.
  void abort();

  const SourceState& state(uint8_t slot, schedule::Source source) const {
    return states_[slot][static_cast<std::size_t>(source)];
  }

 private:
  void addPair(const settings::FixturePair& pair);
  bool paired(uint8_t competition) const;
  // Fixtures/live/table outcome of the prepared request for the route.
  void settle(SourceState& state, bool ok);
  Step advance(bool ok, const Clock& clock);
  bool consumeElement(ArduinoJson::JsonVariantConst element);
  // Starts the prepared request of the current run (current_, source_,
  // type_, map_ set): a new run's preparation, the URL, the streaming
  // state. False when the run has nothing (more) to request.
  bool begin(MatchStore& store, const Clock& clock, Request& out);
  // The request at the current position of the run, moving past steps that
  // make none; 'out' null only checks. False at the end of the run.
  bool build(Request* out);
  void liveItems(const MatchStore& store, int64_t utc);
  // The stored row of the events run's match (event_match_), null if gone.
  const StoredMatch* eventsRow() const;
  void addDay(uint32_t day);

  std::array<std::array<SourceState, schedule::kSourceCount>,
             cfg::kCompetitionMax>
      states_{};
  SourceState pick_state_{};  // the fixture picker's run (RAM only)
  bool picking_ = false;      // the prepared request is the picker's
  // The prepared request: its source, data type and consume context.
  SourceState* current_ = nullptr;
  MatchStore* store_ = nullptr;
  settings::Source source_;
  schedule::Source kind_ = schedule::Source::kPrimary;
  bool fallback_on_ = false;        // the entry's fallback is active
  cfg::Provider fallback_provider_ = cfg::Provider::kOpenLigaDb;
  cfg::DataType type_ = cfg::DataType::kCore;
  MapContext map_;
  Sink sink_ = Sink::kMatches;
  uint32_t event_match_ = 0;  // the own match of an events run
  bool discover_ = false;  // OpenLigaDB rows: note season and round
  bool filter_ = false;    // OpenLigaDB rows: only this league and season
  int season_number_ = 0;  // filter season, 0 = any
  int found_round_ = 0;
  uint32_t found_change_ = 0;
  settings::Bytes<cfg::kSeasonMaxBytes> found_season_;
  // ESPN match days while streaming: the days from the first matchday that
  // has not ended, up to the end of the one after it.
  std::array<uint32_t, 2 * kEspnDays> scan_{};
  uint8_t scan_count_ = 0, scan_blocks_ = 0;
  bool scan_done_ = false;
  // The fixture pairs of all competitions (setPairs); primary provider and
  // route are set per fallback request.
  Pairing pairing_;
};

// One URL path or query component: every byte outside the RFC 3986
// unreserved set is percent-encoded (OpenLigaDB shortcuts may contain
// spaces or umlauts). 'out' holds 'size' bytes including the terminator;
// input that does not fit is cut at a whole escape.
void encode(std::string_view in, char* out, std::size_t size);

// Days of the current and the next matchday from ascending provider day
// starts: days less than kMatchdayGapHours apart form one matchday; the
// current one is the first whose last day has not ended at 'utc'. At most
// 'capacity' days. Returns the number written.
std::size_t matchdayDays(const uint32_t* days, std::size_t count,
                         int64_t utc, uint32_t* out, std::size_t capacity);

// FNV-1a hash of a change date text (0 is never returned).
uint32_t changeHash(std::string_view text);

}  // namespace football::plan
