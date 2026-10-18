// Event catalog and queue admission of the event bus (docs/ARCHITECTURE.md).
// Pure C++, independent of ESP-IDF; the esp_event loop lives in
// components/events (event_bus.h).
//
// State events are notifications without data: at most one per kind is
// queued, and subscribers read current state from its owner. UI commands use
// the controller's separate single FIFO and never enter this bus.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "app_config.h"

namespace events {

// The enum value is the esp_event ID. State events come first; their value
// is also their pending bit.
enum class Event : uint8_t {
  kSettingsChanged,  // read the needed settings snapshot
  kNetworkState,     // producer: WiFi manager (P3)
  kDataUpdated,      // producer: repository (P4)
  kOtaState,         // producer: OTA (P3.7)
  kTimeChanged,      // read timekeeping::status() / localNow()
};

inline constexpr std::size_t kStateEventCount = 5;
inline constexpr std::size_t kEventCount = kStateEventCount;
constexpr bool isStateEvent(Event event) {
  return static_cast<std::size_t>(event) < kEventCount;
}
inline constexpr std::size_t kQueueLength = kStateEventCount;

const char* eventName(Event event);

// Thread-safe; any task may call it.
class Admission {
 public:
  // True marks one not-yet-pending state kind. False coalesces a duplicate
  // or rejects an invalid kind; no action slots or payload allocations exist.
  bool admit(Event event);

  // Undoes admit(). The dispatcher calls it before delivering the event, so
  // a change during delivery is posted again; a poster calls it after a
  // failed post.
  void release(Event event);

 private:
  std::atomic<uint32_t> pending_{0};
};

}  // namespace events
