// Single software configuration (ADR-007): every software default, limit
// and enum lives here, nowhere else. Hardware facts live in the hardware
// profiles (boards/, displays/, targets/), secrets in include/secrets.h.
//
// Currently only the debug group; the full settings model (types, limits,
// schema version) follows.

#pragma once

#include <cstdint>

namespace cfg {

// ---- Debug -----------------------------------------------------------------
// Periodic status log (free heap, largest block, low-water mark, LVGL pool).
// Sent to the console today; routed through the planned debug helper later
// and then switchable at runtime as a debug setting.
inline constexpr bool kDebugStatusLog = true;
inline constexpr uint32_t kDebugStatusIntervalS = 30;

}  // namespace cfg
