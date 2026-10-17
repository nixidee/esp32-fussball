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
  kToggleDirection,
  kShowIp,        // from the Web UI only; no input maps to it
  kAutomaticNext  // Demo only; no physical input maps to it
};
// One UI command carries the full version of the model used to produce it.
// The existing event bus and controller FIFO both transport this same word.
constexpr uint64_t actionPayload(Action action, uint32_t generation) {
  return static_cast<uint64_t>(generation) << 32 |
         static_cast<uint32_t>(action);
}
constexpr uint32_t actionGeneration(uint64_t payload) {
  return static_cast<uint32_t>(payload >> 32);
}
constexpr Action actionFromPayload(uint64_t payload) {
  const uint32_t action = static_cast<uint32_t>(payload);
  return action <= static_cast<uint32_t>(Action::kAutomaticNext)
             ? static_cast<Action>(action)
             : Action::kNone;
}
class InputPolicy {
 public:
  // Pure monotonic state machine: bit (1 << RawInput) per emitted event.
  uint8_t update(bool active, int64_t now, bool double_press,
                 uint16_t repeat_ms);
  // Discard a begun gesture. A held or debouncing key stays silent until
  // its release has debounced; that release emits no action.
  void cancel();

 private:
  bool raw_ = false, stable_ = false, long_sent_ = false, pending_ = false,
       blocked_ = false;
  int64_t edge_ = 0, pressed_ = 0, repeat_ = 0, released_ = 0;
};
// Action of one input event for the input's role (OQ-141…OQ-143); `down` is
// the current direction of a Scroll input.
Action mapInput(cfg::InputRole role, RawInput event, bool down);
}  // namespace ui
