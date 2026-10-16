#include "web_service.h"

#include <strings.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <string_view>

#include "app_config.h"
#include "display_port.h"
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
#include "request_budget.h"
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
// Longest owner's short name in bytes (the stored text, UTF-8).
constexpr std::size_t kShortNameBytes =
    sizeof(football::TeamInfo{}.custom.data) - 1;

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
inline constexpr Fault kDebugOff{"403 Forbidden", "debug_disabled",
                                 "browser debug is disabled"};
inline constexpr Fault kScreenshotUnavailable{
    "503 Service Unavailable", "screenshot_unavailable",
    "display is unavailable for capture"};
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
// Appends 'text' to out[used..] as JSON string content (quotes, backslashes
// and control characters escaped) and keeps 'keep' bytes of 'cap' free. A
// text that does not fit is cut before a whole character and ends in "...".
std::size_t jsonText(char* out, std::size_t cap, std::size_t used,
                     const char* text, std::size_t keep) {
  const std::size_t end = cap - keep - 3;  // room for "..."
  for (const char* p = text; *p != 0;) {
    const auto c = static_cast<unsigned char>(*p);
    char piece[8];
    std::size_t n = 1;     // bytes written
    std::size_t step = 1;  // bytes read
    if (c == '"' || c == '\\') {
      piece[0] = '\\';
      piece[1] = static_cast<char>(c);
      n = 2;
    } else if (c < 0x20) {
      n = static_cast<std::size_t>(
          snprintf(piece, sizeof(piece), "\\u%04x", c));
    } else {
      step = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      // A UTF-8 character moves whole; a cut-off one ends with the text.
      for (n = 0; n < step && p[n] != 0; ++n) piece[n] = p[n];
      step = n;
    }
    if (used + n > end) {
      memcpy(out + used, "...", 3);
      return used + 3;
    }
    memcpy(out + used, piece, n);
    used += n;
    p += step;
  }
  return used;
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
  // Each text keeps room for the syntax and texts after it, so the answer is
  // valid JSON even for a long or odd field (an unknown key).
  char result[256];
  std::size_t used = 0;
  auto syntax = [&](const char* s) {
    const auto n = strlen(s);
    memcpy(result + used, s, n);
    used += n;
  };
  syntax("{\"error\":\"");
  used = jsonText(result, sizeof(result), used, message, 64);
  syntax("\",\"code\":\"");
  used = jsonText(result, sizeof(result), used, code, 24);
  if (field) {
    syntax("\",\"field\":\"");
    used = jsonText(result, sizeof(result), used, field, 3);
  }
  syntax("\"}");
  result[used] = 0;
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
// device's own page (CSRF protection, docs/API.md).
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
// Read endpoints need no session (docs/API.md); a token the request does
// carry is accepted and moves the expiry of its session (readBoundary()).
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
  out["ota_max"] = ota::capacity();
  out["selection_text_max"] = cfg::kSelectionTextMaxBytes;
  // The page checks imported short names against these before any write.
  out["short_names_max"] = cfg::kShortNamesMax;
  out["short_name_max"] = kShortNameBytes;
  return json(req, d.doc);
}
esp_err_t getStatus(httpd_req_t* req) {
  Request request(req);
  Doc d;
  auto out = d.doc.to<JsonObject>();
  const auto net = network::status();
  const auto data = football::status();
  files::Usage usage{};
  files::usage(usage);
  const auto* description = esp_app_get_description();
  out["version"] = description->version;
  char firmware_id[2 * sizeof(description->app_elf_sha256) + 1];
  for (std::size_t i = 0; i < sizeof(description->app_elf_sha256); ++i)
    snprintf(firmware_id + 2 * i, 3, "%02x",
             static_cast<unsigned>(description->app_elf_sha256[i]));
  out["firmware_id"] = firmware_id;
  out["target"] = ota::identity().target;
  out["layout"] = ota::identity().layout;
  out["ip"] = net.ip;
  out["ap_ip"] = net.ap_ip;
  out["network"] = network::stateName(net.state);
  out["network_reason"] =
      network::reasonName(network::offlineReason(net.state));
  auto initialization = out["network_init"].to<JsonObject>();
  initialization["ready"] = net.initialized;
  initialization["step"] = network::initStepKey(net.failure_step);
  initialization["error"] = net.init_error;
  initialization["error_name"] = esp_err_to_name(net.init_error);
  initialization["code"] = network::initStepCode(net.failure_step);
  initialization["retry_ms"] = net.retry_at_ms;
  const auto selected_image = images::status();
  auto image = out["image"].to<JsonObject>();
  image["active"] = selected_image.active;
  image["pending"] = selected_image.pending;
  image["id"] = selected_image.desired_id;
  image["role"] =
      selected_image.role == cfg::kStockCrest ? "crest" : "background";
  image["actual"] = images::fileSourceKey(selected_image.actual);
  image["failed"] = images::fileSourceKey(selected_image.failed);
  image["reason"] = images::failureKey(selected_image.reason);
  image["error"] = selected_image.error;
  image["error_name"] = esp_err_to_name(selected_image.error);
  image["code"] = images::failureCode(selected_image.reason);
  image["revision"] = selected_image.revision;
  image["attempt"] = selected_image.attempt;
  image["retry_ms"] = selected_image.retry_at_ms;
  out["ap"] = net.ap;
  out["rssi"] = net.rssi;
  out["time_valid"] = timekeeping::status().valid;
  out["uptime_s"] = timekeeping::monotonicMs() / 1000;
  out["reset_reason"] = health::resetReason();
  const auto heap = health::observeHeap();
  out["heap_free"] = heap.point.free_bytes;
  out["heap_largest"] = heap.point.largest_bytes;
  if (heap.scope == health::MinimumScope::kUnavailable)
    out["heap_min"] = nullptr;
  else
    out["heap_min"] = heap.point.minimum_bytes;
  out["heap_min_scope"] =
      heap.scope == health::MinimumScope::kLifetime   ? "lifetime"
      : heap.scope == health::MinimumScope::kInterval ? "interval"
                                                      : "unavailable";
  out["heap_min_start_ms"] = heap.interval_start_ms;
  const auto app_bytes = ota::imageBytes();
  if (app_bytes != 0)
    out["app_bytes"] = app_bytes;
  else
    out["app_bytes"] = nullptr;
  std::array<health::TaskRow, cfg::kHealthMaxTasks> stacks;
  const auto task_count = health::stackMinima(stacks.data(), stacks.size());
  auto tasks = out["tasks"].to<JsonArray>();
  for (std::size_t i = 0; i < task_count; ++i) {
    auto task = tasks.add<JsonObject>();
    task["name"] = stacks[i].name;
    task["id"] = stacks[i].id;
    task["stack_min_free"] = stacks[i].min_free_bytes;
    task["stack_usable_bytes"] = stacks[i].usable_bytes;
  }
  out["fs_total"] = usage.total_bytes;
  out["fs_used"] = usage.used_bytes;
  out["trial"] = ota::trial();
  out["settings_source"] = settings::originName(settings::origin());
  out["competitions_source"] =
      settings::originName(settings::competitionsOrigin());
  out["provider_started"] = data.started;
  out["provider_busy"] = data.busy;
  out["last_update_ms"] = data.fetched_ms;
  out["data_stale"] = data.stale;
  out["next_request_ms"] = data.next_ms;
  out["next_request_reason"] = football::cadenceName(data.cadence);
  auto counts = out["requests"].to<JsonArray>();
  for (auto count : data.requests) counts.add(count);
  // What the display shows and why (OQ-91). 'screen' is the index of the
  // screen in the settings' 'screens' array.
  const auto shown = ui::status();
  out["screen"] = static_cast<uint8_t>(shown.screen);
  out["screen_reason"] = ui::reasonName(shown.reason);
  out["screen_starting"] = shown.starting;
  // The club's current or next match and the state of each competition.
  time_t utc = 0;
  timekeeping::utcNow(utc);
  const auto overview = football::overview(utc);
  auto own = out["own_match"].to<JsonObject>();
  own["present"] = overview.own.present;
  if (overview.own.present) {
    own["competition"] = settings::competitionId(overview.own.route);
    own["phase"] = football::phaseName(overview.own.phase);
    if (overview.own.kickoff) own["kickoff"] = overview.own.kickoff;
    if (overview.own.home_goals >= 0) own["home_goals"] = overview.own.home_goals;
    if (overview.own.away_goals >= 0) own["away_goals"] = overview.own.away_goals;
    if (overview.own.minute >= 0) own["minute"] = overview.own.minute;
    own["home"] = overview.own.home.data;
    own["away"] = overview.own.away.data;
  }
  // One row per competition in list order, with its id.
  auto routes = out["competitions"].to<JsonArray>();
  for (std::size_t i = 0; i < cfg::kCompetitionMax; ++i) {
    const uint8_t id = settings::competitionId(i);
    if (id == 0) break;
    auto row = routes.add<JsonObject>();
    row["id"] = id;
    row["matches"] = overview.routes[i].matches;
    row["provider"] = football::providerName(overview.routes[i].provider);
    // Why the competition has no current data (OQ-280): the stable key for
    // the Web UI texts and the code the display shows.
    const auto* state = football::findState(data.competitions, id);
    const auto cause = state ? state->cause : football::Cause::kNone;
    row["cause"] = football::causeKey(cause);
    if (cause != football::Cause::kNone) {
      char code[football::kErrorCodeBytes];
      football::errorCode(code, state->provider, cause, state->http);
      row["code"] = code;
      row["error_provider"] = football::providerName(state->provider);
      if (state->http) row["http"] = state->http;
    }
    // What the device could not hold (OQ-310), whatever the cause shown.
    if (state && state->notices) {
      auto missing = row["missing"].to<JsonArray>();
      if (state->notices & football::kNoticeMatches) missing.add("matches");
      if (state->notices & football::kNoticeTable) missing.add("table");
      if (state->notices & football::kNoticeTeams) missing.add("teams");
      if (state->notices & football::kNoticeEvents) missing.add("events");
    }
    if (state && state->fetched_ms) row["last_update_ms"] = state->fetched_ms;
  }
  return json(req, d.doc);
}
esp_err_t getWifi(httpd_req_t* req) {
  Request request(req);
  Doc d;
  network::AccessPoint results[cfg::kScanEntries];
  network::ScanStatus scan{};
  const auto count = network::scanResults(results, cfg::kScanEntries, &scan);
  d.doc["busy"] = scan.busy;
  d.doc["error_code"] = scan.error_code;
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
  out["revision"] = state.revision;
  out["error_code"] = football::causeKey(state.cause);
  if (state.cause != football::Cause::kNone) {
    char code[football::kErrorCodeBytes];
    football::errorCode(code, state.provider, state.cause, state.http);
    out["code"] = code;
    if (state.http) out["http"] = state.http;
  }
  // What holds the next selection request and for how many seconds more.
  const int64_t left = state.wait_ms - timekeeping::monotonicMs();
  const bool waits = state.busy && state.wait_ms && left > 0;
  out["wait"] =
      football::cadenceName(waits ? state.wait : football::Cadence::kNone);
  out["wait_s"] = waits ? (left + 999) / 1000 : 0;
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
// ?competition=N: the teams of the competition in the published data with
// the owner's short names.
esp_err_t getTeams(httpd_req_t* req) {
  Request request(req);
  char query[64], value[16];
  uint32_t id = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "competition", value, sizeof(value)) !=
          ESP_OK)
    return invalid(req, Reason::kRequired, "competition required",
                   "competition");
  if (!parseUnsigned(value, UINT8_MAX, id) || id == 0)
    return invalid(req, Reason::kRange, "competition rejected", "competition");
  Doc d;
  auto out = d.doc.to<JsonObject>();
  out["competition"] = id;
  out["max"] = cfg::kShortNamesMax;
  auto list = out["items"].to<JsonArray>();
  uint32_t revision;
  const auto count = football::teamCount(static_cast<uint8_t>(id), revision);
  football::TeamInfo team;
  for (unsigned i = 0; i < count; ++i) {
    if (!football::teamAt(static_cast<uint8_t>(id), i, revision, team))
      return error(req, fault::kChanged);
    auto row = list.add<JsonObject>();
    row["provider"] = static_cast<unsigned>(team.provider);
    row["id"] = team.id;
    row["short_name"] = team.short_name.data;
    row["custom"] = team.custom.data;
  }
  return json(req, d.doc);
}
// One short-name entry {provider, id, name}; the field that is not valid
// otherwise ('why' and 'field' set).
bool shortNameValid(JsonObjectConst in, Reason& why, const char*& field) {
  field = "provider";
  if (!smallInt(in["provider"], cfg::kProviderCount - 1, why)) return false;
  field = "id";
  if (!smallInt(in["id"], UINT32_MAX, why) || in["id"].as<uint32_t>() == 0) {
    why = in["id"].isNull() ? Reason::kRequired : Reason::kRange;
    return false;
  }
  field = "name";
  const auto name = in["name"].as<JsonString>();
  if (!in["name"].is<const char*>()) {
    why = in["name"].isNull() ? Reason::kRequired : Reason::kType;
    return false;
  }
  why = Reason::kTooLong;
  if (name.size() > kShortNameBytes) return false;
  why = Reason::kInvalid;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const auto c = static_cast<unsigned char>(name.c_str()[i]);
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}
// {provider, id, name}: sets the owner's short name of a team ("" removes
// it).
esp_err_t postTeam(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc, nullptr, true)) return error(req, *f);
  const auto in = d.doc.as<JsonObjectConst>();
  Reason why;
  const char* field;
  if (!shortNameValid(in, why, field))
    return invalid(req, why, "short name rejected", field);
  const auto name = in["name"].as<JsonString>();
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  const auto err = football::setShortName(
      static_cast<cfg::Provider>(in["provider"].as<uint8_t>()),
      in["id"].as<uint32_t>(), std::string_view(name.c_str(), name.size()));
  if (err != ESP_OK) return failed(req, err);
  return ok(req, d.doc);
}
// {short_names: [{provider, id, name}]}: replaces every short name (import
// of a settings export; [] removes them all).
esp_err_t putTeams(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc, nullptr, true)) return error(req, *f);
  const auto in = d.doc.as<JsonObjectConst>();
  for (const auto key : in)
    if (!sameText(key.key(), "short_names"))
      return invalid(req, Reason::kUnknownKey, "key rejected",
                     key.key().c_str());
  const auto list = in["short_names"];
  if (!list.is<JsonArrayConst>())
    return invalid(req, list.isNull() ? Reason::kRequired : Reason::kType,
                   "short names rejected", "short_names");
  if (list.size() > cfg::kShortNamesMax)
    return invalid(req, Reason::kTooMany, "too many short names",
                   "short_names");
  // The table (1.5 KB) stays off the httpd stack.
  std::unique_ptr<football::persist::ShortNames> table(
      new (std::nothrow) football::persist::ShortNames);
  if (!table) return failed(req, ESP_ERR_NO_MEM);
  char path[40];
  unsigned i = 0;
  for (const auto entry : list.as<JsonArrayConst>()) {
    Reason why = Reason::kType;
    const char* field = nullptr;
    const auto item = entry.as<JsonObjectConst>();
    if (entry.is<JsonObjectConst>() && shortNameValid(item, why, field)) {
      const auto name = item["name"].as<JsonString>();
      const auto provider =
          static_cast<cfg::Provider>(item["provider"].as<uint8_t>());
      const auto id = item["id"].as<uint32_t>();
      // An export carries no empty name and every team once.
      why = name.size() == 0 ? Reason::kRequired : Reason::kDuplicate;
      field = name.size() == 0 ? "name" : "id";
      if (name.size() != 0 && table->find(provider, id) == nullptr) {
        table->set(provider, id, std::string_view(name.c_str(), name.size()));
        ++i;
        continue;
      }
    }
    snprintf(path, sizeof(path),
             field ? "short_names[%u].%s" : "short_names[%u]", i,
             field ? field : "");
    return invalid(req, why, "short name rejected", path);
  }
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  const auto err = football::replaceShortNames(*table);
  if (err != ESP_OK) return failed(req, err);
  return ok(req, d.doc);
}
// Names of the image roles (stock images) in the API (cfg::StockImage order).
constexpr const char* kStockNames[] = {"crest", "background"};
static_assert(std::size(kStockNames) == cfg::kStockImageCount,
              "one API name per stock image");
// The role called 'name'; cfg::kStockImageCount if there is none.
cfg::StockImage roleNamed(std::string_view name) {
  for (std::size_t i = 0; i < std::size(kStockNames); ++i)
    if (name == kStockNames[i]) return static_cast<cfg::StockImage>(i);
  return cfg::kStockImageCount;
}
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
  // Each role shows its replacement when one exists, else the factory image.
  auto roles = out["roles"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg::kStockImageCount; ++i) {
    const auto stock = static_cast<cfg::StockImage>(i);
    const auto role = images::role(stock);
    auto row = roles.add<JsonObject>();
    row["name"] = kStockNames[i];
    row["replaced"] = role.replaced;
    row["bytes"] = role.bytes;
    row["revision"] = images::roleRevision(stock);
  }
  const auto usage = images::usage();
  out["count"] = usage.count;
  out["used"] = usage.bytes;
  out["quota"] = cfg::kImageTotalBytes;
  out["max"] = cfg::kImageLibraryMax;
  return json(req, d.doc);
}
// ?id=N or ?stock=crest|background, optionally &r=<revision>. ?display=1
// reads the actual selected file, including factory fallback while a damaged
// replacement remains. This dynamic selection is never cached as immutable.
esp_err_t getImageFile(httpd_req_t* req) {
  Request request(req);
  char query[48], value[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    return invalid(req, Reason::kRequired, "image required", "id");
  uint8_t id = cfg::kStockImageId;
  cfg::StockImage stock = cfg::kStockCrest;
  const bool selected =
      httpd_query_key_value(query, "display", value, sizeof(value)) == ESP_OK;
  if (selected) {
    if (strcmp(value, "1") != 0)
      return invalid(req, Reason::kInvalid, "display image rejected",
                     "display");
  } else if (httpd_query_key_value(query, "id", value, sizeof(value)) ==
             ESP_OK) {
    uint32_t number = 0;
    if (!parseUnsigned(value, cfg::kImageLibraryMax, number) || number < 1)
      return invalid(req, Reason::kRange, "image id rejected", "id");
    id = number;
  } else if (httpd_query_key_value(query, "stock", value, sizeof(value)) ==
             ESP_OK) {
    stock = roleNamed(value);
    if (stock == cfg::kStockImageCount)
      return invalid(req, Reason::kInvalid, "stock image rejected", "stock");
  } else
    return invalid(req, Reason::kRequired, "image required", "id");
  char path[96];
  const bool found =
      selected ? images::filePath(images::status(), path, sizeof(path))
               : images::filePath(id, stock, path, sizeof(path));
  if (!found) return error(req, fault::kMissing, "id");
  FILE* file = fopen(path, "rb");
  if (file == nullptr) return error(req, fault::kMissing, "id");
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control",
                     !selected && httpd_query_key_value(query, "r", value,
                                                        sizeof(value)) == ESP_OK
                         ? "public, max-age=31536000, immutable"
                         : "no-cache");
  uint8_t buffer[1024];
  esp_err_t err = ESP_OK;
  for (;;) {
    const auto count = fread(buffer, 1, sizeof(buffer), file);
    if (count == 0) {
      if (ferror(file)) err = ESP_FAIL;
      break;
    }
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
  const auto err = settings::save(model, &why, true);
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
  // The competition store (2.5 KB) stays off the httpd stack.
  std::unique_ptr<settings::Competitions> list(new (std::nothrow)
                                                   settings::Competitions);
  if (!list) return failed(req, ESP_ERR_NO_MEM);
  settings::competitions(*list);
  std::unique_ptr<football::persist::ShortNames> names(
      new (std::nothrow) football::persist::ShortNames);
  if (!names) return failed(req, ESP_ERR_NO_MEM);
  football::shortNames(*names);
  d.doc.clear();
  auto out = d.doc.to<JsonObject>();
  settingsJson(out, settings::current(), secrets);
  competitionsJson(out, *list);
  // The owner's short names, imported through PUT /teams.
  auto rows = out["short_names"].to<JsonArray>();
  for (std::size_t i = 0; i < cfg::kShortNamesMax; ++i) {
    const auto& name = names->at(i);
    if (name.id == 0) continue;
    auto row = rows.add<JsonObject>();
    row["provider"] = static_cast<unsigned>(name.provider);
    row["id"] = name.id;
    row["name"] = name.name.data;
  }
  return json(req, d.doc);
}
// ---- Competitions ----------------------------------------------------------
// The competition store has its own record and endpoint (OQ-200); the
// settings JSON does not carry it.
esp_err_t getCompetitions(httpd_req_t* req) {
  Request request(req);
  std::unique_ptr<settings::Competitions> list(new (std::nothrow)
                                                   settings::Competitions);
  if (!list) return failed(req, ESP_ERR_NO_MEM);
  settings::competitions(*list);
  Doc d;
  competitionsJson(d.doc.to<JsonObject>(), *list);
  return json(req, d.doc);
}
// A list in the body replaces that list; the answer is the saved store with
// the ids of new entries.
esp_err_t putCompetitions(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc, nullptr, true)) return error(req, *f);
  std::unique_ptr<settings::Competitions> list(new (std::nothrow)
                                                   settings::Competitions);
  if (!list) return failed(req, ESP_ERR_NO_MEM);
  settings::competitions(*list);
  settings::Violation why;
  if (!applyCompetitionsJson(d.doc.as<JsonObjectConst>(), *list, &why))
    return rejected(req, why);
  const auto err = settings::saveCompetitions(*list, &why);
  if (err == ESP_ERR_INVALID_ARG) return rejected(req, why);
  if (err != ESP_OK) return failed(req, err);
  d.doc.clear();
  auto out = d.doc.to<JsonObject>();
  out["ok"] = true;
  competitionsJson(out, *list);
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
  if (!sameText(in["confirm"].as<JsonString>(), "reset"))
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
  // The guard keeps the provider worker from storing a counter meanwhile.
  if (err == ESP_OK) err = football::budget::erase();
  // Kept data and the owner's short names go with the settings.
  if (err == ESP_OK) football::eraseFiles();
  if (err != ESP_OK) return failed(req, err);
  ota::reboot();
  return ok(req, d.doc);
}
// Repair only the image filesystem. Formatting also removes the stock files;
// they are restored by USB uploadfs, not by resetting the settings.
esp_err_t repairImages(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc, nullptr, true)) return error(req, *f);
  if (!sameText(d.doc["confirm"].as<JsonString>(), "repair"))
    return invalid(req, Reason::kInvalid, "confirmation required", "confirm");
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  const auto formatted = images::format();
  if (formatted != ESP_OK) return failed(req, formatted);
  football::filesLost();
  auto model = settings::current();
  esp_err_t saved = ESP_OK;
  if (settings::dropMissingImages(model, images::present()))
    saved = settings::save(model);
  d.doc.clear();
  d.doc["storage_formatted"] = true;
  if (saved != ESP_OK) {
    ESP_LOGW("web", "image storage formatted; reference save failed: %s",
             esp_err_to_name(saved));
    httpd_resp_set_status(req, "500 Internal Server Error");
    d.doc["code"] = "repair_references";
    d.doc["error"] =
        "image storage formatted but references could not be saved";
  } else
    d.doc["ok"] = true;
  return json(req, d.doc);
}

// ---- Network, provider data, display --------------------------------------
esp_err_t wifiScan(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc)) return error(req, *f);
  const auto err = network::requestScan();
  if (err == ESP_ERR_NOT_FINISHED) return error(req, fault::kBusy);
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
  // Only the fields this request uses are read and checked, so stale form
  // content of an unused field cannot block a list (W130): the competition
  // for a club list, the season for a club list or an OpenLigaDB competition
  // list, the country for an API-Football competition list.
  const bool openliga = job.provider == cfg::Provider::kOpenLigaDb;
  const struct {
    const char* name;
    bool used;
    unsigned max;
  } fields[] = {
      {"competition", job.teams, cfg::kCompetitionIdMaxBytes},
      {"season", job.teams || openliga, cfg::kSeasonMaxBytes},
      {"search", true, cfg::kSelectionTextMaxBytes},
      {"country", !job.teams && job.provider == cfg::Provider::kApiFootball,
       cfg::kSelectionTextMaxBytes},
  };
  for (const auto& field : fields) {
    if (!field.used) continue;
    if (!in[field.name].isNull() && !in[field.name].is<const char*>())
      return invalid(req, Reason::kType, "selection text rejected", field.name);
    auto text = in[field.name].as<JsonString>();
    if (text &&
        (text.size() > field.max || strlen(text.c_str()) != text.size()))
      return invalid(req, Reason::kTooLong, "selection length rejected",
                     field.name);
  }
  if (job.teams) job.competition.assign(in["competition"] | "");
  if (job.teams || openliga) job.season.assign(in["season"] | "");
  job.search.assign(in["search"] | "");
  if (fields[3].used) job.country.assign(in["country"] | "");
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
  const auto name = value.as<JsonString>();
  ui::Action action;
  if (sameText(name, "next"))
    action = ui::Action::kNext;
  else if (sameText(name, "ip"))
    action = ui::Action::kShowIp;
  else
    return invalid(req, Reason::kInvalid, "display action rejected", "action");
  const auto err =
      events::post(events::Event::kUiAction,
                   ui::actionPayload(action, settings::generation()));
  // All queue slots for UI actions are taken: the display is busy.
  if (err == ESP_ERR_TIMEOUT) return error(req, fault::kBusy);
  return err == ESP_OK ? ok(req, d.doc) : failed(req, err);
}

// ---- Images ----------------------------------------------------------------
esp_err_t deleteImage(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc, nullptr, true)) return error(req, *f);
  // Id 0 passes smallInt() and is out of range like one above the maximum.
  Reason why = Reason::kRange;
  if (!smallInt(d.doc["id"], cfg::kImageLibraryMax, why) ||
      d.doc["id"].as<uint8_t>() < 1)
    return invalid(req, why, "image id required", "id");
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  const auto err = images::remove(d.doc["id"].as<uint8_t>());
  return err == ESP_OK ? ok(req, d.doc) : failed(req, err);
}
// {"role":"crest"|"background","action":"reset"} deletes the replacement, so
// the role shows its factory image again; {"role":…,"action":"copy","id":N}
// makes a copy of library image N the replacement (the library image stays).
esp_err_t postImageRole(httpd_req_t* req) {
  Request request(req);
  Doc d;
  if (const auto* f = admit(req, d.doc, nullptr, true)) return error(req, *f);
  const auto name = d.doc["role"];
  if (name.isNull())
    return invalid(req, Reason::kRequired, "image role required", "role");
  if (!name.is<const char*>())
    return invalid(req, Reason::kType, "image role rejected", "role");
  const auto text = name.as<JsonString>();
  const auto role = roleNamed(std::string_view(text.c_str(), text.size()));
  if (role == cfg::kStockImageCount)
    return invalid(req, Reason::kInvalid, "image role rejected", "role");
  const auto action = d.doc["action"];
  if (action.isNull())
    return invalid(req, Reason::kRequired, "role action required", "action");
  if (!action.is<const char*>())
    return invalid(req, Reason::kType, "role action rejected", "action");
  const bool copy = sameText(action.as<JsonString>(), "copy");
  if (!copy && !sameText(action.as<JsonString>(), "reset"))
    return invalid(req, Reason::kInvalid, "role action rejected", "action");
  Reason why = Reason::kRange;
  if (copy && (!smallInt(d.doc["id"], cfg::kImageLibraryMax, why) ||
               d.doc["id"].as<uint8_t>() < 1))
    return invalid(req, why == Reason::kRequired || why == Reason::kType
                            ? why
                            : Reason::kRange,
                   "image id required", "id");
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  const auto err = copy ? images::copyToRole(d.doc["id"].as<uint8_t>(), role)
                        : images::resetRole(role);
  return err == ESP_OK ? ok(req, d.doc)
                       : failed(req, err, err == ESP_ERR_NOT_FOUND ? "id" : nullptr);
}
// Streams the request body into the image library or the firmware slot.
esp_err_t upload(httpd_req_t* req, bool firmware) {
  Request request(req);
  if (!origin(req)) return error(req, fault::kOrigin);
  if (!firmware && ota::trial()) return error(req, fault::kTrial);
  if (!authenticate(req)) return error(req, fault::kSession);
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  // Automatic uploads skip stored references and the browser's reserved ids.
  // X-Image-Id replaces/restores that id; X-Image-Role replaces a role.
  uint8_t index = cfg::kStockImageId;
  settings::ImageMask reserved = 0;
  auto role = cfg::kStockImageCount;
  if (!firmware) {
    char id[8]{};
    const auto found =
        httpd_req_get_hdr_value_str(req, "X-Image-Id", id, sizeof(id));
    if (found != ESP_ERR_NOT_FOUND) {
      uint32_t value = 0;
      if (found != ESP_OK || !parseUnsigned(id, cfg::kImageLibraryMax, value) ||
          value < 1)
        return invalid(req, Reason::kRange, "image id rejected", "X-Image-Id");
      index = value;
    }
    char name[16]{};
    const auto named =
        httpd_req_get_hdr_value_str(req, "X-Image-Role", name, sizeof(name));
    if (named != ESP_ERR_NOT_FOUND) {
      if (found != ESP_ERR_NOT_FOUND)
        return invalid(req, Reason::kConflict, "image id and role together",
                       "X-Image-Role");
      if (named == ESP_OK) role = roleNamed(name);
      if (role == cfg::kStockImageCount)
        return invalid(req, Reason::kInvalid, "image role rejected",
                       "X-Image-Role");
    }
    char mask[8]{};
    const auto masked = httpd_req_get_hdr_value_str(req, "X-Image-Reserved",
                                                    mask, sizeof(mask));
    if (masked != ESP_ERR_NOT_FOUND) {
      if (found != ESP_ERR_NOT_FOUND || named != ESP_ERR_NOT_FOUND)
        return invalid(req, Reason::kConflict,
                       "reserved ids require an automatic library upload",
                       "X-Image-Reserved");
      uint32_t value = 0;
      if (masked != ESP_OK ||
          !parseUnsigned(mask, settings::kLibraryImageMask, value) ||
          (value & ~uint32_t{settings::kLibraryImageMask}) != 0)
        return invalid(req, Reason::kRange, "reserved image ids rejected",
                       "X-Image-Reserved");
      reserved = static_cast<settings::ImageMask>(value);
    }
  }
  const int64_t deadline = request_deadline;
  uint8_t buffer[1024];
  std::size_t received = 0, first = 0;
  bool begun = false;
  esp_err_t err = ESP_OK;
  if (!firmware) {
    err = role == cfg::kStockImageCount
              ? images::begin(index, req->content_len, reserved)
              : images::beginRole(role, req->content_len);
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
  char reply[40];
  if (role != cfg::kStockImageCount)
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"role\":\"%s\"}",
             kStockNames[role]);
  else
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
// httpd answers a WebSocket handshake itself and never calls the handler for
// it; this runs just before that answer (ws_pre_handshake_cb). A refusal is an
// ordinary HTTP answer, and ESP_FAIL then closes the socket.
esp_err_t debugAdmit(httpd_req_t* req) {
  Request request(req);
  const Fault* refusal = nullptr;
  Serial serial = 0;
  if (!origin(req))
    refusal = &fault::kOrigin;
  else if (!browserDebug())
    refusal = &fault::kDebugOff;
  else if (!(serial = authenticate(req, true)))
    refusal = &fault::kSession;
  else {
    int expected = -1;
    if (!debug_fd.compare_exchange_strong(expected, httpd_req_to_sockfd(req)))
      refusal = &fault::kBusy;
  }
  if (refusal) {
    error(req, *refusal);
    return ESP_FAIL;
  }
  debug_serial.store(serial);
  return ESP_OK;
}
// The frames a client sends on the open stream; their content is ignored.
esp_err_t debug(httpd_req_t* req) {
  Request request(req);
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
    // The first failing competition's code and how many fail.
    char code[football::kErrorCodeBytes] = "";
    unsigned errors = 0;
    for (const auto& state : data.competitions)
      if (state.active && state.cause != football::Cause::kNone &&
          errors++ == 0)
        football::errorCode(code, state.provider, state.cause, state.http);
    const int length = snprintf(
        debug_record, sizeof(debug_record),
        "{\"ms\":%lld,\"net\":\"%s\",\"heap\":%u,\"rev\":%"
        "lu,\"stale\":%s,\"busy\":%s,\"error\":\"%s\",\"errors\":%u,"
        "\"requests\":[%lu,%lu,"
        "%lu,%lu]}",
        static_cast<long long>(timekeeping::monotonicMs()),
        network::stateName(net.state),
        static_cast<unsigned>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned long>(data.revision),
        data.stale ? "true" : "false", data.busy ? "true" : "false", code,
        errors, static_cast<unsigned long>(data.requests[0]),
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

// ---- Screenshot ------------------------------------------------------------
// Used only on the screenshot socket, which is closed after its response.
// The SDK repeats its send function on partial writes: each invocation must
// share the absolute capture deadline, not start another socket timeout.
int screenshotSend(httpd_handle_t, int fd, const char* bytes, size_t count,
                   int flags) {
  if (bytes == nullptr) return HTTPD_SOCK_ERR_INVALID;
  const auto left = request_deadline - timekeeping::monotonicMs();
  if (left <= 0) return HTTPD_SOCK_ERR_FAIL;
  const auto wait_ms = std::min<int64_t>(cfg::kSocketTimeoutMs, left);
  timeval timeout{static_cast<long>(wait_ms / 1000),
                  static_cast<long>((wait_ms % 1000) * 1000)};
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
    return HTTPD_SOCK_ERR_FAIL;
  const int result = send(fd, bytes, count, flags);
  // Zero progress must also terminate httpd_send_all(), which otherwise loops.
  return result > 0 && timekeeping::monotonicMs() < request_deadline
             ? result
             : HTTPD_SOCK_ERR_FAIL;
}
struct ScreenshotResponse {
  httpd_req_t* req;
  bool started = false;

  static void word(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
  }
  esp_err_t sendChunk(const uint8_t* bytes, std::size_t count) {
    return httpd_resp_send_chunk(req, reinterpret_cast<const char*>(bytes),
                                 count);
  }
  static esp_err_t begin(const display::ScreenshotFrame& frame, void* ctx) {
    auto& self = *static_cast<ScreenshotResponse*>(ctx);
    const auto err = httpd_sess_set_send_override(
        server, httpd_req_to_sockfd(self.req), screenshotSend);
    if (err != ESP_OK) return err;
    // From here even a partially sent header requires closing the socket.
    // A null override cannot restore the SDK's private default send function.
    self.started = true;
    if (httpd_resp_set_type(self.req, "application/octet-stream") != ESP_OK ||
        httpd_resp_set_hdr(self.req, "Cache-Control", "no-store") != ESP_OK ||
        httpd_resp_set_hdr(self.req, "Connection", "close") != ESP_OK ||
        httpd_resp_set_hdr(self.req, "X-Content-Type-Options", "nosniff") !=
            ESP_OK)
      return ESP_FAIL;
    uint8_t header[12]{'F', 'B', 'S', '1'};
    word(header + 4, frame.width);
    word(header + 6, frame.height);
    header[8] = frame.rotation;
    header[9] = frame.round ? 1 : 0;
    header[10] = 1;  // Compact little-endian RGB565.
    header[11] = cfg::kScreenshotRows;
    return self.sendChunk(header, sizeof(header));
  }
  static esp_err_t strip(const display::ScreenshotStrip& strip, void* ctx) {
    auto& self = *static_cast<ScreenshotResponse*>(ctx);
    uint8_t header[8];
    word(header, strip.x);
    word(header + 2, strip.y);
    word(header + 4, strip.width);
    word(header + 6, strip.height);
    const auto err = self.sendChunk(header, sizeof(header));
    return err == ESP_OK
               ? self.sendChunk(strip.pixels,
                                std::size_t{strip.width} * strip.height * 2)
               : err;
  }
};
esp_err_t postScreenshot(httpd_req_t* req) {
  Request request(req);
  {
    Doc d;
    // Diagnostics do not change saved settings and remain useful in OTA trial.
    if (const auto* f = admit(req, d.doc)) return error(req, *f);
  }
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  request_deadline = std::min(request_deadline, timekeeping::monotonicMs() +
                                                    cfg::kScreenshotDeadlineMs);
  ScreenshotResponse response{req};
  const display::ScreenshotSink sink{ScreenshotResponse::begin,
                                     ScreenshotResponse::strip};
  const auto err = display::screenshot(sink, &response, request_deadline);
  if (!response.started) {
    // This is temporary capture RAM, not a full image-storage quota.
    if (err == ESP_ERR_NO_MEM) return error(req, fault::kMemory);
    return err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_SUPPORTED
               ? error(req, fault::kScreenshotUnavailable)
               : failed(req, err);
  }
  // Do not turn an incomplete stream into a valid image or append JSON to it.
  if (err != ESP_OK) return ESP_FAIL;
  const uint8_t end[8]{};
  if (response.sendChunk(end, sizeof(end)) != ESP_OK ||
      httpd_resp_send_chunk(req, nullptr, 0) != ESP_OK)
    return ESP_FAIL;
  // Queue the close only after capture has detached and released its strip.
  // No producer waits for work on this HTTP task. A failed enqueue makes the
  // handler fail, so the server closes the socket directly instead.
  return httpd_sess_trigger_close(server, httpd_req_to_sockfd(req));
}

// ---- Routes ----------------------------------------------------------------
struct Route {
  const char* path;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t*);
  // WebSocket route when set: decides before httpd answers the handshake.
  esp_err_t (*handshake)(httpd_req_t*);
};
// The common boundary of every read route: a token the request carries
// moves the expiry of its session, then the route answers (a read needs no
// session, so an unknown or missing token changes nothing).
esp_err_t readBoundary(httpd_req_t* req) {
  if (httpd_req_get_hdr_value_len(req, "X-CSRF-Token") > 0) authenticate(req);
  return static_cast<const Route*>(req->user_ctx)->handler(req);
}
constexpr uint16_t kMaxHandlers = 40;
constexpr Route kRoutes[] = {
    {"/", HTTP_GET, page, nullptr},
    {"/api/v1/session", HTTP_GET, getSession, nullptr},
    {"/api/v1/status", HTTP_GET, getStatus, nullptr},
    {"/api/v1/schema", HTTP_GET, getSchema, nullptr},
    {"/api/v1/settings", HTTP_GET, getSettings, nullptr},
    {"/api/v1/competitions", HTTP_GET, getCompetitions, nullptr},
    {"/api/v1/wifi", HTTP_GET, getWifi, nullptr},
    {"/api/v1/selection", HTTP_GET, getSelection, nullptr},
    {"/api/v1/teams", HTTP_GET, getTeams, nullptr},
    {"/api/v1/images", HTTP_GET, getImages, nullptr},
    {"/api/v1/images/file", HTTP_GET, getImageFile, nullptr},
    {"/api/v1/debug", HTTP_GET, debug, debugAdmit},
    {"/api/v1/session", HTTP_POST, login, nullptr},
    {"/api/v1/logout", HTTP_POST, logout, nullptr},
    {"/api/v1/settings", HTTP_POST, putSettings, nullptr},
    {"/api/v1/settings", HTTP_PUT, putSettings, nullptr},
    {"/api/v1/competitions", HTTP_POST, putCompetitions, nullptr},
    {"/api/v1/competitions", HTTP_PUT, putCompetitions, nullptr},
    {"/api/v1/export", HTTP_POST, exportSettings, nullptr},
    {"/api/v1/wifi/scan", HTTP_POST, wifiScan, nullptr},
    {"/api/v1/data/select", HTTP_POST, dataSelect, nullptr},
    {"/api/v1/data/refresh", HTTP_POST, dataRefresh, nullptr},
    {"/api/v1/teams", HTTP_POST, postTeam, nullptr},
    {"/api/v1/teams", HTTP_PUT, putTeams, nullptr},
    {"/api/v1/display", HTTP_POST, postDisplay, nullptr},
    {"/api/v1/screenshot", HTTP_POST, postScreenshot, nullptr},
    {"/api/v1/images/delete", HTTP_POST, deleteImage, nullptr},
    {"/api/v1/images/repair", HTTP_POST, repairImages, nullptr},
    {"/api/v1/images/role", HTTP_POST, postImageRole, nullptr},
    {"/api/v1/images/upload", HTTP_POST, uploadImage, nullptr},
    {"/api/v1/ota", HTTP_POST, uploadFirmware, nullptr},
    {"/api/v1/reboot", HTTP_POST, reboot, nullptr},
    {"/api/v1/reset", HTTP_POST, reset, nullptr},
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
    // Reads pass the session boundary; the debug socket authenticates in
    // its handshake.
    const bool read = route.method == HTTP_GET && route.handshake == nullptr;
    uri.handler = read ? readBoundary : route.handler;
    uri.user_ctx = const_cast<Route*>(&route);
    uri.is_websocket = route.handshake != nullptr;
    uri.ws_pre_handshake_cb = route.handshake;
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
