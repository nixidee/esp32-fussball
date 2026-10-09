#include "event_admission.h"

namespace events {

namespace {

uint32_t pendingBit(Event event) {
  return uint32_t{1} << static_cast<uint32_t>(event);
}

}  // namespace

const char* eventName(Event event) {
  switch (event) {
    case Event::kSettingsChanged: return "settings changed";
    case Event::kNetworkState: return "network state";
    case Event::kDataUpdated: return "data updated";
    case Event::kOtaState: return "OTA state";
    case Event::kUiAction: return "UI action";
  }
  return "unknown";
}

bool Admission::admit(Event event) {
  if (isStateEvent(event)) {
    const uint32_t bit = pendingBit(event);
    return (pending_.fetch_or(bit, std::memory_order_acq_rel) & bit) == 0;
  }
  uint32_t taken = ui_in_flight_.load(std::memory_order_relaxed);
  do {
    if (taken >= cfg::kEventUiActionSlots) return false;
  } while (!ui_in_flight_.compare_exchange_weak(
      taken, taken + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
  return true;
}

void Admission::release(Event event) {
  if (isStateEvent(event)) {
    pending_.fetch_and(~pendingBit(event), std::memory_order_acq_rel);
    return;
  }
  // Never below zero, even after an unmatched release.
  uint32_t taken = ui_in_flight_.load(std::memory_order_relaxed);
  do {
    if (taken == 0) return;
  } while (!ui_in_flight_.compare_exchange_weak(
      taken, taken - 1, std::memory_order_acq_rel, std::memory_order_relaxed));
}

}  // namespace events
