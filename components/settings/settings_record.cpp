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
  const uint8_t* position() const { return at_; }

 private:
  uint8_t* at_;
};

// Reads fields in order. A field is either complete, missing (the payload
// ended before it, so it keeps its initial value) or partial (an error). The
// read functions return true when they read a value.
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> payload) : payload_(payload) {}

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
  // Reads a list's entry count (at most 'capacity'). The entries that follow
  // must be complete: a payload ending before them is a partial field, not a
  // missing one. Call endList() after the entries.
  bool count(std::size_t capacity, uint8_t& value) {
    if (!u8(value)) return false;
    if (value > capacity) return reject(Field::kApplication);
    entries_required_ = value > 0;
    return true;
  }
  void endList() { entries_required_ = false; }

 private:
  const uint8_t* take(std::size_t size) {
    if (failed()) return nullptr;
    if (pos_ == payload_.size()) {
      if (entries_required_) result_ = DecodeResult::kPartialField;
      return nullptr;
    }
    if (payload_.size() - pos_ < size) {
      result_ = DecodeResult::kPartialField;
      return nullptr;
    }
    const uint8_t* at = payload_.data() + pos_;
    pos_ += size;
    return at;
  }
  bool reject(Field field) {
    result_ = DecodeResult::kInvalidValue;
    invalid_ = field;
    return false;
  }

  std::span<const uint8_t> payload_;
  std::size_t pos_ = 0;
  DecodeResult result_ = DecodeResult::kOk;
  Field invalid_ = Field::kWifiSsid;
  bool entries_required_ = false;
};

void writeSource(Writer& writer, const Source& source) {
  writer.u8(static_cast<uint8_t>(source.provider));
  writer.text(source.competition);
  writer.text(source.season);
  writer.text(source.team);
}

void readSource(Reader& reader, Source& source) {
  auto provider = static_cast<uint8_t>(source.provider);
  reader.u8(provider);
  source.provider = static_cast<cfg::Provider>(provider);
  reader.text(Field::kApplication, source.competition);
  reader.text(Field::kApplication, source.season);
  reader.text(Field::kApplication, source.team);
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

void readStyle(Reader& reader, ScreenStyle& style) {
  reader.flag(Field::kApplication, style.enabled);
  reader.flag(Field::kApplication, style.background);
  reader.u32(style.colour);
  reader.u32(style.text);
  reader.u32(style.accent);
  reader.u8(style.text_scale);
  reader.u8(style.image);
}

// The styles of the first kListedScreens screens, after the secrets.
void readScreens(Reader& reader, Model& model) {
  for (std::size_t i = 0; i < kListedScreens; ++i)
    readStyle(reader, model.screens[i]);
}

void writeStyle(Writer& writer, const ScreenStyle& style) {
  writer.flag(style.enabled);
  writer.flag(style.background);
  writer.u32(style.colour);
  writer.u32(style.text);
  writer.u32(style.accent);
  writer.u8(style.text_scale);
  writer.u8(style.image);
}

// 7 flags, 9 u8 values, 9 u16 values.
void readScalars(Reader& reader, Model& model) {
  reader.flag(Field::kApplication, model.espn_opt_in);
  reader.flag(Field::kApplication, model.demo);
  reader.flag(Field::kApplication, model.backgrounds);
  reader.flag(Field::kApplication, model.night_enabled);
  reader.flag(Field::kApplication, model.ip_badge_ap_permanent);
  reader.flag(Field::kApplication, model.ip_badge_bottom);
  reader.flag(Field::kApplication, model.browser_debug);
  for (auto* value : {&model.language, &model.brightness,
                      &model.night_brightness, &model.rotation,
                      &model.visible_matches, &model.table_window})
    reader.u8(*value);
  for (auto* screen :
       {&model.idle_screen, &model.matchday_screen, &model.own_match_screen}) {
    auto value = static_cast<uint8_t>(*screen);
    reader.u8(value);
    *screen = static_cast<cfg::Screen>(value);
  }
  for (auto* value :
       {&model.manual_return_s, &model.scroll_reset_s, &model.scroll_repeat_ms,
        &model.slideshow_s, &model.night_start, &model.night_end,
        &model.ip_badge_s, &model.window_before, &model.window_after})
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
  // Later tail: the styles after the listed screens, the table step time,
  // the matchday mode (validate() rejects unknown values).
  for (std::size_t i = kListedScreens; i < cfg::kScreenCount; ++i)
    readStyle(reader, model.screens[i]);
  reader.u16(model.table_page_s);
  uint8_t mode = 0;
  if (reader.u8(mode))
    model.matchday_mode = static_cast<cfg::MatchdayMode>(mode);
}

void writeEntry(Writer& writer, const Competition& entry) {
  writer.flag(entry.active);
  writeSource(writer, entry.primary);
  writer.text(entry.competition_name);
  writer.text(entry.team_name);
}

void readEntry(Reader& reader, Competition& entry) {
  reader.flag(Field::kApplication, entry.active);
  readSource(reader, entry.primary);
  reader.text(Field::kApplication, entry.competition_name);
  reader.text(Field::kApplication, entry.team_name);
}

void writeScalars(Writer& writer, const Model& model) {
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
        model.window_before, model.window_after})
    writer.u16(value);
}

// Konferenz mode, whole-matchday flag and input roles; validate() rejects
// unknown mode and role values.
void readFormat5(Reader& reader, Model& model) {
  uint8_t value = 0;
  if (reader.u8(value))
    model.conference_mode = static_cast<cfg::ConferenceMode>(value);
  reader.flag(Field::kApplication, model.whole_matchday);
  for (auto& role : model.input_roles)
    if (reader.u8(value)) role = static_cast<cfg::InputRole>(value);
}

void readSlides(Reader& reader, Model& model) {
  uint8_t count = 0;
  if (!reader.count(model.slides.kCapacity, count)) return;
  model.slides.clear();
  for (uint8_t i = 0; i < count; ++i) reader.u8(model.slides.items[i]);
  model.slides.count = count;
  reader.endList();
}

}  // namespace

const char* decodeResultText(DecodeResult result) noexcept {
  switch (result) {
    case DecodeResult::kOk: return "ok";
    case DecodeResult::kTooShort: return "shorter than its header";
    case DecodeResult::kTooLong: return "longer than the accepted maximum";
    case DecodeResult::kLengthMismatch: return "length field mismatch";
    case DecodeResult::kChecksumMismatch: return "check value mismatch";
    case DecodeResult::kOutdatedVersion: return "older format version";
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

std::size_t encode(const Model& model,
                   std::span<uint8_t, kMaxRecordBytes> out) noexcept {
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
  for (std::size_t i = 0; i < kListedScreens; ++i)
    writeStyle(writer, model.screens[i]);
  writeScalars(writer, model);
  writer.u8(model.slides.count);
  for (const uint8_t slide : model.slides) writer.u8(slide);
  writer.flag(model.pin_own_match);  // format 4
  writer.u8(static_cast<uint8_t>(model.conference_mode));  // format 5
  writer.flag(model.whole_matchday);
  for (const auto role : model.input_roles)
    writer.u8(static_cast<uint8_t>(role));
  for (const auto& plan : model.providers) {
    writeLimit(writer, plan.limit);
    for (const uint32_t interval : plan.interval_s) writer.u32(interval);
  }
  writeLimit(writer, model.global_limit);
  for (std::size_t i = kListedScreens; i < cfg::kScreenCount; ++i)
    writeStyle(writer, model.screens[i]);
  writer.u16(model.table_page_s);
  writer.u8(static_cast<uint8_t>(model.matchday_mode));
  const auto used = static_cast<std::size_t>(writer.position() - out.data());
  seal(out.first(used));
  return used;
}

void seal(std::span<uint8_t> record, uint16_t version) noexcept {
  writeU16(record.data() + kVersionOffset, version);
  writeU16(record.data() + kLengthOffset,
           static_cast<uint16_t>(record.size() - kHeaderBytes));
  writeU32(record.data() + kCrcOffset, recordCrc(record));
}

namespace {

// Header checks shared by both records.
DecodeResult checkHeader(std::span<const uint8_t> record, std::size_t max) {
  if (record.size() < kHeaderBytes) return DecodeResult::kTooShort;
  if (record.size() > max) return DecodeResult::kTooLong;
  if (readU16(record.data() + kLengthOffset) != record.size() - kHeaderBytes)
    return DecodeResult::kLengthMismatch;
  if (readU32(record.data() + kCrcOffset) != recordCrc(record))
    return DecodeResult::kChecksumMismatch;
  return DecodeResult::kOk;
}

// An older format version (they start at 1) is no longer read (OQ-253);
// any other version is unknown (a newer firmware wrote it).
DecodeResult checkVersion(std::span<const uint8_t> record, uint16_t current) {
  const auto version = readU16(record.data() + kVersionOffset);
  if (version != 0 && version < current) return DecodeResult::kOutdatedVersion;
  if (version != current) return DecodeResult::kUnknownVersion;
  return DecodeResult::kOk;
}

}  // namespace

DecodeResult decode(std::span<const uint8_t> record, Model& model,
                    Field* invalid, const Limits& limits) noexcept {
  if (const auto header = checkHeader(record, cfg::kSettingsMaxRecordBytes);
      header != DecodeResult::kOk)
    return header;
  if (const auto version = checkVersion(record, cfg::kSettingsFormatVersion);
      version != DecodeResult::kOk)
    return version;

  Model candidate = model;
  Reader reader(record.subspan(kHeaderBytes));
  readCore(reader, candidate);
  readSecrets(reader, candidate);
  readScreens(reader, candidate);
  readScalars(reader, candidate);
  readSlides(reader, candidate);
  reader.flag(Field::kApplication, candidate.pin_own_match);
  readFormat5(reader, candidate);
  readRequestPlans(reader, candidate);
  DecodeResult result = reader.result();
  Field bad = reader.invalidField();
  // A record from a display with more room: the rows fit this display.
  if (cfg::kVisibleMatchesRange.contains(candidate.visible_matches))
    candidate.visible_matches =
        std::min(candidate.visible_matches, limits.visible_matches_max);
  if (cfg::kTableWindowRange.contains(candidate.table_window))
    candidate.table_window =
        std::min(candidate.table_window, limits.table_window_max);
  // Earlier firmware allowed the own-match screen while other matches run;
  // such a record keeps everything else and shows the Konferenz (OQ-122).
  if (candidate.matchday_screen == cfg::Screen::kLiveSingle)
    candidate.matchday_screen = cfg::Screen::kLiveMulti;
  if (result == DecodeResult::kOk &&
      !validate(candidate, &bad, nullptr, limits))
    result = DecodeResult::kInvalidValue;
  if (result != DecodeResult::kOk) {
    if (result == DecodeResult::kInvalidValue && invalid != nullptr)
      *invalid = bad;
    return result;
  }
  model = candidate;
  return DecodeResult::kOk;
}

std::size_t encodeCompetitions(
    const Competitions& competitions,
    std::span<uint8_t, kMaxCompetitionRecordBytes> out) noexcept {
  std::fill(out.begin(), out.end(), 0);
  Writer writer(out.data() + kHeaderBytes);
  writer.u8(competitions.next_id);
  writer.u8(competitions.entries.count);
  for (const auto& entry : competitions.entries) {
    writer.u8(entry.id);
    writeEntry(writer, entry);
    writer.u8(entry.screens);
  }
  const auto used = static_cast<std::size_t>(writer.position() - out.data());
  seal(out.first(used), cfg::kCompetitionsFormatVersion);
  return used;
}

DecodeResult decodeCompetitions(std::span<const uint8_t> record,
                                Competitions& competitions) noexcept {
  if (const auto header =
          checkHeader(record, cfg::kCompetitionsMaxRecordBytes);
      header != DecodeResult::kOk)
    return header;
  if (const auto version =
          checkVersion(record, cfg::kCompetitionsFormatVersion);
      version != DecodeResult::kOk)
    return version;
  Competitions candidate;
  Reader reader(record.subspan(kHeaderBytes));
  reader.u8(candidate.next_id);
  uint8_t count = 0;
  if (reader.count(candidate.entries.kCapacity, count)) {
    for (uint8_t i = 0; i < count; ++i) {
      auto& entry = candidate.entries.items[i];
      reader.u8(entry.id);
      readEntry(reader, entry);
      reader.u8(entry.screens);
    }
    candidate.entries.count = count;
    reader.endList();
  }
  DecodeResult result = reader.result();
  if (result == DecodeResult::kOk && !validate(candidate))
    result = DecodeResult::kInvalidValue;
  if (result == DecodeResult::kOk) competitions = candidate;
  return result;
}

}  // namespace settings
