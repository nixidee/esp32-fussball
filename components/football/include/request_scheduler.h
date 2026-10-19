// Request scheduler (pure logic): decides which provider request runs next
// and when to ask again, so the provider worker never waits in a loop. A
// shared job is one data type of one exact resolved source; metadata resolution
// and exact Single summary demand also have incarnation-aware view scopes. Its
// due time follows the provider's interval for the data
// type (OQ-216); live data types run only while a match of the competition
// is in its live window, from kLiveLeadMinutes before kickoff (OQ-220) to
// the expected end of kExpectedMatchMinutes (OQ-213) or the reported end.
// Limits: per provider and globally a daily cap and a window limit of N
// requests in any W seconds (OQ-225, OQ-226), plus the small device gap
// kRequestGapMs (OQ-212). When a daily cap cannot cover the expected
// requests of the rest of the UTC day, data types are stretched or skipped
// in the order live table, table, fixtures, live; the globally focused special
// own match is served last. The live table and the exact Single's summary get
// one more request after their window ends (final table, last events),
// unless the window ended longer ago than their interval. Not thread-safe: the
// provider worker owns it.

#pragma once

#include <array>
#include <bitset>
#include <cstdint>

#include "app_config.h"
#include "match_store.h"
#include "settings_model.h"

namespace football::schedule {

// A window limit of 'requests' in any 'window_s' seconds (both > 0, else
// no limit). Counted in cfg::kLimitWindowBuckets time buckets: a bucket
// counts for the whole window while any part of it lies inside, so the
// limit is never exceeded; the price is up to one bucket width
// (window / (buckets − 1)) of unused allowance. Monotonic seconds.
class Window {
 public:
  // Changing the limit forgets the counted requests.
  void set(uint16_t requests, uint16_t window_s);
  bool limited() const { return requests_ > 0 && window_s_ > 0; }
  // The earliest second >= 'now_s' at which one more request is allowed.
  uint32_t admissibleAt(uint32_t now_s) const;
  // A request started at 'now_s' (never earlier than a previous one).
  void record(uint32_t now_s);

 private:
  uint32_t counted(uint32_t first, uint32_t last) const;

  std::array<uint16_t, cfg::kLimitWindowBuckets> counts_{};
  uint32_t newest_ = 0;  // bucket number of the newest request
  uint16_t requests_ = 0, window_s_ = 0, width_ = 1;
};

// Identity of shared source demand or incarnation-aware configured-view demand.
struct ScopeKey {
  SourceId source;  // shared data jobs, independent of their current requester
  uint32_t incarnation = 0;
  uint8_t competition = 0;  // per-view resolution/summary jobs
  bool valid() const { return bool(source) || competition != 0; }
  bool operator==(const ScopeKey&) const = default;
};
// What one scope wants this pass: the transport route, provider and data-type
// bits. Invalid key = unused slot; changed identity starts fresh jobs.
// 'cost' is the number of requests one run of a job is expected
// to make (0 counts as 1); the daily-cap degradation multiplies its demand
// by it. 'held': data types (bits as in 'types') that count in the daily
// demand but do not run now, because their request is not possible yet
// (a Single summary before kickoff).
struct Wanted {
  ScopeKey key;
  uint8_t competition = 0, route = 0;
  cfg::Provider provider{};
  uint8_t types = 0;
  uint8_t held = 0;
  std::array<uint8_t, cfg::kDataTypeCount> cost{};
  MatchKey event_match;  // exact eligible Single, independent of ownership
  uint32_t priority_match = 0;  // globally focused special own fixture only
  bool event_priority = false;
};

// Unlimited remaining daily requests (no daily cap).
inline constexpr uint32_t kUnlimited = UINT32_MAX;

struct Inputs {
  std::array<Wanted, cfg::kRequestScopes> scopes{};
  const settings::ProviderPlans* plans = nullptr;
  settings::RequestLimit global;
  // Requests left today per provider and over all providers (the daily
  // ledgers); kUnlimited without a cap.
  std::array<uint32_t, cfg::kProviderCount> daily_left{};
  uint32_t global_daily_left = kUnlimited;
  const MatchStore* store = nullptr;
  Clock clock;  // UTC (live windows, day end) and monotonic seconds
};

struct Job {
  ScopeKey key;
  uint8_t slot = 0, competition = 0, route = 0;
  cfg::DataType type = cfg::DataType::kFixtures;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  MatchKey event_match;
  SourceId source() const {
    return key.source ? key.source : event_match.source;
  }
  bool event_priority = false;
};

// Why nothing runs now.
enum class Wait : uint8_t {
  kIdle,    // nothing due before 'wake_s'
  kGap,     // device gap after the previous request
  kWindow,  // a window limit (provider or global)
  kDaily,   // a daily cap is used up until the end of the UTC day
  kPaused,  // the provider answered "too many requests"
  kBusy,    // the device was busy (another operation held the network)
};

struct Decision {
  bool run = false;
  Job job;  // valid when 'run'
  Wait wait = Wait::kIdle;
  uint32_t wake_s = 0;  // monotonic second to ask again; 0 = nothing planned
};

// Degradation classes in the order they give way (OQ-221).
enum class Class : uint8_t {
  kLiveTable,
  kTable,
  kFixtures,
  kLive,
  kOwn,  // live data/summary of the globally focused special own fixture
};
inline constexpr std::size_t kClassCount = 5;
// Interval factor of a class: 1 normal, n stretched n times, 0 skipped.
using Factors = std::array<uint8_t, kClassCount>;

// How a job's turn ended (finished()).
enum class Outcome : uint8_t {
  kOk,           // its request was sent and the response was complete
  kFailed,       // its request was sent and failed
  kRateLimited,  // its request was sent and answered "too many requests"
  kNothing,      // its run had nothing to request: complete without one
  kDeferred,     // not sent or not completed for a local reason (device
                 // busy, settings or network changed, daily cap): no
                 // failure, the job stays due
};

class Scheduler {
 public:
  Scheduler();
  void reset();
  // The job to run now, or when to ask again. Pure apart from configuring
  // the window limits from 'inputs.plans' and 'inputs.global'.
  Decision next(const Inputs& inputs);
  // A request of 'provider' starts at monotonic second 'now_s' (a job's
  // or one outside the jobs, the owner's selection lists): the device gap
  // and the window limits count it. Called right before the HTTP request,
  // never for a request that is not sent.
  void sent(cfg::Provider provider, uint32_t now_s);
  // How the turn of 'job' ended; 'at_s' is the start of its request for a
  // sent outcome (kOk, kFailed, kRateLimited), else the current second. A
  // sent outcome and kNothing make the job's interval count from 'at_s';
  // the table and the live table share their request, so either counts for
  // both. A failure retries after kProviderErrorRetryS, doubling up to the
  // larger of the job's interval and kProviderErrorRetryMaxS (OQ-262);
  // "too many requests" also pauses the provider for kProviderRateRetryS.
  // kDeferred changes no job and lets every provider wait for the device
  // gap after 'at_s' (Wait::kBusy). 'more' after a success: the run of the
  // job needs another request, so the job stays due (its class and the
  // limits still apply, a daily-cap skip does not) until a request ends
  // without 'more'.
  void finished(const Job& job, Outcome outcome, uint32_t at_s,
                bool more = false);

  // Owner refresh: every job is due now. Counted requests, window limits,
  // pauses and the device gap stay.
  void refresh();
  // The jobs of slot 'slot' start fresh (its competition's source
  // changed): as for a slot whose id changes.
  void forget(uint8_t slot);
  void restartFixtures(uint16_t sources);
  // Window limits from the provider plans and the global limit (next()
  // applies them too; a changed limit forgets its counted requests).
  void configure(const settings::ProviderPlans& plans,
                 const settings::RequestLimit& global);
  // A request outside the jobs (the owner's selection lists): the earliest
  // second >= 'now_s' at which 'provider' may send (device gap, deferral,
  // pause, window limits; the daily caps are the ledgers'), and in 'why'
  // (optional) the limit that holds it longest.
  uint32_t admissibleAt(cfg::Provider provider, uint32_t now_s,
                        Wait* why = nullptr) const;

  // Factors applied in the last next() over all scopes, and whether a
  // daily cap was stretching or skipping anything (Web UI hint).
  const Factors& factors() const { return factors_; }
  bool degraded() const;

 private:
  static constexpr uint32_t kNever = UINT32_MAX;
  static constexpr int32_t kNotStarted = INT32_MAX;
  std::size_t index(uint8_t slot, cfg::DataType type) const;
  void clear(uint8_t slot);

  // Per job: monotonic second of the last start (negative: before boot, a
  // seeded start; kNotStarted = none), failures since the last success.
  // Table and live table keep both equal (one request).
  std::array<int32_t, cfg::kRequestScopes * cfg::kDataTypeCount> last_{};
  std::array<uint8_t, cfg::kRequestScopes * cfg::kDataTypeCount> failures_{};
  // Jobs whose run continues with another request.
  std::bitset<cfg::kRequestScopes * cfg::kDataTypeCount> pending_;
  std::array<ScopeKey, cfg::kRequestScopes> keys_{};
  std::array<MatchKey, cfg::kRequestScopes> event_ids_{};
  std::array<Window, cfg::kProviderCount> windows_{};
  Window global_window_;
  std::array<uint32_t, cfg::kProviderCount> paused_until_{};
  // Monotonic second of the last request start, and until when a deferred
  // turn holds every provider back.
  uint32_t last_start_ = kNever, deferred_until_ = 0;
  Factors factors_{};
  // Rebuilt for each activity assessment; indices only, never cached facts.
  // Owner-approved 1024B scratch keeps canonical/display order untouched.
  std::array<MatchIndex, cfg::kStoreMatches> activity_order_;
};

// Factors for one daily cap: 'left' requests for the expected 'demand' per
// class until the end of the day. Classes give way in Class order: a class
// is skipped while the others alone need the whole allowance, otherwise
// stretched just enough (0 when it would need more than 255 times).
Factors degrade(const std::array<uint32_t, kClassCount>& demand, uint32_t left);

// Live window of a stored match in UTC seconds: from kLiveLeadMinutes
// before kickoff to the final whistle when finished, to kickoff +
// kUnknownMatchEndMinutes while the provider reports it running or a previous
// live observation justifies its suspension, else to kickoff +
// kExpectedMatchMinutes. Without kickoff, confirmed live/suspended matches
// begin at first_live_observed_utc with the same hard bound; a finished match
// has a zero-length final-whistle boundary for the final request. No kickoff
// is invented. False for an unknown time basis, unjustified suspension,
// finished without final-whistle time, postponed, cancelled or abandoned.
bool liveWindow(const StoredMatch& match, uint32_t& start, uint32_t& end);

// Freshness of one shown source/data type. Actual failures and incomplete
// acquisitions remain local to that data type. Fixtures also age while one
// of this source's matches is in its existing live window. UTC 0 disables
// only the live-window age check; monotonic timestamps remain 64-bit.
bool datasetStale(const MatchStore& store, uint8_t competition,
                  MatchStore::Dataset dataset, int64_t utc, int64_t now_ms,
                  cfg::Provider provider);

}  // namespace football::schedule
