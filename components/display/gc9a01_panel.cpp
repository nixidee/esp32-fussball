// GC9A01 panel driver for esp_lcd. See gc9a01_panel.h.

#include "gc9a01_panel.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace display {
namespace {

constexpr const char* kTag = "gc9a01";

// Reset timing as in the earlier driver (see gc9a01_panel.h).
constexpr uint32_t kResetPulseMs = 20;
constexpr uint32_t kAfterResetMs = 120;
// Wait after SLPOUT before the next command.
constexpr uint32_t kAfterSleepOutMs = 120;

constexpr uint8_t kBytesPerPixel = 2;  // RGB565

struct Panel {
  esp_lcd_panel_t base;  // must stay the first member (toPanel)
  esp_lcd_panel_io_handle_t io;
  int reset_gpio;
  int x_gap;
  int y_gap;
  uint8_t madctl;  // last value written to MADCTL
};

static_assert(std::is_standard_layout_v<Panel> && offsetof(Panel, base) == 0,
              "toPanel() needs base as the first member");

Panel* toPanel(esp_lcd_panel_t* panel) {
  return reinterpret_cast<Panel*>(panel);
}

struct InitCommand {
  uint8_t command;
  uint8_t size;
  uint8_t data[12];
};

// Vendor, power, gamma and pixel format registers, in this order. Values from
// the earlier driver (verified on hardware); not documented further by the
// vendor. SLPOUT, MADCTL, INVON/INVOFF and DISPON follow separately.
constexpr InitCommand kInitCommands[] = {
    {0xEF, 0, {}},
    {0xEB, 1, {0x14}},
    {0xFE, 0, {}},
    {0xEF, 0, {}},
    {0xEB, 1, {0x14}},
    {0x84, 1, {0x40}},
    {0x85, 1, {0xFF}},
    {0x86, 1, {0xFF}},
    {0x87, 1, {0xFF}},
    {0x88, 1, {0x0A}},
    {0x89, 1, {0x21}},
    {0x8A, 1, {0x00}},
    {0x8B, 1, {0x80}},
    {0x8C, 1, {0x01}},
    {0x8D, 1, {0x01}},
    {0x8E, 1, {0xFF}},
    {0x8F, 1, {0xFF}},
    {0xB6, 2, {0x00, 0x20}},
    // 16 bit per pixel on the SPI interface (0x05 and 0x55 both select it;
    // 0x05 is the value of the earlier driver).
    {LCD_CMD_COLMOD, 1, {0x05}},
    {0x90, 4, {0x08, 0x08, 0x08, 0x08}},
    {0xBD, 1, {0x06}},
    {0xBC, 1, {0x00}},
    {0xFF, 3, {0x60, 0x01, 0x04}},
    {0xC3, 1, {0x13}},
    {0xC4, 1, {0x13}},
    {0xC9, 1, {0x22}},
    {0xBE, 1, {0x11}},
    {0xE1, 2, {0x10, 0x0E}},
    {0xDF, 3, {0x21, 0x0C, 0x02}},
    {0xF0, 6, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}},
    {0xF1, 6, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}},
    {0xF2, 6, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}},
    {0xF3, 6, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}},
    {0xED, 2, {0x1B, 0x0B}},
    {0xAE, 1, {0x77}},
    {0xCD, 1, {0x63}},
    {0x70, 9, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}},
    {0xE8, 1, {0x34}},
    {0x62, 12,
     {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}},
    {0x63, 12,
     {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}},
    {0x64, 7, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}},
    {0x66, 10, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}},
    {0x67, 10, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}},
    {0x74, 7, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}},
    {0x98, 2, {0x3E, 0x07}},
    {LCD_CMD_TEON, 0, {}},  // 0x35, tearing effect line on
};

esp_err_t txCommand(Panel* p, uint8_t command, const uint8_t* data = nullptr,
                    std::size_t size = 0) {
  return esp_lcd_panel_io_tx_param(p->io, command, data, size);
}

esp_err_t writeMadctl(Panel* p) {
  return txCommand(p, LCD_CMD_MADCTL, &p->madctl, 1);
}

void setBit(uint8_t& value, uint8_t bit, bool on) {
  value = on ? static_cast<uint8_t>(value | bit)
             : static_cast<uint8_t>(value & ~bit);
}

esp_err_t panelDel(esp_lcd_panel_t* panel) {
  Panel* p = toPanel(panel);
  if (p->reset_gpio >= 0) gpio_reset_pin(static_cast<gpio_num_t>(p->reset_gpio));
  delete p;
  return ESP_OK;
}

esp_err_t panelReset(esp_lcd_panel_t* panel) {
  Panel* p = toPanel(panel);
  if (p->reset_gpio >= 0) {
    const auto pin = static_cast<gpio_num_t>(p->reset_gpio);
    gpio_set_level(pin, 0);  // reset is active low
    vTaskDelay(pdMS_TO_TICKS(kResetPulseMs));
    gpio_set_level(pin, 1);
  } else {
    ESP_RETURN_ON_ERROR(txCommand(p, LCD_CMD_SWRESET), kTag, "SWRESET failed");
  }
  vTaskDelay(pdMS_TO_TICKS(kAfterResetMs));
  return ESP_OK;
}

esp_err_t panelInit(esp_lcd_panel_t* panel) {
  Panel* p = toPanel(panel);
  for (const InitCommand& c : kInitCommands) {
    ESP_RETURN_ON_ERROR(txCommand(p, c.command, c.data, c.size), kTag,
                        "init command 0x%02X failed", c.command);
  }
  ESP_RETURN_ON_ERROR(txCommand(p, LCD_CMD_SLPOUT), kTag, "SLPOUT failed");
  vTaskDelay(pdMS_TO_TICKS(kAfterSleepOutMs));
  return writeMadctl(p);
}

esp_err_t panelDrawBitmap(esp_lcd_panel_t* panel, int x_start, int y_start,
                          int x_end, int y_end, const void* color_data) {
  Panel* p = toPanel(panel);
  x_start += p->x_gap;
  x_end += p->x_gap;
  y_start += p->y_gap;
  y_end += p->y_gap;

  // End coordinates are exclusive in esp_lcd, inclusive on the controller.
  const uint8_t columns[] = {
      static_cast<uint8_t>(x_start >> 8), static_cast<uint8_t>(x_start),
      static_cast<uint8_t>((x_end - 1) >> 8), static_cast<uint8_t>(x_end - 1)};
  const uint8_t rows[] = {
      static_cast<uint8_t>(y_start >> 8), static_cast<uint8_t>(y_start),
      static_cast<uint8_t>((y_end - 1) >> 8), static_cast<uint8_t>(y_end - 1)};
  ESP_RETURN_ON_ERROR(txCommand(p, LCD_CMD_CASET, columns, sizeof(columns)),
                      kTag, "CASET failed");
  ESP_RETURN_ON_ERROR(txCommand(p, LCD_CMD_RASET, rows, sizeof(rows)), kTag,
                      "RASET failed");

  const std::size_t size = static_cast<std::size_t>(x_end - x_start) *
                           static_cast<std::size_t>(y_end - y_start) *
                           kBytesPerPixel;
  return esp_lcd_panel_io_tx_color(p->io, LCD_CMD_RAMWR, color_data, size);
}

esp_err_t panelInvertColor(esp_lcd_panel_t* panel, bool invert) {
  return txCommand(toPanel(panel), invert ? LCD_CMD_INVON : LCD_CMD_INVOFF);
}

esp_err_t panelMirror(esp_lcd_panel_t* panel, bool mirror_x, bool mirror_y) {
  Panel* p = toPanel(panel);
  setBit(p->madctl, LCD_CMD_MX_BIT, mirror_x);
  setBit(p->madctl, LCD_CMD_MY_BIT, mirror_y);
  return writeMadctl(p);
}

esp_err_t panelSwapXy(esp_lcd_panel_t* panel, bool swap_axes) {
  Panel* p = toPanel(panel);
  setBit(p->madctl, LCD_CMD_MV_BIT, swap_axes);
  return writeMadctl(p);
}

esp_err_t panelSetGap(esp_lcd_panel_t* panel, int x_gap, int y_gap) {
  Panel* p = toPanel(panel);
  p->x_gap = x_gap;
  p->y_gap = y_gap;
  return ESP_OK;
}

esp_err_t panelDispOnOff(esp_lcd_panel_t* panel, bool on) {
  return txCommand(toPanel(panel), on ? LCD_CMD_DISPON : LCD_CMD_DISPOFF);
}

esp_err_t panelSleep(esp_lcd_panel_t* panel, bool sleep) {
  Panel* p = toPanel(panel);
  ESP_RETURN_ON_ERROR(txCommand(p, sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT),
                      kTag, "sleep command failed");
  if (!sleep) vTaskDelay(pdMS_TO_TICKS(kAfterSleepOutMs));
  return ESP_OK;
}

}  // namespace

esp_err_t newGc9a01Panel(esp_lcd_panel_io_handle_t io,
                         const Gc9a01Config& config,
                         esp_lcd_panel_handle_t* ret_panel) {
  ESP_RETURN_ON_FALSE(io != nullptr && ret_panel != nullptr,
                      ESP_ERR_INVALID_ARG, kTag, "invalid argument");

  if (config.reset_gpio >= 0) {
    gpio_config_t reset_config = {};
    reset_config.pin_bit_mask = 1ULL << config.reset_gpio;
    reset_config.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&reset_config), kTag,
                        "reset GPIO config failed");
  }

  auto* p = new (std::nothrow) Panel{};
  if (p == nullptr) {
    if (config.reset_gpio >= 0) {
      gpio_reset_pin(static_cast<gpio_num_t>(config.reset_gpio));
    }
    return ESP_ERR_NO_MEM;
  }
  p->io = io;
  p->reset_gpio = config.reset_gpio;
  p->madctl = config.bgr_order ? LCD_CMD_BGR_BIT : 0;
  p->base.del = panelDel;
  p->base.reset = panelReset;
  p->base.init = panelInit;
  p->base.draw_bitmap = panelDrawBitmap;
  p->base.mirror = panelMirror;
  p->base.swap_xy = panelSwapXy;
  p->base.set_gap = panelSetGap;
  p->base.invert_color = panelInvertColor;
  p->base.disp_on_off = panelDispOnOff;
  p->base.disp_sleep = panelSleep;
  *ret_panel = &p->base;
  return ESP_OK;
}

}  // namespace display
