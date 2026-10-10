#pragma once
#include <cstddef>

#include "esp_err.h"
#include "hw_profile.h"
namespace ui {
esp_err_t init(const hw::DisplayProfile& display, std::size_t inputs);
void input(std::size_t index, bool active);
void poll();
void resetProgress(uint16_t remaining_s);
bool healthy();
}  // namespace ui
