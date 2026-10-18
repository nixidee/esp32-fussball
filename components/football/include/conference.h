// Konferenz rules and page request (docs/UI.md, "Konferenz"), used by the
// store views (store_views.h) and the UI. Pure: no clock, settings store,
// hardware or LVGL.
#pragma once
#include <array>
#include <cstdint>

#include "app_config.h"

namespace football {
inline constexpr std::size_t kConferenceRowsMax = 9;
struct ConferenceRequest {
  cfg::ConferenceMode mode = cfg::kConferenceModeDefault;
  bool pin_own = cfg::kPinOwnMatchDefault;
  // Kickoff-order paging by the user, in either mode.
  bool manual = false;
  uint8_t per_page = cfg::kVisibleMatchesDefault;
  // Wrapped by the page count when automatic, clamped when manual.
  unsigned page = 0;
};
struct ConferencePage {
  std::array<uint16_t, kConferenceRowsMax> rows{};  // store indices
  uint8_t row_count = 0;
  uint16_t page = 0, pages = 0;
};
}  // namespace football
