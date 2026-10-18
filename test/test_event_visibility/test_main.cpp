// Visible canonical event loss: owner-required overflow warnings must survive
// unrelated successful responses and clear only when every affected list fits.
#include <unity.h>

#include "match_store.h"

namespace {
using namespace football;
MatchStore store;
constexpr Clock kClock{1792243800, 1000};
StoredMatch& row(uint32_t id) {
  StoredMatch match;
  match.id = id;
  match.competition = 1;
  match.home = store.team(1, match.provider, 40, "Own club");
  store.selectClub(1, match.provider, 40, kClock);
  match.phase = Phase::kLive;
  match.kickoff = kClock.utc - 1800;
  auto* result = store.upsert(match, kClock);
  TEST_ASSERT_NOT_NULL(result);
  return *result;
}
void add(unsigned count, uint32_t first) {
  for (unsigned i = 0; i < count; ++i) {
    StoredEvent event;
    event.identity = first + i;
    event.minute = i + 1;
    event.player.assign("Scorer");
    store.addEvent(event);
  }
}
void fill(uint32_t id, unsigned count, uint32_t first, bool complete = true) {
  auto& target = row(id);
  TEST_ASSERT_TRUE(store.beginEvents(target, false, kClock.utc));
  add(count, first);
  store.endEvents(complete);
}
}  // namespace
void setUp() { store.clear(); }
void tearDown() {}

void test_thirteenth_event_produces_visible_competition_warning() {
  fill(10, 13, 100);
  const auto* match = store.find(1, cfg::Provider::kOpenLigaDb, 10);
  TEST_ASSERT_TRUE(match->flags & kMatchEventsTruncated);
  TEST_ASSERT_TRUE(store.notices(1) & kNoticeEvents);
  TEST_ASSERT_EQUAL_UINT8(12, store.detailAt(match->detail).event_count);
}
void test_another_success_cannot_hide_existing_event_loss() {
  fill(10, 13, 100);
  fill(20, 1, 200);
  TEST_ASSERT_TRUE(store.notices(1) & kNoticeEvents);
  const auto* match = store.find(1, cfg::Provider::kOpenLigaDb, 10);
  TEST_ASSERT_TRUE(match->flags & kMatchEventsTruncated);
}
void test_complete_fitting_replacement_clears_the_warning() {
  fill(10, 13, 100);
  fill(20, 1, 200);
  fill(10, 1, 300);
  const auto* match = store.find(1, cfg::Provider::kOpenLigaDb, 10);
  TEST_ASSERT_FALSE(match->flags & kMatchEventsTruncated);
  TEST_ASSERT_FALSE(store.notices(1) & kNoticeEvents);
}
void test_incomplete_thirteen_event_merge_keeps_loss_visible() {
  fill(10, 10, 100);
  TEST_ASSERT_FALSE(store.notices(1) & kNoticeEvents);
  fill(10, 3, 200, false);
  const auto* match = store.find(1, cfg::Provider::kOpenLigaDb, 10);
  TEST_ASSERT_EQUAL_UINT8(12, store.detailAt(match->detail).event_count);
  TEST_ASSERT_TRUE(match->flags & kMatchEventsTruncated);
  TEST_ASSERT_TRUE(store.notices(1) & kNoticeEvents);
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_thirteenth_event_produces_visible_competition_warning);
  RUN_TEST(test_another_success_cannot_hide_existing_event_loss);
  RUN_TEST(test_complete_fitting_replacement_clears_the_warning);
  RUN_TEST(test_incomplete_thirteen_event_merge_keeps_loss_visible);
  return UNITY_END();
}
