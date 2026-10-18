#!/usr/bin/env python3
"""Replay actual Web boundaries plus the installed SDK HTTP/WebSocket send loops.

No network or device access. SDK socket/queue calls are controlled; production
request/deadline, debug ownership, media-type and image completion code is real.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def block(source, signature):
    start = source.index(signature)
    depth = 0
    for at in range(source.index("{", start), len(source)):
        depth += (source[at] == "{") - (source[at] == "}")
        if depth == 0:
            return source[start:at + 1] + "\n"
    raise RuntimeError(signature)


PREAMBLE = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <sys/socket.h>
#include <vector>
#include "app_config.h"
#include "file_names.h"
#include "json_stream.h"
#include "session_table.h"
#include "settings_model.h"
using esp_err_t = int;
using httpd_handle_t = void*;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 1,
              ESP_ERR_NOT_FOUND = 2, ESP_ERR_INVALID_ARG = 3, ESP_ERR_INVALID_SIZE = 4, HTTPD_SOCK_ERR_INVALID = -2,
              HTTPD_SOCK_ERR_FAIL = -3, HTTPD_SOCK_ERR_TIMEOUT = -4;
constexpr int HTTPD_WS_CLIENT_HTTP = 1, HTTPD_WS_CLIENT_WEBSOCKET = 2;
using httpd_ws_type_t = int;
constexpr int HTTPD_WS_TYPE_TEXT = 1, HTTPD_WS_TYPE_CLOSE = 8;
constexpr int HTTPD_WS_FIN_BIT = 0x80, HTTPD_WS_CONTINUE = 0,
              HTTPD_WS_MASK_BIT = 0x80;
using Send = int(*)(httpd_handle_t, int, const char*, size_t, int);
using Receive = int(*)(httpd_handle_t, int, char*, size_t, int);
struct sock_db { httpd_handle_t handle = reinterpret_cast<void*>(1);
  int fd = -1, kind = HTTPD_WS_CLIENT_HTTP; Send send_fn = nullptr;
  Receive receive_fn = nullptr; };
struct httpd_req_aux { sock_db* sd; };
struct httpd_req_t { httpd_req_aux* aux; int fd; size_t content_len = 2;
  std::string query = "id=1&r=1", content_type = "application/json"; };
struct StaticTask_t {};
using StackType_t = uint32_t;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(x) ((void)(x))
std::function<void()> after_unlock;
void testExitCritical(int*) {
  auto hook = std::move(after_unlock); after_unlock = {};
  if (hook) hook();
}
#define taskEXIT_CRITICAL(x) testExitCritical(x)
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGD(...)
#define ESP_LOGW(...)
constexpr int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
using web::SessionTable;
using Serial = SessionTable::Serial;
using settings::Reason;
int64_t clock_ms = 100, step_ms = 1, socket_wait_ms = 1000;
size_t send_limit = SIZE_MAX;
bool send_zero = false, send_error = false, fail_receive_install = false,
     fail_send_install = false, fail_queue = false, debug_enabled = true;
std::map<int, sock_db> sockets;
std::map<int, httpd_req_aux> auxiliaries;
std::vector<int> closed;
std::string sent;
unsigned send_calls = 0, terminators = 0, errors = 0, ticks = 0;
struct Stop {};
namespace timekeeping { int64_t monotonicMs() { return clock_ms; } }
const char* esp_err_to_name(int) { return "controlled"; }
void vTaskDelay(unsigned) { if (++ticks > 1) throw Stop{}; }
int httpd_req_to_sockfd(httpd_req_t* req) { return req->fd; }
int testSetsockopt(int, int, int option, const void* value, socklen_t) {
  if (option == SO_SNDTIMEO) {
    const auto& timeout = *static_cast<const timeval*>(value);
    socket_wait_ms = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
    assert(socket_wait_ms > 0 && socket_wait_ms <= cfg::kSocketTimeoutMs);
  }
  return 0;
}
int testSend(int, const char* bytes, size_t count, int) {
  ++send_calls;
  clock_ms += std::min(step_ms, socket_wait_ms);
  if (send_zero) return 0;
  if (send_error) return -1;
  const auto size = std::min(count, send_limit);
  sent.append(bytes, size);
  return static_cast<int>(size);
}
int testReceive(int, char*, size_t, int) { errno = EAGAIN; return -1; }
int testClose(int) { return 0; }
int httpd_sess_set_recv_override(void*, int fd, Receive callback) {
  if (fail_receive_install) return ESP_FAIL;
  sockets[fd].receive_fn = callback; return ESP_OK;
}
int httpd_sess_set_send_override(void*, int fd, Send callback) {
  if (fail_send_install) return ESP_FAIL;
  sockets[fd].send_fn = callback; return ESP_OK;
}
sock_db* httpd_sess_get(void*, int fd) {
  const auto found = sockets.find(fd);
  return found == sockets.end() ? nullptr : &found->second;
}
int httpd_ws_get_fd_info(void*, int fd) {
  const auto* socket = httpd_sess_get(nullptr, fd);
  return socket ? socket->kind : 0;
}
int httpd_sess_trigger_close(void*, int fd) { closed.push_back(fd); return ESP_OK; }
using Work = void(*)(void*);
struct Queued { Work work; void* argument; };
std::vector<Queued> queue;
int httpd_queue_work(void* handle, Work work, void* argument) {
  if (!handle || fail_queue) return ESP_FAIL;
  queue.push_back({work, argument}); return ESP_OK;
}
namespace network {
struct Status { int state = 1; };
Status status() { return {}; }
const char* stateName(int) { return "connected"; }
}
namespace football {
constexpr unsigned kErrorCodeBytes = 16;
enum class Cause { kNone };
struct Competition { bool active = false; Cause cause = Cause::kNone;
  int provider = 0, http = 0; };
struct Status { std::array<Competition, 1> competitions{}; unsigned revision = 1;
  bool stale = false, busy = false; std::array<unsigned, 4> requests{}; };
Status status() { return {}; }
void errorCode(char*, int, Cause, int) {}
}
unsigned heap_caps_get_free_size(int) { return 1234; }
bool browserDebug() { return debug_enabled; }
struct Fault {};
namespace fault { const Fault kOrigin, kDebugOff, kSession, kBusy, kMissing,
  kBodyMissing, kBodyLarge, kContentType; }
int error(httpd_req_t*, const Fault&, const char* = nullptr) { ++errors; return ESP_FAIL; }
int invalid(httpd_req_t*, Reason, const char*, const char*) { ++errors; return ESP_FAIL; }
int service_error=ESP_OK;
int failed(httpd_req_t*, int error, const char* =nullptr) { service_error=error; ++errors; return ESP_FAIL; }
bool origin(httpd_req_t*) { return true; }
Serial authenticated = 0;
Serial authenticate(httpd_req_t*, bool = false) { return authenticated; }
int httpd_req_get_hdr_value_str(httpd_req_t* req, const char*, char* out, size_t capacity) {
  if (req->content_type.size() >= capacity) return ESP_FAIL;
  std::strcpy(out, req->content_type.c_str()); return ESP_OK;
}
int httpd_req_get_url_query_str(httpd_req_t* req, char* out, size_t capacity) {
  if (req->query.size() >= capacity) return ESP_FAIL;
  std::strcpy(out, req->query.c_str()); return ESP_OK;
}
int httpd_query_key_value(const char* query, const char* key, char* out, size_t capacity) {
  const std::string text = std::string("&") + query, prefix = std::string("&") + key + '=';
  const auto at = text.find(prefix);
  if (at == std::string::npos) return ESP_ERR_NOT_FOUND;
  const auto start = at + prefix.size(), end = text.find('&', start);
  const auto value = text.substr(start, end - start);
  if (value.size() >= capacity) return ESP_FAIL;
  std::strcpy(out, value.c_str()); return ESP_OK;
}
int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
namespace images {
std::string file;
struct Status {};
Status status() { return {}; }
int filePath(uint8_t, cfg::StockImage, char* out, size_t capacity) {
  if (file.size() >= capacity) return ESP_ERR_INVALID_SIZE;
  std::strcpy(out, file.c_str()); return ESP_OK;
}
bool filePath(Status, char* out, size_t capacity) { return filePath(1, cfg::kStockCrest, out, capacity)==ESP_OK; }
}
bool fail_file_close = false;
int file_open_error=0;
FILE* testFopen(const char* path,const char* mode) {
  if (file_open_error) { errno=file_open_error; return nullptr; }
  return std::fopen(path,mode);
}
int testFclose(FILE* file) { const auto result = std::fclose(file); return fail_file_close ? EOF : result; }
#define setsockopt testSetsockopt
#define send testSend
#define recv testReceive
#define close testClose
#define fopen testFopen
#define fclose testFclose
'''

MAIN = r'''
void reset() {
  clock_ms = 100; step_ms = 1; socket_wait_ms = 1000; send_limit = SIZE_MAX;
  send_zero = send_error = fail_receive_install = fail_send_install = fail_queue = false;
  debug_enabled = true; fail_file_close = false; file_open_error=0; service_error=ESP_OK;
  sockets.clear(); auxiliaries.clear(); queue.clear(); closed.clear(); sent.clear();
  after_unlock = {};
  send_calls = terminators = errors = ticks = 0;
  sessions = SessionTable{}; debug_fd = -1; debug_serial = 0;
  debug_send_state = DebugSendState::kIdle; debug_length = 0;
  request_deadline = 0; body_read = 0; body_broken = false;
  server = reinterpret_cast<void*>(1);
  for (auto& state : connections) state = {};
}
httpd_req_t request(int fd) {
  auto& socket = sockets[fd]; socket = {}; socket.fd = fd;
  auxiliaries[fd] = {&socket};
  assert(openSession(server, fd) == ESP_OK);
  return {&auxiliaries[fd], fd};
}
void produce() { ticks = 0; try { streamDebug(nullptr); } catch (const Stop&) {} }
void runQueued() {
  assert(queue.size() == 1);
  const auto item = queue.front(); queue.clear(); item.work(item.argument);
}
Serial startDebug(httpd_req_t& req, const char* token) {
  authenticated = sessions.open(token, clock_ms);
  assert(debugAdmit(&req) == ESP_OK);
  assert(connection(req.fd)->deadline == clock_ms + cfg::kOperationDeadlineMs);
  sockets[req.fd].kind = HTTPD_WS_CLIENT_WEBSOCKET;
  return authenticated;
}
constexpr const char* first_token = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* second_token = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
void transport() {
  reset(); auto req = request(42);
  { Request scope(&req); send_limit = 1; step_ms = 900; char bytes[512]{};
    assert(httpd_send_all(&req, bytes, sizeof(bytes)) != ESP_OK);
    assert(clock_ms <= cfg::kOperationDeadlineMs + 100 && send_calls < 40);
  }
  assert(request_deadline == 0 && connection(42)->deadline == 0);
  reset(); req = request(42);
  { Request scope(&req); send_limit = 7; const std::string bytes(1024, 'z');
    assert(httpd_send_all(&req, bytes.data(), bytes.size()) == ESP_OK);
    assert(sent == bytes);
  }
  reset(); req = request(42);
  { Request scope(&req); send_zero = true;
    assert(httpd_send_all(&req, "abc", 3) != ESP_OK && send_calls == 1);
  }
  reset(); req = request(42);
  { Request scope(&req); request_deadline = clock_ms + cfg::kScreenshotDeadlineMs;
    send_limit = 1; step_ms = 900; char bytes[64]{};
    assert(screenshotSend(server, 42, bytes, sizeof(bytes), 0) < 0);
    assert(clock_ms <= cfg::kScreenshotDeadlineMs + 100);
  }
  reset(); fail_send_install = true; sockets[42].fd = 42;
  assert(openSession(server, 42) == ESP_FAIL && connection(42) == nullptr);
  fail_send_install = false; req = request(42);
  { Request scope(&req); assert(connection(42)->deadline == request_deadline); }
  std::cout << "HTTP: positive partial/zero/deadline/pipeline/open rollback and screenshot cap PASS\n";
}
void debugCompletion() {
  reset(); auto req = request(42); startDebug(req, first_token);
  produce(); assert(queue.size() == 1);
  const auto bytes = std::string(debug_record, debug_length.load());
  produce(); assert(queue.size() == 1 && bytes == std::string(debug_record, debug_length.load()));
  send_limit = 3; runQueued();
  assert(debug_send_state == DebugSendState::kIdle && closed.empty());
  assert(sent.size() == bytes.size() + (bytes.size() <= 125 ? 2 : 4));
  assert(sent.substr(sent.size() - bytes.size()) == bytes);
  // A failed enqueue/shutdown never pins the buffer or clears a new selector.
  fail_queue = true; produce(); assert(queue.empty() && debug_send_state == DebugSendState::kIdle);
  assert(debug_fd == 42); fail_queue = false;
  produce(); runQueued(); assert(debug_fd == 42);
  server = nullptr; produce(); assert(queue.empty() && debug_send_state == DebugSendState::kIdle);
  closeSession(nullptr, 42); assert(debug_fd == -1);
  std::cout << "debug: actual SDK partial header/payload, pinned buffer and enqueue/shutdown release PASS\n";
}
void debugReuse() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    reset(); auto old = request(42); const auto serial = startDebug(old, first_token);
    produce(); closeSession(server, 42);
    auto next = request(42); Serial next_serial = 0;
    if (mode) {
      if (mode == 1) {
        authenticated = serial; assert(debugAdmit(&next) == ESP_OK); next_serial = serial;
        sockets[42].kind = HTTPD_WS_CLIENT_WEBSOCKET;
      } else next_serial = startDebug(next, second_token);
    }
    send_error = true; runQueued(); // Old failure must never touch this new socket.
    assert(sent.empty() && closed.empty());
    assert(debug_fd == (mode ? 42 : -1) && debug_serial == next_serial);
    assert(debug_send_state == DebugSendState::kIdle);
  }
  reset(); auto req = request(42); const auto serial = startDebug(req, first_token);
  produce(); sockets[42].kind = HTTPD_WS_CLIENT_HTTP;
  runQueued(); assert(sent.empty() && closed.empty() && debug_fd == -1);
  debug_fd = 43; debug_serial = serial; debug_send_state = DebugSendState::kPending;
  sendDebug(nullptr); assert(debug_fd == 43 && debug_serial == serial && closed.empty());
  assert(debug_send_state == DebugSendState::kIdle);
  std::cout << "debug: public HTTP/same-token/different-token fd reuse, stale/zero work PASS\n";
}
void debugRejection() {
  for (unsigned mode = 0; mode < 4; ++mode) {
    reset(); auto req = request(42); const auto serial = startDebug(req, first_token);
    produce();
    if (mode == 0) sessions.close(serial);
    if (mode == 1) clock_ms += cfg::kSessionLifetimeMs;
    if (mode == 2) debug_enabled = false;
    if (mode == 3) send_zero = true;
    runQueued(); assert(debug_fd == -1 && debug_serial == 0);
    assert(debug_send_state == DebugSendState::kIdle && closed == std::vector<int>{42});
    if (mode < 3) assert(sent.empty());
  }
  std::cout << "debug: logout/expiry/disabled/failed send closes exact owner and releases frame PASS\n";
}
void debugClaimInterleaving() {
  reset(); auto req = request(42); const auto serial = startDebug(req, first_token);
  after_unlock = [&] {
    // HTTP closes/reopens immediately after the producer's snapshot lock.
    // Claiming outside that lock used to miss detachment for an identical
    // fd+token pair; no synthetic producer state mutation is needed here.
    closeSession(server, 42);
    auto next = request(42); authenticated = serial;
    assert(debugAdmit(&next) == ESP_OK);
    sockets[42].kind = HTTPD_WS_CLIENT_WEBSOCKET;
  };
  produce(); assert(queue.size() == 1);
  runQueued();
  assert(sent.empty() && closed.empty() && debug_fd == 42 && debug_serial == serial);
  assert(debug_send_state == DebugSendState::kIdle);
  std::cout << "debug: close/reopen at snapshot/claim release invalidates same-fd+token frame PASS\n";
}
void mediaAndFile(const char* path) {
  reset(); auto req = request(42);
  for (const char* type : {"application/json", "Application/JSON", " application/json\t",
       "application/json; charset=utf-8", "\tapplication/json \t; charset=utf-8"}) {
    req.content_type = type; assert(checkJsonBody(&req) == nullptr);
  }
  for (const char* type : {"application/jsongarbage", "application/json+xml", "application/jsonx; utf-8",
       "application/json garbage", "text/plain", ""}) {
    req.content_type = type; assert(checkJsonBody(&req) == &fault::kContentType);
  }
  images::file = path; auto* file = std::fopen(path, "wb"); assert(file);
  assert(std::fwrite("jpeg", 1, 4, file) == 4); assert(std::fclose(file) == 0);
  fail_file_close = true;
  assert(getImageFile(&req) == ESP_FAIL && terminators == 0 && errors == 0);
  assert(sent == "jpeg");
  sent.clear(); fail_file_close = false;
  assert(getImageFile(&req) == ESP_OK && terminators == 1 && errors == 0);
  for (const auto error : {EIO, ENOENT}) {
    sent.clear(); terminators=errors=0; file_open_error=error;
    assert(getImageFile(&req)==ESP_FAIL&&terminators==0&&sent.empty()&&errors==1);
    assert(service_error==(error==ENOENT?ESP_ERR_NOT_FOUND:ESP_FAIL));
  }
  std::cout << "media/file: strict token+parameters/OWS, fclose has no success terminator, fopen I/O differs from absent file PASS\n";
}
int main(int argc, char** argv) {
  assert(argc == 2); transport(); debugCompletion(); debugReuse(); debugRejection(); debugClaimInterleaving(); mediaAndFile(argv[1]);
}
'''


def main():
    web = (ROOT / "components/web/web_service.cpp").read_text()
    sdk = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    sdk /= "packages/framework-espidf/components/esp_http_server"
    txrx = (sdk / "src/httpd_txrx.c").read_text()
    ws = (sdk / "src/httpd_ws.c").read_text()
    sdk_header = (sdk / "include/esp_http_server.h").read_text()
    start = sdk_header.index("typedef struct httpd_ws_frame {")
    end = sdk_header.index("} httpd_ws_frame_t;", start) + len("} httpd_ws_frame_t;")
    declarations = sdk_header[start:end]
    production = web[web.index("SessionTable sessions;"):web.index("// Takes up to 'count'")]
    production += block(web, "int screenshotSend(")
    production += block(web, "bool jsonContentType(")
    production += block(web, "const Fault* checkJsonBody(")
    for name in ("bool clearDebug(", "void failDebug(", "void sendDebug(",
                 "void closeSession(", "esp_err_t debugAdmit(", "void streamDebug("):
        production += block(web, name)
    helpers_at = web.index("// API image-role identity")
    production += web[helpers_at:web.index("esp_err_t getImages(", helpers_at)]
    production += block(web, "bool parseUnsigned(")
    production += block(web, "esp_err_t getImageFile(")
    loops = block(txrx, "static esp_err_t httpd_send_all(")
    loops += block(ws, "esp_err_t httpd_ws_send_frame_async(")
    response = r'''
int httpd_resp_send_chunk(httpd_req_t* req, const char* bytes, size_t size) {
  if (!size) { ++terminators; return httpd_send_all(req, "0\r\n\r\n", 5); }
  return httpd_send_all(req, bytes, size);
}
'''
    # Match the production path buffer; macOS's default temp root is much
    # longer than the device's /fs mount prefix.
    with tempfile.TemporaryDirectory(prefix="wt-", dir="/tmp") as folder:
        work = Path(folder)
        source = work / "main.cpp"
        # SDK loops precede the production callers; declarations come from the
        # installed SDK, while all test syscall behavior stays in PREAMBLE.
        source.write_text(PREAMBLE + declarations + loops + response + production
                          + "\n#undef fclose\n#undef fopen\n#undef close\n" + MAIN)
        executable = work / "replay"
        subprocess.run([
            os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-g",
            "-Wno-c++11-narrowing", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-I", str(ROOT / "include"),
            "-I", str(ROOT / "components/json/include"),
            "-I", str(ROOT / "components/files/include"),
            "-I", str(ROOT / "components/settings/include"),
            "-I", str(ROOT / "components/web/include"),
            str(ROOT / "components/json/json_stream.cpp"), str(source),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable), str(work / "image.jpg")], check=True)


if __name__ == "__main__":
    main()
