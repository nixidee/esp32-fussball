// Pure screen resolver; no clock, repository, hardware driver or LVGL calls.
#pragma once
#include <array>

#include "settings_model.h"
namespace ui {
struct NavigationInput {
  bool configured, matchday, own_window, manual;
  std::size_t inputs;
  cfg::Screen selected;
};
struct NavigationResult {
  std::array<cfg::Screen, cfg::kScreenCount> available{};
  uint8_t count = 0;
  cfg::Screen default_screen = cfg::Screen::kCrest,
              selected = cfg::Screen::kCrest;
};
NavigationResult resolve(const settings::Model& model, NavigationInput input);
}  // namespace ui
