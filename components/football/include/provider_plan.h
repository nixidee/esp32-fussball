// Provider request plans (pure logic): which provider requests one run of a
// scheduled job makes, their URLs, and where each streamed element goes in
// the match store. A run can take several requests (OpenLigaDB: current and
// next round plus the club's fixtures; ESPN: match days, then one scoreboard
// per day); the scheduler keeps such a job due between them
// (Scheduler::finished(..., more)). No I/O and no ESP-IDF, so the native
// tests drive it with recorded responses. Not thread-safe: the provider
// worker owns it.
//
// Per provider and data type (OQ-216, OQ-218; no core data, OQ-248):
//   OpenLigaDB  fixtures   getmatchdata/{league} (current round, also the
//                          current season), getmatchdata/…/{round + 1},
//                          getmatchesbyteamid/{club}/… (club only)
//               table      getbltable/{league}/{season} (also live table)
//               live       per round with a match in its live window:
//                          getlastchangedate/…/{round}, then
//                          getmatchdata/…/{round} when the date changed or
//                          the last full request is kOpenLigaDbFullRefreshS
//                          old; goals come with the match data
//   ESPN        fixtures   calendar/ondays (match days), then the scoreboard
//                          of every day of the current and the next matchday
//               table      site.web.api standings (also live table; cups
//                          have none, an absent list is an empty table)
//               live       the scoreboard of each match day with a match in
//                          its live window; goals and cards come with it
//               events     the exact policy-eligible Single's match summary
//                          (substitutions), for own and ordinary matches;
//                          preview requests wait for kickoff; body cap
//                          kProviderSummaryBodyBytes
//   API-Football, football-data.org: generic test sources (OQ-205):
//   fixtures within kFixtureHorizonDays, standings, today's fixtures.
//   API-Football first resolves the current season (/leagues) when it is
//   unknown; a fixtures run kSeasonRecheckS after the last resolution
//   resolves it again (OQ-259).
// Other providers make no separate events request: OpenLigaDB delivers
// goals with the live data, API-Football events are removed (OQ-206).
//
// Every active competition retains fixtures/live as phase-classification
// prerequisites. Current-phase enabled Table and Single entries add table/live
// table and exact ESPN summary demand respectively; manual-only is enabled too.
// Picture-frame mode pauses automatic demand. Each competition has one source.

#pragma once

#include <ArduinoJson.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "app_config.h"
#include "match_store.h"
#include "request_scheduler.h"
#include "screen_selection.h"
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

// How a prepared request ended in the provider worker.
enum class Fetch : uint8_t {
  kOk,           // sent, the response was complete
  kFailed,       // sent, no complete response
  kRateLimited,  // sent, answered "too many requests"
  kInterrupted,  // settings or the network changed (sent or not)
  kBusy,         // not sent: another operation held the network
  kCapped,       // not sent: a daily cap is used up
  kRefused,      // not sent: the provider cannot be used (switched off, no
                 // key) or its request counter could not be stored
};

// The scheduler's view of an ended request (Planner::complete()).
struct Completion {
  schedule::Outcome outcome = schedule::Outcome::kOk;
  bool more = false;     // the run continues with another request
  bool invalid = false;  // the response lacked the season the run needs
};

// What one competition's source keeps between requests (RAM only).
struct SourceState {
  uint32_t key = 0;  // hash of the competition id and its source settings
  settings::Bytes<cfg::kSeasonMaxBytes> season;  // resolved current season
  uint32_t season_utc = 0;  // UTC second of its resolution, 0 = unknown
  uint8_t round = 0;        // OpenLigaDB current round, 0 = unknown
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
};

// What a source resolved that a competition file keeps (football_files.h):
// its current season (with the UTC second of its resolution), OpenLigaDB
// round and ESPN match days.
struct Resolved {
  settings::Bytes<cfg::kSeasonMaxBytes> season;
  uint32_t season_utc = 0;
  uint8_t round = 0;
  std::array<uint32_t, kEspnDays> days{};
  uint8_t day_count = 0;
};

// One complete response retains the closest candidate in each selection class.
// The final selector alone commits it; the protected own bank is unchanged.
struct ResponseDetails {
  struct Candidate {
    selection::MatchKey match;
    uint32_t kickoff = 0;
    Text<cfg::kNameBytes> home, away;
    uint8_t count = 0;
    bool truncated = false;
  };
  // Only the event payload is transient heap memory. Candidate identity, names
  // and duplicate detection belong to the existing worker-owned planner, so a
  // simultaneous bounded settings request stays inside the shared heap limit.
  struct EventBuffer {
    std::array<std::array<StoredEvent, cfg::kMatchEvents>, 2> candidates{};
  };
  std::array<Candidate, 2> candidates{};
  std::array<uint32_t, cfg::kStoreMatches> seen{};
  uint16_t seen_count = 0;
  int8_t receiving = -1;
  bool ambiguous = false;
  std::unique_ptr<EventBuffer> events;
  // Reset only live metadata; do not create a full metadata stack temporary.
  void clear();
};
static_assert(sizeof(ResponseDetails::EventBuffer) <= 2016);
static_assert(sizeof(ResponseDetails) <= 1216);

class Planner {
 public:
  // Borrowed coherent worker snapshot; never a full retained policy copy.
  void context(const settings::Competitions& competitions,
               const settings::screen_policy::Eligibility& eligibility,
               DateContext dates = {}, bool break_proven = false);
  void reset();
  // The source state of slot 'slot' starts afresh (its data was dropped).
  void forget(uint8_t slot);
  // The resolved part of a source state; empty when the state does not
  // belong to the current source of 'entry'.
  Resolved saved(uint8_t slot, const settings::Competition& entry) const;
  // Restores a saved source state for the current source of 'entry' (as
  // after its requests); a run in progress starts over.
  void restore(uint8_t slot, const settings::Competition& entry,
               const Resolved& resolved);
  // Scheduler inputs of slot 'slot' (entry null, inactive or without
  // competition: unused) at 'clock': exact Single summaries are held until
  // their match has started; an API-Football fixtures run whose season
  // is due for resolution costs its /leagues request too (OQ-264).
  void wanted(uint8_t slot, const settings::Competition* entry,
              const MatchStore& store, const Clock& clock,
              schedule::Wanted& out,
              const selection::Selection* selected = nullptr) const;
  // The next request of 'job' for competition 'entry' (its slot and id);
  // prepares the streaming into 'store' (a table request begins the
  // staging). False when the run has nothing (more) to request: the run is
  // complete without a request.
  bool request(const schedule::Job& job, const settings::Competition& entry,
               MatchStore& store, const Clock& clock, Request& out);
  // Shared source/season filter for match rows before any external observer.
  // Club fixture endpoints may legitimately contain other competitions.
  bool acceptsMatch(ArduinoJson::JsonVariantConst element) const;
  // fetchJson consumer for the prepared request; 'planner' is this.
  static bool consume(ArduinoJson::JsonVariantConst element, void* planner);
  // Ends the prepared request; 'ok' = the response was complete. A failure
  // ends the run (the next run starts from its first request).
  Step finish(bool ok, const Clock& clock);
  // Ends the prepared request and its run without an outcome (settings or
  // network changed, the request was not sent): a staged table is dropped,
  // no source state changes; the next run starts from its first request.
  void abort();
  // Ends the prepared request without an outcome but keeps the run's
  // position: the next request() asks for the same request again.
  void release();
  // Ends the prepared request as 'fetch' says (finish(), abort() or
  // release()) and tells the scheduler how its turn ended: a sent request
  // counts as kOk, kFailed or kRateLimited; a local refusal (busy, changed,
  // daily cap) defers it without a failure; a provider that cannot be used
  // fails.
  Completion complete(Fetch fetch, const Clock& clock);

  const SourceState& state(uint8_t slot) const { return states_[slot]; }

 private:
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
  static void observeNames(const StoredMatch&, std::string_view,
                           std::string_view, void*);
  static void observeEvent(const StoredEvent&, void*);
  void finishDetails(bool ok, const Clock& clock);
  const settings::Competitions* competitions_ = nullptr;
  const settings::screen_policy::Eligibility* eligibility_ = nullptr;
  DateContext dates_;
  bool break_proven_ = false;
  ResponseDetails response_details_;

  std::array<SourceState, cfg::kCompetitionMax> states_{};
  // The prepared request: its source, data type and consume context.
  SourceState* current_ = nullptr;
  MatchStore* store_ = nullptr;
  settings::Source source_;
  cfg::DataType type_ = cfg::DataType::kFixtures;
  MapContext map_;
  Sink sink_ = Sink::kMatches;
  uint32_t event_match_ = 0;  // exact selected match of an events run
  bool discover_ = false;     // OpenLigaDB rows: note season and round
  bool filter_ = false;       // OpenLigaDB rows: only this league and season
  int season_number_ = 0;     // filter season, 0 = any
  int found_round_ = 0;
  uint32_t found_change_ = 0;
  settings::Bytes<cfg::kSeasonMaxBytes> found_season_;
  // ESPN match days while streaming: the days from the first matchday that
  // has not ended, up to the end of the one after it.
  std::array<uint32_t, 2 * kEspnDays> scan_{};
  uint8_t scan_count_ = 0, scan_blocks_ = 0;
  bool scan_done_ = false;
};

// One URL path or query component: every byte outside the RFC 3986
// unreserved set is percent-encoded (OpenLigaDB shortcuts may contain
// spaces or umlauts). 'out' holds 'size' bytes including the terminator;
// input that does not fit is cut at a whole escape.
void encode(std::string_view in, char* out, std::size_t size);

// Days of the current and the next matchday from ascending provider day
// starts, grouped by matchday::starts(); the current one is the first whose
// last provider day has not ended at 'utc' (results of a held matchday are
// final and stay stored, so it is not fetched again, OQ-272). At most
// 'capacity' days. Returns the number written.
std::size_t matchdayDays(const uint32_t* days, std::size_t count, int64_t utc,
                         uint32_t* out, std::size_t capacity);

// FNV-1a hash of a change date text (0 is never returned).
uint32_t changeHash(std::string_view text);

}  // namespace football::plan
