// The complete screen API round trip, strict errors and capped C6 allocation
// model. Host allocator measurements describe the host ABI, not device reserve.
#include <ArduinoJson.h>
#include <unity.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>

#include "screen_json.h"

namespace {
namespace policy = settings::screen_policy;
using namespace ArduinoJson;

policy::Policy model;
policy::Policy candidate;
JsonDocument doc;
settings::Violation violation;
uint32_t generation = 0;

void populate(bool maximum) {
  model = {};
  for (unsigned i = 0; i < model.competitions.size(); ++i)
    model.competitions[i] = {UINT32_MAX - i, static_cast<uint8_t>(255 - i),
                             !maximum};
  for (unsigned p = 0; p < model.shared.size(); ++p) {
    model.expanded[p] = 15;
    for (auto& rule : model.shared[p])
      rule = {static_cast<uint16_t>(maximum ? 43200 : 15), !maximum, true};
    for (auto& sport : model.overrides[p])
      for (auto& rule : sport)
        rule = {static_cast<uint16_t>(maximum ? 43200 : 15),
                static_cast<uint8_t>(maximum ? 0 : 3), 7};
  }
  for (unsigned i = 0; i < model.appearance.size(); ++i) {
    auto& style = model.appearance[i];
    style.colour = style.text = style.accent = maximum ? 0xffffff : 0;
    style.text_scale = maximum ? 130 : 70;
    style.image = i == screen::index(screen::Type::kCrest) ? 0
                  : maximum                                ? 10
                                                           : 0;
    style.background = style.dots = !maximum;
  }
  model.slides.fill(10);
  model.slide_count = model.slides.size();
  model.break_days = maximum ? 366 : 1;
  model.own_before_min = model.own_after_min = model.other_before_min =
      model.other_after_min = maximum ? 180 : 0;
  model.manual_return_s = maximum ? 4000 : 0;
  model.per_image_s = maximum ? 3600 : 3;
  model.visible_matches = maximum ? 5 : 2;
  model.table_window = maximum ? 9 : 1;
  model.backgrounds = model.pin_own_match = model.frame = model.frame_crest =
      model.automatic_break = !maximum;
  model.conference_mode =
      maximum ? cfg::ConferenceMode::kOnePage : cfg::ConferenceMode::kAllPages;
  model.matchday_mode =
      maximum ? cfg::MatchdayMode::kNext : cfg::MatchdayMode::kCurrent;
}

void encode(uint32_t expected_generation = 37) {
  doc.clear();
  web::screenPolicyJson(doc.to<JsonObject>(), model, expected_generation);
  TEST_ASSERT_FALSE(doc.overflowed());
}

bool apply(const policy::Limits& limits = {}) {
  return web::applyScreenPolicyJson(doc.as<JsonObjectConst>(), candidate,
                                    generation, &violation, limits);
}

void rejects(const char* path, settings::Reason reason) {
  TEST_ASSERT_FALSE(apply());
  TEST_ASSERT_EQUAL_STRING(path, violation.path.data());
  TEST_ASSERT_EQUAL(reason, violation.reason);
}

void complete_policy_keeps_all_options_and_rules() {
  for (const bool maximum : {false, true}) {
    populate(maximum);
    TEST_ASSERT_TRUE(policy::validate(model));
    encode(UINT32_MAX);
    std::string body;
    serializeJson(doc, body);
    doc.clear();
    TEST_ASSERT_FALSE(deserializeJson(doc, body));
    TEST_ASSERT_TRUE(apply());
    TEST_ASSERT_TRUE(candidate == model);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, generation);
  }
}

void omitted_override_fields_inherit_and_explicit_false_survives_collapse() {
  model.competitions[0] = {1234, 9, false};
  model.shared[3][2] = {90, true, true};
  auto& override = model.overrides[3][2][0];
  override = {15, 0, policy::kEnabled};
  encode();
  TEST_ASSERT_TRUE(doc["competitions"][0]["idle"]["table"][1].isNull());
  TEST_ASSERT_FALSE(doc["competitions"][0]["idle"]["table"][0].as<bool>());
  TEST_ASSERT_TRUE(apply());
  TEST_ASSERT_TRUE(candidate == model);
  // An inactive expansion retains the override without applying it.
  TEST_ASSERT_TRUE(policy::resolve(candidate, policy::Phase::kIdle,
                                   screen::Type::kTable, 9, 1234)
                       .enabled);
  candidate.expanded[3] = 1u << 2;
  const auto effective = policy::resolve(candidate, policy::Phase::kIdle,
                                         screen::Type::kTable, 9, 1234);
  TEST_ASSERT_FALSE(effective.enabled);
  TEST_ASSERT_TRUE(effective.automatic);
  TEST_ASSERT_EQUAL_UINT16(90, effective.visit_s);
}

void complete_object_and_boot_generation_are_required() {
  encode(0);
  TEST_ASSERT_TRUE(apply());
  TEST_ASSERT_EQUAL_UINT32(0, generation);
  doc.remove("generation");
  rejects("generation", settings::Reason::kRequired);
  encode();
  doc.remove("per_image_s");
  rejects("per_image_s", settings::Reason::kRequired);
  encode();
  doc["phases"]["idle"].remove("table");
  rejects("phases.idle.table", settings::Reason::kRequired);
  encode();
  doc["appearance"]["images"].remove("dots");
  rejects("appearance.images.dots", settings::Reason::kRequired);
}

void unknown_fields_and_embedded_nuls_cannot_be_silently_accepted() {
  encode();
  doc["secret"] = "x";
  rejects("secret", settings::Reason::kUnknownKey);
  encode();
  doc["phases"]["idle"]["unknown"] = false;
  rejects("phases.idle.unknown", settings::Reason::kUnknownKey);
  encode();
  const std::string key("frame\0x", 7);
  doc[key] = false;
  TEST_ASSERT_FALSE(apply());
  TEST_ASSERT_EQUAL(settings::Reason::kUnknownKey, violation.reason);
  encode();
  doc["order"][0] = std::string("crest\0x", 7);
  rejects("order[0]", settings::Reason::kInvalid);
}

void scalar_types_and_rule_tuple_shapes_are_strict() {
  encode();
  doc["frame"] = 1;
  rejects("frame", settings::Reason::kType);
  encode();
  doc["manual_return_s"] = -1;
  rejects("manual_return_s", settings::Reason::kRange);
  encode();
  doc["generation"] = uint64_t{UINT32_MAX} + 1;
  rejects("generation", settings::Reason::kRange);
  encode();
  doc["phases"]["idle"]["table"][0] = nullptr;
  rejects("phases.idle.table[0]", settings::Reason::kType);
  encode();
  doc["phases"]["idle"]["table"].as<JsonArray>().remove(2);
  rejects("phases.idle.table", settings::Reason::kRequired);
  encode();
  doc["phases"]["idle"]["table"].as<JsonArray>().add(0);
  rejects("phases.idle.table", settings::Reason::kTooMany);
}

void duplicate_order_and_expansion_types_are_rejected() {
  encode();
  doc["order"][1] = "crest";
  rejects("order[1]", settings::Reason::kDuplicate);
  encode();
  auto expansion = doc["phases"]["idle"]["expanded"].to<JsonArray>();
  expansion.add("table");
  expansion.add("table");
  rejects("phases.idle.expanded[1]", settings::Reason::kDuplicate);
  encode();
  doc["phases"]["idle"]["expanded"].to<JsonArray>().add("images");
  rejects("phases.idle.expanded[0]", settings::Reason::kInvalid);
}

void competition_identity_and_each_retained_rule_are_validated() {
  populate(true);
  encode();
  doc["competitions"][1]["id"] = 255;
  rejects("competitions[1].id", settings::Reason::kDuplicate);
  encode();
  doc["competitions"][1]["incarnation"] = UINT32_MAX;
  rejects("competitions[1].incarnation", settings::Reason::kDuplicate);
  encode();
  doc["competitions"][7]["matchday"]["conference"][2] = 43201;
  rejects("competitions[7].matchday.conference[2]", settings::Reason::kRange);
  encode();
  doc["competitions"][0]["id"] = 0;
  rejects("competitions[0].id", settings::Reason::kRange);
  encode();
  doc["competitions"][0]["idle"].remove("table");
  rejects("competitions[0].idle.table", settings::Reason::kRequired);
}

void image_visits_must_be_automatic_and_exactly_divisible() {
  encode();
  auto images = doc["phases"]["idle"]["images"];
  images[0] = true;
  images[1] = false;
  rejects("phases.idle.images[1]", settings::Reason::kConflict);
  images[1] = true;
  images[2] = 61;
  rejects("phases.idle.images[2]", settings::Reason::kConflict);
  images[2] = 60;
  TEST_ASSERT_TRUE(apply());
  doc["slides"].to<JsonArray>().add(0);
  rejects("slides[0]", settings::Reason::kRange);
}

void device_layout_limits_and_appearance_bounds_remain_enforced() {
  encode();
  doc["visible_matches"] = 1;
  rejects("visible_matches", settings::Reason::kRange);
  encode();
  doc["visible_matches"] = 5;
  TEST_ASSERT_FALSE(apply({3, 5}));
  TEST_ASSERT_EQUAL_STRING("visible_matches", violation.path.data());
  encode();
  doc["appearance"]["crest"]["image"] = 1;
  rejects("appearance.crest.image", settings::Reason::kRange);
  encode();
  doc["appearance"]["single"]["colour"] = 0x1000000;
  rejects("appearance.single.colour", settings::Reason::kRange);
}

// An independent walk counts ArduinoJson object members as a key and value
// slot, array entries as one value slot, and deduplicated owned strings. The
// fixed representation has no user-provided text, so this covers its longest
// valid input without assuming host pointer sizes equal the device's sizes.
struct Structure {
  std::size_t slots = 0;
  std::set<std::string> strings;
  std::size_t longest = 0;
  void text(JsonString value) {
    const std::string s(value.c_str(), value.size());
    longest = std::max(longest, s.size());
    if (s.size() > ArduinoJson::detail::tinyStringMaxLength) strings.insert(s);
  }
  void walk(JsonVariantConst value) {
    if (value.is<JsonObjectConst>()) {
      for (const auto pair : value.as<JsonObjectConst>()) {
        slots += 2;
        text(pair.key());
        walk(pair.value());
      }
    } else if (value.is<JsonArrayConst>()) {
      for (const auto item : value.as<JsonArrayConst>()) {
        ++slots;
        walk(item);
      }
    } else if (value.is<const char*>()) {
      text(value.as<JsonString>());
    }
  }
  std::size_t c6HeapBound() const {
    // C6 ArduinoJson 7.4.3: slot8, pool128, pointer4, StringNode data offset8;
    // ResourceManager default pool list grows 4->8->16. Allocator headers are
    // conservatively budgeted at 16 B even when target max_align_t is smaller.
    const auto pools = (slots + 127) / 128;
    std::size_t pool_capacity = 4;
    while (pool_capacity < pools) pool_capacity *= 2;
    const auto pool_list = pools <= 4 ? 0 : pool_capacity * 8 + 16;
    std::size_t owned = 0;
    for (const auto& s : strings) owned += s.size() + 1 + 8 + 16;
    // StringBuilder's temporary starts at 31 chars and may grow. All known
    // names fit its initial node; retain a conservative 128 B temporary.
    return pools * (128 * 8 + 16) + pool_list + owned + 128;
  }
};

// Match the production JsonAllocator accounting, including its aligned header,
// while tracking the high-water mark. Test on the host's larger 16-byte DOM
// slots as well as the independent 8-byte C6 bound above.
class CappedMeter : public Allocator {
 public:
  void* allocate(std::size_t bytes) override {
    if (bytes > cfg::kJsonHeapBytes - used ||
        sizeof(Header) > cfg::kJsonHeapBytes - used - bytes)
      return nullptr;
    auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + bytes));
    if (!header) return nullptr;
    header->bytes = bytes;
    used += sizeof(Header) + bytes;
    peak = std::max(peak, used);
    return header + 1;
  }
  void deallocate(void* pointer) override {
    if (!pointer) return;
    auto* header = static_cast<Header*>(pointer) - 1;
    used -= sizeof(Header) + header->bytes;
    std::free(header);
  }
  void* reallocate(void* pointer, std::size_t bytes) override {
    if (!pointer) return allocate(bytes);
    auto* header = static_cast<Header*>(pointer) - 1;
    const auto old = header->bytes;
    if (bytes > cfg::kJsonHeapBytes - (used - old)) return nullptr;
    auto* next =
        static_cast<Header*>(std::realloc(header, sizeof(Header) + bytes));
    if (!next) return nullptr;
    next->bytes = bytes;
    used = used - old + bytes;
    peak = std::max(peak, used);
    return next + 1;
  }
  std::size_t used = 0;
  std::size_t peak = 0;

 private:
  struct alignas(std::max_align_t) Header {
    std::size_t bytes;
  };
};

void maximum_complete_request_fits_existing_wire_and_c6_heap_budgets() {
  populate(true);
  // false is one byte longer than true. Disabled shared image entries may
  // legally retain automatic=false; their retained flags must also fit.
  for (auto& phase : model.shared)
    for (auto& rule : phase) rule.automatic = false;
  TEST_ASSERT_TRUE(policy::validate(model));
  encode(UINT32_MAX);
  const auto bytes = measureJson(doc);
  Structure shape;
  shape.walk(doc.as<JsonVariantConst>());
  const auto heap_bound = shape.c6HeapBound();
  std::printf(
      "screen-json: worst body=%zu B, slots=%zu, strings=%zu, "
      "longest=%zu, C6 allocator bound=%zu B\n",
      bytes, shape.slots, shape.strings.size(), shape.longest, heap_bound);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(cfg::kApiBodyBytes, bytes);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(cfg::kJsonHeapBytes, heap_bound);
  std::string body;
  serializeJson(doc, body);
  doc.clear();
  TEST_ASSERT_FALSE(deserializeJson(doc, body));
  TEST_ASSERT_TRUE(apply());
  TEST_ASSERT_TRUE(candidate == model);
  CappedMeter meter;
  {
    JsonDocument capped(&meter);
    web::screenPolicyJson(capped.to<JsonObject>(), model, UINT32_MAX);
    TEST_ASSERT_FALSE(capped.overflowed());
    TEST_ASSERT_EQUAL_UINT32(bytes, measureJson(capped));
    const auto serialization_peak = meter.peak;
    capped.clear();
    meter.peak = 0;
    TEST_ASSERT_FALSE(deserializeJson(capped, body));
    TEST_ASSERT_TRUE(web::applyScreenPolicyJson(
        capped.as<JsonObjectConst>(), candidate, generation, &violation));
    TEST_ASSERT_TRUE(candidate == model);
    std::printf("screen-json: capped host serialization=%zu B, parse=%zu B\n",
                serialization_peak, meter.peak);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(cfg::kJsonHeapBytes, meter.peak);
  }
  TEST_ASSERT_EQUAL_UINT32(0, meter.used);
}

void schema_explains_complete_named_rules_and_null_inheritance() {
  doc.clear();
  web::screenPolicySchemaJson(doc.to<JsonObject>(), {3, 5});
  TEST_ASSERT_EQUAL_STRING("enabled", doc["rule_fields"][0].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("automatic",
                           doc["rule_fields"][1].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("visit_s", doc["rule_fields"][2].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("inherit_shared_field",
                           doc["override_null"].as<const char*>());
  TEST_ASSERT_TRUE(doc["complete_put"].as<bool>());
  TEST_ASSERT_TRUE(doc["generation_required"].as<bool>());
  TEST_ASSERT_EQUAL_UINT16(43200, doc["bounds"]["visit_s"][1].as<uint16_t>());
  TEST_ASSERT_EQUAL_UINT8(3, doc["bounds"]["visible_matches"][1].as<uint8_t>());
  TEST_ASSERT_EQUAL_UINT8(2, doc["bounds"]["visible_matches"][0].as<uint8_t>());
  TEST_ASSERT_EQUAL_UINT8(5, doc["bounds"]["table_window"][1].as<uint8_t>());
}
}  // namespace

void setUp() {
  model = {};
  candidate = {};
  doc.clear();
  violation = {};
  generation = 0;
}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(complete_policy_keeps_all_options_and_rules);
  RUN_TEST(
      omitted_override_fields_inherit_and_explicit_false_survives_collapse);
  RUN_TEST(complete_object_and_boot_generation_are_required);
  RUN_TEST(unknown_fields_and_embedded_nuls_cannot_be_silently_accepted);
  RUN_TEST(scalar_types_and_rule_tuple_shapes_are_strict);
  RUN_TEST(duplicate_order_and_expansion_types_are_rejected);
  RUN_TEST(competition_identity_and_each_retained_rule_are_validated);
  RUN_TEST(image_visits_must_be_automatic_and_exactly_divisible);
  RUN_TEST(device_layout_limits_and_appearance_bounds_remain_enforced);
  RUN_TEST(maximum_complete_request_fits_existing_wire_and_c6_heap_budgets);
  RUN_TEST(schema_explains_complete_named_rules_and_null_inheritance);
  return UNITY_END();
}
