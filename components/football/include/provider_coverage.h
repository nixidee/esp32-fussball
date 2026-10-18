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
  settings::Bytes<cfg::kSeasonMaxBytes> season;
  // Closed UTC bounds of an explicitly requested date range. Full-season
  // responses establish only the interval between their actual outer rows.
  uint32_t from = 0, to = 0;
  QueryKind kind = QueryKind::kFullSeason;
};

// No new season resolver: use the existing source's resolved year, or its
// explicit year. The second query is the adjacent next season. Non-year OLG
// season identifiers cannot be incremented and are left unknown.
bool discoveryQuery(const settings::CompetitionRoute& entry,
                    std::string_view resolved_season, uint32_t anchor_utc,
                    uint16_t horizon_days, uint8_t season_index, Query& out,
                    Reason* why = nullptr) noexcept;

// Normal fixture rows can refute a prior proof before display admission or
// eviction. They never refresh a season's checked_utc or establish coverage.
bool observeNormalRow(Evidence& evidence,
                      const settings::CompetitionRoute& entry,
                      const provider::Fixture& row) noexcept;

class Response {
 public:
  // Both references remain alive and unchanged until finish(). No provider
  // source/policy snapshot or JSON document is copied or retained here.
  void begin(const settings::CompetitionRoute& entry, const Query& query,
             uint32_t anchor_utc) noexcept;
  // Every primary fixture row, before MatchStore admission or filtering.
  // Unknown/TBD timing invalidates evidence even when the store skips it.
  void observe(const provider::Fixture& row) noexcept;
  void parsed(const provider::ParseResult& result) noexcept;
  const Span& finish(bool success, uint32_t checked_utc) noexcept;
  const Identity& source() const noexcept { return collector_.source(); }

 private:
  const settings::CompetitionRoute* entry_ = nullptr;
  const Query* query_ = nullptr;
  Collector collector_;
  provider::ParseResult parsed_;
  uint32_t first_ = 0, last_ = 0;
};

}  // namespace football::coverage
