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

// LVGL path (drive letter A:) of a library image, or of the stock image of
// 'stock' for id 0. False if a library image does not exist.
bool source(uint8_t id, cfg::StockImage stock, char* out, std::size_t capacity);

// Id 0 stores a new image under the lowest free id; a library id replaces
// that image in place. No free id or no quota left: ESP_ERR_NO_MEM; unknown
// id: ESP_ERR_NOT_FOUND. Unavailable during the OTA trial.
esp_err_t begin(uint8_t id, std::size_t size);
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

// Outside the OTA trial, once: removes the legacy named files (OQ-73),
// copies the crest image of a settings record before format 6 into the crest
// role (settings::legacyCrest(), OQ-152) and removes settings references to
// images that do not exist. True when done (or nothing to do); false while
// the files are not mounted, the trial runs, the display stays busy or an
// upload runs.
bool maintain();

// Explicit full storage reset, including recovery from damaged LittleFS.
// Caller owns the heavy-operation admission; unavailable during OTA trial.
esp_err_t format();
// Increases with every change of the library.
uint32_t revision();
}  // namespace images
