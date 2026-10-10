#pragma once
#include <cstdint>

#include "app_config.h"

namespace ui {
enum class RawInput : uint8_t { kShort, kLong, kDouble, kRelease, kRepeat };
enum class Action : uint8_t {
  kNone,
  kNext,
  kPrevious,
  kDefault,
  kUp,
  kDown,
  kToggleDirection
};
class InputPolicy {
 public:
  // Pure monotonic state machine: bit (1 << RawInput) per emitted event.
  uint8_t update(bool active, int64_t now, bool double_press,
                 uint16_t repeat_ms);

 private:
  bool raw_ = false, stable_ = false, long_sent_ = false, pending_ = false;
  int64_t edge_ = 0, pressed_ = 0, repeat_ = 0, released_ = 0;
};
Action mapInput(unsigned inputs, unsigned index, RawInput event, bool down);
}  // namespace ui
