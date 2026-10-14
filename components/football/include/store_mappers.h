// Provider JSON → compact match store (match_store.h). One call per element
// of a streamed response, as the provider client delivers them. Pure: no
// I/O and no ESP-IDF, so the native tests run them on recorded responses.

#pragma once

#include <ArduinoJson.h>

#include <array>
#include <cstdint>

#include "match_store.h"

namespace football {

// The fixture pairs of the fallback sources: each binds a fallback match id
// of a competition to the id of its primary match (verified by the owner,
// never guessed).
struct Pairing {
  struct Pair {
    uint32_t secondary = 0, primary = 0;
    uint8_t competition = 0;
    bool swapped = false;  // the fallback lists home and away the other way
  };
  cfg::Provider primary = cfg::Provider::kOpenLigaDb;  // primary's provider
  // The fallback route while the primary source fails: every match is
  // stored on its own (as the primary's stand-in), none fills. Otherwise
  // paired matches fill and unpaired ones are dropped.
  bool standalone = false;
  uint8_t count = 0;
  std::array<Pair, cfg::kFixtureMappingMax> pairs{};
  const Pair* find(uint8_t competition, uint32_t secondary) const;
};

// Pick mode (fixture picker): an own-club match of the response, with its
// provider match id, kickoff (UTC seconds, 0 = unknown) and full names.
using MatchPick = void (*)(uint32_t id, uint32_t kickoff, const char* home,
                           const char* away, void* context);

struct MapContext {
  uint8_t competition = 0;  // stable competition id (1-255)
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  uint32_t club = 0;  // own club's team id at this provider, 0 = none
  Clock clock;
  // Set for a fallback source: a paired match fills its primary match
  // (MatchStore::fill(), its events as fallback events), an unpaired one
  // is dropped; on the fallback route (standalone) every match is stored
  // on its own.
  const Pairing* pairing = nullptr;
  // Set: pick mode. Own-club matches go to 'pick' instead of the store;
  // nothing is stored (no teams, labels, matches or events).
  MatchPick pick = nullptr;
  void* pick_context = nullptr;
};

// One match: an OpenLigaDB match, an ESPN scoreboard event, an API-Football
// fixture item or a football-data.org match. An own-club match also gets
// its events when the row carries them (OpenLigaDB goals, ESPN details).
// A match that does not fit is noted by the store and skipped; a fallback
// source's match goes where 'context.pairing' says. False only for a row
// without a numeric match id.
bool storeMatch(ArduinoJson::JsonVariantConst row, MatchStore& store,
                const MapContext& context);
// One event of 'match' between MatchStore::beginEvents() and endEvents():
// an API-Football fixtures/events item or an ESPN scoreboard detail. Event
// types the display does not show are skipped. 'match' names the teams the
// event's team id is compared with (the provider's own match row).
void storeEvent(ArduinoJson::JsonVariantConst row, MatchStore& store,
                const StoredMatch& match, cfg::Provider provider);
// One ESPN match summary key event between MatchStore::beginEvents(...,
// detail) and endEvents(): only substitutions (OQ-207); the summary names
// the player coming on first, then the one going off. 'match' as for
// storeEvent().
void storeSummaryEvent(ArduinoJson::JsonVariantConst row, MatchStore& store,
                       const StoredMatch& match);
// One table element between MatchStore::beginTable() and commitTable(): an
// OpenLigaDB table row, an ESPN standings entry, an API-Football standings
// item (all its groups) or a football-data.org standing (only "TOTAL").
// False for a row without a team.
bool storeTableRow(ArduinoJson::JsonVariantConst row, MatchStore& store,
                   const MapContext& context);

// One core-data element: an OpenLigaDB available team, an ESPN teams entry,
// an API-Football teams item or a football-data.org team. Updates the short
// name of a team the store already holds; core data adds no teams, the
// directory serves stored matches and table rows only. False for an element
// without a team id.
bool storeTeam(ArduinoJson::JsonVariantConst row, MatchStore& store,
               const MapContext& context);

}  // namespace football
