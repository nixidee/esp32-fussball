// Owner-visible screen-policy contracts, using the production pure model and
// explicit record. These checks do not prove API, provider or device behavior.
#include <unity.h>

#include <algorithm>
#include <array>
#include <cstdint>

#include "screen_policy.h"
#include "screen_record.h"
#include "settings_record.h"

namespace {
namespace policy = settings::screen_policy;
namespace record = settings::screen_record;
using screen::Type;
constexpr auto kPhase = policy::Phase::kIdle;
constexpr auto kType = Type::kTable;
constexpr auto kPhaseIndex = policy::index(kPhase);
constexpr auto kTypeIndex = screen::index(kType);
constexpr auto kSportIndex = screen::sportIndex(kType);

policy::Policy model;
policy::Policy restored;
std::array<uint8_t, record::kRecordBytes> bytes{};

void addCompetitions() {
  for (unsigned i = 0; i < model.competitions.size(); ++i)
    model.competitions[i] = {1000u + i, static_cast<uint8_t>(i + 1),
                             i % 2 == 0};
}

policy::Rule resolved() {
  return policy::resolve(model, kPhase, kType, 1, 1000);
}

void fresh_profiles_keep_every_sport_manual_and_choose_one_automatic_type() {
  const std::array<Type, policy::kPhaseCount> automatic{
      Type::kLiveSingle, Type::kLiveMulti, Type::kMatchday, Type::kTable,
      Type::kTable};
  for (unsigned phase = 0; phase < policy::kPhaseCount; ++phase)
    for (unsigned type = 0; type < screen::kTypeCount; ++type) {
      const auto kind = static_cast<Type>(type);
      const auto& rule = model.shared[phase][type];
      if (screen::sporting(kind)) {
        TEST_ASSERT_TRUE(rule.enabled);
        TEST_ASSERT_EQUAL(kind == automatic[phase], rule.automatic);
      } else {
        TEST_ASSERT_FALSE(rule.enabled);
        TEST_ASSERT_TRUE(rule.automatic);
      }
    }
  TEST_ASSERT_FALSE(model.frame);
  TEST_ASSERT_FALSE(model.automatic_break);
  TEST_ASSERT_TRUE(model.frame_crest);
  TEST_ASSERT_EQUAL_UINT16(21, model.break_days);
  TEST_ASSERT_TRUE(policy::validate(model));
}

void live_inheritance_preserves_explicit_false_and_dormant_overrides() {
  addCompetitions();
  model.shared[kPhaseIndex][kTypeIndex] = {30, true, true};
  model.expanded[kPhaseIndex] = 1u << kSportIndex;
  auto& own = model.overrides[kPhaseIndex][kSportIndex][0];
  own.fields = policy::kEnabled;
  TEST_ASSERT_FALSE(resolved().enabled);
  TEST_ASSERT_TRUE(resolved().automatic);
  model.shared[kPhaseIndex][kTypeIndex].visit_s = 90;
  TEST_ASSERT_EQUAL_UINT16(90, resolved().visit_s);
  own.fields |= policy::kVisit | policy::kAutomatic;
  own.visit_s = 45;
  TEST_ASSERT_FALSE(resolved().automatic);
  TEST_ASSERT_EQUAL_UINT16(45, resolved().visit_s);
  model.expanded[kPhaseIndex] = 0;
  TEST_ASSERT_TRUE(resolved().automaticallyReachable());
  TEST_ASSERT_EQUAL_UINT16(90, resolved().visit_s);
  model.expanded[kPhaseIndex] = 1u << kSportIndex;
  TEST_ASSERT_FALSE(resolved().enabled);
  TEST_ASSERT_EQUAL_UINT16(45, resolved().visit_s);
  own.fields &= ~policy::kEnabled;
  TEST_ASSERT_TRUE(resolved().enabled);
  TEST_ASSERT_FALSE(resolved().automaticallyReachable());
  TEST_ASSERT_TRUE(policy::validate(model));
}

void reused_competition_id_cannot_inherit_a_former_competitions_override() {
  addCompetitions();
  model.shared[kPhaseIndex][kTypeIndex] = {60, true, true};
  model.expanded[kPhaseIndex] = 1u << kSportIndex;
  model.overrides[kPhaseIndex][kSportIndex][0] = {120, 0, policy::kVisit};
  TEST_ASSERT_EQUAL_UINT16(120, resolved().visit_s);
  TEST_ASSERT_EQUAL_UINT16(
      60, policy::resolve(model, kPhase, kType, 1, 9999).visit_s);
  TEST_ASSERT_EQUAL_UINT16(
      60, policy::resolve(model, kPhase, kType, 9, 1000).visit_s);
  model.competitions[1].id = model.competitions[0].id;
  policy::Error error;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Reason::kDuplicate, error.reason);
  TEST_ASSERT_EQUAL(policy::Property::kId, error.property);
  model.competitions[1].id = 2;
  model.competitions[1].incarnation = model.competitions[0].incarnation;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Property::kIncarnation, error.property);
}

void image_time_multiple_is_exact_and_disabled_visits_are_retained() {
  auto& slides = model.shared[kPhaseIndex][screen::index(Type::kSlideshow)];
  slides = {60, true, true};
  model.per_image_s = 20;
  TEST_ASSERT_TRUE(policy::validate(model));
  slides.visit_s = 61;
  policy::Error error;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Reason::kImageMultiple, error.reason);
  TEST_ASSERT_EQUAL_UINT16(61, slides.visit_s);
  slides.enabled = false;
  TEST_ASSERT_TRUE(policy::validate(model));
  TEST_ASSERT_EQUAL_UINT16(61, slides.visit_s);
  slides.enabled = true;
  slides.visit_s = 60;
  slides.automatic = false;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Reason::kImageAutomatic, error.reason);
  slides.automatic = true;
  // Crest has its own visit, independent of the library image interval.
  model.shared[kPhaseIndex][screen::index(Type::kCrest)] = {15, true, true};
  TEST_ASSERT_TRUE(policy::validate(model));
}

void manual_only_sports_keep_their_time_and_pages_receive_the_whole_visit() {
  policy::Rule manual{15, true, false};
  TEST_ASSERT_FALSE(manual.automaticallyReachable());
  TEST_ASSERT_EQUAL_UINT32(40000, policy::effectiveVisitMs(manual, 8));
  manual.visit_s = 16;
  const auto total = policy::effectiveVisitMs(manual, 3);
  TEST_ASSERT_EQUAL_UINT32(16000, total);
  TEST_ASSERT_EQUAL_UINT32(0, policy::pageBoundaryMs(total, 3, 0));
  TEST_ASSERT_EQUAL_UINT32(5333, policy::pageBoundaryMs(total, 3, 1));
  TEST_ASSERT_EQUAL_UINT32(10666, policy::pageBoundaryMs(total, 3, 2));
  TEST_ASSERT_EQUAL_UINT32(total, policy::pageBoundaryMs(total, 3, 3));
  uint32_t sum = 0;
  for (unsigned page = 0; page < 3; ++page) {
    const auto duration = policy::pageBoundaryMs(total, 3, page + 1) -
                          policy::pageBoundaryMs(total, 3, page);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(5000, duration);
    sum += duration;
  }
  TEST_ASSERT_EQUAL_UINT32(total, sum);
  TEST_ASSERT_EQUAL_UINT32(327675000, policy::effectiveVisitMs(manual, 65535));
  TEST_ASSERT_EQUAL_UINT32(327675000,
                           policy::pageBoundaryMs(327675000, 65535, 65535));
}

void out_of_range_and_unknown_configuration_is_rejected_without_clamping() {
  auto& rule = model.shared[kPhaseIndex][kTypeIndex];
  rule.visit_s = 43200;
  model.manual_return_s = 0;
  model.break_days = 366;
  TEST_ASSERT_TRUE(policy::validate(model));
  rule.visit_s = 43201;
  policy::Error error;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL_UINT16(43201, rule.visit_s);
  TEST_ASSERT_EQUAL(policy::Property::kVisit, error.property);
  rule.visit_s = 15;
  model.manual_return_s = 4000;
  TEST_ASSERT_TRUE(policy::validate(model));
  model.manual_return_s = 4001;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Field::kManualReturn, error.field);
  model.manual_return_s = 60;
  model.expanded[kPhaseIndex] = 0x80;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Reason::kUnknownBits, error.reason);
  model.expanded[kPhaseIndex] = 0;
  model.order[0] = model.order[1];
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Field::kOrder, error.field);
}

void complete_record_retains_every_rule_appearance_and_option() {
  addCompetitions();
  for (unsigned phase = 0; phase < policy::kPhaseCount; ++phase) {
    model.expanded[phase] = (1u << screen::kSportTypeCount) - 1;
    for (auto& rule : model.shared[phase]) {
      rule.enabled = true;
      rule.automatic = true;
    }
    for (unsigned type = 0; type < screen::kSportTypeCount; ++type)
      for (unsigned competition = 0; competition < cfg::kCompetitionMax;
           ++competition)
        model.overrides[phase][type][competition] = {
            static_cast<uint16_t>(15 + 1000 * phase + 100 * type + competition),
            static_cast<uint8_t>(competition % 4), policy::kOverrideFields};
  }
  for (unsigned type = 0; type < screen::kTypeCount; ++type) {
    auto& style = model.appearance[type];
    style.colour = type * 0x1234;
    style.text = 0xffffff - type * 0x4321;
    style.accent = 0x123456 + type;
    style.text_scale = 70 + type * 10;
    style.image = type == screen::index(Type::kCrest) ? 0 : type + 1;
    style.background = type % 2;
    style.dots = !(type % 2);
  }
  std::reverse(model.order.begin(), model.order.end());
  model.slides = {1, 2, 3, 4, 10};
  model.slide_count = model.slides.size();
  model.break_days = 366;
  model.own_before_min = 0;
  model.own_after_min = 180;
  model.other_before_min = 179;
  model.other_after_min = 1;
  model.manual_return_s = 4000;
  model.per_image_s = 3;
  model.visible_matches =
      2;  // Common owner-approved minimum leaves pin capacity.
  model.table_window = 9;
  model.conference_mode = cfg::ConferenceMode::kOnePage;
  model.matchday_mode = cfg::MatchdayMode::kNext;
  model.backgrounds = false;
  model.pin_own_match = false;
  model.frame = true;
  model.frame_crest = false;
  model.automatic_break = true;
  TEST_ASSERT_TRUE(policy::validate(model));
  TEST_ASSERT_EQUAL(record::kRecordBytes, record::encode(model, bytes));
  TEST_ASSERT_EQUAL_UINT8(1, bytes[0]);
  TEST_ASSERT_EQUAL_UINT8(0, bytes[1]);
  TEST_ASSERT_EQUAL(record::DecodeResult::kOk, record::decode(bytes, restored));
  TEST_ASSERT_TRUE(model == restored);
}

void corrupt_truncated_or_unknown_records_never_reach_the_candidate() {
  TEST_ASSERT_EQUAL(record::kRecordBytes, record::encode(model, bytes));
  restored.manual_return_s = 321;
  const auto before = restored;
  for (auto& byte : bytes) {
    byte ^= 0x80;
    TEST_ASSERT_NOT_EQUAL(record::DecodeResult::kOk,
                          record::decode(bytes, restored));
    TEST_ASSERT_TRUE(before == restored);
    byte ^= 0x80;
  }
  TEST_ASSERT_EQUAL(record::DecodeResult::kTooShort,
                    record::decode(std::span(bytes).first(7), restored));
  TEST_ASSERT_EQUAL(
      record::DecodeResult::kLengthMismatch,
      record::decode(std::span(bytes).first(bytes.size() - 1), restored));
  bytes[0] = 2;
  TEST_ASSERT_EQUAL(record::DecodeResult::kUnknownVersion,
                    record::decode(bytes, restored));
  bytes[0] = 0;
  TEST_ASSERT_EQUAL(record::DecodeResult::kOutdatedVersion,
                    record::decode(bytes, restored));
}

void reseal() {
  const auto crc =
      settings::crc32(std::span(bytes).subspan(record::kHeaderBytes),
                      settings::crc32(std::span(bytes).first(4)));
  for (unsigned i = 0; i < 4; ++i)
    bytes[4 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

void checksum_valid_but_semantically_invalid_records_are_rejected() {
  TEST_ASSERT_EQUAL(record::kRecordBytes, record::encode(model, bytes));
  bytes[record::kHeaderBytes] |= 0x80;  // Unknown global flag.
  reseal();
  policy::Error error;
  TEST_ASSERT_EQUAL(record::DecodeResult::kInvalidValue,
                    record::decode(bytes, restored, &error));
  TEST_ASSERT_EQUAL(policy::Reason::kUnknownBits, error.reason);
  TEST_ASSERT_EQUAL(record::kRecordBytes, record::encode(model, bytes));
  bytes.back() = 0x80;  // Unknown explicit-field mask in final override.
  reseal();
  TEST_ASSERT_EQUAL(record::DecodeResult::kInvalidValue,
                    record::decode(bytes, restored, &error));
  TEST_ASSERT_EQUAL(policy::Field::kOverride, error.field);
  TEST_ASSERT_EQUAL(policy::Property::kFields, error.property);
  // Oversize/capacity errors are explicit and do not write a partial record.
  std::array<uint8_t, record::kRecordBytes + 1> larger{};
  TEST_ASSERT_EQUAL(record::DecodeResult::kTooLong,
                    record::decode(larger, restored));
  bytes.fill(0xa5);
  TEST_ASSERT_EQUAL(
      0, record::encode(model, std::span(bytes).first(bytes.size() - 1)));
  TEST_ASSERT_TRUE(std::all_of(bytes.begin(), bytes.end(),
                               [](uint8_t value) { return value == 0xa5; }));
}

void common_visible_count_requires_two_but_available_content_may_be_one() {
  policy::Error error;
  model.visible_matches = 1;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Field::kVisibleMatches, error.field);
  TEST_ASSERT_EQUAL(policy::Reason::kRange, error.reason);
  model.visible_matches = 2;
  TEST_ASSERT_TRUE(policy::validate(model));
}

}  // namespace

void setUp() {
  model = policy::Policy{};
  restored = policy::Policy{};
}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(
      fresh_profiles_keep_every_sport_manual_and_choose_one_automatic_type);
  RUN_TEST(live_inheritance_preserves_explicit_false_and_dormant_overrides);
  RUN_TEST(reused_competition_id_cannot_inherit_a_former_competitions_override);
  RUN_TEST(image_time_multiple_is_exact_and_disabled_visits_are_retained);
  RUN_TEST(
      manual_only_sports_keep_their_time_and_pages_receive_the_whole_visit);
  RUN_TEST(out_of_range_and_unknown_configuration_is_rejected_without_clamping);
  RUN_TEST(complete_record_retains_every_rule_appearance_and_option);
  RUN_TEST(corrupt_truncated_or_unknown_records_never_reach_the_candidate);
  RUN_TEST(checksum_valid_but_semantically_invalid_records_are_rejected);
  RUN_TEST(common_visible_count_requires_two_but_available_content_may_be_one);
  return UNITY_END();
}
