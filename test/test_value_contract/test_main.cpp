// Regressions for aliasing assignments and accepted club identities that the
// runtime previously could not represent. Exercise production callers/codecs.
#include <unity.h>

#include <cstring>
#include <string_view>

#include "../json_codec_support.h"
#include "app_config_record.h"
#include "football_model.h"
#include "match_store.h"
#include "settings_json.h"
#include "settings_model.h"

namespace {
void ssid_assignment_preserves_self_and_overlapping_raw_bytes() {
  settings::Model model;
  constexpr char bytes[] = {'W', '\0', 'L', 'A', 'N'};
  TEST_ASSERT_TRUE(model.wifi_ssid.assign({bytes, sizeof(bytes)}));
  TEST_ASSERT_TRUE(model.wifi_ssid.assign(model.wifi_ssid.view()));
  TEST_ASSERT_EQUAL_UINT8(sizeof(bytes), model.wifi_ssid.length);
  TEST_ASSERT_EQUAL_MEMORY(bytes, model.wifi_ssid.data.data(), sizeof(bytes));
  TEST_ASSERT_TRUE(model.wifi_ssid.assign(model.wifi_ssid.view().substr(1, 3)));
  TEST_ASSERT_EQUAL_MEMORY(bytes + 1, model.wifi_ssid.data.data(), 3);
  TEST_ASSERT_EQUAL_UINT8(3, model.wifi_ssid.length);
  for (std::size_t i = 3; i < model.wifi_ssid.data.size(); ++i)
    TEST_ASSERT_EQUAL_CHAR(0, model.wifi_ssid.data[i]);
}

void team_refresh_accepts_its_existing_borrowed_labels() {
  football::MatchStore store;
  const auto index = store.team(1, cfg::Provider::kOpenLigaDb, 16,
                                "Bayern M\xc3\xbcnchen", "FCB");
  const auto& existing = store.teamAt(index);
  TEST_ASSERT_EQUAL_UINT8(
      index, store.team(1, cfg::Provider::kOpenLigaDb, 16, existing.name.data,
                        existing.short_name.data));
  TEST_ASSERT_EQUAL_STRING("Bayern M\xc3\xbcnchen", existing.name.data);
  TEST_ASSERT_EQUAL_STRING("FCB", existing.short_name.data);

  football::Text<8> label;
  label.assign("M\xc3\xbcnchen");
  label.assign(std::string_view(label.data + 1, 7));
  TEST_ASSERT_EQUAL_STRING("\xc3\xbcnchen", label.data);
  football::Text<3> clipped;
  clipped.assign("ab\xc3\xa4");
  TEST_ASSERT_EQUAL_STRING("ab", clipped.data);
}

void formatted_text_never_splits_a_multibyte_tail() {
  char three[4], five[6], tiny[1];
  values::formatText(three, sizeof(three), "%s", "abä");
  TEST_ASSERT_EQUAL_STRING("ab", three);
  values::formatText(five, sizeof(five), "%s · %s", "ä", "Ö");
  TEST_ASSERT_EQUAL_STRING("ä ·", five);
  values::formatText(tiny, sizeof(tiny), "%s", "ä");
  TEST_ASSERT_EQUAL_STRING("", tiny);
  TEST_ASSERT_TRUE(values::isUtf8Text(five));
}

settings::Competitions configured(uint32_t team, cfg::Provider provider) {
  settings::Competitions out;
  settings::initialCompetitions({.competition = "bl1", .team = "16"}, out);
  out.entries[0].primary.provider = provider;
  out.entries[0].club.team = team;
  return out;
}

void configured_club_ids_match_the_runtime_positive_u32_domain() {
  for (const auto provider :
       {cfg::Provider::kOpenLigaDb, cfg::Provider::kApiFootball,
        cfg::Provider::kEspn, cfg::Provider::kFootballData})
    for (uint32_t team : {0u, 1u, UINT32_MAX, 16u}) {
      const auto entries = configured(team, provider);
      TEST_ASSERT_TRUE(settings::validate(entries));
      TEST_ASSERT_EQUAL_UINT32(team, entries.entries[0].club.team);
    }
  // Competition identifiers remain provider text, including known shortcuts.
  auto entries = configured(16, cfg::Provider::kOpenLigaDb);
  entries.entries[0].primary.competition.assign("th\xc3\xbcli-26");
  TEST_ASSERT_TRUE(settings::validate(entries));
}

void competition_json_rejects_unrepresentable_club_ids_at_the_input_path() {
  for (const auto* token : {"\"abc\"", "4294967296", "-1", "1.0", "true"}) {
    const std::string body =
        std::string(
            "{\"competitions\":[{\"primary\":{\"competition\":\"bl1\"},"
            "\"club\":{\"team\":") +
        token + "}}]}";
    struct Source {
      std::string_view text;
      std::size_t at = 0;
      json::Input input() {
        return {
            this, [](void* raw) {
              auto& source = *static_cast<Source*>(raw);
              return source.at < source.text.size()
                         ? static_cast<unsigned char>(source.text[source.at++])
                         : -1;
            }};
      }
    } source{body};
    settings::Competitions candidate;
    settings::Violation why;
    TEST_ASSERT_EQUAL(
        web::SettingsDecode::kViolation,
        web::applyCompetitionsJson(source.input(), candidate, &why));
    TEST_ASSERT_EQUAL_STRING("competitions[0].club.team", why.path.data());
    TEST_ASSERT_EQUAL(!strcmp(token, "4294967296") || !strcmp(token, "-1")
                          ? settings::Reason::kRange
                          : settings::Reason::kType,
                      why.reason);
  }
}

void preset_cannot_create_a_club_that_the_runtime_silently_treats_as_absent() {
  for (const auto* team : {"abc", "4294967296", "0"}) {
    settings::Competitions entries;
    settings::FieldMask rejected = 0;
    settings::initialCompetitions({.competition = "bl1", .team = team}, entries,
                                  &rejected);
    TEST_ASSERT_TRUE(entries.entries.empty());
    TEST_ASSERT_EQUAL_UINT32(settings::fieldBit(settings::Field::kApplication),
                             rejected);
  }
}

void aggregate_roundtrips_the_exact_numeric_club_domain() {
  const auto current = configured(16, cfg::Provider::kOpenLigaDb);
  for (uint32_t team : {0u, 1u, UINT32_MAX}) {
    const auto value = configured(team, cfg::Provider::kOpenLigaDb);
    settings::AppConfig config;
    config.model = settings::initialValues({});
    config.competitions = value;
    std::array<uint8_t, settings::app_record::kMaxBytes> record{};
    const auto length = settings::app_record::encode(config, record);
    settings::AppConfig restored_config;
    restored_config.competitions = current;
    TEST_ASSERT_EQUAL(settings::DecodeResult::kOk,
                      settings::app_record::decode(
                          std::span(record).first(length), restored_config));
    const auto& restored = restored_config.competitions;
    TEST_ASSERT_TRUE(restored == value);
  }
}
void provider_dates_have_no_hidden_application_year_cutoff() {
  TEST_ASSERT_EQUAL_INT64(-86400, football::parseUtc("1969-12-31T00:00:00Z"));
  TEST_ASSERT_EQUAL_INT64(0, football::parseUtc("1970-01-01T00:00:00Z"));
  TEST_ASSERT_EQUAL_INT64(951782400,
                          football::parseUtc("2000-02-29T00:00:00Z"));
  TEST_ASSERT_EQUAL_INT64(4133980800LL,
                          football::parseUtc("2101-01-01T00:00:00Z"));
  TEST_ASSERT_EQUAL_INT64(253402300799LL,
                          football::parseUtc("9999-12-31T23:59:59Z"));
  TEST_ASSERT_EQUAL_INT64(0, football::parseUtc("2100-02-29T00:00:00Z"));
  TEST_ASSERT_EQUAL_INT64(0, football::parseUtc("2026-04-31T00:00:00Z"));
}

}  // namespace

void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(ssid_assignment_preserves_self_and_overlapping_raw_bytes);
  RUN_TEST(team_refresh_accepts_its_existing_borrowed_labels);
  RUN_TEST(formatted_text_never_splits_a_multibyte_tail);
  RUN_TEST(configured_club_ids_match_the_runtime_positive_u32_domain);
  RUN_TEST(competition_json_rejects_unrepresentable_club_ids_at_the_input_path);
  RUN_TEST(
      preset_cannot_create_a_club_that_the_runtime_silently_treats_as_absent);
  RUN_TEST(aggregate_roundtrips_the_exact_numeric_club_domain);
  RUN_TEST(provider_dates_have_no_hidden_application_year_cutoff);
  return UNITY_END();
}
