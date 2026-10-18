// Main/Web function bodies are included unchanged from current production;
// the complete OTA translation unit is included with host SDK adapters.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>

#include "app_config.h"
#include "event_bus.h"
#include "hw_target.h"
#include "ota_service.h"
#include "runtime_readiness_sdk.h"

#define section(name) section("__DATA,__const")
#include "components/system/ota_service.cpp"
#undef section

namespace probe {
const char* scenario = "success";
int64_t ms = 0;
int valid = 0, rollback = 0, network_polls = 0, watchdog_polls = 0;
int starts = 0, stops = 0, routes = 0, errors = 0, debug_attempts = 0;
int64_t previous_start_at = 0;
int debug_workers = 0, live_servers = 0;
bool advertised = false;
struct Finished {};
bool is(const char* name) { return std::strcmp(scenario, name) == 0; }
void require(bool pass, const char* why) {
  if (!pass) {
    std::fprintf(stderr, "FAIL %s: %s\n", scenario, why);
    std::exit(1);
  }
}
bool httpFault() { return std::strncmp(scenario, "http_", 5) == 0; }
}  // namespace probe

int64_t esp_timer_get_time() { return probe::ms * 1000; }
void esp_restart() { throw probe::Finished{}; }
const esp_app_desc_t* esp_app_get_description() {
  static const esp_app_desc_t app{};
  return &app;
}
const esp_partition_t* esp_ota_get_running_partition() {
  static const esp_partition_t p{0x10000, 1835008};
  return &p;
}
const esp_partition_t* esp_ota_get_next_update_partition(
    const esp_partition_t*) {
  return esp_ota_get_running_partition();
}
esp_err_t esp_image_get_metadata(const esp_partition_pos_t*,
                                 esp_image_metadata_t* out) {
  out->image_len = 1000;
  return ESP_OK;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t*,
                                      esp_ota_img_states_t* out) {
  *out = ESP_OTA_IMG_PENDING_VERIFY;
  return ESP_OK;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() {
  ++probe::rollback;
  return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
  ++probe::valid;
  return ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t*, std::size_t,
                        esp_ota_handle_t*) {
  return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, std::size_t) {
  return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t) { return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) { return ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t) { return ESP_OK; }

using httpd_handle_t = void*;
struct httpd_req_t {
  const char* uri;
};
using httpd_method_t = int;
inline constexpr int HTTP_GET = 0, HTTP_POST = 1, HTTP_PUT = 2;
inline constexpr int HTTPD_404_NOT_FOUND = 404,
                     HTTPD_405_METHOD_NOT_ALLOWED = 405;
using httpd_err_code_t = int;
struct httpd_config_t {
  uint32_t stack_size = 0;
  unsigned max_open_sockets = 0, max_uri_handlers = 0, recv_wait_timeout = 0,
           send_wait_timeout = 0;
  bool lru_purge_enable = false;
  esp_err_t (*open_fn)(httpd_handle_t, int) = nullptr;
  void (*close_fn)(httpd_handle_t, int) = nullptr;
};
#define HTTPD_DEFAULT_CONFIG() \
  httpd_config_t {}
struct httpd_uri_t {
  const char* uri;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t*);
  void* user_ctx;
  bool is_websocket;
  esp_err_t (*ws_pre_handshake_cb)(httpd_req_t*);
};
esp_err_t httpd_start(httpd_handle_t* server, const httpd_config_t*) {
  if (probe::starts)
    probe::require(
        probe::ms - probe::previous_start_at >= cfg::kHttpInitRetryMs,
        "failed HTTP startup waits the full retry interval");
  probe::previous_start_at = probe::ms;
  ++probe::starts;
  probe::require(probe::live_servers == 0,
                 "no second server over retained owner");
  if (probe::is("web") || (probe::is("http_start") && probe::starts == 1))
    return ESP_FAIL;
  probe::live_servers = 1;
  probe::routes = probe::errors = 0;
  *server = reinterpret_cast<void*>(1);
  return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t) {
  ++probe::stops;
  if (probe::is("http_stop") && probe::stops == 1) return ESP_FAIL;
  probe::require(probe::live_servers == 1, "stop an owned live server");
  probe::live_servers = 0;
  return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*) {
  if ((probe::is("http_route") || probe::is("http_stop")) && probe::starts == 1)
    return ESP_FAIL;
  ++probe::routes;
  return ESP_OK;
}
esp_err_t httpd_register_err_handler(httpd_handle_t, int code,
                                     esp_err_t (*)(httpd_req_t*,
                                                   httpd_err_code_t)) {
  if (probe::starts == 1 && ((probe::is("http_404") && code == 404) ||
                             (probe::is("http_405") && code == 405)))
    return ESP_FAIL;
  ++probe::errors;
  return ESP_OK;
}
using StaticTask_t = int;
using StackType_t = uint32_t;
void* xTaskCreateStatic(void (*)(void*), const char*, uint32_t, void*, int,
                        StackType_t*, StaticTask_t*) {
  ++probe::debug_attempts;
  if (probe::is("http_debug") && probe::debug_attempts == 1) return nullptr;
  ++probe::debug_workers;
  probe::require(probe::debug_workers == 1,
                 "exactly one successful static debug worker");
  return reinterpret_cast<void*>(1);
}

namespace events {
esp_err_t init() { return ESP_OK; }
esp_err_t subscribe(Event, Callback, void*) { return ESP_OK; }
esp_err_t post(Event) { return ESP_OK; }
void runDeviceTest() {}
}  // namespace events
namespace settings {
struct Limits {
  uint8_t visible_matches_max, table_window_max, inputs;
};
void init(const Limits&) {}
struct DiagnosticSettings {
  bool enabled;
  uint16_t interval_s;
};
DiagnosticSettings diagnosticSettings() { return {false, 10}; }
const cfg::TimeZone& clockZone() { return cfg::kTimeZones[0]; }
void runDeviceTest() {}
}  // namespace settings
namespace files {
enum class State { kUnavailable, kMounted };
void init() {}
void runDeviceTest() {}
State state() {
  return probe::is("files") ? State::kUnavailable : State::kMounted;
}
}  // namespace files
namespace timekeeping {
int64_t monotonicMs() { return probe::ms; }
void init(const cfg::TimeZone&) {}
void runDeviceTest() {}
void configureZone(const cfg::TimeZone&) {}
void poll() {}
}  // namespace timekeeping
namespace display {
esp_err_t init(const hw::DisplayProfile&, const hw::DisplayWiring&) {
  return probe::is("display") ? ESP_FAIL : ESP_OK;
}
void logMemory() {}
}  // namespace display
namespace health {
void init(bool, uint16_t) {}
void runDeviceTest() {}
void configureConsole(bool, uint16_t) {}
void poll() { ++probe::watchdog_polls; }
esp_err_t startLoopWatchdog() {
  return probe::is("watchdog") ? ESP_FAIL : ESP_OK;
}
}  // namespace health
namespace network {
esp_err_t init() { return probe::is("network") ? ESP_FAIL : ESP_OK; }
void poll() { ++probe::network_polls; }
void applySettings() {}
bool healthy() { return !probe::is("network"); }
class HeavyGuard {
 public:
  explicit operator bool() const { return true; }
};
void setHttpReady(bool ready) {
  probe::advertised = ready;
  if (ready)
    probe::require(
        probe::live_servers == 1 && probe::errors == 2 &&
            probe::debug_workers == 1,
        "advertisement waits for successful handlers/debug/listener");
}
}  // namespace network
namespace football {
esp_err_t init() { return ESP_OK; }
}  // namespace football
namespace ui {
esp_err_t init(const hw::DisplayProfile&, std::size_t, bool) { return ESP_OK; }
bool showStartScreen(const hw::DisplayProfile&, bool) { return true; }
void poll() {}
bool healthy() { return !probe::is("ui"); }
}  // namespace ui
namespace images {
bool maintain() { return true; }
}  // namespace images

namespace web {
namespace {
httpd_handle_t server = nullptr;
bool server_ready = false, debug_started = false;
int64_t startup_retry_at = 0;
StaticTask_t debug_tcb;
StackType_t debug_stack[cfg::kDebugTaskStackBytes / sizeof(StackType_t)];
struct Route {
  const char* path;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t*);
  esp_err_t (*handshake)(httpd_req_t*);
};
struct Request {
  explicit Request(httpd_req_t*) {}
};
namespace fault {
constexpr int kUnknownRoute = 1, kMethod = 2;
}
esp_err_t error(httpd_req_t*, int) { return ESP_OK; }
esp_err_t readBoundary(httpd_req_t*) { return ESP_OK; }
esp_err_t openSession(httpd_handle_t, int) { return ESP_OK; }
void closeSession(httpd_handle_t, int) {}
void streamDebug(void*) {}
}  // namespace
#include "runtime_web.inc"
}  // namespace web

void logBootInfo() {}
void logHardwareProfile() {}
void logHeap(const char*) {}
void showBootScreen() {}
using InputStates = std::array<bool, hw::kMaxInputs>;
void initInputs(InputStates&) {}
void logInputChanges(InputStates&) {}
std::atomic<bool> settings_changed{false};
void vTaskDelay(uint32_t ticks) {
  if (probe::valid && (!probe::httpFault() || web::healthy()))
    throw probe::Finished{};
  probe::ms += ticks;
  if (probe::ms > cfg::kOtaLocalHealthMs + cfg::kHttpInitRetryMs + 2000)
    throw std::runtime_error("runtime recovery deadline exceeded");
}
#define pdMS_TO_TICKS(value) (value)
#include "runtime_main.inc"

void checkOtaHeaders() {
  static_assert(sizeof(ota::Identity) == 92);
  static_assert(offsetof(ota::Identity, settings_format) == 88);
  std::array<uint8_t, 1024> bytes{};
  esp_image_header_t image{};
  image.magic = ESP_IMAGE_HEADER_MAGIC;
  image.chip_id = CONFIG_IDF_FIRMWARE_CHIP_ID;
  esp_app_desc_t app = *esp_app_get_description();
  app.magic_word = ESP_APP_DESC_MAGIC_WORD;
  auto identity = ota::identity();
  auto check = [&](esp_err_t expected, std::size_t prefix = 1024) {
    std::memcpy(bytes.data(), &image, sizeof(image));
    std::memcpy(bytes.data() + 32, &app, sizeof(app));
    std::memcpy(bytes.data() + 288, &identity, sizeof(identity));
    probe::require(ota::begin(bytes.data(), prefix, bytes.size()) == expected,
                   "actual OTA checks exact firmware identity offsets");
    ota::abort();
  };
  probe::require(identity.version == cfg::kFirmwareIdentityVersion &&
                     identity.settings_format == cfg::kAppConfigFormatVersion,
                 "running OTA descriptor identifies aggregate format");
  check(ESP_OK);
  ++identity.settings_format;
  check(ESP_OK);
  identity.settings_format = 0;
  check(ESP_ERR_INVALID_RESPONSE);
  identity = ota::identity();
  identity.version = 1;
  identity.settings_format = 11;
  check(ESP_ERR_INVALID_RESPONSE);
  identity = ota::identity();
  identity.target[0] = 'X';
  check(ESP_ERR_INVALID_RESPONSE);
  identity = ota::identity();
  identity.layout[0] = 'X';
  check(ESP_ERR_INVALID_RESPONSE);
  identity = ota::identity();
  app.project_name[0] = 'X';
  check(ESP_ERR_INVALID_RESPONSE);
  app.project_name[0] = 0;
  image.chip_id = 0;
  check(ESP_ERR_INVALID_RESPONSE);
  image.chip_id = CONFIG_IDF_FIRMWARE_CHIP_ID;
  check(ESP_ERR_INVALID_SIZE, 379);
  std::puts(
      "PASS OTA real-layout headers: current/newer aggregate, old "
      "aggregate/descriptor, target/layout/project/chip, truncation");
}

int main(int argc, char** argv) {
  if (argc > 1) probe::scenario = argv[1];
  if (probe::is("ota_headers")) {
    checkOtaHeaders();
    return 0;
  }
  bool watchdog_failed = false;
  try {
    app_main();
  } catch (const probe::Finished&) {
  } catch (const WatchdogFailure&) {
    watchdog_failed = true;
  }
  if (probe::is("watchdog")) {
    probe::require(watchdog_failed && probe::valid == 0,
                   "missing local watchdog cannot confirm OTA");
  } else if (probe::is("display") || probe::is("ui")) {
    probe::require(probe::rollback == 1 && probe::valid == 0,
                   "local display/UI failure rolls back");
  } else {
    probe::require(probe::valid == 1 && probe::rollback == 0,
                   "offline/service failure does not veto local OTA health");
    probe::require(probe::network_polls > 0 && probe::watchdog_polls > 0,
                   "actual loop keeps polling network/watchdog");
    if (probe::httpFault()) {
      probe::require(
          web::healthy() && probe::starts == 2 && probe::debug_workers == 1,
          "actual loop retries failed listener/handler/debug setup");
      probe::require(probe::ms >= cfg::kHttpInitRetryMs,
                     "startup failure respects retry deadline");
      const auto starts = probe::starts, workers = probe::debug_attempts;
      web::init();
      web::poll();
      probe::require(
          starts == probe::starts && workers == probe::debug_attempts,
          "completed HTTP startup is idempotent");
    }
  }
  std::printf("PASS %s valid=%d rollback=%d HTTPstarts=%d debugWorkers=%d\n",
              probe::scenario, probe::valid, probe::rollback, probe::starts,
              probe::debug_workers);
}
