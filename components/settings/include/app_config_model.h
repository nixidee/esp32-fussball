// One coherent application configuration. Provider responses never belong here.
#pragma once

#include <cstdint>

#include "names_ref.h"
#include "screen_policy.h"
#include "settings_model.h"

namespace settings {

struct AppConfig {
  Model model;
  Competitions competitions;
  screen_policy::Policy screens;
  NamesRef names;
  uint32_t revision = 0;
  uint64_t last_apply_id = 0;
};

bool validate(const AppConfig&, Violation* violation = nullptr,
              screen_policy::Error* policy_error = nullptr,
              const Limits& limits = {}) noexcept;

}  // namespace settings
