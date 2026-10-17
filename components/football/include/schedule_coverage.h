#pragma once

// Compact evidence of a fixture-free interval. This pure module neither
// fetches providers nor stores files: acquisition must prove its metadata
// through the existing bounded parser before publishing a Complete span.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "budget_ledger.h"
#include "settings_model.h"

namespace football::coverage {

enum class Completeness : uint8_t { kUnknown, kPartial, kComplete };
enum class Proof : uint8_t { kNone, kFullSeason, kDateRange, kDayWhitelist };
enum class Reason : uint8_t {
  kNone,
  kUnknown,
  kIdentity,
  kMissingMetadata,
  kPartialResponse,
  kBodyLimit,
  kRequestLimit,
  kProviderLimit,
  kUndatedFixture,
  kStale,
  kClock,
  kUncovered,
  kMissingNeighbours,
  kTimingChanged,
  kCorrupt,
  kSeasonLimit,
  kRange,
};

struct Identity {
  uint64_t source = 0;
  uint32_t incarnation = 0;
  uint8_t id = 0;
  cfg::Provider provider = cfg::kProviderDefault;
  bool operator==(const Identity&) const noexcept = default;
};
// Hashes every source field with explicit separators/lengths. This is a
// 64-bit change detector, not a cryptographic authentication claim.
uint64_t sourceHash(const settings::Source& source) noexcept;
Identity identity(const settings::Competition& competition) noexcept;

struct Span {
  settings::Bytes<cfg::kSeasonMaxBytes> season;
  Completeness completeness = Completeness::kUnknown;
  Proof proof = Proof::kNone;
  Reason reason = Reason::kMissingMetadata;
  // Closed UTC-second interval actually proved by the provider response.
  // Calendar/day markers may establish bounds, never previous/next kickoffs.
  uint32_t from = 0, to = 0, checked_utc = 0;
  // Actual provider kickoffs around the collector's anchor. Zero means none
  // in the proved interval, not an unbounded absence outside that interval.
  uint32_t previous = 0, next = 0;
  bool operator==(const Span&) const noexcept = default;
};

struct Evidence {
  // Flat header avoids padding between Identity and the fixed two spans.
  uint64_t source = 0;
  uint32_t incarnation = 0;
  uint8_t id = 0;
  cfg::Provider provider = cfg::kProviderDefault;
  uint8_t season_count = 0;
  Reason blocked = Reason::kUnknown;
  std::array<Span, cfg::kCoverageMaxSeasons> seasons{};
};
using EvidenceArray = std::array<Evidence, cfg::kCompetitionMax>;

void bind(Evidence& evidence, const Identity& source) noexcept;
bool matches(const Evidence& evidence, const Identity& source) noexcept;
bool validate(const Evidence& evidence) noexcept;
// Replaces the named season or adds a second one. A third season or wrong
// identity is rejected without modifying evidence. Partial results block the
// old proof; a failed refresh cannot silently keep claiming a break.
Reason accept(Evidence& evidence, const Identity& source,
              const Span& span) noexcept;
// A global failure has no reliable season provenance: all complete spans
// become partial. Another season's success cannot re-certify those spans.
void invalidate(Evidence& evidence, Reason reason) noexcept;
// Normal successful data may contradict a proved gap; it never refreshes a
// whole season's checked_utc. Unknown/TBD timing invalidates the old proof.
bool observeNormal(Evidence& evidence, const Identity& source, uint32_t kickoff,
                   bool definite = true) noexcept;

struct Integrity {
  uint32_t expected_rows = 0;
  bool count_known = false;
  bool response_complete = false;
  bool metadata_verified = false;
  bool pages_complete = false;
};

class Collector {
 public:
  void begin(const Identity& source, std::string_view season, uint32_t from,
             uint32_t to, uint32_t anchor_utc, Proof proof) noexcept;
  // Bounds may arrive after streamed rows, e.g. verified wrapper metadata.
  void bounds(uint32_t from, uint32_t to) noexcept;
  void observe(uint32_t kickoff, bool definite = true) noexcept;
  void failed(Reason reason) noexcept;
  // No hidden copy: returned Span belongs to this collector. Publish only
  // after checking its result and the caller's unchanged source identity.
  const Span& finish(uint32_t checked_utc, const Integrity& integrity) noexcept;
  const Identity& source() const noexcept { return source_; }
  uint32_t rows() const noexcept { return rows_; }

 private:
  Identity source_{};
  Span span_{};
  uint32_t anchor_ = 0, rows_ = 0, first_ = 0, last_ = 0;
};

enum class Verdict : uint8_t { kUnknown, kNoBreak, kBreak };
struct Assessment {
  Verdict verdict = Verdict::kUnknown;
  Reason reason = Reason::kUnknown;
  uint8_t competition = 0;
  uint32_t previous = 0, next = 0;
};
// Every active competition counts, with or without a club/screen override.
// No MatchStore argument: eviction or its 224-row capacity cannot prove an
// empty schedule. Gap is strictly greater than days*24h, independent of DST.
Assessment assess(const EvidenceArray& evidence,
                  const settings::Competitions& competitions, uint32_t now_utc,
                  uint16_t threshold_days) noexcept;

struct Progress {
  uint32_t run_day = 0;
  bool active = false;
  uint8_t route = 0, season = 0, step = 0;
};
struct DiscoveryRecord {
  budget::Record runs{cfg::kBudgetRecordMagic, 0, 0};
  budget::Record requests{cfg::kBudgetRecordMagic, 0, 0};
  Progress progress{};
  // Full configured route/order/access fingerprint. A cursor may only resume
  // against its original inputs; the evidence file uses a current snapshot.
  uint64_t binding = 0;
};
bool validate(const DiscoveryRecord& record) noexcept;

class Discovery {
 public:
  enum class Admission : uint8_t { kAllowed, kStore, kUsedUp, kNoRun, kActive };
  void load(budget::Load state, const DiscoveryRecord& record = {}) noexcept;
  // kStore => persist the complete proposed record FIRST, then commitStart.
  // A failed write leaves admission closed; don't call the HTTP transport.
  Admission start(uint32_t utc_day, DiscoveryRecord& proposed) noexcept;
  void commitStart(const DiscoveryRecord& stored) noexcept;
  // The cap belongs to the device-wide RUN, not a route or current UTC day.
  // A run crossing midnight cannot acquire another set of 16 requests.
  // kAllowed is already durably reserved; kStore requires commitRequest.
  Admission request(DiscoveryRecord& proposed) noexcept;
  void commitRequest(const DiscoveryRecord& stored) noexcept;
  bool stage(const Progress& progress,
             DiscoveryRecord& proposed) const noexcept;
  void commitProgress(const DiscoveryRecord& stored) noexcept;
  const DiscoveryRecord& stored() const noexcept { return stored_; }
  // A corrupt guard must never be overwritten by a fresh empty one merely
  // because unrelated normal fixtures were saved. Recovery starts a new run
  // no earlier than the next observed UTC day, like the existing budget.
  bool canPersist() const noexcept { return readable_; }

 private:
  budget::Counter runs_, requests_;
  DiscoveryRecord stored_{};
  bool readable_ = true;
};

namespace record {
inline constexpr uint16_t kVersion = 2;
inline constexpr std::size_t kHeaderBytes = 8;
// Explicit little endian: each evidence header is source u64, incarnation
// u32, id/provider/season_count/blocked u8. Each span is season length+16
// bytes, three enum bytes, and five u32s. Then two budget records (3*u32)
// and progress (run_day u32, active/route/season/step u8). CRC covers header
// version/length plus payload. No compiler padding is stored.
inline constexpr std::size_t kSpanBytes = cfg::kSeasonMaxBytes + 1 + 3 + 5 * 4;
inline constexpr std::size_t kEvidenceBytes =
    16 + cfg::kCoverageMaxSeasons * kSpanBytes;
inline constexpr std::size_t kRecordBytes =
    kHeaderBytes + cfg::kCompetitionMax * kEvidenceBytes + 2 * 12 + 8 + 8;
inline constexpr std::size_t kGuardBytes = kHeaderBytes + 2 * 12 + 8 + 8;
static_assert(kRecordBytes <= 1024);
enum class DecodeResult : uint8_t {
  kOk,
  kLength,
  kVersion,
  kChecksum,
  kInvalid,
};
// Zero for invalid state/insufficient space; failure leaves out untouched.
std::size_t encode(const EvidenceArray& evidence, const DiscoveryRecord& guard,
                   std::span<uint8_t> out,
                   uint8_t pending_competition = 0) noexcept;
// Caller-owned candidates may change after a valid header/CRC. No large
// snapshot temporary: publish neither candidate unless the result is kOk.
DecodeResult decode(std::span<const uint8_t> bytes, EvidenceArray& evidence,
                    DiscoveryRecord& guard) noexcept;
// The authoritative device-wide discovery guard lives in the existing budget
// NVS namespace, so a formatted/missing evidence file cannot reopen a run.
// The guard in the evidence file is a non-authoritative transaction snapshot.
std::size_t encodeGuard(const DiscoveryRecord& guard,
                        std::span<uint8_t> out) noexcept;
DecodeResult decodeGuard(std::span<const uint8_t> bytes,
                         DiscoveryRecord& guard) noexcept;
}  // namespace record

}  // namespace football::coverage
