// Screen and row/page navigation over borrowed canonical data; no service,
// hardware driver or LVGL calls.
#pragma once
#include <array>

#include "input_policy.h"
#include "settings_model.h"
#include "store_views.h"
namespace ui {
struct NavigationInput {
  bool configured, matchday, own_window, manual;
  std::size_t inputs;
  cfg::Screen selected;
};
struct NavigationResult {
  std::array<cfg::Screen, cfg::kScreenCount> available{};
  uint8_t count = 0;
  cfg::Screen default_screen = cfg::Screen::kCrest,
              selected = cfg::Screen::kCrest;
};
NavigationResult resolve(const settings::Model& model, NavigationInput input);

struct Cycle {
  uint8_t competition = 0;
  int page = 0;
  int64_t at = 0;
};
// One owner of manual selection and paging; no retained repository pointers.
struct NavigationState {
  cfg::Screen selected = cfg::Screen::kCrest,
              default_screen = cfg::Screen::kCrest;
  int table_first = -1;
  int conference_page = 0;
  Cycle table_cycle, matchday_cycle;
  uint32_t matchday_first = 0;
  int64_t input_at = 0, scroll_at = 0;
  bool manual = false, scroll_down = true, paged = false;
};
// The caller owns the short-lived fault-id projection; all football data stay
// in their canonical store. Local offset is supplied by the time service.
struct NavigationData {
  const football::MatchStore& store;
  const football::view::Shown& shown;
  const uint8_t* faults = nullptr;
  std::size_t fault_count = 0;
  int64_t utc = 0;
  int32_t local_offset = 0;
  bool configured = false;
};
enum class AutomaticReason : uint8_t { kSettings, kDefault, kTimeout };
void automatic(NavigationState& state, int64_t now, AutomaticReason reason);
void synchronize(NavigationState& state, const settings::Model& model,
                 const NavigationData& data, std::size_t inputs);
void applyAction(NavigationState& state, Action action,
                 const settings::Model& model, const NavigationData& data,
                 int64_t now);
struct TableSelection {
  uint8_t competition = 0;
  int first = 0, count = 0, total = 0;
  bool fault = false;
};
TableSelection tableSelection(NavigationState& state,
                              const settings::Model& model,
                              const NavigationData& data, int64_t now,
                              bool advance = true, int direction = 0);
struct MatchdaySelection {
  uint8_t competition = 0;
  football::matchday::Span span;
  int page = 0, pages = 1;
  bool available = false, fault = false;
};
MatchdaySelection matchdaySelection(NavigationState& state,
                                    const settings::Model& model,
                                    const NavigationData& data, int64_t now,
                                    bool advance = true, int direction = 0);
}  // namespace ui
