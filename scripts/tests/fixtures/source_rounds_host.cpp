// Provider round corrections and explicit loss admission through actual owners.
#include <cassert>
#include <cstdio>

#include "fixture_retention.h"
#include "store_mappers.h"
using namespace football;
namespace {
SourceSlot bind(MatchStore& store) {
  assert(store.bind(1, 1, 0, {}));
  SourceKey key;
  assert(key.assign(cfg::Provider::kEspn, "ger.dfb_pokal", "2026"));
  const auto source = store.sourceSlot(store.resolve(1, key, {}));
  assert(source != kNoSource);
  return source;
}
void fillLabels(MatchStore& store, unsigned begin = 0) {
  for (unsigned i = begin; i < cfg::kStoreRoundLabels; ++i) {
    char label[24];
    std::snprintf(label, sizeof(label), "Reserved label %u", i);
    assert(store.label(label) != kNoIndex);
  }
}
bool correctedRound() {
  MatchStore store;
  const auto source = bind(store);
  const auto label = store.label("Quarterfinals");
  store.roundResult(source, 4, label, 4, true, 100, 200, RoundKind::kCalendar);
  store.roundResult(source, 4, label, 4, true, 110, 210, RoundKind::kCalendar);
  return store.roundCount() == 1 && store.round(0).from == 110 &&
         store.round(0).to == 210;
}
bool mapperLabelLimit() {
  MatchStore store;
  const auto source = bind(store);
  StoredMatch prior;
  prior.id = 99;
  prior.source = source;
  prior.label = store.label("Previously reported");
  prior.round = 4;
  prior.principal = {2, 1};
  assert(store.upsert(prior, {}));
  fillLabels(store, 1);
  provider::Fixture update;
  update.facts.id = 99;
  update.facts.principal = {3, 1};
  update.fields = provider::kPrincipal | provider::kRound;
  update.facts.round = 5;
  update.round_label.assign("New provider round");
  MapContext context;
  context.source = source;
  const bool accepted = storeMatch(update, {}, store, context);
  const auto* kept = store.find(source, 99);
  assert(kept);
  return !accepted && kept->principal.home == 2 && kept->round == 4 &&
         kept->label == prior.label && (store.notices(source) & kNoticeMatches);
}
bool calendarLabelLimit() {
  MatchStore store;
  const auto source = bind(store);
  fillLabels(store);
  constexpr uint32_t day = 1792195200;
  retention::Index index(0, day + 3600, {}, true);
  for (uint8_t i = 1; i <= 2; ++i) {
    provider::CalendarRound round;
    round.number = i;
    round.from = day + (i - 1) * 7 * 86400;
    round.to = round.from + 86400 - 1;
    round.label.assign(i == 1 ? "Quarterfinals" : "Semifinals");
    assert(index.observeRound(round));
    assert(index.observeDay(round.from));
  }
  retention::Selection selected;
  const bool accepted = index.select(selected, &store, source);
  return !accepted && !store.roundCount() &&
         (store.notices(source) & kNoticeMatches);
}
}  // namespace
void testSourceRounds() {
  const bool corrected = correctedRound(), mapped = mapperLabelLimit(),
             calendar = calendarLabelLimit();
  if (!corrected || !mapped || !calendar)
    std::fprintf(
        stderr,
        "round correction=%d mapper label limit=%d calendar label limit=%d\n",
        corrected, mapped, calendar);
  assert(corrected && mapped && calendar);
  std::puts(
      "source rounds: corrected calendar range replaces same stage; full label "
      "pool preserves old match and rejects calendar proof with visible "
      "capacity failure PASS");
}
