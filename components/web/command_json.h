#pragma once
#include "input_policy.h"
#include "settings_json.h"

namespace web {
struct DisplayCommand {
  ui::Action action = ui::Action::kNone;
  uint32_t config_revision = 0;
};
// Complete, strict command body. No side effect or output mutation on failure.
SettingsDecode readDisplayCommand(json::Input input, DisplayCommand& command,
                                  settings::Violation& violation);
}  // namespace web
