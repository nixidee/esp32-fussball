#pragma once
#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "esp_err.h"
#include "settings_model.h"

// Image library (OQ-71): uploaded JPEGs are files "img<id>.jpg" with ids
// 1..cfg::kImageLibraryMax. Id 0 is the stock image, a firmware file chosen
// by role (cfg::StockImage). Settings reference images only by id.
namespace images {
struct Entry {
  std::size_t bytes;
  bool present;
};
// Library ids only; every other id is not present.
Entry info(uint8_t id);
// Bit n set = library image n exists.
settings::ImageMask present();

// Library totals: images present and their summed bytes (quota use).
struct Usage {
  std::size_t count;
  std::size_t bytes;
};
Usage usage();
// Changes whenever the file of library image 'id' is replaced, deleted or
// wiped (wraps at 65536) and starts from a random value at every boot; 0 for
// every other id. Browsers use it as '?r=' to tell cached versions apart.
uint16_t revisionOf(uint8_t id);
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
// Id written by the last successful begin().
uint8_t uploadedId();
esp_err_t write(const uint8_t* bytes, std::size_t size);
esp_err_t finish(int64_t deadline_ms);
void abort();

// Real delete: references in the settings (crest, screens, slides) are
// replaced or removed and saved first, then the file is deleted.
esp_err_t remove(uint8_t id);
// Deletes every library file; the caller resets the settings.
esp_err_t removeAll();

// Outside the OTA trial, once: removes the legacy named files (OQ-73) and the
// settings references to images that do not exist. True when done (or
// nothing to do); false while the files are not mounted or the trial runs.
bool maintain();

// Explicit full storage reset, including recovery from damaged LittleFS.
// Caller owns the heavy-operation admission; unavailable during OTA trial.
esp_err_t format();
// Increases with every change of the library.
uint32_t revision();
}  // namespace images
