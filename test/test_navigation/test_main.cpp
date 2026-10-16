// Owner-visible navigation contracts, using the production policy and real
// canonical store/views. Synthetic rows exercise transitions; they do not
// prove provider responses, input hardware or display output on the device.
#include <unity.h>

#include <algorithm>
#include <cstdint>

#include "navigation_policy.h"

namespace {
using football::MatchStore;
using football::Phase;
using ui::Action;
using ui::AutomaticReason;
using ui::NavigationData;
using ui::NavigationState;
namespace view = football::view;

constexpr uint8_t kLeague = 3, kCup = 5;
constexpr auto kProvider = cfg::Provider::kOpenLigaDb;
constexpr int64_t kDay = 86400;
constexpr int64_t kFriday = 1792108800;  // 2026-10-16 00:00 UTC.
constexpr int64_t kKickoff = kFriday + 18 * 3600;
MatchStore store;

view::Shown shown(bool cup = false) {
  view::Shown out;
  out.add(view::Source{kLeague, kProvider, 40});
  if (cup) out.add(view::Source{kCup, kProvider, 40});
  return out;
}

uint8_t team(uint32_t id) { return store.team(kLeague, kProvider, id, "Club"); }

void table(uint8_t competition, int rows, int own = -1) {
  store.beginTable(competition);
  for (int i = 0; i < rows; ++i) {
    football::StoredRow row;
    row.team = team(100u * competition + static_cast<unsigned>(i));
    row.position = static_cast<uint8_t>(i + 1);
    if (i == own) row.flags = football::kRowOwn;
    TEST_ASSERT_TRUE(store.appendRow(row));
  }
  TEST_ASSERT_TRUE(store.commitTable(true));
}

void match(uint8_t competition, uint32_t id, int64_t kickoff, bool own = false,
           Phase phase = Phase::kScheduled) {
  football::StoredMatch row;
  row.competition = competition;
  row.provider = kProvider;
  row.id = id;
  row.kickoff = static_cast<uint32_t>(kickoff);
  row.phase = phase;
  row.home = team(own ? 40 : 1000 + id * 2);
  row.away = team(1001 + id * 2);
  if (own) row.flags = football::kMatchOwn;
  TEST_ASSERT_NOT_NULL(store.upsert(row, {kKickoff, 0}));
}

void matchday(uint8_t competition, uint32_t first_id, int64_t kickoff,
              int matches) {
  for (int i = 0; i < matches; ++i)
    match(competition, first_id + i, kickoff + i * 60);
}

NavigationData data(const view::Shown& sources, int64_t utc = kKickoff) {
  return {store, sources, nullptr, 0, utc, 0, true};
}

ui::TableSelection tableView(NavigationState& state,
                             const settings::Model& model,
                             const NavigationData& current, int64_t now = 1) {
  return ui::tableSelection(state, model, current, now, false);
}

ui::MatchdaySelection dayView(NavigationState& state,
                              const settings::Model& model,
                              const NavigationData& current, int64_t now = 1) {
  return ui::matchdaySelection(state, model, current, now, false);
}

void assertDay(const ui::MatchdaySelection& selected, uint8_t competition,
               int page, int pages, int64_t first) {
  TEST_ASSERT_TRUE(selected.available);
  TEST_ASSERT_FALSE(selected.fault);
  TEST_ASSERT_EQUAL_UINT8(competition, selected.competition);
  TEST_ASSERT_EQUAL_INT(page, selected.page);
  TEST_ASSERT_EQUAL_INT(pages, selected.pages);
  TEST_ASSERT_EQUAL_UINT32(first, selected.span.first);
}

void test_own_table_first_press_moves_the_visible_row_at_every_edge() {
  const auto sources = shown();
  settings::Model model;
  for (int own : {0, 1, 8, 16, 17}) {
    store.clear();
    table(kLeague, 18, own);
    const auto current = data(sources);
    for (int window : {1, 5, 7}) {
      model.table_window = static_cast<uint8_t>(window);
      const int first = std::clamp(own - window / 2, 0, 18 - window);
      NavigationState down;
      down.selected = cfg::Screen::kTable;
      TEST_ASSERT_EQUAL_INT(first, tableView(down, model, current).first);
      if (first < 18 - window) {
        ui::applyAction(down, Action::kDown, model, current, 20);
        TEST_ASSERT_EQUAL_INT(first + 1,
                              tableView(down, model, current, 20).first);
      }
      NavigationState up;
      up.selected = cfg::Screen::kTable;
      TEST_ASSERT_EQUAL_INT(first, tableView(up, model, current).first);
      if (first > 0) {
        ui::applyAction(up, Action::kUp, model, current, 20);
        TEST_ASSERT_EQUAL_INT(first - 1,
                              tableView(up, model, current, 20).first);
      }
    }
  }
}

void test_table_every_row_is_reached_and_one_competition_wraps() {
  table(kLeague, 18);
  const auto sources = shown();
  const auto current = data(sources);
  settings::Model model;
  model.table_window = 5;
  NavigationState state;
  state.selected = cfg::Screen::kTable;
  TEST_ASSERT_EQUAL_INT(0, tableView(state, model, current).first);
  for (int first = 1; first <= 13; ++first) {
    ui::applyAction(state, Action::kDown, model, current, first * 20);
    TEST_ASSERT_EQUAL_INT(first, tableView(state, model, current).first);
  }
  ui::applyAction(state, Action::kDown, model, current, 280);
  TEST_ASSERT_EQUAL_INT(0, tableView(state, model, current).first);
  TEST_ASSERT_EQUAL_UINT8(kLeague, state.table_cycle.competition);
  // Backward wrap enters that competition's usual table view too.
  ui::applyAction(state, Action::kUp, model, current, 300);
  TEST_ASSERT_EQUAL_INT(0, tableView(state, model, current).first);
  TEST_ASSERT_EQUAL_UINT8(kLeague, state.table_cycle.competition);
}

void test_table_wrap_keeps_action_order_across_competitions() {
  table(kLeague, 18, 0);
  table(kCup, 13, 12);
  const auto sources = shown(true);
  const auto current = data(sources);
  settings::Model model;
  model.table_window = 5;
  NavigationState state;
  state.selected = cfg::Screen::kTable;
  TEST_ASSERT_EQUAL_INT(0, tableView(state, model, current).first);
  ui::applyAction(state, Action::kUp, model, current, 20);
  auto selected = tableView(state, model, current);
  TEST_ASSERT_EQUAL_UINT8(kCup, selected.competition);
  TEST_ASSERT_EQUAL_INT(8, selected.first);
  ui::applyAction(state, Action::kDown, model, current, 40);
  selected = tableView(state, model, current);
  TEST_ASSERT_EQUAL_UINT8(kLeague, selected.competition);
  TEST_ASSERT_EQUAL_INT(0, selected.first);
  ui::applyAction(state, Action::kDown, model, current, 60);
  ui::applyAction(state, Action::kUp, model, current, 80);
  TEST_ASSERT_EQUAL_UINT8(kLeague, state.table_cycle.competition);
  TEST_ASSERT_EQUAL_INT(0, tableView(state, model, current).first);
}

void test_automatic_tables_page_then_change_competition_at_the_interval() {
  table(kLeague, 18);
  table(kCup, 4, 2);
  const auto sources = shown(true);
  const auto current = data(sources);
  settings::Model model;
  model.table_window = 7;
  model.table_page_s = 8;
  NavigationState state;
  TEST_ASSERT_EQUAL_INT(0,
                        ui::tableSelection(state, model, current, 1000).first);
  TEST_ASSERT_EQUAL_INT(0,
                        ui::tableSelection(state, model, current, 8999).first);
  TEST_ASSERT_EQUAL_INT(7,
                        ui::tableSelection(state, model, current, 9000).first);
  TEST_ASSERT_EQUAL_INT(11,
                        ui::tableSelection(state, model, current, 17000).first);
  auto selected = ui::tableSelection(state, model, current, 25000);
  TEST_ASSERT_EQUAL_UINT8(kCup, selected.competition);
  TEST_ASSERT_EQUAL_INT(0, selected.first);
  selected = ui::tableSelection(state, model, current, 33000);
  TEST_ASSERT_EQUAL_UINT8(kLeague, selected.competition);
  TEST_ASSERT_EQUAL_INT(0, selected.first);
}

void test_settings_save_resets_all_navigation_even_when_return_is_disabled() {
  table(kLeague, 18, 8);
  const auto sources = shown();
  const auto current = data(sources);
  settings::Model model;
  model.manual_return_s = 0;
  NavigationState state;
  state.default_screen = cfg::Screen::kTable;
  state.selected = cfg::Screen::kMatchday;
  state.manual = state.paged = true;
  state.scroll_down = false;
  state.table_first = 12;
  state.conference_page = 3;
  state.table_cycle = {kCup, 2, 111};
  state.matchday_cycle = {kLeague, 1, 222};
  state.matchday_first = static_cast<uint32_t>(kKickoff);
  state.input_at = 333;
  state.scroll_at = 444;
  ui::automatic(state, 10000, AutomaticReason::kSettings);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_FALSE(state.paged);
  TEST_ASSERT_TRUE(state.scroll_down);
  TEST_ASSERT_EQUAL(cfg::Screen::kTable, state.selected);
  TEST_ASSERT_EQUAL_INT(-1, state.table_first);
  TEST_ASSERT_EQUAL_UINT32(0, state.matchday_first);
  TEST_ASSERT_EQUAL_INT(0, state.conference_page);
  TEST_ASSERT_EQUAL_UINT8(0, state.table_cycle.competition);
  TEST_ASSERT_EQUAL_UINT8(0, state.matchday_cycle.competition);
  TEST_ASSERT_EQUAL_INT(0, state.table_cycle.page);
  TEST_ASSERT_EQUAL_INT(0, state.matchday_cycle.page);
  TEST_ASSERT_EQUAL_INT64(10000, state.table_cycle.at);
  TEST_ASSERT_EQUAL_INT64(10000, state.matchday_cycle.at);
  TEST_ASSERT_EQUAL_INT64(10000, state.input_at);
  TEST_ASSERT_EQUAL_INT64(10000, state.scroll_at);
  // The next automatic view uses the saved window, not the old offset.
  model.table_window = 7;
  TEST_ASSERT_EQUAL_INT(5,
                        ui::tableSelection(state, model, current, 10000).first);
}

void test_default_and_timeout_resume_cycles_with_their_existing_timers() {
  for (auto reason : {AutomaticReason::kDefault, AutomaticReason::kTimeout}) {
    NavigationState state;
    state.default_screen = cfg::Screen::kTable;
    state.selected = cfg::Screen::kMatchday;
    state.manual = state.paged = true;
    state.scroll_down = false;
    state.table_first = 6;
    state.matchday_first = static_cast<uint32_t>(kKickoff);
    state.table_cycle = {kLeague, 2, 111};
    state.matchday_cycle = {kCup, 3, 222};
    state.input_at = 333;
    state.scroll_at = 444;
    ui::automatic(state, 10000, reason);
    TEST_ASSERT_FALSE(state.manual);
    TEST_ASSERT_FALSE(state.paged);
    TEST_ASSERT_EQUAL(cfg::Screen::kTable, state.selected);
    TEST_ASSERT_EQUAL_INT(-1, state.table_first);
    TEST_ASSERT_EQUAL_UINT32(0, state.matchday_first);
    TEST_ASSERT_EQUAL_UINT8(kLeague, state.table_cycle.competition);
    TEST_ASSERT_EQUAL_INT(2, state.table_cycle.page);
    TEST_ASSERT_EQUAL_INT64(111, state.table_cycle.at);
    TEST_ASSERT_EQUAL_UINT8(kCup, state.matchday_cycle.competition);
    TEST_ASSERT_EQUAL_INT(3, state.matchday_cycle.page);
    TEST_ASSERT_EQUAL_INT64(222, state.matchday_cycle.at);
    TEST_ASSERT_EQUAL_INT64(333, state.input_at);
    TEST_ASSERT_EQUAL_INT64(444, state.scroll_at);
    TEST_ASSERT_EQUAL(reason != AutomaticReason::kTimeout, state.scroll_down);
  }
}

void test_two_matchday_down_actions_before_a_render_move_two_pages() {
  matchday(kLeague, 1, kKickoff, 7);
  const auto sources = shown();
  const auto current = data(sources);
  settings::Model model;
  model.visible_matches = 2;
  NavigationState state;
  state.selected = cfg::Screen::kMatchday;
  assertDay(dayView(state, model, current), kLeague, 0, 4, kKickoff);
  ui::applyAction(state, Action::kDown, model, current, 20);
  TEST_ASSERT_EQUAL_INT(1, state.matchday_cycle.page);
  ui::applyAction(state, Action::kDown, model, current, 40);
  assertDay(dayView(state, model, current, 500), kLeague, 2, 4, kKickoff);
}

void test_matchday_actions_keep_order_across_unequal_competition_pages() {
  matchday(kLeague, 1, kKickoff, 5);  // Three pages.
  matchday(kCup, 21, kKickoff, 3);    // Two pages.
  const auto sources = shown(true);
  const auto current = data(sources);
  settings::Model model;
  model.visible_matches = 2;
  NavigationState state;
  state.selected = cfg::Screen::kMatchday;
  assertDay(dayView(state, model, current), kLeague, 0, 3, kKickoff);
  // Up crosses to the previous competition's LAST page (OQ-277).
  ui::applyAction(state, Action::kUp, model, current, 20);
  assertDay(dayView(state, model, current), kCup, 1, 2, kKickoff);
  ui::applyAction(state, Action::kDown, model, current, 40);
  assertDay(dayView(state, model, current), kLeague, 0, 3, kKickoff);
  // Crossing forward then back returns to the previous LAST page, not 0.
  ui::applyAction(state, Action::kDown, model, current, 60);
  ui::applyAction(state, Action::kDown, model, current, 80);
  ui::applyAction(state, Action::kDown, model, current, 100);
  assertDay(dayView(state, model, current), kCup, 0, 2, kKickoff);
  ui::applyAction(state, Action::kUp, model, current, 120);
  assertDay(dayView(state, model, current), kLeague, 2, 3, kKickoff);
}

void test_matchday_buttons_visit_other_stored_days_before_competitions() {
  matchday(kLeague, 1, kKickoff, 3);
  matchday(kLeague, 11, kKickoff + 7 * kDay, 5);
  matchday(kCup, 21, kKickoff, 3);
  const auto sources = shown(true);
  const auto current = data(sources);
  settings::Model model;
  model.visible_matches = 2;
  NavigationState state;
  state.selected = cfg::Screen::kMatchday;
  assertDay(dayView(state, model, current), kLeague, 0, 2, kKickoff);
  ui::applyAction(state, Action::kDown, model, current, 20);
  ui::applyAction(state, Action::kDown, model, current, 40);
  assertDay(dayView(state, model, current), kLeague, 0, 3, kKickoff + 7 * kDay);
  ui::applyAction(state, Action::kUp, model, current, 60);
  assertDay(dayView(state, model, current), kLeague, 1, 2, kKickoff);
  // Automatic return drops the manually held next day.
  ui::automatic(state, 80, AutomaticReason::kTimeout);
  assertDay(dayView(state, model, current), kLeague, 1, 2, kKickoff);
}

void test_matchday_100_ms_repeats_are_applied_in_order_without_rendering() {
  matchday(kLeague, 1, kKickoff, 7);
  matchday(kCup, 21, kKickoff, 3);
  const auto sources = shown(true);
  const auto current = data(sources);
  settings::Model model;
  model.visible_matches = 2;
  NavigationState state;
  state.selected = cfg::Screen::kMatchday;
  dayView(state, model, current);
  ui::InputPolicy input;
  unsigned accepted = 0;
  for (int64_t now = 0; now <= 960; now += 20) {
    const auto emitted = input.update(true, now, false, 100);
    for (unsigned event = 0;
         event <= static_cast<unsigned>(ui::RawInput::kRepeat); ++event) {
      if (!(emitted & (1u << event))) continue;
      const auto action = ui::mapInput(cfg::InputRole::kDown,
                                       static_cast<ui::RawInput>(event), true);
      if (action == Action::kNone) continue;
      ui::applyAction(state, action, model, current, now);
      ++accepted;
      TEST_ASSERT_EQUAL_UINT8(accepted < 4 ? kLeague : kCup,
                              state.matchday_cycle.competition);
      TEST_ASSERT_EQUAL_INT(accepted < 4 ? accepted : 0,
                            state.matchday_cycle.page);
    }
  }
  // Debounce at 40 ms, long at 640 ms, repeats at 740/840/940 ms.
  TEST_ASSERT_EQUAL_UINT(4, accepted);
  assertDay(dayView(state, model, current, 1000), kCup, 0, 2, kKickoff);
}

void test_next_matchday_skips_held_only_competition_but_buttons_reach_it() {
  matchday(kLeague, 1, kKickoff + 7 * kDay, 5);
  matchday(kCup, 21, kKickoff, 3);
  const auto sources = shown(true);
  const auto current = data(sources, kFriday + kDay + 12 * 3600);
  settings::Model model;
  model.visible_matches = 2;
  model.matchday_mode = cfg::MatchdayMode::kNext;
  NavigationState state;
  state.selected = cfg::Screen::kMatchday;
  assertDay(dayView(state, model, current), kLeague, 0, 3, kKickoff + 7 * kDay);
  ui::applyAction(state, Action::kUp, model, current, 20);
  assertDay(dayView(state, model, current), kCup, 1, 2, kKickoff);
  ui::automatic(state, 40, AutomaticReason::kTimeout);
  assertDay(dayView(state, model, current), kLeague, 0, 3, kKickoff + 7 * kDay);
}

void test_fault_without_data_stays_in_both_cycles_as_a_page() {
  matchday(kLeague, 1, kKickoff, 3);
  table(kLeague, 8);
  const auto sources = shown(true);
  const uint8_t faults[] = {kCup};
  auto current = data(sources);
  current.faults = faults;
  current.fault_count = 1;
  settings::Model model;
  model.visible_matches = 2;
  NavigationState state;
  state.selected = cfg::Screen::kMatchday;
  dayView(state, model, current);
  ui::applyAction(state, Action::kUp, model, current, 20);
  auto selected = dayView(state, model, current);
  TEST_ASSERT_TRUE(selected.available);
  TEST_ASSERT_TRUE(selected.fault);
  TEST_ASSERT_EQUAL_UINT8(kCup, selected.competition);
  TEST_ASSERT_EQUAL_INT(0, selected.page);
  TEST_ASSERT_EQUAL_INT(1, selected.pages);
  ui::applyAction(state, Action::kDown, model, current, 40);
  assertDay(dayView(state, model, current), kLeague, 0, 2, kKickoff);
  state = NavigationState{};
  state.selected = cfg::Screen::kTable;
  tableView(state, model, current);
  ui::applyAction(state, Action::kUp, model, current, 60);
  const auto selected_table = tableView(state, model, current);
  TEST_ASSERT_TRUE(selected_table.fault);
  TEST_ASSERT_EQUAL_UINT8(kCup, selected_table.competition);
  ui::applyAction(state, Action::kDown, model, current, 80);
  TEST_ASSERT_EQUAL_UINT8(kLeague,
                          tableView(state, model, current).competition);
}

void test_conference_all_pages_wraps_both_directions_with_own_pin() {
  for (uint32_t id = 1; id <= 6; ++id) match(kLeague, id, kKickoff, id == 1);
  const auto sources = shown();
  const auto current = data(sources);
  settings::Model model;
  model.visible_matches = 3;
  model.pin_own_match = true;
  model.conference_mode = cfg::ConferenceMode::kAllPages;
  NavigationState state;
  state.selected = cfg::Screen::kLiveMulti;
  ui::applyAction(state, Action::kUp, model, current, 20);
  TEST_ASSERT_EQUAL_INT(2, state.conference_page);
  ui::applyAction(state, Action::kDown, model, current, 40);
  TEST_ASSERT_EQUAL_INT(0, state.conference_page);
  ui::applyAction(state, Action::kUp, model, current, 60);
  const auto members = view::conferenceAll(store, sources);
  football::ConferenceRequest request;
  request.manual = true;
  request.pin_own = true;
  request.per_page = 3;
  request.page = state.conference_page;
  const auto page = view::conferencePage(store, members, request);
  TEST_ASSERT_EQUAL_UINT8(3, page.pages);
  TEST_ASSERT_EQUAL_UINT32(1, store.match(page.rows[0]).id);
  TEST_ASSERT_EQUAL_UINT32(6, store.match(page.rows[1]).id);
}

void test_one_page_first_down_enters_first_then_wraps_in_kickoff_order() {
  for (uint32_t id = 1; id <= 5; ++id) match(kLeague, id, kKickoff);
  const auto sources = shown();
  const auto current = data(sources);
  settings::Model model;
  model.visible_matches = 2;
  model.pin_own_match = false;
  model.conference_mode = cfg::ConferenceMode::kOnePage;
  NavigationState state;
  state.selected = cfg::Screen::kLiveMulti;
  ui::applyAction(state, Action::kDown, model, current, 20);
  TEST_ASSERT_EQUAL_INT(0, state.conference_page);
  ui::applyAction(state, Action::kDown, model, current, 40);
  TEST_ASSERT_EQUAL_INT(1, state.conference_page);
  ui::applyAction(state, Action::kDown, model, current, 60);
  TEST_ASSERT_EQUAL_INT(2, state.conference_page);
  ui::applyAction(state, Action::kDown, model, current, 80);
  TEST_ASSERT_EQUAL_INT(0, state.conference_page);
}

void test_one_page_first_up_enters_last_then_wraps_to_first() {
  for (uint32_t id = 1; id <= 5; ++id) match(kLeague, id, kKickoff);
  const auto sources = shown();
  const auto current = data(sources);
  settings::Model model;
  model.visible_matches = 2;
  model.pin_own_match = false;
  model.conference_mode = cfg::ConferenceMode::kOnePage;
  NavigationState state;
  state.selected = cfg::Screen::kLiveMulti;
  ui::applyAction(state, Action::kUp, model, current, 20);
  TEST_ASSERT_EQUAL_INT(2, state.conference_page);
  ui::applyAction(state, Action::kDown, model, current, 40);
  TEST_ASSERT_EQUAL_INT(0, state.conference_page);
  ui::applyAction(state, Action::kUp, model, current, 60);
  TEST_ASSERT_EQUAL_INT(2, state.conference_page);
}

void test_conference_empty_and_single_page_actions_are_bounded() {
  const auto sources = shown();
  const auto current = data(sources);
  for (auto mode :
       {cfg::ConferenceMode::kAllPages, cfg::ConferenceMode::kOnePage}) {
    settings::Model model;
    model.visible_matches = 2;
    model.conference_mode = mode;
    for (bool single : {false, true}) {
      store.clear();
      if (single) match(kLeague, 1, kKickoff, true);
      NavigationState state;
      state.selected = cfg::Screen::kLiveMulti;
      for (auto action : {Action::kUp, Action::kDown, Action::kUp}) {
        ui::applyAction(state, action, model, current, 20);
        TEST_ASSERT_EQUAL_INT(0, state.conference_page);
      }
    }
  }
}

void test_screen_selection_uses_situation_priority_and_manual_override() {
  const auto sources = shown();
  auto current = data(sources);
  settings::Model model;
  NavigationState state;
  ui::synchronize(state, model, current, 3);
  TEST_ASSERT_EQUAL(cfg::Screen::kTable, state.selected);
  match(kLeague, 1, kKickoff);
  ui::synchronize(state, model, current, 3);
  TEST_ASSERT_EQUAL(cfg::Screen::kLiveMulti, state.selected);
  match(kLeague, 2, kKickoff, true);
  ui::synchronize(state, model, current, 3);
  TEST_ASSERT_EQUAL(cfg::Screen::kLiveSingle, state.selected);
  state.manual = true;
  state.selected = cfg::Screen::kCrest;
  ui::synchronize(state, model, current, 3);
  TEST_ASSERT_EQUAL(cfg::Screen::kCrest, state.selected);
  ui::synchronize(state, model, current, 0);
  TEST_ASSERT_EQUAL(cfg::Screen::kLiveSingle, state.selected);
  model.screens[static_cast<unsigned>(cfg::Screen::kLiveSingle)].enabled =
      false;
  ui::synchronize(state, model, current, 0);
  TEST_ASSERT_EQUAL(cfg::Screen::kLiveMulti, state.selected);
  state.manual = false;
  current.configured = false;
  ui::synchronize(state, model, current, 3);
  TEST_ASSERT_EQUAL(cfg::Screen::kCrest, state.selected);
}

void test_screen_buttons_skip_disabled_screens_and_default_restores_automatic() {
  const auto sources = shown();
  const auto current = data(sources);
  settings::Model model;
  model.screens[static_cast<unsigned>(cfg::Screen::kMatchday)].enabled = false;
  NavigationState state;
  state.selected = cfg::Screen::kLiveMulti;
  state.default_screen = cfg::Screen::kTable;
  ui::applyAction(state, Action::kNext, model, current, 20);
  TEST_ASSERT_EQUAL(cfg::Screen::kTable, state.selected);
  ui::applyAction(state, Action::kPrevious, model, current, 40);
  TEST_ASSERT_EQUAL(cfg::Screen::kLiveMulti, state.selected);
  ui::applyAction(state, Action::kDefault, model, current, 60);
  TEST_ASSERT_EQUAL(cfg::Screen::kTable, state.selected);
  TEST_ASSERT_FALSE(state.manual);
}
}  // namespace

void setUp() { store.clear(); }
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_own_table_first_press_moves_the_visible_row_at_every_edge);
  RUN_TEST(test_table_every_row_is_reached_and_one_competition_wraps);
  RUN_TEST(test_table_wrap_keeps_action_order_across_competitions);
  RUN_TEST(test_automatic_tables_page_then_change_competition_at_the_interval);
  RUN_TEST(
      test_settings_save_resets_all_navigation_even_when_return_is_disabled);
  RUN_TEST(test_default_and_timeout_resume_cycles_with_their_existing_timers);
  RUN_TEST(test_two_matchday_down_actions_before_a_render_move_two_pages);
  RUN_TEST(test_matchday_actions_keep_order_across_unequal_competition_pages);
  RUN_TEST(test_matchday_buttons_visit_other_stored_days_before_competitions);
  RUN_TEST(test_matchday_100_ms_repeats_are_applied_in_order_without_rendering);
  RUN_TEST(test_next_matchday_skips_held_only_competition_but_buttons_reach_it);
  RUN_TEST(test_fault_without_data_stays_in_both_cycles_as_a_page);
  RUN_TEST(test_conference_all_pages_wraps_both_directions_with_own_pin);
  RUN_TEST(test_one_page_first_down_enters_first_then_wraps_in_kickoff_order);
  RUN_TEST(test_one_page_first_up_enters_last_then_wraps_to_first);
  RUN_TEST(test_conference_empty_and_single_page_actions_are_bounded);
  RUN_TEST(test_screen_selection_uses_situation_priority_and_manual_override);
  RUN_TEST(
      test_screen_buttons_skip_disabled_screens_and_default_restores_automatic);
  return UNITY_END();
}
