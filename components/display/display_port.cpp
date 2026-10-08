// Display port: SPI bus, panel controller, esp_lvgl_port. See display_port.h.

#include "display_port.h"

#include <algorithm>
#include <cstddef>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "gc9a01_panel.h"
#include "lvgl.h"

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

esp_lcd_panel_io_handle_t g_io = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
lv_display_t* g_display = nullptr;

void setBacklight(const hw::DisplayWiring& wiring, bool on) {
  if (wiring.backlight == hw::kNoPin) return;
  gpio_set_level(static_cast<gpio_num_t>(wiring.backlight),
                 on == wiring.backlight_active_high ? 1 : 0);
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
  heap_caps_free(black);
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

  const uint32_t rows =
      std::max<uint32_t>(1, profile.height / kDrawBufferFraction);
  const uint32_t buffer_pixels = uint32_t{profile.width} * rows;
  const std::size_t buffer_bytes = buffer_pixels * kBytesPerPixel;

  ESP_RETURN_ON_ERROR(initBacklight(wiring), kTag, "backlight init failed");
  ESP_RETURN_ON_ERROR(initPanel(profile, wiring, buffer_bytes), kTag,
                      "panel bring-up failed");
  setBacklight(wiring, true);
  ESP_RETURN_ON_ERROR(initLvgl(profile, buffer_pixels), kTag,
                      "LVGL bring-up failed");

  ESP_LOGI(kTag, "ready: %ux%u, draw buffers 2 x %u B (%u rows), SPI %u Hz",
           static_cast<unsigned>(profile.width),
           static_cast<unsigned>(profile.height),
           static_cast<unsigned>(buffer_bytes), static_cast<unsigned>(rows),
           static_cast<unsigned>(wiring.spi_clock_hz));
  return ESP_OK;
}

bool lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }

void unlock() { lvgl_port_unlock(); }

void logMemory() {
  // Without a registered display the LVGL port may not exist; locking it
  // would hit its assert.
  if (g_display == nullptr) return;
  if (!lock(0)) return;
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
