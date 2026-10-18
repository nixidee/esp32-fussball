// Narrow configuration snapshots for runtime owners; no service dependencies.
#pragma once

#include "settings_model.h"

namespace settings {
struct NetworkSettings {
  decltype(Model::wifi_ssid) wifi_ssid;
  decltype(Model::wifi_password) wifi_password;
  decltype(Model::ap_password) ap_password;
  decltype(Model::hostname) hostname;
  decltype(Model::ntp_server) ntp_server;
  bool external_antenna = cfg::kExternalAntennaDefault;
  bool operator==(const NetworkSettings&) const noexcept = default;
};
struct DiagnosticSettings {
  bool enabled;
  uint16_t interval_s;
};
struct WebSettings {
  decltype(Model::hostname) hostname;
  decltype(Model::admin_password) admin_password;
  bool browser_debug = cfg::kBrowserDebugDefault;
  uint8_t language = cfg::kLanguageDefault;
};
void projectNetworkSettings(const Model& model, NetworkSettings& out) noexcept;
void projectWebSettings(const Model& model, WebSettings& out) noexcept;
}  // namespace settings
