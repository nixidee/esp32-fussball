#include "event_bus.h"

#include <array>
#include <atomic>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace events {

namespace {

constexpr const char* kTag = "events";
constexpr const char* kTaskName = "events";

ESP_EVENT_DEFINE_BASE(FUSSBALL_EVENTS);

struct Subscriber {
  Event event;
  Callback callback;
  void* context;
};

esp_event_loop_handle_t loop = nullptr;
TaskHandle_t task = nullptr;
Admission admission;

// Entries below subscriber_count never change; subscribe() appends under the
// lock and publishes the new count last.
std::array<Subscriber, cfg::kEventMaxSubscribers> subscribers{};
std::atomic<std::size_t> subscriber_count{0};
portMUX_TYPE subscribe_lock = portMUX_INITIALIZER_UNLOCKED;

std::atomic<uint32_t> post_failures{0};

void dispatch(void*, esp_event_base_t, int32_t id, void*) {
  if (id < 0 || static_cast<std::size_t>(id) >= kEventCount) return;
  const auto event = static_cast<Event>(id);
  admission.release(event);

  const std::size_t count = subscriber_count.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count; ++i) {
    const Subscriber& s = subscribers[i];
    if (s.event == event) s.callback(event, s.context);
  }
}

}  // namespace

esp_err_t init() {
  const esp_event_loop_args_t args = {
      .queue_size = static_cast<int32_t>(kQueueLength),
      .task_name = kTaskName,
      .task_priority = cfg::kEventTaskPriority,
      .task_stack_size = cfg::kEventTaskStackBytes,
      .task_core_id = tskNO_AFFINITY,
  };
  esp_event_loop_handle_t created = nullptr;
  esp_err_t err = esp_event_loop_create(&args, &created);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "loop not created: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_event_handler_register_with(created, FUSSBALL_EVENTS,
                                        ESP_EVENT_ANY_ID, dispatch, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "dispatcher not registered: %s", esp_err_to_name(err));
    esp_event_loop_delete(created);
    return err;
  }
  task = xTaskGetHandle(kTaskName);
  loop = created;
  ESP_LOGI(kTag, "ready: queue %u state entries, task stack %u B",
           static_cast<unsigned>(kQueueLength),
           static_cast<unsigned>(cfg::kEventTaskStackBytes));
  return ESP_OK;
}

esp_err_t subscribe(Event event, Callback callback, void* context) {
  if (callback == nullptr || !isStateEvent(event)) return ESP_ERR_INVALID_ARG;
  esp_err_t err = ESP_ERR_NO_MEM;
  taskENTER_CRITICAL(&subscribe_lock);
  const std::size_t count = subscriber_count.load(std::memory_order_relaxed);
  if (count < subscribers.size()) {
    subscribers[count] = {event, callback, context};
    subscriber_count.store(count + 1, std::memory_order_release);
    err = ESP_OK;
  }
  taskEXIT_CRITICAL(&subscribe_lock);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "subscriber table full (%u), %s not subscribed",
             static_cast<unsigned>(subscribers.size()), eventName(event));
  }
  return err;
}

esp_err_t post(Event event) {
  if (!isStateEvent(event)) return ESP_ERR_INVALID_ARG;
  if (loop == nullptr) return ESP_ERR_INVALID_STATE;
  if (!admission.admit(event)) return ESP_OK;
  const esp_err_t err = esp_event_post_to(
      loop, FUSSBALL_EVENTS, static_cast<int32_t>(event), nullptr, 0, 0);
  if (err != ESP_OK) {
    // Cannot happen with the reserved queue slots; counted if it does.
    admission.release(event);
    post_failures.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGE(kTag, "%s lost: %s", eventName(event), esp_err_to_name(err));
  }
  return err;
}

void logStatus() {
  ESP_LOGI(
      kTag, "task stack min free %u B, failed posts %u",
      task != nullptr ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(task))
                      : 0U,
      static_cast<unsigned>(post_failures.load(std::memory_order_relaxed)));
}

}  // namespace events
