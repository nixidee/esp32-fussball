// Host checks of the fixtures a competition keeps from a provider response
// (football::keepFixture): the current round, the own club's fixtures within
// kFixtureHorizonDays and other clubs' fixtures within
// kConferenceHorizonHours, also outside the current round (catch-up
// fixtures). They prove only this rule, not which fixtures OpenLigaDB
// delivers or the owner's device acceptance.

#include <unity.h>

#include "app_config.h"
#include "football_model.h"

namespace {

using football::FixtureFilter;
using football::keepFixture;
using football::Match;
using football::Source;

constexpr int64_t kNow = 1'760'000'000;
constexpr int64_t kHour = 3600, kDay = 86400;

Match fixture(const char* home, const char* away, uint16_t round,
              int64_t offset) {
  Match m{};
  m.id.assign("1");
  m.home.id.assign(home);
  m.away.id.assign(away);
  if (round) m.round = {round, Source::kProvider};
  m.kickoff = {kNow + offset, Source::kProvider};
  return m;
}

void test_current_round_kept_at_any_distance() {
  const FixtureFilter filter{"7", 5};
  TEST_ASSERT_TRUE(keepFixture(fixture("1", "2", 5, 10 * kDay), kNow, filter));
  TEST_ASSERT_TRUE(keepFixture(fixture("1", "2", 5, -10 * kDay), kNow, filter));
}

// W86: another club's catch-up fixture outside the current round.
void test_other_club_outside_round_within_horizon() {
  const FixtureFilter filter{"7", 5};
  const int64_t edge = cfg::kConferenceHorizonHours * kHour;
  TEST_ASSERT_TRUE(keepFixture(fixture("1", "2", 3, 2 * kHour), kNow, filter));
  TEST_ASSERT_TRUE(keepFixture(fixture("1", "2", 3, -edge), kNow, filter));
  TEST_ASSERT_TRUE(keepFixture(fixture("1", "2", 6, edge), kNow, filter));
  TEST_ASSERT_FALSE(keepFixture(fixture("1", "2", 3, -edge - 1), kNow, filter));
  TEST_ASSERT_FALSE(keepFixture(fixture("1", "2", 6, edge + 1), kNow, filter));
}

void test_own_club_within_fixture_horizon() {
  const FixtureFilter filter{"7", 5};
  const int64_t edge = cfg::kFixtureHorizonDays * kDay;
  TEST_ASSERT_TRUE(keepFixture(fixture("7", "2", 3, edge), kNow, filter));
  TEST_ASSERT_TRUE(keepFixture(fixture("2", "7", 9, -edge), kNow, filter));
  TEST_ASSERT_FALSE(keepFixture(fixture("7", "2", 3, edge + 1), kNow, filter));
}

void test_provider_without_rounds_uses_same_horizons() {
  const FixtureFilter filter{"7", 0};
  const int64_t edge = cfg::kConferenceHorizonHours * kHour;
  TEST_ASSERT_TRUE(keepFixture(fixture("1", "2", 0, edge), kNow, filter));
  TEST_ASSERT_FALSE(keepFixture(fixture("1", "2", 0, edge + 1), kNow, filter));
}

void test_missing_id_or_kickoff() {
  const FixtureFilter filter{"7", 5};
  auto nameless = fixture("1", "2", 5, 0);
  nameless.id = {};
  TEST_ASSERT_FALSE(keepFixture(nameless, kNow, filter));
  auto own = fixture("7", "2", 3, 0);
  own.kickoff = {};
  TEST_ASSERT_TRUE(keepFixture(own, kNow, filter));
  auto other = fixture("1", "2", 3, 0);
  other.kickoff = {};
  TEST_ASSERT_FALSE(keepFixture(other, kNow, filter));
  TEST_ASSERT_TRUE(keepFixture(fixture("1", "2", 3, 30 * kDay), 0, filter));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_current_round_kept_at_any_distance);
  RUN_TEST(test_other_club_outside_round_within_horizon);
  RUN_TEST(test_own_club_within_fixture_horizon);
  RUN_TEST(test_provider_without_rounds_uses_same_horizons);
  RUN_TEST(test_missing_id_or_kickoff);
  return UNITY_END();
}
