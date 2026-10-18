#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "screen_policy.h"

namespace settings::screen_record {

// Explicit little-endian global display payload. Competition identities, club
// preferences and dormant fieldwise overrides are owned by their competition
// records. One aggregate header/checksum activates the complete configuration.
// Flags u8; seven u16 windows/times; five u8 options/count; fixed slides/order/
// per-competition activation masks; six15-byte appearances; five×six3-byte
// rules.
inline constexpr std::size_t kPayloadBytes =
    1 + 7 * 2 + 5 + cfg::kSlideMax + screen::kTypeCount +
    screen_policy::kPhaseCount + screen::kTypeCount * 15 +
    screen_policy::kPhaseCount * screen::kTypeCount * 3;
enum class DecodeResult : uint8_t {
  kOk,
  kLengthMismatch,
  kInvalidValue,
};

// Payload helpers for the single application-config envelope. No nested
// header/checksum. Decode changes scratch; publish only kOk.
std::size_t encodePayload(const screen_policy::Policy&, std::span<uint8_t>,
                          screen_policy::Error* error = nullptr,
                          const screen_policy::Limits& limits = {}) noexcept;
DecodeResult decodePayload(std::span<const uint8_t>, screen_policy::Policy&,
                           screen_policy::Error* error = nullptr,
                           const screen_policy::Limits& limits = {}) noexcept;

}  // namespace settings::screen_record
