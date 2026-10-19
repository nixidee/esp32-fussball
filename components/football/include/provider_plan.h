// Provider request plans (pure logic): which provider requests one run of a
// scheduled job makes, their URLs, and where each streamed element goes in
// the match store. A fixture run selects from a compact index before admitting
// full rows; the scheduler keeps the job due between its requests
// (Scheduler::finished(..., more)). No I/O and no ESP-IDF, so the native
// tests drive it with recorded responses. Not thread-safe: the provider
// worker owns it.
//
// Per provider and data type (OQ-216, OQ-218; no core data, OQ-248):
//   OpenLigaDB  fixtures   exact-season index, then selected fixture admission
//               table      getbltable/{league}/{season} (also live table)
//               live       per round with a match in its live window:
//                          getlastchangedate/…/{round}, then
//                          getmatchdata/…/{round} when the date changed or
//                          the last full request is kOpenLigaDbFullRefreshS
//                          old; goals come with the match data
//   ESPN        fixtures   exact-season calendar, selected whole-day
//   scoreboards,
//                          then past/future own index and admission per
//                          distinct club; one previous and three next own
//                          fixtures total
//               table      site.web.api standings (also live table; cups
//                          have none, an absent list is an empty table)
//               live       the scoreboard of each match day with a match in
//                          its live window; goals and cards come with it
//               events     the exact policy-eligible Single's match summary
//                          (substitutions), for own and ordinary matches;
//                          preview requests wait for kickoff; body cap
//                          kProviderSummaryBodyBytes
//   API-Football, football-data.org: exact-season fixture index and admission,
//   standings and live fixtures. Automatic season resolution is a separate
//   metadata-only job before any data admission, repeated after
//   kSeasonRecheckS.
// Other providers make no separate events request: OpenLigaDB delivers
// goals with the live data, API-Football events are removed (OQ-206).
//
// Every active competition retains fixtures/live as phase-classification
// prerequisites. Current-phase enabled Table and Single entries add table/live
// table and exact ESPN summary demand respectively; manual-only is enabled too.
// Picture-frame mode pauses automatic demand. Equal resolved source keys share
// facts and jobs; configured views retain independent club and summary demand.

#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

#include "app_config.h"
#include "fixture_retention.h"
#include "match_store.h"
#include "provider_adapter.h"
#include "request_scheduler.h"
#include "screen_selection.h"
#include "settings_model.h"
#include "store_mappers.h"

namespace football::plan {

// ESPN match days kept per source: the current and the next matchday.
inline constexpr std::size_t kEspnDays = cfg::kFixtureCalendarDays;
// Rounds or match days one live run checks.
inline constexpr std::size_t kLiveItems = cfg::kProviderLiveItems;

// Where the elements of a response go.
enum class Sink : uint8_t {
  kMatches,   // storeMatch(); OpenLigaDB rows also give season and round
  kTable,     // storeTableRow() between beginTable() and commitTable()
  kChange,    // OpenLigaDB change date (one JSON string)
  kDays,      // ESPN match days (calendar/ondays "dates")
  kSeason,    // API-Football current season (/leagues "response")
  kEvents,    // storeSummaryEvent() between beginEvents() and endEvents()
  kIndex,     // compact complete-season index; no fixture admission
  kOwnIndex,  // explicit own schedule; no competition-round completeness
  kOwnMatches,
};

struct Request {
  provider::RequestSpec spec;
  Sink sink = Sink::kMatches;
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
  // Refreshing retained identities proves no new fixture/round completeness.
  bool preserve_scope = false;
};

// What one exact resolved source keeps between requests (RAM only).
struct SourceState {
  // Reconstruct directly in owned storage; aggregate assignment creates a
  // large source-array temporary on the provider task's limited stack.
  void clear();
  SourceId source;
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
};

// One complete response retains the closest candidate in each selection class.
// The final selector alone commits it; the protected own bank is unchanged.
struct ResponseDetails {
  struct Candidate {
    selection::MatchKey match;
    uint32_t kickoff = 0;
    uint8_t count = 0;
    bool truncated = false;
  };
  // Only the event payload is transient heap memory. Candidate identity
  // and duplicate detection belong to the existing worker-owned planner, so a
  // simultaneous bounded settings request stays inside the shared heap limit.
  struct EventBuffer {
    std::array<std::array<EventFacts, cfg::kStoreOtherEvents>, 2> candidates{};
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
static_assert(sizeof(ResponseDetails::EventBuffer) <= 7680);
static_assert(sizeof(ResponseDetails) <= 2368);
static_assert(cfg::kRetainedOwnFixtures <=
              std::numeric_limits<uint8_t>::digits);

class Planner {
 public:
  // Borrowed coherent worker snapshot; never a full retained policy copy.
  void context(const settings::CompetitionRoutes& competitions,
               const settings::screen_policy::Eligibility& eligibility,
               DateContext dates = {}, bool break_proven = false);
  void reset();
  bool synchronize(MatchStore&, const Clock&);
  void restartFixtures(uint16_t sources);
  bool prepareDemand(MatchStore&, const Clock&, schedule::Inputs&,
                     const selection::Selection* selected = nullptr) const;
  // The internal source slot starts afresh (its data was dropped).
  void forget(uint8_t slot);
  // Per-view resolution and summary inputs (null/inactive entry: unused).
  // Exact Single summaries are held until kickoff. prepareDemand combines
  // these with shared source demand; resolution never admits match facts.
  void wanted(uint8_t slot, const settings::CompetitionRoute* entry,
              const MatchStore& store, const Clock& clock,
              schedule::Wanted& out,
              const selection::Selection* selected = nullptr) const;
  // The next request of 'job' for competition 'entry' (its slot and id);
  // prepares the streaming into 'store' (a table request begins the
  // staging). False when the run has nothing (more) to request: the run is
  // complete without a request.
  bool request(const schedule::Job& job,
               const settings::CompetitionRoute& entry, MatchStore& store,
               const Clock& clock, Request& out);
  // Typed response callbacks; the worker serialises each mutation.
  bool acceptsMatch(const provider::Fixture&) const;
  bool consume(const provider::Fixture&, std::span<const provider::Event>);
  bool consume(const provider::TableRow&);
  bool consume(const provider::TableGroup&);
  bool consume(const provider::Event&);
  bool consumeDay(uint32_t);
  bool consumeCalendarRound(const provider::CalendarRound&);
  bool consumeText(std::string_view);
  bool response(const provider::ParseResult&);
  provider::Status responseFailure() const { return response_failure_; }
  Cause responseCause() const { return response_cause_; }
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
  static void observeMatch(const StoredMatch&, void*);
  static void observeEvent(const EventFacts&, void*);
  void finishDetails(bool ok, const Clock& clock);
  // A complete-season index covers every configured club of this same
  // request source. Explicit team schedules keep their one-team boundary.
  retention::Index* makeRetentionIndex(bool calendar = false) const;
  bool resolveResponse(const Clock&);
  uint8_t nextClub(uint8_t first) const;
  uint32_t currentClub() const;
  void sourceWanted(SourceSlot, const MatchStore&, const Clock&,
                    schedule::Wanted&, const selection::Selection*) const;
  const settings::CompetitionRoutes* competitions_ = nullptr;
  const settings::screen_policy::Eligibility* eligibility_ = nullptr;
  DateContext dates_;
  bool break_proven_ = false;
  ResponseDetails response_details_;
  std::unique_ptr<retention::Index> retention_index_;
  std::array<uint16_t, cfg::kRetainedRounds> retained_rows_{};
  provider::Status response_failure_ = provider::Status::kOk;
  Cause response_cause_ = Cause::kNone;
  bool retain_admission_ = false;
  bool known_only_ = false;
  bool response_sources_ = true;
  std::bitset<cfg::kRetainedClubFixtures> own_seen_;
  std::bitset<cfg::kRetainedClubFixtures> own_expected_;

  struct FixtureRun {
    retention::Selection selection;
    SourceId source;
    retention::OwnFixtures own_candidates;
    std::bitset<cfg::kRetainedClubFixtures> past_expected, future_expected;
    uint8_t club_route = 0, future_seen = 0;
    void clear() {
      selection.clear();
      source = {};
      club_route = 0;
      own_candidates = {};
      past_expected.reset();
      future_expected.reset();
      future_seen = 0;
    }
  } fixtures_;
  std::array<SourceState, cfg::kStoreSources> states_{};
  SourceState resolving_;
  // The prepared request: its source, data type and consume context.
  SourceState* current_ = nullptr;
  MatchStore* store_ = nullptr;
  settings::Source source_;
  const settings::CompetitionRoute* entry_ = nullptr;
  bool resolving = false;
  cfg::DataType type_ = cfg::DataType::kFixtures;
  MapContext map_;
  Sink sink_ = Sink::kMatches;
  uint32_t event_match_ = 0;  // exact selected match of an events run
  uint16_t response_rows_ = 0;
  uint8_t response_round_ = 0, response_label_ = kNoIndex;
  bool response_round_known_ = true, response_whole_ = false;
  bool filter_ = false;  // admit only rows matching this exact source
  int found_round_ = 0;
  uint32_t found_change_ = 0;
  settings::Bytes<cfg::kSeasonMaxBytes> found_season_;
};

// Days of the previous/current/next derived matchday from ascending provider
// day markers under the shared matchday rule. At a season boundary only
// existing neighbours remain. At most capacity days; overflow is explicit.
std::size_t matchdayDays(const uint32_t* days, std::size_t count, int64_t utc,
                         uint32_t* out, std::size_t capacity,
                         bool* complete = nullptr);

// FNV-1a hash of a change date text (0 is never returned).
uint32_t changeHash(std::string_view text);

}  // namespace football::plan
