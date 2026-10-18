#include "civil_time.h"

namespace timekeeping {

const cfg::TimeZone* findZone(std::string_view label) noexcept {
  for (const cfg::TimeZone& zone : cfg::kTimeZones) {
    if (label == zone.label) return &zone;
  }
  return nullptr;
}

bool validCivilDate(int year, unsigned month, unsigned day) noexcept {
  constexpr unsigned kMonthDays[] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12 || day < 1) return false;
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day <= kMonthDays[month - 1] + (month == 2 && leap);
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) noexcept {
  const int64_t adjusted = static_cast<int64_t>(year) - (month <= 2);
  const int64_t era = (adjusted >= 0 ? adjusted : adjusted - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(adjusted - era * 400);
  const unsigned day_of_year =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

bool contains(DailyWindow window, uint16_t minute_of_day) noexcept {
  if (minute_of_day >= kMinutesPerDay) return false;
  const uint16_t start = window.start_minute;
  const uint16_t end = window.end_minute;
  if (start <= end) return start <= minute_of_day && minute_of_day < end;
  return minute_of_day >= start || minute_of_day < end;
}

}  // namespace timekeeping
