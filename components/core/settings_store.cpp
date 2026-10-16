#include "settings_store.h"

#include <atomic>
#include <cstdlib>
#include <span>

#include "app_config.h"
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

// Guards current_model, current_competitions, both origins and publication of
// their generations; held only for short snapshot reads. Writers also hold
// write_mutex, so a writer may read them without this mutex.
StaticSemaphore_t model_mutex_buffer;
SemaphoreHandle_t model_mutex = nullptr;
Model current_model;
Origin current_source = Origin::kInitial;
Competitions current_competitions;
Origin competitions_source = Origin::kInitial;
// Set once by init() before any other task runs; read-only afterwards.
Limits display_limits;
std::atomic<uint32_t> generation_counter{0}, data_generation_counter{0};

// Serialises save(), saveCompetitions() and reset(); guards the flags
// below.
StaticSemaphore_t write_mutex_buffer;
SemaphoreHandle_t write_mutex = nullptr;
bool nvs_ready = false;
// The stored record exists but could not be read (read error or no memory
// for the buffer). Saving would replace data that was never checked, so only
// reset() may replace it.
bool record_unread = false;
// The same for the competition record.
bool competitions_unread = false;

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

// Publish while holding model_mutex, then notify after releasing it.
// 'data_changed' also discards fetched provider data (dataGeneration()).
void advanceGenerations(bool data_changed) {
  if (data_changed)
    data_generation_counter.fetch_add(1, std::memory_order_release);
  generation_counter.fetch_add(1, std::memory_order_release);
}

void setCurrent(const Model& model, Origin from) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const bool data_changed = !sameProviderData(current_model, model);
  current_model = model;
  current_source = from;
  advanceGenerations(data_changed);
  xSemaphoreGive(model_mutex);
  events::post(events::Event::kSettingsChanged);
}

void setCompetitions(const Competitions& competitions, Origin from) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const bool data_changed =
      !sameProviderData(current_competitions, competitions);
  current_competitions = competitions;
  competitions_source = from;
  advanceGenerations(data_changed);
  xSemaphoreGive(model_mutex);
  events::post(events::Event::kSettingsChanged);
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

// Initialises the default NVS partition. Erases it only for the two errors
// that ESP-IDF documents as needing an erase (decision C1); this also drops
// the radio calibration data, which is measured again.
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

// Records where the current settings come from (boot only, before other
// tasks run).
void setOrigin(Origin why) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  current_source = why;
  xSemaphoreGive(model_mutex);
}

void refuseSavingAfter(const char* step, esp_err_t err) {
  record_unread = true;
  setOrigin(Origin::kUnreadable);
  ESP_LOGE(kTag,
           "stored settings not readable (%s: %s): initial values apply, "
           "saving is disabled until a reset",
           step, esp_err_to_name(err));
}

void setCompetitionsOrigin(Origin why) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  competitions_source = why;
  xSemaphoreGive(model_mutex);
}

void refuseCompetitionsAfter(const char* step, esp_err_t err) {
  competitions_unread = true;
  setCompetitionsOrigin(Origin::kUnreadable);
  ESP_LOGE(kTag,
           "stored competitions not readable (%s: %s): initial values apply, "
           "saving them is disabled until a reset",
           step, esp_err_to_name(err));
}

// A stored record read into a heap buffer (boot only). 'step' names what
// failed when 'err' is neither ESP_OK nor ESP_ERR_NVS_NOT_FOUND;
// ESP_ERR_INVALID_SIZE means longer than 'max' (buffer stays null).
struct Blob {
  uint8_t* data = nullptr;
  std::size_t size = 0;
  esp_err_t err = ESP_OK;
  const char* step = nullptr;
  Blob() = default;
  Blob(const Blob&) = delete;
  Blob& operator=(const Blob&) = delete;
  ~Blob() { std::free(data); }
};

void readBlob(nvs_handle_t handle, const char* key, std::size_t max,
              Blob& blob) {
  blob.err = nvs_get_blob(handle, key, nullptr, &blob.size);
  if (blob.err != ESP_OK) {
    blob.step = "size";
    return;
  }
  if (blob.size > max) {
    blob.err = ESP_ERR_INVALID_SIZE;
    return;
  }
  blob.data = static_cast<uint8_t*>(std::malloc(blob.size > 0 ? blob.size : 1));
  if (blob.data == nullptr) {
    blob.err = ESP_ERR_NO_MEM;
    blob.step = "buffer";
    return;
  }
  blob.err = nvs_get_blob(handle, key, blob.data, &blob.size);
  if (blob.err != ESP_OK) blob.step = "read";
}

// Loads the competition record over the initial competitions.
void loadCompetitions(nvs_handle_t handle) {
  Blob blob;
  readBlob(handle, cfg::kCompetitionsNvsKey, cfg::kCompetitionsMaxRecordBytes,
           blob);
  if (blob.err == ESP_ERR_NVS_NOT_FOUND) return;
  if (blob.err == ESP_ERR_INVALID_SIZE) {
    setCompetitionsOrigin(Origin::kDamaged);
    ESP_LOGE(kTag,
             "stored competitions damaged (%u B, %s): initial competitions "
             "apply until the next save",
             static_cast<unsigned>(blob.size),
             decodeResultText(DecodeResult::kTooLong));
    return;
  }
  if (blob.err != ESP_OK) {
    refuseCompetitionsAfter(blob.step, blob.err);
    return;
  }
  const DecodeResult result =
      decodeCompetitions({blob.data, blob.size}, current_competitions);
  if (result == DecodeResult::kOk) {
    setCompetitionsOrigin(Origin::kStored);
    ESP_LOGI(kTag, "stored competitions loaded (%u B)",
             static_cast<unsigned>(blob.size));
  } else if (result == DecodeResult::kOutdatedVersion) {
    setCompetitionsOrigin(Origin::kOutdated);
    ESP_LOGW(kTag,
             "stored competitions have an older format (%u B): discarded, "
             "initial competitions apply until the next save",
             static_cast<unsigned>(blob.size));
  } else {
    setCompetitionsOrigin(result == DecodeResult::kInvalidValue
                              ? Origin::kRejected
                              : Origin::kDamaged);
    ESP_LOGE(kTag,
             "stored competitions %s (%u B, %s): initial competitions apply "
             "until the next save",
             result == DecodeResult::kInvalidValue ? "rejected" : "damaged",
             static_cast<unsigned>(blob.size), decodeResultText(result));
  }
}

// Loads the settings record over the initial values in current_model.
void loadSettings(nvs_handle_t handle) {
  Blob blob;
  readBlob(handle, cfg::kSettingsNvsKey, cfg::kSettingsMaxRecordBytes, blob);
  if (blob.err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kTag, "no stored settings: initial values apply");
    return;
  }
  if (blob.err == ESP_ERR_INVALID_SIZE) {
    setOrigin(Origin::kDamaged);
    ESP_LOGE(kTag,
             "stored settings damaged (%u B, %s): initial values apply until "
             "the next save",
             static_cast<unsigned>(blob.size),
             decodeResultText(DecodeResult::kTooLong));
    return;
  }
  if (blob.err != ESP_OK) {
    refuseSavingAfter(blob.step, blob.err);
    return;
  }

  Model loaded = current_model;
  Field invalid = Field::kWifiSsid;
  const DecodeResult result =
      decode({blob.data, blob.size}, loaded, &invalid, display_limits);
  if (result == DecodeResult::kOk) {
    current_model = loaded;
    setOrigin(Origin::kStored);
    ESP_LOGI(kTag, "stored settings loaded (%u B)",
             static_cast<unsigned>(blob.size));
    return;
  }
  if (result == DecodeResult::kOutdatedVersion) {
    setOrigin(Origin::kOutdated);
    ESP_LOGW(kTag,
             "stored settings have an older format (%u B): discarded, "
             "initial values apply until the next save",
             static_cast<unsigned>(blob.size));
  } else if (result == DecodeResult::kInvalidValue) {
    setOrigin(Origin::kRejected);
    ESP_LOGE(kTag,
             "stored settings rejected (%s: %s): initial values apply until "
             "the next save",
             fieldName(invalid), decodeResultText(result));
  } else {
    setOrigin(Origin::kDamaged);
    ESP_LOGE(kTag,
             "stored settings damaged (%u B, %s): initial values apply until "
             "the next save",
             static_cast<unsigned>(blob.size), decodeResultText(result));
  }
}

esp_err_t writeRecord(const char* key, std::span<const uint8_t> record) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(cfg::kSettingsNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(handle, key, record.data(), record.size());
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

esp_err_t writeSettings(const Model& model) {
  auto* buffer = static_cast<uint8_t*>(std::malloc(kMaxRecordBytes));
  if (buffer == nullptr) return ESP_ERR_NO_MEM;
  const std::span<uint8_t, kMaxRecordBytes> record(buffer, kMaxRecordBytes);
  const esp_err_t err =
      writeRecord(cfg::kSettingsNvsKey, record.first(encode(model, record)));
  std::free(buffer);
  return err;
}

esp_err_t writeCompetitions(const Competitions& competitions) {
  auto* buffer =
      static_cast<uint8_t*>(std::malloc(kMaxCompetitionRecordBytes));
  if (buffer == nullptr) return ESP_ERR_NO_MEM;
  const std::span<uint8_t, kMaxCompetitionRecordBytes> record(
      buffer, kMaxCompetitionRecordBytes);
  const esp_err_t err = writeRecord(
      cfg::kCompetitionsNvsKey,
      record.first(encodeCompetitions(competitions, record)));
  std::free(buffer);
  return err;
}

// Loads both records (boot only) and writes nothing; a discarded older record
// stays until the next save, so an OTA rollback can still read it. Settings
// of formats 2 to 7 carried the competitions: without a competition record,
// those competitions count as discarded too.
void load() {
  nvs_handle_t handle;
  const esp_err_t err =
      nvs_open(cfg::kSettingsNvsNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kTag, "no stored settings: initial values apply");
    return;
  }
  if (err != ESP_OK) {
    refuseSavingAfter("open", err);
    refuseCompetitionsAfter("open", err);
    return;
  }
  loadSettings(handle);
  loadCompetitions(handle);
  nvs_close(handle);
  if (current_source == Origin::kOutdated &&
      competitions_source == Origin::kInitial)
    setCompetitionsOrigin(Origin::kOutdated);
}

const char* setText(std::size_t length) { return length > 0 ? "set" : "empty"; }

}  // namespace

void init(const Limits& limits) {
  display_limits = limits;
  model_mutex = xSemaphoreCreateMutexStatic(&model_mutex_buffer);
  write_mutex = xSemaphoreCreateMutexStatic(&write_mutex_buffer);

  FieldMask rejected = 0;
  current_model = initialValues(presets(), &rejected, display_limits);
  logRejectedPresets(rejected);
  initialCompetitions(presets(), current_competitions, &rejected);
  logRejectedPresets(rejected);

  nvs_ready = initNvs();
  if (nvs_ready) {
    load();
  } else {
    setOrigin(Origin::kNoStorage);
    setCompetitionsOrigin(Origin::kNoStorage);
  }
  logCurrent("at boot");
}

const Limits& limits() { return display_limits; }

Model current(uint32_t* out_generation) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Model copy = current_model;
  if (out_generation != nullptr)
    *out_generation = generation_counter.load(std::memory_order_acquire);
  xSemaphoreGive(model_mutex);
  return copy;
}

ImageMask referencedImages() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const auto result = referencedImages(current_model);
  xSemaphoreGive(model_mutex);
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
    case Origin::kInitial:
      return "initial";
    case Origin::kStored:
      return "stored";
    case Origin::kRejected:
      return "rejected";
    case Origin::kOutdated: return "outdated";
    case Origin::kDamaged:
      return "damaged";
    case Origin::kUnreadable:
      return "unreadable";
    case Origin::kNoStorage:
      return "no_storage";
  }
  return "initial";
}

uint32_t generation() {
  return generation_counter.load(std::memory_order_acquire);
}

uint32_t dataGeneration() {
  return data_generation_counter.load(std::memory_order_acquire);
}

esp_err_t save(const Model& model, Violation* violation, bool check_access) {
  Field invalid = Field::kWifiSsid;
  if (!validate(model, &invalid, violation, display_limits)) {
    ESP_LOGE(kTag, "save rejected: %s violates its limits", fieldName(invalid));
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(write_mutex, portMAX_DELAY);
  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (!nvs_ready || record_unread) {
    ESP_LOGE(
        kTag, "save refused: %s",
        nvs_ready ? "stored record unread, reset first" : "NVS unavailable");
  } else if (check_access &&
             !checkProviderAccess(model, current_competitions, violation)) {
    err = ESP_ERR_INVALID_ARG;
    ESP_LOGE(kTag, "save rejected: a competition uses a disabled provider");
  } else {
    err = writeSettings(model);
    if (err == ESP_OK) {
      setCurrent(model, Origin::kStored);
    } else {
      ESP_LOGE(kTag, "save failed: %s, settings unchanged",
               esp_err_to_name(err));
    }
  }
  xSemaphoreGive(write_mutex);
  return err;
}

std::size_t competitionCount() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const std::size_t count = current_competitions.entries.size();
  xSemaphoreGive(model_mutex);
  return count;
}

bool competitionAt(std::size_t index, Competition& out) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const bool found = index < current_competitions.entries.size();
  if (found) out = current_competitions.entries[index];
  xSemaphoreGive(model_mutex);
  return found;
}

bool competitionById(uint8_t id, Competition& out) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Competition* entry = id != 0 ? current_competitions.find(id) : nullptr;
  if (entry != nullptr) out = *entry;
  xSemaphoreGive(model_mutex);
  return entry != nullptr;
}

void competitions(Competitions& out) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  out = current_competitions;
  xSemaphoreGive(model_mutex);
}

uint8_t competitionId(std::size_t index) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const uint8_t id = index < current_competitions.entries.size()
                         ? current_competitions.entries[index].id
                         : 0;
  xSemaphoreGive(model_mutex);
  return id;
}

Origin competitionsOrigin() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Origin from = competitions_source;
  xSemaphoreGive(model_mutex);
  return from;
}

esp_err_t saveCompetitions(Competitions& candidate, Violation* violation) {
  xSemaphoreTake(write_mutex, portMAX_DELAY);
  esp_err_t err = ESP_ERR_INVALID_STATE;
  // Ids are the store's: a new entry gets the next free one.
  candidate.next_id = current_competitions.next_id;
  assignIds(candidate);
  if (!nvs_ready || competitions_unread) {
    ESP_LOGE(kTag, "competitions save refused: %s",
             nvs_ready ? "stored record unread, reset first"
                       : "NVS unavailable");
  } else if (!validate(candidate, violation) ||
             !checkProviderAccess(current_model, candidate, violation)) {
    err = ESP_ERR_INVALID_ARG;
    ESP_LOGE(kTag, "competitions save rejected: %s",
             violation != nullptr ? violation->path.data() : "invalid");
  } else if ((err = writeCompetitions(candidate)) != ESP_OK) {
    ESP_LOGE(kTag, "competitions save failed: %s, competitions unchanged",
             esp_err_to_name(err));
  } else {
    setCompetitions(candidate, Origin::kStored);
  }
  xSemaphoreGive(write_mutex);
  return err;
}

esp_err_t reset() {
  xSemaphoreTake(write_mutex, portMAX_DELAY);
  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (nvs_ready) {
    nvs_handle_t handle;
    err = nvs_open(cfg::kSettingsNvsNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
      err = nvs_erase_all(handle);
      if (err == ESP_OK) err = nvs_commit(handle);
      nvs_close(handle);
    }
  }
  if (err == ESP_OK) {
    record_unread = false;
    competitions_unread = false;
    // Built in place: the store is too large for the caller's stack.
    xSemaphoreTake(model_mutex, portMAX_DELAY);
    initialCompetitions(presets(), current_competitions);
    competitions_source = Origin::kInitial;
    xSemaphoreGive(model_mutex);
    // Every fetched value may belong to a removed competition.
    data_generation_counter.fetch_add(1, std::memory_order_release);
    setCurrent(initialValues(presets(), nullptr, display_limits),
               Origin::kInitial);
    ESP_LOGI(kTag, "settings reset: initial values apply");
  } else {
    ESP_LOGE(kTag, "reset failed: %s, settings unchanged",
             esp_err_to_name(err));
  }
  xSemaphoreGive(write_mutex);
  return err;
}

void logCurrent(const char* when) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Model model = current_model;
  const Origin from = current_source;
  const auto competition_count =
      static_cast<unsigned>(current_competitions.entries.size());
  const Origin competitions_from = competitions_source;
  xSemaphoreGive(model_mutex);

  ESP_LOGI(
      kTag,
      "%s (%s): wifi ssid %s (%u B), wifi password %s, ap password %s, "
      "hostname %.*s, external antenna %s, status log %s every %u s, "
      "time zone %.*s, ntp server %.*s",
      when, originName(from),
      setText(model.wifi_ssid.length),
      static_cast<unsigned>(model.wifi_ssid.length),
      setText(model.wifi_password.length), setText(model.ap_password.length),
      static_cast<int>(model.hostname.length), model.hostname.data.data(),
      model.external_antenna ? "on" : "off",
      model.debug_status_log ? "on" : "off",
      static_cast<unsigned>(model.debug_status_interval_s),
      static_cast<int>(model.time_zone.length), model.time_zone.data.data(),
      static_cast<int>(model.ntp_server.length), model.ntp_server.data.data());
  ESP_LOGI(kTag, "%s (%s): %u competitions", when,
           originName(competitions_from), competition_count);
}

}  // namespace settings
