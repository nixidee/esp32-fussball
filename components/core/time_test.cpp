// Device test of the time service, selected by cfg::kTimeTest
// (app_config.h). Expected results: docs/NETWORK.md, "Time". One boot: the
// wall clock must be invalid at boot; then every zone rule is checked at its
// 2026-2027 transitions, a night window across midnight and the
// daylight-saving changes, and two clock jumps. Afterwards the wall clock
// stays at the test time (valid, 2030) until the next boot.

#include <atomic>
#include <cstdlib>
#include <iterator>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "time_service.h"
#include "time_service_internal.h"

namespace timekeeping {

namespace {

constexpr const char* kTag = "time_test";

using cfg::TimeTest;

// UTC offset (seconds) just before and at a UTC instant. Generated from the
// IANA tzdata 2026c (Python zoneinfo): every transition in 2026-2027, and
// one mid-2026 instant for zones without daylight saving.
struct RuleCase {
  const char* label;
  time_t utc;
  int32_t before_s;
  int32_t after_s;
};
constexpr RuleCase kRuleCases[] = {
    {"Africa/Cairo", 1776981600, 7200, 10800},
    {"Africa/Cairo", 1793307600, 10800, 7200},
    {"Africa/Cairo", 1809036000, 7200, 10800},
    {"Africa/Cairo", 1824757200, 10800, 7200},
    {"Africa/Johannesburg", 1782864000, 7200, 7200},
    {"Africa/Lagos", 1782864000, 3600, 3600},
    {"America/Anchorage", 1772967600, -32400, -28800},
    {"America/Anchorage", 1793527200, -28800, -32400},
    {"America/Anchorage", 1805022000, -32400, -28800},
    {"America/Anchorage", 1825581600, -28800, -32400},
    {"America/Chicago", 1772956800, -21600, -18000},
    {"America/Chicago", 1793516400, -18000, -21600},
    {"America/Chicago", 1805011200, -21600, -18000},
    {"America/Chicago", 1825570800, -18000, -21600},
    {"America/Denver", 1772960400, -25200, -21600},
    {"America/Denver", 1793520000, -21600, -25200},
    {"America/Denver", 1805014800, -25200, -21600},
    {"America/Denver", 1825574400, -21600, -25200},
    {"America/Los_Angeles", 1772964000, -28800, -25200},
    {"America/Los_Angeles", 1793523600, -25200, -28800},
    {"America/Los_Angeles", 1805018400, -28800, -25200},
    {"America/Los_Angeles", 1825578000, -25200, -28800},
    {"America/Mexico_City", 1782864000, -21600, -21600},
    {"America/New_York", 1772953200, -18000, -14400},
    {"America/New_York", 1793512800, -14400, -18000},
    {"America/New_York", 1805007600, -18000, -14400},
    {"America/New_York", 1825567200, -14400, -18000},
    {"America/Phoenix", 1782864000, -25200, -25200},
    {"America/Sao_Paulo", 1782864000, -10800, -10800},
    {"Asia/Bangkok", 1782864000, 25200, 25200},
    {"Asia/Dubai", 1782864000, 14400, 14400},
    {"Asia/Kolkata", 1782864000, 19800, 19800},
    {"Asia/Shanghai", 1782864000, 28800, 28800},
    {"Asia/Singapore", 1782864000, 28800, 28800},
    {"Asia/Tokyo", 1782864000, 32400, 32400},
    {"Australia/Perth", 1782864000, 28800, 28800},
    {"Australia/Sydney", 1775318400, 39600, 36000},
    {"Australia/Sydney", 1791043200, 36000, 39600},
    {"Australia/Sydney", 1806768000, 39600, 36000},
    {"Australia/Sydney", 1822492800, 36000, 39600},
    {"Europe/Amsterdam", 1774746000, 3600, 7200},
    {"Europe/Amsterdam", 1792890000, 7200, 3600},
    {"Europe/Amsterdam", 1806195600, 3600, 7200},
    {"Europe/Amsterdam", 1824944400, 7200, 3600},
    {"Europe/Athens", 1774746000, 7200, 10800},
    {"Europe/Athens", 1792890000, 10800, 7200},
    {"Europe/Athens", 1806195600, 7200, 10800},
    {"Europe/Athens", 1824944400, 10800, 7200},
    {"Europe/Berlin", 1774746000, 3600, 7200},
    {"Europe/Berlin", 1792890000, 7200, 3600},
    {"Europe/Berlin", 1806195600, 3600, 7200},
    {"Europe/Berlin", 1824944400, 7200, 3600},
    {"Europe/Dublin", 1774746000, 0, 3600},
    {"Europe/Dublin", 1792890000, 3600, 0},
    {"Europe/Dublin", 1806195600, 0, 3600},
    {"Europe/Dublin", 1824944400, 3600, 0},
    {"Europe/Helsinki", 1774746000, 7200, 10800},
    {"Europe/Helsinki", 1792890000, 10800, 7200},
    {"Europe/Helsinki", 1806195600, 7200, 10800},
    {"Europe/Helsinki", 1824944400, 10800, 7200},
    {"Europe/Istanbul", 1782864000, 10800, 10800},
    {"Europe/Kyiv", 1774746000, 7200, 10800},
    {"Europe/Kyiv", 1792890000, 10800, 7200},
    {"Europe/Kyiv", 1806195600, 7200, 10800},
    {"Europe/Kyiv", 1824944400, 10800, 7200},
    {"Europe/Lisbon", 1774746000, 0, 3600},
    {"Europe/Lisbon", 1792890000, 3600, 0},
    {"Europe/Lisbon", 1806195600, 0, 3600},
    {"Europe/Lisbon", 1824944400, 3600, 0},
    {"Europe/London", 1774746000, 0, 3600},
    {"Europe/London", 1792890000, 3600, 0},
    {"Europe/London", 1806195600, 0, 3600},
    {"Europe/London", 1824944400, 3600, 0},
    {"Europe/Madrid", 1774746000, 3600, 7200},
    {"Europe/Madrid", 1792890000, 7200, 3600},
    {"Europe/Madrid", 1806195600, 3600, 7200},
    {"Europe/Madrid", 1824944400, 7200, 3600},
    {"Europe/Moscow", 1782864000, 10800, 10800},
    {"Europe/Paris", 1774746000, 3600, 7200},
    {"Europe/Paris", 1792890000, 7200, 3600},
    {"Europe/Paris", 1806195600, 3600, 7200},
    {"Europe/Paris", 1824944400, 7200, 3600},
    {"Europe/Prague", 1774746000, 3600, 7200},
    {"Europe/Prague", 1792890000, 7200, 3600},
    {"Europe/Prague", 1806195600, 3600, 7200},
    {"Europe/Prague", 1824944400, 7200, 3600},
    {"Europe/Rome", 1774746000, 3600, 7200},
    {"Europe/Rome", 1792890000, 7200, 3600},
    {"Europe/Rome", 1806195600, 3600, 7200},
    {"Europe/Rome", 1824944400, 7200, 3600},
    {"Europe/Stockholm", 1774746000, 3600, 7200},
    {"Europe/Stockholm", 1792890000, 7200, 3600},
    {"Europe/Stockholm", 1806195600, 3600, 7200},
    {"Europe/Stockholm", 1824944400, 7200, 3600},
    {"Europe/Vienna", 1774746000, 3600, 7200},
    {"Europe/Vienna", 1792890000, 7200, 3600},
    {"Europe/Vienna", 1806195600, 3600, 7200},
    {"Europe/Vienna", 1824944400, 7200, 3600},
    {"Europe/Warsaw", 1774746000, 3600, 7200},
    {"Europe/Warsaw", 1792890000, 7200, 3600},
    {"Europe/Warsaw", 1806195600, 3600, 7200},
    {"Europe/Warsaw", 1824944400, 7200, 3600},
    {"Europe/Zurich", 1774746000, 3600, 7200},
    {"Europe/Zurich", 1792890000, 7200, 3600},
    {"Europe/Zurich", 1806195600, 3600, 7200},
    {"Europe/Zurich", 1824944400, 7200, 3600},
    {"Pacific/Auckland", 1775311200, 46800, 43200},
    {"Pacific/Auckland", 1790431200, 43200, 46800},
    {"Pacific/Auckland", 1806760800, 46800, 43200},
    {"Pacific/Auckland", 1821880800, 43200, 46800},
    {"Pacific/Honolulu", 1782864000, -36000, -36000},
    {"UTC", 1782864000, 0, 0},
};

// Night window 23:00-07:00 in Europe/Berlin: the evening and morning edges,
// both daylight-saving changes (inside the window) and the morning edge
// after the spring change (07:00 CEST = 05:00 UTC).
constexpr DailyWindow kNight{23 * 60, 7 * 60};
struct WindowCase {
  time_t utc;
  uint16_t minute;
  bool daylight;
  bool inside;
};
constexpr WindowCase kNightCases[] = {
    {1791579570, 22 * 60 + 59, true, false},  // 2026-10-09 22:59:30 CEST
    {1791579600, 23 * 60, true, true},        // 2026-10-09 23:00:00 CEST
    {1791583199, 23 * 60 + 59, true, true},   // 2026-10-09 23:59:59 CEST
    {1791583200, 0, true, true},              // 2026-10-10 00:00:00 CEST
    {1791608399, 6 * 60 + 59, true, true},    // 2026-10-10 06:59:59 CEST
    {1791608400, 7 * 60, true, false},        // 2026-10-10 07:00:00 CEST
    {1792889999, 2 * 60 + 59, true, true},    // 2026-10-25 02:59:59 CEST
    {1792890000, 2 * 60, false, true},        // 2026-10-25 02:00:00 CET
    {1806195599, 1 * 60 + 59, false, true},   // 2027-03-28 01:59:59 CET
    {1806195600, 3 * 60, true, true},         // 2027-03-28 03:00:00 CEST
    {1774760399, 6 * 60 + 59, true, true},    // 2026-03-29 06:59:59 CEST
    {1774760400, 7 * 60, true, false},        // 2026-03-29 07:00:00 CEST
};

constexpr time_t kJumpBackUtc = 1577836800;     // 2020-01-01 00:00:00 UTC
constexpr time_t kJumpForwardUtc = 1893456000;  // 2030-01-01 00:00:00 UTC
// Allowed difference between the set and the read wall clock.
constexpr int64_t kJumpToleranceS = 2;
// A setTime() call must not move the monotonic clock by more than this.
constexpr int64_t kMonotonicToleranceMs = 1000;
constexpr int64_t kEventWaitMs = 200;
constexpr TickType_t kEventPollTicks = pdMS_TO_TICKS(10);

std::atomic<uint32_t> time_events{0};

void onTimeChanged(events::Event, uint32_t, void*) {
  time_events.fetch_add(1, std::memory_order_relaxed);
}

std::size_t freeHeap() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Days since 1970-01-01 of a proleptic Gregorian date (month 1..12).
int64_t daysFromCivil(int64_t year, int month, int day) {
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t year_of_era = year - era * 400;
  const int64_t day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

// Local minus UTC in seconds at a UTC instant, in the applied zone.
int64_t offsetAt(time_t utc) {
  std::tm local{};
  toLocal(utc, local);
  const int64_t local_s =
      daysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) *
          86400 +
      local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
  return local_s - utc;
}

const cfg::TimeZone* zoneOrLog(const char* label) {
  const cfg::TimeZone* zone = findZone(label);
  if (zone == nullptr) {
    ESP_LOGE(kTag, "zone %s not in cfg::kTimeZones", label);
  } else if (!internal::applyZone(*zone)) {
    ESP_LOGE(kTag, "zone %s not applied", label);
    zone = nullptr;
  }
  return zone;
}

uint32_t checkBoot() {
  time_t utc = 0;
  std::tm local{};
  const bool invalid = !status().valid && !utcNow(utc) && !localNow(local);
  if (!invalid) ESP_LOGE(kTag, "wall clock valid at boot");
  return invalid ? 0 : 1;
}

uint32_t checkRules() {
  uint32_t failures = 0;
  for (const RuleCase& c : kRuleCases) {
    if (zoneOrLog(c.label) == nullptr) {
      ++failures;
      continue;
    }
    const int64_t before = offsetAt(c.utc - 1);
    const int64_t after = offsetAt(c.utc);
    if (before != c.before_s || after != c.after_s) {
      ESP_LOGE(kTag, "%s at %lld: offset %lld/%lld s, expected %ld/%ld s",
               c.label, static_cast<long long>(c.utc),
               static_cast<long long>(before), static_cast<long long>(after),
               static_cast<long>(c.before_s), static_cast<long>(c.after_s));
      ++failures;
    }
  }
  return failures;
}

uint32_t checkJump(time_t target, const char* name, int direction = 0);

uint32_t checkNight() {
  if (zoneOrLog("Europe/Berlin") == nullptr) return 1;
  uint32_t failures = 0;
  for (const WindowCase& c : kNightCases) {
    std::tm local{};
    failures += checkJump(c.utc, "window clock set");
    if (!localNow(local)) {
      ESP_LOGE(kTag, "no local clock in window test");
      ++failures;
      continue;
    }
    const auto minute =
        static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min);
    if (minute != c.minute || (local.tm_isdst > 0) != c.daylight) {
      ESP_LOGE(kTag, "window clock at %lld: minute %u DST %d, expected %u/%d",
               static_cast<long long>(c.utc), static_cast<unsigned>(minute),
               local.tm_isdst, static_cast<unsigned>(c.minute), c.daylight);
      ++failures;
    }
    if (contains(kNight, minute) != c.inside) {
      ESP_LOGE(kTag, "night window at %lld (local %02d:%02d): %s, expected %s",
               static_cast<long long>(c.utc), local.tm_hour, local.tm_min,
               c.inside ? "outside" : "inside",
               c.inside ? "inside" : "outside");
      ++failures;
    }
  }
  return failures;
}

bool eventSince(uint32_t count) {
  for (int64_t waited = 0; waited < kEventWaitMs;
       waited += pdTICKS_TO_MS(kEventPollTicks)) {
    if (time_events.load(std::memory_order_relaxed) != count) return true;
    vTaskDelay(kEventPollTicks);
  }
  return time_events.load(std::memory_order_relaxed) != count;
}

uint32_t checkJump(time_t target, const char* name, int direction) {
  // No other clock producer runs during this test. Yield to the higher-
  // priority events task before taking the baseline, so previous zone
  // notifications cannot satisfy this set. Callbacks only increment a counter.
  vTaskDelay(pdMS_TO_TICKS(kEventWaitMs));
  const uint32_t events_before = time_events.load(std::memory_order_relaxed);
  time_t previous = 0;
  const bool was_valid = utcNow(previous);
  const uint32_t previous_sets = status().set_count;
  const int64_t mono_before = monotonicMs();
  if (setTime({target, 0}, Source::kDeviceTest) != ESP_OK) {
    ESP_LOGE(kTag, "%s: setTime failed", name);
    return 1;
  }
  const int64_t mono_step = monotonicMs() - mono_before;
  uint32_t failures = 0;
  if ((direction < 0 && (!was_valid || target >= previous)) ||
      (direction > 0 && (!was_valid || target <= previous))) {
    ESP_LOGE(kTag, "%s: wrong jump direction from %lld to %lld", name,
             static_cast<long long>(previous), static_cast<long long>(target));
    ++failures;
  }
  time_t now = 0;
  if (!utcNow(now) ||
      llabs(static_cast<int64_t>(now - target)) > kJumpToleranceS) {
    ESP_LOGE(kTag, "%s: wall clock %lld, expected %lld", name,
             static_cast<long long>(now), static_cast<long long>(target));
    ++failures;
  }
  if (mono_step < 0 || mono_step > kMonotonicToleranceMs) {
    ESP_LOGE(kTag, "%s: monotonic clock moved %lld ms", name,
             static_cast<long long>(mono_step));
    ++failures;
  }
  const Status s = status();
  if (!s.valid || s.source != Source::kDeviceTest ||
      s.set_count != previous_sets + 1) {
    ESP_LOGE(kTag, "%s: status valid %d, source %s", name, s.valid,
             sourceName(s.source));
    ++failures;
  }
  if (!eventSince(events_before)) {
    ESP_LOGE(kTag, "%s: no time event", name);
    ++failures;
  }
  return failures;
}

// 2030-01-01 00:00 UTC is 01:00 CET in Berlin.
uint32_t checkLocalNow() {
  std::tm local{};
  if (!localNow(local) || local.tm_year != 130 || local.tm_mon != 0 ||
      local.tm_mday != 1 || local.tm_hour != 1) {
    ESP_LOGE(kTag, "local time after the forward jump is wrong");
    return 1;
  }
  return 0;
}

void runRulesAndJumps() {
  const std::size_t heap_before = freeHeap();
  uint32_t failures = checkBoot();
  if (events::subscribe(events::Event::kTimeChanged, onTimeChanged, nullptr) !=
      ESP_OK) {
    ++failures;
  }
  const int64_t rules_start_ms = monotonicMs();
  failures += checkRules();
  ESP_LOGI(kTag, "%u rule cases in %lld ms",
           static_cast<unsigned>(std::size(kRuleCases)),
           static_cast<long long>(monotonicMs() - rules_start_ms));
  failures += checkNight();
  failures += checkJump(kJumpForwardUtc, "establish 2030 baseline");
  failures += checkJump(kJumpBackUtc, "jump back to 2020", -1);
  failures += checkJump(kJumpForwardUtc, "jump forward to 2030", 1);
  failures += checkLocalNow();
  applySettings();
  if (failures == 0) {
    ESP_LOGI(kTag,
             "PASS: boot invalid, %u rule cases, %u window cases, "
             "2 jumps; heap %u -> %u B",
             static_cast<unsigned>(std::size(kRuleCases)),
             static_cast<unsigned>(std::size(kNightCases)),
             static_cast<unsigned>(heap_before),
             static_cast<unsigned>(freeHeap()));
  } else {
    ESP_LOGE(kTag, "FAIL: %u checks failed", static_cast<unsigned>(failures));
  }
  logStatus();
}

}  // namespace

void runDeviceTest() {
  if constexpr (cfg::kTimeTest != TimeTest::kNone) {
    ESP_LOGW(kTag, "device test %u active (app_config.h kTimeTest)",
             static_cast<unsigned>(cfg::kTimeTest));
    switch (cfg::kTimeTest) {
      case TimeTest::kRulesAndJumps: runRulesAndJumps(); break;
      case TimeTest::kNone: break;
    }
  }
}

}  // namespace timekeeping
