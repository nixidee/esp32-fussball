#include "settings_record.h"

#include <algorithm>

namespace settings {

namespace {

using binary::readU16;
using binary::readU32;
class Writer : public binary::Writer {
 public:
  using binary::Writer::Writer;
  template <std::size_t N>
  void text(const Bytes<N>& value) {
    auto* at = take(1 + N);
    if (!at) return;
    *at++ = value.length;
    std::copy_n(value.data.begin(), value.length, at);
  }
  void flag(bool value) { u8(value ? 1 : 0); }
};

// Exact payload reader; a missing or partial field is always an error.
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> payload) : cursor_(payload) {}

  bool failed() const { return result_ != DecodeResult::kOk; }
  DecodeResult result() const { return result_; }
  Field invalidField() const { return invalid_; }

  template <std::size_t N>
  bool text(Field field, Bytes<N>& value) {
    const uint8_t* at = take(1 + N);
    if (at == nullptr) return false;
    if (at[0] > N) return reject(field);
    value.assign({reinterpret_cast<const char*>(at + 1), at[0]});
    return true;
  }
  bool flag(Field field, bool& value) {
    const uint8_t* at = take(1);
    if (at == nullptr) return false;
    if (at[0] > 1) return reject(field);
    value = at[0] == 1;
    return true;
  }
  bool u16(uint16_t& value) {
    const uint8_t* at = take(2);
    if (at == nullptr) return false;
    value = readU16(at);
    return true;
  }
  bool u8(uint8_t& value) {
    const auto* at = take(1);
    if (at == nullptr) return false;
    value = *at;
    return true;
  }
  bool u32(uint32_t& value) {
    const auto* at = take(4);
    if (at == nullptr) return false;
    value = readU32(at);
    return true;
  }
  bool complete() const { return !failed() && cursor_.remaining() == 0; }

 private:
  const uint8_t* take(std::size_t size) {
    if (failed()) return nullptr;
    if (cursor_.remaining() < size) {
      result_ = DecodeResult::kLengthMismatch;
      return nullptr;
    }
    return cursor_.take(size);
  }
  bool reject(Field field) {
    result_ = DecodeResult::kInvalidValue;
    invalid_ = field;
    return false;
  }

  binary::Cursor<const uint8_t> cursor_;
  DecodeResult result_ = DecodeResult::kOk;
  Field invalid_ = Field::kWifiSsid;
};

void writeSource(Writer& writer, const Source& source) {
  writer.u8(static_cast<uint8_t>(source.provider));
  writer.text(source.competition);
  writer.text(source.season);
}

void readSource(Reader& reader, Source& source) {
  auto provider = static_cast<uint8_t>(source.provider);
  reader.u8(provider);
  source.provider = static_cast<cfg::Provider>(provider);
  reader.text(Field::kApplication, source.competition);
  reader.text(Field::kApplication, source.season);
}

void readCore(Reader& reader, Model& model) {
  reader.text(Field::kWifiSsid, model.wifi_ssid);
  reader.text(Field::kWifiPassword, model.wifi_password);
  reader.text(Field::kApPassword, model.ap_password);
  reader.text(Field::kHostname, model.hostname);
  reader.flag(Field::kExternalAntenna, model.external_antenna);
  reader.flag(Field::kDebugStatusLog, model.debug_status_log);
  reader.u16(model.debug_status_interval_s);
  reader.text(Field::kTimeZone, model.time_zone);
  reader.text(Field::kNtpServer, model.ntp_server);
}

void readSecrets(Reader& reader, Model& model) {
  reader.text(Field::kApplication, model.admin_password);
  reader.text(Field::kApplication, model.api_football_key);
  reader.text(Field::kApplication, model.football_data_key);
}

// General application fields; screen policy follows in the same AppConfig.
void readScalars(Reader& reader, Model& model) {
  for (auto* value : {&model.espn_opt_in, &model.demo, &model.night_enabled,
                      &model.ip_badge_ap_permanent, &model.ip_badge_bottom,
                      &model.browser_debug})
    reader.flag(Field::kApplication, *value);
  for (auto* value : {&model.language, &model.brightness,
                      &model.night_brightness, &model.rotation})
    reader.u8(*value);
  for (auto* value : {&model.scroll_reset_s, &model.scroll_repeat_ms,
                      &model.night_start, &model.night_end, &model.ip_badge_s})
    reader.u16(*value);
}

void writeLimit(Writer& writer, const RequestLimit& limit) {
  writer.u16(limit.daily);
  writer.u16(limit.window_requests);
  writer.u16(limit.window_s);
}

void readLimit(Reader& reader, RequestLimit& limit) {
  reader.u16(limit.daily);
  reader.u16(limit.window_requests);
  reader.u16(limit.window_s);
}

// The plan per provider, then the global limit.
void readRequestPlans(Reader& reader, Model& model) {
  for (auto& plan : model.providers) {
    readLimit(reader, plan.limit);
    for (auto& interval : plan.interval_s) reader.u32(interval);
  }
  readLimit(reader, model.global_limit);
}

void writeEntry(Writer& writer, const Competition& entry) {
  writer.flag(entry.active);
  writeSource(writer, entry.primary);
  writer.text(entry.competition_name);
  writer.u32(entry.club.team);
  writer.text(entry.club.name);
  writer.flag(entry.club.own_special);
  writer.u8(entry.club.group);
  for (const auto& phase : entry.display.overrides)
    for (const auto& rule : phase) {
      writer.u16(rule.visit_s);
      writer.u8(rule.values);
      writer.u8(rule.fields);
    }
}

void readEntry(Reader& reader, Competition& entry) {
  reader.flag(Field::kApplication, entry.active);
  readSource(reader, entry.primary);
  reader.text(Field::kApplication, entry.competition_name);
  reader.u32(entry.club.team);
  reader.text(Field::kApplication, entry.club.name);
  reader.flag(Field::kApplication, entry.club.own_special);
  reader.u8(entry.club.group);
  for (auto& phase : entry.display.overrides)
    for (auto& rule : phase) {
      reader.u16(rule.visit_s);
      reader.u8(rule.values);
      reader.u8(rule.fields);
    }
}

void writeScalars(Writer& writer, const Model& model) {
  for (const bool value : {model.espn_opt_in, model.demo, model.night_enabled,
                           model.ip_badge_ap_permanent, model.ip_badge_bottom,
                           model.browser_debug})
    writer.flag(value);
  for (const uint8_t value : {model.language, model.brightness,
                              model.night_brightness, model.rotation})
    writer.u8(value);
  for (const uint16_t value :
       {model.scroll_reset_s, model.scroll_repeat_ms, model.night_start,
        model.night_end, model.ip_badge_s})
    writer.u16(value);
}

void readRoles(Reader& reader, Model& model) {
  uint8_t value = 0;
  for (auto& role : model.input_roles)
    if (reader.u8(value)) role = static_cast<cfg::InputRole>(value);
}

}  // namespace

std::size_t encodeModelPayload(const Model& model,
                               std::span<uint8_t> out) noexcept {
  if (out.size() < kModelPayloadBytes) return 0;
  out = out.first(kModelPayloadBytes);
  std::fill(out.begin(), out.end(), 0);
  Writer writer(out);
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
  writeScalars(writer, model);
  for (const auto role : model.input_roles)
    writer.u8(static_cast<uint8_t>(role));
  for (const auto& plan : model.providers) {
    writeLimit(writer, plan.limit);
    for (const uint32_t interval : plan.interval_s) writer.u32(interval);
  }
  writeLimit(writer, model.global_limit);
  writer.flag(model.overview_screenshot);
  return writer.good() ? writer.position() : 0;
}

std::size_t encodeCompetitionPayload(const Competitions& competitions,
                                     std::span<uint8_t> out) noexcept {
  const auto required =
      6 + competitions.entries.size() * (kCompetitionBytes + 5);
  if (out.size() < required) return 0;
  out = out.first(required);
  std::fill(out.begin(), out.end(), 0);
  Writer writer(out);
  writer.u8(competitions.next_id);
  writer.u32(competitions.next_incarnation);
  writer.u8(competitions.entries.count);
  for (const auto& entry : competitions.entries) {
    writer.u8(entry.id);
    writer.u32(entry.incarnation);
    writeEntry(writer, entry);
  }
  return writer.good() ? writer.position() : 0;
}

DecodeResult decodeModelPayload(std::span<const uint8_t> payload,
                                Model& candidate, Field* invalid,
                                const Limits& limits) noexcept {
  if (payload.size() != kModelPayloadBytes)
    return DecodeResult::kLengthMismatch;
  Reader reader(payload);
  readCore(reader, candidate);
  readSecrets(reader, candidate);
  readScalars(reader, candidate);
  readRoles(reader, candidate);
  readRequestPlans(reader, candidate);
  reader.flag(Field::kApplication, candidate.overview_screenshot);
  if (!reader.complete()) {
    if (invalid) *invalid = reader.invalidField();
    return reader.failed() ? reader.result() : DecodeResult::kLengthMismatch;
  }
  return validate(candidate, invalid, nullptr, limits)
             ? DecodeResult::kOk
             : DecodeResult::kInvalidValue;
}

DecodeResult decodeCompetitionPayload(std::span<const uint8_t> payload,
                                      Competitions& candidate) noexcept {
  if (payload.size() < 6) return DecodeResult::kLengthMismatch;
  const auto count = payload[5];
  if (count > cfg::kCompetitionMax ||
      payload.size() != 6 + count * (kCompetitionBytes + 5))
    return DecodeResult::kLengthMismatch;
  candidate.clear();
  Reader reader(payload);
  reader.u8(candidate.next_id);
  reader.u32(candidate.next_incarnation);
  uint8_t read_count = 0;
  if (!reader.u8(read_count)) return reader.result();
  for (uint8_t i = 0; i < read_count; ++i) {
    auto& entry = candidate.entries.items[i];
    reader.u8(entry.id);
    reader.u32(entry.incarnation);
    readEntry(reader, entry);
  }
  candidate.entries.count = read_count;
  if (!reader.complete())
    return reader.failed() ? reader.result() : DecodeResult::kLengthMismatch;
  return validate(candidate) ? DecodeResult::kOk : DecodeResult::kInvalidValue;
}

}  // namespace settings
