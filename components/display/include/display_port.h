// Display port (ADR-003): brings up SPI bus, panel controller and LVGL
// (esp_lvgl_port) for the display described by the hardware profile.
//
// LVGL runs in the esp_lvgl_port task. Every LVGL call from another task
// must be wrapped in lock() / unlock().

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "hw_profile.h"

namespace display {

// Call once at boot. On success the panel shows black and LVGL is running.
// On failure the display stays unusable until the next restart.
esp_err_t init(const hw::DisplayProfile& profile,
               const hw::DisplayWiring& wiring);

// Takes the LVGL lock. timeout_ms 0 waits forever. Returns false on timeout.
bool lock(uint32_t timeout_ms);
void unlock();

// Logs the LVGL memory pool usage (takes the lock itself). Does nothing if
// init() has not succeeded.
void logMemory();

}  // namespace display
