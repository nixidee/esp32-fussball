#include "football_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "display_port.h"
#include "image_service.h"
#include "lvgl.h"
#include "ui_fonts.h"
namespace ui {
namespace {
// One full-screen draw object paints every shape and text of a screen from a
// fixed item list (docs/UI.md "Drawing"). Each render compares the new items
// with the previous ones and invalidates changed boxes. LVGL may merge them
// or repaint the full screen; LCD transfers use partial stripe buffers.
enum class Kind : uint8_t { kText, kPill, kArc, kDots };
struct Item {
  Kind kind = Kind::kText;
  uint8_t a = 0;  // text: font slot; arc: width; dots: count
  uint8_t b = 0;  // dots: active dot
  lv_opa_t opa = LV_OPA_COVER;
  // Text and pill: box. Arc: centre x1/y1, radius x2. Dots: first centre
  // x1/y1, radius x2, pitch y2.
  int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  // Arc: degrees clockwise from 12 o'clock. Text: arena offset and length.
  int16_t start = 0, end = 0;
  uint32_t colour = 0, colour2 = 0;  // dots: active / other dots
  uint32_t hash = 0;  // every field except the text offset, plus the text
};
// Hashed as raw bytes: the layout must have no padding.
static_assert(sizeof(Item) == 28);
constexpr std::size_t kItems = 72;
constexpr std::size_t kArena = 1280;
Item items[kItems];
std::size_t item_count = 0, previous_count = 0;
char arena[kArena];
std::size_t arena_used = 0;
bool overflow = false;
const lv_font_t* const kFonts[] = {&ui_font_text_12, &ui_font_text_14,
                                   &ui_font_text_18, &ui_font_score_48};
constexpr uint8_t kScoreFont = 3;
constexpr char kEllipsis[] = "\xE2\x80\xA6";

lv_obj_t *image = nullptr, *scene = nullptr;
char requested_source[64]{};
uint32_t requested_revision = UINT32_MAX;
enum class SourceState : uint8_t { kPending, kAccepted, kRejected };
SourceState source_state = SourceState::kPending;
// A requested file/revision is acknowledged only after LVGL accepted its
// source. Full decoding happens later; this does not report frame health.
void applyImageSource(const ViewModel& model) {
  if (strcmp(requested_source, model.image) ||
      requested_revision != model.image_revision) {
    requested_revision = model.image_revision;
    snprintf(requested_source, sizeof(requested_source), "%s", model.image);
    source_state = SourceState::kPending;
  }
  if (source_state != SourceState::kPending) return;
  if (!requested_source[0]) {
    display::observeImageSource(nullptr, requested_revision);
    if (lv_image_get_src(image) != nullptr) lv_image_set_src(image, nullptr);
    source_state = SourceState::kAccepted;
    return;
  }
  display::observeImageSource(requested_source, requested_revision);
  // LVGL 9.6 duplicates file paths before freeing the previous copy. A
  // successful assignment has a new address even for the same file; an
  // allocation failure preserves the old pointer. Comparing text alone
  // would falsely acknowledge a failed reload of a new image revision.
  const auto previous = reinterpret_cast<uintptr_t>(lv_image_get_src(image));
  lv_image_set_src(image, requested_source);
  const void* applied = lv_image_get_src(image);
  const bool accepted =
      applied != nullptr && reinterpret_cast<uintptr_t>(applied) != previous &&
      lv_image_src_get_type(applied) == LV_IMAGE_SRC_FILE &&
      strcmp(static_cast<const char*>(applied), requested_source) == 0;
  images::acknowledgeSource(requested_revision, accepted);
  source_state = accepted ? SourceState::kAccepted : SourceState::kRejected;
}
bool imageVisible() {
  return requested_source[0] && source_state == SourceState::kAccepted;
}
// LVGL 9.6 style setters invalidate the object even when the value is
// unchanged, and a full-screen invalidation redraws the JPEG behind every
// stripe. Apply a style only when it differs from the current value.
void bgColour(lv_obj_t* object, lv_color_t value) {
  if (!lv_color_eq(lv_obj_get_style_bg_color(object, LV_PART_MAIN), value))
    lv_obj_set_style_bg_color(object, value, 0);
}

uint32_t mix(uint32_t a, uint32_t b, unsigned t) {
  uint32_t out = 0;
  for (int shift = 16; shift >= 0; shift -= 8) {
    const int x = (a >> shift) & 0xff, y = (b >> shift) & 0xff;
    const int v =
        x + ((y - x) * static_cast<int>(t) + (y >= x ? 127 : -127)) / 255;
    out |= static_cast<uint32_t>(std::clamp(v, 0, 255)) << shift;
  }
  return out;
}
uint32_t hashBytes(uint32_t hash, const void* data, std::size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (std::size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 16777619u;
  return hash;
}
// Next code point of UTF-8 text; malformed bytes count as one '?'.
uint32_t decode(const char* text, std::size_t length, std::size_t& i) {
  const auto c = static_cast<uint8_t>(text[i++]);
  if (c < 0x80) return c;
  const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  uint32_t cp = c & (0x3F >> extra);
  for (int n = 0; n < extra; ++n) {
    if (i >= length || (static_cast<uint8_t>(text[i]) & 0xC0) != 0x80)
      return '?';
    cp = cp << 6 | (static_cast<uint8_t>(text[i++]) & 0x3F);
  }
  return extra ? cp : '?';
}
int width(uint8_t font, const char* text, std::size_t length) {
  int sum = 0;
  for (std::size_t i = 0; i < length;)
    sum += lv_font_get_glyph_width(kFonts[font], decode(text, length, i), 0);
  return sum;
}
int width(uint8_t font, const char* text) {
  return width(font, text, strlen(text));
}
int line(uint8_t font) { return kFonts[font]->line_height; }

lv_area_t arcArea(const Item& item, int start, int end) {
  lv_area_t area{};
  const int r = item.x2;
  if (end - start >= 360) {
    area = {item.x1 - r, item.y1 - r, item.x1 + r, item.y1 + r};
    return area;
  }
  // LVGL measures from 3 o'clock; the mockup degrees from 12 o'clock.
  lv_draw_arc_get_area(item.x1, item.y1, static_cast<uint16_t>(r),
                       (start + 270) % 360, (end + 270) % 360, item.a, false,
                       &area);
  return area;
}
lv_area_t bounds(const Item& item) {
  switch (item.kind) {
    case Kind::kArc: return arcArea(item, item.start, item.end);
    case Kind::kDots:
      return {item.x1 - item.x2, item.y1 - item.x2,
              item.x1 + (item.a - 1) * item.y2 + item.x2, item.y1 + item.x2};
    case Kind::kText:
      // Glyphs may overhang their advance; the layer clips, not the box.
      return {item.x1 - 2, item.y1, item.x2 + 2, item.y2};
    case Kind::kPill: break;
  }
  return {item.x1, item.y1, item.x2, item.y2};
}
void invalidate(const lv_area_t& area) {
  if (area.x2 >= area.x1 && area.y2 >= area.y1)
    lv_obj_invalidate_area(scene, &area);
}
uint32_t hashOf(const Item& item) {
  Item copy = item;
  copy.hash = 0;
  if (item.kind == Kind::kText) copy.start = 0;
  uint32_t hash = hashBytes(2166136261u, &copy, sizeof(copy));
  if (item.kind == Kind::kText)
    hash = hashBytes(hash, arena + item.start, item.end);
  return hash;
}
// Stores the next item in place and invalidates what changed on screen.
void push(Item item) {
  if (item_count >= kItems) {
    overflow = true;
    return;
  }
  item.hash = hashOf(item);
  Item& old = items[item_count];
  if (item_count >= previous_count) {
    invalidate(bounds(item));
  } else if (old.hash != item.hash) {
    Item same_end = old;
    same_end.end = item.end;
    if (item.kind == Kind::kArc && old.kind == Kind::kArc &&
        hashOf(same_end) == item.hash && old.end - old.start < 360 &&
        item.end - item.start < 360) {
      // A growing minute ring only repaints the swept part.
      invalidate(arcArea(item, std::min(old.end, item.end),
                         std::max(old.end, item.end)));
    } else {
      invalidate(bounds(old));
      invalidate(bounds(item));
    }
  }
  old = item;
  ++item_count;
}

void drawScene(lv_event_t* event) {
  auto* layer = lv_event_get_layer(event);
  // Shapes first, then every text above them.
  for (std::size_t i = 0; i < item_count; ++i) {
    const Item& item = items[i];
    if (item.kind == Kind::kPill) {
      lv_draw_rect_dsc_t dsc;
      lv_draw_rect_dsc_init(&dsc);
      dsc.radius = LV_RADIUS_CIRCLE;
      dsc.bg_color = lv_color_hex(item.colour);
      dsc.bg_opa = item.opa;
      const lv_area_t box{item.x1, item.y1, item.x2, item.y2};
      lv_draw_rect(layer, &dsc, &box);
    } else if (item.kind == Kind::kDots) {
      lv_draw_rect_dsc_t dsc;
      lv_draw_rect_dsc_init(&dsc);
      dsc.radius = LV_RADIUS_CIRCLE;
      for (int n = 0; n < item.a; ++n) {
        dsc.bg_color = lv_color_hex(n == item.b ? item.colour : item.colour2);
        const int x = item.x1 + n * item.y2;
        const lv_area_t box{x - item.x2, item.y1 - item.x2, x + item.x2,
                            item.y1 + item.x2};
        lv_draw_rect(layer, &dsc, &box);
      }
    } else if (item.kind == Kind::kArc) {
      lv_draw_arc_dsc_t dsc;
      lv_draw_arc_dsc_init(&dsc);
      dsc.center = {item.x1, item.y1};
      dsc.radius = static_cast<uint16_t>(item.x2);
      dsc.width = item.a;
      dsc.color = lv_color_hex(item.colour);
      dsc.opa = item.opa;
      dsc.rounded = 0;
      const int start = (item.start + 270) % 360;
      dsc.start_angle = start;
      dsc.end_angle =
          item.end - item.start >= 360 ? start + 360 : (item.end + 270) % 360;
      lv_draw_arc(layer, &dsc);
    }
  }
  for (std::size_t i = 0; i < item_count; ++i) {
    const Item& item = items[i];
    if (item.kind != Kind::kText) continue;
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = arena + item.start;
    dsc.text_length = item.end;
    dsc.text_local = 0;
    dsc.font = kFonts[item.a];
    dsc.color = lv_color_hex(item.colour);
    dsc.opa = item.opa;
    dsc.flag = LV_TEXT_FLAG_EXPAND;
    const lv_area_t box{item.x1, item.y1, item.x2 + 2, item.y2};
    lv_draw_label(layer, &dsc, &box);
  }
}

// Layout state of one render (docs/UI.md "Layout rules"): size class fonts,
// gap unit and the screen palette.
struct Frame {
  const ViewModel* m;
  const geometry::SafeArea* area;
  int s, g, R, cx, cy, rim;
  bool round, ok;
  // Status pill (OQ-284): the layout's status line y, and the top of a
  // bottom IP chip the pill stacks above.
  bool band, chip_low;
  int band_y, chip_top;
  uint8_t cap, body, title;
  uint32_t bg, text, dim, track, accent, warn, error, neutral, chip, dot_on,
      dot_off;
  uint32_t c(uint32_t value) const {
    return m->night && m->night_colour < 255
               ? mix(cfg::kNightBackground, value, m->night_colour)
               : value;
  }
  uint32_t tone(Tone value) const {
    switch (value) {
      case Tone::kDim: return dim;
      case Tone::kAccent: return accent;
      case Tone::kWarn: return warn;
      case Tone::kError: return error;
      case Tone::kNeutral: return neutral;
      case Tone::kText: break;
    }
    return text;
  }
};
Frame f;
enum class Align : uint8_t { kCentre, kLeft, kRight };

// Largest compiled text font not above the role size scaled by the screen's
// text size; the caption font is the minimum.
uint8_t fontFor(int size_class, int role, uint8_t scale) {
  constexpr int kSizes[] = {12, 14, 18};
  const int px = cfg::kTextRolePx[size_class][role] * scale / 100;
  uint8_t font = 0;
  for (uint8_t i = 1; i < kScoreFont; ++i)
    if (px >= kSizes[i]) font = i;
  return font;
}
// Stores `text` in the arena, shortened with an ellipsis in the last font of
// `fonts` when no font fits `w`. Returns the chosen font.
uint8_t fit(const char* text, const uint8_t* fonts, int count, int w,
            uint16_t& offset, uint16_t& length) {
  const std::size_t full = strlen(text);
  uint8_t font = fonts[count - 1];
  std::size_t keep = full;
  bool cut = true;
  for (int i = 0; i < count; ++i)
    if (width(fonts[i], text, full) <= w) {
      font = fonts[i];
      cut = false;
      break;
    }
  if (cut) {
    const int room = w - width(font, kEllipsis);
    int used = 0;
    keep = 0;
    for (std::size_t i = 0; i < full;) {
      used += lv_font_get_glyph_width(kFonts[font], decode(text, full, i), 0);
      if (used > room) break;
      keep = i;
    }
    while (keep > 0 && text[keep - 1] == ' ') --keep;
  }
  const std::size_t total = keep + (cut ? sizeof(kEllipsis) - 1 : 0);
  // Each slice ends in a NUL: LVGL's expanded label measures the text to
  // the terminator and ignores text_length there.
  if (arena_used + total + 1 > kArena) {
    overflow = true;
    length = 0;
    return font;
  }
  offset = static_cast<uint16_t>(arena_used);
  memcpy(arena + arena_used, text, keep);
  if (cut) memcpy(arena + arena_used + keep, kEllipsis, sizeof(kEllipsis) - 1);
  arena[arena_used + total] = '\0';
  arena_used += total + 1;
  length = static_cast<uint16_t>(total);
  return font;
}
// One text band at y. Without x/w the text takes the chord of the band; a
// smaller font of the list is centred in the line of the largest.
int label(int y, const char* text, std::initializer_list<uint8_t> fonts,
          uint32_t colour, Align align = Align::kCentre, int x = 0, int w = -1,
          int band = 0) {
  if (!text[0]) return 0;
  const uint8_t* list = fonts.begin();
  const int count = static_cast<int>(fonts.size());
  int tallest = 0;
  for (const uint8_t font : fonts) tallest = std::max(tallest, line(font));
  if (band == 0) band = tallest;
  if (w < 0) {
    const auto span = f.area->spanForBand(y, line(list[0]));
    x = span.x;
    w = span.width;
  }
  if (w <= 0) {
    f.ok = false;
    return 0;
  }
  Item item;
  item.kind = Kind::kText;
  uint16_t offset = 0, length = 0;
  item.a = fit(text, list, count, w, offset, length);
  if (length == 0) return 0;
  item.start = static_cast<int16_t>(offset);
  item.end = static_cast<int16_t>(length);
  const int tw = width(item.a, arena + offset, length);
  const int tx = align == Align::kLeft    ? x
                 : align == Align::kRight ? x + w - tw
                                          : x + (w - tw) / 2;
  const int ty = y + (band - line(item.a)) / 2;
  item.x1 = static_cast<int16_t>(tx);
  item.y1 = static_cast<int16_t>(ty);
  item.x2 = static_cast<int16_t>(tx + tw - 1);
  item.y2 = static_cast<int16_t>(ty + line(item.a) - 1);
  item.colour = colour;
  if (!f.area->contains({tx, ty, std::max(tw, 1), line(item.a)})) f.ok = false;
  push(item);
  return tw;
}
void pill(int x, int y, int w, int h, uint32_t colour, lv_opa_t opa) {
  if (w <= 0 || h <= 0) return;
  Item item;
  item.kind = Kind::kPill;
  item.x1 = static_cast<int16_t>(x);
  item.y1 = static_cast<int16_t>(y);
  item.x2 = static_cast<int16_t>(x + w - 1);
  item.y2 = static_cast<int16_t>(y + h - 1);
  item.colour = colour;
  item.opa = opa;
  push(item);
}
void dots(int y, int count, int active, uint32_t on, uint32_t off) {
  if (count < 2) return;
  const int r = std::max(2, f.s / 100), pitch = r * 4;
  Item item;
  item.kind = Kind::kDots;
  item.a = static_cast<uint8_t>(count);
  item.b = static_cast<uint8_t>(active);
  item.x1 = static_cast<int16_t>(f.cx - (count - 1) * pitch / 2);
  item.y1 = static_cast<int16_t>(y);
  item.x2 = static_cast<int16_t>(r);
  item.y2 = static_cast<int16_t>(pitch);
  item.colour = on;
  item.colour2 = off;
  push(item);
}
// Arc on the rim of a round panel; degrees clockwise from 12 o'clock.
void arc(int start, int end, uint32_t colour, int w = 0, int inset = 0) {
  if (!f.round || end <= start) return;
  Item item;
  item.kind = Kind::kArc;
  item.a = static_cast<uint8_t>(w ? w : f.rim);
  item.x1 = static_cast<int16_t>(f.cx);
  item.y1 = static_cast<int16_t>(f.cy);
  item.x2 = static_cast<int16_t>(f.s / 2 - 1 - inset);
  item.start = static_cast<int16_t>(start);
  item.end = static_cast<int16_t>(std::min(end, start + 360));
  item.colour = colour;
  push(item);
}

// IP chip: pill behind one or two caption lines, each fitted to its chord.
// Lines start at y, or end at y when the chip grows upward.
void chip(int y, bool upward) {
  const auto& m = *f.m;
  const int n = m.badge_lines, cap = line(f.cap);
  const int top = upward ? y - n * cap : y;
  if (m.badge_bottom) {
    f.chip_low = true;
    f.chip_top = top - f.g / 2;
  }
  int w = 0;
  for (int i = 0; i < n; ++i)
    w = std::max(w,
                 std::min<int>(width(f.cap, m.badge[i]),
                               f.area->spanForBand(top + i * cap, cap).width));
  pill(f.cx - (w + 4 * f.g) / 2, top - f.g / 2, w + 4 * f.g, n * cap + f.g,
       f.chip, 235);
  for (int i = 0; i < n; ++i) label(top + i * cap, m.badge[i], {f.cap}, f.text);
}
// Status at line y: the place of the pill drawn over the screen at the end
// (statusPill()), or a permanent line ("Demo") when the line is 'free'.
void statusAt(int y, bool free) {
  const auto& m = *f.m;
  if (m.status_pill) {
    f.band = true;
    f.band_y = y;
  } else if (free) {
    label(y, m.status, {f.cap}, f.tone(m.status_tone));
  }
}
// Bottom zone: the IP chip when it sits at the bottom, and the status.
void statusBand(int y) {
  const auto& m = *f.m;
  const bool low_chip = m.badge_lines && m.badge_bottom;
  if (low_chip) chip(y, false);
  statusAt(y, !low_chip);
}
// Temporary status pill over everything else at the bottom (docs/UI.md
// "Status pill"), stacked above a bottom IP chip; slightly transparent.
void statusPill() {
  const auto& m = *f.m;
  if (!f.band || !m.status[0]) return;
  const int cap = line(f.cap), h = cap + f.g;
  const int y = f.chip_low ? f.chip_top - h : f.band_y;
  const int w =
      std::min<int>(width(f.cap, m.status), f.area->spanForBand(y, cap).width);
  pill(f.cx - (w + 4 * f.g) / 2, y - f.g / 2, w + 4 * f.g, h, f.chip,
       cfg::kStatusPillOpa);
  label(y, m.status, {f.cap}, f.tone(m.status_tone));
}
// Caption band at the top, or the IP chip growing upward from its bottom.
void captionBand(int y, const char* text, uint32_t colour) {
  const auto& m = *f.m;
  if (m.badge_lines && !m.badge_bottom)
    chip(y + line(f.cap), true);
  else
    label(y, text, {f.cap}, colour);
}

void ownMatch() {
  const auto& m = *f.m;
  const auto& own = m.own;
  const int g = f.g, cap = line(f.cap), title = line(f.title),
            score = line(kScoreFont), body = line(f.body);
  const int heights = cap + g + title + g + score + g + title + 2 * g + body +
                      g + g + cap + g + cap;
  int y = f.cy - heights / 2;
  captionBand(y, m.caption, f.dim);
  y += cap + g;
  label(y, own.home, {f.title, f.body}, own.own_home ? f.accent : f.text);
  y += title + g;
  if (strspn(own.centre, " 0123456789:-?") == strlen(own.centre))
    label(y, own.centre, {kScoreFont}, f.text);
  else
    label(y, own.centre, {f.title, f.body}, f.text, Align::kCentre, 0, -1,
          score);
  y += score + g;
  label(y, own.away, {f.title, f.body}, own.own_away ? f.accent : f.text);
  y += title + 2 * g;
  if (own.pill[0]) {
    const int pw = width(f.body, own.pill) + 4 * g, ph = body + g;
    pill(f.cx - pw / 2, y, pw, ph, f.tone(own.pill_tone), 70);
    label(y + g / 2, own.pill, {f.body}, f.tone(own.pill_text));
  }
  y += body + g + g;
  label(y, own.highlight, {f.cap}, f.tone(own.highlight_tone));
  y += cap + g;
  statusBand(y);
}

void conference() {
  const auto& m = *f.m;
  const int g = f.g, cap = line(f.cap), body = line(f.body);
  const int rows = m.row_count;
  const int pitch = body + 2 * g;
  const int slots = m.pages > 1 ? std::max<int>(m.slots, rows) : rows;
  const int rows_top = f.cy - slots * pitch / 2;
  captionBand(rows_top - cap - 2 * g, m.caption, f.accent);
  // A centre wider than a score ("Sa 15:30" on the matchday screen) widens
  // the column so that at least its caption-size text fits.
  int score_w = std::max(width(f.body, "10:10"), width(f.body, "00:00"));
  for (int i = 0; i < rows; ++i)
    score_w = std::max(score_w, width(f.cap, m.matches[i].centre));
  const int half = f.area->spanForBand(rows_top, slots * pitch).width / 2;
  const int name_w = half - score_w / 2 - 2 * g;
  uint8_t font = f.body;
  for (int i = 0; i < rows; ++i)
    if (width(f.body, m.matches[i].home) > name_w ||
        width(f.body, m.matches[i].away) > name_w)
      font = f.cap;
  int y = rows_top;
  for (int i = 0; i < rows; ++i) {
    const auto& row = m.matches[i];
    if (row.goal)
      pill(f.cx - half, y, 2 * half, pitch - g, f.accent, LV_OPA_COVER);
    else if (row.own)
      pill(f.cx - half, y, 2 * half, pitch - g, f.chip, 200);
    const uint32_t fg = row.goal ? f.bg : f.text;
    const int ty = y + (pitch - g - line(font)) / 2;
    label(ty, row.home, {font}, row.own_home && !row.goal ? f.accent : fg,
          Align::kRight, f.cx - score_w / 2 - g - name_w, name_w);
    label(ty, row.away, {font}, row.own_away && !row.goal ? f.accent : fg,
          Align::kLeft, f.cx + score_w / 2 + g, name_w);
    label(y + (pitch - g - body) / 2, row.centre, {f.body, f.cap},
          row.goal ? f.bg : f.tone(row.centre_tone), Align::kCentre,
          f.cx - score_w / 2, score_w, body);
    y += pitch;
  }
  y = rows_top + slots * pitch + g;
  const bool low_chip = m.badge_lines && m.badge_bottom;
  if (low_chip)
    chip(y, false);
  else if (m.pages > 1)
    dots(y + cap / 2, m.pages, m.page, f.accent, f.track);
  // The page dots take the line of a permanent status; the pill covers them.
  statusAt(y, !low_chip && m.pages <= 1);
}

void table() {
  const auto& m = *f.m;
  const int g = f.g, cap = line(f.cap), body = line(f.body);
  const int rows = m.row_count;
  const int pitch = rows <= 5 ? body + 2 * g : body + g / 2;
  const int rows_top = f.cy - rows * pitch / 2;
  const int header_y = rows_top - cap - g;
  const int half = f.area->spanForBand(rows_top, rows * pitch).width / 2;
  const int left = f.cx - half + g, right = f.cx + half - g;
  int pos_w = std::max(width(f.cap, "#"), width(f.body, "--"));
  int widths[3];
  for (int c = 0; c < 3; ++c)
    widths[c] = std::max(width(f.cap, m.columns[c]), width(f.body, "--"));
  for (int i = 0; i < rows; ++i) {
    const auto& row = m.table[i];
    pos_w = std::max(pos_w, width(f.body, row.position));
    const char* values[3] = {row.points, row.difference, row.played};
    for (int c = 0; c < 3; ++c)
      widths[c] = std::max(widths[c], width(f.body, values[c]));
  }
  const int name_x = left + pos_w + 2 * g;
  const int min_name = 10 * width(f.cap, "n");
  int column_x[3]{}, shown = 0, x = right;
  for (; shown < 3; ++shown) {
    // Priority points > difference > played while ten letters still fit.
    if (x - widths[shown] - 2 * g - name_x < min_name) break;
    x -= widths[shown];
    column_x[shown] = x;
    x -= 2 * g;
  }
  const int name_w = x - name_x;
  uint8_t font = f.body;
  for (int i = 0; i < rows; ++i)
    if (width(f.body, m.table[i].name) > name_w) font = f.cap;
  if (m.badge_lines && !m.badge_bottom) {
    chip(header_y + cap, true);
  } else {
    const auto span = f.area->spanForBand(header_y, cap);
    bool titles = shown > 0 && span.x <= left;
    for (int i = 0; i < shown; ++i)
      titles &= column_x[i] + widths[i] <= span.x + span.width;
    if (titles) {
      label(header_y, "#", {f.cap}, f.dim, Align::kRight, left, pos_w);
      label(header_y, m.caption, {f.cap}, f.dim, Align::kLeft, name_x, name_w);
      for (int i = 0; i < shown; ++i)
        label(header_y, m.columns[i], {f.cap}, f.dim, Align::kRight,
              column_x[i] - g, widths[i] + g);
    } else {
      label(header_y, m.caption, {f.cap}, f.dim);
    }
  }
  int y = rows_top;
  for (int i = 0; i < rows; ++i) {
    const auto& row = m.table[i];
    if (row.own)
      pill(left - g, y, right - left + 2 * g, pitch - g / 2, f.accent,
           LV_OPA_COVER);
    const uint32_t fg = row.own ? f.bg : f.text;
    const uint32_t minor = row.own ? f.bg : f.dim;
    const int ty = y + (pitch - g / 2 - body) / 2;
    label(ty, row.position, {f.body}, minor, Align::kRight, left, pos_w);
    label(ty + (body - line(font)) / 2, row.name, {font}, fg, Align::kLeft,
          name_x, name_w);
    const char* values[3] = {row.points, row.difference, row.played};
    for (int c = 0; c < shown; ++c)
      label(ty, values[c], {f.body}, c == 0 ? fg : minor, Align::kRight,
            column_x[c] - g, widths[c] + g);
    y += pitch;
  }
  statusBand(y + g);
}

void crest() {
  const auto& m = *f.m;
  const int cap = line(f.cap), r = std::max(2, f.s / 100);
  const int dots_y = f.cy + f.R - 3 * f.g;
  const int bottom = dots_y - r - 2 * f.g - cap;
  if (m.pages > 1) dots(dots_y, m.pages, m.page, f.dot_on, f.dot_off);
  if (m.badge_lines && !m.badge_bottom) chip(2 * f.cy - bottom, true);
  // A bottom chip ends on the status line, above the slide dots.
  const bool low_chip = m.badge_lines && m.badge_bottom;
  if (low_chip) chip(bottom + cap, true);
  statusAt(bottom, !low_chip);
}

void card() {
  const auto& m = *f.m;
  const auto& card = m.card;
  const int g = f.g;
  int total = line(f.title);
  for (int i = 0; i < card.line_count; ++i) total += g + line(f.body);
  for (int i = 0; i < card.hint_count; ++i) total += g + line(f.cap);
  const int top = f.cy - total / 2;
  int y = top;
  label(y, card.title, {f.title, f.body}, f.text);
  y += line(f.title) + g;
  for (int i = 0; i < card.line_count; ++i) {
    label(y, card.lines[i], {f.body, f.cap},
          card.accent[i] ? f.accent : f.text);
    y += line(f.body) + g;
  }
  for (int i = 0; i < card.hint_count; ++i) {
    label(y, card.hints[i], {f.cap}, f.dim);
    y += line(f.cap) + g;
  }
  if (m.badge_lines) {
    if (m.badge_bottom)
      chip(y, false);
    else
      chip(top - g, true);
  }
  // A card has no permanent status line; the pill goes below its text.
  statusAt(y, false);
}

void rim() {
  const auto& rim = f.m->rim;
  switch (rim.kind) {
    case RimKind::kNone: break;
    case RimKind::kMatch:
      arc(0, 360, f.track);
      if (rim.minute >= 0)
        arc(0, std::min(360, rim.minute * 4), rim.finished ? f.dim : f.accent);
      if (rim.extra && rim.minute > 90)
        arc(0, std::min(360, (rim.minute - 90) * 12), f.accent, 0, f.rim + 1);
      if (rim.gap) arc(179, 181, f.bg);
      break;
    case RimKind::kRing: arc(0, 360, f.tone(rim.tone)); break;
    case RimKind::kSegments:
      for (int i = 0; i < 3; ++i)
        arc(i * 120 + 4, i * 120 + 116,
            i < rim.done ? f.accent
            : i == rim.current
                ? f.c(mix(f.m->style.colour, f.m->style.accent, 115))
                : f.track);
      break;
    case RimKind::kScroll:
      if (rim.total == 0) break;
      arc(40, 140, f.track);
      arc(40 + 100 * rim.first / rim.total,
          40 + 100 * (rim.first + rim.count) / rim.total, f.accent, f.rim + 1);
      break;
  }
}
}  // namespace

bool render(const ViewModel& m, const geometry::SafeArea& area) {
  auto* root = lv_screen_active();
  if (root == nullptr || !area.isValid()) return false;
  if (scene == nullptr) {
    lv_obj_clean(root);
    image = lv_image_create(root);
    scene = lv_obj_create(root);
    if (!image || !scene) return false;
    lv_obj_remove_style_all(scene);
    lv_obj_set_clickable(scene, false);
    lv_obj_set_scrollable(scene, false);
    lv_obj_add_event_cb(scene, drawScene, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_center(image);
  }
  if (lv_obj_get_width(scene) != area.width() ||
      lv_obj_get_height(scene) != area.height())
    lv_obj_set_size(scene, area.width(), area.height());
  bgColour(root,
           lv_color_hex(m.night ? cfg::kNightBackground : m.style.colour));
  applyImageSource(m);
  const lv_opa_t opacity = m.night ? LV_OPA_20 : LV_OPA_COVER;
  if (lv_obj_get_style_image_opa(image, LV_PART_MAIN) != opacity)
    lv_obj_set_style_image_opa(image, opacity, 0);
  lv_obj_set_hidden(image, !imageVisible());

  const int s = area.shorterSide();
  const int size_class = s <= cfg::kSizeClassSmallMax    ? 0
                         : s <= cfg::kSizeClassMediumMax ? 1
                                                         : 2;
  f = {};
  f.m = &m;
  f.area = &area;
  f.s = s;
  f.g = std::max(2, s / 60);
  f.R = s / 2 - area.margin();
  f.cx = area.width() / 2;
  f.cy = area.height() / 2;
  f.rim = std::max(2, s / 80);
  f.round = area.shape() == hw::DisplayShape::kRound;
  f.ok = true;
  f.cap = fontFor(size_class, 0, m.style.text_scale);
  f.body = fontFor(size_class, 1, m.style.text_scale);
  f.title = fontFor(size_class, 2, m.style.text_scale);
  const auto& style = m.style;
  f.bg = m.night ? cfg::kNightBackground : style.colour;
  f.text = f.c(style.text);
  f.dim = f.c(mix(style.colour, style.text, 153));
  f.track = f.c(mix(style.colour, style.text, 41));
  f.accent = f.c(style.accent);
  f.warn = f.c(cfg::kWarnColour);
  f.error = f.c(cfg::kErrorColour);
  f.neutral = f.c(cfg::kNeutralColour);
  f.chip = f.c(cfg::kChipColour);
  f.dot_on = f.c(cfg::kSlideDotOnColour);
  f.dot_off = f.c(cfg::kSlideDotOffColour);

  previous_count = item_count;
  item_count = 0;
  arena_used = 0;
  overflow = false;
  // The rim first: its items keep their slots when texts come and go.
  rim();
  switch (m.layout) {
    case Layout::kOwnMatch: ownMatch(); break;
    case Layout::kConference: conference(); break;
    case Layout::kTable: table(); break;
    case Layout::kCard: card(); break;
    case Layout::kImage: crest(); break;
  }
  // Last: the pill lies over the content.
  statusPill();
  for (std::size_t i = item_count; i < previous_count; ++i)
    invalidate(bounds(items[i]));
  return f.ok && !overflow;
}
}  // namespace ui
