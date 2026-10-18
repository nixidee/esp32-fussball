// LVGL's 4-bpp RLE stream, no prefilter. Decode in the public font callback,
// directly into its existing A8 target, without native decoder line buffers.
// Stream state transitions follow LVGL's MIT-licensed fmt_txt RLE format.
#include "ui_font_rle.h"

#include <stddef.h>

typedef struct {
  const uint8_t* bytes;
  uint32_t bit, limit;
} Bits;

static bool take(Bits* bits, uint8_t count, uint8_t* value) {
  if (count > bits->limit - bits->bit) return false;
  const uint32_t byte = bits->bit / 8;
  const uint8_t offset = bits->bit % 8;
  uint16_t pair = (uint16_t)bits->bytes[byte] << 8;
  if (offset + count > 8) pair |= bits->bytes[byte + 1];
  *value = (pair >> (16 - offset - count)) & ((1u << count) - 1);
  bits->bit += count;
  return true;
}

const void* ui_font_rle_bitmap(lv_font_glyph_dsc_t* glyph,
                               lv_draw_buf_t* target) {
  if (glyph == NULL || glyph->resolved_font == NULL || target == NULL ||
      target->data == NULL || glyph->req_raw_bitmap)
    return NULL;
  const ui_font_rle_dsc_t* data = glyph->resolved_font->dsc;
  if (data == NULL || data->text.glyph_bitmap == NULL ||
      data->text.glyph_dsc == NULL || data->text.bpp != 4 ||
      data->bitmap_bytes > UINT32_MAX / 8 || glyph->gid.index == 0 ||
      glyph->gid.index >= data->glyph_count)
    return NULL;
  const lv_font_fmt_txt_glyph_dsc_t* desc =
      &data->text.glyph_dsc[glyph->gid.index];
  const uint32_t begin = desc->bitmap_index;
  const uint32_t end = glyph->gid.index + 1 < data->glyph_count
                           ? desc[1].bitmap_index
                           : data->bitmap_bytes;
  const uint32_t width = desc->box_w, height = desc->box_h;
  const uint32_t stride = target->header.stride;
  if (width == 0 || height == 0 || begin > end || end > data->bitmap_bytes ||
      target->header.cf != LV_COLOR_FORMAT_A8 || target->header.w < width ||
      target->header.h < height || stride < width ||
      height > target->data_size / stride)
    return NULL;

  Bits bits = {data->text.glyph_bitmap + begin, 0, (end - begin) * 8};
  enum { SINGLE, REPEATED } state = SINGLE;
  uint8_t previous = 0, repeats = 0;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row = target->data + y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t pixel = previous;
      if (state == SINGLE) {
        if (!take(&bits, 4, &pixel)) return NULL;
        if (bits.bit > 4 && pixel == previous) {
          state = REPEATED;
          repeats = 0;
        }
      } else if (state == REPEATED) {
        uint8_t same;
        if (!take(&bits, 1, &same)) return NULL;
        ++repeats;
        if (same && repeats == 11) {
          if (!take(&bits, 6, &repeats)) return NULL;
          // The generator emits only the zero-count escape, never long-run
          // counters. Reject unsupported streams instead of another mode.
          if (repeats != 0 || !take(&bits, 4, &pixel)) return NULL;
          state = SINGLE;
        } else if (!same) {
          if (!take(&bits, 4, &pixel)) return NULL;
          state = SINGLE;
        }
      }
      previous = pixel;
      row[x] = pixel * 17;  // Preserve every original 4-bpp alpha value.
    }
  }
  // Only zero padding to the next byte may remain in an immutable glyph.
  uint8_t padding = 0;
  const uint32_t remaining = bits.limit - bits.bit;
  if (remaining > 7 || (remaining && !take(&bits, remaining, &padding)) ||
      padding != 0)
    return NULL;
  lv_draw_buf_flush_cache(target, NULL);
  return target;
}
