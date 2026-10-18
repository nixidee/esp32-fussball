// Host checks of the request scheduler (football::schedule). All matches,
// plans and timings below are written by the agent; they prove only the
// scheduler rules as implemented, not provider behaviour or the owner's
// device acceptance.

#include <unity.h>

#include <cstdint>
#include <vector>

#include "app_config.h"
#include "football_model.h"
#include "match_store.h"
#include "request_scheduler.h"
#include "settings_model.h"

namespace {

using football::Clock;
using football::MatchStore;
using football::Phase;
using football::StoredMatch;
using namespace football::schedule;

constexpr uint32_t kBoot = 1000;  // monotonic second at kBase
const int64_t kBase = football::parseUtc("2026-10-14T12:00:00Z");
constexpr uint32_t kLead = cfg::kLiveLeadMinutes * 60u;
constexpr uint32_t kExpected = cfg::kExpectedMatchMinutes * 60u;

MatchStore store;
Scheduler scheduler;
settings::ProviderPlans plans;

// Seconds after kBase.
Clock at(int64_t dt) { return {kBase + dt, static_cast<uint32_t>(kBoot + dt)}; }
uint32_t mono(int64_t dt) { return static_cast<uint32_t>(kBoot + dt); }
uint32_t utc(int64_t dt) { return static_cast<uint32_t>(kBase + dt); }

uint8_t bit(cfg::DataType type) {
  return static_cast<uint8_t>(1u << static_cast<unsigned>(type));
}

constexpr uint8_t kId = 1;
constexpr auto kOlg = cfg::Provider::kOpenLigaDb;
constexpr auto kApi = cfg::Provider::kApiFootball;

Inputs inputs(int64_t dt) {
  Inputs in;
  in.plans = &plans;
  in.daily_left.fill(kUnlimited);
  in.store = &store;
  in.clock = at(dt);
  return in;
}

void want(Inputs& in, uint8_t slot, uint8_t id, cfg::Provider provider,
          uint8_t types) {
  in.competitions[slot].id = id;
  in.competitions[slot].provider = provider;
  in.competitions[slot].types = types;
}

Inputs inputs(int64_t dt, uint8_t types, cfg::Provider provider = kOlg) {
  Inputs in = inputs(dt);
  want(in, 0, kId, provider, types);
  return in;
}

// The provider planner supplies an explicit selected Single identity. Do not
// infer event demand from an owned flag in the scheduler's fixture builder.
Inputs selectedInputs(int64_t dt, uint8_t types, uint32_t event_match) {
  auto in = inputs(dt, types);
  in.competitions[0].event_match = event_match;
  return in;
}

// A match of competition kId kicking off 'kickoff_dt' after kBase.
void addMatch(uint32_t id, int64_t kickoff_dt, bool own = false,
              Phase phase = Phase::kScheduled) {
  StoredMatch m;
  m.competition = kId;
  m.provider = kOlg;
  m.id = id;
  m.kickoff = utc(kickoff_dt);
  m.phase = phase;
  m.home = store.team(kId, kOlg, id * 2, "Home");
  m.away = store.team(kId, kOlg, id * 2 + 1, "Away");
  if (own) m.flags = football::kMatchOwn;
  TEST_ASSERT_NOT_NULL(store.upsert(m, at(0)));
}

// Runs the decision of next(): a run job sends its request now and ends
// with 'outcome'.
Decision step(const Inputs& in, Outcome outcome = Outcome::kOk,
              bool more = false) {
  const Decision d = scheduler.next(in);
  if (d.run) {
    scheduler.sent(d.job.provider, in.clock.mono_s);
    scheduler.finished(d.job, outcome, in.clock.mono_s, more);
  }
  return d;
}

void assertRuns(const Decision& d, cfg::DataType type) {
  TEST_ASSERT_TRUE(d.run);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(type),
                          static_cast<uint8_t>(d.job.type));
}

void assertWaits(const Decision& d, Wait why, uint32_t wake) {
  TEST_ASSERT_FALSE(d.run);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(why),
                          static_cast<uint8_t>(d.wait));
  TEST_ASSERT_EQUAL_UINT32(wake, d.wake_s);
}

}  // namespace

void setUp() {
  store.clear();
  scheduler.reset();
  plans = settings::providerPlansDefault();
  for (auto& p : plans) {
    p.limit = {};
    p.interval_s = {3600, 900, 60, 120, 30};
  }
}
void tearDown() {}

// ------------------------------------------------------------ window limit

void test_window_never_exceeds_limit() {
  for (const uint16_t window_s : {60, 61, 7, 3600}) {
    Window w;
    const uint16_t limit = 10;
    w.set(limit, window_s);
    std::vector<uint32_t> sent;
    uint32_t seed = 12345;
    const uint32_t span = window_s * 12u;
    // First half bursty demand, second half a request wanted every second.
    for (uint32_t t = 100; t < 100 + 2 * span; ++t) {
      seed = seed * 1103515245u + 12345u;
      if (t < 100 + span && (seed >> 16) % 3 == 0) continue;
      const uint32_t a = w.admissibleAt(t);
      TEST_ASSERT_TRUE(a >= t);
      if (a != t) continue;
      w.record(t);
      sent.push_back(t);
      std::size_t in_window = 0;
      for (const auto s : sent)
        if (s + window_s > t) ++in_window;
      TEST_ASSERT_TRUE(in_window <= limit);
    }
    // Saturated half: at most one bucket width of each window stays unused.
    const uint32_t width = (window_s + cfg::kLimitWindowBuckets - 2) /
                           (cfg::kLimitWindowBuckets - 1);
    std::size_t saturated = 0;
    for (const auto s : sent)
      if (s >= 100 + span) ++saturated;
    const uint32_t per_window = limit < window_s ? limit : window_s;
    const uint32_t floor_sent =
        per_window * (span / (window_s + width + 1)) - per_window;
    TEST_ASSERT_TRUE(saturated >= floor_sent);
  }
}

void test_window_admissible_at_is_the_first_free_bucket_second() {
  Window w;
  w.set(3, 60);  // bucket width 2 s
  w.record(10);
  w.record(11);
  w.record(20);
  TEST_ASSERT_TRUE(w.admissibleAt(20) > 20);
  const uint32_t free_at = w.admissibleAt(21);
  TEST_ASSERT_TRUE(free_at > 21);
  // The bucket of seconds 10-11 leaves the window 60 s after its end.
  TEST_ASSERT_EQUAL_UINT32(72, free_at);
  TEST_ASSERT_EQUAL_UINT32(72, w.admissibleAt(72));
  TEST_ASSERT_EQUAL_UINT32(72, w.admissibleAt(71));
  // Then 20 and 72 are in the window: one more fits, a fourth waits for the
  // bucket of second 20 (seconds 20-21) to leave at 82.
  w.record(72);
  TEST_ASSERT_EQUAL_UINT32(73, w.admissibleAt(73));
  w.record(73);
  TEST_ASSERT_EQUAL_UINT32(82, w.admissibleAt(74));
}

void test_window_unlimited_and_reset_on_change() {
  Window w;
  w.set(0, 60);
  TEST_ASSERT_FALSE(w.limited());
  w.record(5);
  TEST_ASSERT_EQUAL_UINT32(5, w.admissibleAt(5));
  w.set(2, 60);
  w.record(5);
  w.record(6);
  TEST_ASSERT_TRUE(w.admissibleAt(7) > 7);
  w.set(2, 60);  // unchanged: keeps the count
  TEST_ASSERT_TRUE(w.admissibleAt(7) > 7);
  w.set(3, 60);  // changed: starts empty
  TEST_ASSERT_EQUAL_UINT32(7, w.admissibleAt(7));
}

// ------------------------------------------------------------- degradation

void test_degrade_gives_way_in_class_order() {
  // live table, table, fixtures, live, own
  const std::array<uint32_t, kClassCount> demand{100, 10, 7, 50, 50};
  Factors f = degrade(demand, 1000);
  for (const auto x : f) TEST_ASSERT_EQUAL_UINT8(1, x);
  // The live table alone is stretched: 117 others, 3 left for 100.
  f = degrade(demand, 120);
  TEST_ASSERT_EQUAL_UINT8(34, f[0]);
  for (std::size_t c = 1; c < kClassCount; ++c)
    TEST_ASSERT_EQUAL_UINT8(1, f[c]);
  // Exactly the live and own demand fit: everything before is skipped.
  f = degrade(demand, 100);
  const Factors skip3{0, 0, 0, 1, 1};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(skip3.data(), f.data(), kClassCount);
  // Live is stretched before the own match is touched.
  f = degrade(demand, 60);
  const Factors live5{0, 0, 0, 5, 1};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(live5.data(), f.data(), kClassCount);
  // The own match is stretched last.
  f = degrade(demand, 10);
  const Factors own5{0, 0, 0, 0, 5};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(own5.data(), f.data(), kClassCount);
  // More than 255 times counts as skipped.
  f = degrade({0, 0, 0, 0, 3000}, 10);
  TEST_ASSERT_EQUAL_UINT8(0, f[4]);
  f = degrade(demand, 0);
  for (const auto x : f) TEST_ASSERT_EQUAL_UINT8(0, x);
}

// ------------------------------------------------------------- live window

void test_live_window_per_phase() {
  StoredMatch m;
  m.kickoff = utc(0);
  uint32_t start = 0, end = 0;
  m.phase = Phase::kScheduled;
  TEST_ASSERT_TRUE(liveWindow(m, start, end));
  TEST_ASSERT_EQUAL_UINT32(utc(0) - kLead, start);
  TEST_ASSERT_EQUAL_UINT32(utc(0) + kExpected, end);
  m.phase = Phase::kLive;  // reported by the provider
  TEST_ASSERT_TRUE(liveWindow(m, start, end));
  TEST_ASSERT_EQUAL_UINT32(utc(0) + cfg::kUnknownMatchEndMinutes * 60u, end);
  m.estimated = football::kEstimatedPhase;  // derived by the device
  TEST_ASSERT_TRUE(liveWindow(m, start, end));
  TEST_ASSERT_EQUAL_UINT32(utc(0) + kExpected, end);
  m.estimated = 0;
  m.phase = Phase::kFinished;
  m.finished_at = utc(6300);
  TEST_ASSERT_TRUE(liveWindow(m, start, end));
  TEST_ASSERT_EQUAL_UINT32(utc(6300), end);
  m.finished_at = 0;
  TEST_ASSERT_FALSE(liveWindow(m, start, end));
  for (const auto p :
       {Phase::kPostponed, Phase::kCancelled, Phase::kAbandoned}) {
    m.phase = p;
    TEST_ASSERT_FALSE(liveWindow(m, start, end));
  }
  m.phase = Phase::kScheduled;
  m.kickoff = 0;
  TEST_ASSERT_FALSE(liveWindow(m, start, end));
}

// ------------------------------------------------------------- due times

void test_periodic_job_runs_once_per_interval() {
  const uint8_t types = bit(cfg::DataType::kFixtures);
  assertRuns(step(inputs(0, types)), cfg::DataType::kFixtures);
  assertWaits(step(inputs(1, types)), Wait::kIdle, mono(3600));
  assertWaits(step(inputs(3599, types)), Wait::kIdle, mono(3600));
  assertRuns(step(inputs(3600, types)), cfg::DataType::kFixtures);
}

void test_live_job_runs_only_in_the_live_window() {
  addMatch(1, 3600);
  const uint8_t types = bit(cfg::DataType::kLive);
  // Before the window: wake at kickoff − lead.
  assertWaits(step(inputs(0, types)), Wait::kIdle, mono(3600 - kLead));
  assertRuns(step(inputs(3600 - kLead, types)), cfg::DataType::kLive);
  assertWaits(step(inputs(3600 - kLead + 10, types)), Wait::kIdle,
              mono(3600 - kLead + 60));
  assertRuns(step(inputs(3600 - kLead + 60, types)), cfg::DataType::kLive);
  // After the expected end: nothing planned, no final request for live.
  const int64_t end = 3600 + kExpected;
  assertWaits(step(inputs(end + 5, types)), Wait::kIdle, 0);
}

void test_final_fetch_for_events_and_live_table() {
  addMatch(1, 3600, true);
  const uint8_t types =
      bit(cfg::DataType::kEvents) | bit(cfg::DataType::kLiveTable);
  const int64_t end = 3600 + kExpected;
  // Both ran inside the window.
  Decision a = step(selectedInputs(end - 100, types, 1));
  Decision b = step(selectedInputs(end - 90, types, 1));
  TEST_ASSERT_TRUE(a.run && b.run);
  TEST_ASSERT_TRUE(a.job.type != b.job.type);
  // Once each after the end, within its interval.
  a = step(selectedInputs(end + 5, types, 1));
  b = step(selectedInputs(end + 7, types, 1));
  TEST_ASSERT_TRUE(a.run && b.run);
  TEST_ASSERT_TRUE(a.job.type != b.job.type);
  assertWaits(step(selectedInputs(end + 20, types, 1)), Wait::kIdle, 0);
}

void test_no_final_fetch_after_its_interval() {
  addMatch(1, 3600, true);
  const uint8_t types = bit(cfg::DataType::kEvents);
  const int64_t end = 3600 + kExpected;
  assertRuns(step(selectedInputs(end - 100, types, 1)), cfg::DataType::kEvents);
  // Events interval is 30 s: 31 s after the end it is too late.
  assertWaits(step(selectedInputs(end + 31, types, 1)), Wait::kIdle, 0);
}

void test_events_without_selected_identity_never_infer_an_owned_match() {
  for (const bool own : {false, true}) {
    store.clear();
    scheduler.reset();
    addMatch(1, -60, own, Phase::kLive);
    const auto in = inputs(0, bit(cfg::DataType::kEvents));
    TEST_ASSERT_EQUAL_UINT32(0, in.competitions[0].event_match);
    assertWaits(step(in), Wait::kIdle, 0);
  }
}

void test_window_that_ended_before_boot_gets_no_final_fetch() {
  // A match that ended a day ago, the device booted 30 s ago: the end lies
  // far beyond one live-table interval, so nothing is due.
  addMatch(1, -86400);
  Inputs in = inputs(0, bit(cfg::DataType::kLiveTable));
  in.clock.mono_s = 30;
  assertWaits(step(in), Wait::kIdle, 0);
}

void test_window_that_ended_just_before_boot_gets_its_final_fetch() {
  // The end lies 60 s before now, booted 30 s ago, interval 120 s.
  addMatch(1, -60 - static_cast<int64_t>(kExpected));
  Inputs in = inputs(0, bit(cfg::DataType::kLiveTable));
  in.clock.mono_s = 30;
  assertRuns(step(in), cfg::DataType::kLiveTable);
}

void test_table_and_live_table_share_their_start() {
  addMatch(1, 3600);
  const uint8_t types =
      bit(cfg::DataType::kTable) | bit(cfg::DataType::kLiveTable);
  // At the window start both are due; the table (higher class) runs and
  // its start covers the live table.
  const int64_t start = 3600 - kLead;
  assertRuns(step(inputs(start, types)), cfg::DataType::kTable);
  const Decision d = step(inputs(start + 2, types));
  TEST_ASSERT_FALSE(d.run);
  // The next live table follows its interval (120 s).
  assertRuns(step(inputs(start + 120, types)), cfg::DataType::kLiveTable);
}

void test_window_reentry_runs_at_once() {
  addMatch(1, 3600);
  addMatch(2, 3600 + 3 * 3600);  // second window, separate from the first
  const uint8_t types = bit(cfg::DataType::kLive);
  assertRuns(step(inputs(3600, types)), cfg::DataType::kLive);
  // Between the windows: wake at the next start.
  const int64_t second = 3600 + 3 * 3600 - kLead;
  assertWaits(step(inputs(3600 + kExpected + 1, types)), Wait::kIdle,
              mono(second));
  assertRuns(step(inputs(second, types)), cfg::DataType::kLive);
}

// ------------------------------------------------------------- selection

void test_own_live_first_then_class_order() {
  addMatch(1, 0, true);
  const uint8_t types = bit(cfg::DataType::kFixtures) |
                        bit(cfg::DataType::kTable) | bit(cfg::DataType::kLive);
  assertRuns(step(inputs(0, types)), cfg::DataType::kLive);
  assertRuns(step(inputs(2, types)), cfg::DataType::kFixtures);
  assertRuns(step(inputs(4, types)), cfg::DataType::kTable);
}

void test_device_gap() {
  const uint8_t types =
      bit(cfg::DataType::kFixtures) | bit(cfg::DataType::kTable);
  assertRuns(step(inputs(0, types)), cfg::DataType::kFixtures);
  assertWaits(step(inputs(1, types)), Wait::kGap, mono(2));
  assertRuns(step(inputs(2, types)), cfg::DataType::kTable);
}

void test_paused_provider_does_not_hold_back_another() {
  addMatch(1, 0, true);
  Inputs in = inputs(0);
  want(in, 0, kId, kApi, bit(cfg::DataType::kLive));
  want(in, 1, 7, kOlg, bit(cfg::DataType::kFixtures));
  Decision d = step(in, Outcome::kRateLimited);
  assertRuns(d, cfg::DataType::kLive);
  // The paused provider's own live job is due again at 60 s, but waits;
  // the other provider's fixtures job runs.
  in.clock = at(60);
  assertRuns(step(in), cfg::DataType::kFixtures);
  in.clock = at(70);
  assertWaits(step(in), Wait::kPaused, mono(cfg::kProviderRateRetryS));
}

void test_daily_cap_used_up_waits_for_the_utc_day_end() {
  Inputs in = inputs(0, bit(cfg::DataType::kFixtures), kApi);
  in.daily_left[static_cast<std::size_t>(kApi)] = 0;
  // kBase is 12:00 UTC: the day ends 12 h later.
  assertWaits(step(in), Wait::kDaily, mono(12 * 3600));
  in.daily_left[static_cast<std::size_t>(kApi)] = kUnlimited;
  in.global_daily_left = 0;
  assertWaits(step(in), Wait::kDaily, mono(12 * 3600));
}

void test_provider_window_limit_blocks() {
  plans[static_cast<std::size_t>(kOlg)].limit = {0, 2, 60};
  const uint8_t types =
      bit(cfg::DataType::kTable) | bit(cfg::DataType::kFixtures);
  // Two competitions of the same provider: four jobs, two per minute.
  const auto both = [types](int64_t dt) {
    Inputs in = inputs(dt, types);
    want(in, 1, 2, kOlg, types);
    return in;
  };
  // Fixtures rank before the table (OQ-221 order).
  assertRuns(step(both(0)), cfg::DataType::kFixtures);
  TEST_ASSERT_TRUE(step(both(2)).run);
  const Decision d = step(both(4));
  TEST_ASSERT_FALSE(d.run);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Wait::kWindow),
                          static_cast<uint8_t>(d.wait));
  TEST_ASSERT_TRUE(d.wake_s >= mono(60) && d.wake_s <= mono(64));
  TEST_ASSERT_TRUE(step(both(d.wake_s - kBoot)).run);
}

void test_failure_backoff_doubles_up_to_the_interval() {
  const uint8_t types = bit(cfg::DataType::kFixtures);
  int64_t t = 0;
  uint32_t expected = cfg::kProviderErrorRetryS;
  for (int i = 0; i < 9; ++i) {
    const Decision d = step(inputs(t, types), Outcome::kFailed);
    assertRuns(d, cfg::DataType::kFixtures);
    const uint32_t wait = expected < 3600 ? expected : 3600;
    assertWaits(step(inputs(t + 1, types)), Wait::kIdle, mono(t + wait));
    t += wait;
    expected *= 2;
  }
  step(inputs(t, types), Outcome::kOk);
  assertWaits(step(inputs(t + 1, types)), Wait::kIdle, mono(t + 3600));
}

void test_slot_id_change_starts_fresh() {
  const uint8_t types = bit(cfg::DataType::kFixtures);
  assertRuns(step(inputs(0, types)), cfg::DataType::kFixtures);
  Inputs in = inputs(10);
  want(in, 0, 2, kOlg, types);
  const Decision d = step(in);
  assertRuns(d, cfg::DataType::kFixtures);
  TEST_ASSERT_EQUAL_UINT8(2, d.job.competition);
}

void test_daily_cap_degrades_before_it_runs_out() {
  // 12 h left: fixtures 12, table 48 requests expected.
  Inputs in = inputs(
      0, bit(cfg::DataType::kTable) | bit(cfg::DataType::kFixtures), kApi);
  in.daily_left[static_cast<std::size_t>(kApi)] = 1;
  const Decision d = step(in);
  assertRuns(d, cfg::DataType::kFixtures);
  TEST_ASSERT_TRUE(scheduler.degraded());
  const Factors expected{1, 0, 12, 1, 1};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), scheduler.factors().data(),
                                kClassCount);
  // Plenty left: back to normal.
  in.daily_left[static_cast<std::size_t>(kApi)] = 1000;
  in.clock = at(2);
  assertRuns(step(in), cfg::DataType::kTable);
  TEST_ASSERT_FALSE(scheduler.degraded());
}

void test_continued_run_stays_due_until_its_last_request() {
  const uint8_t types = bit(cfg::DataType::kFixtures);
  Decision d = step(inputs(0, types), Outcome::kOk, true);
  assertRuns(d, cfg::DataType::kFixtures);
  // The next request of the run follows after the device gap.
  assertWaits(step(inputs(1, types)), Wait::kGap, mono(2));
  d = step(inputs(2, types), Outcome::kOk);
  assertRuns(d, cfg::DataType::kFixtures);
  // Run complete: the interval counts from its last request.
  assertWaits(step(inputs(4, types)), Wait::kIdle, mono(2 + 3600));
  // A failure ends a continued run.
  step(inputs(3602, types), Outcome::kOk, true);
  d = step(inputs(3604, types), Outcome::kFailed);
  assertRuns(d, cfg::DataType::kFixtures);
  assertWaits(step(inputs(3606, types)), Wait::kIdle,
              mono(3604 + cfg::kProviderErrorRetryS));
}

void test_run_cost_multiplies_the_daily_demand() {
  // 12 h left, fixtures every hour: 12 runs.
  Inputs in = inputs(0, bit(cfg::DataType::kFixtures), kApi);
  in.daily_left[static_cast<std::size_t>(kApi)] = 20;
  assertRuns(scheduler.next(in), cfg::DataType::kFixtures);
  TEST_ASSERT_FALSE(scheduler.degraded());
  // Three requests per run: 36 expected, 20 left.
  in.competitions[0].cost[static_cast<std::size_t>(cfg::DataType::kFixtures)] =
      3;
  scheduler.next(in);
  TEST_ASSERT_TRUE(scheduler.degraded());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_window_never_exceeds_limit);
  RUN_TEST(test_window_admissible_at_is_the_first_free_bucket_second);
  RUN_TEST(test_window_unlimited_and_reset_on_change);
  RUN_TEST(test_degrade_gives_way_in_class_order);
  RUN_TEST(test_live_window_per_phase);
  RUN_TEST(test_periodic_job_runs_once_per_interval);
  RUN_TEST(test_live_job_runs_only_in_the_live_window);
  RUN_TEST(test_final_fetch_for_events_and_live_table);
  RUN_TEST(test_no_final_fetch_after_its_interval);
  RUN_TEST(test_events_without_selected_identity_never_infer_an_owned_match);
  RUN_TEST(test_window_that_ended_before_boot_gets_no_final_fetch);
  RUN_TEST(test_window_that_ended_just_before_boot_gets_its_final_fetch);
  RUN_TEST(test_table_and_live_table_share_their_start);
  RUN_TEST(test_window_reentry_runs_at_once);
  RUN_TEST(test_own_live_first_then_class_order);
  RUN_TEST(test_device_gap);
  RUN_TEST(test_paused_provider_does_not_hold_back_another);
  RUN_TEST(test_daily_cap_used_up_waits_for_the_utc_day_end);
  RUN_TEST(test_provider_window_limit_blocks);
  RUN_TEST(test_failure_backoff_doubles_up_to_the_interval);
  RUN_TEST(test_slot_id_change_starts_fresh);
  RUN_TEST(test_daily_cap_degrades_before_it_runs_out);
  RUN_TEST(test_continued_run_stays_due_until_its_last_request);
  RUN_TEST(test_run_cost_multiplies_the_daily_demand);
  return UNITY_END();
}
