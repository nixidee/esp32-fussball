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

// Fixed parts shared by format 3 and, where noted, format 2.
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

// Screen styles; format 2 has no image id.
void readScreens(Reader& reader, Model& model, bool with_image) {
  for (auto& style : model.screens) {
    reader.flag(Field::kApplication, style.enabled);
    reader.flag(Field::kApplication, style.background);
    reader.u32(style.colour);
    reader.u32(style.text);
    reader.u32(style.accent);
    reader.u8(style.text_scale);
    if (with_image) reader.u8(style.image);
  }
}

// 7 flags, 9 u8 values, 10 u16 values (formats 2 and 3).
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
        &model.ip_badge_s, &model.window_before, &model.window_after,
        &model.api_daily_budget})
    reader.u16(*value);
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
        model.window_before, model.window_after, model.api_daily_budget})
    writer.u16(value);
}

// Format 3 after the secrets: screens, scalars, crest image and the lists.
void readLists(Reader& reader, Model& model) {
  readScreens(reader, model, true);
  readScalars(reader, model);
  reader.u8(model.crest_image);

  uint8_t count = 0;
  if (reader.count(model.slides.kCapacity, count)) {
    model.slides.clear();
    for (uint8_t i = 0; i < count; ++i) reader.u8(model.slides.items[i]);
    model.slides.count = count;
    reader.endList();
  }
  if (reader.count(model.competitions.kCapacity, count)) {
    model.competitions.clear();
    for (uint8_t i = 0; i < count; ++i) {
      auto& entry = model.competitions.items[i];
      reader.flag(Field::kApplication, entry.active);
      reader.flag(Field::kApplication, entry.fallback_active);
      readSource(reader, entry.primary);
      readSource(reader, entry.fallback);
      reader.text(Field::kApplication, entry.competition_name);
      reader.text(Field::kApplication, entry.team_name);
    }
    model.competitions.count = count;
    reader.endList();
  }
  if (reader.count(model.fixture_pairs.kCapacity, count)) {
    model.fixture_pairs.clear();
    for (uint8_t i = 0; i < count; ++i) {
      auto& pair = model.fixture_pairs.items[i];
      reader.flag(Field::kApplication, pair.active);
      reader.flag(Field::kApplication, pair.swapped);
      reader.u8(pair.competition);
      reader.text(Field::kApplication, pair.primary_id);
      reader.text(Field::kApplication, pair.secondary_id);
    }
    model.fixture_pairs.count = count;
    reader.endList();
  }
}

// Format 2 stored its lists as fixed slots with an enabled flag each: three
// routes with their fallbacks and eight fixture mappings that refer to a
// route by index. They become ordered lists: a complete route is a competition
// (a disabled one stays as paused), an empty slot disappears, a fallback
// becomes its competition's property and a mapping is renumbered to its
// competition (or dropped with its route).
constexpr unsigned kSlotRoutes = 3;
constexpr unsigned kSlotMappings = 8;
constexpr uint8_t kNoCompetition = 0xff;

void readSlots(Reader& reader, Model& model) {
  uint8_t competition_of[kSlotRoutes];
  std::fill(std::begin(competition_of), std::end(competition_of),
            kNoCompetition);
  bool replaced = false;
  for (unsigned i = 0; i < kSlotRoutes; ++i) {
    Competition entry;
    bool enabled = false;
    if (!reader.flag(Field::kApplication, enabled)) break;
    readSource(reader, entry.primary);
    entry.active = enabled;
    if (!replaced) model.competitions.clear();
    replaced = true;
    if (entry.primary.complete()) {
      competition_of[i] = model.competitions.count;
      model.competitions.push(entry);
    }
  }
  readScreens(reader, model, false);
  readScalars(reader, model);

  for (unsigned i = 0; i < kSlotRoutes; ++i) {
    Source fallback;
    bool enabled = false;
    if (!reader.flag(Field::kApplication, enabled)) break;
    readSource(reader, fallback);
    if (competition_of[i] == kNoCompetition) continue;
    auto& entry = model.competitions[competition_of[i]];
    entry.fallback_active = enabled;
    if (fallback.complete()) entry.fallback = fallback;
  }

  replaced = false;
  for (unsigned i = 0; i < kSlotMappings; ++i) {
    FixturePair pair;
    uint8_t route = 0;
    if (!reader.flag(Field::kApplication, pair.active)) break;
    reader.u8(route);
    reader.flag(Field::kApplication, pair.swapped);
    reader.text(Field::kApplication, pair.primary_id);
    reader.text(Field::kApplication, pair.secondary_id);
    if (!replaced) model.fixture_pairs.clear();
    replaced = true;
    if (route < kSlotRoutes && competition_of[route] != kNoCompetition &&
        !pair.primary_id.empty() && !pair.secondary_id.empty()) {
      pair.competition = competition_of[route];
      model.fixture_pairs.push(pair);
    }
  }
}

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
  for (const auto& style : model.screens) {
    writer.flag(style.enabled);
    writer.flag(style.background);
    writer.u32(style.colour);
    writer.u32(style.text);
    writer.u32(style.accent);
    writer.u8(style.text_scale);
    writer.u8(style.image);
  }
  writeScalars(writer, model);
  writer.u8(model.crest_image);
  writer.u8(model.slides.count);
  for (const uint8_t slide : model.slides) writer.u8(slide);
  writer.u8(model.competitions.count);
  for (const auto& entry : model.competitions) {
    writer.flag(entry.active);
    writer.flag(entry.fallback_active);
    writeSource(writer, entry.primary);
    writeSource(writer, entry.fallback);
    writer.text(entry.competition_name);
    writer.text(entry.team_name);
  }
  writer.u8(model.fixture_pairs.count);
  for (const auto& pair : model.fixture_pairs) {
    writer.flag(pair.active);
    writer.flag(pair.swapped);
    writer.u8(pair.competition);
    writer.text(pair.primary_id);
    writer.text(pair.secondary_id);
  }
  writer.flag(model.pin_own_match);  // format 4
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

DecodeResult decode(std::span<const uint8_t> record, Model& model,
                    Field* invalid, const Limits& limits) noexcept {
  if (record.size() < kHeaderBytes) return DecodeResult::kTooShort;
  if (record.size() > cfg::kSettingsMaxRecordBytes)
    return DecodeResult::kTooLong;
  if (readU16(record.data() + kLengthOffset) != record.size() - kHeaderBytes)
    return DecodeResult::kLengthMismatch;
  if (readU32(record.data() + kCrcOffset) != recordCrc(record))
    return DecodeResult::kChecksumMismatch;
  const auto version = readU16(record.data() + kVersionOffset);
  const bool legacy = version == cfg::kSettingsLegacyFormatVersion;
  const bool slots = version == cfg::kSettingsSlotFormatVersion;
  const bool lists = version == cfg::kSettingsListFormatVersion;
  if (version != cfg::kSettingsFormatVersion && !legacy && !slots && !lists)
    return DecodeResult::kUnknownVersion;
  if (legacy && record.size() > cfg::kSettingsLegacyMaxRecordBytes)
    return DecodeResult::kTooLong;

  Model candidate = model;
  const auto readable =
      legacy ? std::min(record.size(), cfg::kSettingsLegacyCoreRecordBytes)
             : record.size();
  Reader reader(record.subspan(kHeaderBytes, readable - kHeaderBytes));
  readCore(reader, candidate);
  if (!legacy) {
    readSecrets(reader, candidate);
    if (slots)
      readSlots(reader, candidate);
    else
      readLists(reader, candidate);
    // Format 3 ends before the pin flag and keeps the initial value.
    if (version == cfg::kSettingsFormatVersion)
      reader.flag(Field::kApplication, candidate.pin_own_match);
  }
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

}  // namespace settings
