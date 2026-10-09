// P1.8 integration budget probe. See budget_probe.h.
//
// Not product code: limits, timeouts and the page layout below are
// probe-local and do not decide OQ-36 or the UI design.

#include "budget_probe.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <sys/stat.h>

#include "ArduinoJson.h"
#include "display_port.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "hw_target.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "safe_area.h"

#include "app_config.h"

// include/secrets.h is optional and git-ignored. Only the presence of values
// is logged, never the values themselves.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef SECRET_WIFI_SSID_1
#define SECRET_WIFI_SSID_1 ""
#endif
#ifndef SECRET_WIFI_PASSWORD_1
#define SECRET_WIFI_PASSWORD_1 ""
#endif
#ifndef SECRET_DEFAULT_COMPETITION
#define SECRET_DEFAULT_COMPETITION ""
#endif
#ifndef SECRET_DEFAULT_TEAM_ID
#define SECRET_DEFAULT_TEAM_ID ""
#endif
#ifndef SECRET_TIMEZONE
#define SECRET_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

namespace probe {
namespace {

constexpr const char* kTag = "probe";

constexpr const char* kApiBase = "https://api.openligadb.de";
constexpr int kSeason = 2026;
constexpr const char* kFallbackCompetition = "bl1";
constexpr const char* kNtpServer = "pool.ntp.org";

// LittleFS partition (partitions/4mb_ota_littlefs.csv) and the baseline scene.
// LVGL addresses it as "A:scene.jpg": drive letter 65 ('A') and working
// directory "/littlefs/" are set in sdkconfig.defaults.budget.
constexpr const char* kFsLabel = "littlefs";
constexpr const char* kFsBasePath = "/littlefs";
constexpr const char* kSceneFile = "/littlefs/scene.jpg";
constexpr const char* kSceneSource = "A:scene.jpg";
// LVGL refresh cycles at least this long are logged.
constexpr int64_t kSlowRefreshMs = 30;

// HTTP server of the probe (stage 3). Probe-local limits, not the product API.
constexpr size_t kMaxUploadBytes = 98304;       // largest accepted scene upload
constexpr size_t kUploadChunkBytes = 1024;      // receive buffer on the heap
constexpr size_t kFsReserveBytes = 8192;        // kept free besides the upload
constexpr size_t kServerStackBytes = 6144;
constexpr const char* kUploadTempFile = "/littlefs/upload.tmp";

constexpr uint32_t kTaskStackBytes = 10240;
constexpr UBaseType_t kTaskPriority = 3;
constexpr int kWifiRetries = 5;
constexpr uint32_t kWifiTimeoutMs = 30000;
constexpr uint32_t kSntpTimeoutMs = 20000;
constexpr int kHttpTimeoutMs = 15000;
constexpr uint32_t kPageSeconds = 6;
constexpr uint32_t kRefreshSeconds = 300;
// A clock before this date (2026-01-01) is "not synchronised".
constexpr time_t kMinValidTime = 1767225600;

constexpr size_t kNameLen = 16;
constexpr size_t kMaxTable = 20;
constexpr size_t kMaxMatches = 12;
constexpr size_t kPageRows = 10;  // title + 9 lines
constexpr size_t kTextLen = 28;
constexpr size_t kValueLen = 16;
constexpr uint32_t kHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

constexpr uint32_t kColorTitle = 0x00BFFF;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorOwn = 0xFFD700;
constexpr uint32_t kColorDim = 0xA0A0A0;
constexpr uint32_t kColorError = 0xFF4040;

// ---- Data ------------------------------------------------------------------

struct TableRow {
  char name[kNameLen];
  int16_t points;
  int16_t played;
  int16_t goal_diff;
  bool own;
};

struct TableData {
  std::array<TableRow, kMaxTable> rows;
  size_t count;
};

struct MatchRow {
  char home[kNameLen];
  char away[kNameLen];
  char time[6];  // local "HH:MM" of the kickoff
  int16_t goals_home;
  int16_t goals_away;
  bool has_score;
  bool finished;
  bool own;
};

struct MatchdayData {
  char title[kTextLen];
  std::array<MatchRow, kMaxMatches> rows;
  size_t count;
};

TableData g_table;
MatchdayData g_current;
MatchdayData g_last;

// ---- Heap / timing helpers ---------------------------------------------------

// Logs duration, heap use and the local heap low-water mark of one phase.
class PhaseMeter {
 public:
  explicit PhaseMeter(const char* name)
      : name_(name),
        start_free_(heap_caps_get_free_size(kHeapCaps)),
        start_us_(esp_timer_get_time()) {
    monitoring_ = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
  }
  ~PhaseMeter() {
    const size_t low = heap_caps_get_minimum_free_size(kHeapCaps);
    if (monitoring_) heap_caps_monitor_local_minimum_free_size_stop();
    const size_t free_now = heap_caps_get_free_size(kHeapCaps);
    // The monitor cannot run twice at once; then no peak is reported.
    char peak[16] = "n/a";
    if (monitoring_) {
      std::snprintf(peak, sizeof(peak), "%u",
                    static_cast<unsigned>(start_free_ > low ? start_free_ - low : 0));
    }
    ESP_LOGI(kTag,
             "phase %s: %lld ms, heap free %u -> %u B, local peak use %s B, "
             "largest block %u B, task stack free %u B",
             name_, static_cast<long long>((esp_timer_get_time() - start_us_) / 1000),
             static_cast<unsigned>(start_free_), static_cast<unsigned>(free_now),
             peak,
             static_cast<unsigned>(heap_caps_get_largest_free_block(kHeapCaps)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  }
  PhaseMeter(const PhaseMeter&) = delete;
  PhaseMeter& operator=(const PhaseMeter&) = delete;

 private:
  const char* name_;
  size_t start_free_;
  int64_t start_us_;
  bool monitoring_ = false;
};

// ---- Text helpers ------------------------------------------------------------

// Copies UTF-8 as ASCII: the built-in LVGL font has no umlauts, so German
// letters are transliterated (finding for the font decision in P5).
void copyAscii(char* out, size_t capacity, const char* in) {
  size_t n = 0;
  const auto put = [&](const char* s) {
    while (*s != '\0' && n + 1 < capacity) out[n++] = *s++;
  };
  for (const auto* p = reinterpret_cast<const unsigned char*>(in);
       *p != '\0' && n + 1 < capacity; ++p) {
    if (*p < 0x80) {
      out[n++] = static_cast<char>(*p);
    } else if (*p == 0xC3 && p[1] != 0) {
      ++p;
      switch (*p) {
        case 0xA4: put("ae"); break;
        case 0xB6: put("oe"); break;
        case 0xBC: put("ue"); break;
        case 0x84: put("Ae"); break;
        case 0x96: put("Oe"); break;
        case 0x9C: put("Ue"); break;
        case 0x9F: put("ss"); break;
        default: put("?"); break;
      }
    } else if ((*p & 0xC0) == 0xC0) {
      put("?");
    }
  }
  out[n] = '\0';
}

// ---- Settings from the optional secrets header ---------------------------------

bool validCompetition(const char* s) {
  if (s[0] == '\0') return false;
  for (const char* p = s; *p != '\0'; ++p) {
    const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                    *p == '-';
    if (!ok) return false;
  }
  return true;
}

const char* competition() {
  return validCompetition(SECRET_DEFAULT_COMPETITION)
             ? SECRET_DEFAULT_COMPETITION
             : kFallbackCompetition;
}

int ownTeamId() { return std::atoi(SECRET_DEFAULT_TEAM_ID); }

// ---- Page model and view -----------------------------------------------------

struct PageLine {
  char text[kTextLen];
  char value[kValueLen];
  uint32_t color;
};

struct Page {
  std::array<PageLine, kPageRows> lines;
  size_t count;
};

struct RowWidgets {
  lv_obj_t* row;
  lv_obj_t* left;
  lv_obj_t* right;
};

std::array<RowWidgets, kPageRows> g_widgets;
bool g_screen_ready = false;
bool g_scene_present = false;
lv_obj_t* g_scene = nullptr;
int64_t g_refresh_start_us = 0;

constexpr int32_t kRowPad = 2;
constexpr int32_t kValueWidth = 46;

// ---- LittleFS ----------------------------------------------------------------

// Mounts the LittleFS partition (formatted on first use) and reports space and
// whether the baseline scene file exists.
bool mountFs() {
  PhaseMeter meter("littlefs mount");
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = kFsBasePath;
  conf.partition_label = kFsLabel;
  conf.format_if_mount_failed = true;
  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "LittleFS mount failed: %s", esp_err_to_name(err));
    return false;
  }
  size_t total = 0;
  size_t used = 0;
  if (esp_littlefs_info(kFsLabel, &total, &used) == ESP_OK) {
    ESP_LOGI(kTag, "LittleFS mounted: %u B total, %u B used",
             static_cast<unsigned>(total), static_cast<unsigned>(used));
  }
  struct stat st = {};
  g_scene_present = stat(kSceneFile, &st) == 0 && st.st_size > 0;
  if (g_scene_present) {
    ESP_LOGI(kTag, "scene file: %s, %ld B", kSceneFile,
             static_cast<long>(st.st_size));
  } else {
    ESP_LOGW(kTag, "scene file %s not found (pio run -t uploadfs)", kSceneFile);
  }
  return true;
}

// ---- LVGL refresh timing -------------------------------------------------------

// A refresh cycle includes rendering (and with a JPEG scene the decoding) and
// the flush. Only cycles that took noticeable time are logged.
void onRefreshStart(lv_event_t*) { g_refresh_start_us = esp_timer_get_time(); }

void onRefreshReady(lv_event_t*) {
  const int64_t ms = (esp_timer_get_time() - g_refresh_start_us) / 1000;
  if (ms >= kSlowRefreshMs) {
    ESP_LOGI(kTag, "LVGL refresh cycle: %lld ms", static_cast<long long>(ms));
  }
}

lv_obj_t* addLabel(lv_obj_t* parent, lv_text_align_t align) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
  lv_obj_set_style_text_align(label, align, 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(kColorText), 0);
  return label;
}

// Creates the scene image at the back of the screen (LVGL lock held).
bool createScene(lv_obj_t* screen) {
  g_scene = lv_image_create(screen);
  if (g_scene == nullptr) return false;
  lv_image_set_src(g_scene, kSceneSource);
  lv_obj_set_pos(g_scene, 0, 0);
  lv_obj_move_to_index(g_scene, 0);
  return true;
}

// Replaces scene.jpg with the finished upload and makes LVGL read it again. The
// rename runs under the LVGL lock: a draw task keeps the JPEG open while it
// decodes, and LittleFS refuses to replace an open file.
bool commitScene(bool* shown) {
  *shown = false;
  if (!g_screen_ready || !display::lock(cfg::kBootScreenLockTimeoutMs)) {
    return false;
  }
  const bool renamed = std::rename(kUploadTempFile, kSceneFile) == 0;
  if (renamed) {
    bool ok = true;
    if (g_scene == nullptr) {
      ok = createScene(lv_screen_active());
    } else {
      lv_image_set_src(g_scene, kSceneSource);
    }
    lv_obj_invalidate(lv_screen_active());
    g_scene_present = ok;
    *shown = ok;
  }
  display::unlock();
  return renamed;
}

// Creates the fixed rows. Each row is as wide as the safe area allows for
// its own horizontal band, so nothing is drawn into the physical corners.
bool buildScreen() {
  if (!display::lock(cfg::kBootScreenLockTimeoutMs)) return false;
  bool ok = true;
  const auto safe_area =
      geometry::SafeArea::forDisplay(hw::kDisplay, cfg::kContentMarginDivisor);
  lv_obj_t* screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(screen, false);

  lv_display_t* disp = lv_display_get_default();
  lv_display_add_event_cb(disp, onRefreshStart, LV_EVENT_REFR_START, nullptr);
  lv_display_add_event_cb(disp, onRefreshReady, LV_EVENT_REFR_READY, nullptr);

  // Baseline scene: a 240x240 JPEG from LittleFS, decoded by LVGL (TJPGD)
  // while it renders. Created first so it lies behind the text rows.
  if (g_scene_present) {
    ok = createScene(screen);
  }

  const int32_t line_h =
      lv_font_get_line_height(&lv_font_montserrat_14) + kRowPad;
  const int32_t top =
      (static_cast<int32_t>(hw::kDisplay.height) -
       static_cast<int32_t>(kPageRows) * line_h) / 2;
  for (size_t i = 0; i < kPageRows && ok; ++i) {
    const geometry::HorizontalSpan band = safe_area.spanForBand(
        top + static_cast<int32_t>(i) * line_h, line_h);
    RowWidgets& w = g_widgets[i];
    w.row = lv_obj_create(screen);
    if (w.row == nullptr || band.width <= kValueWidth + 16) {
      ok = false;
      break;
    }
    lv_obj_remove_style_all(w.row);
    lv_obj_set_scrollable(w.row, false);
    // Translucent backing: text over the scene stays readable, and the
    // blend over the decoded image is part of what the probe exercises.
    lv_obj_set_style_bg_color(w.row, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(w.row, LV_OPA_60, 0);
    lv_obj_set_pos(w.row, band.x, top + static_cast<int32_t>(i) * line_h);
    lv_obj_set_size(w.row, band.width, line_h);
    w.left = addLabel(w.row, LV_TEXT_ALIGN_LEFT);
    w.right = addLabel(w.row, LV_TEXT_ALIGN_RIGHT);
    if (w.left == nullptr || w.right == nullptr) {
      ok = false;
      break;
    }
    lv_obj_set_width(w.left, band.width - kValueWidth);
    lv_obj_set_width(w.right, kValueWidth);
    lv_obj_align(w.left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(w.right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_hidden(w.row, true);
  }
  display::unlock();
  g_screen_ready = ok;
  return ok;
}

void showPage(const Page& page) {
  if (!g_screen_ready) return;
  if (!display::lock(cfg::kBootScreenLockTimeoutMs)) {
    ESP_LOGW(kTag, "page skipped: LVGL lock timeout");
    return;
  }
  for (size_t i = 0; i < kPageRows; ++i) {
    RowWidgets& w = g_widgets[i];
    if (i >= page.count) {
      lv_obj_set_hidden(w.row, true);
      continue;
    }
    const PageLine& line = page.lines[i];
    lv_label_set_text(w.left, line.text);
    lv_label_set_text(w.right, line.value);
    lv_obj_set_style_text_color(w.left, lv_color_hex(line.color), 0);
    lv_obj_set_style_text_color(w.right, lv_color_hex(line.color), 0);
    lv_obj_set_hidden(w.row, false);
  }
  display::unlock();
}

PageLine& addLine(Page& page, uint32_t color, const char* text,
                  const char* value) {
  PageLine& line = page.lines[page.count++];
  copyAscii(line.text, sizeof(line.text), text);
  copyAscii(line.value, sizeof(line.value), value);
  line.color = color;
  return line;
}

void showStatus(const char* line1, const char* line2 = "",
                uint32_t color = kColorText) {
  Page page = {};
  addLine(page, kColorTitle, "P1.8 probe", "");
  addLine(page, color, line1, "");
  if (line2[0] != '\0') addLine(page, color, line2, "");
  showPage(page);
}

Page tablePage(size_t first, size_t last_exclusive) {
  Page page = {};
  char title[kTextLen];
  std::snprintf(title, sizeof(title), "Tabelle %s", competition());
  addLine(page, kColorTitle, title, "Pkt");
  for (size_t i = first; i < last_exclusive && i < g_table.count; ++i) {
    const TableRow& r = g_table.rows[i];
    char text[kTextLen];
    std::snprintf(text, sizeof(text), "%2u %s", static_cast<unsigned>(i + 1),
                  r.name);
    char value[kValueLen];
    std::snprintf(value, sizeof(value), "%d", static_cast<int>(r.points));
    addLine(page, r.own ? kColorOwn : kColorText, text, value);
  }
  return page;
}

Page matchdayPage(const MatchdayData& md) {
  Page page = {};
  addLine(page, kColorTitle, md.title, "");
  for (size_t i = 0; i < md.count && page.count < kPageRows; ++i) {
    const MatchRow& m = md.rows[i];
    char text[kTextLen];
    std::snprintf(text, sizeof(text), "%.6s - %.6s", m.home, m.away);
    char value[kValueLen];
    if (m.has_score) {
      std::snprintf(value, sizeof(value), "%d:%d",
                    static_cast<int>(m.goals_home),
                    static_cast<int>(m.goals_away));
    } else {
      std::snprintf(value, sizeof(value), "%s", m.time);
    }
    addLine(page, m.own ? kColorOwn : (m.finished ? kColorText : kColorDim),
            text, value);
  }
  return page;
}

// ---- WiFi / SNTP -------------------------------------------------------------

constexpr EventBits_t kBitGotIp = BIT0;
constexpr EventBits_t kBitFailed = BIT1;
EventGroupHandle_t g_wifi_events = nullptr;
int g_wifi_retry = 0;

void onWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
    ESP_LOGW(kTag, "WiFi disconnected (reason %d), attempt %d of %d",
             static_cast<int>(ev->reason), g_wifi_retry + 1, kWifiRetries);
    if (g_wifi_retry++ < kWifiRetries) {
      esp_wifi_connect();
    } else {
      xEventGroupSetBits(g_wifi_events, kBitFailed);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    g_wifi_retry = 0;
    xEventGroupSetBits(g_wifi_events, kBitGotIp);
  }
}

bool connectWifi() {
  PhaseMeter meter("wifi init + connect");
  if (strlen(SECRET_WIFI_SSID_1) == 0) {
    ESP_LOGW(kTag, "no WiFi credentials (include/secrets.h)");
    showStatus("no WiFi credentials", "include/secrets.h", kColorError);
    return false;
  }
  ESP_LOGI(kTag, "WiFi credentials present (password %s)",
           strlen(SECRET_WIFI_PASSWORD_1) > 0 ? "set" : "empty");

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  g_wifi_events = xEventGroupCreate();
  if (g_wifi_events == nullptr) return false;

  const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &onWifiEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &onWifiEvent, nullptr));

  wifi_config_t conf = {};
  strlcpy(reinterpret_cast<char*>(conf.sta.ssid), SECRET_WIFI_SSID_1,
          sizeof(conf.sta.ssid));
  strlcpy(reinterpret_cast<char*>(conf.sta.password), SECRET_WIFI_PASSWORD_1,
          sizeof(conf.sta.password));
  conf.sta.pmf_cfg.capable = true;
  conf.sta.pmf_cfg.required = false;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &conf));
  ESP_ERROR_CHECK(esp_wifi_start());

  const EventBits_t bits = xEventGroupWaitBits(
      g_wifi_events, kBitGotIp | kBitFailed, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(kWifiTimeoutMs));
  if ((bits & kBitGotIp) == 0) {
    ESP_LOGE(kTag, "WiFi connection failed or timed out");
    showStatus("WiFi failed", "", kColorError);
    return false;
  }
  esp_netif_ip_info_t ip = {};
  esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    ESP_LOGI(kTag, "WiFi connected: RSSI %d dBm, channel %u, IP " IPSTR,
             static_cast<int>(ap.rssi), static_cast<unsigned>(ap.primary),
             IP2STR(&ip.ip));
  }
  return true;
}

bool syncTime() {
  PhaseMeter meter("sntp");
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(kNtpServer);
  if (esp_netif_sntp_init(&config) != ESP_OK) return false;
  const esp_err_t err =
      esp_netif_sntp_sync_wait(pdMS_TO_TICKS(kSntpTimeoutMs));
  setenv("TZ", SECRET_TIMEZONE, 1);
  tzset();
  time_t now = 0;
  time(&now);
  if (err != ESP_OK || now < kMinValidTime) {
    ESP_LOGE(kTag, "time not synchronised (%s)", esp_err_to_name(err));
    return false;
  }
  tm local = {};
  localtime_r(&now, &local);
  char text[32];
  strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
  ESP_LOGI(kTag, "time synchronised: %s local", text);
  return true;
}

// ---- HTTP server (stage 3) ---------------------------------------------------

// Minimal page: status text and a form to replace the scene file. Held in
// flash; probe-only, the real Web UI is designed in P8.
constexpr char kIndexHtml[] =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>esp32-fussball probe</title><h3>P1.8 probe</h3>"
    "<pre id=s>...</pre><input type=file id=f accept='.jpg,.jpeg'>"
    "<button onclick=up()>Upload scene.jpg</button>"
    "<script>function st(){fetch('/status').then(r=>r.text())"
    ".then(t=>s.textContent=t)}st();function up(){var x=f.files[0];"
    "if(!x)return;s.textContent='uploading...';fetch('/upload',"
    "{method:'POST',body:x}).then(r=>r.text()).then(t=>{s.textContent=t;"
    "setTimeout(st,1500)})}</script>";

esp_err_t sendText(httpd_req_t* req, const char* status, const char* text) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, text);
}

esp_err_t indexHandler(httpd_req_t* req) {
  PhaseMeter meter("http GET /");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t statusHandler(httpd_req_t* req) {
  PhaseMeter meter("http GET /status");
  size_t fs_total = 0;
  size_t fs_used = 0;
  esp_littlefs_info(kFsLabel, &fs_total, &fs_used);
  struct stat st = {};
  const long scene_bytes = stat(kSceneFile, &st) == 0 ? static_cast<long>(st.st_size) : -1;
  char body[256];
  std::snprintf(body, sizeof(body),
                "uptime_s %lld\nheap_free %u\nheap_largest %u\nheap_min %u\n"
                "fs_total %u\nfs_used %u\nscene_bytes %ld\n",
                static_cast<long long>(esp_timer_get_time() / 1000000),
                static_cast<unsigned>(heap_caps_get_free_size(kHeapCaps)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(kHeapCaps)),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(kHeapCaps)),
                static_cast<unsigned>(fs_total), static_cast<unsigned>(fs_used),
                scene_bytes);
  return sendText(req, "200 OK", body);
}

// POST /upload: the body is the new scene.jpg. It is streamed into a temporary
// file and only renamed over scene.jpg when it arrived completely, so a failed
// upload keeps the old scene (replacement reserve, OQ-32).
esp_err_t uploadHandler(httpd_req_t* req) {
  PhaseMeter meter("http POST /upload");
  const size_t len = req->content_len;
  if (len < 4 || len > kMaxUploadBytes) {
    return sendText(req, "413 Payload Too Large", "size not accepted\n");
  }
  size_t fs_total = 0;
  size_t fs_used = 0;
  esp_littlefs_info(kFsLabel, &fs_total, &fs_used);
  if (fs_total < fs_used + len + kFsReserveBytes) {
    return sendText(req, "507 Insufficient Storage", "not enough space\n");
  }
  char* chunk = static_cast<char*>(std::malloc(kUploadChunkBytes));
  FILE* file = std::fopen(kUploadTempFile, "wb");
  if (chunk == nullptr || file == nullptr) {
    std::free(chunk);
    if (file != nullptr) std::fclose(file);
    return sendText(req, "500 Internal Server Error", "no memory or file\n");
  }
  size_t received = 0;
  bool ok = true;
  int idle = 0;
  while (received < len && ok) {
    const int n = httpd_req_recv(req, chunk, std::min(kUploadChunkBytes, len - received));
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++idle < 5) continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    idle = 0;
    if (received == 0 &&
        !(static_cast<uint8_t>(chunk[0]) == 0xFF && static_cast<uint8_t>(chunk[1]) == 0xD8)) {
      ok = false;  // not a JPEG
      break;
    }
    ok = std::fwrite(chunk, 1, static_cast<size_t>(n), file) == static_cast<size_t>(n);
    received += static_cast<size_t>(n);
  }
  std::free(chunk);
  ok = (std::fclose(file) == 0) && ok && received == len;
  bool shown = false;
  if (ok) ok = commitScene(&shown);
  if (!ok) {
    std::remove(kUploadTempFile);
    ESP_LOGW(kTag, "upload failed after %u of %u B", static_cast<unsigned>(received),
             static_cast<unsigned>(len));
    return sendText(req, "400 Bad Request", "upload failed, old scene kept\n");
  }
  ESP_LOGI(kTag, "upload ok: %u B, scene reloaded: %s", static_cast<unsigned>(len),
           shown ? "yes" : "no");
  return sendText(req, "200 OK", "ok\n");
}

bool startHttpServer() {
  PhaseMeter meter("http server start");
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = kServerStackBytes;
  config.max_open_sockets = 3;
  config.max_uri_handlers = 4;
  config.lru_purge_enable = true;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(kTag, "HTTP server could not be started");
    return false;
  }
  static const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr},
      {.uri = "/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = nullptr},
      {.uri = "/upload", .method = HTTP_POST, .handler = uploadHandler, .user_ctx = nullptr},
  };
  for (const httpd_uri_t& route : routes) httpd_register_uri_handler(server, &route);
  ESP_LOGI(kTag, "HTTP server listening on port %u, max %d sockets",
           static_cast<unsigned>(config.server_port), config.max_open_sockets);
  return true;
}

// ---- HTTPS + filtered JSON ---------------------------------------------------

// Adapts esp_http_client_read to the reader interface of ArduinoJson (a class
// with read() and readBytes()). The document is parsed straight from the
// connection; the response is never held in RAM as a whole.
class HttpStream {
 public:
  explicit HttpStream(esp_http_client_handle_t client) : client_(client) {}

  int read() {
    if (!fill()) return -1;
    return static_cast<uint8_t>(buf_[pos_++]);
  }
  size_t readBytes(char* out, size_t count) {
    size_t done = 0;
    while (done < count && fill()) {
      const size_t take = std::min(count - done, len_ - pos_);
      std::memcpy(out + done, buf_ + pos_, take);
      pos_ += take;
      done += take;
    }
    return done;
  }
  size_t total() const { return total_; }
  bool failed() const { return failed_; }

 private:
  bool fill() {
    if (pos_ < len_) return true;
    if (eof_) return false;
    const int n = esp_http_client_read(client_, buf_, sizeof(buf_));
    if (n <= 0) {
      eof_ = true;
      failed_ = n < 0;
      return false;
    }
    pos_ = 0;
    len_ = static_cast<size_t>(n);
    total_ += len_;
    return true;
  }

  esp_http_client_handle_t client_;
  char buf_[512];
  size_t pos_ = 0;
  size_t len_ = 0;
  size_t total_ = 0;
  bool eof_ = false;
  bool failed_ = false;
};

// GET <base><path>, filtered parse into doc. Logs TLS/transfer/parse figures.
bool fetchJson(const char* path, const JsonDocument& filter, JsonDocument& doc) {
  PhaseMeter meter(path);
  char url[128];
  std::snprintf(url, sizeof(url), "%s%s", kApiBase, path);

  esp_http_client_config_t config = {};
  config.url = url;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = kHttpTimeoutMs;
  config.buffer_size = 1024;
  config.buffer_size_tx = 512;
  config.keep_alive_enable = false;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(kTag, "http client init failed");
    return false;
  }
  esp_http_client_set_header(client, "Accept", "application/json");

  const int64_t t0 = esp_timer_get_time();
  const esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "%s: connect/TLS failed: %s", path, esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  const int64_t t_open = esp_timer_get_time();
  ESP_LOGI(kTag, "%s: TLS connected in %lld ms, heap free %u B, largest %u B",
           path, static_cast<long long>((t_open - t0) / 1000),
           static_cast<unsigned>(heap_caps_get_free_size(kHeapCaps)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(kHeapCaps)));

  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  bool ok = false;
  if (status != 200) {
    ESP_LOGE(kTag, "%s: HTTP status %d", path, status);
  } else {
    HttpStream stream(client);
    const DeserializationError jerr = deserializeJson(
        doc, stream, DeserializationOption::Filter(filter));
    const int64_t t_done = esp_timer_get_time();
    ok = !jerr && !doc.overflowed() && !stream.failed();
    ESP_LOGI(kTag,
             "%s: %u B received, parse+transfer %lld ms, result: %s", path,
             static_cast<unsigned>(stream.total()),
             static_cast<long long>((t_done - t_open) / 1000),
             ok ? "ok" : (jerr ? jerr.c_str() : "stream error"));
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

bool fetchTable() {
  JsonDocument filter;
  filter[0]["teamInfoId"] = true;
  filter[0]["shortName"] = true;
  filter[0]["points"] = true;
  filter[0]["matches"] = true;
  filter[0]["goalDiff"] = true;

  char path[64];
  std::snprintf(path, sizeof(path), "/getbltable/%s/%d", competition(),
                kSeason);
  JsonDocument doc;
  if (!fetchJson(path, filter, doc)) return false;

  const int own = ownTeamId();
  g_table.count = 0;
  for (JsonObjectConst t : doc.as<JsonArrayConst>()) {
    if (g_table.count >= kMaxTable) break;
    TableRow& r = g_table.rows[g_table.count++];
    copyAscii(r.name, sizeof(r.name), t["shortName"] | "?");
    r.points = t["points"] | 0;
    r.played = t["matches"] | 0;
    r.goal_diff = t["goalDiff"] | 0;
    r.own = own > 0 && (t["teamInfoId"] | -1) == own;
  }
  ESP_LOGI(kTag, "table: %u teams", static_cast<unsigned>(g_table.count));
  return g_table.count > 0;
}

// Current matchday number; 0 on failure.
int fetchCurrentGroup() {
  JsonDocument filter;
  filter["groupOrderID"] = true;
  char path[48];
  std::snprintf(path, sizeof(path), "/getcurrentgroup/%s", competition());
  JsonDocument doc;
  if (!fetchJson(path, filter, doc)) return 0;
  return doc["groupOrderID"] | 0;
}

void parseKickoff(const char* iso, char (&out)[6]) {
  // "2026-10-09T20:30:00" (already local time): take HH:MM.
  if (std::strlen(iso) >= 16 && iso[10] == 'T') {
    std::memcpy(out, iso + 11, 5);
    out[5] = '\0';
  } else {
    std::strcpy(out, "--:--");
  }
}

bool fetchMatchday(int group, MatchdayData& out) {
  JsonDocument filter;
  filter[0]["matchDateTime"] = true;
  filter[0]["matchIsFinished"] = true;
  filter[0]["group"]["groupName"] = true;
  filter[0]["team1"]["teamId"] = true;
  filter[0]["team1"]["shortName"] = true;
  filter[0]["team2"]["teamId"] = true;
  filter[0]["team2"]["shortName"] = true;
  filter[0]["matchResults"][0]["resultTypeID"] = true;
  filter[0]["matchResults"][0]["resultOrderID"] = true;
  filter[0]["matchResults"][0]["pointsTeam1"] = true;
  filter[0]["matchResults"][0]["pointsTeam2"] = true;

  char path[64];
  std::snprintf(path, sizeof(path), "/getmatchdata/%s/%d/%d", competition(),
                kSeason, group);
  JsonDocument doc;
  if (!fetchJson(path, filter, doc)) return false;

  const int own = ownTeamId();
  out.count = 0;
  std::snprintf(out.title, sizeof(out.title), "%d. Spieltag", group);
  for (JsonObjectConst m : doc.as<JsonArrayConst>()) {
    if (out.count >= kMaxMatches) break;
    MatchRow& r = out.rows[out.count++];
    copyAscii(r.home, sizeof(r.home), m["team1"]["shortName"] | "?");
    copyAscii(r.away, sizeof(r.away), m["team2"]["shortName"] | "?");
    parseKickoff(m["matchDateTime"] | "", r.time);
    r.finished = m["matchIsFinished"] | false;
    r.own = own > 0 && ((m["team1"]["teamId"] | -1) == own ||
                        (m["team2"]["teamId"] | -1) == own);
    // Final result (type 2) wins; otherwise the latest known result (live).
    r.has_score = false;
    r.goals_home = r.goals_away = 0;
    int best = -1;
    for (JsonObjectConst res : m["matchResults"].as<JsonArrayConst>()) {
      int rank = res["resultOrderID"] | 0;
      if ((res["resultTypeID"] | 0) == 2) rank += 100;
      if (rank > best) {
        best = rank;
        r.has_score = true;
        r.goals_home = res["pointsTeam1"] | 0;
        r.goals_away = res["pointsTeam2"] | 0;
      }
    }
  }
  ESP_LOGI(kTag, "matchday %d: %u matches", group,
           static_cast<unsigned>(out.count));
  return out.count > 0;
}

// OQ-32 stage 1, information only: asks for a full redraw (JPEG decode) right
// before the fetches so that decode and TLS/parse overlap in time.
void requestSceneRedraw() {
  if (!g_scene_present || !display::lock(cfg::kBootScreenLockTimeoutMs)) return;
  lv_obj_invalidate(lv_screen_active());
  display::unlock();
  ESP_LOGI(kTag, "overlap run: full redraw requested at the start of the refresh");
}

// One refresh of table, current and previous matchday. Heavy operations run
// one after the other (OQ-32 stage 1).
// (No PhaseMeter here: the local heap minimum monitor cannot be nested.)
bool refreshData() {
  requestSceneRedraw();
  const int64_t t0 = esp_timer_get_time();
  bool ok = fetchTable();
  const int group = fetchCurrentGroup();
  if (group > 0) {
    ok = fetchMatchday(group, g_current) && ok;
    if (group > 1) ok = fetchMatchday(group - 1, g_last) && ok;
  } else {
    ok = false;
  }
  ESP_LOGI(kTag, "refresh all: %lld ms, %s",
           static_cast<long long>((esp_timer_get_time() - t0) / 1000),
           ok ? "ok" : "incomplete");
  return ok;
}

void showPages() {
  const size_t half = (kPageRows - 1);
  showPage(tablePage(0, half));
  vTaskDelay(pdMS_TO_TICKS(kPageSeconds * 1000));
  if (g_table.count > half) {
    showPage(tablePage(half, 2 * half));
    vTaskDelay(pdMS_TO_TICKS(kPageSeconds * 1000));
  }
  if (g_current.count > 0) {
    showPage(matchdayPage(g_current));
    vTaskDelay(pdMS_TO_TICKS(kPageSeconds * 1000));
  }
  if (g_last.count > 0) {
    showPage(matchdayPage(g_last));
    vTaskDelay(pdMS_TO_TICKS(kPageSeconds * 1000));
  }
}

void probeTask(void*) {
  ESP_LOGI(kTag, "probe task started, competition %s, season %d, own team %s",
           competition(), kSeason,
           ownTeamId() > 0 ? "configured" : "not configured");
  if (!mountFs()) {
    ESP_LOGE(kTag, "continuing without LittleFS");
  }
  if (!buildScreen()) {
    ESP_LOGE(kTag, "probe screen could not be built");
  }

  showStatus("WiFi ...");
  if (!connectWifi()) {
    vTaskDelete(nullptr);
    return;
  }
  startHttpServer();
  showStatus("WiFi ok", "time ...");
  if (!syncTime()) {
    showStatus("no time (SNTP)", "", kColorError);
    vTaskDelete(nullptr);
    return;
  }
  showStatus("time ok", "loading data ...");

  int64_t last_refresh_us = 0;
  bool have_data = false;
  while (true) {
    if (!have_data ||
        esp_timer_get_time() - last_refresh_us >=
            static_cast<int64_t>(kRefreshSeconds) * 1000000) {
      last_refresh_us = esp_timer_get_time();
      have_data = refreshData();
      if (!have_data) {
        showStatus("data failed", "retry in 60 s", kColorError);
        vTaskDelay(pdMS_TO_TICKS(60000));
        continue;
      }
      ESP_LOGI(kTag, "data refreshed; heap free %u B, largest %u B, min free %u B",
               static_cast<unsigned>(heap_caps_get_free_size(kHeapCaps)),
               static_cast<unsigned>(heap_caps_get_largest_free_block(kHeapCaps)),
               static_cast<unsigned>(heap_caps_get_minimum_free_size(kHeapCaps)));
    }
    showPages();
  }
}

}  // namespace

void start() {
  if (xTaskCreate(probeTask, "probe", kTaskStackBytes, nullptr, kTaskPriority,
                  nullptr) != pdPASS) {
    ESP_LOGE(kTag, "probe task could not be created");
  }
}

}  // namespace probe
