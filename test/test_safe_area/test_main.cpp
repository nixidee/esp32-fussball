// Host acceptance for conservative whole-pixel content geometry.
// The reference checks all four outer corners with int64_t arithmetic; it does
// not use the implementation's integer square root or farthest-axis reduction.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

#include <unity.h>

#include "safe_area.h"

namespace {

using geometry::HorizontalSpan;
using geometry::Rect;
using geometry::SafeArea;
using hw::DisplayShape;

constexpr std::array<DisplayShape, 2> kShapes = {
    DisplayShape::kRound, DisplayShape::kRect};

bool oracleValid(uint16_t width, uint16_t height, DisplayShape shape,
                 uint16_t margin) {
  return width > 0 && height > 0 &&
         (shape == DisplayShape::kRound || shape == DisplayShape::kRect) &&
         int64_t{2} * margin < std::min(width, height);
}

bool oracleContains(uint16_t width, uint16_t height, DisplayShape shape,
                    uint16_t margin, const Rect& rect) {
  if (!oracleValid(width, height, shape, margin) || rect.x < 0 || rect.y < 0 ||
      rect.width <= 0 || rect.height <= 0)
    return false;

  const int64_t right = int64_t{rect.x} + rect.width;
  const int64_t bottom = int64_t{rect.y} + rect.height;
  if (right > width || bottom > height) return false;

  if (shape == DisplayShape::kRect) {
    return rect.x >= margin && rect.y >= margin && right <= width - margin &&
           bottom <= height - margin;
  }

  const int64_t diameter = std::min(width, height) - int64_t{2} * margin;
  const std::array<int64_t, 2> x_edges = {rect.x, right};
  const std::array<int64_t, 2> y_edges = {rect.y, bottom};
  for (const int64_t x : x_edges) {
    for (const int64_t y : y_edges) {
      const int64_t dx = 2 * x - width;
      const int64_t dy = 2 * y - height;
      if (dx * dx + dy * dy > diameter * diameter) return false;
    }
  }
  return true;
}

// Brute force is deliberately limited to small displays. No chord calculation
// is shared with the production implementation.
HorizontalSpan oracleSpan(uint16_t width, uint16_t height, DisplayShape shape,
                          uint16_t margin, int32_t y, int32_t band_height) {
  HorizontalSpan widest{0, 0};
  for (int32_t x = 0; x < width; ++x) {
    for (int32_t span_width = 1; span_width <= width - x; ++span_width) {
      if (span_width > widest.width &&
          oracleContains(width, height, shape, margin,
                         {x, y, span_width, band_height})) {
        widest = {x, span_width};
      }
    }
  }
  return widest;
}

void assertSpan(const HorizontalSpan& expected, const HorizontalSpan& actual) {
  TEST_ASSERT_EQUAL_INT32(expected.x, actual.x);
  TEST_ASSERT_EQUAL_INT32(expected.width, actual.width);
}

void assertMaximalSpan(const SafeArea& area, int32_t y, int32_t height) {
  const HorizontalSpan span = area.spanForBand(y, height);
  if (span.width == 0) {
    TEST_ASSERT_EQUAL_INT32(0, span.x);
    return;
  }
  TEST_ASSERT_TRUE(area.contains({span.x, y, span.width, height}));
  TEST_ASSERT_FALSE(area.contains({span.x - 1, y, span.width + 1, height}));
  TEST_ASSERT_FALSE(area.contains({span.x, y, span.width + 1, height}));
}

void assertContainsMatchesOracle(uint16_t width, uint16_t height,
                                 DisplayShape shape, uint16_t margin,
                                 const SafeArea& area, const Rect& rect) {
  const bool expected = oracleContains(width, height, shape, margin, rect);
  const bool actual = area.contains(rect);
  if (actual != expected) {
    char detail[128];
    std::snprintf(detail, sizeof(detail),
                  "display=%u x %u shape=%u margin=%u rect=(%ld,%ld,%ld,%ld)",
                  static_cast<unsigned>(width), static_cast<unsigned>(height),
                  static_cast<unsigned>(shape), static_cast<unsigned>(margin),
                  static_cast<long>(rect.x), static_cast<long>(rect.y),
                  static_cast<long>(rect.width), static_cast<long>(rect.height));
    TEST_ASSERT_EQUAL_MESSAGE(expected, actual, detail);
  }
}

void testActualUnityVersion() {
  // The v2.7.0 tag has stale package metadata, so inspect the actual header.
  TEST_ASSERT_EQUAL_UINT(2, UNITY_VERSION_MAJOR);
  TEST_ASSERT_EQUAL_UINT(7, UNITY_VERSION_MINOR);
  TEST_ASSERT_EQUAL_UINT(0, UNITY_VERSION_BUILD);
}

void testRectangularProfiles() {
  for (const uint16_t size : {uint16_t{240}, uint16_t{360}, uint16_t{466}}) {
    const SafeArea area(size, size, DisplayShape::kRect, 10);
    TEST_ASSERT_TRUE(area.isValid());
    TEST_ASSERT_TRUE(area.contains({10, 10, size - 20, size - 20}));
    assertSpan({10, size - 20}, area.spanForBand(10, size - 20));
    assertSpan({0, 0}, area.spanForBand(9, 1));
    assertSpan({0, 0}, area.spanForBand(size - 10, 1));
    TEST_ASSERT_FALSE(area.contains({9, 10, size - 20, 1}));
    TEST_ASSERT_FALSE(area.contains({10, 10, size - 19, 1}));
    assertMaximalSpan(area, 10, size - 20);
  }

  const SafeArea tall(240, 360, DisplayShape::kRect, 10);
  TEST_ASSERT_TRUE(tall.contains({10, 10, 220, 340}));
  assertSpan({10, 220}, tall.spanForBand(10, 340));
  assertSpan({0, 0}, tall.spanForBand(350, 1));
}

void testKnownRoundSpans() {
  const SafeArea small(240, 240, DisplayShape::kRound, 10);
  const SafeArea medium(360, 360, DisplayShape::kRound, 15);
  const SafeArea large(466, 466, DisplayShape::kRound, 20);
  TEST_ASSERT_TRUE(small.isValid());
  TEST_ASSERT_TRUE(medium.isValid());
  TEST_ASSERT_TRUE(large.isValid());
  assertSpan({11, 218}, small.spanForBand(110, 20));
  assertSpan({16, 328}, medium.spanForBand(165, 30));
  assertSpan({21, 424}, large.spanForBand(223, 20));
  assertMaximalSpan(small, 110, 20);
  assertMaximalSpan(medium, 165, 30);
  assertMaximalSpan(large, 223, 20);
  assertSpan({0, 0}, small.spanForBand(10, 1));
  assertSpan({0, 0}, small.spanForBand(229, 1));
  assertSpan({0, 0}, small.spanForBand(0, 240));
}

void testTangencyAndWholeRectangleCorners() {
  // A centred 3:4:5 rectangle has four corners exactly on the circle.
  const SafeArea small(240, 240, DisplayShape::kRound, 0);
  TEST_ASSERT_TRUE(small.contains({48, 24, 144, 192}));
  TEST_ASSERT_FALSE(small.contains({47, 24, 145, 192}));
  TEST_ASSERT_FALSE(small.contains({48, 24, 145, 192}));
  TEST_ASSERT_FALSE(small.contains({48, 23, 144, 193}));
  TEST_ASSERT_FALSE(small.contains({48, 24, 144, 193}));
  assertSpan({48, 144}, small.spanForBand(24, 192));
  assertMaximalSpan(small, 24, 192);

  const SafeArea medium(360, 360, DisplayShape::kRound, 0);
  TEST_ASSERT_TRUE(medium.contains({72, 36, 216, 288}));
  const SafeArea large(466, 466, DisplayShape::kRound, 3);
  TEST_ASSERT_TRUE(large.contains({95, 49, 276, 368}));
  TEST_ASSERT_FALSE(large.contains({94, 49, 277, 368}));

  // Checking a centre point or centre row alone would accept these corners.
  TEST_ASSERT_FALSE(small.contains({0, 119, 240, 2}));
  TEST_ASSERT_FALSE(small.contains({1, 1, 238, 238}));
}

void testOddAndNonSquareDisplays() {
  const SafeArea odd(5, 5, DisplayShape::kRound, 0);
  assertSpan({1, 3}, odd.spanForBand(2, 1));
  TEST_ASSERT_TRUE(odd.contains({1, 2, 3, 1}));
  assertMaximalSpan(odd, 2, 1);

  const SafeArea wide(8, 6, DisplayShape::kRound, 0);
  assertSpan({2, 4}, wide.spanForBand(2, 2));
  assertMaximalSpan(wide, 2, 2);
  const SafeArea tall(6, 8, DisplayShape::kRound, 0);
  assertSpan({1, 4}, tall.spanForBand(3, 2));
  assertMaximalSpan(tall, 3, 2);

  const SafeArea offset(9, 7, DisplayShape::kRound, 1);
  assertSpan({3, 3}, offset.spanForBand(3, 1));
  assertMaximalSpan(offset, 3, 1);
  const SafeArea mixed(9, 8, DisplayShape::kRound, 1);
  assertSpan({2, 5}, mixed.spanForBand(3, 2));
  assertMaximalSpan(mixed, 3, 2);

  const SafeArea tiny(1, 1, DisplayShape::kRound, 0);
  TEST_ASSERT_TRUE(tiny.isValid());
  assertSpan({0, 0}, tiny.spanForBand(0, 1));
  TEST_ASSERT_FALSE(tiny.contains({0, 0, 1, 1}));
  const SafeArea tiny_rect(1, 1, DisplayShape::kRect, 0);
  TEST_ASSERT_TRUE(tiny_rect.contains({0, 0, 1, 1}));
}

void testInvalidGeometry() {
  for (const auto shape : kShapes) {
    for (const SafeArea area : {
             SafeArea(0, 240, shape, 0), SafeArea(240, 0, shape, 0),
             SafeArea(240, 240, shape, 120), SafeArea(240, 240, shape, 121),
             SafeArea(240, 360, shape, 65535)}) {
      TEST_ASSERT_FALSE(area.isValid());
      assertSpan({0, 0}, area.spanForBand(0, 1));
      TEST_ASSERT_FALSE(area.contains({0, 0, 1, 1}));
    }
  }
  const SafeArea unknown(240, 240, static_cast<DisplayShape>(255), 0);
  TEST_ASSERT_FALSE(unknown.isValid());
  assertSpan({0, 0}, unknown.spanForBand(119, 2));
  TEST_ASSERT_FALSE(unknown.contains({119, 119, 2, 2}));
}

void testMalformedAndOverflowProneInputs() {
  constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
  constexpr int32_t kMax = std::numeric_limits<int32_t>::max();
  constexpr std::array<Rect, 17> kInvalidRects = {{
      {-1, 120, 1, 1}, {120, -1, 1, 1}, {120, 120, 0, 1},
      {120, 120, 1, 0}, {120, 120, -1, 1}, {120, 120, 1, -1},
      {240, 120, 1, 1}, {120, 240, 1, 1}, {239, 120, 2, 1},
      {120, 239, 1, 2}, {kMax, 120, 1, 1}, {120, kMax, 1, 1},
      {120, 120, kMax, 1}, {120, 120, 1, kMax},
      {kMin, 120, kMax, 1}, {120, kMin, 1, kMax},
      {kMax, kMax, kMax, kMax},
  }};
  constexpr std::array<std::array<int32_t, 2>, 9> kInvalidBands = {{
      {-1, 1}, {120, 0}, {120, -1}, {240, 1}, {239, 2},
      {kMax, 1}, {120, kMax}, {kMin, kMax}, {kMax, kMax},
  }};
  for (const auto shape : kShapes) {
    const SafeArea area(240, 240, shape, 0);
    for (const auto& rect : kInvalidRects) {
      TEST_ASSERT_FALSE(area.contains(rect));
      assertContainsMatchesOracle(240, 240, shape, 0, area, rect);
    }
    for (const auto& band : kInvalidBands) {
      assertSpan({0, 0}, area.spanForBand(band[0], band[1]));
    }
  }
}

void testMaximumDimensions() {
  const SafeArea rectangle(65535, 65535, DisplayShape::kRect, 0);
  TEST_ASSERT_TRUE(rectangle.isValid());
  TEST_ASSERT_TRUE(rectangle.contains({0, 0, 65535, 65535}));
  assertSpan({0, 65535}, rectangle.spanForBand(0, 65535));
  assertMaximalSpan(rectangle, 0, 65535);

  const SafeArea round(65535, 65535, DisplayShape::kRound, 0);
  TEST_ASSERT_TRUE(round.isValid());
  assertSpan({1, 65533}, round.spanForBand(32767, 1));
  assertMaximalSpan(round, 32767, 1);
  for (const Rect rect : {
           Rect{0, 0, 65535, 65535}, Rect{0, 32767, 65535, 1},
           Rect{1, 32767, 65533, 1}, Rect{32767, 32767, 1, 1},
           Rect{65535, 0, 1, 1}, Rect{65534, 0, 2, 1}}) {
    assertContainsMatchesOracle(65535, 65535, DisplayShape::kRound, 0, round,
                                rect);
  }

  const SafeArea eroded(65535, 65535, DisplayShape::kRect, 32767);
  TEST_ASSERT_TRUE(eroded.isValid());
  assertSpan({32767, 1}, eroded.spanForBand(32767, 1));
  TEST_ASSERT_TRUE(eroded.contains({32767, 32767, 1, 1}));
  TEST_ASSERT_FALSE(SafeArea(65535, 65535, DisplayShape::kRound, 32768).isValid());
  const SafeArea thin(65535, 1, DisplayShape::kRound, 0);
  TEST_ASSERT_TRUE(thin.isValid());
  assertSpan({0, 0}, thin.spanForBand(0, 1));
}

void testExhaustiveSmallGeometryAgainstIndependentOracle() {
  for (const auto shape : kShapes) {
    for (uint16_t width = 1; width <= 9; ++width) {
      for (uint16_t height = 1; height <= 9; ++height) {
        for (uint16_t margin = 0; margin <= std::min(width, height); ++margin) {
          const SafeArea area(width, height, shape, margin);
          TEST_ASSERT_EQUAL(oracleValid(width, height, shape, margin),
                            area.isValid());
          for (int32_t y = 0; y < height; ++y) {
            for (int32_t band_height = 1; band_height <= height - y;
                 ++band_height) {
              assertSpan(oracleSpan(width, height, shape, margin, y, band_height),
                         area.spanForBand(y, band_height));
              assertMaximalSpan(area, y, band_height);
              for (int32_t x = 0; x < width; ++x) {
                for (int32_t rect_width = 1; rect_width <= width - x;
                     ++rect_width) {
                  assertContainsMatchesOracle(
                      width, height, shape, margin, area,
                      {x, y, rect_width, band_height});
                }
              }
            }
          }
        }
      }
    }
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testActualUnityVersion);
  RUN_TEST(testRectangularProfiles);
  RUN_TEST(testKnownRoundSpans);
  RUN_TEST(testTangencyAndWholeRectangleCorners);
  RUN_TEST(testOddAndNonSquareDisplays);
  RUN_TEST(testInvalidGeometry);
  RUN_TEST(testMalformedAndOverflowProneInputs);
  RUN_TEST(testMaximumDimensions);
  RUN_TEST(testExhaustiveSmallGeometryAgainstIndependentOracle);
  return UNITY_END();
}
