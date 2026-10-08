// Exact whole-pixel bounds without a framebuffer, lookup table or libm.

#include "safe_area.h"

#include <algorithm>

namespace geometry {
namespace {

// Restoring integer square root: at most 16 iterations for a uint32_t input.
uint32_t floorSqrt(uint32_t value) noexcept {
  uint32_t root = 0;
  uint32_t bit = uint32_t{1} << 30;
  while (bit > value) bit >>= 2;
  while (bit != 0) {
    if (value >= root + bit) {
      value -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

// The caller first bounds the interval by a uint16_t display dimension.
// Doubled distances then stay in [-65535, 65535], including odd dimensions.
uint32_t farthestDoubledDistance(int32_t start, int32_t extent,
                                uint16_t dimension) noexcept {
  const int32_t first = 2 * start - dimension;
  const int32_t last = 2 * (start + extent) - dimension;
  const uint32_t first_distance = first < 0 ? -first : first;
  const uint32_t last_distance = last < 0 ? -last : last;
  return std::max(first_distance, last_distance);
}

// Subtraction before endpoint construction prevents signed input overflow.
bool intervalInDisplay(int32_t start, int32_t extent,
                       uint16_t dimension) noexcept {
  return start >= 0 && extent > 0 && start <= dimension &&
         extent <= static_cast<int32_t>(dimension) - start;
}

}  // namespace

SafeArea::SafeArea(uint16_t width, uint16_t height, hw::DisplayShape shape,
                   uint16_t margin) noexcept
    : width_(width), height_(height), margin_(margin), shape_(shape) {}

bool SafeArea::isValid() const noexcept {
  return (shape_ == hw::DisplayShape::kRound ||
          shape_ == hw::DisplayShape::kRect) &&
         width_ > 0 && height_ > 0 &&
         uint32_t{2} * margin_ < std::min(width_, height_);
}

HorizontalSpan SafeArea::spanForBand(int32_t y, int32_t height) const noexcept {
  if (!isValid() || !intervalInDisplay(y, height, height_)) return {0, 0};

  if (shape_ == hw::DisplayShape::kRect) {
    if (y < margin_ || height > static_cast<int32_t>(height_) - margin_ - y)
      return {0, 0};
    return {margin_, static_cast<int32_t>(width_) - 2 * margin_};
  }

  const uint32_t diameter = std::min(width_, height_) - uint32_t{2} * margin_;
  const uint32_t dy = farthestDoubledDistance(y, height, height_);
  if (dy > diameter) return {0, 0};

  // Each square fits uint32_t. Avoid adding squares, which could overflow.
  const uint32_t chord = floorSqrt(diameter * diameter - dy * dy);
  const int32_t left = (static_cast<uint32_t>(width_) - chord + 1) / 2;
  const int32_t right = (static_cast<uint32_t>(width_) + chord) / 2;
  return right > left ? HorizontalSpan{left, right - left}
                      : HorizontalSpan{0, 0};
}

bool SafeArea::contains(const Rect& rect) const noexcept {
  if (!isValid() || !intervalInDisplay(rect.x, rect.width, width_) ||
      !intervalInDisplay(rect.y, rect.height, height_))
    return false;

  if (shape_ == hw::DisplayShape::kRect) {
    return rect.x >= margin_ && rect.y >= margin_ &&
           rect.width <= static_cast<int32_t>(width_) - margin_ - rect.x &&
           rect.height <= static_cast<int32_t>(height_) - margin_ - rect.y;
  }

  const uint32_t diameter = std::min(width_, height_) - uint32_t{2} * margin_;
  const uint32_t dy = farthestDoubledDistance(rect.y, rect.height, height_);
  if (dy > diameter) return false;
  const uint32_t dx = farthestDoubledDistance(rect.x, rect.width, width_);
  return dx * dx <= diameter * diameter - dy * dy;
}

}  // namespace geometry
