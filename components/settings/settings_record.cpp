#include "settings_record.h"

#include <algorithm>

namespace settings {

namespace {

constexpr std::size_t kVersionOffset = 0;
constexpr std::size_t kLengthOffset = 2;
constexpr std::size_t kCrcOffset = 4;

uint16_t readU16(const uint8_t* at) {
  return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

uint32_t readU32(const uint8_t* at) {
  return uint32_t{at[0]} | (uint32_t{at[1]} << 8) | (uint32_t{at[2]} << 16) |
         (uint32_t{at[3]} << 24);
}

void writeU16(uint8_t* at, uint16_t value) {
  at[0] = static_cast<uint8_t>(value);
  at[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t* at, uint32_t value) {
  for (int i = 0; i < 4; ++i) at[i] = static_cast<uint8_t>(value >> (8 * i));
}

// CRC over the version/length bytes and the payload (the CRC field itself
// is excluded).
uint32_t recordCrc(std::span<const uint8_t> record) {
  return crc32(record.subspan(kHeaderBytes), crc32(record.first(kCrcOffset)));
}

class Writer {
 public:
  explicit Writer(uint8_t* at) : at_(at) {}

  template <std::size_t N>
  void text(const Bytes<N>& value) {
    *at_++ = value.length;
    std::copy_n(value.data.begin(), value.length, at_);
    at_ += N;
  }
  void flag(bool value) { *at_++ = value ? 1 : 0; }
  void u16(uint16_t value) {
    writeU16(at_, value);
    at_ += 2;
  }
  void u8(uint8_t value) { *at_++ = value; }
  void u32(uint32_t value) {
    writeU32(at_, value);
    at_ += 4;
  }

 private:
  uint8_t* at_;
};

// Reads fields in order. A field is either complete, missing (the payload
// ended before it, so it keeps its initial value) or partial (an error).
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> payload) : payload_(payload) {}

  bool failed() const { return result_ != DecodeResult::kOk; }
  DecodeResult result() const { return result_; }
  Field invalidField() const { return invalid_; }

  template <std::size_t N>
  void text(Field field, Bytes<N>& value) {
    const uint8_t* at = take(1 + N);
    if (at == nullptr) return;
    if (at[0] > N) return reject(field);
    value.assign({reinterpret_cast<const char*>(at + 1), at[0]});
  }
  void flag(Field field, bool& value) {
    const uint8_t* at = take(1);
    if (at == nullptr) return;
    if (at[0] > 1) return reject(field);
    value = at[0] == 1;
  }
  void u16(uint16_t& value) {
    const uint8_t* at = take(2);
    if (at != nullptr) value = readU16(at);
  }
  void u8(uint8_t& value) {
    const auto* at = take(1);
    if (at != nullptr) value = *at;
  }
  void u32(uint32_t& value) {
    const auto* at = take(4);
    if (at != nullptr) value = readU32(at);
  }

 private:
  const uint8_t* take(std::size_t size) {
    if (failed() || pos_ == payload_.size()) return nullptr;
    if (payload_.size() - pos_ < size) {
      result_ = DecodeResult::kPartialField;
      return nullptr;
    }
    const uint8_t* at = payload_.data() + pos_;
    pos_ += size;
    return at;
  }
  void reject(Field field) {
    result_ = DecodeResult::kInvalidValue;
    invalid_ = field;
  }

  std::span<const uint8_t> payload_;
  std::size_t pos_ = 0;
  DecodeResult result_ = DecodeResult::kOk;
  Field invalid_ = Field::kWifiSsid;
};

}  // namespace

const char* decodeResultText(DecodeResult result) noexcept {
  switch (result) {
    case DecodeResult::kOk: return "ok";
    case DecodeResult::kTooShort: return "shorter than its header";
    case DecodeResult::kTooLong: return "longer than the accepted maximum";
    case DecodeResult::kLengthMismatch: return "length field mismatch";
    case DecodeResult::kChecksumMismatch: return "check value mismatch";
    case DecodeResult::kUnknownVersion: return "unknown format version";
    case DecodeResult::kPartialField: return "ends inside a field";
    case DecodeResult::kInvalidValue: return "value out of limits";
  }
  return "unknown";
}

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) noexcept {
  // Bitwise, no table: the record is small and read or written rarely.
  crc = ~crc;
  for (const uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void encode(const Model& model, std::span<uint8_t, kRecordBytes> out) noexcept {
  std::fill(out.begin(), out.end(), 0);
  Writer writer(out.data() + kHeaderBytes);
  writer.text(model.wifi_ssid);
  writer.text(model.wifi_password);
  writer.text(model.ap_password);
  writer.text(model.hostname);
  writer.flag(model.external_antenna);
  writer.flag(model.debug_status_log);
  writer.u16(model.debug_status_interval_s);
  writer.text(model.time_zone);
  writer.text(model.ntp_server);
  writer.text(model.admin_password);
  writer.text(model.api_football_key);
  writer.text(model.football_data_key);
  for (const auto& route : model.routes) {
    writer.flag(route.enabled);
    writer.u8(static_cast<uint8_t>(route.provider));
    writer.text(route.competition);
    writer.text(route.season);
    writer.text(route.team);
  }
  for (const auto& style : model.screens) {
    writer.flag(style.enabled);
    writer.flag(style.background);
    writer.u32(style.colour);
    writer.u32(style.text);
    writer.u32(style.accent);
    writer.u8(style.text_scale);
  }
  writer.flag(model.espn_opt_in);
  writer.flag(model.demo);
  writer.flag(model.backgrounds);
  writer.flag(model.night_enabled);
  writer.flag(model.ip_badge_ap_permanent);
  writer.flag(model.ip_badge_bottom);
  writer.flag(model.browser_debug);
  for (uint8_t value :
       {model.language, model.brightness, model.night_brightness,
        model.rotation, model.visible_matches, model.table_window,
        static_cast<uint8_t>(model.idle_screen),
        static_cast<uint8_t>(model.matchday_screen),
        static_cast<uint8_t>(model.own_match_screen)})
    writer.u8(value);
  for (uint16_t value :
       {model.manual_return_s, model.scroll_reset_s, model.scroll_repeat_ms,
        model.slideshow_s, model.night_start, model.night_end, model.ip_badge_s,
        model.window_before, model.window_after, model.api_daily_budget})
    writer.u16(value);
  for (const auto& route : model.fallback_routes) {
    writer.flag(route.enabled);
    writer.u8(static_cast<uint8_t>(route.provider));
    writer.text(route.competition);
    writer.text(route.season);
    writer.text(route.team);
  }
  for (const auto& mapping : model.fixture_mappings) {
    writer.flag(mapping.enabled);
    writer.u8(mapping.route);
    writer.flag(mapping.swapped);
    writer.text(mapping.primary);
    writer.text(mapping.secondary);
  }
  seal(out);
}

void seal(std::span<uint8_t> record, uint16_t version) noexcept {
  writeU16(record.data() + kVersionOffset, version);
  writeU16(record.data() + kLengthOffset,
           static_cast<uint16_t>(record.size() - kHeaderBytes));
  writeU32(record.data() + kCrcOffset, recordCrc(record));
}

DecodeResult decode(std::span<const uint8_t> record, Model& model,
                    Field* invalid) noexcept {
  if (record.size() < kHeaderBytes) return DecodeResult::kTooShort;
  if (record.size() > cfg::kSettingsMaxRecordBytes)
    return DecodeResult::kTooLong;
  if (readU16(record.data() + kLengthOffset) != record.size() - kHeaderBytes)
    return DecodeResult::kLengthMismatch;
  if (readU32(record.data() + kCrcOffset) != recordCrc(record))
    return DecodeResult::kChecksumMismatch;
  const auto version = readU16(record.data() + kVersionOffset);
  const bool legacy = version == cfg::kSettingsLegacyFormatVersion;
  if (version != cfg::kSettingsFormatVersion && !legacy)
    return DecodeResult::kUnknownVersion;
  if (legacy && record.size() > cfg::kSettingsLegacyMaxRecordBytes)
    return DecodeResult::kTooLong;

  Model candidate = model;
  const auto readable =
      legacy ? std::min(record.size(), cfg::kSettingsLegacyCoreRecordBytes)
             : record.size();
  Reader reader(record.subspan(kHeaderBytes, readable - kHeaderBytes));
  reader.text(Field::kWifiSsid, candidate.wifi_ssid);
  reader.text(Field::kWifiPassword, candidate.wifi_password);
  reader.text(Field::kApPassword, candidate.ap_password);
  reader.text(Field::kHostname, candidate.hostname);
  reader.flag(Field::kExternalAntenna, candidate.external_antenna);
  reader.flag(Field::kDebugStatusLog, candidate.debug_status_log);
  reader.u16(candidate.debug_status_interval_s);
  reader.text(Field::kTimeZone, candidate.time_zone);
  reader.text(Field::kNtpServer, candidate.ntp_server);
  if (!legacy) {
    reader.text(Field::kApplication, candidate.admin_password);
    reader.text(Field::kApplication, candidate.api_football_key);
    reader.text(Field::kApplication, candidate.football_data_key);
    for (auto& route : candidate.routes) {
      reader.flag(Field::kApplication, route.enabled);
      auto value = static_cast<uint8_t>(route.provider);
      reader.u8(value);
      route.provider = static_cast<cfg::Provider>(value);
      reader.text(Field::kApplication, route.competition);
      reader.text(Field::kApplication, route.season);
      reader.text(Field::kApplication, route.team);
    }
    for (auto& style : candidate.screens) {
      reader.flag(Field::kApplication, style.enabled);
      reader.flag(Field::kApplication, style.background);
      reader.u32(style.colour);
      reader.u32(style.text);
      reader.u32(style.accent);
      reader.u8(style.text_scale);
    }
    reader.flag(Field::kApplication, candidate.espn_opt_in);
    reader.flag(Field::kApplication, candidate.demo);
    reader.flag(Field::kApplication, candidate.backgrounds);
    reader.flag(Field::kApplication, candidate.night_enabled);
    reader.flag(Field::kApplication, candidate.ip_badge_ap_permanent);
    reader.flag(Field::kApplication, candidate.ip_badge_bottom);
    reader.flag(Field::kApplication, candidate.browser_debug);
    for (auto* value : {&candidate.language, &candidate.brightness,
                        &candidate.night_brightness, &candidate.rotation,
                        &candidate.visible_matches, &candidate.table_window})
      reader.u8(*value);
    for (auto* screen : {&candidate.idle_screen, &candidate.matchday_screen,
                         &candidate.own_match_screen}) {
      auto value = static_cast<uint8_t>(*screen);
      reader.u8(value);
      *screen = static_cast<cfg::Screen>(value);
    }
    for (auto* value : {&candidate.manual_return_s, &candidate.scroll_reset_s,
                        &candidate.scroll_repeat_ms, &candidate.slideshow_s,
                        &candidate.night_start, &candidate.night_end,
                        &candidate.ip_badge_s, &candidate.window_before,
                        &candidate.window_after, &candidate.api_daily_budget})
      reader.u16(*value);

    for (auto& route : candidate.fallback_routes) {
      reader.flag(Field::kApplication, route.enabled);
      auto provider = static_cast<uint8_t>(route.provider);
      reader.u8(provider);
      route.provider = static_cast<cfg::Provider>(provider);
      reader.text(Field::kApplication, route.competition);
      reader.text(Field::kApplication, route.season);
      reader.text(Field::kApplication, route.team);
    }
    for (auto& mapping : candidate.fixture_mappings) {
      reader.flag(Field::kApplication, mapping.enabled);
      reader.u8(mapping.route);
      reader.flag(Field::kApplication, mapping.swapped);
      reader.text(Field::kApplication, mapping.primary);
      reader.text(Field::kApplication, mapping.secondary);
    }
  }
  DecodeResult result = reader.result();
  Field bad = reader.invalidField();
  if (result == DecodeResult::kOk && !validate(candidate, &bad))
    result = DecodeResult::kInvalidValue;
  if (result != DecodeResult::kOk) {
    if (result == DecodeResult::kInvalidValue && invalid != nullptr)
      *invalid = bad;
    return result;
  }
  model = candidate;
  return DecodeResult::kOk;
}

}  // namespace settings
