// Boot test screen. See boot_screen.h.

#include "boot_screen.h"

#include <algorithm>
#include <cstdint>

#include "lvgl.h"

namespace ui {
namespace {

// Pure colours, so a red/blue swap is obvious.
constexpr uint32_t kRed = 0xFF0000;
constexpr uint32_t kGreen = 0x00FF00;
constexpr uint32_t kBlue = 0x0000FF;
constexpr uint32_t kWhite = 0xFFFFFF;

// Geometry relative to the shorter display side, no fixed pixel values.
constexpr int32_t kRingWidthDivisor = 120;   // 240 px: 2 px ring
constexpr int32_t kRingGapDivisor = 40;      // 240 px: 6 px between rings
constexpr int32_t kTextWidthPercent = 70;    // text block width

lv_obj_t* addRing(lv_obj_t* parent, int32_t diameter, int32_t width,
                  uint32_t color) {
  lv_obj_t* ring = lv_obj_create(parent);
  lv_obj_remove_style_all(ring);
  lv_obj_set_size(ring, diameter, diameter);
  lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, width, 0);
  lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
  return ring;
}

void addLabel(lv_obj_t* parent, const char* text, int32_t width,
              uint32_t color) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_label_set_text(label, text);
}

}  // namespace

void showBootScreen(const BootScreenModel& model) {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  const int32_t size =
      std::min(lv_display_get_horizontal_resolution(nullptr),
               lv_display_get_vertical_resolution(nullptr));
  const int32_t ring_width = std::max<int32_t>(1, size / kRingWidthDivisor);
  const int32_t ring_gap = std::max<int32_t>(2, size / kRingGapDivisor);

  addRing(screen, size, ring_width, kRed);
  addRing(screen, size - 2 * (ring_width + ring_gap), ring_width, kGreen);

  lv_obj_t* text = lv_obj_create(screen);
  lv_obj_remove_style_all(text);
  lv_obj_set_size(text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(text, ring_gap, 0);
  lv_obj_center(text);

  const int32_t text_width = size * kTextWidthPercent / 100;
  addLabel(text, model.title, text_width, kWhite);
  addLabel(text, model.detail, text_width, kWhite);
  addLabel(text, model.version, text_width, kBlue);
}

}  // namespace ui
