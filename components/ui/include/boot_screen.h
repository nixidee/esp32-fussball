// Boot test screen: shows that panel, colours and geometry are right.
// View only: renders the given texts, no logic (docs/ARCHITECTURE.md).
//
// Expected picture: red ring at the display edge, green ring just inside,
// white title and detail text, version text in blue. Swapped red/blue means
// wrong colour order (DisplayProfile::bgr_order), wrong brightness/colours
// mean wrong inversion (DisplayProfile::invert_colors).

#pragma once

namespace geometry {
class SafeArea;
}

namespace ui {

struct BootScreenModel {
  const char* title;
  const char* detail;
  const char* version;
};

// Replaces the content of the active LVGL screen. The caller holds the
// display lock. Content is fitted to the supplied geometry; diagnostic rings
// retain their physical-display contract. Returns false if the complete text
// cannot fit or an object cannot be created. Internal LVGL allocation recovery
// is handled separately from this view's geometry contract.
bool showBootScreen(const BootScreenModel& model,
                    const geometry::SafeArea& safe_area);

}  // namespace ui
