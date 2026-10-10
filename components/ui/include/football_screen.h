#pragma once
#include <array>

#include "safe_area.h"
#include "settings_model.h"
namespace ui {
struct Row {
  char text[96]{};
  bool own = false;
};
struct ViewModel {
  cfg::Screen screen = cfg::Screen::kCrest;
  settings::ScreenStyle style;
  char title[64]{}, score[32]{}, minute[32]{}, teams[96]{}, highlight[96]{},
      status[64]{};
  std::array<Row, 9> rows{};
  uint8_t row_count = 0;
  char image[64]{}, badge[64]{};
  bool night = false, badge_bottom = false;
  uint32_t image_revision = 0;
};
bool render(const ViewModel& model, const geometry::SafeArea& safe_area);
}  // namespace ui
