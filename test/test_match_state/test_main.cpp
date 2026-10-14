// Host checks of the match state derived from provider status strings.
// They prove only this implementation, not the owner's device acceptance.

#include <unity.h>

#include "football_model.h"

namespace {

using football::Phase;
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

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_football_data_statuses);
  RUN_TEST(test_unknown_status_stays_unknown);
  return UNITY_END();
}
