// Runtime settings model: fixed-capacity values, validation against the
// limits in app_config.h and the initial values (secrets.h presets over
// defaults). Pure C++, independent of ESP-IDF; storage lives in
// components/core.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "app_config.h"

namespace settings {

// Byte string with explicit length; may contain any byte (an SSID may).
template <std::size_t Capacity>
struct Bytes {
  static_assert(Capacity <= 255, "length is stored in one byte");
  static constexpr std::size_t kCapacity = Capacity;

  uint8_t length = 0;
  std::array<char, Capacity> data{};

  std::string_view view() const noexcept { return {data.data(), length}; }
  bool empty() const noexcept { return length == 0; }

  // Fails and keeps the old value if the value does not fit; never truncates.
  bool assign(std::string_view value) noexcept {
    if (value.size() > Capacity) return false;
    data.fill(0);
    // memcpy: string_view::copy links the libstdc++ exception support (about
    // 2 KB flash), std::copy_n becomes memmove (about 200 B IRAM on the C6).
    if (!value.empty()) std::memcpy(data.data(), value.data(), value.size());
    length = static_cast<uint8_t>(value.size());
    return true;
  }

  bool operator==(const Bytes& other) const noexcept {
    return view() == other.view();
  }
};

// Fields in stored order (record format 1). Append only.
enum class Field : uint8_t {
  kWifiSsid,
  kWifiPassword,
  kApPassword,
  kHostname,
  kExternalAntenna,
  kDebugStatusLog,
  kDebugStatusIntervalS,
  kTimeZone,
  kNtpServer,
  kApplication,
};
inline constexpr std::size_t kFieldCount = 10;

struct Route {
  bool enabled = cfg::kRouteEnabledDefault;
  cfg::Provider provider = cfg::kProviderDefault;
  Bytes<32> competition;
  Bytes<16> season;
  Bytes<32> team;
  bool operator==(const Route&) const noexcept = default;
};

struct ScreenStyle {
  bool enabled = cfg::kScreenEnabledDefault;
  bool background = cfg::kBackgroundsDefault;
  uint32_t colour = cfg::kThemeBackgroundDefault;
  uint32_t text = cfg::kThemeTextDefault;
  uint32_t accent = cfg::kThemeAccentDefault;
  uint8_t text_scale = cfg::kTextScaleDefault;
  bool operator==(const ScreenStyle&) const noexcept = default;
};

const char* fieldName(Field field) noexcept;

// Explicitly verified provider fixture identity; no name/time matching.
struct FixtureMapping {
  bool enabled = cfg::kFixtureMappingEnabledDefault;
  uint8_t route = 0;
  bool swapped = cfg::kFixtureMappingSwappedDefault;
  Bytes<cfg::kIdBytes> primary, secondary;
  bool operator==(const FixtureMapping&) const noexcept = default;
};

struct Model {
  // Boot construction initializes the fixed defaults without flash-resident
  // copies of every large global model. No allocation or persistent write.
  Model() noexcept {}
  Bytes<cfg::kWifiSsidMaxBytes> wifi_ssid;          // empty = no network
  Bytes<cfg::kWifiPskHexChars> wifi_password;       // empty = open network
  Bytes<cfg::kWifiPassphraseMaxChars> ap_password;  // empty = open AP
  Bytes<cfg::kHostnameMaxChars> hostname;
  bool external_antenna = cfg::kExternalAntennaDefault;
  bool debug_status_log = cfg::kDebugStatusLogDefault;
  uint16_t debug_status_interval_s = cfg::kDebugStatusIntervalDefaultS;
  Bytes<cfg::kTimeZoneLabelMaxChars> time_zone;  // label of cfg::kTimeZones
  Bytes<cfg::kNtpServerMaxChars> ntp_server;

  // Append-only application settings, after the original nine fields.
  Bytes<64> admin_password;
  Bytes<96> api_football_key;
  Bytes<64> football_data_key;
  std::array<Route, cfg::kRouteCount> routes{};
  std::array<ScreenStyle, cfg::kScreenCount> screens{};
  bool espn_opt_in = cfg::kEspnOptInDefault;
  bool demo = cfg::kDemoDefault;
  bool backgrounds = cfg::kBackgroundsDefault;
  bool night_enabled = cfg::kNightEnabledDefault;
  bool ip_badge_ap_permanent = cfg::kIpBadgeApPermanentDefault;
  bool ip_badge_bottom = cfg::kIpBadgeBottomDefault;
  bool browser_debug = cfg::kBrowserDebugDefault;
  uint8_t language = cfg::kLanguageDefault;  // 0 German, 1 English
  uint8_t brightness = cfg::kBrightnessDefault;
  uint8_t night_brightness = cfg::kNightBrightnessDefault;
  uint8_t rotation = cfg::kRotationDefault;
  uint8_t visible_matches = cfg::kVisibleMatchesDefault;
  uint8_t table_window = cfg::kTableWindowDefault;
  cfg::Screen idle_screen = cfg::kIdleScreenDefault;
  cfg::Screen matchday_screen = cfg::kMatchdayScreenDefault;
  cfg::Screen own_match_screen = cfg::kOwnMatchScreenDefault;
  uint16_t manual_return_s = cfg::kManualReturnDefaultS;
  uint16_t scroll_reset_s = cfg::kScrollResetDefaultS;
  uint16_t scroll_repeat_ms = cfg::kScrollRepeatDefaultMs;
  uint16_t slideshow_s = cfg::kSlideshowDefaultS;
  uint16_t night_start = cfg::kNightStartDefaultMinutes;
  uint16_t night_end = cfg::kNightEndDefaultMinutes;
  uint16_t ip_badge_s = cfg::kIpBadgeDefaultS;
  uint16_t window_before = cfg::kMatchWindowDefaultMinutes;
  uint16_t window_after = cfg::kMatchWindowDefaultMinutes;
  uint16_t api_daily_budget = cfg::kProviderBudgetDefault;
  std::array<Route, cfg::kRouteCount> fallback_routes{};
  std::array<FixtureMapping, cfg::kFixtureMappings> fixture_mappings{};

  bool operator==(const Model& other) const noexcept = default;
};

// Limits of single values (also used for secrets.h presets).
bool isValidWifiPassword(std::string_view value) noexcept;
bool isValidApPassword(std::string_view value) noexcept;
bool isValidHostname(std::string_view value) noexcept;
bool isValidStatusInterval(uint16_t seconds) noexcept;
bool isValidTimeZone(std::string_view label) noexcept;
bool isValidNtpServer(std::string_view value) noexcept;

// True if every field is within its limits; otherwise *invalid (if given)
// names the first rejected field.
bool validate(const Model& model, Field* invalid = nullptr) noexcept;

// Values from secrets.h; empty = not set.
struct Presets {
  std::string_view wifi_ssid;
  std::string_view wifi_password;
  std::string_view ap_password;
  std::string_view hostname;
  std::string_view time_zone;
  std::string_view admin_password;
  std::string_view api_football_key;
  std::string_view football_data_key;
  std::string_view provider;
  std::string_view competition;
  std::string_view team;
  std::string_view season;
};

// Bit (1 << Field) per preset that was set but violates its limits.
using FieldMask = uint32_t;
inline constexpr FieldMask fieldBit(Field field) noexcept {
  return FieldMask{1} << static_cast<uint8_t>(field);
}

// Defaults from app_config.h, each overridden by its preset when set and
// valid. Invalid presets are skipped (default kept) and reported in *rejected.
Model initialValues(const Presets& presets,
                    FieldMask* rejected = nullptr) noexcept;

}  // namespace settings
