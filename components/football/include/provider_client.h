#pragma once

#include <cstddef>

#include "esp_err.h"
#include "football_model.h"
#include "provider_adapter.h"
#include "provider_settings.h"
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
  // Optional enclosing interactive operation deadline; never extends transport.
  int64_t deadline_ms = 0;
};
// Adapter owns URL, authentication provider, scope and body ceiling. Transport
// owns DNS/TLS, absolute deadlines, actual send admission and complete I/O.
// All borrowed request/sink/model values remain alive until this returns.
esp_err_t fetchProvider(const provider::RequestSpec& spec,
                        const provider::Sink& sink,
                        const settings::ProviderSettings& model,
                        uint32_t generation, provider::ParseResult& parsed,
                        FetchResult* transport = nullptr,
                        const SendHooks* hooks = nullptr);
}  // namespace football
