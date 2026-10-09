// Settings store (docs/CONFIGURATION.md): one NVS record, loaded at boot,
// copied to readers, saved all-or-nothing, reset by erasing its namespace.
// Initial values are the secrets.h presets over the app_config.h defaults.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "settings_model.h"

namespace settings {

// Initialises NVS and loads the stored record. Call once at boot before any
// other function here. Never fails: without a usable record the initial
// values apply (see save() for when saving is refused).
void init();

// Copy of the current settings (short lock; about 240 B on the caller's
// stack).
Model current();

// Increases with every successful save or reset, which also posts
// events::Event::kSettingsChanged (event_bus.h). Work started under one
// generation can compare it later to detect a change.
uint32_t generation();

// Validates and stores the complete model, then makes it current.
// ESP_ERR_INVALID_ARG: a value violates its limits, nothing stored.
// ESP_ERR_INVALID_STATE: NVS unusable, or the stored record could not be
// read at boot (only reset() may replace it).
// Other errors come from NVS or allocation; the current settings stay.
esp_err_t save(const Model& model);

// Erases the whole settings namespace; the initial values become current.
esp_err_t reset();

// Logs the current settings; passwords and the SSID only as set/length.
void logCurrent(const char* when);

// Runs the device test selected by cfg::kSettingsTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace settings
