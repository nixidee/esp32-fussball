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
// values apply (see save() for when saving is refused). 'limits' are the
// maxima of the display in use (hardware profile); stored values above them
// are clamped on load and initial values stay within them.
void init(const Limits& limits = {});

// The limits given to init(); validation of every save uses them.
const Limits& limits();

// Copy of the current settings (short lock; sizeof(Model) is about 1.9 KB on
// the caller's stack, pinned by the host test).
Model current();

// Library id of the crest image a format 3 to 5 record referred to (format
// 6 has no such field), 0 if none or after reset(). The image service copies that image into
// the crest role once, saves the settings in the current format and then
// calls forgetLegacyCrest().
uint8_t legacyCrest();
void forgetLegacyCrest();

// Increases with every successful save or reset, which also posts
// events::Event::kSettingsChanged (event_bus.h). Work started under one
// generation can compare it later to detect a change.
uint32_t generation();

// Increases with every save or reset that changes a value provider data
// depends on (sameProviderData, settings_model.h), at most as often as
// generation(). The data service compares it, so display-only saves keep the
// fetched data and do not interrupt a running request.
uint32_t dataGeneration();

// Where the current settings come from. Every value except kStored means the
// initial values apply; kRejected and kDamaged records are overwritten by the
// next save, kUnreadable and kNoStorage refuse saving (see save()).
enum class Origin : uint8_t {
  kInitial,     // no stored record, or reset() since boot
  kStored,      // the stored record, or the last successful save
  kRejected,    // stored record decoded, but a value violates its limits
  kDamaged,     // stored record too long or not decodable
  kUnreadable,  // stored record could not be read (NVS error, no memory)
  kNoStorage,   // NVS unusable
};
Origin origin();
// Stable name for the REST status ("initial", "stored", "rejected",
// "damaged", "unreadable", "no_storage").
const char* originName(Origin origin) noexcept;

// Validates and stores the complete model, then makes it current.
// ESP_ERR_INVALID_ARG: a value violates its limits, nothing stored; *violation
// (if given) names the first offending field.
// ESP_ERR_INVALID_STATE: NVS unusable, or the stored record could not be
// read at boot (only reset() may replace it).
// Other errors come from NVS or allocation; the current settings stay.
esp_err_t save(const Model& model, Violation* violation = nullptr);

// Erases the whole settings namespace; the initial values become current.
esp_err_t reset();

// Logs the current settings; passwords and the SSID only as set/length.
void logCurrent(const char* when);

// Runs the device test selected by cfg::kSettingsTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace settings
