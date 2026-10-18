// Only the display consumer's values: no network secrets/provider plans or
// retained competition preference matrix. The programme is derived once.
#pragma once
#include "settings_model.h"
namespace settings {
struct DisplaySettings {
  InputRoles input_roles{};
  uint16_t scroll_repeat_ms = 0, scroll_reset_s = 0;
  uint16_t night_start = 0, night_end = 0, ip_badge_s = 0;
  uint8_t language = 0, brightness = 0, night_brightness = 0, rotation = 0;
  bool demo = false, night_enabled = false, ip_badge_ap_permanent = false,
       ip_badge_bottom = false;
  bool operator==(const DisplaySettings&) const noexcept = default;
};
struct StartupSettings {
  Bytes<cfg::kDisplayNameMaxBytes> club;
  cfg::Provider provider = cfg::kProviderDefault;
  bool active = false;
};
struct UiConfig {
  DisplaySettings device;
  StartupSettings startup;
  screen_policy::Options screens;
  screen_policy::Program program;
  screen_policy::PhaseRules shared;
  std::array<screen::Type, screen::kTypeCount> order{};
  std::array<uint8_t, cfg::kCompetitionMax> configured{};
  uint32_t revision = 0, data_generation = 0;
  screen_policy::Phase phase = screen_policy::Phase::kIdle;
};
void projectUiConfig(const Model&, const screen_policy::Policy&,
                     const Competitions&, screen_policy::Phase,
                     UiConfig&) noexcept;
// Explicit navigation effects. Appearance, labels, hardware brightness and
// network changes update their own consumers without ending a manual visit.
bool sameNavigation(const UiConfig&, const UiConfig&) noexcept;
// Demo's synthetic routes use the shared current-phase rules when no saved
// competition exists. Saved identities keep their compiled effective rules.
void demoProgram(const UiConfig&, const CompetitionRoutes&,
                 screen_policy::Program&) noexcept;
}  // namespace settings
