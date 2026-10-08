// Board: Waveshare ESP32-S3-LCD-1.28, non-touch (docs/HARDWARE.md). The
// display is built in; its wiring is in targets/waveshare_esp32s3_lcd128.h.
// Included only via a target.

#pragma once

#include "hw_profile.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "boards/waveshare_esp32s3_lcd128.h needs an ESP32-S3 build: check the env's board in platformio.ini"
#endif

namespace hw {

// Occupied by on-board parts: 0 BOOT button (not reachable in the housing),
// 1 battery ADC, 6/7 IMU QMI8658 SDA/SCL, 43/44 UART0 console (CH343).
inline constexpr int kWaveshareEsp32S3Lcd128Reserved[] = {0, 1, 6, 7, 43, 44};

inline constexpr BoardProfile kBoard{
    .name = "Waveshare ESP32-S3-LCD-1.28",
    .reserved_pins = kWaveshareEsp32S3Lcd128Reserved,
    .has_antenna_switch = false,
    .antenna_enable_pin = kNoPin,
    .antenna_enable_active_high = false,
    .antenna_select_pin = kNoPin,
    .antenna_external_level_high = false,
};

}  // namespace hw
