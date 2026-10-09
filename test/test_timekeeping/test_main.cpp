// Host acceptance for the time-zone table and the daily windows of the time
// service (docs/NETWORK.md "Time"). Every POSIX rule is compared with the
// host's IANA database; the device test (cfg::kTimeTest) checks the same
// rules with the firmware's C library.

#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "app_config.h"
#include "civil_time.h"

namespace {

using timekeeping::contains;
using timekeeping::DailyWindow;
using timekeeping::findZone;

constexpr time_t kCompareFromUtc = 1767225600;  // 2026-01-01 00:00:00 UTC
constexpr time_t kCompareToUtc = 1861920000;    // 2029-01-01 00:00:00 UTC
constexpr time_t kHourS = 3600;

void setZone(const char* tz) {
  setenv("TZ", tz, 1);
  tzset();
}

// UTC offset at the start and the last second of every hour in the compare
// range, in the zone set by setZone().
std::vector<long> offsets() {
  std::vector<long> result;
  for (time_t t = kCompareFromUtc; t < kCompareToUtc; t += kHourS) {
    for (const time_t at : {t, t + kHourS - 1}) {
      std::tm local{};
      localtime_r(&at, &local);
      result.push_back(local.tm_gmtoff);
    }
  }
  return result;
}

int minuteAt(time_t utc) {
  std::tm local{};
  localtime_r(&utc, &local);
  return local.tm_hour * 60 + local.tm_min;
}

void testFindZone() {
  const cfg::TimeZone* berlin = findZone("Europe/Berlin");
  TEST_ASSERT_NOT_NULL(berlin);
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", berlin->rule);
  TEST_ASSERT_NOT_NULL(findZone("UTC"));
  TEST_ASSERT_NULL(findZone(""));
  TEST_ASSERT_NULL(findZone("europe/berlin"));
  TEST_ASSERT_NULL(findZone("Europe/Berli"));
  TEST_ASSERT_NULL(findZone("Europe/Berlin "));
  TEST_ASSERT_NULL(findZone("CET-1CEST,M3.5.0,M10.5.0/3"));
  // A label is matched by its length, not by a terminating zero.
  TEST_ASSERT_NULL(findZone(std::string_view("Europe/Berlin\0x", 15)));
}

void testTableEntries() {
  TEST_ASSERT_NOT_NULL(findZone(cfg::kTimeZoneDefault));
  for (const cfg::TimeZone& zone : cfg::kTimeZones) {
    const std::size_t length = std::strlen(zone.label);
    TEST_ASSERT_TRUE(length > 0);
    TEST_ASSERT_TRUE(length <= cfg::kTimeZoneLabelMaxChars);
    TEST_ASSERT_TRUE(std::strlen(zone.rule) > 0);
    // Unique: the lookup finds this very entry.
    TEST_ASSERT_EQUAL_PTR(&zone, findZone(zone.label));
  }
}

void testRulesMatchIanaDatabase() {
  for (const cfg::TimeZone& zone : cfg::kTimeZones) {
    setZone(zone.rule);
    const std::vector<long> from_rule = offsets();
    setZone(zone.label);
    const std::vector<long> from_database = offsets();
    for (std::size_t i = 0; i < from_rule.size(); ++i) {
      if (from_rule[i] != from_database[i]) {
        const std::string message = std::string(zone.label) + " sample " +
                                    std::to_string(i) + ": rule " +
                                    std::to_string(from_rule[i]) + " s, IANA " +
                                    std::to_string(from_database[i]) + " s";
        TEST_FAIL_MESSAGE(message.c_str());
      }
    }
  }
  setZone("UTC0");
}

void testDayWindow() {
  constexpr DailyWindow kDay{7 * 60, 23 * 60};
  TEST_ASSERT_FALSE(contains(kDay, 0));
  TEST_ASSERT_FALSE(contains(kDay, 7 * 60 - 1));
  TEST_ASSERT_TRUE(contains(kDay, 7 * 60));
  TEST_ASSERT_TRUE(contains(kDay, 23 * 60 - 1));
  TEST_ASSERT_FALSE(contains(kDay, 23 * 60));
  TEST_ASSERT_FALSE(contains(kDay, 1439));
}

void testWindowAcrossMidnight() {
  constexpr DailyWindow kNight{23 * 60, 7 * 60};
  TEST_ASSERT_FALSE(contains(kNight, 23 * 60 - 1));
  TEST_ASSERT_TRUE(contains(kNight, 23 * 60));
  TEST_ASSERT_TRUE(contains(kNight, 1439));
  TEST_ASSERT_TRUE(contains(kNight, 0));
  TEST_ASSERT_TRUE(contains(kNight, 7 * 60 - 1));
  TEST_ASSERT_FALSE(contains(kNight, 7 * 60));
  TEST_ASSERT_FALSE(contains(kNight, 12 * 60));
}

void testEmptyWindowAndInvalidMinutes() {
  constexpr DailyWindow kEmpty{600, 600};
  for (uint16_t minute = 0; minute < timekeeping::kMinutesPerDay; ++minute) {
    TEST_ASSERT_FALSE(contains(kEmpty, minute));
  }
  constexpr DailyWindow kNight{23 * 60, 7 * 60};
  TEST_ASSERT_FALSE(contains(kNight, timekeeping::kMinutesPerDay));
  TEST_ASSERT_FALSE(contains(kNight, 0xFFFF));
}

void testNightWindowAcrossDstChanges() {
  // The window follows the local clock (Europe/Berlin rule).
  setZone(findZone("Europe/Berlin")->rule);
  constexpr DailyWindow kNight{23 * 60, 7 * 60};
  struct Case {
    time_t utc;
    int minute;
    bool inside;
  };
  const Case cases[] = {
      {1791579570, 22 * 60 + 59, false},  // 2026-10-09 20:59:30 UTC
      {1791579600, 23 * 60, true},        // 2026-10-09 21:00:00 UTC
      {1791608399, 6 * 60 + 59, true},    // 2026-10-10 04:59:59 UTC
      {1791608400, 7 * 60, false},        // 2026-10-10 05:00:00 UTC
      {1792889999, 2 * 60 + 59, true},    // CEST ends: 02:59:59 CEST
      {1792890000, 2 * 60, true},         // then 02:00:00 CET
      {1795672799, 6 * 60 + 59, true},    // 2026-11-26 05:59:59 UTC (CET)
      {1795672800, 7 * 60, false},        // 2026-11-26 06:00:00 UTC
      {1806195599, 1 * 60 + 59, true},    // CEST starts: 01:59:59 CET
      {1806195600, 3 * 60, true},         // then 03:00:00 CEST
      {1774760399, 6 * 60 + 59, true},    // 2026-03-29 04:59:59 UTC (CEST)
      {1774760400, 7 * 60, false},        // 2026-03-29 05:00:00 UTC
  };
  for (const Case& c : cases) {
    const int minute = minuteAt(c.utc);
    TEST_ASSERT_EQUAL_INT(c.minute, minute);
    TEST_ASSERT_EQUAL(c.inside,
                      contains(kNight, static_cast<uint16_t>(minute)));
  }
  setZone("UTC0");
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testFindZone);
  RUN_TEST(testTableEntries);
  RUN_TEST(testRulesMatchIanaDatabase);
  RUN_TEST(testDayWindow);
  RUN_TEST(testWindowAcrossMidnight);
  RUN_TEST(testEmptyWindowAndInvalidMinutes);
  RUN_TEST(testNightWindowAcrossDstChanges);
  return UNITY_END();
}
