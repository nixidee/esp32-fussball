#include <new>

#include "application_config.h"
#include "file_error.h"
#include "settings_store.h"
namespace application {
esp_err_t detail::ConfigSnapshot::prepare() {
  if (!admitted()) return ESP_ERR_NOT_FINISHED;
  if (config_) return ESP_OK;
  config_.reset(new (std::nothrow) settings::AppConfig);
  if (!config_) return ESP_ERR_NO_MEM;
  settings::snapshot(*config_);
  return ESP_OK;
}
football::names::Decode ApplySession::stageNames(json::Reader& reader,
                                                 const json::Token& token,
                                                 settings::Violation& why) {
  using football::names::Decode;
  if (!snapshot_.value()) {
    names_error_ = ESP_ERR_INVALID_STATE;
    return Decode::kIo;
  }
  names_error_ = football::names::begin(names_, true);
  if (names_error_ != ESP_OK) return Decode::kIo;
  const auto decoded =
      football::names::readAdoptedJson(reader, token, names_, why);
  switch (decoded) {
    case Decode::kNoMemory: names_error_ = ESP_ERR_NO_MEM; break;
    case Decode::kStorageFull: names_error_ = ::files::kStorageFull; break;
    case Decode::kIo: names_error_ = ESP_FAIL; break;
    default: break;
  }
  return decoded;
}
ApplyResult ApplySession::publish(uint32_t expected, bool names_changed,
                                  settings::Violation& why) {
  ApplyResult result;
  if (!snapshot_.value())
    return {ESP_ERR_INVALID_STATE, ApplyStage::kSnapshot, {}};
  if (names_error_ != ESP_OK) return {names_error_, ApplyStage::kNames, {}};
  if (settings::generation() != expected)
    return {ESP_ERR_INVALID_STATE, ApplyStage::kRevision, {}};
  settings::NamesChange names_change;
  if (names_changed) {
    const auto error = football::names::prepareChange(names_, names_change);
    if (error != ESP_OK) return {error, ApplyStage::kNames, {}};
  }
  result.error = settings::apply(draft(), expected, &why, &result.policy,
                                 names_changed ? &names_change : nullptr);
  if (settings::origin() == settings::Origin::kOutcomeUnknown)
    result.stage = ApplyStage::kUncertain;
  else if (result.error == ESP_ERR_INVALID_ARG)
    result.stage = ApplyStage::kValidation;
  else if (result.error != ESP_OK)
    result.stage = ApplyStage::kPublication;
  return result;
}
esp_err_t ConfigExportSession::prepare() {
  const auto error = snapshot_.prepare();
  return error == ESP_OK ? names_.prepare() : error;
}
}  // namespace application
