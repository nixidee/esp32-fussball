// Approved ordinary Singles use the canonical mapper, selector and event pool.
// Synthetic cases reproduce first-response loss, partial-response ambiguity and
// ownership/priority errors; the recorded case is historical provider evidence.
#include <ArduinoJson.h>
#include <unity.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "../provider_adapter_support.h"
#include "../test_match_store/recorded.h"
#include "provider_plan.h"
#include "store_views.h"

namespace detail_allocation {
bool watch = false, fail = false;
unsigned attempts = 0;
}  // namespace detail_allocation
// Observe/fail only the bounded payload allocation. This verifies that the
// first-send settings fence has no retained event buffer to overlap, and that
// allocation failure remains visible instead of claiming complete details.
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  if (detail_allocation::watch &&
      bytes == sizeof(football::plan::ResponseDetails::EventBuffer)) {
    ++detail_allocation::attempts;
    if (detail_allocation::fail) return nullptr;
  }
  return std::malloc(bytes);
}

namespace {
using namespace football;
namespace plan = football::plan;
namespace schedule = football::schedule;
namespace policy = settings::screen_policy;
constexpr auto kProvider = cfg::Provider::kEspn;
const uint32_t kNow = static_cast<uint32_t>(parseUtc("2026-10-17T12:00:00Z"));
MatchStore store;
plan::Planner planner;
settings::CompetitionRoutes competitions;
policy::Policy options;
policy::Eligibility eligibility;
settings::ProviderPlans plans;

settings::CompetitionRoute& entry() { return competitions.entries[0]; }
Clock now() { return {kNow, 1000}; }
void refresh() {
  eligibility = policy::eligibility(options, competitions);
  planner.context(competitions, eligibility);
}
schedule::Job job(cfg::DataType type = cfg::DataType::kLive) {
  schedule::Job out;
  out.competition = 1;
  out.provider = kProvider;
  out.type = type;
  return out;
}
void begin() {
  plan::Request request;
  TEST_ASSERT_TRUE(planner.request(job(), entry(), store, now(), request));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(plan::Sink::kMatches),
                        static_cast<int>(request.sink));
}
void row(uint32_t id, const char* status = "STATUS_IN_PROGRESS",
         const char* state = "in", const char* date = "2026-10-17T11:30:00Z",
         unsigned events = 1, bool own = false, bool accepted = true) {
  ArduinoJson::JsonDocument doc;
  doc["id"] = id;
  doc["date"] = date;
  auto c = doc["competitions"][0];
  c["status"]["type"]["state"] = state;
  c["status"]["type"]["name"] = status;
  c["status"]["type"]["completed"] = !strcmp(state, "post");
  c["status"]["displayClock"] = "30'";
  auto home = c["competitors"][0];
  home["homeAway"] = "home";
  home["team"]["id"] = own ? 99 : 91;
  home["team"]["displayName"] = "Full foreign home name";
  home["team"]["shortDisplayName"] = "HOM";
  home["score"] = "1";
  auto away = c["competitors"][1];
  away["homeAway"] = "away";
  away["team"]["id"] = 65;
  away["team"]["displayName"] = "Full foreign away name";
  away["team"]["shortDisplayName"] = "AWY";
  away["score"] = "0";
  for (unsigned i = 0; i < events; ++i) {
    auto e = c["details"][i];
    e["type"]["text"] = "Goal";
    e["scoringPlay"] = true;
    e["team"]["id"] = 91;
    e["clock"]["displayValue"] = std::to_string(i + 1) + "'";
    e["athletesInvolved"][0]["displayName"] = "Scorer";
  }
  TEST_ASSERT_EQUAL(
      accepted, test_support::consume(doc.as<ArduinoJson::JsonVariantConst>(),
                                      planner, entry().primary));
}
const StoredMatch& find(uint32_t id) {
  const auto* m = store.find(1, kProvider, id);
  TEST_ASSERT_NOT_NULL(m);
  return *m;
}
schedule::Wanted wanted() {
  schedule::Wanted out;
  planner.wanted(0, &entry(), store, now(), out);
  return out;
}
bool wants(const schedule::Wanted& out, cfg::DataType type) {
  return out.types & (1u << static_cast<unsigned>(type));
}
schedule::Inputs requests(const schedule::Wanted& wanted) {
  schedule::Inputs out;
  out.competitions[0] = wanted;
  out.plans = &plans;
  out.store = &store;
  out.clock = now();
  out.daily_left.fill(schedule::kUnlimited);
  return out;
}
}  // namespace

void setUp() {
  detail_allocation::watch = detail_allocation::fail = false;
  detail_allocation::attempts = 0;
  store.clear();
  planner.reset();
  competitions = {};
  options = {};
  plans = settings::Model{}.providers;
  settings::CompetitionRoute c;
  c.id = 1;
  c.incarnation = 1;
  c.primary.provider = kProvider;
  c.primary.competition.assign("ger.1");
  TEST_ASSERT_TRUE(competitions.entries.push(c));
  refresh();
}
void tearDown() {}

void test_initial_complete_response_keeps_foreign_names_and_events() {
  begin();
  row(10);
  TEST_ASSERT_FALSE(store.hasDetail(find(10)));
  planner.finish(true, now());
  const auto& m = find(10);
  TEST_ASSERT_FALSE(store.isOwn(m));
  TEST_ASSERT_TRUE(store.hasDetail(m));
  TEST_ASSERT_TRUE(m.detail >= cfg::kStoreOwnMatches);
  TEST_ASSERT_EQUAL_STRING("Full foreign home name",
                           view::teamName(store, m, true));
  TEST_ASSERT_EQUAL_UINT8(1, view::events(store, m).count);
}

void test_partial_response_does_not_commit_new_foreign_details() {
  begin();
  row(10);
  planner.finish(false, now());
  TEST_ASSERT_FALSE(store.hasDetail(find(10)));
}

void test_late_cancellation_of_stale_competitor_keeps_initial_events() {
  begin();
  row(20);
  planner.finish(true, now());
  begin();
  row(10);
  row(20, "STATUS_CANCELED", "post", "2026-10-17T11:45:00Z", 0);
  planner.finish(true, now());
  TEST_ASSERT_TRUE(store.hasDetail(find(10)));
  TEST_ASSERT_EQUAL_UINT8(1, view::events(store, find(10)).count);
}

void test_second_running_match_prevents_arbitrary_ordinary_target() {
  begin();
  row(10);
  row(20);
  planner.finish(true, now());
  TEST_ASSERT_FALSE(store.hasDetail(find(10)));
  TEST_ASSERT_FALSE(store.hasDetail(find(20)));
  TEST_ASSERT_EQUAL_UINT32(0, wanted().event_match);
}

void test_running_match_wins_over_earlier_buffer_candidate() {
  begin();
  row(10, "STATUS_SCHEDULED", "pre", "2026-10-17T12:30:00Z");
  row(20);
  planner.finish(true, now());
  TEST_ASSERT_FALSE(store.hasDetail(find(10)));
  TEST_ASSERT_TRUE(store.hasDetail(find(20)));
  TEST_ASSERT_EQUAL_UINT32(20, wanted().event_match);
}

void test_manual_single_requests_exact_foreign_summary_without_own_priority() {
  options
      .shared[policy::index(policy::Phase::kOtherGames)]
             [screen::index(screen::Type::kLiveSingle)]
      .automatic = false;
  refresh();
  begin();
  row(10);
  planner.finish(true, now());
  const auto demand = wanted();
  TEST_ASSERT_TRUE(wants(demand, cfg::DataType::kEvents));
  TEST_ASSERT_EQUAL_UINT32(10, demand.event_match);
  TEST_ASSERT_FALSE(demand.event_priority);
  auto target = job(cfg::DataType::kEvents);
  target.event_match = demand.event_match;
  plan::Request request;
  TEST_ASSERT_TRUE(planner.request(target, entry(), store, now(), request));
  TEST_ASSERT_NOT_NULL(
      strstr(test_support::http(request).url, "summary?event=10"));
  planner.abort();
}

void test_special_off_owned_single_keeps_owned_bank_without_special_priority() {
  entry().club.team = 99;
  entry().club.own_special = false;
  refresh();
  begin();
  row(10, "STATUS_IN_PROGRESS", "in", "2026-10-17T11:30:00Z", 1, true);
  planner.finish(true, now());
  TEST_ASSERT_TRUE(store.isOwn(find(10)));
  TEST_ASSERT_TRUE(find(10).detail < cfg::kStoreOwnMatches);
  TEST_ASSERT_EQUAL_UINT32(10, wanted().event_match);
  TEST_ASSERT_EQUAL_UINT32(0, wanted().priority_match);
  TEST_ASSERT_FALSE(wanted().event_priority);
}

void test_disabled_views_retain_phase_classification_prerequisites() {
  for (auto& phase : options.shared)
    for (auto& rule : phase) rule.enabled = false;
  refresh();
  const auto demand = wanted();
  TEST_ASSERT_TRUE(wants(demand, cfg::DataType::kFixtures));
  TEST_ASSERT_TRUE(wants(demand, cfg::DataType::kLive));
  TEST_ASSERT_FALSE(wants(demand, cfg::DataType::kTable));
  TEST_ASSERT_FALSE(wants(demand, cfg::DataType::kEvents));
}

void test_frame_has_no_automatic_provider_demand() {
  options.frame = true;
  refresh();
  TEST_ASSERT_EQUAL_UINT8(0, wanted().id);
  TEST_ASSERT_EQUAL_UINT8(0, wanted().types);
}

void test_duplicate_response_identity_keeps_detail_loss_visible() {
  begin();
  row(10);
  row(10);
  planner.finish(true, now());
  TEST_ASSERT_TRUE(store.notices(1) & kNoticeEvents);
  TEST_ASSERT_TRUE(find(10).flags & kMatchEventsPoolCut);
  TEST_ASSERT_EQUAL_UINT8(0, view::events(store, find(10)).count);
  begin();
  row(10);
  planner.finish(true, now());
  TEST_ASSERT_FALSE(store.notices(1) & kNoticeEvents);
  TEST_ASSERT_EQUAL_UINT8(1, view::events(store, find(10)).count);
}

void test_response_event_limit_keeps_latest_twelve_and_marks_truncation() {
  begin();
  row(10, "STATUS_IN_PROGRESS", "in", "2026-10-17T11:30:00Z", 14);
  planner.finish(true, now());
  const auto events = view::events(store, find(10));
  TEST_ASSERT_EQUAL_UINT8(12, events.count);
  TEST_ASSERT_EQUAL_UINT8(3, store.eventAt(events.index[0]).minute);
  TEST_ASSERT_TRUE(find(10).flags & kMatchEventsTruncated);
}

void test_changed_exact_target_runs_even_with_previous_target_interval() {
  begin();
  row(10);
  planner.finish(true, now());
  auto demand = wanted();
  demand.types = 1u << static_cast<unsigned>(cfg::DataType::kEvents);
  schedule::Scheduler scheduler;
  auto input = requests(demand);
  auto first = scheduler.next(input);
  TEST_ASSERT_TRUE(first.run);
  TEST_ASSERT_EQUAL_UINT32(10, first.job.event_match);
  scheduler.finished(first.job, schedule::Outcome::kOk, input.clock.mono_s);
  begin();
  row(10, "STATUS_CANCELED", "post", "2026-10-17T11:30:00Z", 0);
  row(20);
  planner.finish(true, now());
  demand = wanted();
  demand.types = 1u << static_cast<unsigned>(cfg::DataType::kEvents);
  input.competitions[0] = demand;
  const auto next = scheduler.next(input);
  TEST_ASSERT_TRUE(next.run);
  TEST_ASSERT_EQUAL_UINT32(20, next.job.event_match);
}

void test_recorded_scoreboard_row_keeps_first_response_foreign_detail() {
  ArduinoJson::JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, recorded::kEspnDay));
  const auto recorded_row = doc["events"][0];
  const Clock clock{parseUtc(recorded_row["date"] | "") +
                        cfg::kExpectedMatchMinutes * 60 + 60,
                    1000};
  plan::Request request;
  TEST_ASSERT_TRUE(planner.request(job(), entry(), store, clock, request));
  TEST_ASSERT_TRUE(
      test_support::consume(recorded_row, planner, entry().primary));
  planner.finish(true, clock);
  const auto& match = find(401884775);
  TEST_ASSERT_FALSE(store.isOwn(match));
  TEST_ASSERT_TRUE(store.hasDetail(match));
  TEST_ASSERT_EQUAL_UINT8(4, view::events(store, match).count);
  TEST_ASSERT_EQUAL_STRING(
      recorded_row["competitions"][0]["competitors"][0]["team"]["displayName"],
      view::teamName(store, match, true));
}

void test_foreign_summary_adds_substitution_with_both_names() {
  begin();
  row(10);
  planner.finish(true, now());
  auto target = job(cfg::DataType::kEvents);
  target.event_match = wanted().event_match;
  plan::Request request;
  TEST_ASSERT_TRUE(planner.request(target, entry(), store, now(), request));
  ArduinoJson::JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(
      doc, R"({"id":"50","type":{"type":"substitution"},"team":{"id":"91"},)"
           R"("clock":{"displayValue":"40'"},"participants":[)"
           R"({"athlete":{"displayName":"Coming on"}},)"
           R"({"athlete":{"displayName":"Going off"}}]})"));
  TEST_ASSERT_TRUE(test_support::consume(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  planner.finish(true, now());
  const auto events = view::events(store, find(10));
  TEST_ASSERT_EQUAL_UINT8(2, events.count);
  const auto& substitution = store.eventAt(events.index[1]);
  TEST_ASSERT_EQUAL(EventType::kSubstitution, substitution.type);
  TEST_ASSERT_EQUAL_STRING("Coming on", substitution.player_in.data);
  TEST_ASSERT_EQUAL_STRING("Going off", substitution.player.data);
  TEST_ASSERT_FALSE(store.isOwn(find(10)));
}

void test_event_polling_uses_exact_target_instead_of_another_competition_window() {
  StoredMatch finished;
  finished.id = 10;
  finished.competition = 1;
  finished.provider = kProvider;
  finished.phase = Phase::kFinished;
  finished.kickoff = kNow - 10000;
  finished.finished_at = kNow - 5000;
  store.upsert(finished, now());
  StoredMatch running_match = finished;
  running_match.id = 20;
  running_match.phase = Phase::kLive;
  running_match.kickoff = kNow - 600;
  running_match.finished_at = 0;
  store.upsert(running_match, now());
  schedule::Wanted demand;
  demand.id = 1;
  demand.provider = kProvider;
  demand.event_match = 10;
  demand.types = 1u << static_cast<unsigned>(cfg::DataType::kEvents);
  auto input = requests(demand);
  schedule::Scheduler scheduler;
  const auto first = scheduler.next(input);
  TEST_ASSERT_TRUE(first.run);  // one initial detail for this selected result
  scheduler.finished(first.job, schedule::Outcome::kOk, input.clock.mono_s);
  input.clock.mono_s +=
      plans[static_cast<unsigned>(kProvider)]
          .interval_s[static_cast<unsigned>(cfg::DataType::kEvents)];
  input.clock.utc += input.clock.mono_s - now().mono_s;
  TEST_ASSERT_FALSE(scheduler.next(input).run);
}

namespace {
// Exercise the real selection->demand->schedule->URL boundary. Only unrelated
// data types are masked off; selected identity, hold and priority are derived.
void checkSelectedSummaryCompletion(bool own, bool past_deadline) {
  store.clear();
  planner.reset();
  entry().club.team = own ? 99 : 0;
  refresh();
  auto& source = plans[static_cast<unsigned>(kProvider)];
  source.limit = {};
  source.interval_s[static_cast<unsigned>(cfg::DataType::kEvents)] = 30;
  StoredMatch m;
  m.competition = entry().id;
  m.provider = kProvider;
  m.id = 401892555;
  m.kickoff = kNow - 60;
  m.phase = Phase::kLive;
  m.home = store.team(m.competition, kProvider, own ? 99 : 1000000, "Home");
  store.selectClub(m.competition, kProvider, entry().club.team, now());
  TEST_ASSERT_NOT_NULL(store.upsert(m, now()));
  TEST_ASSERT_TRUE(store.ensureDetail(entry().id, kProvider, m.id, now()));

  const auto inputAt = [&](uint32_t dt) {
    const Clock clock{kNow + dt, 1000 + dt};
    schedule::Wanted demand;
    planner.wanted(0, &entry(), store, clock, demand);
    TEST_ASSERT_TRUE(wants(demand, cfg::DataType::kEvents));
    TEST_ASSERT_EQUAL_UINT32(m.id, demand.event_match);
    TEST_ASSERT_EQUAL(own, demand.event_priority);
    TEST_ASSERT_EQUAL_UINT8(0, demand.held);
    demand.types &= 1u << static_cast<unsigned>(cfg::DataType::kEvents);
    auto input = requests(demand);
    input.clock = clock;
    return input;
  };
  schedule::Scheduler scheduler;
  const auto summary = [&](const schedule::Inputs& input) {
    const auto decision = scheduler.next(input);
    TEST_ASSERT_TRUE(decision.run);
    TEST_ASSERT_EQUAL_UINT32(m.id, decision.job.event_match);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cfg::DataType::kEvents),
                            static_cast<uint8_t>(decision.job.type));
    plan::Request request;
    TEST_ASSERT_TRUE(
        planner.request(decision.job, entry(), store, input.clock, request));
    TEST_ASSERT_EQUAL_STRING(
        "https://site.api.espn.com/apis/site/v2/sports/soccer/ger.1/"
        "summary?event=401892555",
        test_support::http(request).url);
    planner.abort();
    scheduler.sent(decision.job.provider, input.clock.mono_s);
    scheduler.finished(decision.job, schedule::Outcome::kOk,
                       input.clock.mono_s);
  };
  summary(inputAt(20));
  m.phase = Phase::kFinished;
  m.finished_at = kNow + 120;
  TEST_ASSERT_NOT_NULL(store.upsert(m, {kNow + 120, 1120}));
  if (past_deadline) {
    // Final interval30s: elapsed31s forbids a final request despite the
    // still-selected held result and its explicit identity.
    TEST_ASSERT_FALSE(scheduler.next(inputAt(151)).run);
  } else {
    summary(inputAt(125));
    TEST_ASSERT_FALSE(scheduler.next(inputAt(140)).run);
  }
  TEST_ASSERT_EQUAL(own, store.isOwn(*store.find(entry().id, kProvider, m.id)));
}
}  // namespace

void test_planner_selected_own_summary_keeps_final_fetch_and_deadline() {
  checkSelectedSummaryCompletion(true, false);
  checkSelectedSummaryCompletion(true, true);
}

void test_planner_selected_ordinary_summary_keeps_final_fetch_and_deadline() {
  checkSelectedSummaryCompletion(false, false);
  checkSelectedSummaryCompletion(false, true);
}

void test_planner_rejects_a_summary_job_for_a_different_selected_match() {
  begin();
  row(10);
  planner.finish(true, now());
  const auto demand = wanted();
  TEST_ASSERT_EQUAL_UINT32(10, demand.event_match);
  auto selected_job = job(cfg::DataType::kEvents);
  selected_job.event_match = 20;
  plan::Request request;
  TEST_ASSERT_FALSE(
      planner.request(selected_job, entry(), store, now(), request));
  selected_job.event_match = demand.event_match;
  TEST_ASSERT_TRUE(
      planner.request(selected_job, entry(), store, now(), request));
  TEST_ASSERT_NOT_NULL(
      std::strstr(test_support::http(request).url, "summary?event=10"));
  planner.abort();
}

void test_preview_holds_summary_until_kickoff_without_faking_ownership() {
  begin();
  row(10, "STATUS_SCHEDULED", "pre", "2026-10-17T12:30:00Z");
  planner.finish(true, now());
  const auto demand = wanted();
  TEST_ASSERT_EQUAL_UINT32(10, demand.event_match);
  TEST_ASSERT_TRUE(demand.held &
                   (1u << static_cast<unsigned>(cfg::DataType::kEvents)));
  auto input = requests(demand);
  input.competitions[0].types =
      1u << static_cast<unsigned>(cfg::DataType::kEvents);
  schedule::Scheduler scheduler;
  TEST_ASSERT_FALSE(scheduler.next(input).run);
}

void test_proven_break_uses_its_own_manual_eligibility_profile() {
  options.automatic_break = true;
  for (auto& phase : options.shared)
    phase[screen::index(screen::Type::kTable)].enabled = false;
  options
      .shared[policy::index(policy::Phase::kBreak)]
             [screen::index(screen::Type::kTable)]
      .enabled = true;
  refresh();
  TEST_ASSERT_FALSE(wants(wanted(), cfg::DataType::kTable));
  planner.context(competitions, eligibility, {}, true);
  TEST_ASSERT_TRUE(wants(wanted(), cfg::DataType::kTable));
}

void test_protected_own_rows_can_evict_live_candidate_without_losing_buffer_events() {
  // Only two foreign rows fit. A subsequent own row evicts the farther live
  // row, leaving the earlier buffer as the final unique ordinary Single.
  entry().club.team = 99;
  refresh();
  for (unsigned i = 0; i < cfg::kStoreMatches - 2; ++i) {
    StoredMatch own;
    own.competition = 2;
    own.provider = kProvider;
    own.id = 1000 + i;
    own.home = store.team(2, kProvider, 1000000, "Own club");
    store.selectClub(2, kProvider, 1000000, now());
    own.phase = Phase::kScheduled;
    own.kickoff = kNow + 86400;
    TEST_ASSERT_NOT_NULL(store.upsert(own, now()));
  }
  begin();
  row(10, "STATUS_SCHEDULED", "pre", "2026-10-17T12:30:00Z");
  row(20, "STATUS_IN_PROGRESS", "in", "2026-10-17T11:00:00Z");
  row(30, "STATUS_SCHEDULED", "pre", "2026-10-27T12:00:00Z", 0, true);
  planner.finish(true, now());
  TEST_ASSERT_NULL(store.find(1, kProvider, 20));
  TEST_ASSERT_TRUE(store.hasDetail(find(10)));
  TEST_ASSERT_EQUAL_UINT8(1, view::events(store, find(10)).count);
  TEST_ASSERT_EQUAL_STRING("Full foreign home name",
                           view::teamName(store, find(10), true));
}

void test_rejected_row_never_appends_its_events_to_the_previous_candidate() {
  begin();
  row(10, "STATUS_IN_PROGRESS", "in", "2026-10-17T11:45:00Z", 1);
  planner.finish(true, now());
  TEST_ASSERT_TRUE(store.hasDetail(find(10)));
  for (unsigned i = 0; i < cfg::kStoreMatches - 1; ++i) {
    StoredMatch own;
    own.competition = 2;
    own.provider = kProvider;
    own.id = 1000 + i;
    own.home = store.team(2, kProvider, 1000000, "Own club");
    store.selectClub(2, kProvider, 1000000, now());
    own.phase = Phase::kScheduled;
    own.kickoff = kNow + 86400;
    TEST_ASSERT_NOT_NULL(store.upsert(own, now()));
  }
  begin();
  row(10, "STATUS_IN_PROGRESS", "in", "2026-10-17T11:45:00Z", 1);
  row(20, "STATUS_IN_PROGRESS", "in", "2026-10-17T11:00:00Z", 3, false, false);
  TEST_ASSERT_EQUAL(provider::Status::kTooLarge, planner.responseFailure());
  planner.finish(false, now());
  TEST_ASSERT_NULL(store.find(1, kProvider, 20));
  TEST_ASSERT_TRUE(store.hasDetail(find(10)));
  TEST_ASSERT_EQUAL_UINT8(1, view::events(store, find(10)).count);
}

void test_olg_discovery_filters_other_competitions_and_explicit_seasons() {
  entry().primary.provider = cfg::Provider::kOpenLigaDb;
  entry().primary.competition.assign("bl1");
  entry().club.team = 40;
  entry().primary.season.assign("2026");
  refresh();
  auto fixtures = job(cfg::DataType::kLive);
  fixtures.provider = cfg::Provider::kOpenLigaDb;
  plan::Request request;
  TEST_ASSERT_TRUE(planner.request(fixtures, entry(), store, now(), request));
  ArduinoJson::JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(
      doc, R"({"matchID":10,"leagueShortcut":"bl1","leagueSeason":2026,)"
           R"("group":{"groupOrderID":6}})"));
  TEST_ASSERT_TRUE(test_support::accepts(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  TEST_ASSERT_TRUE(test_support::consume(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  TEST_ASSERT_EQUAL(provider::Operation::kCurrentRound, request.spec.operation);
  doc["matchID"] = 20;
  doc["leagueShortcut"] = "dfb";
  TEST_ASSERT_FALSE(test_support::accepts(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  TEST_ASSERT_TRUE(test_support::consume(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  TEST_ASSERT_NULL(store.find(1, cfg::Provider::kOpenLigaDb, 20));
  doc["matchID"] = 30;
  doc["leagueShortcut"] = "BL1";
  doc["leagueSeason"] = 2025;
  TEST_ASSERT_FALSE(test_support::accepts(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  TEST_ASSERT_TRUE(test_support::consume(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  TEST_ASSERT_NULL(store.find(1, cfg::Provider::kOpenLigaDb, 30));
  doc["matchID"] = 40;
  doc["leagueSeason"] = 2026;
  TEST_ASSERT_TRUE(test_support::accepts(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  TEST_ASSERT_TRUE(test_support::consume(
      doc.as<ArduinoJson::JsonVariantConst>(), planner, entry().primary));
  TEST_ASSERT_NOT_NULL(store.find(1, cfg::Provider::kOpenLigaDb, 40));
  planner.finish(true, now());
}

void test_event_payload_begins_with_first_body_row_after_first_send_admission() {
  detail_allocation::watch = true;
  begin();
  TEST_ASSERT_EQUAL_UINT(0, detail_allocation::attempts);
  row(10);
  TEST_ASSERT_EQUAL_UINT(1, detail_allocation::attempts);
  row(20);
  TEST_ASSERT_EQUAL_UINT(1, detail_allocation::attempts);
  planner.finish(false, now());
}

void test_event_payload_allocation_failure_is_visible_and_not_retried_per_row() {
  detail_allocation::watch = detail_allocation::fail = true;
  begin();
  TEST_ASSERT_EQUAL_UINT(0, detail_allocation::attempts);
  row(10);
  row(20, "STATUS_CANCELED", "pre");
  TEST_ASSERT_EQUAL_UINT(1, detail_allocation::attempts);
  planner.finish(true, now());
  TEST_ASSERT_TRUE(store.notices(1) & kNoticeEvents);
  TEST_ASSERT_TRUE(find(10).flags & kMatchEventsPoolCut);
  TEST_ASSERT_EQUAL_UINT8(0, view::events(store, find(10)).count);
}

void test_empty_complete_response_keeps_existing_details_without_new_loss() {
  begin();
  row(10);
  planner.finish(true, now());
  TEST_ASSERT_EQUAL_UINT8(1, view::events(store, find(10)).count);
  TEST_ASSERT_FALSE(store.notices(1) & kNoticeEvents);
  detail_allocation::watch = true;
  begin();
  planner.finish(true, now());
  TEST_ASSERT_EQUAL_UINT(0, detail_allocation::attempts);
  TEST_ASSERT_EQUAL_UINT8(1, view::events(store, find(10)).count);
  TEST_ASSERT_FALSE(store.notices(1) & kNoticeEvents);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_initial_complete_response_keeps_foreign_names_and_events);
  RUN_TEST(test_partial_response_does_not_commit_new_foreign_details);
  RUN_TEST(test_late_cancellation_of_stale_competitor_keeps_initial_events);
  RUN_TEST(test_second_running_match_prevents_arbitrary_ordinary_target);
  RUN_TEST(test_running_match_wins_over_earlier_buffer_candidate);
  RUN_TEST(
      test_manual_single_requests_exact_foreign_summary_without_own_priority);
  RUN_TEST(
      test_special_off_owned_single_keeps_owned_bank_without_special_priority);
  RUN_TEST(test_disabled_views_retain_phase_classification_prerequisites);
  RUN_TEST(test_frame_has_no_automatic_provider_demand);
  RUN_TEST(test_duplicate_response_identity_keeps_detail_loss_visible);
  RUN_TEST(test_response_event_limit_keeps_latest_twelve_and_marks_truncation);
  RUN_TEST(test_changed_exact_target_runs_even_with_previous_target_interval);
  RUN_TEST(test_recorded_scoreboard_row_keeps_first_response_foreign_detail);
  RUN_TEST(test_foreign_summary_adds_substitution_with_both_names);
  RUN_TEST(
      test_event_polling_uses_exact_target_instead_of_another_competition_window);
  RUN_TEST(test_planner_selected_own_summary_keeps_final_fetch_and_deadline);
  RUN_TEST(
      test_planner_selected_ordinary_summary_keeps_final_fetch_and_deadline);
  RUN_TEST(test_planner_rejects_a_summary_job_for_a_different_selected_match);
  RUN_TEST(test_preview_holds_summary_until_kickoff_without_faking_ownership);
  RUN_TEST(test_proven_break_uses_its_own_manual_eligibility_profile);
  RUN_TEST(
      test_protected_own_rows_can_evict_live_candidate_without_losing_buffer_events);
  RUN_TEST(
      test_rejected_row_never_appends_its_events_to_the_previous_candidate);
  RUN_TEST(test_olg_discovery_filters_other_competitions_and_explicit_seasons);
  RUN_TEST(
      test_event_payload_begins_with_first_body_row_after_first_send_admission);
  RUN_TEST(
      test_event_payload_allocation_failure_is_visible_and_not_retried_per_row);
  RUN_TEST(
      test_empty_complete_response_keeps_existing_details_without_new_loss);
  return UNITY_END();
}
