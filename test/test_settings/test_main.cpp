// Current general settings acceptance. The exact pre-cutover suite is
// archived in dated legacy-settings evidence. Superseded screen settings
// assertions moved to policy/sequence tests; surviving general checks remain.
// Current formats are general11/competitions4; no development migration.

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
// Current record wire positions are specified independently of the codec.
constexpr std::size_t kAdminAt = kCoreBytes;
constexpr std::size_t kApiKeyAt = kAdminAt + 65;
constexpr std::size_t kFootballKeyAt = kApiKeyAt + 97;
constexpr std::size_t kScalarsAt = kFootballKeyAt + 65;
constexpr std::size_t kLanguageAt = kScalarsAt + 6;
constexpr std::size_t kScrollResetAt = kScalarsAt + 10;
constexpr std::size_t kPlansBytes =
    cfg::kProviderCount * (6 + 4 * cfg::kDataTypeCount) + 6;
constexpr std::size_t kStoreNextIdAt = kHeaderBytes;
constexpr std::size_t kStoreCountAt = kStoreNextIdAt + 1 + 4;
constexpr std::size_t kStoreEntryAt = kStoreCountAt + 1;

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
  entry.incarnation = 1;
  entry.competition_name.assign("Bundesliga");
  entry.team_name.assign("FC Test");
  competitions.entries.push(entry);
  competitions.next_id = 2;
  competitions.next_incarnation = 2;
  return competitions;
}

// Every general-settings text at its capacity.
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
    entry.incarnation = i + 1;
    competitions.entries.push(entry);
  }
  competitions.next_id = 9;
  competitions.next_incarnation = 9;
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
  TEST_ASSERT_TRUE(model.providers == settings::providerPlansDefault());
  TEST_ASSERT_TRUE(model.global_limit == RequestLimit{});
  TEST_ASSERT_TRUE(settings::validate(model));

  // An empty competition store; the first new entry gets id 1.
  const Competitions store;
  TEST_ASSERT_TRUE(store.entries.empty());
  TEST_ASSERT_EQUAL_UINT8(1, store.next_id);
  TEST_ASSERT_TRUE(settings::validate(store));
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

// OQ-200/OQ-209/OQ-247: the competitions have their own record with stable
// stable identities and one source each.

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

  // Application secrets and general scalar values.
  Model rich = sample();
  rich.admin_password.assign("admin");
  rich.api_football_key.assign("key-1");
  rich.football_data_key.assign("key-2");
  rich.language = 1;
  rich.espn_opt_in = true;
  TEST_ASSERT_TRUE(settings::validate(rich));
  Model loaded_rich = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(encoded(rich), loaded_rich));
  TEST_ASSERT_TRUE(loaded_rich == rich);

  // The competition store with stable identities and a paused entry.
  Competitions store = withLists();
  Competition second = competition(cfg::Provider::kEspn, "ger.1", "", "");
  second.id = 4;
  second.incarnation = store.next_incarnation++;
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
  // ends after the scalar values keeps later general settings.
  Model initial = initialValues({.admin_password = "preset-admin"});
  Model later = stored;
  later.language = 1;
  record = encoded(later);
  const auto before_secrets = std::span(record).first(kAdminAt);
  settings::seal(before_secrets);
  loaded = initial;
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(before_secrets, loaded));
  TEST_ASSERT_TRUE(loaded.hostname == stored.hostname);
  TEST_ASSERT_TRUE(loaded.admin_password.view() == "preset-admin");
  TEST_ASSERT_EQUAL_UINT8(0, loaded.language);
}

void testPartialFieldIsRejected() {
  const Model original = sample();
  for (const std::size_t size :
       {kSsidAt + 1, kPasswordAt - 1, kHostnameAt + 10, kIntervalAt + 1,
        kTimeZoneAt + 5, kNtpServerAt + 1, kCoreBytes - 1,
        kAdminAt + 10,          // inside the admin password
        kApiKeyAt + 5,          // inside a provider key
        kScrollResetAt + 1}) {  // inside a 16-bit value
    Record record = encoded(original);
    const auto cut = std::span(record).first(size);
    settings::seal(cut);
    Model loaded = defaults();
    TEST_ASSERT_EQUAL(DecodeResult::kPartialField, decodeInto(cut, loaded));
    TEST_ASSERT_TRUE(loaded == defaults());
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

  // Older development formats are no longer read: the device starts
  // with the initial values ("outdated").
  record = encoded(original);
  for (uint16_t version = 1; version < cfg::kSettingsFormatVersion; ++version) {
    settings::seal(record, version);
    TEST_ASSERT_EQUAL(DecodeResult::kOutdatedVersion,
                      decodeInto(record, loaded));
  }
  // Version 0 and newer versions are unknown.
  for (const uint16_t version :
       {uint16_t{0}, uint16_t{cfg::kSettingsFormatVersion + 1},
        uint16_t{0xFFFF}}) {
    settings::seal(record, version);
    TEST_ASSERT_EQUAL(DecodeResult::kUnknownVersion,
                      decodeInto(record, loaded));
  }
  TEST_ASSERT_TRUE(loaded == defaults());

  // Older competition development formats are no longer read.
  const Competitions store = withLists();
  Competitions loaded_store;
  record = encodedCompetitions(store);
  TEST_ASSERT_EQUAL(
      DecodeResult::kTooShort,
      decodeStore(std::span(record).first(kHeaderBytes - 1), loaded_store));
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
  for (const uint16_t version :
       {uint16_t{0}, uint16_t{cfg::kCompetitionsFormatVersion + 1},
        uint16_t{8}}) {
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

  // Every stored competition has its own stable identity.
  store = withLists();
  store.entries[0].id = 0;
  expectViolation(store, "competitions[0].id", "required");

  store = withLists();
  store.entries.push(store.entries[0]);
  expectViolation(store, "competitions[1].id", "duplicate");
}

void testViolationKeepsFieldMapping() {
  Model model = sample();
  Field field = Field::kWifiSsid;
  model.time_zone.assign("Mars/Olympus");
  TEST_ASSERT_FALSE(settings::validate(model, &field));
  TEST_ASSERT_EQUAL_STRING("time_zone", settings::fieldName(field));
  model = sample();
  model.brightness = 101;
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

// OQ-122: the own-match screen is no choice while other matches run. A
// stored record from earlier firmware keeps all values and shows the
// Konferenz instead; new input with that value is rejected.

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

  // A current record that ends before the plans keeps the initial ones;
  // one that ends inside a value is partial.
  Record cut = record;
  cut.resize(record.size() - kPlansBytes);
  settings::seal(cut);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(cut, loaded));
  TEST_ASSERT_TRUE(loaded.providers == initial.providers);
  TEST_ASSERT_TRUE(loaded.global_limit == RequestLimit{});
  for (const std::size_t missing : {kPlansBytes - 7, std::size_t{1}}) {
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
  const std::size_t events = bad.size() - 6 - 4;
  for (std::size_t i = 0; i < 4; ++i) bad[events + i] = 0;
  settings::seal(bad);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kInvalidValue, decodeInto(bad, loaded));
  TEST_ASSERT_TRUE(loaded == defaults());
}

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
      entry.incarnation = store.next_incarnation++;
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
  TEST_ASSERT_EQUAL_STRING("competitions[0].primary.provider", why.path.data());
  model.espn_opt_in = true;
  TEST_ASSERT_TRUE(settings::checkProviderAccess(model, store));

  Competition second = store.entries[0];
  second.id = 2;
  second.primary.provider = cfg::Provider::kApiFootball;
  store.entries.push(second);
  model.api_football_key.assign("");
  TEST_ASSERT_FALSE(settings::checkProviderAccess(model, store, &why));
  TEST_ASSERT_EQUAL_STRING("competitions[1].primary.provider", why.path.data());
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
  RUN_TEST(testRoundTrip);
  RUN_TEST(testShorterRecordKeepsInitialValues);
  RUN_TEST(testPartialFieldIsRejected);
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
  RUN_TEST(testViolationKeepsFieldMapping);
  RUN_TEST(testViolationFitsPathBuffer);
  RUN_TEST(testInputRoles);
  RUN_TEST(testRequestPlans);
  RUN_TEST(testSameProviderData);
  RUN_TEST(testProviderAccess);
  RUN_TEST(testPresetsOverrideDefaults);
  RUN_TEST(testInvalidPresetsKeepDefaults);
  RUN_TEST(testPresetsCreateCompetition);
  RUN_TEST(testStoredValuesOverridePresets);
  return UNITY_END();
}
