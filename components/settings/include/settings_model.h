// Runtime settings model: fixed-capacity values, validation against the
// limits in app_config.h and the initial values (secrets.h presets over
// defaults). Pure C++, independent of ESP-IDF; storage lives in
// components/core.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "app_config.h"

namespace settings {

// Byte string with explicit length; may contain any byte (an SSID may).
template <std::size_t Capacity>
struct Bytes {
  static_assert(Capacity <= 255, "length is stored in one byte");
  static constexpr std::size_t kCapacity = Capacity;

  uint8_t length = 0;
  std::array<char, Capacity> data{};

  std::string_view view() const noexcept { return {data.data(), length}; }
  bool empty() const noexcept { return length == 0; }

  // Fails and keeps the old value if the value does not fit; never truncates.
  bool assign(std::string_view value) noexcept {
    if (value.size() > Capacity) return false;
    data.fill(0);
    // memcpy: string_view::copy links the libstdc++ exception support (about
    // 2 KB flash), std::copy_n becomes memmove (about 200 B IRAM on the C6).
    if (!value.empty()) std::memcpy(data.data(), value.data(), value.size());
    length = static_cast<uint8_t>(value.size());
    return true;
  }

  bool operator==(const Bytes& other) const noexcept {
    return view() == other.view();
  }
};

// Fields in stored order (record format 1). Append only.
enum class Field : uint8_t {
  kWifiSsid,
  kWifiPassword,
  kApPassword,
  kHostname,
  kExternalAntenna,
  kDebugStatusLog,
  kDebugStatusIntervalS,
};
inline constexpr std::size_t kFieldCount = 7;

const char* fieldName(Field field) noexcept;

struct Model {
  Bytes<cfg::kWifiSsidMaxBytes> wifi_ssid;          // empty = no network
  Bytes<cfg::kWifiPskHexChars> wifi_password;       // empty = open network
  Bytes<cfg::kWifiPassphraseMaxChars> ap_password;  // empty = open AP
  Bytes<cfg::kHostnameMaxChars> hostname;
  bool external_antenna = false;
  bool debug_status_log = false;
  uint16_t debug_status_interval_s = 0;

  bool operator==(const Model& other) const noexcept = default;
};

// Limits of single values (also used for secrets.h presets).
bool isValidWifiPassword(std::string_view value) noexcept;
bool isValidApPassword(std::string_view value) noexcept;
bool isValidHostname(std::string_view value) noexcept;
bool isValidStatusInterval(uint16_t seconds) noexcept;

// True if every field is within its limits; otherwise *invalid (if given)
// names the first rejected field.
bool validate(const Model& model, Field* invalid = nullptr) noexcept;

// Values from secrets.h; empty = not set.
struct Presets {
  std::string_view wifi_ssid;
  std::string_view wifi_password;
  std::string_view ap_password;
  std::string_view hostname;
};

// Bit (1 << Field) per preset that was set but violates its limits.
using FieldMask = uint32_t;
inline constexpr FieldMask fieldBit(Field field) noexcept {
  return FieldMask{1} << static_cast<uint8_t>(field);
}

// Defaults from app_config.h, each overridden by its preset when set and
// valid. Invalid presets are skipped (default kept) and reported in *rejected.
Model initialValues(const Presets& presets,
                    FieldMask* rejected = nullptr) noexcept;

}  // namespace settings
