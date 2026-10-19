// One coherent application configuration in one NVS blob. Existing section
// accessors delegate to this sole owner. No development-record migration.

#pragma once

#include <cstddef>
#include <cstdint>

#include "app_config_model.h"
#include "civil_time.h"
#include "esp_err.h"
#include "provider_settings.h"
#include "runtime_config.h"
#include "screen_policy.h"
#include "settings_model.h"
#include "ui_config.h"

namespace settings {

// Initialises NVS and loads the stored record. Call once at boot before any
// other function here. Never fails: without a usable record the initial
// values apply (see apply() for when writing is refused). 'limits' are the
// maxima of the display in use (hardware profile); stored values above them
// are clamped on load and initial values stay within them.
void init(const Limits& limits = {});

// The limits given to init(); validation of every save uses them.
const Limits& limits();

// Coherent narrow snapshots; no Model-sized temporary in service readers.
NetworkSettings networkSettings();
DiagnosticSettings diagnosticSettings();
WebSettings webSettings(uint32_t* out_revision = nullptr);
// Reference to the immutable configured zone; remains valid across Apply.
const cfg::TimeZone& clockZone();

// Coherent copy, including durable revision/names reference/last apply ID.
void snapshot(AppConfig& out);
NamesRef namesReference(uint32_t* out_revision = nullptr);
// Request-local lease prepared by the names owner under heavy admission.
// Callbacks perform no I/O and never enter a config writer. The config owner
// commits the exact reference, activates names, then publishes the revision.
struct NamesChange {
  NamesRef expected, desired;
  void* context = nullptr;
  esp_err_t (*activate)(void*) = nullptr;
  void (*preserve)(void*) = nullptr;
};
// Candidate is caller-owned scratch; its revision is assigned on admission.
// Publication occurs only after verified persistence. An uncertain mutation
// keeps the old RAM snapshot and refuses later writes until reboot/recovery.
esp_err_t apply(AppConfig& candidate, uint32_t expected_revision,
                Violation* violation = nullptr,
                screen_policy::Error* policy_error = nullptr,
                const NamesChange* names = nullptr);

// Desired library ids under the same short model lock, without a Model copy.
// File presence and screen enabled state do not release a desired id.
ImageMask referencedImages();

// Remove missing image references in one config commit before completion.
// The callback runs under the writer lock and must not call another writer.
using ImageCleanupCompletion = esp_err_t (*)(void* context);
struct ImageCleanupProgress {
  bool references_cleared = false;
  bool settings_saved = false;
  bool outcome_unknown = false;
  uint32_t revision = 0;
};
esp_err_t dropMissingImages(ImageMask present,
                            ImageCleanupCompletion completion = nullptr,
                            void* context = nullptr,
                            ImageCleanupProgress* progress = nullptr);

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

// One aggregate origin. Invalid/unknown development records use defaults.
// Unreadable storage refuses writes; an uncertain write keeps the last coherent
// RAM config and requires reboot/recovery before further changes.
enum class Origin : uint8_t {
  kInitial,         // no stored record, or reset() since boot
  kStored,          // the stored record, or the last successful save
  kOutdated,        // stored record of an older format, discarded (OQ-253)
  kRejected,        // stored record decoded, but a value violates its limits
  kDamaged,         // stored record too long or not decodable
  kUnreadable,      // stored record could not be read (NVS error, no memory)
  kNoStorage,       // NVS unusable
  kOutcomeUnknown,  // mutation may be durable; read-only until reboot/recovery
};
Origin origin();
// Stable name for the REST status ("initial", "stored", "outdated",
// "rejected", "damaged", "unreadable", "no_storage").
const char* originName(Origin origin) noexcept;

// Copies one competition or the complete section into caller-owned storage.
// No independently persistent competition owner. Missing ID leaves out
// unchanged.
bool competitionById(uint8_t id, Competition& out);
void competitions(Competitions& out);
// The controller's two retained models and both matching generations, copied
// under one model lock. Competition data are borrowed from the matching
// football publication, rather than duplicated in another retained snapshot.
void uiSettings(UiConfig& out, screen_policy::Phase phase);
// Coherent worker snapshot: its existing model/routes and both matching
// generations, with an optional small policy subset in that route order.
// No full policy copy or temporary model/competition snapshot is created.
// The projection receives whether those competitions came from a saved
// record; bootstrap presets must not claim saved policy identity bindings.
void footballSettings(
    ProviderSettings& model, CompetitionRoutes& routes,
    uint32_t& settings_generation, uint32_t& data_generation,
    screen_policy::Eligibility* eligibility = nullptr,
    void (*project_routes)(const ProviderSettings&, CompetitionRoutes&,
                           bool saved_competitions) = nullptr);

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

// Erases the settings namespace; the initial values
// become current.
esp_err_t reset();

// Logs the current settings; passwords and the SSID only as set/length.
void logCurrent(const char* when);

// Runs the device test selected by cfg::kSettingsTest (app_config.h); does
// nothing in a normal build. Call after init().
void runDeviceTest();

}  // namespace settings
