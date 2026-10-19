// Shared season facts, independent configured views, and stale logical IDs.
#include <cassert>
#include <cstdio>

#include "match_store.h"

using namespace football;
void testSourceViews();
void testSourceRounds();

namespace {
constexpr Clock clock_now{1800000000, 30};
SourceKey source(std::string_view season) {
  SourceKey out;
  assert(out.assign(cfg::Provider::kOpenLigaDb, "bl1", season));
  return out;
}
void event(MatchStore& store, const StoredMatch& match, uint32_t identity) {
  assert(store.beginEvents(match));
  EventFacts facts;
  facts.identity = identity;
  facts.player.assign("Recorded scorer");
  store.addEvent(facts);
  store.endEvents();
}
void expectEvent(const MatchStore& store, MatchKey key, uint32_t identity) {
  const auto* match = store.find(key);
  assert(match && store.hasDetail(*match));
  const auto& detail = store.detailAt(match->detail);
  if (detail.event_count != 1)
    for (uint8_t i = 0; i < detail.event_count; ++i)
      std::fprintf(stderr, "match=%u event=%u flags=%u expected=%u\n", key.id,
                   store.eventAt(detail.events[i]).identity,
                   store.eventAt(detail.events[i]).flags, identity);
  assert(detail.event_count == 1);
  assert(store.eventAt(detail.events[0]).identity == identity);
}
StoredMatch row(MatchStore& store, SourceSlot slot, uint32_t id, uint32_t home,
                uint32_t away) {
  StoredMatch match;
  match.id = id;
  match.source = slot;
  match.home = store.team(slot, home, "Home");
  match.away = store.team(slot, away, "Away");
  match.kickoff = clock_now.utc - 60;
  match.phase = Phase::kLive;
  match.principal = {1, 0};
  return match;
}
}  // namespace

int main() {
  testSourceViews();
  testSourceRounds();
  MatchStore store;
  assert(store.bind(7, 1, 40, clock_now));
  assert(store.bind(91, 9, 50, clock_now));
  const auto shared = store.resolve(7, source("2026"), clock_now);
  assert(shared && store.resolve(91, source("2026"), clock_now) == shared);
  const auto slot = store.sourceSlot(shared);
  assert(slot == 0);  // The first physical slot must work in every path.
  auto match = row(store, slot, 100, 40, 50);
  assert(store.upsert(match, clock_now));
  match.principal = {2, 0};
  assert(store.upsert(match, clock_now));
  const MatchKey key{100, shared};
  assert(store.matchCount() == 1 && store.find(key)->principal.home == 2);
  bool home = false, away = false;
  store.ownSides(7, *store.find(key), home, away);
  assert(home && !away);
  store.ownSides(91, *store.find(key), home, away);
  assert(!home && away);
  event(store, *store.find(key), 10);
  assert(store.ensureDetail(key, 91, clock_now));
  auto corrected = *store.find(key);
  corrected.home = store.team(slot, 70, "Corrected home");
  corrected.away = store.team(slot, 80, "Corrected away");
  assert(store.upsert(corrected, clock_now));
  expectEvent(store, key, 10);
  corrected.home = match.home;
  corrected.away = match.away;
  assert(store.upsert(corrected, clock_now));
  expectEvent(store, key, 10);

  store.beginTable(slot);
  StoredRow standing;
  standing.team = match.home;
  standing.position = 1;
  assert(store.appendRow(standing));
  standing.team = match.away;
  standing.position = 2;
  assert(store.appendRow(standing) && store.commitTable(true));
  assert(store.rowCount() == 2);
  assert(store.isOwn(7, store.row(0)) && !store.isOwn(91, store.row(0)));
  assert(store.isOwn(91, store.row(1)) && !store.isOwn(7, store.row(1)));
  store.recordDataset(slot, MatchStore::Dataset::kTable, 200, 0, false,
                      Cause::kHttp, 503);
  store.recordDataset(slot, MatchStore::Dataset::kFixtures, 300, 301, true,
                      Cause::kNone);
  assert(store.datasetStatus(slot, MatchStore::Dataset::kTable).http == 503);
  store.beginTable(slot);
  assert(!store.commitTable(false) && store.rowCount() == 2);
  store.beginTable(slot);
  assert(store.commitTable(true) && store.rowCount() == 0);

  // Distinct ordinary views can request distinct matches of the same source.
  assert(store.bind(7, 1, 0, clock_now));
  assert(store.bind(91, 9, 0, clock_now));
  assert(store.upsert(row(store, slot, 200, 60, 70), clock_now));
  assert(store.ensureDetail(key, 7, clock_now));
  event(store, *store.find(key), 10);
  const MatchKey second{200, shared};
  assert(store.ensureDetail(second, 91, clock_now));
  event(store, *store.find(second), 22);
  expectEvent(store, key, 10);
  assert(store.ensureDetail(key, 91, clock_now));
  expectEvent(store, key, 10);
  assert(store.ensureDetail(second, 91, clock_now));
  expectEvent(store, key, 10);
  expectEvent(store, second, 22);

  // A rollover changes only this binding's current source. Previous results
  // remain addressable through their original logical ID and view membership.
  const auto next = store.resolve(7, source("2027"), clock_now);
  assert(next && next != shared);
  assert(store.currentSource(7) == store.sourceSlot(next));
  assert(store.currentSource(91) == slot && store.includes(7, slot));
  expectEvent(store, key, 10);
  store.unbind(91, clock_now);
  expectEvent(store, key, 10);
  assert(store.includes(7, slot));
  event(store, *store.find(key), 11);
  const auto& withdrawn = store.detailAt(store.find(key)->detail);
  assert(withdrawn.event_count == 2);
  assert(store.eventAt(withdrawn.events[0]).identity == 11);
  assert(store.eventAt(withdrawn.events[1]).identity == 10);
  assert(store.eventAt(withdrawn.events[1]).flags & kEventRemoved);
  // Reusing a configured ID with a new incarnation removes the old interest.
  assert(store.bind(7, 2, 40, clock_now));
  assert(!store.find(key) && store.sourceSlot(shared) == kNoSource);
  const auto reused = store.resolve(7, source("2026"), clock_now);
  assert(reused && reused != shared);
  assert(!store.find(key));
  store.clear();
  assert(store.bind(7, 3, 40, clock_now));
  const auto after_reset = store.resolve(7, source("2026"), clock_now);
  assert(after_reset && after_reset != reused && after_reset != shared);
  assert(store.upsert(row(store, store.sourceSlot(after_reset), 100, 40, 50),
                      clock_now));
  assert(!store.find(key) && store.matchCount() == 1);
  std::printf(
      "shared-season store: dedup/per-view clubs/shared table/failure/"
      "empty/detail interests/rollover/unbind/reused IDs/reset PASS; "
      "Store=%zu Match=%zu Details=%zu Round=%zu\n",
      sizeof(MatchStore), sizeof(StoredMatch), sizeof(MatchDetails),
      sizeof(StoredRound));
}
