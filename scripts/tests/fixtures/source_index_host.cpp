#include <array>
#include <cassert>
#include <iostream>
#include <string>

#include "source_index.h"

using namespace football;

int main() {
  SourceIndex sources;
  SourceKey automatic, explicit_season, next_season;
  assert(automatic.assign(cfg::Provider::kOpenLigaDb, "bl1", "2026"));
  assert(explicit_season.assign(cfg::Provider::kOpenLigaDb, "bl1", "2026"));
  assert(next_season.assign(cfg::Provider::kOpenLigaDb, "bl1", "2027"));
  const auto first = sources.intern(automatic);
  assert(first && sources.intern(explicit_season) == first);
  const auto next = sources.intern(next_season);
  assert(next && next != first && sources.find(automatic) == first);
  assert(*sources.key(sources.slot(first)) == automatic);
  assert(sources.id(sources.slot(next)) == next);

  // Resolution must happen before interning. Invalid/overlong input preserves
  // the prior key instead of silently turning a prefix into another source.
  const auto before = automatic;
  for (const auto& season : {std::string{}, std::string(17, '2'),
                             std::string("20\0x", 4), std::string("\xff", 1)}) {
    assert(!automatic.assign(cfg::Provider::kOpenLigaDb, "bl1", season));
    assert(automatic == before);
  }
  assert(!automatic.assign(cfg::Provider::kOpenLigaDb, "", "2026"));
  assert(!automatic.assign(cfg::Provider::kOpenLigaDb, std::string(33, 'a'),
                           "2026"));
  assert(!automatic.assign(static_cast<cfg::Provider>(255), "bl1", "2026"));
  SourceKey malformed;
  malformed.competition.length = 255;
  assert(!sources.intern(malformed) && !sources.find(malformed));
  malformed = before;
  malformed.season.length = 255;
  assert(!sources.intern(malformed) && automatic == before);

  // Competition spelling and provider are exact, independently bounded facts.
  SourceKey different;
  assert(different.assign(cfg::Provider::kOpenLigaDb, "bl12", "026"));
  assert(sources.intern(different) != first);
  assert(different.assign(cfg::Provider::kEspn, "bl1", "2026"));
  assert(sources.intern(different) != first);
  assert(different.assign(cfg::Provider::kOpenLigaDb, "BL1", "2026"));
  assert(sources.intern(different) != first);

  // Keep every existing source on capacity exhaustion, including the previous
  // season. Release/reuse/reset never make a delayed SourceId name new facts.
  std::array<SourceId, cfg::kStoreSources> retained;
  for (std::size_t i = 0; i < retained.size(); ++i) {
    assert(different.assign(cfg::Provider::kEspn, "league" + std::to_string(i),
                            "2026"));
    retained[i] = sources.intern(different);
  }
  for (const auto id : retained)
    if (id) assert(sources.key(sources.slot(id)));
  assert(different.assign(cfg::Provider::kEspn, "overflow", "2026"));
  assert(!sources.intern(different));
  assert(sources.intern(explicit_season) == first);
  const auto old_slot = sources.slot(first);
  sources.release(first);
  assert(sources.slot(first) == kNoSource && !sources.key(old_slot));
  const auto replacement = sources.intern(different);
  assert(replacement != first && sources.slot(replacement) == old_slot);
  sources.clear();
  assert(sources.slot(replacement) == kNoSource);
  const auto after_reset = sources.intern(before);
  assert(after_reset.value > replacement.value &&
         sources.slot(first) == kNoSource);
  assert(!sources.key(kNoSource) && !sources.id(kNoSource));
  sources.release({});
  assert(sources.find(before) == after_reset);
  std::cout
      << "resolved source index: auto/explicit equality, rollover retention, "
         "strict input/capacity and stale IDs after slot/reset reuse PASS\n";
  std::cout << "host ABI key=" << sizeof(SourceKey)
            << " index=" << sizeof(SourceIndex) << '\n';
}
