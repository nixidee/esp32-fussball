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
Identity identity(const settings::CompetitionRoute& competition) noexcept;

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
                  const settings::CompetitionRoutes& competitions,
                  uint32_t now_utc, uint16_t threshold_days) noexcept;

// Operational request credits only. Route/season traversal and source binding
// belong to the worker's RAM and restart from the beginning after reboot.
struct DiscoveryRecord {
  budget::Record runs{cfg::kBudgetRecordMagic, 0, 0};
  budget::Record requests{cfg::kBudgetRecordMagic, 0, 0};
  // The request counter's day is the run's credit lifetime, even at midnight.
  bool active = false;
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
  // Closing/retiring a run forfeits its remaining credits. Persist first;
  // failure leaves it active and prevents a replacement run from starting.
  bool finish(DiscoveryRecord& proposed) const noexcept;
  void commitFinish(const DiscoveryRecord& stored) noexcept;
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
inline constexpr uint16_t kVersion = 3;
inline constexpr std::size_t kHeaderBytes = 8;
// Explicit little-endian discovery request/run guard, independently of RAM
// schedule evidence. CRC covers header version/length plus payload.
inline constexpr std::size_t kGuardBytes = kHeaderBytes + 2 * 12 + 4;
enum class DecodeResult : uint8_t {
  kOk,
  kLength,
  kVersion,
  kChecksum,
  kInvalid,
};
// Only run/request credits persist in the budget NVS namespace.
// Zero for invalid state/insufficient space; failure leaves out untouched.
std::size_t encodeGuard(const DiscoveryRecord& guard,
                        std::span<uint8_t> out) noexcept;
DecodeResult decodeGuard(std::span<const uint8_t> bytes,
                         DiscoveryRecord& guard) noexcept;
}  // namespace record

}  // namespace football::coverage
