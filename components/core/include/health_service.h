// Console health diagnostics and app-loop supervision. No dedicated task.
// init/configure/poll/taskSnapshot/logStatus run only in the app task.
// Heap phase meters may run in other tasks through the shared coordinator.

#pragma once

#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "esp_err.h"
#include "health_meter.h"

namespace health {

struct TaskRow {
  char name[cfg::kHealthTaskNameBytes];
  uint32_t id;
  uint32_t min_free_bytes;
};

struct TaskSnapshot {
  // Owned storage, valid until the next snapshot; never borrowed task names.
  const TaskRow* rows;
  std::size_t count;
  uint32_t total_tasks;
  uint32_t pause_us;
  bool complete;
};

struct ConsoleStatus {
  bool enabled;
  uint16_t interval_s;
  uint32_t reports;
  uint32_t last_report_us;
  uint32_t max_report_us;
};

// After settings/display and their boot tests. Applies stored console settings.
void init();
void applySettings();
// Validated range, also used for nonpersistent diagnostic test overrides.
void configureConsole(bool enabled, uint16_t interval_s);
ConsoleStatus consoleStatus();

// Idempotent; call once startup work is complete. Uses the existing TWDT.
esp_err_t startLoopWatchdog();
// Once per completed app-loop iteration, including while console is disabled.
void poll();
void logStatus();

// Fixed storage. Too many tasks (or a smaller test capacity) => no rows.
TaskSnapshot taskSnapshot(std::size_t capacity = cfg::kHealthMaxTasks);

Coordinator& heapCoordinator();
Observation observeHeap();
void runDeviceTest();

}  // namespace health
