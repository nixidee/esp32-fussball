#include "image_service.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "display_port.h"
#include "esp_timer.h"
#include "file_service.h"
#include "hw_target.h"
#include "ota_service.h"
namespace images {
namespace {
constexpr auto& names = cfg::kImageNames;
FILE* temporary = nullptr;
uint8_t destination = 0;
std::size_t received = 0, expected = 0;
std::atomic<uint32_t> changed{0};
void path(uint8_t index, bool defaults, char* out, std::size_t size) {
  char name[32];
  if (defaults)
    snprintf(name, sizeof(name), "%s", cfg::kDefaultImageFiles[index]);
  else
    snprintf(name, sizeof(name), "%s.jpg", names[index]);
  files::buildPath(cfg::kFsBasePath, name, {out, size});
}
void tempPath(char* out, std::size_t size) {
  snprintf(out, size, "%s/.image_upload", cfg::kFsBasePath);
}
std::size_t bytes(const char* p) {
  struct stat s{};
  return stat(p, &s) == 0 ? static_cast<std::size_t>(s.st_size) : 0;
}
}  // namespace
Entry info(uint8_t index) {
  if (index >= cfg::kImageCount) return {};
  char p[96];
  path(index, false, p, sizeof(p));
  auto size = bytes(p);
  if (size > 0) return {names[index], size, true, true};
  path(index, true, p, sizeof(p));
  size = bytes(p);
  return {names[index], size, false, size > 0};
}
bool source(uint8_t index, char* out, std::size_t capacity) {
  if (index >= cfg::kImageCount) return false;
  auto entry = info(index);
  if (!entry.available) return false;
  const int count =
      entry.uploaded
          ? snprintf(out, capacity, "A:%s.jpg", names[index])
          : snprintf(out, capacity, "A:%s", cfg::kDefaultImageFiles[index]);
  return count >= 0 && count < static_cast<int>(capacity);
}
esp_err_t begin(uint8_t index, std::size_t size) {
  if (index >= cfg::kImageCount || temporary != nullptr || ota::trial() ||
      files::state() != files::State::kMounted)
    return ESP_ERR_INVALID_STATE;
  if (size == 0 || size > cfg::kImageMaxBytes) return ESP_ERR_INVALID_SIZE;
  // Recover only this service's unpublished file after an interrupted upload.
  char stale[96];
  tempPath(stale, sizeof(stale));
  unlink(stale);
  std::size_t total = 0;
  for (uint8_t i = 0; i < cfg::kImageCount; ++i) {
    auto e = info(i);
    if (e.uploaded && i != index) total += e.bytes;
  }
  files::Usage usage{};
  if (total + size > cfg::kImageTotalBytes || files::usage(usage) != ESP_OK ||
      usage.used_bytes + size + cfg::kImageReserveBytes > usage.total_bytes)
    return ESP_ERR_NO_MEM;
  char p[96];
  tempPath(p, sizeof(p));
  temporary = fopen(p, "wb");
  if (temporary == nullptr) return ESP_FAIL;
  destination = index;
  received = 0;
  expected = size;
  return ESP_OK;
}
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
  char temp[96], target[96], src[64];
  tempPath(temp, sizeof(temp));
  path(destination, false, target, sizeof(target));
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
    snprintf(src, sizeof(src), "A:%s.jpg", names[destination]);
    if (rendering) display::dropImageCache(src);
    err = rename(temp, target) == 0 ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) changed.fetch_add(1);
    if (rendering) display::unlock();
  }
  if (err != ESP_OK) abort();
  return err;
}
esp_err_t reset(uint8_t index) {
  if (index >= cfg::kImageCount || ota::trial()) return ESP_ERR_INVALID_ARG;
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  char p[96], src[64];
  path(index, false, p, sizeof(p));
  snprintf(src, sizeof(src), "A:%s.jpg", names[index]);
  if (rendering) display::dropImageCache(src);
  const bool exists = bytes(p) > 0;
  const auto err = !exists || unlink(p) == 0 ? ESP_OK : ESP_FAIL;
  if (err == ESP_OK) changed.fetch_add(1);
  if (rendering) display::unlock();
  return err;
}
esp_err_t resetAll() {
  for (uint8_t i = 0; i < cfg::kImageCount; ++i) {
    auto err = reset(i);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}
uint32_t revision() { return changed.load(); }
esp_err_t format() {
  if (ota::trial() || temporary != nullptr) return ESP_ERR_INVALID_STATE;
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  if (rendering) display::dropImageCache(nullptr);
  const auto err = files::format();
  if (err == ESP_OK) changed.fetch_add(1);
  if (rendering) display::unlock();
  return err;
}
}  // namespace images
