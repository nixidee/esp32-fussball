// Requirement cases for the pure screen selector. Synthetic fixtures prove
// selection boundaries and grouping, not current provider/device behavior.
#include <unity.h>

#include "screen_selection.h"

namespace {
using namespace football;
namespace sel = football::selection;
namespace policy = settings::screen_policy;
MatchStore store;
settings::CompetitionRoutes competitions;
policy::Policy rules;
constexpr auto kProvider = cfg::Provider::kOpenLigaDb;

int64_t at(const char* value) { return parseUtc(value); }

settings::CompetitionRoute& competition(uint8_t id, bool club = false,
                                        bool special = true) {
  settings::CompetitionRoute c;
  c.id = id;
  c.incarnation = id;
  c.primary.provider = kProvider;
  c.primary.competition.assign("league");
  if (club) c.club.team = 40;
  TEST_ASSERT_TRUE(competitions.entries.push(c));
  competitions.entries[competitions.entries.size() - 1].club.own_special =
      special;
  return competitions.entries[competitions.entries.size() - 1];
}

StoredMatch& match(uint8_t competition, uint32_t id, int64_t kickoff,
                   Phase phase = Phase::kScheduled, bool own = false,
                   uint8_t round = 0, int64_t seen = 0) {
  StoredMatch m;
  m.id = id;
  m.competition = competition;
  m.provider = competitions.find(competition)->primary.provider;
  m.kickoff = kickoff;
  m.phase = phase;
  m.round = round;
  if (own) m.flags = kMatchOwn;
  auto* out = store.upsert(m, {seen, 1});
  TEST_ASSERT_NOT_NULL(out);
  return *out;
}

sel::Input input(int64_t now, bool break_proven = false,
                 DateContext dates = {}) {
  return {rules, competitions, store, {now, 1}, dates, break_proven};
}

void test_global_own_priority_and_running_own_beats_earlier_buffers() {
  competition(1, true);
  competition(2, true);
  competition(3);
  const auto now = at("2026-10-17T14:00:00Z");
  rules.own_before_min = 60;
  match(1, 10, now + 1800, Phase::kScheduled, true);
  match(2, 20, now - 600, Phase::kLive, true, 0, now - 600);
  match(3, 30, now - 600, Phase::kLive, false, 0, now - 600);
  auto selected = sel::select(input(now));
  TEST_ASSERT_EQUAL(policy::Phase::kOwn, selected.phase);
  TEST_ASSERT_EQUAL_UINT32(20, selected.focus.id);
  match(1, 11, now - 300, Phase::kLive, true, 0, now - 300);
  selected = sel::select(input(now));
  TEST_ASSERT_EQUAL_UINT32(11, selected.focus.id);
}

void test_own_phase_exposes_only_the_focused_single() {
  const auto& first = competition(1, true);
  const auto& second = competition(2, true);
  const auto now = at("2026-10-17T14:00:00Z");
  match(1, 10, now - 300, Phase::kLive, true, 0, now - 300);
  match(2, 20, now + 86400, Phase::kScheduled, true);
  const auto in = input(now);
  const auto selected = sel::select(in);
  TEST_ASSERT_EQUAL_UINT32(10, sel::single(in, selected, first).match.id);
  TEST_ASSERT_EQUAL(sel::Content::kEmpty,
                    sel::single(in, selected, second).content);
}

void test_own_special_off_is_an_ordinary_single_without_losing_ownership() {
  const auto& c = competition(1, true, false);
  const auto now = at("2026-10-17T14:00:00Z");
  match(1, 10, now - 300, Phase::kLive, true, 0, now - 300);
  const auto in = input(now);
  const auto selected = sel::select(in);
  TEST_ASSERT_EQUAL(policy::Phase::kOtherGames, selected.phase);
  TEST_ASSERT_EQUAL_UINT32(10, sel::single(in, selected, c).match.id);
  TEST_ASSERT_TRUE(store.find(1, kProvider, 10)->isOwn());
}

void test_unique_reported_live_beats_prewindow_and_multiple_live_use_club_static() {
  const auto& c = competition(1, true, false);
  const auto now = at("2026-10-17T14:00:00Z");
  rules.other_before_min = 60;
  match(1, 10, now - 300, Phase::kLive, false, 0, now - 300);
  match(1, 11, now + 1800);
  match(1, 20, now + 86400, Phase::kScheduled, true);
  auto in = input(now);
  TEST_ASSERT_EQUAL_UINT32(10, sel::single(in, sel::select(in), c).match.id);
  match(1, 12, now - 600, Phase::kLive, false, 0, now - 600);
  const auto selected = sel::single(in, sel::select(in), c);
  TEST_ASSERT_EQUAL_UINT32(20, selected.match.id);
  TEST_ASSERT_EQUAL(sel::Content::kPreview, selected.content);
}

void test_static_single_without_a_club_is_empty() {
  const auto& c = competition(1);
  const auto now = at("2026-10-17T14:00:00Z");
  match(1, 10, now + 86400);
  TEST_ASSERT_EQUAL(
      sel::Content::kEmpty,
      sel::single(input(now), sel::select(input(now)), c).content);
}

void test_unfinished_estimate_and_reported_live_release_at_110_and_240() {
  const auto& c = competition(1, true);
  const auto kickoff = at("2026-10-17T14:00:00Z");
  auto& estimated = match(1, 10, kickoff, Phase::kLive, true);
  estimated.estimated = kEstimatedPhase | kEstimatedMinute;
  rules.own_after_min =
      180;  // A missing whistle does not create a post buffer.
  TEST_ASSERT_TRUE(
      sel::window(input(kickoff + 110 * 60 - 1), c, estimated).active);
  TEST_ASSERT_FALSE(
      sel::window(input(kickoff + 110 * 60), c, estimated).active);
  const auto reported = match(1, 10, kickoff, Phase::kLive, true, 0, kickoff);
  TEST_ASSERT_TRUE(
      sel::window(input(kickoff + 240 * 60 - 1), c, reported).active);
  TEST_ASSERT_FALSE(sel::window(input(kickoff + 240 * 60), c, reported).active);
  TEST_ASSERT_NOT_EQUAL(policy::Phase::kOwn,
                        sel::select(input(kickoff + 240 * 60)).phase);
  TEST_ASSERT_EQUAL(Phase::kLive, store.find(1, kProvider, 10)->phase);
}

void test_unknown_kickoff_and_interruption_keep_the_original_bounded_slot() {
  const auto& c = competition(1, true);
  const auto observed = at("2026-10-17T14:00:00Z");
  match(1, 10, 0, Phase::kLive, true, 0, observed);
  auto suspended = *store.find(1, kProvider, 10);
  suspended.phase = Phase::kSuspended;
  store.upsert(suspended, {observed + 60, 2});
  const auto& m = *store.find(1, kProvider, 10);
  TEST_ASSERT_TRUE(sel::window(input(observed + 240 * 60 - 1), c, m).active);
  TEST_ASSERT_FALSE(sel::window(input(observed + 240 * 60), c, m).active);
  TEST_ASSERT_EQUAL_UINT32(0, m.kickoff);
  TEST_ASSERT_EQUAL_UINT32(0, m.finished_at);
  match(1, 11, 0, Phase::kSuspended, true, 0, observed);
  TEST_ASSERT_FALSE(
      sel::window(input(observed + 60), c, *store.find(1, kProvider, 11))
          .active);
}

void test_overlapping_own_buffers_handoff_at_clamped_midpoint() {
  const auto& c = competition(1, true);
  const auto finish = at("2026-10-17T14:00:00Z");
  auto& ended = match(1, 10, finish - 7200, Phase::kFinished, true);
  ended.finished_at = finish;
  match(1, 11, finish + 1800, Phase::kScheduled, true);
  rules.own_before_min = rules.own_after_min = 60;
  auto in = input(finish + 899);
  TEST_ASSERT_EQUAL_UINT32(10, sel::select(in).focus.id);
  TEST_ASSERT_EQUAL_UINT32(11, sel::select(input(finish + 900)).focus.id);
  rules.own_before_min = 10;
  TEST_ASSERT_EQUAL_UINT32(10, sel::select(input(finish + 1199)).focus.id);
  TEST_ASSERT_EQUAL_UINT32(11, sel::select(input(finish + 1200)).focus.id);
  rules.own_before_min = 60;
  rules.own_after_min = 10;
  TEST_ASSERT_EQUAL_UINT32(10, sel::select(input(finish + 599)).focus.id);
  TEST_ASSERT_EQUAL_UINT32(11, sel::select(input(finish + 600)).focus.id);
}

void test_verified_round_clusters_split_but_content_keeps_the_complete_round() {
  const auto& c = competition(1);
  match(1, 10, at("2026-10-16T18:00:00Z"), Phase::kFinished, false, 5);
  match(1, 11, at("2026-10-18T14:00:00Z"), Phase::kScheduled, false, 5);
  match(1, 12, at("2026-10-22T18:00:00Z"), Phase::kScheduled, false, 5);
  match(1, 13, at("2026-10-17T18:00:00Z"), Phase::kScheduled, false, 6);
  const auto in = input(at("2026-10-17T00:00:00Z"));
  const auto period = sel::phasePeriod(in, c);
  TEST_ASSERT_TRUE(period.valid());
  TEST_ASSERT_TRUE(period.verified);
  TEST_ASSERT_TRUE(
      sel::roundContains(in, period, *store.find(1, kProvider, 12)));
  TEST_ASSERT_FALSE(
      sel::roundContains(in, period, *store.find(1, kProvider, 13)));
  TEST_ASSERT_FALSE(
      sel::phasePeriod(input(at("2026-10-20T00:00:00Z")), c).valid());
  TEST_ASSERT_EQUAL(policy::Phase::kMatchday, sel::select(in).phase);
}

void test_fallback_has_four_inclusive_dates_and_one_free_intermediate_day() {
  const auto& c = competition(1);
  match(1, 10, at("2026-10-16T18:00:00Z"));
  match(1, 11, at("2026-10-18T18:00:00Z"));
  match(1, 12, at("2026-10-19T18:00:00Z"));
  match(1, 13, at("2026-10-20T18:00:00Z"));
  const auto in = input(at("2026-10-17T00:00:00Z"));
  const auto first = sel::round(in, c, cfg::MatchdayMode::kCurrent);
  TEST_ASSERT_FALSE(first.verified);
  TEST_ASSERT_TRUE(
      sel::roundContains(in, first, *store.find(1, kProvider, 12)));
  TEST_ASSERT_FALSE(
      sel::roundContains(in, first, *store.find(1, kProvider, 13)));
  store.clear();
  match(1, 10, at("2026-10-16T18:00:00Z"));
  match(1, 11, at("2026-10-19T18:00:00Z"));
  TEST_ASSERT_FALSE(
      sel::phasePeriod(input(at("2026-10-17T00:00:00Z")), c).valid());
}

void test_only_provider_round_provenance_can_verify_a_label() {
  auto& c = competition(1);
  c.primary.provider = cfg::Provider::kFootballData;
  auto& fd = match(1, 10, at("2026-10-17T18:00:00Z"));
  fd.label = store.label("REGULAR_SEASON");
  TEST_ASSERT_FALSE(
      sel::phasePeriod(input(at("2026-10-17T12:00:00Z")), c).verified);
  store.clear();
  c.primary.provider = cfg::Provider::kApiFootball;
  auto& api = match(1, 20, at("2026-10-17T18:00:00Z"));
  api.label = store.label("Regular Season - 5");
  TEST_ASSERT_TRUE(
      sel::phasePeriod(input(at("2026-10-17T12:00:00Z")), c).verified);
}

void test_matchday_current_holds_but_next_never_uses_a_held_only_group() {
  const auto& c = competition(1);
  match(1, 10, at("2026-10-18T18:00:00Z"), Phase::kFinished, false, 5);
  const auto in = input(at("2026-10-19T12:00:00Z"));
  TEST_ASSERT_TRUE(sel::round(in, c, cfg::MatchdayMode::kCurrent).valid());
  TEST_ASSERT_FALSE(sel::round(in, c, cfg::MatchdayMode::kNext).valid());
  match(1, 11, at("2026-10-20T18:00:00Z"), Phase::kScheduled, false, 6);
  TEST_ASSERT_EQUAL_UINT32(
      11, sel::round(in, c, cfg::MatchdayMode::kNext).anchor.id);
}

int32_t fallOffset(int64_t utc, void*) {
  return utc < at("2026-10-25T01:00:00Z") ? 7200 : 3600;
}

void test_static_result_uses_finish_day_then_preview_even_after_dst_change() {
  const auto& c = competition(1, true);
  auto& ended =
      match(1, 10, at("2026-10-24T20:30:00Z"), Phase::kFinished, true);
  ended.finished_at = at("2026-10-24T22:30:00Z");
  match(1, 11, at("2026-10-30T18:30:00Z"), Phase::kScheduled, true);
  const auto monday = input(at("2026-10-26T22:30:00Z"), false, {fallOffset});
  const auto tuesday = input(at("2026-10-26T23:00:00Z"), false, {fallOffset});
  TEST_ASSERT_EQUAL(sel::Content::kResult,
                    sel::single(monday, sel::select(monday), c).content);
  const auto next = sel::single(tuesday, sel::select(tuesday), c);
  TEST_ASSERT_EQUAL(sel::Content::kPreview, next.content);
  TEST_ASSERT_EQUAL_UINT32(11, next.match.id);
}

void test_break_requires_explicit_enable_and_proof_and_yields_to_matchday() {
  const auto& c = competition(1);
  const auto now = at("2026-10-17T12:00:00Z");
  TEST_ASSERT_EQUAL(policy::Phase::kIdle, sel::select(input(now, true)).phase);
  rules.automatic_break = true;
  TEST_ASSERT_EQUAL(policy::Phase::kIdle, sel::select(input(now)).phase);
  TEST_ASSERT_EQUAL(policy::Phase::kBreak, sel::select(input(now, true)).phase);
  match(1, 10, at("2026-10-16T18:00:00Z"), Phase::kFinished, false, 5);
  match(1, 11, at("2026-10-18T18:00:00Z"), Phase::kScheduled, false, 5);
  TEST_ASSERT_TRUE(sel::phasePeriod(input(now), c).valid());
  TEST_ASSERT_EQUAL(policy::Phase::kMatchday,
                    sel::select(input(now, true)).phase);
}
void test_explicit_group_priority_and_optional_multiple_own_singles() {
  competition(1, true);
  competition(2, true);
  competition(3, true);
  competitions.entries[0].club.group = competitions.entries[2].club.group = 1;
  const auto now = at("2026-10-17T14:00:00Z");
  match(2, 20, now - 300, Phase::kLive, true, 0, now - 300);
  match(3, 30, now - 300, Phase::kLive, true, 0, now - 300);
  auto selected = sel::select(input(now));
  TEST_ASSERT_EQUAL_UINT32(30, selected.focus.id);
  TEST_ASSERT_FALSE(
      sel::single(input(now), selected, competitions.entries[1]).match.valid());
  rules.only_one_own_single = false;
  TEST_ASSERT_EQUAL_UINT32(
      20, sel::single(input(now), selected, competitions.entries[1]).match.id);
  TEST_ASSERT_EQUAL_UINT32(
      30, sel::single(input(now), selected, competitions.entries[2]).match.id);
  competitions.entries[2].club.group = 0;
  selected = sel::select(input(now));
  TEST_ASSERT_EQUAL_UINT32(20, selected.focus.id);
}

}  // namespace

void setUp() {
  store.clear();
  competitions.clear();
  rules = policy::Policy{};
}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_global_own_priority_and_running_own_beats_earlier_buffers);
  RUN_TEST(test_own_phase_exposes_only_the_focused_single);
  RUN_TEST(test_own_special_off_is_an_ordinary_single_without_losing_ownership);
  RUN_TEST(
      test_unique_reported_live_beats_prewindow_and_multiple_live_use_club_static);
  RUN_TEST(test_static_single_without_a_club_is_empty);
  RUN_TEST(test_unfinished_estimate_and_reported_live_release_at_110_and_240);
  RUN_TEST(
      test_unknown_kickoff_and_interruption_keep_the_original_bounded_slot);
  RUN_TEST(test_overlapping_own_buffers_handoff_at_clamped_midpoint);
  RUN_TEST(
      test_verified_round_clusters_split_but_content_keeps_the_complete_round);
  RUN_TEST(
      test_fallback_has_four_inclusive_dates_and_one_free_intermediate_day);
  RUN_TEST(test_only_provider_round_provenance_can_verify_a_label);
  RUN_TEST(test_matchday_current_holds_but_next_never_uses_a_held_only_group);
  RUN_TEST(
      test_static_result_uses_finish_day_then_preview_even_after_dst_change);
  RUN_TEST(
      test_break_requires_explicit_enable_and_proof_and_yields_to_matchday);
  RUN_TEST(test_explicit_group_priority_and_optional_multiple_own_singles);
  return UNITY_END();
}
