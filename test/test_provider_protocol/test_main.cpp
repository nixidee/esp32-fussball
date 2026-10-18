// Actual typed provider consumers plus the shared grammar. Legacy observer
// configuration, wrapper walking and ArduinoJson allocator contracts are gone.
#include <unity.h>

#include <cstdlib>
#include <new>
#include <string>

#include "provider_adapter.h"

namespace allocation {
bool fail = false;
unsigned calls = 0;
}  // namespace allocation
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  ++allocation::calls;
  return allocation::fail ? nullptr : std::malloc(bytes);
}
namespace {
using namespace football;
struct Input {
  std::string_view text;
  std::size_t at = 0;
  static int read(void* raw) {
    auto& in = *static_cast<Input*>(raw);
    return in.at < in.text.size() ? static_cast<unsigned char>(in.text[in.at++])
                                  : -1;
  }
};
struct Facts {
  unsigned rows = 0;
  Goals score;
  bool cut = false;
};
provider::ParseResult parse(std::string_view body, Facts& facts,
                            provider::RequestSpec request = {}) {
  request.source.provider = cfg::Provider::kFootballData;
  Input input{body};
  json::Reader reader({&input, Input::read});
  provider::Sink sink;
  sink.context = &facts;
  sink.fixture = [](const provider::Fixture& row,
                    std::span<const provider::Event>, void* raw) {
    auto& out = *static_cast<Facts*>(raw);
    ++out.rows;
    out.score = row.facts.principal;
    out.cut = row.events_truncated;
    return true;
  };
  return provider::readResponse(reader, request, sink);
}
const char* fixture =
    R"({"id":1,"utcDate":"2026-10-17T13:30:00Z","status":"IN_PLAY","score":{"duration":"REGULAR","fullTime":{"home":2,"away":1}}})";
std::string document(std::string_view before = {},
                     std::string_view after = {}) {
  return "{" + std::string(before) + "\"matches\":[" + fixture + "]" +
         std::string(after) + "}";
}
void malformed_metadata_keeps_valid_scores_without_scope_proof() {
  for (const std::string& body :
       {document("\"filters\":{},"), document({}, ",\"filters\":{}"),
        document("\"filters\":[],"),
        document("\"filters\":{\"unknown\":\"" + std::string(30000, 'x') +
                 "\"},")}) {
    Facts facts;
    const auto result = parse(body, facts);
    TEST_ASSERT_EQUAL_UINT(1, facts.rows);
    TEST_ASSERT_EQUAL_UINT8(2, facts.score.home);
    TEST_ASSERT_EQUAL_UINT8(1, facts.score.away);
    TEST_ASSERT_FALSE(result.metadata.metadata_complete);
  }
}
void only_the_declared_primary_can_provide_fixtures() {
  Facts facts;
  auto result = parse(
      std::string("{\"wrapper\":{\"matches\":[") + fixture + "]}}", facts);
  TEST_ASSERT_EQUAL(provider::Status::kMissing, result.status);
  TEST_ASSERT_EQUAL_UINT(0, facts.rows);
  result = parse(document({}, ",\"matches\":[]"), facts);
  TEST_ASSERT_EQUAL_UINT(1, facts.rows);
  TEST_ASSERT_EQUAL(provider::Status::kBadResponse, result.status);
  TEST_ASSERT_FALSE(result.document_complete);
}
void absent_empty_and_wrong_primary_have_distinct_results() {
  Facts facts;
  TEST_ASSERT_EQUAL(provider::Status::kMissing, parse("{}", facts).status);
  const auto empty = parse("{\"matches\":[]}", facts);
  TEST_ASSERT_EQUAL(provider::Status::kOk, empty.status);
  TEST_ASSERT_TRUE(empty.primary_seen && empty.document_complete);
  TEST_ASSERT_EQUAL(provider::Status::kBadResponse,
                    parse("{\"matches\":{}}", facts).status);
  TEST_ASSERT_EQUAL_UINT(0, facts.rows);
}
void skipped_invalid_numbers_escapes_and_unicode_cannot_complete() {
  const char* bad[] = {"1e+",
                       "01",
                       "1.",
                       "tru",
                       "[1,]",
                       "falsex",
                       R"("\q")",
                       R"("\u12xz")",
                       R"("\ud800")",
                       R"("\udc00")",
                       R"("\ud800\u0041")"};
  for (const auto* value : bad) {
    Facts facts;
    const auto body = document({}, std::string(",\"ignored\":") + value);
    const auto result = parse(body, facts);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, facts.rows, value);
    TEST_ASSERT_EQUAL_MESSAGE(provider::Status::kBadResponse, result.status,
                              value);
    TEST_ASSERT_FALSE(result.document_complete);
  }
  Facts facts;
  const std::string invalid =
      std::string(",\"ignored\":\"") + char(0xc0) + char(0xaf) + '"';
  TEST_ASSERT_EQUAL(provider::Status::kBadResponse,
                    parse(document({}, invalid), facts).status);
}
void large_valid_unknown_fields_do_not_need_a_json_tree() {
  Facts facts;
  allocation::calls = 0;
  const auto body =
      document({}, ",\"ignored\":[-1.25e+3,\"\\ud83d\\ude00 München\",\"" +
                       std::string(30000, 'x') + "\"]");
  const auto result = parse(body, facts);
  TEST_ASSERT_EQUAL(provider::Status::kOk, result.status);
  TEST_ASSERT_TRUE(result.document_complete);
  TEST_ASSERT_EQUAL_UINT(1, facts.rows);
  TEST_ASSERT_EQUAL_UINT(1, allocation::calls);
}
void depth_truncation_and_invalid_suffix_keep_observed_score_only() {
  for (const auto& body :
       {document().substr(0, document().size() - 1), document() + "x",
        document({}, ",\"ignored\":" + std::string(cfg::kJsonDepth + 1, '[') +
                         "0" + std::string(cfg::kJsonDepth + 1, ']'))}) {
    Facts facts;
    const auto result = parse(body, facts);
    TEST_ASSERT_EQUAL_UINT(1, facts.rows);
    TEST_ASSERT_EQUAL_UINT8(2, facts.score.home);
    TEST_ASSERT_EQUAL(provider::Status::kBadResponse, result.status);
    TEST_ASSERT_FALSE(result.document_complete);
  }
}
void workspace_failure_is_memory_without_publication() {
  Facts facts;
  allocation::fail = true;
  const auto result = parse(document(), facts);
  allocation::fail = false;
  TEST_ASSERT_EQUAL(provider::Status::kNoMemory, result.status);
  TEST_ASSERT_EQUAL_UINT(0, facts.rows);
  TEST_ASSERT_FALSE(result.document_complete);
}
void decimal_numbers_cannot_round_into_a_different_fixture_or_score() {
  for (const auto* value : {"1.0000000000000000000001", "4294967295.0000000001",
                            "4294967296", "1e-1000"}) {
    Facts facts;
    const auto body = std::string("{\"matches\":[{\"id\":") + value + "}]}";
    TEST_ASSERT_EQUAL(provider::Status::kBadResponse,
                      parse(body, facts).status);
    TEST_ASSERT_EQUAL_UINT(0, facts.rows);
  }
  for (const auto* value :
       {"2.0000000000000000000001", "99.000000000000000000001"}) {
    Facts facts;
    const auto body =
        std::string(
            "{\"matches\":[{\"id\":1,\"score\":{\"fullTime\":{\"home\":") +
        value + ",\"away\":1}}}]}";
    parse(body, facts);
    TEST_ASSERT_EQUAL_UINT(1, facts.rows);
    TEST_ASSERT_EQUAL_UINT8(kNoValue, facts.score.home);
  }
  Facts exact;
  TEST_ASSERT_EQUAL(
      provider::Status::kOk,
      parse("{\"matches\":[{\"id\":1e2,\"score\":{\"duration\":\"REGULAR\","
            "\"fullTime\":{\"home\":2.0,\"away\":100e-2}}}]}",
            exact)
          .status);
  TEST_ASSERT_EQUAL_UINT8(2, exact.score.home);
  TEST_ASSERT_EQUAL_UINT8(1, exact.score.away);
}
}  // namespace
void setUp() { allocation::fail = false; }
void tearDown() { allocation::fail = false; }
int main() {
  UNITY_BEGIN();
  RUN_TEST(malformed_metadata_keeps_valid_scores_without_scope_proof);
  RUN_TEST(only_the_declared_primary_can_provide_fixtures);
  RUN_TEST(absent_empty_and_wrong_primary_have_distinct_results);
  RUN_TEST(skipped_invalid_numbers_escapes_and_unicode_cannot_complete);
  RUN_TEST(large_valid_unknown_fields_do_not_need_a_json_tree);
  RUN_TEST(depth_truncation_and_invalid_suffix_keep_observed_score_only);
  RUN_TEST(workspace_failure_is_memory_without_publication);
  RUN_TEST(decimal_numbers_cannot_round_into_a_different_fixture_or_score);
  return UNITY_END();
}
