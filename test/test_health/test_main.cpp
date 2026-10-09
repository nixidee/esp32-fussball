#include <unity.h>

#include <cstdint>
#include <type_traits>

#include "health_meter.h"

namespace {

using health::Coordinator;
using health::Meter;
using health::MinimumScope;
using health::Point;

static_assert(!std::is_copy_constructible_v<Meter>);
static_assert(!std::is_move_constructible_v<Meter>);
static_assert(!std::is_copy_assignable_v<Meter>);
static_assert(!std::is_move_assignable_v<Meter>);

struct Fake {
  Point point{1000, 1000, 900, 800};
  uint32_t starts = 0;
  uint32_t stops = 0;
  uint32_t samples = 0;
  uint32_t saved_minimum = 0;
  int32_t start_error = 0;
  int32_t stop_error = 0;
  bool monitoring = false;
  void (*on_sample)(Fake&) = nullptr;
  void (*on_start)(Fake&) = nullptr;
  void (*on_stop)(Fake&) = nullptr;
  Coordinator* coordinator = nullptr;
  MinimumScope observed_scope = MinimumScope::kLifetime;

  static Point sample(void* context) noexcept {
    auto& fake = *static_cast<Fake*>(context);
    ++fake.samples;
    if (fake.on_sample != nullptr) {
      const auto hook = fake.on_sample;
      fake.on_sample = nullptr;
      hook(fake);
    }
    return fake.point;
  }

  static int32_t start(void* context) noexcept {
    auto& fake = *static_cast<Fake*>(context);
    ++fake.starts;
    if (fake.on_start != nullptr) {
      const auto hook = fake.on_start;
      fake.on_start = nullptr;
      hook(fake);
    }
    if (fake.start_error != 0) return fake.start_error;
    fake.saved_minimum = fake.point.minimum_bytes;
    fake.point.minimum_bytes = fake.point.free_bytes;
    fake.monitoring = true;
    return 0;
  }

  static int32_t stop(void* context) noexcept {
    auto& fake = *static_cast<Fake*>(context);
    ++fake.stops;
    if (fake.on_stop != nullptr) {
      const auto hook = fake.on_stop;
      fake.on_stop = nullptr;
      hook(fake);
    }
    if (fake.stop_error != 0) return fake.stop_error;
    if (fake.saved_minimum < fake.point.minimum_bytes)
      fake.point.minimum_bytes = fake.saved_minimum;
    fake.monitoring = false;
    return 0;
  }

  health::Backend backend() noexcept { return {this, sample, start, stop}; }
};

void testLoserFinishesBeforeOwner() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  Meter owner(coordinator);
  fake.point = {1020, 700, 650, 600};
  Meter loser(coordinator);
  fake.point.monotonic_ms = 1040;
  const auto& lost = loser.finish();
  TEST_ASSERT_FALSE(lost.owned);
  TEST_ASSERT_EQUAL(MinimumScope::kUnavailable, lost.scope);
  TEST_ASSERT_EQUAL_UINT32(700, lost.end.free_bytes);
  TEST_ASSERT_EQUAL_INT64(1020, lost.begin.monotonic_ms);
  TEST_ASSERT_EQUAL_INT64(1040, lost.end.monotonic_ms);
  TEST_ASSERT_EQUAL_UINT32(0, fake.stops);
  TEST_ASSERT_TRUE(fake.monitoring);
  TEST_ASSERT_EQUAL(MinimumScope::kInterval, coordinator.observe().scope);

  const auto& won = owner.finish();
  TEST_ASSERT_TRUE(won.owned);
  TEST_ASSERT_EQUAL(MinimumScope::kInterval, won.scope);
  TEST_ASSERT_EQUAL_UINT32(600, won.end.minimum_bytes);
  TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
}

void testOwnerFinishesBeforeLoserWithoutRetry() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  Meter owner(coordinator);
  Meter loser(coordinator);
  owner.finish();
  TEST_ASSERT_EQUAL(MinimumScope::kLifetime, coordinator.observe().scope);

  {
    Meter next_owner(coordinator);
    TEST_ASSERT_EQUAL_UINT32(2, fake.starts);
    loser.finish();
    TEST_ASSERT_TRUE(fake.monitoring);
    TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
    TEST_ASSERT_EQUAL(MinimumScope::kInterval, coordinator.observe().scope);
  }
  TEST_ASSERT_EQUAL_UINT32(2, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(2, fake.stops);
}

void testNestedMetersKeepFirstOwner() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  {
    Meter outer(coordinator);
    {
      Meter inner(coordinator);
      Meter deepest(coordinator);
      TEST_ASSERT_FALSE(deepest.finish().owned);
      TEST_ASSERT_FALSE(inner.finish().owned);
      TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
      TEST_ASSERT_EQUAL_UINT32(0, fake.stops);
    }
    TEST_ASSERT_TRUE(fake.monitoring);
  }
  TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
}

void testFailedStartReleasesGate() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  fake.start_error = 42;
  Meter failed(coordinator);
  const auto& failed_result = failed.finish();
  TEST_ASSERT_EQUAL_INT32(42, failed_result.start_error);
  TEST_ASSERT_FALSE(failed_result.owned);
  TEST_ASSERT_EQUAL(MinimumScope::kUnavailable, failed_result.scope);
  TEST_ASSERT_EQUAL_UINT32(0, fake.stops);
  TEST_ASSERT_EQUAL(MinimumScope::kLifetime, coordinator.observe().scope);

  fake.start_error = 0;
  Meter good(coordinator);
  TEST_ASSERT_TRUE(good.finish().owned);
  TEST_ASSERT_EQUAL_UINT32(2, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
}

void testFinishAndDestructorAreIdempotent() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  uint32_t samples_after_finish = 0;
  {
    Meter meter(coordinator);
    const auto* first = &meter.finish();
    samples_after_finish = fake.samples;
    TEST_ASSERT_TRUE(first == &meter.finish());
    TEST_ASSERT_EQUAL_UINT32(samples_after_finish, fake.samples);
  }
  TEST_ASSERT_EQUAL_UINT32(samples_after_finish, fake.samples);
  TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
}

void testFailedStopQuarantinesGate() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  fake.stop_error = 73;
  {
    Meter owner(coordinator);
    const auto& result = owner.finish();
    TEST_ASSERT_TRUE(result.owned);
    TEST_ASSERT_EQUAL_INT32(73, result.stop_error);
    TEST_ASSERT_EQUAL(MinimumScope::kUnavailable, result.scope);
    TEST_ASSERT_EQUAL_UINT32(0, result.end.minimum_bytes);
  }
  const auto observed = coordinator.observe();
  TEST_ASSERT_EQUAL(MinimumScope::kUnavailable, observed.scope);
  TEST_ASSERT_EQUAL_UINT32(0, observed.point.minimum_bytes);
  fake.stop_error = 0;
  {
    Meter later(coordinator);
    TEST_ASSERT_FALSE(later.finish().owned);
  }
  TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
}

void testStableObservationScopesAndTimestamp() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  const auto lifetime = coordinator.observe();
  TEST_ASSERT_EQUAL(MinimumScope::kLifetime, lifetime.scope);
  TEST_ASSERT_EQUAL_UINT32(800, lifetime.point.minimum_bytes);
  TEST_ASSERT_EQUAL_INT64(0, lifetime.interval_start_ms);

  fake.point.monotonic_ms = (int64_t{1} << 40) + 123;
  Meter owner(coordinator);
  fake.point.minimum_bytes = 640;
  const auto interval = coordinator.observe();
  TEST_ASSERT_EQUAL(MinimumScope::kInterval, interval.scope);
  TEST_ASSERT_EQUAL_UINT32(640, interval.point.minimum_bytes);
  TEST_ASSERT_EQUAL_INT64((int64_t{1} << 40) + 123, interval.interval_start_ms);
  owner.finish();
  TEST_ASSERT_EQUAL(MinimumScope::kLifetime, coordinator.observe().scope);
}

void testGenerationChangeDuringObservationIsUnavailable() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  fake.coordinator = &coordinator;
  fake.on_sample = [](Fake& active) {
    // The token is idle both before and after this complete interval.
    // Comparing only the lifecycle state would mislabel the sampled minimum.
    Meter temporary(*active.coordinator);
    temporary.finish();
  };
  const auto observation = coordinator.observe();
  TEST_ASSERT_EQUAL(MinimumScope::kUnavailable, observation.scope);
  TEST_ASSERT_EQUAL_UINT32(0, observation.point.minimum_bytes);
  TEST_ASSERT_EQUAL_UINT32(1000, observation.point.free_bytes);
  TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
  TEST_ASSERT_EQUAL(MinimumScope::kLifetime, coordinator.observe().scope);
}

void testStartingStateIsUnavailableAndBusy() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  fake.coordinator = &coordinator;
  fake.on_start = [](Fake& active) {
    active.observed_scope = active.coordinator->observe().scope;
    Meter competing(*active.coordinator);
    TEST_ASSERT_FALSE(competing.finish().owned);
  };
  Meter owner(coordinator);
  TEST_ASSERT_EQUAL(MinimumScope::kUnavailable, fake.observed_scope);
  TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(0, fake.stops);
  TEST_ASSERT_EQUAL(MinimumScope::kInterval, coordinator.observe().scope);
  owner.finish();
}

void testOwnershipIsHeldThroughEndSample() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  fake.coordinator = &coordinator;
  Meter owner(coordinator);
  fake.point = {1200, 700, 650, 600};
  fake.on_sample = [](Fake& active) {
    active.observed_scope = active.coordinator->observe().scope;
    Meter competing(*active.coordinator);
    TEST_ASSERT_FALSE(competing.finish().owned);
  };
  const auto& result = owner.finish();
  TEST_ASSERT_EQUAL(MinimumScope::kUnavailable, fake.observed_scope);
  TEST_ASSERT_EQUAL(MinimumScope::kInterval, result.scope);
  TEST_ASSERT_EQUAL_UINT32(600, result.end.minimum_bytes);
  TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
}

void testOwnershipIsHeldThroughStop() {
  Fake fake;
  Coordinator coordinator(fake.backend());
  fake.coordinator = &coordinator;
  fake.on_stop = [](Fake& active) {
    active.observed_scope = active.coordinator->observe().scope;
    Meter competing(*active.coordinator);
    TEST_ASSERT_FALSE(competing.finish().owned);
  };
  Meter owner(coordinator);
  owner.finish();
  TEST_ASSERT_EQUAL(MinimumScope::kUnavailable, fake.observed_scope);
  TEST_ASSERT_EQUAL_UINT32(1, fake.starts);
  TEST_ASSERT_EQUAL_UINT32(1, fake.stops);
  TEST_ASSERT_EQUAL(MinimumScope::kLifetime, coordinator.observe().scope);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testLoserFinishesBeforeOwner);
  RUN_TEST(testOwnerFinishesBeforeLoserWithoutRetry);
  RUN_TEST(testNestedMetersKeepFirstOwner);
  RUN_TEST(testFailedStartReleasesGate);
  RUN_TEST(testFinishAndDestructorAreIdempotent);
  RUN_TEST(testFailedStopQuarantinesGate);
  RUN_TEST(testStableObservationScopesAndTimestamp);
  RUN_TEST(testGenerationChangeDuringObservationIsUnavailable);
  RUN_TEST(testStartingStateIsUnavailableAndBusy);
  RUN_TEST(testOwnershipIsHeldThroughEndSample);
  RUN_TEST(testOwnershipIsHeldThroughStop);
  return UNITY_END();
}
