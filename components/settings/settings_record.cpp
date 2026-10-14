#include "settings_record.h"

#include <algorithm>

namespace settings {

namespace {

constexpr std::size_t kVersionOffset = 0;
// List capacity of the competitions in formats 2 to 7.
constexpr unsigned kLegacyCompetitions = 3;
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

// One screen style; format 2 has no image id.
void readStyle(Reader& reader, ScreenStyle& style, bool with_image) {
  reader.flag(Field::kApplication, style.enabled);
  reader.flag(Field::kApplication, style.background);
  reader.u32(style.colour);
  reader.u32(style.text);
  reader.u32(style.accent);
  reader.u8(style.text_scale);
  if (with_image) reader.u8(style.image);
}

// The styles of the first kListedScreens screens, after the secrets.
void readScreens(Reader& reader, Model& model, bool with_image) {
  for (std::size_t i = 0; i < kListedScreens; ++i)
    readStyle(reader, model.screens[i], with_image);
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

// 7 flags, 9 u8 values, 9 u16 values; formats 2 to 6 add the API-Football
// daily budget as a tenth u16 value, which becomes its daily cap.
void readScalars(Reader& reader, Model& model, bool budget) {
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
  if (budget)
    reader.u16(model.providers[static_cast<unsigned>(
        cfg::Provider::kApiFootball)].limit.daily);
}

// Format 7 tail: daily cap and request spacing per provider. The caps are
// kept, the spacings are dropped (OQ-225); the window limits keep their
// defaults.
void readRequestLimits(Reader& reader, Model& model) {
  for (auto& plan : model.providers) reader.u16(plan.limit.daily);
  uint16_t spacing = 0;
  for (std::size_t i = 0; i < cfg::kProviderCount; ++i) reader.u16(spacing);
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

// Format 8 tail: the plan per provider, then the global limit.
void readRequestPlans(Reader& reader, Model& model) {
  for (auto& plan : model.providers) {
    readLimit(reader, plan.limit);
    for (auto& interval : plan.interval_s) reader.u32(interval);
  }
  readLimit(reader, model.global_limit);
  // Later format-8 tail: the styles after the listed screens, the table
  // step time.
  for (std::size_t i = kListedScreens; i < cfg::kScreenCount; ++i)
    readStyle(reader, model.screens[i], true);
  reader.u16(model.table_page_s);
}

void writeEntry(Writer& writer, const Competition& entry) {
  writer.flag(entry.active);
  writer.flag(entry.fallback_active);
  writeSource(writer, entry.primary);
  writeSource(writer, entry.fallback);
  writer.text(entry.competition_name);
  writer.text(entry.team_name);
}

void readEntry(Reader& reader, Competition& entry) {
  reader.flag(Field::kApplication, entry.active);
  reader.flag(Field::kApplication, entry.fallback_active);
  readSource(reader, entry.primary);
  readSource(reader, entry.fallback);
  reader.text(Field::kApplication, entry.competition_name);
  reader.text(Field::kApplication, entry.team_name);
}

void writePair(Writer& writer, const FixturePair& pair) {
  writer.flag(pair.active);
  writer.flag(pair.swapped);
  writer.u8(pair.competition);
  writer.text(pair.primary_id);
  writer.text(pair.secondary_id);
}

void readPair(Reader& reader, FixturePair& pair) {
  reader.flag(Field::kApplication, pair.active);
  reader.flag(Field::kApplication, pair.swapped);
  reader.u8(pair.competition);
  reader.text(Field::kApplication, pair.primary_id);
  reader.text(Field::kApplication, pair.secondary_id);
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

// Format 5 tail; validate() rejects unknown mode and role values.
void readFormat5(Reader& reader, Model& model) {
  uint8_t value = 0;
  if (reader.u8(value))
    model.conference_mode = static_cast<cfg::ConferenceMode>(value);
  reader.flag(Field::kApplication, model.whole_matchday);
  for (auto& role : model.input_roles)
    if (reader.u8(value)) role = static_cast<cfg::InputRole>(value);
}

// Formats 3 to 8 after the secrets: screens, scalars (with the budget up to
// format 6), crest image (formats 3 to 5 only, 'crest' given) and the lists;
// formats 3 to 7 ('competitions' given) also the competitions and fixture
// pairs, which get the ids 1..n (pairs referred to the list index).
void readLists(Reader& reader, Model& model, uint8_t* crest, bool budget,
               Competitions* competitions) {
  readScreens(reader, model, true);
  readScalars(reader, model, budget);
  if (crest != nullptr) reader.u8(*crest);

  uint8_t count = 0;
  if (reader.count(model.slides.kCapacity, count)) {
    model.slides.clear();
    for (uint8_t i = 0; i < count; ++i) reader.u8(model.slides.items[i]);
    model.slides.count = count;
    reader.endList();
  }
  if (competitions == nullptr) return;
  // Formats 3 to 7 held at most kLegacyCompetitions competitions.
  if (reader.count(kLegacyCompetitions, count)) {
    auto& entries = competitions->entries;
    entries.clear();
    for (uint8_t i = 0; i < count; ++i) {
      readEntry(reader, entries.items[i]);
      entries.items[i].id = static_cast<uint8_t>(i + 1);
    }
    entries.count = count;
    competitions->next_id = static_cast<uint8_t>(count + 1);
    reader.endList();
  }
  if (reader.count(competitions->pairs.kCapacity, count)) {
    auto& pairs = competitions->pairs;
    pairs.clear();
    for (uint8_t i = 0; i < count; ++i) {
      readPair(reader, pairs.items[i]);
      // An index beyond the list stays invalid (id 0 is never assigned).
      auto& ref = pairs.items[i].competition;
      ref = ref < kLegacyCompetitions ? static_cast<uint8_t>(ref + 1) : 0;
    }
    pairs.count = count;
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

void readSlots(Reader& reader, Model& model, Competitions& competitions) {
  auto& entries = competitions.entries;
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
    if (!replaced) entries.clear();
    replaced = true;
    if (entry.primary.complete()) {
      competition_of[i] = entries.count;
      entry.id = static_cast<uint8_t>(entries.count + 1);
      entries.push(entry);
      competitions.next_id = static_cast<uint8_t>(entries.count + 1);
    }
  }
  readScreens(reader, model, false);
  readScalars(reader, model, true);

  for (unsigned i = 0; i < kSlotRoutes; ++i) {
    Source fallback;
    bool enabled = false;
    if (!reader.flag(Field::kApplication, enabled)) break;
    readSource(reader, fallback);
    if (competition_of[i] == kNoCompetition) continue;
    auto& entry = entries[competition_of[i]];
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
    if (!replaced) competitions.pairs.clear();
    replaced = true;
    if (route < kSlotRoutes && competition_of[route] != kNoCompetition &&
        !pair.primary_id.empty() && !pair.secondary_id.empty()) {
      pair.competition = entries[competition_of[route]].id;
      competitions.pairs.push(pair);
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
  for (const auto& plan : model.providers) {  // format 8
    writeLimit(writer, plan.limit);
    for (const uint32_t interval : plan.interval_s) writer.u32(interval);
  }
  writeLimit(writer, model.global_limit);
  for (std::size_t i = kListedScreens; i < cfg::kScreenCount; ++i)
    writeStyle(writer, model.screens[i]);
  writer.u16(model.table_page_s);
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

}  // namespace

DecodeResult decode(std::span<const uint8_t> record, Model& model,
                    Field* invalid, const Limits& limits,
                    uint8_t* crest_image,
                    Competitions* competitions) noexcept {
  if (const auto header = checkHeader(record, cfg::kSettingsMaxRecordBytes);
      header != DecodeResult::kOk)
    return header;
  const auto version = readU16(record.data() + kVersionOffset);
  const bool legacy = version == cfg::kSettingsLegacyFormatVersion;
  const bool slots = version == cfg::kSettingsSlotFormatVersion;
  const bool lists = version == cfg::kSettingsListFormatVersion;
  const bool pin = version == cfg::kSettingsPinFormatVersion;
  const bool mode = version == cfg::kSettingsModeFormatVersion;
  const bool roles = version == cfg::kSettingsRoleFormatVersion;
  const bool limited = version == cfg::kSettingsLimitFormatVersion;
  const bool current = version == cfg::kSettingsFormatVersion;
  if (!current && !legacy && !slots && !lists && !pin && !mode && !roles &&
      !limited)
    return DecodeResult::kUnknownVersion;
  if (legacy && record.size() > cfg::kSettingsLegacyMaxRecordBytes)
    return DecodeResult::kTooLong;

  Model candidate = model;
  // Formats 2 to 7 carry the competitions; without a target they are read
  // and checked, then dropped. A list the record holds replaces the target's
  // list; a record ending before it keeps the target's initial list.
  Competitions found;
  if (competitions != nullptr) found = *competitions;
  const bool carries_competitions = !legacy && !current;
  const auto readable =
      legacy ? std::min(record.size(), cfg::kSettingsLegacyCoreRecordBytes)
             : record.size();
  Reader reader(record.subspan(kHeaderBytes, readable - kHeaderBytes));
  uint8_t crest = cfg::kStockImageId;
  readCore(reader, candidate);
  if (!legacy) {
    readSecrets(reader, candidate);
    if (slots)
      readSlots(reader, candidate, found);
    else
      readLists(reader, candidate,
                current || limited || roles ? nullptr : &crest, !current &&
                !limited, carries_competitions ? &found : nullptr);
    // Format 3 ends before the pin flag, format 4 before the format-5
    // fields, formats 5 and 6 before the request limits; missing fields keep
    // their initial values.
    if (pin || mode || roles || limited || current)
      reader.flag(Field::kApplication, candidate.pin_own_match);
    if (mode || roles || limited || current) readFormat5(reader, candidate);
    if (limited) readRequestLimits(reader, candidate);
    if (current) readRequestPlans(reader, candidate);
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
  if (result == DecodeResult::kOk && carries_competitions &&
      !validate(found)) {
    bad = Field::kApplication;
    result = DecodeResult::kInvalidValue;
  }
  if (result != DecodeResult::kOk) {
    if (result == DecodeResult::kInvalidValue && invalid != nullptr)
      *invalid = bad;
    return result;
  }
  model = candidate;
  if (carries_competitions && competitions != nullptr) *competitions = found;
  // An id outside the library never passed validation; it is dropped.
  if (crest_image != nullptr)
    *crest_image = crest <= cfg::kImageLibraryMax ? crest : cfg::kStockImageId;
  return DecodeResult::kOk;
}

bool carriesCompetitions(std::span<const uint8_t> record) noexcept {
  if (record.size() < kHeaderBytes) return false;
  const auto version = readU16(record.data() + kVersionOffset);
  return version > cfg::kSettingsLegacyFormatVersion &&
         version < cfg::kSettingsFormatVersion;
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
  writer.u8(competitions.pairs.count);
  for (const auto& pair : competitions.pairs) writePair(writer, pair);
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
  if (readU16(record.data() + kVersionOffset) !=
      cfg::kCompetitionsFormatVersion)
    return DecodeResult::kUnknownVersion;
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
  if (reader.count(candidate.pairs.kCapacity, count)) {
    for (uint8_t i = 0; i < count; ++i)
      readPair(reader, candidate.pairs.items[i]);
    candidate.pairs.count = count;
    reader.endList();
  }
  DecodeResult result = reader.result();
  if (result == DecodeResult::kOk && !validate(candidate))
    result = DecodeResult::kInvalidValue;
  if (result == DecodeResult::kOk) competitions = candidate;
  return result;
}

}  // namespace settings
