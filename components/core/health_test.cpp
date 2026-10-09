// Bounded health diagnostics. Select cfg::kHealthTest; normal is kNone.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "display_port.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health_service.h"

namespace {

constexpr const char* kTag = "health_test";
constexpr uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr std::size_t kTransientBytes = 4096;
constexpr uint32_t kPollMs = 20;
constexpr uint32_t kEnabledWaitMs = 5200;
constexpr uint32_t kDisabledWaitMs = 6100;
constexpr uint32_t kMeterRepeatCount = 32;
std::atomic<uint32_t> fail_monitor_allocation{0};
std::atomic<uint32_t> lock_worker_state{0};
std::atomic<uint32_t> release_lock_worker{0};
StaticTask_t lock_worker_tcb;
StackType_t lock_worker_stack[cfg::kHealthTestTaskStackBytes];
std::array<TaskHandle_t, cfg::kHealthMaxTasks> probe_workers{};
std::atomic<uint32_t> probe_workers_done{0};
constinit std::atomic<vprintf_like_t> original_vprintf{&vprintf};
std::atomic<TaskHandle_t> slow_console_task{nullptr};
std::atomic<uint32_t> slow_console_lines{0};

// The current IDF log-v1 configuration emits each ESP_LOG line in one call.
// Delay only the app task's report. Other tasks and ISRs are never delayed.
int simulateSlowConsole(const char* format, va_list arguments) {
  const vprintf_like_t original =
      original_vprintf.load(std::memory_order_acquire);
  if (!xPortInIsrContext() &&
      xTaskGetCurrentTaskHandle() ==
          slow_console_task.load(std::memory_order_acquire)) {
    slow_console_lines.fetch_add(1, std::memory_order_relaxed);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return original(format, arguments);
}

void waitForProbeCleanup(void*) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  probe_workers_done.fetch_add(1, std::memory_order_release);
  vTaskDelete(nullptr);
}

bool createProbe(std::size_t index) {
  char name[cfg::kHealthTaskNameBytes];
  snprintf(name, sizeof(name), "health_probe%u", static_cast<unsigned>(index));
  return xTaskCreate(waitForProbeCleanup, name, cfg::kHealthTestTaskStackBytes,
                     nullptr, uxTaskPriorityGet(nullptr) + 1,
                     &probe_workers[index]) == pdPASS;
}

bool check(bool pass, const char* what) {
  ESP_LOGI(kTag, "%s: %s", pass ? "PASS" : "FAIL", what);
  return pass;
}

void pollFor(uint32_t duration_ms) {
  const TickType_t start = xTaskGetTickCount();
  while (xTaskGetTickCount() - start < pdMS_TO_TICKS(duration_ms)) {
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
    health::poll();
  }
}

void holdDisplayLock(void*) {
  const bool locked = display::lock(100);
  lock_worker_state.store(locked ? 1 : 2, std::memory_order_release);
  if (locked) {
    const TickType_t start = xTaskGetTickCount();
    while (release_lock_worker.load(std::memory_order_acquire) == 0 &&
           xTaskGetTickCount() - start < pdMS_TO_TICKS(3000)) {
      vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
    display::unlock();
  }
  lock_worker_state.store(3, std::memory_order_release);
  vTaskDelete(nullptr);
}

bool testBusyDisplay() {
  uint32_t slow_report_us = 0;
  lock_worker_state.store(0, std::memory_order_relaxed);
  release_lock_worker.store(0, std::memory_order_relaxed);
  const TaskHandle_t worker = xTaskCreateStatic(
      holdDisplayLock, "health_lock", cfg::kHealthTestTaskStackBytes, nullptr,
      uxTaskPriorityGet(nullptr) + 1, lock_worker_stack, &lock_worker_tcb);
  const TickType_t start = xTaskGetTickCount();
  while (worker != nullptr &&
         lock_worker_state.load(std::memory_order_acquire) == 0 &&
         xTaskGetTickCount() - start < pdMS_TO_TICKS(500)) {
    pollFor(kPollMs);
  }
  const bool locked = lock_worker_state.load(std::memory_order_acquire) == 1;
  if (locked) {
    ESP_LOGI(kTag, "SIMULATED_SLOW_CONSOLE: 50 ms per app report line");
    slow_console_lines.store(0, std::memory_order_relaxed);
    slow_console_task.store(xTaskGetCurrentTaskHandle(),
                            std::memory_order_release);
    // Other tasks cannot use the hook before its previous target is saved.
    vTaskSuspendAll();
    const vprintf_like_t previous = esp_log_set_vprintf(simulateSlowConsole);
    original_vprintf.store(previous, std::memory_order_release);
    xTaskResumeAll();
    health::logStatus();
    esp_log_set_vprintf(previous);  // Restore before any subsequent log.
    slow_console_task.store(nullptr, std::memory_order_release);
    slow_report_us = health::consoleStatus().last_report_us;
  }
  const bool stayed_locked =
      lock_worker_state.load(std::memory_order_acquire) == 1;
  release_lock_worker.store(1, std::memory_order_release);
  const TickType_t release_start = xTaskGetTickCount();
  while (worker != nullptr &&
         lock_worker_state.load(std::memory_order_acquire) != 3 &&
         xTaskGetTickCount() - release_start < pdMS_TO_TICKS(500)) {
    pollFor(kPollMs);
  }
  ESP_LOGI(
      kTag, "SIMULATED_SLOW_CONSOLE: %u lines, complete report %u us",
      static_cast<unsigned>(slow_console_lines.load(std::memory_order_relaxed)),
      static_cast<unsigned>(slow_report_us));
  // The test worker has fixed storage and a 3 s safety deadline. It exists
  // only in diagnostic builds; normal firmware never creates this task.
  return check(
      locked && stayed_locked &&
          slow_console_lines.load(std::memory_order_relaxed) > 0 &&
          lock_worker_state.load(std::memory_order_acquire) == 3 &&
          slow_report_us < CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000000U,
      "SIMULATED_SLOW_CONSOLE: busy-LVGL report skips lock; worker releases; "
      "below watchdog timeout");
}

bool testSnapshots() {
  const health::TaskSnapshot initial = health::taskSnapshot();
  bool main = false, events = false, lvgl = false, idle = false;
  for (std::size_t i = 0; i < initial.count; ++i) {
    const health::TaskRow& row = initial.rows[i];
    main |= strcmp(row.name, "main") == 0;
    events |= strcmp(row.name, "events") == 0;
    lvgl |= strcmp(row.name, "taskLVGL") == 0;
    idle |= strncmp(row.name, "IDLE", 4) == 0;
  }
  bool pass = check(initial.complete && main && events && lvgl && idle,
                    "owned task names and stack minima (C6)");
  ESP_LOGI(kTag, "task snapshot %u tasks, pause %u us",
           static_cast<unsigned>(initial.count),
           static_cast<unsigned>(initial.pause_us));
  const health::TaskSnapshot overflow = health::taskSnapshot(1);
  bool cleared = true;
  for (std::size_t i = 0; i < cfg::kHealthMaxTasks; ++i) {
    cleared &= overflow.rows[i].id == 0 &&
               overflow.rows[i].min_free_bytes == 0 &&
               overflow.rows[i].name[0] == '\0';
  }
  pass &= check(!overflow.complete && overflow.count == 0 &&
                    overflow.total_tasks > 1 && cleared,
                "capacity overflow has no stale/partial rows");
  pass &=
      check(health::taskSnapshot().complete, "full-capacity snapshot recovers");

  // Exercise the real configured capacity, not only a smaller API limit.
  if (!initial.complete || initial.total_tasks == 0 ||
      initial.total_tasks > cfg::kHealthMaxTasks) {
    return check(false, "baseline permits bounded 16/17-task probes") && pass;
  }
  probe_workers.fill(nullptr);
  probe_workers_done.store(0, std::memory_order_relaxed);
  const std::size_t required = cfg::kHealthMaxTasks - initial.total_tasks;
  const std::size_t free_before = heap_caps_get_free_size(kCaps);
  std::size_t created = 0;
  while (created < required && createProbe(created)) ++created;
  const health::TaskSnapshot full = health::taskSnapshot();
  std::size_t named_probes = 0;
  for (std::size_t i = 0; i < full.count; ++i) {
    named_probes += strncmp(full.rows[i].name, "health_probe", 12) == 0;
  }
  const bool full_capacity = created == required && full.complete &&
                             full.count == cfg::kHealthMaxTasks &&
                             full.total_tasks == cfg::kHealthMaxTasks &&
                             named_probes == required;
  pass &= check(full_capacity, "real 16-task snapshot is complete");
  ESP_LOGI(kTag, "full-capacity snapshot %u tasks, pause %u us",
           static_cast<unsigned>(full.count),
           static_cast<unsigned>(full.pause_us));
  if (full_capacity) health::logStatus();

  const bool extra_created = full_capacity && createProbe(created);
  if (extra_created) ++created;
  const health::TaskSnapshot real_overflow = health::taskSnapshot();
  cleared = true;
  for (std::size_t i = 0; i < cfg::kHealthMaxTasks; ++i) {
    cleared &= real_overflow.rows[i].id == 0 &&
               real_overflow.rows[i].min_free_bytes == 0 &&
               real_overflow.rows[i].name[0] == '\0';
  }
  pass &= check(
      extra_created && !real_overflow.complete && real_overflow.count == 0 &&
          real_overflow.total_tasks == cfg::kHealthMaxTasks + 1 && cleared,
      "real 17-task overflow has no stale/partial rows");
  const std::size_t free_at_peak = heap_caps_get_free_size(kCaps);
  for (std::size_t i = 0; i < created; ++i) xTaskNotifyGive(probe_workers[i]);
  const TickType_t cleanup_start = xTaskGetTickCount();
  health::TaskSnapshot recovered = health::taskSnapshot();
  while ((!recovered.complete || recovered.total_tasks != initial.total_tasks ||
          probe_workers_done.load(std::memory_order_acquire) != created) &&
         xTaskGetTickCount() - cleanup_start < pdMS_TO_TICKS(500)) {
    // Let both the notified workers and idle's deferred task cleanup run.
    pollFor(kPollMs);
    recovered = health::taskSnapshot();
  }
  pass &=
      check(recovered.complete && recovered.count == initial.count &&
                recovered.total_tasks == initial.total_tasks &&
                probe_workers_done.load(std::memory_order_acquire) == created,
            "all probe workers deleted; baseline snapshot recovers");
  ESP_LOGI(kTag,
           "probe workers %u; heap free before %u B, at peak %u B, "
           "after cleanup %u B; recovered tasks %u",
           static_cast<unsigned>(created), static_cast<unsigned>(free_before),
           static_cast<unsigned>(free_at_peak),
           static_cast<unsigned>(heap_caps_get_free_size(kCaps)),
           static_cast<unsigned>(recovered.count));
  probe_workers.fill(nullptr);
  return pass;
}

bool testAllocationFailure() {
  fail_monitor_allocation.store(1, std::memory_order_relaxed);
  health::Meter failed(health::heapCoordinator());
  const health::MeterResult& result = failed.finish();
  bool pass =
      check(!result.owned && result.start_error == ESP_ERR_NO_MEM &&
                result.scope == health::MinimumScope::kUnavailable &&
                fail_monitor_allocation.load(std::memory_order_relaxed) == 0 &&
                health::observeHeap().scope == health::MinimumScope::kLifetime,
            "monitor allocation failure consumed; no panic; gate released");
  health::Meter retry(health::heapCoordinator());
  const health::MeterResult& recovered = retry.finish();
  pass &=
      check(recovered.owned && recovered.start_error == ESP_OK &&
                recovered.stop_error == ESP_OK &&
                recovered.scope == health::MinimumScope::kInterval &&
                health::observeHeap().scope == health::MinimumScope::kLifetime,
            "monitor lock and allocation recover on next start/stop");
  return pass;
}

bool testMeters() {
  health::Meter owner(health::heapCoordinator());
  void* transient = heap_caps_malloc(kTransientBytes, kCaps);
  const health::Observation allocated = health::observeHeap();
  health::Meter loser(health::heapCoordinator());
  const health::MeterResult& lost = loser.finish();
  bool pass =
      check(transient != nullptr && !lost.owned &&
                lost.scope == health::MinimumScope::kUnavailable &&
                health::observeHeap().scope == health::MinimumScope::kInterval,
            "overlap loser leaves owner's interval active");
  health::logStatus();  // Must label the real global interval, not lifetime.
  heap_caps_free(transient);
  const health::MeterResult& finished = owner.finish();
  pass &=
      check(finished.owned && finished.start_error == ESP_OK &&
                finished.stop_error == ESP_OK &&
                finished.scope == health::MinimumScope::kInterval &&
                allocated.scope == health::MinimumScope::kInterval &&
                finished.end.minimum_bytes <= allocated.point.minimum_bytes &&
                allocated.point.minimum_bytes <= allocated.point.free_bytes &&
                health::observeHeap().scope == health::MinimumScope::kLifetime,
            "real interval minimum sampled before stop; lifetime restored");
  const int64_t end_ms = finished.end.monotonic_ms;
  pass &= check(owner.finish().end.monotonic_ms == end_ms,
                "repeated finish does not restart/stop monitor");

  const health::Observation before_repeats = health::observeHeap();
  uint32_t repeat_failures = 0;
  for (uint32_t i = 0; i < kMeterRepeatCount; ++i) {
    health::Meter repeated(health::heapCoordinator());
    const health::MeterResult& result = repeated.finish();
    if (!result.owned || result.start_error != ESP_OK ||
        result.stop_error != ESP_OK ||
        result.scope != health::MinimumScope::kInterval ||
        health::observeHeap().scope != health::MinimumScope::kLifetime) {
      ++repeat_failures;
      ESP_LOGE(kTag, "repeated meter cycle %u failed",
               static_cast<unsigned>(i + 1));
    }
  }
  const health::Observation after_repeats = health::observeHeap();
  pass &= check(repeat_failures == 0 &&
                    before_repeats.scope == health::MinimumScope::kLifetime &&
                    after_repeats.scope == health::MinimumScope::kLifetime,
                "32 sequential meters own/start/finish; lifetime restored");
  // These points include other tasks' allocations. Record the measured delta;
  // do not assume a quiescent system or require exact global heap equality.
  ESP_LOGI(kTag,
           "meter repeats %u: before %lld ms free %u B/largest %u B; "
           "after %lld ms free %u B/largest %u B; failures %u",
           static_cast<unsigned>(kMeterRepeatCount),
           static_cast<long long>(before_repeats.point.monotonic_ms),
           static_cast<unsigned>(before_repeats.point.free_bytes),
           static_cast<unsigned>(before_repeats.point.largest_bytes),
           static_cast<long long>(after_repeats.point.monotonic_ms),
           static_cast<unsigned>(after_repeats.point.free_bytes),
           static_cast<unsigned>(after_repeats.point.largest_bytes),
           static_cast<unsigned>(repeat_failures));
  return pass;
}

bool testConsoleAndWatchdog() {
  ESP_ERROR_CHECK(health::startLoopWatchdog());
  health::configureConsole(false, cfg::kDebugStatusIntervalMinS);
  health::configureConsole(true, cfg::kDebugStatusIntervalMinS);
  const uint32_t before = health::consoleStatus().reports;
  pollFor(kEnabledWaitMs);
  bool pass = check(health::consoleStatus().reports == before + 1,
                    "console on: one periodic report");
  health::configureConsole(false, cfg::kDebugStatusIntervalMinS);
  const uint32_t disabled = health::consoleStatus().reports;
  pollFor(kDisabledWaitMs);  // Longer than TWDT; polling must still feed it.
  pass &= check(health::consoleStatus().reports == disabled,
                "console off beyond watchdog timeout: no report, no reset");
  health::configureConsole(true, cfg::kDebugStatusIntervalMinS);
  pollFor(kEnabledWaitMs);
  pass &= check(health::consoleStatus().reports == disabled + 1,
                "console re-enabled: periodic reports resume");
  // The LVGL mutex is recursive: a distinct task must hold it for this test.
  pass &= testBusyDisplay();
  health::poll();
  const health::ConsoleStatus measured = health::consoleStatus();
  ESP_LOGI(kTag, "full status duration: last %u us, max %u us",
           static_cast<unsigned>(measured.last_report_us),
           static_cast<unsigned>(measured.max_report_us));
  health::applySettings();  // Original model untouched; no NVS write.
  return pass;
}

void runRules() {
  health::configureConsole(true, cfg::kDebugStatusIntervalMinS);
  bool pass = testSnapshots();
  pass &= testAllocationFailure();  // Stopped monitor; exercises allocation.
  pass &= testMeters();
  pass &= testConsoleAndWatchdog();
  ESP_LOGI(kTag, "%s: health diagnostics complete; original settings restored",
           pass ? "PASS" : "FAIL");
}

template <bool Enabled>
void runStallOnce() {
  if constexpr (Enabled) {
    struct Record {
      uint32_t magic;
      uint32_t injected;
    };
    static RTC_NOINIT_ATTR Record record;
    constexpr uint32_t kMagic = 0x48323857;
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool abnormal = reason == ESP_RST_TASK_WDT ||
                          reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
                          reason == ESP_RST_PANIC;
    if (record.magic != kMagic || !abnormal) {
      record = {kMagic, 0};
    }
    ESP_ERROR_CHECK(health::startLoopWatchdog());
    if (record.injected == 0) {
      record.injected = 1;  // Survives the expected panic; never a reset loop.
      ESP_LOGW(kTag,
               "one-shot app stall: IDLE/LVGL can run; expect app_loop TWDT");
      health::poll();
      vTaskDelay(pdMS_TO_TICKS((CONFIG_ESP_TASK_WDT_TIMEOUT_S + 2) * 1000));
      check(false, "app stall returned without watchdog reset");
    } else {
      check(reason == ESP_RST_TASK_WDT || reason == ESP_RST_PANIC,
            "one-shot app-stall boot resumed after abnormal reset");
      ESP_LOGI(kTag, "one-shot marker prevents another injected stall");
    }
  }
}

}  // namespace

// Called under the SDK monitor spinlock. Only an atomic one-shot exchange:
// never allocate, log, wait or access any other SDK service here.
extern "C" bool fussball_heap_monitor_fail_allocation() {
  if constexpr (cfg::kHealthTest == cfg::HealthTest::kMetersAndStatus) {
    return fail_monitor_allocation.exchange(0, std::memory_order_relaxed) != 0;
  }
  return false;
}

namespace health {

void runDeviceTest() {
  if constexpr (cfg::kHealthTest != cfg::HealthTest::kNone) {
    ESP_LOGW(kTag, "device test %u active (app_config.h kHealthTest)",
             static_cast<unsigned>(cfg::kHealthTest));
    if constexpr (cfg::kHealthTest == cfg::HealthTest::kMetersAndStatus) {
      runRules();
    } else if constexpr (cfg::kHealthTest == cfg::HealthTest::kAppStallOnce) {
      runStallOnce<true>();
    }
  }
}

}  // namespace health
