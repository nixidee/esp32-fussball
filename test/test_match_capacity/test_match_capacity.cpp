// Host checks of the shared match list's capacity rule
// (football::matchSlot, football::appendMatch): a full list removes the
// other club's match farthest from now for an own match or a nearer one,
// never an own match, keeps event indices valid and marks the data. They
// prove only this rule, not which matches a provider delivers or the
// owner's device acceptance.

#include <unity.h>

#include "app_config.h"
#include "football_model.h"

namespace {

using football::appendMatch;
using football::Match;
using football::matchSlot;
using football::Snapshot;
using football::Source;

constexpr int64_t kNow = 1'760'000'000;
constexpr int64_t kHour = 3600;

Snapshot target, from;

Match fixture(unsigned id, int64_t offset, bool own = false) {
  Match m{};
  char text[8];
  snprintf(text, sizeof(text), "%u", id);
  m.id.assign(text);
  m.own = own;
  m.kickoff = {kNow + offset, Source::kProvider};
  return m;
}

// Fills the list with other clubs' matches 1..kMatches hours from now.
void fill() {
  target.reset();
  for (unsigned i = 0; i < cfg::kMatches; ++i)
    *matchSlot(target, fixture(i, (i + 1) * kHour), kNow) =
        fixture(i, (i + 1) * kHour);
}

void setUp() {
  target.reset();
  from.reset();
}
void tearDown() {}

void test_room_left_appends() {
  auto* slot = matchSlot(target, fixture(1, kHour), kNow);
  TEST_ASSERT_NOT_NULL(slot);
  TEST_ASSERT_EQUAL_UINT(1, target.match_count);
  TEST_ASSERT_FALSE(target.matches_truncated);
}

void test_nearer_match_replaces_farthest() {
  fill();
  auto* slot = matchSlot(target, fixture(99, -kHour / 2), kNow);
  TEST_ASSERT_NOT_NULL(slot);
  *slot = fixture(99, -kHour / 2);
  TEST_ASSERT_EQUAL_UINT(cfg::kMatches, target.match_count);
  TEST_ASSERT_TRUE(target.matches_truncated);
  for (unsigned i = 0; i < target.match_count; ++i)
    TEST_ASSERT_TRUE(target.matches[i].kickoff.value <
                     kNow + cfg::kMatches * kHour);
}

void test_farther_match_is_dropped() {
  fill();
  TEST_ASSERT_NULL(matchSlot(target, fixture(99, 100 * kHour), kNow));
  TEST_ASSERT_EQUAL_UINT(cfg::kMatches, target.match_count);
  TEST_ASSERT_TRUE(target.matches_truncated);
}

void test_own_match_always_fits_and_stays() {
  fill();
  target.matches[cfg::kMatches - 1].own = true;  // the farthest is own
  auto* slot = matchSlot(target, fixture(99, 200 * kHour, true), kNow);
  TEST_ASSERT_NOT_NULL(slot);
  *slot = fixture(99, 200 * kHour, true);
  unsigned own = 0;
  for (unsigned i = 0; i < target.match_count; ++i)
    own += target.matches[i].own;
  TEST_ASSERT_EQUAL_UINT(2, own);
}

void test_match_without_kickoff_goes_first() {
  fill();
  target.matches[3].kickoff = {};
  auto* slot = matchSlot(target, fixture(99, 50 * kHour), kNow);
  TEST_ASSERT_NOT_NULL(slot);
  *slot = fixture(99, 50 * kHour);
  for (unsigned i = 0; i < target.match_count; ++i)
    TEST_ASSERT_TRUE(target.matches[i].kickoff.valid());
}

void test_no_time_removes_nothing() {
  fill();
  TEST_ASSERT_NULL(matchSlot(target, fixture(99, 0), 0));
  TEST_ASSERT_TRUE(target.matches_truncated);
}

// The removed match's events leave the pool; the others keep theirs.
void test_removed_events_leave_the_pool() {
  fill();
  auto add = [](Match& m, uint32_t identity) {
    target.events[target.event_count].identity = identity;
    m.events[m.event_count++] = target.event_count++;
  };
  auto& near = target.matches[0];
  auto& far = target.matches[cfg::kMatches - 1];
  add(far, 1);
  add(near, 2);
  add(far, 3);
  add(near, 4);
  Match candidate = fixture(99, 0);
  candidate.events[candidate.event_count++] = 0;
  from.events[0].identity = 5;
  from.event_count = 1;
  TEST_ASSERT_TRUE(appendMatch(target, from, candidate, kNow));
  TEST_ASSERT_EQUAL_UINT(3, target.event_count);
  const auto& kept = target.matches[0];
  TEST_ASSERT_EQUAL_UINT(2, kept.event_count);
  TEST_ASSERT_EQUAL_UINT32(2, target.events[kept.events[0]].identity);
  TEST_ASSERT_EQUAL_UINT32(4, target.events[kept.events[1]].identity);
  const auto& added = target.matches[target.match_count - 1];
  TEST_ASSERT_EQUAL_STRING("99", added.id.data);
  TEST_ASSERT_EQUAL_UINT32(5, target.events[added.events[0]].identity);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_room_left_appends);
  RUN_TEST(test_nearer_match_replaces_farthest);
  RUN_TEST(test_farther_match_is_dropped);
  RUN_TEST(test_own_match_always_fits_and_stays);
  RUN_TEST(test_match_without_kickoff_goes_first);
  RUN_TEST(test_no_time_removes_nothing);
  RUN_TEST(test_removed_events_leave_the_pool);
  return UNITY_END();
}
