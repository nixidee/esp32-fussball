// Bounded application sessions. Member order keeps candidate/export teardown
// inside the shared admission lifetime. HTTP owns authentication and decoding.
#pragma once
#include <memory>

#include "app_config_model.h"
#include "operation_admission.h"
#include "team_name_json.h"
#include "team_name_service.h"
namespace application {
namespace detail {
class ConfigSnapshot {
 public:
  bool admitted() const { return bool(guard_); }
  esp_err_t prepare();
  settings::AppConfig* value() { return config_.get(); }
  const settings::AppConfig* value() const { return config_.get(); }

 private:
  operations::Guard guard_;
  std::unique_ptr<settings::AppConfig> config_;
};
}  // namespace detail
enum class ApplyStage : uint8_t {
  kNone,
  kSnapshot,
  kNames,
  kRevision,
  kValidation,
  kPublication,
  kUncertain
};
struct ApplyResult {
  esp_err_t error = ESP_OK;
  ApplyStage stage = ApplyStage::kNone;
  settings::screen_policy::Error policy;
};
class ApplySession {
 public:
  bool admitted() const { return snapshot_.admitted(); }
  esp_err_t prepare() { return snapshot_.prepare(); }
  settings::AppConfig& draft() { return *snapshot_.value(); }
  football::names::Decode stageNames(json::Reader&, const json::Token&,
                                     settings::Violation&);
  esp_err_t namesError() const { return names_error_; }
  ApplyResult publish(uint32_t expected_revision, bool names_changed,
                      settings::Violation&);

 private:
  detail::ConfigSnapshot snapshot_;
  football::names::files::Candidate names_;
  esp_err_t names_error_ = ESP_OK;
};
class ConfigExportSession {
 public:
  bool admitted() const { return snapshot_.admitted(); }
  esp_err_t prepare();
  const settings::AppConfig& config() const { return *snapshot_.value(); }
  esp_err_t writeNames(json::Writer& out) { return names_.write(out); }

 private:
  detail::ConfigSnapshot snapshot_;
  football::names::AdoptedExport names_;
};
}  // namespace application
