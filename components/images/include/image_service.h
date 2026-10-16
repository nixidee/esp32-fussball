#pragma once
#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "esp_err.h"
#include "settings_model.h"

// Image library (OQ-71): uploaded JPEGs are files "img<id>.jpg" with ids
// 1..cfg::kImageLibraryMax. Id 0 is the stock image of a role
// (cfg::StockImage): its replacement file when one exists, else the factory
// file (protected images, OQ-151). Settings reference images only by id.
namespace images {
enum class FileSource : uint8_t { kNone, kLibrary, kReplacement, kFactory };
enum class Failure : uint8_t {
  kNone = 0,
  kMissing = 1,
  kUnavailable = 2,
  kInvalid = 3,
  kTimeout = 4,
  kSource = 5,
  kDecode = 6,
  kMemory = 7
};
constexpr const char* fileSourceKey(FileSource source) {
  switch (source) {
    case FileSource::kNone: return "";
    case FileSource::kLibrary: return "library";
    case FileSource::kReplacement: return "replacement";
    case FileSource::kFactory: return "factory";
  }
  return "";
}
constexpr const char* failureKey(Failure failure) {
  switch (failure) {
    case Failure::kNone: return "";
    case Failure::kMissing: return "missing";
    case Failure::kUnavailable: return "unavailable";
    case Failure::kInvalid: return "invalid";
    case Failure::kTimeout: return "timeout";
    case Failure::kSource: return "source";
    case Failure::kDecode: return "decode";
    case Failure::kMemory: return "memory";
  }
  return "";
}
constexpr const char* failureCode(Failure failure) {
  switch (failure) {
    case Failure::kNone: return "";
    case Failure::kMissing: return "IMG E01";
    case Failure::kUnavailable: return "IMG E02";
    case Failure::kInvalid: return "IMG E03";
    case Failure::kTimeout: return "IMG E04";
    case Failure::kSource: return "IMG E05";
    case Failure::kDecode: return "IMG E06";
    case Failure::kMemory: return "IMG E07";
  }
  return "";
}
// One active desired reference and its confirmed fallback. This is content
// state, never hardware/LVGL health or an OTA-trial veto. actual names the
// fully validated choice; pending remains true until LVGL accepts its source
// or while another admitted check is outstanding. Neither proves a drawn
// LCD frame. A pending check can retain a working fallback. Zero is a valid
// wrapping attempt.
struct Status {
  bool active = false, pending = false;
  uint8_t desired_id = 0;
  cfg::StockImage role = cfg::kStockCrest;
  FileSource actual = FileSource::kNone, failed = FileSource::kNone;
  Failure reason = Failure::kNone;
  esp_err_t error = ESP_OK;
  uint32_t revision = 0, attempt = 0;
  int64_t retry_at_ms = 0;
};
// App task, outside LVGL/data locks: checks the desired file and fallback
// chain with the existing complete JPEG validator and heavy admission. The
// supplied LVGL path names Status.actual, or is empty when none is usable.
Status select(uint8_t id, cfg::StockImage role, char* lvgl_source,
              std::size_t capacity);
// App task; off/start screens do not create image failures.
void clearSelection();
// App task under the LVGL lock; no file work. Late acknowledgements cannot
// change a newer attempt. Decoder observations use this same identity.
void acknowledgeSource(uint32_t attempt, bool accepted);
// Coherent snapshot, no filesystem or decoder work.
Status status();
// Explicit actual-file path, including factory while a bad replacement
// still exists. Does not change the existing role-download selection.
bool filePath(const Status& selection, char* out, std::size_t capacity);

struct Entry {
  std::size_t bytes;
  bool present;
};
// Library ids only; every other id is not present.
Entry info(uint8_t id);
// Bit n set = library image n exists.
settings::ImageMask present();

// Replacement of a stock role; bytes 0 and replaced false = factory image.
struct Role {
  std::size_t bytes;
  bool replaced;
};
Role role(cfg::StockImage stock);

// Library images present, and the bytes counted against the image budget:
// library files plus role replacements (OQ-154).
struct Usage {
  std::size_t count;
  std::size_t bytes;
};
Usage usage();
// Changes whenever the file of library image 'id' is replaced, deleted or
// wiped (wraps at 65536) and starts from a random value at every boot; 0 for
// every other id. Browsers use it as '?r=' to tell cached versions apart.
uint16_t revisionOf(uint8_t id);
// The same for the image a stock role shows (replacement or factory file).
uint16_t roleRevision(cfg::StockImage stock);
// Path of the file (VFS, not LVGL) of a library image or, for id 0, of the
// stock image 'stock'. False if the image does not exist or does not fit.
bool filePath(uint8_t id, cfg::StockImage stock, char* out,
              std::size_t capacity);

// Id 0 uses the lowest absent id that is neither a saved desired reference
// nor in excluded (e.g. unsaved browser draft references). A valid explicit
// library id writes that exact id, including restoring a missing desired
// file; excluded and saved references do not prevent this. No free id or
// no quota left: ESP_ERR_NO_MEM; invalid id: ESP_ERR_INVALID_ARG. Unavailable
// during the OTA trial. Publishing never changes settings references.
esp_err_t begin(uint8_t id, std::size_t size, settings::ImageMask excluded = 0);
// Upload of the replacement of a stock role; the factory file stays.
esp_err_t beginRole(cfg::StockImage stock, std::size_t size);
// Id written by the last successful begin(); 0 after beginRole().
uint8_t uploadedId();
esp_err_t write(const uint8_t* bytes, std::size_t size);
esp_err_t finish(int64_t deadline_ms);
void abort();

// Real delete: references in the settings (screens, slides) are
// replaced or removed and saved first, then the file is deleted.
esp_err_t remove(uint8_t id);
// Copies library image 'id' as the replacement of 'stock' (OQ-156); the
// library image stays. Budget and storage reserve as for an upload.
esp_err_t copyToRole(uint8_t id, cfg::StockImage stock);
// Deletes the replacement; the role shows its factory image again.
esp_err_t resetRole(cfg::StockImage stock);
// Deletes every library file and both replacements (OQ-157); the caller
// resets the settings.
esp_err_t removeAll();

// Outside the OTA trial, once: removes the legacy named files (OQ-73).
// Missing desired references stay unchanged for repair/retry. True when done
// (or nothing to do); false while files are unavailable, the trial runs,
// another heavy operation owns admission, or a removal failed. App task; failed
// writes retry after cfg::kImageMaintenanceRetryMs without blocking admission.
bool maintain();

// Explicit full storage reset, including recovery from damaged LittleFS.
// Caller owns the heavy-operation admission; unavailable during OTA trial.
esp_err_t format();
// Increases with every change of the library.
uint32_t revision();
}  // namespace images
