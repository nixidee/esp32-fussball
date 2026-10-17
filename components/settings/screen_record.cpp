#include "screen_record.h"

#include "settings_record.h"  // Shared IEEE CRC32; no separate checksum algorithm.

namespace settings::screen_record {
namespace {

namespace policy = screen_policy;
constexpr uint8_t kBackgrounds = 1 << 0, kPinOwn = 1 << 1, kFrame = 1 << 2,
                  kFrameCrest = 1 << 3, kBreak = 1 << 4;
constexpr uint8_t kGlobalFlags =
    kBackgrounds | kPinOwn | kFrame | kFrameCrest | kBreak;
constexpr uint8_t kBackground = 1 << 0, kDots = 1 << 1;

uint16_t readU16(const uint8_t* at) noexcept {
  return static_cast<uint16_t>(at[0] | (uint16_t{at[1]} << 8));
}
uint32_t readU32(const uint8_t* at) noexcept {
  return uint32_t{at[0]} | (uint32_t{at[1]} << 8) | (uint32_t{at[2]} << 16) |
         (uint32_t{at[3]} << 24);
}
void writeU16(uint8_t* at, uint16_t value) noexcept {
  at[0] = static_cast<uint8_t>(value);
  at[1] = static_cast<uint8_t>(value >> 8);
}
void writeU32(uint8_t* at, uint32_t value) noexcept {
  for (unsigned i = 0; i < 4; ++i)
    at[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t recordCrc(std::span<const uint8_t> record) noexcept {
  return crc32(record.subspan(kHeaderBytes), crc32(record.first(4)));
}

// Payload size is fixed and checked before constructing either cursor.
class Writer {
 public:
  explicit Writer(uint8_t* at) : at_(at) {}
  void u8(uint8_t value) noexcept { *at_++ = value; }
  void u16(uint16_t value) noexcept {
    writeU16(at_, value);
    at_ += 2;
  }
  void u32(uint32_t value) noexcept {
    writeU32(at_, value);
    at_ += 4;
  }

 private:
  uint8_t* at_;
};
class Reader {
 public:
  explicit Reader(const uint8_t* at) : at_(at) {}
  uint8_t u8() noexcept { return *at_++; }
  uint16_t u16() noexcept {
    const auto value = readU16(at_);
    at_ += 2;
    return value;
  }
  uint32_t u32() noexcept {
    const auto value = readU32(at_);
    at_ += 4;
    return value;
  }

 private:
  const uint8_t* at_;
};

bool reject(policy::Error* error, policy::Field field,
            policy::Property property = policy::Property::kValues,
            unsigned phase = 0, unsigned type = 0,
            unsigned competition = 0) noexcept {
  if (error)
    *error = {field,
              policy::Reason::kUnknownBits,
              property,
              static_cast<uint8_t>(phase),
              static_cast<uint8_t>(type),
              static_cast<uint8_t>(competition)};
  return false;
}

void writePayload(Writer& out, const policy::Policy& value) noexcept {
  out.u8((value.backgrounds ? kBackgrounds : 0) |
         (value.pin_own_match ? kPinOwn : 0) | (value.frame ? kFrame : 0) |
         (value.frame_crest ? kFrameCrest : 0) |
         (value.automatic_break ? kBreak : 0));
  for (const auto seconds :
       {value.break_days, value.own_before_min, value.own_after_min,
        value.other_before_min, value.other_after_min, value.manual_return_s,
        value.per_image_s})
    out.u16(seconds);
  out.u8(value.visible_matches);
  out.u8(value.table_window);
  out.u8(static_cast<uint8_t>(value.conference_mode));
  out.u8(static_cast<uint8_t>(value.matchday_mode));
  out.u8(value.slide_count);
  for (const auto id : value.slides) out.u8(id);
  for (const auto type : value.order) out.u8(static_cast<uint8_t>(type));
  for (const auto mask : value.expanded) out.u8(mask);
  for (const auto& entry : value.competitions) {
    out.u8(entry.id);
    out.u32(entry.incarnation);
    out.u8(entry.own_special ? 1 : 0);
  }
  for (const auto& style : value.appearance) {
    out.u32(style.colour);
    out.u32(style.text);
    out.u32(style.accent);
    out.u8(style.text_scale);
    out.u8(style.image);
    out.u8((style.background ? kBackground : 0) | (style.dots ? kDots : 0));
  }
  for (const auto& phase : value.shared)
    for (const auto& rule : phase) {
      out.u16(rule.visit_s);
      out.u8((rule.enabled ? policy::kEnabled : 0) |
             (rule.automatic ? policy::kAutomatic : 0));
    }
  for (const auto& phase : value.overrides)
    for (const auto& type : phase)
      for (const auto& rule : type) {
        out.u16(rule.visit_s);
        out.u8(rule.values);
        out.u8(rule.fields);
      }
}

bool readPayload(Reader& in, policy::Policy& value,
                 policy::Error* error) noexcept {
  const auto flags = in.u8();
  if (flags & ~kGlobalFlags) return reject(error, policy::Field::kNone);
  value.backgrounds = flags & kBackgrounds;
  value.pin_own_match = flags & kPinOwn;
  value.frame = flags & kFrame;
  value.frame_crest = flags & kFrameCrest;
  value.automatic_break = flags & kBreak;
  for (auto* seconds :
       {&value.break_days, &value.own_before_min, &value.own_after_min,
        &value.other_before_min, &value.other_after_min, &value.manual_return_s,
        &value.per_image_s})
    *seconds = in.u16();
  value.visible_matches = in.u8();
  value.table_window = in.u8();
  value.conference_mode = static_cast<cfg::ConferenceMode>(in.u8());
  value.matchday_mode = static_cast<cfg::MatchdayMode>(in.u8());
  value.slide_count = in.u8();
  for (auto& id : value.slides) id = in.u8();
  for (auto& type : value.order) type = static_cast<screen::Type>(in.u8());
  for (auto& mask : value.expanded) mask = in.u8();
  for (unsigned i = 0; i < value.competitions.size(); ++i) {
    auto& entry = value.competitions[i];
    entry.id = in.u8();
    entry.incarnation = in.u32();
    const auto own_special = in.u8();
    if (own_special > 1)
      return reject(error, policy::Field::kCompetition,
                    policy::Property::kValues, 0, 0, i);
    entry.own_special = own_special != 0;
  }
  for (unsigned type = 0; type < value.appearance.size(); ++type) {
    auto& style = value.appearance[type];
    style.colour = in.u32();
    style.text = in.u32();
    style.accent = in.u32();
    style.text_scale = in.u8();
    style.image = in.u8();
    const auto style_flags = in.u8();
    if (style_flags & ~(kBackground | kDots))
      return reject(error, policy::Field::kAppearance,
                    policy::Property::kValues, 0, type);
    style.background = style_flags & kBackground;
    style.dots = style_flags & kDots;
  }
  for (unsigned phase = 0; phase < policy::kPhaseCount; ++phase)
    for (unsigned type = 0; type < screen::kTypeCount; ++type) {
      auto& rule = value.shared[phase][type];
      rule.visit_s = in.u16();
      const auto rule_flags = in.u8();
      if (rule_flags & ~policy::kRuleFlags)
        return reject(error, policy::Field::kShared, policy::Property::kValues,
                      phase, type);
      rule.enabled = rule_flags & policy::kEnabled;
      rule.automatic = rule_flags & policy::kAutomatic;
    }
  for (auto& phase : value.overrides)
    for (auto& type : phase)
      for (auto& rule : type) {
        rule.visit_s = in.u16();
        rule.values = in.u8();
        rule.fields = in.u8();
      }
  return true;
}

}  // namespace

std::size_t encode(const policy::Policy& value, std::span<uint8_t> out,
                   policy::Error* error,
                   const policy::Limits& limits) noexcept {
  if (error) *error = {};
  if (out.size() < kRecordBytes || !policy::validate(value, error, limits))
    return 0;
  out = out.first(kRecordBytes);
  Writer writer(out.data() + kHeaderBytes);
  writePayload(writer, value);
  writeU16(out.data(), kVersion);
  writeU16(out.data() + 2, kPayloadBytes);
  writeU32(out.data() + 4, recordCrc(out));
  return out.size();
}

DecodeResult decode(std::span<const uint8_t> record, policy::Policy& candidate,
                    policy::Error* error,
                    const policy::Limits& limits) noexcept {
  if (error) *error = {};
  if (record.size() < kHeaderBytes) return DecodeResult::kTooShort;
  if (record.size() > kRecordBytes) return DecodeResult::kTooLong;
  const auto version = readU16(record.data());
  if (version < kVersion) return DecodeResult::kOutdatedVersion;
  if (version > kVersion) return DecodeResult::kUnknownVersion;
  if (readU16(record.data() + 2) != kPayloadBytes ||
      record.size() != kRecordBytes)
    return DecodeResult::kLengthMismatch;
  if (recordCrc(record) != readU32(record.data() + 4))
    return DecodeResult::kChecksumMismatch;
  Reader reader(record.data() + kHeaderBytes);
  if (!readPayload(reader, candidate, error) ||
      !policy::validate(candidate, error, limits))
    return DecodeResult::kInvalidValue;
  return DecodeResult::kOk;
}

}  // namespace settings::screen_record
