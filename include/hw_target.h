// Selects the hardware target from the env's build flag and checks its pins
// at compile time (ADR-004). Include this header, never a profile directly.
// Provides hw::kBoard, hw::kDisplay, hw::kTarget.

#pragma once

#if defined(FUSSBALL_TARGET_XIAO_ESP32C6_GC9A01) + \
        defined(FUSSBALL_TARGET_WAVESHARE_ESP32S3_LCD128) != 1
#error "Exactly one FUSSBALL_TARGET_<NAME> must be set in the env's build_flags (platformio.ini)"
#endif

#if defined(FUSSBALL_TARGET_XIAO_ESP32C6_GC9A01)
#include "targets/xiao_esp32c6_gc9a01.h"
#elif defined(FUSSBALL_TARGET_WAVESHARE_ESP32S3_LCD128)
#include "targets/waveshare_esp32s3_lcd128.h"
#endif

#include "esp_bit_defs.h"  // BITn used by the S3 GPIO masks
#include "soc/soc_caps.h"

namespace hw {

static_assert(kTarget.wiring.sclk != kNoPin && kTarget.wiring.mosi != kNoPin &&
                  kTarget.wiring.dc != kNoPin,
              "display wiring needs SCLK, MOSI and DC");
static_assert(kTarget.inputs.size() <= kMaxInputs, "too many inputs");
static_assert(wiringPinsValid(kTarget.wiring, SOC_GPIO_VALID_OUTPUT_GPIO_MASK),
              "display wiring uses a GPIO that does not exist or cannot drive "
              "an output on this chip");
static_assert(inputPinsValid(kTarget.inputs, SOC_GPIO_VALID_GPIO_MASK),
              "an input uses a GPIO that does not exist on this chip");
static_assert(!hasDuplicatePin(usedPins(kTarget)),
              "a GPIO is used twice (display wiring / inputs)");
static_assert(!usesReservedPin(usedPins(kTarget), kBoard.reserved_pins),
              "a GPIO reserved by the board is used for display wiring or an "
              "input");
static_assert(kDisplay.width > 0 && kDisplay.height > 0,
              "display resolution must be positive");
static_assert(kDisplay.spi_mode <= 3, "SPI mode must be 0..3");
static_assert(kTarget.wiring.spi_clock_hz > 0, "SPI clock must be positive");

}  // namespace hw
