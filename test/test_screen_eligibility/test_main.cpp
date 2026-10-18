// Derived provider/classification facts must preserve the owner's effective
// eligibility, including manual-only rules and competition lifetime.
#include <unity.h>

#include <algorithm>

#include "screen_policy.h"
#include "settings_model.h"

namespace {
namespace policy = settings::screen_policy;
settings::Competitions competitions;
policy::Policy rules;

void add(uint8_t id, uint32_t incarnation) {
  settings::Competition c;
  c.id = id;
  c.incarnation = incarnation;
  c.primary.competition.assign("league");
  c.club.team = 40;
  competitions.entries.push(c);
}

policy::Eligibility eligibility() {
  settings::CompetitionRoutes routes;
  for (const auto& entry : competitions.entries)
    routes.entries.push(static_cast<const settings::CompetitionRoute&>(entry));
  return policy::eligibility(rules, routes, &competitions);
}
void manual_only_content_still_needs_its_data() {
  add(1, 37);
  const auto phase = policy::Phase::kOtherGames;
  auto& table =
      rules.shared[policy::index(phase)][screen::index(screen::Type::kTable)];
  table = {60, true, false};
  const auto derived = eligibility();
  TEST_ASSERT_TRUE(derived.allows(phase, screen::Type::kTable, 0));
  table.enabled = false;
  TEST_ASSERT_FALSE(eligibility().allows(phase, screen::Type::kTable, 0));
  competitions.entries[0].active = false;
  TEST_ASSERT_EQUAL_UINT8(0, eligibility().enabled[policy::index(phase)][0]);
}

void shared_edits_and_dormant_overrides_have_one_effective_value() {
  add(1, 37);
  const auto phase = policy::Phase::kIdle;
  const auto type = screen::Type::kTable;
  competitions.entries[0]
      .display.overrides[policy::index(phase)][screen::sportIndex(type)] = {
      60, 0, policy::kEnabled};
  TEST_ASSERT_TRUE(eligibility().allows(phase, type, 0));
  rules.per_competition[policy::index(phase)] |= 1u << screen::sportIndex(type);
  TEST_ASSERT_FALSE(eligibility().allows(phase, type, 0));
  rules.per_competition[policy::index(phase)] = 0;
  rules.shared[policy::index(phase)][screen::index(type)].enabled = false;
  TEST_ASSERT_FALSE(eligibility().allows(phase, type, 0));
  TEST_ASSERT_EQUAL_UINT8(
      policy::kEnabled,
      competitions.entries[0]
          .display.overrides[policy::index(phase)][screen::sportIndex(type)]
          .fields);
}

void reorder_moves_special_treatment_with_identity_and_keeps_buffers() {
  add(1, 37);
  add(2, 81);
  competitions.entries[0].club.own_special = false;
  rules.own_before_min = 10;
  rules.own_after_min = 20;
  rules.other_before_min = 30;
  rules.other_after_min = 40;
  auto derived = policy::selectionRules(rules, competitions);
  TEST_ASSERT_EQUAL_UINT8(2, derived.own_special);
  std::swap(competitions.entries[0], competitions.entries[1]);
  derived = policy::selectionRules(rules, competitions);
  TEST_ASSERT_EQUAL_UINT8(1, derived.own_special);
  TEST_ASSERT_EQUAL_UINT16(10, derived.own_before_min);
  TEST_ASSERT_EQUAL_UINT16(20, derived.own_after_min);
  TEST_ASSERT_EQUAL_UINT16(30, derived.other_before_min);
  TEST_ASSERT_EQUAL_UINT16(40, derived.other_after_min);
}

void reused_id_cannot_inherit_an_old_data_gate() {
  add(1, 38);
  const auto phase = policy::Phase::kIdle;
  const auto type = screen::Type::kTable;
  rules.per_competition[policy::index(phase)] = 1u << screen::sportIndex(type);
  settings::CompetitionRoutes old;
  old.entries.push(
      static_cast<const settings::CompetitionRoute&>(competitions.entries[0]));
  old.entries[0].incarnation = 37;
  competitions.entries[0]
      .display.overrides[policy::index(phase)][screen::sportIndex(type)] = {
      60, 0, policy::kEnabled};
  const auto derived = policy::eligibility(rules, old, &competitions);
  TEST_ASSERT_TRUE(derived.allows(phase, type, 0));
  TEST_ASSERT_EQUAL_UINT8(1, derived.selection.own_special);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(56, sizeof(derived));
}
}  // namespace

void setUp() {
  competitions = {};
  rules = {};
}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(manual_only_content_still_needs_its_data);
  RUN_TEST(shared_edits_and_dormant_overrides_have_one_effective_value);
  RUN_TEST(reorder_moves_special_treatment_with_identity_and_keeps_buffers);
  RUN_TEST(reused_id_cannot_inherit_an_old_data_gate);
  return UNITY_END();
}
