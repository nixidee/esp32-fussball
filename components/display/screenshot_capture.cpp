// One owned strip connects the LVGL producer to the synchronous HTTP caller.
// Normal flushing, byte swapping, SPI completion and failure handling remain
// the original display-port callback's responsibility.
#include "screenshot_capture.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace display {
namespace {

constexpr std::size_t kPixelBytes = 2;
static_assert(cfg::kScreenshotRows > 0);
static_assert(cfg::kScreenshotDeadlineMs > 0);

struct Capture {
  ScreenshotFrame frame{};
  ScreenshotStrip strip{};
  uint8_t* pixels = nullptr;
  std::size_t capacity = 0;
  int64_t deadline = 0;
  uint32_t pixels_sent = 0;
  esp_err_t result = ESP_OK;
  bool ready = false;
  bool cancelled = false;
  bool in_flush = false;
  bool finished = false;
};

lv_display_t* capture_display = nullptr;
const hw::DisplayProfile* capture_profile = nullptr;
lv_display_flush_cb_t normal_flush = nullptr;
Capture* active_capture = nullptr;
bool capture_busy = false;
portMUX_TYPE capture_lock = portMUX_INITIALIZER_UNLOCKED;

// Protects pointer acquisition, lifetime and slot ownership; never contains
// pixel copying, LVGL calls, allocation, socket work or a task wait.
class CaptureLock {
 public:
  CaptureLock() { taskENTER_CRITICAL(&capture_lock); }
  ~CaptureLock() { taskEXIT_CRITICAL(&capture_lock); }
};

int64_t nowMs() { return esp_timer_get_time() / 1000; }

void cancel(Capture& capture) {
  CaptureLock guard;
  capture.cancelled = true;
  if (active_capture == &capture) active_capture = nullptr;
}

// Detach first, then join only the copy/ACK producer. The producer drops its
// last reference BEFORE the original callback can block in the SPI driver.
// No LVGL lock is acquired here, including after a sink timeout/disconnect.
void finish(Capture& capture) {
  cancel(capture);
  for (;;) {
    {
      CaptureLock guard;
      if (!capture.in_flush) break;
    }
    vTaskDelay(1);
  }
  heap_caps_free(capture.pixels);
  CaptureLock guard;
  capture_busy = false;
}

esp_err_t copyStrip(Capture& capture, const lv_area_t& area,
                    const uint8_t* pixels, uint32_t stride, uint32_t first_row,
                    uint16_t rows) {
  {
    CaptureLock guard;
    if (capture.cancelled) return ESP_ERR_INVALID_STATE;
  }
  if (nowMs() >= capture.deadline) return ESP_ERR_TIMEOUT;
  const auto width = static_cast<uint16_t>(area.x2 - area.x1 + 1);
  const auto row_bytes = std::size_t{width} * kPixelBytes;
  for (uint16_t row = 0; row < rows; ++row) {
    std::memcpy(capture.pixels + std::size_t{row} * row_bytes,
                pixels + std::size_t{first_row + row} * stride, row_bytes);
  }
  if (nowMs() >= capture.deadline) return ESP_ERR_TIMEOUT;
  {
    CaptureLock guard;
    if (capture.cancelled) return ESP_ERR_INVALID_STATE;
    capture.strip = {static_cast<uint16_t>(area.x1),
                     static_cast<uint16_t>(area.y1 + first_row), width, rows,
                     capture.pixels};
    capture.ready = true;
  }
  for (;;) {
    {
      CaptureLock guard;
      if (capture.cancelled) return ESP_ERR_INVALID_STATE;
      if (!capture.ready) {
        capture.pixels_sent += uint32_t{width} * rows;
        return ESP_OK;
      }
    }
    if (nowMs() >= capture.deadline) return ESP_ERR_TIMEOUT;
    vTaskDelay(1);
  }
}

esp_err_t copyArea(Capture& capture, lv_display_t* display,
                   const lv_area_t& area, const uint8_t* pixels) {
  // Partial LVGL buffers are reshaped for each area: their stride need not
  // equal either the full screen width or the width of the copy strip.
  const auto* buffer = lv_display_get_buf_active(display);
  if (area.x1 < 0 || area.y1 < 0 || area.x2 < area.x1 || area.y2 < area.y1 ||
      area.x2 >= capture.frame.width || area.y2 >= capture.frame.height ||
      buffer == nullptr || pixels == nullptr || buffer->data != pixels)
    return ESP_ERR_INVALID_RESPONSE;
  const auto width = static_cast<uint32_t>(area.x2 - area.x1 + 1);
  const auto height = static_cast<uint32_t>(area.y2 - area.y1 + 1);
  const uint32_t stride = buffer->header.stride;
  const auto row_bytes = std::size_t{width} * kPixelBytes;
  if (stride < row_bytes ||
      uint64_t{stride} * (height - 1) + row_bytes > buffer->data_size ||
      row_bytes > capture.capacity)
    return ESP_ERR_INVALID_RESPONSE;
  const auto rows =
      std::min<std::size_t>(cfg::kScreenshotRows, capture.capacity / row_bytes);
  for (uint32_t first = 0; first < height; first += rows) {
    const auto count =
        static_cast<uint16_t>(std::min<std::size_t>(rows, height - first));
    const auto err = copyStrip(capture, area, pixels, stride, first, count);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}

void captureFlush(lv_display_t* display, const lv_area_t* area,
                  uint8_t* pixels) {
  Capture* capture;
  {
    CaptureLock guard;
    capture = active_capture;
    if (capture != nullptr) capture->in_flush = true;
  }
  if (capture != nullptr) {
    const bool last = lv_display_flush_is_last(display);
    auto result = area == nullptr ? ESP_ERR_INVALID_RESPONSE
                                  : copyArea(*capture, display, *area, pixels);
    {
      CaptureLock guard;
      if (last && result == ESP_OK &&
          capture->pixels_sent !=
              uint32_t{capture->frame.width} * capture->frame.height)
        result = ESP_ERR_INVALID_RESPONSE;
      if (last || result != ESP_OK) {
        capture->result = result;
        capture->finished = true;
        if (active_capture == capture) active_capture = nullptr;
      }
      // Last capture access. The HTTP caller may now destroy its stack
      // context and strip, even if normal flushing below stalls.
      capture->in_flush = false;
    }
  }
  // Exactly once, even on rejected geometry, timeout, disconnect or abort.
  // It owns the byte swap and the normal SPI flush-ready lifecycle.
  normal_flush(display, area, pixels);
}

}  // namespace

esp_err_t initScreenshotCapture(lv_display_t* display,
                                const hw::DisplayProfile& profile) {
  if (capture_display != nullptr || display == nullptr)
    return ESP_ERR_INVALID_STATE;
  if (!lock(cfg::kBootScreenLockTimeoutMs)) return ESP_ERR_TIMEOUT;
  normal_flush = lv_display_get_flush_cb(display);
  if (normal_flush == nullptr) {
    unlock();
    return ESP_ERR_INVALID_STATE;
  }
  capture_display = display;
  capture_profile = &profile;
  lv_display_set_flush_cb(display, captureFlush);
  unlock();
  return ESP_OK;
}

esp_err_t screenshot(const ScreenshotSink& sink, void* context,
                     int64_t deadline_ms) {
  if (sink.begin == nullptr || sink.strip == nullptr)
    return ESP_ERR_INVALID_ARG;
  if (capture_display == nullptr) return ESP_ERR_INVALID_STATE;
  const auto now = nowMs();
  // Half the watchdog window also keeps the paused S3's shorter watchdog
  // safe. The active C6 uses the approved five-second maximum.
  const auto budget = std::min<int64_t>(
      cfg::kScreenshotDeadlineMs, int64_t{CONFIG_ESP_TASK_WDT_TIMEOUT_S} * 500);
  Capture capture;
  capture.deadline = std::min(deadline_ms, now + budget);
  if (now >= capture.deadline) return ESP_ERR_TIMEOUT;
  {
    CaptureLock guard;
    if (capture_busy) return ESP_ERR_INVALID_STATE;
    capture_busy = true;
  }
  const auto max_side =
      std::max(capture_profile->width, capture_profile->height);
  capture.capacity = std::size_t{max_side} *
                     std::min<std::size_t>(cfg::kScreenshotRows, max_side) *
                     kPixelBytes;
  capture.pixels = static_cast<uint8_t*>(heap_caps_malloc(
      capture.capacity, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
  if (capture.pixels == nullptr) {
    finish(capture);
    return ESP_ERR_NO_MEM;
  }
  if (!lock(cfg::kStatusLockTimeoutMs)) {
    finish(capture);
    return ESP_ERR_TIMEOUT;
  }
  const auto width = lv_display_get_horizontal_resolution(capture_display);
  const auto height = lv_display_get_vertical_resolution(capture_display);
  const auto rotation = lv_display_get_rotation(capture_display);
  auto* screen = lv_display_get_screen_active(capture_display);
  const bool valid =
      width > 0 && width <= UINT16_MAX && height > 0 && height <= UINT16_MAX &&
      rotation <= LV_DISPLAY_ROTATION_270 && screen != nullptr &&
      lv_display_get_color_format(capture_display) == LV_COLOR_FORMAT_RGB565 &&
      lv_display_get_render_mode(capture_display) ==
          LV_DISPLAY_RENDER_MODE_PARTIAL;
  if (!valid || nowMs() >= capture.deadline) {
    unlock();
    finish(capture);
    return valid ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
  }
  capture.frame = {static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                   static_cast<uint8_t>(rotation),
                   capture_profile->shape == hw::DisplayShape::kRound};
  {
    CaptureLock guard;
    active_capture = &capture;
  }
  // Invalidation wakes the existing LVGL task. Running lv_refr_now() here
  // would deadlock its producer against this not-yet-running consumer.
  lv_obj_invalidate(screen);
  unlock();

  auto result = sink.begin(capture.frame, context);
  while (result == ESP_OK) {
    if (nowMs() >= capture.deadline) {
      result = ESP_ERR_TIMEOUT;
      break;
    }
    ScreenshotStrip strip;
    bool ready;
    bool finished;
    {
      CaptureLock guard;
      ready = capture.ready;
      finished = capture.finished;
      result = capture.result;
      if (ready) strip = capture.strip;
    }
    if (result != ESP_OK || finished) break;
    if (ready) {
      result = sink.strip(strip, context);
      if (result == ESP_OK && nowMs() >= capture.deadline)
        result = ESP_ERR_TIMEOUT;
      CaptureLock guard;
      if (result != ESP_OK) {
        capture.cancelled = true;
        if (active_capture == &capture) active_capture = nullptr;
      }
      capture.ready = false;  // ACK only after the sink released its borrow.
    } else {
      vTaskDelay(1);
    }
  }
  finish(capture);
  return result;
}

}  // namespace display
