// Synthetic native fixture context. Production keeps exact provider keys;
// fixture competition numbers below deliberately name distinct test sources.
#pragma once

#include <cassert>
#include <cstdio>
#include <optional>

#include "match_store.h"

namespace football::test_source {
inline SourceSlot source(MatchStore& store, uint8_t competition,
                         cfg::Provider provider,
                         std::optional<uint32_t> club = std::nullopt,
                         Clock clock = {}) {
  auto current = store.currentSource(competition);
  if (current != kNoSource) {
    assert(store.provider(current) == provider);
    if (club) assert(store.bind(competition, competition, *club, clock));
    return current;
  }
  assert(store.bind(competition, competition, club.value_or(0), clock));
  char identity[4];
  std::snprintf(identity, sizeof(identity), "%u", competition);
  SourceKey key;
  assert(key.assign(provider, identity, "2026"));
  current = store.sourceSlot(store.resolve(competition, key, clock));
  assert(current != kNoSource);
  return current;
}
inline SourceSlot source(MatchStore& store, uint8_t competition) {
  const auto current = store.currentSource(competition);
  return current == kNoSource
             ? source(store, competition, cfg::kProviderDefault)
             : current;
}
inline uint8_t competition(const MatchStore& store, SourceSlot source) {
  for (unsigned id = 1; id <= UINT8_MAX; ++id)
    if (store.currentSource(static_cast<uint8_t>(id)) == source)
      return static_cast<uint8_t>(id);
  assert(false && "fixture source has no configured context");
  return 0;
}
inline void selectClub(MatchStore& store, uint8_t competition,
                       cfg::Provider provider, uint32_t club, Clock clock) {
  source(store, competition, provider, club, clock);
}
}  // namespace football::test_source
