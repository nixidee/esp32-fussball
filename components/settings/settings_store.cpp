#include "settings_store.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>

#include "app_config.h"
#include "app_config_record.h"
#include "esp_log.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings_record.h"

// Presets (docs/CONFIGURATION.md): secrets.h is optional, every key too.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#if defined(SECRET_WIFI_SSID_1) || defined(SECRET_WIFI_PASSWORD_1)
// Renamed keys (see include/secrets.h.example).
#error "secrets.h: rename SECRET_WIFI_*_1 to SECRET_WIFI_SSID/_PASSWORD"
#endif
#ifdef SECRET_TIMEZONE
// Renamed key with a new value type (POSIX rule -> location label).
#error \
    "secrets.h: rename SECRET_TIMEZONE to SECRET_TIME_ZONE, value: a label such as \"Europe/Berlin\""
#endif
#ifndef SECRET_WIFI_SSID
#define SECRET_WIFI_SSID ""
#endif
#ifndef SECRET_WIFI_PASSWORD
#define SECRET_WIFI_PASSWORD ""
#endif
#ifndef SECRET_AP_PASSWORD
#define SECRET_AP_PASSWORD ""
#endif
#ifndef SECRET_HOSTNAME
#define SECRET_HOSTNAME ""
#endif
#ifndef SECRET_TIME_ZONE
#define SECRET_TIME_ZONE ""
#endif
#ifndef SECRET_ADMIN_PASSWORD
#define SECRET_ADMIN_PASSWORD ""
#endif
#ifndef SECRET_API_FOOTBALL_KEY
#define SECRET_API_FOOTBALL_KEY ""
#endif
#ifndef SECRET_FOOTBALL_DATA_KEY
#define SECRET_FOOTBALL_DATA_KEY ""
#endif
#ifndef SECRET_DEFAULT_PROVIDER
#define SECRET_DEFAULT_PROVIDER ""
#endif
#ifndef SECRET_DEFAULT_COMPETITION
#define SECRET_DEFAULT_COMPETITION ""
#endif
#ifndef SECRET_DEFAULT_TEAM_ID
#define SECRET_DEFAULT_TEAM_ID ""
#endif
#ifndef SECRET_DEFAULT_SEASON
#define SECRET_DEFAULT_SEASON ""
#endif

namespace settings {
namespace {
constexpr const char* kTag = "settings";
StaticSemaphore_t model_mutex_buffer, write_mutex_buffer;
SemaphoreHandle_t model_mutex = nullptr, write_mutex = nullptr;
AppConfig current_config;
Origin current_source = Origin::kInitial;
Limits display_limits;
std::atomic<bool> football_paused{cfg::kScreenFrameDefault};
std::atomic<uint32_t> generation_counter{0}, data_generation_counter{0};
bool nvs_ready = false, record_unread = false;

Presets presets() {
  return {
      .wifi_ssid = SECRET_WIFI_SSID,
      .wifi_password = SECRET_WIFI_PASSWORD,
      .ap_password = SECRET_AP_PASSWORD,
      .hostname = SECRET_HOSTNAME,
      .time_zone = SECRET_TIME_ZONE,
      .admin_password = SECRET_ADMIN_PASSWORD,
      .api_football_key = SECRET_API_FOOTBALL_KEY,
      .football_data_key = SECRET_FOOTBALL_DATA_KEY,
      .provider = SECRET_DEFAULT_PROVIDER,
      .competition = SECRET_DEFAULT_COMPETITION,
      .team = SECRET_DEFAULT_TEAM_ID,
      .season = SECRET_DEFAULT_SEASON,
  };
}

void logRejectedPresets(FieldMask rejected) {
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const auto field = static_cast<Field>(i);
    if (rejected & fieldBit(field)) {
      ESP_LOGE(kTag, "secrets.h: %s violates its limits, default used",
               fieldName(field));
    }
  }
}

bool initNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGE(kTag,
             "NVS partition unusable (%s): erasing it; stored settings and "
             "radio calibration data are lost",
             esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag,
             "NVS unavailable (%s): settings stay in RAM only, saving is "
             "disabled",
             esp_err_to_name(err));
    return false;
  }
  return true;
}

void initialConfig() {
  FieldMask rejected = 0;
  current_config.model = initialValues(presets(), &rejected, display_limits);
  logRejectedPresets(rejected);
  initialCompetitions(presets(), current_config.competitions, &rejected);
  logRejectedPresets(rejected);
  std::destroy_at(&current_config.screens);
  std::construct_at(&current_config.screens);
  current_config.screens.visible_matches =
      std::min(current_config.screens.visible_matches,
               display_limits.visible_matches_max);
  current_config.screens.table_window = std::min(
      current_config.screens.table_window, display_limits.table_window_max);
  current_config.names = {};
  current_config.revision = 0;
  current_config.last_apply_id = 0;
  football_paused.store(current_config.screens.frame,
                        std::memory_order_release);
}
void setOrigin(Origin source) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  current_source = source;
  xSemaphoreGive(model_mutex);
}
void unknownWrite(esp_err_t error) {
  record_unread = true;
  setOrigin(Origin::kOutcomeUnknown);
  ESP_LOGE(kTag,
           "config write outcome unknown (%s): keeping RAM snapshot; "
           "further writes require reboot/recovery",
           esp_err_to_name(error));
}
void publish(const AppConfig& candidate) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const bool data_changed =
      !sameProviderData(current_config.model, candidate.model) ||
      !sameProviderData(current_config.competitions, candidate.competitions);
  current_config = candidate;
  current_source = Origin::kStored;
  football_paused.store(candidate.screens.frame, std::memory_order_release);
  generation_counter.store(candidate.revision, std::memory_order_release);
  if (data_changed)
    data_generation_counter.fetch_add(1, std::memory_order_release);
  xSemaphoreGive(model_mutex);
  events::post(events::Event::kSettingsChanged);
}

// One temporary wire buffer; readback reuses it after NVS consumed the input.
// A mutation/commit error is uncertain even if an immediate read sees old data.
esp_err_t persist(const AppConfig& candidate) {
  auto* buffer = static_cast<uint8_t*>(std::malloc(app_record::kMaxBytes));
  if (!buffer) return ESP_ERR_NO_MEM;
  const auto size = app_record::encode(
      candidate, {buffer, app_record::kMaxBytes}, display_limits);
  if (!size) {
    std::free(buffer);
    return ESP_ERR_INVALID_ARG;
  }
  const auto fingerprint = crc32({buffer, size});
  nvs_handle_t handle;
  auto error = nvs_open(cfg::kSettingsNvsNamespace, NVS_READWRITE, &handle);
  if (error == ESP_OK) {
    error = nvs_set_blob(handle, cfg::kAppConfigNvsKey, buffer, size);
    if (error == ESP_OK) error = nvs_commit(handle);
    if (error == ESP_OK) {
      std::size_t actual = size;
      error = nvs_get_blob(handle, cfg::kAppConfigNvsKey, buffer, &actual);
      if (error == ESP_OK &&
          (actual != size || crc32({buffer, actual}) != fingerprint))
        error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) unknownWrite(error);
    nvs_close(handle);
  }
  std::free(buffer);
  return error;
}

// Boot only: decode directly into the sole private owner; reconstruct defaults
// on any failure before tasks can observe it. No partial generation is
// published.
void load() {
  nvs_handle_t handle;
  auto error = nvs_open(cfg::kSettingsNvsNamespace, NVS_READONLY, &handle);
  if (error == ESP_ERR_NVS_NOT_FOUND) return;
  if (error != ESP_OK) {
    record_unread = true;
    current_source = Origin::kUnreadable;
    return;
  }
  std::size_t size = 0;
  error = nvs_get_blob(handle, cfg::kAppConfigNvsKey, nullptr, &size);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return;
  }
  if (error != ESP_OK) {
    nvs_close(handle);
    record_unread = true;
    current_source = Origin::kUnreadable;
    return;
  }
  if (!size || size > app_record::kMaxBytes) {
    nvs_close(handle);
    current_source = Origin::kDamaged;
    return;
  }
  auto* buffer = static_cast<uint8_t*>(std::malloc(size));
  if (!buffer) {
    nvs_close(handle);
    record_unread = true;
    current_source = Origin::kUnreadable;
    return;
  }
  error = nvs_get_blob(handle, cfg::kAppConfigNvsKey, buffer, &size);
  nvs_close(handle);
  const auto decoded =
      error == ESP_OK
          ? app_record::decode({buffer, size}, current_config, display_limits)
          : DecodeResult::kTooShort;
  std::free(buffer);
  if (error != ESP_OK || decoded != DecodeResult::kOk) {
    initialConfig();
    record_unread = error != ESP_OK;
    current_source =
        error != ESP_OK                            ? Origin::kUnreadable
        : decoded == DecodeResult::kUnknownVersion ? Origin::kOutdated
        : decoded == DecodeResult::kInvalidValue   ? Origin::kRejected
                                                   : Origin::kDamaged;
    return;
  }
  current_source = Origin::kStored;
  generation_counter.store(current_config.revision, std::memory_order_release);
  football_paused.store(current_config.screens.frame,
                        std::memory_order_release);
}
std::unique_ptr<AppConfig> candidate() {
  return std::unique_ptr<AppConfig>(new (std::nothrow)
                                        AppConfig(current_config));
}
const char* setText(std::size_t length) { return length > 0 ? "set" : "empty"; }
}  // namespace

void init(const Limits& limits) {
  display_limits = limits;
  model_mutex = xSemaphoreCreateMutexStatic(&model_mutex_buffer);
  write_mutex = xSemaphoreCreateMutexStatic(&write_mutex_buffer);
  initialConfig();
  nvs_ready = initNvs();
  if (nvs_ready)
    load();
  else
    current_source = Origin::kNoStorage;
  logCurrent("at boot");
}
const Limits& limits() { return display_limits; }
void snapshot(AppConfig& out) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  out = current_config;
  xSemaphoreGive(model_mutex);
}

NamesRef namesReference(uint32_t* out_revision) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const auto result = current_config.names;
  if (out_revision) *out_revision = current_config.revision;
  xSemaphoreGive(model_mutex);
  return result;
}

namespace {
esp_err_t applyLocked(AppConfig& config, uint32_t expected_revision,
                      Violation* violation = nullptr,
                      screen_policy::Error* policy_error = nullptr,
                      const NamesChange* names = nullptr) {
  if (!nvs_ready || record_unread ||
      expected_revision != current_config.revision ||
      current_config.revision == UINT32_MAX)
    return ESP_ERR_INVALID_STATE;
  if (names) {
    if (!names->activate || !names->preserve || !names->desired.slot)
      return ESP_ERR_INVALID_ARG;
    if (names->expected != current_config.names) return ESP_ERR_INVALID_STATE;
    config.names = names->desired;
  } else if (config.names != current_config.names) {
    return ESP_ERR_INVALID_STATE;
  }
  // One publication owner assigns all new identities inside the transaction.
  // Per-competition preferences move with the entry; no second binding table
  // or HTTP save choreography needs reconciliation.
  if (!assignIncarnations(config.competitions, current_config.competitions,
                          violation) ||
      !validate(config, violation, policy_error, display_limits))
    return ESP_ERR_INVALID_ARG;
  config.revision = current_config.revision + 1;
  auto error = persist(config);
  if (error == ESP_OK && names) {
    error = names->activate(names->context);
    if (error != ESP_OK) unknownWrite(error);
  }
  if (error == ESP_OK)
    publish(config);
  else if (names && record_unread)
    names->preserve(names->context);
  return error;
}
}  // namespace
esp_err_t apply(AppConfig& config, uint32_t expected_revision,
                Violation* violation, screen_policy::Error* policy_error,
                const NamesChange* names) {
  xSemaphoreTake(write_mutex, portMAX_DELAY);
  const auto error =
      applyLocked(config, expected_revision, violation, policy_error, names);
  xSemaphoreGive(write_mutex);
  return error;
}
ImageMask referencedImages() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  // Disabled policy entries retain ownership of their desired images.
  const auto result = referencedImages(current_config.screens);
  xSemaphoreGive(model_mutex);
  return result;
}

esp_err_t dropMissingImages(ImageMask present,
                            ImageCleanupCompletion completion, void* context,
                            ImageCleanupProgress* progress) {
  if (progress) *progress = {};
  xSemaphoreTake(write_mutex, portMAX_DELAY);
  esp_err_t result = ESP_ERR_INVALID_STATE;
  if (nvs_ready && !record_unread) {
    result = ESP_OK;
    if (referencedImages(current_config.screens) & ~present) {
      auto next = candidate();
      if (!next)
        result = ESP_ERR_NO_MEM;
      else {
        dropMissingImages(next->screens, present);
        result = applyLocked(*next, current_config.revision);
        if (progress) progress->settings_saved = result == ESP_OK;
      }
    }
    if (progress) progress->references_cleared = result == ESP_OK;
    if (result == ESP_OK && completion) result = completion(context);
  }
  if (progress) {
    progress->revision = current_config.revision;
    progress->outcome_unknown = current_source == Origin::kOutcomeUnknown;
  }
  xSemaphoreGive(write_mutex);
  return result;
}
Origin origin() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Origin from = current_source;
  xSemaphoreGive(model_mutex);
  return from;
}

const char* originName(Origin origin) noexcept {
  switch (origin) {
    case Origin::kInitial: return "initial";
    case Origin::kStored: return "stored";
    case Origin::kRejected: return "rejected";
    case Origin::kOutdated: return "outdated";
    case Origin::kDamaged: return "damaged";
    case Origin::kUnreadable: return "unreadable";
    case Origin::kNoStorage: return "no_storage";
    case Origin::kOutcomeUnknown: return "outcome_unknown";
  }
  return "initial";
}

uint32_t generation() {
  return generation_counter.load(std::memory_order_acquire);
}

uint32_t dataGeneration() {
  return data_generation_counter.load(std::memory_order_acquire);
}

std::size_t competitionCount() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const std::size_t count = current_config.competitions.entries.size();
  xSemaphoreGive(model_mutex);
  return count;
}

bool competitionAt(std::size_t index, Competition& out) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const bool found = index < current_config.competitions.entries.size();
  if (found) out = current_config.competitions.entries[index];
  xSemaphoreGive(model_mutex);
  return found;
}

bool competitionById(uint8_t id, Competition& out) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Competition* entry =
      id != 0 ? current_config.competitions.find(id) : nullptr;
  if (entry != nullptr) out = *entry;
  xSemaphoreGive(model_mutex);
  return entry != nullptr;
}

void competitions(Competitions& out) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  out = current_config.competitions;
  xSemaphoreGive(model_mutex);
}

uint8_t competitionId(std::size_t index) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const uint8_t id = index < current_config.competitions.entries.size()
                         ? current_config.competitions.entries[index].id
                         : 0;
  xSemaphoreGive(model_mutex);
  return id;
}

Origin competitionsOrigin() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Origin from = current_source;
  xSemaphoreGive(model_mutex);
  return from;
}

void screenPolicy(screen_policy::Policy& out, uint32_t* out_generation) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  out = current_config.screens;
  if (out_generation != nullptr)
    *out_generation = generation_counter.load(std::memory_order_acquire);
  xSemaphoreGive(model_mutex);
}

void uiSettings(UiConfig& out, screen_policy::Phase phase) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  projectUiConfig(current_config.model, current_config.screens,
                  current_config.competitions, phase, out);
  out.revision = generation_counter.load(std::memory_order_acquire);
  out.data_generation = data_generation_counter.load(std::memory_order_acquire);
  xSemaphoreGive(model_mutex);
}

void footballSettings(ProviderSettings& model, CompetitionRoutes& routes,
                      uint32_t& settings_generation, uint32_t& data_generation,
                      screen_policy::Eligibility* eligibility,
                      void (*project_routes)(const ProviderSettings&,
                                             CompetitionRoutes&,
                                             bool saved_competitions)) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  projectProviderSettings(current_config.model, model);
  routes.clear();
  for (const auto& entry : current_config.competitions.entries)
    routes.entries.push(static_cast<const CompetitionRoute&>(entry));
  if (project_routes != nullptr)
    project_routes(model, routes, current_source == Origin::kStored);
  if (eligibility != nullptr)
    *eligibility = screen_policy::eligibility(current_config.screens, routes,
                                              &current_config.competitions);
  settings_generation = generation_counter.load(std::memory_order_acquire);
  data_generation = data_generation_counter.load(std::memory_order_acquire);
  xSemaphoreGive(model_mutex);
}

NetworkSettings networkSettings() {
  NetworkSettings out;
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  projectNetworkSettings(current_config.model, out);
  xSemaphoreGive(model_mutex);
  return out;
}

DiagnosticSettings diagnosticSettings() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const DiagnosticSettings out{current_config.model.debug_status_log,
                               current_config.model.debug_status_interval_s};
  xSemaphoreGive(model_mutex);
  return out;
}

WebSettings webSettings(uint32_t* out_revision) {
  WebSettings out;
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  projectWebSettings(current_config.model, out);
  if (out_revision) *out_revision = current_config.revision;
  xSemaphoreGive(model_mutex);
  return out;
}

const cfg::TimeZone& clockZone() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const auto* zone =
      timekeeping::findZone(current_config.model.time_zone.view());
  xSemaphoreGive(model_mutex);
  // Validation guarantees a known label, including initial/default values.
  return *zone;
}

Origin screenPolicyOrigin() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const auto result = current_source;
  xSemaphoreGive(model_mutex);
  return result;
}

bool footballPaused() {
  return football_paused.load(std::memory_order_acquire);
}

esp_err_t beginFootballSend(bool explicit_setup,
                            uint32_t expected_data_generation,
                            uint32_t expected_settings_generation) {
  if (xSemaphoreTake(write_mutex, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
  esp_err_t result = ESP_OK;
  if (dataGeneration() != expected_data_generation ||
      generation() != expected_settings_generation)
    result = ESP_ERR_INVALID_STATE;
  else if (!explicit_setup && footballPaused())
    result = ESP_ERR_NOT_ALLOWED;
  if (result != ESP_OK) xSemaphoreGive(write_mutex);
  return result;
}

void endFootballSend() { xSemaphoreGive(write_mutex); }

esp_err_t reset() {
  xSemaphoreTake(write_mutex, portMAX_DELAY);
  esp_err_t error = ESP_ERR_INVALID_STATE;
  if (nvs_ready) {
    nvs_handle_t handle;
    error = nvs_open(cfg::kSettingsNvsNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
      error = nvs_erase_all(handle);
      if (error == ESP_OK) error = nvs_commit(handle);
      if (error != ESP_OK) unknownWrite(error);
      nvs_close(handle);
    }
  }
  if (error == ESP_OK) {
    xSemaphoreTake(model_mutex, portMAX_DELAY);
    const auto next_revision =
        current_config.revision == UINT32_MAX ? 1 : current_config.revision + 1;
    initialConfig();
    current_config.revision = next_revision;
    generation_counter.store(next_revision, std::memory_order_release);
    data_generation_counter.fetch_add(1, std::memory_order_release);
    current_source = Origin::kInitial;
    record_unread = false;
    xSemaphoreGive(model_mutex);
    events::post(events::Event::kSettingsChanged);
  }
  xSemaphoreGive(write_mutex);
  return error;
}
void logCurrent(const char* when) {
  // Retain only printable non-secret fields, never a complete settings model.
  struct LogSnapshot {
    decltype(Model::hostname) hostname;
    decltype(Model::time_zone) time_zone;
    decltype(Model::ntp_server) ntp_server;
    Origin origin;
    uint16_t ssid_bytes, status_interval;
    uint8_t competitions;
    bool wifi_password, ap_password, external_antenna, status_log;
  } log;
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const auto& model = current_config.model;
  log = {model.hostname,
         model.time_zone,
         model.ntp_server,
         current_source,
         model.wifi_ssid.length,
         model.debug_status_interval_s,
         static_cast<uint8_t>(current_config.competitions.entries.size()),
         model.wifi_password.length != 0,
         model.ap_password.length != 0,
         model.external_antenna,
         model.debug_status_log};
  xSemaphoreGive(model_mutex);

  ESP_LOGI(kTag,
           "%s (%s): wifi ssid %s (%u B), wifi password %s, ap password %s, "
           "hostname %.*s, external antenna %s, status log %s every %u s, "
           "time zone %.*s, ntp server %.*s",
           when, originName(log.origin), setText(log.ssid_bytes),
           static_cast<unsigned>(log.ssid_bytes), setText(log.wifi_password),
           setText(log.ap_password), static_cast<int>(log.hostname.length),
           log.hostname.data.data(), log.external_antenna ? "on" : "off",
           log.status_log ? "on" : "off",
           static_cast<unsigned>(log.status_interval),
           static_cast<int>(log.time_zone.length), log.time_zone.data.data(),
           static_cast<int>(log.ntp_server.length), log.ntp_server.data.data());
  ESP_LOGI(kTag, "%s (%s): %u competitions", when, originName(log.origin),
           static_cast<unsigned>(log.competitions));
}

}  // namespace settings
