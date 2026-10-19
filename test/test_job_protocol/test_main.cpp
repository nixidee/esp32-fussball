// Host replays of the provider worker's job protocol: the turn of
// football_service.cpp runJob() with the real scheduler, request plans and
// match store. Every request is answered from the recorded public ESPN
// responses (../test_match_store/recorded.h, fetched 2026-10-14); the ESPN
// calendar days, the summary, the running status and the failure patterns
// (busy device, unreadable bodies) are written by the agent. The replays
// reproduce the review findings of 2026-10-14 (wanted jobs without a
// request, starts not counted at the real request, a provider's own limit
// holding back the others, growing retries); they prove the protocol as
// implemented, not provider behaviour or the owner's device acceptance.
// The HTTP layer (get() in football_service.cpp) does not compile on the
// host: that 'sent()' runs right before every real request is checked there
// by reading, not here.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../provider_adapter_support.h"
#include "../source_support.h"
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
using football::plan::Completion;
using football::plan::Fetch;
using football::plan::Planner;
using football::plan::Request;
using namespace football::schedule;
using namespace recorded;

constexpr uint32_t kBoot = 1000;
// The own match 401884770 (Frankfurt, ESPN team 125) kicks off at 18:30 UTC.
const int64_t kBase = football::parseUtc("2026-10-16T18:00:00Z");
constexpr int64_t kKickoff = 30 * 60;
constexpr uint32_t kGapS = cfg::kRequestGapMs / 1000;

MatchStore store;
settings::CompetitionRoutes configured_routes;
settings::screen_policy::Policy configured_policy;
settings::screen_policy::Eligibility eligibility;
Planner planner;
Scheduler scheduler;
settings::ProviderPlans plans;
settings::Competition entry;

Clock at(int64_t dt) { return {kBase + dt, static_cast<uint32_t>(kBoot + dt)}; }

// ESPN calendar of the 2026/27 Bundesliga around the own match (agent).
const char* kDays =
    R"({"type":"day","season":{"$ref":"https://sports.core.api.espn.com/v2/sports/soccer/leagues/ger.1/seasons/2026"},"startDate":"2026-07-01T04:00Z","endDate":"2027-07-01T03:59Z","eventDate":{"dates":["2026-10-09T07:00Z","2026-10-10T07:00Z",)"
    R"("2026-10-11T07:00Z","2026-10-16T07:00Z","2026-10-17T07:00Z",)"
    R"("2026-10-18T07:00Z","2026-10-23T07:00Z","2026-10-24T07:00Z",)"
    R"("2026-10-25T07:00Z","2026-10-30T07:00Z"]}})";
const char* kSummary = R"({"boxscore":{},"keyEvents":[]})";

// The recorded scoreboard of the own match day, scheduled or (as the
// provider would report it after the kickoff) in the first half.
std::string scoreboard(bool running, const Request& request = {}) {
  std::string body = kEspnUpcoming;
  if (running) {
    const std::string pre =
        R"("displayClock":"0'","type":{"name":"STATUS_SCHEDULED","state":"pre")";
    body.replace(body.find(pre), pre.size(),
                 R"("displayClock":"10'","type":{"name":"STATUS_FIRST_HALF",)"
                 R"("state":"in")");
  }
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, body));
  doc["leagues"][0]["slug"] = "ger.1";
  doc["leagues"][0]["season"]["year"] = 2026;
  for (auto row : doc["events"].as<ArduinoJson::JsonArray>()) {
    row["season"]["year"] = 2026;
    row["league"]["slug"] = "ger.1";
  }
  if (request.spec.operation == football::provider::Operation::kOwnFixtures) {
    doc["requestedSeason"]["year"] = 2026;
    doc["team"]["id"] = request.spec.team;
  } else if (request.spec.from && request.spec.from / 86400 !=
                                      static_cast<uint32_t>(kBase) / 86400) {
    // The recorded fixture belongs to October16 only; another calendar day
    // returns an explicit empty scoreboard of this same source and season.
    doc["events"].to<ArduinoJson::JsonArray>();
  }
  body.clear();
  serializeJson(doc, body);
  return body;
}

bool feed(const Request& request, const std::string& body) {
  return football::test_support::feed(planner, request.spec, body).status ==
         football::provider::Status::kOk;
}

// What the device and the provider do with one prepared request.
struct Answer {
  Fetch fetch = Fetch::kOk;
  std::string body;  // for a sent kOk
};

// The ESPN responses for 'r' (bodies above).
Answer espn(const Request& r, bool running) {
  const std::string url = football::test_support::http(r).url;
  if (url.find("calendar/ondays") != std::string::npos)
    return {Fetch::kOk, kDays};
  if (url.find("/standings") != std::string::npos)
    return {Fetch::kOk, kEspnTable};
  if (url.find("/summary") != std::string::npos) return {Fetch::kOk, kSummary};
  if (url.find("/scoreboard") != std::string::npos ||
      r.spec.operation == football::provider::Operation::kOwnFixtures)
    return {Fetch::kOk, scoreboard(running, r)};
  TEST_FAIL_MESSAGE(football::test_support::http(r).url);
  return {};
}

struct Start {
  uint32_t mono_s;
  cfg::DataType type;
  cfg::Provider provider;
};

struct Trace {
  std::vector<Start> starts;  // counted request starts (scheduler.sent)
  uint32_t nothing = 0;       // turns without a request (Outcome::kNothing)
  uint32_t deferred = 0;      // turns ended by a local refusal
};

template <typename Respond>
// Replays the provider worker from 'from' to 'until' (seconds after
// kBase): the scheduler decision, the plan's request, the response, the
// planner's completion and the scheduler's outcome, as runJob() does. The
// worker sleeps until the decision's wake second, so a decision that
// neither runs nor sleeps would spin; at most a few turns end in one
// second.
void replay(int64_t from, int64_t until, Trace& trace, Respond respond) {
  int64_t dt = from;
  uint32_t same_second = 0;
  int64_t last_dt = -1;
  while (dt < until) {
    const Clock clock = at(dt);
    Inputs in;
    in.plans = &plans;
    in.daily_left.fill(kUnlimited);
    in.store = &store;
    in.clock = clock;
    configured_routes.entries[0] = entry;
    eligibility = settings::screen_policy::eligibility(configured_policy,
                                                       configured_routes);
    planner.synchronize(store, clock);
    const auto changed = store.takeFixtureChanges();
    planner.restartFixtures(changed);
    scheduler.restartFixtures(changed);
    planner.prepareDemand(store, clock, in);
    const Decision d = scheduler.next(in);
    same_second = dt == last_dt ? same_second + 1 : 0;
    last_dt = dt;
    TEST_ASSERT_TRUE_MESSAGE(same_second <= 2 * cfg::kDataTypeCount,
                             "decisions spin within one second");
    if (!d.run) {
      if (d.wake_s == 0) return;
      TEST_ASSERT_TRUE_MESSAGE(d.wake_s > clock.mono_s,
                               "a waiting decision sleeps until now");
      dt = static_cast<int64_t>(d.wake_s) - kBoot;
      continue;
    }
    Request r;
    if (!planner.request(d.job, entry, store, clock, r)) {
      scheduler.finished(d.job, Outcome::kNothing, clock.mono_s);
      ++trace.nothing;
      continue;
    }
    Answer a = respond(r, d.job, dt);
    const bool sent = a.fetch == Fetch::kOk || a.fetch == Fetch::kFailed ||
                      a.fetch == Fetch::kRateLimited;
    if (sent) {
      scheduler.sent(d.job.provider, clock.mono_s);
      trace.starts.push_back({clock.mono_s, d.job.type, d.job.provider});
      if (a.fetch == Fetch::kOk && !feed(r, a.body)) a.fetch = Fetch::kFailed;
    }
    const Completion c = planner.complete(a.fetch, clock);
    if (c.outcome == Outcome::kDeferred) ++trace.deferred;
    scheduler.finished(d.job, c.outcome, clock.mono_s, c.more);
  }
}

std::vector<uint32_t> startsOf(const Trace& trace, cfg::DataType type) {
  std::vector<uint32_t> out;
  for (const auto& s : trace.starts)
    if (s.type == type) out.push_back(s.mono_s);
  return out;
}

// Every two counted starts keep the device gap.
void assertGap(const Trace& trace) {
  for (std::size_t i = 1; i < trace.starts.size(); ++i)
    TEST_ASSERT_TRUE(trace.starts[i].mono_s >=
                     trace.starts[i - 1].mono_s + kGapS);
}

void setEspnSource() {
  entry = {};
  entry.id = 3;
  entry.primary.provider = cfg::Provider::kEspn;
  entry.primary.competition.assign("ger.1");
  entry.club.team = 125;
}

}  // namespace

void setUp() {
  store.clear();
  planner.reset();
  scheduler.reset();
  plans = settings::providerPlansDefault();
  for (auto& p : plans) p.limit = {};
  setEspnSource();
  entry.incarnation = 1;
  configured_routes.clear();
  configured_routes.entries.push(entry);
  eligibility = settings::screen_policy::eligibility(configured_policy,
                                                     configured_routes);
  // Match the real worker's coherent routes/eligibility context. Fixtures and
  // live classification remain prerequisites; table/Single demand is explicit.
  planner.context(configured_routes, eligibility);
}
void tearDown() {}

// F-DATA-01: from 30 minutes before the kickoff the own-match events are
// wanted, but ESPN still reports the match as scheduled (here until 7.5
// minutes after the planned kickoff). The events job is held instead of being
// decided again and again; fixtures, table and live data still run. Once
// the scoreboard reports the match running, the summary is asked for.
void test_events_held_until_the_provider_reports_the_kickoff() {
  bool running = false;
  auto respond = [&](const Request& r, const Job&, int64_t) {
    return espn(r, running);
  };
  Trace trace;
  const int64_t reported = kKickoff + 450;
  replay(0, reported, trace, respond);
  TEST_ASSERT_TRUE(startsOf(trace, cfg::DataType::kEvents).empty());
  TEST_ASSERT_FALSE(startsOf(trace, cfg::DataType::kFixtures).empty());
  TEST_ASSERT_FALSE(startsOf(trace, cfg::DataType::kTable).empty());
  // The live scoreboard every minute of the live window.
  const auto live = startsOf(trace, cfg::DataType::kLive);
  TEST_ASSERT_TRUE(live.size() >= 30);
  assertGap(trace);

  running = true;
  Trace after;
  replay(reported, reported + 5 * 60, after, respond);
  const auto events = startsOf(after, cfg::DataType::kEvents);
  TEST_ASSERT_FALSE(events.empty());
  // Right after the first scoreboard that reported the match running, not
  // an events interval later.
  const uint32_t first_running = startsOf(after, cfg::DataType::kLive)[0];
  TEST_ASSERT_TRUE(events.front() > first_running);
  TEST_ASSERT_TRUE(events.front() <=
                   first_running + cfg::kDataTypeCount * kGapS);
  assertGap(after);
}

// F-DATA-01: the own match is running, but every own-match slot of the
// store holds a nearer own match (agent-written, another competition), so
// the summary has nowhere to go and its turn makes no request. The turn
// ends with Outcome::kNothing and the events job waits for its interval
// instead of being decided again and again; the live scoreboard keeps its
// minute.
void test_events_without_an_own_slot_wait_for_their_interval() {
  for (uint32_t i = 0; i < cfg::kStoreOwnMatches; ++i) {
    football::StoredMatch m;
    m.source = football::test_source::source(store, 9, cfg::Provider::kEspn,
                                             1000000, at(kKickoff));
    m.id = 900 + i;
    m.kickoff = static_cast<uint32_t>(kBase + kKickoff + 60 + 60 * i);
    m.home = store.team(m.source, 1000000, "Home");
    m.away = store.team(m.source, 2 * m.id + 1, "Away");
    football::test_source::selectClub(store, 9, cfg::Provider::kEspn, 1000000,
                                      at(kKickoff));
    TEST_ASSERT_NOT_NULL(store.upsert(m, at(kKickoff)));
  }
  auto respond = [&](const Request& r, const Job&, int64_t) {
    return espn(r, true);
  };
  Trace trace;
  const int64_t from = kKickoff + 10 * 60, until = from + 3600;
  replay(from, until, trace, respond);
  const auto* own = store.find(store.currentSource(entry.id), 401884770);
  TEST_ASSERT_NOT_NULL(own);
  TEST_ASSERT_FALSE(own->detail < cfg::kStoreOwnMatches);
  TEST_ASSERT_TRUE(startsOf(trace, cfg::DataType::kEvents).empty());
  // One turn per events interval, no more.
  const uint32_t events_interval =
      plans[static_cast<std::size_t>(cfg::Provider::kEspn)]
          .interval_s[static_cast<std::size_t>(cfg::DataType::kEvents)];
  TEST_ASSERT_TRUE(trace.nothing >= 1);
  TEST_ASSERT_TRUE(trace.nothing <= 3600 / events_interval + 1);
  const auto live = startsOf(trace, cfg::DataType::kLive);
  TEST_ASSERT_TRUE(live.size() >= 55);
  assertGap(trace);
}

// F-DATA-02: with a window limit of one request per minute the device is
// busy (another operation holds the network) for a while, across a
// window-bucket boundary. Busy turns are no requests: they count no start,
// are no failure, and the job runs right after; the counted starts never
// exceed the limit and keep the device gap.
void test_busy_device_counts_only_real_starts() {
  auto& espn_plan = plans[static_cast<std::size_t>(cfg::Provider::kEspn)];
  espn_plan.limit.window_requests = 1;
  espn_plan.limit.window_s = 60;
  const int64_t busy_from = kKickoff + 50, busy_until = kKickoff + 75;
  auto respond = [&](const Request& r, const Job&, int64_t dt) {
    if (dt >= busy_from && dt < busy_until) return Answer{Fetch::kBusy, {}};
    return espn(r, true);
  };
  Trace trace;
  replay(kKickoff, kKickoff + 3600, trace, respond);
  TEST_ASSERT_TRUE(trace.deferred > 0);
  for (std::size_t i = 1; i < trace.starts.size(); ++i)
    TEST_ASSERT_TRUE(trace.starts[i].mono_s - trace.starts[i - 1].mono_s >= 60);
  // A start right after the busy time, not after a failure retry.
  bool resumed = false;
  for (const auto& s : trace.starts)
    if (s.mono_s >= at(busy_until).mono_s &&
        s.mono_s < at(busy_until).mono_s + 60)
      resumed = true;
  TEST_ASSERT_TRUE(resumed);
  // The busy turns were no failures: the own live scoreboard keeps its
  // minute (the limit's one request per minute), no growing retry.
  const auto live = startsOf(trace, cfg::DataType::kLive);
  for (std::size_t i = 1; i < live.size(); ++i)
    TEST_ASSERT_TRUE(live[i] - live[i - 1] <= 2 * 60 + kGapS);
}

// F-DATA-03: a selection list request used up OpenLigaDB's window limit.
// The selection waits for OpenLigaDB only (reported as a window wait with
// its second), while ESPN's live data keeps running.
void test_one_providers_window_does_not_hold_back_another() {
  auto& olg = plans[static_cast<std::size_t>(cfg::Provider::kOpenLigaDb)];
  olg.limit.window_requests = 1;
  olg.limit.window_s = 60;
  scheduler.configure(plans, {});
  scheduler.sent(cfg::Provider::kOpenLigaDb, at(kKickoff).mono_s);
  Wait why = Wait::kIdle;
  const uint32_t olg_at = scheduler.admissibleAt(
      cfg::Provider::kOpenLigaDb, at(kKickoff + kGapS).mono_s, &why);
  TEST_ASSERT_TRUE(olg_at >= at(kKickoff + 60).mono_s);
  TEST_ASSERT_EQUAL(Wait::kWindow, why);
  TEST_ASSERT_EQUAL_UINT32(at(kKickoff + kGapS).mono_s,
                           scheduler.admissibleAt(cfg::Provider::kEspn,
                                                  at(kKickoff + kGapS).mono_s));
  auto respond = [&](const Request& r, const Job&, int64_t) {
    return espn(r, true);
  };
  Trace trace;
  replay(kKickoff + kGapS, kKickoff + 60, trace, respond);
  TEST_ASSERT_FALSE(trace.starts.empty());
  TEST_ASSERT_EQUAL_UINT32(at(kKickoff + kGapS).mono_s,
                           trace.starts.front().mono_s);
  TEST_ASSERT_EQUAL_UINT32(
      olg_at, scheduler.admissibleAt(cfg::Provider::kOpenLigaDb, olg_at));
}

// Starts of one job retrying after failures: 60 s, doubling up to 900 s or
// the job's longer interval (OQ-262). Another job due in the same second
// may shift a start by device gaps.
void assertBackoff(const std::vector<uint32_t>& starts, uint32_t interval,
                   std::size_t at_least) {
  const uint32_t cap = interval > cfg::kProviderErrorRetryMaxS
                           ? interval
                           : cfg::kProviderErrorRetryMaxS;
  TEST_ASSERT_TRUE(starts.size() >= at_least);
  uint32_t expected = cfg::kProviderErrorRetryS;
  for (std::size_t i = 1; i < starts.size(); ++i) {
    const uint32_t wait = starts[i] - starts[i - 1];
    TEST_ASSERT_TRUE(wait >= expected);
    TEST_ASSERT_TRUE(wait <= expected + cfg::kDataTypeCount * kGapS);
    expected = expected * 2 < cap ? expected * 2 : cap;
  }
  TEST_ASSERT_EQUAL_UINT32(cap, expected);
}

uint32_t interval(cfg::DataType type) {
  return plans[static_cast<std::size_t>(cfg::Provider::kEspn)]
      .interval_s[static_cast<std::size_t>(type)];
}

// OQ-262: ESPN answers every request with an unreadable body (a proxy's
// error page). During the own match the live scoreboard and the match
// summary retry after 60 s, doubling to 900 s; a complete response resets
// the retry to the normal interval.
void test_failing_provider_backs_off_up_to_900_s_in_the_live_window() {
  bool failing = true;
  auto respond = [&](const Request& r, const Job&, int64_t) {
    if (failing) return Answer{Fetch::kOk, "<html>502 Bad Gateway</html>"};
    return espn(r, true);
  };
  // Running own match in the store (live window until kickoff + 240 min).
  football::MapContext c;
  football::SourceKey source;
  TEST_ASSERT_TRUE(source.assign(cfg::Provider::kEspn, "ger.1", "2026"));
  TEST_ASSERT_TRUE(store.bind(entry.id, entry.incarnation, 125, at(0)));
  c.source = store.sourceSlot(store.resolve(entry.id, source, at(0)));
  c.clock = at(0);
  JsonDocument doc;
  deserializeJson(doc, scoreboard(true));
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>())
    football::storeMatch(e, store, c);
  Trace trace;
  replay(kKickoff, kKickoff + 3 * 3600, trace, respond);
  assertBackoff(startsOf(trace, cfg::DataType::kLive),
                interval(cfg::DataType::kLive), 8);
  assertBackoff(startsOf(trace, cfg::DataType::kEvents),
                interval(cfg::DataType::kEvents), 8);

  failing = false;
  const auto live = startsOf(trace, cfg::DataType::kLive);
  const int64_t from = static_cast<int64_t>(live.back()) - kBoot + 900;
  Trace healthy;
  replay(from, from + 600, healthy, respond);
  const auto again = startsOf(healthy, cfg::DataType::kLive);
  TEST_ASSERT_TRUE(again.size() >= 5);
  for (std::size_t i = 1; i < again.size(); ++i)
    TEST_ASSERT_TRUE(again[i] - again[i - 1] <=
                     interval(cfg::DataType::kLive) + kGapS);
}

// OQ-263 amendment 2026-10-15: table and live table are the same standings
// request and share one failure count. ESPN answers only the standings with
// an unreadable body during the own match; the standings requests of both
// jobs together retry after 60 s, doubling to 900 s, instead of the sibling
// job starting the backoff again at its own first failure.
void test_table_and_live_table_back_off_as_one_request() {
  auto respond = [&](const Request& r, const Job&, int64_t) {
    if (std::string(football::test_support::http(r).url).find("/standings") !=
        std::string::npos)
      return Answer{Fetch::kOk, "<html>502 Bad Gateway</html>"};
    return espn(r, true);
  };
  football::MapContext c;
  football::SourceKey source;
  TEST_ASSERT_TRUE(source.assign(cfg::Provider::kEspn, "ger.1", "2026"));
  TEST_ASSERT_TRUE(store.bind(entry.id, entry.incarnation, 125, at(0)));
  c.source = store.sourceSlot(store.resolve(entry.id, source, at(0)));
  c.clock = at(0);
  JsonDocument doc;
  deserializeJson(doc, scoreboard(true));
  for (JsonVariantConst e : doc["events"].as<JsonArrayConst>())
    football::storeMatch(e, store, c);
  Trace trace;
  replay(kKickoff, kKickoff + 3 * 3600, trace, respond);
  std::vector<uint32_t> standings;
  for (const auto& s : trace.starts)
    if (s.type == cfg::DataType::kTable || s.type == cfg::DataType::kLiveTable)
      standings.push_back(s.mono_s);
  TEST_ASSERT_FALSE(startsOf(trace, cfg::DataType::kLiveTable).empty());
  assertBackoff(standings, interval(cfg::DataType::kLiveTable), 8);
}

// OQ-262: the same failure outside a live window. Fixtures and table retry
// after 60 s, doubling past 900 s up to their intervals (8 h and 4 h).
void test_failing_provider_backs_off_up_to_the_interval() {
  // A resolved season is required before table/data backoff is meaningful.
  entry.primary.season.assign("2026");
  auto respond = [&](const Request&, const Job&, int64_t) {
    return Answer{Fetch::kOk, "<html>502 Bad Gateway</html>"};
  };
  Trace trace;
  replay(0, 24 * 3600, trace, respond);
  assertBackoff(startsOf(trace, cfg::DataType::kFixtures),
                interval(cfg::DataType::kFixtures), 11);
  assertBackoff(startsOf(trace, cfg::DataType::kTable),
                interval(cfg::DataType::kTable), 10);
  TEST_ASSERT_TRUE(startsOf(trace, cfg::DataType::kLive).empty());
}

void test_failed_resolution_never_sends_unresolved_data_or_table() {
  Trace trace;
  replay(0, 3600, trace, [](const Request&, const Job&, int64_t) {
    return Answer{Fetch::kOk, "<html>502 Bad Gateway</html>"};
  });
  TEST_ASSERT_FALSE(startsOf(trace, cfg::DataType::kFixtures).empty());
  for (const auto type : {cfg::DataType::kTable, cfg::DataType::kLive,
                          cfg::DataType::kLiveTable, cfg::DataType::kEvents})
    TEST_ASSERT_TRUE(startsOf(trace, type).empty());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_events_held_until_the_provider_reports_the_kickoff);
  RUN_TEST(test_events_without_an_own_slot_wait_for_their_interval);
  RUN_TEST(test_busy_device_counts_only_real_starts);
  RUN_TEST(test_one_providers_window_does_not_hold_back_another);
  RUN_TEST(test_failing_provider_backs_off_up_to_900_s_in_the_live_window);
  RUN_TEST(test_failing_provider_backs_off_up_to_the_interval);
  RUN_TEST(test_table_and_live_table_back_off_as_one_request);
  RUN_TEST(test_failed_resolution_never_sends_unresolved_data_or_table);
  return UNITY_END();
}
