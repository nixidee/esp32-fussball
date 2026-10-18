// Real Presenter + canonical store. No LVGL, renderer or device substitute.
#include <cassert>
#include <cstdio>
#include <cstring>

#include "presenter.h"

using namespace football;
constexpr int64_t kNow = 1792245600;
static MatchStore store;
settings::Competitions configured;
settings::CompetitionRoutes routes;
view::Shown shown;
settings::screen_policy::Policy policy;
settings::UiConfig config;
CompetitionStates states;
ui::ViewModel output;
ui::PresentationState presentation;
ui::NavigationState navigation;
network::Status network_status{};
names::Targets labels;
uint32_t last_status = 0;
bool calendar_frozen = false;
int calendar_calls = 0;
uint8_t last_calendar_count = 0;
int64_t offset_transition = 0;
int32_t offset_before = 0, offset_after = 0;
int32_t calendarOffset(int64_t utc, void*) {
  assert(
      !calendar_frozen);  // No service access after facts have been prepared.
  ++calendar_calls;
  return utc < offset_transition ? offset_before : offset_after;
}

void setup(Phase phase = Phase::kLive) {
  store.clear();
  configured = {};
  routes = {};
  shown = {};
  policy = {};
  states = {};
  output = {};
  presentation = {};
  navigation = {};
  labels = {};
  presentation.starting = false;
  settings::Competition entry;
  entry.id = 1;
  entry.incarnation = 1;
  entry.club.team = 40;
  entry.primary.competition.assign("league");
  entry.competition_name.assign("League");
  configured.entries.push(entry);
  routes.entries.push(static_cast<const settings::CompetitionRoute&>(entry));
  shown.add(entry);
  StoredMatch match;
  match.id = 1;
  match.competition = 1;
  store.selectClub(1, match.provider, 40, {});
  match.phase = phase;
  match.kickoff = kNow - 5400;
  match.principal = {1, 1};
  match.minute = 90;
  match.period = Period::kSecondHalf;
  match.home =
      store.team(1, cfg::kProviderDefault, 40, "Home full name", "HOM");
  match.away =
      store.team(1, cfg::kProviderDefault, 41, "Away full name", "AWY");
  assert(store.upsert(match, {kNow, 100}));
  states[0].id = 1;
  states[0].active = true;
  states[0].fetched_ms = 1;
  store.recordDataset(1, MatchStore::Dataset::kFixtures, 1, 1, true,
                      Cause::kNone);
}
StoredMatch& match() {
  return *const_cast<StoredMatch*>(store.find(1, cfg::kProviderDefault, 1));
}
ui::sequence::ContentKind renderModel(int64_t now_ms = 0, int64_t utc = kNow,
                                      int64_t retry_ms = 0) {
  const DateContext dates{calendarOffset, nullptr, 0};
  ui::NavigationData facts{store, shown, routes, {utc, 100},
                           dates, true,  false};
  selection::Input input(policy, routes, store, {utc, 100}, dates);
  settings::projectUiConfig({}, policy, configured,
                            selection::select(input).phase, config);
  ui::NavigationContext context(config, facts);
  ui::sequence::synchronize(navigation, context.input(), now_ms);
  Published published{store,    shown,  1,      0,     true, false, 1,
                      retry_ms, states, routes, false, 1,    0,     {utc, 100}};
  ui::BindContext bind{utc, now_ms, true, true, false, &network_status};
  const auto scene = ui::presentationFacts(
      navigation, context, now_ms, published.fetched_ms, published.retry_ms);
  last_calendar_count = scene.calendar.count;
  if (retry_ms > now_ms)
    assert(scene.calendar.retry != ui::CalendarFacts::kMissing);
  const auto prepared_calls = calendar_calls;
  calendar_frozen = true;
  labels = {};
  last_status =
      ui::Presenter(config.device, output, config.screens, presentation, 0,
                    &published, &scene, &labels, &config, "test")
          .bind(bind);
  assert(calendar_calls == prepared_calls);
  calendar_frozen = false;
  return context
      .prepare({screen::Type::kLiveMulti, 1, 1},
               ui::sequence::PageMode::kAutomatic, 0)
      .content.kind;
}
int main() {
  // Europe/Berlin's 2026 autumn transition: the future kickoff uses its own
  // offset, not the current offset. Both UTC instants display 02:30 locally.
  offset_transition = 1792890000;
  offset_before = 7200;
  offset_after = 3600;
  setup(Phase::kScheduled);
  match().kickoff = 1792891800;
  for (auto& phase : policy.shared) {
    for (auto& rule : phase) rule.automatic = false;
    phase[screen::index(screen::Type::kLiveSingle)].automatic = true;
  }
  renderModel(1000, 1792888200);
  assert(output.layout == ui::Layout::kOwnMatch);
  assert(std::strcmp(output.own.centre, "02:30") == 0);
  assert(std::strcmp(output.own.pill, "So 25.10.") == 0);
  offset_before = offset_after = 0;
  // The largest supported page still has room for independent current,
  // global-as-of, dataset-as-of and retry instants. All five rows show times.
  setup(Phase::kScheduled);
  configured.entries[0].club.own_special = false;
  routes.entries[0].club.own_special = false;
  match().kickoff = kNow + 60;
  policy.visible_matches = cfg::kVisibleMatchesRange.max;
  for (auto& phase : policy.shared) {
    for (auto& rule : phase) rule.automatic = false;
    phase[screen::index(screen::Type::kLiveMulti)].automatic = true;
  }
  for (unsigned i = 1; i < cfg::kVisibleMatchesRange.max; ++i) {
    auto other = match();
    other.id = i + 1;
    other.kickoff += 60 * i;
    assert(store.upsert(other, {kNow, 100}));
  }
  store.recordDataset(1, MatchStore::Dataset::kFixtures, 60000, 60000, true,
                      Cause::kNone);
  store.recordDataset(1, MatchStore::Dataset::kFixtures, 95000, 95000, false,
                      Cause::kNetwork);
  renderModel(120000, kNow, 241000);
  assert(output.layout == ui::Layout::kConference && output.row_count == 5);
  assert(last_calendar_count == cfg::kVisibleMatchesRange.max + 4);
  for (unsigned i = 0; i < output.row_count; ++i)
    assert(std::strcmp(output.matches[i].centre, "--:--") != 0);
  setup();
  match().minute = 93;
  match().added = 3;
  renderModel();
  assert(output.layout == ui::Layout::kOwnMatch);
  assert(!output.rim.extra_known && output.rim.progress_degrees == 360);
  assert(output.rim.top == ui::Tone::kWarn);
  assert(std::strcmp(output.own.pill, "93+3'") == 0);
  assert(labels.count == 2 &&
         labels.entries[0].short_output == output.own.home_short);
  assert(std::strcmp(output.own.home_short, "HOM") == 0);
  std::snprintf(labels.entries[0].short_output,
                labels.entries[0].short_capacity, "XYZ");
  assert(std::strcmp(output.own.home_short, "XYZ") == 0);
  setup();
  match().minute = 60;
  renderModel();
  assert(output.rim.top == ui::Tone::kDim &&
         output.rim.bottom == ui::Tone::kDim);
  setup(Phase::kExtraTime);
  match().period = Period::kExtraFirstHalf;
  match().minute = 105;
  match().added = 2;
  renderModel();
  assert(output.rim.extra_known && output.rim.extra_degrees == 180);
  assert(output.rim.bottom == ui::Tone::kWarn);
  setup(Phase::kPenalties);
  match().shootout = {9, 9};
  renderModel();
  assert(output.own.shootout.active && !output.own.shootout.count);
  assert(std::strstr(output.own.shootout.caption, "nicht geliefert"));
  assert(store.beginEvents(match()));
  for (unsigned i = 0; i < 18; ++i) {
    StoredEvent event;
    event.identity = i + 1;
    event.minute = 120;
    event.flags = kEventShootout;
    event.type = EventType::kPenaltyGoal;
    event.actor = i % 2 ? Side::kAway : Side::kHome;
    store.addEvent(event);
  }
  store.endEvents(true);
  renderModel();
  assert(output.own.shootout.count == 10);
  assert(output.own.shootout.attempts[0] == ui::Attempt::kHomeScored);
  assert(output.own.shootout.attempts[1] == ui::Attempt::kAwayScored);
  assert(std::strstr(output.own.shootout.caption, "1–10/18"));
  renderModel(cfg::kHighlightRotateMs);
  assert(output.own.shootout.count == 8 &&
         std::strstr(output.own.shootout.caption, "11–18/18"));
  assert(store.beginEvents(match()));
  StoredEvent correction;
  correction.identity = 1;
  correction.minute = 120;
  correction.flags = kEventShootout;
  correction.type = EventType::kPenaltyMissed;
  correction.actor = Side::kHome;
  store.addEvent(correction);
  correction.identity = 2;
  correction.actor = Side::kUnknown;
  store.addEvent(correction);
  store.endEvents(true);
  renderModel();
  assert(output.own.shootout.count == 2);
  assert(output.own.shootout.attempts[0] == ui::Attempt::kHomeMissed);
  assert(output.own.shootout.attempts[1] == ui::Attempt::kUnknownMissed);
  assert(std::strstr(output.own.shootout.caption, "unvollständig"));
  // Overview has one member and uses the same Single presentation; a second
  // member switches to Conference without changing the selected type.
  setup();
  configured.entries[0].club.own_special = false;
  routes.entries[0].club.own_special = false;
  renderModel();
  assert(navigation.entry.type == screen::Type::kLiveMulti);
  assert(output.layout == ui::Layout::kOwnMatch);
  auto second = match();
  second.id = 2;
  second.flags = 0;
  second.home = store.team(1, cfg::kProviderDefault, 42, "Third");
  assert(store.upsert(second, {kNow, 100}));
  renderModel();
  assert(output.layout == ui::Layout::kConference && output.row_count == 2);
  // Follow actual prepare -> eligibility -> synchronize -> bind. Confirmed
  // empty Conference is skipped; unknown/failed content reaches common cards.
  for (unsigned state = 0; state < 3; ++state) {
    setup(Phase::kScheduled);
    match().kickoff = kNow + 86400;
    for (auto& phase : policy.shared) {
      for (auto& rule : phase) rule.enabled = false;
      auto& conference = phase[screen::index(screen::Type::kLiveMulti)];
      conference.enabled = conference.automatic = true;
    }
    if (state == 1) {
      store.clear();  // no dataset proof yet, distinct from confirmed empty
    } else {
      store.recordDataset(1,
                          state == 0 ? MatchStore::Dataset::kTable
                                     : MatchStore::Dataset::kFixtures,
                          200, 200, false, Cause::kNetwork, 503);
      states[0].cause = Cause::kNetwork;
      states[0].http = 503;
      states[0].stale = true;
    }
    const auto content = renderModel();
    assert(output.layout == ui::Layout::kCard);
    if (state == 0) {
      assert(content == ui::sequence::ContentKind::kEmpty);
      assert(!navigation.entry.valid());
      assert(std::strstr(output.card.title, "Keine automatischen Inhalte"));
    } else {
      assert(content == ui::sequence::ContentKind::kError);
      assert(navigation.entry.type == screen::Type::kLiveMulti);
      if (state == 1)
        assert(std::strcmp(output.card.title, "Keine Daten") == 0);
      else {
        assert(presentation.card_fault == 1);
        assert(std::strcmp(output.card.title, "League") == 0);
      }
    }
  }
  setup();
  for (auto& phase : policy.shared)
    for (auto& rule : phase) rule.automatic = false;
  for (auto& phase : policy.shared)
    phase[screen::index(screen::Type::kTable)].automatic = true;
  policy.table_window = 2;
  store.beginTable(1);
  assert(store.appendGroup("a", "Group A", 0));
  assert(store.appendGroup("b", "Group B", 1));
  for (unsigned group = 0; group < 2; ++group)
    for (unsigned i = 0; i < 3 + group; ++i) {
      StoredRow row;
      row.group = group;
      row.position = i + 1;
      row.team = store.team(1, cfg::kProviderDefault,
                            group == 1 && i == 3 ? 40 : 100 + 10 * group + i,
                            group ? "B Club" : "A Club");

      assert(store.appendRow(row));
    }
  assert(store.commitTable(true));
  renderModel();
  assert(output.layout == ui::Layout::kTable && output.row_count == 2);
  assert(std::strstr(output.caption, "Group B"));
  assert(navigation.pages == 1);
  for (unsigned step = 0; step < 3; ++step) {
    ui::NavigationData facts{store, shown, routes, {kNow, 100},
                             {},    true,  false};
    ui::NavigationContext context(config, facts);
    ui::sequence::applyAction(navigation, context.input(), ui::Action::kUp,
                              100 + step);
    renderModel(100 + step);
  }
  assert(navigation.paged && navigation.pages == 5);
  assert(std::strstr(output.caption, "Group A"));
  assert(std::strcmp(output.table[0].name, "A Club") == 0);
  offset_before = 7200;
  offset_after = 3600;
  store.recordDataset(1, MatchStore::Dataset::kTable, 60000, 60000, true,
                      Cause::kNone);
  store.recordDataset(1, MatchStore::Dataset::kTable, 200000, 200000, false,
                      Cause::kNetwork, 503);
  renderModel(3660000, 1792891800);
  assert(std::strstr(output.status, "Stand 02:30"));
  offset_before = offset_after = 0;
  // Usable table rows survive table failure. Fixtures success cannot replace
  // the table's own last-success time or clear its warning.
  navigation = {};
  store.recordDataset(1, MatchStore::Dataset::kTable, 60000, 60000, true,
                      Cause::kNone);
  store.recordDataset(1, MatchStore::Dataset::kTable, 200000, 200000, false,
                      Cause::kNetwork, 503);
  store.recordDataset(1, MatchStore::Dataset::kFixtures, 500000, 500000, true,
                      Cause::kNone);
  states[0].cause = Cause::kNetwork;
  states[0].stale = true;
  states[0].fetched_ms = 500000;
  renderModel(560000);
  assert(output.layout == ui::Layout::kTable && output.row_count == 2);
  char expected_time[8];
  const time_t table_time = kNow - (560000 - 60000) / 1000;
  std::tm date{};
  gmtime_r(&table_time, &date);
  std::strftime(expected_time, sizeof(expected_time), "%H:%M", &date);
  assert(std::strstr(output.status, "Stand"));
  assert(std::strstr(output.status, expected_time));
  // Network failures keep the existing error tone, with usable rows retained.
  assert(output.status_tone == ui::Tone::kError);
  // A fresh table is independent of an ensuing Fixtures failure.
  store.recordDataset(1, MatchStore::Dataset::kTable, 600000, 600000, true,
                      Cause::kNone);
  store.recordDataset(1, MatchStore::Dataset::kFixtures, 650000, 650000, false,
                      Cause::kNetwork, 503);
  states[0].fetched_ms = 600000;
  renderModel(700000);
  assert(output.layout == ui::Layout::kTable && output.row_count == 2);
  assert(output.status[0] == 0);
  const auto previous_status = last_status;
  const auto previous_type = navigation.entry.type;
  {
    ui::NavigationData facts{store, shown, routes, {kNow, 100},
                             {},    true,  false};
    ui::NavigationContext context(config, facts);
    ui::sequence::applyAction(navigation, context.input(), ui::Action::kNext,
                              700001);
  }
  renderModel(700001);
  assert(navigation.entry.type != previous_type);
  assert(last_status == previous_status);  // navigation never rearms warning
  {
    ui::NavigationData facts{store, shown, routes, {kNow, 100},
                             {},    true,  false};
    ui::NavigationContext context(config, facts);
    ui::sequence::applyAction(navigation, context.input(), ui::Action::kDefault,
                              700002);
  }
  renderModel(700002);
  // The error card reports the selected dataset, even when another newer
  // dataset failure supplies the competition's aggregate warning.
  store.beginTable(1);
  assert(store.commitTable(true));
  store.recordDataset(1, MatchStore::Dataset::kTable, 800000, 800000, false,
                      Cause::kBadResponse, 422);
  store.recordDataset(1, MatchStore::Dataset::kFixtures, 900000, 900000, false,
                      Cause::kNetwork, 503);
  states[0].cause = Cause::kNetwork;
  states[0].http = 503;
  renderModel(950000);
  assert(last_status != previous_status);  // same aggregate, new Table failure
  assert(output.layout == ui::Layout::kCard && presentation.card_fault == 1);
  char table_code[kErrorCodeBytes];
  errorCode(table_code, cfg::kProviderDefault, Cause::kBadResponse, 422);
  assert(std::strcmp(output.card.hints[0], table_code) == 0);
  offset_before = 7200;
  offset_after = 3600;
  renderModel(950001, 1792888200, 4550001);
  assert(std::strcmp(output.card.hints[0], "Neuer Versuch 02:30") == 0);
  assert(std::strcmp(output.card.hints[1], table_code) == 0);
  offset_before = offset_after = 0;
  // Capacity notices stay meaningful even with a different dataset's active
  // request failure. Failed staging never replaces previously usable rows.
  store.recordDataset(1, MatchStore::Dataset::kTable, 1000000, 1000000, true,
                      Cause::kNone);
  store.beginTable(1);
  StoredRow overflow_row;
  for (unsigned i = 0; i <= cfg::kStoreTableRows; ++i)
    store.appendRow(overflow_row);
  assert(!store.commitTable(false));
  states[0].notices = store.notices(1);
  renderModel(1000001);
  assert(output.layout == ui::Layout::kCard && presentation.card_fault == 1);
  errorCode(table_code, cfg::kProviderDefault, Cause::kStoreFull, 0);
  assert(std::strcmp(output.card.hints[0], table_code) == 0);
  std::puts(
      "actual Presenter: time periods, real18 attempts/windows/correction, "
      "missing data, short targets, overview layouts, group-local "
      "tables/manual cross-group, dataset-specific routing/warnings/as-of "
      "and immutable calendar facts/DST/kickoff/as-of/retry/max page "
      "PASS");
}
