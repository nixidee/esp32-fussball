#include "provider_coverage.h"

#include <algorithm>

#include "civil_time.h"

namespace football::coverage {
namespace {
constexpr uint32_t kDay = timekeeping::kSecondsPerDay;
}  // namespace

bool observeNormalRow(EvidenceArray& evidence, const SourceKey& source,
                      const provider::Fixture& row) noexcept {
  if (!row.source_matches) {
    invalidateFamily(evidence, source, Reason::kIdentity);
    return true;
  }
  return observeNormal(evidence, source, row.facts.kickoff, row.time_definite);
}

bool discoveryQuery(const SourceKey& source, uint32_t anchor_utc,
                    uint16_t horizon_days, Query& out, Reason* why) noexcept {
  const auto fail = [why](Reason reason) {
    if (why) *why = reason;
    return false;
  };
  if (!source.valid()) return fail(Reason::kIdentity);
  if (!provider::capabilities(source.provider).coverage)
    return fail(Reason::kMissingMetadata);
  if (!anchor_utc) return fail(Reason::kClock);
  if (!settings::isValidSeason(source.provider, source.season.view()))
    return fail(Reason::kMissingMetadata);
  if (source.provider != cfg::Provider::kOpenLigaDb) {
    if (!horizon_days || horizon_days > cfg::kScreenBreakDaysRange.max + 1)
      return fail(Reason::kRange);
    const uint64_t horizon = uint64_t{horizon_days} * kDay;
    if (anchor_utc <= horizon ||
        uint64_t{anchor_utc} + horizon + kDay > UINT32_MAX)
      return fail(Reason::kRange);
  }
  out = {};
  out.source = source;
  out.http.spec.source = {out.source.provider, out.source.competition.view(),
                          out.source.season.view()};
  out.http.spec.operation = provider::Operation::kFixtures;
  if (source.provider == cfg::Provider::kOpenLigaDb)
    out.kind = QueryKind::kFullSeason;
  else {
    const uint64_t horizon = uint64_t{horizon_days} * kDay;
    out.kind = QueryKind::kDateRange;
    out.from = (anchor_utc - horizon) / kDay * kDay;
    out.to = (anchor_utc + horizon) / kDay * kDay + kDay - 1;
    out.http.spec.from = out.from;
    out.http.spec.to = out.to;
  }
  if (why) *why = Reason::kNone;
  return true;
}

void Response::begin(const Query& query, uint32_t anchor_utc) noexcept {
  query_ = &query;
  first_ = last_ = 0;
  parsed_ = {};
  const Proof proof = query.kind == QueryKind::kFullSeason ? Proof::kFullSeason
                                                           : Proof::kDateRange;
  collector_.begin(query.source, query.from, query.to, anchor_utc, proof);
}
void Response::observe(const provider::Fixture& row) noexcept {
  if (!query_) return;
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
  if (!query_) {
    collector_.failed(Reason::kIdentity);
    return collector_.finish(checked_utc, {});
  }
  const auto& meta = parsed_.metadata;
  const bool complete = success && parsed_.status == provider::Status::kOk &&
                        parsed_.primary_seen && parsed_.document_complete &&
                        !parsed_.early_stopped;
  bool verified = meta.metadata_complete && meta.source_matches &&
                  !meta.duplicate && !meta.invalid;
  bool count_known = meta.expected_rows_present;
  if (query_->source.provider == cfg::Provider::kOpenLigaDb) {
    collector_.bounds(first_, last_);
    verified = collector_.rows() != 0;
    count_known = false;
  }
  return collector_.finish(checked_utc, {meta.expected_rows, count_known,
                                         complete, verified, verified});
}
}  // namespace football::coverage
