// Host acceptance for the settings model, its limits, the initial values and
// the stored record: format 6 (variable length, lists, Konferenz pin flag,
// Konferenz mode, whole-matchday flag, input roles) and the conversion of the
// older formats 1 to 5, including the crest image id of formats 3 to 5
// (docs/CONFIGURATION.md).

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
using settings::decode;
using settings::DecodeResult;
using settings::encode;
using settings::Field;
using settings::fieldBit;
using settings::FieldMask;
using settings::FixturePair;
using settings::initialValues;
using settings::kHeaderBytes;
using settings::kMaxRecordBytes;
using settings::Model;
using settings::Presets;

// A record has exactly its used length (format 6 varies, formats 1 and 2 are
// built by the tests themselves, formats 3 to 5 from format 6 by
// olderFormat()).
using Record = std::vector<uint8_t>;

// Offsets of the record, header included, written out independently of the
// implementation. The core (format 1) comes first in every format.
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
// Format 3 continues with the secrets, the screens, the scalar values, the
// crest image and then the lists; format 4 appends the pin flag, format 5
// the Konferenz mode, the whole-matchday flag and the input roles; format 6
// drops the crest image, so its lists start where the crest image was.
constexpr std::size_t kAdminAt = kCoreBytes;
constexpr std::size_t kApiKeyAt = kAdminAt + 1 + 64;
constexpr std::size_t kFootballKeyAt = kApiKeyAt + 1 + 96;
constexpr std::size_t kScreensAt = kFootballKeyAt + 1 + 64;
constexpr std::size_t kScreenBytes = 16;
constexpr std::size_t kScalarsAt = kScreensAt + 4 * kScreenBytes;
constexpr std::size_t kLanguageAt = kScalarsAt + 7;
constexpr std::size_t kSlideshowAt = kScalarsAt + 16 + 2 * 3;
constexpr std::size_t kCrestAt = kScalarsAt + 7 + 9 + 2 * 10;  // formats 3-5
constexpr std::size_t kSlidesAt = kCrestAt;  // format 6: count, then one byte each
constexpr std::size_t kSourceBytes = 1 + 33 + 17 + 33;
constexpr std::size_t kCompetitionBytes = 2 + 2 * kSourceBytes + 33 + 33;
constexpr std::size_t kPairBytes = 3 + 33 + 33;
// Format 5 and 6 tail after the pin flag: mode, whole matchday, eight input roles.
constexpr std::size_t kFormat5TailBytes = 1 + 1 + 8;
// Lists empty plus the pin flag and the format-5 tail: the shortest record.
constexpr std::size_t kEmptyListsBytes = kSlidesAt + 3 + 1 + kFormat5TailBytes;
// The model is copied onto task stacks (settings::current(), HTTP handlers):
// growth is a stack budget change, see docs/ARCHITECTURE.md. Format 5 added
// 10 bytes of fields and 2 bytes of padding (1,944 -> 1,956).
constexpr std::size_t kModelBytesBudget = 1956;

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

// One competition with an active fallback and one fixture pair.
Model withLists() {
  Model model = sample();
  Competition entry = competition(cfg::Provider::kOpenLigaDb, "bl1", "2025", "16");
  entry.fallback_active = true;
  entry.fallback.provider = cfg::Provider::kFootballData;
  entry.fallback.competition.assign("BL1");
  entry.fallback.season.assign("2025");
  entry.fallback.team.assign("5");
  entry.competition_name.assign("Bundesliga");
  entry.team_name.assign("FC Test");
  model.competitions.push(entry);
  FixturePair pair;
  pair.primary_id.assign("100");
  pair.secondary_id.assign("200");
  model.fixture_pairs.push(pair);
  return model;
}

// Every list full, every text at its capacity.
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
  while (!model.competitions.full()) {
    Competition entry;
    entry.fallback_active = true;
    entry.primary.provider = cfg::Provider::kOpenLigaDb;
    entry.primary.competition.assign(std::string(32, 'c'));
    entry.primary.season.assign(std::string(16, '9'));
    entry.primary.team.assign(std::string(32, 't'));
    entry.fallback.provider = cfg::Provider::kApiFootball;
    entry.fallback.competition.assign(std::string(32, 'f'));
    entry.fallback.season.assign("2025");
    entry.fallback.team.assign(std::string(32, 'u'));
    entry.competition_name.assign(std::string(32, 'N'));
    entry.team_name.assign(std::string(32, 'T'));
    model.competitions.push(entry);
  }
  for (std::size_t i = 0; !model.fixture_pairs.full(); ++i) {
    FixturePair pair;
    pair.competition = static_cast<uint8_t>(i % model.competitions.size());
    pair.swapped = i % 2 == 1;
    pair.primary_id.assign(std::string(31, 'x') + static_cast<char>('a' + i));
    pair.secondary_id.assign(std::string(31, 'y') + static_cast<char>('a' + i));
    model.fixture_pairs.push(pair);
  }
  return model;
}

Record encoded(const Model& model) {
  std::array<uint8_t, kMaxRecordBytes> buffer{};
  const std::size_t used = encode(model, buffer);
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

// The record of format 3, 4 or 5 for the format-6 bytes 'current' of the
// same model: the crest image id before the lists, without the fields the
// format did not have yet.
Record olderFormat(const Record& current, uint16_t version,
                   uint8_t crest = cfg::kStockImageId) {
  std::size_t end = current.size();
  if (version < 5) end -= kFormat5TailBytes;
  if (version < 4) end -= 1;  // the pin flag
  Record record(current.begin(), current.begin() + kCrestAt);
  record.push_back(crest);
  record.insert(record.end(), current.begin() + kCrestAt,
                current.begin() + end);
  settings::seal(record, version);
  return record;
}

// Record with one payload byte changed and a valid header, so the value
// check (not the CRC) has to catch it.
Record withByte(const Model& model, std::size_t at, uint8_t value) {
  Record record = encoded(model);
  record[at] = value;
  settings::seal(record);
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

// Builds the records of the older formats from scratch, byte by byte.
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

// 7 flags, 9 u8 values and 10 u16 values: formats 2 and 3.
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
      .u16(model.window_after)
      .u16(model.api_daily_budget);
}

// Format 2 kept three routes, their fallbacks and eight fixture mappings as
// fixed slots with an enabled flag each.
struct SlotSource {
  bool enabled = false;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  std::string_view competition, season, team;
};

struct SlotMapping {
  bool enabled = false;
  uint8_t route = 0;
  bool swapped = false;
  std::string_view primary, secondary;
};

void slotSource(Out& out, const SlotSource& source) {
  out.u8(static_cast<uint8_t>(source.provider))
      .text(source.competition, 32)
      .text(source.season, 16)
      .text(source.team, 32);
}

Record format2(const Model& base, const std::array<SlotSource, 3>& routes,
               const std::array<SlotSource, 3>& fallbacks,
               const std::array<SlotMapping, 8>& mappings) {
  Out out;
  core(out, base);
  secrets(out, base);
  for (const auto& route : routes) {
    out.u8(route.enabled);
    slotSource(out, route);
  }
  for (const auto& style : base.screens)
    out.u8(style.enabled)
        .u8(style.background)
        .u32(style.colour)
        .u32(style.text)
        .u32(style.accent)
        .u8(style.text_scale);
  scalars(out, base);
  for (const auto& fallback : fallbacks) {
    out.u8(fallback.enabled);
    slotSource(out, fallback);
  }
  for (const auto& mapping : mappings)
    out.u8(mapping.enabled)
        .u8(mapping.route)
        .u8(mapping.swapped)
        .text(mapping.primary, 32)
        .text(mapping.secondary, 32);
  return out.record(2);
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
  TEST_ASSERT_TRUE(model.competitions.empty());
  TEST_ASSERT_TRUE(model.fixture_pairs.empty());
  TEST_ASSERT_TRUE(model.slides.empty());
  for (const auto& style : model.screens)
    TEST_ASSERT_EQUAL_UINT8(cfg::kStockImageId, style.image);
  TEST_ASSERT_TRUE(settings::validate(model));
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

void testFormat6Layout() {
  TEST_ASSERT_EQUAL_size_t(8, kHeaderBytes);
  // Pinned: the longest record is the NVS blob ceiling, a memory budget item.
  TEST_ASSERT_EQUAL_size_t(1941, kMaxRecordBytes);
  TEST_ASSERT_TRUE(kMaxRecordBytes <= cfg::kSettingsMaxRecordBytes);
  TEST_ASSERT_EQUAL_size_t(676, kEmptyListsBytes);

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
  Competition entry = competition(cfg::Provider::kOpenLigaDb, "bl1", "2025", "16");
  entry.active = false;
  entry.fallback_active = true;
  entry.fallback.provider = cfg::Provider::kFootballData;
  entry.fallback.competition.assign("BL1");
  entry.fallback.season.assign("2025");
  entry.fallback.team.assign("5");
  entry.competition_name.assign("Liga");
  entry.team_name.assign("Club");
  model.competitions.push(entry);
  FixturePair pair;
  pair.swapped = true;
  pair.primary_id.assign("100");
  pair.secondary_id.assign("200");
  model.fixture_pairs.push(pair);
  model.conference_mode = cfg::ConferenceMode::kOnePage;
  model.whole_matchday = true;
  model.input_roles = {cfg::InputRole::kScroll, cfg::InputRole::kOff,
                       cfg::InputRole::kMandatory, cfg::InputRole::kDown,
                       cfg::InputRole::kUp,     cfg::InputRole::kOff,
                       cfg::InputRole::kOff,    cfg::InputRole::kOff};
  TEST_ASSERT_TRUE(settings::validate(model));

  const Record record = encoded(model);
  const std::size_t expected =
      kEmptyListsBytes + 2 + kCompetitionBytes + kPairBytes;
  TEST_ASSERT_EQUAL_size_t(expected, record.size());

  TEST_ASSERT_EQUAL_HEX8(0x06, record[0]);  // version 6
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
  const uint8_t colours[] = {0x33, 0x22, 0x11, 0x00, 0x66, 0x55,
                             0x44, 0x00, 0x99, 0x88, 0x77, 0x00};
  for (std::size_t i = 0; i < sizeof(colours); ++i)
    TEST_ASSERT_EQUAL_HEX8(colours[i], record[second + 2 + i]);
  TEST_ASSERT_EQUAL_UINT8(120, record[second + 14]);
  TEST_ASSERT_EQUAL_UINT8(3, record[second + 15]);
  TEST_ASSERT_EQUAL_HEX8(1, record[kLanguageAt]);
  TEST_ASSERT_EQUAL_HEX8(0x23, record[kSlideshowAt]);
  TEST_ASSERT_EQUAL_HEX8(0x01, record[kSlideshowAt + 1]);

  // The lists, each a count followed by its entries, right after the
  // scalar values.
  TEST_ASSERT_EQUAL_UINT8(2, record[kSlidesAt]);
  TEST_ASSERT_EQUAL_UINT8(0, record[kSlidesAt + 1]);
  TEST_ASSERT_EQUAL_UINT8(3, record[kSlidesAt + 2]);
  const std::size_t competitions = kSlidesAt + 3;
  TEST_ASSERT_EQUAL_UINT8(1, record[competitions]);
  const std::size_t at = competitions + 1;
  TEST_ASSERT_EQUAL_HEX8(0, record[at]);      // active
  TEST_ASSERT_EQUAL_HEX8(1, record[at + 1]);  // fallback_active
  const std::size_t primary = at + 2;
  TEST_ASSERT_EQUAL_UINT8(0, record[primary]);  // OpenLigaDB
  expectText(record, primary + 1, "bl1", 32);
  expectText(record, primary + 1 + 33, "2025", 16);
  expectText(record, primary + 1 + 33 + 17, "16", 32);
  const std::size_t fallback = primary + kSourceBytes;
  TEST_ASSERT_EQUAL_UINT8(3, record[fallback]);  // football-data.org
  expectText(record, fallback + 1, "BL1", 32);
  expectText(record, fallback + 1 + 33, "2025", 16);
  expectText(record, fallback + 1 + 33 + 17, "5", 32);
  expectText(record, fallback + kSourceBytes, "Liga", 32);
  expectText(record, fallback + kSourceBytes + 33, "Club", 32);
  const std::size_t pairs = at + kCompetitionBytes;
  TEST_ASSERT_EQUAL_UINT8(1, record[pairs]);
  TEST_ASSERT_EQUAL_HEX8(1, record[pairs + 1]);  // active
  TEST_ASSERT_EQUAL_HEX8(1, record[pairs + 2]);  // swapped
  TEST_ASSERT_EQUAL_UINT8(0, record[pairs + 3]);  // competition index
  expectText(record, pairs + 4, "100", 32);
  expectText(record, pairs + 4 + 33, "200", 32);
  // Format 4: the pin flag after the lists (default on).
  const std::size_t pin = pairs + 1 + kPairBytes;
  TEST_ASSERT_EQUAL_HEX8(1, record[pin]);
  // Formats 5 and 6: mode, whole matchday and one role byte per input slot
  // close the record.
  TEST_ASSERT_EQUAL_HEX8(1, record[pin + 1]);  // One page
  TEST_ASSERT_EQUAL_HEX8(1, record[pin + 2]);  // whole matchday on
  const uint8_t roles[] = {4, 0, 1, 3, 2, 0, 0, 0};
  for (std::size_t i = 0; i < sizeof(roles); ++i)
    TEST_ASSERT_EQUAL_HEX8(roles[i], record[pin + 3 + i]);
  TEST_ASSERT_EQUAL_size_t(pin + 1 + kFormat5TailBytes, record.size());

  // CRC over header bytes 0..3 followed by the payload.
  std::vector<uint8_t> covered(record.begin(), record.begin() + 4);
  covered.insert(covered.end(), record.begin() + kHeaderBytes, record.end());
  TEST_ASSERT_EQUAL_HEX32(settings::crc32(covered), readU32(&record[4]));

  // The record length follows the lists; encode() reports it.
  std::array<uint8_t, kMaxRecordBytes> buffer{};
  TEST_ASSERT_EQUAL_size_t(expected, encode(model, buffer));
  for (std::size_t i = expected; i < buffer.size(); ++i)
    TEST_ASSERT_EQUAL_HEX8(0, buffer[i]);
  TEST_ASSERT_EQUAL_size_t(kEmptyListsBytes, encoded(defaults()).size());
}

void testFullModelUsesTheMaximumRecord() {
  const Model model = full();
  TEST_ASSERT_TRUE(settings::validate(model));
  TEST_ASSERT_TRUE(model.competitions.full());
  TEST_ASSERT_TRUE(model.fixture_pairs.full());
  TEST_ASSERT_TRUE(model.slides.full());

  std::array<uint8_t, kMaxRecordBytes> buffer{};
  TEST_ASSERT_EQUAL_size_t(kMaxRecordBytes, encode(model, buffer));
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(buffer, loaded));
  TEST_ASSERT_TRUE(loaded == model);

  TEST_ASSERT_TRUE(sizeof(Model) <= kModelBytesBudget);
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

  // Lists, images, screens and the scalar values.
  Model lists = withLists();
  lists.admin_password.assign("admin");
  lists.api_football_key.assign("key-1");
  lists.football_data_key.assign("key-2");
  lists.screens[0].image = 7;
  lists.screens[1].colour = 0xabcdef;
  lists.slides.push(0);
  lists.slides.push(4);
  lists.slides.push(9);
  lists.language = 1;
  lists.espn_opt_in = true;
  lists.slideshow_s = 3600;
  FixturePair second;
  second.active = false;
  second.swapped = true;
  second.competition = 0;
  second.primary_id.assign("7");
  second.secondary_id.assign("8");
  lists.fixture_pairs.push(second);
  TEST_ASSERT_TRUE(settings::validate(lists));
  Model loaded_lists = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decodeInto(encoded(lists), loaded_lists));
  TEST_ASSERT_TRUE(loaded_lists == lists);
  TEST_ASSERT_EQUAL_size_t(1, loaded_lists.competitions.size());
  TEST_ASSERT_EQUAL_size_t(2, loaded_lists.fixture_pairs.size());
  TEST_ASSERT_EQUAL_size_t(3, loaded_lists.slides.size());

  // A stored empty list replaces a non-empty initial list.
  Model initial = withLists();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(encoded(sample()), initial));
  TEST_ASSERT_TRUE(initial.competitions.empty());
  TEST_ASSERT_TRUE(initial.fixture_pairs.empty());
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

  // A format-3 record that ends after the core keeps the initial secrets and
  // lists; one that ends after the scalar values keeps the initial lists.
  const Presets presets{.admin_password = "preset-admin",
                        .provider = "football-data",
                        .competition = "BL1",
                        .team = "5",
                        .season = "2025"};
  const Model initial = initialValues(presets);
  TEST_ASSERT_EQUAL_size_t(1, initial.competitions.size());
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
  TEST_ASSERT_TRUE(loaded.competitions == initial.competitions);

  record = encoded(later);
  const auto before_lists = std::span(record).first(kSlidesAt);
  settings::seal(before_lists);
  loaded = initial;
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(before_lists, loaded));
  TEST_ASSERT_EQUAL_UINT8(1, loaded.language);
  TEST_ASSERT_EQUAL_UINT16(20, loaded.slideshow_s);
  TEST_ASSERT_TRUE(loaded.competitions == initial.competitions);
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
  const Model original = withLists();
  const Record base = encoded(original);
  // No slides: the competition count follows the slide count directly.
  const std::size_t competitions = kSlidesAt + 1;
  const std::size_t entry = competitions + 1;
  const std::size_t pairs = entry + kCompetitionBytes;
  TEST_ASSERT_EQUAL_UINT8(1, base[competitions]);
  TEST_ASSERT_EQUAL_UINT8(1, base[pairs]);

  // A list ends cleanly before its count (older record): OK, later lists keep
  // their initial value.
  for (const std::size_t size : {kSlidesAt, competitions, pairs}) {
    Record record = base;
    const auto cut = std::span(record).first(size);
    settings::seal(cut);
    Model loaded = withLists();
    TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(cut, loaded));
  }
  // A count without its entries, or entries cut in the middle: partial.
  for (const std::size_t size :
       {competitions + 1, entry + 1, entry + 100, entry + kCompetitionBytes - 1,
        pairs + 1, pairs + 1 + kPairBytes - 1}) {
    Record record = base;
    const auto cut = std::span(record).first(size);
    settings::seal(cut);
    Model loaded = defaults();
    TEST_ASSERT_EQUAL(DecodeResult::kPartialField, decodeInto(cut, loaded));
    TEST_ASSERT_TRUE(loaded == defaults());
  }

  // Counts above the capacity reject the record.
  struct Case {
    std::size_t at;
    uint8_t value;
  };
  const Case counts[] = {
      {kSlidesAt, cfg::kSlideMax + 1},
      {competitions, cfg::kCompetitionMax + 1},
      {pairs, cfg::kFixtureMappingMax + 1},
      {kSlidesAt, 255},
  };
  for (const Case& c : counts) {
    const Record record = withByte(original, c.at, c.value);
    Model loaded = defaults();
    Field invalid = Field::kWifiSsid;
    TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue,
                      decodeInto(record, loaded, &invalid));
    TEST_ASSERT_EQUAL_STRING("application", settings::fieldName(invalid));
    TEST_ASSERT_TRUE(loaded == defaults());
  }

  // Stored values that break the list rules: the crest screen with an image,
  // an image id above the library, a competition without a team.
  const std::size_t crest_screen_image =
      kScreensAt + 3 * kScreenBytes + 15;
  const std::size_t first_screen_image = kScreensAt + 15;
  const Case values[] = {
      {crest_screen_image, 1},
      {first_screen_image, cfg::kImageLibraryMax + 1},
  };
  for (const Case& c : values) {
    const Record record = withByte(sample(), c.at, c.value);
    Model loaded = defaults();
    Field invalid = Field::kWifiSsid;
    TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue,
                      decodeInto(record, loaded, &invalid));
    TEST_ASSERT_EQUAL_STRING("application", settings::fieldName(invalid));
  }
  Model loaded = defaults();
  const std::size_t primary_team = entry + 2 + 1 + 33 + 17;
  const Record no_team = withByte(original, primary_team, 0);
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(no_team, loaded));
  TEST_ASSERT_TRUE(loaded == defaults());
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

  // Formats 1 to 6 are known; any other version is not.
  record = encoded(original);
  for (const uint16_t version : {uint16_t{0}, uint16_t{7}, uint16_t{0xFFFF}}) {
    settings::seal(record, version);
    TEST_ASSERT_EQUAL(DecodeResult::kUnknownVersion,
                      decodeInto(record, loaded));
  }

  TEST_ASSERT_TRUE(loaded == defaults());
}

void testFormat1IsMigrated() {
  // The record of the first firmware: the core only, 335 bytes.
  const Model stored = sample();
  Out out;
  core(out, stored);
  TEST_ASSERT_EQUAL_size_t(kCoreBytes - kHeaderBytes, out.size());
  Record record = out.record(1);
  TEST_ASSERT_EQUAL_size_t(kCoreBytes, record.size());

  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == stored);  // everything else is initial

  // Settings the format never had keep the initial values, lists included.
  const Presets presets{.admin_password = "preset-admin",
                        .provider = "football-data",
                        .competition = "BL1",
                        .team = "5",
                        .season = "2025"};
  const Model initial = initialValues(presets);
  loaded = initial;
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded.hostname == stored.hostname);
  TEST_ASSERT_TRUE(loaded.admin_password.view() == "preset-admin");
  TEST_ASSERT_TRUE(loaded.competitions == initial.competitions);

  // Only the core counts; a longer legacy record is read up to 1024 bytes.
  record.resize(1024, 0xA5);
  settings::seal(record, 1);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == stored);
  record.push_back(0xA5);
  settings::seal(record, 1);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kTooLong, decodeInto(record, loaded));

  // The next save writes format 6 with the same values.
  const Record saved = encoded(stored);
  TEST_ASSERT_EQUAL_HEX8(6, saved[0]);
}

void testFormat2IsConvertedToLists() {
  Model base = sample();
  base.admin_password.assign("secret");
  base.api_football_key.assign("key-1");
  base.screens[0].colour = 0x102030;
  base.screens[2].enabled = false;
  base.espn_opt_in = true;
  base.language = 1;
  base.slideshow_s = 20;

  std::array<SlotSource, 3> routes{}, fallbacks{};
  std::array<SlotMapping, 8> mappings{};
  // Route 0 is an empty slot: it disappears and the later routes move up.
  routes[1] = {.enabled = true,
               .provider = cfg::Provider::kOpenLigaDb,
               .competition = "bl1",
               .season = "2025",
               .team = "16"};
  routes[2] = {.enabled = false,  // paused, but complete: it stays
               .provider = cfg::Provider::kFootballData,
               .competition = "BL1",
               .season = "2025",
               .team = "5"};
  fallbacks[1] = {.enabled = true,
                  .provider = cfg::Provider::kApiFootball,
                  .competition = "78",
                  .season = "2025",
                  .team = "157"};
  mappings[0] = {.enabled = true,
                 .route = 1,
                 .primary = "100",
                 .secondary = "200"};
  mappings[1] = {.enabled = true,  // route 0 is gone: dropped
                 .route = 0,
                 .primary = "1",
                 .secondary = "2"};
  mappings[2] = {.enabled = false,  // paused pair of the paused route
                 .route = 2,
                 .swapped = true,
                 .primary = "7",
                 .secondary = "8"};
  mappings[3] = {.enabled = true,  // secondary id missing: dropped
                 .route = 1,
                 .primary = "3"};
  mappings[4] = {.enabled = true,  // no such route: dropped
                 .route = 7,
                 .primary = "5",
                 .secondary = "6"};

  Model expected = base;
  Competition first = competition(cfg::Provider::kOpenLigaDb, "bl1", "2025", "16");
  first.fallback_active = true;
  first.fallback.provider = cfg::Provider::kApiFootball;
  first.fallback.competition.assign("78");
  first.fallback.season.assign("2025");
  first.fallback.team.assign("157");
  Competition second =
      competition(cfg::Provider::kFootballData, "BL1", "2025", "5");
  second.active = false;
  expected.competitions.push(first);
  expected.competitions.push(second);
  FixturePair a;
  a.primary_id.assign("100");
  a.secondary_id.assign("200");
  FixturePair b;
  b.active = false;
  b.swapped = true;
  b.competition = 1;
  b.primary_id.assign("7");
  b.secondary_id.assign("8");
  expected.fixture_pairs.push(a);
  expected.fixture_pairs.push(b);
  TEST_ASSERT_TRUE(settings::validate(expected));

  const Record record = format2(base, routes, fallbacks, mappings);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == expected);
  TEST_ASSERT_EQUAL_size_t(2, loaded.competitions.size());
  TEST_ASSERT_EQUAL_size_t(2, loaded.fixture_pairs.size());

  // The next save writes format 6 and reads back the same.
  const Record saved = encoded(loaded);
  TEST_ASSERT_EQUAL_HEX8(6, saved[0]);
  Model again = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(saved, again));
  TEST_ASSERT_TRUE(again == expected);

  // No routes at all: the lists are stored empty and replace the initial ones.
  const Record none = format2(base, {}, {}, {});
  loaded = withLists();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(none, loaded));
  TEST_ASSERT_TRUE(loaded.competitions.empty());
  TEST_ASSERT_TRUE(loaded.fixture_pairs.empty());
  TEST_ASSERT_TRUE(loaded.language == base.language);

  // An enabled route without a team is incomplete and dropped, with its
  // fallback.
  routes = {};
  routes[0] = {.enabled = true,
               .provider = cfg::Provider::kEspn,
               .competition = "ger.1"};
  fallbacks = {};
  fallbacks[0] = {.enabled = true,
                  .provider = cfg::Provider::kOpenLigaDb,
                  .competition = "bl1",
                  .season = "2025",
                  .team = "16"};
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decodeInto(format2(base, routes, fallbacks, {}), loaded));
  TEST_ASSERT_TRUE(loaded.competitions.empty());
}

void testFormat2WithoutListsKeepsInitialLists() {
  // A format-2 record that ends before its first route.
  const Model base = sample();
  Out out;
  core(out, base);
  secrets(out, base);
  const Record record = out.record(2);
  const Presets presets{.provider = "football-data",
                        .competition = "BL1",
                        .team = "5",
                        .season = "2025"};
  const Model initial = initialValues(presets);
  Model loaded = initial;
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded.hostname == base.hostname);
  TEST_ASSERT_EQUAL_size_t(1, loaded.competitions.size());
  TEST_ASSERT_TRUE(loaded.competitions == initial.competitions);
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

void expectViolation(const Model& model, const char* path, const char* reason,
                     const settings::Limits& limits = {}) {
  const Rejected result = reject(model, limits);
  TEST_ASSERT_TRUE(result.rejected);
  TEST_ASSERT_EQUAL_STRING(path, result.path.c_str());
  TEST_ASSERT_EQUAL_STRING(reason, result.reason.c_str());
}

void testViolationNamesScalarPath() {
  TEST_ASSERT_FALSE(reject(withLists()).rejected);
  Model model = withLists();
  model.hostname.assign("bad_name");
  expectViolation(model, "hostname", "invalid");

  model = withLists();
  model.brightness = 101;
  expectViolation(model, "brightness", "range");

  model = withLists();
  model.night_start = 1440;
  expectViolation(model, "night_start", "range");

  model = withLists();
  model.language = 2;
  expectViolation(model, "language", "range");
}

void testViolationNamesListPath() {
  Model model = withLists();
  model.competitions[0].fallback.team.assign("");
  expectViolation(model, "competitions[0].fallback.team", "required");

  model = withLists();
  model.competitions[0].primary.competition.assign("");
  expectViolation(model, "competitions[0].primary.competition", "required");

  model = withLists();
  model.competitions[0].primary.team.assign("bad\tid");
  expectViolation(model, "competitions[0].primary.team", "invalid");

  model = withLists();
  model.competitions[0].fallback.provider = model.competitions[0].primary.provider;
  expectViolation(model, "competitions[0].fallback.provider", "conflict");

  model = withLists();
  model.fixture_pairs[0].competition = 3;
  expectViolation(model, "fixture_pairs[0].competition", "reference");

  model = withLists();
  model.fixture_pairs[0].secondary_id.assign("");
  expectViolation(model, "fixture_pairs[0].secondary_id", "required");

  model = withLists();
  model.competitions[0].fallback_active = false;
  expectViolation(model, "fixture_pairs[0].competition", "conflict");

  model = withLists();
  FixturePair twin = model.fixture_pairs[0];
  twin.secondary_id.assign("201");
  model.fixture_pairs.push(twin);
  expectViolation(model, "fixture_pairs[1].primary_id", "duplicate");
}

void testViolationNamesImageAndScreenPath() {
  Model model = withLists();
  model.slides.push(cfg::kImageLibraryMax + 1);
  expectViolation(model, "slides[0]", "range");

  model = withLists();
  model.screens[1].text_scale = 69;
  expectViolation(model, "screens[1].text_scale", "range");

  model = withLists();
  model.screens[static_cast<unsigned>(cfg::Screen::kCrest)].image = 1;
  expectViolation(model, "screens[3].image", "invalid");
  TEST_ASSERT_EQUAL_UINT(3, static_cast<unsigned>(cfg::Screen::kCrest));

  model = withLists();
  for (auto& style : model.screens) style.enabled = false;
  expectViolation(model, "screens", "none_enabled");
}

void testViolationKeepsFieldMapping() {
  Model model = withLists();
  Field field = Field::kWifiSsid;
  model.time_zone.assign("Mars/Olympus");
  TEST_ASSERT_FALSE(settings::validate(model, &field));
  TEST_ASSERT_EQUAL_STRING("time_zone", settings::fieldName(field));
  model = withLists();
  model.slides.push(99);
  TEST_ASSERT_FALSE(settings::validate(model, &field));
  TEST_ASSERT_EQUAL_STRING("application", settings::fieldName(field));
}

void testViolationFitsPathBuffer() {
  // The longest path of a full model must not be cut.
  Model model = full();
  model.competitions[2].fallback.competition.assign("bad\tid");
  const Rejected result = reject(model);
  TEST_ASSERT_TRUE(result.rejected);
  TEST_ASSERT_EQUAL_STRING("competitions[2].fallback.competition",
                           result.path.c_str());
  TEST_ASSERT_TRUE(result.path.size() + 1 <= cfg::kFieldPathBytes);
}

void testLimitsAreStrict() {
  const settings::Limits round{.visible_matches_max = 4, .table_window_max = 7};
  Model model = withLists();
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
  Model stored = withLists();
  stored.visible_matches = 5;
  stored.table_window = 9;
  const Record record = encoded(stored);

  Model loaded = initialValues({}, nullptr, round);
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decode(record, loaded, nullptr, round));
  TEST_ASSERT_EQUAL_UINT8(4, loaded.visible_matches);
  TEST_ASSERT_EQUAL_UINT8(7, loaded.table_window);
  TEST_ASSERT_TRUE(loaded.competitions == stored.competitions);

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
  Model stored = withLists();
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
  TEST_ASSERT_TRUE(loaded.competitions == stored.competitions);

  stored.matchday_screen = cfg::Screen::kCrest;
  TEST_ASSERT_TRUE(settings::validate(stored));
}

// OQ-123/OQ-126: format 4 stores the Konferenz pin flag after the lists; a
// format-3 record (the same bytes without it) loads the initial value.
// Formats 5 and 6 keep the flag at that place, followed by their tail.
void testPinOwnMatchFormats() {
  TEST_ASSERT_TRUE(defaults().pin_own_match);
  const auto pinAt = [](const Record& r) {
    return r.size() - 1 - kFormat5TailBytes;
  };

  Model stored = withLists();
  stored.pin_own_match = false;
  Record record = encoded(stored);
  TEST_ASSERT_EQUAL_HEX8(0, record[pinAt(record)]);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == stored);

  // Format 3: without the flag, the stored value cannot win.
  record = olderFormat(record, 3);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded.pin_own_match);
  TEST_ASSERT_TRUE(loaded.competitions == stored.competitions);
  TEST_ASSERT_TRUE(loaded.fixture_pairs == stored.fixture_pairs);

  // A format-3 record with trailing bytes does not read them as the flag.
  record.push_back(0);
  settings::seal(record, 3);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded.pin_own_match);

  // The next save writes format 6 with the flag.
  record = encoded(loaded);
  TEST_ASSERT_EQUAL_HEX8(6, record[0]);
  TEST_ASSERT_EQUAL_HEX8(1, record[pinAt(record)]);

  // Only 0 and 1 are a flag.
  record = encoded(stored);
  record[pinAt(record)] = 2;
  settings::seal(record);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(record, loaded));
}

// OQ-136/OQ-139/OQ-141: format 5 stores the Konferenz mode, the
// whole-matchday flag and one role per input slot; a format-4 record (the
// same bytes without that tail) loads the initial values.
void testFormat5Fields() {
  const Model initial = defaults();
  TEST_ASSERT_EQUAL(cfg::ConferenceMode::kAllPages, initial.conference_mode);
  TEST_ASSERT_FALSE(initial.whole_matchday);

  Model stored = withLists();
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

  // Format 5: the same fields after a crest image id; a library id is
  // handed over, an id outside the library is dropped (OQ-152).
  uint8_t crest = cfg::kStockImageId;
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decode(olderFormat(record, 5, 4), loaded, nullptr, {},
                           &crest));
  TEST_ASSERT_TRUE(loaded == stored);
  TEST_ASSERT_EQUAL_UINT8(4, crest);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decode(olderFormat(record, 5, cfg::kImageLibraryMax + 1),
                           loaded, nullptr, {}, &crest));
  TEST_ASSERT_TRUE(loaded == stored);
  TEST_ASSERT_EQUAL_UINT8(cfg::kStockImageId, crest);
  // Format 6 has no crest image id.
  crest = 4;
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decode(record, loaded, nullptr, {}, &crest));
  TEST_ASSERT_EQUAL_UINT8(cfg::kStockImageId, crest);

  // Format 4: the pin flag is read, the tail fields keep the initial values.
  const std::size_t tail = record.size() - kFormat5TailBytes;
  crest = cfg::kStockImageId;
  Record format4 = olderFormat(record, 4, 2);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk,
                    decode(format4, loaded, nullptr, {}, &crest));
  TEST_ASSERT_EQUAL_UINT8(2, crest);
  TEST_ASSERT_FALSE(loaded.pin_own_match);
  TEST_ASSERT_EQUAL(cfg::ConferenceMode::kAllPages, loaded.conference_mode);
  TEST_ASSERT_FALSE(loaded.whole_matchday);
  TEST_ASSERT_TRUE(loaded.input_roles == initial.input_roles);
  TEST_ASSERT_TRUE(loaded.competitions == stored.competitions);
  // A format-4 record with trailing bytes does not read them as the tail.
  format4.push_back(1);
  settings::seal(format4, 4);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(format4, loaded));
  TEST_ASSERT_EQUAL(cfg::ConferenceMode::kAllPages, loaded.conference_mode);

  // Unknown mode, a flag other than 0/1 and an unknown role are rejected.
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
}

void testValidateLists() {
  using cfg::Provider;
  const auto invalid = [](const Model& model) {
    Field field = Field::kWifiSsid;
    return !settings::validate(model, &field) && field == Field::kApplication;
  };
  TEST_ASSERT_TRUE(settings::validate(withLists()));

  Model model = withLists();
  model.competitions[0].fallback.provider = Provider::kOpenLigaDb;  // = primary
  TEST_ASSERT_TRUE(invalid(model));

  model = withLists();
  model.competitions[0].fallback.team.assign("");  // active fallback, no team
  TEST_ASSERT_TRUE(invalid(model));
  // A fallback that is switched off may stay incomplete (no pair may use it).
  model.competitions[0].fallback_active = false;
  model.fixture_pairs.clear();
  TEST_ASSERT_TRUE(settings::validate(model));

  model = withLists();
  model.competitions[0].primary.team.assign("");  // incomplete competition
  TEST_ASSERT_TRUE(invalid(model));

  model = withLists();
  model.competitions[0].primary.competition.assign("bl\t1");
  TEST_ASSERT_TRUE(invalid(model));

  model = withLists();
  model.competitions[0].fallback.season.assign("25");  // 4 digits needed
  TEST_ASSERT_TRUE(invalid(model));

  // An empty season is the current season for every provider (D3).
  model = withLists();
  model.competitions[0].primary.provider = Provider::kApiFootball;
  model.competitions[0].primary.season.assign("");
  TEST_ASSERT_TRUE(settings::validate(model));

  model = withLists();
  model.competitions[0].competition_name.assign("a\nb");  // control character
  TEST_ASSERT_TRUE(invalid(model));

  // Fixture pairs refer to a competition with an active fallback.
  model = withLists();
  model.fixture_pairs[0].competition = 1;  // no such competition
  TEST_ASSERT_TRUE(invalid(model));

  model = withLists();
  model.competitions[0].fallback_active = false;
  TEST_ASSERT_TRUE(invalid(model));  // an active pair needs the fallback
  model.fixture_pairs[0].active = false;
  TEST_ASSERT_TRUE(settings::validate(model));

  model = withLists();
  model.fixture_pairs[0].secondary_id.assign("");
  TEST_ASSERT_TRUE(invalid(model));

  // Active pairs of one competition use distinct ids on both sides.
  model = withLists();
  FixturePair pair;
  pair.primary_id.assign("100");
  pair.secondary_id.assign("201");
  model.fixture_pairs.push(pair);
  TEST_ASSERT_TRUE(invalid(model));
  model.fixture_pairs[1].primary_id.assign("101");
  model.fixture_pairs[1].secondary_id.assign("200");
  TEST_ASSERT_TRUE(invalid(model));
  model.fixture_pairs[1].secondary_id.assign("201");
  TEST_ASSERT_TRUE(settings::validate(model));
  model.fixture_pairs[1].primary_id.assign("100");
  model.fixture_pairs[1].active = false;  // a paused pair is not compared
  TEST_ASSERT_TRUE(settings::validate(model));

  // Images: library ids only; the crest screen has no background image.
  model = defaults();
  model.screens[0].image = cfg::kImageLibraryMax;
  model.slides.push(0);
  model.slides.push(cfg::kImageLibraryMax);
  TEST_ASSERT_TRUE(settings::validate(model));

  model = defaults();
  model.slides.push(cfg::kImageLibraryMax + 1);
  TEST_ASSERT_TRUE(invalid(model));
  model = defaults();
  model.screens[2].image = cfg::kImageLibraryMax + 1;
  TEST_ASSERT_TRUE(invalid(model));
  model = defaults();
  model.screens[static_cast<std::size_t>(cfg::Screen::kCrest)].image = 1;
  TEST_ASSERT_TRUE(invalid(model));
}

// Which saves keep the fetched data. Proves only the comparison; the device
// evidence (a brightness save without a provider request and without an
// empty screen) is separate.
// A save through the REST API refuses an active source whose provider has
// no key or no opt-in, and names its provider field.
void testProviderAccess() {
  Model model = withLists();
  model.football_data_key.assign("key");
  model.espn_opt_in = false;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model));

  settings::Violation why;
  model.football_data_key.assign("");
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model, &why));
  TEST_ASSERT_EQUAL_STRING("competitions[0].fallback.provider",
                           why.path.data());
  TEST_ASSERT_TRUE(why.reason == settings::Reason::kConflict);
  // An inactive fallback or competition is not requested.
  model.competitions[0].fallback_active = false;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model));
  model.competitions[0].fallback_active = true;
  model.competitions[0].active = false;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model));
  model.competitions[0].active = true;

  model = withLists();
  model.football_data_key.assign("key");
  model.competitions[0].primary.provider = cfg::Provider::kEspn;
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model, &why));
  TEST_ASSERT_EQUAL_STRING("competitions[0].primary.provider",
                           why.path.data());
  model.espn_opt_in = true;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model));

  model.competitions.push(model.competitions[0]);
  model.competitions[1].primary.provider = cfg::Provider::kApiFootball;
  model.api_football_key.assign("");
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model, &why));
  TEST_ASSERT_EQUAL_STRING("competitions[1].primary.provider",
                           why.path.data());
  model.api_football_key.assign("key");
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model));

  // OpenLigaDB needs neither.
  model = withLists();
  model.competitions[0].fallback_active = false;
  model.api_football_key.assign("");
  model.football_data_key.assign("");
  model.espn_opt_in = false;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model));
  // validate() does not apply this rule, so a stored older record still
  // loads.
  model.competitions[0].fallback_active = true;
  TEST_ASSERT_TRUE(settings::validate(model));
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model));
}

void testSameProviderData() {
  const Model base = withLists();
  Model other = base;
  other.brightness = static_cast<uint8_t>(base.brightness == 1 ? 2 : 1);
  other.language = static_cast<uint8_t>(1 - base.language);
  other.window_before = static_cast<uint16_t>(base.window_before + 1);
  other.api_daily_budget = static_cast<uint16_t>(base.api_daily_budget + 1);
  other.competitions[0].competition_name.assign("Erste Liga");
  other.competitions[0].team_name.assign("Other name");
  TEST_ASSERT_TRUE(settings::sameProviderData(base, other));

  other = base;
  other.competitions[0].primary.season.assign("");
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
  other = base;
  other.competitions[0].fallback.team.assign("6");
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
  other = base;
  other.competitions[0].active = false;
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
  other = base;
  other.competitions[0].fallback_active = false;
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
  other = base;
  other.competitions.push(base.competitions[0]);
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
  other = base;
  other.fixture_pairs[0].swapped = !base.fixture_pairs[0].swapped;
  TEST_ASSERT_FALSE(settings::sameProviderData(base, other));
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
  FieldMask rejected = 0xFFFF;
  Model model = initialValues({.provider = "football-data",
                               .competition = "BL1",
                               .team = "5",
                               .season = "2025"},
                              &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_EQUAL_size_t(1, model.competitions.size());
  const Competition& entry = model.competitions[0];
  TEST_ASSERT_TRUE(entry.active);
  TEST_ASSERT_FALSE(entry.fallback_active);
  TEST_ASSERT_TRUE(entry.primary.provider == cfg::Provider::kFootballData);
  TEST_ASSERT_TRUE(entry.primary.competition.view() == "BL1");
  TEST_ASSERT_TRUE(entry.primary.team.view() == "5");
  TEST_ASSERT_TRUE(entry.primary.season.view() == "2025");
  TEST_ASSERT_TRUE(settings::validate(model));

  // No provider named: the default provider.
  model = initialValues({.competition = "bl1", .team = "16"}, &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_EQUAL_size_t(1, model.competitions.size());
  TEST_ASSERT_TRUE(model.competitions[0].primary.provider ==
                   cfg::kProviderDefault);

  // A competition without a team is not an entry; nothing is rejected.
  model = initialValues({.competition = "bl1"}, &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_TRUE(model.competitions.empty());

  // An invalid identifier is skipped and reported.
  model = initialValues({.competition = "bad\tid", .team = "16"}, &rejected);
  TEST_ASSERT_EQUAL_UINT32(fieldBit(Field::kApplication), rejected);
  TEST_ASSERT_TRUE(model.competitions.empty());
  TEST_ASSERT_TRUE(settings::validate(model));
}

void testStoredValuesOverridePresets() {
  const Presets presets{.wifi_ssid = "Preset Net", .hostname = "preset-host"};
  const Model stored = sample();
  Model loaded = initialValues(presets);
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(encoded(stored), loaded));
  TEST_ASSERT_TRUE(loaded == stored);

  // The stored lists win over the preset competition, even when empty.
  const Presets with_competition{.provider = "football-data",
                                 .competition = "BL1",
                                 .team = "5",
                                 .season = "2025"};
  loaded = initialValues(with_competition);
  TEST_ASSERT_EQUAL_size_t(1, loaded.competitions.size());
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(encoded(stored), loaded));
  TEST_ASSERT_TRUE(loaded.competitions.empty());
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
  RUN_TEST(testFormat6Layout);
  RUN_TEST(testFullModelUsesTheMaximumRecord);
  RUN_TEST(testRoundTrip);
  RUN_TEST(testShorterRecordKeepsInitialValues);
  RUN_TEST(testPartialFieldIsRejected);
  RUN_TEST(testListsPartialAndInvalid);
  RUN_TEST(testLongerRecordIgnoresTail);
  RUN_TEST(testDamagedHeaders);
  RUN_TEST(testFormat1IsMigrated);
  RUN_TEST(testFormat2IsConvertedToLists);
  RUN_TEST(testFormat2WithoutListsKeepsInitialLists);
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
  RUN_TEST(testViolationNamesScalarPath);
  RUN_TEST(testViolationNamesListPath);
  RUN_TEST(testViolationNamesImageAndScreenPath);
  RUN_TEST(testViolationKeepsFieldMapping);
  RUN_TEST(testViolationFitsPathBuffer);
  RUN_TEST(testLimitsAreStrict);
  RUN_TEST(testInitialValuesFollowLimits);
  RUN_TEST(testDecodeClampsToLimits);
  RUN_TEST(testMatchdayScreenWithoutOwnMatch);
  RUN_TEST(testPinOwnMatchFormats);
  RUN_TEST(testFormat5Fields);
  RUN_TEST(testInputRoles);
  RUN_TEST(testSameProviderData);
  RUN_TEST(testProviderAccess);
  RUN_TEST(testDropMissingImages);
  RUN_TEST(testPresetsOverrideDefaults);
  RUN_TEST(testInvalidPresetsKeepDefaults);
  RUN_TEST(testPresetsCreateCompetition);
  RUN_TEST(testStoredValuesOverridePresets);
  return UNITY_END();
}
