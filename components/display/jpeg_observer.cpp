#include "jpeg_observer.h"

#include <cstring>
#include <new>

#include "display_port.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "src/image/lv_image_decoder_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"

// Private callback layout and EOF semantics are checked against source hashes
// by jpeg_observer_guard.cmake, not just the dependency's version number.
static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 6 &&
              LVGL_VERSION_PATCH == 0);
#if !LV_USE_TJPGD || LV_USE_OS != LV_OS_NONE ||                \
    LV_DRAW_SW_DRAW_UNIT_CNT != 1 || LV_CACHE_DEF_SIZE != 0 || \
    LV_IMAGE_HEADER_CACHE_DEF_CNT != 0 || LV_USE_FS_MEMFS ||   \
    !LV_USE_FS_POSIX || LV_FS_POSIX_LETTER != 65 ||            \
    LV_FS_POSIX_CACHE_SIZE != 0 || !LV_USE_ASSERT_MALLOC
#error \
    "JPEG observation requires the reviewed LVGL decoder/cache/FS configuration"
#endif

namespace display {
namespace {
struct Callbacks {
  lv_image_decoder_info_f_t info;
  lv_image_decoder_open_f_t open;
  lv_image_decoder_get_area_cb_t area;
  lv_image_decoder_close_f_t close;
};
struct Key {
  uint32_t attempt;
  bool tracked;
};
// The original TJPGD owns its JDEC and allocations. This small envelope owns
// only the observation identity; each original call receives its own userdata.
struct Session {
  void* decoder_data;
  Key key;
  bool complete;
};
Callbacks original{};
lv_image_decoder_t* installed = nullptr;
void* (*original_file_open)(lv_fs_drv_t*, const char*, lv_fs_mode_t) = nullptr;
// Changed/read only under the LVGL lock. Reuse the caller's stable request
// buffer rather than retaining another copy of the image identity.
const char* watched_source = nullptr;
portMUX_TYPE observation_mux = portMUX_INITIALIZER_UNLOCKED;
ImageObservation observation{};
bool observation_active = false;

bool failed(ImageDecodeResult result) {
  return result != ImageDecodeResult::kPending &&
         result != ImageDecodeResult::kDecoded;
}
void publish(Key key, ImageDecodeResult result) {
  if (!key.tracked) return;
  portENTER_CRITICAL(&observation_mux);
  if (observation_active && key.attempt == observation.attempt &&
      !failed(observation.result))
    observation.result = result;
  portEXIT_CRITICAL(&observation_mux);
}
Key keyFor(const void* source, lv_image_src_t type) {
  const bool tracked =
      watched_source && watched_source[0] && source &&
      type == LV_IMAGE_SRC_FILE &&
      strcmp(watched_source, static_cast<const char*>(source)) == 0;
  return {imageObservation().attempt, tracked};
}
void* fileOpen(lv_fs_drv_t* driver, const char* path, lv_fs_mode_t mode) {
  // LVGL opens the header file before calling decoder_info(). Observe that
  // existing operation too, including file loss after the source was applied.
  const bool tracked = watched_source && watched_source[0] == driver->letter &&
                       watched_source[1] == ':' &&
                       strcmp(watched_source + 2, path) == 0;
  const Key key{imageObservation().attempt, tracked};
  void* file = original_file_open(driver, path, mode);
  if (!file || file == reinterpret_cast<void*>(-1))
    publish(key, ImageDecodeResult::kOpenFailed);
  return file;
}
lv_result_t info(lv_image_decoder_t* decoder, lv_image_decoder_dsc_t* dsc,
                 lv_image_header_t* header) {
  const auto key = keyFor(dsc->src, dsc->src_type);
  const auto result = original.info(decoder, dsc, header);
  if (result != LV_RESULT_OK) publish(key, ImageDecodeResult::kHeaderFailed);
  return result;
}
lv_result_t open(lv_image_decoder_t* decoder, lv_image_decoder_dsc_t* dsc) {
  const auto key = keyFor(dsc->src, dsc->src_type);
  const auto result = original.open(decoder, dsc);
  if (result != LV_RESULT_OK) {
    publish(key, ImageDecodeResult::kOpenFailed);
    return result;
  }
  auto* memory = lv_malloc(sizeof(Session));
  // Preserve the existing fatal LVGL-allocation contract (assert + watchdog).
  LV_ASSERT_MALLOC(memory);
  if (!memory) {
    original.close(decoder, dsc);
    publish(key, ImageDecodeResult::kOpenFailed);
    return LV_RESULT_INVALID;
  }
  dsc->user_data = new (memory) Session{dsc->user_data, key, false};
  return result;
}
lv_result_t area(lv_image_decoder_t* decoder, lv_image_decoder_dsc_t* dsc,
                 const lv_area_t* full, lv_area_t* decoded) {
  auto& session = *static_cast<Session*>(dsc->user_data);
  // Tiled/repeated walks can restart the same open session with this sentinel.
  if (decoded->y1 == LV_COORD_MIN) session.complete = false;
  // Do not ask the unchanged decoder to read a nonexistent extra MCU. Its
  // public get-area contract uses INVALID for a normally completed walk too.
  if (session.complete) return LV_RESULT_INVALID;
  dsc->user_data = session.decoder_data;
  const auto result = original.area(decoder, dsc, full, decoded);
  dsc->user_data = &session;
  if (result == LV_RESULT_OK) {
    // The guarded TJPGD walks MCUs sequentially from top left. Only a
    // successfully delivered final MCU proves a complete decode, not open or
    // close. The following INVALID is its ordinary end-of-image result.
    if (decoded->x2 == static_cast<int32_t>(dsc->header.w) - 1 &&
        decoded->y2 == static_cast<int32_t>(dsc->header.h) - 1) {
      session.complete = true;
      publish(session.key, ImageDecodeResult::kDecoded);
    }
  } else if (!session.complete) {
    publish(session.key, ImageDecodeResult::kDecodeFailed);
  }
  return result;
}
void close(lv_image_decoder_t* decoder, lv_image_decoder_dsc_t* dsc) {
  auto* session = static_cast<Session*>(dsc->user_data);
  dsc->user_data = session->decoder_data;
  original.close(decoder, dsc);
  lv_free(session);
  dsc->user_data = nullptr;
}
}  // namespace

esp_err_t initImageObserver() {
  if (installed) return ESP_ERR_INVALID_STATE;
  if (lv_image_cache_is_enabled() || lv_image_header_cache_is_enabled())
    return ESP_ERR_NOT_SUPPORTED;
  lv_image_decoder_t* found = nullptr;
  for (auto* decoder = lv_image_decoder_get_next(nullptr); decoder;
       decoder = lv_image_decoder_get_next(decoder)) {
    if (!decoder->name || strcmp(decoder->name, "TJPGD")) continue;
    if (found) return ESP_ERR_INVALID_STATE;
    found = decoder;
  }
  auto* driver = lv_fs_get_drv(LV_FS_POSIX_LETTER);
  if (!found || !found->info_cb || !found->open_cb || !found->get_area_cb ||
      !found->close_cb || found->custom_draw_cb || !driver ||
      !driver->open_cb || driver->cache_size != 0)
    return ESP_ERR_NOT_SUPPORTED;
  // Check everything before replacing any callback. No second decoder or
  // filesystem driver is registered, and the original functions are unchanged.
  original = {found->info_cb, found->open_cb, found->get_area_cb,
              found->close_cb};
  original_file_open = driver->open_cb;
  lv_image_decoder_set_info_cb(found, info);
  lv_image_decoder_set_open_cb(found, open);
  lv_image_decoder_set_get_area_cb(found, area);
  lv_image_decoder_set_close_cb(found, close);
  driver->open_cb = fileOpen;
  installed = found;
  return ESP_OK;
}
void observeImageSource(const char* source, uint32_t attempt) {
  watched_source = source;
  portENTER_CRITICAL(&observation_mux);
  observation = {attempt, ImageDecodeResult::kPending};
  observation_active = source && source[0];
  portEXIT_CRITICAL(&observation_mux);
}
ImageObservation imageObservation() {
  portENTER_CRITICAL(&observation_mux);
  const auto copy = observation;
  portEXIT_CRITICAL(&observation_mux);
  return copy;
}
}  // namespace display
