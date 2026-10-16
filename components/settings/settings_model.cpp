#include "settings_model.h"

#include <algorithm>
#include <array>
#include <utility>

#include "civil_time.h"

namespace settings {

namespace {

bool isPrintableAscii(char c) { return c >= 0x20 && c <= 0x7e; }
bool isHeaderValue(std::string_view text) {
  return std::all_of(text.begin(), text.end(),
                     [](char c) { return c >= 0x21 && c <= 0x7e; });
}

bool isHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool isHostnameChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c == '-';
}

// DNS label: 1..63 letters, digits or hyphens, no hyphen at either end.
bool isDnsLabel(std::string_view label) {
  return !label.empty() && label.size() <= 63 && label.front() != '-' &&
         label.back() != '-' &&
         std::all_of(label.begin(), label.end(), isHostnameChar);
}

bool isPassphrase(std::string_view value) {
  return value.size() >= cfg::kWifiPassphraseMinChars &&
         value.size() <= cfg::kWifiPassphraseMaxChars &&
         std::all_of(value.begin(), value.end(), isPrintableAscii);
}

}  // namespace

const char* fieldName(Field field) noexcept {
  switch (field) {
    case Field::kWifiSsid: return "wifi_ssid";
    case Field::kWifiPassword: return "wifi_password";
    case Field::kApPassword: return "ap_password";
    case Field::kHostname: return "hostname";
    case Field::kExternalAntenna: return "external_antenna";
    case Field::kDebugStatusLog: return "debug_status_log";
    case Field::kDebugStatusIntervalS: return "debug_status_interval_s";
    case Field::kTimeZone: return "time_zone";
    case Field::kNtpServer: return "ntp_server";
    case Field::kApplication: return "application";
  }
  return "unknown";
}

bool isValidWifiPassword(std::string_view value) noexcept {
  if (value.empty() || isPassphrase(value)) return true;
  return value.size() == cfg::kWifiPskHexChars &&
         std::all_of(value.begin(), value.end(), isHexDigit);
}

bool isValidApPassword(std::string_view value) noexcept {
  return value.empty() || isPassphrase(value);
}

bool isValidHostname(std::string_view value) noexcept {
  return value.size() <= cfg::kHostnameMaxChars && isDnsLabel(value);
}

bool isValidStatusInterval(uint16_t seconds) noexcept {
  return seconds >= cfg::kDebugStatusIntervalMinS &&
         seconds <= cfg::kDebugStatusIntervalMaxS;
}

bool isValidTimeZone(std::string_view label) noexcept {
  return timekeeping::findZone(label) != nullptr;
}

bool isValidNtpServer(std::string_view value) noexcept {
  if (value.empty() || value.size() > cfg::kNtpServerMaxChars) return false;
  // No substr(): it may throw, which links the exception support.
  while (true) {
    const std::size_t dot = value.find('.');
    const bool last = dot == std::string_view::npos;
    if (!isDnsLabel({value.data(), last ? value.size() : dot})) return false;
    if (last) return true;
    value.remove_prefix(dot + 1);
  }
}

bool isValidIdentifier(std::string_view value) noexcept {
  return (value.empty() || (value.front() != ' ' && value.back() != ' ')) &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f;
         });
}

bool isValidSeason(cfg::Provider provider, std::string_view season) noexcept {
  return season.empty() ||
         ((provider == cfg::Provider::kOpenLigaDb || season.size() == 4) &&
          std::all_of(season.begin(), season.end(),
                      [](char c) { return c >= '0' && c <= '9'; }));
}

const char* dataTypeName(cfg::DataType type) noexcept {
  switch (type) {
    case cfg::DataType::kFixtures: return "fixtures";
    case cfg::DataType::kTable: return "table";
    case cfg::DataType::kLive: return "live";
    case cfg::DataType::kLiveTable: return "live_table";
    case cfg::DataType::kEvents: return "events";
  }
  return "unknown";
}

bool sameProviderData(const Model& a, const Model& b) noexcept {
  return a.api_football_key == b.api_football_key &&
         a.football_data_key == b.football_data_key &&
         a.espn_opt_in == b.espn_opt_in && a.demo == b.demo;
}

bool sameProviderData(const Competitions& a, const Competitions& b) noexcept {
  if (a.entries.size() != b.entries.size()) return false;
  for (std::size_t i = 0; i < a.entries.size(); ++i) {
    const auto& x = a.entries[i];
    const auto& y = b.entries[i];
    if (x.id != y.id || x.active != y.active || !(x.primary == y.primary))
      return false;
  }
  return true;
}

void assignIds(Competitions& competitions) noexcept {
  const auto used = [&](uint8_t id) {
    return competitions.find(id) != nullptr;
  };
  const auto advance = [](uint8_t id) {
    return static_cast<uint8_t>(id == 255 ? 1 : id + 1);
  };
  if (competitions.next_id == 0) competitions.next_id = 1;
  for (auto& entry : competitions.entries) {
    if (entry.id != 0) continue;
    // At most kCompetitionMax ids are in use, so a free one is found.
    uint8_t id = competitions.next_id;
    while (used(id)) id = advance(id);
    entry.id = id;
    competitions.next_id = advance(id);
  }
}

bool providerUsable(cfg::Provider provider, const Model& model) noexcept {
  switch (provider) {
    case cfg::Provider::kEspn:
      return model.espn_opt_in;
    case cfg::Provider::kApiFootball:
      return !model.api_football_key.empty();
    case cfg::Provider::kFootballData:
      return !model.football_data_key.empty();
    default:
      return true;
  }
}

bool checkProviderAccess(const Model& model, const Competitions& competitions,
                         Violation* violation) noexcept {
  for (unsigned i = 0; i < competitions.entries.size(); ++i) {
    const auto& entry = competitions.entries[i];
    if (!entry.active) continue;
    if (providerUsable(entry.primary.provider, model)) continue;
    if (violation != nullptr) {
      FieldPath at;
      at.key("competitions").index(i).key("primary").key("provider");
      violation->set(at, Reason::kConflict);
    }
    return false;
  }
  return true;
}

namespace {

// Records the first violation and reports false so callers can return it.
class Checker {
 public:
  explicit Checker(Violation* out) : out_(out) {}
  bool fail(const FieldPath& path, Reason reason) {
    if (out_ != nullptr && !failed_) out_->set(path, reason);
    failed_ = true;
    return false;
  }
  // Fails with kRange at the key 'name' unless the number is within 'range'.
  bool range(const char* name, cfg::Range range, unsigned value) {
    return (value <= 0xffff && range.contains(static_cast<uint16_t>(value))) ||
           fail(FieldPath().with(name), Reason::kRange);
  }
  bool invalid(const char* name, bool valid) {
    return valid || fail(FieldPath().with(name), Reason::kInvalid);
  }

 private:
  Violation* out_;
  bool failed_ = false;
};

// Display names come from provider lists: any text without control
// characters (they are shown on the display and embedded in JSON).
bool isDisplayName(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) {
    return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f;
  });
}

// An empty season means the provider's current season, resolved at runtime
// for every provider (owner decision D3).
bool checkSource(Checker& check, const FieldPath& at, const Source& source) {
  if (static_cast<unsigned>(source.provider) >= cfg::kProviderCount)
    return check.fail(at.with("provider"), Reason::kRange);
  if (!isValidIdentifier(source.competition.view()))
    return check.fail(at.with("competition"), Reason::kInvalid);
  if (!isValidIdentifier(source.team.view()))
    return check.fail(at.with("team"), Reason::kInvalid);
  if (!isValidSeason(source.provider, source.season.view()))
    return check.fail(at.with("season"), Reason::kInvalid);
  return true;
}

bool checkSources(Checker& check, const Competitions& competitions) {
  for (unsigned i = 0; i < competitions.entries.size(); ++i) {
    const auto& entry = competitions.entries[i];
    FieldPath at;
    at.key("competitions").index(i);
    if (entry.id == 0) return check.fail(at.with("id"), Reason::kRequired);
    for (unsigned j = 0; j < i; ++j)
      if (competitions.entries[j].id == entry.id)
        return check.fail(at.with("id"), Reason::kDuplicate);
    if (entry.screens >> cfg::kCompetitionScreenCount)
      return check.fail(at.with("screens"), Reason::kRange);
    const auto primary = at.with("primary");
    if (entry.primary.competition.empty())
      return check.fail(FieldPath(primary).with("competition"),
                        Reason::kRequired);
    if (!checkSource(check, primary, entry.primary)) return false;
    if (!isDisplayName(entry.competition_name.view()))
      return check.fail(at.with("competition_name"), Reason::kInvalid);
    if (!isDisplayName(entry.team_name.view()))
      return check.fail(at.with("team_name"), Reason::kInvalid);
  }
  return true;
}

bool checkImages(Checker& check, const Model& model) {
  for (unsigned i = 0; i < model.slides.size(); ++i)
    if (model.slides[i] > cfg::kImageLibraryMax)
      return check.fail(FieldPath().with("slides").index(i), Reason::kRange);
  bool enabled = false;
  for (unsigned i = 0; i < model.screens.size(); ++i) {
    const auto& style = model.screens[i];
    FieldPath at;
    at.key("screens").index(i);
    enabled |= style.enabled;
    if (style.colour > 0xffffff)
      return check.fail(at.with("colour"), Reason::kRange);
    if (style.text > 0xffffff)
      return check.fail(at.with("text"), Reason::kRange);
    if (style.accent > 0xffffff)
      return check.fail(at.with("accent"), Reason::kRange);
    if (!cfg::kTextScaleRange.contains(style.text_scale))
      return check.fail(at.with("text_scale"), Reason::kRange);
    if (style.image > cfg::kImageLibraryMax)
      return check.fail(at.with("image"), Reason::kRange);
  }
  // The crest screen shows the crest, never a background image.
  if (model.screens[static_cast<unsigned>(cfg::Screen::kCrest)].image !=
      cfg::kStockImageId)
    return check.fail(FieldPath().key("screens").index(
                          static_cast<unsigned>(cfg::Screen::kCrest))
                          .key("image"),
                      Reason::kInvalid);
  if (!enabled) return check.fail(FieldPath().with("screens"), Reason::kNoneEnabled);
  return true;
}

// Every slot holds a known role; exactly one of the profile's inputs carries
// the mandatory one-key set (OQ-141). A target without inputs has no rule.
bool checkInputs(Checker& check, const Model& model, std::size_t inputs) {
  const auto at = FieldPath().with("input_roles");
  unsigned mandatory = 0;
  for (unsigned i = 0; i < model.input_roles.size(); ++i) {
    if (model.input_roles[i] > cfg::kInputRoleLast)
      return check.fail(at.withIndex(i), Reason::kRange);
    if (i < inputs) mandatory += model.input_roles[i] == cfg::InputRole::kMandatory;
  }
  if (inputs > 0 && mandatory != 1)
    return check.fail(at, mandatory == 0 ? Reason::kRequired : Reason::kConflict);
  return true;
}

// A window limit needs both values or none (OQ-225).
bool checkLimit(Checker& check, const FieldPath& at, const RequestLimit& limit) {
  if (!cfg::kDailyRequestsRange.contains(limit.daily))
    return check.fail(at.with("daily_requests"), Reason::kRange);
  if (!cfg::kWindowRequestsRange.contains(limit.window_requests))
    return check.fail(at.with("window_requests"), Reason::kRange);
  if (!cfg::kWindowRange.contains(limit.window_s))
    return check.fail(at.with("window_s"), Reason::kRange);
  if ((limit.window_requests == 0) != (limit.window_s == 0))
    return check.fail(at.with(limit.window_s == 0 ? "window_s"
                                                  : "window_requests"),
                      Reason::kRequired);
  return true;
}

// Request plan per provider (OQ-216, OQ-225) and the global limit (OQ-226).
bool checkRequestPlans(Checker& check, const Model& model) {
  for (unsigned p = 0; p < model.providers.size(); ++p) {
    const auto at = FieldPath().with("providers").withIndex(p);
    const auto& plan = model.providers[p];
    if (!checkLimit(check, at, plan.limit)) return false;
    for (unsigned t = 0; t < plan.interval_s.size(); ++t)
      if (plan.interval_s[t] < cfg::kIntervalMinS ||
          plan.interval_s[t] > cfg::kIntervalMaxS)
        return check.fail(at.with("intervals").with(dataTypeName(
                              static_cast<cfg::DataType>(t))),
                          Reason::kRange);
  }
  return checkLimit(check, FieldPath().with("global_limit"),
                    model.global_limit);
}

}  // namespace

const char* reasonName(Reason reason) noexcept {
  switch (reason) {
    case Reason::kInvalid: return "invalid";
    case Reason::kRange: return "range";
    case Reason::kType: return "type";
    case Reason::kUnknownKey: return "unknown_key";
    case Reason::kTooLong: return "too_long";
    case Reason::kTooMany: return "too_many";
    case Reason::kRequired: return "required";
    case Reason::kDuplicate: return "duplicate";
    case Reason::kConflict: return "conflict";
    case Reason::kNoneEnabled: return "none_enabled";
  }
  return "invalid";
}

bool validate(const Model& model, Field* invalid, Violation* violation,
              const Limits& limits) noexcept {
  Checker check(violation);
  Field bad = Field::kApplication;
  const auto core = [&](Field field, const char* name, bool valid) {
    if (valid) return true;
    bad = field;
    return check.fail(FieldPath().with(name), Reason::kInvalid);
  };
  // Any SSID byte sequence up to its capacity is valid.
  bool ok =
      core(Field::kWifiPassword, "wifi_password",
           isValidWifiPassword(model.wifi_password.view())) &&
      core(Field::kApPassword, "ap_password",
           isValidApPassword(model.ap_password.view())) &&
      core(Field::kHostname, "hostname", isValidHostname(model.hostname.view())) &&
      core(Field::kDebugStatusIntervalS, "debug_status_interval_s",
           isValidStatusInterval(model.debug_status_interval_s)) &&
      core(Field::kTimeZone, "time_zone", isValidTimeZone(model.time_zone.view())) &&
      core(Field::kNtpServer, "ntp_server",
           isValidNtpServer(model.ntp_server.view()));
  if (ok) {
    const cfg::Range visible{
        cfg::kVisibleMatchesRange.min,
        std::min<uint16_t>(cfg::kVisibleMatchesRange.max,
                           limits.visible_matches_max)};
    const cfg::Range window{
        cfg::kTableWindowRange.min,
        std::min<uint16_t>(cfg::kTableWindowRange.max,
                           limits.table_window_max)};
    const cfg::Range screens{0, static_cast<uint16_t>(cfg::kScreenCount - 1)};
    // While other matches run the own club does not play (OQ-122).
    const cfg::Range matchday{static_cast<uint16_t>(cfg::Screen::kLiveMulti),
                              screens.max};
    ok =
        check.invalid("admin_password", model.admin_password.view().find(
                                            '\0') == std::string_view::npos) &&
        check.invalid("api_football_key",
                      isHeaderValue(model.api_football_key.view())) &&
        check.invalid("football_data_key",
                      isHeaderValue(model.football_data_key.view())) &&
        check.range("language", {0, 1}, model.language) &&
        check.range("rotation", cfg::kRotationRange, model.rotation) &&
        check.range("brightness", cfg::kBrightnessRange, model.brightness) &&
        check.range("night_brightness", cfg::kBrightnessRange,
                    model.night_brightness) &&
        check.range("visible_matches", visible, model.visible_matches) &&
        check.range("table_window", window, model.table_window) &&
        check.range("manual_return_s", cfg::kReturnTimeRange,
                    model.manual_return_s) &&
        check.range("scroll_reset_s", cfg::kReturnTimeRange,
                    model.scroll_reset_s) &&
        check.range("scroll_repeat_ms", cfg::kScrollRepeatRange,
                    model.scroll_repeat_ms) &&
        check.range("slideshow_s", cfg::kSlideshowRange, model.slideshow_s) &&
        check.range("table_page_s", cfg::kTablePageRange, model.table_page_s) &&
        check.range("night_start", cfg::kDayMinutesRange, model.night_start) &&
        check.range("night_end", cfg::kDayMinutesRange, model.night_end) &&
        check.range("ip_badge_s", cfg::kReturnTimeRange, model.ip_badge_s) &&
        check.range("window_before", cfg::kMatchWindowRange,
                    model.window_before) &&
        check.range("window_after", cfg::kMatchWindowRange,
                    model.window_after) &&
        check.range("idle_screen", screens,
                    static_cast<unsigned>(model.idle_screen)) &&
        check.range("matchday_screen", matchday,
                    static_cast<unsigned>(model.matchday_screen)) &&
        check.range("own_match_screen", screens,
                    static_cast<unsigned>(model.own_match_screen)) &&
        check.range("conference_mode",
                    {0, static_cast<uint16_t>(cfg::ConferenceMode::kOnePage)},
                    static_cast<unsigned>(model.conference_mode)) &&
        check.range("matchday_mode",
                    {0, static_cast<uint16_t>(cfg::MatchdayMode::kNext)},
                    static_cast<unsigned>(model.matchday_mode)) &&
        checkInputs(check, model, limits.inputs) &&
        checkRequestPlans(check, model) && checkImages(check, model);
  }
  if (ok) return true;
  if (invalid != nullptr) *invalid = bad;
  return false;
}

bool validate(const Competitions& competitions, Violation* violation) noexcept {
  Checker check(violation);
  return checkSources(check, competitions);
}

ImageMask referencedImages(const Model& model) noexcept {
  ImageMask result = 0;
  const auto include = [&](uint8_t id) {
    if (id >= 1 && id <= cfg::kImageLibraryMax) result |= ImageMask{1} << id;
  };
  for (const auto& style : model.screens) include(style.image);
  for (const uint8_t id : model.slides) include(id);
  return result;
}

bool dropMissingImages(Model& model, ImageMask present) noexcept {
  const auto missing = [present](uint8_t id) {
    return id != cfg::kStockImageId &&
           (id > cfg::kImageLibraryMax || ((present >> id) & 1) == 0);
  };
  bool changed = false;
  const auto replace = [&](uint8_t& id) {
    if (!missing(id)) return;
    id = cfg::kStockImageId;
    changed = true;
  };
  for (auto& style : model.screens) replace(style.image);
  // Slide 0 is the crest, never a missing image.
  decltype(model.slides) kept;
  for (const uint8_t id : model.slides)
    if (!missing(id)) kept.push(id);
  if (kept.size() != model.slides.size()) {
    model.slides = kept;
    changed = true;
  }
  return changed;
}

Model initialValues(const Presets& presets, FieldMask* rejected,
                    const Limits& limits) noexcept {
  Model model;
  // A default above the display's capacity would never validate.
  model.visible_matches =
      std::min<uint8_t>(model.visible_matches, limits.visible_matches_max);
  model.table_window =
      std::min<uint8_t>(model.table_window, limits.table_window_max);
  model.input_roles = inputRolesDefault(limits.inputs);
  model.hostname.assign(cfg::kHostnameDefault);
  model.external_antenna = cfg::kExternalAntennaDefault;
  model.debug_status_log = cfg::kDebugStatusLogDefault;
  model.debug_status_interval_s = cfg::kDebugStatusIntervalDefaultS;
  model.time_zone.assign(cfg::kTimeZoneDefault);
  model.ntp_server.assign(cfg::kNtpServerDefault);

  FieldMask skipped = 0;
  if (!presets.wifi_ssid.empty() && !model.wifi_ssid.assign(presets.wifi_ssid))
    skipped |= fieldBit(Field::kWifiSsid);
  if (!presets.wifi_password.empty()) {
    if (isValidWifiPassword(presets.wifi_password))
      model.wifi_password.assign(presets.wifi_password);
    else
      skipped |= fieldBit(Field::kWifiPassword);
  }
  if (!presets.ap_password.empty()) {
    if (isValidApPassword(presets.ap_password))
      model.ap_password.assign(presets.ap_password);
    else
      skipped |= fieldBit(Field::kApPassword);
  }
  if (!presets.hostname.empty()) {
    if (isValidHostname(presets.hostname))
      model.hostname.assign(presets.hostname);
    else
      skipped |= fieldBit(Field::kHostname);
  }
  if (!presets.time_zone.empty()) {
    if (isValidTimeZone(presets.time_zone))
      model.time_zone.assign(presets.time_zone);
    else
      skipped |= fieldBit(Field::kTimeZone);
  }
  auto preset = [&](auto& field, std::string_view value) {
    if (!field.assign(value)) skipped |= fieldBit(Field::kApplication);
  };
  if (presets.admin_password.find('\0') == std::string_view::npos)
    preset(model.admin_password, presets.admin_password);
  else
    skipped |= fieldBit(Field::kApplication);
  if (isHeaderValue(presets.api_football_key))
    preset(model.api_football_key, presets.api_football_key);
  else
    skipped |= fieldBit(Field::kApplication);
  if (isHeaderValue(presets.football_data_key))
    preset(model.football_data_key, presets.football_data_key);
  else
    skipped |= fieldBit(Field::kApplication);
  if (rejected != nullptr) *rejected = skipped;
  return model;
}

void initialCompetitions(const Presets& presets, Competitions& competitions,
                         FieldMask* rejected) noexcept {
  competitions.clear();
  FieldMask skipped = 0;
  auto preset = [&](auto& field, std::string_view value) {
    if (!field.assign(value)) skipped |= fieldBit(Field::kApplication);
  };
  Competition entry;
  if (presets.provider == "api-football")
    entry.primary.provider = cfg::Provider::kApiFootball;
  else if (presets.provider == "espn")
    entry.primary.provider = cfg::Provider::kEspn;
  else if (presets.provider == "football-data")
    entry.primary.provider = cfg::Provider::kFootballData;
  else if (!presets.provider.empty() && presets.provider != "openligadb")
    skipped |= fieldBit(Field::kApplication);
  preset(entry.primary.competition, presets.competition);
  preset(entry.primary.team, presets.team);
  preset(entry.primary.season, presets.season);
  if (!entry.primary.competition.empty()) {
    competitions.entries.push(entry);
    assignIds(competitions);
    if (!validate(competitions)) {
      competitions.clear();
      skipped |= fieldBit(Field::kApplication);
    }
  }
  if (rejected != nullptr) *rejected = skipped;
}

}  // namespace settings
