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
// Developer colour test at boot (rings and texts, see boot_screen.h) instead
// of the start screen. Must be kNone in every normal build.
enum class DisplayTest : uint8_t { kNone, kColourTest };
inline constexpr DisplayTest kDisplayTest = DisplayTest::kNone;

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
  kLockStall,          // LVGL lock held past the watchdog timeout: restart
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
// A wall-clock change larger than this is a jump: logged as a warning and
// counted in timekeeping::Status::step_count (a new provider-data baseline).
// Smaller changes, such as the regular SNTP resync, are neither.
inline constexpr int64_t kTimeJumpThresholdS = 2;

// Device test of the time service. Must be kNone in every normal build.
enum class TimeTest : uint8_t {
  kNone,
  kRulesAndJumps,  // all zone rules, DST, midnight window, clock jumps
};
inline constexpr TimeTest kTimeTest = TimeTest::kNone;

// ---- Settings record -------------------------------------------------------
// Format 9: the lists are stored as a count followed by that many entries, so
// the record length varies; the competitions have their own record (OQ-200,
// OQ-209); the request limits are a plan per provider (daily cap, window
// limit, interval per data type; OQ-216, OQ-225) plus one global limit
// (OQ-226); format 9 drops the core-data interval (OQ-248, OQ-257). A shorter
// record keeps the initial values of its missing trailing fields. Records of
// formats 1 to 8 are not read (OQ-253, OQ-257): the device starts with the
// initial values (origin "outdated"); the old record stays in NVS until the
// next save, so an OTA rollback can still read it.
inline constexpr uint16_t kSettingsFormatVersion = 9;
// Longest stored record that is read (newer firmware may have appended
// fields); a longer one counts as damaged.
inline constexpr std::size_t kSettingsMaxRecordBytes = 2048;
// NVS namespace and key, at most 15 characters each.
inline constexpr char kSettingsNvsNamespace[] = "settings";
inline constexpr char kSettingsNvsKey[] = "record";
// Competition record (OQ-200): same namespace, own key and format. Format 2
// has one source per competition (OQ-247); format 1 (fallback source and
// fixture pairs) is not read (OQ-253).
inline constexpr char kCompetitionsNvsKey[] = "competitions";
inline constexpr uint16_t kCompetitionsFormatVersion = 2;
// Longest competition record that is read (newer firmware may append).
inline constexpr std::size_t kCompetitionsMaxRecordBytes = 4096;

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
// Screens in settings order (the stored styles; kMatchday came last, OQ-193).
enum class Screen : uint8_t { kLiveSingle, kLiveMulti, kTable, kCrest, kMatchday };
// Konferenz page order (OQ-136): every page in turn in kickoff order, or a
// single page ranked by activity.
enum class ConferenceMode : uint8_t { kAllPages, kOnePage };
inline constexpr ConferenceMode kConferenceModeDefault =
    ConferenceMode::kAllPages;
// Matchday screen selection (OQ-244, OQ-267): the current matchday (the
// finished one until the end of the following local day, OQ-268) or the
// next one (from the end of the last kickoff's local date, OQ-271).
enum class MatchdayMode : uint8_t { kCurrent, kNext };
inline constexpr MatchdayMode kMatchdayModeDefault = MatchdayMode::kCurrent;
// Function of one input (OQ-141…OQ-143). Exactly one active input carries the
// mandatory one-key set: short Next, double Previous, long Default screen.
// Up/Down page or scroll; Scroll moves in its current direction while held and
// toggles the direction on release (reset after `scroll_reset_s`).
enum class InputRole : uint8_t { kOff, kMandatory, kUp, kDown, kScroll };
inline constexpr InputRole kInputRoleLast = InputRole::kScroll;
// Default role of input `index` (0-based) of a profile with `inputs` inputs:
// the mapping before settings format 5. Input 1 mandatory; with two inputs the
// second scrolls with direction toggle; with more, input 3 Down and the others
// Up (three inputs: OQ-150).
constexpr InputRole inputRoleDefault(std::size_t index, std::size_t inputs) {
  if (index >= inputs) return InputRole::kOff;
  if (index == 0) return InputRole::kMandatory;
  if (inputs == 2) return InputRole::kScroll;
  return index == 2 ? InputRole::kDown : InputRole::kUp;
}
inline constexpr Provider kProviderDefault = Provider::kOpenLigaDb;
inline constexpr Screen kIdleScreenDefault = Screen::kTable,
                        kMatchdayScreenDefault = Screen::kLiveMulti,
                        kOwnMatchScreenDefault = Screen::kLiveSingle;
// A new competition is active.
inline constexpr bool kEntryActiveDefault = true,
                      kScreenEnabledDefault = true, kBackgroundsDefault = true,
                      kEspnOptInDefault = false, kDemoDefault = false,
                      kNightEnabledDefault = true,
                      kIpBadgeApPermanentDefault = true,
                      kIpBadgeBottomDefault = false,
                      kBrowserDebugDefault = false,
                      // Konferenz keeps the own match in a fixed row (OQ-123).
                      kPinOwnMatchDefault = true,
                      // Own match shown for its whole matchday (OQ-139).
                      kWholeMatchdayDefault = false;
inline constexpr uint8_t kLanguageDefault = 0, kRotationDefault = 0;
inline constexpr std::size_t kScreenCount = 5;
// Order of the screens on the display (Next/Previous) and in the Web UI.
inline constexpr Screen kScreenOrder[kScreenCount] = {
    Screen::kLiveSingle, Screen::kLiveMulti, Screen::kMatchday, Screen::kTable,
    Screen::kCrest};
// List maxima of the settings model (target capacities; ordered lists without
// empty entries). A competition is a league or cup, optionally with an own
// club (OQ-194, OQ-195).
inline constexpr std::size_t kCompetitionMax = 8;
inline constexpr std::size_t kSlideMax = 5;
// Screens a competition is used for (OQ-192), one bit each in
// Competition::screens; a new competition is used for all of them.
enum class CompetitionScreen : uint8_t {
  kOwnMatch,
  kConference,
  kMatchday,
  kTable,
};
inline constexpr std::size_t kCompetitionScreenCount = 4;
inline constexpr uint8_t kCompetitionScreensDefault = 0x0f;
inline constexpr std::size_t kImageLibraryMax = 10;
// Text capacities (bytes) of the settings model; validation, the stored
// record and the schema derive from these.
inline constexpr std::size_t kAdminPasswordMaxBytes = 64;
inline constexpr std::size_t kApiFootballKeyMaxBytes = 96;
inline constexpr std::size_t kFootballDataKeyMaxBytes = 64;
inline constexpr std::size_t kCompetitionIdMaxBytes = 32;
inline constexpr std::size_t kSeasonMaxBytes = 16;
inline constexpr std::size_t kTeamIdMaxBytes = 32;
inline constexpr std::size_t kDisplayNameMaxBytes = 32;
// Provider ids of a match or team.
inline constexpr std::size_t kIdBytes = 32;
inline constexpr std::size_t kNameBytes = 64;
// Longest search text of a competition or team list request.
inline constexpr std::size_t kSelectionTextMaxBytes = 48;
inline constexpr std::size_t kMatchEvents = 12;
// Compact match store (OQ-227, football::MatchStore), sized for eight
// competitions: teams of all tables and stored matches (one entry per
// provider and team id; at most 255, the directory index is one byte),
// matches of every competition, table rows of all competitions, own-club
// matches with full names and events, the shared event pool and round labels.
inline constexpr std::size_t kStoreTeams = 240;
inline constexpr std::size_t kStoreMatches = 192;
inline constexpr std::size_t kStoreTableRows = 192;
inline constexpr std::size_t kStoreOwnMatches = 8;
inline constexpr std::size_t kStoreEvents = 32;
inline constexpr std::size_t kStoreRoundLabels = 24;
// Matches of one competition whose kickoffs are less than this apart form
// one matchday (OQ-201, OQ-224).
inline constexpr uint16_t kMatchdayGapHours = 48;
inline constexpr std::size_t kSelectionEntries = 64;
inline constexpr std::size_t kJsonHeapBytes = 24576;
inline constexpr std::size_t kProviderBodyBytes = 262144;
inline constexpr std::size_t kSelectionBodyBytes = 1048576;
// ESPN match summary of the own match (substitutions, OQ-207): about 400 KB
// measured 2026-10-14, streamed (no RAM growth).
inline constexpr std::size_t kProviderSummaryBodyBytes = 524288;
inline constexpr uint8_t kJsonDepth = 16;
inline constexpr uint32_t kOperationDeadlineMs = 30000;
inline constexpr uint32_t kSocketTimeoutMs = 1000;
// Longest provider silence tolerated while sending a request or receiving
// its response; waits run in kSocketTimeoutMs slices within the deadline.
inline constexpr uint32_t kProviderIdleTimeoutMs = 8000;
inline constexpr std::size_t kProviderHostBytes = 64;
inline constexpr uint32_t kProviderStackBytes = 12800;
inline constexpr uint32_t kHttpStackBytes = 10240;
inline constexpr uint32_t kDnsStackBytes = 2048;
inline constexpr uint8_t kHttpSockets = 4;
inline constexpr std::size_t kApiBodyBytes = 8192;
inline constexpr uint32_t kWifiScanDeadlineMs = 15000;
inline constexpr uint32_t kWifiIpDeadlineMs = 30000;
inline constexpr uint32_t kWifiMissingScanMs = 20000;
inline constexpr uint32_t kWifiFallbackMs = 300000;
// Longest time a phone on the setup AP postpones home-network attempts.
inline constexpr uint32_t kWifiApClientHoldMs = 300000;
inline constexpr uint32_t kWifiAuthRetryMs[] = {10000, 30000, 60000};
// Retry delay after a radio reconfiguration failed in the WiFi driver.
inline constexpr uint32_t kWifiRadioRetryMs = 10000;
// Station modem sleep (IDF default WIFI_PS_MIN_MODEM) is off: the device is
// mains powered, and in modem sleep the AP holds downlink frames until the
// next DTIM beacon, which adds latency to every TCP round trip.
inline constexpr bool kWifiPowerSave = false;
inline constexpr uint32_t kAntennaSettleMs = 100;
// Data types a provider is requested for (OQ-216, OQ-218; the core data are
// gone, OQ-248): fixtures (current and next matchday), table, live scores
// (also from 30 minutes before kickoff, OQ-220), live table and the events
// of an own match.
enum class DataType : uint8_t {
  kFixtures,
  kTable,
  kLive,
  kLiveTable,
  kEvents,
};
inline constexpr std::size_t kDataTypeCount = 5;
// Default interval per data type in seconds, the same for every provider
// (OQ-216: fixtures every 8 h, table every 4 h, live every minute, events of
// the own match every 10 minutes, OQ-207; the live table every 10 minutes is
// the agent's choice under OQ-227).
inline constexpr uint32_t kIntervalDefaultS[kDataTypeCount] = {28800, 14400, 60,
                                                               600, 600};
inline constexpr uint32_t kIntervalMinS = 10, kIntervalMaxS = 604800;
// An API-Football source resolves its current season again when a fixtures
// run starts this long after the last resolution (OQ-259; before OQ-248 the
// daily core-data run did it).
inline constexpr uint32_t kSeasonRecheckS = 86400;
// Defaults of the per-provider request limits (OQ-225), provider order
// OpenLigaDB, API-Football, ESPN, football-data.org: the limits the
// providers document (checked 2026-10-13): OpenLigaDB 60 requests per minute
// and IP, no quota; API-Football free plan 100 requests per day; ESPN
// publishes no limit (unofficial interface); football-data.org free tier 10
// calls per minute, no daily quota. 0 means no limit: a daily cap of 0 keeps
// no counter and writes no NVS. The global limit over all providers defaults
// to off (OQ-222, OQ-226).
inline constexpr uint16_t kDailyRequestsDefault[] = {0, 100, 0, 0};
inline constexpr uint16_t kWindowRequestsDefault[] = {60, 0, 0, 10};
inline constexpr uint16_t kWindowDefaultS[] = {60, 0, 0, 60};
// Small gap between the starts of any two provider requests, a device
// relief independent of the limits (OQ-212).
inline constexpr uint32_t kRequestGapMs = 2000;
// Time buckets of a window limit (request_scheduler.h): more buckets use
// the allowance more fully and cost 2 bytes each per limit.
inline constexpr std::size_t kLimitWindowBuckets = 32;
// Days before and after today of a club or date-range fixture request
// (provider_plan.cpp).
inline constexpr uint16_t kFixtureHorizonDays = 7;
// Latest end of a match the provider reports as still running.
inline constexpr uint16_t kUnknownMatchEndMinutes = 240;
// Expected end of a match without a reported running phase or final whistle
// (90 minutes, half time, stoppage time; OQ-213).
inline constexpr uint16_t kExpectedMatchMinutes = 110;
// Live data is requested from this long before kickoff (OQ-220).
inline constexpr uint16_t kLiveLeadMinutes = 30;
// Automatic Konferenz page change (OQ-81).
inline constexpr uint16_t kConferencePageMs = 8000;
// Shown data counts as stale while a match of its competition is in its
// live window and the competition's last successful match request is older
// than this, or while its last request failed.
inline constexpr uint16_t kDataStaleS = 900;
// A fetched match that kicked off this long ago and is neither finished nor
// has a result marks its provider as not maintaining the competition
// (OQ-281).
inline constexpr uint32_t kUnmaintainedAfterS = 24 * 3600;
// The provider worker re-estimates phases and minutes, drops passed
// matchdays and frees unused directory entries this often.
inline constexpr uint16_t kStoreMaintenanceS = 60;
inline constexpr uint16_t kMatchWindowDefaultMinutes = 30;
inline constexpr uint16_t kManualReturnDefaultS = 60;
inline constexpr uint16_t kScrollResetDefaultS = 20;
inline constexpr uint16_t kScrollRepeatDefaultMs = 250;
inline constexpr uint16_t kInputDebounceMs = 40;
inline constexpr uint16_t kInputLongMs = 600;
inline constexpr uint16_t kInputDoubleMs = 300;
inline constexpr uint8_t kVisibleMatchesDefault = 5;
inline constexpr uint8_t kTableWindowDefault = 5;
inline constexpr uint16_t kSlideshowDefaultS = 15;
// Automatic table step (OQ-196, OQ-197): the next page of a table without an
// own club, or the next competition's table.
inline constexpr uint16_t kTablePageDefaultS = 8;
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
// Login sessions (several browsers at once, least recently used replaced);
// every accepted request moves the expiry to now + kSessionLifetimeMs.
inline constexpr uint32_t kSessionLifetimeMs = 900000;
inline constexpr uint8_t kSessionSlots = 4;
// Request body bytes the server reads and discards before it answers a
// rejected request, so the client receives the status instead of a reset
// (D6). A longer body (firmware) ends with a closed connection.
inline constexpr std::size_t kDrainMaxBytes = 32768;
inline constexpr std::size_t kImageMaxBytes = 32768;
inline constexpr std::size_t kImageTotalBytes = 327680;
inline constexpr std::size_t kImageReserveBytes = 40960;
// Competition files (football_files.h): "comp_<id>.bin" per competition,
// written through a temporary file. A competition with new data is written
// at most every kCompetitionFileGapS and never while one of its matches is
// in its live window. All football files together (competition files and
// the short-name file) use at most kFootballFilesBytes, counted in whole
// filesystem blocks of kFsBlockBytes, and a write always leaves
// kImageReserveBytes free.
inline constexpr char kCompetitionFilePrefix[] = "comp_";
inline constexpr char kCompetitionFileSuffix[] = ".bin";
inline constexpr char kFootballTempFile[] = ".football_tmp";
inline constexpr uint16_t kCompetitionFileGapS = 600;
inline constexpr std::size_t kFootballFilesBytes = 49152;
inline constexpr std::size_t kFsBlockBytes = 4096;
// The owner's team short names (OQ-185): replacements per provider and team
// id, at most kShortNamesMax, in one file.
inline constexpr char kShortNameFile[] = "short_names.bin";
inline constexpr std::size_t kShortNamesMax = 48;
inline constexpr uint16_t kDebugStreamPeriodMs = 1000;
inline constexpr uint16_t kAppPollMs = 20;
inline constexpr uint16_t kUiRefreshMs = 500;
// Status pill (OQ-284, OQ-285): shown this long after a state change
// (failure, network, time, stale data), then the screen is free again.
inline constexpr uint16_t kStatusPillS = 5;
// Opacity of the status pill's background (0..255): slightly transparent,
// the content under it stays visible (OQ-284).
inline constexpr uint8_t kStatusPillOpa = 204;
inline constexpr uint16_t kHighlightRotateMs = 5000;

// Longest field path of a rejected value, e.g. "competitions[2].primary.team"
// (NUL included); the API error names the field with it.
inline constexpr std::size_t kFieldPathBytes = 48;

// Validation and schema share these bounds. Browser controls read the schema.
struct Range {
  uint16_t min, max;
  constexpr bool contains(uint16_t n) const { return n >= min && n <= max; }
};
inline constexpr Range kBrightnessRange{0, 100}, kRotationRange{0, 3};
inline constexpr Range kVisibleMatchesRange{1, 5}, kTableWindowRange{1, 9};
inline constexpr Range kReturnTimeRange{0, 4000}, kScrollRepeatRange{100, 2000};
inline constexpr Range kSlideshowRange{3, 3600}, kDayMinutesRange{0, 1439};
inline constexpr Range kMatchWindowRange{0, 180};
inline constexpr Range kTablePageRange{3, 600};
// Request limits (0 = none): requests per UTC day, requests per window and
// the window length in seconds. A window limit needs both values or none.
inline constexpr Range kDailyRequestsRange{0, 65535},
    kWindowRequestsRange{0, 65535}, kWindowRange{0, 86400 / 2};
inline constexpr Range kTextScaleRange{70, 130};
inline constexpr uint8_t kTextScaleDefault = 100;
inline constexpr std::size_t kProviderHeaderBytes = 8192;
inline constexpr std::size_t kProviderWireOverheadBytes = 32768;
inline constexpr uint16_t kProviderWaitSliceMs = 100;
inline constexpr uint16_t kProviderErrorRetryS = 60, kProviderRateRetryS = 300;
// Ceiling of the failure backoff for jobs with a shorter interval (OQ-262):
// a failing request is retried after 60, 120, 240, 480, then every 900 s;
// longer intervals grow up to their interval.
inline constexpr uint16_t kProviderErrorRetryMaxS = 900;
// Maximum age of reused OpenLigaDB data: a full refresh of the round (and of
// another round with a fixture in its match window) is forced after this
// time even with an unchanged change date.
inline constexpr uint16_t kOpenLigaDbFullRefreshS = 3600;
inline constexpr uint16_t kNetworkPublishMs = 1000;
inline constexpr uint8_t kScanEntries = 16, kApClients = 2, kApChannel = 1;
inline constexpr uint8_t kApIp[] = {192, 168, 4, 1};
inline constexpr char kApNamePrefix[] = "Fussball";
// Image id 0 is the stock image (a firmware file); ids 1..kImageLibraryMax are
// uploaded library files "img<id>.jpg".
inline constexpr uint8_t kStockImageId = 0;
enum StockImage : uint8_t { kStockCrest, kStockBackground, kStockImageCount };
inline constexpr char kStockImageFiles[][32] = {"default_crest.jpg",
                                                "default_background.jpg"};
// Protected images (OQ-151): the replacement of a stock image is a separate
// file; the factory file above stays. A replacement counts towards
// kImageTotalBytes but not towards kImageLibraryMax (OQ-154).
inline constexpr char kRoleImageFiles[][32] = {"role_crest.jpg",
                                               "role_background.jpg"};
// Named image files of settings format 2 and earlier. Format 3 does not
// migrate them; the image service removes them once (OQ-73).
inline constexpr char kLegacyImageNames[][12] = {
    "crest",  "slide1",      "slide2",     "slide3", "slide4",
    "slide5", "live_single", "live_multi", "table",  "slideshow"};
inline constexpr std::size_t kProviderCount = 4;
static_assert(sizeof(kDailyRequestsDefault) / sizeof(uint16_t) ==
                  kProviderCount &&
              sizeof(kWindowRequestsDefault) / sizeof(uint16_t) ==
                  kProviderCount &&
              sizeof(kWindowDefaultS) / sizeof(uint16_t) == kProviderCount);
// Daily request counters: one record per provider with a daily cap, so a
// damaged record affects only its provider. The single record of earlier
// firmware ("credits", all providers) is read once for API-Football.
inline constexpr char kBudgetNvsNamespace[] = "provider_budget",
                      kBudgetLegacyNvsKey[] = "credits";
inline constexpr char kBudgetNvsKeys[][16] = {"openligadb", "api_football",
                                              "espn", "football_data"};
// The optional global daily cap over all providers (same record format).
inline constexpr char kBudgetGlobalNvsKey[] = "global";
inline constexpr uint32_t kBudgetLegacyMagic = 0x46554231,
                          kBudgetRecordMagic = 0x46554232;
inline constexpr uint8_t kBudgetReservationRequests = 8;
inline constexpr char kSelectionCountryDefault[] = "Germany";
// The OpenLigaDB competition list loads one season. Without a season in the
// request it is the current one: from this month on the season named after
// the current year, before it the previous year's.
inline constexpr int kSeasonStartMonth = 7;
inline constexpr uint32_t kNightBackground = 0x030609;
inline constexpr uint16_t kSizeClassSmallMax = 260, kSizeClassMediumMax = 400;
// Display design (docs/UI.md "Layout rules"): font role sizes in px per size
// class (caption, body, title, score); the text scale picks the largest
// compiled font at or below role x scale / 100.
inline constexpr uint8_t kTextRolePx[3][4] = {
    {12, 14, 18, 48}, {16, 20, 26, 72}, {20, 26, 34, 96}};
// Semantic colours: status lines and state-card rims (warn, error, nothing to
// show), IP chip and own Konferenz row, slideshow dots.
inline constexpr uint32_t kWarnColour = 0xF2B84B, kErrorColour = 0xEF5B5B,
                          kNeutralColour = 0x40505C, kChipColour = 0x22303C,
                          kSlideDotOnColour = 0xFFFFFF,
                          kSlideDotOffColour = 0x707880;
// A Konferenz row is drawn in the accent colour for this long after a goal.
inline constexpr uint16_t kGoalFlashS = 120;

}  // namespace cfg
