// Actual LVGL native 4-bpp reader versus the production zero-heap callback.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "font_cases.h"
#include "ui_font_rle.h"

// Host adapters only: A8 stride follows this fixture's LVGL config. Neither
// adapter reads pixels, descriptors, Unicode maps or compressed streams.
uint32_t lv_draw_buf_width_to_stride(uint32_t width, lv_color_format_t format) {
  assert(format == LV_COLOR_FORMAT_A8);
  return (width + LV_DRAW_BUF_STRIDE_ALIGN - 1) / LV_DRAW_BUF_STRIDE_ALIGN *
         LV_DRAW_BUF_STRIDE_ALIGN;
}
static unsigned flushes;
void lv_draw_buf_flush_cache(const lv_draw_buf_t* buffer,
                             const lv_area_t* area) {
  assert(buffer != NULL && area == NULL);
  ++flushes;
}
void* lv_malloc(size_t size) {
  (void)size;
  assert(!"unexpected font heap allocation");
  return NULL;
}
void lv_free(void* pointer) {
  (void)pointer;
  assert(!"unexpected font heap release");
}

static unsigned glyphs, nonempty, pixels;
static void malformed(void) {
  lv_font_fmt_txt_glyph_dsc_t desc[2] = {{0}, {.box_w = 1, .box_h = 1}};
  uint8_t encoded = 0x10, output = 0;
  ui_font_rle_dsc_t data = {
      .text = {.glyph_bitmap = &encoded, .glyph_dsc = desc, .bpp = 4},
      .bitmap_bytes = 1,
      .glyph_count = 2};
  lv_font_t font = {.dsc = &data};
  lv_font_glyph_dsc_t glyph = {.resolved_font = &font, .gid = {.index = 1}};
  lv_draw_buf_t target = {
      .header = {.cf = LV_COLOR_FORMAT_A8, .w = 1, .h = 1, .stride = 1},
      .data = &output,
      .data_size = 1};
  assert(ui_font_rle_bitmap(&glyph, &target) == &target && output == 17);
  const unsigned before = flushes;
  encoded = 0x1f;  // nonzero trailing padding is corrupt
  assert(ui_font_rle_bitmap(&glyph, &target) == NULL);
  desc[1].bitmap_index = 2;  // outside immutable payload
  assert(ui_font_rle_bitmap(&glyph, &target) == NULL);
  desc[1].bitmap_index = 0;
  data.text.bpp = 2;
  assert(ui_font_rle_bitmap(&glyph, &target) == NULL);
  assert(flushes == before);
}

static double benchmark(bool original) {
  uint8_t output[4096];
  const clock_t begin = clock();
  for (unsigned repeat = 0; repeat < 100; ++repeat)
    for (unsigned f = 0; f < sizeof(cases) / sizeof(cases[0]); ++f)
      for (unsigned i = 0; i < cases[f].count; ++i) {
        lv_font_glyph_dsc_t glyph = {0};
        const lv_font_t* font = original ? cases[f].plain : cases[f].font;
        assert(lv_font_get_glyph_dsc(font, &glyph, cases[f].codepoints[i], 0));
        if (!glyph.box_w || !glyph.box_h) continue;
        const unsigned stride =
            lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
        lv_draw_buf_t target = {.header = {.cf = LV_COLOR_FORMAT_A8,
                                           .w = glyph.box_w,
                                           .h = glyph.box_h,
                                           .stride = stride},
                                .data = output,
                                .data_size = sizeof(output)};
        assert(lv_font_get_glyph_bitmap(&glyph, &target) == &target);
      }
  return (double)(clock() - begin) / CLOCKS_PER_SEC;
}
static void equalGlyph(const lv_font_t* font, const lv_font_t* plain,
                       uint32_t codepoint) {
  lv_font_glyph_dsc_t a = {0}, b = {0};
  assert(lv_font_get_glyph_dsc(font, &a, codepoint, 0));
  assert(lv_font_get_glyph_dsc(plain, &b, codepoint, 0));
  assert(a.adv_w == b.adv_w && a.box_w == b.box_w && a.box_h == b.box_h &&
         a.ofs_x == b.ofs_x && a.ofs_y == b.ofs_y && a.format == b.format &&
         a.stride == b.stride && a.gid.index == b.gid.index);
  ++glyphs;
  if (!a.box_w || !a.box_h) return;
  ++nonempty;
  const unsigned stride =
      lv_draw_buf_width_to_stride(a.box_w, LV_COLOR_FORMAT_A8);
  uint8_t decoded[4096], original[4096];
  assert(stride * a.box_h <= sizeof(decoded));
  memset(decoded, 0xcc, sizeof(decoded));
  memset(original, 0xcc, sizeof(original));
  lv_draw_buf_t output = {.header = {.cf = LV_COLOR_FORMAT_A8,
                                     .w = a.box_w,
                                     .h = a.box_h,
                                     .stride = stride},
                          .data = decoded,
                          .data_size = sizeof(decoded)};
  lv_draw_buf_t reference = output;
  reference.data = original;
  assert(lv_font_get_glyph_bitmap(&a, &output) == &output);
  assert(lv_font_get_glyph_bitmap(&b, &reference) == &reference);
  assert(memcmp(decoded, original, sizeof(decoded)) == 0);
  pixels += a.box_w * a.box_h;

  const unsigned before = flushes;
  assert(lv_font_get_glyph_bitmap(&a, NULL) ==
         NULL);  // failed caller allocation
  output.data = NULL;
  assert(lv_font_get_glyph_bitmap(&a, &output) == NULL);
  output.data = decoded;
  output.data_size = stride * a.box_h - 1;
  assert(lv_font_get_glyph_bitmap(&a, &output) == NULL);
  output.data_size = sizeof(decoded);
  output.header.stride = a.box_w - 1;
  assert(lv_font_get_glyph_bitmap(&a, &output) == NULL);
  output.header.stride = stride;
  output.header.cf = LV_COLOR_FORMAT_RGB565;
  assert(lv_font_get_glyph_bitmap(&a, &output) == NULL);
  output.header.cf = LV_COLOR_FORMAT_A8;
  a.req_raw_bitmap = 1;
  assert(ui_font_rle_bitmap(&a, &output) == NULL);
  a.req_raw_bitmap = 0;

  // Treat the current glyph as the last of a deliberately truncated source.
  ui_font_rle_dsc_t truncated = *(const ui_font_rle_dsc_t*)font->dsc;
  const unsigned end =
      a.gid.index + 1 < truncated.glyph_count
          ? truncated.text.glyph_dsc[a.gid.index + 1].bitmap_index
          : truncated.bitmap_bytes;
  truncated.glyph_count = a.gid.index + 1;
  truncated.bitmap_bytes = end - 1;
  lv_font_t broken = *font;
  broken.dsc = &truncated;
  a.resolved_font = &broken;
  assert(lv_font_get_glyph_bitmap(&a, &output) == NULL);
  a.gid.index = truncated.glyph_count;
  assert(lv_font_get_glyph_bitmap(&a, &output) == NULL);
  assert(flushes == before);  // no partial output is published on failure
}

int main(void) {
  for (unsigned f = 0; f < sizeof(cases) / sizeof(cases[0]); ++f) {
    const lv_font_t *font = cases[f].font, *plain = cases[f].plain;
    assert(font->line_height == plain->line_height &&
           font->base_line == plain->base_line &&
           font->cap_height == plain->cap_height &&
           font->x_height == plain->x_height);
    for (unsigned i = 0; i < cases[f].count; ++i)
      equalGlyph(font, plain, cases[f].codepoints[i]);
    equalGlyph(font, plain, 0x10ffff);  // existing question-mark fallback
  }
  assert(nonempty == 695);  // 691 nonempty originals plus four fallback checks
  malformed();
  printf(
      "PASS %u glyph/fallback metrics, %u A8 bitmap comparisons (%u pixels), "
      "zero font heap; truncated/OOM sinks\n",
      glyphs, nonempty, pixels);
  const double plain = benchmark(true), compressed = benchmark(false);
  printf(
      "Host sanitizer CPU for100 complete font sets: plain %.3fs, RLE %.3fs "
      "(%.2fx); not C6 timing\n",
      plain, compressed, compressed / plain);
}
