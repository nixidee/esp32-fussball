#include "screen_record.h"

#include "binary_codec.h"

namespace settings::screen_record {
namespace {

namespace policy = screen_policy;
constexpr uint8_t kBackgrounds = 1 << 0, kPinOwn = 1 << 1, kFrame = 1 << 2,
                  kFrameCrest = 1 << 3, kBreak = 1 << 4, kOneOwnSingle = 1 << 5;
constexpr uint8_t kGlobalFlags =
    kBackgrounds | kPinOwn | kFrame | kFrameCrest | kBreak | kOneOwnSingle;
constexpr uint8_t kBackground = 1 << 0, kDots = 1 << 1;

using Reader = binary::Reader;
using Writer = binary::Writer;

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
         (value.automatic_break ? kBreak : 0) |
         (value.only_one_own_single ? kOneOwnSingle : 0));
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
  for (const auto mask : value.per_competition) out.u8(mask);
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
  value.only_one_own_single = flags & kOneOwnSingle;
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
  for (auto& mask : value.per_competition) mask = in.u8();
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
  return true;
}

}  // namespace

std::size_t encodePayload(const policy::Policy& value, std::span<uint8_t> out,
                          policy::Error* error,
                          const policy::Limits& limits) noexcept {
  if (error) *error = {};
  if (out.size() < kPayloadBytes || !policy::validate(value, error, limits))
    return 0;
  Writer writer(out.first(kPayloadBytes));
  writePayload(writer, value);
  return writer.complete() ? kPayloadBytes : 0;
}

DecodeResult decodePayload(std::span<const uint8_t> bytes,
                           policy::Policy& candidate, policy::Error* error,
                           const policy::Limits& limits) noexcept {
  if (error) *error = {};
  if (bytes.size() != kPayloadBytes) return DecodeResult::kLengthMismatch;
  Reader reader(bytes);
  return readPayload(reader, candidate, error) && reader.complete() &&
                 policy::validate(candidate, error, limits)
             ? DecodeResult::kOk
             : DecodeResult::kInvalidValue;
}

}  // namespace settings::screen_record
