// Current-only application envelope: one header/CRC, three bare payloads and
// exact names reference/revision/apply correlation. No older-format migration.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "app_config_model.h"
#include "screen_record.h"
#include "settings_record.h"

namespace settings::app_record {
inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kMetadataBytes = 4 + 8 + 1 + 4 + 2 + 4;
inline constexpr std::size_t kMaxBytes =
    kHeaderBytes + kModelPayloadBytes + kMaxCompetitionPayloadBytes +
    screen_record::kPayloadBytes + kMetadataBytes;

// Current aggregate header: version u16, payload length u16 and IEEE CRC32
// over the first four header bytes plus payload. Also used by fault injection.
// Requires kHeaderBytes <= bytes.size() <= kHeaderBytes + UINT16_MAX.
void seal(std::span<uint8_t> bytes,
          uint16_t version = cfg::kAppConfigFormatVersion) noexcept;

std::size_t encode(const AppConfig&, std::span<uint8_t>,
                   const Limits& limits = {}) noexcept;
// Header/length/CRC checked before mutation; candidate is caller-owned scratch.
// Semantic failure may change it. Publish only kOk.
DecodeResult decode(std::span<const uint8_t>, AppConfig& candidate,
                    const Limits& limits = {}) noexcept;
}  // namespace settings::app_record
