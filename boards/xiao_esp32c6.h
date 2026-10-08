// Board: Seeed XIAO ESP32-C6 (docs/HARDWARE.md). Included only via a target.

#pragma once

#include "hw_profile.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32C6
#error "boards/xiao_esp32c6.h needs an ESP32-C6 build: check the env's board in platformio.ini"
#endif

namespace hw {

// 3, 14: RF switch (antenna driver only); 15: user LED, strapping;
// 4, 5, 8, 9: strapping; 12, 13: USB D-/D+ (console, USB-Serial/JTAG).
inline constexpr int kXiaoEsp32C6Reserved[] = {3, 14, 15, 4, 5, 8, 9, 12, 13};

// GPIOs on the header pads D0-D10 (docs/HARDWARE.md → Header map). Display
// wiring and inputs must use these: the SoC masks also accept flash GPIO24-30,
// JTAG pads 6/7 and GPIO10/11, which the fitted QFN32 package lacks.
inline constexpr int kXiaoEsp32C6HeaderPins[] = {0,  1,  2,  21, 22, 23,
                                                 16, 17, 19, 20, 18};

inline constexpr BoardProfile kBoard{
    .name = "Seeed XIAO ESP32-C6",
    .reserved_pins = kXiaoEsp32C6Reserved,
    .has_antenna_switch = true,
    .antenna_enable_pin = 3,  // LOW = RF switch enabled
    .antenna_enable_active_high = false,
    .antenna_select_pin = 14,  // LOW = on-board ceramic, HIGH = external U.FL
    .antenna_external_level_high = true,
};

}  // namespace hw
