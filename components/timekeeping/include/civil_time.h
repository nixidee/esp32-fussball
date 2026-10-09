// Pure helpers of the time service (docs/NETWORK.md "Time"): time-zone label
// lookup and daily windows in local civil time. The ESP-IDF part (clock,
// validity, applying the zone) lives in components/core (time_service.h).

#pragma once

#include <cstdint>
#include <string_view>

#include "app_config.h"

namespace timekeeping {

// Entry of cfg::kTimeZones with exactly this label (case-sensitive), or
// nullptr.
const cfg::TimeZone* findZone(std::string_view label) noexcept;

inline constexpr uint16_t kMinutesPerDay = 24 * 60;

// Daily window in local civil time, minutes after midnight (0..1439). The
// start is inside, the end is not. start > end crosses midnight (e.g. night
// 23:00-07:00); start == end is an empty window.
struct DailyWindow {
  uint16_t start_minute;
  uint16_t end_minute;
};

// True if the local minute of day (0..1439) lies in the window. Values of
// 1440 or more are never inside. Around a daylight-saving change the local
// clock skips or repeats an hour; the window follows the local clock.
bool contains(DailyWindow window, uint16_t minute_of_day) noexcept;

}  // namespace timekeeping
