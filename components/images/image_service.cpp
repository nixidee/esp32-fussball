#include "image_service.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "display_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "file_candidate.h"
#include "file_io.h"
#include "file_service.h"
#include "freertos/FreeRTOS.h"
#include "hw_target.h"
#include "operation_admission.h"
#include "ota_service.h"
#include "settings_store.h"
namespace images {
namespace {
constexpr const char* kTag = "images";
// Writable files ("slots"): library ids 1..kImageLibraryMax, then one
// replacement per stock role.
constexpr uint8_t kSlots = cfg::kImageLibraryMax + cfg::kStockImageCount;
files::Candidate staged;
uint8_t destination = 0;  // slot of the running upload or copy
const files::Context& fileContext() {
  static constexpr files::Context value{
      cfg::kFsBasePath, nullptr, [](void*, files::Usage& out) {
        const auto error = files::usage(out);
        if (error == ESP_OK) return true;
        errno = error == ESP_ERR_NO_MEM ? ENOMEM : EIO;
        return false;
      }};
  return value;
}
esp_err_t candidateError() {
  const auto error = staged.error();
  return error == EINVAL                    ? ESP_ERR_INVALID_SIZE
         : error == EILSEQ                  ? ESP_ERR_INVALID_RESPONSE
         : error == EBADF || error == EBUSY ? ESP_ERR_INVALID_STATE
         : error == ETIMEDOUT ? ESP_ERR_TIMEOUT
                              : files::error(files::fromErrno(error));
}
std::atomic<uint32_t> changed{0};
// Successful validation belongs to an unchanged file, not to a screen visit.
// HTTP writers invalidate/confirm slots atomically; decoder failures invalidate
// the observed source. Ordinary decoding remains the content-read boundary.
static_assert(kSlots + cfg::kStockImageCount <= 32);
std::atomic<uint32_t> validated{0};
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
  if (slot >= 1 && slot <= kSlots)
    validated.fetch_and(~(uint32_t{1} << (slot - 1)));
  changed.fetch_add(1);
  if (slot >= 1 && slot <= kSlots) revisions[slot - 1].fetch_add(1);
}
uint16_t slotRevision(uint8_t slot) {
  // Random per boot: a '?r=' URL a browser cached during an earlier run must
  // not match a different image after the counters restart.
  static const uint16_t offset = static_cast<uint16_t>(esp_random());
  return static_cast<uint16_t>(offset + revisions[slot - 1].load());
}
bool slotFile(uint8_t slot, char* out, std::size_t size) {
  return files::ownedName({files::FileKind::kImageSlot, slot}, {out, size});
}
void slotPath(uint8_t slot, char* out, std::size_t size) {
  files::ownedPath(cfg::kFsBasePath, {files::FileKind::kImageSlot, slot},
                   {out, size});
}
std::size_t bytes(const char* p, esp_err_t* error = nullptr) {
  struct stat info{};
  esp_err_t result = ESP_OK;
  if (stat(p, &info) != 0)
    result = errno == ENOENT ? ESP_ERR_NOT_FOUND
                             : files::error(files::fromErrno(errno));
  else if (!S_ISREG(info.st_mode) || info.st_size < 0)
    result = ESP_FAIL;
  if (error) *error = result;
  return result == ESP_OK ? static_cast<std::size_t>(info.st_size) : 0;
}
std::size_t slotBytes(uint8_t slot, esp_err_t* error = nullptr) {
  char p[files::kPathBufferBytes];
  slotPath(slot, p, sizeof(p));
  return bytes(p, error);
}
uint32_t validationBit(const Status& value, FileSource source) {
  const unsigned index = source == FileSource::kFactory   ? kSlots + value.role
                         : source == FileSource::kLibrary ? value.desired_id - 1
                         : source == FileSource::kReplacement
                             ? roleSlot(value.role) - 1
                             : 32;
  return index < 32 ? uint32_t{1} << index : 0;
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
    return files::ownedName({files::FileKind::kStockImage, value.role},
                            {out, capacity});
  }
  return slotFile(
      source == FileSource::kLibrary ? value.desired_id : roleSlot(value.role),
      out, capacity);
}
bool candidatePath(const Status& value, FileSource source, char* out,
                   std::size_t capacity) {
  char file[cfg::kFileNameMaxChars + 1];
  return selectionName(value, source, file, sizeof(file)) &&
         files::buildPath(cfg::kFsBasePath, file, {out, capacity});
}
void selectionSource(const Status& value, char* out, std::size_t capacity) {
  if (out == nullptr || capacity == 0) return;
  out[0] = 0;
  char file[cfg::kFileNameMaxChars + 1];
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
  char file[cfg::kFileNameMaxChars + 1], src[files::kPathBufferBytes];
  slotFile(slot, file, sizeof(file));
  snprintf(src, sizeof(src), "A:%s", file);
  display::dropImageCache(src);
}
// Deletes one slot file under the display lock, after LVGL dropped it from
// its image cache.
esp_err_t unlinkImage(uint8_t slot, bool* deleted = nullptr,
                      bool* missing = nullptr) {
  if (files::state() != files::State::kMounted) return ESP_ERR_INVALID_STATE;
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  char p[files::kPathBufferBytes];
  slotPath(slot, p, sizeof(p));
  if (rendering) dropCache(slot);
  // A zero-byte file still exists; a failed stat never proves absence.
  // Attempt deletion directly and accept only ENOENT as already removed.
  const bool removed = unlink(p) == 0;
  const bool absent = !removed && errno == ENOENT;
  if (deleted) *deleted = removed;
  if (missing) *missing = absent;
  const auto err =
      removed || absent ? ESP_OK : files::error(files::fromErrno(errno));
  if (removed)
    bump(slot);
  else if (absent)
    validated.fetch_and(~(uint32_t{1} << (slot - 1)));
  if (rendering) display::unlock();
  return err;
}
// Opens the temporary file for 'slot'. The image budget is checked without
// the file the slot holds now, the storage reserve with the old file still
// counted.
esp_err_t start(uint8_t slot, std::size_t size) {
  if (staged.busy() || ota::trial() || files::state() != files::State::kMounted)
    return ESP_ERR_INVALID_STATE;
  if (size == 0 || size > cfg::kImageMaxBytes) return ESP_ERR_INVALID_SIZE;
  std::size_t total = 0;
  for (uint8_t i = 1; i <= kSlots; ++i) {
    if (i == slot) continue;
    char existing[files::kPathBufferBytes];
    slotPath(i, existing, sizeof(existing));
    esp_err_t checked;
    const auto charge = bytes(existing, &checked);
    if (checked != ESP_OK && checked != ESP_ERR_NOT_FOUND) return checked;
    if (charge > cfg::kImageTotalBytes - total) return files::kStorageFull;
    total += charge;
  }
  if (size > cfg::kImageTotalBytes - total) return files::kStorageFull;
  if (!staged.begin(fileContext(), {files::FileKind::kImageCandidate, 0}, size))
    return candidateError();
  destination = slot;
  return ESP_OK;
}
// Moves the closed temporary file onto its slot under the display lock.
esp_err_t publish() {
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  if (rendering) dropCache(destination);
  const auto err = staged.publish({files::FileKind::kImageSlot, destination})
                       ? ESP_OK
                       : candidateError();
  if (err == ESP_OK) {
    bump(destination);
    validated.fetch_or(uint32_t{1} << (destination - 1));
  }
  if (rendering) display::unlock();
  return err;
}
// Return before JPEG validation, releasing the bounded copy frame first.
__attribute__((noinline)) esp_err_t copyBytes(const char* source,
                                              std::size_t size) {
  int input = files::openRead(source);
  if (input < 0) return files::error(files::fromErrno(errno));
  struct stat info{};
  esp_err_t error =
      fstat(input, &info) == 0 ? ESP_OK : files::error(files::fromErrno(errno));
  if (error == ESP_OK && (!S_ISREG(info.st_mode) || info.st_size < 0 ||
                          static_cast<std::size_t>(info.st_size) != size))
    error = ESP_FAIL;
  uint8_t buffer[cfg::kFileCopyChunkBytes];
  for (std::size_t offset = 0; error == ESP_OK && offset < size;) {
    const auto count = std::min(sizeof(buffer), size - offset);
    error = files::readExact(input, offset, buffer, count)
                ? write(buffer, count)
                : files::error(files::fromErrno(errno));
    offset += count;
  }
  const bool closed = files::closeDescriptor(input);
  return error != ESP_OK ? error
         : closed        ? ESP_OK
                         : files::error(files::fromErrno(errno));
}
esp_err_t copy(uint8_t id, cfg::StockImage stock) {
  if (!libraryId(id) || !stockRole(stock)) return ESP_ERR_INVALID_ARG;
  char from[files::kPathBufferBytes];
  slotPath(id, from, sizeof(from));
  esp_err_t error;
  const auto size = bytes(from, &error);
  if (error != ESP_OK) return error;
  if (!size) return ESP_ERR_INVALID_RESPONSE;
  error = start(roleSlot(stock), size);
  if (error != ESP_OK) return error;
  error = copyBytes(from, size);
  if (error == ESP_OK)
    error =
        finish(esp_timer_get_time() / 1000 + cfg::kImageValidationTimeoutMs);
  if (error != ESP_OK) abort();
  return error;
}
}  // namespace
esp_err_t inventory(Inventory& out) {
  out = {};
  if (files::state() != files::State::kMounted) return ESP_ERR_INVALID_STATE;
  for (uint8_t slot = 1; slot <= kSlots; ++slot) {
    esp_err_t error;
    const auto size = slotBytes(slot, &error);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return error;
    const bool library = libraryId(slot);
    auto& entry = library ? out.library[slot - 1]
                          : out.replacements[slot - cfg::kImageLibraryMax - 1];
    entry = {size, slotRevision(slot), error == ESP_OK};
    if (entry.present) {
      if (size > SIZE_MAX - out.bytes) return ESP_ERR_INVALID_SIZE;
      out.bytes += size;
      if (library) ++out.count;
    }
  }
  return ESP_OK;
}
esp_err_t filePath(uint8_t id, cfg::StockImage stock, char* out,
                   std::size_t capacity) {
  if (out == nullptr || capacity == 0) return ESP_ERR_INVALID_ARG;
  out[0] = 0;
  if (id == cfg::kStockImageId ? !stockRole(stock) : !libraryId(id))
    return ESP_ERR_INVALID_ARG;
  if (files::state() != files::State::kMounted) return ESP_ERR_INVALID_STATE;
  esp_err_t error;
  slotBytes(id == cfg::kStockImageId ? roleSlot(stock) : id, &error);
  if (error != ESP_OK &&
      !(id == cfg::kStockImageId && error == ESP_ERR_NOT_FOUND))
    return error;
  char p[files::kPathBufferBytes];
  const files::Location location =
      error == ESP_ERR_NOT_FOUND
          ? files::Location{files::FileKind::kStockImage, stock}
          : files::Location{files::FileKind::kImageSlot,
                            id == cfg::kStockImageId ? roleSlot(stock) : id};
  if (!files::ownedPath(cfg::kFsBasePath, location, p))
    return ESP_ERR_INVALID_SIZE;
  const int count = snprintf(out, capacity, "%s", p);
  return count >= 0 && count < static_cast<int>(capacity)
             ? ESP_OK
             : ESP_ERR_INVALID_SIZE;
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
        validated.fetch_and(~validationBit(value, value.actual));
        rejected(value, Failure::kUnavailable, ESP_FAIL);
        break;
      case display::ImageDecodeResult::kOpenFailed:
        validated.fetch_and(~validationBit(value, value.actual));
        rejected(value, Failure::kUnavailable, ESP_FAIL);
        break;
      case display::ImageDecodeResult::kDecodeFailed:
        validated.fetch_and(~validationBit(value, value.actual));
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
    operations::Guard guard;
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
        char candidate_path[files::kPathBufferBytes];
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
          if (error == ESP_OK &&
              !(validated.load() & validationBit(value, source))) {
            error = display::validateJpeg(candidate_path, hw::kDisplay.width,
                                          hw::kDisplay.height, deadline);
            if (error == ESP_OK)
              validated.fetch_or(validationBit(value, source));
          }
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
  if (staged.busy() || ota::trial() || files::state() != files::State::kMounted)
    return ESP_ERR_INVALID_STATE;
  if (id != cfg::kStockImageId && !libraryId(id)) return ESP_ERR_INVALID_ARG;
  if (id == cfg::kStockImageId) {
    excluded |= settings::referencedImages();
    for (uint8_t i = 1; i <= cfg::kImageLibraryMax && id == 0; ++i) {
      if ((excluded >> i) & 1) continue;
      esp_err_t error;
      slotBytes(i, &error);
      if (error == ESP_ERR_NOT_FOUND)
        id = i;
      else if (error != ESP_OK)
        return error;
    }
    if (id == cfg::kStockImageId) return files::kStorageFull;
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
  if (!staged.busy()) return ESP_ERR_INVALID_SIZE;
  return staged.writeAt(staged.size(), buffer, size) ? ESP_OK
                                                     : candidateError();
}
void abort() {
  if (!staged.abort())
    ESP_LOGW(kTag, "unpublished image cleanup failed (%d)", errno);
}
esp_err_t finish(int64_t deadline_ms) {
  if (!staged.busy()) return ESP_ERR_INVALID_STATE;
  struct Check {
    int64_t deadline;
    esp_err_t result = ESP_OK;
  } check{deadline_ms};
  const bool valid = staged.complete(
      staged.limit(),
      [](const char* path, void* raw) {
        auto& check = *static_cast<Check*>(raw);
        check.result = display::validateJpeg(
            path, hw::kDisplay.width, hw::kDisplay.height, check.deadline);
        if (check.result == ESP_OK) return true;
        errno = check.result == files::kStorageFull        ? ENOSPC
                : check.result == ESP_ERR_NO_MEM           ? ENOMEM
                : check.result == ESP_ERR_TIMEOUT          ? ETIMEDOUT
                : check.result == ESP_ERR_INVALID_RESPONSE ? EILSEQ
                                                           : EIO;
        return false;
      },
      &check);
  auto err = valid                    ? ESP_OK
             : check.result != ESP_OK ? check.result
                                      : candidateError();
  if (err == ESP_OK && esp_timer_get_time() / 1000 >= deadline_ms)
    err = ESP_ERR_TIMEOUT;
  if (err == ESP_OK) err = publish();
  if (err != ESP_OK) abort();
  return err;
}
esp_err_t remove(uint8_t id, DeleteResult* result) {
  if (result) *result = {};
  if (!libraryId(id)) return ESP_ERR_INVALID_ARG;
  if (ota::trial() || files::state() != files::State::kMounted)
    return ESP_ERR_INVALID_STATE;
  struct Deletion {
    uint8_t id;
    DeleteResult* result;
  } deletion{id, result};
  settings::ImageCleanupProgress progress;
  const settings::ImageMask others =
      settings::kLibraryImageMask & ~(settings::ImageMask{1} << id);
  const auto error = settings::dropMissingImages(
      others,
      [](void* raw) {
        const auto& deletion = *static_cast<Deletion*>(raw);
        return unlinkImage(
            deletion.id,
            deletion.result ? &deletion.result->file_deleted : nullptr,
            deletion.result ? &deletion.result->already_missing : nullptr);
      },
      &deletion, &progress);
  if (result) {
    result->references_cleared = progress.references_cleared;
    result->settings_saved = progress.settings_saved;
    result->settings_revision = progress.revision;
    result->outcome_unknown = progress.outcome_unknown;
  }
  return error;
}
esp_err_t copyToRole(uint8_t id, cfg::StockImage stock) {
  if (ota::trial()) return ESP_ERR_INVALID_STATE;
  return copy(id, stock);
}
esp_err_t resetRole(cfg::StockImage stock) {
  if (!stockRole(stock)) return ESP_ERR_INVALID_ARG;
  if (ota::trial() || staged.busy()) return ESP_ERR_INVALID_STATE;
  return unlinkImage(roleSlot(stock));
}
esp_err_t removeAll(DeleteAllResult* result) {
  if (result) *result = {};
  if (ota::trial() || staged.busy()) return ESP_ERR_INVALID_STATE;
  for (uint8_t slot = 1; slot <= kSlots; ++slot) {
    bool deleted = false, missing = false;
    const auto err = unlinkImage(slot, &deleted, &missing);
    if (result) {
      result->deleted += deleted;
      result->already_missing += missing;
    }
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}
uint32_t revision() { return changed.load(); }
esp_err_t format() {
  if (ota::trial() || staged.busy()) return ESP_ERR_INVALID_STATE;
  const bool rendering = display::ready();
  if (rendering && !display::lock(cfg::kBootScreenLockTimeoutMs))
    return ESP_ERR_TIMEOUT;
  if (rendering) display::dropImageCache(nullptr);
  const auto err = files::format();
  if (err == ESP_OK) {
    validated.store(0);
    for (uint8_t slot = 1; slot <= kSlots; ++slot) bump(slot);
  }
  if (rendering) display::unlock();
  return err;
}
}  // namespace images
