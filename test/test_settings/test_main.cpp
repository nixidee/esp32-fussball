// Host acceptance for the settings model, its limits, the initial values
// and the stored record format 1 (docs/CONFIGURATION.md).

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

using settings::decode;
using settings::DecodeResult;
using settings::encode;
using settings::Field;
using settings::fieldBit;
using settings::FieldMask;
using settings::initialValues;
using settings::kHeaderBytes;
using settings::kRecordBytes;
using settings::Model;
using settings::Presets;

using Record = std::array<uint8_t, kRecordBytes>;

// Offsets of format 1, written out independently of the implementation.
constexpr std::size_t kSsidAt = 8;
constexpr std::size_t kPasswordAt = 41;
constexpr std::size_t kApPasswordAt = 106;
constexpr std::size_t kHostnameAt = 170;
constexpr std::size_t kAntennaAt = 234;
constexpr std::size_t kStatusLogAt = 235;
constexpr std::size_t kIntervalAt = 236;
constexpr std::size_t kFormat1Bytes = 238;

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
  return model;
}

Record encoded(const Model& model) {
  Record record{};
  encode(model, record);
  return record;
}

uint32_t readU32(const uint8_t* at) {
  return uint32_t{at[0]} | (uint32_t{at[1]} << 8) | (uint32_t{at[2]} << 16) |
         (uint32_t{at[3]} << 24);
}

DecodeResult decodeInto(std::span<const uint8_t> record, Model& model,
                        Field* invalid = nullptr) {
  return decode(record, model, invalid);
}

// Record with one payload byte changed and a valid header, so the value
// check (not the CRC) has to catch it.
Record withByte(const Model& model, std::size_t at, uint8_t value) {
  Record record = encoded(model);
  record[at] = value;
  settings::seal(record);
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
  TEST_ASSERT_TRUE(settings::validate(model));
}

void testFormat1Layout() {
  TEST_ASSERT_EQUAL_size_t(8, kHeaderBytes);
  TEST_ASSERT_EQUAL_size_t(kFormat1Bytes, kRecordBytes);

  Model model = defaults();
  model.wifi_ssid.assign("AB");
  model.hostname.assign("x");
  model.external_antenna = true;
  model.debug_status_log = false;
  model.debug_status_interval_s = 0x0123;  // 291 s, little-endian 23 01
  const Record record = encoded(model);

  TEST_ASSERT_EQUAL_HEX8(0x01, record[0]);  // version 1
  TEST_ASSERT_EQUAL_HEX8(0x00, record[1]);
  TEST_ASSERT_EQUAL_HEX8(230, record[2]);  // payload length 230
  TEST_ASSERT_EQUAL_HEX8(0x00, record[3]);

  TEST_ASSERT_EQUAL_HEX8(2, record[kSsidAt]);
  TEST_ASSERT_EQUAL_HEX8('A', record[kSsidAt + 1]);
  TEST_ASSERT_EQUAL_HEX8('B', record[kSsidAt + 2]);
  TEST_ASSERT_EQUAL_HEX8(0, record[kPasswordAt]);
  TEST_ASSERT_EQUAL_HEX8(0, record[kApPasswordAt]);
  TEST_ASSERT_EQUAL_HEX8(1, record[kHostnameAt]);
  TEST_ASSERT_EQUAL_HEX8('x', record[kHostnameAt + 1]);
  TEST_ASSERT_EQUAL_HEX8(1, record[kAntennaAt]);
  TEST_ASSERT_EQUAL_HEX8(0, record[kStatusLogAt]);
  TEST_ASSERT_EQUAL_HEX8(0x23, record[kIntervalAt]);
  TEST_ASSERT_EQUAL_HEX8(0x01, record[kIntervalAt + 1]);

  // Unused text bytes are zero.
  for (std::size_t i = kSsidAt + 3; i < kPasswordAt; ++i)
    TEST_ASSERT_EQUAL_HEX8(0, record[i]);
  for (std::size_t i = kHostnameAt + 2; i < kAntennaAt; ++i)
    TEST_ASSERT_EQUAL_HEX8(0, record[i]);

  // CRC over header bytes 0..3 followed by the payload.
  std::vector<uint8_t> covered(record.begin(), record.begin() + 4);
  covered.insert(covered.end(), record.begin() + kHeaderBytes, record.end());
  TEST_ASSERT_EQUAL_HEX32(settings::crc32(covered), readU32(&record[4]));
}

void testRoundTrip() {
  const Model original = sample();
  const Record record = encoded(original);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == original);

  // Full capacities, any SSID bytes, a raw hex key.
  Model full = defaults();
  full.wifi_ssid.assign(std::string("\x00\xff ssid", 7) + std::string(25, 'S'));
  full.wifi_password.assign(std::string(64, 'a'));
  full.ap_password.assign(std::string(63, '~'));
  full.hostname.assign("a" + std::string(61, '-') + "z");
  full.debug_status_interval_s = 3600;
  Model loaded_full = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(encoded(full), loaded_full));
  TEST_ASSERT_TRUE(loaded_full == full);
}

void testShorterRecordKeepsInitialValues() {
  const Model stored = sample();
  Record record = encoded(stored);

  // Without the last field (interval).
  const auto without_interval = std::span(record).first(kIntervalAt);
  settings::seal(without_interval);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(without_interval, loaded));
  TEST_ASSERT_TRUE(loaded.hostname == stored.hostname);
  TEST_ASSERT_TRUE(loaded.external_antenna);
  TEST_ASSERT_EQUAL_UINT16(30, loaded.debug_status_interval_s);

  // Header only: every field keeps its initial value.
  record = encoded(stored);
  const auto header_only = std::span(record).first(kHeaderBytes);
  settings::seal(header_only);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(header_only, loaded));
  TEST_ASSERT_TRUE(loaded == defaults());
}

void testPartialFieldIsRejected() {
  const Model original = sample();
  for (const std::size_t size :
       {kSsidAt + 1, kPasswordAt - 1, kHostnameAt + 10, kIntervalAt + 1}) {
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
  std::vector<uint8_t> record(kRecordBytes + 20, 0xA5);
  encode(original,
         std::span<uint8_t, kRecordBytes>(record.data(), kRecordBytes));
  settings::seal(record);
  Model loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
  TEST_ASSERT_TRUE(loaded == original);

  // Largest accepted record, and one byte more.
  record.assign(cfg::kSettingsMaxRecordBytes, 0);
  encode(original,
         std::span<uint8_t, kRecordBytes>(record.data(), kRecordBytes));
  settings::seal(record);
  loaded = defaults();
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(record, loaded));
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

  record = encoded(original);
  settings::seal(record, 2);
  TEST_ASSERT_EQUAL(DecodeResult::kUnknownVersion, decodeInto(record, loaded));
  settings::seal(record, 0);
  TEST_ASSERT_EQUAL(DecodeResult::kUnknownVersion, decodeInto(record, loaded));

  TEST_ASSERT_TRUE(loaded == defaults());
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

void testPresetsOverrideDefaults() {
  const Presets presets{
      .wifi_ssid = "Preset Net",
      .wifi_password = "preset-pass",
      .ap_password = "ap-preset",
      .hostname = "preset-host",
  };
  FieldMask rejected = 0xFFFF;
  const Model model = initialValues(presets, &rejected);
  TEST_ASSERT_EQUAL_UINT32(0, rejected);
  TEST_ASSERT_TRUE(model.wifi_ssid.view() == "Preset Net");
  TEST_ASSERT_TRUE(model.wifi_password.view() == "preset-pass");
  TEST_ASSERT_TRUE(model.ap_password.view() == "ap-preset");
  TEST_ASSERT_TRUE(model.hostname.view() == "preset-host");
  TEST_ASSERT_EQUAL_UINT16(30, model.debug_status_interval_s);
}

void testInvalidPresetsKeepDefaults() {
  const Presets presets{
      .wifi_ssid = std::string_view("123456789012345678901234567890123"),
      .wifi_password = "short",
      .ap_password = "tiny",
      .hostname = "bad_host",
  };
  FieldMask rejected = 0;
  const Model model = initialValues(presets, &rejected);
  TEST_ASSERT_EQUAL_UINT32(
      fieldBit(Field::kWifiSsid) | fieldBit(Field::kWifiPassword) |
          fieldBit(Field::kApPassword) | fieldBit(Field::kHostname),
      rejected);
  TEST_ASSERT_TRUE(model == defaults());
  TEST_ASSERT_TRUE(settings::validate(model));
}

void testStoredValuesOverridePresets() {
  const Presets presets{.wifi_ssid = "Preset Net", .hostname = "preset-host"};
  const Model stored = sample();
  Model loaded = initialValues(presets);
  TEST_ASSERT_EQUAL(DecodeResult::kOk, decodeInto(encoded(stored), loaded));
  TEST_ASSERT_TRUE(loaded == stored);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(testActualUnityVersion);
  RUN_TEST(testCrc32KnownVector);
  RUN_TEST(testDefaults);
  RUN_TEST(testFormat1Layout);
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
  RUN_TEST(testSsidCapacity);
  RUN_TEST(testValidateNamesFirstInvalidField);
  RUN_TEST(testPresetsOverrideDefaults);
  RUN_TEST(testInvalidPresetsKeepDefaults);
  RUN_TEST(testStoredValuesOverridePresets);
  return UNITY_END();
}
