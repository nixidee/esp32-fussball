#include "provider_client.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_send_admission.h"
#include "json_stream.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "network_service.h"
#include "settings_store.h"
#include "time_service.h"
namespace football {
namespace {
// The SDK header-fetch loop has no whole-operation deadline of its own.
// Bound the transport itself, including trickled headers, without sharing
// or closing a live TLS handle from another task.
struct Transport {
  esp_transport_handle_t tls = nullptr;
  uint32_t generation, epoch;
  int64_t deadline;
  std::size_t wire_limit, wire_bytes = 0, header_bytes = 0;
  uint32_t ending = 0;
  bool headers = true, failed = false;
  const SendHooks* send_hooks = nullptr;
  const provider::Sink* consumer = nullptr;
  http::SendAdmission send{};
  // Where the request stopped and the socket's pending error, for the log.
  const char* stage = "dns";
  int socket_error = 0;
  bool valid() {
    const bool allowed =
        send.allowed(settings::footballPaused(),
                     send_hooks != nullptr && send_hooks->pause_with_frame);
    const bool ok = timekeeping::monotonicMs() < deadline &&
                    settings::dataGeneration() == generation &&
                    network::ready() && network::status().epoch == epoch &&
                    allowed &&
                    (!consumer || !consumer->cancelled ||
                     !consumer->cancelled(consumer->context));
    failed |= !ok;
    return ok;
  }
  int timeout(int requested) {
    return static_cast<int>(std::max<int64_t>(
        1,
        std::min<int64_t>(requested, deadline - timekeeping::monotonicMs())));
  }
  // One bounded wait slice before idle_end, so validity is rechecked often.
  int slice(int64_t idle_end) {
    return timeout(static_cast<int>(std::min<int64_t>(
        cfg::kSocketTimeoutMs, idle_end - timekeeping::monotonicMs())));
  }
};
Transport& transport(esp_transport_handle_t t) {
  return *static_cast<Transport*>(esp_transport_get_context_data(t));
}
// lwIP copies the name before dispatch returns. Only that dispatch owns the
// fixed name buffer; late DNS callbacks carry a token and cannot overwrite a
// later operation. No callback references a caller's stack or TLS handle.
struct DnsJob {
  std::atomic<bool> queued{false};
  char name[cfg::kProviderHostBytes]{};
  ip_addr_t address{};
  uint32_t token = 0, completed = 0;
  bool resolved = false;
};
DnsJob dns_job;
portMUX_TYPE dns_lock = portMUX_INITIALIZER_UNLOCKED;
void dnsFound(const char*, const ip_addr_t* address, void* argument) {
  const auto token =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argument));
  portENTER_CRITICAL(&dns_lock);
  if (dns_job.token == token) {
    dns_job.resolved = address != nullptr && IP_IS_V4(address);
    if (dns_job.resolved) dns_job.address = *address;
    dns_job.completed = token;
  }
  portEXIT_CRITICAL(&dns_lock);
}
void dispatchDns(void* argument) {
  ip_addr_t address{};
  const auto result = dns_gethostbyname_addrtype(
      dns_job.name, &address, dnsFound, argument, LWIP_DNS_ADDRTYPE_IPV4);
  if (result == ERR_OK)
    dnsFound(nullptr, &address, argument);
  else if (result != ERR_INPROGRESS)
    dnsFound(nullptr, nullptr, argument);
  dns_job.queued.store(false, std::memory_order_release);
}
bool resolve(Transport& c, const char* host, char* numeric,
             std::size_t capacity) {
  const auto length = std::strlen(host);
  if (length == 0 || length >= sizeof(dns_job.name) || !c.valid()) return false;
  bool queued = false;
  if (!dns_job.queued.compare_exchange_strong(queued, true,
                                              std::memory_order_acq_rel))
    return false;
  std::memcpy(dns_job.name, host, length + 1);
  portENTER_CRITICAL(&dns_lock);
  if (++dns_job.token == 0) ++dns_job.token;
  const auto token = dns_job.token;
  dns_job.completed = 0;
  dns_job.resolved = false;
  portEXIT_CRITICAL(&dns_lock);
  if (tcpip_try_callback(
          dispatchDns,
          reinterpret_cast<void*>(static_cast<uintptr_t>(token))) != ERR_OK) {
    dns_job.queued.store(false, std::memory_order_release);
    return false;
  }
  while (c.valid()) {
    ip_addr_t address{};
    portENTER_CRITICAL(&dns_lock);
    const bool done = dns_job.completed == token;
    const bool success = dns_job.resolved;
    if (done && success) address = dns_job.address;
    portEXIT_CRITICAL(&dns_lock);
    if (done)
      return success && ipaddr_ntoa_r(&address, numeric, capacity) != nullptr;
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
  return false;
}
// A failed connect is "tls" once the TCP connection stands (its socket is
// writable without a pending error), otherwise "tcp".
int connectFailed(Transport& c) {
  const int socket = esp_transport_get_socket(c.tls);
  if (socket < 0) return -1;
  socklen_t size = sizeof(c.socket_error);
  if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &c.socket_error, &size) != 0)
    return -1;
  fd_set writable;
  FD_ZERO(&writable);
  FD_SET(socket, &writable);
  timeval now{};
  if (c.socket_error == 0 &&
      select(socket + 1, nullptr, &writable, nullptr, &now) > 0)
    c.stage = "tls";
  return -1;
}
int connect(esp_transport_handle_t t, const char* host, int port, int) {
  auto& c = transport(t);
  char numeric[IP4ADDR_STRLEN_MAX]{};
  if (!resolve(c, host, numeric, sizeof(numeric))) return -1;
  c.stage = "tcp";
  // Keep the original hostname for both certificate verification and SNI.
  esp_transport_ssl_set_common_name(c.tls, host);
  while (c.valid()) {
    const int progress = esp_transport_connect_async(
        c.tls, numeric, port, c.timeout(cfg::kSocketTimeoutMs));
    if (progress == 1) {
      if (!c.valid()) return -1;
      c.stage = "http";
      return 0;
    }
    if (progress < 0) return connectFailed(c);
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
  return connectFailed(c);
}
// The async connect leaves the TLS socket non-blocking. A partly received TLS
// record then reads as 0 (timeout) at once instead of after `timeout`, so
// keep waiting until data arrives or the provider stayed silent that long.
int readTransport(esp_transport_handle_t t, char* bytes, int length,
                  int timeout) {
  auto& c = transport(t);
  const int64_t idle_end = timekeeping::monotonicMs() + timeout;
  int count;
  do {
    if (!c.valid()) return -1;
    count = esp_transport_read(c.tls, bytes, length, c.slice(idle_end));
  } while (count == 0 && timekeeping::monotonicMs() < idle_end);
  if (count <= 0) return count;
  c.wire_bytes += count;
  for (int i = 0; i < count && c.headers; ++i) {
    ++c.header_bytes;
    c.ending = (c.ending << 8) | static_cast<uint8_t>(bytes[i]);
    if (c.ending == 0x0d0a0d0a) c.headers = false;
  }
  if (c.header_bytes > cfg::kProviderHeaderBytes ||
      c.wire_bytes > c.wire_limit || !c.valid()) {
    c.failed = true;
    return -1;
  }
  return count;
}
// Same non-blocking socket: wait for send space in slices, then write. A full
// TLS send path reports WANT_WRITE/WANT_READ instead of blocking.
int writeTransport(esp_transport_handle_t t, const char* bytes, int length,
                   int timeout) {
  auto& c = transport(t);
  const int64_t idle_end = timekeeping::monotonicMs() + timeout;
  bool want_read = false;
  while (c.valid()) {
    const int ready = want_read
                          ? esp_transport_poll_read(c.tls, c.slice(idle_end))
                          : esp_transport_poll_write(c.tls, c.slice(idle_end));
    if (ready < 0) return -1;
    if (ready > 0) {
      // Linearization point: the service has reserved this actual HTTP
      // send attempt under the same mutex that publishes frame changes.
      // The underlying TLS socket is nonblocking; poll_write was above.
      int write_errno = 0;
      const int count = c.send.write(
          [&] {
            return c.send_hooks != nullptr
                       ? c.send_hooks->begin(c.send_hooks->context)
                       : ESP_OK;
          },
          [&] {
            // The SDK ssl_write has its OWN poll_write(timeout). It must
            // not wait while the first-send callback owns the writer mutex.
            errno = 0;
            const int result = esp_transport_write(c.tls, bytes, length, 0);
            write_errno = errno;
            return result;
          },
          [&] {
            if (c.send_hooks != nullptr)
              c.send_hooks->end(c.send_hooks->context);
          });
      if (c.send.refusal() != ESP_OK ||
          !http::retryWrite(count, write_errno, ESP_TLS_ERR_SSL_WANT_READ,
                            ESP_TLS_ERR_SSL_WANT_WRITE))
        return count;
      want_read = count == ESP_TLS_ERR_SSL_WANT_READ;
    }
    if (timekeeping::monotonicMs() >= idle_end) return 0;
  }
  return -1;
}
int closeTransport(esp_transport_handle_t t) {
  return esp_transport_close(transport(t).tls);
}
int pollRead(esp_transport_handle_t t, int timeout) {
  auto& c = transport(t);
  return c.valid() ? esp_transport_poll_read(c.tls, c.timeout(timeout)) : -1;
}
int pollWrite(esp_transport_handle_t t, int timeout) {
  auto& c = transport(t);
  return c.valid() ? esp_transport_poll_write(c.tls, c.timeout(timeout)) : -1;
}
int destroyTransport(esp_transport_handle_t) { return 0; }
class Stream {
 public:
  Stream(esp_http_client_handle_t client, uint32_t generation,
         std::size_t limit, int64_t deadline,
         const provider::Sink* consumer = nullptr)
      : client_(client),
        generation_(generation),
        epoch_(network::status().epoch),
        limit_(limit),
        deadline_(deadline),
        consumer_(consumer) {}
  json::Input input() {
    return {this, [](void* raw) { return static_cast<Stream*>(raw)->next(); }};
  }
  bool complete() const {
    return !failed_ && pos_ == size_ &&
           esp_http_client_is_complete_data_received(client_);
  }
  bool failed() const { return failed_; }
  bool tooLarge() const { return too_large_; }
  std::size_t bytes() const { return bytes_; }

 private:
  int next() {
    if (timekeeping::monotonicMs() >= deadline_ ||
        settings::dataGeneration() != generation_ || !network::ready() ||
        network::status().epoch != epoch_ ||
        (consumer_ && consumer_->cancelled &&
         consumer_->cancelled(consumer_->context))) {
      failed_ = true;
      return -1;
    }
    if (pos_ == size_) {
      int received = esp_http_client_read(client_, buffer_, sizeof(buffer_));
      if (received <= 0) {
        failed_ |= received < 0;
        return -1;
      }
      bytes_ += received;
      if (bytes_ > limit_) {
        failed_ = too_large_ = true;
        return -1;
      }
      pos_ = 0;
      size_ = received;
    }
    return static_cast<unsigned char>(buffer_[pos_++]);
  }
  esp_http_client_handle_t client_;
  uint32_t generation_, epoch_;
  std::size_t limit_, bytes_ = 0, pos_ = 0, size_ = 0;
  int64_t deadline_;
  const provider::Sink* consumer_;
  char buffer_[cfg::kProviderReadBufferBytes];
  bool failed_ = false, too_large_ = false;
};
}  // namespace
esp_err_t fetchProvider(const provider::RequestSpec& spec,
                        const provider::Sink& sink,
                        const settings::ProviderSettings& model,
                        uint32_t generation, provider::ParseResult& parsed,
                        FetchResult* result, const SendHooks* send_hooks) {
  parsed = {};
  if (result) *result = {};
  provider::HttpRequest request;
  if (!provider::buildRequest(spec, request) || request.body_limit == 0 ||
      (send_hooks && (!send_hooks->begin || !send_hooks->end)))
    return ESP_ERR_INVALID_ARG;
  const auto limit = request.body_limit;
  esp_http_client_config_t config{};
  Transport context_transport{
      esp_transport_ssl_init(), generation, network::status().epoch,
      timekeeping::monotonicMs() + cfg::kOperationDeadlineMs,
      limit + cfg::kProviderHeaderBytes + cfg::kProviderWireOverheadBytes};
  if (send_hooks && send_hooks->deadline_ms > 0)
    context_transport.deadline =
        std::min(context_transport.deadline, send_hooks->deadline_ms);
  context_transport.send_hooks = send_hooks;
  context_transport.consumer = &sink;
  auto bounded = esp_transport_init();
  if (bounded == nullptr || context_transport.tls == nullptr) {
    if (bounded) esp_transport_destroy(bounded);
    if (context_transport.tls) esp_transport_destroy(context_transport.tls);
    return ESP_ERR_NO_MEM;
  }
  esp_transport_ssl_crt_bundle_attach(context_transport.tls,
                                      esp_crt_bundle_attach);
  esp_transport_set_default_port(bounded, 443);
  esp_transport_set_context_data(bounded, &context_transport);
  esp_transport_set_func(bounded, connect, readTransport, writeTransport,
                         closeTransport, pollRead, pollWrite, destroyTransport);
  config.transport = bounded;
  config.url = request.url;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = cfg::kProviderIdleTimeoutMs;
  config.disable_auto_redirect = true;
  config.buffer_size = cfg::kProviderReadBufferBytes;
  config.buffer_size_tx = cfg::kProviderSendBufferBytes;
  config.keep_alive_enable = false;
  auto client = esp_http_client_init(&config);
  if (client == nullptr) {
    esp_transport_destroy(bounded);
    esp_transport_destroy(context_transport.tls);
    return ESP_ERR_NO_MEM;
  }
  constexpr auto key_capacity =
      std::max(cfg::kApiFootballKeyMaxBytes, cfg::kFootballDataKeyMaxBytes);
  char key[key_capacity + 1]{};
  const auto auth = provider::authentication(spec.source.provider, model);
  esp_err_t err = ESP_OK;
  if (auth.header) {
    if (auth.value.size() >= sizeof(key))
      err = ESP_ERR_INVALID_ARG;
    else {
      std::memcpy(key, auth.value.data(), auth.value.size());
      err = esp_http_client_set_header(client, auth.header, key);
    }
  }
  std::memset(key, 0, sizeof(key));
  if (err == ESP_OK) err = esp_http_client_open(client, 0);
  if (err == ESP_OK) {
    const auto length = esp_http_client_fetch_headers(client);
    const int code = esp_http_client_get_status_code(client);
    if (result) result->status = code;
    // A negative length is a header read that failed or timed out.
    if (length < 0)
      err = ESP_ERR_INVALID_RESPONSE;
    else if (length > 0 && static_cast<uint64_t>(length) > limit)
      err = ESP_ERR_INVALID_SIZE;
    else if (code == 429)
      err = ESP_ERR_TIMEOUT;
    else if (code != 200)
      err = ESP_FAIL;
  }
  if (err == ESP_OK) {
    Stream stream(client, generation, limit, context_transport.deadline, &sink);
    json::Reader reader(stream.input());
    parsed = provider::readResponse(reader, spec, sink);
    if (stream.tooLarge()) {
      parsed.status = provider::Status::kTooLarge;
      parsed.document_complete = false;
    }
    if (context_transport.failed || stream.failed() ||
        (!parsed.early_stopped && !stream.complete())) {
      parsed.document_complete = false;
      if (parsed.status == provider::Status::kOk)
        parsed.status = provider::Status::kBadResponse;
    }
    switch (parsed.status) {
      case provider::Status::kOk:
        if (!parsed.document_complete && !parsed.early_stopped)
          err = ESP_ERR_INVALID_RESPONSE;
        break;
      case provider::Status::kNoMemory: err = ESP_ERR_NO_MEM; break;
      case provider::Status::kTooLarge: err = ESP_ERR_INVALID_SIZE; break;
      case provider::Status::kBadResponse:
      case provider::Status::kMissing:
      case provider::Status::kRejected: err = ESP_ERR_INVALID_RESPONSE; break;
    }
    if (result) result->bytes = stream.bytes();
  }
  if (result) {
    result->stage = context_transport.stage;
    result->socket_error = context_transport.socket_error;
    result->paused = context_transport.send.paused();
    result->admission_error = context_transport.send.refusal();
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  esp_transport_destroy(bounded);
  esp_transport_destroy(context_transport.tls);
  return err;
}
}  // namespace football
