// Immutable 4-bpp LVGL RLE without XOR prefiltering. The public bitmap
// callback expands directly into LVGL's A8 target; it never allocates.
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  // First member preserves the public native metrics/cmap reader contract.
  lv_font_fmt_txt_dsc_t text;
  uint32_t bitmap_bytes;
  uint16_t glyph_count;  // includes the reserved glyph zero
} ui_font_rle_dsc_t;

const void* ui_font_rle_bitmap(lv_font_glyph_dsc_t* glyph,
                               lv_draw_buf_t* target);

#ifdef __cplusplus
}
#endif
