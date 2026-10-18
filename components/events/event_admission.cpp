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
    case Event::kTimeChanged: return "time changed";
  }
  return "unknown";
}

bool Admission::admit(Event event) {
  if (!isStateEvent(event)) return false;
  const uint32_t bit = pendingBit(event);
  return (pending_.fetch_or(bit, std::memory_order_acq_rel) & bit) == 0;
}
void Admission::release(Event event) {
  if (isStateEvent(event))
    pending_.fetch_and(~pendingBit(event), std::memory_order_acq_rel);
}
}  // namespace events
