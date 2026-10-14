// Request scheduler (pure logic): decides which provider request runs next
// and when to ask again, so the provider worker never waits in a loop. A
// job is one data type of one source (primary or fallback) of one
// competition. Its due time follows the provider's interval for the data
// type (OQ-216); live data types run only while a match of the competition
// is in its live window, from kLiveLeadMinutes before kickoff (OQ-220) to
// the expected end of kExpectedMatchMinutes (OQ-213) or the reported end.
// Limits: per provider and globally a daily cap and a window limit of N
// requests in any W seconds (OQ-225, OQ-226), plus the small device gap
// kRequestGapMs (OQ-212). When a daily cap cannot cover the expected
// requests of the rest of the UTC day, data types are stretched or skipped
// in the order live table, table, core data, fixtures, live; the own match
// is served last (OQ-221). The live table and the own-match events get one
// more request after a window ends (final table, last events), unless the
// window ended longer ago than their interval. Not thread-safe: the
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

enum class Source : uint8_t { kPrimary, kFallback };
inline constexpr std::size_t kSourceCount = 2;

// What one competition slot wants requested this pass (built by the
// provider plan from the settings): the provider and one bit per
// cfg::DataType for each source. id 0 = unused slot. A slot whose id
// changes starts with fresh jobs. 'cost' is the number of requests one run
// of a job is expected to make (0 counts as 1); the daily-cap degradation
// multiplies its demand by it.
struct Wanted {
  uint8_t id = 0;
  std::array<cfg::Provider, kSourceCount> provider{};
  std::array<uint8_t, kSourceCount> types{};
  std::array<std::array<uint8_t, cfg::kDataTypeCount>, kSourceCount> cost{};
};

// Unlimited remaining daily requests (no daily cap).
inline constexpr uint32_t kUnlimited = UINT32_MAX;

struct Inputs {
  std::array<Wanted, cfg::kCompetitionMax> competitions{};
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
  uint8_t slot = 0, competition = 0;  // slot index and competition id
  Source source = Source::kPrimary;
  cfg::DataType type = cfg::DataType::kCore;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
};

// Why nothing runs now.
enum class Wait : uint8_t {
  kIdle,      // nothing due before 'wake_s'
  kGap,       // device gap after the previous request
  kWindow,    // a window limit (provider or global)
  kDaily,     // a daily cap is used up until the end of the UTC day
  kPaused,    // the provider answered "too many requests"
};

struct Decision {
  bool run = false;
  Job job;              // valid when 'run'
  Wait wait = Wait::kIdle;
  uint32_t wake_s = 0;  // monotonic second to ask again; 0 = nothing planned
};

// Degradation classes in the order they give way (OQ-221).
enum class Class : uint8_t {
  kLiveTable,
  kTable,
  kCore,
  kFixtures,
  kLive,
  kOwn,  // live data of a competition whose own match is in its window,
         // and the events of an own match
};
inline constexpr std::size_t kClassCount = 6;
// Interval factor of a class: 1 normal, n stretched n times, 0 skipped.
using Factors = std::array<uint8_t, kClassCount>;

enum class Outcome : uint8_t { kOk, kFailed, kRateLimited };

class Scheduler {
 public:
  Scheduler();
  void reset();
  // The job to run now, or when to ask again. Pure apart from configuring
  // the window limits from 'inputs.plans' and 'inputs.global'.
  Decision next(const Inputs& inputs);
  // The request of 'job' was sent at monotonic second 'now_s'. The table
  // and the live table of one source share their request, so a start of
  // either counts for both.
  void started(const Job& job, uint32_t now_s);
  // Its result. A failure retries after kProviderErrorRetryS, doubling up
  // to the job's interval; "too many requests" also pauses the provider for
  // kProviderRateRetryS. 'more' after a success: the run of the job needs
  // another request, so the job stays due (its class and the limits still
  // apply, a daily-cap skip does not) until a request ends without 'more'.
  void finished(const Job& job, Outcome outcome, uint32_t now_s,
                bool more = false);

  // Owner refresh: every job is due now. Counted requests, window limits,
  // pauses and the device gap stay.
  void refresh();
  // The jobs of slot 'slot' start fresh (its competition's sources
  // changed): as for a slot whose id changes.
  void forget(uint8_t slot);
  // Data restored from a competition file: the non-live job 'type' of
  // 'source' in slot 'slot' (competition 'id') counts as started 'age_s'
  // seconds before monotonic second 'now_s', also before boot. A slot with
  // another id starts fresh first. Live data types are not seeded.
  void seed(uint8_t slot, uint8_t id, Source source, cfg::DataType type,
            uint32_t age_s, uint32_t now_s);
  // Window limits from the provider plans and the global limit (next()
  // applies them too; a changed limit forgets its counted requests).
  void configure(const settings::ProviderPlans& plans,
                 const settings::RequestLimit& global);
  // A request outside the jobs (the owner's selection lists and the
  // fixture picker): the earliest second >= 'now_s' at which 'provider' may
  // send (device gap, pause, window limits; the daily caps are the
  // ledgers'), and its start, counted like a job's.
  uint32_t admissibleAt(cfg::Provider provider, uint32_t now_s) const;
  void record(cfg::Provider provider, uint32_t now_s);

  // Factors applied in the last next() over all scopes, and whether a
  // daily cap was stretching or skipping anything (Web UI hint).
  const Factors& factors() const { return factors_; }
  bool degraded() const;

 private:
  static constexpr uint32_t kNever = UINT32_MAX;
  static constexpr int32_t kNotStarted = INT32_MAX;
  std::size_t index(uint8_t slot, Source source, cfg::DataType type) const;
  void clear(uint8_t slot);

  // Per job: monotonic second of the last start (negative: before boot, a
  // seeded start; kNotStarted = none), failures since the last success.
  std::array<int32_t, cfg::kCompetitionMax * kSourceCount *
                          cfg::kDataTypeCount> last_{};
  std::array<uint8_t, cfg::kCompetitionMax * kSourceCount *
                          cfg::kDataTypeCount> failures_{};
  // Jobs whose run continues with another request.
  std::bitset<cfg::kCompetitionMax * kSourceCount * cfg::kDataTypeCount>
      pending_;
  std::array<uint8_t, cfg::kCompetitionMax> ids_{};
  std::array<Window, cfg::kProviderCount> windows_{};
  Window global_window_;
  std::array<uint32_t, cfg::kProviderCount> paused_until_{};
  uint32_t last_start_ = kNever;
  Factors factors_{};
};

// Factors for one daily cap: 'left' requests for the expected 'demand' per
// class until the end of the day. Classes give way in Class order: a class
// is skipped while the others alone need the whole allowance, otherwise
// stretched just enough (0 when it would need more than 255 times).
Factors degrade(const std::array<uint32_t, kClassCount>& demand,
                uint32_t left);

// Live window of a stored match in UTC seconds: from kLiveLeadMinutes
// before kickoff to the final whistle when finished, to kickoff +
// kUnknownMatchEndMinutes while the provider reports it running, else to
// kickoff + kExpectedMatchMinutes. False without kickoff, for a finished
// match without final-whistle time and for postponed, cancelled or
// abandoned matches.
bool liveWindow(const StoredMatch& match, uint32_t& start, uint32_t& end);

}  // namespace football::schedule
