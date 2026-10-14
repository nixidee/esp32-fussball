#include "navigation_policy.h"
namespace ui {
NavigationResult resolve(const settings::Model& model, NavigationInput input) {
  NavigationResult result;
  // In display order (cfg::kScreenOrder).
  for (const auto screen : cfg::kScreenOrder)
    if (model.screens[static_cast<unsigned>(screen)].enabled)
      result.available[result.count++] = screen;
  result.default_screen = !input.configured  ? cfg::Screen::kCrest
                          : input.own_window ? model.own_match_screen
                          : input.matchday   ? model.matchday_screen
                                             : model.idle_screen;
  if (!model.screens[static_cast<unsigned>(result.default_screen)].enabled &&
      result.count)
    result.default_screen = result.available[0];
  result.selected =
      input.inputs && input.manual ? input.selected : result.default_screen;
  if (!model.screens[static_cast<unsigned>(result.selected)].enabled)
    result.selected = result.default_screen;
  return result;
}
}  // namespace ui
