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
// Format 8 continues with the secrets, the screen styles, the scalar values,
// the slides (u8 count, then one u8 each), pin_own_match (u8 0/1, since
// format 4), conference_mode (u8), whole_matchday (u8 0/1) and input_roles
// (one u8 per input slot, hw::kMaxInputs slots; all three since format 5),
// then per provider its limit (daily, window requests, window seconds: u16
// each) and one u32 interval per data type, then the global limit (3 u16).
// Format 7 (competitions and fixture pairs after the slides, each a u8 count
// followed by the entries; daily cap and request spacing per provider at the
// end), format 6 (API-Football's daily cap as the last scalar u16, no
// request limits at the end), format 5 (also a crest image u8 before the
// lists), format 4 (also without the format-5 fields), format 3 (also
// without the pin flag), format 2 (lists as fixed slots) and format 1 (core
// only) are converted when read.
//
// Competition record (own NVS key, own format version): next_id (u8), the
// competitions (u8 count; per entry the id u8, the format-3 entry layout and
// the screen bits u8) and the fixture pairs (u8 count; format-3 layout, the
// competition byte holds the competition id).

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
// Screens whose styles follow the secrets (formats 2 to 8); the styles of
// later screens (the matchday screen) and the table step time form the
// format-8 tail after the global limit, which an earlier format-8 record
// lacks (missing fields keep their initial values).
inline constexpr std::size_t kListedScreens = 4;
inline constexpr std::size_t kFormat8TailBytes =
    (cfg::kScreenCount - kListedScreens) * kScreenBytes + 2;
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
    (1 + decltype(Model::slides)::kCapacity) +
    1 +                           // format 4: Konferenz pin flag
    2 + hw::kMaxInputs +          // format 5: mode, whole matchday, input roles
    cfg::kProviderCount * kPlanBytes + kLimitBytes +  // format 8
    2;  // format-8 tail: table step time (its styles are counted above)
// Longest record encode() produces (slide list full); a model with fewer
// slides produces a shorter record.
inline constexpr std::size_t kMaxRecordBytes = kHeaderBytes + kMaxPayloadBytes;
static_assert(kMaxRecordBytes <= cfg::kSettingsMaxRecordBytes);
// Longest competition record (both lists full).
inline constexpr std::size_t kMaxCompetitionRecordBytes =
    kHeaderBytes + 1 +
    (1 + cfg::kCompetitionMax * (kCompetitionBytes + 2)) +
    (1 + cfg::kFixtureMappingMax * kFixturePairBytes);
static_assert(kMaxCompetitionRecordBytes <= cfg::kCompetitionsMaxRecordBytes);

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
// keeps the initial pin_own_match, formats 3 and 4 the initial
// conference_mode, whole_matchday and input_roles, formats 1 to 7 the
// initial request plans except the daily caps (formats 2 to 6 store
// API-Football's, format 7 every provider's; its spacings are dropped,
// OQ-225). Formats 2 to 7 also carry the competitions and fixture pairs:
// they replace *competitions (if given; on entry its initial values) with
// ids 1..n, pairs referring to those ids; they are checked even without a
// target. None is rewritten.
// A stored visible_matches / table_window above the display's maximum
// ('limits') is clamped to it; values outside the general range stay invalid.
// On any result other than kOk 'model' is unchanged; for kInvalidValue *invalid
// (if given) names the field. On kOk *crest_image (if given) is the crest
// image id of a format 3 to 5 record when it is a library id, else 0
// (formats 6 and 7 have no such field).
DecodeResult decode(std::span<const uint8_t> record, Model& model,
                    Field* invalid = nullptr, const Limits& limits = {},
                    uint8_t* crest_image = nullptr,
                    Competitions* competitions = nullptr) noexcept;

// True if 'record' is a settings record of formats 2 to 7, whose decode()
// yields the competitions to migrate into the competition record.
bool carriesCompetitions(std::span<const uint8_t> record) noexcept;

// Writes the complete competition record into the start of 'out' (the rest
// is zeroed) and returns its length.
std::size_t encodeCompetitions(
    const Competitions& competitions,
    std::span<uint8_t, kMaxCompetitionRecordBytes> out) noexcept;

// Reads a competition record; on any result other than kOk 'competitions'
// is unchanged. Missing trailing fields keep the defaults of an empty store.
DecodeResult decodeCompetitions(std::span<const uint8_t> record,
                                Competitions& competitions) noexcept;

}  // namespace settings
