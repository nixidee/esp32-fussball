#pragma once
#include <cstddef>

#include "app_config.h"
#include "esp_err.h"
#include "hw_profile.h"
namespace ui {
// Why the current screen is shown (Web UI overview, no text: the page
// translates the code).
enum class Reason : uint8_t { kSetup, kManual, kOwnMatch, kMatchday, kIdle };
struct Status {
  cfg::Screen screen = cfg::Screen::kCrest;
  Reason reason = Reason::kSetup;
};
// `backlight` false: the panel cannot dim, so night mode dims the colours.
esp_err_t init(const hw::DisplayProfile& display, std::size_t inputs,
               bool backlight);
// Draws the start screen once the display runs, before init(); the next
// poll() continues it. False: lock timeout or the screen did not fit.
bool showStartScreen(const hw::DisplayProfile& display, bool backlight);
void input(std::size_t index, bool active);
void poll();
bool healthy();
// Screen shown by the last poll(); readable from any task.
Status status();
const char* reasonName(Reason reason);
}  // namespace ui
