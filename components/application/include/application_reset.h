// Application reset use case. HTTP and other callers supply typed options;
// existing domain owners retain their data and physical I/O responsibilities.
#pragma once

#include "esp_err.h"
#include "image_service.h"

namespace application {
struct ResetOptions {
  bool images = false, format_storage = false;
};
enum class ResetStage : uint8_t {
  kNone,
  kBusy,
  kAdmission,
  kSettings,
  kCounters,
  kNames,
  kImages,
  kFormat
};
struct ResetProgress {
  bool settings = false, counters = false, adopted_names = false;
  bool images = false, format = false;
  images::DeleteAllResult image_files;
};
struct ResetResult {
  esp_err_t error = ESP_OK;
  ResetStage stage = ResetStage::kNone;
  ResetProgress progress;
};
// Success schedules a reboot and retains the provider publication fence until
// then. Failure releases it and reports only actually completed operations.
ResetResult reset(const ResetOptions&);
}  // namespace application
