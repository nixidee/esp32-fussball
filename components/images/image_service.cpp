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
// Writable files ("slots"): library ids 1..kImageLibraryMax, then one
// replacement per stock role.
constexpr uint8_t kSlots = cfg::kImageLibraryMax + cfg::kStockImageCount;
FILE* temporary = nullptr;
uint8_t destination = 0;  // slot of the running upload or copy
std::size_t received = 0, expected = 0;
std::atomic<uint32_t> changed{0};
// Per slot (index slot - 1): increases whenever its file is replaced,
// deleted or wiped, so '?r=' URLs never show a stale image.
std::atomic<uint16_t> revisions[kSlots];
bool libraryId(uint8_t id) { return id >= 1 && id <= cfg::kImageLibraryMax; }
bool stockRole(cfg::StockImage stock) { return stock < cfg::kStockImageCount; }
uint8_t roleSlot(cfg::StockImage stock) {
  return static_cast<uint8_t>(cfg::kImageLibraryMax + 1 + stock);
}
void bump(uint8_t slot) {
  changed.fetch_add(1);
  if (slot >= 1 && slot <= kSlots) revisions[slot - 1].fetch_add(1);
}
uint16_t slotRevision(uint8_t slot) {
  // Random per boot: a '?r=' URL a browser cached during an earlier run must
  // not match a different image after the counters restart.
  static const uint16_t offset = static_cast<uint16_t>(esp_random());
  return static_cast<uint16_t>(offset + revisions[slot - 1].load());
}
void slotFile(uint8_t slot, char* out, std::size_t size) {
  if (libraryId(slot))
    snprintf(out, size, "img%u.jpg", static_cast<unsigned>(slot));
  else
    snprintf(out, size, "%s",
             cfg::kRoleImageFiles[slot - cfg::kImageLibraryMax - 1]);
}
void path(const char* file, char* out, std::size_t size) {
  files::buildPath(cfg::kFsBasePath, file, {out, size});
}
void slotPath(uint8_t slot, char* out, std::size_t size) {
  char file[32];
  slotFile(slot, file, sizeof(file));
  path(file, out, size);
}
void tempPath(char* out, std::size_t size) {
  snprintf(out, size, "%s/.image_upload", cfg::kFsBasePath);
}
std::size_t bytes(const char* p) {
  struct stat s{};
  return stat(p, &s) == 0 ? static_cast<std::size_t>(s.st_size) : 0;
}
std::size_t slotBytes(uint8_t slot) {
  char p[96];
  slotPath(slot, p, sizeof(p));
  return bytes(p);
}
// File a role shows: its replacement when one exists, else the factory file.
void roleFile(cfg::StockImage stock, char* out, std::size_t size) {
  if (slotBytes(roleSlot(stock)) > 0)
    slotFile(roleSlot(stock), out, size);
  else
    snprintf(out, size, "%s", cfg::kStockImageFiles[stock]);
}
// File name of a settings reference: a library file or, for id 0, the file
// of the stock role.
void name(uint8_t id, cfg::StockImage stock, char* out, std::size_t size) {
  if (id == cfg::kStockImageId)
    roleFile(stock, out, size);
  else
    slotFile(id, out, size);
}
void dropCache(uint8_t slot) {
  char file[32], src[40];
  slotFile(slot, file, sizeof(file));
  snprintf(src, sizeof(src), "A:%s", file);
  display::dropImageCache(src);
}
// Deletes one slot file under the display lock, after LVGL dropped it from
// its image cache.
esp_err_t unlinkImage(uint8_t slot) {
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  char p[96];
  slotPath(slot, p, sizeof(p));
  if (rendering) dropCache(slot);
  const bool exists = bytes(p) > 0;
  const auto err = !exists || unlink(p) == 0 ? ESP_OK : ESP_FAIL;
  if (err == ESP_OK && exists) bump(slot);
  if (rendering) display::unlock();
  return err;
}
// Opens the temporary file for 'slot'. 'budget' checks the image budget
// without the file the slot holds now; the storage reserve is always
// checked, with the old file still counted.
esp_err_t start(uint8_t slot, std::size_t size, bool budget) {
  if (temporary != nullptr || ota::trial() ||
      files::state() != files::State::kMounted)
    return ESP_ERR_INVALID_STATE;
  if (size == 0 || size > cfg::kImageMaxBytes) return ESP_ERR_INVALID_SIZE;
  // Recover only this service's unpublished file after an interrupted upload.
  char p[96];
  tempPath(p, sizeof(p));
  unlink(p);
  std::size_t total = 0;
  for (uint8_t i = 1; i <= kSlots; ++i)
    if (i != slot) total += slotBytes(i);
  files::Usage usage{};
  if ((budget && total + size > cfg::kImageTotalBytes) ||
      files::usage(usage) != ESP_OK ||
      usage.used_bytes + size + cfg::kImageReserveBytes > usage.total_bytes)
    return ESP_ERR_NO_MEM;
  temporary = fopen(p, "wb");
  if (temporary == nullptr) return ESP_FAIL;
  destination = slot;
  received = 0;
  expected = size;
  return ESP_OK;
}
// Closes the temporary file; ESP_OK when every expected byte was written.
esp_err_t close() {
  if (temporary == nullptr) return ESP_ERR_INVALID_STATE;
  const int synced = fflush(temporary);
  const int closed = fclose(temporary);
  temporary = nullptr;
  return received == expected && synced == 0 && closed == 0 ? ESP_OK
                                                            : ESP_FAIL;
}
// Moves the closed temporary file onto its slot under the display lock.
esp_err_t publish() {
  char temp[96], target[96];
  tempPath(temp, sizeof(temp));
  slotPath(destination, target, sizeof(target));
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  if (rendering) dropCache(destination);
  const auto err = rename(temp, target) == 0 ? ESP_OK : ESP_FAIL;
  if (err == ESP_OK) bump(destination);
  if (rendering) display::unlock();
  return err;
}
// Copies library image 'id' into the slot of 'stock'.
esp_err_t copy(uint8_t id, cfg::StockImage stock, bool budget) {
  if (!libraryId(id) || !stockRole(stock)) return ESP_ERR_INVALID_ARG;
  char from[96];
  slotPath(id, from, sizeof(from));
  const auto size = bytes(from);
  if (size == 0) return ESP_ERR_NOT_FOUND;
  auto err = start(roleSlot(stock), size, budget);
  if (err != ESP_OK) return err;
  FILE* in = fopen(from, "rb");
  if (in == nullptr) err = ESP_FAIL;
  uint8_t buffer[512];
  while (err == ESP_OK) {
    const auto count = fread(buffer, 1, sizeof(buffer), in);
    if (count == 0) break;
    err = write(buffer, count);
  }
  if (in != nullptr) fclose(in);
  if (err == ESP_OK) err = close();
  // The library file passed the JPEG check when it was uploaded.
  if (err == ESP_OK) err = publish();
  if (err != ESP_OK) abort();
  return err;
}
}  // namespace
Entry info(uint8_t id) {
  if (!libraryId(id)) return {};
  const auto size = slotBytes(id);
  return {size, size > 0};
}
Role role(cfg::StockImage stock) {
  if (!stockRole(stock)) return {};
  const auto size = slotBytes(roleSlot(stock));
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
  for (uint8_t i = 0; i < cfg::kStockImageCount; ++i)
    total.bytes += role(static_cast<cfg::StockImage>(i)).bytes;
  return total;
}
uint16_t revisionOf(uint8_t id) {
  return libraryId(id) ? slotRevision(id) : 0;
}
uint16_t roleRevision(cfg::StockImage stock) {
  return stockRole(stock) ? slotRevision(roleSlot(stock)) : 0;
}
bool filePath(uint8_t id, cfg::StockImage stock, char* out,
              std::size_t capacity) {
  if (id == cfg::kStockImageId ? !stockRole(stock) : !info(id).present)
    return false;
  char file[32], p[96];
  name(id, stock, file, sizeof(file));
  path(file, p, sizeof(p));
  const int count = snprintf(out, capacity, "%s", p);
  return count >= 0 && count < static_cast<int>(capacity);
}
bool source(uint8_t id, cfg::StockImage stock, char* out,
            std::size_t capacity) {
  if (id == cfg::kStockImageId ? !stockRole(stock) : !info(id).present)
    return false;
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
  if (id == cfg::kStockImageId) {
    for (uint8_t i = 1; i <= cfg::kImageLibraryMax && id == 0; ++i)
      if (!info(i).present) id = i;
    if (id == cfg::kStockImageId) return ESP_ERR_NO_MEM;
  } else if (!info(id).present) {
    return ESP_ERR_NOT_FOUND;
  }
  return start(id, size, true);
}
esp_err_t beginRole(cfg::StockImage stock, std::size_t size) {
  if (!stockRole(stock)) return ESP_ERR_INVALID_ARG;
  return start(roleSlot(stock), size, true);
}
uint8_t uploadedId() {
  return libraryId(destination) ? destination : cfg::kStockImageId;
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
  auto err = close();
  if (err == ESP_ERR_INVALID_STATE) return err;
  char temp[96];
  tempPath(temp, sizeof(temp));
  if (err == ESP_OK)
    err = display::validateJpeg(temp, hw::kDisplay.width, hw::kDisplay.height,
                                deadline_ms);
  if (err == ESP_OK && esp_timer_get_time() / 1000 >= deadline_ms)
    err = ESP_ERR_TIMEOUT;
  if (err == ESP_OK) err = publish();
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
esp_err_t copyToRole(uint8_t id, cfg::StockImage stock) {
  if (ota::trial()) return ESP_ERR_INVALID_STATE;
  return copy(id, stock, true);
}
esp_err_t resetRole(cfg::StockImage stock) {
  if (!stockRole(stock)) return ESP_ERR_INVALID_ARG;
  if (ota::trial() || temporary != nullptr) return ESP_ERR_INVALID_STATE;
  return unlinkImage(roleSlot(stock));
}
esp_err_t removeAll() {
  if (ota::trial() || temporary != nullptr) return ESP_ERR_INVALID_STATE;
  for (uint8_t slot = 1; slot <= kSlots; ++slot) {
    const auto err = unlinkImage(slot);
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
  // A crest reference of a format 3 to 5 record becomes the crest
  // replacement once. The copy may exceed the image budget (the file is
  // already counted in it) but not the storage reserve.
  const uint8_t crest = settings::legacyCrest();
  const bool migrate = crest != cfg::kStockImageId;
  if (migrate) {
    const auto err = info(crest).present ? copy(crest, cfg::kStockCrest, false)
                                         : ESP_ERR_NOT_FOUND;
    // Display busy or an upload running: try again on the next call.
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) return false;
    if (err == ESP_OK)
      ESP_LOGI(kTag, "crest image %u copied into the crest role",
               static_cast<unsigned>(crest));
    else
      ESP_LOGW(kTag, "crest image %u not copied (%s), it stays in the library",
               static_cast<unsigned>(crest), esp_err_to_name(err));
  }
  auto model = settings::current();
  const bool dropped = settings::dropMissingImages(model, present());
  // Saving writes the current format, so the old reference is not read again.
  if (dropped || migrate) {
    const auto err = settings::save(model);
    if (err == ESP_OK && dropped)
      ESP_LOGI(kTag, "removed references to missing images");
    else if (err != ESP_OK)
      ESP_LOGW(kTag, "settings not saved after image maintenance: %s",
               esp_err_to_name(err));
  }
  if (migrate) settings::forgetLegacyCrest();
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
    for (uint8_t slot = 1; slot <= kSlots; ++slot) bump(slot);
  if (rendering) display::unlock();
  return err;
}
}  // namespace images
