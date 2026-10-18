#include "runtime_config.h"

namespace settings {
void projectNetworkSettings(const Model& model, NetworkSettings& out) noexcept {
  out.wifi_ssid = model.wifi_ssid;
  out.wifi_password = model.wifi_password;
  out.ap_password = model.ap_password;
  out.hostname = model.hostname;
  out.ntp_server = model.ntp_server;
  out.external_antenna = model.external_antenna;
}
void projectWebSettings(const Model& model, WebSettings& out) noexcept {
  out.hostname = model.hostname;
  out.admin_password = model.admin_password;
  out.browser_debug = model.browser_debug;
  out.language = model.language;
}
}  // namespace settings
