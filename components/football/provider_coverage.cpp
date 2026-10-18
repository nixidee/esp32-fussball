#include "provider_coverage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#include "civil_time.h"

namespace football::coverage {
namespace {
constexpr uint32_t kDay = timekeeping::kSecondsPerDay;
}  // namespace

bool observeNormalRow(Evidence& evidence,
                      const settings::CompetitionRoute& entry,
                      const provider::Fixture& row) noexcept {
  if (!row.source_matches) {
    invalidate(evidence, Reason::kIdentity);
    return true;
  }
  return observeNormal(evidence, identity(entry), row.facts.kickoff,
                       row.time_definite);
}

bool discoveryQuery(const settings::CompetitionRoute& entry,
                    std::string_view resolved_season, uint32_t anchor_utc,
                    uint16_t horizon_days, uint8_t season_index, Query& out,
                    Reason* why) noexcept {
  const auto fail = [why](Reason reason) {
    if (why) *why = reason;
    return false;
  };
  if (!entry.active || entry.id == 0 || entry.incarnation == 0 ||
      entry.primary.competition.empty())
    return fail(Reason::kIdentity);
  if (!provider::capabilities(entry.primary.provider).coverage)
    return fail(Reason::kMissingMetadata);
  if (anchor_utc == 0) return fail(Reason::kClock);
  if (season_index >= cfg::kCoverageMaxSeasons)
    return fail(Reason::kSeasonLimit);
  const auto& source = entry.primary;
  if (static_cast<unsigned>(source.provider) >= cfg::kProviderCount)
    return fail(Reason::kIdentity);
  if (source.provider != cfg::Provider::kOpenLigaDb) {
    if (horizon_days == 0 || horizon_days > 367) return fail(Reason::kRange);
    const uint64_t horizon = uint64_t{horizon_days} * kDay;
    if (anchor_utc <= horizon ||
        uint64_t{anchor_utc} + horizon + kDay > UINT32_MAX)
      return fail(Reason::kRange);
  }
  std::string_view season =
      source.season.empty() ? resolved_season : source.season.view();
  if (season.empty() || !settings::isValidSeason(source.provider, season))
    return fail(Reason::kMissingMetadata);
  char next_season[5]{};
  if (season_index != 0) {
    uint32_t year = 0;
    if (season.size() != 4 || !(year = decimalId(season)) || year >= 9999)
      return fail(Reason::kSeasonLimit);
    std::snprintf(next_season, sizeof(next_season), "%04u", unsigned(year + 1));
    season = next_season;
  }
  // Start constructing only after validation; a rejected query never replaces
  // the caller's current request or points at a temporary string.
  out = {};
  if (!out.season.assign(season)) return fail(Reason::kRange);
  out.http.spec.source = {source.provider, source.competition.view(),
                          out.season.view()};
  if (source.provider == cfg::Provider::kOpenLigaDb) {
    out.kind = QueryKind::kFullSeason;
    out.http.spec.operation = provider::Operation::kFixtures;
  } else {
    const uint64_t horizon = uint64_t{horizon_days} * kDay;
    out.kind = QueryKind::kDateRange;
    out.from = (anchor_utc - horizon) / kDay * kDay;
    out.to = (anchor_utc + horizon) / kDay * kDay + kDay - 1;
    out.http.spec.operation = provider::Operation::kFixtures;
    out.http.spec.from = out.from;
    out.http.spec.to = out.to;
  }
  if (why) *why = Reason::kNone;
  return true;
}

void Response::begin(const settings::CompetitionRoute& entry,
                     const Query& query, uint32_t anchor_utc) noexcept {
  entry_ = &entry;
  query_ = &query;
  first_ = last_ = 0;
  parsed_ = {};
  const Proof proof = query.kind == QueryKind::kFullSeason ? Proof::kFullSeason
                                                           : Proof::kDateRange;
  collector_.begin(identity(entry), query.season.view(), query.from, query.to,
                   anchor_utc, proof);
}
void Response::observe(const provider::Fixture& row) noexcept {
  if (!entry_ || !query_) return;
  if (!row.source_matches || !row.facts.id)
    collector_.failed(Reason::kIdentity);
  if (!row.scope_matches) collector_.failed(Reason::kRange);
  collector_.observe(row.facts.kickoff, row.time_definite);
  if (row.facts.kickoff) {
    first_ = first_ ? std::min(first_, row.facts.kickoff) : row.facts.kickoff;
    last_ = std::max(last_, row.facts.kickoff);
  }
}
void Response::parsed(const provider::ParseResult& result) noexcept {
  parsed_ = result;
  // Invalid identities are rejected before store/scope callbacks. Their row
  // count still prevents a skipped row from becoming evidence of absence.
  if (query_ && result.rows > collector_.rows())
    collector_.failed(Reason::kIdentity);
  if ((result.metadata.current_page && result.metadata.current_page != 1) ||
      (result.metadata.total_pages && result.metadata.total_pages != 1))
    collector_.failed(Reason::kProviderLimit);
}
const Span& Response::finish(bool success, uint32_t checked_utc) noexcept {
  if (!entry_ || !query_) {
    collector_.failed(Reason::kIdentity);
    return collector_.finish(checked_utc, {});
  }
  const auto& meta = parsed_.metadata;
  const bool complete = success && parsed_.status == provider::Status::kOk &&
                        parsed_.primary_seen && parsed_.document_complete &&
                        !parsed_.early_stopped;
  bool verified = meta.metadata_complete && !meta.duplicate && !meta.invalid;
  bool count_known = meta.expected_rows_present;
  if (entry_->primary.provider == cfg::Provider::kOpenLigaDb) {
    collector_.bounds(first_, last_);
    verified = collector_.rows() != 0;
    count_known = false;
  }
  return collector_.finish(checked_utc, {meta.expected_rows, count_known,
                                         complete, verified, verified});
}
}  // namespace football::coverage
