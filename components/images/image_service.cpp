#include "image_service.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "display_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "file_service.h"
#include "hw_target.h"
#include "ota_service.h"
#include "settings_store.h"
namespace images {
namespace {
constexpr const char* kTag = "images";
FILE* temporary = nullptr;
uint8_t destination = 0;
std::size_t received = 0, expected = 0;
std::atomic<uint32_t> changed{0};
// Per library image (index id - 1): increases whenever the file at that id is
// replaced, deleted or wiped, so '?r=' URLs never show a stale image.
std::atomic<uint16_t> revisions[cfg::kImageLibraryMax];
bool libraryId(uint8_t id) { return id >= 1 && id <= cfg::kImageLibraryMax; }
void bump(uint8_t id) {
  changed.fetch_add(1);
  if (libraryId(id)) revisions[id - 1].fetch_add(1);
}
// File name inside the file system: the library file or the stock file.
void name(uint8_t id, cfg::StockImage stock, char* out, std::size_t size) {
  if (id == cfg::kStockImageId)
    snprintf(out, size, "%s", cfg::kStockImageFiles[stock]);
  else
    snprintf(out, size, "img%u.jpg", static_cast<unsigned>(id));
}
void path(const char* file, char* out, std::size_t size) {
  files::buildPath(cfg::kFsBasePath, file, {out, size});
}
void libraryPath(uint8_t id, char* out, std::size_t size) {
  char file[32];
  name(id, cfg::kStockCrest, file, sizeof(file));
  path(file, out, size);
}
void tempPath(char* out, std::size_t size) {
  snprintf(out, size, "%s/.image_upload", cfg::kFsBasePath);
}
std::size_t bytes(const char* p) {
  struct stat s{};
  return stat(p, &s) == 0 ? static_cast<std::size_t>(s.st_size) : 0;
}
// Deletes one library file under the display lock, after LVGL dropped it from
// its image cache.
esp_err_t unlinkImage(uint8_t id) {
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  char p[96], src[32];
  libraryPath(id, p, sizeof(p));
  snprintf(src, sizeof(src), "A:img%u.jpg", static_cast<unsigned>(id));
  if (rendering) display::dropImageCache(src);
  const bool exists = bytes(p) > 0;
  const auto err = !exists || unlink(p) == 0 ? ESP_OK : ESP_FAIL;
  if (err == ESP_OK && exists) bump(id);
  if (rendering) display::unlock();
  return err;
}
}  // namespace
Entry info(uint8_t id) {
  if (!libraryId(id)) return {};
  char p[96];
  libraryPath(id, p, sizeof(p));
  const auto size = bytes(p);
  return {size, size > 0};
}
settings::ImageMask present() {
  settings::ImageMask mask = 0;
  for (uint8_t id = 1; id <= cfg::kImageLibraryMax; ++id)
    if (info(id).present) mask |= settings::ImageMask{1} << id;
  return mask;
}
Usage usage() {
  Usage total{};
  for (uint8_t id = 1; id <= cfg::kImageLibraryMax; ++id) {
    const auto e = info(id);
    if (!e.present) continue;
    ++total.count;
    total.bytes += e.bytes;
  }
  return total;
}
uint16_t revisionOf(uint8_t id) {
  if (!libraryId(id)) return 0;
  // Random per boot: a '?r=' URL a browser cached during an earlier run must
  // not match a different image after the counters restart.
  static const uint16_t offset = static_cast<uint16_t>(esp_random());
  return static_cast<uint16_t>(offset + revisions[id - 1].load());
}
bool filePath(uint8_t id, cfg::StockImage stock, char* out,
              std::size_t capacity) {
  if (id != cfg::kStockImageId && !info(id).present) return false;
  char file[32], p[96];
  name(id, stock, file, sizeof(file));
  path(file, p, sizeof(p));
  const int count = snprintf(out, capacity, "%s", p);
  return count >= 0 && count < static_cast<int>(capacity);
}
bool source(uint8_t id, cfg::StockImage stock, char* out,
            std::size_t capacity) {
  if (id != cfg::kStockImageId && !info(id).present) return false;
  char file[32];
  name(id, stock, file, sizeof(file));
  const int count = snprintf(out, capacity, "A:%s", file);
  return count >= 0 && count < static_cast<int>(capacity);
}
esp_err_t begin(uint8_t id, std::size_t size) {
  if (temporary != nullptr || ota::trial() ||
      files::state() != files::State::kMounted)
    return ESP_ERR_INVALID_STATE;
  if (id != cfg::kStockImageId && !libraryId(id)) return ESP_ERR_INVALID_ARG;
  if (size == 0 || size > cfg::kImageMaxBytes) return ESP_ERR_INVALID_SIZE;
  // Recover only this service's unpublished file after an interrupted upload.
  char stale[96];
  tempPath(stale, sizeof(stale));
  unlink(stale);
  std::size_t total = 0;
  uint8_t lowest_free = 0;
  for (uint8_t i = 1; i <= cfg::kImageLibraryMax; ++i) {
    const auto e = info(i);
    if (!e.present && lowest_free == 0) lowest_free = i;
    if (e.present && i != id) total += e.bytes;
  }
  if (id == cfg::kStockImageId) {
    if (lowest_free == 0) return ESP_ERR_NO_MEM;
    id = lowest_free;
  } else if (!info(id).present) {
    return ESP_ERR_NOT_FOUND;
  }
  files::Usage usage{};
  if (total + size > cfg::kImageTotalBytes || files::usage(usage) != ESP_OK ||
      usage.used_bytes + size + cfg::kImageReserveBytes > usage.total_bytes)
    return ESP_ERR_NO_MEM;
  char p[96];
  tempPath(p, sizeof(p));
  temporary = fopen(p, "wb");
  if (temporary == nullptr) return ESP_FAIL;
  destination = id;
  received = 0;
  expected = size;
  return ESP_OK;
}
uint8_t uploadedId() { return destination; }
esp_err_t write(const uint8_t* buffer, std::size_t size) {
  if (temporary == nullptr || received + size > expected)
    return ESP_ERR_INVALID_SIZE;
  if (fwrite(buffer, 1, size, temporary) != size) return ESP_FAIL;
  received += size;
  return ESP_OK;
}
void abort() {
  if (temporary != nullptr) {
    fclose(temporary);
    temporary = nullptr;
  }
  char p[96];
  tempPath(p, sizeof(p));
  unlink(p);
}
esp_err_t finish(int64_t deadline_ms) {
  if (temporary == nullptr) return ESP_ERR_INVALID_STATE;
  const int synced = fflush(temporary);
  const int closed = fclose(temporary);
  temporary = nullptr;
  char temp[96], target[96], src[32];
  tempPath(temp, sizeof(temp));
  libraryPath(destination, target, sizeof(target));
  if (received != expected || synced != 0 || closed != 0) {
    abort();
    return ESP_FAIL;
  }
  auto err = display::validateJpeg(temp, hw::kDisplay.width,
                                   hw::kDisplay.height, deadline_ms);
  if (esp_timer_get_time() / 1000 >= deadline_ms) err = ESP_ERR_TIMEOUT;
  const bool rendering = display::ready();
  if (err == ESP_OK && rendering &&
      !display::lock(cfg::kBootScreenLockTimeoutMs))
    err = ESP_ERR_TIMEOUT;
  else if (err == ESP_OK) {
    snprintf(src, sizeof(src), "A:img%u.jpg", static_cast<unsigned>(destination));
    if (rendering) display::dropImageCache(src);
    err = rename(temp, target) == 0 ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) bump(destination);
    if (rendering) display::unlock();
  }
  if (err != ESP_OK) abort();
  return err;
}
esp_err_t remove(uint8_t id) {
  if (!libraryId(id)) return ESP_ERR_INVALID_ARG;
  if (ota::trial()) return ESP_ERR_INVALID_STATE;
  // References first: a failed save keeps the file, so no reference is left
  // pointing to a deleted image.
  auto model = settings::current();
  const settings::ImageMask others = present() & ~(settings::ImageMask{1} << id);
  if (settings::dropMissingImages(model, others)) {
    const auto err = settings::save(model);
    if (err != ESP_OK) return err;
  }
  return unlinkImage(id);
}
esp_err_t removeAll() {
  if (ota::trial()) return ESP_ERR_INVALID_STATE;
  for (uint8_t id = 1; id <= cfg::kImageLibraryMax; ++id) {
    const auto err = unlinkImage(id);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}
bool maintain() {
  if (ota::trial() || files::state() != files::State::kMounted) return false;
  for (const auto& legacy : cfg::kLegacyImageNames) {
    char file[32], p[96];
    snprintf(file, sizeof(file), "%s.jpg", legacy);
    path(file, p, sizeof(p));
    if (unlink(p) == 0) {
      ESP_LOGI(kTag, "removed legacy image %s", file);
      changed.fetch_add(1);
    }
  }
  auto model = settings::current();
  if (settings::dropMissingImages(model, present())) {
    const auto err = settings::save(model);
    if (err == ESP_OK)
      ESP_LOGI(kTag, "removed references to missing images");
    else
      ESP_LOGW(kTag, "references to missing images kept: %s",
               esp_err_to_name(err));
  }
  return true;
}
uint32_t revision() { return changed.load(); }
esp_err_t format() {
  if (ota::trial() || temporary != nullptr) return ESP_ERR_INVALID_STATE;
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  if (rendering) display::dropImageCache(nullptr);
  const auto err = files::format();
  if (err == ESP_OK)
    for (uint8_t id = 1; id <= cfg::kImageLibraryMax; ++id) bump(id);
  if (rendering) display::unlock();
  return err;
}
}  // namespace images
