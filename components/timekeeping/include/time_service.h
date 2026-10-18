// Time service (docs/NETWORK.md "Time"): wall-clock validity, the local time
// zone and a monotonic clock. The wall clock counts as valid only after a
// source set it in this boot; until then date-dependent logic waits. Every
// change of validity, wall clock or zone posts events::Event::kTimeChanged.
// Only this service sets the C library's time zone (TZ is process-wide).
// Zone lookup and daily windows: civil_time.h.

#pragma once

#include <sys/time.h>

#include <cstdint>
#include <ctime>

#include "civil_time.h"
#include "esp_err.h"

namespace timekeeping {

// Who set the wall clock last. SNTP is connected in P3.1.
enum class Source : uint8_t {
  kNone,        // not set since boot
  kSntp,        // network time (P3.1)
  kDeviceTest,  // cfg::kTimeTest
};

const char* sourceName(Source source);

struct Status {
  bool valid;
  Source source;
  int64_t set_at_ms;   // monotonicMs() of the last set; 0 while not valid
  uint32_t set_count;  // sets since boot
  // First valid set, zone changes and wall-clock jumps above
  // cfg::kTimeJumpThresholdS since boot: changes that move the clock or the
  // local time noticeably. A regular resync does not count.
  uint32_t step_count;
};

// Call once after events::init(), with a validated immutable configuration
// zone. The time service does not query the configuration service.
void init(const cfg::TimeZone& zone);

// Applies the requested zone if it differs; posts kTimeChanged then. Call
// from the app task. Retains this immutable reference for allocation retries.
void configureZone(const cfg::TimeZone& zone);

// Retries a zone that could not be applied (no memory for the rule) every
// cfg::kTimeZoneRetryMs; the previous zone stays until then. App task.
void poll();

// Sets the wall clock (UTC) and marks it valid; logs and counts the first
// set and every jump above cfg::kTimeJumpThresholdS. ESP_FAIL: the clock
// refused the value, nothing changed.
esp_err_t setTime(const timeval& utc, Source source);

Status status();

// Current UTC; false (out unchanged) while the wall clock is not valid.
bool utcNow(time_t& out);

// Current local civil time in the applied zone; false (out unchanged) while
// the wall clock is not valid.
bool localNow(std::tm& out);

// Converts a UTC instant (e.g. a fixture time) to local civil time in the
// applied zone; works whether or not the wall clock is valid.
void toLocal(time_t utc, std::tm& out);

// Local UTC offset in seconds at the UTC instant 'utc' in the applied zone
// (daylight saving included); works whether or not the wall clock is valid.
int32_t localOffset(time_t utc);

// Milliseconds since boot, unaffected by wall-clock changes: use it for
// delays, retries, deadlines, freshness and input/overlay timeouts.
int64_t monotonicMs();

// Logs validity, source, local time and zone.
void logStatus();

// Runs the device test selected by cfg::kTimeTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace timekeeping
