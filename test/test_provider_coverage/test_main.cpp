// Semantic provider proof through the actual shared streaming parser. These
// controlled replies establish rejection behavior; real bodies are replayed
// separately and keyed-provider runtime acceptance remains independent.
#include <unity.h>

#include <cstring>
#include <ctime>
#include <string>

#include "provider_coverage.h"
#include "provider_json_stream.h"

namespace {
using namespace football;
namespace c = football::coverage;
constexpr uint32_t kNow = 1792238400;  // 2026-10-17T12:00:00Z
settings::Competition entry;
c::Query query;
c::Response response;
class Stream {
 public:
  explicit Stream(const char* value, std::size_t limit = SIZE_MAX)
      : text_(value), limit_(limit) {}
  int peek() {
    if (at_ >= limit_ && text_[at_] != 0) {
      failed_ = true;
      return -1;
    }
    return text_[at_] ? static_cast<unsigned char>(text_[at_]) : -1;
  }
  int read() {
    const int value = peek();
    if (value < 0 || (number_ && !numberChar(value))) return -1;
    ++at_;
    return value;
  }
  std::size_t readBytes(char* out, std::size_t count) {
    std::size_t n = 0;
    for (; n < count; ++n) {
      const int value = read();
      if (value < 0) break;
      out[n] = value;
    }
    return n;
  }
  int nonSpace() {
    int value;
    do {
      value = read();
    } while (json_detail::space(value));
    return value;
  }
  void endAfterNumber(bool on) { number_ = on; }
  bool complete() const { return !failed_ && text_[at_] == 0; }

 private:
  static bool numberChar(int value) {
    return (value >= '0' && value <= '9') || value == '-' || value == '+' ||
           value == '.' || value == 'e' || value == 'E';
  }
  const char* text_;
  std::size_t limit_, at_ = 0;
  bool number_ = false, failed_ = false;
};
bool observe(ArduinoJson::JsonVariantConst row, void*) {
  response.observe(row);
  return true;
}
bool replay(const std::string& body, std::size_t limit = SIZE_MAX) {
  response.begin(entry, query, kNow);
  Stream stream(body.c_str(), limit);
  JsonAllocator allocator;
  return readJson(stream, query.http.array, entry.primary.provider, observe,
                  nullptr, allocator, false, nullptr, response.observer(),
                  &response.scan());
}
void source(cfg::Provider provider, const char* competition) {
  entry = {};
  entry.id = 1;
  entry.incarnation = 12;
  entry.primary.provider = provider;
  entry.primary.competition.assign(competition);
  entry.primary.season.assign("2026");
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
void espn_day_whitelist_is_metadata_never_a_kickoff_proof() {
  source(cfg::Provider::kEspn, "aus.1");
  const char* body =
      R"({"leagues":[{"slug":"aus.1","season":{"year":2026},"calendarType":"day","calendarIsWhitelist":true,"calendarStartDate":"2026-07-01T04:00Z","calendarEndDate":"2027-07-01T03:59Z","calendar":["2026-10-17T07:00Z","2026-11-30T07:00Z"]}],"events":[{"id":"1","date":"2026-10-17T04:00Z","season":{"year":2026},"competitions":[{"timeValid":true}]}]})";
  TEST_ASSERT_TRUE(replay(body));
  TEST_ASSERT_EQUAL(c::Completeness::kPartial,
                    response.finish(true, kNow).completeness);
  TEST_ASSERT_TRUE(response.calendar().verified);
  TEST_ASSERT_EQUAL(c::Reason::kMissingNeighbours,
                    response.finish(true, kNow).reason);
}
void source_query_is_bounded_encoded_and_does_not_resolve_unknown_years() {
  source(cfg::Provider::kOpenLigaDb, "league test");
  TEST_ASSERT_NOT_NULL(strstr(query.http.url, "league%20test/2026"));
  TEST_ASSERT_TRUE(c::discoveryQuery(entry, {}, kNow, 40, 1, query));
  TEST_ASSERT_EQUAL_STRING("2027", query.season.data.data());
  TEST_ASSERT_FALSE(c::discoveryQuery(entry, {}, kNow, 40, 2, query));
  entry.primary.season = {};
  TEST_ASSERT_FALSE(c::discoveryQuery(entry, {}, kNow, 40, 0, query));
  source(cfg::Provider::kFootballData, "BL1");
  TEST_ASSERT_NOT_NULL(strstr(query.http.url, "dateTo=2026-11-27"));
}
void normal_rows_only_invalidate_and_never_refresh_coverage() {
  source(cfg::Provider::kOpenLigaDb, "bl1");
  TEST_ASSERT_TRUE(replay(kOlRows));
  c::Evidence evidence;
  const auto key = c::identity(entry);
  c::bind(evidence, key);
  TEST_ASSERT_EQUAL(c::Reason::kNone,
                    c::accept(evidence, key, response.finish(true, kNow)));
  ArduinoJson::JsonDocument row;
  deserializeJson(
      row,
      R"({"leagueShortcut":"bl1","leagueSeason":2026,"matchDateTimeUTC":"2026-08-01T12:00Z"})");
  TEST_ASSERT_FALSE(c::observeNormalRow(
      evidence, entry, row.as<ArduinoJson::JsonVariantConst>()));
  TEST_ASSERT_EQUAL_UINT32(kNow, evidence.seasons[0].checked_utc);
  row["matchDateTimeUTC"] = "2026-10-17T12:00Z";
  TEST_ASSERT_TRUE(c::observeNormalRow(
      evidence, entry, row.as<ArduinoJson::JsonVariantConst>()));
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
  ArduinoJson::JsonDocument row;
  deserializeJson(
      row,
      R"({"date":"2026-10-17T04:00Z","season":{"year":2026},"competitions":[{"timeValid":false}]})");
  TEST_ASSERT_TRUE(c::observeNormalRow(
      evidence, entry, row.as<ArduinoJson::JsonVariantConst>()));
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
  RUN_TEST(espn_day_whitelist_is_metadata_never_a_kickoff_proof);
  RUN_TEST(source_query_is_bounded_encoded_and_does_not_resolve_unknown_years);
  RUN_TEST(normal_rows_only_invalidate_and_never_refresh_coverage);
  RUN_TEST(espn_provisional_time_refutes_an_old_proof_before_display_admission);
  return UNITY_END();
}
