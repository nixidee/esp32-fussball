#include "app_config_model.h"

namespace settings {
bool validate(const AppConfig& config, Violation* violation,
              screen_policy::Error* policy_error,
              const Limits& limits) noexcept {
  if (!validate(config.model, nullptr, violation, limits) ||
      !validate(config.competitions, violation) ||
      !checkProviderAccess(config.model, config.competitions, violation) ||
      !screen_policy::validate(
          config.screens, policy_error,
          {limits.visible_matches_max, limits.table_window_max}))
    return false;
  const auto& names = config.names;
  const bool valid_names =
      names.slot == 0
          ? names.generation == 0 && names.length == 0 && names.crc == 0
          : names.slot <= 2 && names.generation != 0 && names.length != 0 &&
                names.length <= cfg::kTeamNamesFileBytes;
  if (!valid_names && violation)
    violation->set(FieldPath().key("names"), Reason::kRange);
  return valid_names;
}

}  // namespace settings
