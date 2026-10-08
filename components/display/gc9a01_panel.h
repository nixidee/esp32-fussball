// GC9A01 panel driver for esp_lcd (private to the display component).
//
// Clean rewrite of the command sequence of an earlier ESP-IDF driver by the
// same author, verified on hardware (ESP32-S3 + GC9A01). Implements the generic
// esp_lcd_panel_t interface, so the rest of the firmware only uses the
// esp_lcd_panel_* calls.

#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

namespace display {

struct Gc9a01Config {
  int reset_gpio;  // -1: no reset line, software reset is used
  bool bgr_order;  // MADCTL BGR bit
};

// Creates the panel object. Does not talk to the panel yet; call
// esp_lcd_panel_reset() and esp_lcd_panel_init() afterwards.
esp_err_t newGc9a01Panel(esp_lcd_panel_io_handle_t io,
                         const Gc9a01Config& config,
                         esp_lcd_panel_handle_t* ret_panel);

}  // namespace display
