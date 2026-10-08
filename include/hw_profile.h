// Hardware profile types and compile-time pin checks (ADR-004).
//
// A target combines one board (boards/), one display (displays/) and the
// wiring between them (targets/). The selected target is included through
// hw_target.h; nothing else includes the profile headers directly.
// Everything here is constexpr: no RAM, flash only for strings that are used.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hw {

// Marks a signal that is not connected on this target.
inline constexpr int kNoPin = -1;

// Upper bound for TargetProfile::inputs (size of the check buffer below).
inline constexpr std::size_t kMaxInputs = 8;

enum class DisplayController : uint8_t { kGc9a01 };
enum class DisplayShape : uint8_t { kRound, kRect };

// MCU board: facts that do not depend on the attached display.
struct BoardProfile {
  const char* name;
  // GPIOs that must never be used for display wiring or inputs.
  std::span<const int> reserved_pins;
  // RF switch between on-board and external antenna (docs/HARDWARE.md).
  bool has_antenna_switch;
  int antenna_enable_pin;
  bool antenna_enable_active_high;
  int antenna_select_pin;
  bool antenna_external_level_high;
};

// Display module: facts that do not depend on the board.
struct DisplayProfile {
  const char* name;
  DisplayController controller;
  DisplayShape shape;
  uint16_t width;
  uint16_t height;
  uint8_t spi_mode;
  // Panel colour order is BGR (MADCTL BGR bit) instead of RGB.
  bool bgr_order;
  // Panel needs inverted colour data (INVON) to show correct colours.
  bool invert_colors;
};

// Which board GPIO drives which display signal.
struct DisplayWiring {
  int sclk;
  int mosi;
  int cs;
  int dc;
  int reset;
  int backlight;  // kNoPin: backlight not switchable (always on)
  bool backlight_active_high;
  uint32_t spi_clock_hz;
};

struct InputPin {
  int pin;
  bool active_high;
};

struct TargetProfile {
  const char* name;
  DisplayWiring wiring;
  // Input 1, 2, 3 ... in this order (action mapping: docs/UI.md).
  std::span<const InputPin> inputs;
};

// ---- Compile-time checks (used by hw_target.h) ----

inline constexpr std::array<int, 6> wiringPins(const DisplayWiring& w) {
  return {w.sclk, w.mosi, w.cs, w.dc, w.reset, w.backlight};
}

// All GPIOs the target drives or reads, kNoPin skipped.
struct UsedPins {
  std::array<int, 6 + kMaxInputs> pin{};
  std::size_t size = 0;
};

inline constexpr UsedPins usedPins(const TargetProfile& t) {
  UsedPins used;
  for (int p : wiringPins(t.wiring)) {
    if (p != kNoPin) used.pin[used.size++] = p;
  }
  for (const InputPin& in : t.inputs) {
    if (in.pin != kNoPin) used.pin[used.size++] = in.pin;
  }
  return used;
}

inline constexpr bool hasDuplicatePin(const UsedPins& used) {
  for (std::size_t i = 0; i < used.size; ++i) {
    for (std::size_t j = i + 1; j < used.size; ++j) {
      if (used.pin[i] == used.pin[j]) return true;
    }
  }
  return false;
}

inline constexpr bool usesReservedPin(const UsedPins& used,
                                      std::span<const int> reserved) {
  for (std::size_t i = 0; i < used.size; ++i) {
    for (int r : reserved) {
      if (used.pin[i] == r) return true;
    }
  }
  return false;
}

// mask: bit n set = GPIO n usable (SOC_GPIO_VALID_*_MASK of the chip).
inline constexpr bool isGpioIn(int pin, uint64_t mask) {
  return pin >= 0 && pin < 64 && ((mask >> pin) & 1U) != 0;
}

inline constexpr bool wiringPinsValid(const DisplayWiring& w,
                                      uint64_t output_mask) {
  for (int p : wiringPins(w)) {
    if (p != kNoPin && !isGpioIn(p, output_mask)) return false;
  }
  return true;
}

inline constexpr bool inputPinsValid(std::span<const InputPin> inputs,
                                     uint64_t input_mask) {
  for (const InputPin& in : inputs) {
    if (!isGpioIn(in.pin, input_mask)) return false;
  }
  return true;
}

}  // namespace hw
