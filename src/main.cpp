// Firmware entry point. Boot log: version, chip, memory baseline and the
// selected hardware profile. Event bus, settings load and file service
// mount, display bring-up with the boot test screen, an input level log and
// the periodic status log.

#include <array>
#include <atomic>
#include <cstdio>

#include "app_config.h"
#include "boot_screen.h"
#include "display_port.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "event_bus.h"
#include "file_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_target.h"
#include "safe_area.h"
#include "sdkconfig.h"
#include "settings_store.h"

namespace {

constexpr const char* kTag = "boot";

// Bring-up diagnostic: input levels are polled and every change is logged.
// Replaced by the input driver later.
constexpr uint32_t kInputPollMs = 20;
// Time for LVGL to render the boot screen before its memory use is logged.
constexpr uint32_t kFirstRenderWaitMs = 200;

void logHeap(const char* when) {
  // Internal 8-bit capable heap: the budget that matters on boards without
  // PSRAM. "largest block" shows fragmentation, "min free" the low-water mark.
  constexpr uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  ESP_LOGI(kTag, "heap %s: free %u B, largest block %u B, min free %u B", when,
           static_cast<unsigned>(heap_caps_get_free_size(kCaps)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(kCaps)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(kCaps)));
}

void logBootInfo() {
  const esp_app_desc_t* app = esp_app_get_description();
  ESP_LOGI(kTag, "%s %s (ESP-IDF %s)", app->project_name, app->version,
           app->idf_ver);

  esp_chip_info_t chip = {};
  esp_chip_info(&chip);
  ESP_LOGI(kTag, "chip %s rev %u.%u, %u core(s)", CONFIG_IDF_TARGET,
           static_cast<unsigned>(chip.revision / 100),
           static_cast<unsigned>(chip.revision % 100),
           static_cast<unsigned>(chip.cores));
  logHeap("at boot");
}

const char* pinText(int pin, char (&buf)[12]) {
  if (pin == hw::kNoPin) return "-";
  snprintf(buf, sizeof(buf), "%d", pin);
  return buf;
}

void logHardwareProfile() {
  ESP_LOGI(kTag, "target %s: %s + %s", hw::kTarget.name, hw::kBoard.name,
           hw::kDisplay.name);

  const hw::DisplayWiring& w = hw::kTarget.wiring;
  char cs[12], reset[12], bl[12];
  ESP_LOGI(kTag,
           "display %ux%u %s, SPI mode %u @ %u Hz: SCLK %d MOSI %d CS %s "
           "DC %d RST %s BL %s%s",
           static_cast<unsigned>(hw::kDisplay.width),
           static_cast<unsigned>(hw::kDisplay.height),
           hw::kDisplay.shape == hw::DisplayShape::kRound ? "round" : "rect",
           static_cast<unsigned>(hw::kDisplay.spi_mode),
           static_cast<unsigned>(w.spi_clock_hz), w.sclk, w.mosi,
           pinText(w.cs, cs), w.dc, pinText(w.reset, reset),
           pinText(w.backlight, bl),
           w.backlight == hw::kNoPin ? ""
           : w.backlight_active_high ? " (active high)"
                                     : " (active low)");

  if (hw::kBoard.has_antenna_switch) {
    ESP_LOGI(kTag, "antenna switch: enable GPIO%d, select GPIO%d",
             hw::kBoard.antenna_enable_pin, hw::kBoard.antenna_select_pin);
  }

  ESP_LOGI(kTag, "inputs: %u", static_cast<unsigned>(hw::kTarget.inputs.size()));
  for (std::size_t i = 0; i < hw::kTarget.inputs.size(); ++i) {
    const hw::InputPin& in = hw::kTarget.inputs[i];
    ESP_LOGI(kTag, "  input %u: GPIO%d, active %s",
             static_cast<unsigned>(i + 1), in.pin,
             in.active_high ? "high" : "low");
  }
}

void showBootScreen() {
  char detail[32];
  snprintf(detail, sizeof(detail), "%ux%u %s",
           static_cast<unsigned>(hw::kDisplay.width),
           static_cast<unsigned>(hw::kDisplay.height),
           hw::kDisplay.shape == hw::DisplayShape::kRound ? "round" : "rect");
  const ui::BootScreenModel model{
      .title = hw::kTarget.name,
      .detail = detail,
      .version = esp_app_get_description()->version,
  };
  const auto safe_area = geometry::SafeArea::forDisplay(
      hw::kDisplay, cfg::kContentMarginDivisor);
  if (!display::lock(cfg::kBootScreenLockTimeoutMs)) {
    ESP_LOGE(kTag, "boot screen skipped: LVGL lock timeout");
    return;
  }
  const bool rendered = ui::showBootScreen(model, safe_area);
  display::unlock();
  if (!rendered) {
    ESP_LOGE(kTag, "boot screen failed: content did not fit or an LVGL object "
                  "could not be created");
    return;
  }
  ESP_LOGI(kTag,
           "boot screen: expect a red outer ring, a green inner ring, white "
           "text and the version in blue");
}

bool inputActive(const hw::InputPin& in) {
  return gpio_get_level(static_cast<gpio_num_t>(in.pin)) ==
         (in.active_high ? 1 : 0);
}

using InputStates = std::array<bool, hw::kMaxInputs>;

void initInputs(InputStates& active) {
  const auto inputs = hw::kTarget.inputs;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << inputs[i].pin;
    config.mode = GPIO_MODE_INPUT;
    // Keeps an unconnected input at its inactive level.
    config.pull_up_en =
        inputs[i].active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
    config.pull_down_en =
        inputs[i].active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
    active[i] = inputActive(inputs[i]);
    ESP_LOGI(kTag, "input %u (GPIO%d): %s", static_cast<unsigned>(i + 1),
             inputs[i].pin, active[i] ? "active" : "inactive");
  }
}

void logInputChanges(InputStates& active) {
  const auto inputs = hw::kTarget.inputs;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const bool now = inputActive(inputs[i]);
    if (now == active[i]) continue;
    active[i] = now;
    ESP_LOGI(kTag, "input %u (GPIO%d): %s", static_cast<unsigned>(i + 1),
             inputs[i].pin, now ? "pressed" : "released");
  }
}

void logStatus() {
  logHeap("status");
  display::logMemory();
  // ESP-IDF counts task stacks in bytes.
  ESP_LOGI(kTag, "main task stack: min free %u B",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  events::logStatus();
  files::logStatus();
}

struct StatusLogSettings {
  bool enabled;
  TickType_t period;
};

// Set in the event bus task, consumed by the diagnostic loop.
std::atomic<bool> settings_changed{false};

void onSettingsChanged(events::Event, uint32_t, void*) {
  settings_changed.store(true, std::memory_order_release);
}

StatusLogSettings statusLogSettings() {
  const settings::Model model = settings::current();
  return {model.debug_status_log,
          pdMS_TO_TICKS(uint32_t{model.debug_status_interval_s} * 1000)};
}

// Never returns. Input polling is replaced by the input driver, the status
// log moves to the planned debug helper.
void runDiagnosticLoop() {
  InputStates active{};
  initInputs(active);

  // Subscribe before the first read so that no change is missed.
  ESP_ERROR_CHECK(events::subscribe(events::Event::kSettingsChanged,
                                    onSettingsChanged, nullptr));
  StatusLogSettings status = statusLogSettings();
  TickType_t last_status = xTaskGetTickCount();
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(kInputPollMs));
    logInputChanges(active);
    if (settings_changed.exchange(false, std::memory_order_acq_rel)) {
      status = statusLogSettings();
    }
    if (status.enabled && xTaskGetTickCount() - last_status >= status.period) {
      last_status = xTaskGetTickCount();
      logStatus();
    }
  }
}

}  // namespace

extern "C" void app_main() {
  logBootInfo();
  logHardwareProfile();

  // A failure here is a memory budget error at boot: controlled restart.
  ESP_ERROR_CHECK(events::init());
  logHeap("after event bus init");
  events::runDeviceTest();

  settings::init();
  settings::runDeviceTest();
  logHeap("after settings init");

  files::init();
  files::runDeviceTest();
  logHeap("after file service init");

  const esp_err_t err = display::init(hw::kDisplay, hw::kTarget.wiring);
  if (err == ESP_OK) {
    showBootScreen();
    vTaskDelay(pdMS_TO_TICKS(kFirstRenderWaitMs));
    display::logMemory();
  } else {
    ESP_LOGE(kTag, "display not started (%s), running headless",
             esp_err_to_name(err));
  }
  logHeap("after display init");

  runDiagnosticLoop();
}
