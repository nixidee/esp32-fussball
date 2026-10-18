#pragma once
#include <cstddef>

#include "app_config.h"
#include "esp_err.h"
#include "hw_profile.h"
#include "input_policy.h"
#include "screen_policy.h"
namespace ui {
// Why the current screen is shown (Web UI overview, no text: the page
// translates the code).
enum class Reason : uint8_t {
  kSetup,
  kManual,
  kOwnMatch,
  kMatchday,
  kIdle,
  kOtherGames,
  kBreak,
  kFrame
};
struct Status {
  screen::Type screen = screen::Type::kCrest;
  Reason reason = Reason::kSetup;
  bool starting = true;
  bool manual = false, frame = false;
  uint8_t phase = static_cast<uint8_t>(settings::screen_policy::Phase::kIdle);
  uint8_t automatic_entries = 0;
};
// Night mode controls only a connected backlight; colours remain unchanged.
esp_err_t init(const hw::DisplayProfile& display, std::size_t inputs,
               bool backlight);
// Draws the start screen once the display runs, before init(); the next
// poll() continues it. `backlight` enables the saved day/night PWM brightness.
// False: lock timeout or the screen did not fit.
bool showStartScreen(const hw::DisplayProfile& display, bool backlight);
// Physical inputs and admitted Web commands share this one bounded FIFO.
esp_err_t submit(Action, uint32_t expected_generation);
void input(std::size_t index, bool active);
void poll();
bool healthy();
// Screen shown by the last poll(); readable from any task.
Status status();
const char* reasonName(Reason reason);
}  // namespace ui
