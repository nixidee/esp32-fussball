// Target: Seeed XIAO ESP32-C6 + external GC9A01 module + 3 x TTP223B
// (docs/HARDWARE.md; wiring verified on the device).
// ESP-IDF settings: targets/xiao_esp32c6_gc9a01.sdkconfig.defaults (ADR-010).

#pragma once

#include "boards/xiao_esp32c6.h"
#include "displays/gc9a01_240_round.h"

namespace hw {

// TTP223B with factory jumpers: active high (verified on the device).
inline constexpr InputPin kXiaoEsp32C6Gc9a01Inputs[] = {
    {.pin = 19, .active_high = true},  // touch 1, D8
    {.pin = 20, .active_high = true},  // touch 2, D9
    {.pin = 18, .active_high = true},  // touch 3, D10
};

inline constexpr TargetProfile kTarget{
    .name = "xiao_esp32c6_gc9a01",
    .wiring =
        {
            .sclk = 23,  // D5
            .mosi = 22,  // D4
            .cs = 21,    // D3
            .dc = 2,     // D2
            .reset = 1,  // D1
            .backlight = kNoPin,  // module has no BL pin
            .backlight_active_high = true,
            // Verified on the S3 and on the C6 with this module.
            .spi_clock_hz = 40'000'000,
        },
    .inputs = kXiaoEsp32C6Gc9a01Inputs,
};

}  // namespace hw
