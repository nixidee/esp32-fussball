#include "settings_model.h"

#include <algorithm>

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

bool validate(const Model& model, Field* invalid) noexcept {
  Field bad;
  // Any SSID byte sequence up to its capacity is valid.
  if (!isValidWifiPassword(model.wifi_password.view())) {
    bad = Field::kWifiPassword;
  } else if (!isValidApPassword(model.ap_password.view())) {
    bad = Field::kApPassword;
  } else if (!isValidHostname(model.hostname.view())) {
    bad = Field::kHostname;
  } else if (!isValidStatusInterval(model.debug_status_interval_s)) {
    bad = Field::kDebugStatusIntervalS;
  } else if (!isValidTimeZone(model.time_zone.view())) {
    bad = Field::kTimeZone;
  } else if (!isValidNtpServer(model.ntp_server.view())) {
    bad = Field::kNtpServer;
  } else {
    bool ok =
        model.admin_password.view().find('\0') == std::string_view::npos &&
        isHeaderValue(model.api_football_key.view()) &&
        isHeaderValue(model.football_data_key.view()) && model.language <= 1 &&
        cfg::kRotationRange.contains(model.rotation) &&
        cfg::kBrightnessRange.contains(model.brightness) &&
        cfg::kBrightnessRange.contains(model.night_brightness) &&
        cfg::kVisibleMatchesRange.contains(model.visible_matches) &&
        cfg::kTableWindowRange.contains(model.table_window) &&
        cfg::kReturnTimeRange.contains(model.manual_return_s) &&
        cfg::kReturnTimeRange.contains(model.scroll_reset_s) &&
        cfg::kScrollRepeatRange.contains(model.scroll_repeat_ms) &&
        cfg::kSlideshowRange.contains(model.slideshow_s) &&
        cfg::kDayMinutesRange.contains(model.night_start) &&
        cfg::kDayMinutesRange.contains(model.night_end) &&
        cfg::kReturnTimeRange.contains(model.ip_badge_s) &&
        cfg::kMatchWindowRange.contains(model.window_before) &&
        cfg::kMatchWindowRange.contains(model.window_after) &&
        cfg::kProviderBudgetRange.contains(model.api_daily_budget) &&
        static_cast<unsigned>(model.idle_screen) < cfg::kScreenCount &&
        static_cast<unsigned>(model.matchday_screen) < cfg::kScreenCount &&
        static_cast<unsigned>(model.own_match_screen) < cfg::kScreenCount;
    auto identifier = [](std::string_view s) {
      return std::all_of(s.begin(), s.end(), [](char c) {
        return isHostnameChar(c) || c == '.' || c == '_';
      });
    };
    auto validateRoute = [&](const Route& route) {
      const auto season = route.season.view();
      const bool valid_season =
          season.empty()
              ? route.provider != cfg::Provider::kApiFootball || !route.enabled
              : (route.provider == cfg::Provider::kOpenLigaDb ||
                 season.size() == 4) &&
                    std::all_of(season.begin(), season.end(),
                                [](char c) { return c >= '0' && c <= '9'; });
      return static_cast<unsigned>(route.provider) < cfg::kProviderCount &&
             identifier(route.competition.view()) &&
             identifier(route.team.view()) && valid_season &&
             (!route.enabled ||
              (!route.competition.empty() && !route.team.empty()));
    };
    for (unsigned i = 0; i < cfg::kRouteCount; ++i) {
      ok &= validateRoute(model.routes[i]) &&
            validateRoute(model.fallback_routes[i]);
      if (model.fallback_routes[i].enabled)
        ok &= model.routes[i].enabled &&
              model.routes[i].provider != model.fallback_routes[i].provider;
    }
    for (unsigned i = 0; i < model.fixture_mappings.size(); ++i) {
      const auto& mapping = model.fixture_mappings[i];
      ok &= mapping.route < cfg::kRouteCount &&
            identifier(mapping.primary.view()) &&
            identifier(mapping.secondary.view());
      if (!mapping.enabled || mapping.route >= cfg::kRouteCount) continue;
      ok &= model.fallback_routes[mapping.route].enabled &&
            !mapping.primary.empty() && !mapping.secondary.empty();
      for (unsigned j = 0; j < i; ++j) {
        const auto& other = model.fixture_mappings[j];
        if (other.enabled && other.route == mapping.route)
          ok &= other.primary != mapping.primary &&
                other.secondary != mapping.secondary;
      }
    }
    bool enabled = false;
    for (const auto& style : model.screens) {
      enabled |= style.enabled;
      ok &= style.colour <= 0xffffff && style.text <= 0xffffff &&
            style.accent <= 0xffffff &&
            cfg::kTextScaleRange.contains(style.text_scale);
    }
    if (ok && enabled) return true;
    bad = Field::kApplication;
  }
  if (invalid != nullptr) *invalid = bad;
  return false;
}

Model initialValues(const Presets& presets, FieldMask* rejected) noexcept {
  Model model;
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
  auto& route = model.routes[0];
  if (presets.provider == "api-football")
    route.provider = cfg::Provider::kApiFootball;
  else if (presets.provider == "espn")
    route.provider = cfg::Provider::kEspn;
  else if (presets.provider == "football-data")
    route.provider = cfg::Provider::kFootballData;
  else if (!presets.provider.empty() && presets.provider != "openligadb")
    skipped |= fieldBit(Field::kApplication);
  preset(route.competition, presets.competition);
  preset(route.team, presets.team);
  preset(route.season, presets.season);
  route.enabled = !route.competition.empty() && !route.team.empty();
  if (!validate(model)) {
    route = {};
    skipped |= fieldBit(Field::kApplication);
  }
  if (rejected != nullptr) *rejected = skipped;
  return model;
}

}  // namespace settings
