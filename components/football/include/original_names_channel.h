// One bounded, correlated stream between the existing provider worker and HTTP
// caller. The channel owns bytes and lifetime, never provider work or JSON.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "esp_err.h"
#include "settings_model.h"

namespace football::originals {
enum class State : uint8_t {
  kStale,
  kPending,
  kStreaming,
  kComplete,
  kFailed,
  kCancelled
};
struct Read {
  std::size_t bytes = 0;
  State state = State::kStale;
  esp_err_t error = ESP_ERR_INVALID_STATE;
};
// No heap, task or additional client. A busy channel returns INVALID_STATE.
// The existing operation deadline starts here and bounds every channel wait.
esp_err_t begin(const settings::Source& source, uint32_t& query);
// Absolute esp_timer milliseconds, or zero for a stale query. The existing
// provider transport can share this deadline instead of starting a new one.
uint64_t deadlineMs(uint32_t query);
// Claim also hands a cancelled-before-start query to its worker, which must
// acknowledge it with complete(). There is exactly one producer.
bool claim(uint32_t query, settings::Source& source);
bool claimNext(uint32_t& query, settings::Source& source);
bool active(uint32_t query);
bool write(uint32_t query, std::string_view bytes);
Read read(uint32_t query, char* output, std::size_t capacity, uint32_t wait_ms);
// A disconnected HTTP consumer cancels immediately; the slot remains reserved
// until the worker acknowledges completion. No stale ID can affect a new query.
void cancel(uint32_t query);
void complete(uint32_t query, esp_err_t error);
// Release after a terminal response. Cancels an unfinished response, too.
void release(uint32_t query);
}  // namespace football::originals
