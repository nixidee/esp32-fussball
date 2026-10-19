#include "matchday.h"

#include "app_config.h"
#include "civil_time.h"

namespace football::matchday {
bool starts(uint32_t previous, uint32_t next) {
  return next - previous >= cfg::kMatchdayGapHours * 3600u;
}

std::size_t retainedDays(const uint32_t* days, std::size_t count, int64_t utc,
                         uint32_t* out, std::size_t capacity, bool* complete) {
  const auto last = [&](std::size_t first) {
    while (first + 1 < count && !starts(days[first], days[first + 1])) ++first;
    return first;
  };
  std::size_t current = 0, previous = 0, before_previous = 0;
  bool has_previous = false, has_before_previous = false;
  while (current < count) {
    const auto end = last(current);
    if (static_cast<int64_t>(days[end]) + timekeeping::kSecondsPerDay > utc)
      break;
    before_previous = previous;
    has_before_previous = has_previous;
    previous = current;
    has_previous = true;
    current = end + 1;
  }
  std::size_t at = current == count && has_before_previous ? before_previous
                   : has_previous                          ? previous
                                                           : current;
  std::size_t needed = 0, written = 0;
  for (unsigned block = 0; block < (has_previous ? 3u : 2u) && at < count;
       ++block) {
    const auto end = last(at);
    for (; at <= end; ++at) {
      ++needed;
      if (written < capacity) out[written++] = days[at];
    }
  }
  if (complete) *complete = needed <= capacity;
  return written;
}

}  // namespace football::matchday
