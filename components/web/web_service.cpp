#include "web_service.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "file_service.h"
#include "football_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_target.h"
#include "image_service.h"
#include "lwip/sockets.h"
#include "network_service.h"
#include "ota_service.h"
#include "provider_client.h"
#include "settings_json.h"
#include "settings_store.h"
#include "time_service.h"
#include "web_asset.h"
namespace web {
using namespace ArduinoJson;
namespace {
httpd_handle_t server = nullptr;
char token[33]{};
std::atomic<int64_t> expires{0};
std::atomic<uint32_t> session_generation{0};
std::atomic<int> debug_fd{-1};
StaticTask_t debug_tcb;
StackType_t debug_stack[cfg::kDebugTaskStackBytes / sizeof(StackType_t)];
std::atomic<bool> debug_sending{false};
char debug_record[256];
// URI handlers run on the single HTTP server task. The debug producer never
// reads this deadline; its send is separately bounded by the SDK socket
// timeout.
int64_t request_deadline = 0;
struct Connection {
  int fd = -1;
  int64_t deadline = 0;
};
Connection connections[cfg::kHttpSockets];
Connection* connection(int fd) {
  for (auto& item : connections)
    if (item.fd == fd) return &item;
  return nullptr;
}
class Request {
 public:
  explicit Request(httpd_req_t* req)
      : connection_(connection(httpd_req_to_sockfd(req))) {
    const auto fallback =
        timekeeping::monotonicMs() + cfg::kOperationDeadlineMs;
    request_deadline =
        connection_ && connection_->deadline ? connection_->deadline : fallback;
  }
  ~Request() {
    if (connection_) connection_->deadline = 0;
  }

 private:
  Connection* connection_;
};
int boundedReceive(httpd_handle_t, int fd, char* bytes, size_t count,
                   int flags) {
  auto* state = connection(fd);
  if (!state || !bytes) return HTTPD_SOCK_ERR_INVALID;
  const auto now = timekeeping::monotonicMs();
  if (state->deadline == 0) state->deadline = now + cfg::kOperationDeadlineMs;
  const auto left = state->deadline - now;
  if (left <= 0) return HTTPD_SOCK_ERR_FAIL;
  const auto wait_ms = std::min<int64_t>(cfg::kSocketTimeoutMs, left);
  timeval timeout{static_cast<long>(wait_ms / 1000),
                  static_cast<long>((wait_ms % 1000) * 1000)};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
    return HTTPD_SOCK_ERR_FAIL;
  const int result = recv(fd, bytes, count, flags);
  if (timekeeping::monotonicMs() >= state->deadline) return HTTPD_SOCK_ERR_FAIL;
  if (result >= 0) return result;
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
             ? HTTPD_SOCK_ERR_TIMEOUT
             : HTTPD_SOCK_ERR_FAIL;
}
esp_err_t openSession(httpd_handle_t handle, int fd) {
  for (auto& item : connections) {
    if (item.fd >= 0) continue;
    item.fd = fd;
    item.deadline = 0;
    const auto err = httpd_sess_set_recv_override(handle, fd, boundedReceive);
    if (err != ESP_OK) item.fd = -1;
    return err;
  }
  return ESP_ERR_NO_MEM;
}
bool sendReady(httpd_req_t* req) {
  const auto left = request_deadline - timekeeping::monotonicMs();
  if (left <= 0) return false;
  const auto wait_ms = std::min<int64_t>(cfg::kSocketTimeoutMs, left);
  timeval timeout{static_cast<long>(wait_ms / 1000),
                  static_cast<long>((wait_ms % 1000) * 1000)};
  return setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                    sizeof(timeout)) == 0;
}
esp_err_t error(httpd_req_t* req, const char* code, const char* message) {
  if (!sendReady(req)) return ESP_FAIL;
  httpd_resp_set_status(req, code);
  httpd_resp_set_type(req, "application/json");
  char result[160];
  snprintf(result, sizeof(result), "{\"error\":\"%s\"}", message);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_sendstr(req, result);
  return ESP_FAIL;  // Also discard unread bodies by closing the connection.
}
class Writer {
 public:
  explicit Writer(httpd_req_t* req) : req_(req) {}
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t length) {
    if (failed_) return 0;
    for (size_t i = 0; i < length; ++i) {
      buffer_[used_++] = data[i];
      if (used_ == sizeof(buffer_) && !flush()) return i;
    }
    return length;
  }
  bool flush() {
    if (failed_) return false;
    if (!used_) return true;
    const bool ok = sendReady(req_) &&
                    httpd_resp_send_chunk(req_, buffer_, used_) == ESP_OK;
    used_ = 0;
    failed_ |= !ok;
    return ok;
  }

 private:
  httpd_req_t* req_;
  char buffer_[512];
  size_t used_ = 0;
  bool failed_ = false;
};
esp_err_t json(httpd_req_t* req, JsonDocument& doc) {
  if (doc.overflowed())
    return error(req, "507 Insufficient Storage", "response memory limit");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  Writer writer(req);
  serializeJson(doc, writer);
  return writer.flush() && sendReady(req)
             ? httpd_resp_send_chunk(req, nullptr, 0)
             : ESP_FAIL;
}
bool equal(const char* a, const char* b, std::size_t length) {
  uint8_t difference = 0;
  for (std::size_t i = 0; i < length; ++i)
    difference |= static_cast<uint8_t>(a[i] ^ b[i]);
  return difference == 0;
}
bool origin(httpd_req_t* req) {
  char host[96]{}, request_origin[128]{};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
    return false;
  char bare[96];
  snprintf(bare, sizeof(bare), "%s", host);
  if (auto* colon = strchr(bare, ':')) {
    if (strcmp(colon, ":80")) return false;
    *colon = 0;
  }
  auto net = network::status();
  auto model = settings::current();
  char name[72]{};
  snprintf(name, sizeof(name), "%.*s.local", model.hostname.length,
           model.hostname.data.data());
  if (strcmp(bare, net.ip) && strcmp(bare, net.ap_ip) && strcmp(bare, name) &&
      model.hostname.view() != bare)
    return false;
  if (httpd_req_get_hdr_value_len(req, "Origin") == 0) return true;
  if (httpd_req_get_hdr_value_str(req, "Origin", request_origin,
                                  sizeof(request_origin)) != ESP_OK)
    return false;
  char expected[128];
  snprintf(expected, sizeof(expected), "http://%s", host);
  return strcmp(expected, request_origin) == 0;
}
bool session(httpd_req_t* req, bool query = false) {
  if (session_generation != settings::generation() ||
      timekeeping::monotonicMs() >= expires || token[0] == 0)
    return false;
  char supplied[33]{};
  if (query) {
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "token", supplied, sizeof(supplied)) != ESP_OK)
      return false;
  } else if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", supplied,
                                         sizeof(supplied)) != ESP_OK)
    return false;
  return equal(token, supplied, 32) && origin(req);
}
bool receive(httpd_req_t* req, JsonDocument& doc) {
  if (req->content_len == 0 || req->content_len > cfg::kApiBodyBytes)
    return false;
  char type[48];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) !=
          ESP_OK ||
      strncmp(type, "application/json", 16) != 0)
    return false;
  auto* buffer = static_cast<char*>(std::malloc(req->content_len + 1));
  if (buffer == nullptr) return false;
  const auto deadline = request_deadline;
  std::size_t used = 0;
  while (used < req->content_len && timekeeping::monotonicMs() < deadline) {
    const int n = httpd_req_recv(req, buffer + used, req->content_len - used);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) break;
    used += n;
  }
  buffer[used] = 0;
  struct Reader {
    const char* data;
    std::size_t size, at = 0;
    int read() {
      return at < size ? static_cast<unsigned char>(data[at++]) : -1;
    }
    std::size_t readBytes(char* out, std::size_t length) {
      const auto count = std::min(length, size - at);
      memcpy(out, data + at, count);
      at += count;
      return count;
    }
  } reader{buffer, used};
  bool ok =
      used == req->content_len &&
      !deserializeJson(doc, reader,
                       DeserializationOption::NestingLimit(cfg::kJsonDepth));
  while (reader.at < used) {
    const char c = buffer[reader.at++];
    ok &= c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }
  memset(buffer, 0, req->content_len + 1);
  std::free(buffer);
  return ok && doc.is<JsonObject>();
}
esp_err_t page(httpd_req_t* req) {
  Request request(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  if (!sendReady(req)) return ESP_FAIL;
  return httpd_resp_send(req, reinterpret_cast<const char*>(kWebAsset),
                         kWebAssetBytes);
}
esp_err_t get(httpd_req_t* req) {
  Request request(req);
  football::JsonAllocator allocator;
  JsonDocument doc(&allocator);
  auto out = doc.to<JsonObject>();
  const char* path = req->uri;
  if (!strcmp(path, "/api/v1/settings"))
    settingsJson(out, settings::current());
  else if (!strcmp(path, "/api/v1/schema")) {
    schemaJson(out);
    out["width"] = hw::kDisplay.width;
    out["height"] = hw::kDisplay.height;
    out["round"] = hw::kDisplay.shape == hw::DisplayShape::kRound;
    out["antenna"] = hw::kBoard.has_antenna_switch;
  } else if (!strcmp(path, "/api/v1/status")) {
    auto net = network::status();
    auto data = football::status();
    files::Usage usage{};
    files::usage(usage);
    out["version"] = esp_app_get_description()->version;
    out["target"] = ota::identity().target;
    out["layout"] = ota::identity().layout;
    out["ip"] = net.ip;
    out["ap_ip"] = net.ap_ip;
    out["network"] = network::stateName(net.state);
    out["ap"] = net.ap;
    out["rssi"] = net.rssi;
    out["time_valid"] = timekeeping::status().valid;
    out["uptime_s"] = timekeeping::monotonicMs() / 1000;
    out["heap_free"] =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out["heap_largest"] =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out["fs_total"] = usage.total_bytes;
    out["fs_used"] = usage.used_bytes;
    out["trial"] = ota::trial();
    out["provider_busy"] = data.busy;
    out["last_update_ms"] = data.fetched_ms;
    out["data_stale"] = data.stale;
    out["data_error"] = data.error.data;
    out["routing_notice"] = data.routing_notice.data;
    auto counts = out["requests"].to<JsonArray>();
    for (auto count : data.requests) counts.add(count);
  } else if (!strcmp(path, "/api/v1/wifi")) {
    network::AccessPoint results[cfg::kScanEntries];
    const auto count = network::scanResults(results, cfg::kScanEntries);
    auto list = out["networks"].to<JsonArray>();
    for (unsigned i = 0; i < count; ++i) {
      auto item = list.add<JsonObject>();
      item["ssid"] = results[i].ssid;
      item["rssi"] = results[i].rssi;
      item["secure"] = results[i].secure;
    }
  } else if (!strcmp(path, "/api/v1/selection")) {
    auto state = football::selectionStatus();
    out["busy"] = state.busy;
    out["more"] = state.more;
    out["revision"] = state.revision;
    out["error"] = state.error.data;
    auto list = out["items"].to<JsonArray>();
    football::Selection item;
    if (!state.busy)
      for (unsigned i = 0; i < state.count; ++i) {
        if (!football::selectionAt(i, state.revision, item))
          return error(req, "409 Conflict", "selection changed");
        auto row = list.add<JsonObject>();
        row["id"] = item.id.data;
        row["name"] = item.name.data;
        row["season"] = item.season.data;
      }
  } else if (!strcmp(path, "/api/v1/images")) {
    auto list = out["items"].to<JsonArray>();
    for (unsigned i = 0; i < cfg::kImageCount; ++i) {
      auto e = images::info(i);
      auto row = list.add<JsonObject>();
      row["id"] = i;
      row["name"] = e.name;
      row["bytes"] = e.bytes;
      row["uploaded"] = e.uploaded;
      row["available"] = e.available;
    }
  } else
    return error(req, "404 Not Found", "unknown route");
  return json(req, doc);
}
esp_err_t mutation(httpd_req_t* req) {
  Request request(req);
  if (!origin(req)) return error(req, "403 Forbidden", "origin rejected");
  football::JsonAllocator allocator;
  JsonDocument doc(&allocator);
  const bool login = !strcmp(req->uri, "/api/v1/session");
  if (ota::trial() && (!strcmp(req->uri, "/api/v1/settings") ||
                       !strcmp(req->uri, "/api/v1/reset") ||
                       !strcmp(req->uri, "/api/v1/images/reset")))
    return error(req, "409 Conflict", "OTA trial is read only");
  if (!login && !session(req))
    return error(req, "401 Unauthorized", "session required");
  if (!receive(req, doc))
    return error(req, "400 Bad Request", "invalid JSON or body limit");
  const auto in = doc.as<JsonObjectConst>();
  esp_err_t err = ESP_OK;
  if (login) {
    const auto model = settings::current();
    auto supplied = in["password"].as<JsonString>();
    if (!model.admin_password.empty() &&
        (!supplied || supplied.size() != model.admin_password.length ||
         !equal(supplied.c_str(), model.admin_password.data.data(),
                supplied.size())))
      return error(req, "401 Unauthorized", "password rejected");
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    for (unsigned i = 0; i < 16; ++i)
      snprintf(token + i * 2, 3, "%02x", bytes[i]);
    expires = timekeeping::monotonicMs() + cfg::kSessionLifetimeMs;
    session_generation = settings::generation();
    doc.clear();
    doc["token"] = token;
    doc["expires_ms"] = cfg::kSessionLifetimeMs;
    return json(req, doc);
  }
  if (!strcmp(req->uri, "/api/v1/logout")) {
    token[0] = 0;
    expires = 0;
  } else if (!strcmp(req->uri, "/api/v1/settings")) {
    auto model = settings::current();
    if (!applyJson(in, model))
      return error(req, "400 Bad Request", "settings rejected");
    err = settings::save(model);
  } else if (!strcmp(req->uri, "/api/v1/export")) {
    const bool secrets = in["secrets"].is<bool>() && in["secrets"].as<bool>();
    doc.clear();
    settingsJson(doc.to<JsonObject>(), settings::current(), secrets);
    return json(req, doc);
  } else if (!strcmp(req->uri, "/api/v1/wifi/scan"))
    err = network::requestScan();
  else if (!strcmp(req->uri, "/api/v1/data/refresh"))
    football::refresh();
  else if (!strcmp(req->uri, "/api/v1/data/select")) {
    if (!in["provider"].is<uint8_t>() ||
        in["provider"].as<uint8_t>() >= cfg::kProviderCount)
      return error(req, "400 Bad Request", "provider rejected");
    football::SelectionRequest request;
    request.provider = static_cast<cfg::Provider>(in["provider"].as<uint8_t>());
    if (!in["teams"].is<bool>() ||
        (!in["offset"].isNull() && !in["offset"].is<uint16_t>()))
      return error(req, "400 Bad Request", "selection types rejected");
    request.teams = in["teams"].as<bool>();
    request.offset = in["offset"].as<uint16_t>();
    for (const char* field : {"competition", "season", "search", "country"}) {
      if (!in[field].isNull() && !in[field].is<const char*>())
        return error(req, "400 Bad Request", "selection text rejected");
      auto text = in[field].as<JsonString>();
      const unsigned max =
          (!strcmp(field, "search") || !strcmp(field, "country")) ? 48
          : !strcmp(field, "season") ? 16
                                     : cfg::kIdBytes;
      if (text && (text.size() > max || strlen(text.c_str()) != text.size()))
        return error(req, "400 Bad Request", "selection length rejected");
    }
    request.competition.assign(in["competition"] | "");
    request.season.assign(in["season"] | "");
    request.search.assign(in["search"] | "");
    request.country.assign(in["country"] | "");
    settings::Model validate = settings::initialValues({});
    auto& route = validate.routes[0];
    route.provider = request.provider;
    route.competition.assign(request.competition.data);
    route.season.assign(request.season.data);
    if (!settings::validate(validate) ||
        (request.teams && request.competition.empty()) ||
        (request.teams && request.provider == cfg::Provider::kApiFootball &&
         request.season.empty()))
      return error(req, "400 Bad Request", "selection rejected");
    err = football::select(request);
  } else if (!strcmp(req->uri, "/api/v1/images/reset")) {
    network::HeavyGuard guard;
    if (!guard) return error(req, "409 Conflict", "operation busy");
    if (!in["id"].is<uint8_t>())
      return error(req, "400 Bad Request", "image id required");
    err = images::reset(in["id"].as<uint8_t>());
  } else if (!strcmp(req->uri, "/api/v1/reboot"))
    ota::reboot();
  else if (!strcmp(req->uri, "/api/v1/reset")) {
    if (strcmp(in["confirm"] | "", "reset"))
      return error(req, "400 Bad Request", "confirmation required");
    network::HeavyGuard guard;
    if (!guard) return error(req, "409 Conflict", "operation busy");
    if ((!in["images"].isNull() && !in["images"].is<bool>()) ||
        (!in["format_storage"].isNull() && !in["format_storage"].is<bool>()))
      return error(req, "400 Bad Request", "reset options rejected");
    if (in["format_storage"].as<bool>())
      err = images::format();
    else if (in["images"].as<bool>())
      err = images::resetAll();
    if (err == ESP_OK) err = settings::reset();
    if (err == ESP_OK) ota::reboot();
  } else
    return error(req, "404 Not Found", "unknown route");
  if (err != ESP_OK) return error(req, "409 Conflict", esp_err_to_name(err));
  doc.clear();
  doc["ok"] = true;
  return json(req, doc);
}
esp_err_t upload(httpd_req_t* req) {
  Request request(req);
  if (!session(req)) return error(req, "401 Unauthorized", "session required");
  network::HeavyGuard guard;
  if (!guard) return error(req, "409 Conflict", "operation busy");
  const bool firmware = !strcmp(req->uri, "/api/v1/ota");
  char id[8]{};
  uint8_t index = 0;
  if (!firmware) {
    if (httpd_req_get_hdr_value_str(req, "X-Image-Id", id, sizeof(id)) !=
        ESP_OK)
      return error(req, "400 Bad Request", "image id required");
    char* end;
    const long value = strtol(id, &end, 10);
    if (id == end || *end || value < 0 ||
        value >= static_cast<long>(cfg::kImageCount))
      return error(req, "400 Bad Request", "image id rejected");
    index = value;
  }
  const int64_t deadline = request_deadline;
  uint8_t buffer[1024];
  std::size_t received = 0, first = 0;
  bool begun = false;
  esp_err_t err = ESP_OK;
  if (!firmware) {
    err = images::begin(index, req->content_len);
    begun = err == ESP_OK;
  }
  if (firmware && (req->content_len < 32 + 256 + sizeof(ota::Identity) ||
                   req->content_len > ota::capacity()))
    err = ESP_ERR_INVALID_SIZE;
  while (err == ESP_OK && received < req->content_len &&
         timekeeping::monotonicMs() < deadline) {
    const auto capacity =
        firmware && !begun ? sizeof(buffer) - first : sizeof(buffer);
    const int count = httpd_req_recv(
        req, reinterpret_cast<char*>(buffer) + (firmware && !begun ? first : 0),
        std::min<std::size_t>(capacity, req->content_len - received));
    if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (count <= 0) {
      err = ESP_FAIL;
      break;
    }
    received += count;
    if (firmware && !begun) {
      first += count;
      if (first < 32 + 256 + sizeof(ota::Identity)) continue;
      err = ota::begin(buffer, first, req->content_len);
      begun = err == ESP_OK;
      if (err == ESP_OK) err = ota::write(buffer, first);
    } else
      err = firmware ? ota::write(buffer, count) : images::write(buffer, count);
  }
  if (err == ESP_OK && received != req->content_len) err = ESP_ERR_TIMEOUT;
  if (err == ESP_OK && timekeeping::monotonicMs() >= deadline)
    err = ESP_ERR_TIMEOUT;
  if (err == ESP_OK) err = firmware ? ota::finish() : images::finish(deadline);
  if (err != ESP_OK) {
    if (firmware)
      ota::abort();
    else
      images::abort();
    return error(req, "400 Bad Request", esp_err_to_name(err));
  }
  if (firmware) ota::reboot();
  httpd_resp_set_type(req, "application/json");
  if (!sendReady(req)) return ESP_FAIL;
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}
void closeSession(httpd_handle_t, int fd) {
  if (debug_fd.load() == fd) debug_fd.store(-1);
  if (auto* state = connection(fd)) {
    state->fd = -1;
    state->deadline = 0;
  }
  close(fd);
}
esp_err_t debug(httpd_req_t* req) {
  Request request(req);
  if (req->method == HTTP_GET) {
    if (!session(req, true) || !settings::current().browser_debug)
      return ESP_FAIL;
    const int fd = httpd_req_to_sockfd(req);
    int expected = -1;
    return debug_fd.compare_exchange_strong(expected, fd) ? ESP_OK : ESP_FAIL;
  }
  httpd_ws_frame_t frame{};
  frame.type = HTTPD_WS_TYPE_TEXT;
  const auto err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK || frame.len > 128) return ESP_FAIL;
  if (frame.type == HTTPD_WS_TYPE_CLOSE) debug_fd.store(-1);
  uint8_t discard[128];
  frame.payload = discard;
  return httpd_ws_recv_frame(req, &frame, sizeof(discard));
}
void streamDebug(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(cfg::kDebugStreamPeriodMs));
    const int fd = debug_fd.load();
    if (fd < 0) continue;
    if (!settings::current().browser_debug ||
        timekeeping::monotonicMs() >= expires ||
        session_generation != settings::generation()) {
      httpd_sess_trigger_close(server, fd);
      continue;
    }
    if (debug_sending.exchange(true)) continue;
    auto net = network::status();
    auto data = football::status();
    const int length = snprintf(
        debug_record, sizeof(debug_record),
        "{\"ms\":%lld,\"net\":\"%s\",\"heap\":%u,\"rev\":%"
        "lu,\"stale\":%s,\"busy\":%s,\"error\":\"%.48s\",\"requests\":[%lu,%lu,"
        "%lu,%lu]}",
        static_cast<long long>(timekeeping::monotonicMs()),
        network::stateName(net.state),
        static_cast<unsigned>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned long>(data.revision),
        data.stale ? "true" : "false", data.busy ? "true" : "false",
        data.error.data, static_cast<unsigned long>(data.requests[0]),
        static_cast<unsigned long>(data.requests[1]),
        static_cast<unsigned long>(data.requests[2]),
        static_cast<unsigned long>(data.requests[3]));
    if (length <= 0 || length >= static_cast<int>(sizeof(debug_record))) {
      debug_sending.store(false);
      continue;
    }
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(debug_record);
    frame.len = length;
    const auto err = httpd_ws_send_data_async(
        server, fd, &frame,
        [](esp_err_t result, int socket, void*) {
          debug_sending.store(false);
          if (result != ESP_OK) {
            debug_fd.store(-1);
            httpd_sess_trigger_close(server, socket);
          }
        },
        nullptr);
    if (err != ESP_OK) {
      debug_sending.store(false);
      debug_fd.store(-1);
      httpd_sess_trigger_close(server, fd);
    }
  }
}
}  // namespace
esp_err_t init() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = cfg::kHttpStackBytes;
  config.max_open_sockets = cfg::kHttpSockets;
  config.max_uri_handlers = 40;
  config.recv_wait_timeout = 1;
  config.send_wait_timeout = 1;
  config.lru_purge_enable = true;
  config.open_fn = openSession;
  config.close_fn = closeSession;
  auto err = httpd_start(&server, &config);
  if (err != ESP_OK) return err;
  auto add = [&](const char* path, httpd_method_t method,
                 esp_err_t (*handler)(httpd_req_t*), bool websocket = false) {
    httpd_uri_t uri{};
    uri.uri = path;
    uri.method = method;
    uri.handler = handler;
    uri.is_websocket = websocket;
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &uri);
  };
  add("/", HTTP_GET, page);
  for (const char* path :
       {"/api/v1/status", "/api/v1/schema", "/api/v1/settings", "/api/v1/wifi",
        "/api/v1/selection", "/api/v1/images"})
    add(path, HTTP_GET, get);
  for (const char* path :
       {"/api/v1/session", "/api/v1/logout", "/api/v1/settings",
        "/api/v1/export", "/api/v1/wifi/scan", "/api/v1/data/select",
        "/api/v1/data/refresh", "/api/v1/images/reset", "/api/v1/reboot",
        "/api/v1/reset"})
    add(path, HTTP_POST, mutation);
  add("/api/v1/settings", HTTP_PUT, mutation);
  add("/api/v1/ota", HTTP_POST, upload);
  add("/api/v1/images/upload", HTTP_POST, upload);
  add("/api/v1/debug", HTTP_GET, debug, true);
  if (err != ESP_OK) {
    httpd_stop(server);
    server = nullptr;
    return err;
  }
  err = httpd_register_err_handler(
      server, HTTPD_404_NOT_FOUND, [](httpd_req_t* req, httpd_err_code_t) {
        Request request(req);
        return !strncmp(req->uri, "/api/", 5)
                   ? error(req, "404 Not Found", "unknown route")
                   : page(req);
      });
  if (err != ESP_OK) {
    httpd_stop(server);
    server = nullptr;
    return err;
  }
  if (!xTaskCreateStatic(streamDebug, "web_debug", cfg::kDebugTaskStackBytes,
                         nullptr, 2, debug_stack, &debug_tcb)) {
    httpd_stop(server);
    server = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
bool healthy() { return server != nullptr; }
}  // namespace web
