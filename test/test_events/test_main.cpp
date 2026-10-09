// Host acceptance for the event catalog and the queue admission of the event
// bus (docs/ARCHITECTURE.md): coalesced state events, bounded UI slots and a
// queue that state events can never overflow.

#include <unity.h>

#include <cstddef>
#include <cstring>
#include <iterator>

#include "app_config.h"
#include "event_admission.h"

namespace {

using events::Admission;
using events::Event;
using events::kQueueLength;
using events::kStateEventCount;

constexpr Event kStateEvents[] = {Event::kSettingsChanged, Event::kNetworkState,
                                  Event::kDataUpdated, Event::kOtaState,
                                  Event::kTimeChanged};

void testCatalog() {
  TEST_ASSERT_EQUAL_UINT(kStateEventCount, std::size(kStateEvents));
  for (Event event : kStateEvents) {
    TEST_ASSERT_TRUE(events::isStateEvent(event));
  }
  TEST_ASSERT_FALSE(events::isStateEvent(Event::kUiAction));
  TEST_ASSERT_EQUAL_UINT(kStateEventCount + cfg::kEventUiActionSlots,
                         kQueueLength);
}

void testEventNamesAreDistinct() {
  constexpr Event kAll[] = {Event::kSettingsChanged, Event::kNetworkState,
                            Event::kDataUpdated,     Event::kOtaState,
                            Event::kTimeChanged,     Event::kUiAction};
  for (std::size_t i = 0; i < std::size(kAll); ++i) {
    TEST_ASSERT_NOT_EQUAL(0,
                          std::strcmp("unknown", events::eventName(kAll[i])));
    for (std::size_t j = i + 1; j < std::size(kAll); ++j) {
      TEST_ASSERT_NOT_EQUAL(0, std::strcmp(events::eventName(kAll[i]),
                                           events::eventName(kAll[j])));
    }
  }
}

void testStateEventCoalesces() {
  Admission admission;
  TEST_ASSERT_TRUE(admission.admit(Event::kSettingsChanged));
  TEST_ASSERT_FALSE(admission.admit(Event::kSettingsChanged));
  TEST_ASSERT_FALSE(admission.admit(Event::kSettingsChanged));
}

void testReleaseAllowsStateEventAgain() {
  Admission admission;
  TEST_ASSERT_TRUE(admission.admit(Event::kSettingsChanged));
  admission.release(Event::kSettingsChanged);
  TEST_ASSERT_TRUE(admission.admit(Event::kSettingsChanged));
}

void testStateEventsAreIndependent() {
  Admission admission;
  for (Event event : kStateEvents) TEST_ASSERT_TRUE(admission.admit(event));
  admission.release(Event::kNetworkState);
  TEST_ASSERT_FALSE(admission.admit(Event::kSettingsChanged));
  TEST_ASSERT_TRUE(admission.admit(Event::kNetworkState));
  TEST_ASSERT_FALSE(admission.admit(Event::kDataUpdated));
  TEST_ASSERT_FALSE(admission.admit(Event::kOtaState));
  TEST_ASSERT_FALSE(admission.admit(Event::kTimeChanged));
}

void testUiSlotsAreBounded() {
  Admission admission;
  for (std::size_t i = 0; i < cfg::kEventUiActionSlots; ++i) {
    TEST_ASSERT_TRUE(admission.admit(Event::kUiAction));
  }
  TEST_ASSERT_FALSE(admission.admit(Event::kUiAction));
  admission.release(Event::kUiAction);
  TEST_ASSERT_TRUE(admission.admit(Event::kUiAction));
  TEST_ASSERT_FALSE(admission.admit(Event::kUiAction));
}

void testUnmatchedUiReleaseKeepsBound() {
  Admission admission;
  admission.release(Event::kUiAction);
  for (std::size_t i = 0; i < cfg::kEventUiActionSlots; ++i) {
    TEST_ASSERT_TRUE(admission.admit(Event::kUiAction));
  }
  TEST_ASSERT_FALSE(admission.admit(Event::kUiAction));
}

void testStateEventsFindRoomWithFullUiSlots() {
  // Every admitted event is one queue entry; with all UI slots taken each
  // state kind can still be admitted, and the total never exceeds the queue.
  Admission admission;
  std::size_t admitted = 0;
  for (std::size_t i = 0; i < 2 * cfg::kEventUiActionSlots; ++i) {
    if (admission.admit(Event::kUiAction)) ++admitted;
  }
  for (int round = 0; round < 3; ++round) {
    for (Event event : kStateEvents) {
      if (admission.admit(event)) ++admitted;
    }
  }
  TEST_ASSERT_EQUAL_UINT(kQueueLength, admitted);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testCatalog);
  RUN_TEST(testEventNamesAreDistinct);
  RUN_TEST(testStateEventCoalesces);
  RUN_TEST(testReleaseAllowsStateEventAgain);
  RUN_TEST(testStateEventsAreIndependent);
  RUN_TEST(testUiSlotsAreBounded);
  RUN_TEST(testUnmatchedUiReleaseKeepsBound);
  RUN_TEST(testStateEventsFindRoomWithFullUiSlots);
  return UNITY_END();
}
