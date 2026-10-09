#include "settings_model.h"

#include <algorithm>

namespace settings {

namespace {

bool isPrintableAscii(char c) { return c >= 0x20 && c <= 0x7e; }

bool isHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool isHostnameChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c == '-';
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
  return !value.empty() && value.size() <= cfg::kHostnameMaxChars &&
         value.front() != '-' && value.back() != '-' &&
         std::all_of(value.begin(), value.end(), isHostnameChar);
}

bool isValidStatusInterval(uint16_t seconds) noexcept {
  return seconds >= cfg::kDebugStatusIntervalMinS &&
         seconds <= cfg::kDebugStatusIntervalMaxS;
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
  } else {
    return true;
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
  if (rejected != nullptr) *rejected = skipped;
  return model;
}

}  // namespace settings
