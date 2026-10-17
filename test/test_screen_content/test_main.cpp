// Owner-visible common-sequence projections over the actual canonical store.
// These checks prove controller content rules, not provider/device acceptance.
#include <unity.h>

#include "navigation_policy.h"

namespace {
namespace seq = ui::sequence;
namespace policy = settings::screen_policy;
using football::Phase;
using screen::Type;
constexpr int64_t kDay = 86400, kToday = 1792195200;  // 2026-10-17 UTC.
football::MatchStore store;
settings::Competitions competitions;
policy::Policy rules;
football::view::Shown shown;
seq::State state;

void competition(uint8_t id, bool club = true) {
  settings::Competition c;
  c.id = id;
  c.incarnation = id;
  c.primary.competition.assign("league");
  if (club) c.primary.team.assign("40");
  TEST_ASSERT_TRUE(competitions.entries.push(c));
  rules.competitions[id - 1] = {id, id, true};
  shown.add(c);
}
void match(uint8_t competition, uint32_t id, int64_t kickoff, uint8_t round,
           bool own = false, Phase phase = Phase::kScheduled) {
  football::StoredMatch row;
  row.id = id;
  row.competition = competition;
  row.kickoff = static_cast<uint32_t>(kickoff);
  row.round = round;
  row.phase = phase;
  row.home = store.team(competition, row.provider, own ? 40 : id * 2, "Home");
  row.away = store.team(competition, row.provider, id * 2 + 1, "Away");
  if (own) row.flags = football::kMatchOwn;
  if (phase == Phase::kFinished) row.finished_at = kickoff + 7200;
  TEST_ASSERT_NOT_NULL(
      store.upsert(row, {kToday, 10}, "Home Club", "Away Club"));
}
ui::NavigationData facts(int64_t utc = kToday, uint8_t loaded_matches = 255,
                         uint8_t loaded_tables = 255, uint8_t failed = 0) {
  return {store,          shown,         competitions, {utc, 10}, {},
          loaded_matches, loaded_tables, failed,       true,      false};
}
seq::Entry entry(Type type, uint8_t competition = 1) {
  return {type, competition, competition};
}
seq::Content content(ui::NavigationContext& context, Type type,
                     seq::PageMode mode = seq::PageMode::kAutomatic,
                     uint16_t page = 0, uint8_t competition = 1) {
  return ui::content(entry(type, competition), mode, page, &context);
}
void test_load_proofs_distinguish_empty_matches_unloaded_table_and_failure() {
  competition(1);
  auto data = facts(kToday, 1, 0);
  ui::NavigationContext context(rules, data);
  TEST_ASSERT_EQUAL(seq::ContentKind::kEmpty,
                    content(context, Type::kLiveMulti).kind);
  TEST_ASSERT_EQUAL(seq::ContentKind::kError,
                    content(context, Type::kTable).kind);
  data.loaded_tables = 1;
  TEST_ASSERT_EQUAL(seq::ContentKind::kEmpty,
                    content(context, Type::kTable).kind);
  data.failed = 1;
  TEST_ASSERT_EQUAL(seq::ContentKind::kError,
                    content(context, Type::kLiveMulti).kind);
}
void test_special_off_own_match_is_ordinary_single_without_own_priority() {
  competition(1);
  rules.competitions[0].own_special = false;
  match(1, 1, kToday - 3600, 1, true, Phase::kLive);
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  TEST_ASSERT_EQUAL(policy::Phase::kOtherGames, context.selected.phase);
  TEST_ASSERT_EQUAL(seq::ContentKind::kData,
                    content(context, Type::kLiveSingle).kind);
  const auto selected = football::selection::single(
      context.selection, context.selected, competitions.entries[0]);
  TEST_ASSERT_TRUE(
      store.find(1, cfg::Provider::kOpenLigaDb, selected.match.id)->isOwn());
}
void test_own_phase_never_exposes_other_competition_single_even_when_unloaded() {
  competition(1);
  competition(2);
  match(1, 1, kToday - 3600, 1, true, Phase::kLive);
  const auto data = facts(kToday, 1, 0);
  ui::NavigationContext context(rules, data);
  TEST_ASSERT_EQUAL(policy::Phase::kOwn, context.selected.phase);
  TEST_ASSERT_EQUAL(
      seq::ContentKind::kEmpty,
      content(context, Type::kLiveSingle, seq::PageMode::kAutomatic, 0, 2)
          .kind);
}
void test_conference_competition_projection_and_one_page_complete_manual_view() {
  competition(1, false);
  competition(2, false);
  rules.visible_matches = 2;
  rules.conference_mode = cfg::ConferenceMode::kOnePage;
  for (uint32_t i = 1; i <= 5; ++i)
    match(1, i, kToday - 3600, 1, false, Phase::kLive);
  match(2, 10, kToday - 3600, 1, false, Phase::kLive);
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  TEST_ASSERT_EQUAL_UINT16(1, content(context, Type::kLiveMulti).pages);
  TEST_ASSERT_EQUAL_UINT16(
      3, content(context, Type::kLiveMulti, seq::PageMode::kManualHeld).pages);
  const auto members = ui::conferenceMembers(context, 1);
  TEST_ASSERT_EQUAL_UINT16(5, members.count);
  for (unsigned i = 0; i < members.count; ++i)
    TEST_ASSERT_EQUAL_UINT8(1, store.match(members.index[i]).competition);
  TEST_ASSERT_EQUAL_UINT8(1, ui::conferenceMembers(context, 2).count);
}
void test_special_off_ordinary_own_match_keeps_optional_conference_pin() {
  competition(1);
  rules.competitions[0].own_special = false;
  rules.visible_matches = 2;
  match(1, 1, kToday - 3600, 1, false, Phase::kLive);
  match(1, 2, kToday - 3600, 1, true, Phase::kLive);
  match(1, 3, kToday - 3600, 1, false, Phase::kLive);
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  TEST_ASSERT_EQUAL(policy::Phase::kOtherGames, context.selected.phase);
  const auto first = ui::conferencePage(context, entry(Type::kLiveMulti),
                                        seq::PageMode::kAutomatic, 0);
  const auto second = ui::conferencePage(context, entry(Type::kLiveMulti),
                                         seq::PageMode::kAutomatic, 1);
  TEST_ASSERT_TRUE(store.match(first.rows[0]).isOwn());
  TEST_ASSERT_EQUAL_UINT8(first.rows[0], second.rows[0]);
}
void test_one_page_manual_anchor_tracks_the_match_actually_shown_first() {
  competition(1, false);
  rules.visible_matches = 2;
  rules.conference_mode = cfg::ConferenceMode::kOnePage;
  for (uint32_t i = 1; i <= 5; ++i)
    match(1, i, kToday - 3600, 1, false, Phase::kLive);
  auto active = *store.find(1, cfg::Provider::kOpenLigaDb, 5);
  active.principal = {2, 0};
  TEST_ASSERT_NOT_NULL(store.upsert(active, {kToday, 10}));
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  const auto automatic = ui::conferencePage(context, entry(Type::kLiveMulti),
                                            seq::PageMode::kAutomatic, 0);
  TEST_ASSERT_EQUAL_UINT32(5, store.match(automatic.rows[0]).id);
  TEST_ASSERT_EQUAL_UINT16(
      2, content(context, Type::kLiveMulti, seq::PageMode::kManualHeld)
             .anchor_page);
}
void test_balanced_conference_manual_anchor_uses_actual_page_boundaries() {
  competition(1, false);
  rules.visible_matches = 5;
  rules.conference_mode = cfg::ConferenceMode::kOnePage;
  for (uint32_t id = 1; id <= 11; ++id)
    match(1, id, kToday - 3600 + id, 1, false, Phase::kLive);
  auto changed = *store.find(1, cfg::Provider::kOpenLigaDb, 5);
  changed.principal = {2, 0};
  TEST_ASSERT_NOT_NULL(store.upsert(changed, {kToday, 10}));
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  const auto automatic = ui::conferencePage(context, entry(Type::kLiveMulti),
                                            seq::PageMode::kAutomatic, 0);
  TEST_ASSERT_EQUAL_UINT32(5, store.match(automatic.rows[0]).id);
  const auto held =
      content(context, Type::kLiveMulti, seq::PageMode::kManualHeld);
  TEST_ASSERT_EQUAL_UINT16(3, held.pages);
  TEST_ASSERT_EQUAL_UINT16(1, held.anchor_page);  // 4+4+3, fifth game on page1.
  seq::reset(state, context.input(), 0);
  seq::presented(state, state.revision, 0);
  seq::applyAction(state, context.input(), ui::Action::kDown, 1000);
  TEST_ASSERT_TRUE(state.paged);
  TEST_ASSERT_EQUAL_UINT16(2, state.page);  // Anchor1, then move one page.
}
void test_balanced_conference_automatic_rotation_keeps_original_pages() {
  competition(1, false);
  rules.visible_matches = 5;
  for (uint32_t id = 1; id <= 11; ++id)
    match(1, id, kToday - 3600, 1, false, Phase::kLive);
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  seq::reset(state, context.input(), 0);
  seq::presented(state, state.revision, 0);
  TEST_ASSERT_EQUAL(Type::kLiveMulti, state.entry.type);
  TEST_ASSERT_EQUAL_UINT16(3, state.pages);
  TEST_ASSERT_EQUAL_UINT8(
      4, ui::conferencePage(context, state.entry, seq::PageMode::kAutomatic,
                            state.page)
             .row_count);
  seq::synchronize(state, context.input(), state.total_ms / 3);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  seq::presented(state, state.revision, state.total_ms / 3);
  seq::synchronize(state, context.input(), state.total_ms * 2 / 3);
  TEST_ASSERT_EQUAL_UINT16(2, state.page);
  TEST_ASSERT_EQUAL_UINT8(
      3, ui::conferencePage(context, state.entry, seq::PageMode::kAutomatic,
                            state.page)
             .row_count);
}
void test_next_mode_keeps_held_round_manually_reachable_without_automatic_content() {
  competition(1);
  rules.matchday_mode = cfg::MatchdayMode::kNext;
  match(1, 1, kToday - kDay + 3600, 4, false, Phase::kFinished);
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  TEST_ASSERT_EQUAL(seq::ContentKind::kEmpty,
                    content(context, Type::kMatchday).kind);
  TEST_ASSERT_EQUAL(
      seq::ContentKind::kData,
      content(context, Type::kMatchday, seq::PageMode::kManualHeld).kind);
  TEST_ASSERT_EQUAL(
      seq::ContentKind::kData,
      content(context, Type::kMatchday, seq::PageMode::kManualAutomatic).kind);
  const auto manual =
      seq::adjacent(context.input(), entry(Type::kTable), true, -1);
  TEST_ASSERT_EQUAL(Type::kMatchday, manual.type);
  const auto selection = ui::matchdaySelection(context, entry(Type::kMatchday),
                                               seq::PageMode::kManualHeld, 0);
  TEST_ASSERT_TRUE(selection.available);
  TEST_ASSERT_EQUAL_UINT32(1, selection.round.anchor.id);
}
void test_table_manual_anchor_uses_actual_clamped_origin_and_reaches_every_row() {
  competition(1);
  rules.table_window = 3;
  store.beginTable(1);
  for (uint8_t i = 0; i < 8; ++i) {
    football::StoredRow row;
    row.team = store.team(1, cfg::Provider::kOpenLigaDb, 100 + i, "Club");
    if (i == 7) row.flags = football::kRowOwn;
    TEST_ASSERT_TRUE(store.appendRow(row));
  }
  TEST_ASSERT_TRUE(store.commitTable(true));
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  TEST_ASSERT_EQUAL_UINT16(1, content(context, Type::kTable).pages);
  const auto manual =
      content(context, Type::kTable, seq::PageMode::kManualHeld);
  TEST_ASSERT_EQUAL_UINT16(6, manual.pages);
  TEST_ASSERT_EQUAL_UINT16(5, manual.anchor_page);
  TEST_ASSERT_EQUAL_INT(0, ui::tableSelection(context, entry(Type::kTable),
                                              seq::PageMode::kManualHeld, 0)
                               .first);
  TEST_ASSERT_EQUAL_INT(5, ui::tableSelection(context, entry(Type::kTable),
                                              seq::PageMode::kManualHeld, 5)
                               .first);
}
void test_verified_round_content_includes_rescheduled_fixture_outside_phase_cluster() {
  competition(1);
  rules.visible_matches = 2;
  match(1, 1, kToday + 3600, 5);
  match(1, 2, kToday + 8 * kDay, 5);
  match(1, 3, kToday + 7 * kDay, 6);
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  const auto chosen = ui::matchdaySelection(context, entry(Type::kMatchday),
                                            seq::PageMode::kAutomatic, 0);
  TEST_ASSERT_EQUAL_UINT16(1, chosen.pages);
  const auto rows = ui::roundMembers(context, chosen.round);
  TEST_ASSERT_EQUAL_UINT16(2, rows.count);
  TEST_ASSERT_EQUAL_UINT32(2, store.match(rows.index[1]).id);
  const auto reach = football::selection::rounds(
      context.selection, competitions.entries[0], cfg::MatchdayMode::kCurrent);
  TEST_ASSERT_EQUAL_UINT8(2, reach.count);
}
void test_matchday_held_projection_flattens_previous_current_and_next_round_pages() {
  competition(1);
  rules.visible_matches = 2;
  match(1, 1, kToday - kDay + 3600, 4, false, Phase::kFinished);
  match(1, 2, kToday + 3600, 5);
  match(1, 3, kToday + 7200, 5);
  match(1, 4, kToday + 7 * kDay, 6);
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  const auto manual =
      content(context, Type::kMatchday, seq::PageMode::kManualHeld, 1);
  TEST_ASSERT_EQUAL_UINT16(3, manual.pages);
  TEST_ASSERT_EQUAL_UINT16(2, manual.anchor_page);
  const auto last = ui::matchdaySelection(context, entry(Type::kMatchday),
                                          seq::PageMode::kManualHeld, 2);
  const auto rows = ui::roundMembers(context, last.round);
  TEST_ASSERT_EQUAL_UINT32(4, store.match(rows.index[0]).id);
}
void test_two_verified_rounds_on_one_date_remain_manually_reachable() {
  competition(1);
  rules.visible_matches = 2;
  match(1, 1, kToday + 3600, 4);
  match(1, 2, kToday + 7200, 5);
  const auto data = facts();
  ui::NavigationContext context(rules, data);
  TEST_ASSERT_EQUAL_UINT16(
      2, content(context, Type::kMatchday, seq::PageMode::kManualHeld).pages);
  const auto selected = ui::matchdaySelection(context, entry(Type::kMatchday),
                                              seq::PageMode::kManualHeld, 1);
  TEST_ASSERT_EQUAL_UINT32(2, selected.round.anchor.id);
}
void test_real_projection_growth_waits_next_visit_but_manual_held_reaches_new_pages() {
  competition(1, false);
  rules.visible_matches = 2;
  rules.shared[policy::index(policy::Phase::kOtherGames)]
              [screen::index(Type::kLiveMulti)] = {15, true, true};
  match(1, 1, kToday - 3600, 1, false, Phase::kLive);
  match(1, 2, kToday - 3600, 1, false, Phase::kLive);
  auto data = facts();
  ui::NavigationContext context(rules, data);
  seq::reset(state, context.input(), 0);
  seq::presented(state, state.revision, 0);
  TEST_ASSERT_EQUAL_UINT16(1, state.pages);
  match(1, 3, kToday - 3600, 1, false, Phase::kLive);
  seq::synchronize(state, context.input(), 1000);
  TEST_ASSERT_EQUAL_UINT16(1, state.pages);
  seq::applyAction(state, context.input(), ui::Action::kDown, 1000);
  TEST_ASSERT_TRUE(state.paged);
  TEST_ASSERT_EQUAL_UINT16(2, state.pages);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
}
}  // namespace
void setUp() {
  store.clear();
  competitions = {};
  rules = {};
  shown = {};
  state = {};
}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(
      test_load_proofs_distinguish_empty_matches_unloaded_table_and_failure);
  RUN_TEST(test_special_off_own_match_is_ordinary_single_without_own_priority);
  RUN_TEST(
      test_own_phase_never_exposes_other_competition_single_even_when_unloaded);
  RUN_TEST(
      test_conference_competition_projection_and_one_page_complete_manual_view);
  RUN_TEST(test_special_off_ordinary_own_match_keeps_optional_conference_pin);
  RUN_TEST(test_one_page_manual_anchor_tracks_the_match_actually_shown_first);
  RUN_TEST(test_balanced_conference_manual_anchor_uses_actual_page_boundaries);
  RUN_TEST(test_balanced_conference_automatic_rotation_keeps_original_pages);
  RUN_TEST(
      test_next_mode_keeps_held_round_manually_reachable_without_automatic_content);
  RUN_TEST(
      test_table_manual_anchor_uses_actual_clamped_origin_and_reaches_every_row);
  RUN_TEST(
      test_verified_round_content_includes_rescheduled_fixture_outside_phase_cluster);
  RUN_TEST(
      test_matchday_held_projection_flattens_previous_current_and_next_round_pages);
  RUN_TEST(test_two_verified_rounds_on_one_date_remain_manually_reachable);
  RUN_TEST(
      test_real_projection_growth_waits_next_visit_but_manual_held_reaches_new_pages);
  return UNITY_END();
}
