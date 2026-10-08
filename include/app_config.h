// Single software configuration (ADR-007): every software default, limit
// and enum lives here, nowhere else. Hardware facts live in the hardware
// profiles (boards/, displays/, targets/), secrets in include/secrets.h.
//
// Currently boot-layout and debug defaults; the full settings model (types, limits,
// schema version) follows.

#pragma once

#include <cstdint>

namespace cfg {

// ---- Boot layout -----------------------------------------------------------
// Relative to the shorter display side; these are compile-time defaults.
inline constexpr uint16_t kContentMarginDivisor = 60;  // 240 px: 4 px
inline constexpr int32_t kBootRingWidthDivisor = 120;  // 240 px: 2 px
inline constexpr int32_t kBootRingGapDivisor = 40;     // 240 px: 6 px
inline constexpr int32_t kBootTextWidthPercent = 70;

// ---- Debug -----------------------------------------------------------------
// Periodic status log (free heap, largest block, low-water mark, LVGL pool).
// Sent to the console today; routed through the planned debug helper later
// and then switchable at runtime as a debug setting.
inline constexpr bool kDebugStatusLog = true;
inline constexpr uint32_t kDebugStatusIntervalS = 30;

}  // namespace cfg
