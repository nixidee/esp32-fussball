// Competition identity behavior required by retained screen overrides. Uses
// production allocation and codecs; this is not an NVS/device acceptance test.
#include <unity.h>

#include <algorithm>
#include <array>
#include <limits>

#include "screen_policy.h"
#include "screen_record.h"
#include "settings_model.h"
#include "settings_record.h"

namespace {
settings::Competitions current;
settings::Competitions candidate;
settings::Competitions restored;
std::array<uint8_t, settings::kMaxCompetitionRecordBytes> bytes;

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
  const auto old = current.entries[0];
  policy.competitions[0] = {old.incarnation, old.id, true};
  const auto phase =
      settings::screen_policy::index(settings::screen_policy::Phase::kIdle);
  const auto type = screen::sportIndex(screen::Type::kTable);
  policy.expanded[phase] = 1u << type;
  policy.overrides[phase][type][0] = {120, 0, settings::screen_policy::kVisit};
  candidate.clear();
  candidate.entries.push(entry());
  current.next_id = old.id;  // force the allocator's wrap/reuse case
  TEST_ASSERT_TRUE(settings::assignIncarnations(candidate, current));
  const auto replacement = candidate.entries[0];
  TEST_ASSERT_EQUAL_UINT8(old.id, replacement.id);
  TEST_ASSERT_NOT_EQUAL(old.incarnation, replacement.incarnation);
  TEST_ASSERT_EQUAL_UINT16(120,
                           settings::screen_policy::resolve(
                               policy, settings::screen_policy::Phase::kIdle,
                               screen::Type::kTable, old.id, old.incarnation)
                               .visit_s);
  TEST_ASSERT_EQUAL_UINT16(
      cfg::kScreenVisitDefaultS[screen::index(screen::Type::kTable)],
      settings::screen_policy::resolve(
          policy, settings::screen_policy::Phase::kIdle, screen::Type::kTable,
          replacement.id, replacement.incarnation)
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

void record_restores_identity_counter_and_rejects_old_or_corrupt_identity() {
  seedTwo();
  const auto size = settings::encodeCompetitions(current, bytes);
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOk,
      settings::decodeCompetitions({bytes.data(), size}, restored));
  TEST_ASSERT_TRUE(current == restored);
  // A correctly sealed old format is discarded; successful decoding never
  // turns a zero incarnation into an allocation for a different identity.
  settings::seal({bytes.data(), size}, 2);
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOutdatedVersion,
      settings::decodeCompetitions({bytes.data(), size}, restored));
  TEST_ASSERT_TRUE(current == restored);
  const auto size2 = settings::encodeCompetitions(current, bytes);
  // Header + next_id + next_incarnation + count + first id => incarnation.
  constexpr auto at = settings::kHeaderBytes + 1 + 4 + 1 + 1;
  std::fill_n(bytes.begin() + at, 4, 0);
  settings::seal({bytes.data(), size2}, cfg::kCompetitionsFormatVersion);
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kInvalidValue,
      settings::decodeCompetitions({bytes.data(), size2}, restored));
  TEST_ASSERT_TRUE(current == restored);
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

void retained_policy_cannot_resurrect_after_fallback_save_and_second_boot() {
  namespace policy = settings::screen_policy;
  policy::Policy old_policy;
  old_policy.competitions[0] = {1, 1, true};
  const auto phase = policy::index(policy::Phase::kIdle);
  const auto type = screen::sportIndex(screen::Type::kTable);
  old_policy.expanded[phase] = 1u << type;
  old_policy.overrides[phase][type][0] = {120, 0, policy::kVisit};
  std::array<uint8_t, settings::screen_record::kRecordBytes> old_bytes;
  TEST_ASSERT_EQUAL_UINT32(
      old_bytes.size(), settings::screen_record::encode(old_policy, old_bytes));

  // First reboot: competition blob is missing/damaged, but the old policy
  // blob remains intact. Presets initially reuse its small id/revision.
  settings::Presets presets{};
  presets.competition = "bl1";
  settings::initialCompetitions(presets, current);
  policy::Policy loaded;
  TEST_ASSERT_EQUAL(settings::screen_record::DecodeResult::kOk,
                    settings::screen_record::decode(old_bytes, loaded));
  uint32_t highwater = 0;
  for (const auto& binding : loaded.competitions)
    highwater = std::max(highwater, binding.incarnation);
  TEST_ASSERT_TRUE(settings::reserveIncarnationFloor(current, highwater, true));
  TEST_ASSERT_GREATER_THAN_UINT32(1, current.entries[0].incarnation);
  candidate = current;
  TEST_ASSERT_TRUE(settings::assignIncarnations(candidate, current));
  const auto size = settings::encodeCompetitions(candidate, bytes);

  // The owner saves only competitions. On the second reboot, load the NEW
  // competition blob alongside the SAME OLD, unmodified policy blob.
  TEST_ASSERT_EQUAL(
      settings::DecodeResult::kOk,
      settings::decodeCompetitions({bytes.data(), size}, restored));
  TEST_ASSERT_EQUAL(settings::screen_record::DecodeResult::kOk,
                    settings::screen_record::decode(old_bytes, loaded));
  TEST_ASSERT_TRUE(
      settings::reserveIncarnationFloor(restored, highwater, false));
  TEST_ASSERT_EQUAL_UINT8(1, restored.entries[0].id);
  TEST_ASSERT_EQUAL_UINT16(
      60,
      policy::resolve(loaded, policy::Phase::kIdle, screen::Type::kTable,
                      restored.entries[0].id, restored.entries[0].incarnation)
          .visit_s);
}

void stored_counter_reserves_even_bindings_that_will_be_pruned() {
  seedTwo();
  const auto first = current.entries[0].incarnation;
  TEST_ASSERT_TRUE(settings::reserveIncarnationFloor(current, 40, false));
  TEST_ASSERT_EQUAL_UINT32(first, current.entries[0].incarnation);
  TEST_ASSERT_EQUAL_UINT32(41, current.next_incarnation);
  candidate = current;
  candidate.entries.push(entry());
  TEST_ASSERT_TRUE(settings::assignIncarnations(candidate, current));
  TEST_ASSERT_EQUAL_UINT32(41, candidate.entries[2].incarnation);
  TEST_ASSERT_TRUE(settings::validate(candidate));
}

void retained_maximum_identity_never_wraps_or_reissues_a_provisional_id() {
  seedTwo();
  const auto first = current.entries[0].incarnation;
  TEST_ASSERT_FALSE(settings::reserveIncarnationFloor(
      current, std::numeric_limits<uint32_t>::max(), true));
  TEST_ASSERT_EQUAL_UINT32(first, current.entries[0].incarnation);
  TEST_ASSERT_EQUAL_UINT32(std::numeric_limits<uint32_t>::max(),
                           current.next_incarnation);
  candidate = current;
  candidate.entries.push(entry());
  TEST_ASSERT_FALSE(settings::assignIncarnations(candidate, current));
  TEST_ASSERT_TRUE(settings::reserveIncarnationFloor(
      current, std::numeric_limits<uint32_t>::max(), false));
  candidate = current;
  TEST_ASSERT_TRUE(settings::assignIncarnations(candidate, current));
  TEST_ASSERT_TRUE(settings::validate(candidate));
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
  RUN_TEST(
      record_restores_identity_counter_and_rejects_old_or_corrupt_identity);
  RUN_TEST(
      changed_incarnation_invalidates_provider_data_even_with_same_small_id);
  RUN_TEST(duplicate_or_backward_record_identities_are_rejected);
  RUN_TEST(initial_presets_allocate_in_ram_without_external_identity);
  RUN_TEST(
      retained_policy_cannot_resurrect_after_fallback_save_and_second_boot);
  RUN_TEST(stored_counter_reserves_even_bindings_that_will_be_pruned);
  RUN_TEST(retained_maximum_identity_never_wraps_or_reissues_a_provisional_id);
  return UNITY_END();
}
