#include "navigation_policy.h"

#include <algorithm>
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
namespace {
bool faulted(const NavigationData& data, uint8_t id) {
  for (std::size_t i = 0; i < data.fault_count; ++i)
    if (data.faults[i] == id) return true;
  return false;
}
int position(Cycle& cycle, const uint8_t* ids, int count, int64_t now) {
  for (int i = 0; i < count; ++i)
    if (cycle.competition == ids[i]) return i;
  cycle = {ids[0], 0, now};
  return 0;
}
void next(Cycle& cycle, const uint8_t* ids, int count, int at, int direction,
          int64_t now) {
  cycle = {ids[(at + direction + count) % count], 0, now};
}
}  // namespace

void automatic(NavigationState& state, int64_t now, AutomaticReason reason) {
  state.manual = false;
  state.paged = false;
  state.table_first = -1;
  state.matchday_first = 0;
  state.conference_page = 0;
  if (reason != AutomaticReason::kTimeout) state.scroll_down = true;
  if (reason == AutomaticReason::kSettings) {
    state.table_cycle = {0, 0, now};
    state.matchday_cycle = {0, 0, now};
    state.input_at = state.scroll_at = now;
  }
  state.selected = state.default_screen;
}
void synchronize(NavigationState& state, const settings::Model& model,
                 const NavigationData& data, std::size_t inputs) {
  const auto shown = data.shown.only(cfg::CompetitionScreen::kConference);
  const auto members = model.demo
                           ? football::view::conferenceAll(data.store, shown)
                           : football::view::conferenceMembers(
                                 data.store, shown, data.utc,
                                 {model.window_before, model.window_after,
                                  model.whole_matchday});
  bool own_window = false;
  football::view::ownForScreen(
      data.store, data.shown.only(cfg::CompetitionScreen::kOwnMatch), data.utc,
      model.window_before, model.window_after, model.demo, own_window);
  const auto result =
      resolve(model, {data.configured,
                      (model.demo || data.utc > 0) && members.count > 0,
                      own_window, state.manual, inputs, state.selected});
  state.default_screen = result.default_screen;
  state.selected = result.selected;
}
TableSelection tableSelection(NavigationState& state,
                              const settings::Model& model,
                              const NavigationData& data, int64_t now,
                              bool advance, int direction) {
  const auto shown = data.shown.only(cfg::CompetitionScreen::kTable);
  uint8_t ids[cfg::kCompetitionMax];
  int count = 0;
  for (std::size_t i = 0; i < shown.count(); ++i) {
    const uint8_t id = shown.at(i).competition;
    if (football::view::table(data.store, id).count || faulted(data, id))
      ids[count++] = id;
  }
  if (!count) return {shown.count() ? shown.at(0).competition : uint8_t{0}};
  auto& cycle = state.table_cycle;
  const auto was = cycle.competition;
  const int at = position(cycle, ids, count, now);
  if (was != cycle.competition) state.table_first = -1;
  auto table = football::view::table(data.store, cycle.competition);
  const int rows = table.count;
  const int visible = std::min<int>(model.table_window, rows);
  const int pages =
      visible && table.own < 0 ? (rows + visible - 1) / visible : 1;
  cycle.page = std::min(cycle.page, pages - 1);
  const int base =
      table.own >= 0 ? table.own - visible / 2 : cycle.page * visible;
  const int last = std::max(0, rows - visible);
  const int first = state.paged && state.table_first >= 0
                        ? std::clamp(state.table_first, 0, last)
                        : std::clamp(base, 0, last);
  if (direction) {
    if (!rows || first + direction < 0 || first + direction > last) {
      next(cycle, ids, count, at, direction, now);
      state.table_first = -1;
    } else
      state.table_first = first + direction;
  } else if (advance && !state.paged &&
             now - cycle.at >= model.table_page_s * 1000) {
    if (cycle.page + 1 < pages) {
      ++cycle.page;
      cycle.at = now;
    } else
      next(cycle, ids, count, at, 1, now);
    state.table_first = -1;
  }
  table = football::view::table(data.store, cycle.competition);
  const int total = table.count;
  const int window = std::min<int>(model.table_window, total);
  const int origin =
      table.own >= 0 ? table.own - window / 2 : cycle.page * window;
  const int effective = state.table_first >= 0 && (state.paged || direction)
                            ? state.table_first
                            : origin;
  const int result = std::clamp(effective, 0, std::max(0, total - window));
  state.table_first = result;
  return {cycle.competition, result, window, total, total == 0};
}
MatchdaySelection matchdaySelection(NavigationState& state,
                                    const settings::Model& model,
                                    const NavigationData& data, int64_t now,
                                    bool advance, int direction) {
  const auto shown = data.shown.only(cfg::CompetitionScreen::kMatchday);
  const bool manual = state.paged || direction != 0;
  const auto reachOf = [&](uint8_t id) {
    return football::view::matchdays(data.store, shown, id, model.matchday_mode,
                                     data.utc, data.local_offset);
  };
  const auto usable = [&](const football::matchday::Reach& r) {
    return manual ? r.count > 0 : r.selected >= 0;
  };
  if (!manual) state.matchday_first = 0;
  uint8_t ids[cfg::kCompetitionMax];
  int count = 0;
  for (std::size_t i = 0; i < shown.count(); ++i) {
    const uint8_t id = shown.at(i).competition;
    if (usable(reachOf(id)) || faulted(data, id)) ids[count++] = id;
  }
  if (!count) return {};
  auto& cycle = state.matchday_cycle;
  const uint8_t was = cycle.competition;
  int at = position(cycle, ids, count, now);
  if (was != cycle.competition) state.matchday_first = 0;
  auto reach = reachOf(cycle.competition);
  bool card = !usable(reach);
  const int per_page = std::max<int>(1, model.visible_matches);
  int day = 0, pages = 1;
  const auto load = [&](int k) {
    day = k;
    const auto list = football::view::matchday(
        data.store, shown, cycle.competition, reach.span[day]);
    pages = std::max(1, (list.count + per_page - 1) / per_page);
  };
  if (!card) {
    int held = reach.selected >= 0 ? reach.selected : reach.count - 1;
    for (int k = 0; k < reach.count; ++k)
      if (reach.span[k].first == state.matchday_first) held = k;
    load(held);
    cycle.page = std::min(cycle.page, pages - 1);
  }
  const auto enter = [&](int k, int sign) {
    load(k);
    cycle.page = sign > 0 ? 0 : pages - 1;
    cycle.at = now;
  };
  const auto enterCompetition = [&](int sign) {
    next(cycle, ids, count, at, sign, now);
    reach = reachOf(cycle.competition);
    card = !usable(reach);
    state.matchday_first = 0;
    if (!card)
      enter(reach.selected >= 0 ? reach.selected : reach.count - 1, sign);
  };
  if (direction > 0) {
    if (!card && cycle.page + 1 < pages)
      cycle = {cycle.competition, cycle.page + 1, now};
    else if (!card && day + 1 < reach.count)
      enter(day + 1, 1);
    else
      enterCompetition(1);
  } else if (direction < 0) {
    if (!card && cycle.page > 0)
      cycle = {cycle.competition, cycle.page - 1, now};
    else if (!card && day > 0)
      enter(day - 1, -1);
    else
      enterCompetition(-1);
  } else if (advance && !state.paged &&
             now - cycle.at >= cfg::kConferencePageMs) {
    if (!card && cycle.page + 1 < pages)
      cycle = {cycle.competition, cycle.page + 1, now};
    else if (count > 1 || (!card && pages > 1))
      enterCompetition(1);
    else
      cycle.at = now;
  }
  if (direction && !card) state.matchday_first = reach.span[day].first;
  return {cycle.competition,
          card ? football::matchday::Span{} : reach.span[day],
          cycle.page,
          pages,
          true,
          card};
}
void applyAction(NavigationState& state, Action action,
                 const settings::Model& model, const NavigationData& data,
                 int64_t now) {
  state.input_at = now;
  if (action == Action::kNext || action == Action::kPrevious) {
    constexpr int count = static_cast<int>(cfg::kScreenCount);
    int at = 0;
    for (int i = 0; i < count; ++i)
      if (cfg::kScreenOrder[i] == state.selected) at = i;
    const int sign = action == Action::kNext ? 1 : -1;
    for (int i = 1; i <= count; ++i) {
      const auto screen = cfg::kScreenOrder[(at + sign * i + count) % count];
      if (model.screens[static_cast<unsigned>(screen)].enabled) {
        state.selected = screen;
        break;
      }
    }
    state.manual = true;
    state.paged = false;
    state.table_first = -1;
    state.matchday_first = 0;
    state.scroll_down = true;
  } else if (action == Action::kDefault)
    automatic(state, now, AutomaticReason::kDefault);
  else if (action == Action::kToggleDirection) {
    state.scroll_down = !state.scroll_down;
    state.scroll_at = now;
  } else if (action == Action::kUp || action == Action::kDown) {
    const int direction = action == Action::kDown ? 1 : -1;
    if (state.selected == cfg::Screen::kLiveMulti) {
      const auto shown = data.shown.only(cfg::CompetitionScreen::kConference);
      const auto members =
          model.demo ? football::view::conferenceAll(data.store, shown)
                     : football::view::conferenceMembers(
                           data.store, shown, data.utc,
                           {model.window_before, model.window_after,
                            model.whole_matchday});
      football::ConferenceRequest request;
      request.mode = model.conference_mode;
      request.pin_own = model.pin_own_match;
      request.manual = true;
      request.per_page = model.visible_matches;
      request.page = 0;
      const auto manual_page =
          football::view::conferencePage(data.store, members, request);
      const int pages = manual_page.pages;
      if (!state.paged &&
          model.conference_mode == cfg::ConferenceMode::kOnePage)
        state.conference_page = direction > 0 ? 0 : std::max(0, pages - 1);
      else {
        request.manual = state.paged;
        request.page =
            state.paged ? state.conference_page : now / cfg::kConferencePageMs;
        const auto current =
            football::view::conferencePage(data.store, members, request);
        state.conference_page =
            pages ? (current.page + direction + pages) % pages : 0;
      }
      state.manual = state.paged = true;
    } else if (state.selected == cfg::Screen::kTable) {
      tableSelection(state, model, data, now, false, direction);
      state.manual = state.paged = true;
      state.scroll_at = now;
    } else if (state.selected == cfg::Screen::kMatchday) {
      matchdaySelection(state, model, data, now, false, direction);
      state.manual = state.paged = true;
      state.scroll_at = now;
    }
  }
}
}  // namespace ui
