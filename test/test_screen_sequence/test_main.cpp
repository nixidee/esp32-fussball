// Pure sequence acceptance with synthetic content probes. A numbered manual
// projection tests the callback contract; it does not replace real table/round
// projection, browser or physical-display acceptance during the later cutover.
#include <unity.h>

#include <array>

#include "screen_sequence.h"

namespace {
namespace seq = ui::sequence;
namespace policy = settings::screen_policy;
using screen::Type;
using ui::Action;
policy::Policy rules;
settings::Competitions competitions;
football::selection::Selection selected;
seq::State state;
struct Item {
  seq::ContentKind kind = seq::ContentKind::kData;
  uint16_t automatic_pages = 1, held_pages = 1, anchor = 0;
};
std::array<std::array<Item, cfg::kCompetitionMax + 1>, screen::kTypeCount>
    items;
bool focused_only = false;

seq::Content probe(const seq::Entry& entry, seq::PageMode mode,
                   uint16_t visible_page, void*) {
  if (focused_only && entry.type == Type::kLiveSingle &&
      selected.phase == policy::Phase::kOwn &&
      entry.competition != selected.focus.competition)
    return {};
  const auto& item = items[screen::index(entry.type)][entry.competition];
  return {item.kind,
          mode == seq::PageMode::kManualHeld ? item.held_pages
                                             : item.automatic_pages,
          static_cast<uint16_t>(item.anchor + visible_page)};
}

seq::Input input() { return {rules, competitions, selected, probe}; }

void competition(uint8_t id) {
  settings::Competition c;
  c.id = id;
  c.incarnation = id;
  c.primary.competition.assign("league");
  TEST_ASSERT_TRUE(competitions.entries.push(c));
  rules.competitions[id - 1] = {id, id, true};
}

void enable(Type type, bool automatic = true, uint16_t seconds = 15) {
  for (auto& phase : rules.shared)
    phase[screen::index(type)] = {seconds, true, automatic};
}

void show(int64_t now) { seq::presented(state, state.revision, now); }
void tick(int64_t now, bool display = true) {
  seq::synchronize(state, input(), now);
  if (display) show(now);
}
void action(Action command, int64_t now, bool display = true) {
  seq::applyAction(state, input(), command, now);
  if (display) show(now);
}

void test_catalog_has_34_auto_32_manual_entries_and_one_shared_order() {
  for (uint8_t id = 1; id <= cfg::kCompetitionMax; ++id) competition(id);
  for (unsigned type = 0; type < screen::kTypeCount; ++type)
    enable(static_cast<Type>(type));
  rules.slide_count = 2;
  rules.slides[0] = 1;
  rules.slides[1] = 2;
  TEST_ASSERT_EQUAL_UINT8(34, seq::count(input(), false));
  TEST_ASSERT_EQUAL_UINT8(32, seq::count(input(), true));
  const auto first = seq::first(input(), false);
  TEST_ASSERT_EQUAL(Type::kCrest, first.type);
  const auto after_images = seq::adjacent(input(), {Type::kSlideshow}, true, 1);
  TEST_ASSERT_EQUAL(Type::kLiveSingle, after_images.type);
  TEST_ASSERT_EQUAL_UINT8(1, after_images.competition);
  const auto before_images =
      seq::adjacent(input(), {Type::kSlideshow}, true, -1);
  TEST_ASSERT_EQUAL(Type::kTable, before_images.type);
  TEST_ASSERT_EQUAL_UINT8(8, before_images.competition);
  const auto second = seq::adjacent(input(), after_images, true, 1);
  TEST_ASSERT_EQUAL(Type::kLiveSingle, second.type);
  TEST_ASSERT_EQUAL_UINT8(2, second.competition);
}

void test_empty_is_skipped_but_error_and_retained_warning_content_stay_eligible() {
  competition(1);
  competition(2);
  enable(Type::kTable);
  items[screen::index(Type::kTable)][1].kind = seq::ContentKind::kEmpty;
  items[screen::index(Type::kTable)][2].kind = seq::ContentKind::kError;
  tick(0);
  TEST_ASSERT_EQUAL_UINT8(2, state.entry.competition);
  tick(5000);
  const auto revision = state.revision;
  items[screen::index(Type::kTable)][1].kind = seq::ContentKind::kData;
  items[screen::index(Type::kTable)][2].kind = seq::ContentKind::kData;
  tick(6000);
  TEST_ASSERT_EQUAL_UINT8(2, state.entry.competition);
  TEST_ASSERT_EQUAL_UINT32(6000, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(revision, state.revision);
}

void test_every_phase_change_ends_manual_and_starts_first_even_a_long_crest() {
  competition(1);
  enable(Type::kCrest, true, 3600);
  enable(Type::kTable);
  tick(0);
  action(Action::kNext, 1000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  selected.phase = policy::Phase::kOwn;
  tick(2000);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_EQUAL(Type::kCrest, state.entry.type);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  action(Action::kNext, 3000);
  selected.phase = policy::Phase::kIdle;
  tick(4000);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_EQUAL(Type::kCrest, state.entry.type);
}

void test_same_phase_focus_replaces_visible_single_and_keeps_remaining_visit() {
  competition(1);
  competition(2);
  enable(Type::kLiveSingle, true, 30);
  selected.phase = policy::Phase::kOwn;
  selected.focus = {10, 1, cfg::Provider::kOpenLigaDb};
  focused_only = true;
  tick(0);
  tick(5000);
  selected.focus = {20, 2, cfg::Provider::kOpenLigaDb};
  tick(7000);
  TEST_ASSERT_EQUAL_UINT8(2, state.entry.competition);
  TEST_ASSERT_EQUAL_UINT32(7000, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(30000, state.total_ms);
}

void test_manual_next_is_relative_to_visible_entry_even_exactly_at_its_expiry() {
  competition(1);
  competition(2);
  enable(Type::kLiveSingle, false);
  enable(Type::kLiveMulti, false);
  enable(Type::kTable);
  tick(0);
  TEST_ASSERT_EQUAL_UINT8(1, state.entry.competition);
  action(Action::kPrevious, 15000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_EQUAL(Type::kLiveMulti, state.entry.type);
  TEST_ASSERT_EQUAL_UINT8(2, state.entry.competition);
  action(Action::kNext, 16000);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  TEST_ASSERT_EQUAL_UINT8(1, state.entry.competition);
}

void test_equal_subpages_require_visible_time_and_never_skip_unseen_outputs() {
  competition(1);
  enable(Type::kLiveMulti);
  items[screen::index(Type::kLiveMulti)][1].automatic_pages = 3;
  tick(0);
  tick(4999);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  tick(5000, false);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  tick(20000, false);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(5000, state.elapsed_ms);
  show(20000);
  tick(25000);
  TEST_ASSERT_EQUAL_UINT16(2, state.page);
  tick(30000);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
}

void test_manual_only_entry_uses_retained_duration_until_manual_page_hold() {
  competition(1);
  enable(Type::kLiveMulti, false, 30);
  enable(Type::kTable);
  auto& content = items[screen::index(Type::kLiveMulti)][1];
  content.automatic_pages = 3;
  content.held_pages = 10;
  content.anchor = 5;
  rules.manual_return_s = 0;
  tick(0);
  action(Action::kNext, 0);
  TEST_ASSERT_EQUAL(Type::kLiveMulti, state.entry.type);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_FALSE(state.paged);
  tick(10000);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  action(Action::kUp, 11000);
  TEST_ASSERT_TRUE(state.paged);
  TEST_ASSERT_EQUAL_UINT16(5, state.page);  // anchor5 + visible1 - one step
  tick(1000000);
  TEST_ASSERT_EQUAL_UINT16(5, state.page);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_EQUAL_UINT32(0,
                           seq::view(state, input(), 1000000).return_left_ms);
}

void test_full_table_moves_one_row_then_same_type_competition_at_boundaries() {
  competition(1);
  competition(2);
  enable(Type::kTable);
  auto& table1 = items[screen::index(Type::kTable)][1];
  table1.held_pages = 20;
  table1.anchor = 16;
  items[screen::index(Type::kTable)][2].held_pages = 30;
  tick(0);
  action(Action::kDown, 1000);
  TEST_ASSERT_EQUAL_UINT16(17, state.page);
  action(Action::kUp, 2000);
  TEST_ASSERT_EQUAL_UINT16(16, state.page);
  action(Action::kDown, 3000);
  action(Action::kDown, 4000);
  action(Action::kDown, 5000);
  action(Action::kDown, 6000);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  TEST_ASSERT_EQUAL_UINT8(2, state.entry.competition);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  action(Action::kUp, 7000);
  TEST_ASSERT_EQUAL_UINT8(1, state.entry.competition);
  TEST_ASSERT_EQUAL_UINT16(19, state.page);
}

void test_manual_matchday_projection_keeps_rounds_and_pages_before_next_competition() {
  competition(1);
  competition(2);
  enable(Type::kMatchday);
  auto& round1 = items[screen::index(Type::kMatchday)][1];
  round1.held_pages = 6;  // two held + two current + two next pages
  round1.anchor = 2;
  items[screen::index(Type::kMatchday)][2].held_pages = 4;
  tick(0);
  action(Action::kDown, 1000);
  TEST_ASSERT_EQUAL_UINT16(3, state.page);
  action(Action::kDown, 2000);
  TEST_ASSERT_EQUAL_UINT16(4, state.page);
  action(Action::kDown, 3000);
  TEST_ASSERT_EQUAL_UINT16(5, state.page);
  action(Action::kDown, 4000);
  TEST_ASSERT_EQUAL_UINT8(2, state.entry.competition);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
}

void test_manual_timeout_default_and_every_input_renewal_restart_first_auto() {
  competition(1);
  enable(Type::kCrest);
  enable(Type::kTable);
  rules.manual_return_s = 30;
  tick(0);
  action(Action::kNext, 1000);
  action(Action::kShowIp, 30000);
  tick(59000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_EQUAL_UINT32(1000,
                           seq::view(state, input(), 59000).return_left_ms);
  tick(60000);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_EQUAL(Type::kCrest, state.entry.type);
  action(Action::kNext, 61000);
  action(Action::kDefault, 62000);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_EQUAL(Type::kCrest, state.entry.type);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
}

void slides() {
  rules.slide_count = 4;
  rules.slides[0] = 1;
  rules.slides[1] = 2;
  rules.slides[2] = 3;
  rules.slides[3] = 4;
  rules.per_image_s = 15;
}

void test_normal_images_resume_after_last_actually_shown_without_uptime_modulo() {
  competition(1);
  enable(Type::kSlideshow, true, 30);
  enable(Type::kTable);
  slides();
  tick(0);
  TEST_ASSERT_EQUAL_UINT8(1, state.image_id);
  tick(15000);
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  tick(30000);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  tick(180000);
  TEST_ASSERT_EQUAL(Type::kSlideshow, state.entry.type);
  TEST_ASSERT_EQUAL_UINT8(3, state.image_id);
  tick(195000);
  TEST_ASSERT_EQUAL_UINT8(4, state.image_id);
}

void test_unseen_and_stale_presentations_never_advance_normal_image_cursor() {
  enable(Type::kSlideshow, true, 30);
  slides();
  tick(0, false);
  seq::presented(state, state.revision - 1, 0);
  tick(100000, false);
  TEST_ASSERT_EQUAL_UINT8(1, state.image_id);
  TEST_ASSERT_EQUAL_UINT8(0, state.next_image);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  show(100000);
  TEST_ASSERT_EQUAL_UINT8(1, state.next_image);
  tick(115000, false);
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  tick(200000, false);
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  TEST_ASSERT_EQUAL_UINT8(1, state.next_image);
}

void test_frame_entry_and_first_manual_navigation_use_last_confirmed_output() {
  competition(1);
  competition(2);
  enable(Type::kSlideshow, true, 30);
  enable(Type::kTable);
  slides();
  tick(0);
  tick(15000,
       false);  // Image2 is planned, image1 is still the displayed image.
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  rules.frame = true;
  tick(16000);
  TEST_ASSERT_EQUAL_UINT8(1, state.image_id);
  rules.frame = false;
  tick(17000);
  tick(32000);
  tick(47000, false);  // Table planned; second normal image still displayed.
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  action(Action::kNext, 48000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  TEST_ASSERT_EQUAL_UINT8(1, state.entry.competition);
}

void test_frame_preserves_normal_cursor_and_return_restarts_viewed_image_fully() {
  competition(1);
  enable(Type::kSlideshow, true, 30);
  enable(Type::kTable);
  slides();
  rules.manual_return_s = 30;
  tick(0);
  tick(15000);
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  rules.frame = true;
  tick(16000);
  TEST_ASSERT_TRUE(state.frame);
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  action(Action::kNext, 17000);
  TEST_ASSERT_EQUAL_UINT8(3, state.image_id);
  TEST_ASSERT_TRUE(state.manual);
  selected.phase = policy::Phase::kOwn;
  tick(46000);
  TEST_ASSERT_TRUE(state.manual);  // Football phases cannot interrupt frame.
  tick(47000);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_EQUAL_UINT8(3, state.image_id);
  TEST_ASSERT_EQUAL_UINT32(0, state.image_elapsed_ms);
  tick(61999);
  TEST_ASSERT_EQUAL_UINT8(3, state.image_id);
  tick(62000);
  TEST_ASSERT_EQUAL_UINT8(4, state.image_id);
  rules.frame = false;
  tick(63000);
  TEST_ASSERT_FALSE(state.frame);
  TEST_ASSERT_EQUAL(Type::kSlideshow, state.entry.type);
  TEST_ASSERT_EQUAL_UINT8(3, state.image_id);
}

void test_frame_zero_return_holds_and_default_resumes_same_image() {
  slides();
  rules.frame = true;
  rules.manual_return_s = 0;
  tick(0);
  TEST_ASSERT_EQUAL(Type::kCrest, state.entry.type);
  action(Action::kDown, 1000);
  TEST_ASSERT_EQUAL_UINT8(1, state.image_id);
  tick(1000000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_EQUAL_UINT8(1, state.image_id);
  action(Action::kDefault, 1001000);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_EQUAL_UINT8(1, state.image_id);
  TEST_ASSERT_EQUAL_UINT32(0, state.image_elapsed_ms);
}

void test_empty_frame_never_enables_a_football_entry() {
  competition(1);
  enable(Type::kTable);
  rules.frame = true;
  rules.frame_crest = false;
  tick(0);
  TEST_ASSERT_TRUE(seq::view(state, input(), 0).empty);
  action(Action::kNext, 1000);
  TEST_ASSERT_FALSE(state.entry.valid());
  TEST_ASSERT_TRUE(state.frame);
}

void test_disabled_manual_only_empty_auto_sequence_does_not_reactivate_content() {
  competition(1);
  enable(Type::kTable, false);
  tick(0);
  TEST_ASSERT_FALSE(state.entry.valid());
  action(Action::kNext, 1000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  rules.shared[policy::index(selected.phase)][screen::index(Type::kTable)]
      .enabled = false;
  tick(2000);
  TEST_ASSERT_FALSE(state.entry.valid());
  TEST_ASSERT_FALSE(state.manual);
}

void test_growth_retains_visible_visit_clock_and_uses_all_pages_next_visit() {
  competition(1);
  enable(Type::kLiveMulti);
  enable(Type::kTable);
  auto& content = items[screen::index(Type::kLiveMulti)][1];
  content.automatic_pages = 3;
  tick(0);
  tick(2000);
  const auto revision = state.revision;
  content.automatic_pages = 5;
  tick(4000);
  TEST_ASSERT_EQUAL_UINT16(3, state.pages);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  TEST_ASSERT_EQUAL_UINT32(4000, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(15000, state.total_ms);
  TEST_ASSERT_EQUAL_UINT32(revision, state.revision);
  const auto visible = seq::view(state, input(), 4000);
  TEST_ASSERT_EQUAL_UINT16(3, visible.pages);
  TEST_ASSERT_EQUAL_UINT32(15000, visible.total_ms);
  tick(5000);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  tick(10000);
  TEST_ASSERT_EQUAL_UINT16(2, state.page);
  tick(15000);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  tick(30000);
  TEST_ASSERT_EQUAL(Type::kLiveMulti, state.entry.type);
  TEST_ASSERT_EQUAL_UINT16(5, state.pages);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(25000, state.total_ms);
  tick(35000);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
}

void test_growth_before_first_presentation_keeps_pending_visit_and_late_ticks() {
  competition(1);
  enable(Type::kLiveMulti);
  auto& content = items[screen::index(Type::kLiveMulti)][1];
  content.automatic_pages = 2;
  tick(0, false);
  const auto revision = state.revision;
  content.automatic_pages = 4;
  tick(100000, false);
  TEST_ASSERT_EQUAL_UINT16(2, state.pages);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(15000, state.total_ms);
  TEST_ASSERT_EQUAL_UINT32(revision, state.revision);
  show(100000);
  tick(107500, false);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(7500, state.elapsed_ms);
  tick(200000, false);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(7500, state.elapsed_ms);
  show(200000);
  tick(207500);
  TEST_ASSERT_EQUAL_UINT16(4, state.pages);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(20000, state.total_ms);
}

void test_manual_only_rotating_visit_defers_growth_until_its_next_visit() {
  competition(1);
  enable(Type::kLiveMulti, false, 30);
  enable(Type::kTable);
  rules.manual_return_s = 0;
  auto& content = items[screen::index(Type::kLiveMulti)][1];
  content.automatic_pages = 3;
  tick(0);
  action(Action::kNext, 1000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_FALSE(state.paged);
  content.automatic_pages = 8;
  tick(11000);
  TEST_ASSERT_EQUAL_UINT16(3, state.pages);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(10000, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(30000, state.total_ms);
  tick(21000);
  TEST_ASSERT_EQUAL_UINT16(2, state.page);
  tick(31000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_FALSE(state.paged);
  TEST_ASSERT_EQUAL(Type::kLiveMulti, state.entry.type);
  TEST_ASSERT_EQUAL_UINT16(8, state.pages);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(40000, state.total_ms);
}

void test_growth_does_not_block_manual_navigation_or_its_return_timer() {
  competition(1);
  enable(Type::kLiveMulti, false);
  enable(Type::kTable);
  rules.manual_return_s = 10;
  tick(0);
  items[screen::index(Type::kTable)][1].automatic_pages = 5;
  action(Action::kNext, 1000);
  TEST_ASSERT_EQUAL(Type::kLiveMulti, state.entry.type);
  TEST_ASSERT_TRUE(state.manual);
  items[screen::index(Type::kLiveMulti)][1].automatic_pages = 4;
  action(Action::kShowIp, 5000);
  TEST_ASSERT_EQUAL_UINT32(4000, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(10000,
                           seq::view(state, input(), 5000).return_left_ms);
  tick(14999);
  TEST_ASSERT_TRUE(state.manual);
  tick(15000);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  TEST_ASSERT_EQUAL_UINT16(5, state.pages);
}

void test_held_manual_projection_exposes_growth_and_preserves_visible_position() {
  competition(1);
  enable(Type::kTable);
  rules.manual_return_s = 0;
  auto& content = items[screen::index(Type::kTable)][1];
  content.held_pages = 3;
  tick(0);
  action(Action::kDown, 1000);
  TEST_ASSERT_TRUE(state.paged);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  const auto revision = state.revision;
  content.held_pages = 5;
  tick(100000);
  TEST_ASSERT_EQUAL_UINT16(5, state.pages);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(revision, state.revision);
  action(Action::kDown, 101000);
  action(Action::kDown, 102000);
  action(Action::kDown, 103000);
  TEST_ASSERT_EQUAL_UINT16(4, state.page);
}

void test_shrink_clamps_removed_page_retains_duration_and_regrowth_waits() {
  competition(1);
  enable(Type::kLiveMulti);
  auto& content = items[screen::index(Type::kLiveMulti)][1];
  content.automatic_pages = 4;
  tick(0);
  tick(5000);
  tick(10000);
  TEST_ASSERT_EQUAL_UINT16(2, state.page);
  const auto revision = state.revision;
  content.automatic_pages = 2;
  tick(11000);
  TEST_ASSERT_EQUAL_UINT16(2, state.pages);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(11000, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(20000, state.total_ms);
  TEST_ASSERT_NOT_EQUAL(revision, state.revision);
  content.automatic_pages = 4;
  tick(15000);
  TEST_ASSERT_EQUAL_UINT16(2, state.pages);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(15000, state.elapsed_ms);
  tick(20000);
  TEST_ASSERT_EQUAL_UINT16(4, state.pages);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(20000, state.total_ms);
}

void test_growth_at_expiry_restarts_visit_with_current_pages_without_another_tick() {
  competition(1);
  enable(Type::kLiveMulti);
  tick(0);
  tick(5000);
  items[screen::index(Type::kLiveMulti)][1].automatic_pages = 4;
  tick(15000);
  TEST_ASSERT_EQUAL_UINT16(4, state.pages);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  TEST_ASSERT_EQUAL_UINT32(20000, state.total_ms);
  tick(20000);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(5000, state.elapsed_ms);
}
void test_demo_next_advances_each_page_then_next_competition_without_manual() {
  competition(1);
  competition(2);
  enable(Type::kLiveMulti, true, 60);
  enable(Type::kTable, true, 3600);
  items[screen::index(Type::kLiveMulti)][1].automatic_pages = 3;
  const auto saved = rules;
  tick(0);
  action(Action::kAutomaticNext, 1000);
  TEST_ASSERT_EQUAL_UINT16(1, state.page);
  TEST_ASSERT_EQUAL_UINT32(20000, state.elapsed_ms);
  action(Action::kAutomaticNext, 2000);
  TEST_ASSERT_EQUAL_UINT16(2, state.page);
  action(Action::kAutomaticNext, 3000);
  TEST_ASSERT_EQUAL(Type::kLiveMulti, state.entry.type);
  TEST_ASSERT_EQUAL_UINT8(2, state.entry.competition);
  TEST_ASSERT_EQUAL_UINT16(0, state.page);
  action(Action::kAutomaticNext, 4000);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_TRUE(saved == rules);
}

void test_demo_next_resumes_first_auto_before_advancing_from_manual() {
  competition(1);
  enable(Type::kCrest, true, 3600);
  enable(Type::kLiveSingle, false);
  enable(Type::kTable);
  rules.manual_return_s = 0;
  tick(0);
  action(Action::kNext, 1000);
  TEST_ASSERT_TRUE(state.manual);
  action(Action::kAutomaticNext, 2000);
  TEST_ASSERT_EQUAL(Type::kCrest, state.entry.type);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_EQUAL_UINT32(0, state.elapsed_ms);
  action(Action::kAutomaticNext, 3000);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  TEST_ASSERT_FALSE(state.manual);
}

void test_demo_next_keeps_slideshow_images_and_later_continuation() {
  competition(1);
  enable(Type::kSlideshow, true, 30);
  enable(Type::kTable);
  slides();
  tick(0);
  TEST_ASSERT_EQUAL_UINT8(1, state.image_id);
  action(Action::kAutomaticNext, 1000);
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  action(Action::kAutomaticNext, 2000);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  action(Action::kAutomaticNext, 3000);
  TEST_ASSERT_EQUAL(Type::kSlideshow, state.entry.type);
  TEST_ASSERT_EQUAL_UINT8(3, state.image_id);
  TEST_ASSERT_FALSE(state.manual);
}

void test_demo_next_frame_moves_images_and_manual_return_keeps_frame_image() {
  slides();
  rules.frame = true;
  tick(0);
  action(Action::kAutomaticNext, 1000);
  TEST_ASSERT_EQUAL_UINT8(1, state.image_id);
  TEST_ASSERT_FALSE(state.manual);
  action(Action::kNext, 2000);
  TEST_ASSERT_TRUE(state.manual);
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  action(Action::kAutomaticNext, 3000);
  TEST_ASSERT_FALSE(state.manual);
  TEST_ASSERT_TRUE(state.frame);
  TEST_ASSERT_EQUAL_UINT8(2, state.image_id);
  action(Action::kAutomaticNext, 4000);
  TEST_ASSERT_EQUAL_UINT8(3, state.image_id);
  TEST_ASSERT_TRUE(state.frame);
}

void test_demo_next_never_skips_unseen_output_or_revives_empty_auto() {
  competition(1);
  enable(Type::kCrest);
  enable(Type::kTable);
  tick(0, false);
  const auto revision = state.revision;
  action(Action::kAutomaticNext, 1000, false);
  TEST_ASSERT_EQUAL(Type::kCrest, state.entry.type);
  TEST_ASSERT_EQUAL_UINT32(revision, state.revision);
  show(1000);
  action(Action::kAutomaticNext, 2000, false);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  action(Action::kAutomaticNext, 3000, false);
  TEST_ASSERT_EQUAL(Type::kTable, state.entry.type);
  for (auto& phase : rules.shared)
    for (auto& rule : phase) rule.automatic = false;
  action(Action::kAutomaticNext, 4000);
  TEST_ASSERT_FALSE(state.entry.valid());
  TEST_ASSERT_FALSE(state.manual);
}

void test_demo_next_action_payload_preserves_full_generation_and_rejects_unknown() {
  constexpr uint32_t version = 0xfedcba98;
  const auto payload = ui::actionPayload(Action::kAutomaticNext, version);
  TEST_ASSERT_EQUAL(Action::kAutomaticNext, ui::actionFromPayload(payload));
  TEST_ASSERT_EQUAL_UINT32(version, ui::actionGeneration(payload));
  TEST_ASSERT_EQUAL(Action::kNone, ui::actionFromPayload(
                                    ui::actionPayload(static_cast<Action>(255), version)));
}
}  // namespace

void setUp() {
  rules = policy::Policy{};
  for (auto& phase : rules.shared)
    for (auto& rule : phase) rule = {15, false, false};
  competitions.clear();
  selected = {};
  selected.phase = policy::Phase::kIdle;
  state = {};
  items = {};
  focused_only = false;
}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_catalog_has_34_auto_32_manual_entries_and_one_shared_order);
  RUN_TEST(
      test_empty_is_skipped_but_error_and_retained_warning_content_stay_eligible);
  RUN_TEST(
      test_every_phase_change_ends_manual_and_starts_first_even_a_long_crest);
  RUN_TEST(
      test_same_phase_focus_replaces_visible_single_and_keeps_remaining_visit);
  RUN_TEST(
      test_manual_next_is_relative_to_visible_entry_even_exactly_at_its_expiry);
  RUN_TEST(
      test_equal_subpages_require_visible_time_and_never_skip_unseen_outputs);
  RUN_TEST(
      test_manual_only_entry_uses_retained_duration_until_manual_page_hold);
  RUN_TEST(
      test_full_table_moves_one_row_then_same_type_competition_at_boundaries);
  RUN_TEST(
      test_manual_matchday_projection_keeps_rounds_and_pages_before_next_competition);
  RUN_TEST(
      test_manual_timeout_default_and_every_input_renewal_restart_first_auto);
  RUN_TEST(
      test_normal_images_resume_after_last_actually_shown_without_uptime_modulo);
  RUN_TEST(
      test_unseen_and_stale_presentations_never_advance_normal_image_cursor);
  RUN_TEST(
      test_frame_entry_and_first_manual_navigation_use_last_confirmed_output);
  RUN_TEST(
      test_frame_preserves_normal_cursor_and_return_restarts_viewed_image_fully);
  RUN_TEST(test_frame_zero_return_holds_and_default_resumes_same_image);
  RUN_TEST(test_empty_frame_never_enables_a_football_entry);
  RUN_TEST(
      test_disabled_manual_only_empty_auto_sequence_does_not_reactivate_content);
  RUN_TEST(
      test_growth_retains_visible_visit_clock_and_uses_all_pages_next_visit);
  RUN_TEST(
      test_growth_before_first_presentation_keeps_pending_visit_and_late_ticks);
  RUN_TEST(test_manual_only_rotating_visit_defers_growth_until_its_next_visit);
  RUN_TEST(test_growth_does_not_block_manual_navigation_or_its_return_timer);
  RUN_TEST(
      test_held_manual_projection_exposes_growth_and_preserves_visible_position);
  RUN_TEST(test_shrink_clamps_removed_page_retains_duration_and_regrowth_waits);
  RUN_TEST(
      test_growth_at_expiry_restarts_visit_with_current_pages_without_another_tick);
  RUN_TEST(test_demo_next_advances_each_page_then_next_competition_without_manual);
  RUN_TEST(test_demo_next_resumes_first_auto_before_advancing_from_manual);
  RUN_TEST(test_demo_next_keeps_slideshow_images_and_later_continuation);
  RUN_TEST(test_demo_next_frame_moves_images_and_manual_return_keeps_frame_image);
  RUN_TEST(test_demo_next_never_skips_unseen_output_or_revives_empty_auto);
  RUN_TEST(test_demo_next_action_payload_preserves_full_generation_and_rejects_unknown);
  return UNITY_END();
}
