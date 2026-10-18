// OQ-398/399 regression cases: polling must follow the same justified live
// interval as canonical screen selection, including an unknown kickoff and
// suspension. Synthetic fixtures prove boundaries, not provider/device results.
#include <unity.h>

#include "request_scheduler.h"
#include "screen_selection.h"

namespace {
using namespace football;
namespace schedule = football::schedule;
namespace selection = football::selection;
namespace policy = settings::screen_policy;

constexpr uint32_t kObserved = 1792238400;
constexpr uint32_t kHardSeconds = cfg::kUnknownMatchEndMinutes * 60u;
constexpr uint32_t kExpectedSeconds = cfg::kExpectedMatchMinutes * 60u;
constexpr auto kProvider = cfg::Provider::kOpenLigaDb;
MatchStore store;
settings::CompetitionRoutes competitions;
settings::ProviderPlans plans;

StoredMatch match(Phase phase, uint32_t kickoff = 0,
                  uint32_t observed = kObserved) {
  StoredMatch m;
  m.id = 10;
  m.competition = 1;
  m.provider = kProvider;
  m.phase = phase;
  m.kickoff = kickoff;
  m.first_live_observed_utc = observed;
  return m;
}

selection::Window selectedWindow(const StoredMatch& m, uint32_t utc) {
  store.clear();
  TEST_ASSERT_NOT_NULL(store.upsert(m, {0, 0}));
  policy::SelectionRules rules;
  rules.other_before_min = cfg::kLiveLeadMinutes;
  rules.other_after_min = 0;
  const selection::Input in{rules, competitions, store, {utc, 1000}};
  return selection::window(in, competitions.entries[0], store.match(0));
}

schedule::Inputs requestsAt(uint32_t utc,
                            cfg::DataType type = cfg::DataType::kLive,
                            uint32_t event_match = 0) {
  schedule::Inputs in;
  in.plans = &plans;
  in.daily_left.fill(schedule::kUnlimited);
  in.store = &store;
  in.clock = {utc, 1000};
  auto& wanted = in.competitions[0];
  wanted.id = 1;
  wanted.provider = kProvider;
  wanted.types = static_cast<uint8_t>(1u << static_cast<unsigned>(type));
  wanted.event_match = event_match;
  return in;
}

schedule::Decision fromStoredAt(uint32_t utc,
                                cfg::DataType type = cfg::DataType::kLive,
                                uint32_t event_match = 0) {
  schedule::Scheduler scheduler;
  return scheduler.next(requestsAt(utc, type, event_match));
}

schedule::Decision polledAt(const StoredMatch& m, uint32_t utc,
                            cfg::DataType type = cfg::DataType::kLive) {
  store.clear();
  TEST_ASSERT_NOT_NULL(store.upsert(m, {0, 0}));
  return fromStoredAt(utc, type);
}

void appendMatch(StoredMatch m, uint32_t id) {
  m.id = id;
  TEST_ASSERT_NOT_NULL(store.upsert(m, {0, 0}));
}

void test_unknown_kickoff_confirmed_live_polls_from_first_observation() {
  auto m = match(Phase::kLive);
  uint32_t start = 0, end = 0;
  TEST_ASSERT_TRUE(schedule::liveWindow(m, start, end));
  TEST_ASSERT_EQUAL_UINT32(kObserved, start);
  TEST_ASSERT_EQUAL_UINT32(kObserved + kHardSeconds, end);
  TEST_ASSERT_FALSE(selectedWindow(m, start - 1).active);
  TEST_ASSERT_FALSE(polledAt(m, start - 1).run);
  TEST_ASSERT_TRUE(selectedWindow(m, start).active);
  TEST_ASSERT_TRUE(polledAt(m, start).run);
  TEST_ASSERT_EQUAL_UINT32(0, m.kickoff);
}

void test_unknown_kickoff_has_an_exclusive_240_minute_bound() {
  const auto m = match(Phase::kLive);
  TEST_ASSERT_TRUE(selectedWindow(m, kObserved + kHardSeconds - 1).active);
  TEST_ASSERT_TRUE(polledAt(m, kObserved + kHardSeconds - 1).run);
  TEST_ASSERT_FALSE(selectedWindow(m, kObserved + kHardSeconds).active);
  TEST_ASSERT_FALSE(polledAt(m, kObserved + kHardSeconds).run);
}

void test_live_without_kickoff_or_observation_never_invents_a_window() {
  const auto m = match(Phase::kLive, 0, 0);
  uint32_t start = 0, end = 0;
  TEST_ASSERT_FALSE(schedule::liveWindow(m, start, end));
  TEST_ASSERT_FALSE(selectedWindow(m, kObserved).active);
  TEST_ASSERT_FALSE(polledAt(m, kObserved).run);
}

void test_estimated_live_without_kickoff_does_not_reuse_old_observation() {
  auto m = match(Phase::kLive);
  m.estimated = kEstimatedPhase;
  uint32_t start = 0, end = 0;
  TEST_ASSERT_FALSE(schedule::liveWindow(m, start, end));
  TEST_ASSERT_FALSE(selectedWindow(m, kObserved).active);
  TEST_ASSERT_FALSE(polledAt(m, kObserved).run);
}

void test_scheduled_without_kickoff_does_not_reuse_old_observation() {
  const auto m = match(Phase::kScheduled);
  uint32_t start = 0, end = 0;
  TEST_ASSERT_FALSE(schedule::liveWindow(m, start, end));
  TEST_ASSERT_FALSE(selectedWindow(m, kObserved).active);
  TEST_ASSERT_FALSE(polledAt(m, kObserved).run);
}

void test_suspension_without_a_previous_live_report_is_not_polled() {
  for (const uint32_t kickoff : {uint32_t{0}, kObserved}) {
    const auto m = match(Phase::kSuspended, kickoff, 0);
    uint32_t start = 0, end = 0;
    TEST_ASSERT_FALSE(schedule::liveWindow(m, start, end));
    TEST_ASSERT_FALSE(selectedWindow(m, kObserved).active);
    TEST_ASSERT_FALSE(polledAt(m, kObserved).run);
  }
}

void test_justified_suspension_uses_known_kickoff_and_240_minute_bound() {
  const auto m = match(Phase::kSuspended, kObserved, kObserved + 60);
  uint32_t start = 0, end = 0;
  TEST_ASSERT_TRUE(schedule::liveWindow(m, start, end));
  TEST_ASSERT_EQUAL_UINT32(kObserved - cfg::kLiveLeadMinutes * 60u, start);
  TEST_ASSERT_EQUAL_UINT32(kObserved + kHardSeconds, end);
  TEST_ASSERT_TRUE(selectedWindow(m, kObserved + kExpectedSeconds + 1).active);
  TEST_ASSERT_TRUE(polledAt(m, kObserved + kExpectedSeconds + 1).run);
  TEST_ASSERT_TRUE(selectedWindow(m, end - 1).active);
  TEST_ASSERT_TRUE(polledAt(m, end - 1).run);
  TEST_ASSERT_FALSE(selectedWindow(m, end).active);
  TEST_ASSERT_FALSE(polledAt(m, end).run);
}

void test_justified_suspension_without_kickoff_keeps_observation_bound() {
  const auto m = match(Phase::kSuspended);
  uint32_t start = 0, end = 0;
  TEST_ASSERT_TRUE(schedule::liveWindow(m, start, end));
  TEST_ASSERT_EQUAL_UINT32(kObserved, start);
  TEST_ASSERT_EQUAL_UINT32(kObserved + kHardSeconds, end);
  TEST_ASSERT_FALSE(selectedWindow(m, start - 1).active);
  TEST_ASSERT_TRUE(selectedWindow(m, start).active);
  TEST_ASSERT_TRUE(polledAt(m, start).run);
  TEST_ASSERT_FALSE(selectedWindow(m, end).active);
  TEST_ASSERT_FALSE(polledAt(m, end).run);
}

void test_reported_finish_without_kickoff_admits_only_the_final_fetch() {
  auto m = match(Phase::kFinished, 0, 0);
  m.finished_at = kObserved;
  uint32_t start = 0, end = 0;
  TEST_ASSERT_TRUE(schedule::liveWindow(m, start, end));
  TEST_ASSERT_EQUAL_UINT32(kObserved, start);
  TEST_ASSERT_EQUAL_UINT32(kObserved, end);
  TEST_ASSERT_FALSE(polledAt(m, kObserved).run);
  TEST_ASSERT_TRUE(polledAt(m, kObserved, cfg::DataType::kLiveTable).run);
  const auto& source = plans[static_cast<unsigned>(kProvider)];
  const auto interval =
      source.interval_s[static_cast<unsigned>(cfg::DataType::kLiveTable)];
  TEST_ASSERT_FALSE(
      polledAt(m, kObserved + interval, cfg::DataType::kLiveTable).run);
  TEST_ASSERT_EQUAL_UINT32(0, m.kickoff);
}

void test_finish_without_a_final_whistle_and_inactive_states_have_no_window() {
  uint32_t start = 0, end = 0;
  TEST_ASSERT_FALSE(
      schedule::liveWindow(match(Phase::kFinished, kObserved), start, end));
  for (const auto phase :
       {Phase::kPostponed, Phase::kCancelled, Phase::kAbandoned}) {
    auto m = match(phase, kObserved);
    TEST_ASSERT_FALSE(schedule::liveWindow(m, start, end));
    TEST_ASSERT_FALSE(selectedWindow(m, kObserved).active);
    TEST_ASSERT_FALSE(polledAt(m, kObserved).run);
  }
}

void test_future_known_kickoff_does_not_hide_current_unknown_kickoff_live_game() {
  appendMatch(match(Phase::kScheduled, kObserved + 3600, 0), 10);
  appendMatch(match(Phase::kLive), 20);
  TEST_ASSERT_EQUAL_UINT32(10, store.match(0).id);
  TEST_ASSERT_EQUAL_UINT32(20, store.match(1).id);
  TEST_ASSERT_TRUE(fromStoredAt(kObserved).run);
}

void test_clock_rollback_keeps_earliest_future_observation_as_next_window() {
  // Observations survive a clock correction. Equal unknown kickoffs keep
  // insertion order, which is deliberately different from observation order.
  appendMatch(match(Phase::kLive, 0, kObserved + 3600), 10);
  appendMatch(match(Phase::kLive, 0, kObserved + 600), 20);
  appendMatch(match(Phase::kLive, 0, kObserved + 1800), 30);
  const auto decision = fromStoredAt(kObserved);
  TEST_ASSERT_FALSE(decision.run);
  TEST_ASSERT_EQUAL_UINT32(1000 + 600, decision.wake_s);
}

void test_future_known_game_does_not_hide_unknown_kickoff_final_request() {
  auto future = match(Phase::kScheduled, kObserved + 3600, 0);
  future.home = store.team(1, kProvider, 40, "Own club");
  store.selectClub(1, kProvider, 40, {kObserved, 1});
  appendMatch(future, 10);
  auto result = match(Phase::kFinished, 0, 0);
  result.home = store.team(1, kProvider, 40, "Own club");
  store.selectClub(1, kProvider, 40, {kObserved, 1});
  result.finished_at = kObserved - 1;
  appendMatch(result, 20);
  // Select the finished identity explicitly, as the provider planner does.
  TEST_ASSERT_TRUE(fromStoredAt(kObserved, cfg::DataType::kEvents, 20).run);
}

void test_selected_unknown_kickoff_final_fetch_after_an_initial_request() {
  appendMatch(match(Phase::kScheduled, kObserved + 3600, 0), 10);
  appendMatch(match(Phase::kLive), 20);
  schedule::Scheduler scheduler;
  auto in = requestsAt(kObserved, cfg::DataType::kEvents, 20);
  const auto first = scheduler.next(in);
  TEST_ASSERT_TRUE(first.run);
  TEST_ASSERT_EQUAL_UINT32(20, first.job.event_match);
  scheduler.sent(first.job.provider, in.clock.mono_s);
  scheduler.finished(first.job, schedule::Outcome::kOk, in.clock.mono_s);

  auto result = match(Phase::kFinished, 0, 0);
  result.finished_at = kObserved + 10;
  appendMatch(result, 20);
  in.clock = {kObserved + 11, 1011};
  const auto final = scheduler.next(in);
  TEST_ASSERT_TRUE(final.run);
  TEST_ASSERT_EQUAL_UINT32(20, final.job.event_match);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cfg::DataType::kEvents),
                          static_cast<uint8_t>(final.job.type));
  scheduler.sent(final.job.provider, in.clock.mono_s);
  scheduler.finished(final.job, schedule::Outcome::kOk, in.clock.mono_s);
  in.clock = {kObserved + 20, 1020};
  TEST_ASSERT_FALSE(scheduler.next(in).run);
  TEST_ASSERT_EQUAL_UINT32(0, store.find(1, kProvider, 20)->kickoff);
}

void test_all_finished_windows_stay_idle_after_final_request_deadline() {
  auto known = match(Phase::kFinished, kObserved - 12000, 0);
  known.finished_at = kObserved - 1000;
  appendMatch(known, 10);
  auto unknown = match(Phase::kFinished, 0, 0);
  unknown.finished_at = kObserved - 2000;
  appendMatch(unknown, 20);
  const auto decision = fromStoredAt(kObserved, cfg::DataType::kLiveTable);
  TEST_ASSERT_FALSE(decision.run);
  TEST_ASSERT_EQUAL_UINT32(0, decision.wake_s);
}

void test_overlap_chain_and_repeated_starts_do_not_stretch_polling_under_cap() {
  // Four rows describe one continuous live interval, with 190 further
  // minute polls plus its initial poll. Duplicates must not consume another
  // window's allowance or slow down otherwise affordable live polling.
  appendMatch(match(Phase::kLive, 0, kObserved - 3000), 10);
  appendMatch(match(Phase::kLive, 0, kObserved - 18000), 20);
  appendMatch(match(Phase::kLive, 0, kObserved - 7200), 30);
  appendMatch(match(Phase::kLive, 0, kObserved - 3000), 40);
  auto in = requestsAt(kObserved);
  in.daily_left[static_cast<unsigned>(kProvider)] = 191;
  schedule::Scheduler scheduler;
  TEST_ASSERT_TRUE(scheduler.next(in).run);
  TEST_ASSERT_FALSE(scheduler.degraded());
}

void test_disjoint_out_of_order_windows_keep_the_earliest_next_start() {
  appendMatch(match(Phase::kLive, 0, kObserved + 20000), 10);
  appendMatch(match(Phase::kLive, 0, kObserved + 1000), 20);
  const auto decision = fromStoredAt(kObserved);
  TEST_ASSERT_FALSE(decision.run);
  TEST_ASSERT_EQUAL_UINT32(2000, decision.wake_s);
}
}  // namespace

void setUp() {
  store.clear();
  competitions = {};
  settings::Competition c;
  c.id = 1;
  c.incarnation = 1;
  c.primary.provider = kProvider;
  c.primary.competition.assign("league");
  TEST_ASSERT_TRUE(competitions.entries.push(c));
  plans = {};
  for (auto& plan : plans) plan.interval_s = {3600, 900, 60, 120, 30};
}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_unknown_kickoff_confirmed_live_polls_from_first_observation);
  RUN_TEST(test_unknown_kickoff_has_an_exclusive_240_minute_bound);
  RUN_TEST(test_live_without_kickoff_or_observation_never_invents_a_window);
  RUN_TEST(test_estimated_live_without_kickoff_does_not_reuse_old_observation);
  RUN_TEST(test_scheduled_without_kickoff_does_not_reuse_old_observation);
  RUN_TEST(test_suspension_without_a_previous_live_report_is_not_polled);
  RUN_TEST(test_justified_suspension_uses_known_kickoff_and_240_minute_bound);
  RUN_TEST(test_justified_suspension_without_kickoff_keeps_observation_bound);
  RUN_TEST(test_reported_finish_without_kickoff_admits_only_the_final_fetch);
  RUN_TEST(
      test_finish_without_a_final_whistle_and_inactive_states_have_no_window);
  RUN_TEST(
      test_future_known_kickoff_does_not_hide_current_unknown_kickoff_live_game);
  RUN_TEST(
      test_clock_rollback_keeps_earliest_future_observation_as_next_window);
  RUN_TEST(test_future_known_game_does_not_hide_unknown_kickoff_final_request);
  RUN_TEST(test_selected_unknown_kickoff_final_fetch_after_an_initial_request);
  RUN_TEST(test_all_finished_windows_stay_idle_after_final_request_deadline);
  RUN_TEST(
      test_overlap_chain_and_repeated_starts_do_not_stretch_polling_under_cap);
  RUN_TEST(test_disjoint_out_of_order_windows_keep_the_earliest_next_start);
  return UNITY_END();
}
