// Host checks of the provider JSON mappers (football::mapEvent) with
// ArduinoJson. The goal record is trimmed from a recorded ESPN scoreboard
// response (ger.1, 2026-09-19); the substitution records are written by the
// agent, because the scoreboard responses recorded on 2026-10-13 contained
// no substitution. They prove only the mapping, not what a provider delivers
// or the owner's device acceptance.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstring>

#include "app_config.h"
#include "football_model.h"
#include "provider_mappers.h"

namespace {

using football::EventType;
using football::Match;
using football::MatchEvent;
using football::Snapshot;

Snapshot out;
Match match;

const MatchEvent& mapOne(const char* json) {
  ArduinoJson::JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, json));
  out.reset();
  match = Match{};
  match.provider = cfg::Provider::kEspn;
  match.home.id.assign("268");
  match.away.id.assign("131");
  TEST_ASSERT_TRUE(football::mapEvent(doc.as<ArduinoJson::JsonVariantConst>(),
                                      out, match, cfg::Provider::kEspn));
  TEST_ASSERT_EQUAL_UINT8(1, match.event_count);
  return out.events[match.events[0]];
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_recorded_espn_goal() {
  const auto& e = mapOne(
      R"({"type":{"id":"70","text":"Goal"},)"
      R"("clock":{"value":651.0,"displayValue":"11'"},"team":{"id":"268"},)"
      R"("scoreValue":1,"scoringPlay":true,"redCard":false,)"
      R"("yellowCard":false,"penaltyKick":false,"ownGoal":false,)"
      R"("shootout":false,"athletesInvolved":[{"id":"238527",)"
      R"("displayName":"Florian Neuhaus","team":{"id":"268"}}]})");
  TEST_ASSERT_EQUAL(EventType::kGoal, e.type);
  TEST_ASSERT_EQUAL_STRING("Florian Neuhaus", e.player.data);
  TEST_ASSERT_EQUAL_INT16(11, e.minute.value);
  TEST_ASSERT_EQUAL_STRING("268", e.beneficiary.data);
}

void test_substitution_keeps_both_names_without_direction() {
  const auto& e = mapOne(
      R"({"type":{"text":"Substitution"},"clock":{"displayValue":"64'"},)"
      R"("team":{"id":"268"},"athletesInvolved":[)"
      R"({"displayName":"Wael Mohya"},{"displayName":"Florian Neuhaus"}]})");
  TEST_ASSERT_EQUAL(EventType::kSubstitution, e.type);
  TEST_ASSERT_EQUAL_STRING("Wael Mohya / Florian Neuhaus", e.player.data);
  TEST_ASSERT_TRUE(e.player_in.empty());
  TEST_ASSERT_TRUE(e.player_out.empty());
  TEST_ASSERT_EQUAL_STRING("Substitution", e.detail.data);
}

void test_substitution_with_one_name() {
  const auto& e = mapOne(
      R"({"type":{"text":"Substitution"},"clock":{"displayValue":"70'"},)"
      R"("team":{"id":"131"},"athletesInvolved":[{"displayName":"A. B"}]})");
  TEST_ASSERT_EQUAL_STRING("A. B", e.player.data);
}

void test_substitution_names_cut_at_a_whole_character() {
  // 27 + " / " = 30 bytes; "ü" of "Müller" would occupy bytes 32 and 33.
  const auto& e = mapOne(
      R"({"type":{"text":"Substitution"},"clock":{"displayValue":"80'"},)"
      R"("team":{"id":"268"},"athletesInvolved":[)"
      R"({"displayName":"Abcdefghijklmnopqrstuvwxyza"},)"
      R"({"displayName":"Müller"}]})");
  TEST_ASSERT_EQUAL_STRING("Abcdefghijklmnopqrstuvwxyza / M", e.player.data);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_recorded_espn_goal);
  RUN_TEST(test_substitution_keeps_both_names_without_direction);
  RUN_TEST(test_substitution_with_one_name);
  RUN_TEST(test_substitution_names_cut_at_a_whole_character);
  return UNITY_END();
}
