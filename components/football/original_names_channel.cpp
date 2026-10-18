#include "original_names_channel.h"

#include <algorithm>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "original_names_channel_state.h"

namespace football::originals {
namespace {
detail::Channel channel;
StaticEventGroup_t event_storage;
EventGroupHandle_t events = nullptr;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
#ifdef ESP_PLATFORM
static_assert(sizeof(channel) + sizeof(event_storage) + sizeof(events) +
                  sizeof(mutex) <=
              512);
#endif
constexpr EventBits_t kData = 1, kSpace = 2;
class Lock {
 public:
  Lock() { portENTER_CRITICAL(&mutex); }
  ~Lock() { portEXIT_CRITICAL(&mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};
uint64_t nowMs() { return esp_timer_get_time() / 1000; }
void changed(EventBits_t bits = kData | kSpace) {
  xEventGroupSetBits(events, bits);
}
void waitChange(EventBits_t bits, uint64_t remaining) {
  const auto slice = std::min<uint64_t>(remaining, cfg::kProviderWaitSliceMs);
  const auto ticks = std::max<TickType_t>(1, pdMS_TO_TICKS(slice));
  xEventGroupWaitBits(events, bits, pdTRUE, pdFALSE, ticks);
}
}  // namespace

esp_err_t begin(const settings::Source& source, uint32_t& query) {
  query = 0;
  if (!source.complete() ||
      static_cast<unsigned>(source.provider) >= cfg::kProviderCount)
    return ESP_ERR_INVALID_ARG;
  {
    Lock lock;
    if (!events) events = xEventGroupCreateStatic(&event_storage);
    if (!events) return ESP_ERR_NO_MEM;
    if (!channel.begin(source, nowMs())) return ESP_ERR_INVALID_STATE;
    query = channel.query();
  }
  changed();
  return ESP_OK;
}
uint64_t deadlineMs(uint32_t query) {
  Lock lock;
  return channel.deadline(query);
}
bool claim(uint32_t query, settings::Source& source) {
  Lock lock;
  channel.expire(nowMs());
  return channel.claim(query, source);
}
bool claimNext(uint32_t& query, settings::Source& source) {
  Lock lock;
  channel.expire(nowMs());
  const auto pending = channel.query();
  const bool claimed = channel.claim(pending, source);
  query = claimed ? pending : 0;
  return claimed;
}
bool active(uint32_t query) {
  Lock lock;
  channel.expire(nowMs());
  return channel.active(query);
}
bool write(uint32_t query, std::string_view bytes) {
  while (!bytes.empty()) {
    uint64_t deadline = 0;
    std::size_t written = 0;
    {
      Lock lock;
      channel.expire(nowMs());
      if (!channel.active(query)) return false;
      written = channel.write(query, bytes);
      deadline = channel.deadline(query);
    }
    if (written) {
      bytes.remove_prefix(written);
      changed(kData);
    } else {
      const auto now = nowMs();
      if (now < deadline) waitChange(kSpace, deadline - now);
    }
  }
  return active(query);
}
Read read(uint32_t query, char* output, std::size_t capacity,
          uint32_t wait_ms) {
  if (!output || !capacity) return {0, State::kFailed, ESP_ERR_INVALID_ARG};
  const auto until = nowMs() + wait_ms;
  for (;;) {
    uint64_t deadline = 0;
    Read result;
    {
      Lock lock;
      channel.expire(nowMs());
      result = channel.read(query, output, capacity);
      deadline = channel.deadline(query);
    }
    if (result.bytes) changed(kSpace);
    if (result.bytes ||
        (result.state != State::kPending && result.state != State::kStreaming))
      return result;
    const auto now = nowMs();
    const auto end = std::min(until, deadline);
    if (now >= end) return result;
    waitChange(kData, end - now);
  }
}
void cancel(uint32_t query) {
  {
    Lock lock;
    channel.cancel(query);
  }
  if (events) changed();
}
void complete(uint32_t query, esp_err_t error) {
  {
    Lock lock;
    channel.expire(nowMs());
    channel.complete(query, error);
  }
  if (events) changed();
}
void release(uint32_t query) { cancel(query); }
}  // namespace football::originals
