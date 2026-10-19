#include "schedule_coverage.h"

#include <algorithm>
#include <cstdio>
#include <limits>

#include "binary_codec.h"
#include "civil_time.h"
#include "settings_record.h"

namespace football::coverage {
namespace {
constexpr uint32_t kMaximum = std::numeric_limits<uint32_t>::max();

bool validReason(Reason reason) { return reason <= Reason::kRange; }
bool validSpan(const Span& span) {
  if (span.completeness > Completeness::kComplete ||
      span.proof > Proof::kDayWhitelist || !validReason(span.reason))
    return false;
  if (span.completeness != Completeness::kComplete) return true;
  return span.proof != Proof::kNone && span.reason == Reason::kNone &&
         span.checked_utc != 0 && span.from != 0 && span.from <= span.to &&
         (span.previous == 0 ||
          (span.previous >= span.from && span.previous <= span.to &&
           span.previous <= span.checked_utc)) &&
         (span.next == 0 || (span.next >= span.from && span.next <= span.to)) &&
         (span.previous == 0 || span.next == 0 || span.previous < span.next);
}
Reason usable(const Span& span, uint32_t now) {
  if (span.completeness != Completeness::kComplete)
    return span.reason == Reason::kNone ? Reason::kPartialResponse
                                        : span.reason;
  if (span.checked_utc > now) return Reason::kClock;
  if (now - span.checked_utc >= cfg::kCoverageMaxAgeS) return Reason::kStale;
  return Reason::kNone;
}
uint32_t gapFrom(const Span& span) {
  return span.previous == 0 ? span.from : span.previous;
}
uint32_t gapTo(const Span& span) {
  return span.next == 0 ? span.to : span.next;
}
bool covers(
    const std::array<const Evidence*, cfg::kCoverageMaxSeasons>& evidence,
    uint32_t from, uint32_t to, uint32_t now) {
  // A season proves absence only inside its own remembered neighbour gap.
  // Another season may join adjacent calendar coverage, but cannot hide the
  // unknown remainder of an overlapping season after its next kickoff passes.
  // Partial/stale overlapping seasons are likewise not replaced by a fresh
  // proof for a different season of this competition.
  for (const auto* proof : evidence) {
    if (!proof || !proof->populated) continue;
    const auto& span = proof->span;
    if (span.from == 0 || span.from > span.to) return false;
    const uint32_t first = std::max(from, span.from);
    const uint32_t last = std::min(to, span.to);
    if (first > last) continue;
    if (usable(span, now) != Reason::kNone || first < gapFrom(span) ||
        last > gapTo(span))
      return false;
  }
  uint64_t end = 0;
  bool begun = false;
  // Two passes join at most two spans, regardless of their storage order.
  for (std::size_t pass = 0; pass < cfg::kCoverageMaxSeasons; ++pass)
    for (const auto* proof : evidence) {
      if (!proof || !proof->populated) continue;
      const auto& span = proof->span;
      if (usable(span, now) != Reason::kNone) continue;
      const uint32_t first = gapFrom(span), last = gapTo(span);
      if ((!begun && first <= from && last >= from) ||
          (begun && first <= end + 1 && last >= from)) {
        end = std::max<uint64_t>(end, last);
        begun = true;
      }
    }
  return begun && end >= to;
}
bool same(const budget::Record& a, const budget::Record& b) {
  return a.magic == b.magic && a.day == b.day && a.reserved == b.reserved;
}
bool validArray(const EvidenceArray& evidence) {
  for (std::size_t i = 0; i < evidence.size(); ++i) {
    if (!validate(evidence[i])) return false;
    if (!evidence[i].source.valid()) continue;
    for (std::size_t j = 0; j < i; ++j)
      if (evidence[j].source == evidence[i].source) return false;
  }
  return true;
}
bool sameFamily(const SourceKey& a, const SourceKey& b) {
  return a.valid() && b.valid() && a.provider == b.provider &&
         a.competition == b.competition;
}
bool required(const SourceKey& source,
              std::span<const Requirement> requirements) {
  SourceKey key;
  for (const auto& requirement : requirements) {
    if (!requirement.competition || !requirement.source) continue;
    for (uint8_t season = 0; season < cfg::kCoverageMaxSeasons; ++season)
      if (seasonSource(*requirement.source, season, key) && key == source)
        return true;
  }
  return false;
}
auto proofPair(const EvidenceArray& evidence, const SourceKey& source) {
  std::array<const Evidence*, cfg::kCoverageMaxSeasons> result{};
  SourceKey key;
  for (uint8_t i = 0; i < result.size(); ++i)
    if (seasonSource(source, i, key)) result[i] = find(evidence, key);
  return result;
}
}  // namespace

bool seasonSource(const SourceKey& source, uint8_t index,
                  SourceKey& out) noexcept {
  if (!source.valid() || index >= cfg::kCoverageMaxSeasons) return false;
  if (!index) {
    out = source;
    return true;
  }
  const auto season = source.season.view();
  const auto year = values::positiveUint32(season);
  if (season.size() != 4 || !year || year >= 9999) return false;
  char next[5];
  std::snprintf(next, sizeof(next), "%04u", unsigned(year + 1));
  return out.assign(source.provider, source.competition.view(), next);
}
Evidence* find(EvidenceArray& evidence, const SourceKey& source) noexcept {
  if (!source.valid()) return nullptr;
  for (auto& proof : evidence)
    if (proof.source == source) return &proof;
  return nullptr;
}
const Evidence* find(const EvidenceArray& evidence,
                     const SourceKey& source) noexcept {
  if (!source.valid()) return nullptr;
  for (const auto& proof : evidence)
    if (proof.source == source) return &proof;
  return nullptr;
}
bool synchronize(EvidenceArray& evidence,
                 std::span<const Requirement> requirements) noexcept {
  bool changed = false;
  for (auto& proof : evidence)
    if (proof.source.valid() && !required(proof.source, requirements)) {
      proof = {};
      changed = true;
    }
  SourceKey key;
  for (const auto& requirement : requirements) {
    if (!requirement.competition || !requirement.source) continue;
    for (uint8_t season = 0; season < cfg::kCoverageMaxSeasons; ++season) {
      if (!seasonSource(*requirement.source, season, key) ||
          find(evidence, key))
        continue;
      for (auto& proof : evidence)
        if (!proof.source.valid()) {
          bind(proof, key);
          changed = true;
          break;
        }
    }
  }
  return changed;
}
void bind(Evidence& evidence, const SourceKey& source) noexcept {
  evidence = {};
  if (source.valid()) evidence.source = source;
}
bool matches(const Evidence& evidence, const SourceKey& source) noexcept {
  return source.valid() && evidence.source == source;
}
bool validate(const Evidence& evidence) noexcept {
  if (!evidence.source.valid())
    return evidence.source == SourceKey{} && !evidence.populated &&
           evidence.span == Span{};
  return evidence.populated ? validSpan(evidence.span)
                            : evidence.span == Span{};
}
Reason accept(Evidence& evidence, const SourceKey& source,
              const Span& span) noexcept {
  if (!matches(evidence, source)) return Reason::kIdentity;
  if (!validate(evidence) || !validSpan(span)) return Reason::kRange;
  evidence.span = span;
  evidence.populated = true;
  return Reason::kNone;
}
Reason accept(EvidenceArray& evidence, const SourceKey& source,
              const Span& span) noexcept {
  auto* proof = find(evidence, source);
  if (!proof) return Reason::kIdentity;
  if (span.reason == Reason::kIdentity ||
      span.reason == Reason::kMissingMetadata)
    invalidateFamily(evidence, source, span.reason);
  return accept(*proof, source, span);
}
void invalidate(Evidence& evidence, Reason reason) noexcept {
  if (!evidence.source.valid()) return;
  evidence.populated = true;
  evidence.span.completeness = Completeness::kPartial;
  evidence.span.reason = reason == Reason::kNone ? Reason::kUnknown : reason;
}
void invalidateFamily(EvidenceArray& evidence, const SourceKey& source,
                      Reason reason) noexcept {
  for (auto& proof : evidence)
    if (sameFamily(proof.source, source)) invalidate(proof, reason);
}
bool observeNormal(EvidenceArray& evidence, const SourceKey& source,
                   uint32_t kickoff, bool definite) noexcept {
  Reason why = Reason::kNone;
  for (const auto& proof : evidence) {
    if (!sameFamily(proof.source, source)) continue;
    if (!validate(proof)) {
      why = Reason::kRange;
      break;
    }
    if (!kickoff || !definite) {
      why = Reason::kUndatedFixture;
      break;
    }
    if (!proof.populated || proof.span.completeness != Completeness::kComplete)
      continue;
    const auto& span = proof.span;
    if ((span.previous ? kickoff > span.previous : kickoff >= span.from) &&
        (span.next ? kickoff < span.next : kickoff <= span.to)) {
      why = Reason::kTimingChanged;
      break;
    }
  }
  if (why == Reason::kNone) return false;
  invalidateFamily(evidence, source, why);
  return true;
}

void Collector::begin(const SourceKey& source, uint32_t from, uint32_t to,
                      uint32_t anchor_utc, Proof proof) noexcept {
  source_ = source;
  span_ = {};
  span_.completeness = Completeness::kPartial;
  span_.reason = Reason::kNone;
  span_.proof = proof;
  span_.from = from;
  span_.to = to;
  anchor_ = anchor_utc;
  rows_ = first_ = last_ = 0;
  if (!source.valid()) failed(Reason::kIdentity);
  if (!settings::isValidSeason(source.provider, source.season.view()))
    failed(Reason::kRange);
}

void Collector::bounds(uint32_t from, uint32_t to) noexcept {
  span_.from = from;
  span_.to = to;
}

void Collector::observe(uint32_t kickoff, bool definite) noexcept {
  if (rows_ == kMaximum) {
    failed(Reason::kRange);
    return;
  }
  ++rows_;
  if (kickoff == 0 || !definite) {
    failed(Reason::kUndatedFixture);
    return;
  }
  if (first_ == 0 || kickoff < first_) first_ = kickoff;
  last_ = std::max(last_, kickoff);
  if (kickoff <= anchor_)
    span_.previous = std::max(span_.previous, kickoff);
  else if (span_.next == 0 || kickoff < span_.next)
    span_.next = kickoff;
}

void Collector::failed(Reason reason) noexcept {
  if (span_.reason == Reason::kNone)
    span_.reason = reason == Reason::kNone ? Reason::kUnknown : reason;
  span_.completeness = Completeness::kPartial;
}

const Span& Collector::finish(uint32_t checked_utc,
                              const Integrity& integrity) noexcept {
  span_.checked_utc = checked_utc;
  if (!integrity.response_complete) failed(Reason::kPartialResponse);
  if (!integrity.metadata_verified || !source_.valid() ||
      span_.proof == Proof::kNone)
    failed(Reason::kMissingMetadata);
  if (!integrity.pages_complete ||
      (integrity.count_known && integrity.expected_rows != rows_))
    failed(Reason::kPartialResponse);
  if (anchor_ == 0 || checked_utc < anchor_) failed(Reason::kClock);
  if (span_.from == 0 || span_.from > span_.to ||
      (first_ != 0 && (first_ < span_.from || last_ > span_.to)))
    failed(Reason::kUncovered);
  if (span_.reason == Reason::kNone) {
    span_.completeness = Completeness::kComplete;
    if (!validSpan(span_)) failed(Reason::kRange);
  }
  return span_;
}

Assessment assess(const EvidenceArray& evidence,
                  std::span<const Requirement> requirements, uint32_t now_utc,
                  uint16_t threshold_days) noexcept {
  Assessment out;
  if (!now_utc) {
    out.reason = Reason::kClock;
    return out;
  }
  if (!cfg::kScreenBreakDaysRange.contains(threshold_days) ||
      requirements.size() > cfg::kCompetitionMax || !validArray(evidence)) {
    out.reason = Reason::kRange;
    return out;
  }
  for (const auto& requirement : requirements) {
    if (!requirement.competition) continue;
    out.competition = requirement.competition;
    if (!requirement.source || !requirement.source->valid()) {
      out.reason = Reason::kMissingMetadata;
      return out;
    }
    const auto pair = proofPair(evidence, *requirement.source);
    if (!pair[0]) {
      out.reason = Reason::kIdentity;
      return out;
    }
    bool any = false;
    Reason why = Reason::kMissingMetadata;
    for (const auto* proof : pair) {
      if (!proof || !proof->populated) continue;
      const auto& span = proof->span;
      const auto reason = usable(span, now_utc);
      if (reason != Reason::kNone) {
        why = reason;
        continue;
      }
      any = true;
      for (const auto kickoff : {span.previous, span.next}) {
        if (!kickoff) continue;
        if (kickoff <= now_utc)
          out.previous = std::max(out.previous, kickoff);
        else if (!out.next || kickoff < out.next)
          out.next = kickoff;
      }
    }
    if (!any) {
      out.reason = why;
      return out;
    }
  }
  if (!out.previous || !out.next) {
    out.reason = Reason::kMissingNeighbours;
    return out;
  }
  for (const auto& requirement : requirements) {
    if (!requirement.competition) continue;
    if (!covers(proofPair(evidence, *requirement.source), out.previous,
                out.next, now_utc)) {
      out.competition = requirement.competition;
      out.reason = Reason::kUncovered;
      return out;
    }
  }
  out.competition = 0;
  out.reason = Reason::kNone;
  const uint64_t gap = uint64_t{out.next} - out.previous;
  out.verdict = gap > uint64_t{threshold_days} * timekeeping::kSecondsPerDay
                    ? Verdict::kBreak
                    : Verdict::kNoBreak;
  return out;
}

bool validate(const DiscoveryRecord& record) noexcept {
  if (record.runs.magic != cfg::kBudgetRecordMagic ||
      record.requests.magic != cfg::kBudgetRecordMagic ||
      record.runs.reserved > cfg::kCoverageRunsPerUtcDay ||
      record.requests.reserved > cfg::kCoverageRequestsPerRun)
    return false;
  if (record.runs.day == 0)
    return !record.active && record.runs.reserved == 0 &&
           record.requests.day == 0 && record.requests.reserved == 0;
  return record.runs.reserved == cfg::kCoverageRunsPerUtcDay &&
         record.requests.day == record.runs.day;
}

void Discovery::load(budget::Load state,
                     const DiscoveryRecord& record) noexcept {
  if (state == budget::Load::kValid && !validate(record))
    state = budget::Load::kDamaged;
  stored_ = state == budget::Load::kValid ? record : DiscoveryRecord{};
  runs_.load(state, stored_.runs);
  requests_.load(state, stored_.requests);
  readable_ = state != budget::Load::kDamaged;
}

Discovery::Admission Discovery::start(uint32_t utc_day,
                                      DiscoveryRecord& proposed) noexcept {
  if (stored_.active) return Admission::kActive;
  if (utc_day == 0) return Admission::kUsedUp;
  budget::Record next{};
  if (runs_.admit(utc_day, cfg::kCoverageRunsPerUtcDay, &next) !=
      budget::Counter::Verdict::kStore)
    return Admission::kUsedUp;
  proposed = stored_;
  proposed.runs = next;
  proposed.requests = {cfg::kBudgetRecordMagic, next.day, 0};
  proposed.active = true;
  return Admission::kStore;
}

void Discovery::commitStart(const DiscoveryRecord& stored) noexcept {
  if (!validate(stored) || !stored.active || stored_.active ||
      stored.runs.day <= stored_.runs.day || stored.requests.reserved != 0)
    return;
  runs_.commit(stored.runs);
  requests_.load(budget::Load::kValid, stored.requests);
  stored_ = stored;
  readable_ = true;
}

Discovery::Admission Discovery::request(DiscoveryRecord& proposed) noexcept {
  if (!stored_.active || !readable_) return Admission::kNoRun;
  budget::Record next{};
  switch (
      requests_.admit(stored_.runs.day, cfg::kCoverageRequestsPerRun, &next)) {
    case budget::Counter::Verdict::kAllowed: return Admission::kAllowed;
    case budget::Counter::Verdict::kUsedUp: return Admission::kUsedUp;
    case budget::Counter::Verdict::kStore:
      proposed = stored_;
      proposed.requests = next;
      return Admission::kStore;
  }
  return Admission::kUsedUp;
}

void Discovery::commitRequest(const DiscoveryRecord& stored) noexcept {
  if (!validate(stored) || !stored.active || !stored_.active ||
      !same(stored.runs, stored_.runs) ||
      stored.requests.reserved <= stored_.requests.reserved)
    return;
  requests_.commit(stored.requests);
  stored_ = stored;
}

bool Discovery::finish(DiscoveryRecord& proposed) const noexcept {
  if (!readable_ || !stored_.active) return false;
  proposed = stored_;
  proposed.active = false;
  return true;
}

void Discovery::commitFinish(const DiscoveryRecord& stored) noexcept {
  if (validate(stored) && !stored.active && stored_.active &&
      same(stored.runs, stored_.runs) &&
      same(stored.requests, stored_.requests))
    stored_ = stored;
}

namespace record {
namespace {
uint32_t crc(std::span<const uint8_t> bytes) {
  return settings::crc32(bytes.subspan(kHeaderBytes),
                         settings::crc32(bytes.first(4)));
}
struct Writer : binary::Writer {
  using binary::Writer::Writer;
  void counter(const budget::Record& value) {
    u32(value.magic);
    u32(value.day);
    u32(value.reserved);
  }
};
struct Reader : binary::Reader {
  using binary::Reader::Reader;
  void counter(budget::Record& value) {
    value.magic = u32();
    value.day = u32();
    value.reserved = u32();
  }
};
}  // namespace

std::size_t encodeGuard(const DiscoveryRecord& guard,
                        std::span<uint8_t> out) noexcept {
  if (out.size() < kGuardBytes || !validate(guard)) return 0;
  const auto bytes = out.first(kGuardBytes);
  Writer writer{bytes.subspan(kHeaderBytes)};
  writer.counter(guard.runs);
  writer.counter(guard.requests);
  writer.u8(guard.active ? 1 : 0);
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  binary::writeU16(bytes.data(), kVersion);
  binary::writeU16(bytes.data() + 2, kGuardBytes - kHeaderBytes);
  binary::writeU32(bytes.data() + 4, crc(bytes));
  return writer.complete() ? kGuardBytes : 0;
}

DecodeResult decodeGuard(std::span<const uint8_t> bytes,
                         DiscoveryRecord& guard) noexcept {
  if (bytes.size() != kGuardBytes ||
      binary::readU16(bytes.data() + 2) != kGuardBytes - kHeaderBytes)
    return DecodeResult::kLength;
  if (binary::readU16(bytes.data()) != kVersion) return DecodeResult::kVersion;
  if (binary::readU32(bytes.data() + 4) != crc(bytes))
    return DecodeResult::kChecksum;
  DiscoveryRecord candidate;
  Reader reader{bytes.subspan(kHeaderBytes)};
  reader.counter(candidate.runs);
  reader.counter(candidate.requests);
  const auto active = reader.u8();
  if (active > 1 || reader.u8() || reader.u8() || reader.u8())
    return DecodeResult::kInvalid;
  candidate.active = active != 0;
  if (!reader.complete() || !validate(candidate)) return DecodeResult::kInvalid;
  guard = candidate;
  return DecodeResult::kOk;
}
}  // namespace record
}  // namespace football::coverage
