// Boot test screen: shows that panel, colours and geometry are right.
// View only: renders the given texts, no logic (docs/ARCHITECTURE.md).
//
// Expected picture: red ring at the display edge, green ring just inside,
// white title and detail text, version text in blue. Swapped red/blue means
// wrong colour order (DisplayProfile::bgr_order), wrong brightness/colours
// mean wrong inversion (DisplayProfile::invert_colors).

#pragma once

namespace ui {

struct BootScreenModel {
  const char* title;
  const char* detail;
  const char* version;
};

// Replaces the content of the active LVGL screen. The caller holds the
// display lock.
void showBootScreen(const BootScreenModel& model);

}  // namespace ui
