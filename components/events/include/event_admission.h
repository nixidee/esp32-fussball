// Event catalog and queue admission of the event bus (docs/ARCHITECTURE.md).
// Pure C++, independent of ESP-IDF; the esp_event loop lives in
// components/core (event_bus.h).
//
// State events are notifications without data: at most one per kind is
// queued, and the subscriber reads the current state from its owner. UI
// actions carry an action code and share a fixed number of queue slots. The
// queue is sized so that state events always find room.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "app_config.h"

namespace events {

// The enum value is the esp_event ID. State events come first; their value
// is also their pending bit.
enum class Event : uint8_t {
  kSettingsChanged,  // read settings::current()
  kNetworkState,     // producer: WiFi manager (P3)
  kDataUpdated,      // producer: repository (P4)
  kOtaState,         // producer: OTA (P3.7)
  kTimeChanged,      // read timekeeping::status() / localNow()
  kUiAction,         // payload: action code (input mapper, P5.4)
};

inline constexpr std::size_t kStateEventCount = 5;
inline constexpr std::size_t kEventCount = kStateEventCount + 1;
static_assert(static_cast<std::size_t>(Event::kUiAction) == kStateEventCount,
              "state events must precede the UI action");

constexpr bool isStateEvent(Event event) { return event != Event::kUiAction; }

// Queue entries needed so that no state event can ever be refused.
inline constexpr std::size_t kQueueLength =
    kStateEventCount + cfg::kEventUiActionSlots;

const char* eventName(Event event);

// Thread-safe; any task may call it.
class Admission {
 public:
  // True: the caller may post the event. A state event was not pending and
  // is now marked; a UI action found a free slot and took it. False: the
  // state event is already queued (coalesced) or all UI slots are taken.
  bool admit(Event event);

  // Undoes admit(). The dispatcher calls it before delivering the event, so
  // a change during delivery is posted again; a poster calls it after a
  // failed post.
  void release(Event event);

 private:
  std::atomic<uint32_t> pending_{0};
  std::atomic<uint32_t> ui_in_flight_{0};
};

}  // namespace events
