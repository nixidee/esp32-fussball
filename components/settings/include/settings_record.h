// Stored settings record (docs/CONFIGURATION.md, ADR-017/018): explicit
// little-endian layout, independent of compiler struct layout.
//
// Header (8 bytes): format version (u16), payload length (u16), CRC32 (u32)
// over the first four header bytes and the payload.
// Payload, in Field order (append only):
//   wifi_ssid      u8 length + 32 bytes     wifi_password  u8 length + 64 bytes
//   ap_password    u8 length + 63 bytes     hostname       u8 length + 63 bytes
//   external_antenna u8 (0/1)   debug_status_log u8 (0/1)
//   debug_status_interval_s u16
//   time_zone      u8 length + 32 bytes     ntp_server     u8 length + 63 bytes
// Unused bytes after a text are written as zero and ignored when read.
// It continues with the secrets, the screen styles, the scalar values, the
// slides (u8 count, then one u8 each), pin_own_match (u8 0/1),
// conference_mode (u8), whole_matchday (u8 0/1) and input_roles (one u8 per
// input slot, hw::kMaxInputs slots), then per provider its limit (daily,
// window requests, window seconds: u16 each) and one u32 interval per data
// type (format 9: fixtures, table, live, live table, events), then the
// global limit (3 u16), then the later tail (the styles of the screens after
// the listed ones, the table step time u16, the matchday mode u8). Records of
// formats 1 to 8 are not read (OQ-253, OQ-257).
//
// Competition record (own NVS key, own format version): next_id (u8) and the
// competitions (u8 count; per entry the id u8, the active flag u8, the
// source (provider u8, competition, season, team), the two display names
// and the screen bits u8). Format 1 (with a fallback source per entry and
// fixture pairs) is not read (OQ-253).

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
inline constexpr std::size_t kScreenBytes = 16;  // 2 flags, 3 colours, 2 u8
// Screens whose styles follow the secrets; the styles of
// later screens (the matchday screen) and the table step time form the
// later tail after the global limit (missing trailing fields keep their
// initial values).
inline constexpr std::size_t kListedScreens = 4;
inline constexpr std::size_t kScalarBytes = 7 + 9 + 18;  // flags, u8, u16
inline constexpr std::size_t kLimitBytes = 3 * 2;
inline constexpr std::size_t kPlanBytes = kLimitBytes + 4 * cfg::kDataTypeCount;
// Longest payload: the slide list full.
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
    cfg::kScreenCount * kScreenBytes + kScalarBytes +
    (1 + decltype(Model::slides)::kCapacity) + 1 +  // Konferenz pin flag
    2 + hw::kMaxInputs +  // mode, whole matchday, input roles
    cfg::kProviderCount * kPlanBytes + kLimitBytes +  // request plans
    2 + 1;  // later tail: table step time, matchday mode (styles above)
// Longest record encode() produces (slide list full); a model with fewer
// slides produces a shorter record.
inline constexpr std::size_t kMaxRecordBytes = kHeaderBytes + kMaxPayloadBytes;
static_assert(kMaxRecordBytes <= cfg::kSettingsMaxRecordBytes);
// Longest competition record (list full).
inline constexpr std::size_t kMaxCompetitionRecordBytes =
    kHeaderBytes + 1 + (1 + cfg::kCompetitionMax * (kCompetitionBytes + 2));
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
// A stored visible_matches / table_window above the display's maximum
// ('limits') is clamped to it; values outside the general range stay invalid.
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
