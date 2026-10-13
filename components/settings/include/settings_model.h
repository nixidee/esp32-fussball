// Runtime settings model: fixed-capacity values, validation against the
// limits in app_config.h and the initial values (secrets.h presets over
// defaults). Pure C++, independent of ESP-IDF; storage lives in
// components/core.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "app_config.h"
#include "hw_profile.h"

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

// Ordered list: fixed storage plus a count. Entries 0..count-1 are always
// valid (no holes); comparison ignores the unused storage.
template <typename T, std::size_t Capacity>
struct List {
  static_assert(Capacity <= 255, "count is stored in one byte");
  static constexpr std::size_t kCapacity = Capacity;

  uint8_t count = 0;
  std::array<T, Capacity> items{};

  std::size_t size() const noexcept { return count; }
  bool empty() const noexcept { return count == 0; }
  bool full() const noexcept { return count == Capacity; }
  T& operator[](std::size_t index) noexcept { return items[index]; }
  const T& operator[](std::size_t index) const noexcept {
    return items[index];
  }
  T* begin() noexcept { return items.data(); }
  T* end() noexcept { return items.data() + count; }
  const T* begin() const noexcept { return items.data(); }
  const T* end() const noexcept { return items.data() + count; }

  // Fails and keeps the list if it is full.
  bool push(const T& value) noexcept {
    if (full()) return false;
    items[count++] = value;
    return true;
  }
  void clear() noexcept {
    items = {};
    count = 0;
  }

  bool operator==(const List& other) const noexcept {
    return count == other.count && std::equal(begin(), end(), other.begin());
  }
};

// Where one competition's data comes from: a provider's competition and team.
struct Source {
  cfg::Provider provider = cfg::kProviderDefault;
  Bytes<cfg::kCompetitionIdMaxBytes> competition;
  Bytes<cfg::kSeasonMaxBytes> season;
  Bytes<cfg::kTeamIdMaxBytes> team;
  bool complete() const noexcept {
    return !competition.empty() && !team.empty();
  }
  bool operator==(const Source&) const noexcept = default;
};

// A league or cup the team plays in. 'active' false pauses it (the entry stays
// complete and visible). The fallback is a second source of another provider.
// The display names are stored when a competition and club are picked from a
// provider list (OQ-75); they may be empty.
struct Competition {
  bool active = cfg::kEntryActiveDefault;
  bool fallback_active = cfg::kFallbackActiveDefault;
  Source primary;
  Source fallback;
  Bytes<cfg::kDisplayNameMaxBytes> competition_name;
  Bytes<cfg::kDisplayNameMaxBytes> team_name;
  bool operator==(const Competition&) const noexcept = default;
};

struct ScreenStyle {
  bool enabled = cfg::kScreenEnabledDefault;
  bool background = cfg::kBackgroundsDefault;
  uint32_t colour = cfg::kThemeBackgroundDefault;
  uint32_t text = cfg::kThemeTextDefault;
  uint32_t accent = cfg::kThemeAccentDefault;
  uint8_t text_scale = cfg::kTextScaleDefault;
  // Library image id (1..cfg::kImageLibraryMax) or cfg::kStockImageId. The
  // crest screen shows the crest or its slides and has no background image.
  uint8_t image = cfg::kStockImageId;
  bool operator==(const ScreenStyle&) const noexcept = default;
};

const char* fieldName(Field field) noexcept;

// Explicitly verified provider fixture identity of a competition (index into
// Model::competitions): the match's id at the primary and at the fallback
// provider; no name/time matching.
struct FixturePair {
  bool active = cfg::kEntryActiveDefault;
  bool swapped = cfg::kFixturePairSwappedDefault;
  uint8_t competition = 0;
  Bytes<cfg::kIdBytes> primary_id, secondary_id;
  bool operator==(const FixturePair&) const noexcept = default;
};

// Role per input slot of a profile with `inputs` inputs (cfg::inputRoleDefault).
using RequestLimits = std::array<uint16_t, cfg::kProviderCount>;
constexpr RequestLimits requestLimits(
    const uint16_t (&values)[cfg::kProviderCount]) noexcept {
  RequestLimits limits{};
  for (std::size_t i = 0; i < limits.size(); ++i) limits[i] = values[i];
  return limits;
}

using InputRoles = std::array<cfg::InputRole, hw::kMaxInputs>;
constexpr InputRoles inputRolesDefault(std::size_t inputs) noexcept {
  InputRoles roles{};
  for (std::size_t i = 0; i < roles.size(); ++i)
    roles[i] = cfg::inputRoleDefault(i, inputs);
  return roles;
}

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
  Bytes<cfg::kAdminPasswordMaxBytes> admin_password;
  Bytes<cfg::kApiFootballKeyMaxBytes> api_football_key;
  Bytes<cfg::kFootballDataKeyMaxBytes> football_data_key;
  std::array<ScreenStyle, cfg::kScreenCount> screens{};
  bool espn_opt_in = cfg::kEspnOptInDefault;
  bool demo = cfg::kDemoDefault;
  bool backgrounds = cfg::kBackgroundsDefault;
  bool night_enabled = cfg::kNightEnabledDefault;
  bool ip_badge_ap_permanent = cfg::kIpBadgeApPermanentDefault;
  bool ip_badge_bottom = cfg::kIpBadgeBottomDefault;
  bool browser_debug = cfg::kBrowserDebugDefault;
  bool pin_own_match = cfg::kPinOwnMatchDefault;  // format 4
  bool whole_matchday = cfg::kWholeMatchdayDefault;  // format 5
  cfg::ConferenceMode conference_mode = cfg::kConferenceModeDefault;  // format 5
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

  // Format 3 lists. The first active competition supplies the league table.
  List<Competition, cfg::kCompetitionMax> competitions;
  List<FixturePair, cfg::kFixtureMappingMax> fixture_pairs;
  // Crest screen slideshow in play order: 0 = the crest, else a library id.
  List<uint8_t, cfg::kSlideMax> slides;

  // Format 5 (stored after pin_own_match). Input slots after the profile's
  // inputs are stored but unused.
  InputRoles input_roles = inputRolesDefault(hw::kMaxInputs);

  // Format 7, indexed by cfg::Provider: requests per UTC day (0 = no cap)
  // and the minimum spacing between two request starts in seconds.
  RequestLimits daily_requests = requestLimits(cfg::kDailyRequestsDefault);
  RequestLimits request_spacing_s = requestLimits(cfg::kRequestSpacingDefaultS);

  bool operator==(const Model& other) const noexcept = default;
};

// Limits of single values (also used for secrets.h presets).
bool isValidWifiPassword(std::string_view value) noexcept;
bool isValidApPassword(std::string_view value) noexcept;
bool isValidHostname(std::string_view value) noexcept;
bool isValidStatusInterval(uint16_t seconds) noexcept;
bool isValidTimeZone(std::string_view label) noexcept;
bool isValidNtpServer(std::string_view value) noexcept;
// A provider's competition, team or fixture identifier (may be empty): any
// text without control characters and without a space at either end. Some
// OpenLigaDB shortcuts contain spaces or umlauts; requests percent-encode it.
bool isValidIdentifier(std::string_view value) noexcept;
// A season: empty, or digits (exactly four unless the provider is OpenLigaDB).
bool isValidSeason(cfg::Provider provider, std::string_view season) noexcept;

// Maxima of the two settings whose range depends on the display layout
// (OQ-85, OQ-92) and the number of inputs of the target (OQ-141). The boot
// code passes the profile's values; the defaults are the widest bounds.
struct Limits {
  uint8_t visible_matches_max = static_cast<uint8_t>(cfg::kVisibleMatchesRange.max);
  uint8_t table_window_max = static_cast<uint8_t>(cfg::kTableWindowRange.max);
  uint8_t inputs = static_cast<uint8_t>(hw::kMaxInputs);
};

// Why a value was rejected. The names (reasonName) are the stable codes the
// REST API reports next to the field path.
enum class Reason : uint8_t {
  kInvalid,      // wrong format or characters
  kRange,        // number outside its bounds
  kType,         // JSON value of the wrong type
  kUnknownKey,   // JSON key the device does not know
  kTooLong,      // text longer than its capacity
  kTooMany,      // more list entries than the capacity
  kRequired,     // empty where a value is needed
  kDuplicate,    // value used twice where it must be unique
  kReference,    // points at an entry that does not exist or is inactive
  kConflict,     // contradicts another field
  kNoneEnabled,  // not a single screen enabled
};
const char* reasonName(Reason reason) noexcept;

// A JSON path such as "competitions[1].fallback.team" (names as in the
// settings JSON). Text beyond the buffer is cut, never written past it.
class FieldPath {
 public:
  FieldPath& key(const char* name) noexcept {
    if (length_ > 0) append('.');
    while (*name != '\0') append(*name++);
    return *this;
  }
  FieldPath& index(unsigned n) noexcept {
    append('[');
    if (n >= 10) append(static_cast<char>('0' + n / 10 % 10));
    append(static_cast<char>('0' + n % 10));
    append(']');
    return *this;
  }
  // A copy with one more key; the original stays the parent path.
  FieldPath with(const char* name) const noexcept {
    FieldPath copy = *this;
    copy.key(name);
    return copy;
  }
  FieldPath withIndex(unsigned n) const noexcept {
    FieldPath copy = *this;
    copy.index(n);
    return copy;
  }
  const char* c_str() const noexcept { return text_.data(); }

 private:
  void append(char c) noexcept {
    if (length_ + 1 < text_.size()) text_[length_++] = c;
  }
  std::array<char, cfg::kFieldPathBytes> text_{};
  std::size_t length_ = 0;
};

// The first rejected value: where it is and why.
struct Violation {
  std::array<char, cfg::kFieldPathBytes> path{};  // NUL-terminated
  Reason reason = Reason::kInvalid;

  void set(const FieldPath& at, Reason why) noexcept {
    std::size_t n = 0;
    for (const char* c = at.c_str(); *c != '\0' && n + 1 < path.size(); ++c)
      path[n++] = *c;
    path[n] = '\0';
    reason = why;
  }
};

// True if every field is within its limits; otherwise *invalid (if given)
// names the first rejected field group and *violation (if given) the exact
// path. 'limits' narrows the layout-dependent ranges.
bool validate(const Model& model, Field* invalid = nullptr,
              Violation* violation = nullptr,
              const Limits& limits = {}) noexcept;

// True if 'a' and 'b' agree in every value that provider requests or the
// fetched match data depend on: each competition's active flags and sources
// (not its display names), the fixture pairs, both provider keys, the ESPN
// opt-in and demo mode. A save that changes one of them discards the fetched
// data; any other save keeps it.
bool sameProviderData(const Model& a, const Model& b) noexcept;

// True if the device may request 'provider' with these settings: ESPN needs
// the opt-in, API-Football and football-data.org need their key.
bool providerUsable(cfg::Provider provider, const Model& model) noexcept;

// True if every active competition uses only usable providers (the primary
// source, and the fallback source while it is active); otherwise
// *violation (if given) names the first such provider field with kConflict.
// Checked when settings are changed through the REST API, not when a stored
// record is loaded, so an older record keeps its values.
bool checkProviderAccess(const Model& model,
                         Violation* violation = nullptr) noexcept;

// Bit n set = library image n exists (bit 0, the stock image, is ignored).
using ImageMask = uint16_t;
static_assert(cfg::kImageLibraryMax < 16, "ImageMask has 16 bits");

// Replaces every reference to a library image missing from 'present' by the
// stock image and removes it from the slides (a slide never stays empty).
// True if the model changed.
bool dropMissingImages(Model& model, ImageMask present) noexcept;

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
// 'limits' clamps the layout-dependent defaults (the default equals the
// maximum, OQ-85).
Model initialValues(const Presets& presets, FieldMask* rejected = nullptr,
                    const Limits& limits = {}) noexcept;

}  // namespace settings
