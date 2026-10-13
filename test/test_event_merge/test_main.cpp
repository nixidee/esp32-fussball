// Host checks of the event merge of a fixture pair (owner decision on goal
// deduplication: only scored goals decide whether the fallback's goals are
// dropped; missed penalties are deduplicated separately). They prove only the
// implementation of that rule, not the owner's device acceptance.

#include <unity.h>

#include <initializer_list>

#include "app_config.h"
#include "football_model.h"

namespace {

using football::EventType;
using football::Match;
using football::mergeEvents;
using football::Snapshot;

Snapshot primary, secondary;

void addEvent(Snapshot& snapshot, Match& match, EventType type,
              int16_t minute) {
  auto& event = snapshot.events[snapshot.event_count];
  event = {};
  event.type = type;
  event.minute.value = minute;
  match.events[match.event_count++] = snapshot.event_count++;
}

Match& fresh(Snapshot& snapshot) {
  snapshot.reset();
  auto& match = snapshot.matches[snapshot.match_count++];
  match = Match{};
  return match;
}

// Types of the merged events of 'match', in order.
void expectTypes(const Match& match, std::initializer_list<EventType> types) {
  TEST_ASSERT_EQUAL_UINT(types.size(), match.event_count);
  unsigned i = 0;
  for (const auto type : types)
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(type),
                           static_cast<unsigned>(
                               primary.events[match.events[i++]].type));
}

void test_primary_goal_drops_secondary_goals() {
  auto& a = fresh(primary);
  auto& b = fresh(secondary);
  addEvent(primary, a, EventType::kGoal, 10);
  addEvent(secondary, b, EventType::kGoal, 10);
  addEvent(secondary, b, EventType::kOwnGoal, 20);
  addEvent(secondary, b, EventType::kPenaltyGoal, 30);
  addEvent(secondary, b, EventType::kYellow, 40);
  mergeEvents(primary, a, secondary, b, false);
  expectTypes(a, {EventType::kGoal, EventType::kYellow});
}

void test_primary_missed_penalty_keeps_secondary_goals() {
  auto& a = fresh(primary);
  auto& b = fresh(secondary);
  addEvent(primary, a, EventType::kPenaltyMissed, 5);
  addEvent(secondary, b, EventType::kGoal, 10);
  addEvent(secondary, b, EventType::kPenaltyGoal, 30);
  mergeEvents(primary, a, secondary, b, false);
  expectTypes(a, {EventType::kPenaltyMissed, EventType::kGoal,
                  EventType::kPenaltyGoal});
}

void test_primary_goal_keeps_secondary_missed_penalty() {
  auto& a = fresh(primary);
  auto& b = fresh(secondary);
  addEvent(primary, a, EventType::kGoal, 10);
  addEvent(secondary, b, EventType::kPenaltyMissed, 50);
  mergeEvents(primary, a, secondary, b, false);
  expectTypes(a, {EventType::kGoal, EventType::kPenaltyMissed});
}

void test_primary_missed_penalty_drops_secondary_missed_penalty() {
  auto& a = fresh(primary);
  auto& b = fresh(secondary);
  addEvent(primary, a, EventType::kPenaltyMissed, 50);
  addEvent(secondary, b, EventType::kPenaltyMissed, 50);
  addEvent(secondary, b, EventType::kRed, 60);
  mergeEvents(primary, a, secondary, b, false);
  expectTypes(a, {EventType::kPenaltyMissed, EventType::kRed});
}

void test_without_primary_events_everything_is_added() {
  auto& a = fresh(primary);
  auto& b = fresh(secondary);
  addEvent(secondary, b, EventType::kGoal, 10);
  addEvent(secondary, b, EventType::kPenaltyMissed, 50);
  mergeEvents(primary, a, secondary, b, false);
  expectTypes(a, {EventType::kGoal, EventType::kPenaltyMissed});
}

void test_swapped_pair_exchanges_score_after() {
  auto& a = fresh(primary);
  auto& b = fresh(secondary);
  addEvent(secondary, b, EventType::kGoal, 10);
  secondary.events[0].after.home.value = 1;
  secondary.events[0].after.away.value = 0;
  mergeEvents(primary, a, secondary, b, true);
  TEST_ASSERT_EQUAL_INT(0, primary.events[a.events[0]].after.home.value);
  TEST_ASSERT_EQUAL_INT(1, primary.events[a.events[0]].after.away.value);
}

void test_full_event_list_marks_truncation() {
  auto& a = fresh(primary);
  auto& b = fresh(secondary);
  for (unsigned i = 0; i < cfg::kMatchEvents; ++i)
    addEvent(primary, a, EventType::kYellow, static_cast<int16_t>(i));
  addEvent(secondary, b, EventType::kRed, 80);
  mergeEvents(primary, a, secondary, b, false);
  TEST_ASSERT_EQUAL_UINT(cfg::kMatchEvents, a.event_count);
  TEST_ASSERT_TRUE(a.events_truncated);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_primary_goal_drops_secondary_goals);
  RUN_TEST(test_primary_missed_penalty_keeps_secondary_goals);
  RUN_TEST(test_primary_goal_keeps_secondary_missed_penalty);
  RUN_TEST(test_primary_missed_penalty_drops_secondary_missed_penalty);
  RUN_TEST(test_without_primary_events_everything_is_added);
  RUN_TEST(test_swapped_pair_exchanges_score_after);
  RUN_TEST(test_full_event_list_marks_truncation);
  return UNITY_END();
}
