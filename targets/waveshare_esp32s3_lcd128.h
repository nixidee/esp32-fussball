// Target: Waveshare ESP32-S3-LCD-1.28, built-in GC9A01, no inputs
// (docs/HARDWARE.md; pins verified in an earlier project on this board).
// ESP-IDF settings: targets/waveshare_esp32s3_lcd128.sdkconfig.defaults (ADR-010).

#pragma once

#include "boards/waveshare_esp32s3_lcd128.h"
#include "displays/gc9a01_240_round.h"

namespace hw {

inline constexpr TargetProfile kTarget{
    .name = "waveshare_esp32s3_lcd128",
    .wiring =
        {
            .sclk = 10,
            .mosi = 11,
            .cs = 9,
            .dc = 8,
            .reset = 12,
            .backlight = 40,
            .backlight_active_high = true,
            .spi_clock_hz = 40'000'000,
        },
    .inputs = {},  // BOOT button not reachable in the housing
};

}  // namespace hw
