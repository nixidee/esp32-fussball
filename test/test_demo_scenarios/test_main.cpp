// Canonical Demo acceptance: deterministic test time and actual production
// selectors/storage, without a renderer or a simulated second football model.
#include <ArduinoJson.h>
#include <unity.h>

#include <cstring>

#include "../provider_adapter_support.h"
#include "../source_support.h"
#include "../test_match_store/recorded.h"
#include "demo_scenarios.h"
#include "screen_selection.h"
#include "store_views.h"

namespace {
using namespace football;
namespace demo = football::demo;
namespace selection = football::selection;
namespace policy = settings::screen_policy;
MatchStore store, recorded_store;
settings::CompetitionRoutes routes;
demo::Bindings bindings;
policy::Policy options;
Clock apply(demo::Scenario scenario, uint8_t step) {
  bindings = demo::configure(routes, scenario, false);
  return demo::apply(store, routes, bindings, scenario, step, 5000);
}
const StoredMatch& find(uint32_t id,
                        uint8_t competition = demo::kFirstCompetition) {
  const auto* row = store.find(store.currentSource(competition), id);
  TEST_ASSERT_NOT_NULL(row);
  return *row;
}
selection::Input input(Clock clock, bool break_proven = false) {
  return {options, routes, store, clock, {}, break_proven};
}
}  // namespace
void setUp() {
  store.clear();
  recorded_store.clear();
  routes.clear();
  options = {};
}
void tearDown() {}

void test_own_phase_focus_and_following_day_result_use_real_selector() {
  auto clock = apply(demo::Scenario::kOwn, 0);
  auto selected = selection::select(input(clock));
  TEST_ASSERT_EQUAL(policy::Phase::kOwn, selected.phase);
  TEST_ASSERT_EQUAL_UINT32(401884775, selected.focus.id);
  clock = apply(demo::Scenario::kOwn, 7);
  TEST_ASSERT_EQUAL(policy::Phase::kOwn, selection::select(input(clock)).phase);
  clock = apply(demo::Scenario::kOwn, 8);
  const auto facts = input(clock);
  selected = selection::select(facts);
  TEST_ASSERT_NOT_EQUAL(policy::Phase::kOwn, selected.phase);
  const auto single = selection::single(facts, selected, routes.entries[0]);
  TEST_ASSERT_EQUAL(selection::Content::kResult, single.content);
  TEST_ASSERT_EQUAL_UINT32(401884775, single.match.id);
}

void test_concurrent_owned_competitions_use_order_without_faking_ownership() {
  settings::CompetitionRoute saved;
  saved.id = demo::kFirstCompetition;
  saved.incarnation = 1;
  TEST_ASSERT_TRUE(routes.entries.push(saved));
  saved.id = demo::kSecondCompetition;
  saved.incarnation = 2;
  TEST_ASSERT_TRUE(routes.entries.push(saved));
  bindings = demo::configure(routes, demo::Scenario::kOwn, true);
  const auto clock =
      demo::apply(store, routes, bindings, demo::Scenario::kOwn, 4, 5000);
  TEST_ASSERT_TRUE(store.isOwn(find(401884775)));
  TEST_ASSERT_TRUE(store.isOwn(find(401884777, demo::kSecondCompetition)));
  TEST_ASSERT_EQUAL_UINT8(demo::kFirstCompetition,
                          selection::select(input(clock)).competition);
  routes.entries[0].club.own_special = false;
  routes.entries[1].club.own_special = false;
  TEST_ASSERT_EQUAL(policy::Phase::kOtherGames,
                    selection::select(input(clock)).phase);
  TEST_ASSERT_TRUE(store.isOwn(find(401884775)));
}

void test_unknown_kickoff_and_justified_interruption_have_bounded_real_windows() {
  auto clock = apply(demo::Scenario::kOwn, 5);
  const auto& suspended = find(401884775);
  TEST_ASSERT_EQUAL(Phase::kSuspended, suspended.phase);
  TEST_ASSERT_TRUE(suspended.first_live_observed_utc != 0);
  TEST_ASSERT_TRUE(selection::window(input(clock), routes.entries[0], suspended)
                       .reported_running);
  clock = apply(demo::Scenario::kOwn, 6);
  const auto& unknown = find(401884775);
  TEST_ASSERT_EQUAL_UINT32(0, unknown.kickoff);
  const auto window =
      selection::window(input(clock), routes.entries[0], unknown);
  TEST_ASSERT_TRUE(window.reported_running);
  TEST_ASSERT_EQUAL_INT64(
      unknown.first_live_observed_utc + cfg::kUnknownMatchEndMinutes * 60u,
      window.end);
}

void test_no_club_ordinary_single_keeps_same_provider_names_and_available_events() {
  const auto clock = apply(demo::Scenario::kOrdinary, 4);
  const auto facts = input(clock);
  const auto selected = selection::select(facts);
  TEST_ASSERT_EQUAL_UINT32(0, routes.entries[1].club.team);
  const auto single = selection::single(facts, selected, routes.entries[1]);
  TEST_ASSERT_EQUAL_UINT32(401884777, single.match.id);
  const auto& ordinary = find(401884777, demo::kSecondCompetition);
  TEST_ASSERT_FALSE(store.isOwn(ordinary));
  TEST_ASSERT_TRUE(store.hasDetail(ordinary));
  ArduinoJson::JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, recorded::kEspnDay));
  MapContext map;
  map.source = test_source::source(recorded_store, demo::kSecondCompetition,
                                   cfg::Provider::kEspn, 132);
  map.clock = clock;
  TEST_ASSERT_TRUE(storeMatch(doc["events"][1], recorded_store, map));
  const auto* recorded_match = recorded_store.find(map.source, 401884777);
  TEST_ASSERT_NOT_NULL(recorded_match);
  TEST_ASSERT_EQUAL_STRING(
      view::teamName(recorded_store, *recorded_match, true),
      view::teamName(store, ordinary, true));
  const auto shown = view::events(store, ordinary);
  const auto original = view::events(recorded_store, *recorded_match);
  TEST_ASSERT_EQUAL_UINT8(original.count, shown.count);
  for (unsigned i = 0; i < shown.count; ++i) {
    const auto& event = store.eventAt(shown.index[i]);
    const auto& source = recorded_store.eventAt(original.index[i]);
    TEST_ASSERT_EQUAL(source.type, event.type);
    TEST_ASSERT_EQUAL(source.minute, event.minute);
    TEST_ASSERT_EQUAL_STRING(source.player.data, event.player.data);
  }
}

void test_foreign_result_without_club_is_empty_after_its_window() {
  const auto clock = apply(demo::Scenario::kOrdinary, 5);
  const auto facts = input(clock);
  const auto chosen =
      selection::single(facts, selection::select(facts), routes.entries[1]);
  TEST_ASSERT_EQUAL(selection::Content::kEmpty, chosen.content);
}

void test_five_conference_matches_stay_reachable_without_single_allocation() {
  const auto clock = apply(demo::Scenario::kConference, 1);
  TEST_ASSERT_EQUAL_UINT16(5, store.matchCount());
  TEST_ASSERT_EQUAL(policy::Phase::kOtherGames,
                    selection::select(input(clock)).phase);
  view::List members;
  for (unsigned i = 0; i < store.matchCount(); ++i) {
    TEST_ASSERT_FALSE(store.isOwn(store.match(i)));
    TEST_ASSERT_FALSE(store.hasDetail(store.match(i)));
    members.index[members.count++] = i;
  }
  ConferenceRequest request;
  request.per_page = 4;
  const auto first = view::conferencePage(store, members, request);
  request.page = 1;
  const auto second = view::conferencePage(store, members, request);
  TEST_ASSERT_EQUAL_UINT8(2, first.pages);
  TEST_ASSERT_EQUAL_UINT8(5, first.row_count + second.row_count);
}

void test_cup_keeps_scores_and_all_reported_attempts_separate_from_highlights() {
  auto clock = apply(demo::Scenario::kCup, 1);
  const auto& extra = find(401875181, demo::kSecondCompetition);
  TEST_ASSERT_EQUAL(Phase::kExtraTime, extra.phase);
  clock = apply(demo::Scenario::kCup, 2);
  const auto& first_shots = find(401875181, demo::kSecondCompetition);
  TEST_ASSERT_EQUAL(Phase::kPenalties, first_shots.phase);
  const auto first_events = view::events(store, first_shots);
  const auto first_attempts = view::shootout(store, first_shots);
  unsigned prior_goals = 0;
  for (unsigned i = 0; i < first_events.count; ++i) {
    const auto& event = store.eventAt(first_events.index[i]);
    prior_goals += event.type == EventType::kGoal;
  }
  TEST_ASSERT_EQUAL_UINT(2, first_attempts.count);
  TEST_ASSERT_EQUAL_UINT(2, prior_goals);
  clock = apply(demo::Scenario::kCup, 4);
  const auto& final = find(401875181, demo::kSecondCompetition);
  TEST_ASSERT_EQUAL(Phase::kFinished, final.phase);
  TEST_ASSERT_EQUAL_UINT8(2, final.shootout.home);
  TEST_ASSERT_EQUAL_UINT8(4, final.shootout.away);
  // Owner requirement: all reported shots are separate from12 highlights;
  // this14-event body fits the existing pool and must not claim truncation.
  TEST_ASSERT_FALSE(final.flags & kMatchEventsTruncated);
  TEST_ASSERT_FALSE(store.notices(demo::kSecondCompetition) & kNoticeEvents);
  const auto shown = view::events(store, final);
  TEST_ASSERT_EQUAL_UINT8(8, shown.count);
  const auto attempts = view::shootout(store, final);
  TEST_ASSERT_EQUAL_UINT8(6, attempts.count);
  for (unsigned i = 0; i < attempts.count; ++i)
    TEST_ASSERT_EQUAL(EventType::kPenaltyGoal,
                      store.eventAt(attempts.index[i]).type);
  // No unreported missed attempts are fabricated.
}

void test_verified_round_keeps_later_rescheduled_fixture_in_same_content() {
  const auto clock = apply(demo::Scenario::kRounds, 0);
  const auto facts = input(clock);
  const auto chosen =
      selection::round(facts, routes.entries[0], cfg::MatchdayMode::kCurrent);
  TEST_ASSERT_TRUE(chosen.verified);
  unsigned members = 0;
  for (unsigned i = 0; i < store.matchCount(); ++i)
    members += selection::roundContains(facts, chosen, store.match(i));
  TEST_ASSERT_EQUAL_UINT(3, members);
  TEST_ASSERT_TRUE(selection::roundContains(facts, chosen, find(401884764)));
  apply(demo::Scenario::kRounds, 1);
  TEST_ASSERT_EQUAL(Phase::kFinished, find(401884767).phase);
  TEST_ASSERT_EQUAL(Phase::kFinished, find(401884765).phase);
  TEST_ASSERT_TRUE(find(401884767).principal.valid());
  TEST_ASSERT_EQUAL(Phase::kScheduled, find(401884764).phase);
}

void test_synthetic_break_respects_owner_switch_and_empty_has_no_data() {
  auto clock = apply(demo::Scenario::kBreak, 0);
  TEST_ASSERT_TRUE(demo::breakProven(demo::Scenario::kBreak));
  TEST_ASSERT_TRUE(demo::breakProven(demo::Scenario::kBreak, 49));
  TEST_ASSERT_FALSE(demo::breakProven(demo::Scenario::kBreak, 50));
  TEST_ASSERT_FALSE(demo::breakProven(demo::Scenario::kBreak, 366));
  TEST_ASSERT_NOT_EQUAL(policy::Phase::kBreak,
                        selection::select(input(clock, true)).phase);
  options.automatic_break = true;
  TEST_ASSERT_EQUAL(policy::Phase::kBreak,
                    selection::select(input(clock, true)).phase);
  clock = apply(demo::Scenario::kEmpty, 0);
  TEST_ASSERT_EQUAL_UINT16(0, store.matchCount());
  TEST_ASSERT_EQUAL_UINT16(0, store.rowCount());
  TEST_ASSERT_FALSE(demo::breakProven(demo::Scenario::kEmpty));
}

void test_clock_is_deterministic_goal_flash_uses_real_mono_and_invalid_is_noop() {
  auto clock = apply(demo::Scenario::kOwn, 4);
  TEST_ASSERT_EQUAL_UINT32(5000, find(401884775).score_changed);
  const auto again =
      demo::apply(store, routes, bindings, demo::Scenario::kOwn, 4, 6000);
  TEST_ASSERT_EQUAL_INT64(clock.utc, again.utc);
  TEST_ASSERT_EQUAL_UINT32(6000, again.mono_s);
  TEST_ASSERT_EQUAL_UINT32(6000, find(401884775).score_changed);
  const auto count = store.matchCount();
  TEST_ASSERT_EQUAL_INT64(
      0, demo::apply(store, routes, bindings, demo::Scenario::kOwn, 255, 7000)
             .utc);
  TEST_ASSERT_EQUAL_UINT(count, store.matchCount());
  TEST_ASSERT_EQUAL_UINT8(0, demo::steps(static_cast<demo::Scenario>(255)));
  TEST_ASSERT_EQUAL_STRING("", demo::stepTitle(demo::Scenario::kOwn, 255));
}

void test_saved_order_and_club_preferences_drive_projected_own_matches() {
  settings::CompetitionRoute saved;
  saved.id = 47;
  saved.incarnation = 103;
  saved.primary.provider = cfg::Provider::kOpenLigaDb;
  saved.primary.competition.assign("bl1");
  saved.club.team = 80;
  TEST_ASSERT_TRUE(routes.entries.push(saved));
  saved.id = 12;
  saved.incarnation = 101;
  saved.primary.provider = cfg::Provider::kFootballData;
  saved.primary.competition.assign("PL");
  TEST_ASSERT_TRUE(routes.entries.push(saved));
  saved.id = 93;
  saved.incarnation = 111;
  TEST_ASSERT_TRUE(routes.entries.push(saved));
  routes.entries[0].club.own_special = false;
  routes.entries[1].club.own_special = true;
  bindings = demo::configure(routes, demo::Scenario::kOwn, true);
  TEST_ASSERT_TRUE(bindings[0].saved);
  TEST_ASSERT_TRUE(bindings[1].saved);
  TEST_ASSERT_EQUAL_UINT8(47, routes.entries[0].id);
  TEST_ASSERT_EQUAL_UINT32(103, routes.entries[0].incarnation);
  TEST_ASSERT_EQUAL_UINT8(12, routes.entries[1].id);
  TEST_ASSERT_EQUAL_UINT32(101, routes.entries[1].incarnation);
  TEST_ASSERT_EQUAL_UINT8(2, routes.entries.size());
  const auto clock =
      demo::apply(store, routes, bindings, demo::Scenario::kOwn, 4, 5000);
  TEST_ASSERT_EQUAL_UINT8(12, selection::select(input(clock)).competition);
  const auto* first = store.find(store.currentSource(47), 401884775);
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_TRUE(store.isOwn(*first));
}

void test_one_saved_entry_has_explicit_unbound_collision_free_second_dataset() {
  settings::CompetitionRoute saved;
  saved.id = demo::kSecondCompetition;
  saved.incarnation = 73;
  TEST_ASSERT_TRUE(routes.entries.push(saved));
  bindings = demo::configure(routes, demo::Scenario::kOwn, true);
  TEST_ASSERT_TRUE(bindings[0].saved);
  TEST_ASSERT_FALSE(bindings[1].saved);
  TEST_ASSERT_EQUAL_UINT32(0, bindings[1].incarnation);
  TEST_ASSERT_EQUAL_UINT8(saved.id, bindings[0].competition);
  TEST_ASSERT_NOT_EQUAL(bindings[0].competition, bindings[1].competition);
  TEST_ASSERT_EQUAL_UINT32(73, bindings[0].incarnation);
  const auto clock =
      demo::apply(store, routes, bindings, demo::Scenario::kOwn, 4, 5000);
  TEST_ASSERT_EQUAL_UINT8(saved.id,
                          selection::select(input(clock)).competition);
  TEST_ASSERT_NOT_NULL(
      store.find(store.currentSource(bindings[1].competition), 401884777));
}

void test_uncommitted_presets_never_claim_a_saved_demo_binding() {
  settings::CompetitionRoute preset;
  preset.id = 5;
  preset.incarnation = 123;
  TEST_ASSERT_TRUE(routes.entries.push(preset));
  bindings = demo::configure(routes, demo::Scenario::kOwn, false);
  TEST_ASSERT_FALSE(bindings[0].saved);
  TEST_ASSERT_FALSE(bindings[1].saved);
  TEST_ASSERT_EQUAL_UINT32(0, bindings[0].incarnation);
  TEST_ASSERT_EQUAL_UINT32(0, bindings[1].incarnation);
  TEST_ASSERT_EQUAL_UINT8(demo::kFirstCompetition, bindings[0].competition);
  TEST_ASSERT_EQUAL_UINT8(demo::kSecondCompetition, bindings[1].competition);
}

void test_stale_demo_binding_never_rebuilds_the_previous_store() {
  apply(demo::Scenario::kOwn, 4);
  const auto count = store.matchCount();
  ++bindings[0].incarnation;
  TEST_ASSERT_EQUAL_INT64(
      0,
      demo::apply(store, routes, bindings, demo::Scenario::kOwn, 7, 6000).utc);
  TEST_ASSERT_EQUAL_UINT(count, store.matchCount());
  TEST_ASSERT_EQUAL(Phase::kLive, find(401884775).phase);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_own_phase_focus_and_following_day_result_use_real_selector);
  RUN_TEST(
      test_concurrent_owned_competitions_use_order_without_faking_ownership);
  RUN_TEST(
      test_unknown_kickoff_and_justified_interruption_have_bounded_real_windows);
  RUN_TEST(
      test_no_club_ordinary_single_keeps_same_provider_names_and_available_events);
  RUN_TEST(test_foreign_result_without_club_is_empty_after_its_window);
  RUN_TEST(
      test_five_conference_matches_stay_reachable_without_single_allocation);
  RUN_TEST(
      test_cup_keeps_scores_and_all_reported_attempts_separate_from_highlights);
  RUN_TEST(test_verified_round_keeps_later_rescheduled_fixture_in_same_content);
  RUN_TEST(test_synthetic_break_respects_owner_switch_and_empty_has_no_data);
  RUN_TEST(
      test_clock_is_deterministic_goal_flash_uses_real_mono_and_invalid_is_noop);
  RUN_TEST(test_saved_order_and_club_preferences_drive_projected_own_matches);
  RUN_TEST(
      test_one_saved_entry_has_explicit_unbound_collision_free_second_dataset);
  RUN_TEST(test_uncommitted_presets_never_claim_a_saved_demo_binding);
  RUN_TEST(test_stale_demo_binding_never_rebuilds_the_previous_store);
  return UNITY_END();
}
