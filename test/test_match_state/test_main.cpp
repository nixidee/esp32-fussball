// Host checks of the match state derived from provider status strings and
// from the kickoff alone. They prove only this implementation, not the
// owner's device acceptance.

#include <unity.h>

#include "football_model.h"

namespace {

using football::Match;
using football::Phase;
using football::Source;
using football::statusPhase;

void expectPhase(Phase expected, const char* status) {
  TEST_ASSERT_EQUAL_UINT_MESSAGE(static_cast<unsigned>(expected),
                                 static_cast<unsigned>(statusPhase(status)),
                                 status);
}

// Every value of the football-data.org v4 match status lookup table.
void test_football_data_statuses() {
  expectPhase(Phase::kScheduled, "SCHEDULED");
  expectPhase(Phase::kScheduled, "TIMED");
  expectPhase(Phase::kLive, "IN_PLAY");
  expectPhase(Phase::kHalfTime, "PAUSED");
  expectPhase(Phase::kExtraTime, "EXTRA_TIME");
  expectPhase(Phase::kPenalties, "PENALTY_SHOOTOUT");
  expectPhase(Phase::kFinished, "FINISHED");
  expectPhase(Phase::kPostponed, "SUSPENDED");
  expectPhase(Phase::kPostponed, "POSTPONED");
  expectPhase(Phase::kCancelled, "CANCELLED");
  expectPhase(Phase::kAbandoned, "AWARDED");
}

void test_unknown_status_stays_unknown() {
  expectPhase(Phase::kUnknown, "");
  expectPhase(Phase::kUnknown, "in_play");
  expectPhase(Phase::kUnknown, "SOMETHING_NEW");
}

constexpr int64_t kKickoff = 1'760'000'000;

Match estimated(int64_t minutes) {
  Match m;
  m.kickoff = {kKickoff, Source::kProvider};
  football::estimate(m, kKickoff + minutes * 60);
  return m;
}

void test_estimate_first_half() {
  const auto m = estimated(44);
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(Phase::kLive),
                         static_cast<unsigned>(m.phase));
  TEST_ASSERT_EQUAL_INT16(44, m.minute.value);
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(Source::kEstimated),
                         static_cast<unsigned>(m.minute.source));
}

void test_estimate_half_time() {
  const auto m = estimated(50);
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(Phase::kHalfTime),
                         static_cast<unsigned>(m.phase));
  TEST_ASSERT_FALSE(m.minute.valid());
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(Source::kEstimated),
                         static_cast<unsigned>(m.phase_source));
}

void test_estimate_second_half() {
  const auto m = estimated(61);
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(Phase::kLive),
                         static_cast<unsigned>(m.phase));
  TEST_ASSERT_EQUAL_INT16(46, m.minute.value);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_football_data_statuses);
  RUN_TEST(test_unknown_status_stays_unknown);
  RUN_TEST(test_estimate_first_half);
  RUN_TEST(test_estimate_half_time);
  RUN_TEST(test_estimate_second_half);
  return UNITY_END();
}
