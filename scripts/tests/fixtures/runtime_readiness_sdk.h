// Host SDK boundaries for lifecycle fault injection, not an ESP implementation.
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int32_t;
inline constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101,
                           ESP_ERR_INVALID_STATE = 0x103,
                           ESP_ERR_INVALID_SIZE = 0x104,
                           ESP_ERR_INVALID_RESPONSE = 0x108,
                           ESP_ERR_NOT_FINISHED = 0x10c;
inline const char* esp_err_to_name(esp_err_t) { return "injected SDK failure"; }
struct WatchdogFailure {};
#define ESP_ERROR_CHECK(value)                   \
  if ((value) != ESP_OK) throw WatchdogFailure { \
    }
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
inline constexpr uint32_t ESP_APP_DESC_MAGIC_WORD = 0xabcd5432;
struct esp_app_desc_t {
  uint32_t magic_word;
  uint8_t before_version[12];
  char version[32], project_name[32];
  uint8_t after_project[176];
};
static_assert(sizeof(esp_app_desc_t) == 256);
static_assert(offsetof(esp_app_desc_t, version) == 16);
static_assert(offsetof(esp_app_desc_t, project_name) == 48);
const esp_app_desc_t* esp_app_get_description();
inline constexpr uint8_t ESP_IMAGE_HEADER_MAGIC = 0xe9;
inline constexpr uint16_t CONFIG_IDF_FIRMWARE_CHIP_ID = 13;
struct esp_image_header_t {
  uint8_t magic;
  uint8_t before_chip[11];
  uint16_t chip_id;
  uint8_t after_chip[10];
};
static_assert(sizeof(esp_image_header_t) == 24);
static_assert(offsetof(esp_image_header_t, chip_id) == 12);
struct esp_partition_t {
  uint32_t address, size;
};
struct esp_partition_pos_t {
  uint32_t offset, size;
};
struct esp_image_metadata_t {
  uint32_t image_len;
};
using esp_ota_handle_t = uint32_t;
enum esp_ota_img_states_t { ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID };
const esp_partition_t* esp_ota_get_running_partition();
const esp_partition_t* esp_ota_get_next_update_partition(
    const esp_partition_t*);
esp_err_t esp_image_get_metadata(const esp_partition_pos_t*,
                                 esp_image_metadata_t*);
esp_err_t esp_ota_get_state_partition(const esp_partition_t*,
                                      esp_ota_img_states_t*);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot();
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
esp_err_t esp_ota_begin(const esp_partition_t*, std::size_t, esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t, const void*, std::size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
esp_err_t esp_ota_abort(esp_ota_handle_t);
int64_t esp_timer_get_time();
void esp_restart();
