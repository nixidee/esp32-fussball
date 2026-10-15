// Host checks of the compact match store (football::MatchStore) and the
// store mappers. OpenLigaDB and ESPN inputs are recorded public responses
// (recorded.h, fetched 2026-10-14 and 2026-10-15). The API-Football and
// football-data.org samples and the synthetic matches below are written by the
// agent; they prove only the mapping and the store rules, not what a provider
// delivers or the owner's device acceptance.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstring>
#include <string_view>

#include "app_config.h"
#include "football_model.h"
#include "match_store.h"
#include "matchday.h"
#include "recorded.h"
#include "store_mappers.h"

namespace {

using football::Clock;
using football::EventType;
using football::Goals;
using football::kNoIndex;
using football::kNoValue;
using football::MapContext;
using football::MatchStore;
using football::Phase;
using football::Side;
using football::StoredEvent;
using football::StoredMatch;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonVariantConst;

constexpr uint8_t kBundesliga = 1;  // OpenLigaDB
constexpr uint8_t kEspnLeague = 2;  // ESPN
constexpr int32_t kCest = 7200;     // local offset in October

MatchStore store;
JsonDocument doc;

uint32_t utc(const char* text) {
  return static_cast<uint32_t>(football::parseUtc(text));
}

JsonVariantConst parse(const char* json) {
  doc.clear();
  TEST_ASSERT_FALSE(deserializeJson(doc, json));
  return doc.as<JsonVariantConst>();
}

MapContext context(uint8_t competition, cfg::Provider provider,
                   uint32_t club, const char* now = "2026-10-14T12:00:00Z",
                   uint32_t mono_s = 1000) {
  MapContext c;
  c.competition = competition;
  c.provider = provider;
  c.club = club;
  c.clock = {football::parseUtc(now), mono_s};
  return c;
}

// Every element of 'rows', as the provider client streams them.
std::size_t storeAll(JsonVariantConst rows, const MapContext& c) {
  std::size_t stored = 0;
  for (JsonVariantConst row : rows.as<JsonArrayConst>()) {
    TEST_ASSERT_TRUE(football::storeMatch(row, store, c));
    ++stored;
  }
  return stored;
}

bool storeTable(JsonVariantConst rows, const MapContext& c,
                bool complete = true) {
  store.beginTable(c.competition);
  for (JsonVariantConst row : rows.as<JsonArrayConst>())
    TEST_ASSERT_TRUE(football::storeTableRow(row, store, c));
  return store.commitTable(complete);
}

void loadBundesliga(uint32_t club) {
  const auto c = context(kBundesliga, cfg::Provider::kOpenLigaDb, club);
  storeAll(parse(recorded::kOpenLigaMatchday5), c);
  storeAll(parse(recorded::kOpenLigaMatchday6), c);
}

const StoredMatch& found(uint8_t competition, cfg::Provider provider,
                         uint32_t id) {
  const auto* m = store.find(competition, provider, id);
  TEST_ASSERT_NOT_NULL(m);
  return *m;
}

const StoredEvent& eventOf(const StoredMatch& m, std::size_t n) {
  TEST_ASSERT_NOT_EQUAL(kNoIndex, m.own);
  const auto& own = store.ownAt(m.own);
  TEST_ASSERT_TRUE(n < own.event_count);
  return store.eventAt(own.events[n]);
}

std::string_view teamName(uint8_t index) {
  TEST_ASSERT_NOT_EQUAL(kNoIndex, index);
  return store.teamAt(index).short_name.data;
}

void assertGoals(int home, int away, Goals g) {
  TEST_ASSERT_TRUE(g.valid());
  TEST_ASSERT_EQUAL_UINT8(home, g.home);
  TEST_ASSERT_EQUAL_UINT8(away, g.away);
}

// A synthetic match of 'competition' with numeric teams 'home'/'away'.
StoredMatch synthetic(uint8_t competition, uint32_t id, uint32_t kickoff,
                      uint32_t home = 1, uint32_t away = 2) {
  StoredMatch m;
  m.competition = competition;
  m.provider = cfg::Provider::kOpenLigaDb;
  m.id = id;
  m.kickoff = kickoff;
  m.home = store.team(competition, m.provider, home, "Home");
  m.away = store.team(competition, m.provider, away, "Away");
  return m;
}

// The n-th matchday of 'competition' with its number of matches (OQ-274:
// the checks of the removed MatchStore::matchday(), through the matchday
// module).
struct Matchday {
  uint32_t first = 0, last = 0;
  uint8_t count = 0;
};
bool matchday(uint8_t competition, std::size_t n, Matchday& out) {
  football::matchday::Span span;
  const auto all = [](const StoredMatch&) { return true; };
  for (std::size_t from = 0;
       football::matchday::next(store, competition, all, from, span);
       from = span.end) {
    if (n-- != 0) continue;
    out = {span.first, span.last, 0};
    for (std::size_t i = span.begin; i < span.end; ++i)
      if (store.match(i).competition == competition) ++out.count;
    return true;
  }
  return false;
}

StoredEvent goal(uint32_t identity, uint8_t minute, const char* player) {
  StoredEvent e;
  e.identity = identity;
  e.revision = identity;
  e.minute = minute;
  e.player.assign(player);
  return e;
}

}  // namespace

void setUp() { store.clear(); }
void tearDown() {}

void test_openliga_matchdays_from_kickoff_blocks() {
  loadBundesliga(0);
  TEST_ASSERT_EQUAL(18, store.matchCount());
  for (std::size_t i = 1; i < store.matchCount(); ++i)
    TEST_ASSERT_TRUE(store.match(i - 1).kickoff <= store.match(i).kickoff);
  Matchday day;
  TEST_ASSERT_TRUE(matchday(kBundesliga, 0, day));
  TEST_ASSERT_EQUAL_UINT32(utc("2026-10-09T18:30:00Z"), day.first);
  TEST_ASSERT_EQUAL_UINT32(utc("2026-10-11T15:30:00Z"), day.last);
  TEST_ASSERT_EQUAL_UINT8(9, day.count);
  TEST_ASSERT_TRUE(matchday(kBundesliga, 1, day));
  TEST_ASSERT_EQUAL_UINT32(utc("2026-10-16T18:30:00Z"), day.first);
  TEST_ASSERT_EQUAL_UINT32(utc("2026-10-18T15:30:00Z"), day.last);
  TEST_ASSERT_EQUAL_UINT8(9, day.count);
  TEST_ASSERT_FALSE(matchday(kBundesliga, 2, day));
  TEST_ASSERT_EQUAL(18, store.teamCount());
}

void test_openliga_match_fields() {
  loadBundesliga(0);
  const auto& m = found(kBundesliga, cfg::Provider::kOpenLigaDb, 83192);
  TEST_ASSERT_EQUAL_UINT32(utc("2026-10-09T18:30:00Z"), m.kickoff);
  TEST_ASSERT_TRUE(teamName(m.home) == "Dortmund");
  TEST_ASSERT_TRUE(teamName(m.away) == "Bremen");
  TEST_ASSERT_EQUAL(Phase::kFinished, m.phase);
  TEST_ASSERT_EQUAL_UINT8(0, m.estimated & football::kEstimatedPhase);
  assertGoals(2, 2, m.principal);
  assertGoals(0, 0, m.halftime);
  assertGoals(2, 2, m.regulation);
  TEST_ASSERT_FALSE(m.extra_time.valid());
  TEST_ASSERT_EQUAL_UINT8(5, m.round);
  TEST_ASSERT_NOT_EQUAL(kNoIndex, m.label);
  TEST_ASSERT_EQUAL_STRING("5. Spieltag", store.labelAt(m.label).data);
  // First seen finished and never seen running: the expected end.
  TEST_ASSERT_EQUAL_UINT32(m.kickoff + cfg::kExpectedMatchMinutes * 60,
                           m.finished_at);
  TEST_ASSERT_TRUE(m.estimated & football::kEstimatedFinish);
  TEST_ASSERT_FALSE(m.isOwn());
  TEST_ASSERT_EQUAL_UINT8(kNoIndex, m.own);
  TEST_ASSERT_EQUAL_UINT32(0, m.score_changed);
  // Not finished, kickoff ahead: estimated as scheduled.
  const auto& next = found(kBundesliga, cfg::Provider::kOpenLigaDb, 83203);
  TEST_ASSERT_EQUAL(Phase::kScheduled, next.phase);
  TEST_ASSERT_TRUE(next.estimated & football::kEstimatedPhase);
  TEST_ASSERT_FALSE(next.principal.valid());
  TEST_ASSERT_EQUAL_UINT8(6, next.round);
  TEST_ASSERT_EQUAL_STRING("6. Spieltag", store.labelAt(next.label).data);
}

void test_openliga_own_matches_get_slots_names_and_goals() {
  loadBundesliga(7);  // Borussia Dortmund
  const auto& m = found(kBundesliga, cfg::Provider::kOpenLigaDb, 83192);
  TEST_ASSERT_TRUE(m.isOwn());
  const auto& own = store.ownAt(m.own);
  TEST_ASSERT_EQUAL_UINT32(83192, own.match);
  TEST_ASSERT_EQUAL_STRING("Borussia Dortmund", own.home_name.data);
  TEST_ASSERT_EQUAL_STRING("SV Werder Bremen", own.away_name.data);
  TEST_ASSERT_EQUAL_UINT8(4, own.event_count);
  const auto& first = eventOf(m, 0);
  TEST_ASSERT_EQUAL_UINT32(149305, first.identity);
  TEST_ASSERT_EQUAL(EventType::kGoal, first.type);
  TEST_ASSERT_EQUAL_UINT8(80, first.minute);
  TEST_ASSERT_EQUAL_STRING("Daniel Svensson", first.player.data);
  assertGoals(1, 0, first.after);
  TEST_ASSERT_EQUAL(Side::kHome, first.beneficiary);
  TEST_ASSERT_EQUAL(Side::kHome, first.actor);
  const auto& last = eventOf(m, 3);
  TEST_ASSERT_EQUAL_STRING("N. Füllkrug", last.player.data);
  assertGoals(2, 2, last.after);
  TEST_ASSERT_EQUAL(Side::kAway, last.beneficiary);
  // The next own match has a slot, no events yet; others have none.
  const auto& next = found(kBundesliga, cfg::Provider::kOpenLigaDb, 83204);
  TEST_ASSERT_TRUE(next.isOwn());
  TEST_ASSERT_NOT_EQUAL(m.own, next.own);
  TEST_ASSERT_EQUAL_UINT8(0, store.ownAt(next.own).event_count);
  TEST_ASSERT_EQUAL_STRING("1. FC Union Berlin",
                           store.ownAt(next.own).home_name.data);
  const auto& other = found(kBundesliga, cfg::Provider::kOpenLigaDb, 83196);
  TEST_ASSERT_EQUAL_UINT8(kNoIndex, other.own);
}

void test_openliga_own_goal_benefits_the_other_side() {
  loadBundesliga(175);  // TSG Hoffenheim
  const auto& m = found(kBundesliga, cfg::Provider::kOpenLigaDb, 83194);
  const auto& own_goal = eventOf(m, 1);
  TEST_ASSERT_EQUAL(EventType::kOwnGoal, own_goal.type);
  TEST_ASSERT_EQUAL_STRING("S. Bornauw", own_goal.player.data);
  TEST_ASSERT_EQUAL(Side::kHome, own_goal.beneficiary);
  TEST_ASSERT_EQUAL(Side::kAway, own_goal.actor);
}

void test_expire_keeps_a_matchday_through_the_next_local_day() {
  loadBundesliga(7);
  const uint8_t slot =
      found(kBundesliga, cfg::Provider::kOpenLigaDb, 83192).own;
  // Last kickoff Sunday 17:30 local: kept through Monday.
  store.expire(kBundesliga, football::parseUtc("2026-10-12T12:00:00Z"), kCest);
  TEST_ASSERT_EQUAL(18, store.matchCount());
  // Tuesday 00:00 local.
  store.expire(kBundesliga, football::parseUtc("2026-10-12T22:00:00Z"), kCest);
  TEST_ASSERT_EQUAL(9, store.matchCount());
  for (std::size_t i = 0; i < store.matchCount(); ++i)
    TEST_ASSERT_EQUAL_UINT8(6, store.match(i).round);
  TEST_ASSERT_EQUAL_UINT32(0, store.ownAt(slot).match);
  for (uint8_t i = 0; i < cfg::kStoreEvents; ++i)
    TEST_ASSERT_EQUAL_UINT8(kNoIndex, store.eventAt(i).slot);
  Matchday day;
  TEST_ASSERT_TRUE(matchday(kBundesliga, 0, day));
  TEST_ASSERT_EQUAL_UINT32(utc("2026-10-16T18:30:00Z"), day.first);
}

void test_collect_frees_unreferenced_teams() {
  storeAll(parse(recorded::kOpenLigaMatchday5),
           context(kBundesliga, cfg::Provider::kOpenLigaDb, 0));
  TEST_ASSERT_EQUAL(18, store.teamCount());
  storeAll(parse(recorded::kEspnDay)["events"],
           context(kEspnLeague, cfg::Provider::kEspn, 0));
  TEST_ASSERT_EQUAL(22, store.teamCount());
  store.removeCompetition(kEspnLeague);
  TEST_ASSERT_EQUAL(22, store.teamCount());
  store.collect();
  TEST_ASSERT_EQUAL(18, store.teamCount());
  TEST_ASSERT_EQUAL(9, store.matchCount());
}

void test_openliga_table_with_estimated_positions() {
  const auto c = context(kBundesliga, cfg::Provider::kOpenLigaDb, 7);
  TEST_ASSERT_TRUE(storeTable(parse(recorded::kOpenLigaTable), c));
  TEST_ASSERT_EQUAL(18, store.rowCount());
  const auto& top = store.row(0);
  TEST_ASSERT_EQUAL_UINT8(kBundesliga, top.competition);
  TEST_ASSERT_TRUE(teamName(top.team) == "Freiburg");
  TEST_ASSERT_EQUAL_UINT8(1, top.position);
  TEST_ASSERT_TRUE(top.flags & football::kRowPositionEstimated);
  TEST_ASSERT_EQUAL_INT16(13, top.points);
  TEST_ASSERT_EQUAL_INT16(10, top.difference);
  TEST_ASSERT_EQUAL_UINT8(5, top.played);
  TEST_ASSERT_EQUAL_UINT8(14, top.goals_for);
  TEST_ASSERT_EQUAL_UINT8(4, top.goals_against);
  TEST_ASSERT_FALSE(top.flags & football::kRowOwn);
  const auto& dortmund = store.row(1);
  TEST_ASSERT_TRUE(teamName(dortmund.team) == "Dortmund");
  TEST_ASSERT_TRUE(dortmund.flags & football::kRowOwn);
  TEST_ASSERT_EQUAL_UINT8(2, dortmund.position);
  TEST_ASSERT_EQUAL_INT16(-11, store.row(17).difference);
  TEST_ASSERT_EQUAL_UINT8(18, store.row(17).position);
}

void test_table_survives_a_broken_or_empty_response() {
  const auto c = context(kBundesliga, cfg::Provider::kOpenLigaDb, 7);
  TEST_ASSERT_TRUE(storeTable(parse(recorded::kOpenLigaTable), c));
  // Broken off after three rows.
  store.beginTable(kBundesliga);
  int n = 0;
  for (JsonVariantConst row :
       parse(recorded::kOpenLigaTable).as<JsonArrayConst>())
    if (n++ < 3) TEST_ASSERT_TRUE(football::storeTableRow(row, store, c));
  TEST_ASSERT_FALSE(store.commitTable(false));
  TEST_ASSERT_EQUAL(18, store.rowCount());
  TEST_ASSERT_TRUE(teamName(store.row(17).team) == "Gladbach");
  // Complete but empty.
  store.beginTable(kBundesliga);
  TEST_ASSERT_FALSE(store.commitTable(true));
  TEST_ASSERT_EQUAL(18, store.rowCount());
  TEST_ASSERT_EQUAL_UINT8(0, store.notices(kBundesliga));
}

void test_table_of_another_competition_is_kept() {
  TEST_ASSERT_TRUE(storeTable(parse(recorded::kOpenLigaTable),
                              context(kBundesliga, cfg::Provider::kOpenLigaDb,
                                      0)));
  const auto espn = context(kEspnLeague, cfg::Provider::kEspn, 0);
  TEST_ASSERT_TRUE(storeTable(
      parse(recorded::kEspnTable)["children"][0]["standings"]["entries"],
      espn));
  TEST_ASSERT_EQUAL(36, store.rowCount());
  // Replacing one table keeps the other and the row order.
  TEST_ASSERT_TRUE(storeTable(
      parse(recorded::kEspnTable)["children"][0]["standings"]["entries"],
      espn));
  TEST_ASSERT_EQUAL(36, store.rowCount());
  for (std::size_t i = 0; i < 18; ++i) {
    TEST_ASSERT_EQUAL_UINT8(kBundesliga, store.row(i).competition);
    TEST_ASSERT_EQUAL_UINT8(kEspnLeague, store.row(18 + i).competition);
    TEST_ASSERT_EQUAL_UINT8(i + 1, store.row(18 + i).position);
  }
}

void test_table_staging_makes_room_from_its_own_rows() {
  // Fill the row pool with the competition's own previous table.
  store.beginTable(kBundesliga);
  football::StoredRow row;
  row.team = store.team(kBundesliga, cfg::Provider::kOpenLigaDb, 1, "A");
  for (std::size_t i = 0; i < cfg::kStoreTableRows; ++i)
    TEST_ASSERT_TRUE(store.appendRow(row));
  TEST_ASSERT_TRUE(store.commitTable(true));
  TEST_ASSERT_EQUAL(cfg::kStoreTableRows, store.rowCount());
  const auto c = context(kBundesliga, cfg::Provider::kOpenLigaDb, 0);
  TEST_ASSERT_TRUE(storeTable(parse(recorded::kOpenLigaTable), c));
  TEST_ASSERT_EQUAL(18, store.rowCount());
  // Made room and then broke off: no table, noted.
  store.clear();
  store.beginTable(kBundesliga);
  for (std::size_t i = 0; i < cfg::kStoreTableRows; ++i)
    TEST_ASSERT_TRUE(store.appendRow(row));
  TEST_ASSERT_TRUE(store.commitTable(true));
  store.beginTable(kBundesliga);
  TEST_ASSERT_TRUE(store.appendRow(row));
  TEST_ASSERT_FALSE(store.commitTable(false));
  TEST_ASSERT_EQUAL(0, store.rowCount());
  TEST_ASSERT_TRUE(store.notices(kBundesliga) & football::kNoticeTable);
}

void test_espn_day_finished_match_and_own_events() {
  const auto c = context(kEspnLeague, cfg::Provider::kEspn, 3841);  // Augsburg
  TEST_ASSERT_EQUAL(2, storeAll(parse(recorded::kEspnDay)["events"], c));
  const auto& m = found(kEspnLeague, cfg::Provider::kEspn, 401884777);
  TEST_ASSERT_EQUAL(Phase::kFinished, m.phase);
  assertGoals(2, 2, m.principal);
  TEST_ASSERT_EQUAL_UINT8(kNoValue, m.minute);
  TEST_ASSERT_EQUAL_UINT32(utc("2026-10-10T13:30:00Z"), m.kickoff);
  TEST_ASSERT_TRUE(teamName(m.home) == "Augsburg");
  TEST_ASSERT_TRUE(teamName(m.away) == "Bayern");
  TEST_ASSERT_TRUE(m.isOwn());
  const auto& own = store.ownAt(m.own);
  TEST_ASSERT_EQUAL_STRING("FC Augsburg", own.home_name.data);
  TEST_ASSERT_EQUAL_STRING("Bayern Munich", own.away_name.data);
  TEST_ASSERT_EQUAL_UINT8(7, own.event_count);
  const auto& first = eventOf(m, 0);
  TEST_ASSERT_EQUAL(EventType::kGoal, first.type);
  TEST_ASSERT_EQUAL_STRING("Robin Fellhauer", first.player.data);
  TEST_ASSERT_EQUAL_UINT8(1, first.minute);
  TEST_ASSERT_EQUAL(Side::kHome, first.actor);
  TEST_ASSERT_EQUAL(Side::kHome, first.beneficiary);
  TEST_ASSERT_NOT_EQUAL(0, first.identity);
  const auto& card = eventOf(m, 1);
  TEST_ASSERT_EQUAL(EventType::kYellow, card.type);
  TEST_ASSERT_EQUAL(Side::kUnknown, card.beneficiary);
  const auto& equaliser = eventOf(m, 3);
  TEST_ASSERT_EQUAL_STRING("Ismael Saibari", equaliser.player.data);
  TEST_ASSERT_EQUAL(Side::kAway, equaliser.beneficiary);
  const auto& other = found(kEspnLeague, cfg::Provider::kEspn, 401884775);
  TEST_ASSERT_FALSE(other.isOwn());
  assertGoals(1, 0, other.principal);
}

void test_espn_upcoming_is_scheduled_without_score() {
  const auto c = context(kEspnLeague, cfg::Provider::kEspn, 0);
  storeAll(parse(recorded::kEspnUpcoming)["events"], c);
  const auto& m = found(kEspnLeague, cfg::Provider::kEspn, 401884770);
  TEST_ASSERT_EQUAL(Phase::kScheduled, m.phase);
  TEST_ASSERT_EQUAL_UINT8(0, m.estimated);
  TEST_ASSERT_FALSE(m.principal.valid());
  TEST_ASSERT_EQUAL_UINT32(utc("2026-10-16T18:30:00Z"), m.kickoff);
  TEST_ASSERT_TRUE(teamName(m.away) == "Cologne");
  TEST_ASSERT_EQUAL_UINT32(0, m.finished_at);
}

void test_espn_table_reported_positions() {
  const auto c = context(kEspnLeague, cfg::Provider::kEspn, 126);
  TEST_ASSERT_TRUE(storeTable(
      parse(recorded::kEspnTable)["children"][0]["standings"]["entries"], c));
  TEST_ASSERT_EQUAL(18, store.rowCount());
  const auto& top = store.row(0);
  TEST_ASSERT_TRUE(teamName(top.team) == "Freiburg");
  TEST_ASSERT_EQUAL_UINT8(1, top.position);
  TEST_ASSERT_FALSE(top.flags & football::kRowPositionEstimated);
  TEST_ASSERT_TRUE(top.flags & football::kRowOwn);
  TEST_ASSERT_EQUAL_INT16(13, top.points);
  TEST_ASSERT_EQUAL_UINT8(5, top.played);
  TEST_ASSERT_EQUAL_UINT8(4, top.won);
  TEST_ASSERT_EQUAL_UINT8(1, top.drawn);
  TEST_ASSERT_EQUAL_UINT8(0, top.lost);
  TEST_ASSERT_EQUAL_UINT8(14, top.goals_for);
  TEST_ASSERT_EQUAL_INT16(10, top.difference);
  TEST_ASSERT_EQUAL_INT16(-11, store.row(17).difference);
}

void test_score_change_time_and_final_whistle() {
  const Clock first{football::parseUtc("2026-10-14T18:40:00Z"), 100};
  auto m = synthetic(kBundesliga, 1, utc("2026-10-14T18:30:00Z"));
  m.phase = Phase::kLive;
  m.principal = {0, 0};
  TEST_ASSERT_NOT_NULL(store.upsert(m, first));
  TEST_ASSERT_EQUAL_UINT32(0, store.match(0).score_changed);
  m.principal = {1, 0};
  TEST_ASSERT_NOT_NULL(store.upsert(m, {first.utc + 60, 160}));
  TEST_ASSERT_EQUAL_UINT32(160, store.match(0).score_changed);
  TEST_ASSERT_NOT_NULL(store.upsert(m, {first.utc + 120, 220}));
  TEST_ASSERT_EQUAL_UINT32(160, store.match(0).score_changed);
  // Seen running, then finished: the time it was first seen finished.
  m.phase = Phase::kFinished;
  const int64_t whistle = football::parseUtc("2026-10-14T20:25:00Z");
  TEST_ASSERT_NOT_NULL(store.upsert(m, {whistle, 7000}));
  TEST_ASSERT_EQUAL_UINT32(whistle, store.match(0).finished_at);
  TEST_ASSERT_TRUE(store.match(0).estimated & football::kEstimatedFinish);
  TEST_ASSERT_NOT_NULL(store.upsert(m, {whistle + 600, 7600}));
  TEST_ASSERT_EQUAL_UINT32(whistle, store.match(0).finished_at);
}

void test_events_update_in_place_and_mark_removals_once() {
  auto m = synthetic(kBundesliga, 1, utc("2026-10-14T18:30:00Z"));
  m.flags |= football::kMatchOwn;
  const Clock clock{football::parseUtc("2026-10-14T19:00:00Z"), 1};
  const auto* stored = store.upsert(m, clock, "Home FC", "Away FC");
  TEST_ASSERT_NOT_NULL(stored);
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(goal(10, 5, "A"));
  store.addEvent(goal(20, 15, "B"));
  store.addEvent(goal(30, 25, "C"));
  store.endEvents();
  // B withdrawn, A corrected.
  auto corrected = goal(10, 6, "A");
  corrected.revision = 11;
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(corrected);
  store.addEvent(goal(30, 25, "C"));
  store.endEvents();
  const auto& own = store.ownAt(stored->own);
  TEST_ASSERT_EQUAL_UINT8(3, own.event_count);
  TEST_ASSERT_EQUAL_UINT8(6, eventOf(*stored, 0).minute);
  TEST_ASSERT_EQUAL_UINT32(30, eventOf(*stored, 1).identity);
  const auto& removed = eventOf(*stored, 2);
  TEST_ASSERT_EQUAL_UINT32(20, removed.identity);
  TEST_ASSERT_TRUE(removed.flags & football::kEventRemoved);
  TEST_ASSERT_EQUAL_UINT32(21, removed.revision);
  // The marker is delivered once, then dropped.
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(corrected);
  store.addEvent(goal(30, 25, "C"));
  store.endEvents();
  TEST_ASSERT_EQUAL_UINT8(2, store.ownAt(stored->own).event_count);
  std::size_t used = 0;
  for (uint8_t i = 0; i < cfg::kStoreEvents; ++i)
    used += store.eventAt(i).slot != kNoIndex;
  TEST_ASSERT_EQUAL(2, used);
}

void test_broken_event_stream_keeps_previous_events() {
  auto m = synthetic(kBundesliga, 1, utc("2026-10-14T18:30:00Z"));
  m.flags |= football::kMatchOwn;
  const Clock clock{football::parseUtc("2026-10-14T19:00:00Z"), 1};
  const auto* stored = store.upsert(m, clock);
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(goal(10, 5, "A"));
  store.addEvent(goal(20, 15, "B"));
  store.endEvents();
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(goal(30, 25, "C"));
  store.endEvents(false);
  const auto& own = store.ownAt(stored->own);
  TEST_ASSERT_EQUAL_UINT8(3, own.event_count);
  TEST_ASSERT_EQUAL_UINT32(10, eventOf(*stored, 0).identity);
  TEST_ASSERT_EQUAL_UINT32(20, eventOf(*stored, 1).identity);
  TEST_ASSERT_EQUAL_UINT32(30, eventOf(*stored, 2).identity);
  for (std::size_t n = 0; n < 3; ++n)
    TEST_ASSERT_FALSE(eventOf(*stored, n).flags & football::kEventRemoved);
}

void test_more_events_than_a_list_keep_the_latest() {
  auto m = synthetic(kBundesliga, 1, utc("2026-10-14T18:30:00Z"));
  m.flags |= football::kMatchOwn;
  const auto* stored =
      store.upsert(m, {football::parseUtc("2026-10-14T20:00:00Z"), 1});
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  for (uint32_t i = 1; i <= cfg::kMatchEvents + 2; ++i)
    store.addEvent(goal(i, static_cast<uint8_t>(i), "P"));
  store.endEvents();
  TEST_ASSERT_EQUAL_UINT8(cfg::kMatchEvents,
                          store.ownAt(stored->own).event_count);
  TEST_ASSERT_EQUAL_UINT32(3, eventOf(*stored, 0).identity);
  TEST_ASSERT_TRUE(stored->flags & football::kMatchEventsTruncated);
}

void test_full_store_evicts_the_farthest_other_match() {
  const uint32_t base = utc("2026-10-14T12:00:00Z");
  const Clock clock{base, 1};
  for (uint32_t i = 0; i < cfg::kStoreMatches; ++i)
    TEST_ASSERT_NOT_NULL(store.upsert(
        synthetic(kBundesliga, i + 1, base + (i + 1) * 3600), clock));
  TEST_ASSERT_EQUAL(cfg::kStoreMatches, store.matchCount());
  // Farther than every stored match: does not fit.
  TEST_ASSERT_NULL(store.upsert(
      synthetic(kBundesliga, 9000, base + 400 * 3600), clock));
  TEST_ASSERT_TRUE(store.notices(kBundesliga) & football::kNoticeMatches);
  // Nearer: the farthest stored match makes room.
  TEST_ASSERT_NOT_NULL(store.upsert(
      synthetic(kEspnLeague, 9001, base + 1800), clock));
  TEST_ASSERT_EQUAL(cfg::kStoreMatches, store.matchCount());
  TEST_ASSERT_NULL(
      store.find(kBundesliga, cfg::Provider::kOpenLigaDb, cfg::kStoreMatches));
  TEST_ASSERT_NOT_NULL(store.find(kEspnLeague, cfg::Provider::kOpenLigaDb, 9001));
  TEST_ASSERT_EQUAL_UINT32(9001, store.match(0).id);
  // An own match always fits.
  auto own = synthetic(kBundesliga, 9002, base + 500 * 3600);
  own.flags |= football::kMatchOwn;
  TEST_ASSERT_NOT_NULL(store.upsert(own, clock));
  TEST_ASSERT_NOT_NULL(store.find(kBundesliga, cfg::Provider::kOpenLigaDb, 9002));
}

void test_api_football_sample() {
  // Agent-written sample in the documented API-Football v3 shape.
  const char* fixtures = R"JSON({"response":[{"fixture":{"id":1208021,
    "date":"2026-10-17T14:00:00+00:00","status":{"short":"2H","elapsed":67}},
    "league":{"round":"Regular Season - 8"},
    "teams":{"home":{"id":33,"name":"Manchester United"},
             "away":{"id":40,"name":"Liverpool"}},
    "goals":{"home":1,"away":2},
    "score":{"halftime":{"home":1,"away":1},"fulltime":{"home":null,"away":null},
             "extratime":{"home":null,"away":null},
             "penalty":{"home":null,"away":null}}}]})JSON";
  const auto c = context(3, cfg::Provider::kApiFootball, 40,
                         "2026-10-17T15:15:00Z");
  storeAll(parse(fixtures)["response"], c);
  const auto& m = found(3, cfg::Provider::kApiFootball, 1208021);
  TEST_ASSERT_EQUAL(Phase::kLive, m.phase);
  TEST_ASSERT_EQUAL_UINT8(67, m.minute);
  assertGoals(1, 2, m.principal);
  assertGoals(1, 1, m.halftime);
  TEST_ASSERT_FALSE(m.regulation.valid());
  TEST_ASSERT_EQUAL_STRING("Regular Season - 8", store.labelAt(m.label).data);
  TEST_ASSERT_TRUE(teamName(m.home) == "Manchester United");
  TEST_ASSERT_TRUE(m.isOwn());
  const char* events = R"JSON({"response":[
    {"time":{"elapsed":12,"extra":null},"team":{"id":40},
     "player":{"name":"M. Salah"},"assist":{"name":null},
     "type":"Goal","detail":"Normal Goal"},
    {"time":{"elapsed":45,"extra":2},"team":{"id":33},
     "player":{"name":"L. Martinez"},"assist":{"name":null},
     "type":"Goal","detail":"Own Goal"},
    {"time":{"elapsed":60,"extra":null},"team":{"id":40},
     "player":{"name":"D. Szoboszlai"},"assist":{"name":"C. Jones"},
     "type":"subst","detail":"Substitution 1"},
    {"time":{"elapsed":61,"extra":null},"team":{"id":40},
     "player":{"name":"X"},"assist":{"name":null},
     "type":"Unknown","detail":""}]})JSON";
  TEST_ASSERT_TRUE(store.beginEvents(m));
  for (JsonVariantConst row : parse(events)["response"].as<JsonArrayConst>())
    football::storeEvent(row, store, m, cfg::Provider::kApiFootball);
  store.endEvents();
  TEST_ASSERT_EQUAL_UINT8(3, store.ownAt(m.own).event_count);
  TEST_ASSERT_EQUAL(Side::kAway, eventOf(m, 0).beneficiary);
  const auto& own_goal = eventOf(m, 1);
  TEST_ASSERT_EQUAL(EventType::kOwnGoal, own_goal.type);
  TEST_ASSERT_EQUAL(Side::kHome, own_goal.actor);
  TEST_ASSERT_EQUAL(Side::kAway, own_goal.beneficiary);
  TEST_ASSERT_EQUAL_UINT8(2, own_goal.added);
  const auto& sub = eventOf(m, 2);
  TEST_ASSERT_EQUAL(EventType::kSubstitution, sub.type);
  TEST_ASSERT_EQUAL_STRING("D. Szoboszlai", sub.player.data);
  TEST_ASSERT_EQUAL_STRING("C. Jones", sub.player_in.data);
  const char* standings = R"JSON({"response":[{"league":{"standings":[[
    {"rank":1,"team":{"id":40,"name":"Liverpool"},"points":19,"goalsDiff":11,
     "all":{"played":7,"win":6,"draw":1,"lose":0,
            "goals":{"for":16,"against":5}}}]]}}]})JSON";
  TEST_ASSERT_TRUE(storeTable(parse(standings)["response"], c));
  TEST_ASSERT_EQUAL(1, store.rowCount());
  TEST_ASSERT_EQUAL_INT16(19, store.row(0).points);
  TEST_ASSERT_EQUAL_UINT8(16, store.row(0).goals_for);
  TEST_ASSERT_TRUE(store.row(0).flags & football::kRowOwn);
}

void test_football_data_sample() {
  // Agent-written sample in the documented football-data.org v4 shape.
  const char* matches = R"JSON({"matches":[{"id":537785,
    "utcDate":"2026-10-15T19:00:00Z","status":"FINISHED","matchday":3,
    "stage":"LEAGUE_STAGE",
    "homeTeam":{"id":5,"name":"FC Bayern München","shortName":"Bayern"},
    "awayTeam":{"id":81,"name":"FC Barcelona","shortName":"Barça"},
    "score":{"duration":"PENALTY_SHOOTOUT","fullTime":{"home":6,"away":5},
             "halfTime":{"home":0,"away":1},
             "regularTime":{"home":1,"away":1},
             "extraTime":{"home":1,"away":1},
             "penalties":{"home":4,"away":3}}}]})JSON";
  const auto c = context(4, cfg::Provider::kFootballData, 0);
  storeAll(parse(matches)["matches"], c);
  const auto& m = found(4, cfg::Provider::kFootballData, 537785);
  TEST_ASSERT_EQUAL(Phase::kFinished, m.phase);
  TEST_ASSERT_EQUAL_UINT8(3, m.round);
  assertGoals(1, 1, m.regulation);
  assertGoals(2, 2, m.extra_time);
  assertGoals(2, 2, m.principal);
  assertGoals(4, 3, m.shootout);
  TEST_ASSERT_TRUE(teamName(m.away) == "Barça");
  const char* standings = R"JSON({"standings":[
    {"type":"HOME","table":[{"position":1,"team":{"id":64,"name":"X"}}]},
    {"type":"TOTAL","table":[{"position":1,
      "team":{"id":5,"name":"FC Bayern München","shortName":"Bayern"},
      "playedGames":3,"won":3,"draw":0,"lost":0,"points":9,"goalsFor":11,
      "goalsAgainst":2,"goalDifference":9}]}]})JSON";
  TEST_ASSERT_TRUE(storeTable(parse(standings)["standings"], c));
  TEST_ASSERT_EQUAL(1, store.rowCount());
  TEST_ASSERT_TRUE(teamName(store.row(0).team) == "Bayern");
  TEST_ASSERT_EQUAL_INT16(9, store.row(0).difference);
}

void test_row_without_match_id_is_rejected() {
  const auto c = context(kEspnLeague, cfg::Provider::kEspn, 0);
  TEST_ASSERT_FALSE(football::storeMatch(
      parse(R"({"id":"abc","date":"2026-10-16T18:30Z"})"), store, c));
  TEST_ASSERT_FALSE(football::storeMatch(parse(R"({"id":"0"})"), store, c));
  TEST_ASSERT_EQUAL(0, store.matchCount());
}

// The event 'identity' of 'm' (the store keeps the origins apart; the
// display order by minute is the view's).
const StoredEvent& eventWith(const StoredMatch& m, uint32_t identity) {
  const auto& own = store.ownAt(m.own);
  for (std::size_t n = 0; n < own.event_count; ++n)
    if (store.eventAt(own.events[n]).identity == identity)
      return store.eventAt(own.events[n]);
  TEST_FAIL_MESSAGE("event not found");
  return store.eventAt(0);
}

// Substitutions from ESPN's match summary (detail request): kept beside
// the list events, they give way when the slot is full.
void test_detail_events_keep_their_origin_and_give_way() {
  auto m = synthetic(kBundesliga, 1, utc("2026-10-14T18:30:00Z"));
  m.flags |= football::kMatchOwn;
  const Clock clock{football::parseUtc("2026-10-14T19:00:00Z"), 1};
  const auto* stored = store.upsert(m, clock);
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(goal(10, 5, "A"));
  store.endEvents();
  auto sub = goal(700, 26, "Out");
  sub.type = EventType::kSubstitution;
  sub.player_in.assign("In");
  TEST_ASSERT_TRUE(store.beginEvents(*stored, true));
  store.addEvent(sub);
  store.endEvents();
  const uint8_t slot = stored->own;
  TEST_ASSERT_EQUAL_UINT8(2, store.ownAt(slot).event_count);
  const auto& detail = eventWith(*stored, 700);
  TEST_ASSERT_TRUE(detail.flags & football::kEventDetail);
  TEST_ASSERT_EQUAL_STRING("In", detail.player_in.data);
  // The list stream keeps the detail events.
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(goal(10, 5, "A"));
  store.endEvents();
  TEST_ASSERT_EQUAL_UINT8(2, store.ownAt(slot).event_count);
  // A detail stream without the substitution removes only it.
  TEST_ASSERT_TRUE(store.beginEvents(*stored, true));
  store.endEvents();
  TEST_ASSERT_TRUE(eventWith(*stored, 700).flags & football::kEventRemoved);
  TEST_ASSERT_TRUE(store.beginEvents(*stored, true));
  store.addEvent(sub);
  store.endEvents();
  // A full slot keeps the substitution.
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(goal(10, 5, "A"));
  for (uint32_t i = 1; i <= cfg::kMatchEvents - 2; ++i)
    store.addEvent(goal(i, static_cast<uint8_t>(i), "P"));
  store.endEvents();
  const auto& own = store.ownAt(slot);
  TEST_ASSERT_EQUAL_UINT8(cfg::kMatchEvents, own.event_count);
  bool sub_kept = false;
  for (std::size_t n = 0; n < own.event_count; ++n)
    sub_kept |= eventOf(*stored, n).identity == 700;
  TEST_ASSERT_TRUE(sub_kept);
  // One too many: the substitution gives way.
  TEST_ASSERT_TRUE(store.beginEvents(*stored));
  store.addEvent(goal(10, 5, "A"));
  for (uint32_t i = 1; i <= cfg::kMatchEvents - 1; ++i)
    store.addEvent(goal(i, static_cast<uint8_t>(i), "P"));
  store.endEvents();
  TEST_ASSERT_EQUAL_UINT8(cfg::kMatchEvents, own.event_count);
  for (std::size_t n = 0; n < own.event_count; ++n)
    TEST_ASSERT_FALSE(eventOf(*stored, n).flags & football::kEventDetail);
}

void test_notice_flags_set_and_clear_only_their_bits() {
  store.flag(kBundesliga, football::kNoticeTable, true);
  store.flag(kBundesliga, football::kNoticeEvents, true);
  store.flag(kBundesliga, football::kNoticeEvents, false);
  TEST_ASSERT_EQUAL_UINT8(football::kNoticeTable, store.notices(kBundesliga));
  TEST_ASSERT_EQUAL_UINT8(0, store.notices(kEspnLeague));
}

// P11.19 B2: OpenLigaDB keeps the Champions League matchday of 2026-09-08
// unfinished and without a result; more than 24 h after a kickoff such a
// match marks the competition as not maintained (OQ-281).
void test_unmaintained_competition_from_a_real_response() {
  constexpr uint8_t kChampions = 3;
  const auto c = context(kChampions, cfg::Provider::kOpenLigaDb, 0,
                         "2026-10-15T12:00:00Z");
  TEST_ASSERT_EQUAL(32, storeAll(parse(recorded::kOpenLigaUnmaintained), c));
  const int64_t kickoff = utc("2026-09-08T19:00:00Z");
  TEST_ASSERT_TRUE(store.unmaintained(kChampions, utc("2026-10-15T12:00:00Z")));
  TEST_ASSERT_FALSE(
      store.unmaintained(kChampions, kickoff + cfg::kUnmaintainedAfterS));
  TEST_ASSERT_TRUE(
      store.unmaintained(kChampions, kickoff + cfg::kUnmaintainedAfterS + 1));
  // A maintained league with finished and upcoming matches is not marked,
  // nor is a competition without matches.
  loadBundesliga(0);
  TEST_ASSERT_FALSE(
      store.unmaintained(kBundesliga, utc("2026-10-15T12:00:00Z")));
  TEST_ASSERT_FALSE(
      store.unmaintained(kEspnLeague, utc("2026-10-15T12:00:00Z")));
}

void test_store_size_is_pinned() {
  // Static RAM of the store (OQ-227 budget); a change here is a budget
  // decision, not a test update.
  TEST_ASSERT_EQUAL(22728, sizeof(MatchStore));
  TEST_ASSERT_EQUAL(40, sizeof(StoredMatch));
  TEST_ASSERT_EQUAL(14, sizeof(football::StoredRow));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_openliga_matchdays_from_kickoff_blocks);
  RUN_TEST(test_openliga_match_fields);
  RUN_TEST(test_openliga_own_matches_get_slots_names_and_goals);
  RUN_TEST(test_openliga_own_goal_benefits_the_other_side);
  RUN_TEST(test_expire_keeps_a_matchday_through_the_next_local_day);
  RUN_TEST(test_collect_frees_unreferenced_teams);
  RUN_TEST(test_openliga_table_with_estimated_positions);
  RUN_TEST(test_table_survives_a_broken_or_empty_response);
  RUN_TEST(test_table_of_another_competition_is_kept);
  RUN_TEST(test_table_staging_makes_room_from_its_own_rows);
  RUN_TEST(test_espn_day_finished_match_and_own_events);
  RUN_TEST(test_espn_upcoming_is_scheduled_without_score);
  RUN_TEST(test_espn_table_reported_positions);
  RUN_TEST(test_score_change_time_and_final_whistle);
  RUN_TEST(test_events_update_in_place_and_mark_removals_once);
  RUN_TEST(test_broken_event_stream_keeps_previous_events);
  RUN_TEST(test_more_events_than_a_list_keep_the_latest);
  RUN_TEST(test_full_store_evicts_the_farthest_other_match);
  RUN_TEST(test_api_football_sample);
  RUN_TEST(test_football_data_sample);
  RUN_TEST(test_row_without_match_id_is_rejected);
  RUN_TEST(test_detail_events_keep_their_origin_and_give_way);
  RUN_TEST(test_notice_flags_set_and_clear_only_their_bits);
  RUN_TEST(test_unmaintained_competition_from_a_real_response);
  RUN_TEST(test_store_size_is_pinned);
  return UNITY_END();
}
