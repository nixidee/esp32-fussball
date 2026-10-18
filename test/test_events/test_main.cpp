// Host acceptance for the event catalog and the queue admission of the event
// bus: coalesced state notifications with one guaranteed slot per kind.

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
  TEST_ASSERT_FALSE(events::isStateEvent(static_cast<Event>(255)));
  TEST_ASSERT_EQUAL_UINT(kStateEventCount, kQueueLength);
}

void testEventNamesAreDistinct() {
  constexpr Event kAll[] = {Event::kSettingsChanged, Event::kNetworkState,
                            Event::kDataUpdated, Event::kOtaState,
                            Event::kTimeChanged};
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

void testAllStateKindsAlwaysFitAndInvalidKindsDoNotShiftBits() {
  Admission admission;
  TEST_ASSERT_FALSE(admission.admit(static_cast<Event>(255)));
  admission.release(static_cast<Event>(255));
  std::size_t admitted = 0;
  for (unsigned round = 0; round < 3; ++round)
    for (const auto event : kStateEvents) admitted += admission.admit(event);
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
  RUN_TEST(testAllStateKindsAlwaysFitAndInvalidKindsDoNotShiftBits);
  return UNITY_END();
}
