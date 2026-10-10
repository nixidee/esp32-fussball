#include "ota_service.h"

#include <atomic>
#include <cstring>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "hw_target.h"

extern "C" __attribute__((used, section(".rodata_custom_desc")))
const ota::Identity fussball_image_identity = {0x46424c31, 1,
#if defined(FUSSBALL_TARGET_XIAO_ESP32C6_GC9A01)
                                               "xiao_esp32c6_gc9a01",
                                               "4mb_ota_littlefs_v1",
#elif defined(FUSSBALL_TARGET_WAVESHARE_ESP32S3_LCD128)
                                               "waveshare_esp32s3_lcd128",
                                               "16mb_ota_littlefs_v1",
#else
#error "OTA identity is required for every target"
#endif
                                               cfg::kSettingsFormatVersion};
namespace ota {
namespace {
const esp_partition_t* partition = nullptr;
esp_ota_handle_t handle = 0;
std::atomic<bool> pending{false};
bool writing = false, startup_healthy = true;
int64_t boot_at = 0;
std::atomic<int64_t> restart_at{0};
}  // namespace
const Identity& identity() { return fussball_image_identity; }
void init(bool healthy) {
  boot_at = esp_timer_get_time() / 1000;
  esp_ota_img_states_t state;
  pending = esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                        &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY;
  if (pending && !healthy) {
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();
  }
}
bool trial() { return pending; }
void startupHealth(bool healthy) { startup_healthy = healthy; }
std::size_t capacity() {
  const auto* next = esp_ota_get_next_update_partition(nullptr);
  return next ? next->size : 0;
}
void reboot() { restart_at.store(esp_timer_get_time() / 1000 + 500); }
void poll(bool healthy) {
  const int64_t now = esp_timer_get_time() / 1000;
  if (restart_at.load() != 0 && now >= restart_at.load()) esp_restart();
  if (pending && now - boot_at >= cfg::kOtaLocalHealthMs) {
    if (!healthy || !startup_healthy) {
      esp_ota_mark_app_invalid_rollback_and_reboot();
      esp_restart();
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      pending = false;
      events::post(events::Event::kOtaState);
    } else
      esp_restart();
  }
}
esp_err_t begin(const uint8_t* bytes, std::size_t count, std::size_t total) {
  if (writing || pending) return ESP_ERR_INVALID_STATE;
  constexpr std::size_t kAppOffset = 32;
  constexpr std::size_t kCustomOffset = kAppOffset + sizeof(esp_app_desc_t);
  if (count < kCustomOffset + sizeof(Identity) || total < count)
    return ESP_ERR_INVALID_SIZE;
  esp_app_desc_t app{};
  esp_image_header_t image{};
  memcpy(&image, bytes, sizeof(image));
  Identity custom{};
  memcpy(&app, bytes + kAppOffset, sizeof(app));
  memcpy(&custom, bytes + kCustomOffset, sizeof(custom));
  if (image.magic != ESP_IMAGE_HEADER_MAGIC ||
      image.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
      app.magic_word != ESP_APP_DESC_MAGIC_WORD ||
      memcmp(app.project_name, esp_app_get_description()->project_name,
             sizeof(app.project_name)) != 0 ||
      custom.magic != identity().magic ||
      custom.version != identity().version ||
      custom.settings_format != identity().settings_format ||
      memcmp(custom.target, identity().target, sizeof(custom.target)) != 0 ||
      memcmp(custom.layout, identity().layout, sizeof(custom.layout)) != 0)
    return ESP_ERR_INVALID_RESPONSE;
  partition = esp_ota_get_next_update_partition(nullptr);
  if (partition == nullptr || total > partition->size)
    return ESP_ERR_INVALID_SIZE;
  const auto err = esp_ota_begin(partition, total, &handle);
  if (err == ESP_OK) {
    writing = true;
    events::post(events::Event::kOtaState);
  }
  return err;
}
esp_err_t write(const uint8_t* bytes, std::size_t count) {
  return writing ? esp_ota_write(handle, bytes, count) : ESP_ERR_INVALID_STATE;
}
esp_err_t finish() {
  if (!writing) return ESP_ERR_INVALID_STATE;
  auto err = esp_ota_end(handle);
  writing = false;
  if (err == ESP_OK) err = esp_ota_set_boot_partition(partition);
  events::post(events::Event::kOtaState);
  return err;
}
void abort() {
  if (writing) {
    esp_ota_abort(handle);
    writing = false;
  }
}
}  // namespace ota
