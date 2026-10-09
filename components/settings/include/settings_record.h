// Stored settings record (docs/CONFIGURATION.md, ADR-017/018): explicit
// little-endian layout, independent of compiler struct layout.
//
// Header (8 bytes): format version (u16), payload length (u16), CRC32 (u32)
// over the first four header bytes and the payload.
// Payload, format 1, in Field order (append only):
//   wifi_ssid      u8 length + 32 bytes     wifi_password  u8 length + 64 bytes
//   ap_password    u8 length + 63 bytes     hostname       u8 length + 63 bytes
//   external_antenna u8 (0/1)   debug_status_log u8 (0/1)
//   debug_status_interval_s u16
// Unused bytes after a text are written as zero and ignored when read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "settings_model.h"

namespace settings {

inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kPayloadBytes =
    (1 + decltype(Model::wifi_ssid)::kCapacity) +
    (1 + decltype(Model::wifi_password)::kCapacity) +
    (1 + decltype(Model::ap_password)::kCapacity) +
    (1 + decltype(Model::hostname)::kCapacity) + 1 + 1 + 2;
inline constexpr std::size_t kRecordBytes = kHeaderBytes + kPayloadBytes;
static_assert(kRecordBytes <= cfg::kSettingsMaxRecordBytes);

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

// Writes the complete record of the current format version.
void encode(const Model& model, std::span<uint8_t, kRecordBytes> out) noexcept;

// Writes the header for the payload that follows it: version, payload length
// from the record size, CRC32. Used by encode() and to prepare test records.
// Requires kHeaderBytes <= record.size() <= kHeaderBytes + 0xFFFF.
void seal(std::span<uint8_t> record,
          uint16_t version = cfg::kSettingsFormatVersion) noexcept;

// On entry 'model' holds the initial values. Fields missing from a shorter
// record keep them; trailing fields of a longer record are ignored. On any
// result other than kOk 'model' is unchanged; for kInvalidValue *invalid (if
// given) names the field.
DecodeResult decode(std::span<const uint8_t> record, Model& model,
                    Field* invalid = nullptr) noexcept;

}  // namespace settings
