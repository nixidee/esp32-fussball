// Runtime settings model: fixed-capacity values, validation against the
// limits in app_config.h and the initial values (secrets.h presets over
// defaults). Pure C++, independent of ESP-IDF; storage lives in
// this component's settings_store adapter.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "app_config.h"
#include "hw_profile.h"
#include "screen_policy.h"
#include "value_utils.h"

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
    values::copyPadded(data.data(), data.size(), value);
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
  const T& operator[](std::size_t index) const noexcept { return items[index]; }
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

// Source identity contains no selected-club or display preference.
struct Source {
  cfg::Provider provider = cfg::kProviderDefault;
  Bytes<cfg::kCompetitionIdMaxBytes> competition;
  Bytes<cfg::kSeasonMaxBytes> season;
  bool complete() const noexcept { return !competition.empty(); }
  bool operator==(const Source&) const noexcept = default;
};

struct ClubPreference {
  uint32_t team = 0;  // zero means no preferred club
  Bytes<cfg::kDisplayNameMaxBytes> name;
  bool own_special = cfg::kOwnSpecialDefault;
  uint8_t group = 0;  // explicit membership; zero is ungrouped
  bool operator==(const ClubPreference&) const noexcept = default;
};

// A league or cup, optionally with an own club (OQ-195: an empty team id
// means no own club). 'active' false pauses it (the entry stays complete and
// visible). One source per competition (OQ-247). The display
// names are stored when a competition and club are picked from a provider
// list (OQ-75); they may be empty. 'id' is the stable id the competition
// store assigns (OQ-209; 0 = not stored yet).
struct CompetitionRoute {
  uint8_t id = 0;
  bool active = cfg::kEntryActiveDefault;
  Source primary;
  Bytes<cfg::kDisplayNameMaxBytes> competition_name;
  ClubPreference club;
  // Service-owned identity revision. Never reused when a small id wraps.
  uint32_t incarnation = 0;
  bool hasClub() const noexcept { return club.team != 0; }
  bool operator==(const CompetitionRoute&) const noexcept = default;
};

struct Competition : CompetitionRoute {
  screen_policy::CompetitionPreferences display;
  bool operator==(const Competition&) const noexcept = default;
};
struct CompetitionRoutes {
  List<CompetitionRoute, cfg::kCompetitionMax> entries;
  const CompetitionRoute* find(uint8_t id) const noexcept {
    for (const auto& entry : entries)
      if (entry.id == id) return &entry;
    return nullptr;
  }
  void clear() noexcept { entries.clear(); }
};

struct ScreenStyle {
  bool background = cfg::kBackgroundsDefault;
  uint32_t colour = cfg::kThemeBackgroundDefault;
  uint32_t text = cfg::kThemeTextDefault;
  uint32_t accent = cfg::kThemeAccentDefault;
  uint8_t text_scale = cfg::kTextScaleDefault;
  // Library image id (1..cfg::kImageLibraryMax) or cfg::kStockImageId. The
  // Values are projected from the screen policy by the presenter.
  uint8_t image = cfg::kStockImageId;
  bool operator==(const ScreenStyle&) const noexcept = default;
};

const char* fieldName(Field field) noexcept;

// Competition configuration within AppConfig, separate from general Model.
// 'next_id' is the id the store tries first for a new entry.
struct Competitions {
  List<Competition, cfg::kCompetitionMax> entries;
  uint8_t next_id = 1;
  uint32_t next_incarnation = 1;
  // The entry with this id, or null.
  const Competition* find(uint8_t id) const noexcept {
    for (const auto& entry : entries)
      if (entry.id == id) return &entry;
    return nullptr;
  }
  void clear() noexcept {
    entries.clear();
    next_id = 1;
    next_incarnation = 1;
  }
  bool operator==(const Competitions&) const noexcept = default;
};

// A request limit (OQ-225, OQ-226); every 0 means no limit: at most 'daily'
// requests per UTC day and at most 'window_requests' requests in any
// 'window_s' seconds (both set or both 0).
struct RequestLimit {
  uint16_t daily = 0;
  uint16_t window_requests = 0;
  uint16_t window_s = 0;
  bool operator==(const RequestLimit&) const noexcept = default;
};

// Request plan of one provider: its limit and the interval per data type
// (index cfg::DataType, seconds; OQ-216).
struct ProviderPlan {
  RequestLimit limit;
  std::array<uint32_t, cfg::kDataTypeCount> interval_s{};
  bool operator==(const ProviderPlan&) const noexcept = default;
};
using ProviderPlans = std::array<ProviderPlan, cfg::kProviderCount>;
constexpr ProviderPlans providerPlansDefault() noexcept {
  ProviderPlans plans{};
  for (std::size_t p = 0; p < plans.size(); ++p) {
    plans[p].limit = {cfg::kDailyRequestsDefault[p],
                      cfg::kWindowRequestsDefault[p], cfg::kWindowDefaultS[p]};
    for (std::size_t t = 0; t < cfg::kDataTypeCount; ++t)
      plans[p].interval_s[t] = cfg::kIntervalDefaultS[t];
  }
  return plans;
}
// Stable lower-case key of a data type (settings JSON, logs).
const char* dataTypeName(cfg::DataType type) noexcept;

// Role per input slot of a profile with `inputs` inputs
// (cfg::inputRoleDefault).
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

  // General application settings; screen rules/appearance have one owner in
  // Policy.
  Bytes<cfg::kAdminPasswordMaxBytes> admin_password;
  Bytes<cfg::kApiFootballKeyMaxBytes> api_football_key;
  Bytes<cfg::kFootballDataKeyMaxBytes> football_data_key;
  bool espn_opt_in = cfg::kEspnOptInDefault;
  bool demo = cfg::kDemoDefault;
  bool night_enabled = cfg::kNightEnabledDefault;
  bool ip_badge_ap_permanent = cfg::kIpBadgeApPermanentDefault;
  bool ip_badge_bottom = cfg::kIpBadgeBottomDefault;
  bool browser_debug = cfg::kBrowserDebugDefault;
  uint8_t language = cfg::kLanguageDefault;  // 0 German, 1 English
  uint8_t brightness = cfg::kBrightnessDefault;
  uint8_t night_brightness = cfg::kNightBrightnessDefault;
  uint8_t rotation = cfg::kRotationDefault;
  uint16_t scroll_reset_s = cfg::kScrollResetDefaultS;
  uint16_t scroll_repeat_ms = cfg::kScrollRepeatDefaultMs;
  uint16_t night_start = cfg::kNightStartDefaultMinutes;
  uint16_t night_end = cfg::kNightEndDefaultMinutes;
  uint16_t ip_badge_s = cfg::kIpBadgeDefaultS;

  // Input slots after the profile's inputs are stored but unused.
  InputRoles input_roles = inputRolesDefault(hw::kMaxInputs);

  // Request plan per provider (index cfg::Provider) and the optional limit
  // over all providers together (default off, OQ-222).
  ProviderPlans providers = providerPlansDefault();
  RequestLimit global_limit;
  // Capture remains available under Debug; this only adds its Overview trigger.
  bool overview_screenshot = cfg::kOverviewScreenshotDefault;
  bool operator==(const Model& other) const noexcept = default;
};

// Limits of single values (also used for secrets.h presets).
bool isValidWifiPassword(std::string_view value) noexcept;
bool isValidApPassword(std::string_view value) noexcept;
bool isValidHostname(std::string_view value) noexcept;
bool isValidStatusInterval(uint16_t seconds) noexcept;
bool isValidTimeZone(std::string_view label) noexcept;
bool isValidNtpServer(std::string_view value) noexcept;
// A provider's textual competition identifier (may be empty): any
// text without control characters and without a space at either end. Some
// OpenLigaDB shortcuts contain spaces or umlauts; requests percent-encode it.
bool isValidIdentifier(std::string_view value) noexcept;
// No club, or the positive uint32 identity accepted by every current mapper.
bool isValidTeamId(std::string_view value) noexcept;
// A season: empty, or digits (exactly four unless the provider is OpenLigaDB).
bool isValidSeason(cfg::Provider provider, std::string_view season) noexcept;

// Maxima of the two settings whose range depends on the display layout
// (OQ-85, OQ-92) and the number of inputs of the target (OQ-141). The boot
// code passes the profile's values; the defaults are the widest bounds.
struct Limits {
  uint8_t visible_matches_max =
      static_cast<uint8_t>(cfg::kVisibleMatchesRange.max);
  uint8_t table_window_max = static_cast<uint8_t>(cfg::kTableWindowRange.max);
  uint8_t inputs = static_cast<uint8_t>(hw::kMaxInputs);
};

// Why a value was rejected. The names (reasonName) are the stable codes the
// REST API reports next to the field path.
enum class Reason : uint8_t {
  kInvalid,     // wrong format or characters
  kRange,       // number outside its bounds
  kType,        // JSON value of the wrong type
  kUnknownKey,  // JSON key the device does not know
  kTooLong,     // text longer than its capacity
  kTooMany,     // more list entries than the capacity
  kRequired,    // empty where a value is needed
  kDuplicate,   // value used twice where it must be unique
  kConflict,    // contradicts another field
};
const char* reasonName(Reason reason) noexcept;

// A JSON path such as "competitions[1].club.team" (names as in the
// settings or competitions JSON). Text beyond the buffer is cut, never written
// past it.
class FieldPath {
 public:
  FieldPath& key(const char* name) noexcept {
    if (length_ > 0) append('.');
    while (*name != '\0') append(*name++);
    return *this;
  }
  FieldPath& index(unsigned n) noexcept {
    append('[');
    char digits[10];
    unsigned count = 0;
    do {
      digits[count++] = static_cast<char>('0' + n % 10);
      n /= 10;
    } while (n != 0);
    while (count) append(digits[--count]);
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

// True if every competition is within its limits: ids set and unique, the
// competition given (the club is optional, OQ-195) and screen bits known.
// Otherwise *violation (if given) names the first rejected value in the
// competitions JSON.
bool validate(const Competitions& competitions,
              Violation* violation = nullptr) noexcept;

// True if 'a' and 'b' agree in every value that provider requests or the
// fetched match data depend on: both provider keys, the ESPN opt-in and demo
// mode; for the competitions their order, ids, active flags and sources (not
// display names or screen switches). A save that
// changes one of them discards the fetched data; any other save keeps it.
bool sameProviderData(const Model& a, const Model& b) noexcept;
bool sameProviderData(const Competitions& a, const Competitions& b) noexcept;

// True if the device may request 'provider' with these settings: ESPN needs
// the opt-in, API-Football and football-data.org need their key.
bool providerUsable(cfg::Provider provider, const Model& model) noexcept;

// True if every active competition uses only a usable provider; otherwise
// *violation (if given) names the first such provider field with kConflict.
// Checked when settings or competitions are changed, not when a stored
// record is loaded, so an older record keeps its values.
bool checkProviderAccess(const Model& model, const Competitions& competitions,
                         Violation* violation = nullptr) noexcept;

// Gives every entry with id 0 an unused id, trying 'next_id' first, and
// advances 'next_id' past it (1..255, wrapping). Entries with an id keep it.
void assignIds(Competitions& competitions) noexcept;

// Service-side allocation: takes both counters from current, preserves the
// incarnation of existing ids across edits/reorders, and gives new entries
// (id 0 or an unknown id) a fresh incarnation. Caller-supplied incarnations
// are never trusted. Refuses exhaustion rather than wrapping the counter.
// Candidate is scratch and may change on failure; current remains unchanged.
bool assignIncarnations(Competitions& candidate, const Competitions& current,
                        Violation* violation = nullptr) noexcept;

// Bit n identifies library image n (bit 0, the stock image, is ignored).
using ImageMask = uint16_t;
static_assert(cfg::kImageLibraryMax < 16, "ImageMask has 16 bits");
inline constexpr ImageMask kLibraryImageMask =
    static_cast<ImageMask>((ImageMask{1} << (cfg::kImageLibraryMax + 1)) - 2);

// Saved desired library ids, including disabled screens and every slide.
// Missing files remain references; stock id 0 and invalid ids are ignored.
namespace screen_policy {
struct Policy;
}
ImageMask referencedImages(const screen_policy::Policy& policy) noexcept;

// Replaces every reference to a library image missing from 'present' by the
// stock image and removes it from the slides (a slide never stays empty).
// True if the model changed.
bool dropMissingImages(screen_policy::Policy& policy,
                       ImageMask present) noexcept;

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
// 'limits' selects input-role defaults for the hardware profile.
Model initialValues(const Presets& presets, FieldMask* rejected = nullptr,
                    const Limits& limits = {}) noexcept;

// Writes the competition preset of secrets.h (provider, competition, club,
// season) into 'out' as the only competition with id 1, or none. An invalid
// preset is skipped and reported in *rejected as Field::kApplication. 'out'
// is a parameter so the 2.5 KB store never lands on a caller's stack.
void initialCompetitions(const Presets& presets, Competitions& out,
                         FieldMask* rejected = nullptr) noexcept;

}  // namespace settings
