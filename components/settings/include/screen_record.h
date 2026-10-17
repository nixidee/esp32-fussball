#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "screen_policy.h"

namespace settings::screen_record {

inline constexpr uint16_t kVersion = cfg::kScreenPolicyFormatVersion;
inline constexpr std::size_t kHeaderBytes = 8;
// Version 1, explicit little endian, independent of Policy's compiler layout:
// Header: version u16, payload length u16, CRC32 u32. CRC covers the first
// four header bytes and the whole payload, using settings::crc32.
// Payload in order:
//   global flags u8 (backgrounds, pin own, frame, frame crest, automatic break)
//   break days, own before/after, other before/after, manual return, per-image:
//     seven u16 values
//   visible matches, table window, conference mode, matchday mode, slide count:
//     five u8 values
//   five image ids u8 (unused slots must be zero), six ordered type ids u8,
//     five phase expansion masks u8
//   eight associations: id u8, service incarnation u32, own-special flag u8
//   six appearances: colour/text/accent u32, text scale/image u8,
//     flags u8 (background, dots)
//   five phases x six shared rules: visit seconds u16, enable/automatic flags
//   u8 five phases x four sporting types x eight competition overrides:
//     visit seconds u16, value flags u8, explicit-field mask u8
// No optional tails, compiler padding or implicit truncation in this version.
inline constexpr std::size_t kPayloadBytes =
    1 + 7 * 2 + 5 + cfg::kSlideMax + screen::kTypeCount +
    screen_policy::kPhaseCount + cfg::kCompetitionMax * 6 +
    screen::kTypeCount * 15 +
    screen_policy::kPhaseCount * screen::kTypeCount * 3 +
    screen_policy::kPhaseCount * screen::kSportTypeCount *
        cfg::kCompetitionMax * 4;
inline constexpr std::size_t kRecordBytes = kHeaderBytes + kPayloadBytes;
static_assert(kRecordBytes <= cfg::kScreenPolicyMaxBytes);

enum class DecodeResult : uint8_t {
  kOk,
  kTooShort,
  kTooLong,
  kLengthMismatch,
  kChecksumMismatch,
  kOutdatedVersion,
  kUnknownVersion,
  kInvalidValue,
};

// Returns kRecordBytes, or zero for an invalid policy/insufficient capacity.
// Failure leaves out unchanged; bytes after the record are untouched.
std::size_t encode(const screen_policy::Policy& policy, std::span<uint8_t> out,
                   screen_policy::Error* error = nullptr,
                   const screen_policy::Limits& limits = {}) noexcept;

// Header, exact length and checksum are checked before touching candidate.
// Semantic failure can leave candidate partially decoded. It is caller-owned
// scratch and MUST NOT be published or saved unless the result is kOk.
// This deliberate contract avoids a hidden policy-sized stack/heap copy; the
// existing settings service owns the committed and candidate snapshots.
DecodeResult decode(std::span<const uint8_t> record,
                    screen_policy::Policy& candidate,
                    screen_policy::Error* error = nullptr,
                    const screen_policy::Limits& limits = {}) noexcept;

}  // namespace settings::screen_record
