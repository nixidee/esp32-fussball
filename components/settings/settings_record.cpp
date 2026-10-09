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
  if (readU16(record.data() + kVersionOffset) != cfg::kSettingsFormatVersion)
    return DecodeResult::kUnknownVersion;

  Model candidate = model;
  Reader reader(record.subspan(kHeaderBytes));
  reader.text(Field::kWifiSsid, candidate.wifi_ssid);
  reader.text(Field::kWifiPassword, candidate.wifi_password);
  reader.text(Field::kApPassword, candidate.ap_password);
  reader.text(Field::kHostname, candidate.hostname);
  reader.flag(Field::kExternalAntenna, candidate.external_antenna);
  reader.flag(Field::kDebugStatusLog, candidate.debug_status_log);
  reader.u16(candidate.debug_status_interval_s);

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
