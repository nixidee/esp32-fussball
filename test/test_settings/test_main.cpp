// Host acceptance for the settings model, its limits, the initial values and
// the stored records: the settings record of format 9 (variable length,
// slides, Konferenz pin flag, Konferenz mode, whole-matchday flag, input
// roles, request plan per provider and the global limit) and the competition
// record of format 2 (own NVS key: ids, one source per competition and its
// screen switches). Older formats of either record are not read (OQ-253;
// docs/CONFIGURATION.md).

#include <unity.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "settings_model.h"
#include "settings_record.h"

namespace {

using settings::Competition;
using settings::Competitions;
using settings::decode;
using settings::DecodeResult;
using settings::encode;
using settings::Field;
using settings::fieldBit;
using settings::FieldMask;
using settings::initialCompetitions;
using settings::initialValues;
using settings::kHeaderBytes;
using settings::kMaxCompetitionRecordBytes;
using settings::kMaxRecordBytes;
using settings::Model;
using settings::Presets;
using settings::RequestLimit;
using settings::Source;

// A record has exactly its used length (both records vary in length).
using Record = std::vector<uint8_t>;

// Offsets of the record, header included, written out independently of the
// implementation. The core comes first.
constexpr std::size_t kSsidAt = 8;
constexpr std::size_t kPasswordAt = 41;
constexpr std::size_t kApPasswordAt = 106;
constexpr std::size_t kHostnameAt = 170;
constexpr std::size_t kAntennaAt = 234;
constexpr std::size_t kStatusLogAt = 235;
constexpr std::size_t kIntervalAt = 236;
constexpr std::size_t kTimeZoneAt = 238;
constexpr std::size_t kNtpServerAt = 271;
constexpr std::size_t kCoreBytes = 335;
// Format 9 continues with the secrets, the screens, the scalar values, the
// slides, the pin flag, the Konferenz mode, the whole-matchday flag, the
// input roles and the request plans, and ends with the later tail.
constexpr std::size_t kAdminAt = kCoreBytes;
constexpr std::size_t kApiKeyAt = kAdminAt + 1 + 64;
constexpr std::size_t kFootballKeyAt = kApiKeyAt + 1 + 96;
constexpr std::size_t kScreensAt = kFootballKeyAt + 1 + 64;
constexpr std::size_t kScreenBytes = 16;
constexpr std::size_t kScalarsAt = kScreensAt + 4 * kScreenBytes;
constexpr std::size_t kLanguageAt = kScalarsAt + 7;
constexpr std::size_t kSlideshowAt = kScalarsAt + 16 + 2 * 3;
// After the 7 flags, 9 u8 and 9 u16 scalar values: the slide count, then
// one byte per slide.
constexpr std::size_t kSlidesAt = kScalarsAt + 7 + 9 + 2 * 9;
constexpr std::size_t kSourceBytes = 1 + 33 + 17 + 33;
// Active flag, the source and the two display names.
constexpr std::size_t kCompetitionBytes = 1 + kSourceBytes + 33 + 33;
// After the pin flag: mode, whole matchday, eight input roles.
constexpr std::size_t kModeRolesBytes = 1 + 1 + 8;
// Format 9 end: per provider its limit (daily, window requests, window
// seconds: u16 each) and one u32 interval per data type; then the global
// limit.
constexpr std::size_t kPlanBytes = 3 * 2 + 4 * cfg::kDataTypeCount;
constexpr std::size_t kPlansBytes = cfg::kProviderCount * kPlanBytes + 3 * 2;
// Later tail (OQ-193, OQ-196, OQ-270): the matchday screen's style, the
// table step time (u16), the matchday mode (u8). A shorter format-9 record
// ends before it.
constexpr std::size_t kTailBytes = kScreenBytes + 2 + 1;
// No slides plus the pin flag, mode and roles, the request plans and the
// later tail: the shortest format-9 record.
constexpr std::size_t kShortestBytes =
    kSlidesAt + 1 + 1 + kModeRolesBytes + kPlansBytes + kTailBytes;
// Competition record: next_id, the entry count, per entry its id, the
// competition (active flag, source, display names) and the screen bits.
constexpr std::size_t kStoreNextIdAt = kHeaderBytes;
constexpr std::size_t kStoreCountAt = kStoreNextIdAt + 1;
constexpr std::size_t kStoreEntryAt = kStoreCountAt + 1;
constexpr std::size_t kStoredEntryBytes = 1 + kCompetitionBytes + 1;
// The model is copied onto task stacks (settings::current(), HTTP handlers):
// growth is a stack budget change, see docs/ARCHITECTURE.md. Format 8 moved
// the competitions and fixture pairs into their own store (1,968 -> 824);
// the matchday screen's style and the table step time add 20 B (844).
constexpr std::size_t kModelBytesBudget = 844;
// The competition store is held once (the store's current copy) and passed
// by reference; it never lands on a task stack by value. One source per
// competition and no fixture pairs (OQ-247): 2,459 -> 1,226.
constexpr std::size_t kCompetitionsBytesBudget = 1226;

Model defaults() { return initialValues({}); }

Model sample() {
  Model model = defaults();
  model.wifi_ssid.assign("Home WLAN");
  model.wifi_password.assign("correct horse");
  model.ap_password.assign("setup-1234");
  model.hostname.assign("fussball-kitchen");
  model.external_antenna = true;
  model.debug_status_log = false;
  model.debug_status_interval_s = 600;
  model.time_zone.assign("America/New_York");
  model.ntp_server.assign("time.example.org");
  return model;
}

Competition competition(cfg::Provider provider, std::string_view id,
                        std::string_view season, std::string_view team) {
  Competition entry;
  entry.primary.provider = provider;
  entry.primary.competition.assign(id);
  entry.primary.season.assign(season);
  entry.primary.team.assign(team);
  return entry;
}

// One competition (id 1) with a club and display names.
Competitions withLists() {
  Competitions competitions;
  Competition entry =
      competition(cfg::Provider::kOpenLigaDb, "bl1", "2025", "16");
  entry.id = 1;
  entry.competition_name.assign("Bundesliga");
  entry.team_name.assign("FC Test");
  competitions.entries.push(entry);
  competitions.next_id = 2;
  return competitions;
}

// Every text at its capacity, the slide list full.
Model full() {
  Model model = sample();
  model.wifi_ssid.assign(std::string(32, 'S'));
  model.wifi_password.assign(std::string(64, 'a'));
  model.ap_password.assign(std::string(63, '~'));
  model.hostname.assign("a" + std::string(61, '-') + "z");
  model.time_zone.assign("America/Los_Angeles");
  model.ntp_server.assign(std::string(63, 'n'));
  model.admin_password.assign(std::string(64, 'A'));
  model.api_football_key.assign(std::string(96, 'k'));
  model.football_data_key.assign(std::string(64, 'd'));
  for (std::size_t i = 0; i < 3; ++i) model.screens[i].image = 10;
  for (std::size_t i = 0; i < model.slides.kCapacity; ++i)
    model.slides.push(i % 2 == 0 ? cfg::kImageLibraryMax : 0);
  return model;
}

// The list full, every text at its capacity, ids 1..8.
Competitions fullCompetitions() {
  Competitions competitions;
  for (uint8_t i = 0; !competitions.entries.full(); ++i) {
    Competition entry;
    entry.id = static_cast<uint8_t>(i + 1);
    entry.primary.provider = cfg::Provider::kOpenLigaDb;
    entry.primary.competition.assign(std::string(32, 'c'));
    entry.primary.season.assign(std::string(16, '9'));
    entry.primary.team.assign(std::string(32, 't'));
    entry.competition_name.assign(std::string(32, 'N'));
    entry.team_name.assign(std::string(32, 'T'));
    entry.screens = static_cast<uint8_t>((i * 5) & 0x0f);
    competitions.entries.push(entry);
  }
  competitions.next_id = 9;
  return competitions;
}

Record encoded(const Model& model) {
  std::array<uint8_t, kMaxRecordBytes> buffer{};
  const std::size_t used = encode(model, buffer);
  return Record(buffer.begin(), buffer.begin() + used);
}

Record encodedCompetitions(const Competitions& competitions) {
  std::array<uint8_t, kMaxCompetitionRecordBytes> buffer{};
  const std::size_t used = settings::encodeCompetitions(competitions, buffer);
  return Record(buffer.begin(), buffer.begin() + used);
}

uint32_t readU32(const uint8_t* at) {
  return uint32_t{at[0]} | (uint32_t{at[1]} << 8) | (uint32_t{at[2]} << 16) |
         (uint32_t{at[3]} << 24);
}

DecodeResult decodeInto(std::span<const uint8_t> record, Model& model,
                        Field* invalid = nullptr) {
  return decode(record, model, invalid);
}

DecodeResult decodeStore(std::span<const uint8_t> record,
                         Competitions& competitions) {
  return settings::decodeCompetitions(record, competitions);
}

// Record with one payload byte changed and a valid header, so the value
// check (not the CRC) has to catch it.
Record withByte(const Model& model, std::size_t at, uint8_t value) {
  Record record = encoded(model);
  record[at] = value;
  settings::seal(record);
  return record;
}

Record storeWithByte(const Competitions& competitions, std::size_t at,
                     uint8_t value) {
  Record record = encodedCompetitions(competitions);
  record[at] = value;
  settings::seal(record, cfg::kCompetitionsFormatVersion);
  return record;
}

// The text 'text' at 'at': length byte, the bytes, zero padding up to the
// capacity.
void expectText(const Record& record, std::size_t at, std::string_view text,
                std::size_t capacity) {
  TEST_ASSERT_EQUAL_UINT8(text.size(), record[at]);
  for (std::size_t i = 0; i < text.size(); ++i)
    TEST_ASSERT_EQUAL_HEX8(text[i], record[at + 1 + i]);
  for (std::size_t i = text.size(); i < capacity; ++i)
    TEST_ASSERT_EQUAL_HEX8(0, record[at + 1 + i]);
}

void expectBytes(const Record& record, std::size_t at,
                 std::initializer_list<uint8_t> bytes) {
  for (const uint8_t byte : bytes) TEST_ASSERT_EQUAL_HEX8(byte, record[at++]);
}

// Builds records from scratch, byte by byte.
class Out {
 public:
  Out& u8(uint8_t value) {
    bytes_.push_back(value);
    return *this;
  }
  Out& u16(uint16_t value) {
    u8(static_cast<uint8_t>(value));
    return u8(static_cast<uint8_t>(value >> 8));
  }
  Out& u32(uint32_t value) {
    for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(value >> (8 * i)));
    return *this;
  }
  Out& text(std::string_view value, std::size_t capacity) {
    u8(static_cast<uint8_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    bytes_.insert(bytes_.end(), capacity - value.size(), 0);
    return *this;
  }
  std::size_t size() const { return bytes_.size(); }
  // Header first, then the payload; the version is the caller's.
  Record record(uint16_t version) const {
    Record record(kHeaderBytes, 0);
    record.insert(record.end(), bytes_.begin(), bytes_.end());
    settings::seal(record, version);
    return record;
  }

 private:
  std::vector<uint8_t> bytes_;
};

void core(Out& out, const Model& model) {
  out.text(model.wifi_ssid.view(), 32)
      .text(model.wifi_password.view(), 64)
      .text(model.ap_password.view(), 63)
      .text(model.hostname.view(), 63)
      .u8(model.external_antenna)
      .u8(model.debug_status_log)
      .u16(model.debug_status_interval_s)
      .text(model.time_zone.view(), 32)
      .text(model.ntp_server.view(), 63);
}

void secrets(Out& out, const Model& model) {
  out.text(model.admin_password.view(), 64)
      .text(model.api_football_key.view(), 96)
      .text(model.football_data_key.view(), 64);
}

// One screen style.
void style(Out& out, const settings::ScreenStyle& value) {
  out.u8(value.enabled)
      .u8(value.background)
      .u32(value.colour)
      .u32(value.text)
      .u32(value.accent)
      .u8(value.text_scale)
      .u8(value.image);
}

// The styles after the secrets: the first four screens (own match,
// Konferenz, table, crest).
void screens(Out& out, const Model& model) {
  for (std::size_t i = 0; i < 4; ++i) style(out, model.screens[i]);
}

// Later tail: the matchday screen's style, the table step time and the
// matchday mode.
void laterTail(Out& out, const Model& model) {
  style(out, model.screens[static_cast<std::size_t>(cfg::Screen::kMatchday)]);
  out.u16(model.table_page_s);
  out.u8(static_cast<uint8_t>(model.matchday_mode));
}

// 7 flags, 9 u8 values and 9 u16 values.
void scalars(Out& out, const Model& model) {
  out.u8(model.espn_opt_in)
      .u8(model.demo)
      .u8(model.backgrounds)
      .u8(model.night_enabled)
      .u8(model.ip_badge_ap_permanent)
      .u8(model.ip_badge_bottom)
      .u8(model.browser_debug)
      .u8(model.language)
      .u8(model.brightness)
      .u8(model.night_brightness)
      .u8(model.rotation)
      .u8(model.visible_matches)
      .u8(model.table_window)
      .u8(static_cast<uint8_t>(model.idle_screen))
      .u8(static_cast<uint8_t>(model.matchday_screen))
      .u8(static_cast<uint8_t>(model.own_match_screen))
      .u16(model.manual_return_s)
      .u16(model.scroll_reset_s)
      .u16(model.scroll_repeat_ms)
      .u16(model.slideshow_s)
      .u16(model.night_start)
      .u16(model.night_end)
      .u16(model.ip_badge_s)
      .u16(model.window_before)
      .u16(model.window_after);
}

void slides(Out& out, const Model& model) {
  out.u8(model.slides.count);
  for (const uint8_t slide : model.slides) out.u8(slide);
}

// After the pin flag.
void modeRoles(Out& out, const Model& model) {
  out.u8(static_cast<uint8_t>(model.conference_mode)).u8(model.whole_matchday);
  for (const auto role : model.input_roles) out.u8(static_cast<uint8_t>(role));
}

void source(Out& out, const Source& value) {
  out.u8(static_cast<uint8_t>(value.provider))
      .text(value.competition.view(), 32)
      .text(value.season.view(), 16)
      .text(value.team.view(), 32);
}

// A stored competition between its id and its screen bits.
void entry(Out& out, const Competition& value) {
  out.u8(value.active);
  source(out, value.primary);
  out.text(value.competition_name.view(), 32).text(value.team_name.view(), 32);
}

void limit(Out& out, const RequestLimit& value) {
  out.u16(value.daily).u16(value.window_requests).u16(value.window_s);
}

// The format-9 record of 'model'.
Record format9(const Model& model) {
  Out out;
  core(out, model);
  secrets(out, model);
  screens(out, model);
  scalars(out, model);
  slides(out, model);
  out.u8(model.pin_own_match);
  modeRoles(out, model);
  for (const auto& plan : model.providers) {
    limit(out, plan.limit);
    for (const uint32_t interval : plan.interval_s) out.u32(interval);
  }
  limit(out, model.global_limit);
  laterTail(out, model);
  return out.record(9);
}

// The competition record of 'competitions'.
Record competitionRecord(const Competitions& competitions) {
  Out out;
  out.u8(competitions.next_id).u8(competitions.entries.count);
  for (const auto& value : competitions.entries) {
    out.u8(value.id);
    entry(out, value);
    out.u8(value.screens);
  }
  return out.record(cfg::kCompetitionsFormatVersion);
}

void testActualUnityVersion() {
  // The v2.7.0 tag has stale package metadata, so inspect the actual header.
  TEST_ASSERT_EQUAL_UINT(2, UNITY_VERSION_MAJOR);
  TEST_ASSERT_EQUAL_UINT(7, UNITY_VERSION_MINOR);
  TEST_ASSERT_EQUAL_UINT(0, UNITY_VERSION_BUILD);
}

void testCrc32KnownVector() {
  constexpr std::string_view kCheck = "123456789";
  const auto bytes =
      std::span(reinterpret_cast<const uint8_t*>(kCheck.data()), kCheck.size());
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, settings::crc32(bytes));
  // Chaining equals one pass over the concatenation.
  TEST_ASSERT_EQUAL_HEX32(
      0xCBF43926u,
      settings::crc32(bytes.subspan(4), settings::crc32(bytes.first(4))));
  TEST_ASSERT_EQUAL_HEX32(0u, settings::crc32({}));
}

void testDefaults() {
  const Model model = defaults();
  TEST_ASSERT_TRUE(model.wifi_ssid.empty());
  TEST_ASSERT_TRUE(model.wifi_password.empty());
  TEST_ASSERT_TRUE(model.ap_password.empty());
  TEST_ASSERT_TRUE(model.hostname.view() == "fussball");
  TEST_ASSERT_FALSE(model.external_antenna);
  TEST_ASSERT_TRUE(model.debug_status_log);
  TEST_ASSERT_EQUAL_UINT16(30, model.debug_status_interval_s);
  TEST_ASSERT_TRUE(model.time_zone.view() == "Europe/Berlin");
  TEST_ASSERT_TRUE(model.ntp_server.view() == "pool.ntp.org");
  TEST_ASSERT_TRUE(model.slides.empty());
  for (const auto& style : model.screens)
    TEST_ASSERT_EQUAL_UINT8(cfg::kStockImageId, style.image);
  TEST_ASSERT_TRUE(model.providers == settings::providerPlansDefault());
  TEST_ASSERT_TRUE(model.global_limit == RequestLimit{});
  TEST_ASSERT_TRUE(settings::validate(model));

  // An empty competition store; the first new entry gets id 1.
  const Competitions store;
  TEST_ASSERT_TRUE(store.entries.empty());
  TEST_ASSERT_EQUAL_UINT8(1, store.next_id);
  TEST_ASSERT_TRUE(settings::validate(store));
  // A new competition is used for all four screens (OQ-192).
  TEST_ASSERT_EQUAL_HEX8(0x0f, Competition{}.screens);
  TEST_ASSERT_EQUAL_UINT8(0, Competition{}.id);
}

void testListContainer() {
  settings::List<uint8_t, 3> list;
  TEST_ASSERT_TRUE(list.empty());
  TEST_ASSERT_FALSE(list.full());
  TEST_ASSERT_TRUE(list.push(1));
  TEST_ASSERT_TRUE(list.push(2));
  TEST_ASSERT_TRUE(list.push(3));
  TEST_ASSERT_TRUE(list.full());
  // A full list refuses and stays as it is.
  TEST_ASSERT_FALSE(list.push(4));
  TEST_ASSERT_EQUAL_size_t(3, list.size());
  TEST_ASSERT_EQUAL_UINT8(3, list[2]);
  unsigned sum = 0;
  for (const uint8_t value : list) sum += value;  // only the entries
  TEST_ASSERT_EQUAL_UINT(6, sum);

  list.clear();
  TEST_ASSERT_TRUE(list.empty());
  TEST_ASSERT_EQUAL_UINT8(0, list.items[0]);
  for (const uint8_t value : list) TEST_FAIL_MESSAGE("empty list iterates");

  // Comparison ignores the unused storage.
  settings::List<uint8_t, 3> a, b;
  a.push(5);
  b.push(5);
  a.items[2] = 9;
  TEST_ASSERT_TRUE(a == b);
  b.push(6);
  TEST_ASSERT_FALSE(a == b);
}

void testFormat9Layout() {
  TEST_ASSERT_EQUAL_size_t(8, kHeaderBytes);
  // Pinned: the longest record is an NVS blob size, a memory budget item.
  TEST_ASSERT_EQUAL_size_t(806, kMaxRecordBytes);
  TEST_ASSERT_TRUE(kMaxRecordBytes <= cfg::kSettingsMaxRecordBytes);
  TEST_ASSERT_EQUAL_size_t(26, kPlanBytes);
  TEST_ASSERT_EQUAL_size_t(110, kPlansBytes);
  TEST_ASSERT_EQUAL_size_t(801, kShortestBytes);
  TEST_ASSERT_EQUAL_size_t(19, kTailBytes);

  Model model = defaults();
  model.admin_password.assign("pw");
  auto& style = model.screens[1];
  style.enabled = false;
  style.background = false;
  style.colour = 0x112233;
  style.text = 0x445566;
  style.accent = 0x778899;
  style.text_scale = 120;
  style.image = 3;
  model.language = 1;
  model.slideshow_s = 0x0123;  // 291 s, little-endian 23 01
  model.slides.push(0);
  model.slides.push(3);
  model.conference_mode = cfg::ConferenceMode::kOnePage;
  model.whole_matchday = true;
  model.input_roles = {cfg::InputRole::kScroll, cfg::InputRole::kOff,
                       cfg::InputRole::kMandatory, cfg::InputRole::kDown,
                       cfg::InputRole::kUp,     cfg::InputRole::kOff,
                       cfg::InputRole::kOff,    cfg::InputRole::kOff};
  model.providers[1].limit = {0x0201, 3, 0x0E10};
  model.providers[2].interval_s[2] = 0x00012345;  // live
  model.global_limit = {500, 0, 0};
  TEST_ASSERT_TRUE(settings::validate(model));

  const Record record = encoded(model);
  const std::size_t expected = kShortestBytes + 2;
  TEST_ASSERT_EQUAL_size_t(expected, record.size());
  // The whole record equals the one built from the layout description.
  TEST_ASSERT_TRUE(record == format9(model));

  TEST_ASSERT_EQUAL_HEX8(0x09, record[0]);  // version 9
  TEST_ASSERT_EQUAL_HEX8(0x00, record[1]);
  const std::size_t payload = expected - kHeaderBytes;
  TEST_ASSERT_EQUAL_HEX8(payload & 0xff, record[2]);
  TEST_ASSERT_EQUAL_HEX8(payload >> 8, record[3]);

  // Core: unchanged from format 1.
  expectText(record, kSsidAt, "", 32);
  expectText(record, kHostnameAt, "fussball", 63);
  TEST_ASSERT_EQUAL_HEX8(1, record[kStatusLogAt]);
  expectText(record, kTimeZoneAt, "Europe/Berlin", 32);

  // Secrets, screens, scalars.
  expectText(record, kAdminAt, "pw", 64);
  expectText(record, kApiKeyAt, "", 96);
  expectText(record, kFootballKeyAt, "", 64);
  TEST_ASSERT_EQUAL_HEX8(1, record[kScreensAt]);  // screen 0 enabled
  TEST_ASSERT_EQUAL_HEX8(0, record[kScreensAt + 15]);
  const std::size_t second = kScreensAt + kScreenBytes;
  TEST_ASSERT_EQUAL_HEX8(0, record[second]);
  TEST_ASSERT_EQUAL_HEX8(0, record[second + 1]);
  expectBytes(record, second + 2,
              {0x33, 0x22, 0x11, 0x00, 0x66, 0x55, 0x44, 0x00, 0x99, 0x88,
               0x77, 0x00});
  TEST_ASSERT_EQUAL_UINT8(120, record[second + 14]);
  TEST_ASSERT_EQUAL_UINT8(3, record[second + 15]);
  TEST_ASSERT_EQUAL_HEX8(1, record[kLanguageAt]);
  TEST_ASSERT_EQUAL_HEX8(0x23, record[kSlideshowAt]);
  TEST_ASSERT_EQUAL_HEX8(0x01, record[kSlideshowAt + 1]);

  // The slides right after the scalar values: no competition lists any more.
  expectBytes(record, kSlidesAt, {2, 0, 3});
  // The pin flag (default on), then mode, whole matchday and one role byte
  // per input slot.
  const std::size_t pin = kSlidesAt + 3;
  TEST_ASSERT_EQUAL_HEX8(1, record[pin]);
  expectBytes(record, pin + 1, {1, 1, 4, 0, 1, 3, 2, 0, 0, 0});
  // The request plans in provider order: limit (daily, window requests,
  // window seconds), then the intervals in data type order (fixtures, table,
  // live, live table, events), little-endian.
  const std::size_t plans = pin + 1 + kModeRolesBytes;
  expectBytes(record, plans,  // OpenLigaDB defaults: 60 per 60 s
              {0x00, 0x00, 0x3C, 0x00, 0x3C, 0x00,  //
               0x80, 0x70, 0x00, 0x00, 0x40, 0x38, 0x00, 0x00, 0x3C, 0x00,
               0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00});
  expectBytes(record, plans + kPlanBytes,
              {0x01, 0x02, 0x03, 0x00, 0x10, 0x0E});
  expectBytes(record, plans + 2 * kPlanBytes + 6 + 2 * 4,
              {0x45, 0x23, 0x01, 0x00});
  // The global limit, then the matchday screen's style, the table step time
  // and the matchday mode close the record.
  const std::size_t global = plans + cfg::kProviderCount * kPlanBytes;
  expectBytes(record, global, {0xF4, 0x01, 0x00, 0x00, 0x00, 0x00});
  const std::size_t tail = global + 6;
  TEST_ASSERT_EQUAL_HEX8(1, record[tail]);  // matchday screen on
  TEST_ASSERT_EQUAL_UINT8(cfg::kStockImageId, record[tail + 15]);
  expectBytes(record, tail + kScreenBytes, {0x08, 0x00});  // 8 s
  TEST_ASSERT_EQUAL_HEX8(0, record[tail + kScreenBytes + 2]);  // current
  TEST_ASSERT_EQUAL_size_t(tail + kTailBytes, record.size());

  // CRC over header bytes 0..3 followed by the payload.
  std::vector<uint8_t> covered(record.begin(), record.begin() + 4);
  covered.insert(covered.end(), record.begin() + kHeaderBytes, record.end());
  TEST_ASSERT_EQUAL_HEX32(settings::crc32(covered), readU32(&record[4]));

  // The record length follows the slides; encode() reports it.
  std::array<uint8_t, kMaxRecordBytes> buffer{};
  TEST_ASSERT_EQUAL_size_t(expected, encode(model, buffer));
  for (std::size_t i = expected; i < buffer.size(); ++i)
    TEST_ASSERT_EQUAL_HEX8(0, buffer[i]);
  TEST_ASSERT_EQUAL_size_t(kShortestBytes, encoded(defaults()).size());
}

// OQ-200/OQ-209/OQ-247: the competitions have their own record with stable
// ids and one source each (format 2).
void testCompetitionRecordLayout() {
  // Pinned: the longest record is an NVS blob size, a memory budget item.
  TEST_ASSERT_EQUAL_size_t(1234, kMaxCompetitionRecordBytes);
  TEST_ASSERT_TRUE(kMaxCompetitionRecordBytes <=
                   cfg::kCompetitionsMaxRecordBytes);
  TEST_ASSERT_EQUAL_size_t(153, kStoredEntryBytes);

  Competitions store = withLists();
  store.entries[0].id = 7;
  store.entries[0].active = false;
  store.entries[0].screens = 0x05;
  store.entries[0].competition_name.assign("Liga");
  store.entries[0].team_name.assign("Club");
  store.next_id = 9;
  TEST_ASSERT_TRUE(settings::validate(store));

  const Record record = encodedCompetitions(store);
  const std::size_t expected = kStoreEntryAt + kStoredEntryBytes;
  TEST_ASSERT_EQUAL_size_t(expected, record.size());
  TEST_ASSERT_TRUE(record == competitionRecord(store));

  TEST_ASSERT_EQUAL_HEX8(0x02, record[0]);  // own format version 2
  TEST_ASSERT_EQUAL_HEX8(0x00, record[1]);
  const std::size_t payload = expected - kHeaderBytes;
  TEST_ASSERT_EQUAL_HEX8(payload & 0xff, record[2]);
  TEST_ASSERT_EQUAL_HEX8(payload >> 8, record[3]);

  TEST_ASSERT_EQUAL_UINT8(9, record[kStoreNextIdAt]);
  TEST_ASSERT_EQUAL_UINT8(1, record[kStoreCountAt]);
  TEST_ASSERT_EQUAL_UINT8(7, record[kStoreEntryAt]);  // id
  const std::size_t at = kStoreEntryAt + 1;
  TEST_ASSERT_EQUAL_HEX8(0, record[at]);  // active
  const std::size_t primary = at + 1;
  TEST_ASSERT_EQUAL_UINT8(0, record[primary]);  // OpenLigaDB
  expectText(record, primary + 1, "bl1", 32);
  expectText(record, primary + 1 + 33, "2025", 16);
  expectText(record, primary + 1 + 33 + 17, "16", 32);
  expectText(record, primary + kSourceBytes, "Liga", 32);
  expectText(record, primary + kSourceBytes + 33, "Club", 32);
  TEST_ASSERT_EQUAL_HEX8(0x05, record[at + kCompetitionBytes]);  // screens

  std::vector<uint8_t> covered(record.begin(), record.begin() + 4);
  covered.insert(covered.end(), record.begin() + kHeaderBytes, record.end());
  TEST_ASSERT_EQUAL_HEX32(settings::crc32(covered), readU32(&record[4]));

  // encodeCompetitions() reports the length and zeroes the rest.
  std::array<uint8_t, kMaxCompetitionRecordBytes> buffer;
  buffer.fill(0xA5);
  TEST_ASSERT_EQUAL_size_t(expected,
                           settings::encodeCompetitions(store, buffer));
  for (std::size_t i = expected; i < buffer.size(); ++i)
    TEST_ASSERT_EQUAL_HEX8(0, buffer[i]);

  // The empty store: next_id and an empty count.
  const Record empty = encodedCompetitions(Competitions{});
  TEST_ASSERT_EQUAL_size_t(kHeaderBytes + 2, empty.size());
  expectBytes(empty, kStoreNextIdAt, {1, 0});
}

void testFullModelUsesTheMaximumRecord() {
  const Model model = full();
  TEST_ASSERT_TRUE(settings::validate(model));
  TEST_ASSERT_TRUE(model.slides.full());

  std::array<uint8_t, kMaxRecordBytes> buffer{};
  TEST_ASSERT_EQUAL_size_t(kMaxRecordBytes, encode(model, buffer));
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(buffer, loaded));
  TEST_ASSERT_TRUE(loaded == model);

  const Competitions store = fullCompetitions();
  TEST_ASSERT_TRUE(settings::validate(store));
  TEST_ASSERT_TRUE(store.entries.full());
  std::array<uint8_t, kMaxCompetitionRecordBytes> store_buffer{};
  TEST_ASSERT_EQUAL_size_t(kMaxCompetitionRecordBytes,
                           settings::encodeCompetitions(store, store_buffer));
  Competitions loaded_store;
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeStore(store_buffer, loaded_store));
  TEST_ASSERT_TRUE(loaded_store == store);

  TEST_ASSERT_TRUE(sizeof(Model) <= kModelBytesBudget);
  TEST_ASSERT_TRUE(sizeof(Competitions) <= kCompetitionsBytesBudget);
}

void testRoundTrip() {
  const Model original = sample();
  const Record record = encoded(original);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == original);

  // Full capacities, any SSID bytes, a raw hex key.
  Model full_texts = defaults();
  full_texts.wifi_ssid.assign(std::string("\x00\xff ssid", 7) +
                              std::string(25, 'S'));
  full_texts.wifi_password.assign(std::string(64, 'a'));
  full_texts.ap_password.assign(std::string(63, '~'));
  full_texts.hostname.assign("a" + std::string(61, '-') + "z");
  full_texts.debug_status_interval_s = 3600;
  full_texts.time_zone.assign("America/Los_Angeles");
  full_texts.ntp_server.assign(std::string(63, 'n'));
  Model loaded_full = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decodeInto(encoded(full_texts), loaded_full));
  TEST_ASSERT_TRUE(loaded_full == full_texts);

  // Images, screens, slides and the scalar values.
  Model rich = sample();
  rich.admin_password.assign("admin");
  rich.api_football_key.assign("key-1");
  rich.football_data_key.assign("key-2");
  rich.screens[0].image = 7;
  rich.screens[1].colour = 0xabcdef;
  rich.slides.push(0);
  rich.slides.push(4);
  rich.slides.push(9);
  rich.language = 1;
  rich.espn_opt_in = true;
  rich.slideshow_s = 3600;
  TEST_ASSERT_TRUE(settings::validate(rich));
  Model loaded_rich = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(encoded(rich), loaded_rich));
  TEST_ASSERT_TRUE(loaded_rich == rich);
  TEST_ASSERT_EQUAL_size_t(3, loaded_rich.slides.size());

  // The competition store with ids, screen switches and a paused entry.
  Competitions store = withLists();
  store.entries[0].screens = 0x09;
  Competition second = competition(cfg::Provider::kEspn, "ger.1", "", "");
  second.id = 4;
  second.active = false;
  store.entries.push(second);
  store.next_id = 5;
  TEST_ASSERT_TRUE(settings::validate(store));
  Competitions loaded_store = fullCompetitions();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decodeStore(encodedCompetitions(store), loaded_store));
  TEST_ASSERT_TRUE(loaded_store == store);
  TEST_ASSERT_EQUAL_size_t(2, loaded_store.entries.size());

  // A stored empty store replaces a non-empty one.
  loaded_store = withLists();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decodeStore(encodedCompetitions({}), loaded_store));
  TEST_ASSERT_TRUE(loaded_store == Competitions{});
}

void testShorterRecordKeepsInitialValues() {
  const Model stored = sample();
  Record record = encoded(stored);

  // Without the last field of the core (ntp_server).
  const auto without_ntp = std::span(record).first(kNtpServerAt);
  settings::seal(without_ntp);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(without_ntp, loaded));
  TEST_ASSERT_TRUE(loaded.time_zone == stored.time_zone);
  TEST_ASSERT_TRUE(loaded.ntp_server.view() == "pool.ntp.org");

  // A record of the firmware before the time fields (238 B).
  record = encoded(stored);
  const auto before_time = std::span(record).first(kTimeZoneAt);
  settings::seal(before_time);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(before_time, loaded));
  TEST_ASSERT_EQUAL_UINT16(600, loaded.debug_status_interval_s);
  TEST_ASSERT_TRUE(loaded.time_zone.view() == "Europe/Berlin");
  TEST_ASSERT_TRUE(loaded.ntp_server.view() == "pool.ntp.org");

  // Without the interval and every later field.
  record = encoded(stored);
  const auto without_interval = std::span(record).first(kIntervalAt);
  settings::seal(without_interval);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(without_interval, loaded));
  TEST_ASSERT_TRUE(loaded.hostname == stored.hostname);
  TEST_ASSERT_TRUE(loaded.external_antenna);
  TEST_ASSERT_EQUAL_UINT16(30, loaded.debug_status_interval_s);
  TEST_ASSERT_TRUE(loaded.time_zone.view() == "Europe/Berlin");

  // Header only: every field keeps its initial value.
  record = encoded(stored);
  const auto header_only = std::span(record).first(kHeaderBytes);
  settings::seal(header_only);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(header_only, loaded));
  TEST_ASSERT_TRUE(loaded == defaults());

  // A record that ends after the core keeps the initial secrets; one that
  // ends after the scalar values keeps the initial slides.
  Model initial = initialValues({.admin_password = "preset-admin"});
  initial.slides.push(0);
  Model later = stored;
  later.language = 1;
  later.slideshow_s = 20;
  record = encoded(later);
  const auto before_secrets = std::span(record).first(kAdminAt);
  settings::seal(before_secrets);
  loaded = initial;
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(before_secrets, loaded));
  TEST_ASSERT_TRUE(loaded.hostname == stored.hostname);
  TEST_ASSERT_TRUE(loaded.admin_password.view() == "preset-admin");
  TEST_ASSERT_EQUAL_UINT8(0, loaded.language);
  TEST_ASSERT_TRUE(loaded.slides == initial.slides);

  record = encoded(later);
  const auto before_slides = std::span(record).first(kSlidesAt);
  settings::seal(before_slides);
  loaded = initial;
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(before_slides, loaded));
  TEST_ASSERT_EQUAL_UINT8(1, loaded.language);
  TEST_ASSERT_EQUAL_UINT16(20, loaded.slideshow_s);
  TEST_ASSERT_TRUE(loaded.slides == initial.slides);
}

void testPartialFieldIsRejected() {
  const Model original = sample();
  for (const std::size_t size :
       {kSsidAt + 1, kPasswordAt - 1, kHostnameAt + 10, kIntervalAt + 1,
        kTimeZoneAt + 5, kNtpServerAt + 1, kCoreBytes - 1,
        kAdminAt + 10,         // inside the admin password
        kScreensAt + 5,        // inside a colour
        kSlideshowAt + 1}) {   // inside a 16-bit value
    Record record = encoded(original);
    const auto cut = std::span(record).first(size);
    settings::seal(cut);
    Model loaded = defaults();
    TEST_ASSERT_EQUAL(DecodeResult::kPartialField, decodeInto(cut, loaded));
    TEST_ASSERT_TRUE(loaded == defaults());
  }
}

void testListsPartialAndInvalid() {
  // The slides of the settings record.
  Model original = sample();
  original.slides.push(0);
  original.slides.push(2);
  const Record base = encoded(original);
  TEST_ASSERT_EQUAL_UINT8(2, base[kSlidesAt]);
  {
    // The record ends cleanly before the count (older record): OK, the
    // slides keep their initial value.
    Record record = base;
    const auto cut = std::span(record).first(kSlidesAt);
    settings::seal(cut);
    Model loaded = defaults();
    TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(cut, loaded));
    TEST_ASSERT_TRUE(loaded.slides.empty());
  }
  // A count without its entries, or entries cut: partial.
  for (const std::size_t size : {kSlidesAt + 1, kSlidesAt + 2}) {
    Record record = base;
    const auto cut = std::span(record).first(size);
    settings::seal(cut);
    Model loaded = defaults();
    TEST_ASSERT_EQUAL(DecodeResult::kPartialField, decodeInto(cut, loaded));
    TEST_ASSERT_TRUE(loaded == defaults());
  }
  struct Case {
    std::size_t at;
    uint8_t value;
  };
  // Counts above the capacity, the crest screen with an image and an image
  // id above the library reject the record.
  const Case values[] = {
      {kSlidesAt, cfg::kSlideMax + 1},
      {kSlidesAt, 255},
      {kScreensAt + 3 * kScreenBytes + 15, 1},
      {kScreensAt + 15, cfg::kImageLibraryMax + 1},
  };
  for (const Case& c : values) {
    const Record record = withByte(original, c.at, c.value);
    Model loaded = defaults();
    Field invalid = Field::kWifiSsid;
    TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue,
                      decodeInto(record, loaded, &invalid));
    TEST_ASSERT_EQUAL_STRING("application", settings::fieldName(invalid));
    TEST_ASSERT_TRUE(loaded == defaults());
  }

  // The competition record.
  const Competitions store = withLists();
  const Record stored = encodedCompetitions(store);
  const std::size_t end = kStoreEntryAt + kStoredEntryBytes;
  TEST_ASSERT_EQUAL_UINT8(1, stored[kStoreCountAt]);
  TEST_ASSERT_EQUAL_size_t(end, stored.size());
  // It ends cleanly before a value or count: OK, the rest is empty.
  for (const std::size_t size : {kStoreNextIdAt, kStoreCountAt}) {
    Record record = stored;
    const auto cut = std::span(record).first(size);
    settings::seal(cut, cfg::kCompetitionsFormatVersion);
    Competitions loaded = fullCompetitions();
    TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeStore(cut, loaded));
    TEST_ASSERT_EQUAL_UINT8(size == kStoreNextIdAt ? 1 : 2, loaded.next_id);
    TEST_ASSERT_TRUE(loaded.entries.empty());
  }
  // A count without its entries, or entries cut in the middle: partial.
  for (const std::size_t size :
       {kStoreEntryAt, kStoreEntryAt + 1, kStoreEntryAt + 100, end - 1}) {
    Record record = stored;
    const auto cut = std::span(record).first(size);
    settings::seal(cut, cfg::kCompetitionsFormatVersion);
    Competitions loaded = fullCompetitions();
    TEST_ASSERT_EQUAL(DecodeResult::kPartialField, decodeStore(cut, loaded));
    TEST_ASSERT_TRUE(loaded == fullCompetitions());
  }
  // Counts above the capacity and values that break the list rules: id 0,
  // an unknown screen bit, an empty competition.
  const std::size_t primary = kStoreEntryAt + 1 + 1;
  const Case store_values[] = {
      {kStoreCountAt, cfg::kCompetitionMax + 1},
      {kStoreEntryAt, 0},
      {kStoreEntryAt + 1 + kCompetitionBytes, 0x10},
      {primary + 1, 0},
  };
  for (const Case& c : store_values) {
    Competitions loaded = fullCompetitions();
    TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue,
                      decodeStore(storeWithByte(store, c.at, c.value), loaded));
    TEST_ASSERT_TRUE(loaded == fullCompetitions());
  }
}

void testLongerRecordIgnoresTail() {
  const Model original = sample();
  Record record = encoded(original);
  record.insert(record.end(), 20, 0xA5);
  settings::seal(record);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == original);

  // Largest accepted record, and one byte more.
  record = encoded(original);
  record.resize(cfg::kSettingsMaxRecordBytes, 0);
  settings::seal(record);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == original);
  record.push_back(0);
  settings::seal(record);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kTooLong, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == defaults());

  // The same for the competition record.
  const Competitions store = withLists();
  record = encodedCompetitions(store);
  record.insert(record.end(), 20, 0xA5);
  settings::seal(record, cfg::kCompetitionsFormatVersion);
  Competitions loaded_store;
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeStore(record, loaded_store));
  TEST_ASSERT_TRUE(loaded_store == store);
  record.resize(cfg::kCompetitionsMaxRecordBytes, 0);
  settings::seal(record, cfg::kCompetitionsFormatVersion);
  loaded_store = {};
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeStore(record, loaded_store));
  TEST_ASSERT_TRUE(loaded_store == store);
  record.push_back(0);
  settings::seal(record, cfg::kCompetitionsFormatVersion);
  loaded_store = {};
  TEST_ASSERT_EQUAL(DecodeResult::kTooLong, decodeStore(record, loaded_store));
  TEST_ASSERT_TRUE(loaded_store == Competitions{});
}

void testDamagedHeaders() {
  const Model original = sample();
  Model loaded = defaults();

  Record record = encoded(original);
  TEST_ASSERT_EQUAL(
      DecodeResult::kTooShort,
      decodeInto(std::span(record).first(kHeaderBytes - 1), loaded));
  TEST_ASSERT_EQUAL(DecodeResult::kTooShort, decodeInto({}, loaded));

  record[2] ^= 0x01;  // length field
  TEST_ASSERT_EQUAL(DecodeResult::kLengthMismatch, decodeInto(record, loaded));

  record = encoded(original);
  record[kHostnameAt + 1] ^= 0x20;  // payload byte, CRC kept
  TEST_ASSERT_EQUAL(DecodeResult::kChecksumMismatch,
                    decodeInto(record, loaded));

  record = encoded(original);
  record[5] ^= 0x80;  // CRC byte
  TEST_ASSERT_EQUAL(DecodeResult::kChecksumMismatch,
                    decodeInto(record, loaded));

  record = encoded(original);
  record[0] = 2;  // version changed, CRC kept
  TEST_ASSERT_EQUAL(DecodeResult::kChecksumMismatch,
                    decodeInto(record, loaded));

  // Formats 1 to 8 are no longer read (OQ-253, OQ-257): the device starts
  // with the initial values ("outdated").
  record = encoded(original);
  for (uint16_t version = 1; version < 9; ++version) {
    settings::seal(record, version);
    TEST_ASSERT_EQUAL(DecodeResult::kOutdatedVersion,
                      decodeInto(record, loaded));
  }
  // Version 0 and newer versions are unknown.
  for (const uint16_t version : {uint16_t{0}, uint16_t{10}, uint16_t{0xFFFF}}) {
    settings::seal(record, version);
    TEST_ASSERT_EQUAL(DecodeResult::kUnknownVersion,
                      decodeInto(record, loaded));
  }
  TEST_ASSERT_TRUE(loaded == defaults());

  // The competition record knows only its version 2; version 1 (fallback
  // sources and fixture pairs) is no longer read (OQ-253).
  const Competitions store = withLists();
  Competitions loaded_store;
  record = encodedCompetitions(store);
  TEST_ASSERT_EQUAL(DecodeResult::kTooShort,
                    decodeStore(std::span(record).first(kHeaderBytes - 1),
                                loaded_store));
  record[3] ^= 0x01;
  TEST_ASSERT_EQUAL(DecodeResult::kLengthMismatch,
                    decodeStore(record, loaded_store));
  record = encodedCompetitions(store);
  record[kStoreEntryAt + 5] ^= 0x01;
  TEST_ASSERT_EQUAL(DecodeResult::kChecksumMismatch,
                    decodeStore(record, loaded_store));
  record = encodedCompetitions(store);
  settings::seal(record, 1);
  TEST_ASSERT_EQUAL(DecodeResult::kOutdatedVersion,
                    decodeStore(record, loaded_store));
  for (const uint16_t version : {uint16_t{0}, uint16_t{3}, uint16_t{8}}) {
    record = encodedCompetitions(store);
    settings::seal(record, version);
    TEST_ASSERT_EQUAL(DecodeResult::kUnknownVersion,
                      decodeStore(record, loaded_store));
  }
  // A settings record is no competition record.
  TEST_ASSERT_EQUAL(DecodeResult::kUnknownVersion,
                    decodeStore(encoded(original), loaded_store));
  TEST_ASSERT_TRUE(loaded_store == Competitions{});
}

void testInvalidStoredValuesRejectWholeRecord() {
  const Model original = sample();
  struct Case {
    std::size_t at;
    uint8_t value;
    Field field;
  };
  const Case cases[] = {
      {kSsidAt, 33, Field::kWifiSsid},          // length over capacity
      {kPasswordAt, 65, Field::kWifiPassword},  // length over capacity
      {kPasswordAt, 7, Field::kWifiPassword},   // passphrase too short
      {kApPasswordAt, 64, Field::kApPassword},  // length over capacity
      {kApPasswordAt, 3, Field::kApPassword},   // passphrase too short
      {kHostnameAt, 0, Field::kHostname},       // empty
      {kHostnameAt, 64, Field::kHostname},      // length over capacity
      {kAntennaAt, 2, Field::kExternalAntenna},
      {kStatusLogAt, 2, Field::kDebugStatusLog},
      {kTimeZoneAt, 0, Field::kTimeZone},          // empty
      {kTimeZoneAt, 33, Field::kTimeZone},         // length over capacity
      {kTimeZoneAt + 1, 'a', Field::kTimeZone},    // "america/New_York"
      {kNtpServerAt, 0, Field::kNtpServer},        // empty
      {kNtpServerAt, 64, Field::kNtpServer},       // length over capacity
      {kNtpServerAt + 1, '-', Field::kNtpServer},  // "-ime.example.org"
      {kNtpServerAt + 6, '.', Field::kNtpServer},  // "time..xample.org"
  };
  for (const Case& c : cases) {
    const Record record = withByte(original, c.at, c.value);
    Model loaded = defaults();
    Field invalid = Field::kWifiSsid;
    TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue,
                      decodeInto(record, loaded, &invalid));
    TEST_ASSERT_EQUAL_STRING(settings::fieldName(c.field),
                             settings::fieldName(invalid));
    TEST_ASSERT_TRUE(loaded == defaults());
  }

  // Interval 4 s (little-endian 04 00) and 3601 s (11 0E).
  for (const uint16_t seconds : {uint16_t{4}, uint16_t{3601}}) {
    Record record = encoded(original);
    record[kIntervalAt] = static_cast<uint8_t>(seconds);
    record[kIntervalAt + 1] = static_cast<uint8_t>(seconds >> 8);
    settings::seal(record);
    Model loaded = defaults();
    Field invalid = Field::kWifiSsid;
    TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue,
                      decodeInto(record, loaded, &invalid));
    TEST_ASSERT_EQUAL_STRING("debug_status_interval_s",
                             settings::fieldName(invalid));
    TEST_ASSERT_TRUE(loaded == defaults());
  }
}

void testWifiPasswordLimits() {
  using settings::isValidWifiPassword;
  TEST_ASSERT_TRUE(isValidWifiPassword(""));
  TEST_ASSERT_FALSE(isValidWifiPassword(std::string(7, 'p')));
  TEST_ASSERT_TRUE(isValidWifiPassword(std::string(8, 'p')));
  TEST_ASSERT_TRUE(isValidWifiPassword(std::string(63, 'p')));
  TEST_ASSERT_TRUE(isValidWifiPassword(std::string(64, 'F')));
  TEST_ASSERT_TRUE(
      isValidWifiPassword(std::string(32, 'a') + std::string(32, '9')));
  TEST_ASSERT_FALSE(isValidWifiPassword(std::string(64, 'g')));
  TEST_ASSERT_FALSE(isValidWifiPassword(std::string(65, 'a')));
  TEST_ASSERT_TRUE(isValidWifiPassword(" !~passwd"));
  TEST_ASSERT_FALSE(isValidWifiPassword("pass\tword"));
  TEST_ASSERT_FALSE(
      isValidWifiPassword("passw\x7f"
                          "rd"));
  TEST_ASSERT_FALSE(isValidWifiPassword("pässword"));
}

void testApPasswordLimits() {
  using settings::isValidApPassword;
  TEST_ASSERT_TRUE(isValidApPassword(""));
  TEST_ASSERT_FALSE(isValidApPassword(std::string(7, 'p')));
  TEST_ASSERT_TRUE(isValidApPassword(std::string(8, 'p')));
  TEST_ASSERT_TRUE(isValidApPassword(std::string(63, 'p')));
  TEST_ASSERT_FALSE(isValidApPassword(std::string(64, 'a')));  // no raw key
  TEST_ASSERT_FALSE(isValidApPassword("password\n"));
}

void testHostnameLimits() {
  using settings::isValidHostname;
  TEST_ASSERT_FALSE(isValidHostname(""));
  TEST_ASSERT_TRUE(isValidHostname("a"));
  TEST_ASSERT_TRUE(isValidHostname("9"));
  TEST_ASSERT_TRUE(isValidHostname("Fussball-2"));
  TEST_ASSERT_TRUE(isValidHostname("a--b"));
  TEST_ASSERT_FALSE(isValidHostname("-a"));
  TEST_ASSERT_FALSE(isValidHostname("a-"));
  TEST_ASSERT_FALSE(isValidHostname("-"));
  TEST_ASSERT_FALSE(isValidHostname("a_b"));
  TEST_ASSERT_FALSE(isValidHostname("a.b"));
  TEST_ASSERT_FALSE(isValidHostname("a b"));
  TEST_ASSERT_FALSE(isValidHostname("fußball"));
  TEST_ASSERT_TRUE(isValidHostname(std::string(63, 'h')));
  TEST_ASSERT_FALSE(isValidHostname(std::string(64, 'h')));
}

void testStatusIntervalLimits() {
  using settings::isValidStatusInterval;
  TEST_ASSERT_FALSE(isValidStatusInterval(0));
  TEST_ASSERT_FALSE(isValidStatusInterval(4));
  TEST_ASSERT_TRUE(isValidStatusInterval(5));
  TEST_ASSERT_TRUE(isValidStatusInterval(3600));
  TEST_ASSERT_FALSE(isValidStatusInterval(3601));
  TEST_ASSERT_FALSE(isValidStatusInterval(0xFFFF));
}

void testTimeZoneLimits() {
  using settings::isValidTimeZone;
  TEST_ASSERT_TRUE(isValidTimeZone("Europe/Berlin"));
  TEST_ASSERT_TRUE(isValidTimeZone("UTC"));
  TEST_ASSERT_FALSE(isValidTimeZone(""));
  TEST_ASSERT_FALSE(isValidTimeZone("europe/berlin"));
  TEST_ASSERT_FALSE(isValidTimeZone("CET-1CEST,M3.5.0,M10.5.0/3"));
  TEST_ASSERT_FALSE(isValidTimeZone("Mars/Olympus_Mons"));
}

void testNtpServerLimits() {
  using settings::isValidNtpServer;
  TEST_ASSERT_TRUE(isValidNtpServer("pool.ntp.org"));
  TEST_ASSERT_TRUE(isValidNtpServer("de.pool.ntp.org"));
  TEST_ASSERT_TRUE(isValidNtpServer("fritz-box"));
  TEST_ASSERT_TRUE(isValidNtpServer("192.168.178.1"));
  TEST_ASSERT_TRUE(isValidNtpServer("a"));
  TEST_ASSERT_TRUE(isValidNtpServer(std::string(63, 'n')));
  TEST_ASSERT_TRUE(
      isValidNtpServer(std::string(30, 'a') + "." + std::string(32, 'b')));
  TEST_ASSERT_FALSE(isValidNtpServer(""));
  TEST_ASSERT_FALSE(isValidNtpServer(std::string(64, 'n')));
  TEST_ASSERT_FALSE(isValidNtpServer(".pool.ntp.org"));
  TEST_ASSERT_FALSE(isValidNtpServer("pool.ntp.org."));
  TEST_ASSERT_FALSE(isValidNtpServer("pool..ntp.org"));
  TEST_ASSERT_FALSE(isValidNtpServer("-pool.ntp.org"));
  TEST_ASSERT_FALSE(isValidNtpServer("pool-.ntp.org"));
  TEST_ASSERT_FALSE(isValidNtpServer("pool.ntp_1.org"));
  TEST_ASSERT_FALSE(isValidNtpServer("pool ntp.org"));
  TEST_ASSERT_FALSE(isValidNtpServer("ntp.org:123"));
}

void testIdentifierAndSeasonLimits() {
  using settings::isValidIdentifier;
  using settings::isValidSeason;
  // OpenLigaDB shortcuts may contain spaces and umlauts (owner finding
  // 2026-10-12); requests percent-encode identifiers.
  for (const char* id : {"", "bl1", "BL1", "ger.1", "a_b", "78", "a-b",
                         "Bundesliga 1", "th\xc3\xbcli-26", "a:b"})
    TEST_ASSERT_TRUE(isValidIdentifier(id));
  for (const char* id : {"a\n", "a\t", "a\x7f", " bl1", "bl1 ", " "})
    TEST_ASSERT_FALSE(isValidIdentifier(id));

  using cfg::Provider;
  for (const Provider provider : {Provider::kOpenLigaDb, Provider::kApiFootball,
                                  Provider::kEspn, Provider::kFootballData}) {
    TEST_ASSERT_TRUE(isValidSeason(provider, ""));
    TEST_ASSERT_TRUE(isValidSeason(provider, "2025"));
    TEST_ASSERT_FALSE(isValidSeason(provider, "abcd"));
    TEST_ASSERT_FALSE(isValidSeason(provider, "2025/26"));
  }
  // Only OpenLigaDB takes a season of another length.
  TEST_ASSERT_TRUE(isValidSeason(Provider::kOpenLigaDb, "25"));
  TEST_ASSERT_TRUE(isValidSeason(Provider::kOpenLigaDb, "20255"));
  TEST_ASSERT_FALSE(isValidSeason(Provider::kFootballData, "25"));
  TEST_ASSERT_FALSE(isValidSeason(Provider::kApiFootball, "20255"));
  TEST_ASSERT_FALSE(isValidSeason(Provider::kEspn, "202"));
}

void testSsidCapacity() {
  Model model = defaults();
  TEST_ASSERT_TRUE(model.wifi_ssid.assign(std::string(1, 'x')));
  TEST_ASSERT_TRUE(model.wifi_ssid.assign(std::string(32, 'x')));
  TEST_ASSERT_FALSE(model.wifi_ssid.assign(std::string(33, 'y')));
  // A rejected value keeps the old one, never a truncated copy.
  TEST_ASSERT_TRUE(model.wifi_ssid.view() == std::string(32, 'x'));
  TEST_ASSERT_TRUE(model.wifi_ssid.assign(""));
  TEST_ASSERT_TRUE(model.wifi_ssid.empty());
  TEST_ASSERT_TRUE(settings::validate(model));
}

void testValidateNamesFirstInvalidField() {
  Model model = sample();
  Field invalid = Field::kWifiSsid;
  TEST_ASSERT_TRUE(settings::validate(model, &invalid));

  model.debug_status_interval_s = 3601;
  model.hostname.assign("bad_name");
  TEST_ASSERT_FALSE(settings::validate(model, &invalid));
  TEST_ASSERT_EQUAL_STRING("hostname", settings::fieldName(invalid));

  model.hostname.assign("ok");
  TEST_ASSERT_FALSE(settings::validate(model, &invalid));
  TEST_ASSERT_EQUAL_STRING("debug_status_interval_s",
                           settings::fieldName(invalid));
}

// ---- Field paths, reasons and display limits (P11.7) ----

struct Rejected {
  bool rejected;
  std::string path;
  std::string reason;
};

Rejected reject(const Model& model, const settings::Limits& limits = {}) {
  settings::Violation violation;
  const bool ok = settings::validate(model, nullptr, &violation, limits);
  return {!ok, violation.path.data(), settings::reasonName(violation.reason)};
}

Rejected reject(const Competitions& competitions) {
  settings::Violation violation;
  const bool ok = settings::validate(competitions, &violation);
  return {!ok, violation.path.data(), settings::reasonName(violation.reason)};
}

void expectViolation(const Model& model, const char* path, const char* reason,
                     const settings::Limits& limits = {}) {
  const Rejected result = reject(model, limits);
  TEST_ASSERT_TRUE(result.rejected);
  TEST_ASSERT_EQUAL_STRING(path, result.path.c_str());
  TEST_ASSERT_EQUAL_STRING(reason, result.reason.c_str());
}

void expectViolation(const Competitions& competitions, const char* path,
                     const char* reason) {
  const Rejected result = reject(competitions);
  TEST_ASSERT_TRUE(result.rejected);
  TEST_ASSERT_EQUAL_STRING(path, result.path.c_str());
  TEST_ASSERT_EQUAL_STRING(reason, result.reason.c_str());
}

void testViolationNamesScalarPath() {
  TEST_ASSERT_FALSE(reject(sample()).rejected);
  Model model = sample();
  model.hostname.assign("bad_name");
  expectViolation(model, "hostname", "invalid");

  model = sample();
  model.brightness = 101;
  expectViolation(model, "brightness", "range");

  model = sample();
  model.night_start = 1440;
  expectViolation(model, "night_start", "range");

  model = sample();
  model.language = 2;
  expectViolation(model, "language", "range");
}

void testViolationNamesListPath() {
  TEST_ASSERT_FALSE(reject(withLists()).rejected);
  Competitions store = withLists();
  store.entries[0].primary.competition.assign("");
  expectViolation(store, "competitions[0].primary.competition", "required");

  store = withLists();
  store.entries[0].primary.team.assign("bad\tid");
  expectViolation(store, "competitions[0].primary.team", "invalid");

  store = withLists();
  store.entries[0].primary.provider = static_cast<cfg::Provider>(4);
  expectViolation(store, "competitions[0].primary.provider", "range");

  // OQ-209: every stored competition has its own id; OQ-192: four screen
  // bits.
  store = withLists();
  store.entries[0].id = 0;
  expectViolation(store, "competitions[0].id", "required");

  store = withLists();
  store.entries.push(store.entries[0]);
  expectViolation(store, "competitions[1].id", "duplicate");

  store = withLists();
  store.entries[0].screens = 0x10;
  expectViolation(store, "competitions[0].screens", "range");
}

void testViolationNamesImageAndScreenPath() {
  Model model = sample();
  model.slides.push(cfg::kImageLibraryMax + 1);
  expectViolation(model, "slides[0]", "range");

  model = sample();
  model.screens[1].text_scale = 69;
  expectViolation(model, "screens[1].text_scale", "range");

  model = sample();
  model.screens[static_cast<unsigned>(cfg::Screen::kCrest)].image = 1;
  expectViolation(model, "screens[3].image", "invalid");
  TEST_ASSERT_EQUAL_UINT(3, static_cast<unsigned>(cfg::Screen::kCrest));

  model = sample();
  for (auto& style : model.screens) style.enabled = false;
  expectViolation(model, "screens", "none_enabled");
}

void testViolationKeepsFieldMapping() {
  Model model = sample();
  Field field = Field::kWifiSsid;
  model.time_zone.assign("Mars/Olympus");
  TEST_ASSERT_FALSE(settings::validate(model, &field));
  TEST_ASSERT_EQUAL_STRING("time_zone", settings::fieldName(field));
  model = sample();
  model.slides.push(99);
  TEST_ASSERT_FALSE(settings::validate(model, &field));
  TEST_ASSERT_EQUAL_STRING("application", settings::fieldName(field));
}

void testViolationFitsPathBuffer() {
  // The longest paths of a full store and of the model must not be cut.
  Competitions store = fullCompetitions();
  store.entries[7].primary.competition.assign("bad\tid");
  Rejected result = reject(store);
  TEST_ASSERT_TRUE(result.rejected);
  TEST_ASSERT_EQUAL_STRING("competitions[7].primary.competition",
                           result.path.c_str());
  TEST_ASSERT_TRUE(result.path.size() + 1 <= cfg::kFieldPathBytes);

  Model model = full();
  model.providers[3].interval_s[3] = 9;
  result = reject(model);
  TEST_ASSERT_TRUE(result.rejected);
  TEST_ASSERT_EQUAL_STRING("providers[3].intervals.live_table",
                           result.path.c_str());
  TEST_ASSERT_TRUE(result.path.size() + 1 <= cfg::kFieldPathBytes);
}

void testLimitsAreStrict() {
  const settings::Limits round{.visible_matches_max = 4, .table_window_max = 7};
  Model model = sample();
  model.visible_matches = 4;
  model.table_window = 7;
  TEST_ASSERT_FALSE(reject(model, round).rejected);
  model.visible_matches = 5;
  expectViolation(model, "visible_matches", "range", round);
  model.visible_matches = 4;
  model.table_window = 8;
  expectViolation(model, "table_window", "range", round);
  model.visible_matches = 0;
  model.table_window = 7;
  expectViolation(model, "visible_matches", "range", round);
  // Without limits the general range applies.
  model.visible_matches = 5;
  model.table_window = 9;
  TEST_ASSERT_FALSE(reject(model).rejected);
}

void testInitialValuesFollowLimits() {
  const settings::Limits round{.visible_matches_max = 4, .table_window_max = 7};
  const Model wide = initialValues({});
  TEST_ASSERT_EQUAL_UINT8(cfg::kVisibleMatchesDefault, wide.visible_matches);
  const Model narrow = initialValues({}, nullptr, round);
  TEST_ASSERT_EQUAL_UINT8(4, narrow.visible_matches);
  TEST_ASSERT_EQUAL_UINT8(cfg::kTableWindowDefault, narrow.table_window);
  TEST_ASSERT_FALSE(reject(narrow, round).rejected);
  const settings::Limits tiny{.visible_matches_max = 2, .table_window_max = 3};
  const Model small = initialValues({}, nullptr, tiny);
  TEST_ASSERT_EQUAL_UINT8(2, small.visible_matches);
  TEST_ASSERT_EQUAL_UINT8(3, small.table_window);
  TEST_ASSERT_FALSE(reject(small, tiny).rejected);
}

void testDecodeClampsToLimits() {
  const settings::Limits round{.visible_matches_max = 4, .table_window_max = 7};
  Model stored = sample();
  stored.visible_matches = 5;
  stored.table_window = 9;
  const Record record = encoded(stored);

  Model loaded = initialValues({}, nullptr, round);
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decode(record, loaded, nullptr, round));
  TEST_ASSERT_EQUAL_UINT8(4, loaded.visible_matches);
  TEST_ASSERT_EQUAL_UINT8(7, loaded.table_window);
  TEST_ASSERT_TRUE(loaded.hostname == stored.hostname);

  // Without limits the stored values stay as they are.
  Model wide = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decode(record, wide));
  TEST_ASSERT_EQUAL_UINT8(5, wide.visible_matches);
  TEST_ASSERT_EQUAL_UINT8(9, wide.table_window);

  // A value outside the general range is still damaged, not clamped.
  stored.visible_matches = 6;
  Model damaged = initialValues({}, nullptr, round);
  Field field = Field::kWifiSsid;
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue,
                    decode(encoded(stored), damaged, &field, round));
}

// OQ-122: the own-match screen is no choice while other matches run. A
// stored record from earlier firmware keeps all values and shows the
// Konferenz instead; new input with that value is rejected.
void testMatchdayScreenWithoutOwnMatch() {
  Model stored = sample();
  stored.matchday_screen = cfg::Screen::kLiveSingle;
  stored.idle_screen = cfg::Screen::kLiveSingle;
  stored.own_match_screen = cfg::Screen::kLiveSingle;
  Field field = Field::kWifiSsid;
  TEST_ASSERT_FALSE(settings::validate(stored, &field));
  TEST_ASSERT_EQUAL(Field::kApplication, field);

  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decode(encoded(stored), loaded));
  TEST_ASSERT_EQUAL(cfg::Screen::kLiveMulti, loaded.matchday_screen);
  TEST_ASSERT_EQUAL(cfg::Screen::kLiveSingle, loaded.idle_screen);
  TEST_ASSERT_EQUAL(cfg::Screen::kLiveSingle, loaded.own_match_screen);
  TEST_ASSERT_TRUE(loaded.hostname == stored.hostname);

  stored.matchday_screen = cfg::Screen::kCrest;
  TEST_ASSERT_TRUE(settings::validate(stored));
}

// OQ-123/OQ-126: format 9 stores the Konferenz pin flag after the slides,
// followed by the mode and roles, the request plans and the later tail.
void testPinOwnMatchFlag() {
  TEST_ASSERT_TRUE(defaults().pin_own_match);
  const auto pinAt = [](const Record& r) {
    return r.size() - 1 - kModeRolesBytes - kPlansBytes - kTailBytes;
  };

  Model stored = sample();
  stored.pin_own_match = false;
  Record record = encoded(stored);
  TEST_ASSERT_EQUAL_HEX8(0, record[pinAt(record)]);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == stored);
  TEST_ASSERT_EQUAL_HEX8(1, encoded(defaults())[pinAt(record)]);

  // Only 0 and 1 are a flag.
  record = encoded(stored);
  record[pinAt(record)] = 2;
  settings::seal(record);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(record, loaded));
}

// OQ-136/OQ-139/OQ-141: format 9 stores the Konferenz mode, the
// whole-matchday flag and one role per input slot after the pin flag.
void testModeAndInputRoles() {
  const Model initial = defaults();
  TEST_ASSERT_EQUAL(cfg::ConferenceMode::kAllPages, initial.conference_mode);
  TEST_ASSERT_FALSE(initial.whole_matchday);

  Model stored = sample();
  stored.pin_own_match = false;
  stored.conference_mode = cfg::ConferenceMode::kOnePage;
  stored.whole_matchday = true;
  stored.input_roles[0] = cfg::InputRole::kUp;
  stored.input_roles[1] = cfg::InputRole::kMandatory;
  stored.input_roles[2] = cfg::InputRole::kScroll;
  Record record = encoded(stored);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == stored);

  // Unknown mode, a flag other than 0/1 and an unknown role are rejected.
  const std::size_t tail =
      record.size() - kModeRolesBytes - kPlansBytes - kTailBytes;
  for (const auto& [offset, value] :
       {std::pair{std::size_t{0}, uint8_t{2}}, std::pair{std::size_t{1}, uint8_t{2}},
        std::pair{std::size_t{2 + 7}, uint8_t{5}}}) {
    Record bad = record;
    bad[tail + offset] = value;
    settings::seal(bad);
    loaded = defaults();
    TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(bad, loaded));
  }
}

// OQ-216/OQ-222/OQ-225/OQ-226: format 9 stores per provider a limit (daily
// cap, window requests per window seconds; 0 = none) and an interval per
// data type (five since the core data are gone, OQ-248/OQ-257), then a
// global limit over all providers (default off).
void testRequestPlans() {
  const Model initial = defaults();
  // The documented provider limits (app_config.h, checked 2026-10-13):
  // OpenLigaDB 60 per minute, API-Football 100 per day, ESPN none,
  // football-data.org 10 per minute.
  const std::array<RequestLimit, cfg::kProviderCount> limits{
      RequestLimit{0, 60, 60}, RequestLimit{100, 0, 0}, RequestLimit{0, 0, 0},
      RequestLimit{0, 10, 60}};
  const std::array<uint32_t, cfg::kDataTypeCount> intervals{28800, 14400, 60,
                                                            600, 600};
  for (std::size_t p = 0; p < cfg::kProviderCount; ++p) {
    TEST_ASSERT_TRUE(initial.providers[p].limit == limits[p]);
    TEST_ASSERT_TRUE(initial.providers[p].interval_s == intervals);
  }
  TEST_ASSERT_TRUE(initial.global_limit == RequestLimit{});
  const char* names[] = {"fixtures", "table", "live", "live_table", "events"};
  for (std::size_t t = 0; t < cfg::kDataTypeCount; ++t)
    TEST_ASSERT_EQUAL_STRING(
        names[t], settings::dataTypeName(static_cast<cfg::DataType>(t)));

  Model stored = sample();
  stored.providers[0].limit = {500, 30, 60};
  stored.providers[1].limit = {250, 0, 0};
  stored.providers[3].limit = {65535, 65535, 43200};
  stored.providers[2].interval_s = {604800, 3600, 30, 120, 10};
  stored.global_limit = {1000, 100, 3600};
  TEST_ASSERT_TRUE(settings::validate(stored));
  const Record record = encoded(stored);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == stored);

  // A format-9 record that ends before the plans keeps the initial ones;
  // one that ends inside a value is partial.
  Record cut = record;
  cut.resize(record.size() - kPlansBytes - kTailBytes);
  settings::seal(cut);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(cut, loaded));
  TEST_ASSERT_TRUE(loaded.providers == initial.providers);
  TEST_ASSERT_TRUE(loaded.global_limit == RequestLimit{});
  for (const std::size_t missing :
       {kTailBytes + kPlansBytes - 7, kTailBytes + 1}) {
    cut = record;
    cut.resize(record.size() - missing);
    settings::seal(cut);
    loaded = defaults();
    TEST_ASSERT_EQUAL(DecodeResult::kPartialField, decodeInto(cut, loaded));
    TEST_ASSERT_TRUE(loaded == defaults());
  }

  // Intervals 10 s to 7 days; a window needs both values.
  Model model = defaults();
  model.providers[2].interval_s[1] = 9;
  expectViolation(model, "providers[2].intervals.table", "range");
  model.providers[2].interval_s[1] = 604801;
  expectViolation(model, "providers[2].intervals.table", "range");
  model = defaults();
  model.providers[1].limit = {0, 5, 0};
  expectViolation(model, "providers[1].window_s", "required");
  model.providers[1].limit = {0, 0, 60};
  expectViolation(model, "providers[1].window_requests", "required");
  model.providers[1].limit = {0, 5, 43201};
  expectViolation(model, "providers[1].window_s", "range");
  model = defaults();
  model.global_limit = {0, 5, 0};
  expectViolation(model, "global_limit.window_s", "required");
  model.global_limit = {0, 0, 60};
  expectViolation(model, "global_limit.window_requests", "required");
  model.global_limit = {0, 1, 43201};
  expectViolation(model, "global_limit.window_s", "range");
  model.global_limit = {65535, 1, 43200};
  TEST_ASSERT_TRUE(settings::validate(model));

  // A stored interval out of range rejects the record: football-data.org's
  // events interval, right before the global limit.
  Record bad = encoded(stored);
  const std::size_t events = bad.size() - kTailBytes - 6 - 4;
  for (std::size_t i = 0; i < 4; ++i) bad[events + i] = 0;
  settings::seal(bad);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(bad, loaded));
  TEST_ASSERT_TRUE(loaded == defaults());
}

// OQ-193/OQ-196/OQ-270: the matchday screen's style, the table step time
// and the matchday mode close a format-9 record; a shorter record (without
// them) keeps their initial values, a record that ends inside them is
// partial.
void testLaterTail() {
  const Model initial = defaults();
  constexpr auto kMatchday = static_cast<std::size_t>(cfg::Screen::kMatchday);
  TEST_ASSERT_EQUAL_UINT16(8, initial.table_page_s);
  TEST_ASSERT_TRUE(initial.screens[kMatchday].enabled);
  TEST_ASSERT_EQUAL(cfg::MatchdayMode::kCurrent, initial.matchday_mode);

  Model stored = sample();
  stored.screens[kMatchday].enabled = false;
  stored.screens[kMatchday].colour = 0x010203;
  stored.screens[kMatchday].image = 2;
  stored.table_page_s = 600;
  stored.matchday_mode = cfg::MatchdayMode::kNext;
  TEST_ASSERT_TRUE(settings::validate(stored));
  const Record record = encoded(stored);
  TEST_ASSERT_TRUE(record == format9(stored));
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == stored);

  // Without the tail: everything else loads, the tail keeps its initial
  // values.
  Record cut = record;
  cut.resize(record.size() - kTailBytes);
  settings::seal(cut);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(cut, loaded));
  Model expected = stored;
  expected.screens[kMatchday] = initial.screens[kMatchday];
  expected.table_page_s = initial.table_page_s;
  expected.matchday_mode = initial.matchday_mode;
  TEST_ASSERT_TRUE(loaded == expected);
  // Only the style: the step time and the mode keep their initial values.
  cut = record;
  cut.resize(record.size() - 3);
  settings::seal(cut);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(cut, loaded));
  TEST_ASSERT_FALSE(loaded.screens[kMatchday].enabled);
  TEST_ASSERT_EQUAL_UINT16(8, loaded.table_page_s);
  TEST_ASSERT_EQUAL(cfg::MatchdayMode::kCurrent, loaded.matchday_mode);
  // A record of the firmware before the mode: it loads, the mode is
  // "current matchday" (OQ-267, OQ-270: no re-setup).
  cut = record;
  cut.resize(record.size() - 1);
  settings::seal(cut);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(cut, loaded));
  TEST_ASSERT_EQUAL_UINT16(600, loaded.table_page_s);
  TEST_ASSERT_EQUAL(cfg::MatchdayMode::kCurrent, loaded.matchday_mode);
  // Inside the style: partial.
  cut = record;
  cut.resize(record.size() - kTailBytes + 3);
  settings::seal(cut);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kPartialField, decodeInto(cut, loaded));

  // A flag other than 0/1, a step time out of range and an unknown mode are
  // rejected.
  Record bad = record;
  bad[record.size() - kTailBytes] = 2;
  settings::seal(bad);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(bad, loaded));
  bad = record;
  bad[record.size() - 3] = 2;  // 2 s
  bad[record.size() - 2] = 0;
  settings::seal(bad);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(bad, loaded));
  bad = record;
  bad[record.size() - 1] = 2;
  settings::seal(bad);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(bad, loaded));

  // Step time 3 s to 10 min.
  Model model = defaults();
  model.table_page_s = 2;
  expectViolation(model, "table_page_s", "range");
  model.table_page_s = 601;
  expectViolation(model, "table_page_s", "range");
  model.table_page_s = 3;
  TEST_ASSERT_TRUE(settings::validate(model));
}

// OQ-141/OQ-150: the initial roles follow the profile's input count; exactly
// one of the profile's inputs is mandatory, slots after them do not count.
void testInputRoles() {
  using cfg::InputRole;
  const auto roles = [](uint8_t inputs) {
    return initialValues({}, nullptr, {.inputs = inputs}).input_roles;
  };
  const settings::InputRoles none{};
  TEST_ASSERT_TRUE(roles(0) == none);
  settings::InputRoles one{};
  one[0] = InputRole::kMandatory;
  TEST_ASSERT_TRUE(roles(1) == one);
  settings::InputRoles two = one;
  two[1] = InputRole::kScroll;
  TEST_ASSERT_TRUE(roles(2) == two);
  settings::InputRoles three = one;  // C6 default (OQ-150)
  three[1] = InputRole::kUp;
  three[2] = InputRole::kDown;
  TEST_ASSERT_TRUE(roles(3) == three);
  settings::InputRoles four = three;  // the mapping before format 5
  four[3] = InputRole::kUp;
  TEST_ASSERT_TRUE(roles(4) == four);

  const settings::Limits limits{.inputs = 3};
  Model model = initialValues({}, nullptr, limits);
  TEST_ASSERT_TRUE(settings::validate(model, nullptr, nullptr, limits));
  // A slot after the profile's inputs does not count as a second mandatory.
  model.input_roles[5] = InputRole::kMandatory;
  TEST_ASSERT_TRUE(settings::validate(model, nullptr, nullptr, limits));

  settings::Violation why;
  model = initialValues({}, nullptr, limits);
  model.input_roles[0] = InputRole::kOff;
  TEST_ASSERT_FALSE(settings::validate(model, nullptr, &why, limits));
  TEST_ASSERT_EQUAL_STRING("input_roles", why.path.data());
  TEST_ASSERT_EQUAL(settings::Reason::kRequired, why.reason);

  model = initialValues({}, nullptr, limits);
  model.input_roles[2] = InputRole::kMandatory;
  TEST_ASSERT_FALSE(settings::validate(model, nullptr, &why, limits));
  TEST_ASSERT_EQUAL_STRING("input_roles", why.path.data());
  TEST_ASSERT_EQUAL(settings::Reason::kConflict, why.reason);

  model = initialValues({}, nullptr, limits);
  model.input_roles[7] = static_cast<InputRole>(5);
  TEST_ASSERT_FALSE(settings::validate(model, nullptr, &why, limits));
  TEST_ASSERT_EQUAL_STRING("input_roles[7]", why.path.data());
  TEST_ASSERT_EQUAL(settings::Reason::kRange, why.reason);

  // Every input may be switched off except the mandatory one (OQ-141).
  model = initialValues({}, nullptr, limits);
  model.input_roles[1] = model.input_roles[2] = InputRole::kOff;
  TEST_ASSERT_TRUE(settings::validate(model, nullptr, nullptr, limits));
  // Without inputs there is no mandatory rule.
  model.input_roles = {};
  TEST_ASSERT_TRUE(settings::validate(model, nullptr, nullptr, {.inputs = 0}));

  model = initialValues({}, nullptr, limits);
  model.conference_mode = static_cast<cfg::ConferenceMode>(2);
  TEST_ASSERT_FALSE(settings::validate(model, nullptr, &why, limits));
  TEST_ASSERT_EQUAL_STRING("conference_mode", why.path.data());
  model = initialValues({}, nullptr, limits);
  model.matchday_mode = static_cast<cfg::MatchdayMode>(2);
  TEST_ASSERT_FALSE(settings::validate(model, nullptr, &why, limits));
  TEST_ASSERT_EQUAL_STRING("matchday_mode", why.path.data());
}

void testValidateLists() {
  using cfg::Provider;
  const auto invalid = [](const Competitions& store) {
    return !settings::validate(store);
  };
  TEST_ASSERT_TRUE(settings::validate(withLists()));

  // OQ-195: a competition without an own club (no team).
  Competitions store = withLists();
  store.entries[0].primary.team.assign("");
  TEST_ASSERT_TRUE(settings::validate(store));

  store = withLists();
  store.entries[0].primary.competition.assign("bl\t1");
  TEST_ASSERT_TRUE(invalid(store));

  store = withLists();
  store.entries[0].primary.provider = Provider::kFootballData;
  store.entries[0].primary.season.assign("25");  // 4 digits needed
  TEST_ASSERT_TRUE(invalid(store));

  // An empty season is the current season for every provider (D3).
  store = withLists();
  store.entries[0].primary.provider = Provider::kApiFootball;
  store.entries[0].primary.season.assign("");
  TEST_ASSERT_TRUE(settings::validate(store));

  store = withLists();
  store.entries[0].competition_name.assign("a\nb");  // control character
  TEST_ASSERT_TRUE(invalid(store));
  store = withLists();
  store.entries[0].team_name.assign("a\x7f");
  TEST_ASSERT_TRUE(invalid(store));

  // Images: library ids only; the crest screen has no background image.
  const auto invalid_model = [](const Model& model) {
    Field field = Field::kWifiSsid;
    return !settings::validate(model, &field) && field == Field::kApplication;
  };
  Model model = defaults();
  model.screens[0].image = cfg::kImageLibraryMax;
  model.slides.push(0);
  model.slides.push(cfg::kImageLibraryMax);
  TEST_ASSERT_TRUE(settings::validate(model));

  model = defaults();
  model.slides.push(cfg::kImageLibraryMax + 1);
  TEST_ASSERT_TRUE(invalid_model(model));
  model = defaults();
  model.screens[2].image = cfg::kImageLibraryMax + 1;
  TEST_ASSERT_TRUE(invalid_model(model));
  model = defaults();
  model.screens[static_cast<std::size_t>(cfg::Screen::kCrest)].image = 1;
  TEST_ASSERT_TRUE(invalid_model(model));
}

// OQ-209: a new competition gets an unused id, the store's next_id first;
// next_id runs from 1 to 255 and wraps; existing ids stay.
void testAssignIds() {
  const Competition bl1 =
      competition(cfg::Provider::kOpenLigaDb, "bl1", "2025", "16");
  const auto withIds = [&](std::initializer_list<uint8_t> ids, uint8_t next) {
    Competitions store;
    for (const uint8_t id : ids) {
      Competition entry = bl1;
      entry.id = id;
      store.entries.push(entry);
    }
    store.next_id = next;
    return store;
  };
  const auto ids = [](const Competitions& store) {
    std::vector<uint8_t> out;
    for (const auto& entry : store.entries) out.push_back(entry.id);
    return out;
  };

  Competitions store = withIds({0, 5, 0}, 5);
  settings::assignIds(store);
  TEST_ASSERT_TRUE(ids(store) == (std::vector<uint8_t>{6, 5, 7}));
  TEST_ASSERT_EQUAL_UINT8(8, store.next_id);
  TEST_ASSERT_TRUE(settings::validate(store));

  store = withIds({1, 0, 0}, 255);
  settings::assignIds(store);
  TEST_ASSERT_TRUE(ids(store) == (std::vector<uint8_t>{1, 255, 2}));
  TEST_ASSERT_EQUAL_UINT8(3, store.next_id);
  TEST_ASSERT_TRUE(settings::validate(store));

  store = withIds({0}, 0);
  settings::assignIds(store);
  TEST_ASSERT_TRUE(ids(store) == (std::vector<uint8_t>{1}));
  TEST_ASSERT_EQUAL_UINT8(2, store.next_id);

  // Nothing new: the ids and next_id stay (a deleted id is not reused at
  // once).
  store = withIds({3, 9}, 12);
  settings::assignIds(store);
  TEST_ASSERT_TRUE(ids(store) == (std::vector<uint8_t>{3, 9}));
  TEST_ASSERT_EQUAL_UINT8(12, store.next_id);
}

// Which saves keep the fetched data. Proves only the comparison; the device
// evidence (a brightness save without a provider request and without an
// empty screen) is separate.
// A save through the REST API refuses an active source whose provider has
// no key or no opt-in, and names its provider field.
void testProviderAccess() {
  Model model = sample();
  Competitions store = withLists();
  store.entries[0].primary.provider = cfg::Provider::kFootballData;
  model.football_data_key.assign("key");
  model.espn_opt_in = false;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model, store));

  settings::Violation why;
  model.football_data_key.assign("");
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model, store, &why));
  TEST_ASSERT_EQUAL_STRING("competitions[0].primary.provider", why.path.data());
  TEST_ASSERT_TRUE(why.reason == settings::Reason::kConflict);
  // An inactive competition is not requested.
  store.entries[0].active = false;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model, store));
  store.entries[0].active = true;

  model = sample();
  store = withLists();
  model.football_data_key.assign("key");
  store.entries[0].primary.provider = cfg::Provider::kEspn;
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model, store, &why));
  TEST_ASSERT_EQUAL_STRING("competitions[0].primary.provider",
                           why.path.data());
  model.espn_opt_in = true;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model, store));

  Competition second = store.entries[0];
  second.id = 2;
  second.primary.provider = cfg::Provider::kApiFootball;
  store.entries.push(second);
  model.api_football_key.assign("");
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model, store, &why));
  TEST_ASSERT_EQUAL_STRING("competitions[1].primary.provider",
                           why.path.data());
  model.api_football_key.assign("key");
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model, store));

  // OpenLigaDB needs neither.
  model = sample();
  store = withLists();
  model.api_football_key.assign("");
  model.football_data_key.assign("");
  model.espn_opt_in = false;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model, store));
  // validate() does not apply this rule, so a stored older record still
  // loads.
  store.entries[0].primary.provider = cfg::Provider::kFootballData;
  TEST_ASSERT_TRUE(settings::validate(model));
  TEST_ASSERT_TRUE(settings::validate(store));
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model, store));
}

void testSameProviderData() {
  const Model base = sample();
  Model other = base;
  other.brightness = static_cast<uint8_t>(base.brightness == 1 ? 2 : 1);
  other.language = static_cast<uint8_t>(1 - base.language);
  other.window_before = static_cast<uint16_t>(base.window_before + 1);
  other.providers[1].limit.daily =
      static_cast<uint16_t>(base.providers[1].limit.daily + 1);
  other.providers[0].interval_s[3] = base.providers[0].interval_s[3] + 1;
  other.global_limit = {10, 1, 60};
  TEST_ASSERT_TRUE(settings::sameProviderData(base, other));

  other = base;
  other.api_football_key.assign("key");
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
  other = base;
  other.football_data_key.assign("key");
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
  other = base;
  other.espn_opt_in = !base.espn_opt_in;
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
  other = base;
  other.demo = !base.demo;
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));

  const Competitions store = withLists();
  Competitions changed = store;
  changed.entries[0].competition_name.assign("Erste Liga");
  changed.entries[0].team_name.assign("Other name");
  changed.entries[0].screens = 0x01;
  changed.next_id = 7;
  TEST_ASSERT_TRUE(settings::sameProviderData(store, changed));

  changed = store;
  changed.entries[0].primary.season.assign("");
  TEST_ASSERT_FALSE(settings::sameProviderData(store, changed));
  changed = store;
  changed.entries[0].primary.team.assign("6");
  TEST_ASSERT_FALSE(settings::sameProviderData(store, changed));
  changed = store;
  changed.entries[0].active = false;
  TEST_ASSERT_FALSE(settings::sameProviderData(store, changed));
  changed = store;
  changed.entries[0].id = 3;
  TEST_ASSERT_FALSE(settings::sameProviderData(store, changed));
  changed = store;
  changed.entries.push(store.entries[0]);
  TEST_ASSERT_FALSE(settings::sameProviderData(store, changed));
}

void testDropMissingImages() {
  Model model = defaults();
  model.screens[0].image = 2;
  model.screens[1].image = 5;
  for (const uint8_t slide : {0, 3, 7, 2, 5}) model.slides.push(slide);

  // Images 2 and 3 exist: 5 and 7 go, the crest slide 0 and the order stay.
  const settings::ImageMask present = (1u << 2) | (1u << 3);
  TEST_ASSERT_TRUE(settings::dropMissingImages(model, present));
  TEST_ASSERT_EQUAL_UINT8(2, model.screens[0].image);
  TEST_ASSERT_EQUAL_UINT8(cfg::kStockImageId, model.screens[1].image);
  TEST_ASSERT_EQUAL_size_t(3, model.slides.size());
  TEST_ASSERT_EQUAL_UINT8(0, model.slides[0]);
  TEST_ASSERT_EQUAL_UINT8(3, model.slides[1]);
  TEST_ASSERT_EQUAL_UINT8(2, model.slides[2]);
  TEST_ASSERT_TRUE(settings::validate(model));
  // Nothing more to do on a second run.
  TEST_ASSERT_FALSE(settings::dropMissingImages(model, present));

  // No image left: the stock image and the crest slide remain.
  TEST_ASSERT_TRUE(settings::dropMissingImages(model, 0));
  TEST_ASSERT_EQUAL_UINT8(cfg::kStockImageId, model.screens[0].image);
  TEST_ASSERT_EQUAL_size_t(1, model.slides.size());
  TEST_ASSERT_EQUAL_UINT8(0, model.slides[0]);
  TEST_ASSERT_FALSE(settings::dropMissingImages(model, 0));

  // An id outside the library is missing whatever the mask says; bit 0 (the
  // stock image) is not a library image.
  model = defaults();
  model.screens[0].image = 200;
  model.slides.push(11);
  TEST_ASSERT_TRUE(settings::dropMissingImages(model, 0xFFFF));
  TEST_ASSERT_EQUAL_UINT8(cfg::kStockImageId, model.screens[0].image);
  TEST_ASSERT_TRUE(model.slides.empty());

  // An unchanged model reports no change.
  model = defaults();
  TEST_ASSERT_FALSE(settings::dropMissingImages(model, 0));
}

void testPresetsOverrideDefaults() {
  const Presets presets{
      .wifi_ssid = "Preset Net",
      .wifi_password = "preset-pass",
      .ap_password = "ap-preset",
      .hostname = "preset-host",
      .time_zone = "Asia/Tokyo",
  };
  FieldMask rejected = 0xFFFF;
  const Model model = initialValues(presets, &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_TRUE(model.wifi_ssid.view() == "Preset Net");
  TEST_ASSERT_TRUE(model.wifi_password.view() == "preset-pass");
  TEST_ASSERT_TRUE(model.ap_password.view() == "ap-preset");
  TEST_ASSERT_TRUE(model.hostname.view() == "preset-host");
  TEST_ASSERT_TRUE(model.time_zone.view() == "Asia/Tokyo");
  TEST_ASSERT_EQUAL_UINT16(30, model.debug_status_interval_s);
}

void testInvalidPresetsKeepDefaults() {
  const Presets presets{
      .wifi_ssid = std::string_view("123456789012345678901234567890123"),
      .wifi_password = "short",
      .ap_password = "tiny",
      .hostname = "bad_host",
      // A POSIX rule as in the earlier SECRET_TIMEZONE: not a label.
      .time_zone = "CET-1CEST,M3.5.0,M10.5.0/3",
  };
  FieldMask rejected = 0;
  const Model model = initialValues(presets, &rejected);
  TEST_ASSERT_EQUAL_UINT32(
      fieldBit(Field::kWifiSsid) | fieldBit(Field::kWifiPassword) |
          fieldBit(Field::kApPassword) | fieldBit(Field::kHostname) |
          fieldBit(Field::kTimeZone),
      rejected);
  TEST_ASSERT_TRUE(model == defaults());
  TEST_ASSERT_TRUE(settings::validate(model));
}

void testPresetsCreateCompetition() {
  const Presets presets{.provider = "football-data",
                        .competition = "BL1",
                        .team = "5",
                        .season = "2025"};
  FieldMask rejected = 0xFFFF;
  Competitions store = fullCompetitions();  // replaced
  initialCompetitions(presets, store, &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_EQUAL_size_t(1, store.entries.size());
  TEST_ASSERT_EQUAL_UINT8(2, store.next_id);
  const Competition& entry = store.entries[0];
  TEST_ASSERT_EQUAL_UINT8(1, entry.id);
  TEST_ASSERT_TRUE(entry.active);
  TEST_ASSERT_EQUAL_HEX8(cfg::kCompetitionScreensDefault, entry.screens);
  TEST_ASSERT_TRUE(entry.primary.provider == cfg::Provider::kFootballData);
  TEST_ASSERT_TRUE(entry.primary.competition.view() == "BL1");
  TEST_ASSERT_TRUE(entry.primary.team.view() == "5");
  TEST_ASSERT_TRUE(entry.primary.season.view() == "2025");
  TEST_ASSERT_TRUE(settings::validate(store));
  // The settings model holds no competitions; the presets set no plan.
  TEST_ASSERT_TRUE(initialValues(presets) == defaults());

  // No provider named: the default provider.
  initialCompetitions({.competition = "bl1", .team = "16"}, store, &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_EQUAL_size_t(1, store.entries.size());
  TEST_ASSERT_TRUE(store.entries[0].primary.provider == cfg::kProviderDefault);

  // OQ-195: a competition without a team is an entry without an own club.
  initialCompetitions({.competition = "bl1"}, store, &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_EQUAL_size_t(1, store.entries.size());
  TEST_ASSERT_FALSE(store.entries[0].hasClub());
  TEST_ASSERT_TRUE(settings::validate(store));

  // A team without a competition is no entry.
  initialCompetitions({.team = "16"}, store, &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_TRUE(store.entries.empty());

  // An invalid identifier is skipped and reported.
  initialCompetitions({.competition = "bad\tid", .team = "16"}, store,
                      &rejected);
  TEST_ASSERT_EQUAL_UINT32(fieldBit(Field::kApplication), rejected);
  TEST_ASSERT_TRUE(store == Competitions{});
}

void testStoredValuesOverridePresets() {
  const Presets presets{.wifi_ssid = "Preset Net", .hostname = "preset-host"};
  const Model stored = sample();
  Model loaded = initialValues(presets);
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(encoded(stored), loaded));
  TEST_ASSERT_TRUE(loaded == stored);

  // The stored competition store wins over the preset competition, even
  // when empty.
  const Presets with_competition{.provider = "football-data",
                                 .competition = "BL1",
                                 .team = "5",
                                 .season = "2025"};
  Competitions store;
  initialCompetitions(with_competition, store);
  TEST_ASSERT_EQUAL_size_t(1, store.entries.size());
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decodeStore(encodedCompetitions({}), store));
  TEST_ASSERT_TRUE(store.entries.empty());
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testActualUnityVersion);
  RUN_TEST(testCrc32KnownVector);
  RUN_TEST(testDefaults);
  RUN_TEST(testListContainer);
  RUN_TEST(testFormat9Layout);
  RUN_TEST(testCompetitionRecordLayout);
  RUN_TEST(testFullModelUsesTheMaximumRecord);
  RUN_TEST(testRoundTrip);
  RUN_TEST(testShorterRecordKeepsInitialValues);
  RUN_TEST(testPartialFieldIsRejected);
  RUN_TEST(testListsPartialAndInvalid);
  RUN_TEST(testLongerRecordIgnoresTail);
  RUN_TEST(testDamagedHeaders);
  RUN_TEST(testInvalidStoredValuesRejectWholeRecord);
  RUN_TEST(testWifiPasswordLimits);
  RUN_TEST(testApPasswordLimits);
  RUN_TEST(testHostnameLimits);
  RUN_TEST(testStatusIntervalLimits);
  RUN_TEST(testTimeZoneLimits);
  RUN_TEST(testNtpServerLimits);
  RUN_TEST(testIdentifierAndSeasonLimits);
  RUN_TEST(testSsidCapacity);
  RUN_TEST(testValidateNamesFirstInvalidField);
  RUN_TEST(testValidateLists);
  RUN_TEST(testAssignIds);
  RUN_TEST(testViolationNamesScalarPath);
  RUN_TEST(testViolationNamesListPath);
  RUN_TEST(testViolationNamesImageAndScreenPath);
  RUN_TEST(testViolationKeepsFieldMapping);
  RUN_TEST(testViolationFitsPathBuffer);
  RUN_TEST(testLimitsAreStrict);
  RUN_TEST(testInitialValuesFollowLimits);
  RUN_TEST(testDecodeClampsToLimits);
  RUN_TEST(testMatchdayScreenWithoutOwnMatch);
  RUN_TEST(testPinOwnMatchFlag);
  RUN_TEST(testModeAndInputRoles);
  RUN_TEST(testInputRoles);
  RUN_TEST(testRequestPlans);
  RUN_TEST(testLaterTail);
  RUN_TEST(testSameProviderData);
  RUN_TEST(testProviderAccess);
  RUN_TEST(testDropMissingImages);
  RUN_TEST(testPresetsOverrideDefaults);
  RUN_TEST(testInvalidPresetsKeepDefaults);
  RUN_TEST(testPresetsCreateCompetition);
  RUN_TEST(testStoredValuesOverridePresets);
  return UNITY_END();
}
