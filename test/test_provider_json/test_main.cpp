// Protocol tests use the production template parser and allocator. These
// synthetic cases cover visible score preservation and proof rejection; fresh
// public bodies are replayed separately through the same header.
#include <unity.h>

#include <cstring>
#include <string>

#include "provider_json_stream.h"
#include "store_mappers.h"

namespace {
using namespace football;

class Stream {
 public:
  explicit Stream(const char* text, std::size_t limit = SIZE_MAX)
      : text_(text), limit_(limit) {}
  int peek() {
    if (position_ >= limit_ && text_[position_] != 0) {
      failed_ = true;
      return -1;
    }
    return text_[position_] ? static_cast<unsigned char>(text_[position_]) : -1;
  }
  int read() {
    const int c = peek();
    if (c < 0 || (number_ && !numberChar(c))) return -1;
    ++position_;
    return c;
  }
  void endAfterNumber(bool on) { number_ = on; }
  std::size_t readBytes(char* out, std::size_t count) {
    std::size_t n = 0;
    for (; n < count; ++n) {
      const int c = read();
      if (c < 0) break;
      out[n] = c;
    }
    return n;
  }
  int nonSpace() {
    int c;
    do {
      c = read();
    } while (json_detail::space(c));
    return c;
  }
  bool complete() const { return !failed_ && text_[position_] == 0; }
  std::size_t position() const { return position_; }

 private:
  static bool numberChar(int c) {
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
           c == 'e' || c == 'E';
  }
  const char* text_;
  std::size_t limit_;
  std::size_t position_ = 0;
  bool number_ = false;
  bool failed_ = false;
};
class Meter : public JsonAllocator {
 public:
  void* allocate(std::size_t n) override {
    void* p = JsonAllocator::allocate(n);
    observe();
    return p;
  }
  void* reallocate(void* p, std::size_t n) override {
    void* next = JsonAllocator::reallocate(p, n);
    observe();
    return next;
  }
  std::size_t peak = 0;

 private:
  void observe() {
    if (used() > peak) peak = used();
  }
};
struct Counts {
  unsigned primary = 0;
  unsigned meta = 0;
  unsigned sum = 0;
  unsigned parent_count = 0;
  bool enough = false;
  bool stop = false;
  char name[80]{};
};
bool primary(ArduinoJson::JsonVariantConst value, void* context) {
  auto& c = *static_cast<Counts*>(context);
  ++c.primary;
  if (value.is<unsigned>()) c.sum += value.as<unsigned>();
  if (c.stop) c.enough = true;
  return true;
}
bool metadata(uint8_t tag, ArduinoJson::JsonVariantConst value, void* context) {
  auto& c = *static_cast<Counts*>(context);
  ++c.meta;
  if (tag == 30) c.parent_count = value.as<unsigned>();
  if (value.is<unsigned>()) c.sum += value.as<unsigned>();
  if (tag == 9) {
    const char* name = value["team"]["displayName"] | "";
    std::strncpy(c.name, name, sizeof(c.name) - 1);
    TEST_ASSERT_TRUE(value["unused"].isNull());
  }
  return true;
}
const char* const kResults[] = {"results"};
const char* const kPaging[] = {"paging", "total"};
const char* const kCalendar[] = {"leagues", "calendar"};
const char* const kCompetitors[] = {"header", "competitions", "competitors"};
const char* const kSeason[] = {"leagues", "season", "year"};

bool parse(const char* body, Counts& c, const JsonObserver* observer = nullptr,
           JsonScan* scan = nullptr, bool optional = false,
           const char* array = "response", Meter* meter = nullptr) {
  Stream stream(body);
  Meter own;
  return readJson(stream, array, cfg::Provider::kEspn, primary, &c,
                  meter ? *meter : own, optional, &c.enough, observer, scan);
}

void test_primary_legacy_modes_and_number_separators() {
  Counts c;
  JsonScan scan;
  TEST_ASSERT_TRUE(
      parse("{\"x\":0,\"wrapper\":{\"response\":[1,2,3]},\"last\":9}", c,
            nullptr, &scan));
  TEST_ASSERT_EQUAL(3, c.primary);
  TEST_ASSERT_EQUAL(6, c.sum);
  TEST_ASSERT_TRUE(scan.document_complete);
  c = {};
  TEST_ASSERT_TRUE(parse("[1,2]", c, nullptr, &scan, false, ""));
  TEST_ASSERT_EQUAL(2, c.primary);
  c = {};
  TEST_ASSERT_TRUE(parse("{\"value\":1}", c, nullptr, &scan, false, nullptr));
  TEST_ASSERT_EQUAL(1, c.primary);
}
void test_metadata_before_and_after_scores_in_one_pass() {
  const JsonField fields[] = {
      {kResults, 1, JsonDelivery::kValue, JsonValueKind::kNumber},
      {kPaging, 2, JsonDelivery::kValue, JsonValueKind::kNumber}};
  Counts c;
  const JsonObserver observer{fields, 2, metadata, &c};
  JsonScan scan;
  Meter meter;
  TEST_ASSERT_TRUE(
      parse("{\"results\":2,\"response\":[7,8],\"paging\":{\"total\":1}}", c,
            &observer, &scan, false, "response", &meter));
  TEST_ASSERT_EQUAL(2, c.primary);
  TEST_ASSERT_EQUAL(2, c.meta);
  TEST_ASSERT_EQUAL(18, c.sum);
  TEST_ASSERT_EQUAL_HEX16(3, scan.seen);
  TEST_ASSERT_EQUAL_HEX16(3, scan.completed);
  TEST_ASSERT_EQUAL_HEX16(0, scan.wrong_type | scan.duplicates);
  TEST_ASSERT_TRUE(scan.document_complete);
  TEST_ASSERT_EQUAL(0, meter.used());
  TEST_ASSERT_LESS_OR_EQUAL(cfg::kJsonHeapBytes, meter.peak);
}
void test_absent_and_empty_metadata_do_not_claim_same_evidence() {
  const JsonField fields[] = {{kCalendar, 2, JsonDelivery::kArrayItems,
                               JsonValueKind::kString, nullptr, 0, 1}};
  Counts c;
  JsonObserver observer{fields, 1, metadata, &c};
  JsonScan scan;
  TEST_ASSERT_TRUE(parse("{\"response\":[],\"leagues\":[{\"calendar\":[]}]}", c,
                         &observer, &scan));
  TEST_ASSERT_EQUAL_HEX16(1, scan.seen);
  TEST_ASSERT_EQUAL_HEX16(1, scan.completed);
  TEST_ASSERT_EQUAL(0, c.meta);
  TEST_ASSERT_TRUE(parse("{\"response\":[]}", c, &observer, &scan));
  TEST_ASSERT_EQUAL_HEX16(0, scan.seen | scan.completed);
  TEST_ASSERT_TRUE(scan.document_complete);
}
void test_wrong_metadata_shape_keeps_scores_even_for_large_wrong_object() {
  const JsonField fields[] = {
      {kResults, 1, JsonDelivery::kValue, JsonValueKind::kNumber}};
  Counts c;
  JsonObserver observer{fields, 1, metadata, &c};
  JsonScan scan;
  const std::string body = "{\"results\":{\"huge\":\"" +
                           std::string(30000, 'x') + "\"},\"response\":[4]}";
  TEST_ASSERT_TRUE(parse(body.c_str(), c, &observer, &scan));
  TEST_ASSERT_EQUAL(1, c.primary);
  TEST_ASSERT_EQUAL(0, c.meta);
  TEST_ASSERT_EQUAL_HEX16(1, scan.wrong_type);
  TEST_ASSERT_FALSE(scan.allocation_failed);
  TEST_ASSERT_TRUE(scan.document_complete);
}
void test_wrong_array_or_item_type_is_only_metadata_failure() {
  const JsonField fields[] = {{kCalendar, 2, JsonDelivery::kArrayItems,
                               JsonValueKind::kString, nullptr, 0, 1}};
  Counts c;
  JsonObserver observer{fields, 1, metadata, &c};
  JsonScan scan;
  TEST_ASSERT_TRUE(parse("{\"leagues\":[{\"calendar\":null}],\"response\":[1]}",
                         c, &observer, &scan));
  TEST_ASSERT_EQUAL(1, c.primary);
  TEST_ASSERT_EQUAL_HEX16(1, scan.wrong_type);
  c = {};
  TEST_ASSERT_TRUE(
      parse("{\"leagues\":[{\"calendar\":[\"date\",{}]}],\"response\":[1]}", c,
            &observer, &scan));
  TEST_ASSERT_EQUAL(1, c.meta);
  TEST_ASSERT_EQUAL_HEX16(1, scan.wrong_type);
}
void test_anchored_paths_reject_other_parents_and_wrong_array_wrapper() {
  const JsonField fields[] = {{kSeason, 3, JsonDelivery::kValue,
                               JsonValueKind::kNumber, nullptr, 0, 1}};
  Counts c;
  JsonObserver observer{fields, 1, metadata, &c};
  JsonScan scan;
  TEST_ASSERT_TRUE(
      parse("{\"response\":[1],\"elsewhere\":{\"leagues\":[{\"season\":{"
            "\"year\":9}}]},\"leagues\":{\"season\":{\"year\":8}}}",
            c, &observer, &scan));
  TEST_ASSERT_EQUAL(0, c.meta);
  TEST_ASSERT_EQUAL_HEX16(0, scan.seen);
  TEST_ASSERT_TRUE(
      parse(R"({"response":[],"leagues":[{"season":[{"year":2026}]}]})", c,
            &observer, &scan));
  TEST_ASSERT_EQUAL_HEX16(0, scan.seen);
  TEST_ASSERT_TRUE(
      parse(R"({"response":[],"leagues":[[{"season":{"year":2026}}]]})", c,
            &observer, &scan));
  TEST_ASSERT_EQUAL_HEX16(0, scan.seen);
  TEST_ASSERT_TRUE(
      parse("{\"response\":[],\"leagues\":[{\"season\":{\"year\":2026}}]}", c,
            &observer, &scan));
  TEST_ASSERT_EQUAL(1, c.meta);
  TEST_ASSERT_EQUAL_HEX16(1, scan.completed);
}
void test_duplicates_mean_paths_not_array_items() {
  const JsonField fields[] = {{kCalendar, 2, JsonDelivery::kArrayItems,
                               JsonValueKind::kString, nullptr, 0, 1}};
  Counts c;
  JsonObserver observer{fields, 1, metadata, &c};
  JsonScan scan;
  TEST_ASSERT_TRUE(
      parse("{\"leagues\":[{\"calendar\":[\"a\",\"b\"]}],\"response\":[]}", c,
            &observer, &scan));
  TEST_ASSERT_EQUAL(2, c.meta);
  TEST_ASSERT_EQUAL_HEX16(0, scan.duplicates);
  TEST_ASSERT_TRUE(parse(
      "{\"leagues\":[{\"calendar\":[]},{\"calendar\":[]}],\"response\":[]}", c,
      &observer, &scan));
  TEST_ASSERT_EQUAL_HEX16(1, scan.duplicates);
}
void test_filtered_summary_names_do_not_load_unrelated_fields() {
  const JsonField fields[] = {
      {kCompetitors, 3, JsonDelivery::kArrayItems, JsonValueKind::kObject,
       R"({"homeAway":true,"team":{"id":true,"displayName":true}})", 9, 2}};
  Counts c;
  JsonObserver observer{fields, 1, metadata, &c};
  JsonScan scan;
  const std::string body =
      "{\"response\":[1],\"header\":{\"competitions\":[{\"competitors\":[{"
      "\"homeAway\":\"home\",\"team\":{\"id\":125,\"displayName\":\"Eintracht "
      "Frankfurt\"},\"unused\":\"" +
      std::string(30000, 'x') + "\"}]}]}}";
  TEST_ASSERT_TRUE(parse(body.c_str(), c, &observer, &scan));
  TEST_ASSERT_EQUAL_STRING("Eintracht Frankfurt", c.name);
  TEST_ASSERT_EQUAL(1, c.primary);
  TEST_ASSERT_EQUAL_HEX16(1, scan.completed);
}
void test_early_wizard_stop_cannot_establish_coverage() {
  Counts c;
  c.stop = true;
  JsonScan scan;
  Stream stream("{\"response\":[1,2],\"results\":2}");
  Meter meter;
  TEST_ASSERT_TRUE(readJson(stream, "response", cfg::Provider::kEspn, primary,
                            &c, meter, false, &c.enough, nullptr, &scan));
  TEST_ASSERT_EQUAL(1, c.primary);
  TEST_ASSERT_TRUE(scan.early_stopped);
  TEST_ASSERT_FALSE(scan.document_complete);
  TEST_ASSERT_LESS_THAN(34, stream.position());
}
void test_truncated_or_trailing_invalid_json_never_completes() {
  Counts c;
  JsonScan scan;
  TEST_ASSERT_FALSE(parse("{\"response\":[1]", c, nullptr, &scan));
  TEST_ASSERT_FALSE(scan.document_complete);
  TEST_ASSERT_FALSE(parse("{\"response\":[1]} garbage", c, nullptr, &scan));
  TEST_ASSERT_FALSE(scan.document_complete);
  TEST_ASSERT_FALSE(
      parse("{\"response\":[1],\"ignored\":truX}", c, nullptr, &scan));
  TEST_ASSERT_FALSE(scan.document_complete);
}
void test_body_depth_and_allocator_limits_remain_fatal() {
  Counts c;
  JsonScan scan;
  Meter meter;
  Stream limited("{\"response\":[1,2]}", 15);
  TEST_ASSERT_FALSE(readJson(limited, "response", cfg::Provider::kEspn, primary,
                             &c, meter, false, nullptr, nullptr, &scan));
  TEST_ASSERT_FALSE(scan.document_complete);
  const std::string large =
      "{\"response\":[\"" + std::string(30000, 'x') + "\"]}";
  TEST_ASSERT_FALSE(parse(large.c_str(), c, nullptr, &scan));
  TEST_ASSERT_TRUE(scan.allocation_failed);
  const std::string deep = "{\"response\":[],\"x\":" + std::string(20, '[') +
                           "0" + std::string(20, ']') + "}";
  TEST_ASSERT_FALSE(parse(deep.c_str(), c, nullptr, &scan));
}
void test_optional_absence_does_not_accept_wrong_primary_type() {
  Counts c;
  JsonScan scan;
  TEST_ASSERT_FALSE(parse("{}", c, nullptr, &scan));
  TEST_ASSERT_TRUE(parse("{}", c, nullptr, &scan, true));
  TEST_ASSERT_TRUE(scan.document_complete);
  TEST_ASSERT_FALSE(parse("{\"response\":{}}", c, nullptr, &scan, true));
}
void test_overlapping_observers_are_rejected_before_reading() {
  const char* const parent[] = {"leagues"};
  const JsonField fields[] = {
      {parent, 1, JsonDelivery::kValue, JsonValueKind::kArray},
      {kSeason, 3, JsonDelivery::kValue, JsonValueKind::kNumber}};
  Counts c;
  JsonObserver observer{fields, 2, metadata, &c};
  Stream stream("{\"response\":[1]}");
  Meter meter;
  JsonScan scan;
  TEST_ASSERT_FALSE(readJson(stream, "response", cfg::Provider::kEspn, primary,
                             &c, meter, false, nullptr, &observer, &scan));
  TEST_ASSERT_TRUE(scan.invalid_configuration);
  TEST_ASSERT_EQUAL(0, stream.position());
  TEST_ASSERT_EQUAL(0, meter.peak);
}

void test_skipped_malformed_tokens_cannot_establish_complete_document() {
  const JsonField fields[] = {
      {kResults, 1, JsonDelivery::kValue, JsonValueKind::kNumber}};
  Counts c;
  JsonObserver observer{fields, 1, metadata, &c};
  JsonScan scan;
  const char* bad[] = {R"({"results":{"n":1e+},"response":[1]})",
                       R"({"results":{"n":},"response":[1]})",
                       R"({"results":{"n":01},"response":[1]})",
                       R"({"results":{"n":1.},"response":[1]})",
                       R"({"results":{"n":[1,]},"response":[1]})",
                       R"({"results":{"n":tru},"response":[1]})",
                       R"({"results":{"n":falsex},"response":[1]})"};
  for (const char* body : bad) {
    TEST_ASSERT_FALSE_MESSAGE(parse(body, c, &observer, &scan), body);
    TEST_ASSERT_FALSE(scan.document_complete);
  }
  TEST_ASSERT_TRUE(parse(R"({"results":{"n":-1.25e+3},"response":[1]})", c,
                         &observer, &scan));
  TEST_ASSERT_TRUE(scan.document_complete);
}
void test_primary_filter_skips_still_require_valid_syntax() {
  Counts c;
  JsonScan scan;
  const char* bad[] = {R"({"events":[{"id":"1","ignored":{"n":1e+}}]})",
                       R"({"events":[{"id":"1","ignored":{"n":}}]})",
                       R"({"events":[{"id":"1","ignored":[1,,2]}]})"};
  for (const char* body : bad) {
    TEST_ASSERT_FALSE_MESSAGE(parse(body, c, nullptr, &scan, false, "events"),
                              body);
    TEST_ASSERT_FALSE(scan.document_complete);
  }
}
void test_skipped_escapes_unicode_and_utf8_are_checked() {
  Counts c;
  JsonScan scan;
  const char* bad[] = {R"({"events":[{"id":"1","ignored":"\q"}]})",
                       R"({"events":[{"id":"1","ignored":"\u12xz"}]})",
                       R"({"events":[{"id":"1","ignored":"\ud800"}]})",
                       R"({"events":[{"id":"1","ignored":"\udc00"}]})",
                       R"({"events":[{"id":"1","ignored":"\ud800\u0041"}]})"};
  for (const char* body : bad) {
    TEST_ASSERT_FALSE_MESSAGE(parse(body, c, nullptr, &scan, false, "events"),
                              body);
    TEST_ASSERT_FALSE(scan.document_complete);
  }
  TEST_ASSERT_TRUE(parse(
      R"({"events":[{"id":"1","ignored":"\ud83d\ude00 \u00e4 München"}]})", c,
      nullptr, &scan, false, "events"));
  const std::string invalid =
      std::string("{\"events\":[{\"id\":\"1\",\"ignored\":\"") + char(0xc0) +
      char(0xaf) + "\"}]}";
  TEST_ASSERT_FALSE(parse(invalid.c_str(), c, nullptr, &scan, false, "events"));
}
void test_disjoint_league_metadata_requires_one_proven_parent() {
  const char* const leagues[] = {"leagues"};
  const char* const whitelist[] = {"leagues", "calendarIsWhitelist"};
  const JsonField fields[] = {
      {leagues, 1, JsonDelivery::kArrayCount, JsonValueKind::kNumber, nullptr,
       30},
      {kSeason, 3, JsonDelivery::kValue, JsonValueKind::kNumber, nullptr, 1, 1},
      {whitelist, 2, JsonDelivery::kValue, JsonValueKind::kBoolean, nullptr, 2,
       1},
      {kCalendar, 2, JsonDelivery::kArrayItems, JsonValueKind::kString, nullptr,
       3, 1}};
  Counts c;
  JsonObserver observer{fields, 4, metadata, &c};
  JsonScan scan;
  TEST_ASSERT_TRUE(parse(
      R"({"response":[1],"leagues":[{"season":{"year":2026},"calendarIsWhitelist":true},{"calendar":["date"]}]})",
      c, &observer, &scan));
  TEST_ASSERT_EQUAL_HEX16(15, scan.seen & scan.completed);
  TEST_ASSERT_EQUAL_HEX16(0, scan.duplicates);
  TEST_ASSERT_EQUAL(2, c.parent_count);  // cannot be one league's proof
  TEST_ASSERT_TRUE(parse(
      R"({"response":[1],"leagues":[{"season":{"year":2026},"calendarIsWhitelist":true,"calendar":["date"]}]})",
      c, &observer, &scan));
  TEST_ASSERT_EQUAL(1, c.parent_count);
  TEST_ASSERT_EQUAL_HEX16(15, scan.seen & scan.completed);
}

struct Names {
  uint32_t match = 0;
  unsigned count = 0;
  bool own = false;
  char home[80]{};
  char away[80]{};
};
void names(const StoredMatch& match, std::string_view home,
           std::string_view away, void* context) {
  auto& n = *static_cast<Names*>(context);
  ++n.count;
  n.match = match.id;
  n.own = match.isOwn();
  std::memcpy(n.home, home.data(), home.size());
  std::memcpy(n.away, away.data(), away.size());
}
void test_first_scoreboard_mapping_exposes_full_ordinary_names() {
  MatchStore store;
  Names observed;
  MapContext context{1,        cfg::Provider::kEspn, 0, {1792245600, 1}, names,
                     &observed};
  ArduinoJson::JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(
      doc,
      R"({"id":"42","date":"2026-10-17T15:00Z","competitions":[{"status":{"type":{"state":"in","name":"STATUS_IN_PROGRESS"}},"competitors":[{"homeAway":"home","team":{"id":"1","displayName":"Home full provider name","shortDisplayName":"Home"}},{"homeAway":"away","team":{"id":"2","displayName":"Away full provider name","shortDisplayName":"Away"}}]}]})"));
  TEST_ASSERT_TRUE(
      storeMatch(doc.as<ArduinoJson::JsonVariantConst>(), store, context));
  TEST_ASSERT_EQUAL(1, observed.count);
  TEST_ASSERT_EQUAL_UINT32(42, observed.match);
  TEST_ASSERT_FALSE(observed.own);
  TEST_ASSERT_EQUAL_STRING("Home full provider name", observed.home);
  TEST_ASSERT_EQUAL_STRING("Away full provider name", observed.away);
  const auto* match = store.find(1, cfg::Provider::kEspn, 42);
  TEST_ASSERT_NOT_NULL(match);
  TEST_ASSERT_FALSE(store.hasDetail(*match));
}
void test_object_count_keeps_raw_duplicate_keys_and_container_identity() {
  const char* const parent[] = {"parameters"};
  const char* const league[] = {"parameters", "league"};
  const JsonField fields[] = {
      {parent, 1, JsonDelivery::kObjectCount, JsonValueKind::kNumber, nullptr,
       30},
      {league, 2, JsonDelivery::kValue, JsonValueKind::kString, nullptr, 1}};
  Counts c;
  JsonObserver observer{fields, 2, metadata, &c};
  JsonScan scan;
  TEST_ASSERT_TRUE(parse(
      R"({"parameters":{"league":"99","league":"78","season":2026},"response":[1]})",
      c, &observer, &scan));
  TEST_ASSERT_EQUAL_UINT32(3, c.parent_count);
  TEST_ASSERT_EQUAL_HEX16(2, scan.duplicates);
  TEST_ASSERT_EQUAL_UINT32(1, c.primary);
  c = {};
  TEST_ASSERT_TRUE(
      parse(R"({"parameters":{},"response":[1]})", c, &observer, &scan));
  TEST_ASSERT_EQUAL_UINT32(0, c.parent_count);
  TEST_ASSERT_EQUAL_HEX16(1, scan.completed);
  c = {};
  TEST_ASSERT_TRUE(
      parse(R"({"parameters":[],"response":[1]})", c, &observer, &scan));
  TEST_ASSERT_EQUAL_HEX16(1, scan.wrong_type);
  TEST_ASSERT_EQUAL_UINT32(1, c.primary);
  c = {};
  TEST_ASSERT_TRUE(parse(
      R"({"parameters":{"league":"78"},"parameters":{"season":2026},"response":[1]})",
      c, &observer, &scan));
  TEST_ASSERT_EQUAL_HEX16(1, scan.duplicates);
}
void test_primary_array_location_and_duplicates_do_not_remove_scores() {
  Counts c;
  JsonScan scan;
  TEST_ASSERT_TRUE(parse(R"({"outer":{"response":[1]}})", c, nullptr, &scan));
  TEST_ASSERT_EQUAL_UINT32(1, c.primary);
  TEST_ASSERT_EQUAL_UINT8(2, scan.primary_depth);
  TEST_ASSERT_FALSE(scan.primary_duplicates);
  c = {};
  TEST_ASSERT_TRUE(
      parse(R"({"response":[],"response":[1]})", c, nullptr, &scan));
  TEST_ASSERT_EQUAL_UINT32(1, c.primary);
  TEST_ASSERT_EQUAL_UINT8(1, scan.primary_depth);
  TEST_ASSERT_TRUE(scan.primary_duplicates);
}
}  // namespace

void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_primary_legacy_modes_and_number_separators);
  RUN_TEST(test_metadata_before_and_after_scores_in_one_pass);
  RUN_TEST(test_absent_and_empty_metadata_do_not_claim_same_evidence);
  RUN_TEST(test_wrong_metadata_shape_keeps_scores_even_for_large_wrong_object);
  RUN_TEST(test_wrong_array_or_item_type_is_only_metadata_failure);
  RUN_TEST(test_anchored_paths_reject_other_parents_and_wrong_array_wrapper);
  RUN_TEST(test_duplicates_mean_paths_not_array_items);
  RUN_TEST(test_filtered_summary_names_do_not_load_unrelated_fields);
  RUN_TEST(test_early_wizard_stop_cannot_establish_coverage);
  RUN_TEST(test_truncated_or_trailing_invalid_json_never_completes);
  RUN_TEST(test_body_depth_and_allocator_limits_remain_fatal);
  RUN_TEST(test_optional_absence_does_not_accept_wrong_primary_type);
  RUN_TEST(test_overlapping_observers_are_rejected_before_reading);
  RUN_TEST(test_first_scoreboard_mapping_exposes_full_ordinary_names);
  RUN_TEST(test_skipped_malformed_tokens_cannot_establish_complete_document);
  RUN_TEST(test_primary_filter_skips_still_require_valid_syntax);
  RUN_TEST(test_skipped_escapes_unicode_and_utf8_are_checked);
  RUN_TEST(test_disjoint_league_metadata_requires_one_proven_parent);
  RUN_TEST(test_object_count_keeps_raw_duplicate_keys_and_container_identity);
  RUN_TEST(test_primary_array_location_and_duplicates_do_not_remove_scores);
  return UNITY_END();
}
