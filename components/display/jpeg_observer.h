#pragma once

#include "esp_err.h"

namespace display {
// Called once under the LVGL lock after LVGL initialized its decoders/FS.
// The current display has no deinit/reinit path. Any future teardown must
// restore the original callbacks before lv_tjpgd_deinit(), which identifies
// its decoder by the original info callback.
esp_err_t initImageObserver();
}  // namespace display
