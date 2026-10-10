#include "provider_client.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "network_service.h"
#include "settings_store.h"
#include "time_service.h"
namespace football {
namespace {
struct alignas(std::max_align_t) Header {
  std::size_t size;
};
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
  bool valid() {
    const bool ok = timekeeping::monotonicMs() < deadline &&
                    settings::generation() == generation && network::ready() &&
                    network::status().epoch == epoch;
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
int connect(esp_transport_handle_t t, const char* host, int port, int) {
  auto& c = transport(t);
  char numeric[IP4ADDR_STRLEN_MAX]{};
  if (!resolve(c, host, numeric, sizeof(numeric))) return -1;
  // Keep the original hostname for both certificate verification and SNI.
  esp_transport_ssl_set_common_name(c.tls, host);
  while (c.valid()) {
    const int progress = esp_transport_connect_async(
        c.tls, numeric, port, c.timeout(cfg::kSocketTimeoutMs));
    if (progress == 1) return c.valid() ? 0 : -1;
    if (progress < 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(cfg::kProviderWaitSliceMs));
  }
  return -1;
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
  while (c.valid()) {
    const int ready = esp_transport_poll_write(c.tls, c.slice(idle_end));
    if (ready < 0) return -1;
    if (ready > 0) {
      const int count =
          esp_transport_write(c.tls, bytes, length, c.slice(idle_end));
      if (count != ESP_TLS_ERR_SSL_WANT_WRITE &&
          count != ESP_TLS_ERR_SSL_WANT_READ)
        return count;
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
         std::size_t limit, int64_t deadline)
      : client_(client),
        generation_(generation),
        epoch_(network::status().epoch),
        limit_(limit),
        deadline_(deadline) {}
  int peek() {
    if (look_ == -2) look_ = next();
    return look_;
  }
  int read() {
    const auto c = peek();
    look_ = -2;
    return c;
  }
  std::size_t readBytes(char* out, std::size_t n) {
    std::size_t i = 0;
    for (; i < n; ++i) {
      const int c = read();
      if (c < 0) break;
      out[i] = c;
    }
    return i;
  }
  int nonSpace() {
    int c;
    do {
      c = read();
    } while (c == ' ' || c == '\r' || c == '\n' || c == '\t');
    return c;
  }
  bool complete() const {
    return !failed_ && esp_http_client_is_complete_data_received(client_);
  }
  bool failed() const { return failed_; }
  std::size_t bytes() const { return bytes_; }

 private:
  int next() {
    if (timekeeping::monotonicMs() >= deadline_ ||
        settings::generation() != generation_ || !network::ready() ||
        network::status().epoch != epoch_) {
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
        failed_ = true;
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
  char buffer_[1024];
  int look_ = -2;
  bool failed_ = false;
};
// Walk wrapper JSON without retaining discarded fields. Every scalar still
// goes through the real parser; depth, bytes, document and string allocation
// limits apply to all content, including fields outside the selected array.
bool walk(Stream& stream, const char* wanted, JsonConsumer consume,
          void* context, JsonAllocator& alloc, unsigned depth, bool& found,
          bool selected = false) {
  if (depth > cfg::kJsonDepth) return false;
  int c;
  do {
    c = stream.peek();
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
      stream.read();
    else
      break;
  } while (true);
  if (c == '[') {
    found |= selected;
    stream.read();
    while (stream.peek() == ' ' || stream.peek() == '\r' ||
           stream.peek() == '\n' || stream.peek() == '\t')
      stream.read();
    if (stream.peek() == ']') {
      stream.read();
      return true;
    }
    for (;;) {
      if (selected) {
        ArduinoJson::JsonDocument doc(&alloc);
        if (deserializeJson(doc, stream,
                            ArduinoJson::DeserializationOption::NestingLimit(
                                cfg::kJsonDepth - depth)) ||
            !consume(doc.as<ArduinoJson::JsonVariantConst>(), context))
          return false;
      } else if (!walk(stream, wanted, consume, context, alloc, depth + 1,
                       found))
        return false;
      c = stream.nonSpace();
      if (c == ']') return true;
      if (c != ',') return false;
    }
  }
  if (c == '{') {
    stream.read();
    c = stream.nonSpace();
    if (c == '}') return true;
    // Escaped wrapper keys are rejected rather than matched incorrectly.
    for (;;) {
      if (c != '"') return false;
      char key[96]{};
      std::size_t n = 0;
      while ((c = stream.read()) != '"') {
        if (c < 0 || c == '\\' || c < 32 || n >= sizeof(key) - 1) return false;
        key[n++] = c;
      }
      if (stream.nonSpace() != ':') return false;
      if (!walk(stream, wanted, consume, context, alloc, depth + 1, found,
                strcmp(key, wanted) == 0))
        return false;
      c = stream.nonSpace();
      if (c == '}') return true;
      if (c != ',') return false;
      c = stream.nonSpace();
    }
  }
  ArduinoJson::JsonDocument doc(&alloc);
  if (deserializeJson(doc, stream,
                      ArduinoJson::DeserializationOption::NestingLimit(
                          cfg::kJsonDepth - depth)))
    return false;
  // A requested array represented by a scalar is a malformed response.
  return !selected;
}
}  // namespace
void* JsonAllocator::allocate(std::size_t size) {
  if (size > cfg::kJsonHeapBytes - used_ ||
      sizeof(Header) > cfg::kJsonHeapBytes - used_ - size)
    return nullptr;
  auto* p = static_cast<Header*>(std::malloc(sizeof(Header) + size));
  if (p == nullptr) return nullptr;
  p->size = size;
  used_ += size + sizeof(Header);
  return p + 1;
}
void JsonAllocator::deallocate(void* pointer) {
  if (pointer == nullptr) return;
  auto* p = static_cast<Header*>(pointer) - 1;
  used_ -= p->size + sizeof(Header);
  std::free(p);
}
void* JsonAllocator::reallocate(void* pointer, std::size_t size) {
  if (pointer == nullptr) return allocate(size);
  auto* p = static_cast<Header*>(pointer) - 1;
  const auto old = p->size;
  if (size > cfg::kJsonHeapBytes - (used_ - old)) return nullptr;
  auto* next = static_cast<Header*>(std::realloc(p, sizeof(Header) + size));
  if (next == nullptr) return nullptr;
  next->size = size;
  used_ = used_ - old + size;
  return next + 1;
}
esp_err_t fetchJson(const char* url, const char* array_key,
                    cfg::Provider provider, const settings::Model& model,
                    uint32_t generation, JsonConsumer consume, void* context,
                    bool selection, FetchResult* result) {
  esp_http_client_config_t config{};
  const std::size_t limit =
      selection ? cfg::kSelectionBodyBytes : cfg::kProviderBodyBytes;
  Transport context_transport{
      esp_transport_ssl_init(), generation, network::status().epoch,
      timekeeping::monotonicMs() + cfg::kOperationDeadlineMs,
      limit + cfg::kProviderHeaderBytes + cfg::kProviderWireOverheadBytes};
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
  config.url = url;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = cfg::kProviderIdleTimeoutMs;
  config.disable_auto_redirect = true;
  config.buffer_size = 1024;
  config.buffer_size_tx = 512;
  config.keep_alive_enable = false;
  auto client = esp_http_client_init(&config);
  if (client == nullptr) {
    esp_transport_destroy(bounded);
    esp_transport_destroy(context_transport.tls);
    return ESP_ERR_NO_MEM;
  }
  char key[97]{};
  if (provider == cfg::Provider::kApiFootball) {
    memcpy(key, model.api_football_key.data.data(),
           model.api_football_key.length);
    esp_http_client_set_header(client, "x-apisports-key", key);
  } else if (provider == cfg::Provider::kFootballData) {
    memcpy(key, model.football_data_key.data.data(),
           model.football_data_key.length);
    esp_http_client_set_header(client, "X-Auth-Token", key);
  }
  std::memset(key, 0, sizeof(key));
  esp_err_t err = esp_http_client_open(client, 0);
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
    Stream stream(client, generation, limit, context_transport.deadline);
    JsonAllocator allocator;
    bool ok;
    if (array_key == nullptr) {
      ArduinoJson::JsonDocument doc(&allocator);
      ok = !deserializeJson(doc, stream,
                            ArduinoJson::DeserializationOption::NestingLimit(
                                cfg::kJsonDepth)) &&
           consume(doc.as<ArduinoJson::JsonVariantConst>(), context);
    } else {
      bool found = false;
      ok = walk(stream, array_key, consume, context, allocator, 0, found,
                array_key[0] == 0) &&
           found;
    }
    if (!ok || stream.nonSpace() != -1 || !stream.complete() ||
        context_transport.failed)
      err = ESP_ERR_INVALID_RESPONSE;
    if (result) result->bytes = stream.bytes();
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  esp_transport_destroy(bounded);
  esp_transport_destroy(context_transport.tls);
  return err;
}
}  // namespace football
