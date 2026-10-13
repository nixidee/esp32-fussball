// Host checks of the partial publication after a failed competition fetch:
// the failed competition keeps its earlier matches, events, table and fetch
// time, the others carry their new data. They prove only the model helpers;
// the provider worker loop itself runs only on the device, and none of this
// replaces the owner's device acceptance.

#include <unity.h>

#include "app_config.h"
#include "football_model.h"

namespace {

using football::carryRoute;
using football::EventType;
using football::Match;
using football::oldestFetch;
using football::Phase;
using football::Snapshot;
using football::Source;

Snapshot published, candidate;

Match& addMatch(Snapshot& snapshot, uint8_t route, const char* id) {
  auto& match = snapshot.matches[snapshot.match_count++];
  match = {};
  match.route = route;
  match.id.assign(id);
  return match;
}

void addEvent(Snapshot& snapshot, Match& match, uint32_t identity,
              bool removed) {
  auto& event = snapshot.events[snapshot.event_count];
  event = {};
  event.type = EventType::kGoal;
  event.identity = identity;
  event.removed = removed;
  match.events[match.event_count++] = snapshot.event_count++;
}


// Route 1 succeeded with new data, route 0 failed: route 0 keeps its match,
// its live event, the table and its earlier fetch time.
void test_failed_route_keeps_earlier_data() {
  addMatch(candidate, 1, "b1-new");
  candidate.route_fetched_ms[1] = 5000;
  TEST_ASSERT_TRUE(carryRoute(candidate, published, 0, true, 0));
  TEST_ASSERT_EQUAL_UINT(2, candidate.match_count);
  const auto& carried = candidate.matches[1];
  TEST_ASSERT_EQUAL_STRING("a1", carried.id.data);
  TEST_ASSERT_EQUAL_INT(1, carried.principal.home.value);
  TEST_ASSERT_EQUAL_UINT(1, carried.event_count);
  TEST_ASSERT_EQUAL_UINT(11, candidate.events[carried.events[0]].identity);
  TEST_ASSERT_EQUAL_UINT(2, candidate.table_count);
  TEST_ASSERT_EQUAL_INT(7, candidate.table[0].points.value);
  TEST_ASSERT_EQUAL_INT64(1000, candidate.route_fetched_ms[0]);
  TEST_ASSERT_EQUAL_INT64(5000, candidate.route_fetched_ms[1]);
  TEST_ASSERT_EQUAL_STRING("b1-new", candidate.matches[0].id.data);
}

// Only the failed route is copied; the table only when asked for and when
// the new data has none.
void test_only_the_failed_route_is_copied() {
  candidate.table_count = 1;
  candidate.table[0].points = {3, Source::kOpenLigaDb};
  TEST_ASSERT_TRUE(carryRoute(candidate, published, 1, true, 0));
  TEST_ASSERT_EQUAL_UINT(1, candidate.match_count);
  TEST_ASSERT_EQUAL_STRING("b1", candidate.matches[0].id.data);
  TEST_ASSERT_EQUAL_UINT(1, candidate.table_count);
  TEST_ASSERT_EQUAL_INT(3, candidate.table[0].points.value);
  candidate.reset();
  TEST_ASSERT_TRUE(carryRoute(candidate, published, 1, false, 0));
  TEST_ASSERT_EQUAL_UINT(0, candidate.table_count);
  TEST_ASSERT_EQUAL_INT64(2000, candidate.route_fetched_ms[1]);
  TEST_ASSERT_EQUAL_INT64(0, candidate.route_fetched_ms[0]);
}

// A carried match without a provider status is estimated again at 'utc'.
void test_carried_match_is_estimated_again() {
  published.matches[1].kickoff = {100000, Source::kOpenLigaDb};
  TEST_ASSERT_TRUE(carryRoute(candidate, published, 1, false, 100000 + 600));
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(Phase::kLive),
                         static_cast<unsigned>(candidate.matches[0].phase));
  TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(Source::kEstimated),
                         static_cast<unsigned>(candidate.matches[0].phase_source));
}

// A full match list refuses the copy.
void test_full_match_list_fails() {
  candidate.match_count = static_cast<uint8_t>(cfg::kMatches);
  TEST_ASSERT_FALSE(carryRoute(candidate, published, 0, false, 0));
}

// The status band's "as of" time is the oldest route fetch time.
void test_oldest_fetch() {
  TEST_ASSERT_EQUAL_INT64(1000, oldestFetch(published));
  candidate.fetched_ms = 4000;
  TEST_ASSERT_EQUAL_INT64(4000, oldestFetch(candidate));
}

}  // namespace

// Published data of two competitions: route 0 with a table and two goals
// (one of them a removal marker), route 1 with one match.
void setUp() {
  published.reset();
  candidate.reset();
  auto& a = addMatch(published, 0, "a1");
  a.principal = {{1, Source::kOpenLigaDb}, {0, Source::kOpenLigaDb}};
  addEvent(published, a, 11, false);
  addEvent(published, a, 12, true);
  addMatch(published, 1, "b1");
  published.table_count = 2;
  published.table[0].points = {7, Source::kOpenLigaDb};
  published.route_fetched_ms = {1000, 2000, 0};
  published.fetched_ms = 2000;
}

void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_failed_route_keeps_earlier_data);
  RUN_TEST(test_only_the_failed_route_is_copied);
  RUN_TEST(test_carried_match_is_estimated_again);
  RUN_TEST(test_full_match_list_fails);
  RUN_TEST(test_oldest_fetch);
  return UNITY_END();
}
