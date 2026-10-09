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

namespace settings {

namespace {

constexpr const char* kTag = "settings";

enum class Source : uint8_t { kInitial, kStored };

// Guards current_model and source; held only for copies.
StaticSemaphore_t model_mutex_buffer;
SemaphoreHandle_t model_mutex = nullptr;
Model current_model;
Source source = Source::kInitial;
std::atomic<uint32_t> generation_counter{0};

// Serialises save() and reset(); guards the two flags below.
StaticSemaphore_t write_mutex_buffer;
SemaphoreHandle_t write_mutex = nullptr;
bool nvs_ready = false;
// The stored record exists but could not be read (read error or no memory
// for the buffer). Saving would replace data that was never checked, so only
// reset() may replace it.
bool record_unread = false;

Presets presets() {
  return {
      .wifi_ssid = SECRET_WIFI_SSID,
      .wifi_password = SECRET_WIFI_PASSWORD,
      .ap_password = SECRET_AP_PASSWORD,
      .hostname = SECRET_HOSTNAME,
      .time_zone = SECRET_TIME_ZONE,
  };
}

void setCurrent(const Model& model, Source from) {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  current_model = model;
  source = from;
  xSemaphoreGive(model_mutex);
  generation_counter.fetch_add(1, std::memory_order_release);
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

void refuseSavingAfter(const char* step, esp_err_t err) {
  record_unread = true;
  ESP_LOGE(kTag,
           "stored settings not readable (%s: %s): initial values apply, "
           "saving is disabled until a reset",
           step, esp_err_to_name(err));
}

// Loads the stored record over the initial values in current_model.
void load() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(cfg::kSettingsNvsNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kTag, "no stored settings: initial values apply");
    return;
  }
  if (err != ESP_OK) return refuseSavingAfter("open", err);

  std::size_t size = 0;
  err = nvs_get_blob(handle, cfg::kSettingsNvsKey, nullptr, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    ESP_LOGI(kTag, "no stored settings: initial values apply");
    return;
  }
  if (err != ESP_OK) {
    nvs_close(handle);
    return refuseSavingAfter("size", err);
  }
  if (size > cfg::kSettingsMaxRecordBytes) {
    nvs_close(handle);
    ESP_LOGE(kTag,
             "stored settings damaged (%u B, %s): initial values apply until "
             "the next save",
             static_cast<unsigned>(size),
             decodeResultText(DecodeResult::kTooLong));
    return;
  }

  auto* buffer = static_cast<uint8_t*>(std::malloc(size > 0 ? size : 1));
  if (buffer == nullptr) {
    nvs_close(handle);
    return refuseSavingAfter("buffer", ESP_ERR_NO_MEM);
  }
  err = nvs_get_blob(handle, cfg::kSettingsNvsKey, buffer, &size);
  nvs_close(handle);
  if (err != ESP_OK) {
    std::free(buffer);
    return refuseSavingAfter("read", err);
  }

  Model loaded = current_model;
  Field invalid = Field::kWifiSsid;
  const DecodeResult result = decode({buffer, size}, loaded, &invalid);
  std::free(buffer);
  if (result == DecodeResult::kOk) {
    current_model = loaded;
    source = Source::kStored;
    ESP_LOGI(kTag, "stored settings loaded (%u B)",
             static_cast<unsigned>(size));
  } else if (result == DecodeResult::kInvalidValue) {
    ESP_LOGE(kTag,
             "stored settings rejected (%s: %s): initial values apply until "
             "the next save",
             fieldName(invalid), decodeResultText(result));
  } else {
    ESP_LOGE(kTag,
             "stored settings damaged (%u B, %s): initial values apply until "
             "the next save",
             static_cast<unsigned>(size), decodeResultText(result));
  }
}

esp_err_t writeRecord(std::span<const uint8_t, kRecordBytes> record) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(cfg::kSettingsNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err =
      nvs_set_blob(handle, cfg::kSettingsNvsKey, record.data(), record.size());
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

const char* setText(std::size_t length) { return length > 0 ? "set" : "empty"; }

}  // namespace

void init() {
  model_mutex = xSemaphoreCreateMutexStatic(&model_mutex_buffer);
  write_mutex = xSemaphoreCreateMutexStatic(&write_mutex_buffer);

  FieldMask rejected = 0;
  current_model = initialValues(presets(), &rejected);
  logRejectedPresets(rejected);

  nvs_ready = initNvs();
  if (nvs_ready) load();
  logCurrent("at boot");
}

Model current() {
  xSemaphoreTake(model_mutex, portMAX_DELAY);
  const Model copy = current_model;
  xSemaphoreGive(model_mutex);
  return copy;
}

uint32_t generation() {
  return generation_counter.load(std::memory_order_acquire);
}

esp_err_t save(const Model& model) {
  Field invalid = Field::kWifiSsid;
  if (!validate(model, &invalid)) {
    ESP_LOGE(kTag, "save rejected: %s violates its limits", fieldName(invalid));
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(write_mutex, portMAX_DELAY);
  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (!nvs_ready || record_unread) {
    ESP_LOGE(
        kTag, "save refused: %s",
        nvs_ready ? "stored record unread, reset first" : "NVS unavailable");
  } else if (auto* buffer = static_cast<uint8_t*>(std::malloc(kRecordBytes));
             buffer == nullptr) {
    err = ESP_ERR_NO_MEM;
    ESP_LOGE(kTag, "save failed: %s", esp_err_to_name(err));
  } else {
    const std::span<uint8_t, kRecordBytes> record(buffer, kRecordBytes);
    encode(model, record);
    err = writeRecord(record);
    std::free(buffer);
    if (err == ESP_OK) {
      setCurrent(model, Source::kStored);
    } else {
      ESP_LOGE(kTag, "save failed: %s, settings unchanged",
               esp_err_to_name(err));
    }
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
    setCurrent(initialValues(presets()), Source::kInitial);
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
  const Source from = source;
  xSemaphoreGive(model_mutex);

  ESP_LOGI(
      kTag,
      "%s (%s): wifi ssid %s (%u B), wifi password %s, ap password %s, "
      "hostname %.*s, external antenna %s, status log %s every %u s, "
      "time zone %.*s, ntp server %.*s",
      when, from == Source::kStored ? "stored" : "initial values",
      setText(model.wifi_ssid.length),
      static_cast<unsigned>(model.wifi_ssid.length),
      setText(model.wifi_password.length), setText(model.ap_password.length),
      static_cast<int>(model.hostname.length), model.hostname.data.data(),
      model.external_antenna ? "on" : "off",
      model.debug_status_log ? "on" : "off",
      static_cast<unsigned>(model.debug_status_interval_s),
      static_cast<int>(model.time_zone.length), model.time_zone.data.data(),
      static_cast<int>(model.ntp_server.length), model.ntp_server.data.data());
}

}  // namespace settings
