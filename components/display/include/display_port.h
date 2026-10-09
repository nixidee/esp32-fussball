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

// Call once at boot. On success the panel shows black, LVGL is running and
// supervised. Profile and wiring must have static storage duration.
//
// Failure handling: a bring-up failure, repeated or undrainable draw failures
// and an LVGL stall (task watchdog) restart the device with the reason in the
// panic output. After cfg::kAbnormalResetLimit consecutive abnormal resets
// init() leaves the hardware untouched and returns ESP_ERR_INVALID_STATE
// (headless) until a power cycle or a normal restart. ESP_ERR_INVALID_STATE
// also means "already initialised".
esp_err_t init(const hw::DisplayProfile& profile,
               const hw::DisplayWiring& wiring);

// Takes the LVGL lock. timeout_ms 0 waits forever; never hold the lock for
// long work (the LVGL supervision restarts the device after the task
// watchdog timeout). Returns false on timeout.
bool lock(uint32_t timeout_ms);
void unlock();

// Logs the LVGL memory pool usage (takes the lock itself with a short
// timeout and skips the log if LVGL is busy). Does nothing if init() has not
// succeeded.
void logMemory();

}  // namespace display
