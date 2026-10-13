// Host checks of the Konferenz membership and page layout (docs/UI.md,
// "Konferenz"). The fixtures model one Bundesliga-style round from Friday to
// Sunday plus a second competition; they prove only the implementation of the
// agreed rules, not the owner's device acceptance.

#include <unity.h>

#include <cstdint>
#include <initializer_list>

#include "app_config.h"
#include "conference.h"
#include "football_model.h"

namespace {

using football::conferenceMembers;
using football::conferencePage;
using football::ConferenceList;
using football::ConferencePage;
using football::ConferenceRequest;
using football::ConferenceRules;
using football::Match;
using football::Phase;
using football::Snapshot;
using football::Source;

constexpr int64_t kMin = 60;
constexpr int64_t kHour = 3600;
constexpr int64_t kFriday = 1792108800;  // 2026-10-16 00:00 UTC, a Friday
constexpr int64_t kSaturday = kFriday + 24 * kHour;
constexpr int64_t kSunday = kSaturday + 24 * kHour;
constexpr ConferenceRules kRules{30, 30, false};
constexpr ConferenceRules kWholeMatchday{30, 30, true};

// Snapshot indices of the fixture.
enum : uint8_t {
  kFri,        // league, Friday 18:30
  kSat1,       // league, Saturday 13:30 (five matches)
  kOwn,        // league, Saturday 13:30, own club
  kSat3,
  kSat4,
  kSat5,
  kSatLate,    // league, Saturday 16:30
  kSun1,       // league, Sunday 13:30
  kSun2,       // league, Sunday 15:30
  kSun3,       // league, Sunday 17:30
  kCupSat,     // second competition, Saturday 13:30
  kCupEarly,   // second competition, Saturday 11:00
  kCupSun,     // second competition, Sunday 11:00
  kPostponed,  // league, Saturday 13:30, postponed
  kCount
};

Snapshot data;

Match& add(uint8_t route, uint16_t round, int64_t kickoff, bool own = false) {
  auto& m = data.matches[data.match_count++];
  m = Match{};
  m.route = route;
  m.round = {round, Source::kProvider};
  m.kickoff = {kickoff, Source::kProvider};
  m.phase = Phase::kScheduled;
  m.phase_source = Source::kProvider;
  m.own = own;
  return m;
}

void finish(uint8_t index, int64_t at) {
  auto& m = data.matches[index];
  m.phase = Phase::kFinished;
  m.finished_at = {at, Source::kEstimated};
}

void live(uint8_t index, int16_t home, int16_t away, int64_t changed_ms,
          Phase phase = Phase::kLive) {
  auto& m = data.matches[index];
  m.phase = phase;
  m.principal.home = {home, Source::kProvider};
  m.principal.away = {away, Source::kProvider};
  m.score_changed_ms = changed_ms;
}

void expectList(const ConferenceList& list, std::initializer_list<int> want) {
  TEST_ASSERT_EQUAL_UINT(want.size(), list.count);
  unsigned i = 0;
  for (int index : want) TEST_ASSERT_EQUAL_INT(index, list.index[i++]);
}

void expectRows(const ConferencePage& page, std::initializer_list<int> want) {
  TEST_ASSERT_EQUAL_UINT(want.size(), page.row_count);
  unsigned i = 0;
  for (int index : want) TEST_ASSERT_EQUAL_INT(index, page.rows[i++]);
}

ConferenceList at(int64_t utc, const ConferenceRules& rules = kRules) {
  return conferenceMembers(data, utc, rules);
}

}  // namespace

void setUp() {
  data.reset();
  add(0, 7, kFriday + 18 * kHour + 30 * kMin);
  for (int i = 0; i < 5; ++i)
    add(0, 7, kSaturday + 13 * kHour + 30 * kMin, i == 1);
  add(0, 7, kSaturday + 16 * kHour + 30 * kMin);
  add(0, 7, kSunday + 13 * kHour + 30 * kMin);
  add(0, 7, kSunday + 15 * kHour + 30 * kMin);
  add(0, 7, kSunday + 17 * kHour + 30 * kMin);
  add(1, 2, kSaturday + 13 * kHour + 30 * kMin);
  add(1, 2, kSaturday + 11 * kHour);
  add(1, 2, kSunday + 11 * kHour);
  add(0, 7, kSaturday + 13 * kHour + 30 * kMin).phase = Phase::kPostponed;
  TEST_ASSERT_EQUAL_UINT(kCount, data.match_count);
}

void tearDown() {}

void test_block_opens_window_before_kickoff() {
  expectList(at(kFriday + 17 * kHour + 59 * kMin), {});
  expectList(at(kFriday + 18 * kHour), {kFri});
}

void test_last_block_closes_window_after_final_whistle() {
  finish(kFri, kFriday + 20 * kHour + 25 * kMin);
  expectList(at(kFriday + 20 * kHour + 54 * kMin), {kFri});
  expectList(at(kFriday + 20 * kHour + 55 * kMin), {});
}

void test_unconfirmed_match_ends_after_expected_duration() {
  // No final whistle and no confirmed running phase: kickoff + 115 + after.
  const int64_t end = kFriday + 18 * kHour + 30 * kMin +
                      (cfg::kExpectedMatchMinutes + 30) * kMin;
  expectList(at(end - 1), {kFri});
  expectList(at(end), {});
}

void test_confirmed_running_match_extends_to_maximum() {
  live(kFri, 1, 1, 0, Phase::kExtraTime);
  const int64_t kickoff = kFriday + 18 * kHour + 30 * kMin;
  expectList(at(kickoff + 200 * kMin), {kFri});
  expectList(at(kickoff + (cfg::kUnknownMatchEndMinutes + 30) * kMin), {});
}

void test_blocks_span_competitions_and_skip_postponed() {
  // The early cup match ends at the start of the 13:30 block (OQ-159/160).
  finish(kCupEarly, kSaturday + 12 * kHour + 52 * kMin);
  expectList(at(kSaturday + 12 * kHour + 59 * kMin), {kCupEarly});
  expectList(at(kSaturday + 13 * kHour),
             {kSat1, kOwn, kSat3, kSat4, kSat5, kCupSat});
}

void test_finished_matches_stay_until_block_ends() {
  finish(kSat1, kSaturday + 15 * kHour + 20 * kMin);
  for (uint8_t i : {kOwn, kSat3, kSat4, kSat5, kCupSat}) live(i, 0, 0, 0);
  expectList(at(kSaturday + 15 * kHour + 55 * kMin),
             {kSat1, kOwn, kSat3, kSat4, kSat5, kCupSat});
}

void test_overrun_stays_while_next_block_runs() {
  for (uint8_t i : {kSat1, kOwn, kSat4, kSat5, kCupSat})
    finish(i, kSaturday + 15 * kHour + 25 * kMin);
  live(kSat3, 2, 2, 0, Phase::kExtraTime);
  // The next block opens at 16:00; only the overrun stays with it.
  expectList(at(kSaturday + 15 * kHour + 59 * kMin),
             {kSat1, kOwn, kSat3, kSat4, kSat5, kCupSat});
  expectList(at(kSaturday + 16 * kHour + 10 * kMin), {kSat3, kSatLate});
  finish(kSat3, kSaturday + 16 * kHour + 20 * kMin);
  expectList(at(kSaturday + 16 * kHour + 49 * kMin), {kSat3, kSatLate});
  expectList(at(kSaturday + 16 * kHour + 50 * kMin), {kSatLate});
}

void test_whole_matchday_adds_own_match_of_same_round() {
  finish(kOwn, kSaturday + 15 * kHour + 22 * kMin);
  expectList(at(kSunday + 13 * kHour), {kSun1});
  expectList(at(kSunday + 13 * kHour, kWholeMatchday), {kOwn, kSun1});
}

void test_whole_matchday_waits_for_own_kickoff() {
  expectList(at(kFriday + 18 * kHour, kWholeMatchday), {kFri});
}

void test_whole_matchday_ignores_other_competitions() {
  finish(kOwn, kSaturday + 15 * kHour + 22 * kMin);
  expectList(at(kSunday + 11 * kHour, kWholeMatchday), {kCupSun});
}

ConferenceList saturdayBlock() {
  return at(kSaturday + 14 * kHour);
}

void test_all_pages_pins_own_match_on_every_page() {
  const auto list = saturdayBlock();
  ConferenceRequest request{cfg::ConferenceMode::kAllPages, true, false, 3, 0};
  auto page = conferencePage(data, list, request);
  TEST_ASSERT_EQUAL_UINT(3, page.pages);
  expectRows(page, {kOwn, kSat1, kSat3});
  request.page = 1;
  expectRows(conferencePage(data, list, request), {kOwn, kSat4, kSat5});
  request.page = 2;
  expectRows(conferencePage(data, list, request), {kOwn, kCupSat});
}

void test_automatic_paging_wraps_and_manual_paging_clamps() {
  const auto list = saturdayBlock();
  ConferenceRequest request{cfg::ConferenceMode::kAllPages, true, false, 3, 4};
  auto page = conferencePage(data, list, request);
  TEST_ASSERT_EQUAL_UINT(1, page.page);
  request.manual = true;
  request.page = 9;
  page = conferencePage(data, list, request);
  TEST_ASSERT_EQUAL_UINT(2, page.page);
  expectRows(page, {kOwn, kCupSat});
}

void test_pin_off_keeps_own_match_in_kickoff_order() {
  const auto list = saturdayBlock();
  ConferenceRequest request{cfg::ConferenceMode::kAllPages, false, false, 3, 0};
  const auto page = conferencePage(data, list, request);
  TEST_ASSERT_EQUAL_UINT(2, page.pages);
  expectRows(page, {kSat1, kOwn, kSat3});
}

void test_one_page_ranks_by_activity() {
  live(kOwn, 0, 0, 0);
  live(kSat1, 0, 0, 0);
  live(kSat3, 1, 0, 5000);
  live(kSat4, 2, 1, 9000);
  live(kSat5, 1, 1, 0, Phase::kHalfTime);  // goals before a restart
  const auto list = saturdayBlock();
  ConferenceRequest request{cfg::ConferenceMode::kOnePage, true, false, 5, 0};
  auto page = conferencePage(data, list, request);
  TEST_ASSERT_EQUAL_UINT(1, page.pages);
  expectRows(page, {kOwn, kSat4, kSat3, kSat5, kSat1});
  // Goalless running matches with the same kickoff keep snapshot order.
  request.pin_own = false;
  expectRows(conferencePage(data, list, request),
             {kSat4, kSat3, kSat5, kSat1, kOwn});
}

void test_one_page_puts_finished_before_not_started() {
  finish(kSat1, kSaturday + 15 * kHour + 20 * kMin);
  live(kSat3, 0, 0, 0);
  const auto list = saturdayBlock();
  ConferenceRequest request{cfg::ConferenceMode::kOnePage, false, false, 3, 0};
  expectRows(conferencePage(data, list, request), {kSat3, kSat1, kOwn});
}

void test_one_page_manual_paging_uses_kickoff_order() {
  live(kSat4, 2, 1, 9000);
  const auto list = saturdayBlock();
  ConferenceRequest request{cfg::ConferenceMode::kOnePage, true, true, 3, 1};
  const auto page = conferencePage(data, list, request);
  TEST_ASSERT_EQUAL_UINT(3, page.pages);
  expectRows(page, {kOwn, kSat4, kSat5});
}

void test_single_row_with_pin_shows_only_own_match() {
  const auto list = saturdayBlock();
  ConferenceRequest request{cfg::ConferenceMode::kAllPages, true, false, 1, 3};
  const auto page = conferencePage(data, list, request);
  TEST_ASSERT_EQUAL_UINT(1, page.pages);
  expectRows(page, {kOwn});
}

void test_empty_list_has_no_page() {
  const auto page =
      conferencePage(data, at(kFriday), ConferenceRequest{});
  TEST_ASSERT_EQUAL_UINT(0, page.pages);
  TEST_ASSERT_EQUAL_UINT(0, page.row_count);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_block_opens_window_before_kickoff);
  RUN_TEST(test_last_block_closes_window_after_final_whistle);
  RUN_TEST(test_unconfirmed_match_ends_after_expected_duration);
  RUN_TEST(test_confirmed_running_match_extends_to_maximum);
  RUN_TEST(test_blocks_span_competitions_and_skip_postponed);
  RUN_TEST(test_finished_matches_stay_until_block_ends);
  RUN_TEST(test_overrun_stays_while_next_block_runs);
  RUN_TEST(test_whole_matchday_adds_own_match_of_same_round);
  RUN_TEST(test_whole_matchday_waits_for_own_kickoff);
  RUN_TEST(test_whole_matchday_ignores_other_competitions);
  RUN_TEST(test_all_pages_pins_own_match_on_every_page);
  RUN_TEST(test_automatic_paging_wraps_and_manual_paging_clamps);
  RUN_TEST(test_pin_off_keeps_own_match_in_kickoff_order);
  RUN_TEST(test_one_page_ranks_by_activity);
  RUN_TEST(test_one_page_puts_finished_before_not_started);
  RUN_TEST(test_one_page_manual_paging_uses_kickoff_order);
  RUN_TEST(test_single_row_with_pin_shows_only_own_match);
  RUN_TEST(test_empty_list_has_no_page);
  return UNITY_END();
}
