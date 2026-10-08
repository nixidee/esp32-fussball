// Pure display-content geometry, independent of ESP-IDF and LVGL.
// Dimensions and shape come from the caller's hardware profile; the content
// margin is caller-supplied software policy. No allocation or floating point.

#pragma once

#include <cstdint>

#include "hw_profile.h"

namespace geometry {

// Pixel-boundary rectangle: [x, x + width] x [y, y + height]. Checking these
// outer edges conservatively contains the entire covered pixel area.
struct Rect {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
};

// Whole-pixel horizontal interval [x, x + width]. {0, 0} means no content fits.
struct HorizontalSpan {
  int32_t x;
  int32_t width;
};

class SafeArea {
 public:
  // A round profile uses the centred circle whose diameter is the shorter
  // display side. The margin erodes its radius, or each edge for a rectangle.
  SafeArea(uint16_t width, uint16_t height, hw::DisplayShape shape,
           uint16_t margin) noexcept;

  // Requires positive dimensions, a known shape and a remaining interior.
  // Valid continuous geometry need not contain a nonempty whole-pixel span:
  // for example, a 1 x 1 circle with zero margin has no such rectangle.
  bool isValid() const noexcept;

  // Widest integer span contained over the full band [y, y + height].
  // Invalid geometry/bands or a band with no whole-pixel width return {0, 0}.
  HorizontalSpan spanForBand(int32_t y, int32_t height) const noexcept;

  // Requires positive extents. Invalid geometry, overflow-prone/out-of-display
  // rectangles and rectangles crossing the content boundary return false.
  bool contains(const Rect& rect) const noexcept;

 private:
  uint16_t width_;
  uint16_t height_;
  uint16_t margin_;
  hw::DisplayShape shape_;
};

}  // namespace geometry
