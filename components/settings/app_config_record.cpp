#include "app_config_record.h"

namespace settings::app_record {
void seal(std::span<uint8_t> bytes, uint16_t version) noexcept {
  binary::writeU16(bytes.data(), version);
  binary::writeU16(bytes.data() + 2,
                   static_cast<uint16_t>(bytes.size() - kHeaderBytes));
  binary::writeU32(bytes.data() + 4,
                   crc32(bytes.subspan(kHeaderBytes), crc32(bytes.first(4))));
}

std::size_t encode(const AppConfig& config, std::span<uint8_t> out,
                   const Limits& limits) noexcept {
  const auto competition_bytes =
      6 + config.competitions.entries.size() * (kCompetitionBytes + 5);
  const auto total = kHeaderBytes + kMetadataBytes + kModelPayloadBytes +
                     competition_bytes + screen_record::kPayloadBytes;
  if (out.size() < total ||
      !settings::validate(config, nullptr, nullptr, limits))
    return 0;
  binary::Writer metadata(out.subspan(kHeaderBytes, kMetadataBytes));
  metadata.u32(config.revision);
  metadata.u64(config.last_apply_id);
  metadata.u8(config.names.slot);
  metadata.u32(config.names.generation);
  metadata.u16(config.names.length);
  metadata.u32(config.names.crc);
  if (!metadata.complete()) return 0;
  std::size_t offset = kHeaderBytes + kMetadataBytes;
  offset += encodeModelPayload(config.model, out.subspan(offset));
  offset += encodeCompetitionPayload(config.competitions, out.subspan(offset));
  offset += screen_record::encodePayload(
      config.screens, out.subspan(offset), nullptr,
      {limits.visible_matches_max, limits.table_window_max});
  if (offset != total) return 0;
  seal(out.first(total));
  return total;
}

DecodeResult decode(std::span<const uint8_t> bytes, AppConfig& candidate,
                    const Limits& limits) noexcept {
  constexpr auto fixed = kHeaderBytes + kMetadataBytes + kModelPayloadBytes +
                         screen_record::kPayloadBytes;
  if (bytes.size() < fixed + 6) return DecodeResult::kTooShort;
  if (bytes.size() > kMaxBytes) return DecodeResult::kTooLong;
  if (binary::readU16(bytes.data()) != cfg::kAppConfigFormatVersion)
    return DecodeResult::kUnknownVersion;
  if (binary::readU16(bytes.data() + 2) != bytes.size() - kHeaderBytes)
    return DecodeResult::kLengthMismatch;
  if (binary::readU32(bytes.data() + 4) !=
      crc32(bytes.subspan(kHeaderBytes), crc32(bytes.first(4))))
    return DecodeResult::kChecksumMismatch;
  constexpr auto competitions_at =
      kHeaderBytes + kMetadataBytes + kModelPayloadBytes;
  const auto count = bytes[competitions_at + 5];
  const auto competition_bytes = 6 + count * (kCompetitionBytes + 5);
  if (count > cfg::kCompetitionMax || bytes.size() != fixed + competition_bytes)
    return DecodeResult::kLengthMismatch;
  binary::Reader metadata(bytes.subspan(kHeaderBytes, kMetadataBytes));
  candidate.revision = metadata.u32();
  candidate.last_apply_id = metadata.u64();
  candidate.names.slot = metadata.u8();
  candidate.names.generation = metadata.u32();
  candidate.names.length = metadata.u16();
  candidate.names.crc = metadata.u32();
  if (!metadata.complete()) return DecodeResult::kLengthMismatch;
  auto result = decodeModelPayload(
      bytes.subspan(kHeaderBytes + kMetadataBytes, kModelPayloadBytes),
      candidate.model, nullptr, limits);
  if (result != DecodeResult::kOk) return result;
  result = decodeCompetitionPayload(
      bytes.subspan(competitions_at, competition_bytes),
      candidate.competitions);
  if (result != DecodeResult::kOk) return result;
  if (screen_record::decodePayload(
          bytes.last(screen_record::kPayloadBytes), candidate.screens, nullptr,
          {limits.visible_matches_max, limits.table_window_max}) !=
      screen_record::DecodeResult::kOk)
    return DecodeResult::kInvalidValue;
  return settings::validate(candidate, nullptr, nullptr, limits)
             ? DecodeResult::kOk
             : DecodeResult::kInvalidValue;
}
}  // namespace settings::app_record
