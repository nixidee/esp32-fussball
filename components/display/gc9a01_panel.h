// GC9A01 panel driver for esp_lcd (private to the display component).
//
// Clean rewrite of the command sequence of an earlier ESP-IDF driver by the
// same author, verified on hardware (ESP32-S3 + GC9A01). Implements the generic
// esp_lcd_panel_t interface, so the rest of the firmware only uses the
// esp_lcd_panel_* calls.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_types.h"

namespace display {

// Outcome of one draw_bitmap call, reported to the draw observer.
enum class DrawResult : uint8_t {
  // Queued; the panel IO signals completion when the transfer is done.
  kSubmitted,
  // Failed; every queued transfer was collected, the buffer is free again and
  // no completion will be signalled for this call.
  kFailedBufferFree,
  // Failed and the queue could not be drained: a transfer may still read the
  // buffer.
  kFailedBufferUnknown,
};

// Called synchronously inside draw_bitmap, i.e. in the caller's task.
using DrawObserver = void (*)(DrawResult result, void* ctx);

struct Gc9a01Config {
  int reset_gpio;  // -1: no reset line, software reset is used
  bool bgr_order;  // MADCTL BGR bit
};

// Creates the panel object. Does not talk to the panel yet; call
// esp_lcd_panel_reset() and esp_lcd_panel_init() afterwards.
esp_err_t newGc9a01Panel(esp_lcd_panel_io_handle_t io,
                         const Gc9a01Config& config,
                         esp_lcd_panel_handle_t* ret_panel);

// Installs the draw observer (nullptr removes it). Without an observer a
// failed draw only returns the error. The caller serialises this with the
// draw calls (LVGL lock).
void setGc9a01DrawObserver(esp_lcd_panel_handle_t panel, DrawObserver observer,
                           void* ctx);

}  // namespace display
