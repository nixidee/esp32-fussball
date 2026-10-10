// Single software configuration (ADR-007): every software default, limit
// and enum lives here, nowhere else. Hardware facts live in the hardware
// profiles (boards/, displays/, targets/), secrets in include/secrets.h.
//
// Runtime settings: defaults and limits here, model and record layout in
// components/settings, storage in components/core (docs/CONFIGURATION.md).

#pragma once

#include <cstddef>
#include <cstdint>

namespace cfg {

// ---- Boot layout -----------------------------------------------------------
// Relative to the shorter display side; these are compile-time defaults.
inline constexpr uint16_t kContentMarginDivisor = 60;  // 240 px: 4 px
inline constexpr int32_t kBootRingWidthDivisor = 120;  // 240 px: 2 px
inline constexpr int32_t kBootRingGapDivisor = 40;     // 240 px: 6 px
inline constexpr int32_t kBootTextWidthPercent = 70;

// ---- Display failure handling ----------------------------------------------
// Consecutive failed draw calls (each one recovered) before a restart.
inline constexpr uint32_t kDisplayDrawFailureLimit = 3;
// LVGL supervision timer: feeds the task watchdog user and redraws after a
// recovered draw failure. Must stay well below the task watchdog timeout.
inline constexpr uint32_t kDisplaySupervisionPeriodMs = 500;
// Running this long after display init resets the abnormal-reset counter.
inline constexpr uint32_t kDisplayStablePeriodS = 60;
// Consecutive abnormal resets (panic, watchdog) after which the display is
// no longer initialised (headless) until a power cycle or explicit restart.
inline constexpr uint32_t kAbnormalResetLimit = 3;
// LVGL lock timeouts for callers outside the LVGL task.
inline constexpr uint32_t kBootScreenLockTimeoutMs = 1000;
inline constexpr uint32_t kStatusLockTimeoutMs = 10;

// Fault injection for the display failure tests. Must be kNone in every
// normal build; any other value compiles one test fault in.
enum class DisplayFault : uint8_t {
  kNone,
  kDrawFailOnce,       // one draw call fails: UI recovers, no restart
  kDrawFailAlways,     // every draw call fails: restart, then headless bound
  kDrainFail,          // draw and drain fail: immediate restart
  kLostCompletion,     // colour data never sent: watchdog restart
  kPortMemory,         // port allocation pre-check fails: restart
  kLvglPoolExhausted,  // LVGL pool runs out: assert, watchdog restart
  kLockStall,          // LVGL lock held for 10 s: watchdog restart
};
inline constexpr DisplayFault kDisplayFault = DisplayFault::kNone;

// ---- Event bus (docs/ARCHITECTURE.md) --------------------------------------
// Queue slots shared by UI actions; a UI action posted while all are taken
// is dropped and counted. State events have one reserved slot per kind on
// top (queue: 5 state kinds + these slots, 16 B per entry).
inline constexpr std::size_t kEventUiActionSlots = 4;
// Fixed subscriber table (no heap per subscriber).
inline constexpr std::size_t kEventMaxSubscribers = 8;
// Bus task: runs the subscriber callbacks, which only signal their own task.
// Above the LVGL task (4) so a long render does not delay notifications.
inline constexpr uint32_t kEventTaskPriority = 5;
inline constexpr uint32_t kEventTaskStackBytes = 2304;

// Device test of the event bus. Must be kNone in every normal build.
enum class EventTest : uint8_t {
  kNone,
  kFlood,  // floods UI actions and settings events at a slow subscriber
};
inline constexpr EventTest kEventTest = EventTest::kNone;

// ---- File service (docs/ARCHITECTURE.md) -----------------------------------
// The first data partition of subtype littlefs is mounted here.
inline constexpr char kFsBasePath[] = "/fs";
// Flat file names: 1..kFileNameMaxChars of a-z, 0-9, '_', '-', '.', not
// starting with '.' (reserved for the service's own temporary files).
inline constexpr std::size_t kFileNameMaxChars = 31;

// Device tests of the file service. Must be kNone in every normal build.
// They destroy the filesystem content: never run them on owner data.
enum class FileTest : uint8_t {
  kNone,
  kCorrupt,  // damages a mounted filesystem; the next boot must keep it
  kErase,    // erases the partition; the next boot must initialise it
  kFormat,   // writes and reads a file, formats, the file must be gone
};
inline constexpr FileTest kFileTest = FileTest::kNone;

// ---- Time (docs/NETWORK.md "Time") -----------------------------------------
// Selectable time zones: location label (shown and stored) and the POSIX TZ
// rule handed to the C library. Rules derive from IANA tzdata 2026c footers
// (Dublin uses equivalent civil offsets with positive summer DST), verified
// against 2026e for 2026-2028. They describe current rules only (past changes
// are not covered, which is irrelevant for live data). Labels are stored as
// text, so entries may be added or reordered; removing one makes a stored label
// invalid (initial value applies).
struct TimeZone {
  const char* label;
  const char* rule;
};
inline constexpr TimeZone kTimeZones[] = {
    {"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Lagos", "WAT-1"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Sao_Paulo", "<-03>3"},
    {"Asia/Bangkok", "<+07>-7"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul", "<+03>-3"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Moscow", "MSK-3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific/Honolulu", "HST10"},
    {"UTC", "UTC0"},
};
inline constexpr std::size_t kTimeZoneLabelMaxChars = 32;
inline constexpr char kTimeZoneDefault[] = "Europe/Berlin";
// A wall-clock change larger than this is logged as a jump (warning).
inline constexpr int64_t kTimeJumpLogThresholdS = 2;

// Device test of the time service. Must be kNone in every normal build.
enum class TimeTest : uint8_t {
  kNone,
  kRulesAndJumps,  // all zone rules, DST, midnight window, clock jumps
};
inline constexpr TimeTest kTimeTest = TimeTest::kNone;

// ---- Settings record -------------------------------------------------------
// Format 2 crosses the legacy reader's 1 KB ceiling. Field offsets stay
// append-only; valid format-1 core records load without rewriting NVS.
// Future appends retain the version only within the existing reader ceiling.
inline constexpr uint16_t kSettingsFormatVersion = 2;
inline constexpr uint16_t kSettingsLegacyFormatVersion = 1;
inline constexpr std::size_t kSettingsLegacyMaxRecordBytes = 1024;
inline constexpr std::size_t kSettingsLegacyCoreRecordBytes = 335;
// Longest stored record that is read (newer firmware may have appended
// fields); a longer one counts as damaged.
inline constexpr std::size_t kSettingsMaxRecordBytes = 2048;
// NVS namespace and key, at most 15 characters each.
inline constexpr char kSettingsNvsNamespace[] = "settings";
inline constexpr char kSettingsNvsKey[] = "record";

// ---- Settings: WiFi station (one network, docs/NETWORK.md) -----------------
// SSID: 1..32 arbitrary bytes; empty = no network configured (setup AP).
inline constexpr std::size_t kWifiSsidMaxBytes = 32;
// Password: empty (open network), a passphrase of printable ASCII characters
// or a raw key of exactly kWifiPskHexChars hexadecimal characters. Default
// empty. Other lengths are rejected, never truncated.
inline constexpr std::size_t kWifiPassphraseMinChars = 8;
inline constexpr std::size_t kWifiPassphraseMaxChars = 63;
inline constexpr std::size_t kWifiPskHexChars = 64;

// ---- Settings: setup access point and hostname ------------------------------
// AP password: empty (open AP, default) or a WPA2 passphrase with the
// kWifiPassphrase* limits above.
// mDNS hostname: 1..63 letters, digits or hyphens, no leading/trailing hyphen.
inline constexpr std::size_t kHostnameMaxChars = 63;
inline constexpr char kHostnameDefault[] = "fussball";

// ---- Settings: time ---------------------------------------------------------
// Time zone: one label of kTimeZones; default kTimeZoneDefault (above).
// NTP server (used from P3.1): 1..63 characters, dot-separated labels of
// letters, digits or hyphens, no empty label, no hyphen at a label's start
// or end (a DNS name; an IPv4 address also fits).
inline constexpr std::size_t kNtpServerMaxChars = 63;
inline constexpr char kNtpServerDefault[] = "pool.ntp.org";

// ---- Settings: board --------------------------------------------------------
// External antenna (only boards with an antenna switch use it).
inline constexpr bool kExternalAntennaDefault = false;

// ---- Settings: debug --------------------------------------------------------
// Periodic console health status (heap, LVGL, every task's free stack,
// event/file/time status). The app-loop watchdog stays active when disabled.
inline constexpr bool kDebugStatusLogDefault = true;
inline constexpr uint16_t kDebugStatusIntervalDefaultS = 30;
inline constexpr uint16_t kDebugStatusIntervalMinS = 5;
inline constexpr uint16_t kDebugStatusIntervalMaxS = 3600;

// ---- Health diagnostics ---------------------------------------------------
// Fixed all-task snapshot capacity; overflow produces no partial/stale rows.
inline constexpr std::size_t kHealthMaxTasks = 16;
inline constexpr std::size_t kHealthTaskNameBytes = 16;
// Only the busy-LVGL device test creates a temporary, statically allocated
// task.
inline constexpr uint32_t kHealthTestTaskStackBytes = 1536;
enum class HealthTest : uint8_t {
  kNone,
  kMetersAndStatus,  // snapshots, overlap, allocation failure/retry, on/off
  kAppStallOnce,  // one bounded blocked-app TWDT reset, then automatic resume
};
inline constexpr HealthTest kHealthTest = HealthTest::kNone;

// Device tests of the settings store. Must be kNone in every normal build;
// any other value compiles one test into the boot sequence. A test that
// stores a prepared record takes effect at the next boot (press RST).
enum class SettingsTest : uint8_t {
  kNone,
  kSaveSample,      // saves a sample model; the next boot loads it
  kReset,           // erases the stored settings at every boot
  kCorruptRecord,   // stores the sample with a wrong check value
  kUnknownVersion,  // stores the sample with format version + 1
  kShorterRecord,   // stores the sample without its last field
  kLongerRecord,    // stores the sample with an unknown trailing field
  kNvsFull,         // fills NVS; a save must then fail and change nothing
  kSaveLoop,        // saves two models alternately until power is cut
};
inline constexpr SettingsTest kSettingsTest = SettingsTest::kNone;

// ---- Product services: fixed bounds and defaults ----------------------------
enum class Provider : uint8_t {
  kOpenLigaDb,
  kApiFootball,
  kEspn,
  kFootballData
};
enum class Screen : uint8_t { kLiveSingle, kLiveMulti, kTable, kCrest };
inline constexpr Provider kProviderDefault = Provider::kOpenLigaDb;
inline constexpr Screen kIdleScreenDefault = Screen::kTable,
                        kMatchdayScreenDefault = Screen::kLiveMulti,
                        kOwnMatchScreenDefault = Screen::kLiveSingle;
inline constexpr bool kRouteEnabledDefault = false,
                      kFixtureMappingEnabledDefault = false,
                      kFixtureMappingSwappedDefault = false,
                      kScreenEnabledDefault = true, kBackgroundsDefault = true,
                      kEspnOptInDefault = false, kDemoDefault = false,
                      kNightEnabledDefault = true,
                      kIpBadgeApPermanentDefault = true,
                      kIpBadgeBottomDefault = false,
                      kBrowserDebugDefault = false;
inline constexpr uint8_t kLanguageDefault = 0, kRotationDefault = 0;
inline constexpr std::size_t kRouteCount = 3;  // league plus two optional cups
inline constexpr std::size_t kScreenCount = 4;
inline constexpr std::size_t kIdBytes = 32;
inline constexpr std::size_t kNameBytes = 64;
inline constexpr std::size_t kMatches = 32;
inline constexpr std::size_t kTableRows = 32;
inline constexpr std::size_t kMatchEvents = 12;
inline constexpr std::size_t kSnapshotEvents = 32;
inline constexpr std::size_t kSelectionEntries = 64;
inline constexpr std::size_t kJsonHeapBytes = 24576;
inline constexpr std::size_t kProviderBodyBytes = 262144;
inline constexpr std::size_t kSelectionBodyBytes = 1048576;
inline constexpr uint8_t kJsonDepth = 16;
inline constexpr uint32_t kOperationDeadlineMs = 30000;
inline constexpr uint32_t kSocketTimeoutMs = 1000;
inline constexpr std::size_t kProviderHostBytes = 64;
inline constexpr uint32_t kProviderStackBytes = 10240;
inline constexpr uint32_t kHttpStackBytes = 10240;
inline constexpr uint32_t kDnsStackBytes = 2048;
inline constexpr uint8_t kHttpSockets = 4;
inline constexpr std::size_t kApiBodyBytes = 8192;
inline constexpr uint32_t kWifiScanDeadlineMs = 15000;
inline constexpr uint32_t kWifiIpDeadlineMs = 30000;
inline constexpr uint32_t kWifiMissingScanMs = 20000;
inline constexpr uint32_t kWifiFallbackMs = 300000;
inline constexpr uint32_t kWifiAuthRetryMs[] = {10000, 30000, 60000};
inline constexpr uint32_t kAntennaSettleMs = 100;
inline constexpr uint16_t kPollIdleS = 21600;
inline constexpr uint16_t kPollPrematchS = 300;
inline constexpr uint16_t kPollLiveS = 60;
inline constexpr uint16_t kPollPostmatchS = 300;
inline constexpr uint16_t kProviderBudgetDefault = 100;
inline constexpr uint32_t kProviderMinIntervalMs[] = {30000, 6000, 30000, 6000};
inline constexpr uint16_t kFixtureHorizonDays = 7;
inline constexpr uint16_t kConferenceHorizonHours = 36;
inline constexpr uint16_t kUnknownMatchEndMinutes = 240;
inline constexpr uint16_t kDataStaleS = 900;
inline constexpr uint16_t kMatchWindowDefaultMinutes = 30;
inline constexpr uint16_t kManualReturnDefaultS = 60;
inline constexpr uint16_t kScrollResetDefaultS = 20;
inline constexpr uint16_t kScrollRepeatDefaultMs = 250;
inline constexpr uint16_t kInputDebounceMs = 40;
inline constexpr uint16_t kInputLongMs = 600;
inline constexpr uint16_t kInputDoubleMs = 300;
inline constexpr uint16_t kFactoryResetHoldMs = 8000;
inline constexpr uint8_t kVisibleMatchesDefault = 5;
inline constexpr uint8_t kTableWindowDefault = 5;
inline constexpr uint16_t kSlideshowDefaultS = 15;
inline constexpr uint16_t kNightStartDefaultMinutes = 23 * 60;
inline constexpr uint16_t kNightEndDefaultMinutes = 7 * 60;
inline constexpr uint16_t kIpBadgeDefaultS = 60;
inline constexpr uint32_t kThemeBackgroundDefault = 0x101820;
inline constexpr uint32_t kThemeTextDefault = 0xffffff;
inline constexpr uint32_t kThemeAccentDefault = 0x49cba0;
inline constexpr uint8_t kBrightnessDefault = 100;
inline constexpr uint8_t kNightBrightnessDefault = 10;
inline constexpr uint32_t kBacklightPwmHz = 5000;
inline constexpr uint32_t kDebugTaskStackBytes = 4096;
inline constexpr uint32_t kOtaLocalHealthMs = 10000;
inline constexpr uint32_t kSessionLifetimeMs = 900000;
inline constexpr std::size_t kImageCount =
    10;  // crest, five slides, four backgrounds
inline constexpr std::size_t kImageMaxBytes = 32768;
inline constexpr std::size_t kImageTotalBytes = 327680;
inline constexpr std::size_t kImageReserveBytes = 40960;
inline constexpr uint16_t kDebugStreamPeriodMs = 1000;
inline constexpr uint16_t kAppPollMs = 20;
inline constexpr uint16_t kUiRefreshMs = 500;
inline constexpr uint16_t kHighlightRotateMs = 5000;

// Validation and schema share these bounds. Browser controls read the schema.
struct Range {
  uint16_t min, max;
  constexpr bool contains(uint16_t n) const { return n >= min && n <= max; }
};
inline constexpr Range kBrightnessRange{0, 100}, kRotationRange{0, 3};
inline constexpr Range kVisibleMatchesRange{1, 5}, kTableWindowRange{1, 9};
inline constexpr Range kReturnTimeRange{0, 4000}, kScrollRepeatRange{100, 2000};
inline constexpr Range kSlideshowRange{3, 3600}, kDayMinutesRange{0, 1439};
inline constexpr Range kMatchWindowRange{0, 180},
    kProviderBudgetRange{1, 10000};
inline constexpr Range kTextScaleRange{70, 130};
inline constexpr uint8_t kTextScaleDefault = 100;
inline constexpr uint16_t kProviderDefaultBudget = 10000;
inline constexpr std::size_t kProviderHeaderBytes = 8192;
inline constexpr std::size_t kProviderWireOverheadBytes = 32768;
inline constexpr uint16_t kProviderWaitSliceMs = 100;
inline constexpr uint16_t kProviderErrorRetryS = 60, kProviderRateRetryS = 300;
inline constexpr uint16_t kCatchupRefreshS = 300;
inline constexpr uint16_t kNetworkPublishMs = 1000;
inline constexpr uint8_t kScanEntries = 16, kApClients = 2, kApChannel = 1;
inline constexpr uint8_t kApIp[] = {192, 168, 4, 1};
inline constexpr char kApNamePrefix[] = "Fussball";
inline constexpr char kImageNames[][12] = {
    "crest",  "slide1",      "slide2",     "slide3", "slide4",
    "slide5", "live_single", "live_multi", "table",  "slideshow"};
inline constexpr char kDefaultImageFiles[][32] = {
    "default_crest.jpg",      "default_crest.jpg",
    "default_crest.jpg",      "default_crest.jpg",
    "default_crest.jpg",      "default_crest.jpg",
    "default_background.jpg", "default_background.jpg",
    "default_background.jpg", "default_crest.jpg"};
inline constexpr std::size_t kFixtureMappings = 8;
inline constexpr std::size_t kProviderCount = 4;
inline constexpr char kBudgetNvsNamespace[] = "provider_budget",
                      kBudgetNvsKey[] = "credits";
inline constexpr uint32_t kBudgetRecordMagic = 0x46554231;
inline constexpr uint8_t kBudgetReservationRequests = 8;
inline constexpr char kSelectionCountryDefault[] = "Germany";
inline constexpr uint32_t kNightBackground = 0x030609;
inline constexpr uint16_t kSizeClassSmallMax = 260, kSizeClassMediumMax = 400;

}  // namespace cfg
