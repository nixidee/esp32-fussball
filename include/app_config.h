// Single software configuration (ADR-007): every software default, limit
// and enum lives here, nowhere else. Hardware facts live in the hardware
// profiles (boards/, displays/, targets/), secrets in include/secrets.h.
//
// Currently boot-layout, display failure and debug defaults; the full settings
// model (types, limits, schema version) follows.

#pragma once

#include <cstdint>

namespace cfg {

// ---- Boot layout -----------------------------------------------------------
// Relative to the shorter display side; these are compile-time defaults.
inline constexpr uint16_t kContentMarginDivisor = 60;  // 240 px: 4 px
inline constexpr int32_t kBootRingWidthDivisor = 120;  // 240 px: 2 px
inline constexpr int32_t kBootRingGapDivisor = 40;     // 240 px: 6 px
inline constexpr int32_t kBootTextWidthPercent = 70;

// ---- Display failure handling ----------------------------------------------
// Consecutive failed draw calls (each one recovered) before a restart.
inline constexpr uint32_t kDisplayDrawFailureLimit = 3;
// LVGL supervision timer: feeds the task watchdog user and redraws after a
// recovered draw failure. Must stay well below the task watchdog timeout.
inline constexpr uint32_t kDisplaySupervisionPeriodMs = 500;
// Running this long after display init resets the abnormal-reset counter.
inline constexpr uint32_t kDisplayStablePeriodS = 60;
// Consecutive abnormal resets (panic, watchdog) after which the display is
// no longer initialised (headless) until a power cycle or explicit restart.
inline constexpr uint32_t kAbnormalResetLimit = 3;
// LVGL lock timeouts for callers outside the LVGL task.
inline constexpr uint32_t kBootScreenLockTimeoutMs = 1000;
inline constexpr uint32_t kStatusLockTimeoutMs = 10;

// Fault injection for the display failure tests. Must be kNone in every
// normal build; any other value compiles one test fault in.
enum class DisplayFault : uint8_t {
  kNone,
  kDrawFailOnce,       // one draw call fails: UI recovers, no restart
  kDrawFailAlways,     // every draw call fails: restart, then headless bound
  kDrainFail,          // draw and drain fail: immediate restart
  kLostCompletion,     // colour data never sent: watchdog restart
  kPortMemory,         // port allocation pre-check fails: restart
  kLvglPoolExhausted,  // LVGL pool runs out: assert, watchdog restart
  kLockStall,          // LVGL lock held for 10 s: watchdog restart
};
inline constexpr DisplayFault kDisplayFault = DisplayFault::kNone;

// ---- Debug -----------------------------------------------------------------
// Periodic status log (free heap, largest block, low-water mark, LVGL pool).
// Sent to the console today; routed through the planned debug helper later
// and then switchable at runtime as a debug setting.
inline constexpr bool kDebugStatusLog = true;
inline constexpr uint32_t kDebugStatusIntervalS = 30;

}  // namespace cfg
