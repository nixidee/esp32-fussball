// Equal resolved seasons share data, but selection and presentation context do
// not inherit another configured club's ownership or focus.
#include <cassert>
#include <cstdio>

#include "civil_time.h"
#include "demo_scenarios.h"
#include "screen_selection.h"
#include "store_mappers.h"
#include "store_views.h"

using namespace football;

void testSourceViews() {
  constexpr Clock clock{1800000000, 40};
  settings::CompetitionRoutes routes;
  routes.entries.count = 2;
  for (unsigned i = 0; i < 2; ++i) {
    auto& entry = routes.entries[i];
    entry.id = i ? 91 : 7;
    entry.incarnation = i + 1;
    entry.active = true;
    entry.primary.provider = cfg::Provider::kOpenLigaDb;
    entry.primary.competition.assign("bl1");
    if (i) entry.primary.season.assign("2026");
    entry.club.team = i ? 50 : 40;
  }
  MatchStore store;
  SourceKey key;
  assert(key.assign(cfg::Provider::kOpenLigaDb, "bl1", "2026"));
  SourceId shared;
  for (const auto& entry : routes.entries) {
    assert(store.bind(entry.id, entry.incarnation, entry.club.team, clock));
    const auto id = store.resolve(entry.id, key, clock);
    assert(id && (!shared || shared == id));
    shared = id;
  }
  const auto slot = store.sourceSlot(shared);
  MapContext map;
  map.source = slot;
  map.clock = clock;
  provider::Fixture fixture;
  fixture.facts.id = 100;
  fixture.facts.kickoff = clock.utc - 600;
  fixture.facts.phase = Phase::kLive;
  fixture.facts.round = 6;
  fixture.home.id = 40;
  fixture.away.id = 60;
  fixture.fields = provider::kKickoff | provider::kPhase | provider::kRound;
  assert(storeMatch(fixture, {}, store, map));
  fixture.facts.id = 200;
  fixture.home.id = 70;
  fixture.away.id = 50;
  assert(storeMatch(fixture, {}, store, map));
  assert(storeMatch(fixture, {}, store, map));
  assert(store.matchCount() == 2);
  const auto* first = store.find({100, shared});
  const auto* second = store.find({200, shared});
  assert(first && second);
  assert(store.isOwn(7, *first) && !store.isOwn(91, *first));
  assert(store.isOwn(91, *second) && !store.isOwn(7, *second));

  settings::screen_policy::Options policy;
  policy.only_one_own_single = true;
  const selection::Input input{policy, routes, store, clock};
  const auto selected = selection::select(input);
  assert(selected.phase == selection::ScreenPhase::kOwn);
  assert(selected.competition == 7 && selected.focus == store.key(*first));
  assert(selection::single(input, selected, routes.entries[0]).match ==
         store.key(*first));
  assert(!selection::single(input, selected, routes.entries[1]).match.valid());
  assert(selection::window(input, routes.entries[0], *first).own_special);
  assert(!selection::window(input, routes.entries[1], *first).own_special);

  view::List members;
  members.count = 2;
  members.index[0] = 0;
  members.index[1] = 1;
  ConferenceRequest request;
  request.competition = 91;
  request.pin_own = true;
  request.per_page = 2;
  const auto page = view::conferencePage(store, members, request);
  assert(page.row_count == 2 && store.match(page.rows[0]).id == 200);
  request.competition = 7;
  assert(
      store.match(view::conferencePage(store, members, request).rows[0]).id ==
      100);

  view::Shown shown;
  for (const auto& entry : routes.entries) shown.add(entry);
  assert(shown.shows(store, *first));
  assert(view::matchCount(store, shown, 7) == 2);
  assert(view::matchCount(store, shown, 91) == 2);
  const auto overview = view::ownForOverview(store, shown, clock.utc);
  assert(overview.competition == 7 && store.match(overview.index).id == 100);

  store.beginTable(slot);
  provider::TableRow row;
  row.team.id = 40;
  row.facts.position = 1;
  assert(storeTableRow(row, store, map));
  row.team.id = 50;
  row.facts.position = 2;
  assert(storeTableRow(row, store, map));
  assert(store.commitTable(true));
  assert(view::table(store, 7).own == 0);
  assert(view::table(store, 91).own == 1);

  // A new season cannot make an old round with the same number its content.
  auto old = *first;
  old.phase = Phase::kFinished;
  old.finished_at = clock.utc - 60;
  assert(store.upsert(old, clock));
  assert(key.assign(cfg::Provider::kOpenLigaDb, "bl1", "2027"));
  const auto next = store.resolve(7, key, clock);
  map.source = store.sourceSlot(next);
  fixture.facts.id = 100;
  fixture.facts.kickoff = clock.utc + 7 * timekeeping::kSecondsPerDay;
  fixture.facts.phase = Phase::kScheduled;
  assert(storeMatch(fixture, {}, store, map));
  const auto* upcoming = store.find({100, next});
  const selection::Input after{policy, routes, store, clock};
  const auto round =
      selection::round(after, routes.entries[0], cfg::MatchdayMode::kCurrent);
  assert(round.valid());
  assert(selection::roundContains(after, round, *store.find({100, shared})));
  assert(!selection::roundContains(after, round, *upcoming));
  assert(view::table(store, 7).count == 0);
  assert(view::table(store, 91).count == 2);
  store.recordDataset(slot, MatchStore::Dataset::kFixtures, 300, 0, false,
                      Cause::kHttp, 503);
  const auto next_slot = store.sourceSlot(next);
  store.recordDataset(next_slot, MatchStore::Dataset::kFixtures, 1000, 1100,
                      true, Cause::kNone);
  store.recordDataset(next_slot, MatchStore::Dataset::kTable, 600, 0, false,
                      Cause::kBadResponse, 422);
  const auto summary = store.viewSummary(7);
  assert(summary.cause == Cause::kBadResponse && summary.http == 422 &&
         summary.failed_attempt_ms == 600 && summary.last_success_ms == 1100);

  // Production Demo uses two configured views of one source; the canonical
  // table is shared, and both configured clubs retain their own row.
  const auto bindings = demo::configure(routes, demo::Scenario::kOwn, false);
  assert(demo::apply(store, routes, bindings, demo::Scenario::kOwn, 4, 40).utc);
  assert(store.currentSource(routes.entries[0].id) ==
         store.currentSource(routes.entries[1].id));
  assert(store.rowCount() == 5);
  assert(view::table(store, routes.entries[0].id).own >= 0);
  assert(view::table(store, routes.entries[1].id).own >= 0);
  std::puts(
      "source-aware mapper/selection/conference/table/overview/round/"
      "rollover/Demo views PASS");
}
