#include "input_policy.h"

namespace ui {
uint8_t InputPolicy::update(bool active, int64_t now, bool doubles,
                            uint16_t repeat_ms) {
  uint8_t events = 0;
  auto emit = [&](RawInput event) {
    events |= 1u << static_cast<unsigned>(event);
  };
  if (raw_ != active) {
    raw_ = active;
    edge_ = now;
  }
  if (stable_ != raw_ && now - edge_ >= cfg::kInputDebounceMs) {
    stable_ = raw_;
    if (stable_) {
      pressed_ = repeat_ = now;
      long_sent_ = false;
    } else {
      emit(RawInput::kRelease);
      if (!long_sent_) {
        if (doubles && pending_ && now - released_ <= cfg::kInputDoubleMs) {
          pending_ = false;
          emit(RawInput::kDouble);
        } else if (doubles) {
          pending_ = true;
          released_ = now;
        } else
          emit(RawInput::kShort);
      }
    }
  }
  if (pending_ && now - released_ > cfg::kInputDoubleMs) {
    pending_ = false;
    emit(RawInput::kShort);
  }
  if (stable_ && now - pressed_ >= cfg::kInputLongMs) {
    if (!long_sent_) {
      emit(RawInput::kLong);
      pending_ = false;
    } else if (now - repeat_ >= repeat_ms)
      emit(RawInput::kRepeat);
    if (!long_sent_ || now - repeat_ >= repeat_ms) repeat_ = now;
    long_sent_ = true;
  }
  return events;
}
Action mapInput(cfg::InputRole role, RawInput event, bool down) {
  using cfg::InputRole;
  switch (role) {
    case InputRole::kMandatory:
      if (event == RawInput::kShort) return Action::kNext;
      if (event == RawInput::kDouble) return Action::kPrevious;
      if (event == RawInput::kLong) return Action::kDefault;
      return Action::kNone;
    case InputRole::kUp:
    case InputRole::kDown:
      if (event == RawInput::kShort || event == RawInput::kLong ||
          event == RawInput::kRepeat)
        return role == InputRole::kDown ? Action::kDown : Action::kUp;
      return Action::kNone;
    case InputRole::kScroll:
      if (event == RawInput::kRelease) return Action::kToggleDirection;
      if (event == RawInput::kLong || event == RawInput::kRepeat)
        return down ? Action::kDown : Action::kUp;
      return Action::kNone;
    case InputRole::kOff:
      break;
  }
  return Action::kNone;
}
}  // namespace ui
