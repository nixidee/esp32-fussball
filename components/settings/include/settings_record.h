// Stored settings record (docs/CONFIGURATION.md, ADR-017/018): explicit
// little-endian layout, independent of compiler struct layout.
//
// Header (8 bytes): format version (u16), payload length (u16), CRC32 (u32)
// over the first four header bytes and the payload.
// General settings format 11: core network/time fields in Field order,
// then the three application secrets, six flags (ESPN, Demo, night, permanent
// AP badge, badge position, browser diagnostics), four u8 values (language,
// day/night brightness, rotation), five u16 values (scroll reset/repeat,
// night start/end, badge duration), input roles and provider/global plans.
// Then an optional trailing Overview screenshot flag (2026-10-17). A shorter
// format-11 record keeps its initial default; earlier fields never move.
// Text is length u8 + fixed capacity bytes; unused bytes are zero on write.
// The independent screen-policy record owns all screen selection/appearance,
// image lists, visit/page timing and match buffers. Formats 1..10 reset during
// development; decoding never rewrites persistent state.
// Competition format 4: next_id u8, next_incarnation u32, count u8; each entry
// has id u8, incarnation u32, active u8, source, competition/team display
// names. Formats 1..3 reset; no legacy screen mask remains.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "settings_model.h"

namespace settings {

inline constexpr std::size_t kHeaderBytes = 8;

// Stored size of a text (length byte + capacity) and of a competition.
template <typename Text>
inline constexpr std::size_t kTextBytes = 1 + Text::kCapacity;
inline constexpr std::size_t kSourceBytes =
    1 + kTextBytes<decltype(Source::competition)> +
    kTextBytes<decltype(Source::season)> + kTextBytes<decltype(Source::team)>;
inline constexpr std::size_t kCompetitionBytes =
    1 + kSourceBytes + kTextBytes<decltype(Competition::competition_name)> +
    kTextBytes<decltype(Competition::team_name)>;
inline constexpr std::size_t kScalarBytes = 6 + 4 + 5 * 2;
inline constexpr std::size_t kLimitBytes = 3 * 2;
inline constexpr std::size_t kPlanBytes = kLimitBytes + 4 * cfg::kDataTypeCount;
// Complete fixed-size general settings payload.
inline constexpr std::size_t kMaxPayloadBytes =
    kTextBytes<decltype(Model::wifi_ssid)> +
    kTextBytes<decltype(Model::wifi_password)> +
    kTextBytes<decltype(Model::ap_password)> +
    kTextBytes<decltype(Model::hostname)> + 1 + 1 + 2 +
    kTextBytes<decltype(Model::time_zone)> +
    kTextBytes<decltype(Model::ntp_server)> +
    kTextBytes<decltype(Model::admin_password)> +
    kTextBytes<decltype(Model::api_football_key)> +
    kTextBytes<decltype(Model::football_data_key)> + kScalarBytes +
    hw::kMaxInputs + cfg::kProviderCount * kPlanBytes + kLimitBytes + 1;
// Complete record produced by encode().
inline constexpr std::size_t kMaxRecordBytes = kHeaderBytes + kMaxPayloadBytes;
static_assert(kMaxRecordBytes <= cfg::kSettingsMaxRecordBytes);
// Longest competition record (list full).
inline constexpr std::size_t kMaxCompetitionRecordBytes =
    kHeaderBytes + 1 + 4 +
    (1 + cfg::kCompetitionMax * (kCompetitionBytes + 1 + 4));
static_assert(kMaxCompetitionRecordBytes <= cfg::kCompetitionsMaxRecordBytes);

enum class DecodeResult : uint8_t {
  kOk,
  kTooShort,          // smaller than the header
  kTooLong,           // larger than cfg::kSettingsMaxRecordBytes
  kLengthMismatch,    // header length disagrees with the record size
  kChecksumMismatch,  // CRC32 differs
  kOutdatedVersion,   // older format version, no longer read (OQ-253)
  kUnknownVersion,    // other format version
  kPartialField,      // payload ends inside a field
  kInvalidValue,      // a field violates its limits
};

const char* decodeResultText(DecodeResult result) noexcept;

// CRC-32 (IEEE 802.3, zlib convention); chain calls by passing the previous
// result as crc.
uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

// Writes the complete record of the current format version into the start of
// 'out' (the rest is zeroed) and returns its length, at most kMaxRecordBytes.
std::size_t encode(const Model& model,
                   std::span<uint8_t, kMaxRecordBytes> out) noexcept;

// Writes the header for the payload that follows it: version, payload length
// from the record size, CRC32. Used by encode() and to prepare test records.
// Requires kHeaderBytes <= record.size() <= kHeaderBytes + 0xFFFF.
void seal(std::span<uint8_t> record,
          uint16_t version = cfg::kSettingsFormatVersion) noexcept;

// On entry 'model' holds the initial values. Fields missing from a shorter
// record keep them; trailing fields of a longer record are ignored. An older
// format version gives kOutdatedVersion, a newer one kUnknownVersion.
// On any result other than kOk 'model' is unchanged; for kInvalidValue *invalid
// (if given) names the field.
DecodeResult decode(std::span<const uint8_t> record, Model& model,
                    Field* invalid = nullptr,
                    const Limits& limits = {}) noexcept;

// Writes the complete competition record into the start of 'out' (the rest
// is zeroed) and returns its length.
std::size_t encodeCompetitions(
    const Competitions& competitions,
    std::span<uint8_t, kMaxCompetitionRecordBytes> out) noexcept;

// Reads a competition record; on any result other than kOk 'competitions'
// is unchanged. Missing trailing fields keep the defaults of an empty store;
// an older format version gives kOutdatedVersion.
DecodeResult decodeCompetitions(std::span<const uint8_t> record,
                                Competitions& competitions) noexcept;

}  // namespace settings
