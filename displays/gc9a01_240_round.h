// Display: GC9A01 1.28" 240x240 round, SPI (docs/HARDWARE.md). Included only
// via a target. The controller init sequence lives in the display component
// (components/display/gc9a01_panel.cpp).

#pragma once

#include "hw_profile.h"

namespace hw {

inline constexpr DisplayProfile kDisplay{
    .name = "GC9A01 240x240 round",
    .controller = DisplayController::kGc9a01,
    .shape = DisplayShape::kRound,
    .width = 240,
    .height = 240,
    .spi_mode = 0,
    // BGR + inverted: verified on hardware with the ESP32-S3 and the
    // ESP32-C6 module.
    .bgr_order = true,
    .invert_colors = true,
};

}  // namespace hw
