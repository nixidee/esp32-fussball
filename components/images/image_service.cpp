#include "image_service.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "display_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "file_service.h"
#include "freertos/FreeRTOS.h"
#include "hw_target.h"
#include "network_service.h"
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
// App task only: a failed maintenance write is retried without a tight loop.
int64_t maintenance_retry_at = 0;
portMUX_TYPE selection_mux = portMUX_INITIALIZER_UNLOCKED;
Status selected{};
// App-task resolution cursor; the public snapshot is copied under the mux.
uint8_t next_candidate = 0;
bool resolving = false, cycle_failed = false;
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
std::size_t bytes(const char* p, esp_err_t* error = nullptr) {
  struct stat s{};
  const int result = stat(p, &s);
  if (error != nullptr)
    *error = result == 0       ? ESP_OK
             : errno == ENOENT ? ESP_ERR_NOT_FOUND
                               : ESP_FAIL;
  return result == 0 ? static_cast<std::size_t>(s.st_size) : 0;
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
void publishSelection(const Status& value) {
  portENTER_CRITICAL(&selection_mux);
  selected = value;
  portEXIT_CRITICAL(&selection_mux);
}
FileSource candidate(const Status& value, uint8_t index) {
  if (value.desired_id != cfg::kStockImageId) {
    if (index == 0) return FileSource::kLibrary;
    --index;
  }
  return index == 0   ? FileSource::kReplacement
         : index == 1 ? FileSource::kFactory
                      : FileSource::kNone;
}
uint8_t candidateIndex(const Status& value, FileSource source) {
  if (source == FileSource::kLibrary) return 0;
  return static_cast<uint8_t>((value.desired_id != cfg::kStockImageId ? 1 : 0) +
                              (source == FileSource::kFactory ? 1 : 0));
}
bool selectionName(const Status& value, FileSource source, char* out,
                   std::size_t capacity) {
  if (!stockRole(value.role) || source == FileSource::kNone) return false;
  if (source == FileSource::kLibrary && !libraryId(value.desired_id))
    return false;
  if (source == FileSource::kFactory) {
    const int count =
        snprintf(out, capacity, "%s", cfg::kStockImageFiles[value.role]);
    return count >= 0 && count < static_cast<int>(capacity);
  }
  slotFile(
      source == FileSource::kLibrary ? value.desired_id : roleSlot(value.role),
      out, capacity);
  return true;
}
bool candidatePath(const Status& value, FileSource source, char* out,
                   std::size_t capacity) {
  char file[32];
  return selectionName(value, source, file, sizeof(file)) &&
         files::buildPath(cfg::kFsBasePath, file, {out, capacity});
}
void selectionSource(const Status& value, char* out, std::size_t capacity) {
  if (out == nullptr || capacity == 0) return;
  out[0] = 0;
  char file[32];
  if (value.active && selectionName(value, value.actual, file, sizeof(file))) {
    const int count = snprintf(out, capacity, "A:%s", file);
    if (count < 0 || count >= static_cast<int>(capacity)) out[0] = 0;
  }
}
void failure(Status& value, FileSource source, Failure why, esp_err_t error,
             bool replace = false) {
  // Keep the first cause of fallback; an exhausted chain or failure of the
  // currently displayed file replaces it with that directly actionable fault.
  if (!cycle_failed || replace) {
    value.failed = source;
    value.reason = why;
    value.error = error;
  }
  cycle_failed = true;
  if (value.retry_at_ms == 0)
    value.retry_at_ms = esp_timer_get_time() / 1000 + cfg::kImageSourceRetryMs;
}
void rejected(Status& value, Failure why, esp_err_t error) {
  const auto source = value.actual;
  if (source == FileSource::kNone) return;
  failure(value, source, why, error, true);
  next_candidate = static_cast<uint8_t>(candidateIndex(value, source) + 1);
  value.actual = FileSource::kNone;
  ++value.attempt;
  value.pending = candidate(value, next_candidate) != FileSource::kNone;
  resolving = value.pending;
}
void newSelection(Status& value, uint8_t id, cfg::StockImage role,
                  uint32_t revision) {
  const uint32_t attempt = value.attempt + 1;
  value = {};
  value.active = value.pending = true;
  value.desired_id = id;
  value.role = role;
  value.revision = revision;
  value.attempt = attempt;
  next_candidate = 0;
  resolving = true;
  cycle_failed = false;
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
  // A zero-byte file still exists; a failed stat never proves absence.
  // Attempt deletion directly and accept only ENOENT as already removed.
  const bool removed = unlink(p) == 0;
  const auto err = removed || errno == ENOENT ? ESP_OK : ESP_FAIL;
  if (removed) bump(slot);
  if (rendering) display::unlock();
  return err;
}
// Opens the temporary file for 'slot'. The image budget is checked without
// the file the slot holds now, the storage reserve with the old file still
// counted.
esp_err_t start(uint8_t slot, std::size_t size) {
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
  if (total + size > cfg::kImageTotalBytes || files::usage(usage) != ESP_OK ||
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
  return received == expected && synced == 0 && closed == 0 ? ESP_OK : ESP_FAIL;
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
esp_err_t copy(uint8_t id, cfg::StockImage stock) {
  if (!libraryId(id) || !stockRole(stock)) return ESP_ERR_INVALID_ARG;
  char from[96];
  slotPath(id, from, sizeof(from));
  const auto size = bytes(from);
  if (size == 0) return ESP_ERR_NOT_FOUND;
  auto err = start(roleSlot(stock), size);
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
uint16_t revisionOf(uint8_t id) { return libraryId(id) ? slotRevision(id) : 0; }
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
Status status() {
  portENTER_CRITICAL(&selection_mux);
  const auto value = selected;
  portEXIT_CRITICAL(&selection_mux);
  return value;
}
void clearSelection() {
  const auto old = status();
  if (!old.active) return;
  Status value{};
  value.attempt = old.attempt + 1;
  value.revision = revision();
  resolving = cycle_failed = false;
  next_candidate = 0;
  publishSelection(value);
}
void acknowledgeSource(uint32_t attempt, bool accepted) {
  auto value = status();
  if (!value.active || value.attempt != attempt || value.revision != revision())
    return;
  if (accepted) {
    if (!resolving && value.actual != FileSource::kNone) {
      value.pending = false;
      publishSelection(value);
    }
    return;
  }
  rejected(value, Failure::kSource, ESP_FAIL);
  publishSelection(value);
}
bool filePath(const Status& value, char* out, std::size_t capacity) {
  if (out != nullptr && capacity != 0) out[0] = 0;
  return out != nullptr && value.active &&
         candidatePath(value, value.actual, out, capacity);
}
Status select(uint8_t id, cfg::StockImage role, char* out,
              std::size_t capacity) {
  auto value = status();
  const uint32_t current_revision = revision();
  if (!value.active || value.desired_id != id || value.role != role ||
      value.revision != current_revision)
    newSelection(value, id, role, current_revision);
  if (!stockRole(role) || (id != cfg::kStockImageId && !libraryId(id))) {
    value.pending = false;
    resolving = false;
    failure(value, FileSource::kNone, Failure::kInvalid, ESP_ERR_INVALID_ARG,
            true);
    publishSelection(value);
    selectionSource(value, out, capacity);
    return value;
  }
  const auto observed = display::imageObservation();
  if (value.actual != FileSource::kNone && observed.attempt == value.attempt) {
    switch (observed.result) {
      case display::ImageDecodeResult::kHeaderFailed:
        // An operational header failure can also be an allocation failure;
        // only complete validation proves malformed content.
        rejected(value, Failure::kUnavailable, ESP_FAIL);
        break;
      case display::ImageDecodeResult::kOpenFailed:
        rejected(value, Failure::kUnavailable, ESP_FAIL);
        break;
      case display::ImageDecodeResult::kDecodeFailed:
        rejected(value, Failure::kDecode, ESP_ERR_INVALID_RESPONSE);
        break;
      default: break;
    }
  }
  const int64_t now = esp_timer_get_time() / 1000;
  if (!resolving && value.retry_at_ms != 0 && now >= value.retry_at_ms) {
    next_candidate = 0;
    resolving = true;
    cycle_failed = false;
    value.pending = true;
    value.retry_at_ms = 0;
  }
  if (resolving) {
    network::HeavyGuard guard;
    if (guard) {
      const int64_t deadline = now + cfg::kImageValidationTimeoutMs;
      if (files::state() != files::State::kMounted) {
        failure(value, candidate(value, next_candidate), Failure::kUnavailable,
                ESP_ERR_INVALID_STATE, true);
        if (value.actual != FileSource::kNone) ++value.attempt;
        value.actual = FileSource::kNone;
        resolving = value.pending = false;
      }
      while (resolving) {
        const auto source = candidate(value, next_candidate);
        if (source == FileSource::kNone) {
          value.pending = resolving = false;
          break;
        }
        char candidate_path[96];
        esp_err_t error = ESP_ERR_INVALID_ARG;
        if (candidatePath(value, source, candidate_path,
                          sizeof(candidate_path))) {
          bytes(candidate_path, &error);
          // No replacement is a normal role choice, not a damaged image.
          if (error == ESP_ERR_NOT_FOUND &&
              source == FileSource::kReplacement) {
            ++next_candidate;
            continue;
          }
          if (error == ESP_OK)
            error = display::validateJpeg(candidate_path, hw::kDisplay.width,
                                          hw::kDisplay.height, deadline);
        }
        // An upload/reset may supersede the request. Never publish an old
        // file result or decoder failure as a fault of its fresh revision.
        if (value.revision != revision()) {
          newSelection(value, id, role, revision());
          break;
        }
        if (error == ESP_OK) {
          value.actual = source;
          ++value.attempt;
          value.pending =
              true;  // Await this attempt's LVGL getter acknowledgement.
          resolving = false;
          if (!cycle_failed) {
            value.failed = FileSource::kNone;
            value.reason = Failure::kNone;
            value.error = ESP_OK;
            value.retry_at_ms = 0;
          }
          break;
        }
        const auto why = error == ESP_ERR_NOT_FOUND ? Failure::kMissing
                         : error == ESP_ERR_TIMEOUT ? Failure::kTimeout
                         : error == ESP_ERR_NO_MEM  ? Failure::kMemory
                         : error == ESP_ERR_INVALID_RESPONSE
                             ? Failure::kInvalid
                             : Failure::kUnavailable;
        ++next_candidate;
        const bool exhausted =
            candidate(value, next_candidate) == FileSource::kNone;
        failure(value, source, why, error, exhausted);
        if (value.actual == source) {
          value.actual = FileSource::kNone;
          ++value.attempt;
        }
        if (exhausted) value.pending = resolving = false;
        if (esp_timer_get_time() / 1000 >= deadline) break;
      }
    }
  }
  publishSelection(value);
  selectionSource(value, out, capacity);
  return value;
}
esp_err_t begin(uint8_t id, std::size_t size, settings::ImageMask excluded) {
  if (temporary != nullptr || ota::trial() ||
      files::state() != files::State::kMounted)
    return ESP_ERR_INVALID_STATE;
  if (id != cfg::kStockImageId && !libraryId(id)) return ESP_ERR_INVALID_ARG;
  if (id == cfg::kStockImageId) {
    excluded |= settings::referencedImages();
    for (uint8_t i = 1; i <= cfg::kImageLibraryMax && id == 0; ++i)
      if (((excluded >> i) & 1) == 0 && !info(i).present) id = i;
    if (id == cfg::kStockImageId) return ESP_ERR_NO_MEM;
  }
  return start(id, size);
}
esp_err_t beginRole(cfg::StockImage stock, std::size_t size) {
  if (!stockRole(stock)) return ESP_ERR_INVALID_ARG;
  return start(roleSlot(stock), size);
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
  // Explicit deletion changes only this id; unrelated missing desired files
  // remain referenced. Keep the settings writer through unlink, so a save
  // cannot restore a reference between the cleanup and deleting its file.
  const settings::ImageMask others =
      settings::kLibraryImageMask & ~(settings::ImageMask{1} << id);
  return settings::dropMissingImages(
      others,
      [](void* value) { return unlinkImage(*static_cast<uint8_t*>(value)); },
      &id);
}
esp_err_t copyToRole(uint8_t id, cfg::StockImage stock) {
  if (ota::trial()) return ESP_ERR_INVALID_STATE;
  return copy(id, stock);
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
  if (esp_timer_get_time() / 1000 < maintenance_retry_at) return false;
  network::HeavyGuard guard;
  if (!guard) return false;
  for (const auto& legacy : cfg::kLegacyImageNames) {
    char file[32], p[96];
    snprintf(file, sizeof(file), "%s.jpg", legacy);
    path(file, p, sizeof(p));
    errno = 0;
    if (unlink(p) == 0) {
      ESP_LOGI(kTag, "removed legacy image %s", file);
      changed.fetch_add(1);
    } else if (errno != ENOENT) {
      ESP_LOGW(kTag, "legacy image removal failed (%d); retry later", errno);
      maintenance_retry_at =
          esp_timer_get_time() / 1000 + cfg::kImageMaintenanceRetryMs;
      return false;
    }
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
    for (uint8_t slot = 1; slot <= kSlots; ++slot) bump(slot);
  if (rendering) display::unlock();
  return err;
}
}  // namespace images
