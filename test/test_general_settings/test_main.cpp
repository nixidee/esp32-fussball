// Current wire/API contract after the approved fresh development-format
// cutover.
#include <unity.h>

#include <array>
#include <cstring>

#include "../json_codec_support.h"
#include "app_config_record.h"
#include "settings_json.h"

namespace {
bool apply(ArduinoJson::JsonObjectConst doc, settings::Model& model,
           settings::Violation* violation = nullptr) {
  return codec_test::read(doc, [&](json::Input input) {
    return web::applyJson(input, model, violation) == web::SettingsDecode::kOk;
  });
}
bool applyCompetitions(ArduinoJson::JsonObjectConst doc,
                       settings::Competitions& model,
                       settings::Violation* violation) {
  return codec_test::read(doc, [&](json::Input input) {
    return web::applyCompetitionsJson(input, model, violation) ==
           web::SettingsDecode::kOk;
  });
}
constexpr const char* kRemoved[] = {
    "backgrounds",      "pin_own_match",   "whole_matchday", "visible_matches",
    "table_window",     "manual_return_s", "slideshow_s",    "table_page_s",
    "window_before",    "window_after",    "idle_screen",    "matchday_screen",
    "own_match_screen", "conference_mode", "matchday_mode",  "slides",
    "screens"};
settings::Model initial() { return settings::initialValues({}); }
void current_aggregate_keeps_general_settings_and_one_current_header() {
  settings::AppConfig config;
  auto& model = config.model;
  model = initial();
  model.language = 1;
  model.brightness = 77;
  model.scroll_repeat_ms = 450;
  model.providers[0].limit = {300, 10, 60};
  std::array<uint8_t, settings::app_record::kMaxBytes> record{};
  const auto used = settings::app_record::encode(config, record);
  TEST_ASSERT_EQUAL_size_t(946, used);
  TEST_ASSERT_EQUAL_UINT8(cfg::kAppConfigFormatVersion, record[0]);
  constexpr auto payload_at =
      settings::app_record::kHeaderBytes + settings::app_record::kMetadataBytes;
  TEST_ASSERT_EQUAL_UINT8(1, record[payload_at + 560]);
  TEST_ASSERT_EQUAL_UINT8(77, record[payload_at + 561]);
  TEST_ASSERT_EQUAL_UINT8(450 & 255, record[payload_at + 566]);
  TEST_ASSERT_EQUAL_UINT8(450 >> 8, record[payload_at + 567]);
  settings::AppConfig restored;
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOk,
      settings::app_record::decode(std::span(record).first(used), restored));
  TEST_ASSERT_TRUE(restored.model == model);
  settings::app_record::seal(std::span(record).first(used), 11);
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kUnknownVersion,
      settings::app_record::decode(std::span(record).first(used), restored));
}
void aggregate_keeps_identity_and_attached_display_preferences() {
  settings::AppConfig config;
  config.model = initial();
  auto& entries = config.competitions;
  settings::Competition entry;
  entry.id = 7;
  entry.incarnation = 19;
  entry.club.team = UINT32_MAX;
  entry.club.group = 3;
  entry.club.own_special = false;
  entry.display.overrides[3][2] = {234, 0, 7};
  entry.primary.competition.assign("bl1");
  entries.entries.push(entry);
  entries.next_id = 8;
  entries.next_incarnation = 20;
  std::array<uint8_t, settings::app_record::kMaxBytes> record{};
  const auto size = settings::app_record::encode(config, record);
  TEST_ASSERT_EQUAL_size_t(1155, size);
  constexpr auto at = settings::app_record::kHeaderBytes +
                      settings::app_record::kMetadataBytes +
                      settings::kModelPayloadBytes;
  TEST_ASSERT_EQUAL_UINT8(8, record[at]);
  TEST_ASSERT_EQUAL_UINT8(20, record[at + 1]);
  TEST_ASSERT_EQUAL_UINT8(1, record[at + 5]);
  TEST_ASSERT_EQUAL_UINT8(7, record[at + 6]);
  TEST_ASSERT_EQUAL_UINT8(19, record[at + 7]);
  settings::AppConfig restored;
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOk,
      settings::app_record::decode(std::span(record).first(size), restored));
  TEST_ASSERT_TRUE(entries == restored.competitions);
}
void removed_root_keys_are_rejected_and_are_absent_from_general_schema() {
  ArduinoJson::JsonDocument doc;
  auto model = initial();
  TEST_ASSERT_TRUE(codec_test::write(
      doc, [&](json::Writer& out) { return web::settingsJson(out, model); }));
  for (const auto* key : kRemoved) TEST_ASSERT_TRUE(doc[key].isNull());
  for (const auto* key : kRemoved) {
    doc.clear();
    doc[key] = 1;
    settings::Violation why;
    const auto before = model;
    TEST_ASSERT_FALSE(
        apply(doc.as<ArduinoJson::JsonObjectConst>(), model, &why));
    TEST_ASSERT_EQUAL(settings::Reason::kUnknownKey, why.reason);
    TEST_ASSERT_EQUAL_STRING(key, why.path.data());
    TEST_ASSERT_TRUE(before == model);
  }
  doc.clear();
  TEST_ASSERT_TRUE(codec_test::write(
      doc, [](json::Writer& out) { return web::schemaJson(out, {}, true); }));
  TEST_ASSERT_EQUAL_UINT16(cfg::kAppConfigFormatVersion,
                           doc["version"].as<uint16_t>());
  for (const auto* key : kRemoved) {
    TEST_ASSERT_TRUE(doc["defaults"][key].isNull());
    TEST_ASSERT_TRUE(doc["bounds"][key].isNull());
  }
  TEST_ASSERT_TRUE(doc["competition_screens"].isNull());
  TEST_ASSERT_TRUE(doc["screen_order"].isNull());
  TEST_ASSERT_TRUE(doc["limits"]["screens"].isNull());
  TEST_ASSERT_EQUAL_UINT8(cfg::kImageLibraryMax,
                          doc["limits"]["images"].as<uint8_t>());
}
void competition_api_rejects_legacy_screen_mask_without_losing_other_fields() {
  settings::Competitions entries;
  settings::initialCompetitions({.competition = "bl1", .team = "40"}, entries);
  ArduinoJson::JsonDocument doc;
  TEST_ASSERT_TRUE(codec_test::write(doc, [&](json::Writer& out) {
    return web::competitionsJson(out, entries);
  }));
  TEST_ASSERT_TRUE(doc["competitions"][0]["screens"].isNull());
  TEST_ASSERT_EQUAL_UINT8(entries.entries[0].incarnation,
                          doc["competitions"][0]["incarnation"].as<uint8_t>());
  doc["competitions"][0]["screens"]["table"] = true;
  settings::Violation why;
  TEST_ASSERT_FALSE(
      applyCompetitions(doc.as<ArduinoJson::JsonObjectConst>(), entries, &why));
  TEST_ASSERT_EQUAL(settings::Reason::kUnknownKey, why.reason);
  TEST_ASSERT_EQUAL_STRING("competitions[0].screens", why.path.data());
}
void overview_screenshot_is_required_in_current_payload_and_validated() {
  auto model = initial();
  model.hostname.assign("retained-name");
  model.language = 1;
  model.overview_screenshot = true;
  std::array<uint8_t, settings::kModelPayloadBytes> bytes{};
  TEST_ASSERT_EQUAL_size_t(bytes.size(),
                           settings::encodeModelPayload(model, bytes));
  TEST_ASSERT_EQUAL_UINT8(1, bytes.back());
  auto scratch = initial();
  TEST_ASSERT_EQUAL(settings::DecodeResult::kOk,
                    settings::decodeModelPayload(bytes, scratch));
  TEST_ASSERT_TRUE(scratch == model);
  // Optional-tail compatibility was explicitly retired before alpha.
  TEST_ASSERT_EQUAL(settings::DecodeResult::kLengthMismatch,
                    settings::decodeModelPayload(
                        std::span(bytes).first(bytes.size() - 1), scratch));
  bytes.back() = 2;
  TEST_ASSERT_EQUAL(settings::DecodeResult::kInvalidValue,
                    settings::decodeModelPayload(bytes, scratch));
}
void overview_screenshot_json_is_opt_in_and_does_not_invalidate_provider_data() {
  auto model = initial();
  const auto before = model;
  TEST_ASSERT_FALSE(model.overview_screenshot);
  ArduinoJson::JsonDocument doc;
  doc["overview_screenshot"] = true;
  TEST_ASSERT_TRUE(apply(doc.as<ArduinoJson::JsonObjectConst>(), model));
  TEST_ASSERT_TRUE(model.overview_screenshot);
  TEST_ASSERT_TRUE(settings::sameProviderData(before, model));
  doc.clear();
  TEST_ASSERT_TRUE(codec_test::write(
      doc, [&](json::Writer& out) { return web::settingsJson(out, model); }));
  TEST_ASSERT_TRUE(doc["overview_screenshot"].as<bool>());
  doc.clear();
  doc["overview_screenshot"] = "true";
  settings::Violation why;
  TEST_ASSERT_FALSE(apply(doc.as<ArduinoJson::JsonObjectConst>(), model, &why));
  TEST_ASSERT_EQUAL(settings::Reason::kType, why.reason);
  TEST_ASSERT_EQUAL_STRING("overview_screenshot", why.path.data());
  TEST_ASSERT_TRUE(model.overview_screenshot);
}
}  // namespace
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(current_aggregate_keeps_general_settings_and_one_current_header);
  RUN_TEST(aggregate_keeps_identity_and_attached_display_preferences);
  RUN_TEST(removed_root_keys_are_rejected_and_are_absent_from_general_schema);
  RUN_TEST(
      competition_api_rejects_legacy_screen_mask_without_losing_other_fields);
  RUN_TEST(overview_screenshot_is_required_in_current_payload_and_validated);
  RUN_TEST(
      overview_screenshot_json_is_opt_in_and_does_not_invalidate_provider_data);
  return UNITY_END();
}
