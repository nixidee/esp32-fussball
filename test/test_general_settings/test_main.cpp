// Current wire/API contract after the approved fresh development-format
// cutover.
#include <unity.h>

#include <array>
#include <cstring>

#include "settings_json.h"
#include "settings_record.h"

namespace {
constexpr const char* kRemoved[] = {
    "backgrounds",      "pin_own_match",   "whole_matchday", "visible_matches",
    "table_window",     "manual_return_s", "slideshow_s",    "table_page_s",
    "window_before",    "window_after",    "idle_screen",    "matchday_screen",
    "own_match_screen", "conference_mode", "matchday_mode",  "slides",
    "screens"};
settings::Model initial() { return settings::initialValues({}); }
void current_wire_is_fixed_size_and_keeps_general_settings_only() {
  auto model = initial();
  model.language = 1;
  model.brightness = 77;
  model.scroll_repeat_ms = 450;
  model.providers[0].limit = {300, 10, 60};
  std::array<uint8_t, settings::kMaxRecordBytes> record{};
  TEST_ASSERT_EQUAL_size_t(701, record.size());
  TEST_ASSERT_EQUAL_size_t(701, settings::encode(model, record));
  TEST_ASSERT_EQUAL_UINT8(11, record[0]);
  // Network/time + three fixed-capacity secrets end at byte562. Six general
  // flags precede language/brightness/night brightness/rotation.
  TEST_ASSERT_EQUAL_UINT8(1, record[568]);
  TEST_ASSERT_EQUAL_UINT8(77, record[569]);
  TEST_ASSERT_EQUAL_UINT8(450 & 255, record[574]);
  TEST_ASSERT_EQUAL_UINT8(450 >> 8, record[575]);
  auto restored = initial();
  TEST_ASSERT_EQUAL(settings::DecodeResult::kOk,
                    settings::decode(record, restored));
  TEST_ASSERT_TRUE(restored == model);
  const auto before = restored;
  settings::seal(record, 10);
  TEST_ASSERT_EQUAL(settings::DecodeResult::kOutdatedVersion,
                    settings::decode(record, restored));
  TEST_ASSERT_TRUE(restored == before);
}
void competition_record_keeps_identity_and_drops_only_old_screen_masks() {
  settings::Competitions entries;
  settings::Competition entry;
  entry.id = 7;
  entry.incarnation = 19;
  entry.primary.competition.assign("bl1");
  entries.entries.push(entry);
  entries.next_id = 8;
  entries.next_incarnation = 20;
  std::array<uint8_t, settings::kMaxCompetitionRecordBytes> record{};
  TEST_ASSERT_EQUAL_size_t(1262, record.size());
  const auto size = settings::encodeCompetitions(entries, record);
  TEST_ASSERT_EQUAL_size_t(170, size);
  TEST_ASSERT_EQUAL_UINT8(4, record[0]);
  TEST_ASSERT_EQUAL_UINT8(8, record[8]);
  TEST_ASSERT_EQUAL_UINT8(20, record[9]);
  TEST_ASSERT_EQUAL_UINT8(1, record[13]);
  TEST_ASSERT_EQUAL_UINT8(7, record[14]);
  TEST_ASSERT_EQUAL_UINT8(19, record[15]);
  settings::Competitions restored;
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOk,
      settings::decodeCompetitions({record.data(), size}, restored));
  TEST_ASSERT_TRUE(entries == restored);
  settings::seal({record.data(), size}, 3);
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOutdatedVersion,
      settings::decodeCompetitions({record.data(), size}, restored));
}
void removed_root_keys_are_rejected_and_are_absent_from_general_schema() {
  ArduinoJson::JsonDocument doc;
  auto model = initial();
  web::settingsJson(doc.to<ArduinoJson::JsonObject>(), model);
  for (const auto* key : kRemoved) TEST_ASSERT_TRUE(doc[key].isNull());
  for (const auto* key : kRemoved) {
    doc.clear();
    doc[key] = 1;
    settings::Violation why;
    const auto before = model;
    TEST_ASSERT_FALSE(
        web::applyJson(doc.as<ArduinoJson::JsonObjectConst>(), model, &why));
    TEST_ASSERT_EQUAL(settings::Reason::kUnknownKey, why.reason);
    TEST_ASSERT_EQUAL_STRING(key, why.path.data());
    TEST_ASSERT_TRUE(before == model);
  }
  doc.clear();
  web::schemaJson(doc.to<ArduinoJson::JsonObject>(), {}, true);
  TEST_ASSERT_EQUAL_UINT16(11, doc["version"].as<uint16_t>());
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
  web::competitionsJson(doc.to<ArduinoJson::JsonObject>(), entries);
  TEST_ASSERT_TRUE(doc["competitions"][0]["screens"].isNull());
  TEST_ASSERT_EQUAL_UINT8(entries.entries[0].incarnation,
                          doc["competitions"][0]["incarnation"].as<uint8_t>());
  doc["competitions"][0]["screens"]["table"] = true;
  settings::Violation why;
  TEST_ASSERT_FALSE(web::applyCompetitionsJson(
      doc.as<ArduinoJson::JsonObjectConst>(), entries, &why));
  TEST_ASSERT_EQUAL(settings::Reason::kUnknownKey, why.reason);
  TEST_ASSERT_EQUAL_STRING("competitions[0].screens", why.path.data());
}
void overview_screenshot_tail_preserves_old_records_and_rejects_invalid_flags() {
  auto model = initial();
  model.hostname.assign("retained-name");
  model.language = 1;
  model.overview_screenshot = true;
  std::array<uint8_t, settings::kMaxRecordBytes> record{};
  settings::encode(model, record);
  TEST_ASSERT_EQUAL_UINT8(1, record.back());
  auto restored = initial();
  TEST_ASSERT_EQUAL(settings::DecodeResult::kOk,
                    settings::decode(record, restored));
  TEST_ASSERT_TRUE(restored == model);
  // Exact pre-extension format-11 size: every existing setting stays, the
  // absent opt-in flag defaults off. No format reset or offset migration.
  auto old = std::span<uint8_t>(record).first(700);
  settings::seal(old);
  restored = initial();
  TEST_ASSERT_EQUAL(settings::DecodeResult::kOk,
                    settings::decode(old, restored));
  model.overview_screenshot = false;
  TEST_ASSERT_TRUE(restored == model);
  record.back() = 2;
  settings::seal(record);
  const auto before = restored;
  TEST_ASSERT_EQUAL(settings::DecodeResult::kInvalidValue,
                    settings::decode(record, restored));
  TEST_ASSERT_TRUE(restored == before);
}
void overview_screenshot_json_is_opt_in_and_does_not_invalidate_provider_data() {
  auto model = initial();
  const auto before = model;
  TEST_ASSERT_FALSE(model.overview_screenshot);
  ArduinoJson::JsonDocument doc;
  doc["overview_screenshot"] = true;
  TEST_ASSERT_TRUE(web::applyJson(doc.as<ArduinoJson::JsonObjectConst>(), model));
  TEST_ASSERT_TRUE(model.overview_screenshot);
  TEST_ASSERT_TRUE(settings::sameProviderData(before, model));
  doc.clear();
  web::settingsJson(doc.to<ArduinoJson::JsonObject>(), model);
  TEST_ASSERT_TRUE(doc["overview_screenshot"].as<bool>());
  doc.clear();
  doc["overview_screenshot"] = "true";
  settings::Violation why;
  TEST_ASSERT_FALSE(web::applyJson(doc.as<ArduinoJson::JsonObjectConst>(), model,
                                  &why));
  TEST_ASSERT_EQUAL(settings::Reason::kType, why.reason);
  TEST_ASSERT_EQUAL_STRING("overview_screenshot", why.path.data());
  TEST_ASSERT_TRUE(model.overview_screenshot);
}
}  // namespace
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(current_wire_is_fixed_size_and_keeps_general_settings_only);
  RUN_TEST(competition_record_keeps_identity_and_drops_only_old_screen_masks);
  RUN_TEST(removed_root_keys_are_rejected_and_are_absent_from_general_schema);
  RUN_TEST(
      competition_api_rejects_legacy_screen_mask_without_losing_other_fields);
  RUN_TEST(overview_screenshot_tail_preserves_old_records_and_rejects_invalid_flags);
  RUN_TEST(overview_screenshot_json_is_opt_in_and_does_not_invalidate_provider_data);
  return UNITY_END();
}
