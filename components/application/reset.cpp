#include "application_reset.h"
#include "football_service.h"
#include "operation_admission.h"
#include "ota_service.h"
#include "request_budget.h"
#include "settings_store.h"
#include "team_name_service.h"

namespace application {
ResetResult reset(const ResetOptions& options) {
  ResetResult result;
  operations::Guard guard;
  if (!guard) {
    result.error = ESP_ERR_NOT_FINISHED;
    result.stage = ResetStage::kBusy;
    return result;
  }
  result.stage = ResetStage::kAdmission;
  result.error = football::beginReset();
  if (result.error != ESP_OK) return result;
  struct Fence {
    bool release = true;
    ~Fence() {
      if (release) {
        football::names::invalidate();
        football::endReset();
      }
    }
  } fence;
  auto& progress = result.progress;
  // Reset authority first. The token prevents a new counter write during
  // checked deletion/format; each domain reports its actual partial effects.
  result.stage = ResetStage::kSettings;
  result.error = settings::reset();
  if (result.error != ESP_OK) return result;
  progress.settings = true;
  result.stage = ResetStage::kCounters;
  result.error = football::budget::erase();
  if (result.error != ESP_OK) return result;
  progress.counters = true;
  if (options.format_storage) {
    result.stage = ResetStage::kFormat;
    result.error = images::format();
    if (result.error != ESP_OK) return result;
    progress.format = progress.adopted_names = progress.images = true;
  } else {
    result.stage = ResetStage::kNames;
    result.error = football::names::erase();
    if (result.error != ESP_OK) return result;
    progress.adopted_names = true;
    if (options.images) {
      result.stage = ResetStage::kImages;
      result.error = images::removeAll(&progress.image_files);
      if (result.error != ESP_OK) return result;
      progress.images = true;
    }
  }
  result.stage = ResetStage::kNone;
  fence.release = false;
  ota::reboot();
  return result;
}
}  // namespace application
