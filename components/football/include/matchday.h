// Pure calendar gap and retained acquisition dates. Screen navigation and
// sporting-round content belong to screen_selection, not a second date walk.
#pragma once

#include <cstddef>
#include <cstdint>

namespace football::matchday {

// True when 'next' starts a new matchday after 'previous' (ascending UTC
// seconds of kickoffs or provider day starts): at least kMatchdayGapHours
// apart.
bool starts(uint32_t previous, uint32_t next);

// Previous/current/next derived date clusters when the provider gives no
// distinct round sections. Overflow is explicit; no partial window is complete.
std::size_t retainedDays(const uint32_t* days, std::size_t count, int64_t utc,
                         uint32_t* out, std::size_t capacity, bool* complete);

}  // namespace football::matchday
