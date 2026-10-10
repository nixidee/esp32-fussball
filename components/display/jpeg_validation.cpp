#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "display_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#define jd_prepare lvgl_jd_prepare
#define jd_decomp lvgl_jd_decomp
#include "src/libs/tjpgd/tjpgd.h"

namespace display {
namespace {
struct Validation {
  FILE* file;
  int64_t deadline;
};
size_t input(JDEC* decoder, uint8_t* bytes, size_t count) {
  auto& context = *static_cast<Validation*>(decoder->device);
  if (esp_timer_get_time() / 1000 >= context.deadline) return 0;
  if (bytes != nullptr) return fread(bytes, 1, count, context.file);
  return fseek(context.file, count, SEEK_CUR) == 0 ? count : 0;
}
int output(JDEC* decoder, void*, JRECT*) {
  return esp_timer_get_time() / 1000 <
         static_cast<Validation*>(decoder->device)->deadline;
}
}  // namespace
esp_err_t validateJpeg(const char* path, uint16_t width, uint16_t height,
                       int64_t deadline_ms) {
  FILE* file = fopen(path, "rb");
  if (file == nullptr) return ESP_FAIL;
  uint8_t ending[2]{};
  bool end = fseek(file, -2, SEEK_END) == 0 && fread(ending, 1, 2, file) == 2 &&
             ending[0] == 0xff && ending[1] == 0xd9;
  rewind(file);
  // Owned by the single admitted image writer, independent of the LVGL pool.
  alignas(4) static uint8_t work[4096];
  JDEC decoder{};
  Validation context{file, deadline_ms};
  auto result = jd_prepare(&decoder, input, work, sizeof(work), &context);
  if (!end || result != JDR_OK || decoder.width != width ||
      decoder.height != height) {
    fclose(file);
    return ESP_ERR_INVALID_RESPONSE;
  }
  result = jd_decomp(&decoder, output, 0);
  fclose(file);
  return result == JDR_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
void dropImageCache(const char* source) { lv_image_cache_drop(source); }
}  // namespace display
