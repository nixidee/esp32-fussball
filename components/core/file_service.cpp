#include "file_service.h"

#include <algorithm>
#include <array>
#include <atomic>

#include "app_config.h"
#include "dirent.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_service_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

// LittleFS stores names up to CONFIG_LITTLEFS_OBJ_NAME_LEN - 1 characters.
static_assert(cfg::kFileNameMaxChars < CONFIG_LITTLEFS_OBJ_NAME_LEN);

namespace files {

namespace {

constexpr const char* kTag = "files";
// Stack buffer of the erased check (main task at boot).
constexpr std::size_t kEraseCheckChunkBytes = 256;

const esp_partition_t* fs_partition = nullptr;
std::atomic<State> current_state{State::kNotStarted};
bool initialised_at_boot = false;

// Serialises mount, unmount, format and usage queries.
StaticSemaphore_t mutex_buffer;
SemaphoreHandle_t mutex = nullptr;

class Lock {
 public:
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

esp_err_t mountLocked() {
  // Value-initialised: the optional sources (SD card, block device) and the
  // flags not set here stay off. Never format on a failed mount.
  esp_vfs_littlefs_conf_t conf{};
  conf.base_path = cfg::kFsBasePath;
  conf.partition = fs_partition;
  conf.format_if_mount_failed = false;
  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  current_state.store(err == ESP_OK ? State::kMounted : State::kUnavailable,
                      std::memory_order_release);
  return err;
}

esp_err_t unmountLocked() {
  if (current_state.load(std::memory_order_acquire) != State::kMounted) {
    return ESP_OK;
  }
  const esp_err_t err = esp_vfs_littlefs_unregister_partition(fs_partition);
  if (err == ESP_OK) {
    current_state.store(State::kUnavailable, std::memory_order_release);
  }
  return err;
}

esp_err_t formatLocked() {
  esp_err_t err = unmountLocked();
  if (err != ESP_OK) return err;
  err = esp_littlefs_format_partition(fs_partition);
  if (err != ESP_OK) return err;
  return mountLocked();
}

// Reads the whole partition; erased is true only if every byte is 0xFF.
esp_err_t readErased(bool& erased) {
  std::array<uint8_t, kEraseCheckChunkBytes> chunk;
  for (std::size_t offset = 0; offset < fs_partition->size;
       offset += chunk.size()) {
    const std::size_t length =
        std::min<std::size_t>(chunk.size(), fs_partition->size - offset);
    const esp_err_t err =
        esp_partition_read(fs_partition, offset, chunk.data(), length);
    if (err != ESP_OK) return err;
    if (!isErased({chunk.data(), length})) {
      erased = false;
      return ESP_OK;
    }
  }
  erased = true;
  return ESP_OK;
}

// Regular files in the (flat) root directory; -1 if it cannot be read.
int countFiles() {
  DIR* dir = opendir(cfg::kFsBasePath);
  if (dir == nullptr) return -1;
  int count = 0;
  while (const dirent* entry = readdir(dir)) {
    if (entry->d_type == DT_REG) ++count;
  }
  closedir(dir);
  return count;
}

void logMounted(const char* how) {
  size_t total = 0;
  size_t used = 0;
  esp_littlefs_partition_info(fs_partition, &total, &used);
  ESP_LOGI(kTag, "%s at %s: %d files, %u of %u B used", how, cfg::kFsBasePath,
           countFiles(), static_cast<unsigned>(used),
           static_cast<unsigned>(total));
}

// Mount failed: initialise only verified-empty storage, keep anything else.
void handleMountFailure(esp_err_t mount_err) {
  const int64_t start_us = esp_timer_get_time();
  bool erased = false;
  const esp_err_t read_err = readErased(erased);
  const auto check_ms =
      static_cast<unsigned>((esp_timer_get_time() - start_us) / 1000);
  if (read_err != ESP_OK) {
    ESP_LOGE(kTag,
             "mount failed (%s) and the partition could not be read (%s); "
             "nothing changed, files unavailable",
             esp_err_to_name(mount_err), esp_err_to_name(read_err));
    return;
  }
  if (!erased) {
    ESP_LOGE(kTag,
             "mount failed (%s); the partition holds data and is kept "
             "unchanged (checked in %u ms). Files unavailable until reset.",
             esp_err_to_name(mount_err), check_ms);
    return;
  }
  ESP_LOGW(kTag,
           "partition erased (checked in %u ms): initialising (first use)",
           check_ms);
  const esp_err_t err = formatLocked();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "first-use initialisation failed: %s", esp_err_to_name(err));
    return;
  }
  initialised_at_boot = true;
  logMounted("initialised and mounted");
}

}  // namespace

void init() {
  mutex = xSemaphoreCreateMutexStatic(&mutex_buffer);
  fs_partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, nullptr);
  if (fs_partition == nullptr) {
    current_state.store(State::kUnavailable, std::memory_order_release);
    ESP_LOGE(kTag, "no littlefs data partition, files unavailable");
    return;
  }
  Lock lock;
  const esp_err_t err = mountLocked();
  if (err == ESP_OK) {
    logMounted("mounted");
    return;
  }
  handleMountFailure(err);
}

State state() { return current_state.load(std::memory_order_acquire); }

const char* stateName(State state) {
  switch (state) {
    case State::kNotStarted: return "not started";
    case State::kMounted: return "mounted";
    case State::kUnavailable: return "unavailable";
  }
  return "unknown";
}

bool initialisedAtBoot() { return initialised_at_boot; }

esp_err_t usage(Usage& out) {
  if (mutex == nullptr) return ESP_ERR_INVALID_STATE;
  Lock lock;
  if (state() != State::kMounted) return ESP_ERR_INVALID_STATE;
  size_t total = 0;
  size_t used = 0;
  const esp_err_t err =
      esp_littlefs_partition_info(fs_partition, &total, &used);
  if (err == ESP_OK) out = {total, used};
  return err;
}

esp_err_t format() {
  if (fs_partition == nullptr) return ESP_ERR_INVALID_STATE;
  esp_err_t err;
  {
    Lock lock;
    err = formatLocked();
  }
  if (err == ESP_OK) {
    ESP_LOGW(kTag, "formatted: all files deleted");
  } else {
    ESP_LOGE(kTag, "format failed: %s", esp_err_to_name(err));
  }
  return err;
}

void logStatus() {
  Usage use{};
  if (usage(use) == ESP_OK) {
    ESP_LOGI(kTag, "mounted, %u of %u B used",
             static_cast<unsigned>(use.used_bytes),
             static_cast<unsigned>(use.total_bytes));
  } else {
    ESP_LOGI(kTag, "%s", stateName(state()));
  }
}

namespace internal {

const esp_partition_t* partition() { return fs_partition; }

esp_err_t unmount() {
  if (mutex == nullptr) return ESP_ERR_INVALID_STATE;
  Lock lock;
  return unmountLocked();
}

}  // namespace internal

}  // namespace files
