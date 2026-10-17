#pragma once

// Bounded provider-response evidence. The service owns the one EvidenceArray,
// HTTP admission and files; this adapter only reduces one complete response.
#include "provider_json.h"
#include "provider_plan.h"
#include "schedule_coverage.h"

namespace football::coverage {

enum class QueryKind : uint8_t { kFullSeason, kDateRange, kEspnCalendar };
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
bool discoveryQuery(const settings::Competition& entry,
                    std::string_view resolved_season, uint32_t anchor_utc,
                    uint16_t horizon_days, uint8_t season_index, Query& out,
                    Reason* why = nullptr) noexcept;

// Normal fixture rows can refute a prior proof before display admission or
// eviction. They never refresh a season's checked_utc or establish coverage.
bool observeNormalRow(Evidence& evidence, const settings::Competition& entry,
                      ArduinoJson::JsonVariantConst row) noexcept;

struct Calendar {
  uint32_t from = 0, to = 0, previous_day = 0, next_day = 0;
  uint32_t count = 0;
  bool verified = false;
};

class Response {
 public:
  // Both references remain alive and unchanged until finish(). No provider
  // source/policy snapshot or JSON document is copied or retained here.
  void begin(const settings::Competition& entry, const Query& query,
             uint32_t anchor_utc) noexcept;
  const JsonObserver* observer() noexcept;
  JsonScan& scan() noexcept { return scan_; }
  // Every primary fixture row, before MatchStore admission or filtering.
  // Unknown/TBD timing invalidates evidence even when the store skips it.
  void observe(ArduinoJson::JsonVariantConst row) noexcept;
  const Span& finish(bool success, uint32_t checked_utc) noexcept;
  const Identity& source() const noexcept { return collector_.source(); }
  const Calendar& calendar() const noexcept { return calendar_; }

 private:
  static bool metadata(uint8_t tag, ArduinoJson::JsonVariantConst value,
                       void* context);
  void field(uint8_t tag, ArduinoJson::JsonVariantConst value) noexcept;
  const settings::Competition* entry_ = nullptr;
  const Query* query_ = nullptr;
  Collector collector_;
  JsonObserver observer_{};
  JsonScan scan_{};
  Calendar calendar_{};
  uint32_t anchor_ = 0, first_ = 0, last_ = 0, expected_ = 0;
  uint32_t calendar_first_ = 0, calendar_last_ = 0;
  uint32_t filter_keys_ = 0;
  uint16_t metadata_ok_ = 0;
  bool bad_metadata_ = false, source_matches_ = false;
};

}  // namespace football::coverage
