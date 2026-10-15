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
constexpr auto kEspn = cfg::Provider::kEspn;

MatchStore store;

uint8_t team(uint8_t competition, cfg::Provider provider, uint32_t id) {
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
  return store.upsert(m, {kickoff, 0}, own ? "Own Club Full" : "",
                      own ? "Guest Full" : "");
}

view::Shown shownAll() {
  view::Shown shown;
  shown.add(view::Source{kLeague, kOlg, 40});
  shown.add(view::Source{kCup, kOlg, 40});
  return shown;
}

settings::Competition entry(uint8_t id, cfg::Provider provider) {
  settings::Competition e;
  e.id = id;
  e.active = true;
  e.primary.provider = provider;
  e.primary.competition.assign("bl1");
  e.primary.team.assign("40");
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

void test_own_match_choice_for_screen_and_overview() {
  store.clear();
  const int64_t now = kSaturday + 12 * kHour;
  add(kLeague, 1, 5, now - 3 * 24 * kHour, Phase::kFinished, true)
      ->finished_at = static_cast<uint32_t>(now - 3 * 24 * kHour + 2 * kHour);
  add(kCup, 2, 1, now + 4 * 24 * kHour, Phase::kScheduled, true);
  add(kLeague, 3, 6, now + 20 * kMin, Phase::kScheduled, true);
  const auto shown = shownAll();
  bool window = false;
  int i = view::ownForScreen(store, shown, now, 30, 30, false, window);
  TEST_ASSERT_EQUAL_UINT32(3, store.match(i).id);
  TEST_ASSERT_TRUE(window);
  i = view::ownForScreen(store, shown, now, 10, 30, false, window);
  TEST_ASSERT_EQUAL_UINT32(3, store.match(i).id);  // closest kickoff
  TEST_ASSERT_FALSE(window);
  i = view::ownForOverview(store, shown, now);
  TEST_ASSERT_EQUAL_UINT32(3, store.match(i).id);
  // A running own match wins the overview over a closer one.
  add(kCup, 4, 1, now - 2 * kHour, Phase::kExtraTime, true);
  i = view::ownForOverview(store, shown, now);
  TEST_ASSERT_EQUAL_UINT32(4, store.match(i).id);
}

void test_names_short_then_full() {
  store.clear();
  StoredMatch* m = add(kLeague, 3, 6, kSaturday, Phase::kScheduled, true);
  TEST_ASSERT_EQUAL_STRING("Team 40", view::teamName(store, *m, true));
  TEST_ASSERT_EQUAL_STRING("Own Club Full", view::fullName(store, *m, true));
  TEST_ASSERT_EQUAL_STRING("Guest Full", view::fullName(store, *m, false));
  StoredMatch* other = add(kLeague, 4, 6, kSaturday, Phase::kScheduled);
  TEST_ASSERT_EQUAL_STRING("Team 8", view::fullName(store, *other, true));
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
  e.revision = 1;
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
  static constexpr uint32_t kOrder[] = {6, 4, 1, 2, 3};
  TEST_ASSERT_EQUAL_UINT(5, list.count);
  for (unsigned i = 0; i < list.count; ++i)
    TEST_ASSERT_EQUAL_UINT32(kOrder[i], store.eventAt(list.index[i]).identity);
  // Not an own match: no events.
  StoredMatch* other = add(kLeague, 4, 6, kSaturday, Phase::kLive);
  TEST_ASSERT_EQUAL_UINT(0, view::events(store, *other).count);
}

// OQ-192: a competition shows only on its enabled screens; only() keeps the
// sources of one screen.
void test_screen_switches_filter_the_sources() {
  store.clear();
  auto league = entry(kLeague, kOlg);
  league.screens = 1u << static_cast<unsigned>(cfg::CompetitionScreen::kTable);
  auto cup = entry(kCup, kOlg);
  view::Shown shown;
  shown.add(league);
  shown.add(cup);
  TEST_ASSERT_EQUAL_UINT(2, shown.count());
  TEST_ASSERT_EQUAL_HEX8(league.screens, shown.source(kLeague)->screens);
  const auto table = shown.only(cfg::CompetitionScreen::kTable);
  TEST_ASSERT_EQUAL_UINT(2, table.count());
  const auto konferenz = shown.only(cfg::CompetitionScreen::kConference);
  TEST_ASSERT_EQUAL_UINT(1, konferenz.count());
  TEST_ASSERT_NULL(konferenz.source(kLeague));
  TEST_ASSERT_NOT_NULL(konferenz.source(kCup));

  // Konferenz blocks are formed from the filtered sources only.
  const int64_t kickoff = kSaturday + 13 * kHour + 30 * kMin;
  add(kLeague, 1, 7, kickoff, Phase::kLive);
  add(kCup, 2, 1, kickoff + 2 * kHour, Phase::kScheduled);
  const auto members = view::conferenceMembers(
      store, konferenz, kickoff + 10 * kMin, {30, 15, false});
  TEST_ASSERT_EQUAL_UINT(0, members.count);
  const auto later = view::conferenceMembers(
      store, konferenz, kickoff + 2 * kHour, {30, 15, false});
  TEST_ASSERT_EQUAL_UINT(1, later.count);
}

// OQ-193/OQ-201: the matchday screen lists the first stored matchday (gap
// rule) of the shown provider; matches without kickoff and other
// providers' rows are left out.
void test_matchday_is_the_first_block_of_the_shown_provider() {
  store.clear();
  const int64_t friday = kFriday + 18 * kHour + 30 * kMin;
  add(kLeague, 1, 8, friday, Phase::kFinished);
  add(kLeague, 2, 8, kSaturday + 13 * kHour, Phase::kLive);
  add(kLeague, 3, 8, kSaturday + 24 * kHour + 15 * kHour, Phase::kScheduled);
  add(kLeague, 4, 0, kSaturday + 24 * kHour, Phase::kScheduled, false, kEspn);
  // Next matchday: more than 48 h after the last kickoff.
  add(kLeague, 5, 9, kSaturday + 7 * 24 * kHour, Phase::kScheduled);
  add(kLeague, 6, 9, 0, Phase::kScheduled);  // no kickoff
  add(kCup, 7, 1, kSaturday + 13 * kHour, Phase::kScheduled);
  const auto shown = shownAll();
  // During the matchday: the selected ("current") matchday is the first.
  const auto first = [&](uint8_t competition) {
    const auto reach =
        view::matchdays(store, shown, competition, cfg::MatchdayMode::kCurrent,
                        kSaturday + 14 * kHour, 0);
    return reach.count > 0 ? view::matchday(store, shown, competition,
                                            reach.span[reach.selected])
                           : view::List{};
  };
  const auto list = first(kLeague);
  TEST_ASSERT_EQUAL_UINT(3, list.count);
  TEST_ASSERT_EQUAL_UINT32(1, store.match(list.index[0]).id);
  TEST_ASSERT_EQUAL_UINT32(2, store.match(list.index[1]).id);
  TEST_ASSERT_EQUAL_UINT32(3, store.match(list.index[2]).id);
  TEST_ASSERT_EQUAL_UINT(1, first(kCup).count);
  TEST_ASSERT_EQUAL_UINT(0, first(9).count);
}

// OQ-244/267/268/269/271/275: which matchday the screen selects and which
// ones the buttons can reach, at local midnight (CEST), for both modes. A
// passed matchday that a fetch stores again is never selected or listed.
void test_matchday_selection_at_local_midnight() {
  store.clear();
  constexpr int32_t kCest = 7200;
  constexpr int64_t kDay = 24 * kHour;
  const int64_t md8 = kFriday + 18 * kHour + 30 * kMin;  // Fri 16 Oct
  const int64_t md8_last = kFriday + 2 * kDay + 15 * kHour + 30 * kMin;
  add(kLeague, 81, 8, md8, Phase::kFinished);
  add(kLeague, 82, 8, md8_last, Phase::kFinished);
  add(kLeague, 91, 9, md8 + 7 * kDay, Phase::kScheduled);
  add(kLeague, 101, 10, md8 + 14 * kDay, Phase::kScheduled, true);
  add(kLeague, 111, 11, md8 + 21 * kDay, Phase::kScheduled, true);
  // Matchday 7, already passed, stored again by a fetch.
  add(kLeague, 71, 7, md8 - 6 * kDay, Phase::kFinished);
  // A cup with one matchday on the same Sunday and nothing after it.
  add(kCup, 5, 2, md8_last, Phase::kFinished);
  const auto shown = shownAll();
  const auto reach = [&](uint8_t c, cfg::MatchdayMode mode, int64_t utc) {
    return view::matchdays(store, shown, c, mode, utc, kCest);
  };
  const auto selectedFirst = [&](const football::matchday::Reach& r) {
    return r.selected >= 0 ? r.span[r.selected].first : 0u;
  };
  constexpr auto kCurrent = cfg::MatchdayMode::kCurrent;
  constexpr auto kNext = cfg::MatchdayMode::kNext;
  const int64_t sunday_2359 = kFriday + 2 * kDay + 21 * kHour + 59 * kMin;
  const int64_t monday_0001 = sunday_2359 + 2 * kMin;
  const int64_t tuesday_0001 = monday_0001 + kDay;

  // Sunday 23:59 local: both modes show matchday 8; held, selected, next.
  for (auto mode : {kCurrent, kNext}) {
    const auto r = reach(kLeague, mode, sunday_2359);
    TEST_ASSERT_EQUAL_UINT8(2, r.count);
    TEST_ASSERT_EQUAL_INT8(0, r.selected);
    TEST_ASSERT_EQUAL_UINT32(md8, r.span[0].first);
    TEST_ASSERT_EQUAL_UINT32(md8_last, r.span[0].last);
    TEST_ASSERT_EQUAL_UINT32(md8 + 7 * kDay, r.span[1].first);
  }
  // Monday 00:01: "current" holds matchday 8 for the day (OQ-268), "next"
  // moves on (OQ-271); matchday 8 stays reachable by the buttons and only
  // the one after the selected one is (matchday 11 is not).
  auto r = reach(kLeague, kCurrent, monday_0001);
  TEST_ASSERT_EQUAL_UINT32(md8, selectedFirst(r));
  TEST_ASSERT_EQUAL_UINT8(2, r.count);
  r = reach(kLeague, kNext, monday_0001);
  TEST_ASSERT_EQUAL_UINT32(md8 + 7 * kDay, selectedFirst(r));
  TEST_ASSERT_EQUAL_UINT8(3, r.count);
  TEST_ASSERT_EQUAL_UINT32(md8, r.span[0].first);
  TEST_ASSERT_EQUAL_UINT32(md8 + 14 * kDay, r.span[2].first);
  // Tuesday 00:01: matchday 8 has passed in both modes.
  for (auto mode : {kCurrent, kNext}) {
    r = reach(kLeague, mode, tuesday_0001);
    TEST_ASSERT_EQUAL_UINT32(md8 + 7 * kDay, r.span[0].first);
    TEST_ASSERT_EQUAL_INT8(0, r.selected);
  }
  // The re-stored matchday 7 is never reached; the list of matchday 8 holds
  // only its own two matches.
  r = reach(kLeague, kCurrent, sunday_2359);
  const auto list = view::matchday(store, shown, kLeague, r.span[0]);
  TEST_ASSERT_EQUAL_UINT(2, list.count);
  TEST_ASSERT_EQUAL_UINT32(81, store.match(list.index[0]).id);
  TEST_ASSERT_EQUAL_UINT32(82, store.match(list.index[1]).id);
  // OQ-275: in "next" mode the cup has no upcoming matchday after Sunday
  // (skipped by the cycle), but its finished one stays reachable.
  r = reach(kCup, kNext, monday_0001);
  TEST_ASSERT_EQUAL_INT8(-1, r.selected);
  TEST_ASSERT_EQUAL_UINT8(1, r.count);
  r = reach(kCup, kCurrent, monday_0001);
  TEST_ASSERT_EQUAL_INT8(0, r.selected);
  TEST_ASSERT_EQUAL_UINT8(0, reach(kCup, kNext, tuesday_0001).count);
}

void test_demo_data() {
  view::demo(store, kLeague);
  view::Shown shown;
  shown.add(view::Source{kLeague, kOlg});
  TEST_ASSERT_EQUAL_UINT(1, view::conferenceAll(store, shown).count);
  bool window = false;
  const int own = view::ownForScreen(store, shown, kSaturday, 30, 30, true,
                                     window);
  TEST_ASSERT_TRUE(window);
  const auto& m = store.match(own);
  TEST_ASSERT_EQUAL(Phase::kLive, m.phase);
  TEST_ASSERT_EQUAL_UINT(67, m.minute);
  TEST_ASSERT_EQUAL_UINT(2, m.principal.home);
  TEST_ASSERT_EQUAL_UINT(1, m.principal.away);
  TEST_ASSERT_EQUAL_UINT32(0, m.score_changed);
  TEST_ASSERT_EQUAL_STRING("Bayern", view::teamName(store, m, true));
  TEST_ASSERT_EQUAL_STRING("Rot-Weiss Essen", view::fullName(store, m, false));
  const auto t = view::table(store, kLeague);
  TEST_ASSERT_EQUAL_UINT(5, t.count);
  TEST_ASSERT_EQUAL(2, t.own);
  TEST_ASSERT_EQUAL_STRING("FC Bayern München",
                           store.teamAt(store.row(t.row[2]).team).short_name.data);
  TEST_ASSERT_EQUAL_INT16(24, store.row(t.row[2]).points);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_entry_shows_its_source_rows);
  RUN_TEST(test_own_match_choice_for_screen_and_overview);
  RUN_TEST(test_names_short_then_full);
  RUN_TEST(test_table_rows_of_one_competition);
  RUN_TEST(test_events_in_match_order_without_hidden_ones);
  RUN_TEST(test_screen_switches_filter_the_sources);
  RUN_TEST(test_matchday_is_the_first_block_of_the_shown_provider);
  RUN_TEST(test_matchday_selection_at_local_midnight);
  RUN_TEST(test_demo_data);
  return UNITY_END();
}
