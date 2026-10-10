#include "football_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "lvgl.h"
#include "ui_fonts.h"
namespace ui {
namespace {
lv_obj_t *image = nullptr, *title = nullptr, *score = nullptr,
         *minute = nullptr, *teams = nullptr, *highlight = nullptr,
         *status = nullptr, *badge = nullptr;
lv_obj_t* rows[9]{};
char current_source[64]{};
uint32_t source_revision = UINT32_MAX;
void text(lv_obj_t* label, const char* value) {
  if (strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}
// LVGL 9.6 style setters invalidate the object even when the value is
// unchanged, and a full-screen invalidation redraws the JPEG behind every
// stripe. Apply a style only when it differs from the current value.
void textColour(lv_obj_t* object, lv_color_t value) {
  if (!lv_color_eq(lv_obj_get_style_text_color(object, LV_PART_MAIN), value))
    lv_obj_set_style_text_color(object, value, 0);
}
void bgColour(lv_obj_t* object, lv_color_t value) {
  if (!lv_color_eq(lv_obj_get_style_bg_color(object, LV_PART_MAIN), value))
    lv_obj_set_style_bg_color(object, value, 0);
}
lv_obj_t* label(lv_obj_t* parent) {
  auto* object = lv_label_create(parent);
  if (object == nullptr) return nullptr;
  lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(object, LV_LABEL_LONG_MODE_DOTS);
  return object;
}
bool band(lv_obj_t* object, const geometry::SafeArea& area, int y, int height,
          const char* value, uint32_t colour, int desired = 14,
          bool score_role = false) {
  const auto span = area.spanForBand(y, height);
  if (span.width <= 0) return false;
  const unsigned lines =
      1 + static_cast<unsigned>(std::count(value, value + strlen(value), '\n'));
  lv_obj_set_pos(object, span.x, y);
  lv_obj_set_size(object, span.width, height);
  const lv_font_t* font = &ui_font_text_10;
  if (desired >= 14 &&
      height >= static_cast<int>(ui_font_text_14.line_height * lines))
    font = &ui_font_text_14;
  if (desired >= 18 &&
      height >= static_cast<int>(ui_font_text_18.line_height * lines))
    font = &ui_font_text_18;
  if (score_role && strspn(value, " 0123456789:-") == strlen(value) &&
      height >= ui_font_score_28.line_height)
    font = &ui_font_score_28;
  if (lv_obj_get_style_text_font(object, LV_PART_MAIN) != font)
    lv_obj_set_style_text_font(object, font, 0);
  const int32_t pad =
      std::max(0, (height - static_cast<int>(font->line_height * lines)) / 2);
  if (lv_obj_get_style_pad_top(object, LV_PART_MAIN) != pad)
    lv_obj_set_style_pad_top(object, pad, 0);
  textColour(object, lv_color_hex(colour));
  text(object, value);
  return area.contains({span.x, y, span.width, height});
}
}  // namespace
bool render(const ViewModel& m, const geometry::SafeArea& area) {
  auto* root = lv_screen_active();
  if (root == nullptr || !area.isValid()) return false;
  if (title == nullptr) {
    lv_obj_clean(root);
    image = lv_image_create(root);
    title = label(root);
    score = label(root);
    minute = label(root);
    teams = label(root);
    highlight = label(root);
    status = label(root);
    badge = label(root);
    for (auto& row : rows) row = label(root);
    if (!image || !title || !score || !minute || !teams || !highlight ||
        !status || !badge)
      return false;
    for (auto* row : rows)
      if (row == nullptr) return false;
    lv_obj_set_style_bg_color(badge, lv_color_hex(cfg::kThemeBackgroundDefault),
                              0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_80, 0);
    lv_obj_set_style_radius(badge, area.shorterSide() / 40, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_center(image);
  }
  bgColour(root,
           lv_color_hex(m.night ? cfg::kNightBackground : m.style.colour));
  if (strcmp(current_source, m.image) || source_revision != m.image_revision) {
    source_revision = m.image_revision;
    snprintf(current_source, sizeof(current_source), "%s", m.image);
    if (m.image[0]) lv_image_set_src(image, current_source);
  }
  const lv_opa_t opacity = m.night ? LV_OPA_20 : LV_OPA_COVER;
  if (lv_obj_get_style_image_opa(image, LV_PART_MAIN) != opacity)
    lv_obj_set_style_image_opa(image, opacity, 0);
  if (m.image[0])
    lv_obj_set_hidden(image, false);
  else
    lv_obj_set_hidden(image, true);
  const int s = area.shorterSide();
  const int body = (s <= cfg::kSizeClassSmallMax ? 14 : 18) *
                   m.style.text_scale / cfg::kTextScaleDefault;
  const int h = std::max(
      s / 11, body >= 18 ? static_cast<int>(ui_font_text_18.line_height)
                         : static_cast<int>(ui_font_text_14.line_height));
  bool ok = band(title, area, area.height() / 2 - s * 38 / 100, h, m.title,
                 m.style.accent, body);
  ok &= band(status, area, area.height() / 2 + s * 30 / 100, h, m.status,
             m.style.text, body);
  const bool single = m.screen == cfg::Screen::kLiveSingle;
  ok &= band(minute, area, area.height() / 2 - s * 22 / 100, h,
             single ? m.minute : "", m.style.accent, body);
  ok &= band(score, area, area.height() / 2 - s * 10 / 100, h * 2,
             single ? m.score : "", m.style.text, 18, true);
  ok &= band(teams, area, area.height() / 2 + s * 10 / 100, h,
             single ? m.teams : "", m.style.text, body);
  ok &= band(highlight, area, area.height() / 2 + s * 20 / 100, h,
             single ? m.highlight : "", m.style.accent, body);
  const bool large_own = m.screen == cfg::Screen::kLiveMulti;
  unsigned weight = m.row_count;
  if (large_own)
    for (unsigned i = 0; i < m.row_count; ++i)
      if (m.rows[i].own) ++weight;
  const int row_height = weight ? s * 56 / 100 / weight : h;
  int row_y = area.height() / 2 - static_cast<int>(weight) * row_height / 2;
  for (unsigned i = 0; i < 9; ++i) {
    if (i < m.row_count) {
      const int height =
          large_own && m.rows[i].own ? row_height * 2 : row_height;
      ok &= band(rows[i], area, row_y, height, m.rows[i].text,
                 m.rows[i].own ? m.style.accent : m.style.text,
                 large_own && m.rows[i].own ? 18 : body);
      row_y += height;
    } else
      text(rows[i], "");
  }
  const int badge_y =
      area.height() / 2 + (m.badge_bottom ? s * 20 / 100 : -s * 35 / 100);
  ok &= band(badge, area, badge_y, h * 2, m.badge, m.style.text);
  if (m.badge[0]) {
    lv_obj_set_hidden(badge, false);
    lv_obj_move_to_index(badge, -1);
  } else
    lv_obj_set_hidden(badge, true);
  return ok;
}
}  // namespace ui
