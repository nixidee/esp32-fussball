// Settings store (docs/CONFIGURATION.md): settings, competitions and screen
// policy in three NVS records in one namespace, each
// loaded at boot, copied to readers and saved all-or-nothing; reset erases
// the namespace. Initial values are the secrets.h presets over the
// app_config.h defaults.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "screen_policy.h"
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

// Copy of current general settings under a short lock. The model/codec size
// is measured in scoped host/C6 checks. If given, out_generation receives the
// generation of that same snapshot under the same lock.
Model current(uint32_t* out_generation = nullptr);

// Desired library ids under the same short model lock, without a Model copy.
// File presence and screen enabled state do not release a desired id.
ImageMask referencedImages();

// Removes only references missing from 'present' from the current settings
// and policy under the existing writer mutex. Changed records are saved and
// published sequentially: this is not an atomic transaction across records.
// Every successful record advances generation(), including when a later save
// fails. An unread record or unavailable NVS refuses cleanup.
// Optional completion runs synchronously only after both records are clean,
// while still holding the writer mutex (e.g. unlinking the selected file).
// It must not call a settings writer or retain the context after returning.
// Any cleanup error prevents completion; its own error is returned otherwise.
using ImageCleanupCompletion = esp_err_t (*)(void* context);
esp_err_t dropMissingImages(ImageMask present,
                            ImageCleanupCompletion completion = nullptr,
                            void* context = nullptr);

// Increases with every successful save or reset, which also posts
// events::Event::kSettingsChanged (event_bus.h). Work started under one
// generation can compare it later to detect a change.
uint32_t generation();

// Successful saves advance this counter only when a provider-dependent value
// changes (sameProviderData, settings_model.h). Reset always forces an extra
// invalidation, including when the initial provider values already match.
// The data service compares it, so display-only saves keep fetched data and
// do not interrupt a running request.
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

// Copies the complete policy into caller-owned storage, with the matching
// UI/settings generation. No policy-sized return value or hidden stack copy.
void screenPolicy(screen_policy::Policy& out,
                  uint32_t* out_generation = nullptr);
// The controller's two retained models and both matching generations, copied
// under one model lock. Competition data are borrowed from the matching
// football publication, rather than duplicated in another retained snapshot.
void uiSettings(Model& model, screen_policy::Policy& policy,
                uint32_t& settings_generation, uint32_t& data_generation);
// Coherent worker snapshot: its existing model/routes and both matching
// generations, with an optional small policy subset in that route order.
// No full policy copy or temporary model/competition snapshot is created.
// The projection receives whether those competitions came from a saved
// record; bootstrap presets must not claim saved policy identity bindings.
void footballSettings(
    Model& model, Competitions& routes, uint32_t& settings_generation,
    uint32_t& data_generation,
    screen_policy::Eligibility* eligibility = nullptr,
    void (*project_routes)(const Model&, Competitions&,
                           bool saved_competitions) = nullptr);
Origin screenPolicyOrigin();

// Validates hardware limits and every (id, incarnation) association against
// the current competitions, writes one complete record, and publishes it.
// Wrong/stale identities are rejected; this never silently rebinds a slot.
// Nonempty associations require a saved competition record first; fallback
// presets alone cannot prove the lifetime of a persisted identity.
// An expected generation, checked under the writer mutex, rejects an obsolete
// draft with ESP_ERR_INVALID_STATE. A successful save advances generation()
// only: a request admitted before frame mode may finish under dataGeneration().
esp_err_t saveScreenPolicy(const screen_policy::Policy& candidate,
                           screen_policy::Error* error = nullptr,
                           const uint32_t* expected_generation = nullptr);

// Immediate request-admission gate, without copying Model/Policy or locking.
// Explicit setup-wizard queries are admitted separately by their caller.
bool footballPaused();

// Serializes the first nonblocking HTTP send attempt with settings/policy
// publication. Success transfers the EXISTING settings writer mutex to the
// caller, which must call endFootballSend() immediately after that attempt,
// never after DNS/TLS or the response. TIMEOUT = writer busy, NOT_ALLOWED =
// frame blocks an automatic request, INVALID_STATE = its snapshot is obsolete.
// Every request must match BOTH snapshot generations: policy/limit changes
// must not clear cached data but can invalidate an unsent request. Explicit
// setup queries bypass only the frame check; stale request limits must never
// be used by the wizard either. Budget rules still belong to the football
// service within this short admission.
esp_err_t beginFootballSend(bool explicit_setup,
                            uint32_t expected_data_generation,
                            uint32_t expected_settings_generation);
void endFootballSend();

// Gives new entries (id 0) an id, validates the list,
// requires every active competition to use enabled providers, stores the
// record and makes it current. On success 'candidate' holds the saved list
// with its ids. Errors as save(); ESP_ERR_INVALID_STATE also if the stored
// competition record could not be read at boot, or fallback identities cannot
// be proven safe against an unreadable retained policy (reset required).
esp_err_t saveCompetitions(Competitions& candidate,
                           Violation* violation = nullptr);

// Erases the whole settings namespace (all three records); the initial values
// become current.
esp_err_t reset();

// Logs the current settings; passwords and the SSID only as set/length.
void logCurrent(const char* when);

// Runs the device test selected by cfg::kSettingsTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace settings
