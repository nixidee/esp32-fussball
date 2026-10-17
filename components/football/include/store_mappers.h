// Provider JSON → compact match store (match_store.h). One call per element
// of a streamed response, as the provider client delivers them. Pure: no
// I/O and no ESP-IDF, so the native tests run them on recorded responses.

#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <string_view>

#include "match_store.h"

namespace football {

// Receives every mapped identity and full provider name, including rows that
// the store cannot admit, while the row
// is alive. It may copy bounded transient candidates, but must not mutate the
// store. Selection and committing names after a complete request belong to the
// caller, not the provider mapper.
using MatchNameObserver = void (*)(const StoredMatch& match,
                                   std::string_view home, std::string_view away,
                                   void* context);

using MatchEventObserver = void (*)(const StoredEvent& event, void* context);

struct MapContext {
  uint8_t competition = 0;  // stable competition id (1-255)
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  uint32_t club = 0;  // own club's team id at this provider, 0 = none
  Clock clock;
  MatchNameObserver observe_names = nullptr;
  void* names_context = nullptr;
  MatchEventObserver observe_events = nullptr;
  void* events_context = nullptr;
};

// One match: an OpenLigaDB match, an ESPN scoreboard event, an API-Football
// fixture item or a football-data.org match. A match with a detail slot also
// gets its events when the row carries them (OpenLigaDB goals, ESPN details).
// A match that does not fit is noted by the store and skipped. False only
// for a row without a numeric match id.
bool storeMatch(ArduinoJson::JsonVariantConst row, MatchStore& store,
                const MapContext& context);
// One ESPN match summary key event between MatchStore::beginEvents(...,
// detail) and endEvents(): only substitutions (OQ-207); the summary names
// the player coming on first, then the one going off. 'match' names the
// teams the event's team id is compared with (the provider's own match row).
void storeSummaryEvent(ArduinoJson::JsonVariantConst row, MatchStore& store,
                       const StoredMatch& match);
// One table element between MatchStore::beginTable() and commitTable(): an
// OpenLigaDB table row, an ESPN standings entry, an API-Football standings
// item (all its groups) or a football-data.org standing (only "TOTAL").
// False for a row without a team.
bool storeTableRow(ArduinoJson::JsonVariantConst row, MatchStore& store,
                   const MapContext& context);

}  // namespace football
