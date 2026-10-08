// Boot test screen. See boot_screen.h.

#include "boot_screen.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "app_config.h"
#include "lvgl.h"
#include "safe_area.h"

namespace ui {
namespace {

// Pure colours, so a red/blue swap is obvious.
constexpr uint32_t kRed = 0xFF0000;
constexpr uint32_t kGreen = 0x00FF00;
constexpr uint32_t kBlue = 0x0000FF;
constexpr uint32_t kWhite = 0xFFFFFF;

lv_obj_t* addRing(lv_obj_t* parent, int32_t diameter, int32_t width,
                  uint32_t color) {
  if (diameter <= 0 || width <= 0) return nullptr;
  lv_obj_t* ring = lv_obj_create(parent);
  if (ring == nullptr) return nullptr;
  lv_obj_remove_style_all(ring);
  lv_obj_set_size(ring, diameter, diameter);
  lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, width, 0);
  lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
  return ring;
}

lv_obj_t* addLabel(lv_obj_t* parent, const char* text, int32_t width,
                   uint32_t color) {
  lv_obj_t* label = lv_label_create(parent);
  if (label == nullptr) return nullptr;
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_label_set_text(label, text);
  return label;
}

}  // namespace

bool showBootScreen(const BootScreenModel& model,
                    const geometry::SafeArea& safe_area) {
  if (!safe_area.isValid()) return false;
  lv_obj_t* screen = lv_screen_active();
  if (screen == nullptr) return false;
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  const int32_t width = lv_display_get_horizontal_resolution(nullptr);
  const int32_t height = lv_display_get_vertical_resolution(nullptr);
  const int32_t size = std::min(width, height);
  const int32_t ring_width =
      std::max<int32_t>(1, size / cfg::kBootRingWidthDivisor);
  const int32_t ring_gap =
      std::max<int32_t>(2, size / cfg::kBootRingGapDivisor);

  if (addRing(screen, size, ring_width, kRed) == nullptr ||
      addRing(screen, size - 2 * (ring_width + ring_gap), ring_width, kGreen) ==
          nullptr) {
    return false;
  }

  lv_obj_t* text = lv_obj_create(screen);
  if (text == nullptr) return false;
  lv_obj_remove_style_all(text);
  lv_obj_set_size(text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(text, ring_gap, 0);
  lv_obj_center(text);

  int32_t text_width = size * cfg::kBootTextWidthPercent / 100;
  const std::array<lv_obj_t*, 3> labels{
      addLabel(text, model.title, text_width, kWhite),
      addLabel(text, model.detail, text_width, kWhite),
      addLabel(text, model.version, text_width, kBlue),
  };
  for (lv_obj_t* label : labels) {
    if (label == nullptr) {
      lv_obj_delete(text);
      return false;
    }
  }

  // Wrapped text can be taller than one chord. Reflow against the entire
  // measured band, checking its corners after each strictly smaller width.
  // Diagnostics and the screen background are deliberately not content.
  while (true) {
    lv_obj_update_layout(text);
    lv_area_t bounds;
    lv_obj_get_coords(text, &bounds);
    const geometry::Rect content{
        bounds.x1, bounds.y1, lv_area_get_width(&bounds),
        lv_area_get_height(&bounds)};
    if (safe_area.contains(content)) return true;
    const geometry::HorizontalSpan span =
        safe_area.spanForBand(content.y, content.height);
    if (span.width <= 0 || span.width >= text_width) {
      // Keep diagnostics, but never present a clipped/truncated success.
      lv_obj_delete(text);
      return false;
    }
    text_width = span.width;
    for (lv_obj_t* label : labels) lv_obj_set_width(label, text_width);
  }
}

}  // namespace ui
