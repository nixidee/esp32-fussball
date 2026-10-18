#include "ui_config.h"
namespace settings {
void projectUiConfig(const Model& model, const screen_policy::Policy& policy,
                     const Competitions& competitions,
                     screen_policy::Phase phase, UiConfig& out) noexcept {
  out.device = {model.input_roles,
                model.scroll_repeat_ms,
                model.scroll_reset_s,
                model.night_start,
                model.night_end,
                model.ip_badge_s,
                model.language,
                model.brightness,
                model.night_brightness,
                model.rotation,
                model.demo,
                model.night_enabled,
                model.ip_badge_ap_permanent,
                model.ip_badge_bottom};
  out.startup = {};
  for (const auto& entry : competitions.entries) {
    if (entry.active && !out.startup.active) {
      out.startup.active = true;
      out.startup.provider = entry.primary.provider;
    }
    if (out.startup.club.empty() && !entry.club.name.empty())
      out.startup.club = entry.club.name;
  }
  out.screens = policy;
  out.phase = phase;
  out.shared = policy.shared[screen_policy::index(phase)];
  out.order = policy.order;
  out.configured = {};
  for (std::size_t i = 0; i < competitions.entries.size(); ++i)
    out.configured[i] = competitions.entries[i].id;
  screen_policy::program(policy, competitions, phase, out.program);
}
bool sameNavigation(const UiConfig& a, const UiConfig& b) noexcept {
  const auto& x = a.screens;
  const auto& y = b.screens;
  return a.data_generation == b.data_generation && a.phase == b.phase &&
         a.device.demo == b.device.demo && a.program == b.program &&
         a.shared == b.shared && a.order == b.order &&
         a.configured == b.configured && x.frame == y.frame &&
         x.frame_crest == y.frame_crest && x.slides == y.slides &&
         x.slide_count == y.slide_count && x.per_image_s == y.per_image_s &&
         x.manual_return_s == y.manual_return_s &&
         x.visible_matches == y.visible_matches &&
         x.table_window == y.table_window &&
         x.conference_mode == y.conference_mode &&
         x.matchday_mode == y.matchday_mode &&
         x.pin_own_match == y.pin_own_match &&
         x.own_before_min == y.own_before_min &&
         x.own_after_min == y.own_after_min &&
         x.other_before_min == y.other_before_min &&
         x.other_after_min == y.other_after_min &&
         x.automatic_break == y.automatic_break &&
         x.break_days == y.break_days &&
         x.only_one_own_single == y.only_one_own_single;
}
void demoProgram(const UiConfig& config, const CompetitionRoutes& routes,
                 screen_policy::Program& out) noexcept {
  out = {};
  const auto priority = screen_policy::competitionOrder(routes);
  for (const auto type : config.order) {
    if (!screen::sporting(type)) {
      const auto rule = config.shared[screen::index(type)];
      if (rule.enabled) out.entries[out.count++] = {rule, type, 0};
      continue;
    }
    for (unsigned position = 0; position < priority.count; ++position) {
      const auto& route = routes.entries[priority.slots[position]];
      if (!route.active || !route.id) continue;
      bool configured = false;
      for (const auto id : config.configured)
        configured |= route.incarnation != 0 && id == route.id;
      auto rule = config.shared[screen::index(type)];
      if (configured) {
        rule.enabled = false;
        for (unsigned i = 0; i < config.program.count; ++i) {
          const auto& entry = config.program.entries[i];
          if (entry.type == type && entry.competition == route.id) {
            rule = entry.rule;
            break;
          }
        }
      }
      if (rule.enabled) out.entries[out.count++] = {rule, type, route.id};
    }
  }
}
}  // namespace settings
