#pragma once

#include <cstddef>

#include "esp_err.h"
#include "football_model.h"
#include "provider_json.h"
#include "settings_model.h"
namespace football {
// HTTP status (0 = none received), body bytes read, the stage the request
// reached ("dns", "tcp", "tls", "http") and the socket's pending error after
// a failed connect, for logging only.
struct FetchResult {
  int status = 0;
  std::size_t bytes = 0;
  const char* stage = "";
  int socket_error = 0;
  // Local pre-send refusals, separate from an HTTP/provider error. Paused
  // during DNS/TLS means no HTTP send attempt and no request reservation.
  bool paused = false;
  esp_err_t admission_error = ESP_OK;
};
// Hooks around the first nonblocking HTTP transport write only. begin()
// admits/reserves the request and may transfer a short service lock; end()
// always releases it immediately after that one write. Both must be provided.
// A failed initial send attempt is still a counted attempt. Later writes and
// the complete response never call the hooks or consult the frame gate.
struct SendHooks {
  esp_err_t (*begin)(void* context) = nullptr;
  void (*end)(void* context) = nullptr;
  void* context = nullptr;
  bool pause_with_frame = true;
};
// 'body_limit': the most body bytes read (cfg::kProviderBodyBytes, the
// owner's selection lists cfg::kSelectionBodyBytes, ESPN match summaries
// cfg::kProviderSummaryBodyBytes); a longer response fails.
// 'optional': a response without 'array_key' is valid (no elements), e.g.
// a provider that leaves out an empty list. 'enough': set by the consumer
// (through its context) when it needs no further element; the response ends
// there with success and its rest is not read.
esp_err_t fetchJson(const char* url, const char* array_key,
                    cfg::Provider provider, const settings::Model& model,
                    uint32_t generation, JsonConsumer consumer, void* context,
                    std::size_t body_limit = cfg::kProviderBodyBytes,
                    FetchResult* result = nullptr, bool optional = false,
                    const bool* enough = nullptr,
                    const SendHooks* send_hooks = nullptr,
                    const JsonObserver* observer = nullptr,
                    JsonScan* scan = nullptr);
}  // namespace football
