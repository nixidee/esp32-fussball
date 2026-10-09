#include "civil_time.h"

namespace timekeeping {

const cfg::TimeZone* findZone(std::string_view label) noexcept {
  for (const cfg::TimeZone& zone : cfg::kTimeZones) {
    if (label == zone.label) return &zone;
  }
  return nullptr;
}

bool contains(DailyWindow window, uint16_t minute_of_day) noexcept {
  if (minute_of_day >= kMinutesPerDay) return false;
  const uint16_t start = window.start_minute;
  const uint16_t end = window.end_minute;
  if (start <= end) return start <= minute_of_day && minute_of_day < end;
  return minute_of_day >= start || minute_of_day < end;
}

}  // namespace timekeeping
