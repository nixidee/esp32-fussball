// Private display-port integration for the permanent screenshot flush hook.
#pragma once

#include "display_port.h"
#include "lvgl.h"

namespace display {

esp_err_t initScreenshotCapture(lv_display_t* display,
                                const hw::DisplayProfile& profile);

}  // namespace display
