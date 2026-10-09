// Display port: SPI bus, panel controller, esp_lvgl_port and the display
// failure handling (draw failures, LVGL supervision, controlled restart,
// abnormal-reset bound). See display_port.h.

#include "display_port.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gc9a01_panel.h"
#include "lvgl.h"
#include "sdkconfig.h"

namespace display {
namespace {

constexpr const char* kTag = "display";

constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr std::size_t kBytesPerPixel = 2;  // RGB565

// Each of the two LVGL draw buffers holds 1/kDrawBufferFraction of the screen
// (240x240: 20 rows = 9600 B). Internal, DMA-capable RAM; no PSRAM on the C6.
constexpr uint32_t kDrawBufferFraction = 12;
// Queued SPI transactions of the panel IO (value of the earlier driver the
// GC9A01 sequence comes from, see gc9a01_panel.h).
constexpr std::size_t kTransQueueDepth = 10;
// Rows per transfer when the panel is cleared to black before DISPON.
constexpr uint32_t kClearRows = 8;
// Upper bound, with margin, of esp_lvgl_port 2.9.0's private per-display
// context (plain malloc in lvgl_port_add_disp; about 60 B on 32-bit targets),
// used by the best-effort allocation probe.
constexpr std::size_t kPortContextBytes = 256;

using cfg::DisplayFault;

// Why the display restarted the device. Kept across the reset.
enum class FailReason : uint32_t {
  kNone,
  kInit,
  kPortMemory,
  kDrawFailures,
  kDrawStateUnknown,
};

const char* reasonText(FailReason reason) {
  switch (reason) {
    case FailReason::kNone: return "none recorded (watchdog or other panic)";
    case FailReason::kInit: return "display init failed";
    case FailReason::kPortMemory: return "no memory for the LVGL display";
    case FailReason::kDrawFailures: return "repeated draw failures";
    case FailReason::kDrawStateUnknown: return "draw queue not drainable";
  }
  return "unknown";
}

// Survives panic, watchdog and software resets; random after power-on,
// hence the magic. LP RAM, not part of the internal heap.
struct ResetRecord {
  uint32_t magic;
  uint32_t abnormal_resets;  // consecutive
  FailReason last_failure;
};
constexpr uint32_t kResetRecordMagic = 0x44495350;  // "DISP"
RTC_NOINIT_ATTR ResetRecord g_reset_record;

esp_lcd_panel_io_handle_t g_io = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
lv_display_t* g_display = nullptr;
const hw::DisplayWiring* g_wiring = nullptr;
esp_task_wdt_user_handle_t g_wdt_user = nullptr;
// Only touched in the LVGL task (draw observer, supervision timer).
uint32_t g_draw_failures = 0;
bool g_redraw_pending = false;
bool g_stable = false;
uint32_t g_init_tick = 0;

void setBacklight(const hw::DisplayWiring& wiring, bool on) {
  if (wiring.backlight == hw::kNoPin) return;
  gpio_set_level(static_cast<gpio_num_t>(wiring.backlight),
                 on == wiring.backlight_active_high ? 1 : 0);
}

// Controlled restart: safe state, reason kept for the next boot, panic output
// with backtrace. The reset releases every resource, so nothing is cleaned up.
[[noreturn]] void failRestart(FailReason reason) {
  g_reset_record.last_failure = reason;
  if (g_wiring != nullptr) setBacklight(*g_wiring, false);
  esp_system_abort(reasonText(reason));
}

bool isAbnormalReset(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

// Counts consecutive abnormal resets. Returns false once the bound is
// reached: the display then stays off until a power cycle or a normal
// (software) restart.
bool resetHistoryAllowsDisplay() {
  const esp_reset_reason_t reason = esp_reset_reason();
  ResetRecord& record = g_reset_record;
  if (record.magic != kResetRecordMagic || !isAbnormalReset(reason)) {
    record = {kResetRecordMagic, 0, FailReason::kNone};
    return true;
  }
  if (record.abnormal_resets < cfg::kAbnormalResetLimit) {
    ++record.abnormal_resets;
  }
  ESP_LOGW(kTag,
           "abnormal reset %u of %u (reset reason %d), display failure: %s",
           static_cast<unsigned>(record.abnormal_resets),
           static_cast<unsigned>(cfg::kAbnormalResetLimit),
           static_cast<int>(reason), reasonText(record.last_failure));
  record.last_failure = FailReason::kNone;
  if (record.abnormal_resets < cfg::kAbnormalResetLimit) return true;
  ESP_LOGE(kTag, "display disabled after %u abnormal resets in a row; power "
                 "cycle or restart to try again",
           static_cast<unsigned>(record.abnormal_resets));
  return false;
}

esp_err_t initBacklight(const hw::DisplayWiring& wiring) {
  if (wiring.backlight == hw::kNoPin) return ESP_OK;
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << wiring.backlight;
  config.mode = GPIO_MODE_OUTPUT;
  ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "backlight GPIO failed");
  setBacklight(wiring, false);
  return ESP_OK;
}

// Fills the whole panel with black so no random RAM content is visible when
// the display is switched on.
esp_err_t clearToBlack(const hw::DisplayProfile& profile) {
  const uint32_t rows = std::min<uint32_t>(kClearRows, profile.height);
  const std::size_t size = std::size_t{profile.width} * rows * kBytesPerPixel;
  void* black = heap_caps_calloc(1, size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  ESP_RETURN_ON_FALSE(black != nullptr, ESP_ERR_NO_MEM, kTag,
                      "no memory for clear buffer");

  esp_err_t err = ESP_OK;
  for (uint32_t y = 0; y < profile.height && err == ESP_OK; y += rows) {
    const uint32_t y_end = std::min<uint32_t>(y + rows, profile.height);
    err = esp_lcd_panel_draw_bitmap(g_panel, 0, y, profile.width, y_end, black);
  }
  // DISPON is a parameter transfer; the SPI panel IO waits for all queued
  // colour transfers before sending it, so the buffer is free afterwards.
  if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(g_panel, true);
  // On an error a transfer may still read the buffer: it is not freed, the
  // caller restarts the device.
  if (err == ESP_OK) heap_caps_free(black);
  return err;
}

esp_err_t initPanel(const hw::DisplayProfile& profile,
                    const hw::DisplayWiring& wiring,
                    std::size_t max_transfer_bytes) {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = wiring.mosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = wiring.sclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = static_cast<int>(max_transfer_bytes);
  ESP_RETURN_ON_ERROR(spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO),
                      kTag, "SPI bus init failed");

  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = static_cast<gpio_num_t>(wiring.cs);
  io.dc_gpio_num = static_cast<gpio_num_t>(wiring.dc);
  io.spi_mode = profile.spi_mode;
  io.pclk_hz = wiring.spi_clock_hz;
  io.trans_queue_depth = kTransQueueDepth;
  io.lcd_cmd_bits = 8;
  io.lcd_param_bits = 8;
  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_io_spi(
          static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io, &g_io),
      kTag, "panel IO init failed");

  switch (profile.controller) {
    case hw::DisplayController::kGc9a01:
      ESP_RETURN_ON_ERROR(
          newGc9a01Panel(g_io,
                         {.reset_gpio = wiring.reset,
                          .bgr_order = profile.bgr_order},
                         &g_panel),
          kTag, "panel create failed");
      break;
  }

  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(g_panel), kTag, "panel reset failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(g_panel), kTag, "panel init failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(g_panel, profile.invert_colors),
                      kTag, "panel invert failed");
  return clearToBlack(profile);
}

// Best-effort probe of what lvgl_port_add_disp takes from the heap, in its
// order: the context with an upper-bound size, then draw buffers 1 and 2 with
// their size, alignment and caps. The blocks are freed again, so nothing is
// reserved; another task can allocate before the port does. The port does
// not handle a failed context allocation safely (panic restart), so a shortage
// known here is turned into a controlled restart instead.
bool portMemoryAvailable(std::size_t buffer_bytes) {
  if constexpr (cfg::kDisplayFault == DisplayFault::kPortMemory) {
    ESP_LOGW(kTag, "fault injection: port allocation pre-check fails");
    buffer_bytes = heap_caps_get_total_size(MALLOC_CAP_DMA) + 1;
  }
  void* context = std::malloc(kPortContextBytes);
  void* buffer1 = heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN,
                                          buffer_bytes, MALLOC_CAP_DMA);
  void* buffer2 = heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN,
                                          buffer_bytes, MALLOC_CAP_DMA);
  const bool available =
      context != nullptr && buffer1 != nullptr && buffer2 != nullptr;
  heap_caps_free(buffer2);
  heap_caps_free(buffer1);
  std::free(context);
  return available;
}

// Runs in the LVGL task inside the flush callback (via draw_bitmap).
void onDrawResult(DrawResult result, void* ctx) {
  switch (result) {
    case DrawResult::kSubmitted:
      g_draw_failures = 0;
      return;
    case DrawResult::kFailedBufferUnknown:
      failRestart(FailReason::kDrawStateUnknown);
    case DrawResult::kFailedBufferFree:
      // No completion will come for this area: end LVGL's flush wait here
      // and redraw the screen from the supervision timer (invalidating is
      // not allowed while rendering).
      lv_display_flush_ready(static_cast<lv_display_t*>(ctx));
      g_redraw_pending = true;
      if (++g_draw_failures >= cfg::kDisplayDrawFailureLimit) {
        failRestart(FailReason::kDrawFailures);
      }
      ESP_LOGW(kTag, "draw failed (%u in a row), screen will be redrawn",
               static_cast<unsigned>(g_draw_failures));
      return;
  }
}

// LVGL timer, so it runs only while the LVGL task makes progress: a stuck
// flush, a blocked transfer, a long-held lock or an LVGL assert stop it and
// the task watchdog restarts the device.
void superviseLvgl(lv_timer_t*) {
  esp_task_wdt_reset_user(g_wdt_user);
  if (g_redraw_pending) {
    g_redraw_pending = false;
    lv_obj_t* screen = lv_screen_active();
    if (screen != nullptr) lv_obj_invalidate(screen);
  }
  if (!g_stable &&
      lv_tick_elaps(g_init_tick) >= cfg::kDisplayStablePeriodS * 1000) {
    g_stable = true;
    g_reset_record.abnormal_resets = 0;
    ESP_LOGI(kTag, "display stable for %u s, abnormal reset count cleared",
             static_cast<unsigned>(cfg::kDisplayStablePeriodS));
  }
}

esp_err_t startSupervision() {
  ESP_RETURN_ON_ERROR(esp_task_wdt_add_user("lvgl", &g_wdt_user), kTag,
                      "task watchdog user failed");
  ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_ERR_TIMEOUT, kTag,
                      "LVGL lock failed");
  setGc9a01DrawObserver(g_panel, onDrawResult, g_display);
  g_init_tick = lv_tick_get();
  // An allocation failure here ends in the LVGL malloc assert.
  lv_timer_create(superviseLvgl, cfg::kDisplaySupervisionPeriodMs, nullptr);
  lvgl_port_unlock();
  return ESP_OK;
}

// Test faults that act after a successful init (cfg::kDisplayFault).
void injectRuntimeFault() {
  if constexpr (cfg::kDisplayFault == DisplayFault::kLvglPoolExhausted) {
    ESP_LOGW(kTag, "fault injection: filling the LVGL pool");
    lvgl_port_lock(0);
    while (true) lv_obj_create(lv_screen_active());
  } else if constexpr (cfg::kDisplayFault == DisplayFault::kLockStall) {
    ESP_LOGW(kTag, "fault injection: holding the LVGL lock for 10 s");
    lvgl_port_lock(0);
    vTaskDelay(pdMS_TO_TICKS(10'000));
    lvgl_port_unlock();
  }
}

esp_err_t initLvgl(const hw::DisplayProfile& profile,
                   uint32_t buffer_pixels) {
  // Defaults of esp_lvgl_port 2.9.0: task priority 4, stack 7168 B in
  // internal RAM, any core, 5 ms tick timer.
  const lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_RETURN_ON_ERROR(lvgl_port_init(&port), kTag, "LVGL port init failed");

  lvgl_port_display_cfg_t disp = {};
  disp.io_handle = g_io;
  disp.panel_handle = g_panel;
  disp.buffer_size = buffer_pixels;
  disp.double_buffer = true;
  disp.hres = profile.width;
  disp.vres = profile.height;
  disp.color_format = LV_COLOR_FORMAT_RGB565;
  disp.flags.buff_dma = 1;
  // LVGL renders little-endian RGB565, the panel expects big-endian.
  disp.flags.swap_bytes = 1;
  if (!portMemoryAvailable(std::size_t{buffer_pixels} * kBytesPerPixel)) {
    ESP_LOGE(kTag, "not enough DMA memory for 2 x %u B draw buffers",
             static_cast<unsigned>(buffer_pixels * kBytesPerPixel));
    failRestart(FailReason::kPortMemory);
  }
  g_display = lvgl_port_add_disp(&disp);
  ESP_RETURN_ON_FALSE(g_display != nullptr, ESP_ERR_NO_MEM, kTag,
                      "LVGL display registration failed");
  return ESP_OK;
}

}  // namespace

esp_err_t init(const hw::DisplayProfile& profile,
               const hw::DisplayWiring& wiring) {
  ESP_RETURN_ON_FALSE(g_display == nullptr, ESP_ERR_INVALID_STATE, kTag,
                      "already initialised");
  if (!resetHistoryAllowsDisplay()) return ESP_ERR_INVALID_STATE;
  g_wiring = &wiring;

  const uint32_t rows =
      std::max<uint32_t>(1, profile.height / kDrawBufferFraction);
  const uint32_t buffer_pixels = uint32_t{profile.width} * rows;
  const std::size_t buffer_bytes = buffer_pixels * kBytesPerPixel;

  if (initBacklight(wiring) != ESP_OK ||
      initPanel(profile, wiring, buffer_bytes) != ESP_OK) {
    failRestart(FailReason::kInit);
  }
  setBacklight(wiring, true);
  if (initLvgl(profile, buffer_pixels) != ESP_OK ||
      startSupervision() != ESP_OK) {
    failRestart(FailReason::kInit);
  }

  ESP_LOGI(kTag, "ready: %ux%u, draw buffers 2 x %u B (%u rows), SPI %u Hz",
           static_cast<unsigned>(profile.width),
           static_cast<unsigned>(profile.height),
           static_cast<unsigned>(buffer_bytes), static_cast<unsigned>(rows),
           static_cast<unsigned>(wiring.spi_clock_hz));
  injectRuntimeFault();
  return ESP_OK;
}

bool lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }

void unlock() { lvgl_port_unlock(); }

void logMemory() {
  // Without a registered display the LVGL port may not exist; locking it
  // would hit its assert.
  if (g_display == nullptr) return;
  if (!lock(cfg::kStatusLockTimeoutMs)) {
    ESP_LOGW(kTag, "LVGL busy, pool usage not logged");
    return;
  }
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  unlock();
  ESP_LOGI(kTag,
           "LVGL pool %u B: used %u B (%u%%), max used %u B, largest free "
           "%u B, fragmentation %u%%",
           static_cast<unsigned>(mon.total_size),
           static_cast<unsigned>(mon.total_size - mon.free_size),
           static_cast<unsigned>(mon.used_pct),
           static_cast<unsigned>(mon.max_used),
           static_cast<unsigned>(mon.free_biggest_size),
           static_cast<unsigned>(mon.frag_pct));
}

}  // namespace display
