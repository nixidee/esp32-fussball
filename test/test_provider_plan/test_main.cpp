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

#include "../provider_adapter_support.h"
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
settings::CompetitionRoute entry;

Clock at(int64_t dt) { return {kBase + dt, static_cast<uint32_t>(kBoot + dt)}; }

void setSource(cfg::Provider provider, const char* competition,
               const char* team = "", const char* season = "") {
  entry = {};
  entry.id = 3;
  entry.primary.provider = provider;
  entry.primary.competition.assign(competition);
  entry.club.team = football::decimalId(team);
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
  TEST_ASSERT_EQUAL_STRING(url, football::test_support::http(r).url);
  return r;
}

// Feed the complete provider document through the actual typed Reader. A
// syntactically valid document can still fail the planner's source/scope proof.
bool feed(const Request& request, const char* body) {
  const auto parsed = football::test_support::feed(planner, request.spec, body);
  return parsed.status == football::provider::Status::kOk &&
         planner.responseFailure() == football::provider::Status::kOk;
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
    R"({"type":"day","startDate":"2026-07-01T04:00Z",)"
    R"("endDate":"2027-07-01T03:59Z","season":{"$ref":)"
    R"("https://sports.core.api.espn.com/v2/sports/soccer/leagues/ger.1/seasons/2026"},)"
    R"("eventDate":{"dates":["2026-10-09T07:00Z","2026-10-10T07:00Z",)"
    R"("2026-10-11T07:00Z","2026-10-16T07:00Z","2026-10-17T07:00Z",)"
    R"("2026-10-18T07:00Z","2026-10-23T07:00Z","2026-10-24T07:00Z",)"
    R"("2026-10-25T07:00Z","2026-10-30T07:00Z"]}})";

// Recorded event facts plus explicit current response scope. Empty days need
// the same league/year proof as nonempty days before they can replace data.
std::string espnDay(bool match) {
  JsonDocument doc;
  deserializeJson(doc, match ? kEspnUpcoming : R"({"events":[]})");
  doc["leagues"][0]["slug"] = "ger.1";
  doc["leagues"][0]["season"]["year"] = 2026;
  if (match) doc["events"][0]["season"]["year"] = 2026;
  std::string bytes;
  serializeJson(doc, bytes);
  return bytes;
}

constexpr const char* kApiSeasonEmpty =
    R"({"parameters":{"league":"78","season":"2026"},)"
    R"("paging":{"current":1,"total":1},"results":0,"errors":[],"response":[]})";
constexpr const char* kOpenLigaSeasonUrl =
    "https://api.openligadb.de/getmatchdata/bl1/2026";
constexpr const char* kApiSeasonUrl =
    "https://v3.football.api-sports.io/fixtures?league=78&season=2026";

// Season discovery is separate. Every known-season refresh first indexes the
// complete response, then admits exactly the selected rounds/own identities.
void openLigaSeason(const std::string& body, int64_t dt) {
  Request request = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, dt);
  TEST_ASSERT_EQUAL(Sink::kIndex, request.sink);
  TEST_ASSERT_TRUE(feed(request, body.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(dt)));
  request = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, dt + 1);
  TEST_ASSERT_EQUAL(Sink::kMatches, request.sink);
  TEST_ASSERT_TRUE(feed(request, body.c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(dt + 1)));
}

void emptyApiSeason(int64_t dt) {
  for (unsigned pass = 0; pass < 2; ++pass) {
    const Request request =
        expect(cfg::DataType::kFixtures, kApiSeasonUrl, dt + pass);
    TEST_ASSERT_EQUAL(pass == 0 ? Sink::kIndex : Sink::kMatches, request.sink);
    TEST_ASSERT_TRUE(feed(request, kApiSeasonEmpty));
    TEST_ASSERT_EQUAL(pass == 0 ? Step::kMore : Step::kDone,
                      planner.finish(true, at(dt + pass)));
  }
}

}  // namespace

void setUp() {
  store.clear();
  planner.reset();
}
void tearDown() {}

void test_request_urls_escape_utf8_and_reject_overflow() {
  football::provider::RequestSpec request;
  request.source = {cfg::Provider::kOpenLigaDb, "bl1", "2026"};
  request.operation = football::provider::Operation::kFixtures;
  football::provider::HttpRequest http;
  TEST_ASSERT_TRUE(football::provider::buildRequest(request, http));
  TEST_ASSERT_EQUAL_STRING("https://api.openligadb.de/getmatchdata/bl1/2026",
                           http.url);
  request.source.competition = "ger.1 ü";
  TEST_ASSERT_TRUE(football::provider::buildRequest(request, http));
  TEST_ASSERT_EQUAL_STRING(
      "https://api.openligadb.de/getmatchdata/ger.1%20%C3%BC/2026", http.url);
  request.source.competition = "a b";
  TEST_ASSERT_TRUE(football::provider::buildRequest(request, http));
  TEST_ASSERT_EQUAL_STRING("https://api.openligadb.de/getmatchdata/a%20b/2026",
                           http.url);
  // Exactly the bounded source length must remain complete after 3x escaping.
  // Overlong source fields and embedded NUL are rejected, never URL prefixes.
  const std::string spaces(cfg::kCompetitionIdMaxBytes, ' ');
  request.source.competition = spaces;
  TEST_ASSERT_TRUE(football::provider::buildRequest(request, http));
  TEST_ASSERT_EQUAL_UINT(
      std::strlen("https://api.openligadb.de/getmatchdata//2026") +
          3 * spaces.size(),
      std::strlen(http.url));
  const std::string overlong(cfg::kCompetitionIdMaxBytes + 1, ' ');
  request.source.competition = overlong;
  TEST_ASSERT_FALSE(football::provider::buildRequest(request, http));
  request.source.competition = std::string_view("a\0b", 3);
  TEST_ASSERT_FALSE(football::provider::buildRequest(request, http));
  request.source.competition = "bl1";
  const std::string season(cfg::kSeasonMaxBytes + 1, '2');
  request.source.season = season;
  TEST_ASSERT_FALSE(football::provider::buildRequest(request, http));
}

void test_matchday_days_previous_current_and_next() {
  uint32_t days[10], out[16];
  const char* text[] = {"2026-10-09T07:00Z", "2026-10-10T07:00Z",
                        "2026-10-11T07:00Z", "2026-10-16T07:00Z",
                        "2026-10-17T07:00Z", "2026-10-18T07:00Z",
                        "2026-10-23T07:00Z", "2026-10-24T07:00Z",
                        "2026-10-25T07:00Z", "2026-10-30T07:00Z"};
  for (int i = 0; i < 10; ++i)
    days[i] = static_cast<uint32_t>(football::parseUtc(text[i]));
  // Owner requirement: Wednesday retains complete matchdays5,6 and7.
  TEST_ASSERT_EQUAL(9, football::plan::matchdayDays(days, 10, kBase, out, 16));
  TEST_ASSERT_EQUAL_UINT32(days[0], out[0]);
  TEST_ASSERT_EQUAL_UINT32(days[8], out[8]);
  // Sunday of matchday 5 (its last day has not ended): matchdays 5 and 6.
  const int64_t sunday = football::parseUtc("2026-10-11T18:00:00Z");
  TEST_ASSERT_EQUAL(6, football::plan::matchdayDays(days, 10, sunday, out, 8));
  TEST_ASSERT_EQUAL_UINT32(days[0], out[0]);
  // Capacity cuts the next matchday.
  TEST_ASSERT_EQUAL(4, football::plan::matchdayDays(days, 10, sunday, out, 4));
  // After the season: the latest matchday and its predecessor stay available.
  const int64_t later = football::parseUtc("2026-11-01T00:00:00Z");
  TEST_ASSERT_EQUAL(4, football::plan::matchdayDays(days, 10, later, out, 16));
  TEST_ASSERT_EQUAL_UINT32(days[6], out[0]);
  TEST_ASSERT_EQUAL_UINT32(days[9], out[3]);
}

void test_change_hash() {
  const uint32_t a = football::plan::changeHash("2026-10-16T19:01:02.5");
  TEST_ASSERT_NOT_EQUAL(0, a);
  TEST_ASSERT_NOT_EQUAL(a, football::plan::changeHash("2026-10-16T19:01:03.5"));
  TEST_ASSERT_EQUAL_UINT32(a,
                           football::plan::changeHash("2026-10-16T19:01:02.5"));
}

void test_request_costs_and_inactive_source_no_demand() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "91");
  Wanted w;
  planner.wanted(0, &entry, store, at(0), w);
  TEST_ASSERT_EQUAL_UINT8(3, w.id);
  TEST_ASSERT_EQUAL_UINT8(3, w.cost[0]);  // season resolution + index/admission
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
  TEST_ASSERT_EQUAL(football::provider::Operation::kCurrentSeason,
                    r.spec.operation);
  const std::string round6 =
      "[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "," +
      olgRow(2, "2026-10-17T13:30:00Z", 6, "bl1", 2026, 7, 80) + "]";
  TEST_ASSERT_TRUE(feed(r, round6.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  TEST_ASSERT_EQUAL_UINT8(6, planner.state(0).round);
  TEST_ASSERT_EQUAL_STRING("2026",
                           std::string(planner.state(0).season.view()).c_str());
  TEST_ASSERT_TRUE(stored(1) && stored(2));

  // Complete season, deliberately out of round order. Ordinary matches retain
  // previous/current/next; the own previous + three next identities add rows
  // outside those rounds without retaining the fourth upcoming own match.
  const std::string season =
      "[" + olgRow(9, "2026-11-06T18:30:00Z", 9) + "," +
      olgRow(4, "2026-10-02T18:30:00Z", 4) + "," +
      olgRow(8, "2026-10-30T18:30:00Z", 8) + "," +
      olgRow(7, "2026-10-09T18:30:00Z", 5, "bl1", 2026, 7, 80) + "," +
      olgRow(3, "2026-10-23T18:30:00Z", 7) + "," +
      round6.substr(1, round6.size() - 2) + "]";
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, 1);
  TEST_ASSERT_EQUAL(Sink::kIndex, r.sink);
  TEST_ASSERT_TRUE(feed(r, season.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(1)));
  TEST_ASSERT_FALSE(stored(3) || stored(4));  // Index does not admit fixtures.
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, 2);
  TEST_ASSERT_EQUAL(Sink::kMatches, r.sink);
  TEST_ASSERT_TRUE(feed(r, season.c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(2)));
  for (const auto id : {1u, 2u, 3u, 4u, 7u, 8u}) TEST_ASSERT_TRUE(stored(id));
  TEST_ASSERT_FALSE(stored(9));
  TEST_ASSERT_EQUAL_UINT(6, store.matchCount());
  // A later run starts at the full-season index, using the known season.
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, 6);
  TEST_ASSERT_EQUAL(Sink::kIndex, r.sink);
}

void test_openliga_fixtures_without_club_and_explicit_season() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "", "2026");
  const std::string rows = "[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "]";
  openLigaSeason(rows, 0);
  TEST_ASSERT_TRUE(stored(1));
  // A foreign season/competition cannot establish a complete replacement.
  // Preserve existing facts and never admit those foreign rows.
  const std::string mixed =
      "[" + olgRow(2, "2025-10-16T18:30:00Z", 6, "bl1", 2025) + "," +
      olgRow(3, "2026-10-16T18:30:00Z", 6, "dfb") + "]";
  const Request r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, 4);
  TEST_ASSERT_FALSE(feed(r, mixed.c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(false, at(4)));
  TEST_ASSERT_TRUE(stored(1));
  TEST_ASSERT_FALSE(stored(2) || stored(3));
}

void test_failure_ends_the_run() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "91");
  Request r = expect(cfg::DataType::kFixtures,
                     "https://api.openligadb.de/getmatchdata/bl1");
  const std::string rows = "[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "]";
  TEST_ASSERT_TRUE(feed(r, rows.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, 2);
  TEST_ASSERT_TRUE(feed(r, rows.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(2)));
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, 3);
  TEST_ASSERT_EQUAL(Sink::kMatches, r.sink);
  // Busy was not sent: retry the same admission pass, not the season/index.
  const auto busy = planner.complete(football::plan::Fetch::kBusy, at(3));
  TEST_ASSERT_EQUAL(football::schedule::Outcome::kDeferred, busy.outcome);
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, 4);
  TEST_ASSERT_EQUAL(Sink::kMatches, r.sink);
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(false, at(4)));
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, 60);
  TEST_ASSERT_EQUAL(Sink::kIndex, r.sink);
  TEST_ASSERT_TRUE(stored(1));
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
  const std::string rows = "[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "]";
  openLigaSeason(rows, now);
  Request r;
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getlastchangedate/bl1/2026/6", now);
  TEST_ASSERT_EQUAL(football::provider::Operation::kLastChange,
                    r.spec.operation);
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
  openLigaSeason(rows, now);
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getlastchangedate/bl1/2026/6", now);
  TEST_ASSERT_FALSE(feed(r, "[]"));
}

// OQ-311: only a complete fixtures run sees every match, so only it clears
// the "match list full" notice; a broken run and live requests keep it.
void test_matches_notice_clears_only_after_a_complete_fixtures_run() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "", "2026");
  const int64_t now = football::parseUtc("2026-10-16T18:40:00Z") - kBase;
  const std::string round6 = "[" + olgRow(1, "2026-10-16T18:30:00Z", 6) + "]";
  store.flag(entry.id, football::kNoticeMatches, true);
  Request r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, now);
  TEST_ASSERT_TRUE(feed(r, round6.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(now)));
  TEST_ASSERT_EQUAL_UINT8(football::kNoticeMatches, store.notices(entry.id));
  expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, now + 2);
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(false, at(now + 2)));
  TEST_ASSERT_EQUAL_UINT8(football::kNoticeMatches, store.notices(entry.id));
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getlastchangedate/bl1/2026/6", now + 4);
  TEST_ASSERT_TRUE(feed(r, R"("2026-10-16T18:39:00.1")"));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(now + 4)));
  r = expect(cfg::DataType::kLive,
             "https://api.openligadb.de/getmatchdata/bl1/2026/6", now + 6);
  TEST_ASSERT_TRUE(feed(r, round6.c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now + 6)));
  TEST_ASSERT_EQUAL_UINT8(football::kNoticeMatches, store.notices(entry.id));
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, now + 60);
  TEST_ASSERT_TRUE(feed(r, round6.c_str()));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(now + 60)));
  TEST_ASSERT_EQUAL_UINT8(football::kNoticeMatches, store.notices(entry.id));
  r = expect(cfg::DataType::kFixtures, kOpenLigaSeasonUrl, now + 61);
  TEST_ASSERT_TRUE(feed(r, round6.c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now + 61)));
  TEST_ASSERT_EQUAL_UINT8(0, store.notices(entry.id));
}

void test_espn_fixtures_from_match_days() {
  setSource(cfg::Provider::kEspn, "ger.1", "598");
  Request r = expect(cfg::DataType::kFixtures,
                     "https://sports.core.api.espn.com/v2/sports/soccer/"
                     "leagues/ger.1/calendar/ondays");
  TEST_ASSERT_EQUAL(football::provider::Operation::kCalendarDays,
                    r.spec.operation);
  TEST_ASSERT_TRUE(Sink::kDays == r.sink);
  TEST_ASSERT_TRUE(feed(r, kDays));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  TEST_ASSERT_EQUAL_UINT8(9, planner.state(0).fixtures.count);
  TEST_ASSERT_EQUAL_STRING("2026",
                           std::string(planner.state(0).season.view()).c_str());
  const char* expected[] = {"20261009", "20261010", "20261011",
                            "20261016", "20261017", "20261018",
                            "20261023", "20261024", "20261025"};
  for (int i = 0; i < 9; ++i) {
    const std::string url =
        std::string(
            "https://site.api.espn.com/apis/site/v2/sports/soccer/"
            "ger.1/scoreboard?dates=") +
        expected[i] + "&season=2026";
    r = expect(cfg::DataType::kFixtures, url.c_str(), 2 + i);
    TEST_ASSERT_EQUAL(football::provider::Operation::kFixtures,
                      r.spec.operation);
    // The recorded event belongs to October16, never to another requested day.
    TEST_ASSERT_TRUE(feed(r, espnDay(i == 3).c_str()));
    TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(2 + i)));
  }
  for (unsigned pass = 0; pass < 2; ++pass) {
    r = expect(cfg::DataType::kFixtures,
               "https://site.api.espn.com/apis/site/v2/sports/soccer/ger.1/"
               "teams/598/schedule?season=2026",
               12 + pass);
    TEST_ASSERT_EQUAL(pass == 0 ? Sink::kOwnIndex : Sink::kOwnMatches, r.sink);
    TEST_ASSERT_TRUE(feed(
        r,
        R"({"events":[],"requestedSeason":{"year":2026},"team":{"id":"598"}})"));
    TEST_ASSERT_EQUAL(pass == 0 ? Step::kMore : Step::kDone,
                      planner.finish(true, at(12 + pass)));
  }
  TEST_ASSERT_TRUE(stored(401884770, cfg::Provider::kEspn));
  Wanted w;
  planner.wanted(0, &entry, store, at(14), w);
  TEST_ASSERT_EQUAL_UINT8(12,
                          w.cost[0]);  // Calendar +9days + own index/admission.
}

void test_espn_live_uses_the_match_day() {
  setSource(cfg::Provider::kEspn, "ger.1");
  // Kickoff 2026-10-16 18:30 UTC; ten minutes later.
  const int64_t now = football::parseUtc("2026-10-16T18:40:00Z") - kBase;
  football::MapContext c;
  c.competition = entry.id;
  c.provider = cfg::Provider::kEspn;
  c.clock = at(now);
  JsonDocument doc;
  deserializeJson(doc, kEspnUpcoming);
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>())
    football::storeMatch(e, store, c);
  // Without verified stored match days: the scoreboard without a date.
  Request r = expect(cfg::DataType::kLive,
                     "https://site.api.espn.com/apis/site/v2/sports/soccer/"
                     "ger.1/scoreboard",
                     now);
  TEST_ASSERT_TRUE(feed(r, espnDay(true).c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now)));
  r = expect(cfg::DataType::kFixtures,
             "https://sports.core.api.espn.com/v2/sports/soccer/leagues/"
             "ger.1/calendar/ondays",
             now);
  TEST_ASSERT_TRUE(feed(r, kDays));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(now)));
  // With verified match days: the requested day contains the live kickoff.
  r = expect(cfg::DataType::kLive,
             "https://site.api.espn.com/apis/site/v2/sports/soccer/"
             "ger.1/scoreboard?dates=20261016&season=2026",
             now + 2);
  TEST_ASSERT_TRUE(feed(r, espnDay(true).c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(now + 2)));
}

void test_espn_table() {
  setSource(cfg::Provider::kEspn, "ger.1");
  Request r = expect(cfg::DataType::kTable,
                     "https://site.web.api.espn.com/apis/v2/sports/soccer/"
                     "ger.1/standings");
  TEST_ASSERT_EQUAL(football::provider::Operation::kTable, r.spec.operation);
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
  TEST_ASSERT_TRUE(
      feed(r, R"({"response":[{"seasons":[{"year":2025,)"
              R"("current":false},{"year":2026,"current":true}]}]})"));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(0)));
  emptyApiSeason(2);
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
  emptyApiSeason(2);
  // Less than 24 h later: the known season.
  const int64_t day = cfg::kSeasonRecheckS;
  emptyApiSeason(day - 2);
  // A table run does not resolve it again.
  expect(cfg::DataType::kTable,
         "https://v3.football.api-sports.io/standings?league=78&season=2026",
         day);
  planner.finish(true, at(day));
  // 24 h after the resolution: the fixtures run asks for the season first.
  r = expect(cfg::DataType::kFixtures, leagues, day + 2);
  TEST_ASSERT_TRUE(feed(r, season));
  TEST_ASSERT_EQUAL(Step::kMore, planner.finish(true, at(day + 2)));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kBase + day + 2),
                           planner.state(0).season_utc);
  emptyApiSeason(day + 4);
  // A clock that went back before the resolution counts as stale too.
  expect(cfg::DataType::kFixtures, leagues, -10);
}

// OQ-264: the daily-cap degradation counts an API-Football fixtures run as
// the requests it really sends: 3 while the season is unknown or due, 2 in
// between (complete-season index + admission).
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
      TEST_ASSERT_TRUE(
          feed(r, std::strstr(football::test_support::http(r).url, "leagues?")
                      ? season
                      : kApiSeasonEmpty));
      step = planner.finish(true, at(dt + sent));
      ++sent;
    }
    TEST_ASSERT_EQUAL(Step::kDone, step);
    TEST_ASSERT_EQUAL_UINT8(sent, w.cost[0]);
    return sent;
  };
  const int64_t day = cfg::kSeasonRecheckS;
  TEST_ASSERT_EQUAL_UINT32(3, run(0));         // season unknown
  TEST_ASSERT_EQUAL_UINT32(2, run(day - 10));  // season known
  TEST_ASSERT_EQUAL_UINT32(3, run(day + 10));  // season due again
  TEST_ASSERT_EQUAL_UINT32(2, run(day + 20));
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
  emptyApiSeason(2);
  const Clock no_utc{0, kBoot + 2 * cfg::kSeasonRecheckS};
  TEST_ASSERT_TRUE(
      planner.request(job(cfg::DataType::kFixtures), entry, store, no_utc, r));
  // A full-season request needs no dated horizon. Without UTC, the resolved
  // season is retained rather than triggering another season discovery.
  TEST_ASSERT_EQUAL(football::provider::Operation::kFixtures, r.spec.operation);
  TEST_ASSERT_EQUAL_STRING("2026", std::string(r.spec.source.season).c_str());
}

void test_football_data_urls() {
  setSource(cfg::Provider::kFootballData, "BL1", "", "2026");
  // Fixtures use two full-season passes; only live uses the dated endpoint.
  for (unsigned pass = 0; pass < 2; ++pass) {
    const auto request = expect(cfg::DataType::kFixtures,
                                "https://api.football-data.org/v4/competitions/"
                                "BL1/matches?season=2026");
    TEST_ASSERT_EQUAL(pass == 0 ? Sink::kIndex : Sink::kMatches, request.sink);
    TEST_ASSERT_TRUE(feed(
        request, R"({"matches":[],"filters":{"season":"2026"},)"
                 R"("competition":{"code":"BL1"},"resultSet":{"count":0}})"));
    TEST_ASSERT_EQUAL(pass == 0 ? Step::kMore : Step::kDone,
                      planner.finish(true, at(0)));
  }
  // dateTo is exclusive, so today's live endpoint ends on the next day.
  expect(cfg::DataType::kLive,
         "https://api.football-data.org/v4/competitions/BL1/"
         "matches?dateFrom=2026-10-14&dateTo=2026-10-15&season=2026");
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
  expect(cfg::DataType::kFixtures, "https://api.openligadb.de/getmatchdata/bl2",
         2);
  TEST_ASSERT_EQUAL_UINT8(0, planner.state(0).round);
}

// ESPN substitutions from an exact canonical match target. The summary
// rows below are written by the agent after the shape checked on
// 2026-10-14 (keyEvents, type.type "substitution", participants: on, off).
void test_exact_target_summary_maps_available_substitutions() {
  setSource(cfg::Provider::kEspn, "ger.1", "125");
  auto events_job = job(cfg::DataType::kEvents);
  events_job.event_match = 401884770;

  // Kickoff 2026-10-16 18:30 UTC; ten minutes later.
  const int64_t now = football::parseUtc("2026-10-16T18:40:00Z") - kBase;
  football::MapContext c;
  c.competition = entry.id;
  c.provider = cfg::Provider::kEspn;
  c.club = 125;
  c.clock = at(now);
  JsonDocument doc;
  deserializeJson(doc, kEspnUpcoming);
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>())
    football::storeMatch(e, store, c);
  // Not kicked off according to the provider: nothing to ask.
  Request r;
  TEST_ASSERT_FALSE(planner.request(events_job, entry, store, at(now), r));
  std::string running = kEspnUpcoming;
  const std::string pre =
      R"("displayClock":"0'","type":{"name":"STATUS_SCHEDULED","state":"pre")";
  running.replace(running.find(pre), pre.size(),
                  R"("displayClock":"10'","type":{"name":"STATUS_FIRST_HALF",)"
                  R"("state":"in")");
  deserializeJson(doc, running);
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>())
    football::storeMatch(e, store, c);
  TEST_ASSERT_TRUE(planner.request(events_job, entry, store, at(now), r));
  TEST_ASSERT_EQUAL_STRING(
      "https://site.api.espn.com/apis/site/v2/sports/soccer/ger.1/"
      "summary?event=401884770",
      football::test_support::http(r).url);
  TEST_ASSERT_EQUAL(football::provider::Operation::kSummary, r.spec.operation);
  TEST_ASSERT_EQUAL_UINT32(cfg::kProviderSummaryBodyBytes,
                           football::test_support::http(r).body_limit);
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
  const auto& own = store.detailAt(m->detail);
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
  TEST_ASSERT_TRUE(
      feed(r, ("[" + olgRow(1, "2026-10-14T11:50:00Z", 6) + "]").c_str()));
  TEST_ASSERT_EQUAL(Step::kDone, planner.finish(true, at(0)));
  TEST_ASSERT_EQUAL_UINT8(6, planner.state(0).round);
  TEST_ASSERT_TRUE(stored(1));
}

// Owner-approved dataset semantics: successful complete empty tables replace
// stale rows; failed/incomplete responses keep the last successful table.
void test_complete_empty_table_replaces_stale_rows() {
  setSource(cfg::Provider::kOpenLigaDb, "bl1", "", "2026");
  football::StoredRow row;
  row.points = 12;
  store.beginTable(entry.id);
  TEST_ASSERT_TRUE(store.appendRow(row));
  TEST_ASSERT_TRUE(store.commitTable(true));
  Request request;
  TEST_ASSERT_TRUE(planner.request(job(cfg::DataType::kTable), entry, store,
                                   at(0), request));
  TEST_ASSERT_TRUE(feed(request, "[]"));
  planner.finish(false, at(0));
  TEST_ASSERT_EQUAL_UINT(1, store.rowCount());
  planner.reset();
  TEST_ASSERT_TRUE(planner.request(job(cfg::DataType::kTable), entry, store,
                                   at(1), request));
  TEST_ASSERT_TRUE(feed(request, "[]"));
  planner.finish(true, at(1));
  TEST_ASSERT_EQUAL_UINT(0, store.rowCount());
  TEST_ASSERT_EQUAL(
      football::schedule::Outcome::kOk,
      planner.complete(football::plan::Fetch::kOk, at(1)).outcome);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_complete_empty_table_replaces_stale_rows);
  RUN_TEST(test_request_urls_escape_utf8_and_reject_overflow);
  RUN_TEST(test_matchday_days_previous_current_and_next);
  RUN_TEST(test_change_hash);
  RUN_TEST(test_request_costs_and_inactive_source_no_demand);
  RUN_TEST(test_openliga_fixtures_run);
  RUN_TEST(test_openliga_fixtures_without_club_and_explicit_season);
  RUN_TEST(test_failure_ends_the_run);
  RUN_TEST(test_season_request_without_season_is_invalid);
  RUN_TEST(test_openliga_table);
  RUN_TEST(test_openliga_live_checks_the_change_date_per_round);
  RUN_TEST(test_matches_notice_clears_only_after_a_complete_fixtures_run);
  RUN_TEST(test_espn_fixtures_from_match_days);
  RUN_TEST(test_espn_live_uses_the_match_day);
  RUN_TEST(test_espn_table);
  RUN_TEST(test_api_football_resolves_the_season);
  RUN_TEST(test_api_football_fixtures_recheck_the_season_daily);
  RUN_TEST(test_api_football_fixtures_cost_matches_the_sent_requests);
  RUN_TEST(test_api_football_season_kept_without_a_clock);
  RUN_TEST(test_football_data_urls);
  RUN_TEST(test_events_have_no_request_and_a_source_change_starts_fresh);
  RUN_TEST(test_exact_target_summary_maps_available_substitutions);
  RUN_TEST(test_openliga_live_without_a_known_round_asks_the_current_one);
  return UNITY_END();
}
