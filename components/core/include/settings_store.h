// Settings store (docs/CONFIGURATION.md): two NVS records in one namespace,
// the settings and the competitions (OQ-200), each
// loaded at boot, copied to readers and saved all-or-nothing; reset erases
// the namespace. Initial values are the secrets.h presets over the
// app_config.h defaults.

#pragma once

#include <cstddef>
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

// Copy of the current settings (short lock; sizeof(Model) is about 0.8 KB on
// the caller's stack, pinned by the host test).
Model current();

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
// initial values apply; kOutdated, kRejected and kDamaged records are
// overwritten by the next save, kUnreadable and kNoStorage refuse saving (see
// save()).
enum class Origin : uint8_t {
  kInitial,     // no stored record, or reset() since boot
  kStored,      // the stored record, or the last successful save
  kOutdated,    // stored record of an older format, discarded (OQ-253)
  kRejected,    // stored record decoded, but a value violates its limits
  kDamaged,     // stored record too long or not decodable
  kUnreadable,  // stored record could not be read (NVS error, no memory)
  kNoStorage,   // NVS unusable
};
Origin origin();
// Stable name for the REST status ("initial", "stored", "outdated",
// "rejected", "damaged", "unreadable", "no_storage").
const char* originName(Origin origin) noexcept;

// Validates and stores the complete model, then makes it current.
// 'check_access' (a change by the user) also requires every active
// competition to use only providers this model enables (checkProviderAccess);
// internal saves keep an older record's values.
// ESP_ERR_INVALID_ARG: a value violates its limits, nothing stored; *violation
// (if given) names the first offending field.
// ESP_ERR_INVALID_STATE: NVS unusable, or the stored record could not be
// read at boot (only reset() may replace it).
// Other errors come from NVS or allocation; the current settings stay.
esp_err_t save(const Model& model, Violation* violation = nullptr,
               bool check_access = false);

// The competition store. Readers copy one entry (about 240 B) or, with
// competitions(), the whole store (about 2.5 KB) into storage they own.
std::size_t competitionCount();
// False if 'index' (list order) or 'id' does not exist; 'out' unchanged.
bool competitionAt(std::size_t index, Competition& out);
bool competitionById(uint8_t id, Competition& out);
void competitions(Competitions& out);
// Id of the entry at 'index', 0 if none.
uint8_t competitionId(std::size_t index);
// Where the current competitions come from (as origin() for the settings).
// kOutdated also when there is no competition record and the settings record
// has an older format (formats 2 to 7 carried the competitions).
Origin competitionsOrigin();

// Gives new entries (id 0) an id, validates the list,
// requires every active competition to use enabled providers, stores the
// record and makes it current. On success 'candidate' holds the saved list
// with its ids. Errors as save(); ESP_ERR_INVALID_STATE also if the stored
// competition record could not be read at boot.
esp_err_t saveCompetitions(Competitions& candidate,
                           Violation* violation = nullptr);

// Erases the whole settings namespace (both records); the initial values
// become current.
esp_err_t reset();

// Logs the current settings; passwords and the SSID only as set/length.
void logCurrent(const char* when);

// Runs the device test selected by cfg::kSettingsTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace settings
