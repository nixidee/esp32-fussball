// Actual public Presenter::bind and canonical navigation; only external
// time/status presentation edges are stubbed. No renderer or device proof.
#include <cassert>
#include <cstdio>
#include <ctime>

#include "presenter.h"
#include "time_service.h"

namespace timekeeping {
void toLocal(time_t utc, std::tm& out) { gmtime_r(&utc, &out); }
}  // namespace timekeeping
namespace ui {
bool Presenter::localCivil(int64_t raw, std::tm& out) const {
  const time_t at = raw;
  return gmtime_r(&at, &out) != nullptr;
}
void Presenter::localDate(char* out, std::size_t size, int64_t) {
  snprintf(out, size, "Sat 17");
}
void Presenter::localTime(char* out, std::size_t size, int64_t) {
  snprintf(out, size, "12:00");
}
void Presenter::roundCaption(const football::MatchStore&,
                             const football::StoredMatch&, bool) {}
void Presenter::competitionName(char (&)[48], uint8_t) {}
const settings::CompetitionRoute* Presenter::entryOf(uint8_t) const {
  return nullptr;
}
void Presenter::cardText(const char*, const char*, const char*, Tone) {
  assert(false);
}
uint8_t Presenter::firstFault(int& count) {
  count = 0;
  return 0;
}
void Presenter::faultCard(uint8_t, const football::DatasetStatus&,
                          const char*) {
  assert(false);
}
void Presenter::bindStart(uint8_t) { assert(false); }
bool Presenter::networkFault(const BindContext&) const { return false; }
const char* Presenter::networkPhase(network::InitStep) const { return ""; }
void Presenter::networkFaultCard(const BindContext&) { assert(false); }
uint32_t Presenter::bindStatus(const football::Published&, const BindContext&) {
  return 0;
}
}  // namespace ui
int main() {
  using namespace football;
  constexpr int64_t now = 1792245600;
  static MatchStore store;
  settings::CompetitionRoutes routes;
  settings::CompetitionRoute competition;
  competition.id = 1;
  competition.incarnation = 1;
  competition.primary.competition.assign("league");
  assert(routes.entries.push(competition));
  view::Shown shown;
  shown.add(competition);
  for (unsigned i = 0; i < cfg::kStoreMatches; ++i) {
    StoredMatch match;
    match.id = i + 1;
    match.competition = 1;
    match.round = 1;
    match.phase = Phase::kLive;
    match.kickoff = now - 3600;
    match.principal = {
        static_cast<uint8_t>(i == cfg::kStoreMatches - 2 ? 7 : 1), 0};
    match.home = store.team(match.competition, match.provider,
                            i + 1 == cfg::kStoreMatches ? 40 : 41, "Home");
    store.selectClub(match.competition, match.provider, 40, {});
    assert(store.upsert(match, {now, 1}));
  }
  settings::screen_policy::Policy policy;
  policy.visible_matches = 2;
  policy.pin_own_match = true;
  policy.conference_mode = cfg::ConferenceMode::kAllPages;
  routes.entries[0].club.own_special = false;
  ui::NavigationData facts{store, shown, routes, {now, 1}, {}, true, false};
  settings::UiConfig configuration;
  configuration.screens = policy;
  configuration.program.entries[0] = {
      {10, true, true}, screen::Type::kLiveMulti, 1};
  configuration.program.entries[1] = {
      {10, true, true}, screen::Type::kMatchday, 1};
  configuration.program.count = 2;
  ui::NavigationContext navigation(configuration, facts);
  ui::sequence::State state;
  state.initialized = true;
  state.manual = true;
  state.paged = true;
  state.entry = {screen::Type::kLiveMulti, 1, 1};
  state.page = 510;
  state.pages = 511;
  ui::ViewModel output;
  ui::PresentationState presentation;
  presentation.starting = false;
  settings::DisplaySettings model;
  CompetitionStates states{};
  Published published{store, shown,  1,      1,     true, false, 100,
                      0,     states, routes, false, 255,  255,   {now, 1}};
  network::Status network{};
  ui::BindContext context{now, 1000, true, true, false, &network};
  auto scene = ui::presentationFacts(state, navigation, context.now,
                                     published.fetched_ms, published.retry_ms);
  ui::Presenter presenter(model, output, policy, presentation, 0, &published,
                          &scene, nullptr, &configuration);
  presenter.bind(context);
  assert(output.layout == ui::Layout::kConference);
  assert(output.page == 510 && output.pages == 511 && output.row_count == 2);
  assert(std::string_view(output.matches[1].centre) == "7:0");
  state.entry = {screen::Type::kMatchday, 1, 1};
  state.page = 255;
  state.pages = 256;
  scene = ui::presentationFacts(state, navigation, context.now,
                                published.fetched_ms, published.retry_ms);
  presenter.bind(context);
  assert(output.page == 255 && output.pages == 256 && output.row_count == 2);
  assert(output.rim.first == 510 && output.rim.count == 2 &&
         output.rim.total == 512);
  assert(std::string_view(output.matches[0].centre) == "7:0");
  std::puts(
      "PASS real Presenter::bind: final Conference 511/511 and Matchday "
      "256/256, correct match511/512 and scroll510/512");
}
