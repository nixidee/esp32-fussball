// File service (docs/ARCHITECTURE.md, "File service"): mounts LittleFS from
// the first data partition of subtype littlefs at cfg::kFsBasePath. A mount
// failure never formats. Only storage that reads as completely erased is
// initialised at boot (first use); any other content is kept unchanged and
// reported unavailable until the explicit reset (format()).
// Readers build paths with buildPath() (file_names.h).

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "file_names.h"

namespace files {

enum class State : uint8_t {
  kNotStarted,   // init() not called
  kMounted,      // files can be used
  kUnavailable,  // no partition, damaged content kept, or an I/O error
};

struct Usage {
  std::size_t total_bytes;
  std::size_t used_bytes;
};

// Mounts the filesystem; call once at boot before any other function here.
// Never fails: on a fault the state is kUnavailable and the reason logged.
void init();

State state();
const char* stateName(State state);

// True if init() found erased storage and initialised it during this boot.
bool initialisedAtBoot();

// Filesystem size and use. ESP_ERR_INVALID_STATE unless mounted.
esp_err_t usage(Usage& out);

// Explicit reset: deletes every file and mounts an empty filesystem, also
// when the content was damaged. Must not be called while a file is open:
// the library releases open descriptors without notice. Runtime callers use
// images::format(), which owns the LVGL reader/cache barrier.
esp_err_t format();

// Logs state and use (one line).
void logStatus();

// Runs the device test selected by cfg::kFileTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace files
