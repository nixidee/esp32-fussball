// Host tests for the input state machine and the role mapping (OQ-141…OQ-143):
// one mandatory input (short Next, double Previous, long Default), Up and
// Down, Scroll with direction toggle, Off.

#include <unity.h>

#include <cstdint>
#include <initializer_list>

#include "input_policy.h"

namespace {

using cfg::InputRole;
using ui::Action;
using ui::InputPolicy;
using ui::mapInput;
using ui::RawInput;

constexpr uint16_t kRepeatMs = 250;

uint8_t bit(RawInput event) { return 1u << static_cast<unsigned>(event); }

// Events of a press from `at` held for `held` ms, polled every 10 ms until
// `until`.
uint8_t press(InputPolicy& input, int64_t at, int64_t held, int64_t until,
              bool doubles) {
  uint8_t events = 0;
  for (int64_t now = at; now <= until; now += 10)
    events |= input.update(now < at + held, now, doubles, kRepeatMs);
  return events;
}

void testMandatoryRole() {
  TEST_ASSERT_EQUAL(Action::kNext, mapInput(InputRole::kMandatory, RawInput::kShort, true));
  TEST_ASSERT_EQUAL(Action::kPrevious, mapInput(InputRole::kMandatory, RawInput::kDouble, true));
  TEST_ASSERT_EQUAL(Action::kDefault, mapInput(InputRole::kMandatory, RawInput::kLong, true));
  TEST_ASSERT_EQUAL(Action::kNone, mapInput(InputRole::kMandatory, RawInput::kRepeat, true));
  TEST_ASSERT_EQUAL(Action::kNone, mapInput(InputRole::kMandatory, RawInput::kRelease, true));
}

void testUpDownRoles() {
  for (const auto event : {RawInput::kShort, RawInput::kLong, RawInput::kRepeat}) {
    // The scroll direction does not change a fixed Up or Down input.
    TEST_ASSERT_EQUAL(Action::kUp, mapInput(InputRole::kUp, event, true));
    TEST_ASSERT_EQUAL(Action::kDown, mapInput(InputRole::kDown, event, false));
  }
  TEST_ASSERT_EQUAL(Action::kNone, mapInput(InputRole::kUp, RawInput::kRelease, true));
  TEST_ASSERT_EQUAL(Action::kNone, mapInput(InputRole::kDown, RawInput::kDouble, true));
}

void testScrollRole() {
  TEST_ASSERT_EQUAL(Action::kToggleDirection, mapInput(InputRole::kScroll, RawInput::kRelease, true));
  TEST_ASSERT_EQUAL(Action::kDown, mapInput(InputRole::kScroll, RawInput::kLong, true));
  TEST_ASSERT_EQUAL(Action::kUp, mapInput(InputRole::kScroll, RawInput::kRepeat, false));
  TEST_ASSERT_EQUAL(Action::kNone, mapInput(InputRole::kScroll, RawInput::kShort, true));
}

void testOffRole() {
  for (const auto event : {RawInput::kShort, RawInput::kLong, RawInput::kDouble,
                           RawInput::kRelease, RawInput::kRepeat})
    TEST_ASSERT_EQUAL(Action::kNone, mapInput(InputRole::kOff, event, true));
}

void testShortPressWithoutDoubles() {
  InputPolicy input;
  const uint8_t events = press(input, 0, 100, 200, false);
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kShort) | bit(RawInput::kRelease), events);
}

void testShortAndDoubleWithDoubles() {
  InputPolicy input;
  // A single press reports Short only after the double window. Release is
  // seen once debounced (100 + kInputDebounceMs).
  const int64_t released = 100 + cfg::kInputDebounceMs;
  uint8_t events = press(input, 0, 100, released + 10, true);
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kRelease), events);
  events = press(input, released + 20, 0, released + cfg::kInputDoubleMs + 50,
                 true);
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kShort), events);

  InputPolicy twice;
  events = press(twice, 0, 100, 200, true);
  events |= press(twice, 210, 100, 400, true);
  TEST_ASSERT_TRUE(events & bit(RawInput::kDouble));
  TEST_ASSERT_FALSE(events & bit(RawInput::kShort));
}

void testLongAndRepeat() {
  InputPolicy input;
  const int64_t held = cfg::kInputLongMs + 3 * kRepeatMs + 100;
  const uint8_t events = press(input, 0, held, held + 100, true);
  TEST_ASSERT_TRUE(events & bit(RawInput::kLong));
  TEST_ASSERT_TRUE(events & bit(RawInput::kRepeat));
  TEST_ASSERT_TRUE(events & bit(RawInput::kRelease));
  // A long press is neither a short nor half of a double press.
  TEST_ASSERT_FALSE(events & bit(RawInput::kShort));
  TEST_ASSERT_FALSE(events & bit(RawInput::kDouble));
}

void testSaveDiscardsReleasedMandatoryClick() {
  InputPolicy input;
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kRelease),
                         press(input, 0, 100, 150, true));
  input.cancel();
  TEST_ASSERT_EQUAL_HEX8(0, press(input, 160, 0, 500, true));
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kShort) | bit(RawInput::kRelease),
                         press(input, 600, 100, 1100, true));
}

void testSaveDuringSecondPressDiscardsDouble() {
  InputPolicy input;
  press(input, 0, 100, 150, true);
  TEST_ASSERT_EQUAL_HEX8(0, press(input, 200, 100, 250, true));
  input.cancel();
  TEST_ASSERT_EQUAL_HEX8(0, press(input, 260, 40, 1000, true));
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kShort) | bit(RawInput::kRelease),
                         press(input, 1100, 100, 1600, true));
}

void testSaveSuppressesHeldScrollAndItsRelease() {
  InputPolicy input;
  TEST_ASSERT_EQUAL_HEX8(0, input.update(true, 0, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(0, input.update(true, 40, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kLong),
                         input.update(true, 640, false, kRepeatMs));
  input.cancel();
  for (const int64_t now : {650, 900, 1200})
    TEST_ASSERT_EQUAL_HEX8(0, input.update(true, now, false, kRepeatMs));
  // An inactive bounce is not a completed release.
  TEST_ASSERT_EQUAL_HEX8(0, input.update(false, 1210, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(0, input.update(true, 1230, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(0, input.update(true, 1500, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(0, input.update(false, 1510, false, kRepeatMs));
  const auto release = input.update(false, 1550, false, kRepeatMs);
  TEST_ASSERT_EQUAL_HEX8(0, release);
  TEST_ASSERT_FALSE(release & bit(RawInput::kRelease));
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kLong) | bit(RawInput::kRelease),
                         press(input, 1600, 700, 2400, false));
}

void testSaveDuringPressDebounceWaitsForRelease() {
  InputPolicy input;
  TEST_ASSERT_EQUAL_HEX8(0, input.update(true, 0, false, kRepeatMs));
  input.cancel();  // The press is raw-active but not stable yet.
  TEST_ASSERT_EQUAL_HEX8(0, input.update(true, 40, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(0, input.update(true, 1000, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(0, input.update(false, 1010, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(0, input.update(false, 1050, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kShort) | bit(RawInput::kRelease),
                         press(input, 1100, 100, 1300, false));
}

void testSaveDuringReleaseDebounceEmitsNoRelease() {
  InputPolicy input;
  input.update(true, 0, false, kRepeatMs);
  input.update(true, 40, false, kRepeatMs);
  input.update(false, 100, false, kRepeatMs);
  input.cancel();
  TEST_ASSERT_EQUAL_HEX8(0, input.update(false, 120, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(0, input.update(false, 140, false, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kShort) | bit(RawInput::kRelease),
                         press(input, 150, 100, 350, false));
}

void testRepeatedSavesKeepHeldKeyBlocked() {
  InputPolicy input;
  input.update(true, 0, true, kRepeatMs);
  input.update(true, 40, true, kRepeatMs);
  input.cancel();
  TEST_ASSERT_EQUAL_HEX8(0, input.update(true, 1000, true, kRepeatMs));
  input.cancel();
  TEST_ASSERT_EQUAL_HEX8(0, input.update(false, 1010, true, kRepeatMs));
  input.cancel();
  TEST_ASSERT_EQUAL_HEX8(0, input.update(false, 1050, true, kRepeatMs));
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kShort) | bit(RawInput::kRelease),
                         press(input, 1100, 100, 1600, true));
}

void testIdleCancellationKeepsFirstPressUsable() {
  InputPolicy input;
  input.cancel();
  input.cancel();
  TEST_ASSERT_EQUAL_HEX8(bit(RawInput::kShort) | bit(RawInput::kRelease),
                         press(input, 0, 100, 200, false));
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(testMandatoryRole);
  RUN_TEST(testUpDownRoles);
  RUN_TEST(testScrollRole);
  RUN_TEST(testOffRole);
  RUN_TEST(testShortPressWithoutDoubles);
  RUN_TEST(testShortAndDoubleWithDoubles);
  RUN_TEST(testLongAndRepeat);
  RUN_TEST(testSaveDiscardsReleasedMandatoryClick);
  RUN_TEST(testSaveDuringSecondPressDiscardsDouble);
  RUN_TEST(testSaveSuppressesHeldScrollAndItsRelease);
  RUN_TEST(testSaveDuringPressDebounceWaitsForRelease);
  RUN_TEST(testSaveDuringReleaseDebounceEmitsNoRelease);
  RUN_TEST(testRepeatedSavesKeepHeldKeyBlocked);
  RUN_TEST(testIdleCancellationKeepsFirstPressUsable);
  return UNITY_END();
}
