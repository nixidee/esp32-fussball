// Device tests of the settings store, selected by cfg::kSettingsTest
// (app_config.h). Expected results: docs/CONFIGURATION.md.

#include <array>
#include <cstdio>
#include <span>

#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings_record.h"
#include "settings_store.h"

namespace settings {

namespace {

constexpr const char* kTag = "settings_test";

using cfg::SettingsTest;

// Namespace filled by the kNvsFull test; removed before and after it.
constexpr char kFillNamespace[] = "fill_test";
constexpr uint32_t kSaveLoopPeriodMs = 100;
constexpr uint32_t kSaveLoopCount = 2000;
constexpr uint16_t kSampleIntervalS = 10;
constexpr uint16_t kLoopIntervalsS[] = {100, 200};

// Current settings with the non-secret fields changed, so a stored WiFi
// configuration survives the test.
Model sampleModel() {
  Model model = current();
  model.hostname.assign("fussball-test");
  model.external_antenna = !cfg::kExternalAntennaDefault;
  model.debug_status_log = true;
  model.debug_status_interval_s = kSampleIntervalS;
  return model;
}

// Stores a prepared record as it is, bypassing validation.
void storeRaw(std::span<const uint8_t> record, const char* what) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(cfg::kSettingsNvsNamespace, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_blob(handle, cfg::kSettingsNvsKey, record.data(),
                       record.size());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  if (err == ESP_OK) {
    ESP_LOGW(kTag, "stored %s (%u B): press RST and check the boot log", what,
             static_cast<unsigned>(record.size()));
  } else {
    ESP_LOGE(kTag, "storing %s failed: %s", what, esp_err_to_name(err));
  }
}

void storePrepared(SettingsTest test) {
  // Record plus room for one unknown trailing field.
  constexpr std::size_t kExtraBytes = 4;
  std::array<uint8_t, kRecordBytes + kExtraBytes> buffer{};
  const std::span<uint8_t, kRecordBytes> record(buffer.data(), kRecordBytes);
  encode(sampleModel(), record);

  switch (test) {
    case SettingsTest::kCorruptRecord:
      buffer[kRecordBytes - 1] ^= 0xFF;  // payload changed, CRC kept
      return storeRaw(record, "sample with a wrong check value");
    case SettingsTest::kUnknownVersion:
      seal(record, cfg::kSettingsFormatVersion + 1);
      return storeRaw(record, "sample with format version + 1");
    case SettingsTest::kShorterRecord: {
      // Without debug_status_interval_s (the last field, 2 bytes).
      const auto shorter = record.first(kRecordBytes - 2);
      seal(shorter);
      return storeRaw(shorter, "sample without its last field");
    }
    case SettingsTest::kLongerRecord: {
      const std::span<uint8_t> longer(buffer);
      for (std::size_t i = kRecordBytes; i < longer.size(); ++i)
        longer[i] = 0xA5;
      seal(longer);
      return storeRaw(longer, "sample with an unknown trailing field");
    }
    default: return;
  }
}

void logNvsStats(const char* when) {
  nvs_stats_t stats = {};
  const esp_err_t err = nvs_get_stats(nullptr, &stats);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "NVS stats %s: %s", when, esp_err_to_name(err));
    return;
  }
  ESP_LOGI(kTag, "NVS entries %s: used %u, available %u, total %u", when,
           static_cast<unsigned>(stats.used_entries),
           static_cast<unsigned>(stats.available_entries),
           static_cast<unsigned>(stats.total_entries));
}

esp_err_t eraseFillNamespace() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kFillNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_erase_all(handle);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

// Writes values of decreasing size until not even one entry fits.
void fillNvs() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kFillNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "fill: open failed: %s", esp_err_to_name(err));
    return;
  }
  static constexpr std::size_t kBlobSizes[] = {1000, 100};
  static const std::array<uint8_t, 1000> filler{};
  unsigned written = 0;
  char key[16];
  for (const std::size_t size : kBlobSizes) {
    do {
      snprintf(key, sizeof(key), "f%u", written);
      err = nvs_set_blob(handle, key, filler.data(), size);
      if (err == ESP_OK) ++written;
    } while (err == ESP_OK);
  }
  do {
    snprintf(key, sizeof(key), "f%u", written);
    err = nvs_set_u8(handle, key, 0);
    if (err == ESP_OK) ++written;
  } while (err == ESP_OK);
  nvs_commit(handle);
  nvs_close(handle);
  ESP_LOGI(kTag, "fill: %u values written, last result %s", written,
           esp_err_to_name(err));
}

void runNvsFull() {
  eraseFillNamespace();  // leftovers of an interrupted run
  logNvsStats("before fill");
  fillNvs();
  logNvsStats("after fill");

  const uint32_t before = generation();
  const Model old_model = current();
  const esp_err_t err = save(sampleModel());
  const bool unchanged = generation() == before && current() == old_model;
  if (err != ESP_OK && unchanged) {
    ESP_LOGI(kTag, "PASS: save failed (%s), settings unchanged",
             esp_err_to_name(err));
  } else {
    ESP_LOGE(kTag, "FAIL: save returned %s, settings %s", esp_err_to_name(err),
             unchanged ? "unchanged" : "changed");
  }

  const esp_err_t cleanup = eraseFillNamespace();
  ESP_LOGI(kTag, "fill namespace removed: %s", esp_err_to_name(cleanup));
  logNvsStats("after cleanup");
}

void runSaveLoop() {
  ESP_LOGW(kTag,
           "save loop: %u saves every %u ms; cut the power at any time, the "
           "next boot must load interval %u or %u s",
           static_cast<unsigned>(kSaveLoopCount),
           static_cast<unsigned>(kSaveLoopPeriodMs),
           static_cast<unsigned>(kLoopIntervalsS[0]),
           static_cast<unsigned>(kLoopIntervalsS[1]));
  Model model = sampleModel();
  for (uint32_t i = 0; i < kSaveLoopCount; ++i) {
    model.debug_status_interval_s = kLoopIntervalsS[i % 2];
    const esp_err_t err = save(model);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "save loop: save %u failed: %s", static_cast<unsigned>(i),
               esp_err_to_name(err));
      return;
    }
    if ((i + 1) % 100 == 0)
      ESP_LOGI(kTag, "save loop: %u saves", static_cast<unsigned>(i + 1));
    vTaskDelay(pdMS_TO_TICKS(kSaveLoopPeriodMs));
  }
  ESP_LOGI(kTag, "save loop finished");
}

}  // namespace

void runDeviceTest() {
  if constexpr (cfg::kSettingsTest != SettingsTest::kNone) {
    ESP_LOGW(kTag, "device test %u active (app_config.h kSettingsTest)",
             static_cast<unsigned>(cfg::kSettingsTest));
    switch (cfg::kSettingsTest) {
      case SettingsTest::kSaveSample: {
        const esp_err_t err = save(sampleModel());
        ESP_LOGW(kTag, "sample saved: %s; press RST and check the boot log",
                 esp_err_to_name(err));
        break;
      }
      case SettingsTest::kReset: reset(); break;
      case SettingsTest::kNvsFull: runNvsFull(); break;
      case SettingsTest::kSaveLoop: runSaveLoop(); break;
      default: storePrepared(cfg::kSettingsTest); break;
    }
    logCurrent("after device test");
  }
}

}  // namespace settings
