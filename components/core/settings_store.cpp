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

// Guards current_model, current_competitions and both origins; held only
// for copies. Writers also hold write_mutex, so a writer may read them
// without this mutex.
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
// The stored settings record is a readable format 2 to 7 record: its
// competitions replaced the competition record at boot (an older firmware
// wrote it last). Loading writes nothing (OTA trial, rollback); the first
// save() or saveCompetitions() writes the competition record and then the
// settings record in the current format, so the next boot no longer
// migrates. Interrupted between the two writes, the next boot migrates the
// unchanged legacy competitions again.
bool settings_legacy = false;
// Crest image id of a format 3 to 5 record, read at boot; 0 once handed over.
std::atomic<uint8_t> legacy_crest{cfg::kStockImageId};

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

// Counts a change readers must see; 'data_changed' also discards the
// fetched provider data (dataGeneration()).
void announce(bool data_changed) {
  if (data_changed)
    data_generation_counter.fetch_add(1, std::memory_order_release);
  generation_counter.fetch_add(1, std::memory_order_release);
  events::post(events::Event::kSettingsChanged);
}

void setCurrent(const Model& model, Origin from) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const bool data_changed = !sameProviderData(current_model, model);
  current_model = model;
  current_source = from;
  xSemaphoreGive(model_mutex);
  announce(data_changed);
}

void setCompetitions(const Competitions& competitions, Origin from) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const bool data_changed =
      !sameProviderData(current_competitions, competitions);
  current_competitions = competitions;
  competitions_source = from;
  xSemaphoreGive(model_mutex);
  announce(data_changed);
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

// Loads the settings record over the initial values in current_model; a
// format 2 to 7 record also replaces current_competitions. True if it did.
bool loadSettings(nvs_handle_t handle) {
  Blob blob;
  readBlob(handle, cfg::kSettingsNvsKey, cfg::kSettingsMaxRecordBytes, blob);
  if (blob.err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kTag, "no stored settings: initial values apply");
    return false;
  }
  if (blob.err == ESP_ERR_INVALID_SIZE) {
    setOrigin(Origin::kDamaged);
    ESP_LOGE(kTag,
             "stored settings damaged (%u B, %s): initial values apply until "
             "the next save",
             static_cast<unsigned>(blob.size),
             decodeResultText(DecodeResult::kTooLong));
    return false;
  }
  if (blob.err != ESP_OK) {
    refuseSavingAfter(blob.step, blob.err);
    return false;
  }

  const std::span<const uint8_t> record(blob.data, blob.size);
  const bool migrating = carriesCompetitions(record);
  Model loaded = current_model;
  Field invalid = Field::kWifiSsid;
  uint8_t crest = cfg::kStockImageId;
  const DecodeResult result =
      decode(record, loaded, &invalid, display_limits, &crest,
             migrating ? &current_competitions : nullptr);
  if (result == DecodeResult::kOk) {
    current_model = loaded;
    legacy_crest = crest;
    setOrigin(Origin::kStored);
    ESP_LOGI(kTag, "stored settings loaded (%u B)",
             static_cast<unsigned>(blob.size));
    return migrating;
  }
  if (result == DecodeResult::kInvalidValue) {
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
  return false;
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

// Loads both records (boot only) and writes nothing. A readable settings
// record of format 2 to 7 was written last by an older firmware: its
// competitions (over the initial ones) win over a competition record.
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
  settings_legacy = loadSettings(handle);
  if (!settings_legacy) loadCompetitions(handle);
  nvs_close(handle);
  if (!settings_legacy) return;
  setCompetitionsOrigin(Origin::kStored);
  ESP_LOGI(kTag,
           "competitions taken from the settings record; the next save "
           "moves them into their own record");
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

Model current() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Model copy = current_model;
  xSemaphoreGive(model_mutex);
  return copy;
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
    case Origin::kDamaged:
      return "damaged";
    case Origin::kUnreadable:
      return "unreadable";
    case Origin::kNoStorage:
      return "no_storage";
  }
  return "initial";
}

uint8_t legacyCrest() { return legacy_crest.load(); }

void forgetLegacyCrest() { legacy_crest = cfg::kStockImageId; }

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
  } else if (settings_legacy &&
             (err = writeCompetitions(current_competitions)) != ESP_OK) {
    ESP_LOGE(kTag, "save failed: competitions not moved (%s)",
             esp_err_to_name(err));
  } else {
    err = writeSettings(model);
    if (err == ESP_OK) {
      settings_legacy = false;
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

int competitionIndex(uint8_t id) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  int index = -1;
  for (std::size_t i = 0; i < current_competitions.entries.size(); ++i)
    if (id != 0 && current_competitions.entries[i].id == id)
      index = static_cast<int>(i);
  xSemaphoreGive(model_mutex);
  return index;
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
  } else if (settings_legacy &&
             (err = writeSettings(current_model)) != ESP_OK) {
    // The legacy settings record still wins at the next boot.
    ESP_LOGE(kTag,
             "competitions save failed: settings record not converted (%s), "
             "competitions unchanged",
             esp_err_to_name(err));
  } else {
    settings_legacy = false;
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
    settings_legacy = false;
    legacy_crest = cfg::kStockImageId;
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
  const auto pair_count =
      static_cast<unsigned>(current_competitions.pairs.size());
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
  ESP_LOGI(kTag, "%s (%s): %u competitions, %u fixture pairs", when,
           originName(competitions_from), competition_count, pair_count);
}

}  // namespace settings
