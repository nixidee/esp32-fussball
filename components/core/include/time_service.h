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
};

// Applies the zone from the settings. Call once at boot after events::init()
// and settings::init(), before any other function here.
void init();

// Applies the settings' zone if it differs from the applied one; posts
// kTimeChanged then. Call from the app task after kSettingsChanged, never
// from a bus callback: it copies the settings (328 B of stack on the C6) and
// the C library parses the rule.
void applySettings();

// Sets the wall clock (UTC) and marks it valid; logs the first set and every
// jump above cfg::kTimeJumpLogThresholdS. ESP_FAIL: the clock refused the
// value, nothing changed.
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

// Milliseconds since boot, unaffected by wall-clock changes: use it for
// delays, retries, deadlines, freshness and input/overlay timeouts.
int64_t monotonicMs();

// Logs validity, source, local time and zone.
void logStatus();

// Runs the device test selected by cfg::kTimeTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace timekeeping
