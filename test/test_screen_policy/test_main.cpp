// Owner-visible screen-policy contracts, using the production pure model and
// aggregate/payload codecs. These checks do not prove API, provider or device
// behavior.
#include <unity.h>

#include <algorithm>
#include <array>
#include <cstdint>

#include "app_config_record.h"
#include "demo_scenarios.h"
#include "screen_policy.h"
#include "screen_record.h"
#include "ui_config.h"

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
settings::Competitions competitions;
std::array<uint8_t, record::kPayloadBytes> bytes{};

void addCompetitions() {
  settings::initialCompetitions({.competition = "league"}, competitions);
  competitions.entries[0].id = 1;
  competitions.entries[0].incarnation = 1000;
}
policy::Rule resolved() {
  return policy::resolve(model, kPhase, kType, &competitions.entries[0]);
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
  model.per_competition[kPhaseIndex] = 1u << kSportIndex;
  auto& own =
      competitions.entries[0].display.overrides[kPhaseIndex][kSportIndex];
  own.fields = policy::kEnabled;
  TEST_ASSERT_FALSE(resolved().enabled);
  TEST_ASSERT_TRUE(resolved().automatic);
  model.shared[kPhaseIndex][kTypeIndex].visit_s = 90;
  TEST_ASSERT_EQUAL_UINT16(90, resolved().visit_s);
  own.fields |= policy::kVisit | policy::kAutomatic;
  own.visit_s = 45;
  TEST_ASSERT_FALSE(resolved().automatic);
  TEST_ASSERT_EQUAL_UINT16(45, resolved().visit_s);
  model.per_competition[kPhaseIndex] = 0;
  TEST_ASSERT_TRUE(resolved().automaticallyReachable());
  TEST_ASSERT_EQUAL_UINT16(90, resolved().visit_s);
  model.per_competition[kPhaseIndex] = 1u << kSportIndex;
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
  model.per_competition[kPhaseIndex] = 1u << kSportIndex;
  competitions.entries[0].display.overrides[kPhaseIndex][kSportIndex] = {
      120, 0, policy::kVisit};
  TEST_ASSERT_EQUAL_UINT16(120, resolved().visit_s);
  // Replacement is a new competition value; preferences cannot leak through
  // a separately retained identity-association slot because none exists.
  settings::Competition replacement;
  replacement.id = 1;
  replacement.incarnation = 1001;
  TEST_ASSERT_EQUAL_UINT16(
      60, policy::resolve(model, kPhase, kType, &replacement).visit_s);
  TEST_ASSERT_EQUAL_UINT16(60, policy::resolve(model, kPhase, kType).visit_s);
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
  model.per_competition[kPhaseIndex] = 0x80;
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Reason::kUnknownBits, error.reason);
  model.per_competition[kPhaseIndex] = 0;
  model.order[0] = model.order[1];
  TEST_ASSERT_FALSE(policy::validate(model, &error));
  TEST_ASSERT_EQUAL(policy::Field::kOrder, error.field);
}

void complete_record_retains_every_rule_appearance_and_option() {
  addCompetitions();
  for (unsigned phase = 0; phase < policy::kPhaseCount; ++phase) {
    model.per_competition[phase] = (1u << screen::kSportTypeCount) - 1;
    for (auto& rule : model.shared[phase]) {
      rule.enabled = true;
      rule.automatic = true;
    }
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
  TEST_ASSERT_EQUAL(record::kPayloadBytes, record::encodePayload(model, bytes));
  TEST_ASSERT_EQUAL(record::DecodeResult::kOk,
                    record::decodePayload(bytes, restored));
  TEST_ASSERT_TRUE(model == restored);
}

void corrupt_truncated_or_unknown_aggregates_never_reach_screen_candidate() {
  namespace aggregate = settings::app_record;
  settings::AppConfig config;
  config.model = settings::initialValues({});
  config.screens = model;
  std::array<uint8_t, aggregate::kMaxBytes> encoded{};
  const auto used = aggregate::encode(config, encoded);
  TEST_ASSERT_NOT_EQUAL(0, used);
  settings::AppConfig scratch;
  scratch.screens.manual_return_s = 321;
  const auto before = scratch.screens;
  for (std::size_t at = 0; at < used; ++at) {
    auto corrupt = encoded;
    corrupt[at] ^= 0x80;
    TEST_ASSERT_NOT_EQUAL(
        settings::DecodeResult::kOk,
        aggregate::decode(std::span(corrupt).first(used), scratch));
    TEST_ASSERT_TRUE(before == scratch.screens);
  }
  for (std::size_t size = 0; size < used; ++size) {
    TEST_ASSERT_NOT_EQUAL(
        settings::DecodeResult::kOk,
        aggregate::decode(std::span(encoded).first(size), scratch));
    TEST_ASSERT_TRUE(before == scratch.screens);
  }
  for (const uint16_t version : {0, 2}) {
    aggregate::seal(std::span(encoded).first(used), version);
    TEST_ASSERT_EQUAL(
        settings::DecodeResult::kUnknownVersion,
        aggregate::decode(std::span(encoded).first(used), scratch));
    TEST_ASSERT_TRUE(before == scratch.screens);
  }
}

void exact_screen_payload_rejects_unknown_flags_and_wrong_capacity() {
  TEST_ASSERT_EQUAL(record::kPayloadBytes, record::encodePayload(model, bytes));
  bytes[0] |= 0x80;  // Unknown global flag.
  policy::Error error;
  TEST_ASSERT_EQUAL(record::DecodeResult::kInvalidValue,
                    record::decodePayload(bytes, restored, &error));
  TEST_ASSERT_EQUAL(policy::Reason::kUnknownBits, error.reason);
  TEST_ASSERT_EQUAL(record::kPayloadBytes, record::encodePayload(model, bytes));
  bytes.back() = 0x80;  // Unknown flag in the final shared rule.
  TEST_ASSERT_EQUAL(record::DecodeResult::kInvalidValue,
                    record::decodePayload(bytes, restored, &error));
  TEST_ASSERT_EQUAL(policy::Field::kShared, error.field);
  TEST_ASSERT_EQUAL(policy::Property::kValues, error.property);
  // Oversize/capacity errors are explicit and do not write a partial record.
  std::array<uint8_t, record::kPayloadBytes + 1> larger{};
  TEST_ASSERT_EQUAL(record::DecodeResult::kLengthMismatch,
                    record::decodePayload(larger, restored));
  bytes.fill(0xa5);
  TEST_ASSERT_EQUAL(0, record::encodePayload(
                           model, std::span(bytes).first(bytes.size() - 1)));
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

void normalized_program_keeps_saved_demo_order_and_shared_synthetic_rules() {
  settings::Competitions configured;
  for (const auto id : {47, 12}) {
    settings::Competition entry;
    entry.id = id;
    entry.incarnation = id == 47 ? 103 : 101;
    entry.primary.competition.assign("league");
    entry.club.team = 40;
    configured.entries.push(entry);
  }
  configured.entries[0].club.own_special = false;
  model.per_competition[policy::index(policy::Phase::kOwn)] =
      1u << screen::sportIndex(Type::kLiveSingle);
  configured.entries[0].display.overrides[0][0] = {27, policy::kAutomatic,
                                                   policy::kOverrideFields};
  const auto retained = policy::resolve(
      model, policy::Phase::kOwn, Type::kLiveSingle, &configured.entries[0]);
  TEST_ASSERT_FALSE(retained.enabled);
  TEST_ASSERT_TRUE(retained.automatic);
  TEST_ASSERT_EQUAL_UINT16(27, retained.visit_s);
  settings::UiConfig ui;
  settings::projectUiConfig({}, model, configured, policy::Phase::kOwn, ui);
  settings::CompetitionRoutes routes;
  for (const auto& c : configured.entries)
    routes.entries.push(static_cast<const settings::CompetitionRoute&>(c));
  const auto binding = football::demo::configure(routes);
  TEST_ASSERT_EQUAL_UINT8(47, binding[0].competition);
  TEST_ASSERT_EQUAL_UINT8(12, binding[1].competition);
  const auto gates = policy::eligibility(model, routes, &configured);
  TEST_ASSERT_FALSE(gates.allows(policy::Phase::kOwn, Type::kLiveSingle, 0));
  TEST_ASSERT_TRUE(gates.allows(policy::Phase::kOwn, Type::kLiveSingle, 1));
  policy::Program program;
  settings::demoProgram(ui, routes, program);
  for (unsigned i = 0; i < program.count; ++i)
    if (program.entries[i].type == Type::kLiveSingle)
      TEST_ASSERT_EQUAL_UINT8(12, program.entries[i].competition);
  // A dedicated synthetic id has incarnation zero and shared rules, even if
  // its numeric value happens to equal a previously configured identity.
  routes.entries[0].incarnation = 0;
  settings::demoProgram(ui, routes, program);
  bool synthetic_single = false;
  for (unsigned i = 0; i < program.count; ++i)
    if (program.entries[i].type == Type::kLiveSingle &&
        program.entries[i].competition == 47) {
      synthetic_single = true;
      TEST_ASSERT_TRUE(program.entries[i].rule.enabled);
      TEST_ASSERT_EQUAL_UINT16(model.shared[0][0].visit_s,
                               program.entries[i].rule.visit_s);
    }
  TEST_ASSERT_TRUE(synthetic_single);
  configured.entries[0].id = football::demo::kSecondCompetition;
  configured.entries[0].incarnation = 73;
  configured.entries.count = 1;
  settings::projectUiConfig({}, model, configured, policy::Phase::kOwn, ui);
  routes.clear();
  routes.entries.push(
      static_cast<const settings::CompetitionRoute&>(configured.entries[0]));
  const auto collision = football::demo::configure(routes);
  TEST_ASSERT_FALSE(collision[1].saved);
  TEST_ASSERT_EQUAL_UINT32(0, collision[1].incarnation);
  TEST_ASSERT_NOT_EQUAL(collision[0].competition, collision[1].competition);
  settings::demoProgram(ui, routes, program);
  synthetic_single = false;
  for (unsigned i = 0; i < program.count; ++i)
    synthetic_single |=
        program.entries[i].type == Type::kLiveSingle &&
        program.entries[i].competition == collision[1].competition;
  TEST_ASSERT_TRUE(synthetic_single);
}
void explicit_groups_keep_priority_and_appearance_keeps_navigation() {
  settings::Competitions entries;
  for (unsigned i = 0; i < 4; ++i) {
    settings::Competition c;
    c.id = i + 1;
    c.incarnation = i + 1;
    c.primary.competition.assign("league");
    c.club.team = 40;
    c.club.name.assign("Same name");
    entries.entries.push(c);
  }
  entries.entries[0].club.group = entries.entries[2].club.group = 1;
  auto order = policy::competitionOrder(entries);
  TEST_ASSERT_EQUAL_UINT8(0, order.slots[0]);
  TEST_ASSERT_EQUAL_UINT8(2, order.slots[1]);
  TEST_ASSERT_EQUAL_UINT8(1, order.slots[2]);
  TEST_ASSERT_EQUAL_UINT8(3, order.slots[3]);
  settings::UiConfig before, after;
  settings::projectUiConfig({}, model, entries, kPhase, before);
  model.appearance[0].colour ^= 0x123456;
  settings::projectUiConfig({}, model, entries, kPhase, after);
  TEST_ASSERT_TRUE(settings::sameNavigation(before, after));
  model.shared[kPhaseIndex][kTypeIndex].visit_s++;
  settings::projectUiConfig({}, model, entries, kPhase, after);
  TEST_ASSERT_FALSE(settings::sameNavigation(before, after));
}

}  // namespace

void setUp() {
  competitions = {};
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
  RUN_TEST(
      corrupt_truncated_or_unknown_aggregates_never_reach_screen_candidate);
  RUN_TEST(exact_screen_payload_rejects_unknown_flags_and_wrong_capacity);
  RUN_TEST(common_visible_count_requires_two_but_available_content_may_be_one);
  RUN_TEST(
      normalized_program_keeps_saved_demo_order_and_shared_synthetic_rules);
  RUN_TEST(explicit_groups_keep_priority_and_appearance_keeps_navigation);
  return UNITY_END();
}
