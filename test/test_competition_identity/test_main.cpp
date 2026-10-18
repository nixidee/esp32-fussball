// Competition identity behavior required by retained screen overrides. Uses
// production allocation and codecs; this is not an NVS/device acceptance test.
#include <unity.h>

#include <algorithm>
#include <array>
#include <limits>

#include "app_config_record.h"
#include "screen_policy.h"
#include "settings_model.h"

namespace {
settings::Competitions current;
settings::Competitions candidate;
settings::Competitions restored;
std::array<uint8_t, settings::app_record::kMaxBytes> bytes;

settings::Competition entry(uint8_t id = 0) {
  settings::Competition value;
  value.id = id;
  value.primary.competition.assign("bl1");
  value.primary.season.assign("2026");
  return value;
}

void seedTwo() {
  candidate.entries.push(entry());
  candidate.entries.push(entry());
  TEST_ASSERT_TRUE(settings::assignIncarnations(candidate, current));
  TEST_ASSERT_TRUE(settings::validate(candidate));
  current = candidate;
}

void edits_and_reordering_retain_service_owned_identity() {
  seedTwo();
  std::swap(candidate.entries[0], candidate.entries[1]);
  candidate.entries[0].competition_name.assign("Changed label");
  candidate.entries[0].primary.season.assign("2027");
  candidate.entries[0].incarnation = 12345;  // client input is not authority
  candidate.next_incarnation = 1;
  TEST_ASSERT_TRUE(settings::assignIncarnations(candidate, current));
  TEST_ASSERT_EQUAL_UINT32(current.entries[1].incarnation,
                           candidate.entries[0].incarnation);
  TEST_ASSERT_EQUAL_UINT32(current.entries[0].incarnation,
                           candidate.entries[1].incarnation);
  TEST_ASSERT_EQUAL_UINT32(current.next_incarnation,
                           candidate.next_incarnation);
  TEST_ASSERT_TRUE(settings::validate(candidate));
}

void deleting_and_reusing_an_id_cannot_transfer_screen_overrides() {
  seedTwo();
  settings::screen_policy::Policy policy;
  const auto phase =
      settings::screen_policy::index(settings::screen_policy::Phase::kIdle);
  const auto type = screen::sportIndex(screen::Type::kTable);
  policy.per_competition[phase] = 1u << type;
  current.entries[0].display.overrides[phase][type] = {
      120, 0, settings::screen_policy::kVisit};
  const auto old = current.entries[0];
  candidate.clear();
  candidate.entries.push(entry());
  current.next_id = old.id;
  TEST_ASSERT_TRUE(settings::assignIncarnations(candidate, current));
  const auto& replacement = candidate.entries[0];
  TEST_ASSERT_EQUAL_UINT8(old.id, replacement.id);
  TEST_ASSERT_NOT_EQUAL(old.incarnation, replacement.incarnation);
  TEST_ASSERT_EQUAL_UINT16(120,
                           settings::screen_policy::resolve(
                               policy, settings::screen_policy::Phase::kIdle,
                               screen::Type::kTable, &old)
                               .visit_s);
  TEST_ASSERT_EQUAL_UINT16(
      cfg::kScreenVisitDefaultS[screen::index(screen::Type::kTable)],
      settings::screen_policy::resolve(policy,
                                       settings::screen_policy::Phase::kIdle,
                                       screen::Type::kTable, &replacement)
          .visit_s);
  TEST_ASSERT_FALSE(settings::sameProviderData(current, candidate));
}

void exhaustion_refuses_new_identity_without_wrapping_or_changing_current() {
  seedTwo();
  current.next_incarnation = std::numeric_limits<uint32_t>::max();
  candidate.entries.push(entry());
  settings::Violation violation;
  TEST_ASSERT_FALSE(
      settings::assignIncarnations(candidate, current, &violation));
  TEST_ASSERT_EQUAL_STRING("competitions[2].incarnation",
                           violation.path.data());
  TEST_ASSERT_EQUAL(settings::Reason::kRange, violation.reason);
  TEST_ASSERT_EQUAL_UINT32(std::numeric_limits<uint32_t>::max(),
                           current.next_incarnation);
  TEST_ASSERT_EQUAL_UINT8(2, current.entries.count);
  candidate = current;
  TEST_ASSERT_TRUE(settings::assignIncarnations(candidate, current));
  TEST_ASSERT_TRUE(settings::validate(candidate));
}

void aggregate_restores_identity_counter_and_rejects_corrupt_identity() {
  seedTwo();
  settings::AppConfig config;
  config.model = settings::initialValues({});
  config.competitions = current;
  const auto size = settings::app_record::encode(config, bytes);
  settings::AppConfig scratch;
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOk,
      settings::app_record::decode(std::span(bytes).first(size), scratch));
  TEST_ASSERT_TRUE(current == scratch.competitions);
  constexpr auto incarnation_at = settings::app_record::kHeaderBytes +
                                  settings::app_record::kMetadataBytes +
                                  settings::kModelPayloadBytes + 6 + 1;
  std::fill_n(bytes.begin() + incarnation_at, 4, 0);
  settings::app_record::seal(std::span(bytes).first(size));
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kInvalidValue,
      settings::app_record::decode(std::span(bytes).first(size), scratch));
}

void changed_incarnation_invalidates_provider_data_even_with_same_small_id() {
  seedTwo();
  candidate = current;
  candidate.entries[0].incarnation = candidate.next_incarnation++;
  TEST_ASSERT_TRUE(settings::validate(candidate));
  TEST_ASSERT_FALSE(settings::sameProviderData(current, candidate));
  candidate = current;
  candidate.entries[0].competition_name.assign("Display label");
  TEST_ASSERT_TRUE(settings::sameProviderData(current, candidate));
}

void duplicate_or_backward_record_identities_are_rejected() {
  seedTwo();
  candidate.entries[1].incarnation = candidate.entries[0].incarnation;
  TEST_ASSERT_FALSE(settings::validate(candidate));
  candidate = current;
  candidate.next_incarnation = candidate.entries[1].incarnation;
  TEST_ASSERT_FALSE(settings::validate(candidate));
}

void initial_presets_allocate_in_ram_without_external_identity() {
  settings::Presets presets{};
  presets.competition = "bl1";
  presets.season = "2026";
  settings::initialCompetitions(presets, current);
  TEST_ASSERT_EQUAL_UINT8(1, current.entries.count);
  TEST_ASSERT_EQUAL_UINT8(1, current.entries[0].id);
  TEST_ASSERT_EQUAL_UINT32(1, current.entries[0].incarnation);
  TEST_ASSERT_EQUAL_UINT32(2, current.next_incarnation);
  TEST_ASSERT_TRUE(settings::validate(current));
}

// Split-record fallback/high-water repair tests are retired: one AppConfig
// now owns identities and preferences. Actual NVS fault tests prove complete
// old/new publication; this check proves their same-record roundtrip.
void aggregate_keeps_attached_preferences_with_their_reordered_identity() {
  seedTwo();
  candidate.entries[0].display.overrides[3][3] = {
      120, 0, settings::screen_policy::kVisit};
  std::swap(candidate.entries[0], candidate.entries[1]);
  settings::AppConfig config;
  config.model = settings::initialValues({});
  config.competitions = candidate;
  const auto used = settings::app_record::encode(config, bytes);
  settings::AppConfig scratch;
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOk,
      settings::app_record::decode(std::span(bytes).first(used), scratch));
  TEST_ASSERT_TRUE(candidate == scratch.competitions);
  TEST_ASSERT_EQUAL_UINT16(
      120, scratch.competitions.entries[1].display.overrides[3][3].visit_s);
  TEST_ASSERT_EQUAL_UINT16(
      0, scratch.competitions.entries[0].display.overrides[3][3].fields);
}
}  // namespace

void setUp() {
  current.clear();
  candidate.clear();
  restored.clear();
  bytes.fill(0);
}
void tearDown() {}
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(edits_and_reordering_retain_service_owned_identity);
  RUN_TEST(deleting_and_reusing_an_id_cannot_transfer_screen_overrides);
  RUN_TEST(
      exhaustion_refuses_new_identity_without_wrapping_or_changing_current);
  RUN_TEST(aggregate_restores_identity_counter_and_rejects_corrupt_identity);
  RUN_TEST(
      changed_incarnation_invalidates_provider_data_even_with_same_small_id);
  RUN_TEST(duplicate_or_backward_record_identities_are_rejected);
  RUN_TEST(initial_presets_allocate_in_ram_without_external_identity);
  RUN_TEST(aggregate_keeps_attached_preferences_with_their_reordered_identity);
  return UNITY_END();
}
