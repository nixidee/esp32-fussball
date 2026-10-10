#include "health_service.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "display_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "file_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings_store.h"
#include "time_service.h"

namespace health {
namespace {

constexpr const char* kTag = "health";
constexpr uint32_t kHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static_assert(sizeof(StackType_t) == 1);  // ESP-IDF watermarks count bytes.
static_assert(configMAX_TASK_NAME_LEN <= cfg::kHealthTaskNameBytes);

Point sampleHeap(void*) noexcept {
  multi_heap_info_t info{};
  heap_caps_get_info(&info, kHeapCaps);
  return {esp_timer_get_time() / 1000,
          static_cast<uint32_t>(info.total_free_bytes),
          static_cast<uint32_t>(info.largest_free_block),
          static_cast<uint32_t>(info.minimum_free_bytes)};
}

int32_t startHeapInterval(void*) noexcept {
  return heap_caps_monitor_local_minimum_free_size_start();
}

int32_t stopHeapInterval(void*) noexcept {
  return heap_caps_monitor_local_minimum_free_size_stop();
}

Coordinator coordinator{
    {nullptr, sampleHeap, startHeapInterval, stopHeapInterval}};
std::array<TaskStatus_t, cfg::kHealthMaxTasks> raw_tasks;
std::array<TaskRow, cfg::kHealthMaxTasks> task_rows;
TaskHandle_t app_task = nullptr;
esp_task_wdt_user_handle_t app_watchdog = nullptr;
ConsoleStatus console{};
int64_t next_report_ms = 0;

void requireAppTask() {
  configASSERT(app_task != nullptr && xTaskGetCurrentTaskHandle() == app_task);
}

void logHeap() {
  const Observation heap = observeHeap();
  const char* scope = heap.scope == MinimumScope::kLifetime   ? "lifetime"
                      : heap.scope == MinimumScope::kInterval ? "interval"
                                                              : "unavailable";
  ESP_LOGI(kTag,
           "heap: free %u B, largest %u B, min %u B (%s; sum of region "
           "minima), interval start %lld ms",
           static_cast<unsigned>(heap.point.free_bytes),
           static_cast<unsigned>(heap.point.largest_bytes),
           static_cast<unsigned>(heap.point.minimum_bytes), scope,
           static_cast<long long>(heap.interval_start_ms));
}

}  // namespace

void init() {
  app_task = xTaskGetCurrentTaskHandle();
  applySettings();
  ESP_LOGI(kTag, "initialized: task capacity %u, no dedicated task",
           static_cast<unsigned>(cfg::kHealthMaxTasks));
}

void applySettings() {
  requireAppTask();
  const settings::Model model = settings::current();
  configureConsole(model.debug_status_log, model.debug_status_interval_s);
}

void configureConsole(bool enabled, uint16_t interval_s) {
  requireAppTask();
  interval_s = std::clamp(interval_s, cfg::kDebugStatusIntervalMinS,
                          cfg::kDebugStatusIntervalMaxS);
  if (console.enabled == enabled && console.interval_s == interval_s) return;
  console.enabled = enabled;
  console.interval_s = interval_s;
  next_report_ms = esp_timer_get_time() / 1000 + int64_t{interval_s} * 1000;
  ESP_LOGI(kTag, "console status %s, interval %u s", enabled ? "on" : "off",
           static_cast<unsigned>(interval_s));
}

ConsoleStatus consoleStatus() {
  requireAppTask();
  return console;
}

esp_err_t startLoopWatchdog() {
  requireAppTask();
  if (app_watchdog != nullptr) return ESP_OK;
  const esp_err_t err = esp_task_wdt_add_user("app_loop", &app_watchdog);
  if (err == ESP_OK) {
    ESP_LOGI(kTag, "app-loop watchdog active (%u s), independent of console",
             static_cast<unsigned>(CONFIG_ESP_TASK_WDT_TIMEOUT_S));
  }
  return err;
}

void poll() {
  requireAppTask();
  const int64_t now = esp_timer_get_time() / 1000;
  if (console.enabled && now >= next_report_ms) {
    next_report_ms = now + int64_t{console.interval_s} * 1000;
    logStatus();
  }
  // A separate timer must not mask a blocked app loop or its status work.
  if (app_watchdog != nullptr) {
    ESP_ERROR_CHECK(esp_task_wdt_reset_user(app_watchdog));
  }
}

TaskSnapshot taskSnapshot(std::size_t capacity) {
  requireAppTask();
  task_rows.fill({});
  capacity = std::min(capacity, raw_tasks.size());
  // On the C6 this also protects name lifetime until copies are complete.
  // On SMP, names stay empty; use copied numeric IDs instead of dereferencing
  // a TCB that another core could delete after uxTaskGetSystemState returns.
#if configNUMBER_OF_CORES == 1
  vTaskSuspendAll();
#endif
  const int64_t start_us = esp_timer_get_time();
  const uint32_t total = uxTaskGetNumberOfTasks();
  const std::size_t count =
      uxTaskGetSystemState(raw_tasks.data(), capacity, nullptr);
  const bool complete = count != 0 && count == total;
  if (complete) {
    for (std::size_t i = 0; i < count; ++i) {
      TaskRow& row = task_rows[i];
      row.id = raw_tasks[i].xTaskNumber;
      row.min_free_bytes = raw_tasks[i].usStackHighWaterMark;
#if configNUMBER_OF_CORES == 1
      const char* name = raw_tasks[i].pcTaskName;
      if (name != nullptr) {
        const std::size_t length = strnlen(name, sizeof(row.name) - 1);
        memcpy(row.name, name, length);
      }
#endif
    }
  }
  const uint32_t pause_us =
      static_cast<uint32_t>(esp_timer_get_time() - start_us);
#if configNUMBER_OF_CORES == 1
  xTaskResumeAll();
#endif
  return {task_rows.data(), complete ? count : 0, total, pause_us, complete};
}

void logStatus() {
  requireAppTask();
  if (!console.enabled) return;
  const int64_t start_us = esp_timer_get_time();
  logHeap();
  const TaskSnapshot tasks = taskSnapshot();
  if (!tasks.complete) {
    ESP_LOGW(kTag, "task snapshot unavailable: %u tasks, capacity %u",
             static_cast<unsigned>(tasks.total_tasks),
             static_cast<unsigned>(cfg::kHealthMaxTasks));
  } else {
    ESP_LOGI(kTag, "tasks %u, snapshot pause %u us",
             static_cast<unsigned>(tasks.count),
             static_cast<unsigned>(tasks.pause_us));
    for (std::size_t i = 0; i < tasks.count; ++i) {
      const TaskRow& row = tasks.rows[i];
      ESP_LOGI(kTag, "task %s #%u: stack min free %u B", row.name,
               static_cast<unsigned>(row.id),
               static_cast<unsigned>(row.min_free_bytes));
    }
  }
  display::logMemory();
  events::logStatus();
  files::logStatus();
  timekeeping::logStatus();
  const uint32_t payload_us =
      static_cast<uint32_t>(esp_timer_get_time() - start_us);
  ++console.reports;
  ESP_LOGI(kTag,
           "status report %u: payload %u us, previous complete %u us, "
           "complete max %u us, last reset %s",
           static_cast<unsigned>(console.reports),
           static_cast<unsigned>(payload_us),
           static_cast<unsigned>(console.last_report_us),
           static_cast<unsigned>(console.max_report_us), resetReason());
  // Include the final log write in the measured complete report.
  console.last_report_us =
      static_cast<uint32_t>(esp_timer_get_time() - start_us);
  console.max_report_us =
      std::max(console.max_report_us, console.last_report_us);
}

const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "unknown";
  }
}

Coordinator& heapCoordinator() { return coordinator; }

Observation observeHeap() { return coordinator.observe(); }

}  // namespace health
