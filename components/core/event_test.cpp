// Device test of the event bus, selected by cfg::kEventTest (app_config.h).
// Expected result: docs/ARCHITECTURE.md, "Event bus".

#include <atomic>

#include "app_config.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace events {

namespace {

constexpr const char* kTag = "events_test";

constexpr uint32_t kUiPosts = 20;
constexpr uint32_t kStatePosts = 5;
// The UI subscriber blocks this long per action so that the queue fills.
// Only a test may block in a callback.
constexpr uint32_t kSlowSubscriberMs = 50;
constexpr uint32_t kSettleMs = 2000;

std::atomic<uint32_t> ui_delivered{0};
std::atomic<uint32_t> state_delivered{0};

void onUiAction(Event, uint32_t, void*) {
  ui_delivered.fetch_add(1, std::memory_order_relaxed);
  vTaskDelay(pdMS_TO_TICKS(kSlowSubscriberMs));
}

void onSettingsChanged(Event, uint32_t, void*) {
  state_delivered.fetch_add(1, std::memory_order_relaxed);
}

void runFlood() {
  if (subscribe(Event::kUiAction, onUiAction, nullptr) != ESP_OK ||
      subscribe(Event::kSettingsChanged, onSettingsChanged, nullptr) !=
          ESP_OK) {
    ESP_LOGE(kTag, "FAIL: subscribe");
    return;
  }
  uint32_t ui_dropped = 0;
  uint32_t failures = 0;
  for (uint32_t i = 0; i < kUiPosts; ++i) {
    const esp_err_t err = post(Event::kUiAction, i);
    if (err == ESP_ERR_TIMEOUT) {
      ++ui_dropped;
    } else if (err != ESP_OK) {
      ++failures;
    }
  }
  for (uint32_t i = 0; i < kStatePosts; ++i) {
    if (post(Event::kSettingsChanged) != ESP_OK) ++failures;
  }
  vTaskDelay(pdMS_TO_TICKS(kSettleMs));

  const uint32_t ui = ui_delivered.load(std::memory_order_relaxed);
  const uint32_t state = state_delivered.load(std::memory_order_relaxed);
  // The bus task runs above this one: the first UI action is delivered at
  // once and blocks; the next ones fill the UI slots, the rest are dropped.
  // The settings posts find their reserved slot and coalesce into one.
  const bool pass = failures == 0 && ui + ui_dropped == kUiPosts &&
                    ui <= cfg::kEventUiActionSlots + 1 && ui_dropped > 0 &&
                    state >= 1 && state < kStatePosts;
  ESP_LOGI(kTag,
           "%s: UI actions %u delivered, %u dropped; settings events %u "
           "posted, %u delivered; failed posts %u",
           pass ? "PASS" : "FAIL", static_cast<unsigned>(ui),
           static_cast<unsigned>(ui_dropped),
           static_cast<unsigned>(kStatePosts), static_cast<unsigned>(state),
           static_cast<unsigned>(failures));
  logStatus();
}

}  // namespace

void runDeviceTest() {
  if constexpr (cfg::kEventTest != cfg::EventTest::kNone) {
    ESP_LOGW(kTag, "device test %u active (app_config.h kEventTest)",
             static_cast<unsigned>(cfg::kEventTest));
    runFlood();
  }
}

}  // namespace events
