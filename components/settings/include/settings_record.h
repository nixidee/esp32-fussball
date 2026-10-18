// Bare little-endian general-settings and competition payloads inside the
// sole AppConfig record. They have exact lengths and no independent version,
// header, checksum or optional trailing fields. Text is length u8 plus fixed
// capacity bytes; unused bytes are zero on write.

#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "binary_codec.h"
#include "settings_model.h"

namespace settings {

// Stored size of a text (length byte + capacity) and of a competition.
template <typename Text>
inline constexpr std::size_t kTextBytes = 1 + Text::kCapacity;
inline constexpr std::size_t kSourceBytes =
    1 + kTextBytes<decltype(Source::competition)> +
    kTextBytes<decltype(Source::season)>;
inline constexpr std::size_t kCompetitionBytes =
    1 + kSourceBytes + kTextBytes<decltype(Competition::competition_name)> + 4 +
    kTextBytes<decltype(ClubPreference::name)> + 2 +
    screen_policy::kPhaseCount * screen::kSportTypeCount * 4;
inline constexpr std::size_t kScalarBytes = 6 + 4 + 5 * 2;
inline constexpr std::size_t kLimitBytes = 3 * 2;
inline constexpr std::size_t kPlanBytes = kLimitBytes + 4 * cfg::kDataTypeCount;
// Complete fixed-size general settings payload.
inline constexpr std::size_t kModelPayloadBytes =
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
// next_id u8, next_incarnation u32, count u8, followed by complete entries.
inline constexpr std::size_t kMaxCompetitionPayloadBytes =
    6 + cfg::kCompetitionMax * (kCompetitionBytes + 5);

enum class DecodeResult : uint8_t {
  kOk,
  kTooShort,          // smaller than the aggregate's minimum length
  kTooLong,           // larger than the aggregate's maximum length
  kLengthMismatch,    // header length disagrees with the record size
  kChecksumMismatch,  // CRC32 differs
  kUnknownVersion,    // other format version
  kInvalidValue,      // a field violates its limits
};

// CRC-32 (IEEE 802.3, zlib convention); chain calls by passing the previous
// result as crc.
using binary::crc32;

// Decode writes caller-owned scratch; publish only kOk. All fields must be
// present and no trailing bytes are accepted.
std::size_t encodeModelPayload(const Model&, std::span<uint8_t>) noexcept;
DecodeResult decodeModelPayload(std::span<const uint8_t>, Model&,
                                Field* invalid = nullptr,
                                const Limits& limits = {}) noexcept;
std::size_t encodeCompetitionPayload(const Competitions&,
                                     std::span<uint8_t>) noexcept;
DecodeResult decodeCompetitionPayload(std::span<const uint8_t>,
                                      Competitions&) noexcept;

}  // namespace settings
