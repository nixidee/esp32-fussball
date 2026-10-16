// Display port (ADR-003): brings up SPI bus, panel controller and LVGL
// (esp_lvgl_port) for the display described by the hardware profile.
//
// LVGL runs in the esp_lvgl_port task. Every LVGL call from another task
// must be wrapped in lock() / unlock().

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "hw_profile.h"

namespace display {

// Call once at boot. On success the panel shows black, LVGL is running and
// supervised. Profile and wiring must have static storage duration.
//
// Failure handling: a bring-up failure, repeated or undrainable draw failures
// and an LVGL stall (task watchdog) restart the device with the reason in the
// panic output. After cfg::kAbnormalResetLimit consecutive abnormal resets
// init() leaves the hardware untouched and returns ESP_ERR_INVALID_STATE
// (headless) until a power cycle or a normal restart. ESP_ERR_INVALID_STATE
// also means "already initialised".
esp_err_t init(const hw::DisplayProfile& profile,
               const hw::DisplayWiring& wiring);

// Takes the LVGL lock. timeout_ms 0 waits forever; never hold the lock for
// long work (the LVGL supervision restarts the device after the task
// watchdog timeout). Returns false on timeout.
bool lock(uint32_t timeout_ms);
bool ready();
void unlock();

// Logs the LVGL memory pool usage (takes the lock itself with a short
// timeout and skips the log if LVGL is busy). Does nothing if init() has not
// succeeded.
void logMemory();
// Decodes the JPEG at `path` once without output. Deadline expiry (including
// during file I/O) takes priority: ESP_ERR_TIMEOUT. Confirmed file I/O failure
// including close: ESP_FAIL. Otherwise malformed/unsupported content or other
// dimensions: ESP_ERR_INVALID_RESPONSE. Caller owns shared heavy admission;
// no output or published file changes.
esp_err_t validateJpeg(const char* path, uint16_t width, uint16_t height,
                       int64_t deadline_ms);
// Caller holds the LVGL lock for reader-safe replacement/deletion.
void dropImageCache(const char* source);

// Decoder observations describe image content, never LCD/frame or OTA health.
// A failed attempt stays failed until observeImageSource() starts another one.
enum class ImageDecodeResult : uint8_t {
  kPending,
  kDecoded,
  // Phases only: a rejection can be allocation/I/O, not proven corruption.
  kHeaderFailed,
  kOpenFailed,
  kDecodeFailed,
};
struct ImageObservation {
  uint32_t attempt;
  ImageDecodeResult result;
};
// Caller holds the LVGL lock, before assigning the source. The source buffer
// stays valid and unchanged until the next call; nullptr/empty stops watching.
// Every retry/revision uses a new attempt; zero is an ordinary valid value.
void observeImageSource(const char* source, uint32_t attempt);
// Coherent and readable from any task; the image service owns retry/fallback.
ImageObservation imageObservation();
void setBrightness(uint8_t percent);
void setRotation(uint8_t quarter_turns);

}  // namespace display
