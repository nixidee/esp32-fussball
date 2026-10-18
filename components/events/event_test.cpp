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

constexpr uint32_t kStatePosts = 20;
constexpr uint32_t kSlowSubscriberMs = 50;
constexpr uint32_t kSettleMs = 2000;
std::atomic<uint32_t> state_delivered{0};

void onSettingsChanged(Event, void*) {
  state_delivered.fetch_add(1, std::memory_order_relaxed);
  vTaskDelay(pdMS_TO_TICKS(kSlowSubscriberMs));
}

void runFlood() {
  if (subscribe(Event::kSettingsChanged, onSettingsChanged, nullptr) !=
      ESP_OK) {
    ESP_LOGE(kTag, "FAIL: subscribe");
    return;
  }
  uint32_t failures = 0;
  for (uint32_t i = 0; i < kStatePosts; ++i) {
    if (post(Event::kSettingsChanged) != ESP_OK) ++failures;
  }
  vTaskDelay(pdMS_TO_TICKS(kSettleMs));

  const uint32_t state = state_delivered.load(std::memory_order_relaxed);
  const bool pass = failures == 0 && state >= 1 && state < kStatePosts;
  ESP_LOGI(kTag, "%s: settings events %u posted, %u delivered; failed posts %u",
           pass ? "PASS" : "FAIL", static_cast<unsigned>(kStatePosts),
           static_cast<unsigned>(state), static_cast<unsigned>(failures));
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
