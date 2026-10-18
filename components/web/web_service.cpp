#include "web_service.h"

#include <strings.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>

#include "app_config.h"
#include "command_json.h"
#include "config_json.h"
#include "correlation_id.h"
#include "display_port.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "file_service.h"
#include "football_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health_service.h"
#include "hw_target.h"
#include "image_service.h"
#include "input_policy.h"
#include "json_stream.h"
#include "lwip/sockets.h"
#include "network_service.h"
#include "original_names_channel.h"
#include "ota_service.h"
#include "provider_client.h"
#include "request_budget.h"
#include "request_json.h"
#include "screen_json.h"
#include "session_table.h"
#include "settings_json.h"
#include "settings_store.h"
#include "team_name_catalogue.h"
#include "team_name_json.h"
#include "team_name_service.h"
#include "team_name_store.h"
#include "time_service.h"
#include "ui_controller.h"
#include "web_asset.h"
namespace web {
namespace {
using settings::Reason;
using Serial = SessionTable::Serial;
// Longest owner's short name in bytes (the stored text, UTF-8).
constexpr std::size_t kShortNameBytes = cfg::kTeamShortBytes;

SessionTable sessions;
// Guards sessions and the correlated debug selector across HTTP/debug tasks.
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
enum class DebugSendState : uint8_t { kIdle, kPending, kDetached };
static_assert(sizeof(std::atomic<DebugSendState>) == sizeof(std::atomic<bool>));
std::atomic<DebugSendState> debug_send_state{DebugSendState::kIdle};
std::atomic<std::size_t> debug_length{0};
char debug_record[cfg::kDebugRecordBytes];
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
    if (connection_) connection_->deadline = request_deadline;
    body_read = 0;
    body_broken = false;
  }
  ~Request() {
    if (connection_) connection_->deadline = 0;
    request_deadline = 0;
  }
  // The SDK sends an accepted WebSocket handshake after this callback returns.
  // Keep that deadline until the HTTP-task debug sender observes the upgrade.
  void preserveConnectionDeadline() { connection_ = nullptr; }
  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

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
// Complete one SDK send invocation, including partial socket writes. The SDK's
// HTTP loop otherwise resets its timeout per partial write; its WebSocket path
// even treats a positive partial result as a complete header/payload.
int sendWithin(int fd, const char* bytes, size_t count, int flags,
               int64_t deadline) {
  if (!bytes || count > INT_MAX) return HTTPD_SOCK_ERR_INVALID;
  std::size_t sent = 0;
  while (sent < count) {
    const auto left = deadline - timekeeping::monotonicMs();
    if (left <= 0) return HTTPD_SOCK_ERR_FAIL;
    const auto wait_ms = std::min<int64_t>(cfg::kSocketTimeoutMs, left);
    timeval timeout{static_cast<long>(wait_ms / 1000),
                    static_cast<long>((wait_ms % 1000) * 1000)};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
      return HTTPD_SOCK_ERR_FAIL;
    const int result = send(fd, bytes + sent, count - sent, flags);
    if (result <= 0 || timekeeping::monotonicMs() >= deadline)
      return HTTPD_SOCK_ERR_FAIL;
    sent += static_cast<std::size_t>(result);
  }
  return static_cast<int>(sent);
}
int boundedSend(httpd_handle_t, int fd, const char* bytes, size_t count,
                int flags) {
  const auto* state = connection(fd);
  if (!state) return HTTPD_SOCK_ERR_INVALID;
  // Idle WebSocket frames have no active HTTP request. Their individual
  // header/payload send remains bounded by the established socket window.
  const auto deadline =
      state->deadline ? state->deadline
                      : timekeeping::monotonicMs() + cfg::kSocketTimeoutMs;
  return sendWithin(fd, bytes, count, flags, deadline);
}
esp_err_t openSession(httpd_handle_t handle, int fd) {
  for (auto& item : connections) {
    if (item.fd >= 0) continue;
    item.fd = fd;
    item.deadline = 0;
    auto err = httpd_sess_set_recv_override(handle, fd, boundedReceive);
    if (err == ESP_OK)
      err = httpd_sess_set_send_override(handle, fd, boundedSend);
    if (err != ESP_OK) item.fd = -1;
    return err;
  }
  return ESP_ERR_NO_MEM;
}
bool sendReady(httpd_req_t* req) {
  return connection(httpd_req_to_sockfd(req)) &&
         timekeeping::monotonicMs() < request_deadline;
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
  char sink[cfg::kHttpDrainChunkBytes];
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
inline constexpr Fault kTrial{"409 Conflict", "trial",
                              "OTA trial is read only"};
inline constexpr Fault kBusy{"409 Conflict", "busy", "operation busy"};
inline constexpr Fault kDebugOff{"403 Forbidden", "debug_disabled",
                                 "browser debug is disabled"};
inline constexpr Fault kScreenshotUnavailable{
    "503 Service Unavailable", "screenshot_unavailable",
    "display is unavailable for capture"};
// A rejected start owns no query; the client may retry this explicit refusal.
inline constexpr Fault kSelectionBusy{"409 Conflict", "busy", "selection busy"};
inline constexpr Fault kChanged{"409 Conflict", "changed", "selection changed"};
inline constexpr Fault kSettingsChanged{"409 Conflict", "settings_changed",
                                        "settings changed; reload the draft"};
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
    case ESP_ERR_NO_MEM: return fault::kMemory;
    case files::kStorageFull: return fault::kStorageFull;
    case ESP_ERR_NOT_FOUND: return fault::kMissing;
    case ESP_ERR_TIMEOUT: return fault::kTimeout;
    default: return fault::kInternal;
  }
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
  char buffer_[cfg::kHttpJsonChunkBytes];
  size_t used_ = 0;
  bool failed_ = false;
};
// Answers with {"error", "code"[, "field"]}. The unread request body is
// drained first, so the client receives the answer instead of a reset (D6);
// only an undrainable body ends in a closed connection.
esp_err_t error(httpd_req_t* req, const char* status, const char* code,
                const char* message, const char* field = nullptr,
                const json::ErrorInfo* diagnostic = nullptr) {
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
  // The common encoder streams escaped text through the bounded transport;
  // no private encoder, truncation or unchecked send result remains here.
  Writer transport(req);
  ::json::Writer output(
      {&transport, [](std::string_view bytes, void* context) {
         auto& writer = *static_cast<Writer*>(context);
         return writer.write(reinterpret_cast<const uint8_t*>(bytes.data()),
                             bytes.size()) == bytes.size();
       }});
  const bool encoded =
      output.object() && output.key("error") && output.string(message) &&
      output.key("code") && output.string(code) &&
      (!field || (output.key("field") && output.string(field))) &&
      (!diagnostic || output.field("json_offset", diagnostic->offset)) &&
      output.end() && output.finish();
  if (!encoded || !transport.flush() || !sendReady(req) ||
      httpd_resp_send_chunk(req, nullptr, 0) != ESP_OK)
    return ESP_FAIL;
  return drained ? ESP_OK : ESP_FAIL;
}
esp_err_t error(httpd_req_t* req, const Fault& fault,
                const char* field = nullptr,
                const json::ErrorInfo* diagnostic = nullptr) {
  return error(req, fault.status, fault.code, fault.message, field, diagnostic);
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
// One bounded transport and common JSON encoder for typed object responses.
template <class Encode>
esp_err_t streamJson(httpd_req_t* req, Encode encode) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  Writer transport(req);
  ::json::Writer output(
      {&transport, [](std::string_view bytes, void* context) {
         auto& writer = *static_cast<Writer*>(context);
         return writer.write(reinterpret_cast<const uint8_t*>(bytes.data()),
                             bytes.size()) == bytes.size();
       }});
  return output.object() && encode(output) && output.end() && output.finish() &&
                 transport.flush() && sendReady(req)
             ? httpd_resp_send_chunk(req, nullptr, 0)
             : ESP_FAIL;
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
  char host[cfg::kHttpHostBytes]{}, request_origin[cfg::kHttpOriginBytes]{};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
      host[0] == 0)
    return false;
  if (auto* colon = strchr(host, ':')) {
    if (strcmp(colon, ":80")) return false;
    *colon = 0;
  }
  const auto net = network::status();
  const auto model = settings::webSettings();
  char name[cfg::kHostnameMaxChars + sizeof(".local")]{};
  snprintf(name, sizeof(name), "%.*s.local", model.hostname.length,
           model.hostname.data.data());
  if (strcasecmp(host, net.ip) && strcasecmp(host, net.ap_ip) &&
      strcasecmp(host, name) && !sameName(host, model.hostname.view()))
    return false;
  if (httpd_req_get_hdr_value_len(req, "Origin") == 0) return true;
  if (httpd_req_get_hdr_value_str(req, "Origin", request_origin,
                                  sizeof(request_origin)) != ESP_OK)
    return false;
  char expected[cfg::kHttpOriginBytes];
  snprintf(expected, sizeof(expected), "http://%s", host);
  return strcasecmp(expected, request_origin) == 0;
}
// The live session whose token the request carries (header, or the query of
// the debug socket); the request moves its expiry. 0: none.
Serial authenticate(httpd_req_t* req, bool from_query = false) {
  char supplied[SessionTable::kTokenChars + 1]{};
  if (from_query) {
    char q[cfg::kHttpQueryBytes];
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
bool jsonContentType(const char* value) {
  while (*value == ' ' || *value == '\t') ++value;
  constexpr std::size_t size = sizeof("application/json") - 1;
  if (strncasecmp(value, "application/json", size) != 0) return false;
  value += size;
  while (*value == ' ' || *value == '\t') ++value;
  return *value == 0 || *value == ';';
}
// Checks command transport bounds/type before the strict streaming decoder.
const Fault* checkJsonBody(httpd_req_t* req,
                           std::size_t maximum = cfg::kApiBodyBytes) {
  if (req->content_len == 0) return &fault::kBodyMissing;
  if (req->content_len > maximum) return &fault::kBodyLarge;
  char type[cfg::kHttpContentTypeBytes];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) !=
          ESP_OK ||
      !jsonContentType(type))
    return &fault::kContentType;
  return nullptr;
}
// Origin, optionally the OTA trial, session and JSON body of a call that
// changes something; nullptr when all of it is in order. 'who' receives the
// calling session.
const Fault* admitSession(httpd_req_t* req, Serial* who = nullptr,
                          bool blocked_in_trial = false) {
  if (!origin(req)) return &fault::kOrigin;
  if (blocked_in_trial && ota::trial()) return &fault::kTrial;
  const auto serial = authenticate(req);
  if (!serial) return &fault::kSession;
  if (who) *who = serial;
  return nullptr;
}
// Small transport buffer independent of total body size. The schema reader
// consumes once and cannot publish anything until complete syntax is proven.
class JsonBody {
 public:
  explicit JsonBody(httpd_req_t* req) : req_(req) {}
  ::json::Input input() {
    return {
        this,
        [](void* context) { return static_cast<JsonBody*>(context)->read(); },
        [](json::Error error, std::size_t offset, void* context) {
          static_cast<JsonBody*>(context)->diagnostic_ = {error, offset};
        }};
  }
  bool complete() const {
    return !body_broken && body_read == req_->content_len && at_ == used_;
  }
  const json::ErrorInfo* diagnostic() const {
    return diagnostic_.error == json::Error::kNone ? nullptr : &diagnostic_;
  }

 private:
  json::ErrorInfo diagnostic_;
  httpd_req_t* req_;
  uint8_t buffer_[cfg::kHttpJsonReadBytes];
  std::size_t at_ = 0, used_ = 0;
  int read() {
    if (at_ == used_) {
      if (body_read >= req_->content_len || body_broken) return -1;
      const int n =
          readBody(req_, reinterpret_cast<char*>(buffer_),
                   std::min(sizeof(buffer_), req_->content_len - body_read));
      if (n <= 0) return -1;
      at_ = 0;
      used_ = static_cast<std::size_t>(n);
    }
    return buffer_[at_++];
  }
};
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
[[gnu::noinline]] bool passwordSet() {
  return !settings::webSettings().admin_password.empty();
}
[[gnu::noinline]] bool browserDebug() {
  return settings::webSettings().browser_debug;
}
// No admin password, or the one supplied.
[[gnu::noinline]] bool passwordAccepted(std::string_view supplied) {
  const auto model = settings::webSettings();
  return model.admin_password.empty() ||
         (supplied.size() == model.admin_password.length &&
          equal(supplied.data(), model.admin_password.data.data(),
                supplied.size()));
}
// Admission belongs to each route. All small command bodies share complete
// transport/grammar/error handling, with typed publication only on success.
template <class T, class Decode>
bool readCommand(httpd_req_t* req, T& command, Decode decode,
                 esp_err_t& reply) {
  if (const auto* fault = checkJsonBody(req)) {
    reply = error(req, *fault);
    return false;
  }
  JsonBody body(req);
  settings::Violation why;
  const auto decoded = decode(body.input(), command, why);
  if (decoded == SettingsDecode::kViolation)
    reply = rejected(req, why);
  else if (decoded != SettingsDecode::kOk)
    reply = error(req, body_broken ? fault::kBodyIncomplete : fault::kJson,
                  nullptr, body.diagnostic());
  else if (!body.complete())
    reply = error(req, fault::kBodyIncomplete);
  else
    return true;
  return false;
}
bool readEmptyCommand(httpd_req_t* req, esp_err_t& reply) {
  bool unused = false;
  return readCommand(
      req, unused,
      [](auto input, bool&, auto& why) { return readEmpty(input, why); },
      reply);
}
esp_err_t success(httpd_req_t* req) {
  return streamJson(req, [](::json::Writer& out) {
    return out.key("ok") && out.boolean(true);
  });
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
  const bool valid = origin(req) && authenticate(req) != 0;
  return streamJson(req, [&](::json::Writer& out) {
    return out.key("password_set") && out.boolean(passwordSet()) &&
           out.key("valid") && out.boolean(valid);
  });
}
esp_err_t getConfig(httpd_req_t* req) {
  Request request(req);
  auto config = std::unique_ptr<settings::AppConfig>(new (std::nothrow)
                                                         settings::AppConfig);
  if (!config) return failed(req, ESP_ERR_NO_MEM);
  settings::snapshot(*config);
  return streamJson(
      req, [&](::json::Writer& out) { return configJson(out, *config); });
}
esp_err_t getAdoptedNames(httpd_req_t* req) {
  Request request(req);
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  football::names::AdoptedExport names;
  const auto prepared = names.prepare();
  if (prepared != ESP_OK) return failed(req, prepared);
  const auto reference = settings::namesReference();
  return streamJson(req, [&](::json::Writer& out) {
    return out.key("names_ref") && out.object() && out.key("slot") &&
           out.integer(reference.slot) && out.key("generation") &&
           out.integer(reference.generation) && out.key("length") &&
           out.integer(reference.length) && out.key("crc") &&
           out.integer(reference.crc) && out.end() &&
           out.key("adopted_names") && names.write(out) == ESP_OK;
  });
}
esp_err_t applyConfig(httpd_req_t* req) {
  Request request(req);
  Serial who = 0;
  if (const auto* fault = admitSession(req, &who, true))
    return error(req, *fault);
  if (const auto* fault = checkJsonBody(req, cfg::kConfigBodyMaxBytes))
    return error(req, *fault);
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  auto config = std::unique_ptr<settings::AppConfig>(new (std::nothrow)
                                                         settings::AppConfig);
  if (!config) return failed(req, ESP_ERR_NO_MEM);
  settings::snapshot(*config);
  const auto old_password = config->model.admin_password;
  struct NamesDraft {
    football::names::files::Candidate candidate;
    esp_err_t error = ESP_OK;
  } names;
  JsonBody body(req);
  ApplyRequest application;
  settings::Violation why;
  const auto decoded = readApply(
      body.input(), *config, application, why,
      [](::json::Reader& input, const ::json::Token& token, void* raw,
         settings::Violation& invalid) {
        auto& names = *static_cast<NamesDraft*>(raw);
        names.error = football::names::begin(names.candidate, true);
        if (names.error != ESP_OK) return SettingsDecode::kSyntax;
        switch (football::names::readAdoptedJson(input, token, names.candidate,
                                                 invalid)) {
          case football::names::Decode::kOk: return SettingsDecode::kOk;
          case football::names::Decode::kViolation:
            return SettingsDecode::kViolation;
          case football::names::Decode::kNoMemory:
            names.error = ESP_ERR_NO_MEM;
            break;
          case football::names::Decode::kStorageFull:
            names.error = files::kStorageFull;
            break;
          case football::names::Decode::kIo: names.error = ESP_FAIL; break;
          case football::names::Decode::kSyntax: break;
        }
        return SettingsDecode::kSyntax;
      },
      &names, settings::limits());
  if (names.error != ESP_OK) return failed(req, names.error, "adopted_names");
  if (decoded == SettingsDecode::kViolation) return rejected(req, why);
  if (decoded != SettingsDecode::kOk)
    return error(req, body_broken ? fault::kBodyIncomplete : fault::kJson,
                 nullptr, body.diagnostic());
  if (!body.complete()) return error(req, fault::kBodyIncomplete);
  if (settings::generation() != application.expected_revision)
    return error(req, fault::kSettingsChanged);
  settings::screen_policy::Error policy_error;
  settings::NamesChange names_change;
  if (application.names_changed) {
    const auto prepared =
        football::names::prepareChange(names.candidate, names_change);
    if (prepared != ESP_OK) return failed(req, prepared, "adopted_names");
  }
  const auto err = settings::apply(
      *config, application.expected_revision, &why, &policy_error,
      application.names_changed ? &names_change : nullptr);
  if (settings::origin() == settings::Origin::kOutcomeUnknown)
    return error(req, "503 Service Unavailable", "outcome_unknown",
                 "configuration may be durable; do not repeat Apply; restart "
                 "and read the configuration");
  if (err == ESP_ERR_INVALID_ARG) {
    if (policy_error.reason != settings::screen_policy::Reason::kNone)
      screenPolicyViolation(policy_error, why);
    return rejected(req, why);
  }
  if (err != ESP_OK) return failed(req, err);
  if (!(old_password == config->model.admin_password)) {
    SessionLock lock;
    sessions.closeOthers(who);
  }
  return streamJson(req, [&](::json::Writer& out) {
    return out.key("ok") && out.boolean(true) && configJson(out, *config);
  });
}
settings::screen_policy::Limits screenLimits() {
  const auto& limits = settings::limits();
  return {limits.visible_matches_max, limits.table_window_max};
}
esp_err_t getSchema(httpd_req_t* req) {
  Request request(req);
  return streamJson(req, [](::json::Writer& out) {
    if (!schemaJson(out, settings::limits(),
                    hw::kTarget.wiring.backlight != hw::kNoPin) ||
        !out.key("provider_keys") || !out.array())
      return false;
    for (std::size_t i = 0; i < cfg::kProviderCount; ++i)
      if (!out.string(football::providerName(static_cast<cfg::Provider>(i))))
        return false;
    if (!out.end()) return false;
    return out.key("screen_policy") && out.object() &&
           screenPolicySchemaJson(out, screenLimits()) && out.end() &&
           out.key("width") && out.integer(hw::kDisplay.width) &&
           out.key("height") && out.integer(hw::kDisplay.height) &&
           out.key("round") &&
           out.boolean(hw::kDisplay.shape == hw::DisplayShape::kRound) &&
           out.key("antenna") && out.boolean(hw::kBoard.has_antenna_switch) &&
           out.key("ota_max") && out.integer(ota::capacity()) &&
           out.key("selection_text_max") &&
           out.integer(cfg::kSelectionTextMaxBytes) &&
           out.key("config_version") &&
           out.integer(cfg::kAppConfigFormatVersion) &&
           out.key("max_apply_body") && out.integer(cfg::kConfigBodyMaxBytes) &&
           out.field("operation_deadline_ms", cfg::kOperationDeadlineMs) &&
           out.field("socket_timeout_ms", cfg::kSocketTimeoutMs) &&
           out.field("names_format", cfg::kTeamNamesJsonFormat) &&
           out.field("names_source_version", cfg::kTeamNamesJsonVersion) &&
           out.field("names_wire_version", cfg::kAdoptedNamesJsonVersion) &&
           out.field("names_name_max", cfg::kTeamNameBytes) &&
           out.field("names_key_max", football::names::store::kKeyBytes) &&
           out.key("names_file_bytes") &&
           out.integer(cfg::kTeamNamesFileBytes) && out.key("short_name_max") &&
           out.integer(kShortNameBytes);
  });
}

esp_err_t getStatus(httpd_req_t* req) {
  Request request(req);
  const auto net = network::status();
  time_t utc = 0;
  timekeeping::utcNow(utc);
  football::Status data{};
  football::DatasetSnapshots datasets{};
  auto overview = football::overview(utc, &data, &datasets);
  esp_err_t names_error = ESP_OK;
  if (overview.own.present) {
    static football::names::VisibleCache cache;
    football::names::Targets targets;
    targets.add({overview.own.home_id, overview.own.provider,
                 overview.own.home.data, sizeof(overview.own.home.data)});
    targets.add({overview.own.away_id, overview.own.provider,
                 overview.own.away.data, sizeof(overview.own.away.data)});
    uint32_t revision;
    const auto web = settings::webSettings(&revision);
    names_error =
        football::names::resolveVisible(targets, web.language, revision, cache);
    if (names_error == ESP_ERR_INVALID_STATE)
      return error(req, fault::kSettingsChanged);
  }
  files::Usage usage{};
  files::usage(usage);
  const auto* description = esp_app_get_description();
  return streamJson(req, [&](::json::Writer& out) {
    if (!out.field("version", description->version)) return false;
    const bool names_pending = names_error == ESP_ERR_NOT_FINISHED;
    if (!out.key("names") || !out.object() ||
        !out.field("pending", names_pending) ||
        !out.field("error", names_error) ||
        !out.field("code", names_error == ESP_OK || names_pending
                               ? ""
                               : cfg::kNamesLookupErrorCode) ||
        !out.end())
      return false;
    char firmware_id[2 * sizeof(description->app_elf_sha256) + 1];
    for (std::size_t i = 0; i < sizeof(description->app_elf_sha256); ++i)
      snprintf(firmware_id + 2 * i, 3, "%02x",
               static_cast<unsigned>(description->app_elf_sha256[i]));
    if (!out.field("firmware_id", firmware_id)) return false;
    if (!out.field("target", ota::identity().target)) return false;
    if (!out.field("layout", ota::identity().layout)) return false;
    if (!out.field("ip", net.ip)) return false;
    if (!out.field("ap_ip", net.ap_ip)) return false;
    if (!out.field("network", network::stateName(net.state))) return false;
    if (!out.field("network_reason",
                   network::reasonName(network::offlineReason(net.state))))
      return false;
    if (!out.key("network_init") || !out.object()) return false;
    if (!out.field("ready", net.initialized)) return false;
    if (!out.field("step", network::initStepKey(net.failure_step)))
      return false;
    if (!out.field("error", net.init_error)) return false;
    if (!out.field("error_name", esp_err_to_name(net.init_error))) return false;
    if (!out.field("code", network::initStepCode(net.failure_step)))
      return false;
    if (!out.field("retry_ms", net.retry_at_ms)) return false;
    if (!out.end()) return false;
    const auto selected_image = images::status();
    if (!out.key("image") || !out.object()) return false;
    if (!out.field("active", selected_image.active)) return false;
    if (!out.field("pending", selected_image.pending)) return false;
    if (!out.field("id", selected_image.desired_id)) return false;
    if (!out.field("role", selected_image.role == cfg::kStockCrest
                               ? "crest"
                               : "background"))
      return false;
    if (!out.field("actual", images::fileSourceKey(selected_image.actual)))
      return false;
    if (!out.field("failed", images::fileSourceKey(selected_image.failed)))
      return false;
    if (!out.field("reason", images::failureKey(selected_image.reason)))
      return false;
    if (!out.field("error", selected_image.error)) return false;
    if (!out.field("error_name", esp_err_to_name(selected_image.error)))
      return false;
    if (!out.field("code", images::failureCode(selected_image.reason)))
      return false;
    if (!out.field("revision", selected_image.revision)) return false;
    if (!out.field("attempt", selected_image.attempt)) return false;
    if (!out.field("retry_ms", selected_image.retry_at_ms)) return false;
    if (!out.end()) return false;
    if (!out.field("ap", net.ap)) return false;
    if (!out.field("rssi", net.rssi)) return false;
    if (!out.field("time_valid", timekeeping::status().valid)) return false;
    if (!out.field("uptime_s", timekeeping::monotonicMs() / 1000)) return false;
    if (!out.field("reset_reason", health::resetReason())) return false;
    const auto heap = health::observeHeap();
    if (!out.field("heap_free", heap.point.free_bytes)) return false;
    if (!out.field("heap_largest", heap.point.largest_bytes)) return false;
    if (heap.scope == health::MinimumScope::kUnavailable) {
      if (!out.field("heap_min", nullptr)) return false;
    } else {
      if (!out.field("heap_min", heap.point.minimum_bytes)) return false;
    }
    if (!out.field("heap_min_scope",
                   heap.scope == health::MinimumScope::kLifetime ? "lifetime"
                   : heap.scope == health::MinimumScope::kInterval
                       ? "interval"
                       : "unavailable"))
      return false;
    if (!out.field("heap_min_start_ms", heap.interval_start_ms)) return false;
    const auto app_bytes = ota::imageBytes();
    if (app_bytes != 0) {
      if (!out.field("app_bytes", app_bytes)) return false;
    } else {
      if (!out.field("app_bytes", nullptr)) return false;
    }
    std::array<health::TaskRow, cfg::kHealthMaxTasks> stacks;
    const auto task_count = health::stackMinima(stacks.data(), stacks.size());
    if (!out.key("tasks") || !out.array()) return false;
    for (std::size_t i = 0; i < task_count; ++i) {
      if (!out.object()) return false;
      if (!out.field("name", stacks[i].name)) return false;
      if (!out.field("id", stacks[i].id)) return false;
      if (!out.field("stack_min_free", stacks[i].min_free_bytes)) return false;
      if (!out.field("stack_usable_bytes", stacks[i].usable_bytes))
        return false;
      if (!out.end()) return false;
    }
    if (!out.end()) return false;
    if (!out.field("fs_total", usage.total_bytes)) return false;
    if (!out.field("fs_used", usage.used_bytes)) return false;
    if (!out.field("trial", ota::trial())) return false;
    if (!out.field("settings_source", settings::originName(settings::origin())))
      return false;
    if (!out.field("competitions_source",
                   settings::originName(settings::competitionsOrigin())))
      return false;
    if (!out.field("screen_policy_source",
                   settings::originName(settings::screenPolicyOrigin())))
      return false;
    if (!out.field("football_paused", settings::footballPaused())) return false;
    if (!out.field("provider_started", data.started)) return false;
    if (!out.field("provider_busy", data.busy)) return false;
    if (!out.field("last_update_ms", data.fetched_ms)) return false;
    if (!out.field("data_stale", data.stale)) return false;
    if (!out.field("next_request_ms", data.next_ms)) return false;
    if (!out.field("next_request_reason", football::cadenceName(data.cadence)))
      return false;
    if (!out.key("requests") || !out.array()) return false;
    for (auto count : data.requests)
      if (!out.integer(count)) return false;
    if (!out.end()) return false;
    // Stable screen type identity; sequence order is independently editable.

    const auto shown = ui::status();
    if (!out.field("screen", static_cast<uint8_t>(shown.screen))) return false;
    if (!out.field("screen_reason", ui::reasonName(shown.reason))) return false;
    if (!out.field("screen_starting", shown.starting)) return false;
    if (!out.field("screen_manual", shown.manual)) return false;
    if (!out.field("screen_frame", shown.frame)) return false;
    if (!out.field("screen_phase", shown.phase)) return false;
    if (!out.field("screen_automatic_entries", shown.automatic_entries))
      return false;
    // The club's current or next match and the state of each competition.
    if (!out.field("demo", overview.demo)) return false;
    if (!out.key("own_match") || !out.object()) return false;
    if (!out.field("present", overview.own.present)) return false;
    if (overview.own.present) {
      if (!out.field("competition", overview.own.competition)) return false;
      if (!out.field("phase", football::phaseName(overview.own.phase)))
        return false;
      if (overview.own.kickoff)
        if (!out.field("kickoff", overview.own.kickoff)) return false;
      if (overview.own.home_goals >= 0)
        if (!out.field("home_goals", overview.own.home_goals)) return false;
      if (overview.own.away_goals >= 0)
        if (!out.field("away_goals", overview.own.away_goals)) return false;
      if (overview.own.minute >= 0)
        if (!out.field("minute", overview.own.minute)) return false;
      if (!out.field("home", overview.own.home.data)) return false;
      if (!out.field("away", overview.own.away.data)) return false;
    }
    if (!out.end()) return false;
    // One row per competition in list order, with its id.
    if (!out.key("competitions") || !out.array()) return false;
    for (std::size_t i = 0; i < cfg::kCompetitionMax; ++i) {
      const uint8_t id = overview.routes[i].id;
      if (id == 0) break;
      if (!out.object()) return false;
      if (!out.field("id", id)) return false;
      if (overview.demo && i < overview.demo_bindings.size()) {
        const auto& binding = overview.demo_bindings[i];
        if (!out.field("incarnation", binding.incarnation)) return false;
        if (!out.field("bound", binding.saved)) return false;
      }
      if (!out.field("matches", overview.routes[i].matches)) return false;
      if (!out.field("provider",
                     football::providerName(overview.routes[i].provider)))
        return false;
      constexpr const char* dataset_names[] = {"matches", "table", "details"};
      if (!out.key("datasets") || !out.object()) return false;
      for (std::size_t index = 0; index < datasets[i].size(); ++index) {
        const auto& dataset = datasets[i][index];
        if (!out.key(dataset_names[index]) || !out.object() ||
            !out.field("state", football::datasetStateKey(dataset.state)) ||
            !out.field("last_attempt_ms", dataset.last_attempt_ms) ||
            !out.field("last_success_ms", dataset.last_success_ms) ||
            !out.field("cause", football::causeKey(dataset.cause)) ||
            !out.field("http", dataset.http))
          return false;
        if (dataset.cause != football::Cause::kNone) {
          char code[football::kErrorCodeBytes];
          football::errorCode(code, overview.routes[i].provider, dataset.cause,
                              dataset.http);
          if (!out.field("code", code)) return false;
        }
        if (!out.end()) return false;
      }
      if (!out.end()) return false;
      // Why the competition has no current data (OQ-280): the stable key for
      // the Web UI texts and the code the display shows.
      const auto* state = football::findState(data.competitions, id);
      const auto cause = state ? state->cause : football::Cause::kNone;
      if (!out.field("cause", football::causeKey(cause))) return false;
      if (cause != football::Cause::kNone) {
        char code[football::kErrorCodeBytes];
        football::errorCode(code, state->provider, cause, state->http);
        if (!out.field("code", code)) return false;
        if (!out.field("error_provider",
                       football::providerName(state->provider)))
          return false;
        if (state->http)
          if (!out.field("http", state->http)) return false;
      }
      // What the device could not hold (OQ-310), whatever the cause shown.
      if (state && state->notices) {
        if (!out.key("missing") || !out.array()) return false;
        if (state->notices & football::kNoticeMatches)
          if (!out.string("matches")) return false;
        if (state->notices & football::kNoticeTable)
          if (!out.string("table")) return false;
        if (state->notices & football::kNoticeTeams)
          if (!out.string("teams")) return false;
        if (state->notices & football::kNoticeEvents)
          if (!out.string("events")) return false;
      }
      if (state && state->notices && !out.end()) return false;
      if (state && state->fetched_ms)
        if (!out.field("last_update_ms", state->fetched_ms)) return false;
      if (!out.end()) return false;
    }
    return out.end();
  });
}
esp_err_t getWifi(httpd_req_t* req) {
  Request request(req);
  network::AccessPoint results[cfg::kScanEntries];
  network::ScanStatus scan{};
  const auto count = network::scanResults(results, cfg::kScanEntries, &scan);
  return streamJson(req, [&](::json::Writer& out) {
    if (!out.field("busy", scan.busy) ||
        !out.field("error_code", scan.error_code) || !out.key("networks") ||
        !out.array())
      return false;
    for (unsigned i = 0; i < count; ++i)
      if (!out.object() || !out.key("ssid") ||
          !ssidValueJson(out, results[i].ssid) ||
          !out.field("rssi", results[i].rssi) ||
          !out.field("secure", results[i].secure) || !out.end())
        return false;
    return out.end();
  });
}
esp_err_t getSelection(httpd_req_t* req) {
  Request request(req);
  char parameters[cfg::kHttpQueryBytes], text[17];
  football::SelectionQuery query = 0;
  if (httpd_req_get_url_query_str(req, parameters, sizeof(parameters)) !=
          ESP_OK ||
      httpd_query_key_value(parameters, "query", text, sizeof(text)) !=
          ESP_OK ||
      !parseCorrelationId(text, query))
    return error(req, fault::kRejected, "query");
  football::SelectionStatus state;
  const auto checked = football::selectionStatus(query, state);
  if (checked == ESP_ERR_NOT_FOUND) return error(req, fault::kChanged);
  if (checked != ESP_OK) return failed(req, checked);
  correlationIdText(query, text);
  return streamJson(req, [&](::json::Writer& out) {
    if (!out.field("query", text) || !out.field("busy", state.busy) ||
        !out.field("more", state.more) ||
        !out.field("revision", state.revision) ||
        !out.field("error_code", football::causeKey(state.cause)))
      return false;
    if (state.cause != football::Cause::kNone) {
      char code[football::kErrorCodeBytes];
      football::errorCode(code, state.provider, state.cause, state.http);
      if (!out.field("code", code) ||
          (state.http && !out.field("http", state.http)))
        return false;
    }
    const int64_t left = state.wait_ms - timekeeping::monotonicMs();
    const bool waits = state.busy && state.wait_ms && left > 0;
    if (!out.field("wait",
                   football::cadenceName(waits ? state.wait
                                               : football::Cadence::kNone)) ||
        !out.field("wait_s", waits ? (left + 999) / 1000 : 0) ||
        !out.key("items") || !out.array())
      return false;
    const auto items = [&](auto& item) {
      for (unsigned i = 0; i < state.count; ++i) {
        // Replacement terminates this stream; never attach a second body.
        if (!football::selectionAt(query, i, state.revision, item) ||
            !out.object() || !out.field("id", item.identity()) ||
            !out.field("name", item.name.data))
          return false;
        if constexpr (std::is_same_v<std::remove_reference_t<decltype(item)>,
                                     football::CompetitionSelection>)
          if (!out.field("season", item.season.data)) return false;
        if (!out.end()) return false;
      }
      return true;
    };
    if (!state.busy) {
      if (state.teams) {
        football::TeamSelection item;
        if (!items(item)) return false;
      } else {
        football::CompetitionSelection item;
        if (!items(item)) return false;
      }
    }
    return out.end();
  });
}
// ?competition=N: bounded volatile provider labels and IDs, without names I/O.
esp_err_t getTeams(httpd_req_t* req) {
  Request request(req);
  char query[cfg::kHttpQueryBytes], value[cfg::kHttpQueryValueBytes];
  uint32_t id = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "competition", value, sizeof(value)) !=
          ESP_OK)
    return invalid(req, Reason::kRequired, "competition required",
                   "competition");
  if (!parseUnsigned(value, UINT8_MAX, id) || id == 0)
    return invalid(req, Reason::kRange, "competition rejected", "competition");
  uint32_t revision;
  const auto count = football::teamCount(static_cast<uint8_t>(id), revision);
  return streamJson(req, [&](::json::Writer& out) {
    if (!out.field("competition", id) || !out.key("items") || !out.array())
      return false;
    football::TeamInfo team;
    for (uint16_t i = 0; i < count; ++i) {
      if (!football::teamAt(static_cast<uint8_t>(id), i, revision, team) ||
          !out.object() ||
          !out.field("provider", static_cast<unsigned>(team.provider)) ||
          !out.field("id", team.id) || !out.field("name", team.name.data) ||
          !out.field("short_name", team.short_name.data) || !out.end())
        return false;
    }
    return out.end();
  });
}
// Observe a closed peer without consuming pipelined bytes or changing socket
// flags. The no-output wait of an original query must also notice cancellation.
bool peerConnected(httpd_req_t* req) {
  char byte;
  const int count =
      recv(httpd_req_to_sockfd(req), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
  return count > 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                                     errno == EINTR));
}

// Explicit protected query. The worker reuses provider admission; HTTP owns no
// heavy token while waiting. A partial provider/document/send failure closes
// the stream, so the browser can never adopt an incomplete successful list.
esp_err_t queryOriginalNames(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req)) return error(req, *fault);
  esp_err_t reply;
  if (!readEmptyCommand(req, reply)) return reply;
  char query_text[cfg::kHttpQueryBytes], value[cfg::kHttpQueryValueBytes];
  uint32_t competition = 0;
  if (httpd_req_get_url_query_str(req, query_text, sizeof(query_text)) !=
          ESP_OK ||
      httpd_query_key_value(query_text, "competition", value, sizeof(value)) !=
          ESP_OK)
    return invalid(req, Reason::kRequired, "competition required",
                   "competition");
  if (!parseUnsigned(value, UINT8_MAX, competition) || !competition)
    return invalid(req, Reason::kRange, "competition rejected", "competition");
  uint32_t query = 0;
  const auto err =
      football::beginOriginalNames(static_cast<uint8_t>(competition), query);
  if (err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_INVALID_STATE)
    return error(req, fault::kBusy);
  if (err != ESP_OK) return failed(req, err, "competition");
  struct QueryScope {
    uint32_t id;
    ~QueryScope() { football::originals::release(id); }
  } scope{query};
  bool started = false;
  char bytes[cfg::kOriginalNamesBufferBytes];
  while (timekeeping::monotonicMs() < request_deadline) {
    if (!peerConnected(req)) {
      football::originals::cancel(query);
      return ESP_FAIL;
    }
    const auto part = football::originals::read(query, bytes, sizeof(bytes),
                                                cfg::kProviderWaitSliceMs);
    if (part.bytes) {
      if (!started) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        started = true;
      }
      if (!sendReady(req) ||
          httpd_resp_send_chunk(req, bytes, part.bytes) != ESP_OK)
        return ESP_FAIL;
    }
    using State = football::originals::State;
    if (part.state == State::kComplete) {
      if (!started) return failed(req, ESP_ERR_INVALID_RESPONSE);
      return sendReady(req) ? httpd_resp_send_chunk(req, nullptr, 0) : ESP_FAIL;
    }
    if (part.state == State::kFailed || part.state == State::kCancelled ||
        part.state == State::kStale)
      return started ? ESP_FAIL : failed(req, part.error);
  }
  football::originals::cancel(query);
  return ESP_FAIL;
}

// The running app owns the immutable source. Stream complete original values
// with the existing bounded writer; reading it never adopts any club labels.
esp_err_t getTeamNamesPreset(httpd_req_t* req) {
  Request request(req);
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Team-Names-Source",
                     football::names::catalogue::sourceSha256());
  Writer writer(req);
  const auto write = [](void* context, const char* bytes, std::size_t size) {
    return static_cast<Writer*>(context)->write(
               reinterpret_cast<const uint8_t*>(bytes), size) == size;
  };
  if (!football::names::catalogue::streamJson(write, &writer) ||
      !writer.flush() || !sendReady(req))
    return ESP_FAIL;
  return httpd_resp_send_chunk(req, nullptr, 0);
}
// API image-role identity, in cfg::StockImage order.
constexpr const char* kStockNames[] = {"crest", "background"};
static_assert(std::size(kStockNames) == cfg::kStockImageCount);
cfg::StockImage roleNamed(std::string_view name) {
  for (std::size_t i = 0; i < std::size(kStockNames); ++i)
    if (name == kStockNames[i]) return static_cast<cfg::StockImage>(i);
  return cfg::kStockImageCount;
}
esp_err_t getImages(httpd_req_t* req) {
  Request request(req);
  images::Inventory inventory;
  const auto checked = images::inventory(inventory);
  if (checked != ESP_OK) return failed(req, checked);
  return streamJson(req, [&](::json::Writer& out) {
    if (!out.key("items") || !out.array()) return false;
    for (uint8_t id = 1; id <= cfg::kImageLibraryMax; ++id) {
      const auto& entry = inventory.library[id - 1];
      if (!entry.present) continue;
      if (!out.object() || !out.field("id", id) ||
          !out.field("bytes", entry.bytes) ||
          !out.field("revision", entry.revision) || !out.end())
        return false;
    }
    if (!out.end() || !out.key("roles") || !out.array()) return false;
    for (uint8_t i = 0; i < cfg::kStockImageCount; ++i) {
      const auto& role = inventory.replacements[i];
      if (!out.object() || !out.field("name", kStockNames[i]) ||
          !out.field("replaced", role.present) ||
          !out.field("bytes", role.bytes) ||
          !out.field("revision", role.revision) || !out.end())
        return false;
    }
    return out.end() && out.field("count", inventory.count) &&
           out.field("used", inventory.bytes) &&
           out.field("quota", cfg::kImageTotalBytes) &&
           out.field("max", cfg::kImageLibraryMax);
  });
}
// ?id=N or ?stock=crest|background, optionally &r=<revision>. ?display=1
// reads the actual selected file, including factory fallback while a damaged
// replacement remains. This dynamic selection is never cached as immutable.
esp_err_t getImageFile(httpd_req_t* req) {
  Request request(req);
  char query[cfg::kHttpQueryBytes], value[cfg::kHttpQueryValueBytes];
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
  char path[files::kPathBufferBytes];
  const auto found =
      selected ? (images::filePath(images::status(), path, sizeof(path))
                      ? ESP_OK
                      : ESP_ERR_NOT_FOUND)
               : images::filePath(id, stock, path, sizeof(path));
  if (found != ESP_OK) return failed(req, found, "id");
  FILE* file = fopen(path, "rb");
  if (file == nullptr)
    return failed(req, errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL, "id");
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control",
                     !selected && httpd_query_key_value(query, "r", value,
                                                        sizeof(value)) == ESP_OK
                         ? "public, max-age=31536000, immutable"
                         : "no-cache");
  uint8_t buffer[cfg::kHttpFileChunkBytes];
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
  if (fclose(file) != 0) err = ESP_FAIL;
  if (err == ESP_OK && sendReady(req))
    return httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_FAIL;
}

// ---- Session ---------------------------------------------------------------
esp_err_t login(httpd_req_t* req) {
  Request request(req);
  if (!origin(req)) return error(req, fault::kOrigin);
  LoginRequest command;
  esp_err_t reply;
  if (!readCommand(req, command, readLogin, reply)) return reply;
  if (!passwordAccepted(command.password.view()))
    return error(req, fault::kPassword, "password");
  uint8_t bytes[SessionTable::kTokenChars / 2];
  esp_fill_random(bytes, sizeof(bytes));
  char fresh[SessionTable::kTokenChars + 1];
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    snprintf(fresh + i * 2, 3, "%02x", bytes[i]);
  {
    SessionLock lock;
    sessions.open(fresh, timekeeping::monotonicMs());
  }
  return streamJson(req, [&](::json::Writer& out) {
    return out.key("token") && out.string(fresh) && out.key("expires_ms") &&
           out.integer(cfg::kSessionLifetimeMs);
  });
}
esp_err_t logout(httpd_req_t* req) {
  Request request(req);
  Serial who = 0;
  if (const auto* fault = admitSession(req, &who)) return error(req, *fault);
  esp_err_t reply;
  if (!readEmptyCommand(req, reply)) return reply;
  {
    SessionLock lock;
    sessions.close(who);
  }
  return success(req);
}

// ---- Settings --------------------------------------------------------------
esp_err_t exportSettings(httpd_req_t* req) {
  Request request(req);
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  if (const auto* f = admitSession(req)) return error(req, *f);
  if (const auto* f = checkJsonBody(req)) return error(req, *f);
  JsonBody body(req);
  bool secrets = false;
  settings::Violation why;
  const auto decoded = exportOptions(body.input(), secrets, &why);
  if (decoded == SettingsDecode::kViolation) return rejected(req, why);
  if (decoded != SettingsDecode::kOk)
    return error(req, body_broken ? fault::kBodyIncomplete : fault::kJson,
                 nullptr, body.diagnostic());
  if (!body.complete()) return error(req, fault::kBodyIncomplete);
  // Export one coherent application snapshot. Adopted-name publication shares
  // this heavy-operation guard, so the referenced immutable file stays stable.
  auto config = std::unique_ptr<settings::AppConfig>(new (std::nothrow)
                                                         settings::AppConfig);
  if (!config) return failed(req, ESP_ERR_NO_MEM);
  settings::snapshot(*config);
  football::names::AdoptedExport names;
  const auto prepared = names.prepare();
  if (prepared != ESP_OK) return failed(req, prepared);
  return streamJson(req, [&](::json::Writer& out) {
    return out.key("version") && out.integer(cfg::kAppConfigFormatVersion) &&
           out.key("desired") && out.object() && out.key("settings") &&
           out.object() && settingsJson(out, config->model, secrets) &&
           out.end() && competitionsJson(out, config->competitions) &&
           out.key("screens") && out.object() &&
           screenPolicyJson(out, config->screens) && out.end() && out.end() &&
           out.key("adopted_names") && names.write(out) == ESP_OK;
  });
}
esp_err_t reboot(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req)) return error(req, *fault);
  esp_err_t reply;
  if (!readEmptyCommand(req, reply)) return reply;
  ota::reboot();
  return success(req);
}
struct ResetProgress {
  bool settings = false, counters = false, adopted_names = false;
  bool images = false, format = false;
  ::images::DeleteAllResult image_files;
};
esp_err_t resetFailed(httpd_req_t* req, esp_err_t err, const char* stage,
                      const ResetProgress& progress) {
  ESP_LOGW("web", "factory reset incomplete at %s: %s", stage,
           esp_err_to_name(err));
  httpd_resp_set_status(req, "500 Internal Server Error");
  return streamJson(req, [&](::json::Writer& out) {
    return out.key("code") && out.string("reset_partial") && out.key("error") &&
           out.string(
               "Factory reset incomplete; data may have changed. Retry reset; "
               "select full format if storage is unreadable.") &&
           out.key("partial") && out.boolean(true) && out.key("stage") &&
           out.string(stage) && out.key("settings_reset") &&
           out.boolean(progress.settings) && out.key("counters_erased") &&
           out.boolean(progress.counters) && out.key("adopted_names_deleted") &&
           out.boolean(progress.adopted_names) && out.key("images_deleted") &&
           out.boolean(progress.images) && out.key("image_files_deleted") &&
           out.integer(progress.image_files.deleted) &&
           out.key("image_files_already_missing") &&
           out.integer(progress.image_files.already_missing) &&
           out.key("storage_formatted") && out.boolean(progress.format);
  });
}
esp_err_t reset(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req, nullptr, true))
    return error(req, *fault);
  ResetRequest command;
  esp_err_t reply;
  if (!readCommand(req, command, readReset, reply)) return reply;
  const bool delete_images = command.images,
             format_storage = command.format_storage;
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  const auto fenced = football::beginReset();
  if (fenced != ESP_OK) return failed(req, fenced);
  struct ResetFence {
    bool release = true;
    ~ResetFence() {
      if (release) {
        football::names::invalidate();
        football::endReset();
      }
    }
  } fence;
  ResetProgress progress;
  // Reset authority first. The heavy token prevents the worker from writing
  // another counter during the following checked deletion/format.
  auto err = settings::reset();
  if (err != ESP_OK) return resetFailed(req, err, "settings", progress);
  progress.settings = true;
  err = football::budget::erase();
  if (err != ESP_OK) return resetFailed(req, err, "counters", progress);
  progress.counters = true;
  if (format_storage) {
    // This is the explicitly authorized deletion even if storage cannot mount.
    err = images::format();
    if (err != ESP_OK) return resetFailed(req, err, "format", progress);
    progress.format = progress.adopted_names = progress.images = true;
  } else {
    err = football::names::erase();
    if (err != ESP_OK) return resetFailed(req, err, "adopted_names", progress);
    progress.adopted_names = true;
    if (delete_images) {
      err = images::removeAll(&progress.image_files);
      if (err != ESP_OK) return resetFailed(req, err, "images", progress);
      progress.images = true;
    }
  }
  // Keep publication fenced until the scheduled reboot after a complete reset.
  fence.release = false;
  ota::reboot();
  return success(req);
}
// ---- Network, provider data, display --------------------------------------
esp_err_t wifiScan(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req)) return error(req, *fault);
  esp_err_t reply;
  if (!readEmptyCommand(req, reply)) return reply;
  const auto err = network::requestScan();
  if (err == ESP_ERR_NOT_FINISHED) return error(req, fault::kBusy);
  return err == ESP_OK ? success(req) : failed(req, err);
}
esp_err_t dataRefresh(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req)) return error(req, *fault);
  esp_err_t reply;
  if (!readEmptyCommand(req, reply)) return reply;
  football::refresh();
  return success(req);
}
esp_err_t dataSelect(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req)) return error(req, *fault);
  football::SelectionRequest command;
  esp_err_t reply;
  if (!readCommand(req, command, readSelection, reply)) return reply;
  football::SelectionQuery query = 0;
  const auto err = football::select(command, query);
  if (err == ESP_ERR_NOT_FINISHED) return error(req, fault::kSelectionBusy);
  if (err != ESP_OK) return failed(req, err);
  char text[17];
  correlationIdText(query, text);
  return streamJson(req, [&](::json::Writer& out) {
    return out.field("ok", true) && out.field("query", text);
  });
}
esp_err_t cancelSelection(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req)) return error(req, *fault);
  football::SelectionQuery query = 0;
  esp_err_t reply;
  if (!readCommand(req, query, readSelectionQuery, reply)) return reply;
  const auto err = football::cancelSelection(query);
  if (err == ESP_ERR_NOT_FOUND) return error(req, fault::kChanged);
  return err == ESP_OK ? success(req) : failed(req, err);
}
// Quick actions of the Web UI: the same events the buttons post.
esp_err_t postDisplay(httpd_req_t* req) {
  Request request(req);
  if (const auto* f = admitSession(req)) return error(req, *f);
  DisplayCommand command;
  esp_err_t reply;
  if (!readCommand(req, command, readDisplayCommand, reply)) return reply;
  if (command.config_revision != settings::generation())
    return error(req, fault::kSettingsChanged);
  if (command.action == ui::Action::kAutomaticNext &&
      !football::demoStatus().enabled)
    return error(req, "409 Conflict", "demo_disabled",
                 "enable and save demo mode before advancing automation");
  const auto err = ui::submit(command.action, command.config_revision);
  if (err == ESP_ERR_TIMEOUT) return error(req, fault::kBusy);
  if (err == ESP_ERR_INVALID_STATE &&
      command.config_revision != settings::generation())
    return error(req, fault::kSettingsChanged);
  if (err != ESP_OK) return failed(req, err);
  return success(req);
}

esp_err_t getDemo(httpd_req_t* req) {
  Request request(req);
  const auto status = football::demoStatus();
  return streamJson(req, [&](::json::Writer& out) {
    if (!out.field("enabled", status.enabled) ||
        !out.field("pending", status.pending) ||
        !out.field("scenario", static_cast<uint8_t>(status.scenario)) ||
        !out.field("step", status.step) ||
        !out.field("revision", status.revision) ||
        !out.field("utc", status.clock.utc) || !out.key("bindings") ||
        !out.array())
      return false;
    for (uint8_t i = 0; i < status.bindings.size(); ++i) {
      const auto& binding = status.bindings[i];
      if (!out.object() || !out.field("dataset", i) ||
          !out.field("competition", binding.competition) ||
          !out.field("incarnation", binding.incarnation) ||
          !out.field("saved", binding.saved) || !out.end())
        return false;
    }
    if (!out.end() || !out.key("scenarios") || !out.array()) return false;
    for (uint8_t i = 0; i < football::demo::kScenarioCount; ++i) {
      const auto scenario = static_cast<football::demo::Scenario>(i);
      if (!out.object() || !out.field("id", i) ||
          !out.field("key", football::demo::key(scenario)) ||
          !out.field("title", football::demo::title(scenario)) ||
          !out.key("steps") || !out.array())
        return false;
      for (uint8_t step = 0; step < football::demo::steps(scenario); ++step)
        if (!out.string(football::demo::stepTitle(scenario, step)))
          return false;
      if (!out.end() || !out.end()) return false;
    }
    return out.end();
  });
}
esp_err_t postDemo(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req, nullptr, true))
    return error(req, *fault);
  DemoRequest command;
  esp_err_t reply;
  if (!readCommand(req, command, readDemo, reply)) return reply;
  const auto err = football::demoScenario(
      static_cast<football::demo::Scenario>(command.scenario), command.step);
  if (err == ESP_ERR_TIMEOUT) return error(req, fault::kBusy);
  if (err == ESP_ERR_INVALID_STATE)
    return error(req, "409 Conflict", "demo_disabled",
                 "enable and save demo mode before selecting a scenario");
  if (err != ESP_OK) return failed(req, err);
  return streamJson(req, [](::json::Writer& out) {
    return out.key("accepted") && out.boolean(true);
  });
}

// ---- Images ----------------------------------------------------------------
esp_err_t deleteImage(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req, nullptr, true))
    return error(req, *fault);
  uint8_t id = 0;
  esp_err_t reply;
  if (!readCommand(req, id, readImageId, reply)) return reply;
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  images::DeleteResult result;
  const auto err = images::remove(id, &result);
  const auto& fault = faultFor(err);
  if (err != ESP_OK) httpd_resp_set_status(req, fault.status);
  return streamJson(req, [&](::json::Writer& out) {
    if (!(out.key("references_cleared") &&
          out.boolean(result.references_cleared) && out.key("settings_saved") &&
          out.boolean(result.settings_saved) && out.key("settings_revision") &&
          out.integer(result.settings_revision) && out.key("file_deleted") &&
          out.boolean(result.file_deleted) && out.key("already_missing") &&
          out.boolean(result.already_missing) && out.key("outcome_unknown") &&
          out.boolean(result.outcome_unknown)))
      return false;
    return err == ESP_OK ? out.key("ok") && out.boolean(true)
                         : out.key("error") && out.string(fault.message) &&
                               out.key("code") && out.string(fault.code);
  });
}
esp_err_t postImageRole(httpd_req_t* req) {
  Request request(req);
  if (const auto* fault = admitSession(req, nullptr, true))
    return error(req, *fault);
  ImageRoleRequest command;
  esp_err_t reply;
  if (!readCommand(req, command, readImageRole, reply)) return reply;
  network::HeavyGuard guard;
  if (!guard) return error(req, fault::kBusy);
  const auto role = static_cast<cfg::StockImage>(command.role);
  const auto err = command.copy ? images::copyToRole(command.id, role)
                                : images::resetRole(role);
  return err == ESP_OK
             ? success(req)
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
  uint8_t buffer[cfg::kHttpFileChunkBytes];
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
  return streamJson(req, [&](::json::Writer& out) {
    if (!out.field("ok", true)) return false;
    if (firmware) return true;
    return role != cfg::kStockImageCount
               ? out.field("role", kStockNames[role])
               : out.field("id", images::uploadedId());
  });
}
esp_err_t uploadFirmware(httpd_req_t* req) { return upload(req, true); }
esp_err_t uploadImage(httpd_req_t* req) { return upload(req, false); }

// ---- Debug stream ----------------------------------------------------------
bool clearDebug(int fd, Serial serial = 0) {
  SessionLock lock;
  if (debug_fd.load() != fd || (serial && debug_serial.load() != serial))
    return false;
  // One queued frame owns the buffer until its HTTP-task callback completes.
  // Invalidate that connection even when a reconnect reuses both fd and token.
  auto pending = DebugSendState::kPending;
  debug_send_state.compare_exchange_strong(pending, DebugSendState::kDetached);
  debug_serial.store(0);
  debug_fd.store(-1);
  return true;
}
void failDebug(int fd, Serial serial) {
  if (clearDebug(fd, serial)) httpd_sess_trigger_close(server, fd);
}
// Runs on the HTTP task, which owns socket close/reuse and handshakes. Check
// the original authenticated serial before any write, rather than after an
// SDK async transfer has already used a potentially recycled descriptor.
void sendDebug(void* context) {
  const auto serial = static_cast<Serial>(reinterpret_cast<uintptr_t>(context));
  int fd = -1;
  bool alive = false;
  {
    SessionLock lock;
    if (debug_send_state.load() == DebugSendState::kPending && serial &&
        debug_serial.load() == serial) {
      fd = debug_fd.load();
      alive = sessions.alive(serial, timekeeping::monotonicMs());
    }
  }
  if (fd >= 0) {
    if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
      // A public HTTP connection is not authority to close a recycled socket.
      clearDebug(fd, serial);
    } else if (!alive || !browserDebug()) {
      failDebug(fd, serial);
    } else {
      // The upgrade/previous incoming-frame deadline is finished. The shared
      // send override now bounds each idle frame header/payload by one socket
      // window, while completing every positive partial write.
      if (auto* state = connection(fd)) state->deadline = 0;
      httpd_ws_frame_t frame{};
      frame.type = HTTPD_WS_TYPE_TEXT;
      frame.payload = reinterpret_cast<uint8_t*>(debug_record);
      frame.len = debug_length.load(std::memory_order_acquire);
      if (httpd_ws_send_frame_async(server, fd, &frame) != ESP_OK)
        failDebug(fd, serial);
    }
  }
  // No SDK transfer owns the borrowed buffer after this HTTP-task callback.
  debug_send_state.store(DebugSendState::kIdle);
}
void closeSession(httpd_handle_t, int fd) {
  clearDebug(fd);
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
    SessionLock lock;
    int expected = -1;
    if (!debug_fd.compare_exchange_strong(expected, httpd_req_to_sockfd(req)))
      refusal = &fault::kBusy;
    else
      debug_serial.store(serial);
  }
  if (refusal) {
    error(req, *refusal);
    return ESP_FAIL;
  }
  request.preserveConnectionDeadline();
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
    clearDebug(httpd_req_to_sockfd(req));
  }
  uint8_t discard[128];
  frame.payload = discard;
  return httpd_ws_recv_frame(req, &frame, sizeof(discard));
}
void streamDebug(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(cfg::kDebugStreamPeriodMs));
    int fd;
    Serial serial;
    const auto now = timekeeping::monotonicMs();
    {
      SessionLock lock;
      fd = debug_fd.load();
      serial = debug_serial.load();
      if (fd < 0 || serial == 0) continue;
      auto idle = DebugSendState::kIdle;
      if (!debug_send_state.compare_exchange_strong(idle,
                                                    DebugSendState::kPending))
        continue;
    }
    auto net = network::status();
    auto data = football::status();
    // The first failing competition's code and how many fail.
    char code[football::kErrorCodeBytes] = "";
    unsigned errors = 0;
    for (const auto& state : data.competitions)
      if (state.active && state.cause != football::Cause::kNone &&
          errors++ == 0)
        football::errorCode(code, state.provider, state.cause, state.http);
    std::size_t length = 0;
    ::json::Writer out({&length, [](std::string_view bytes, void* context) {
                          auto& used = *static_cast<std::size_t*>(context);
                          if (bytes.size() > sizeof(debug_record) - used)
                            return false;
                          std::memcpy(debug_record + used, bytes.data(),
                                      bytes.size());
                          used += bytes.size();
                          return true;
                        }});
    bool encoded =
        out.object() && out.field("ms", now) &&
        out.field("net", network::stateName(net.state)) &&
        out.field("heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                  MALLOC_CAP_8BIT)) &&
        out.field("rev", data.revision) && out.field("stale", data.stale) &&
        out.field("busy", data.busy) && out.field("error", code) &&
        out.field("errors", errors) && out.key("requests") && out.array();
    for (const auto count : data.requests)
      encoded = encoded && out.integer(count);
    encoded = encoded && out.end() && out.end() && out.finish();
    if (!encoded) {
      debug_send_state.store(DebugSendState::kIdle);
      continue;
    }
    debug_length.store(length, std::memory_order_release);
    const auto err = httpd_queue_work(
        server, sendDebug,
        reinterpret_cast<void*>(static_cast<uintptr_t>(serial)));
    if (err != ESP_OK) {
      debug_send_state.store(DebugSendState::kIdle);
      // Nothing was queued and no borrowed buffer remains in use. Preserve
      // the selector for a later retry; only the HTTP task may close the
      // correlated connection after checking whether its descriptor changed.
      ESP_LOGW("web", "debug enqueue failed: %s", esp_err_to_name(err));
    }
  }
}

// ---- Screenshot ------------------------------------------------------------
// Used only on the screenshot socket, which is closed after its response.
// The SDK repeats its send function on partial writes: each invocation must
// share the absolute capture deadline, not start another socket timeout.
int screenshotSend(httpd_handle_t, int fd, const char* bytes, size_t count,
                   int flags) {
  return sendWithin(fd, bytes, count, flags, request_deadline);
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
  if (const auto* fault = admitSession(req)) return error(req, *fault);
  esp_err_t reply;
  if (!readEmptyCommand(req, reply)) return reply;
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
constexpr Route kRoutes[] = {
    {"/", HTTP_GET, page, nullptr},
    {"/api/v1/session", HTTP_GET, getSession, nullptr},
    {"/api/v1/config", HTTP_GET, getConfig, nullptr},
    {"/api/v1/config", HTTP_PUT, applyConfig, nullptr},
    {"/api/v1/team-names", HTTP_GET, getAdoptedNames, nullptr},
    {"/api/v1/status", HTTP_GET, getStatus, nullptr},
    {"/api/v1/schema", HTTP_GET, getSchema, nullptr},
    {"/api/v1/demo", HTTP_GET, getDemo, nullptr},
    {"/api/v1/wifi", HTTP_GET, getWifi, nullptr},
    {"/api/v1/selection", HTTP_GET, getSelection, nullptr},
    {"/api/v1/teams", HTTP_GET, getTeams, nullptr},
    {"/api/v1/team-names/preset", HTTP_GET, getTeamNamesPreset, nullptr},
    {"/api/v1/team-names/originals", HTTP_POST, queryOriginalNames, nullptr},
    {"/api/v1/images", HTTP_GET, getImages, nullptr},
    {"/api/v1/images/file", HTTP_GET, getImageFile, nullptr},
    {"/api/v1/debug", HTTP_GET, debug, debugAdmit},
    {"/api/v1/session", HTTP_POST, login, nullptr},
    {"/api/v1/logout", HTTP_POST, logout, nullptr},
    {"/api/v1/export", HTTP_POST, exportSettings, nullptr},
    {"/api/v1/wifi/scan", HTTP_POST, wifiScan, nullptr},
    {"/api/v1/data/select", HTTP_POST, dataSelect, nullptr},
    {"/api/v1/data/select/cancel", HTTP_POST, cancelSelection, nullptr},
    {"/api/v1/data/refresh", HTTP_POST, dataRefresh, nullptr},
    {"/api/v1/display", HTTP_POST, postDisplay, nullptr},
    {"/api/v1/demo", HTTP_POST, postDemo, nullptr},
    {"/api/v1/screenshot", HTTP_POST, postScreenshot, nullptr},
    {"/api/v1/images/delete", HTTP_POST, deleteImage, nullptr},
    {"/api/v1/images/role", HTTP_POST, postImageRole, nullptr},
    {"/api/v1/images/upload", HTTP_POST, uploadImage, nullptr},
    {"/api/v1/ota", HTTP_POST, uploadFirmware, nullptr},
    {"/api/v1/reboot", HTTP_POST, reboot, nullptr},
    {"/api/v1/reset", HTTP_POST, reset, nullptr},
};
static_assert(std::size(kRoutes) <= UINT16_MAX);
// Owned by the application loop; handlers/debug never mutate startup state.
bool server_ready = false, debug_started = false;
int64_t startup_retry_at = 0;
}  // namespace
esp_err_t init() {
  // Main-loop-owned lifecycle. Keep a failed-to-stop handle until cleanup is
  // acknowledged; a retry must never create a second server over that owner.
  if (server_ready) return ESP_OK;
  startup_retry_at = timekeeping::monotonicMs() + cfg::kHttpInitRetryMs;
  network::setHttpReady(false);
  const auto fail = [&](esp_err_t error) {
    ESP_LOGE("web", "HTTP startup: %s; retry in %lu s", esp_err_to_name(error),
             static_cast<unsigned long>(cfg::kHttpInitRetryMs / 1000));
    return error;
  };
  if (server != nullptr) {
    const auto err = httpd_stop(server);
    if (err != ESP_OK) return fail(err);
    server = nullptr;
  }
  network::HeavyGuard guard;
  if (!guard) return ESP_ERR_NOT_FINISHED;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = cfg::kHttpStackBytes;
  config.max_open_sockets = cfg::kHttpSockets;
  config.max_uri_handlers = std::size(kRoutes);
  config.recv_wait_timeout = cfg::kHttpSocketTimeoutS;
  config.send_wait_timeout = cfg::kHttpSocketTimeoutS;
  config.lru_purge_enable = true;
  config.open_fn = openSession;
  config.close_fn = closeSession;
  auto err = httpd_start(&server, &config);
  if (err != ESP_OK) return fail(err);
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
    err = httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED,
                                     [](httpd_req_t* req, httpd_err_code_t) {
                                       Request request(req);
                                       return error(req, fault::kMethod);
                                     });
  if (err != ESP_OK) {
    const auto stopped = httpd_stop(server);
    if (stopped == ESP_OK) server = nullptr;
    return fail(stopped == ESP_OK ? err : stopped);
  }
  if (!debug_started) {
    if (!xTaskCreateStatic(streamDebug, "web_debug", cfg::kDebugTaskStackBytes,
                           nullptr, cfg::kDebugTaskPriority, debug_stack,
                           &debug_tcb)) {
      const auto stopped = httpd_stop(server);
      if (stopped == ESP_OK) server = nullptr;
      return fail(stopped == ESP_OK ? ESP_ERR_NO_MEM : stopped);
    }
    debug_started = true;
  }
  server_ready = true;
  startup_retry_at = 0;
  network::setHttpReady(true);
  return ESP_OK;
}
void poll() {
  if (healthy()) return;
  const auto now = timekeeping::monotonicMs();
  if (now < startup_retry_at) return;
  init();
}
bool healthy() { return server_ready; }
}  // namespace web
