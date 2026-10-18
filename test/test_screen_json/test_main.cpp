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

#include "../json_codec_support.h"
#include "screen_json.h"

namespace {
namespace policy = settings::screen_policy;
using namespace ArduinoJson;

policy::Policy model;
policy::Policy candidate;
JsonDocument doc;
settings::Violation violation;
settings::Competition competition;
settings::Competition decoded_competition;

void populate(bool maximum) {
  model = {};
  for (unsigned p = 0; p < model.shared.size(); ++p) {
    model.per_competition[p] = 15;
    for (auto& rule : model.shared[p])
      rule = {static_cast<uint16_t>(maximum ? 43200 : 15), !maximum, true};
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

void encode() {
  doc.clear();
  TEST_ASSERT_TRUE(codec_test::write(doc, [&](json::Writer& out) {
    return web::screenPolicyJson(out, model);
  }));
  TEST_ASSERT_FALSE(doc.overflowed());
}

bool apply(const policy::Limits& limits = {}) {
  return codec_test::read(doc.as<JsonObjectConst>(), [&](json::Input input) {
    return web::applyScreenPolicyJson(input, candidate, &violation, limits) ==
           web::SettingsDecode::kOk;
  });
}
bool applyPreference() {
  return codec_test::read(doc.as<JsonObjectConst>(), [&](json::Input input) {
    json::Reader reader(input);
    json::Token token;
    if (!reader.next(token)) return false;
    return web::applyCompetitionPreferencesObject(
               reader, token, decoded_competition.display, &violation) ==
               web::SettingsDecode::kOk &&
           reader.finish();
  });
}
void encodePreference() {
  TEST_ASSERT_TRUE(codec_test::write(doc, [&](json::Writer& out) {
    return web::competitionPreferencesJson(out, competition.display);
  }));
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
    encode();
    std::string body;
    serializeJson(doc, body);
    doc.clear();
    TEST_ASSERT_FALSE(deserializeJson(doc, body));
    TEST_ASSERT_TRUE(apply());
    TEST_ASSERT_TRUE(candidate == model);
  }
}

void omitted_override_fields_inherit_and_explicit_false_survives_collapse() {
  model.shared[3][2] = {90, true, true};
  competition.display.overrides[3][2] = {15, 0, policy::kEnabled};
  encodePreference();
  TEST_ASSERT_TRUE(doc["idle"]["table"][1].isNull());
  TEST_ASSERT_FALSE(doc["idle"]["table"][0].as<bool>());
  TEST_ASSERT_TRUE(applyPreference());
  TEST_ASSERT_TRUE(decoded_competition.display == competition.display);
  TEST_ASSERT_TRUE(policy::resolve(model, policy::Phase::kIdle,
                                   screen::Type::kTable, &decoded_competition)
                       .enabled);
  model.per_competition[3] = 1u << 2;
  const auto effective = policy::resolve(
      model, policy::Phase::kIdle, screen::Type::kTable, &decoded_competition);
  TEST_ASSERT_FALSE(effective.enabled);
  TEST_ASSERT_TRUE(effective.automatic);
  TEST_ASSERT_EQUAL_UINT16(90, effective.visit_s);
}

void complete_object_is_required_and_revision_is_owned_by_envelope() {
  encode();
  TEST_ASSERT_TRUE(apply());
  doc["generation"] = 1;
  rejects("generation", settings::Reason::kUnknownKey);
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
  doc["manual_return_s"] = uint64_t{UINT32_MAX} + 1;
  rejects("manual_return_s", settings::Reason::kRange);
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
  auto expansion = doc["phases"]["idle"]["per_competition"].to<JsonArray>();
  expansion.add("table");
  expansion.add("table");
  rejects("phases.idle.per_competition[1]", settings::Reason::kDuplicate);
  encode();
  doc["phases"]["idle"]["per_competition"].to<JsonArray>().add("images");
  rejects("phases.idle.per_competition[0]", settings::Reason::kInvalid);
}

void each_competition_attached_rule_is_validated() {
  encodePreference();
  doc["matchday"]["conference"][2] = 43201;
  TEST_ASSERT_FALSE(applyPreference());
  TEST_ASSERT_EQUAL_STRING("matchday.conference[2]", violation.path.data());
  TEST_ASSERT_EQUAL(settings::Reason::kRange, violation.reason);
  encodePreference();
  doc["idle"].remove("table");
  TEST_ASSERT_FALSE(applyPreference());
  TEST_ASSERT_EQUAL_STRING("idle.table", violation.path.data());
  TEST_ASSERT_EQUAL(settings::Reason::kRequired, violation.reason);
  encodePreference();
  doc["idle"]["table"][1] = 0;
  TEST_ASSERT_FALSE(applyPreference());
  TEST_ASSERT_EQUAL_STRING("idle.table[1]", violation.path.data());
  TEST_ASSERT_EQUAL(settings::Reason::kType, violation.reason);
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

void maximum_complete_request_fits_existing_wire_with_no_production_dom() {
  populate(true);
  for (auto& phase : model.shared)
    for (auto& rule : phase) rule.automatic = false;
  TEST_ASSERT_TRUE(policy::validate(model));
  encode();
  const auto wire_bytes = measureJson(doc);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(cfg::kApiBodyBytes, wire_bytes);
  TEST_ASSERT_TRUE(apply());
  TEST_ASSERT_TRUE(candidate == model);
  std::printf(
      "screen-json: global body=%zu B, streaming Reader=%zu B, Token=%zu B\n",
      wire_bytes, sizeof(json::Reader), sizeof(json::Token));
}

void schema_explains_complete_named_rules_and_null_inheritance() {
  doc.clear();
  TEST_ASSERT_TRUE(codec_test::write(doc, [](json::Writer& out) {
    return web::screenPolicySchemaJson(out, {3, 5});
  }));
  TEST_ASSERT_EQUAL_STRING("enabled", doc["rule_fields"][0].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("automatic",
                           doc["rule_fields"][1].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("visit_s", doc["rule_fields"][2].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("inherit_shared_field",
                           doc["override_null"].as<const char*>());
  TEST_ASSERT_TRUE(doc["complete_put"].as<bool>());
  TEST_ASSERT_TRUE(doc["generation_required"].isNull());
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
  competition = {};
  decoded_competition = {};
}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(complete_policy_keeps_all_options_and_rules);
  RUN_TEST(
      omitted_override_fields_inherit_and_explicit_false_survives_collapse);
  RUN_TEST(complete_object_is_required_and_revision_is_owned_by_envelope);
  RUN_TEST(unknown_fields_and_embedded_nuls_cannot_be_silently_accepted);
  RUN_TEST(scalar_types_and_rule_tuple_shapes_are_strict);
  RUN_TEST(duplicate_order_and_expansion_types_are_rejected);
  RUN_TEST(each_competition_attached_rule_is_validated);
  RUN_TEST(image_visits_must_be_automatic_and_exactly_divisible);
  RUN_TEST(device_layout_limits_and_appearance_bounds_remain_enforced);
  RUN_TEST(maximum_complete_request_fits_existing_wire_with_no_production_dom);
  RUN_TEST(schema_explains_complete_named_rules_and_null_inheritance);
  return UNITY_END();
}
