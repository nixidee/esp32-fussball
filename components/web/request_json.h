// Small typed command bodies. Shared JSON validates every byte before callers
// perform any action; no DOM, unknown keys or duplicate aliases are accepted.
#pragma once
#include "demo_scenarios.h"
#include "provider_query.h"
#include "settings_json.h"

namespace web {
struct LoginRequest {
  settings::Bytes<cfg::kAdminPasswordMaxBytes> password;
};
struct ResetRequest {
  bool images = false, format_storage = false;
};
struct DemoRequest {
  uint8_t scenario = 0, step = 0;
};
struct ImageRoleRequest {
  uint8_t role = 0, id = 0;
  bool copy = false;
};
SettingsDecode readEmpty(json::Input, settings::Violation&);
SettingsDecode readLogin(json::Input, LoginRequest&, settings::Violation&);
SettingsDecode readReset(json::Input, ResetRequest&, settings::Violation&);
SettingsDecode readDemo(json::Input, DemoRequest&, settings::Violation&);
SettingsDecode readImageId(json::Input, uint8_t&, settings::Violation&);
SettingsDecode readImageRole(json::Input, ImageRoleRequest&,
                             settings::Violation&);
SettingsDecode readSelectionQuery(json::Input, uint64_t&, settings::Violation&);
SettingsDecode readSelection(json::Input, football::SelectionRequest&,
                             settings::Violation&);
}  // namespace web
