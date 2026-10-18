#pragma once
#include "input_policy.h"
#include "json_stream.h"
#include "settings_model.h"

namespace web {
enum class CommandDecode : uint8_t { kOk, kSyntax, kViolation };
struct DisplayCommand {
  ui::Action action = ui::Action::kNone;
  uint32_t config_revision = 0;
};
// Complete, strict command body. No side effect or output mutation on failure.
CommandDecode readDisplayCommand(json::Input input, DisplayCommand& command,
                                 settings::Violation& violation);
}  // namespace web
