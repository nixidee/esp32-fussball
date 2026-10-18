#include <cerrno>
#include <cstdio>
#include <cstring>

#include "app_config.h"
#include "display_port.h"
#include "esp_timer.h"
#include "file_service.h"
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
  // Set when a callback stopped the decoder at the deadline, so the result
  // is a timeout and not malformed data.
  bool expired;
  files::Failure io_failure;
};
void ioFailed(Validation& context) {
  if (context.io_failure == files::Failure::kNone)
    context.io_failure = files::fromErrno(errno);
}
bool expired(Validation& context) {
  if (esp_timer_get_time() / 1000 >= context.deadline) context.expired = true;
  return context.expired;
}
size_t input(JDEC* decoder, uint8_t* bytes, size_t count) {
  auto& context = *static_cast<Validation*>(decoder->device);
  if (expired(context)) return 0;
  if (bytes != nullptr) {
    const auto read = fread(bytes, 1, count, context.file);
    if (ferror(context.file)) ioFailed(context);
    return read;
  }
  if (fseek(context.file, count, SEEK_CUR) == 0) return count;
  ioFailed(context);
  return 0;
}
int output(JDEC* decoder, void*, JRECT*) {
  return !expired(*static_cast<Validation*>(decoder->device));
}
esp_err_t finish(Validation& context, esp_err_t content_result) {
  if (fclose(context.file) != 0) ioFailed(context);
  // A read/seek/close can itself outlast the deadline. Use the same clock
  // check as the callbacks, including when the codec stopped on that I/O.
  if (expired(context)) return ESP_ERR_TIMEOUT;
  if (context.io_failure != files::Failure::kNone)
    return files::error(context.io_failure);
  return content_result;
}
}  // namespace
esp_err_t validateJpeg(const char* path, uint16_t width, uint16_t height,
                       int64_t deadline_ms) {
  Validation context{nullptr, deadline_ms, false, files::Failure::kNone};
  if (expired(context)) return ESP_ERR_TIMEOUT;
  context.file = fopen(path, "rb");
  if (context.file == nullptr) {
    const auto error = files::error(files::fromErrno(errno));
    return expired(context) ? ESP_ERR_TIMEOUT : error;
  }
  // Seek from a known valid offset first: a file shorter than the two-byte
  // marker is invalid content, not an I/O failure from a negative offset.
  if (fseek(context.file, 0, SEEK_END) != 0) {
    ioFailed(context);
    return finish(context, ESP_ERR_INVALID_RESPONSE);
  }
  const long length = ftell(context.file);
  if (length < 0) {
    ioFailed(context);
    return finish(context, ESP_ERR_INVALID_RESPONSE);
  }
  if (length < 2) return finish(context, ESP_ERR_INVALID_RESPONSE);
  if (fseek(context.file, length - 2, SEEK_SET) != 0) {
    ioFailed(context);
    return finish(context, ESP_ERR_INVALID_RESPONSE);
  }
  uint8_t ending[2]{};
  const bool end = fread(ending, 1, 2, context.file) == 2 &&
                   ending[0] == 0xff && ending[1] == 0xd9;
  if (ferror(context.file)) ioFailed(context);
  if (!end) return finish(context, ESP_ERR_INVALID_RESPONSE);
  if (fseek(context.file, 0, SEEK_SET) != 0) {
    ioFailed(context);
    return finish(context, ESP_ERR_INVALID_RESPONSE);
  }
  // Shared by operations holding heavy admission, independent of LVGL's pool.
  alignas(4) static uint8_t work[cfg::kJpegValidationWorkBytes];
  JDEC decoder{};
  auto result = jd_prepare(&decoder, input, work, sizeof(work), &context);
  if (result != JDR_OK || decoder.width != width || decoder.height != height)
    return finish(context, ESP_ERR_INVALID_RESPONSE);
  result = jd_decomp(&decoder, output, 0);
  return finish(context, result == JDR_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE);
}
void dropImageCache(const char* source) { lv_image_cache_drop(source); }
}  // namespace display
