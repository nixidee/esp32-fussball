// Typed provider facts → canonical RAM store. Provider schemas and score/event
// derivation live in the adapters; this boundary owns identity admission and
// presence-aware updates. No JSON or filesystem work under the store lock.
#pragma once
#include <span>

#include "provider_adapter.h"
namespace football {
using MatchObserver = void (*)(const StoredMatch&, void*);
using MatchEventObserver = void (*)(const EventFacts&, void*);
struct MapContext {
  SourceSlot source = kNoSource;
  uint8_t round = 0, round_label = kNoIndex;  // reported calendar scope
  Clock clock;
  MatchObserver observe_match = nullptr;
  void* match_context = nullptr;
  MatchEventObserver observe_events = nullptr;
  void* events_context = nullptr;
};
bool storeMatch(const provider::Fixture&, std::span<const provider::Event>,
                MatchStore&, const MapContext&);
bool storeTableRow(const provider::TableRow&, MatchStore&, const MapContext&);
void storeSummaryEvent(const provider::Event&, MatchStore&, const StoredMatch&);
}  // namespace football
