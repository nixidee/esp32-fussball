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
// top (queue: 4 state kinds + these slots, 16 B per entry).
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

// ---- Settings record -------------------------------------------------------
// Format version of the stored record (append-only, ADR-018): increase only
// when a field is reordered, removed or changes meaning or type. A new
// setting is appended and keeps the version.
inline constexpr uint16_t kSettingsFormatVersion = 1;
// Longest stored record that is read (newer firmware may have appended
// fields); a longer one counts as damaged.
inline constexpr std::size_t kSettingsMaxRecordBytes = 1024;
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

// ---- Settings: board --------------------------------------------------------
// External antenna (only boards with an antenna switch use it).
inline constexpr bool kExternalAntennaDefault = false;

// ---- Settings: debug --------------------------------------------------------
// Periodic status log (free heap, largest block, low-water mark, LVGL pool,
// main task stack). Sent to the console; routed through the planned debug
// helper later.
inline constexpr bool kDebugStatusLogDefault = true;
inline constexpr uint16_t kDebugStatusIntervalDefaultS = 30;
inline constexpr uint16_t kDebugStatusIntervalMinS = 5;
inline constexpr uint16_t kDebugStatusIntervalMaxS = 3600;

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

}  // namespace cfg
