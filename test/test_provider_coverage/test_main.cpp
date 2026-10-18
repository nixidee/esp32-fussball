// Semantic provider proof through the actual shared streaming parser. These
// controlled replies establish rejection behavior; real bodies are replayed
// separately and keyed-provider runtime acceptance remains independent.
#include <unity.h>

#include <cstring>
#include <ctime>
#include <string>

#include "provider_adapter.h"
#include "provider_coverage.h"

namespace {
using namespace football;
namespace c = football::coverage;
constexpr uint32_t kNow = 1792238400;  // 2026-10-17T12:00:00Z
settings::Competition entry;
c::Query query;
c::Response response;
struct Stream {
  std::string_view body;
  std::size_t at = 0, limit = SIZE_MAX;
  static int read(void* raw) {
    auto& input = *static_cast<Stream*>(raw);
    return input.at < input.body.size() && input.at < input.limit
               ? static_cast<unsigned char>(input.body[input.at++])
               : -1;
  }
};
bool replay(const std::string& body, std::size_t limit = SIZE_MAX) {
  response.begin(entry, query, kNow);
  Stream stream{body, 0, limit};
  json::Reader reader({&stream, Stream::read});
  auto request = query.http.spec;
  request.from = query.from ? query.from : request.from;
  request.to = query.to;
  provider::Sink sink;
  sink.fixture = [](const provider::Fixture& fixture,
                    std::span<const provider::Event>, void*) {
    response.observe(fixture);
    return true;
  };
  const auto result = provider::readResponse(reader, request, sink);
  response.parsed(result);
  // Preserve this fixture helper's grammar-only return contract. Semantic
  // failures remain in the typed result and cannot certify coverage.
  return reader.error() == json::Error::kNone && stream.at == body.size();
}
provider::HttpRequest http() {
  provider::HttpRequest request;
  TEST_ASSERT_TRUE(provider::buildRequest(query.http.spec, request));
  return request;
}
void source(cfg::Provider provider, const char* competition) {
  entry = {};
  entry.id = 1;
  entry.incarnation = 12;
  entry.primary.provider = provider;
  entry.primary.competition.assign(competition);
  entry.primary.season.assign("2026");
  if (provider == cfg::Provider::kEspn)
    TEST_ASSERT_FALSE(c::discoveryQuery(entry, {}, kNow, 40, 0, query));
  else
    TEST_ASSERT_TRUE(c::discoveryQuery(entry, {}, kNow, 40, 0, query));
}
const char* kOlRows =
    R"([{"matchID":1,"leagueShortcut":"bl1","leagueSeason":2026,"matchDateTimeUTC":"2026-09-01T12:00Z"},)"
    R"({"matchID":2,"leagueShortcut":"bl1","leagueSeason":2026,"matchDateTimeUTC":"2026-12-01T12:00Z"}])";

std::string api(const char* extra = "", const char* paging = "1",
                const char* results = "2", const char* status = "NS") {
  char from[11], to[11];
  const auto format = [](uint32_t at, char* out) {
    const time_t stamp = at;
    std::tm tm{};
    gmtime_r(&stamp, &tm);
    strftime(out, 11, "%Y-%m-%d", &tm);
  };
  format(query.from, from);
  format(query.to, to);
  return std::string(
             R"({"parameters":{"league":"78","season":"2026","from":")") +
         from + R"(","to":")" + to + extra +
         R"("},"paging":{"current":1,"total":)" + paging + R"(},"results":)" +
         results + R"(,"errors":[],"response":[)" +
         R"({"league":{"id":78,"season":2026},"fixture":{"id":1,"date":"2026-09-10T12:00Z","status":{"short":")" +
         status + R"("}}},)" +
         R"({"league":{"id":78,"season":2026},"fixture":{"id":2,"date":"2026-11-20T12:00Z","status":{"short":"NS"}}}]})";
}

void full_season_reduces_all_rows_independent_of_display_store() {
  source(cfg::Provider::kOpenLigaDb, "bl1");
  TEST_ASSERT_TRUE(replay(kOlRows));
  const auto& span = response.finish(true, kNow);
  TEST_ASSERT_EQUAL(c::Completeness::kComplete, span.completeness);
  TEST_ASSERT_EQUAL_UINT32(parseUtc("2026-09-01T12:00Z"), span.previous);
  TEST_ASSERT_EQUAL_UINT32(parseUtc("2026-12-01T12:00Z"), span.next);
  TEST_ASSERT_EQUAL_UINT32(span.previous, span.from);
  TEST_ASSERT_EQUAL_UINT32(span.next, span.to);
}
void full_season_wrong_identity_unknown_timing_empty_and_body_cap_fail_closed() {
  source(cfg::Provider::kOpenLigaDb, "bl1");
  std::string wrong = kOlRows;
  wrong.replace(wrong.find("bl1"), 3, "bl2");
  TEST_ASSERT_TRUE(replay(wrong));
  TEST_ASSERT_EQUAL(c::Reason::kIdentity, response.finish(true, kNow).reason);
  TEST_ASSERT_TRUE(replay("[]"));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  TEST_ASSERT_FALSE(replay(kOlRows, 100));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(false, kNow).completeness);
}
void date_range_requires_parameters_count_and_all_pages() {
  source(cfg::Provider::kApiFootball, "78");
  TEST_ASSERT_TRUE(replay(api()));
  TEST_ASSERT_EQUAL(c::Completeness::kComplete,
                    response.finish(true, kNow).completeness);
  TEST_ASSERT_TRUE(replay(api("", "2")));
  TEST_ASSERT_EQUAL(c::Reason::kProviderLimit,
                    response.finish(true, kNow).reason);
  TEST_ASSERT_TRUE(replay(api("", "1", "1")));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  TEST_ASSERT_TRUE(replay(api("", "1", "2", "TBD")));
  TEST_ASSERT_EQUAL(c::Reason::kUndatedFixture,
                    response.finish(true, kNow).reason);
}
void narrowed_or_duplicate_wrapper_never_proves_absence() {
  source(cfg::Provider::kApiFootball, "78");
  auto body = api();
  const auto position = body.find("\"paging\"");
  body.insert(position,
              R"("parameters":{"league":"78","season":"2026","team":"42"},)");
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  body = api();
  body.insert(body.find("\"league\":\"78\""), R"("league":"99",)");
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  body = api();
  body.insert(body.find("\"current\":1"), R"("current":2,)");
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  body = api();
  body.insert(body.find("},\"paging\""), R"(,"team":"42")");
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
}
void nested_or_duplicate_primary_preserves_rows_without_certifying_coverage() {
  source(cfg::Provider::kApiFootball, "78");
  auto body = api();
  body.replace(body.find("\"response\":"), 11, R"("other":{"response":)");
  body += "}";
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  body = api();
  body.insert(body.find("\"response\":"), R"("response":[],)");
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
}
void football_data_exclusive_end_and_narrowing_are_verified() {
  source(cfg::Provider::kFootballData, "BL1");
  std::string body =
      R"({"filters":{"dateFrom":"2026-09-07","dateTo":"2026-11-27","season":"2026","permission":"TIER_ONE"},"competition":{"id":2002,"code":"BL1"},"resultSet":{"count":0},"matches":[]})";
  // Query dates are computed from the anchor, and must match the provider's
  // exact applied range; this test never substitutes a wider interval.
  query.from = parseUtc("2026-09-07T00:00Z");
  query.to = parseUtc("2026-11-27T00:00Z") - 1;
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kComplete,
                    response.finish(true, kNow).completeness);
  auto foreign_season = body;
  foreign_season.replace(foreign_season.find("\"count\":0"), 9, "\"count\":1");
  foreign_season.replace(
      foreign_season.find("\"matches\":[]"), 12,
      R"("matches":[{"competition":{"code":"BL1"},"season":{"startDate":"2025-08-01"},"utcDate":"2026-10-17T12:00Z","status":"TIMED"}])");
  TEST_ASSERT_TRUE(replay(foreign_season));
  TEST_ASSERT_EQUAL(c::Reason::kIdentity, response.finish(true, kNow).reason);
  auto duplicate_filter = body;
  duplicate_filter.insert(duplicate_filter.find("\"dateFrom\""),
                          R"("dateFrom":"2026-10-01",)");
  TEST_ASSERT_TRUE(replay(duplicate_filter));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  auto duplicate_source = body;
  duplicate_source.insert(duplicate_source.find("\"code\":\"BL1\""),
                          R"("code":"BL2",)");
  TEST_ASSERT_TRUE(replay(duplicate_source));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  body.insert(body.find("},\"competition\""), R"(,"limit":100)");
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
}
void espn_extra_discovery_is_unavailable_and_cannot_invent_a_break() {
  source(cfg::Provider::kEspn, "aus.1");
  TEST_ASSERT_FALSE(provider::capabilities(entry.primary.provider).coverage);
  c::EvidenceArray proof{};
  c::bind(proof[0], c::identity(entry));
  settings::CompetitionRoutes configured;
  configured.entries.push(entry);
  TEST_ASSERT_EQUAL(c::Verdict::kUnknown,
                    c::assess(proof, configured, kNow, 21).verdict);
  // Ordinary season calendars remain a supported retention request.
  provider::HttpRequest calendar;
  provider::RequestSpec request;
  request.source = {entry.primary.provider, entry.primary.competition.view(),
                    entry.primary.season.view()};
  request.operation = provider::Operation::kSeasonCalendar;
  TEST_ASSERT_TRUE(provider::buildRequest(request, calendar));
  TEST_ASSERT_NOT_NULL(strstr(calendar.url, "/calendar/ondays"));
}
void source_query_is_bounded_encoded_and_does_not_resolve_unknown_years() {
  source(cfg::Provider::kOpenLigaDb, "league test");
  TEST_ASSERT_NOT_NULL(strstr(http().url, "league%20test/2026"));
  TEST_ASSERT_TRUE(c::discoveryQuery(entry, {}, kNow, 40, 1, query));
  TEST_ASSERT_EQUAL_STRING("2027", query.season.data.data());
  TEST_ASSERT_FALSE(c::discoveryQuery(entry, {}, kNow, 40, 2, query));
  entry.primary.season = {};
  TEST_ASSERT_FALSE(c::discoveryQuery(entry, {}, kNow, 40, 0, query));
  source(cfg::Provider::kFootballData, "BL1");
  TEST_ASSERT_NOT_NULL(strstr(http().url, "dateTo=2026-11-27"));
}
void normal_rows_only_invalidate_and_never_refresh_coverage() {
  source(cfg::Provider::kOpenLigaDb, "bl1");
  TEST_ASSERT_TRUE(replay(kOlRows));
  c::Evidence evidence;
  const auto key = c::identity(entry);
  c::bind(evidence, key);
  TEST_ASSERT_EQUAL(c::Reason::kNone,
                    c::accept(evidence, key, response.finish(true, kNow)));
  provider::Fixture row;
  row.source_matches = true;
  row.time_definite = true;
  row.facts.kickoff = parseUtc("2026-08-01T12:00Z");
  TEST_ASSERT_FALSE(c::observeNormalRow(evidence, entry, row));
  TEST_ASSERT_EQUAL_UINT32(kNow, evidence.seasons[0].checked_utc);
  row.facts.kickoff = parseUtc("2026-10-17T12:00Z");
  TEST_ASSERT_TRUE(c::observeNormalRow(evidence, entry, row));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    evidence.seasons[0].completeness);
  TEST_ASSERT_EQUAL(c::Reason::kTimingChanged, evidence.seasons[0].reason);
}
void espn_provisional_time_refutes_an_old_proof_before_display_admission() {
  source(cfg::Provider::kEspn, "aus.1");
  c::Evidence evidence;
  const auto key = c::identity(entry);
  c::bind(evidence, key);
  c::Collector collector;
  collector.begin(key, "2026", kNow - 86400, kNow + 86400, kNow,
                  c::Proof::kDateRange);
  collector.observe(kNow - 86400);
  collector.observe(kNow + 86400);
  c::accept(evidence, key, collector.finish(kNow, {2, true, true, true, true}));
  provider::Fixture row;
  row.source_matches = true;
  row.time_definite = false;
  row.facts.kickoff = parseUtc("2026-10-17T04:00Z");
  TEST_ASSERT_TRUE(c::observeNormalRow(evidence, entry, row));
  TEST_ASSERT_EQUAL(c::Reason::kUndatedFixture, evidence.seasons[0].reason);
  TEST_ASSERT_EQUAL_UINT32(kNow, evidence.seasons[0].checked_utc);
}
}  // namespace
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(full_season_reduces_all_rows_independent_of_display_store);
  RUN_TEST(
      full_season_wrong_identity_unknown_timing_empty_and_body_cap_fail_closed);
  RUN_TEST(date_range_requires_parameters_count_and_all_pages);
  RUN_TEST(narrowed_or_duplicate_wrapper_never_proves_absence);
  RUN_TEST(
      nested_or_duplicate_primary_preserves_rows_without_certifying_coverage);
  RUN_TEST(football_data_exclusive_end_and_narrowing_are_verified);
  RUN_TEST(espn_extra_discovery_is_unavailable_and_cannot_invent_a_break);
  RUN_TEST(source_query_is_bounded_encoded_and_does_not_resolve_unknown_years);
  RUN_TEST(normal_rows_only_invalidate_and_never_refresh_coverage);
  RUN_TEST(espn_provisional_time_refutes_an_old_proof_before_display_admission);
  return UNITY_END();
}
