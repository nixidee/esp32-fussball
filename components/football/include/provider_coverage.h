#pragma once

// Bounded provider-response evidence. The service owns the one EvidenceArray,
// HTTP admission; this adapter only reduces one complete response.
#include "provider_adapter.h"
#include "provider_plan.h"
#include "schedule_coverage.h"

namespace football::coverage {

enum class QueryKind : uint8_t { kFullSeason, kDateRange };
struct Query {
  plan::Request http;
  SourceKey source;
  // Closed UTC bounds of an explicitly requested date range. Full-season
  // responses establish only the interval between their actual outer rows.
  uint32_t from = 0, to = 0;
  QueryKind kind = QueryKind::kFullSeason;
};

// The exact requested season is already resolved; acquisition never guesses it.
bool discoveryQuery(const SourceKey& source, uint32_t anchor_utc,
                    uint16_t horizon_days, Query& out,
                    Reason* why = nullptr) noexcept;
// Normal rows can refute all affected season proofs, before display admission.
bool observeNormalRow(EvidenceArray&, const SourceKey&,
                      const provider::Fixture&) noexcept;

class Response {
 public:
  // Both references remain alive and unchanged until finish(). No provider
  // source/policy snapshot or JSON document is copied or retained here.
  void begin(const Query& query, uint32_t anchor_utc) noexcept;
  // Every primary fixture row, before MatchStore admission or filtering.
  // Unknown/TBD timing invalidates evidence even when the store skips it.
  void observe(const provider::Fixture& row) noexcept;
  void parsed(const provider::ParseResult& result) noexcept;
  const Span& finish(bool success, uint32_t checked_utc) noexcept;
  const SourceKey& source() const noexcept { return collector_.source(); }

 private:
  const Query* query_ = nullptr;
  Collector collector_;
  provider::ParseResult parsed_;
  uint32_t first_ = 0, last_ = 0;
};

}  // namespace football::coverage
