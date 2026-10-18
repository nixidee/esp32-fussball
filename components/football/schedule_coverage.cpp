#include "schedule_coverage.h"

#include <algorithm>
#include <limits>

#include "binary_codec.h"
#include "civil_time.h"
#include "settings_record.h"

namespace football::coverage {
namespace {
constexpr uint32_t kMaximum = std::numeric_limits<uint32_t>::max();

bool validReason(Reason reason) { return reason <= Reason::kRange; }
bool validIdentity(const Identity& key) {
  return key.source != 0 && key.incarnation != 0 && key.id != 0 &&
         static_cast<unsigned>(key.provider) < cfg::kProviderCount;
}
bool validSpan(const Span& span, cfg::Provider provider) {
  if (span.season.length > span.season.kCapacity ||
      !settings::isValidSeason(provider, span.season.view()) ||
      span.completeness > Completeness::kComplete ||
      span.proof > Proof::kDayWhitelist || !validReason(span.reason))
    return false;
  if (span.completeness != Completeness::kComplete) return true;
  return !span.season.empty() && span.proof != Proof::kNone &&
         span.reason == Reason::kNone && span.checked_utc != 0 &&
         span.from != 0 && span.from <= span.to &&
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
bool covers(const Evidence& evidence, uint32_t from, uint32_t to,
            uint32_t now) {
  // A season proves absence only inside its own remembered neighbour gap.
  // Another season may join adjacent calendar coverage, but cannot hide the
  // unknown remainder of an overlapping season after its next kickoff passes.
  // Partial/stale overlapping seasons are likewise not replaced by a fresh
  // proof for a different season of this competition.
  for (unsigned i = 0; i < evidence.season_count; ++i) {
    const auto& span = evidence.seasons[i];
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
    for (unsigned i = 0; i < evidence.season_count; ++i) {
      const auto& span = evidence.seasons[i];
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
    if (evidence[i].id == 0) continue;
    for (std::size_t j = 0; j < i; ++j)
      if (evidence[j].id == evidence[i].id ||
          (evidence[j].id != 0 &&
           evidence[j].incarnation == evidence[i].incarnation))
        return false;
  }
  return true;
}
}  // namespace

uint64_t sourceHash(const settings::Source& source) noexcept {
  uint64_t hash = UINT64_C(14695981039346656037);
  const auto byte = [&](uint8_t value) {
    hash = (hash ^ value) * UINT64_C(1099511628211);
  };
  byte(static_cast<uint8_t>(source.provider));
  const auto text = [&](std::string_view value) {
    byte(static_cast<uint8_t>(value.size()));
    for (const unsigned char c : value) byte(c);
  };
  text(source.competition.view());
  text(source.season.view());
  return hash == 0 ? 1 : hash;
}

Identity identity(const settings::CompetitionRoute& competition) noexcept {
  return {sourceHash(competition.primary), competition.incarnation,
          competition.id, competition.primary.provider};
}

void bind(Evidence& evidence, const Identity& source) noexcept {
  evidence.source = source.source;
  evidence.incarnation = source.incarnation;
  evidence.id = source.id;
  evidence.provider = source.provider;
  evidence.season_count = 0;
  evidence.blocked = Reason::kUnknown;
  for (auto& span : evidence.seasons) span = {};
}

bool matches(const Evidence& evidence, const Identity& source) noexcept {
  return evidence.source == source.source &&
         evidence.incarnation == source.incarnation &&
         evidence.id == source.id && evidence.provider == source.provider;
}

bool validate(const Evidence& evidence) noexcept {
  if (!validReason(evidence.blocked) ||
      evidence.season_count > cfg::kCoverageMaxSeasons)
    return false;
  if (evidence.id == 0) {
    if (evidence.source != 0 || evidence.incarnation != 0 ||
        evidence.season_count != 0 ||
        evidence.provider != cfg::kProviderDefault ||
        evidence.blocked != Reason::kUnknown)
      return false;
  } else if (!validIdentity({evidence.source, evidence.incarnation, evidence.id,
                             evidence.provider})) {
    return false;
  }
  for (unsigned i = 0; i < evidence.seasons.size(); ++i) {
    const auto& span = evidence.seasons[i];
    if (i >= evidence.season_count) {
      if (!(span == Span{})) return false;
      continue;
    }
    if (!validSpan(span, evidence.provider)) return false;
    for (unsigned j = 0; j < i; ++j)
      if (evidence.seasons[j].season == span.season) return false;
  }
  return true;
}

Reason accept(Evidence& evidence, const Identity& source,
              const Span& span) noexcept {
  if (!validIdentity(source) || !matches(evidence, source))
    return Reason::kIdentity;
  if (!validate(evidence)) return Reason::kRange;
  if (!validSpan(span, source.provider)) return Reason::kRange;
  if (span.season.empty()) {
    // Missing metadata is a failure of this refresh, not a third anonymous
    // season consuming the bounded slots beside useful diagnostic evidence.
    invalidate(evidence, span.reason == Reason::kNone ? Reason::kMissingMetadata
                                                      : span.reason);
    return Reason::kNone;
  }
  unsigned slot = 0;
  while (slot < evidence.season_count &&
         !(evidence.seasons[slot].season == span.season))
    ++slot;
  if (slot >= cfg::kCoverageMaxSeasons) return Reason::kSeasonLimit;
  evidence.seasons[slot] = span;
  if (slot == evidence.season_count) ++evidence.season_count;
  evidence.blocked = span.completeness == Completeness::kComplete
                         ? Reason::kNone
                     : span.reason == Reason::kNone ? Reason::kPartialResponse
                                                    : span.reason;
  return Reason::kNone;
}

void invalidate(Evidence& evidence, Reason reason) noexcept {
  evidence.blocked = reason == Reason::kNone ? Reason::kUnknown : reason;
  // A later success for another season may clear the global block. It must
  // not re-certify an older proof contradicted by normal data or a failed
  // refresh whose season could not be established. Keep each lost proof
  // partial until that particular season is successfully collected again.
  for (auto& span : evidence.seasons)
    if (span.completeness == Completeness::kComplete) {
      span.completeness = Completeness::kPartial;
      span.reason = evidence.blocked;
    }
}

bool observeNormal(Evidence& evidence, const Identity& source, uint32_t kickoff,
                   bool definite) noexcept {
  Reason why = Reason::kNone;
  if (!validate(evidence)) {
    invalidate(evidence, Reason::kRange);
    return true;
  }
  if (!matches(evidence, source))
    why = Reason::kIdentity;
  else if (kickoff == 0 || !definite)
    why = Reason::kUndatedFixture;
  else
    for (unsigned i = 0; i < evidence.season_count; ++i) {
      const auto& span = evidence.seasons[i];
      if (span.completeness == Completeness::kComplete &&
          (span.previous ? kickoff > span.previous : kickoff >= span.from) &&
          (span.next ? kickoff < span.next : kickoff <= span.to)) {
        why = Reason::kTimingChanged;
        break;
      }
    }
  if (why == Reason::kNone) return false;
  invalidate(evidence, why);
  return true;
}

void Collector::begin(const Identity& source, std::string_view season,
                      uint32_t from, uint32_t to, uint32_t anchor_utc,
                      Proof proof) noexcept {
  source_ = source;
  span_ = {};
  span_.completeness = Completeness::kPartial;
  span_.reason = Reason::kNone;
  span_.proof = proof;
  span_.from = from;
  span_.to = to;
  anchor_ = anchor_utc;
  rows_ = first_ = last_ = 0;
  if (!validIdentity(source)) failed(Reason::kIdentity);
  if (!span_.season.assign(season) ||
      !settings::isValidSeason(source.provider, season))
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
  if (!integrity.metadata_verified || span_.season.empty() ||
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
    if (!validSpan(span_, source_.provider)) failed(Reason::kRange);
  }
  return span_;
}

Assessment assess(const EvidenceArray& evidence,
                  const settings::CompetitionRoutes& competitions,
                  uint32_t now_utc, uint16_t threshold_days) noexcept {
  Assessment out;
  if (now_utc == 0) {
    out.reason = Reason::kClock;
    return out;
  }
  if (!cfg::kScreenBreakDaysRange.contains(threshold_days) ||
      competitions.entries.count > cfg::kCompetitionMax ||
      !validArray(evidence)) {
    out.reason = Reason::kRange;
    return out;
  }
  const auto find =
      [&](const settings::CompetitionRoute& entry) -> const Evidence* {
    for (const auto& row : evidence)
      if (row.id == entry.id) return &row;
    return nullptr;
  };
  for (const auto& entry : competitions.entries) {
    if (!entry.active) continue;
    out.competition = entry.id;
    const auto* row = find(entry);
    if (row == nullptr) {
      out.reason = Reason::kMissingMetadata;
      return out;
    }
    if (!matches(*row, identity(entry))) {
      out.reason = Reason::kIdentity;
      return out;
    }
    if (row->blocked != Reason::kNone) {
      out.reason = row->blocked;
      return out;
    }
    bool any = false;
    Reason why = Reason::kMissingMetadata;
    for (unsigned i = 0; i < row->season_count; ++i) {
      const auto& span = row->seasons[i];
      const auto reason = usable(span, now_utc);
      if (reason != Reason::kNone) {
        why = reason;
        continue;
      }
      any = true;
      for (const auto kickoff : {span.previous, span.next}) {
        if (kickoff == 0) continue;
        if (kickoff <= now_utc)
          out.previous = std::max(out.previous, kickoff);
        else if (out.next == 0 || kickoff < out.next)
          out.next = kickoff;
      }
    }
    if (!any) {
      out.reason = why;
      return out;
    }
  }
  if (out.previous == 0 || out.next == 0) {
    out.reason = Reason::kMissingNeighbours;
    return out;
  }
  for (const auto& entry : competitions.entries) {
    if (!entry.active) continue;
    if (!covers(*find(entry), out.previous, out.next, now_utc)) {
      out.competition = entry.id;
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
