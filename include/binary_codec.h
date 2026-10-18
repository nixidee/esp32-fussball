// Allocation-free wire primitives. Codec owners retain versions, fields,
// framing and error policy; this header owns little endian, CRC and bounds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace binary {
template <class T>
  requires(std::is_unsigned_v<T>)
inline T little(const uint8_t* at) noexcept {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i)
    value |= static_cast<T>(static_cast<T>(at[i]) << (8 * i));
  return value;
}
template <class T>
  requires(std::is_unsigned_v<T>)
inline void little(uint8_t* at, T value) noexcept {
  for (std::size_t i = 0; i < sizeof(T); ++i)
    at[i] = static_cast<uint8_t>(value >> (8 * i));
}
inline uint16_t readU16(const uint8_t* at) noexcept {
  return little<uint16_t>(at);
}
inline uint32_t readU32(const uint8_t* at) noexcept {
  return little<uint32_t>(at);
}
inline void writeU16(uint8_t* at, uint16_t value) noexcept {
  little(at, value);
}
inline void writeU32(uint8_t* at, uint32_t value) noexcept {
  little(at, value);
}
inline uint32_t crc32(std::span<const uint8_t> bytes,
                      uint32_t previous = 0) noexcept {
  uint32_t crc = ~previous;
  for (const uint8_t byte : bytes) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// A failed take never moves or exposes a partial field. Higher-level decoders
// distinguish optional end-of-payload from a required/truncated field.
template <class Byte>
class Cursor {
 public:
  explicit Cursor(std::span<Byte> bytes) noexcept : bytes_(bytes) {}
  Byte* take(std::size_t count) noexcept {
    if (failed_ || count > remaining()) {
      failed_ = true;
      return nullptr;
    }
    auto* at = bytes_.data();
    if (position_) at += position_;
    position_ += count;
    return at;
  }
  std::size_t position() const noexcept { return position_; }
  std::size_t remaining() const noexcept { return bytes_.size() - position_; }
  bool good() const noexcept { return !failed_; }
  bool complete() const noexcept { return good() && remaining() == 0; }

 private:
  std::span<Byte> bytes_;
  std::size_t position_ = 0;
  bool failed_ = false;
};
class Reader : public Cursor<const uint8_t> {
 public:
  using Cursor::Cursor;
  template <class T>
  T value() noexcept {
    const auto* at = take(sizeof(T));
    return at ? little<T>(at) : T{};
  }
  uint8_t u8() noexcept { return value<uint8_t>(); }
  uint16_t u16() noexcept { return value<uint16_t>(); }
  uint32_t u32() noexcept { return value<uint32_t>(); }
  uint64_t u64() noexcept { return value<uint64_t>(); }
};
class Writer : public Cursor<uint8_t> {
 public:
  using Cursor::Cursor;
  template <class T>
  bool value(T value) noexcept {
    auto* at = take(sizeof(T));
    if (!at) return false;
    little<T>(at, value);
    return true;
  }
  bool u8(uint8_t value) noexcept { return this->value(value); }
  bool u16(uint16_t value) noexcept { return this->value(value); }
  bool u32(uint32_t value) noexcept { return this->value(value); }
  bool u64(uint64_t value) noexcept { return this->value(value); }
};
}  // namespace binary
