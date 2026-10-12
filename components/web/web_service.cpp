#include "web_service.h"

#include <strings.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "event_bus.h"
#include "file_service.h"
#include "football_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health_service.h"
#include "hw_target.h"
#include "image_service.h"
#include "input_policy.h"
#include "lwip/sockets.h"
#include "network_service.h"
#include "ota_service.h"
#include "provider_client.h"
#include "session_table.h"
#include "settings_json.h"
#include "settings_store.h"
#include "time_service.h"
#include "ui_controller.h"
#include "web_asset.h"
namespace web {
using namespace ArduinoJson;
namespace {
using settings::Reason;
using Serial = SessionTable::Serial;

SessionTable sessions;
// Guards 'sessions': the HTTP task changes it, the debug task reads it.
portMUX_TYPE sessions_lock = portMUX_INITIALIZER_UNLOCKED;
class SessionLock {
 public:
  SessionLock() { taskENTER_CRITICAL(&sessions_lock); }
  ~SessionLock() { taskEXIT_CRITICAL(&sessions_lock); }
  SessionLock(const SessionLock&) = delete;
  SessionLock& operator=(const SessionLock&) = delete;
};

httpd_handle_t server = nullptr;
// The debug stream belongs to one connection and to the session that opened
// it; it ends with either (OQ-93). The session is stored after the socket, so
// the stream task skips a record whose session is still 0.
std::atomic<int> debug_fd{-1};
std::atomic<Serial> debug_serial{0};
StaticTask_t debug_tcb;
StackType_t debug_stack[cfg::kDebugTaskStackBytes / sizeof(StackType_t)];
std::atomic<bool> debug_sending{false};
char debug_record[256];
// URI handlers run on the single HTTP server task. The debug producer never
// reads this state; its send is separately bounded by the SDK socket timeout.
int64_t request_deadline = 0;
// Request body bytes already taken from the socket, and whether reading
// failed; drain() needs both to finish an interrupted body.
std::size_t body_read = 0;
bool body_broken = false;
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
    body_read = 0;
    body_broken = false;
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
// Takes up to 'count' request body bytes from the socket (at least one).
// Waits within the request deadline. Negative: the connection failed or the
// deadline passed; the rest of the body is lost.
int readBody(httpd_req_t* req, void* out, std::size_t count) {
  while (timekeeping::monotonicMs() < request_deadline) {
    const int n = httpd_req_recv(req, static_cast<char*>(out), count);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) break;
    body_read += n;
    return n;
  }
  body_broken = true;
  return -1;
}
// Reads and discards the unread rest of the request body so that closing or
// reusing the socket never resets the connection before the client has read
// the answer (D6). False when the body is longer than cfg::kDrainMaxBytes or
// could not be read to the end.
bool drain(httpd_req_t* req) {
  if (body_broken) return false;
  std::size_t left =
      req->content_len > body_read ? req->content_len - body_read : 0;
  if (left > cfg::kDrainMaxBytes) return false;
  char sink[256];
  while (left > 0) {
    const int n = readBody(req, sink, std::min(left, sizeof(sink)));
    if (n <= 0) return false;
    left -= n;
  }
  return true;
}
// A rejected request: status, a stable machine-readable code, an English
// message and, when one value caused it, the name of that field.
struct Fault {
  const char* status;
  const char* code;
  const char* message;
};
namespace fault {
inline constexpr Fault kOrigin{"403 Forbidden", "origin", "origin rejected"};
inline constexpr Fault kSession{"401 Unauthorized", "session",
                                "session required"};
inline constexpr Fault kPassword{"401 Unauthorized", "password",
                                 "password rejected"};
inline constexpr Fault kTrial{"409 Conflict", "trial", "OTA trial is read only"};
inline constexpr Fault kBusy{"409 Conflict", "busy", "operation busy"};
// The Web UI waits for the running job and then sends again.
inline constexpr Fault kSelectionBusy{"409 Conflict", "busy", "selection busy"};
inline constexpr Fault kChanged{"409 Conflict", "changed", "selection changed"};
inline constexpr Fault kUnknownRoute{"404 Not Found", "unknown_route",
                                     "unknown route"};
inline constexpr Fault kMethod{"405 Method Not Allowed", "method",
                               "method not allowed"};
inline constexpr Fault kBodyMissing{"400 Bad Request", "body_missing",
                                    "request body required"};
inline constexpr Fault kBodyLarge{"413 Payload Too Large", "body_too_large",
                                  "request body too large"};
inline constexpr Fault kBodyIncomplete{"400 Bad Request", "body_incomplete",
                                       "request body incomplete"};
inline constexpr Fault kContentType{"415 Unsupported Media Type",
                                    "content_type",
                                    "Content-Type must be application/json"};
inline constexpr Fault kJson{"400 Bad Request", "json",
                             "request body is not a JSON object"};
inline constexpr Fault kMemory{"507 Insufficient Storage", "memory",
                               "not enough memory"};
inline constexpr Fault kResponseMemory{"507 Insufficient Storage", "memory",
                                       "response memory limit"};
// From the services' error codes (faultFor).
inline constexpr Fault kState{"409 Conflict", "state",
                              "not possible in the current state"};
inline constexpr Fault kRejected{"400 Bad Request", "invalid",
                                 "value rejected"};
inline constexpr Fault kSize{"400 Bad Request", "size", "size not accepted"};
inline constexpr Fault kContent{"400 Bad Request", "content",
                                "file content rejected"};
inline constexpr Fault kStorageFull{"507 Insufficient Storage", "storage_full",
                                    "not enough space"};
inline constexpr Fault kMissing{"404 Not Found", "not_found", "not found"};
inline constexpr Fault kTimeout{"408 Request Timeout", "timeout",
                                "operation timed out"};
inline constexpr Fault kInternal{"500 Internal Server Error", "internal",
                                 "operation failed"};
}  // namespace fault
const Fault& faultFor(esp_err_t err) {
  switch (err) {
    case ESP_ERR_INVALID_STATE: return fault::kState;
    case ESP_ERR_INVALID_ARG: return fault::kRejected;
    case ESP_ERR_INVALID_SIZE: return fault::kSize;
    case ESP_ERR_INVALID_RESPONSE: return fault::kContent;
    case ESP_ERR_NO_MEM: return fault::kStorageFull;
    case ESP_ERR_NOT_FOUND: return fault::kMissing;
    case ESP_ERR_TIMEOUT: return fault::kTimeout;
    default: return fault::kInternal;
  }
}
// Answers with {"error", "code"[, "field"]}. The unread request body is
// drained first, so the client receives the answer instead of a reset (D6);
// only an undrainable body ends in a closed connection.
esp_err_t error(httpd_req_t* req, const char* status, const char* code,
                const char* message, const char* field = nullptr) {
  // Pairs httpd's "uri handler execution failed" with its cause. The query
  // is left out: the debug socket carries the session token there.
  ESP_LOGW("web", "%s %.*s: %s (%s%s%s)",
           http_method_str(static_cast<http_method>(req->method)),
           static_cast<int>(strcspn(req->uri, "?")), req->uri, status, code,
           field ? " " : "", field ? field : "");
  const bool drained = drain(req);
  if (!sendReady(req)) return ESP_FAIL;
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (!drained) httpd_resp_set_hdr(req, "Connection", "close");
  char result[256];
  if (field)
    snprintf(result, sizeof(result),
             "{\"error\":\"%s\",\"code\":\"%s\",\"field\":\"%s\"}", message,
             code, field);
  else
    snprintf(result, sizeof(result), "{\"error\":\"%s\",\"code\":\"%s\"}",
             message, code);
  httpd_resp_sendstr(req, result);
  return drained ? ESP_OK : ESP_FAIL;
}
esp_err_t error(httpd_req_t* req, const Fault& fault,
                const char* field = nullptr) {
  return error(req, fault.status, fault.code, fault.message, field);
}
// A request value that violates its rules: 400 with the settings reason name
// as code and the field's name.
esp_err_t invalid(httpd_req_t* req, Reason why, const char* message,
                  const char* field) {
  return error(req, "400 Bad Request", settings::reasonName(why), message,
               field);
}
// A failed service call, with the esp_err_t name kept in the log.
esp_err_t failed(httpd_req_t* req, esp_err_t err, const char* field = nullptr) {
  ESP_LOGW("web", "service error %s", esp_err_to_name(err));
  return error(req, faultFor(err), field);
}
esp_err_t rejected(httpd_req_t* req, const settings::Violation& why) {
  return invalid(req, why.reason, "settings rejected",
                 why.path[0] ? why.path.data() : nullptr);
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
// A JSON document on the capped allocator.
struct Doc {
  Doc() : doc(&allocator) {}
  football::JsonAllocator allocator;
  JsonDocument doc;
};
esp_err_t json(httpd_req_t* req, JsonDocument& doc) {
  if (doc.overflowed()) return error(req, fault::kResponseMemory);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  Writer writer(req);
  serializeJson(doc, writer);
  return writer.flush() && sendReady(req)
             ? httpd_resp_send_chunk(req, nullptr, 0)
             : ESP_FAIL;
}
esp_err_t ok(httpd_req_t* req, JsonDocument& doc) {
  doc.clear();
  doc["ok"] = true;
  return json(req, doc);
}
bool equal(const char* a, const char* b, std::size_t length) {
  uint8_t difference = 0;
  for (std::size_t i = 0; i < length; ++i)
    difference |= static_cast<uint8_t>(a[i] ^ b[i]);
  return difference == 0;
}
// DNS names are case-insensitive (W104).
bool sameName(const char* text, std::string_view name) {
  return strlen(text) == name.size() &&
         strncasecmp(text, name.data(), name.size()) == 0;
}
// The Host header names this device and an Origin header, if sent, is this
// device's own page (CSRF protection, docs/NETWORK.md).
bool origin(httpd_req_t* req) {
  char host[96]{}, request_origin[128]{};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
      host[0] == 0)
    return false;
  if (auto* colon = strchr(host, ':')) {
    if (strcmp(colon, ":80")) return false;
    *colon = 0;
  }
  const auto net = network::status();
  const auto model = settings::current();
  char name[72]{};
  snprintf(name, sizeof(name), "%.*s.local", model.hostname.length,
           model.hostname.data.data());
  if (strcasecmp(host, net.ip) && strcasecmp(host, net.ap_ip) &&
      strcasecmp(host, name) && !sameName(host, model.hostname.view()))
    return false;
  if (httpd_req_get_hdr_value_len(req, "Origin") == 0) return true;
  if (httpd_req_get_hdr_value_str(req, "Origin", request_origin,
                                  sizeof(request_origin)) != ESP_OK)
    return false;
  char expected[128];
  snprintf(expected, sizeof(expected), "http://%s", host);
  return strcasecmp(expected, request_origin) == 0;
}
// The live session whose token the request carries (header, or the query of
// the debug socket); the request moves its expiry. 0: none.
Serial authenticate(httpd_req_t* req, bool from_query = false) {
  char supplied[SessionTable::kTokenChars + 1]{};
  if (from_query) {
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "token", supplied, sizeof(supplied)) != ESP_OK)
      return 0;
  } else if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", supplied,
                                         sizeof(supplied)) != ESP_OK)
    return 0;
  const auto now = timekeeping::monotonicMs();
  SessionLock lock;
  return sessions.find(supplied, now);
}
// Receives the JSON object body of a request into 'doc'; nullptr when it is
// there, else why not.
const Fault* receive(httpd_req_t* req, JsonDocument& doc) {
  if (req->content_len == 0) return &fault::kBodyMissing;
  if (req->content_len > cfg::kApiBodyBytes) return &fault::kBodyLarge;
  char type[48];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) !=
          ESP_OK ||
      strncasecmp(type, "application/json", 16) != 0)
    return &fault::kContentType;
  auto* buffer = static_cast<char*>(std::malloc(req->content_len + 1));
  if (buffer == nullptr) return &fault::kMemory;
  std::size_t used = 0;
  while (used < req->content_len) {
    const int n = readBody(req, buffer + used, req->content_len - used);
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
  const bool complete = used == req->content_len;
  bool good =
      complete &&
      !deserializeJson(doc, reader,
                       DeserializationOption::NestingLimit(cfg::kJsonDepth));
  while (reader.at < used) {
    const char c = buffer[reader.at++];
    good &= c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }
  memset(buffer, 0, req->content_len + 1);
  std::free(buffer);
  if (!complete) return &fault::kBodyIncomplete;
  return good && doc.is<JsonObject>() ? nullptr : &fault::kJson;
}
// Origin, optionally the OTA trial, session and JSON body of a call that
// changes something; nullptr when all of it is in order. 'who' receives the
// calling session.
const Fault* admit(httpd_req_t* req, JsonDocument& doc, Serial* who = nullptr,
                   bool blocked_in_trial = false) {
  if (!origin(req)) return &fault::kOrigin;
  if (blocked_in_trial && ota::trial()) return &fault::kTrial;
  const auto serial = authenticate(req);
  if (!serial) return &fault::kSession;
  if (who) *who = serial;
  return receive(req, doc);
}
// Unsigned decimal text within [0, max].
bool parseUnsigned(const char* text, uint32_t max, uint32_t& out) {
  if (text == nullptr || *text < '0' || *text > '9') return false;
  char* end;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || *end != 0 || value > max) return false;
  out = value;
  return true;
}
// An integer JSON value within [0, max]; otherwise the reason it is not.
bool smallInt(JsonVariantConst value, uint32_t max, Reason& why) {
  if (value.isNull()) why = Reason::kRequired;
  else if (!value.is<uint32_t>()) why = Reason::kType;
  else if (value.as<uint32_t>() > max) why = Reason::kRange;
  else return true;
  return false;
}
[[gnu::noinline]] bool passwordSet() {
  return !settings::current().admin_password.empty();
}
[[gnu::noinline]] bool browserDebug() { return settings::current().browser_debug; }
// No admin password, or the one supplied.
[[gnu::noinline]] bool passwordAccepted(JsonVariantConst supplied) {
  const auto model = settings::current();
  if (model.admin_password.empty()) return true;
  const auto text = supplied.as<JsonString>();
  return text && text.size() == model.admin_password.length &&
         equal(text.c_str(), model.admin_password.data.data(), text.size());
}
// The primary provider of competition 'route'; false if there is none.
[[gnu::noinline]] bool primaryProvider(uint8_t route, cfg::Provider& out) {
  const auto model = settings::current();
  if (route >= model.competitions.size()) return false;
  out = model.competitions[route].primary.provider;
  return true;
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

// ---- Reading ---------------------------------------------------------------
// Read endpoints need no session (docs/NETWORK.md); a token the request does
// carry is accepted and moves the expiry of its session.
esp_err_t getSession(httpd_req_t* req) {
  Request request(req);
  Doc d;
  const bool valid = origin(req) && authenticate(req) != 0;
  d.doc["password_set"] = passwordSet();
  d.doc["valid"] = valid;
  return json(req, d.doc);
}
esp_err_t getSettings(httpd_req_t* req) {
  Request request(req);
  Doc d;
  settingsJson(d.doc.to<JsonObject>(), settings::current());
  return json(req, d.doc);
}
esp_err_t getSchema(httpd_req_t* req) {
  Request request(req);
  Doc d;
  auto out = d.doc.to<JsonObject>();
  schemaJson(out, settings::limits());
  out["width"] = hw::kDisplay.width;
  out["height"] = hw::kDisplay.height;
  out["round"] = hw::kDisplay.shape == hw::DisplayShape::kRound;
  out["antenna"] = hw::kBoard.has_antenna_switch;
  return json(req, d.doc);
}
esp_err_t getStatus(httpd_req_t* req) {
  Request request(req);
  if (httpd_req_get_hdr_value_len(req, "X-CSRF-Token") > 0) authenticate(req);
  Doc d;
  auto out = d.doc.to<JsonObject>();
  const auto net = network::status();
  const auto data = football::status();
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
  out["reset_reason"] = health::resetReason();
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
  // What the display shows and why (OQ-91). 'screen' is the index of the
  // screen in the settings' 'screens' array.
  const auto shown = ui::status();
  out["screen"] = static_cast<uint8_t>(shown.screen);
  out["screen_reason"] = ui::reasonName(shown.reason);
  // The club's current or next match and the state of each competition.
  time_t utc = 0;
  timekeeping::utcNow(utc);
  const auto overview = football::overview(utc);
  auto own = out["own_match"].to<JsonObject>();
  own["present"] = overview.own.present;
  if (overview.own.present) {
    own["competition"] = overview.own.route;
    own["phase"] = football::phaseName(overview.own.phase);
    if (overview.own.kickoff) own["kickoff"] = overview.own.kickoff;
    if (overview.own.home_goals >= 0) own["home_goals"] = overview.own.home_goals;
    if (overview.own.away_goals >= 0) own["away_goals"] = overview.own.away_goals;
    if (overview.own.minute >= 0) own["minute"] = overview.own.minute;
    own["home"] = overview.own.home.data;
    own["away"] = overview.own.away.data;
  }
  auto routes = out["competitions"].to<JsonArray>();
  for (std::size_t i = 0; i < cfg::kCompetitionMax; ++i) {
    auto row = routes.add<JsonObject>();
    row["matches"] = overview.routes[i].matches;
    row["provider"] = football::providerName(overview.routes[i].provider);
    row["error"] = data.error_route == i;
  }
  return json(req, d.doc);
}
esp_err_t getWifi(httpd_req_t* req) {
  Request request(req);
  Doc d;
  network::AccessPoint results[cfg::kScanEntries];
  const auto count = network::scanResults(results, cfg::kScanEntries);
  auto list = d.doc["networks"].to<JsonArray>();
  for (unsigned i = 0; i < count; ++i) {
    auto item = list.add<JsonObject>();
    item["ssid"] = results[i].ssid;
    item["rssi"] = results[i].rssi;
    item["secure"] = results[i].secure;
  }
  return json(req, d.doc);
}
esp_err_t getSelection(httpd_req_t* req) {
  Request request(req);
  Doc d;
  auto out = d.doc.to<JsonObject>();
  const auto state = football::selectionStatus();
  out["busy"] = state.busy;
  out["more"] = state.more;
  out["fixtures"] = state.fixtures;
  out["route"] = state.route;
  out["revision"] = state.revision;
  out["error"] = state.error.data;
  auto list = out["items"].to<JsonArray>();
  football::Selection item;
  if (!state.busy)
    for (unsigned i = 0; i < state.count; ++i) {
      if (!football::selectionAt(i, state.revision, item))
        return error(req, fault::kChanged);
      auto row = list.add<JsonObject>();
      row["id"] = item.id.data;
      row["name"] = item.name.data;
      row["season"] = item.season.data;
    }
  return json(req, d.doc);
}
// One match for the pair picker: provider fixture id, kickoff (UTC seconds,
// 0 = unknown) and the two teams.
void fixtureRow(JsonArray list, const football::Selection& item) {
  auto row = list.add<JsonObject>();
  row["id"] = item.id.data;
  row["kickoff"] = strtoll(item.season.data, nullptr, 10);
  row["name"] = item.name.data;
}
// ?competition=N&source=primary|fallback. Primary: the club's matches in the
// published data of the competition. Fallback: the result of the job
// POST /data/fixtures started (loaded stays false until it has finished for
// this competition).
esp_err_t getFixtures(httpd_req_t* req) {
  Request request(req);
  char query[64], value[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    return invalid(req, Reason::kRequired, "competition required",
                   "competition");
  uint32_t route = 0;
  if (httpd_query_key_value(query, "competition", value, sizeof(value)) !=
      ESP_OK)
    return invalid(req, Reason::kRequired, "competition required",
                   "competition");
  if (!parseUnsigned(value, cfg::kCompetitionMax - 1, route))
    return invalid(req, Reason::kRange, "competition rejected", "competition");
  if (httpd_query_key_value(query, "source", value, sizeof(value)) != ESP_OK)
    return invalid(req, Reason::kRequired, "source required", "source");
  const bool primary = !strcmp(value, "primary");
  if (!primary && strcmp(value, "fallback"))
    return invalid(req, Reason::kInvalid, "source rejected", "source");
  Doc d;
  auto out = d.doc.to<JsonObject>();
  out["source"] = primary ? "primary" : "fallback";
  out["competition"] = route;
  auto list = out["items"].to<JsonArray>();
  football::Selection item;
  if (primary) {
    cfg::Provider provider;
    if (!primaryProvider(route, provider))
      return invalid(req, Reason::kReference, "competition not defined",
                     "competition");
    uint32_t revision;
    const auto count = football::publishedFixtureCount(route, provider, revision);
    out["provider"] = football::providerName(provider);
    out["busy"] = false;
    out["loaded"] = true;
    for (unsigned i = 0; i < count; ++i) {
      if (!football::publishedFixtureAt(route, provider, i, revision, item))
        return error(req, fault::kChanged);
      fixtureRow(list, item);
    }
    return json(req, d.doc);
  }
  const auto state = football::selectionStatus();
  const bool mine = state.fixtures && state.route == route;
  out["busy"] = state.busy;
  out["loaded"] = mine && !state.busy && state.error.empty();
  out["error"] = mine && !state.busy ? state.error.data : "";
  if (mine && !state.busy)
    for (unsigned i = 0; i < state.count; ++i) {
      if (!football::selectionAt(i, state.revision, item))
        return error(req, fault::kChanged);
      fixtureRow(list, item);
    }
  return json(req, d.doc);
}
// Names of the stock images in the API (cfg::StockImage order).
constexpr const char* kStockNames[] = {"crest", "background"};
static_assert(std::size(kStockNames) == cfg::kStockImageCount,
              "one API name per stock image");
esp_err_t getImages(httpd_req_t* req) {
  Request request(req);
  Doc d;
  auto out = d.doc.to<JsonObject>();
  auto list = out["items"].to<JsonArray>();
  for (uint8_t id = 1; id <= cfg::kImageLibraryMax; ++id) {
    const auto e = images::info(id);
    if (!e.present) continue;
    auto row = list.add<JsonObject>();
    row["id"] = id;
    row["bytes"] = e.bytes;
    row["revision"] = images::revisionOf(id);
  }
  auto stock = out["stock"].to<JsonArray>();
  for (const char* name : kStockNames) stock.add(name);
  const auto usage = images::usage();
  out["count"] = usage.count;
  out["used"] = usage.bytes;
  out["quota"] = cfg::kImageTotalBytes;
  out["max"] = cfg::kImageLibraryMax;
  return json(req, d.doc);
}
// ?id=N or ?stock=crest|background, optionally &r=<revision>. The revision
// changes whenever the image does, so a URL that carries one never goes stale
// and may be cached for good.
esp_err_t getImageFile(httpd_req_t* req) {
  Request request(req);
  char query[48], value[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    return invalid(req, Reason::kRequired, "image required", "id");
  uint8_t id = cfg::kStockImageId;
  cfg::StockImage stock = cfg::kStockCrest;
  if (httpd_query_key_value(query, "id", value, sizeof(value)) == ESP_OK) {
    uint32_t number = 0;
    if (!parseUnsigned(value, cfg::kImageLibraryMax, number) || number < 1)
      return invalid(req, Reason::kRange, "image id rejected", "id");
    id = number;
  } else if (httpd_query_key_value(query, "stock", value, sizeof(value)) ==
             ESP_OK) {
    std::size_t found = std::size(kStockNames);
    for (std::size_t i = 0; i < std::size(kStockNames); ++i)
      if (!strcmp(value, kStockNames[i])) found = i;
    if (found == std::size(kStockNames))
      return invalid(req, Reason::kInvalid, "stock image rejected", "stock");
    stock = static_cast<cfg::StockImage>(found);
  } else
    return invalid(req, Reason::kRequired, "image required", "id");
  char path[96];
  if (!images::filePath(id, stock, path, sizeof(path)))
    return error(req, fault::kMissing, "id");
  FILE* file = fopen(path, "rb");
  if (file == nullptr) return error(req, fault::kMissing, "id");
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(
      req, "Cache-Control",
      httpd_query_key_value(query, "r", value, sizeof(value)) == ESP_OK
          ? "public, max-age=31536000, immutable"
          : "no-cache");
  uint8_t buffer[1024];
  esp_err_t err = ESP_OK;
  for (;;) {
    const auto count = fread(buffer, 1, sizeof(buffer), file);
    if (count == 0) break;
    if (!sendReady(req) ||
        httpd_resp_send_chunk(req, reinterpret_cast<char*>(buffer), count) !=
            ESP_OK) {
      err = ESP_FAIL;
      break;
    }
  }
  fclose(file);
  if (err == ESP_OK && sendReady(req)) return httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_FAIL;
}

// ---- Session ---------------------------------------------------------------
esp_err_t login(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (!origin(req)) return error(req, fault::kOrigin);
  if (const auto* f = receive(req, d.doc)) return error(req, *f);
  if (!passwordAccepted(d.doc["password"]))
    return error(req, fault::kPassword, "password");
  uint8_t bytes[SessionTable::kTokenChars / 2];
  esp_fill_random(bytes, sizeof(bytes));
  char fresh[SessionTable::kTokenChars + 1];
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    snprintf(fresh + i * 2, 3, "%02x", bytes[i]);
  const auto now = timekeeping::monotonicMs();
  {
    SessionLock lock;
    sessions.open(fresh, now);
  }
  d.doc.clear();
  d.doc["token"] = fresh;
  d.doc["expires_ms"] = cfg::kSessionLifetimeMs;
  return json(req, d.doc);
}
esp_err_t logout(httpd_req_t* req) {
  Request request(req);
  Doc d;
  Serial who = 0;
  if (const auto* f = admit(req, d.doc, &who)) return error(req, *f);
  {
    SessionLock lock;
    sessions.close(who);
  }
  return ok(req, d.doc);
}

// ---- Settings --------------------------------------------------------------
esp_err_t putSettings(httpd_req_t* req) {
  Request request(req);
  Doc d;
  Serial who = 0;
  if (const auto* f = admit(req, d.doc, &who, true)) return error(req, *f);
  auto model = settings::current();
  const auto old_password = model.admin_password;
  settings::Violation why;
  if (!applyJson(d.doc.as<JsonObjectConst>(), model, &why, settings::limits()))
    return rejected(req, why);
  const auto err = settings::save(model, &why);
  if (err == ESP_ERR_INVALID_ARG) return rejected(req, why);
  if (err != ESP_OK) return failed(req, err);
  // Saving keeps every session; a new password ends the other ones.
  if (!(old_password == model.admin_password)) {
    SessionLock lock;
    sessions.closeOthers(who);
  }
  return ok(req, d.doc);
}
esp_err_t exportSettings(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc)) return error(req, *f);
  const auto flag = d.doc["secrets"];
  if (!flag.isNull() && !flag.is<bool>())
    return invalid(req, Reason::kType, "export options rejected", "secrets");
  const bool secrets = flag.as<bool>();
  d.doc.clear();
  settingsJson(d.doc.to<JsonObject>(), settings::current(), secrets);
  return json(req, d.doc);
}
esp_err_t reboot(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc)) return error(req, *f);
  ota::reboot();
  return ok(req, d.doc);
}
esp_err_t reset(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc, nullptr, true)) return error(req, *f);
  const auto in = d.doc.as<JsonObjectConst>();
  if (strcmp(in["confirm"] | "", "reset"))
    return invalid(req, Reason::kInvalid, "confirmation required", "confirm");
  for (const char* option : {"images", "format_storage"})
    if (!in[option].isNull() && !in[option].is<bool>())
      return invalid(req, Reason::kType, "reset options rejected", option);
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  esp_err_t err = ESP_OK;
  if (in["format_storage"].as<bool>())
    err = images::format();
  else if (in["images"].as<bool>())
    err = images::removeAll();
  if (err == ESP_OK) err = settings::reset();
  if (err != ESP_OK) return failed(req, err);
  ota::reboot();
  return ok(req, d.doc);
}

// ---- Network, provider data, display --------------------------------------
esp_err_t wifiScan(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc)) return error(req, *f);
  const auto err = network::requestScan();
  return err == ESP_OK ? ok(req, d.doc) : failed(req, err);
}
esp_err_t dataRefresh(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc)) return error(req, *f);
  football::refresh();
  return ok(req, d.doc);
}
// Lists of competitions or clubs at a provider (the Web UI pickers).
esp_err_t dataSelect(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc)) return error(req, *f);
  const auto in = d.doc.as<JsonObjectConst>();
  Reason why;
  if (!smallInt(in["provider"], cfg::kProviderCount - 1, why))
    return invalid(req, why, "provider rejected", "provider");
  football::SelectionRequest job;
  job.provider = static_cast<cfg::Provider>(in["provider"].as<uint8_t>());
  if (!in["teams"].is<bool>())
    return invalid(req, in["teams"].isNull() ? Reason::kRequired : Reason::kType,
                   "selection types rejected", "teams");
  if (!in["offset"].isNull() && !in["offset"].is<uint16_t>())
    return invalid(req, Reason::kType, "selection types rejected", "offset");
  job.teams = in["teams"].as<bool>();
  job.offset = in["offset"].as<uint16_t>();
  for (const char* field : {"competition", "season", "search", "country"}) {
    if (!in[field].isNull() && !in[field].is<const char*>())
      return invalid(req, Reason::kType, "selection text rejected", field);
    auto text = in[field].as<JsonString>();
    const unsigned max = (!strcmp(field, "search") || !strcmp(field, "country"))
                             ? cfg::kSelectionTextMaxBytes
                         : !strcmp(field, "season") ? cfg::kSeasonMaxBytes
                                                    : cfg::kCompetitionIdMaxBytes;
    if (text && (text.size() > max || strlen(text.c_str()) != text.size()))
      return invalid(req, Reason::kTooLong, "selection length rejected", field);
  }
  job.competition.assign(in["competition"] | "");
  job.season.assign(in["season"] | "");
  job.search.assign(in["search"] | "");
  job.country.assign(in["country"] | "");
  // Distinct messages: the Web UI turns each into an explanation.
  if (!settings::isValidIdentifier(job.competition.data))
    return invalid(req, Reason::kInvalid, "competition or season invalid",
                   "competition");
  if (!settings::isValidSeason(job.provider, job.season.data))
    return invalid(req, Reason::kInvalid, "competition or season invalid",
                   "season");
  if (job.teams && job.competition.empty())
    return invalid(req, Reason::kRequired, "competition required",
                   "competition");
  if (job.teams && job.provider == cfg::Provider::kApiFootball &&
      job.season.empty())
    return invalid(req, Reason::kRequired, "season required", "season");
  const auto err = football::select(job);
  // The Web UI waits for the running job and then sends again.
  if (err == ESP_ERR_NOT_FINISHED) return error(req, fault::kSelectionBusy);
  return err == ESP_OK ? ok(req, d.doc) : failed(req, err);
}
// Starts loading the club's matches at the fallback source of a competition
// (GET /fixtures?source=fallback returns them).
esp_err_t dataFixtures(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc)) return error(req, *f);
  Reason why;
  if (!smallInt(d.doc["competition"], cfg::kCompetitionMax - 1, why))
    return invalid(req, why, "competition rejected", "competition");
  const auto err = football::selectFixtures(d.doc["competition"].as<uint8_t>());
  if (err == ESP_ERR_NOT_FINISHED) return error(req, fault::kSelectionBusy);
  if (err == ESP_ERR_INVALID_ARG)
    return invalid(req, Reason::kReference,
                   "competition has no complete fallback source",
                   "competition");
  return err == ESP_OK ? ok(req, d.doc) : failed(req, err);
}
// Quick actions of the Web UI: the same events the buttons post.
esp_err_t postDisplay(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc)) return error(req, *f);
  const auto value = d.doc["action"];
  if (value.isNull())
    return invalid(req, Reason::kRequired, "display action required", "action");
  if (!value.is<const char*>())
    return invalid(req, Reason::kType, "display action rejected", "action");
  const char* name = value.as<const char*>();
  ui::Action action;
  if (!strcmp(name, "next"))
    action = ui::Action::kNext;
  else if (!strcmp(name, "ip"))
    action = ui::Action::kShowIp;
  else
    return invalid(req, Reason::kInvalid, "display action rejected", "action");
  const auto err =
      events::post(events::Event::kUiAction, static_cast<uint32_t>(action));
  // All queue slots for UI actions are taken: the display is busy.
  if (err == ESP_ERR_TIMEOUT) return error(req, fault::kBusy);
  return err == ESP_OK ? ok(req, d.doc) : failed(req, err);
}

// ---- Images ----------------------------------------------------------------
esp_err_t deleteImage(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc, nullptr, true)) return error(req, *f);
  Reason why;
  if (!smallInt(d.doc["id"], cfg::kImageLibraryMax, why) ||
      d.doc["id"].as<uint8_t>() < 1)
    return invalid(req, why == Reason::kRequired || why == Reason::kType
                            ? why
                            : Reason::kRange,
                   "image id required", "id");
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  const auto err = images::remove(d.doc["id"].as<uint8_t>());
  return err == ESP_OK ? ok(req, d.doc) : failed(req, err);
}
// Streams the request body into the image library or the firmware slot.
esp_err_t upload(httpd_req_t* req, bool firmware) {
  Request request(req);
  if (!origin(req)) return error(req, fault::kOrigin);
  if (!firmware && ota::trial()) return error(req, fault::kTrial);
  if (!authenticate(req)) return error(req, fault::kSession);
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  // No X-Image-Id header: store a new image under the lowest free id; with
  // an id: replace that library image.
  uint8_t index = cfg::kStockImageId;
  if (!firmware) {
    char id[8]{};
    const auto found = httpd_req_get_hdr_value_str(req, "X-Image-Id", id, sizeof(id));
    if (found != ESP_ERR_NOT_FOUND) {
      uint32_t value = 0;
      if (found != ESP_OK ||
          !parseUnsigned(id, cfg::kImageLibraryMax, value) || value < 1)
        return invalid(req, Reason::kRange, "image id rejected", "X-Image-Id");
      index = value;
    }
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
  while (err == ESP_OK && received < req->content_len) {
    const auto capacity =
        firmware && !begun ? sizeof(buffer) - first : sizeof(buffer);
    const int count = readBody(
        req, reinterpret_cast<char*>(buffer) + (firmware && !begun ? first : 0),
        std::min<std::size_t>(capacity, req->content_len - received));
    if (count <= 0) {
      err = ESP_ERR_TIMEOUT;
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
    return failed(req, err);
  }
  if (firmware) ota::reboot();
  httpd_resp_set_type(req, "application/json");
  if (!sendReady(req)) return ESP_FAIL;
  if (firmware) return httpd_resp_sendstr(req, "{\"ok\":true}");
  char reply[24];
  snprintf(reply, sizeof(reply), "{\"ok\":true,\"id\":%u}",
           static_cast<unsigned>(images::uploadedId()));
  return httpd_resp_sendstr(req, reply);
}
esp_err_t uploadFirmware(httpd_req_t* req) { return upload(req, true); }
esp_err_t uploadImage(httpd_req_t* req) { return upload(req, false); }

// ---- Debug stream ----------------------------------------------------------
void closeSession(httpd_handle_t, int fd) {
  if (debug_fd.load() == fd) {
    debug_serial.store(0);
    debug_fd.store(-1);
  }
  if (auto* state = connection(fd)) {
    state->fd = -1;
    state->deadline = 0;
  }
  close(fd);
}
esp_err_t debug(httpd_req_t* req) {
  Request request(req);
  if (req->method == HTTP_GET) {
    // httpd has already answered the handshake when this runs, so a refusal
    // can only close the socket.
    if (!origin(req) || !browserDebug()) return ESP_FAIL;
    const auto serial = authenticate(req, true);
    if (!serial) return ESP_FAIL;
    const int fd = httpd_req_to_sockfd(req);
    int expected = -1;
    if (!debug_fd.compare_exchange_strong(expected, fd)) return ESP_FAIL;
    debug_serial.store(serial);
    return ESP_OK;
  }
  httpd_ws_frame_t frame{};
  frame.type = HTTPD_WS_TYPE_TEXT;
  const auto err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK || frame.len > 128) return ESP_FAIL;
  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    debug_serial.store(0);
    debug_fd.store(-1);
  }
  uint8_t discard[128];
  frame.payload = discard;
  return httpd_ws_recv_frame(req, &frame, sizeof(discard));
}
void streamDebug(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(cfg::kDebugStreamPeriodMs));
    const int fd = debug_fd.load();
    const auto serial = debug_serial.load();
    if (fd < 0 || serial == 0) continue;
    const auto now = timekeeping::monotonicMs();
    bool alive;
    {
      SessionLock lock;
      alive = sessions.alive(serial, now);
    }
    if (!alive || !browserDebug()) {
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
            debug_serial.store(0);
            debug_fd.store(-1);
            httpd_sess_trigger_close(server, socket);
          }
        },
        nullptr);
    if (err != ESP_OK) {
      debug_sending.store(false);
      debug_serial.store(0);
      debug_fd.store(-1);
      httpd_sess_trigger_close(server, fd);
    }
  }
}

// ---- Routes ----------------------------------------------------------------
struct Route {
  const char* path;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t*);
  bool websocket;
};
constexpr uint16_t kMaxHandlers = 40;
constexpr Route kRoutes[] = {
    {"/", HTTP_GET, page, false},
    {"/api/v1/session", HTTP_GET, getSession, false},
    {"/api/v1/status", HTTP_GET, getStatus, false},
    {"/api/v1/schema", HTTP_GET, getSchema, false},
    {"/api/v1/settings", HTTP_GET, getSettings, false},
    {"/api/v1/wifi", HTTP_GET, getWifi, false},
    {"/api/v1/selection", HTTP_GET, getSelection, false},
    {"/api/v1/fixtures", HTTP_GET, getFixtures, false},
    {"/api/v1/images", HTTP_GET, getImages, false},
    {"/api/v1/images/file", HTTP_GET, getImageFile, false},
    {"/api/v1/debug", HTTP_GET, debug, true},
    {"/api/v1/session", HTTP_POST, login, false},
    {"/api/v1/logout", HTTP_POST, logout, false},
    {"/api/v1/settings", HTTP_POST, putSettings, false},
    {"/api/v1/settings", HTTP_PUT, putSettings, false},
    {"/api/v1/export", HTTP_POST, exportSettings, false},
    {"/api/v1/wifi/scan", HTTP_POST, wifiScan, false},
    {"/api/v1/data/select", HTTP_POST, dataSelect, false},
    {"/api/v1/data/fixtures", HTTP_POST, dataFixtures, false},
    {"/api/v1/data/refresh", HTTP_POST, dataRefresh, false},
    {"/api/v1/display", HTTP_POST, postDisplay, false},
    {"/api/v1/images/delete", HTTP_POST, deleteImage, false},
    {"/api/v1/images/upload", HTTP_POST, uploadImage, false},
    {"/api/v1/ota", HTTP_POST, uploadFirmware, false},
    {"/api/v1/reboot", HTTP_POST, reboot, false},
    {"/api/v1/reset", HTTP_POST, reset, false},
};
static_assert(std::size(kRoutes) <= kMaxHandlers,
              "raise max_uri_handlers for the routes");
}  // namespace
esp_err_t init() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = cfg::kHttpStackBytes;
  config.max_open_sockets = cfg::kHttpSockets;
  config.max_uri_handlers = kMaxHandlers;
  config.recv_wait_timeout = 1;
  config.send_wait_timeout = 1;
  config.lru_purge_enable = true;
  config.open_fn = openSession;
  config.close_fn = closeSession;
  auto err = httpd_start(&server, &config);
  if (err != ESP_OK) return err;
  for (const auto& route : kRoutes) {
    httpd_uri_t uri{};
    uri.uri = route.path;
    uri.method = route.method;
    uri.handler = route.handler;
    uri.is_websocket = route.websocket;
    err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) break;
  }
  if (err == ESP_OK)
    err = httpd_register_err_handler(
        server, HTTPD_404_NOT_FOUND, [](httpd_req_t* req, httpd_err_code_t) {
          Request request(req);
          return !strncmp(req->uri, "/api/", 5)
                     ? error(req, fault::kUnknownRoute)
                     : page(req);
        });
  if (err == ESP_OK)
    err = httpd_register_err_handler(
        server, HTTPD_405_METHOD_NOT_ALLOWED,
        [](httpd_req_t* req, httpd_err_code_t) {
          Request request(req);
          return error(req, fault::kMethod);
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
