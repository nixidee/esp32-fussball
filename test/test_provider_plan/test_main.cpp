// Host checks of the provider request plans (football::plan). The ESPN
// match and table inputs are recorded public responses
// (../test_match_store/recorded.h, fetched 2026-10-14). The OpenLigaDB rows,
// change dates, ESPN match days and API-Football season samples below are
// written by the agent after the response shapes checked on 2026-10-14;
// they prove only the request plans and the consumers, not what a provider
// delivers or the owner's device acceptance.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstring>
#include <string>

#include "../test_match_store/recorded.h"
#include "app_config.h"
#include "football_model.h"
#include "match_store.h"
#include "provider_plan.h"
#include "request_scheduler.h"
#include "settings_model.h"

namespace {

using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonVariantConst;
using football::Clock;
using football::MatchStore;
using football::plan::Planner;
using football::plan::Request;
using football::plan::Sink;
using football::plan::Step;
using football::schedule::Job;
using football::schedule::Wanted;
using namespace recorded;

constexpr uint32_t kBoot = 1000;
const int64_t kBase = football::parseUtc("2026-10-14T12:00:00Z");

MatchStore store;
Planner planner;
settings::Competition entry;

Clock at(int64_t dt) { return {kBase + dt, static_cast<uint32_t>(kBoot + dt)}; }

void setSource(cfg::Provider provider, const char* competition,
               const char* team = "", const char* season = "") {
  entry = {};
  entry.id = 3;
  entry.primary.provider = provider;
  entry.primary.competition.assign(competition);
  entry.primary.team.assign(team);
  entry.primary.season.assign(season);
}

Job job(cfg::DataType type) {
  Job j;
  j.slot = 0;
  j.competition = entry.id;
  j.type = type;
  j.provider = entry.primary.provider;
  return j;
}

Request expect(cfg::DataType type, const char* url, int64_t dt = 0) {
  Request r;
  TEST_ASSERT_TRUE(planner.request(job(type), entry, store, at(dt), r));
  TEST_ASSERT_EQUAL_STRING(url, r.url);
  return r;
}

// The first array under 'key' at any depth (as fetchJson finds it).
JsonArrayConst find(JsonVariantConst v, const char* key) {
  if (v.is<ArduinoJson::JsonObjectConst>()) {
    for (auto pair : v.as<ArduinoJson::JsonObjectConst>()) {
      if (pair.key() == key && pair.value().is<JsonArrayConst>())
        return pair.value().as<JsonArrayConst>();
      const JsonArrayConst inner = find(pair.value(), key);
      if (!inner.isNull()) return inner;
    }
  } else if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst item : v.as<JsonArrayConst>()) {
      const JsonArrayConst inner = find(item, key);
      if (!inner.isNull()) return inner;
    }
  }
  return {};
}

// Streams 'json' as fetchJson would for 'r': the elements of the top-level
// array, of the array under key 'r.array', or the whole document.
bool feed(const Request& r, const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (r.array == nullptr) return Planner::consume(root, &planner);
  const JsonArrayConst list =
      r.array[0] == '\0' ? root.as<JsonArrayConst>() : find(root, r.array);
  if (list.isNull()) return r.optional;
  for (JsonVariantConst element : list)
    if (!Planner::consume(element, &planner)) return false;
  return true;
}

// One OpenLigaDB match row.
std::string olgRow(uint32_t id, const char* kickoff, int round,
                   const char* league = "bl1", int season = 2026,
                   uint32_t home = 91, uint32_t away = 65) {
  char row[512];
  snprintf(row, sizeof(row),
           R"({"matchID":%u,"matchDateTimeUTC":"%s","leagueShortcut":"%s",)"
           R"("leagueSeason":%d,"group":{"groupName":"%d. Spieltag",)"
           R"("groupOrderID":%d},"team1":{"teamId":%u,"teamName":"Home %u",)"
           R"("shortName":"H%u"},"team2":{"teamId":%u,"teamName":"Away %u",)"
           R"("shortName":"A%u"},"matchIsFinished":false,"matchResults":[],)"
           R"("goals":[]})",
           id, kickoff, league, season, round, round, home, home, home, away,
           away, away);
  return row;
}

bool stored(uint32_t id, cfg::Provider provider = cfg::Provider::kOpenLigaDb) {
  return store.find(entry.id, provider, id) != nullptr;
}

const char* kDays =
    R"({"eventDate":{"dates":["2026-10-09T07:00Z","2026-10-10T07:00Z",)"
    R"("2026-10-11T07:00Z","2026-10-16T07:00Z","2026-10-17T07:00Z",)"
    R"("2026-10-18T07:00Z","2026-10-23T07:00Z","2026-10-24T07:00Z",)"
    R"("2026-10-25T07:00Z","2026-10-30T07:00Z"]}})";

}  // namespace

void setUp() {
  store.clear();
  planner.reset();
}
void tearDown() {}

void test_encode_escapes_and_cuts_at_a_whole_escape() {
  char out[16];
  football::plan::encode("bl1", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("bl1", out);
  football::plan::encode("ger.1 ü", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("ger.1%20%C3%BC", out);
  football::plan::encode("a b", out, 5);
  TEST_ASSERT_EQUAL_STRING("a%20", out);
  football::plan::encode("a b", out, 4);
  TEST_ASSERT_EQUAL_STRING("a", out);
}

void test_matchday_days_current_and_next() {
  uint32_t days[10], out[8];
  const char* text[] = {"2026-10-09T07:00Z", "2026-10-10T07:00Z",
                        "2026-10-11T07:00Z", "2026-10-16T07:00Z",
                        "2026-10-17T07:00Z", "2026-10-18T07:00Z",
                        "2026-10-23T07:00Z", "2026-10-24T07:00Z",
                        "2026-10-25T07:00Z", "2026-10-30T07:00Z"};
  for (int i = 0; i < 10; ++i)
    days[i] = static_cast<uint32_t>(football::parseUtc(text[i]));
  // Wednesday after matchday 5: matchdays 6 and 7.
  TEST_ASSERT_EQUAL(6, football::plan::matchdayDays(days, 10, kBase, out, 8));
  TEST_ASSERT_EQUAL_UINT32(days[3], out[0]);
  TEST_ASSERT_EQUAL_UINT32(days[8], out[5]);
  // Sunday of matchday 5 (its last day has not ended): matchdays 5 and 6.
  const int64_t sunday = football::parseUtc("2026-10-11T18:00:00Z");
  TEST_ASSERT_EQUAL(6, football::plan::matchdayDays(days, 10, sunday, out, 8));
  TEST_ASSERT_EQUAL_UINT32(days[0], out[0]);
  // Capacity cuts the next matchday.
  TEST_ASSERT_EQUAL(4, football::plan::matchdayDays(days, 10, sunday, out, 4));
  // After the last day: none.
  const int64_t later = football::parseUtc("2026-11-01T00:00:00Z");
  TEST_ASSERT_EQUAL(0, football::plan::matchdayDays(days, 10, later, out, 8));
}

void test_change_hash() {
  const uint32_t a = football::plan::changeHash("2026-10-16T19:01:02.5");
  TEST_ASSERT_NOT_EQUAL(0, a);
  TEST_ASSERT_NOT_EQUAL(a, football::plan::changeHash("2026-10-16T19:01:03.5"));
  TEST_ASSERT_EQUAL_UINT32(a, football::plan::changeHash("2026-10-16T19:01:02.5"));
}

void test_wanted_types_and_costs() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "91");
  Wanted w;
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(3, w.id);
  // Every data type but the own-match events (they come with live data).
  TEST_ASSERT_EQUAL_UINT8(0x0f, w.types);
  TEST_ASSERT_EQUAL_UINT8(3, w.cost[0]);  // fixtures with club
  TEST_ASSERT_EQUAL_UINT8(2, w.cost[1]);  // table: season first
  TEST_ASSERT_EQUAL_UINT8(2, w.cost[2]);  // live: change date + round
  setSource(cfg::Provider::kEspn, "ger.1");
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(8, w.cost[0]);  // match days + 7 days
  TEST_ASSERT_EQUAL_UINT8(1, w.cost[1]);
  entry.active = false;
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(0, w.id);
  planner.wanted(0, nullptr, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(0, w.id);
}

void test_openliga_fixtures_run() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "91");
  Request r = expect(cfg::DataType::kFixtures,
                     "https://api.openligadb.de/getmatchdata/bl1");
  TEST_ASSERT_EQUAL_STRING("", r.array);
  const std::string round6 =
      "[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "," +
      olgRow(2, "2026-10-17T13:30:00Z", 6, "bl1", 2026, 7, 80) + "]";
  TEST_ASSERT_TRUE(feed(r, round6.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  const auto& state = planner.state(0);
  TEST_ASSERT_EQUAL_UINT8(6, state.round);
  TEST_ASSERT_EQUAL_STRING("2026", std::string(state.season.view()).c_str());
  TEST_ASSERT_TRUE(stored(1) && stored(2));

  r = expect(cfg::DataType::kFixtures,
             "https://api.openligadb.de/getmatchdata/bl1/2026/7", 2);
  const std::string round7 = "[" + olgRow(3, "2026-10-23T18:30:00Z", 7) + "]";
  TEST_ASSERT_TRUE(feed(r, round7.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(2)));

  // The club's fixtures: rows of other competitions and seasons are skipped.
  r = expect(cfg::DataType::kFixtures,
             "https://api.openligadb.de/getmatchesbyteamid/91/1/1", 4);
  const std::string club =
      "[" + olgRow(4, "2026-10-21T18:30:00Z", 4) + "," +
      olgRow(5, "2026-10-28T18:30:00Z", 2, "dfb") + "," +
      olgRow(6, "2025-10-28T18:30:00Z", 9, "BL1", 2025) + "]";
  TEST_ASSERT_TRUE(feed(r, club.c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(4)));
  TEST_ASSERT_TRUE(stored(3) && stored(4));
  TEST_ASSERT_FALSE(stored(5));
  TEST_ASSERT_FALSE(stored(6));

  // The next run starts again with the current round.
  expect(cfg::DataType::kFixtures,
         "https://api.openligadb.de/getmatchdata/bl1", 6);
}

void test_openliga_fixtures_without_club_and_explicit_season() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "", "2026");
  Request r = expect(cfg::DataType::kFixtures,
                     "https://api.openligadb.de/getmatchdata/bl1");
  const std::string rows = "[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "," +
                           olgRow(2, "2025-10-16T18:30:00Z", 6, "bl1", 2025) +
                           "]";
  TEST_ASSERT_TRUE(feed(r, rows.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  TEST_ASSERT_TRUE(stored(1));
  TEST_ASSERT_FALSE(stored(2));
  r = expect(cfg::DataType::kFixtures,
             "https://api.openligadb.de/getmatchdata/bl1/2026/7", 2);
  TEST_ASSERT_TRUE(feed(r, "[]"));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(2)));
}

void test_failure_ends_the_run() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "91");
  Request r = expect(cfg::DataType::kFixtures,
                     "https://api.openligadb.de/getmatchdata/bl1");
  TEST_ASSERT_TRUE(feed(r, ("[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "]")
                               .c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  expect(cfg::DataType::kFixtures,
         "https://api.openligadb.de/getmatchdata/bl1/2026/7", 2);
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(false, at(2)));
  expect(cfg::DataType::kFixtures,
         "https://api.openligadb.de/getmatchdata/bl1", 60);
}

void test_season_request_without_season_is_invalid() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1");
  const Request r = expect(cfg::DataType::kTable,
                           "https://api.openligadb.de/getmatchdata/bl1");
  TEST_ASSERT_TRUE(feed(r, "[]"));
  TEST_ASSERT_EQUAL(Step::kInvalid, planner.finish(true, at(0)));
}

void test_openliga_table() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "", "2026");
  const Request r = expect(cfg::DataType::kTable,
                           "https://api.openligadb.de/getbltable/bl1/2026");
  TEST_ASSERT_TRUE(Sink::kTable == r.sink);
  TEST_ASSERT_TRUE(feed(r, kOpenLigaTable));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(0)));
  TEST_ASSERT_EQUAL(18, store.rowCount());
  // A broken response keeps the stored table.
  const Request again = expect(cfg::DataType::kLiveTable,
                               "https://api.openligadb.de/getbltable/bl1/2026");
  TEST_ASSERT_TRUE(feed(again, "[]"));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(false, at(2)));
  TEST_ASSERT_EQUAL(18, store.rowCount());
}

void test_openliga_live_checks_the_change_date_per_round() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "", "2026");
  // A match of round 6 kicked off ten minutes ago.
  const int64_t now = football::parseUtc("2026-10-16T18:40:00Z") - kBase;
  Request r = expect(cfg::DataType::kFixtures,
                     "https://api.openligadb.de/getmatchdata/bl1", now);
  TEST_ASSERT_TRUE(feed(r, ("[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "]")
                               .c_str()));
  planner.finish(true, at(now));
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getlastchangedate/bl1/2026/6", now);
  TEST_ASSERT_NULL(r.array);
  TEST_ASSERT_TRUE(feed(r, R"("2026-10-16T18:39:00.1")"));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(now)));
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getmatchdata/bl1/2026/6", now + 2);
  TEST_ASSERT_TRUE(feed(r, "[]"));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now + 2)));
  // Unchanged a minute later: only the change date.
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getlastchangedate/bl1/2026/6",
             now + 60);
  TEST_ASSERT_TRUE(feed(r, R"("2026-10-16T18:39:00.1")"));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now + 60)));
  // Changed: the round again.
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getlastchangedate/bl1/2026/6",
             now + 120);
  TEST_ASSERT_TRUE(feed(r, R"("2026-10-16T18:41:30.7")"));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(now + 120)));
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getmatchdata/bl1/2026/6", now + 122);
  TEST_ASSERT_TRUE(feed(r, "[]"));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now + 122)));
  // Unchanged but the last full request is kOpenLigaDbFullRefreshS old.
  const int64_t late = now + 122 + cfg::kOpenLigaDbFullRefreshS;
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getlastchangedate/bl1/2026/6", late);
  TEST_ASSERT_TRUE(feed(r, R"("2026-10-16T18:41:30.7")"));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(late)));
  // A change date that is no string is an invalid response.
  setUp();
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "", "2026");
  r = expect(cfg::DataType::kFixtures,
             "https://api.openligadb.de/getmatchdata/bl1", now);
  feed(r, ("[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "]").c_str());
  planner.finish(true, at(now));
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getlastchangedate/bl1/2026/6", now);
  TEST_ASSERT_FALSE(feed(r, "[]"));
}

void test_espn_fixtures_from_match_days() {
  setSource(cfg::Provider::kEspn, "ger.1", "598");
  Request r = expect(cfg::DataType::kFixtures,
                     "https://sports.core.api.espn.com/v2/sports/soccer/"
                     "leagues/ger.1/calendar/ondays");
  TEST_ASSERT_EQUAL_STRING("dates", r.array);
  TEST_ASSERT_TRUE(Sink::kDays == r.sink);
  JsonDocument doc;
  deserializeJson(doc, kDays);
  for (JsonVariantConst day : doc["eventDate"]["dates"].as<JsonArrayConst>())
    TEST_ASSERT_TRUE(Planner::consume(day, &planner));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  TEST_ASSERT_EQUAL_UINT8(6, planner.state(0).day_count);
  const char* expected[] = {"20261016", "20261017", "20261018",
                            "20261023", "20261024", "20261025"};
  for (int i = 0; i < 6; ++i) {
    const std::string url =
        std::string("https://site.api.espn.com/apis/site/v2/sports/soccer/"
                    "ger.1/scoreboard?dates=") +
        expected[i];
    r = expect(cfg::DataType::kFixtures, url.c_str(), 2 + i);
    TEST_ASSERT_EQUAL_STRING("events", r.array);
    TEST_ASSERT_TRUE(feed(r, i == 0 ? kEspnUpcoming : R"({"events":[]})"));
    TEST_ASSERT_EQUAL(i < 5 ? Step::kMore : Step::kDone,
                      planner.finish(true, at(2 + i)));
  }
  TEST_ASSERT_TRUE(stored(401884770, cfg::Provider::kEspn));
  Wanted w;
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(7, w.cost[0]);
}

void test_espn_live_uses_the_match_day() {
  setSource(cfg::Provider::kEspn, "ger.1");
  // Kickoff 2026-10-16 18:30 UTC; ten minutes later.
  const int64_t now = football::parseUtc("2026-10-16T18:40:00Z") - kBase;
  football::MapContext c{entry.id, cfg::Provider::kEspn, 0, at(now)};
  JsonDocument doc;
  deserializeJson(doc, kEspnUpcoming);
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>())
    football::storeMatch(e, store, c);
  // Without stored match days: the scoreboard without date.
  Request r = expect(cfg::DataType::kLive,
                     "https://site.api.espn.com/apis/site/v2/sports/soccer/"
                     "ger.1/scoreboard",
                     now);
  TEST_ASSERT_TRUE(feed(r, kEspnUpcoming));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now)));
  // With the match days: the day holding the kickoff.
  r = expect(cfg::DataType::kFixtures,
             "https://sports.core.api.espn.com/v2/sports/soccer/leagues/"
             "ger.1/calendar/ondays",
             now);
  deserializeJson(doc, kDays);
  for (JsonVariantConst day : doc["eventDate"]["dates"].as<JsonArrayConst>())
    Planner::consume(day, &planner);
  planner.finish(true, at(now));
  r = expect(cfg::DataType::kLive,
             "https://site.api.espn.com/apis/site/v2/sports/soccer/"
             "ger.1/scoreboard?dates=20261016",
             now + 2);
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now + 2)));
}

void test_espn_table() {
  setSource(cfg::Provider::kEspn, "ger.1");
  Request r = expect(cfg::DataType::kTable,
                     "https://site.web.api.espn.com/apis/v2/sports/soccer/"
                     "ger.1/standings");
  TEST_ASSERT_EQUAL_STRING("entries", r.array);
  TEST_ASSERT_TRUE(r.optional);
  TEST_ASSERT_TRUE(feed(r, kEspnTable));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(0)));
  TEST_ASSERT_EQUAL(18, store.rowCount());
  setSource(cfg::Provider::kEspn, "ger.1", "", "2025");
  expect(cfg::DataType::kTable,
         "https://site.web.api.espn.com/apis/v2/sports/soccer/ger.1/"
         "standings?season=2025",
         4);
}

void test_api_football_resolves_the_season() {
  setSource(cfg::Provider::kApiFootball, "78");
  Request r = expect(cfg::DataType::kFixtures,
                     "https://v3.football.api-sports.io/"
                     "leagues?id=78&current=true");
  TEST_ASSERT_TRUE(feed(r, R"({"response":[{"seasons":[{"year":2025,)"
                           R"("current":false},{"year":2026,"current":true}]}]})"));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  r = expect(cfg::DataType::kFixtures,
             "https://v3.football.api-sports.io/fixtures?league=78&season=2026"
             "&from=2026-10-07&to=2026-10-21",
             2);
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(2)));
  expect(cfg::DataType::kLive,
         "https://v3.football.api-sports.io/fixtures?league=78&season=2026"
         "&from=2026-10-14&to=2026-10-14",
         4);
  planner.finish(true, at(4));
}

// The season is resolved again by the first fixtures run 24 h after the
// last resolution (OQ-248, OQ-259); the resolution time survives a save.
void test_api_football_fixtures_recheck_the_season_daily() {
  setSource(cfg::Provider::kApiFootball, "78");
  const char* leagues =
      "https://v3.football.api-sports.io/leagues?id=78&current=true";
  const char* season =
      R"({"response":[{"seasons":[{"year":2026,"current":true}]}]})";
  Request r = expect(cfg::DataType::kFixtures, leagues);
  TEST_ASSERT_TRUE(feed(r, season));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kBase),
                           planner.state(0).season_utc);
  expect(cfg::DataType::kFixtures,
         "https://v3.football.api-sports.io/fixtures?league=78&season=2026"
         "&from=2026-10-07&to=2026-10-21",
         2);
  planner.finish(true, at(2));
  // Less than 24 h later: the known season.
  const int64_t day = cfg::kSeasonRecheckS;
  expect(cfg::DataType::kFixtures,
         "https://v3.football.api-sports.io/fixtures?league=78&season=2026"
         "&from=2026-10-08&to=2026-10-22",
         day - 1);
  planner.finish(true, at(day - 1));
  // A table run does not resolve it again.
  expect(cfg::DataType::kTable,
         "https://v3.football.api-sports.io/standings?league=78&season=2026",
         day);
  planner.finish(true, at(day));
  // Saved and restored: the resolution time comes back with the season.
  const auto resolved = planner.saved(0, entry);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kBase), resolved.season_utc);
  planner.reset();
  planner.restore(0, entry, resolved);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kBase),
                           planner.state(0).season_utc);
  // 24 h after the resolution: the fixtures run asks for the season first.
  r = expect(cfg::DataType::kFixtures, leagues, day + 2);
  TEST_ASSERT_TRUE(feed(r, season));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(day + 2)));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kBase + day + 2),
                           planner.state(0).season_utc);
  expect(cfg::DataType::kFixtures,
         "https://v3.football.api-sports.io/fixtures?league=78&season=2026"
         "&from=2026-10-08&to=2026-10-22",
         day + 4);
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(day + 4)));
  // A clock that went back before the resolution counts as stale too.
  expect(cfg::DataType::kFixtures, leagues, -10);
}

// OQ-264: the daily-cap degradation counts an API-Football fixtures run as
// the requests it really sends: 2 while the season is unknown or due, 1 in
// between.
void test_api_football_fixtures_cost_matches_the_sent_requests() {
  setSource(cfg::Provider::kApiFootball, "78");
  const char* season =
      R"({"response":[{"seasons":[{"year":2026,"current":true}]}]})";
  const auto run = [&](int64_t dt) {
    Wanted w;
    planner.wanted(0, &entry, store, at(dt), w);
    unsigned sent = 0;
    Step step = Step::kMore;
    while (step == Step::kMore) {
      Request r;
      TEST_ASSERT_TRUE(planner.request(job(cfg::DataType::kFixtures), entry,
                                       store, at(dt + sent), r));
      TEST_ASSERT_TRUE(feed(
          r, std::strstr(r.url, "leagues?") ? season : R"({"response":[]})"));
      step = planner.finish(true, at(dt + sent));
      ++sent;
    }
    TEST_ASSERT_EQUAL(Step::kDone, step);
    TEST_ASSERT_EQUAL_UINT8(sent, w.cost[0]);
    return sent;
  };
  const int64_t day = cfg::kSeasonRecheckS;
  TEST_ASSERT_EQUAL_UINT32(2, run(0));         // season unknown
  TEST_ASSERT_EQUAL_UINT32(1, run(day - 10));  // season known
  TEST_ASSERT_EQUAL_UINT32(2, run(day + 10));  // season due again
  TEST_ASSERT_EQUAL_UINT32(1, run(day + 20));
}

// Without a valid UTC clock the known season is kept.
void test_api_football_season_kept_without_a_clock() {
  setSource(cfg::Provider::kApiFootball, "78");
  Request r = expect(cfg::DataType::kFixtures,
                     "https://v3.football.api-sports.io/"
                     "leagues?id=78&current=true");
  TEST_ASSERT_TRUE(feed(r, R"({"response":[{"seasons":[{"year":2026,)"
                           R"("current":true}]}]})"));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  expect(cfg::DataType::kFixtures,
         "https://v3.football.api-sports.io/fixtures?league=78&season=2026"
         "&from=2026-10-07&to=2026-10-21",
         2);
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(2)));
  const Clock no_utc{0, kBoot + 2 * cfg::kSeasonRecheckS};
  TEST_ASSERT_TRUE(
      planner.request(job(cfg::DataType::kFixtures), entry, store, no_utc, r));
  TEST_ASSERT_NULL(std::strstr(r.url, "leagues?"));
  TEST_ASSERT_NOT_NULL(std::strstr(r.url, "season=2026"));
}

void test_football_data_urls() {
  setSource(cfg::Provider::kFootballData, "BL1", "", "2026");
  expect(cfg::DataType::kFixtures,
         "https://api.football-data.org/v4/competitions/BL1/"
         "matches?dateFrom=2026-10-07&dateTo=2026-10-21&season=2026");
  planner.finish(true, at(0));
  expect(cfg::DataType::kTable,
         "https://api.football-data.org/v4/competitions/BL1/"
         "standings?season=2026");
  planner.finish(true, at(0));
}

void test_events_have_no_request_and_a_source_change_starts_fresh() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "91");
  Request r;
  TEST_ASSERT_FALSE(
      planner.request(job(cfg::DataType::kEvents), entry, store, at(0), r));
  r = expect(cfg::DataType::kFixtures,
             "https://api.openligadb.de/getmatchdata/bl1");
  feed(r, ("[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "]").c_str());
  planner.finish(true, at(0));
  TEST_ASSERT_EQUAL_UINT8(6, planner.state(0).round);
  setSource(cfg::Provider::kOpenLigaDb, "bl2", "91");
  expect(cfg::DataType::kFixtures,
         "https://api.openligadb.de/getmatchdata/bl2", 2);
  TEST_ASSERT_EQUAL_UINT8(0, planner.state(0).round);
}

void feedDays(const Request& r) {
  JsonDocument doc;
  deserializeJson(doc, kDays);
  for (JsonVariantConst day : doc["eventDate"]["dates"].as<JsonArrayConst>())
    TEST_ASSERT_TRUE(Planner::consume(day, &planner));
  (void)r;
}

uint8_t bit(cfg::DataType type) {
  return static_cast<uint8_t>(1u << static_cast<unsigned>(type));
}

// The competition's screen switches limit the data types (OQ-192).
void test_screen_switches_mask_the_wanted_types() {
  using Screen = cfg::CompetitionScreen;
  const auto only = [](Screen screen) {
    return static_cast<uint8_t>(1u << static_cast<unsigned>(screen));
  };
  const uint8_t matches =
      bit(cfg::DataType::kFixtures) | bit(cfg::DataType::kLive);
  const uint8_t table = bit(cfg::DataType::kFixtures) |
                        bit(cfg::DataType::kTable) |
                        bit(cfg::DataType::kLiveTable);
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "91");
  Wanted w;
  entry.screens = only(Screen::kTable);
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(table, w.types);
  entry.screens = only(Screen::kOwnMatch);
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(matches, w.types);
  entry.screens = only(Screen::kConference);
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(matches, w.types);
  entry.screens = only(Screen::kMatchday);
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(matches, w.types);
  // The own match needs a club.
  setSource(cfg::Provider::kOpenLigaDb, "bl1");
  entry.screens = only(Screen::kOwnMatch);
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(0, w.types);
  entry.screens = 0;
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(0, w.types);
}

// ESPN's own-match substitutions: the match summary of the latest own
// match whose live window opened, once it kicked off (OQ-207). The summary
// rows below are written by the agent after the shape checked on
// 2026-10-14 (keyEvents, type.type "substitution", participants: on, off).
void test_espn_events_from_the_match_summary() {
  using Screen = cfg::CompetitionScreen;
  setSource(cfg::Provider::kEspn, "ger.1", "125");
  Wanted w;
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(0x0f | bit(cfg::DataType::kEvents), w.types);
  TEST_ASSERT_EQUAL_UINT8(1, w.cost[4]);
  entry.screens = static_cast<uint8_t>(
      cfg::kCompetitionScreensDefault &
      ~(1u << static_cast<unsigned>(Screen::kOwnMatch)));
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(0x0f, w.types);
  entry.screens = cfg::kCompetitionScreensDefault;

  // Kickoff 2026-10-16 18:30 UTC; ten minutes later.
  const int64_t now = football::parseUtc("2026-10-16T18:40:00Z") - kBase;
  football::MapContext c{entry.id, cfg::Provider::kEspn, 125, at(now)};
  JsonDocument doc;
  deserializeJson(doc, kEspnUpcoming);
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>())
    football::storeMatch(e, store, c);
  // Not kicked off according to the provider: nothing to ask.
  Request r;
  TEST_ASSERT_FALSE(
      planner.request(job(cfg::DataType::kEvents), entry, store, at(now), r));
  std::string running = kEspnUpcoming;
  const std::string pre =
      R"("displayClock":"0'","type":{"name":"STATUS_SCHEDULED","state":"pre")";
  running.replace(running.find(pre), pre.size(),
                  R"("displayClock":"10'","type":{"name":"STATUS_FIRST_HALF",)"
                  R"("state":"in")");
  deserializeJson(doc, running);
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>())
    football::storeMatch(e, store, c);
  r = expect(cfg::DataType::kEvents,
             "https://site.api.espn.com/apis/site/v2/sports/soccer/"
             "ger.1/summary?event=401884770",
             now);
  TEST_ASSERT_EQUAL_STRING("keyEvents", r.array);
  TEST_ASSERT_TRUE(r.optional);
  TEST_ASSERT_EQUAL_UINT32(cfg::kProviderSummaryBodyBytes, r.body);
  const char* summary =
      R"({"boxscore":{},"keyEvents":[)"
      R"({"id":"501","type":{"id":"76","text":"Substitution","type":"substitution"},)"
      R"("clock":{"value":1560.0,"displayValue":"26'"},"team":{"id":"125"},)"
      R"("participants":[{"athlete":{"displayName":"On Home"}},)"
      R"({"athlete":{"displayName":"Off Home"}}]},)"
      R"({"id":"502","type":{"id":"70","text":"Goal","type":"goal"},)"
      R"("clock":{"value":1800.0,"displayValue":"30'"},"team":{"id":"125"},)"
      R"("participants":[{"athlete":{"displayName":"Scorer"}}]},)"
      R"({"id":"503","type":{"id":"76","text":"Substitution","type":"substitution"},)"
      R"("clock":{"value":5460.0,"displayValue":"90'+1'"},"team":{"id":"122"},)"
      R"("participants":[{"athlete":{"displayName":"On Away"}},)"
      R"({"athlete":{"displayName":"Off Away"}}]}]})";
  TEST_ASSERT_TRUE(feed(r, summary));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now)));
  const auto* m = store.find(entry.id, cfg::Provider::kEspn, 401884770);
  TEST_ASSERT_NOT_NULL(m);
  const auto& own = store.ownAt(m->own);
  // Only the substitutions; goals come with the scoreboard.
  TEST_ASSERT_EQUAL_UINT8(2, own.event_count);
  const auto& home = store.eventAt(own.events[0]);
  TEST_ASSERT_EQUAL(football::EventType::kSubstitution, home.type);
  TEST_ASSERT_TRUE(home.flags & football::kEventDetail);
  TEST_ASSERT_EQUAL_UINT32(501, home.identity);
  TEST_ASSERT_EQUAL_STRING("Off Home", home.player.data);
  TEST_ASSERT_EQUAL_STRING("On Home", home.player_in.data);
  TEST_ASSERT_EQUAL_UINT8(26, home.minute);
  TEST_ASSERT_EQUAL(football::Side::kHome, home.actor);
  const auto& away = store.eventAt(own.events[1]);
  TEST_ASSERT_EQUAL_UINT8(90, away.minute);
  TEST_ASSERT_EQUAL_UINT8(1, away.added);
  TEST_ASSERT_EQUAL(football::Side::kAway, away.actor);
  // Other providers ask no events.
  setSource(cfg::Provider::kApiFootball, "78", "157");
  TEST_ASSERT_FALSE(
      planner.request(job(cfg::DataType::kEvents), entry, store, at(now), r));
}

void test_openliga_live_without_a_known_round_asks_the_current_one() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "", "2026");
  Request r = expect(cfg::DataType::kLive,
                     "https://api.openligadb.de/getmatchdata/bl1");
  TEST_ASSERT_TRUE(feed(r, ("[" + olgRow(1, "2026-10-14T11:50:00Z", 6) + "]")
                               .c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(0)));
  TEST_ASSERT_EQUAL_UINT8(6, planner.state(0).round);
  TEST_ASSERT_TRUE(stored(1));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_encode_escapes_and_cuts_at_a_whole_escape);
  RUN_TEST(test_matchday_days_current_and_next);
  RUN_TEST(test_change_hash);
  RUN_TEST(test_wanted_types_and_costs);
  RUN_TEST(test_openliga_fixtures_run);
  RUN_TEST(test_openliga_fixtures_without_club_and_explicit_season);
  RUN_TEST(test_failure_ends_the_run);
  RUN_TEST(test_season_request_without_season_is_invalid);
  RUN_TEST(test_openliga_table);
  RUN_TEST(test_openliga_live_checks_the_change_date_per_round);
  RUN_TEST(test_espn_fixtures_from_match_days);
  RUN_TEST(test_espn_live_uses_the_match_day);
  RUN_TEST(test_espn_table);
  RUN_TEST(test_api_football_resolves_the_season);
  RUN_TEST(test_api_football_fixtures_recheck_the_season_daily);
  RUN_TEST(test_api_football_fixtures_cost_matches_the_sent_requests);
  RUN_TEST(test_api_football_season_kept_without_a_clock);
  RUN_TEST(test_football_data_urls);
  RUN_TEST(test_events_have_no_request_and_a_source_change_starts_fresh);
  RUN_TEST(test_screen_switches_mask_the_wanted_types);
  RUN_TEST(test_espn_events_from_the_match_summary);
  RUN_TEST(test_openliga_live_without_a_known_round_asks_the_current_one);
  return UNITY_END();
}
