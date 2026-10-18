// Host checks of the views from the compact store (football::view). The
// fixtures and events below are written by the agent and prove only the
// implementation, not what a provider delivers or the owner's device
// acceptance.

#include <unity.h>

#include <cstdint>
#include <cstring>

#include "app_config.h"
#include "conference.h"
#include "football_model.h"
#include "match_store.h"
#include "matchday.h"
#include "settings_model.h"
#include "store_views.h"

namespace {

using football::Clock;
using football::EventType;
using football::MatchStore;
using football::Phase;
using football::StoredEvent;
using football::StoredMatch;
using football::StoredRow;
namespace view = football::view;

constexpr int64_t kMin = 60;
constexpr int64_t kHour = 3600;
constexpr int64_t kFriday = 1792108800;  // 2026-10-16 00:00 UTC, a Friday
constexpr int64_t kSaturday = kFriday + 24 * kHour;
constexpr uint8_t kLeague = 3, kCup = 5;
constexpr auto kOlg = cfg::Provider::kOpenLigaDb;

MatchStore store;

football::TeamIndex team(uint8_t competition, cfg::Provider provider,
                         uint32_t id) {
  char name[16];
  snprintf(name, sizeof(name), "Team %u", static_cast<unsigned>(id));
  return store.team(competition, provider, id, name);
}

StoredMatch* add(uint8_t competition, uint32_t id, uint8_t round,
                 int64_t kickoff, Phase phase, bool own = false,
                 cfg::Provider provider = kOlg) {
  StoredMatch m;
  m.id = id;
  m.competition = competition;
  m.provider = provider;
  m.round = round;
  m.kickoff = static_cast<uint32_t>(kickoff);
  m.phase = phase;
  m.home = team(competition, provider, own ? 40 : id * 2);
  m.away = team(competition, provider, id * 2 + 1);
  if (own) m.flags = football::kMatchOwn;
  return store.upsert(m, {kickoff, 0});
}

view::Shown shownAll() {
  view::Shown shown;
  shown.add(view::Source{kLeague, kOlg, 40});
  shown.add(view::Source{kCup, kOlg, 40});
  return shown;
}

settings::CompetitionRoute entry(uint8_t id, cfg::Provider provider) {
  settings::CompetitionRoute e;
  e.id = id;
  e.active = true;
  e.primary.provider = provider;
  e.primary.competition.assign("bl1");
  e.club.team = 40;
  return e;
}

void test_entry_shows_its_source_rows() {
  store.clear();
  const int64_t kickoff = kSaturday + 13 * kHour + 30 * kMin;
  add(kLeague, 3, 6, kickoff, Phase::kScheduled, true);  // own

  view::Shown normal;
  normal.add(entry(kLeague, kOlg));
  TEST_ASSERT_EQUAL(kOlg, normal.source(kLeague)->provider);
  TEST_ASSERT_EQUAL_UINT32(40, normal.source(kLeague)->club);
  TEST_ASSERT_EQUAL_UINT(1, view::matchCount(store, normal, kLeague));

  // Inactive competitions show nothing.
  auto paused = entry(kCup, kOlg);
  paused.active = false;
  view::Shown none;
  none.add(paused);
  TEST_ASSERT_EQUAL_UINT(0, none.count());
}

void test_own_match_choice_for_overview() {
  store.clear();
  const int64_t now = kSaturday + 12 * kHour;
  add(kLeague, 1, 5, now - 3 * 24 * kHour, Phase::kFinished, true)
      ->finished_at = static_cast<uint32_t>(now - 3 * 24 * kHour + 2 * kHour);
  add(kCup, 2, 1, now + 4 * 24 * kHour, Phase::kScheduled, true);
  add(kLeague, 3, 6, now + 20 * kMin, Phase::kScheduled, true);
  const auto shown = shownAll();
  int i = view::ownForOverview(store, shown, now);
  TEST_ASSERT_EQUAL_UINT32(3, store.match(i).id);
  // A running own match wins the overview over a closer one.
  add(kCup, 4, 1, now - 2 * kHour, Phase::kExtraTime, true);
  i = view::ownForOverview(store, shown, now);
  TEST_ASSERT_EQUAL_UINT32(4, store.match(i).id);
}

void test_names_share_the_single_original_directory() {
  store.clear();
  StoredMatch* m = add(kLeague, 3, 6, kSaturday, Phase::kScheduled, true);
  TEST_ASSERT_EQUAL_STRING("Team 40", view::teamName(store, *m, true));
  TEST_ASSERT_EQUAL_STRING("Team 40", view::teamName(store, *m, true));
  TEST_ASSERT_EQUAL_STRING("Team 7", view::teamName(store, *m, false));
  store.teamAt(m->home).name.assign("");
  store.teamAt(m->away).name.assign("");
  TEST_ASSERT_EQUAL_STRING("", view::teamName(store, *m, true));
  TEST_ASSERT_EQUAL_STRING("", view::teamName(store, *m, false));
  StoredMatch* other = add(kLeague, 4, 6, kSaturday, Phase::kScheduled);
  TEST_ASSERT_EQUAL_STRING("Team 8", view::teamName(store, *other, true));
}

void test_table_rows_of_one_competition() {
  store.clear();
  for (uint8_t c : {kLeague, kCup}) {
    store.beginTable(c);
    for (uint8_t i = 0; i < 4; ++i) {
      StoredRow r;
      r.team = team(c, kOlg, 100u * c + i);
      r.position = i + 1;
      r.flags = c == kLeague && i == 2 ? football::kRowOwn : 0;
      store.appendRow(r);
    }
    store.commitTable(true);
  }
  const auto t = view::table(store, kLeague);
  TEST_ASSERT_EQUAL_UINT(4, t.count);
  TEST_ASSERT_EQUAL(2, t.own);
  for (uint8_t i = 0; i < t.count; ++i)
    TEST_ASSERT_EQUAL_UINT(i + 1, store.row(t.row[i]).position);
  TEST_ASSERT_EQUAL(-1, view::table(store, kCup).own);
  TEST_ASSERT_EQUAL_UINT(0, view::table(store, 7).count);
}

StoredEvent event(uint32_t identity, EventType type, uint8_t minute,
                  uint8_t added = football::kNoValue, uint8_t flags = 0) {
  StoredEvent e;
  e.identity = identity;
  e.type = type;
  e.minute = minute;
  e.added = added;
  e.flags = flags;
  return e;
}

void test_events_in_match_order_without_hidden_ones() {
  store.clear();
  StoredMatch* m = add(kLeague, 3, 6, kSaturday, Phase::kLive, true);
  const StoredMatch match = *m;
  // Primary: a goal, a card without minute after it, a shootout goal, a
  // goal in added time and an earlier card (out of order).
  TEST_ASSERT_TRUE(store.beginEvents(match));
  store.addEvent(event(1, EventType::kGoal, 52));
  store.addEvent(event(2, EventType::kYellow, football::kNoValue));
  store.addEvent(event(3, EventType::kPenaltyGoal, 120, football::kNoValue,
                       football::kEventShootout));
  store.addEvent(event(4, EventType::kGoal, 45, 2));
  store.addEvent(event(5, EventType::kYellow, 10));
  store.addEvent(event(6, EventType::kGoal, 45));
  store.endEvents();
  // A later primary list without event 5 leaves a removal marker.
  TEST_ASSERT_TRUE(store.beginEvents(match));
  for (const auto& e :
       {event(1, EventType::kGoal, 52),
        event(2, EventType::kYellow, football::kNoValue),
        event(3, EventType::kPenaltyGoal, 120, football::kNoValue,
              football::kEventShootout),
        event(4, EventType::kGoal, 45, 2), event(6, EventType::kGoal, 45)})
    store.addEvent(e);
  store.endEvents();

  const auto list = view::events(store, *store.find(kLeague, kOlg, 3));
  static constexpr uint32_t kOrder[] = {6, 4, 1, 2};
  TEST_ASSERT_EQUAL_UINT(4, list.count);
  for (unsigned i = 0; i < list.count; ++i)
    TEST_ASSERT_EQUAL_UINT32(kOrder[i], store.eventAt(list.index[i]).identity);
  const auto shots = view::shootout(store, *store.find(kLeague, kOlg, 3));
  TEST_ASSERT_EQUAL_UINT(1, shots.count);
  TEST_ASSERT_EQUAL_UINT32(3, store.eventAt(shots.index[0]).identity);
  // Not an own match: no events.
  StoredMatch* other = add(kLeague, 4, 6, kSaturday, Phase::kLive);
  TEST_ASSERT_EQUAL_UINT(0, view::events(store, *other).count);
}

view::List conference(unsigned count, bool own = false) {
  store.clear();
  for (unsigned i = 0; i < count; ++i)
    TEST_ASSERT_NOT_NULL(add(kLeague, i + 1, 1, kSaturday + i * kMin,
                             Phase::kLive, own && i == count / 2));
  view::List list;
  for (unsigned i = 0; i < store.matchCount(); ++i)
    list.index[list.count++] = static_cast<football::MatchIndex>(i);
  return list;
}

void test_conference_owner_page_distributions_preserve_count_and_order() {
  struct Example {
    uint8_t count, capacity, pages;
    std::array<uint8_t, 4> rows;
  };
  constexpr Example examples[] = {
      {16, 5, 4, {4, 4, 4, 4}}, {17, 5, 4, {5, 4, 4, 4}}, {11, 5, 3, {4, 4, 3}},
      {9, 4, 3, {3, 3, 3}},     {6, 5, 2, {3, 3}},        {5, 4, 2, {3, 2}}};
  for (const auto& example : examples) {
    const auto list = conference(example.count);
    football::ConferenceRequest request;
    request.per_page = example.capacity;
    request.pin_own = false;
    unsigned next = 0;
    for (unsigned i = 0; i < example.pages; ++i) {
      request.page = i;
      const auto page = view::conferencePage(store, list, request);
      TEST_ASSERT_EQUAL_UINT8(example.pages, page.pages);
      TEST_ASSERT_EQUAL_UINT8(example.rows[i], page.row_count);
      for (unsigned row = 0; row < page.row_count; ++row) {
        TEST_ASSERT_EQUAL_UINT32(++next, store.match(page.rows[row]).id);
        TEST_ASSERT_EQUAL_INT(
            i, view::conferencePageOf(store, list, request, page.rows[row]));
      }
    }
    TEST_ASSERT_EQUAL_UINT(example.count, next);
    request.page = example.pages;
    TEST_ASSERT_EQUAL_UINT8(0, view::conferencePage(store, list, request).page);
    request.manual = true;
    TEST_ASSERT_EQUAL_UINT8(example.pages - 1,
                            view::conferencePage(store, list, request).page);
  }
}

void test_conference_pin_is_repeated_without_losing_unique_matches() {
  struct Example {
    uint8_t unique, capacity;
    std::array<uint8_t, 3> visible;
  };
  // Capacity includes the pin. The parameterised six-row example partitions
  // eleven unpinned games as 4+4+3, then repeats the pin as visible 5+5+4.
  constexpr Example examples[] = {{10, 5, {4, 4, 4}}, {12, 6, {5, 5, 4}}};
  for (const auto& example : examples) {
    const auto list = conference(example.unique, true);
    football::ConferenceRequest request;
    request.per_page = example.capacity;
    std::array<bool, cfg::kStoreMatches> seen{};
    unsigned unique = 0;
    for (unsigned p = 0; p < 3; ++p) {
      request.page = p;
      const auto page = view::conferencePage(store, list, request);
      TEST_ASSERT_EQUAL_UINT8(3, page.pages);
      TEST_ASSERT_EQUAL_UINT8(example.visible[p], page.row_count);
      TEST_ASSERT_TRUE(store.match(page.rows[0]).isOwn());
      for (unsigned row = 0; row < page.row_count; ++row) {
        if (row) TEST_ASSERT_FALSE(seen[page.rows[row]]);
        if (!seen[page.rows[row]]) ++unique;
        seen[page.rows[row]] = true;
      }
    }
    TEST_ASSERT_EQUAL_UINT(example.unique, unique);
  }
}

void test_conference_minimum_two_keeps_pin_and_all_others_reachable() {
  const auto list = conference(4, true);
  football::ConferenceRequest request;
  request.per_page = 2;
  unsigned others = 0;
  for (unsigned p = 0; p < 3; ++p) {
    request.page = p;
    const auto page = view::conferencePage(store, list, request);
    TEST_ASSERT_EQUAL_UINT8(3, page.pages);
    TEST_ASSERT_EQUAL_UINT8(2, page.row_count);
    TEST_ASSERT_TRUE(store.match(page.rows[0]).isOwn());
    TEST_ASSERT_FALSE(store.match(page.rows[1]).isOwn());
    ++others;
  }
  TEST_ASSERT_EQUAL_UINT(3, others);
}

void test_conference_single_actual_fixture_needs_no_extra_row_or_page() {
  for (bool own : {false, true}) {
    const auto list = conference(1, own);
    football::ConferenceRequest request;
    request.per_page = 2;
    request.page = 5;
    const auto page = view::conferencePage(store, list, request);
    TEST_ASSERT_EQUAL_UINT8(1, page.pages);
    TEST_ASSERT_EQUAL_UINT8(1, page.row_count);
    TEST_ASSERT_EQUAL_UINT32(1, store.match(page.rows[0]).id);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_every_widened_match_index_is_reachable() {
  store.clear();
  view::List list;
  for (unsigned i = 0; i < cfg::kStoreMatches; ++i) {
    StoredMatch match;
    match.id = i + 1;
    match.competition = kLeague;
    match.kickoff = static_cast<uint32_t>(kSaturday);
    match.phase = Phase::kLive;
    if (i + 1 == cfg::kStoreMatches) match.flags = football::kMatchOwn;
    TEST_ASSERT_NOT_NULL(store.upsert(match, {kSaturday, 1}));
    list.index[list.count++] = static_cast<football::MatchIndex>(i);
  }
  football::ConferenceRequest request;
  request.mode = cfg::ConferenceMode::kAllPages;
  request.pin_own = true;
  request.per_page = 2;
  const auto first = view::conferencePage(store, list, request);
  TEST_ASSERT_EQUAL(cfg::kStoreMatches - 1, first.rows[0]);
  TEST_ASSERT_EQUAL(cfg::kStoreMatches - 1, first.pages);
  std::array<bool, cfg::kStoreMatches> reached{};
  for (unsigned page = 0; page < first.pages; ++page) {
    request.page = page;
    const auto visible = view::conferencePage(store, list, request);
    TEST_ASSERT_EQUAL(page, visible.page);
    for (unsigned row = 0; row < visible.row_count; ++row)
      reached[visible.rows[row]] = true;
  }
  for (bool found : reached) TEST_ASSERT_TRUE(found);
  football::matchday::Span span;
  TEST_ASSERT_TRUE(football::matchday::next(
      store, kLeague, [](const StoredMatch&) { return true; }, 0, span));
  TEST_ASSERT_EQUAL(cfg::kStoreMatches, span.end);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_widened_match_index_is_reachable);
  RUN_TEST(test_entry_shows_its_source_rows);
  RUN_TEST(test_own_match_choice_for_overview);
  RUN_TEST(test_names_share_the_single_original_directory);
  RUN_TEST(test_table_rows_of_one_competition);
  RUN_TEST(test_events_in_match_order_without_hidden_ones);
  RUN_TEST(test_conference_owner_page_distributions_preserve_count_and_order);
  RUN_TEST(test_conference_pin_is_repeated_without_losing_unique_matches);
  RUN_TEST(test_conference_minimum_two_keeps_pin_and_all_others_reachable);
  RUN_TEST(test_conference_single_actual_fixture_needs_no_extra_row_or_page);
  return UNITY_END();
}
