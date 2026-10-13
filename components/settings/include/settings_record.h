// Stored settings record (docs/CONFIGURATION.md, ADR-017/018): explicit
// little-endian layout, independent of compiler struct layout.
//
// Header (8 bytes): format version (u16), payload length (u16), CRC32 (u32)
// over the first four header bytes and the payload.
// Payload core shared by all formats, in Field order (append only):
//   wifi_ssid      u8 length + 32 bytes     wifi_password  u8 length + 64 bytes
//   ap_password    u8 length + 63 bytes     hostname       u8 length + 63 bytes
//   external_antenna u8 (0/1)   debug_status_log u8 (0/1)
//   debug_status_interval_s u16
//   time_zone      u8 length + 32 bytes     ntp_server     u8 length + 63 bytes
// Unused bytes after a text are written as zero and ignored when read.
// Format 3 continues with the secrets, the screen styles, the scalar values,
// the crest image and then three lists, each a u8 count followed by that many
// entries (so the record length varies): slides (u8 each), competitions and
// fixture pairs (settings_record.cpp shows the entry layouts).
// Format 4 appends one flag after the lists: pin_own_match (u8 0/1).
// Format 3 (without that flag), format 2 (lists as fixed slots) and format 1
// (core only) are converted when read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "settings_model.h"

namespace settings {

inline constexpr std::size_t kHeaderBytes = 8;

// Stored size of a text (length byte + capacity) and of the format-3 parts.
template <typename Text>
inline constexpr std::size_t kTextBytes = 1 + Text::kCapacity;
inline constexpr std::size_t kSourceBytes =
    1 + kTextBytes<decltype(Source::competition)> +
    kTextBytes<decltype(Source::season)> + kTextBytes<decltype(Source::team)>;
inline constexpr std::size_t kCompetitionBytes =
    2 + 2 * kSourceBytes + kTextBytes<decltype(Competition::competition_name)> +
    kTextBytes<decltype(Competition::team_name)>;
inline constexpr std::size_t kFixturePairBytes =
    3 + kTextBytes<decltype(FixturePair::primary_id)> +
    kTextBytes<decltype(FixturePair::secondary_id)>;
inline constexpr std::size_t kScreenBytes = 16;  // 2 flags, 3 colours, 2 u8
inline constexpr std::size_t kScalarBytes = 7 + 9 + 20;  // flags, u8, u16
// Longest payload: both lists full.
inline constexpr std::size_t kMaxPayloadBytes =
    kTextBytes<decltype(Model::wifi_ssid)> +
    kTextBytes<decltype(Model::wifi_password)> +
    kTextBytes<decltype(Model::ap_password)> +
    kTextBytes<decltype(Model::hostname)> + 1 + 1 + 2 +
    kTextBytes<decltype(Model::time_zone)> +
    kTextBytes<decltype(Model::ntp_server)> +
    kTextBytes<decltype(Model::admin_password)> +
    kTextBytes<decltype(Model::api_football_key)> +
    kTextBytes<decltype(Model::football_data_key)> +
    cfg::kScreenCount * kScreenBytes + kScalarBytes + 1 +
    (1 + decltype(Model::slides)::kCapacity) +
    (1 + decltype(Model::competitions)::kCapacity * kCompetitionBytes) +
    (1 + decltype(Model::fixture_pairs)::kCapacity * kFixturePairBytes) +
    1;  // format 4: Konferenz pin flag
// Longest record encode() produces (both lists full); a model with shorter
// lists produces a shorter record.
inline constexpr std::size_t kMaxRecordBytes = kHeaderBytes + kMaxPayloadBytes;
static_assert(kMaxRecordBytes <= cfg::kSettingsMaxRecordBytes);

enum class DecodeResult : uint8_t {
  kOk,
  kTooShort,          // smaller than the header
  kTooLong,           // larger than cfg::kSettingsMaxRecordBytes
  kLengthMismatch,    // header length disagrees with the record size
  kChecksumMismatch,  // CRC32 differs
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
// record keep them; trailing fields of a longer record are ignored. Format 1
// loads only its legacy core within the old size bound; format 2 is converted
// to lists in RAM (empty slots disappear, filled paused ones stay); format 3
// keeps the initial pin_own_match. None is rewritten.
// A stored visible_matches / table_window above the display's maximum
// ('limits') is clamped to it; values outside the general range stay invalid.
// On any result other than kOk 'model' is unchanged; for kInvalidValue *invalid
// (if given) names the field.
DecodeResult decode(std::span<const uint8_t> record, Model& model,
                    Field* invalid = nullptr,
                    const Limits& limits = {}) noexcept;

}  // namespace settings
