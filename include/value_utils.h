// Allocation-free byte operations. Callers retain their domain rules: raw
// bytes, validated label text and identities are not interchangeable.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace values {

// Input must fit. Copy before clearing the tail so self/subview assignment
// remains valid, including raw byte strings containing NUL.
inline void copyPadded(char* out, std::size_t capacity,
                       std::string_view input) noexcept {
  if (!input.empty()) std::memmove(out, input.data(), input.size());
  if (input.size() < capacity)
    std::memset(out + input.size(), 0, capacity - input.size());
}

// Clips valid UTF-8 at a complete code point; does not validate its input.
// Validation belongs to the boundary that accepts text, not raw bytes.
inline std::size_t utf8PrefixBytes(std::string_view input,
                                   std::size_t limit) noexcept {
  std::size_t count = input.size() < limit ? input.size() : limit;
  while (count < input.size() && count > 0 &&
         (static_cast<unsigned char>(input[count]) & 0xc0) == 0x80)
    --count;
  return count;
}

// Streaming Unicode scalar validation, including U+0000. Text consumers
// choose whether NUL/control characters are permitted in their own domain.
// A false result rejects the sequence; stop feeding that validation run.
class Utf8 {
 public:
  bool accept(uint8_t c) noexcept {
    if (left_) {
      if (c < low_ || c > high_) return false;
      --left_;
      low_ = 0x80;
      high_ = 0xbf;
      return true;
    }
    if (c < 0x80) return true;
    if (c >= 0xc2 && c <= 0xdf) {
      left_ = 1;
      return true;
    }
    if (c >= 0xe0 && c <= 0xef) {
      left_ = 2;
      low_ = c == 0xe0 ? 0xa0 : 0x80;
      high_ = c == 0xed ? 0x9f : 0xbf;
      return true;
    }
    if (c >= 0xf0 && c <= 0xf4) {
      left_ = 3;
      low_ = c == 0xf0 ? 0x90 : 0x80;
      high_ = c == 0xf4 ? 0x8f : 0xbf;
      return true;
    }
    return false;
  }
  bool complete() const noexcept { return left_ == 0; }

 private:
  uint8_t left_ = 0, low_ = 0x80, high_ = 0xbf;
};

// Bounded presentation composition. printf may stop inside a code point;
// retain only the last complete UTF-8 prefix and always terminate the output.
// Input validation remains the accepting domain's responsibility. No heap.
inline int formatText(char* out, std::size_t capacity, const char* format,
                      ...) noexcept {
  if (capacity == 0) return 0;
  va_list arguments;
  va_start(arguments, format);
  const int required = std::vsnprintf(out, capacity, format, arguments);
  va_end(arguments);
  if (required < 0) {
    out[0] = 0;
    return required;
  }
  Utf8 state;
  std::size_t complete = 0;
  for (std::size_t i = 0; i < capacity - 1 && out[i]; ++i) {
    if (!state.accept(static_cast<unsigned char>(out[i]))) break;
    if (state.complete()) complete = i + 1;
  }
  out[complete] = 0;
  return required;
}

inline bool isUtf8Text(std::string_view input) noexcept {
  Utf8 state;
  for (const unsigned char c : input)
    if (c == 0 || !state.accept(c)) return false;
  return state.complete();
}

// Validate the complete input, even beyond the adopted byte limit. Failure
// leaves the output untouched; clipping never hides invalid trailing text.
inline bool utf8TextPrefix(std::string_view input, std::size_t limit,
                           std::string_view& prefix) noexcept {
  Utf8 state;
  std::size_t complete = 0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    const auto c = static_cast<unsigned char>(input[i]);
    if (c == 0 || !state.accept(c)) return false;
    if (state.complete() && i < limit) complete = i + 1;
  }
  if (!state.complete()) return false;
  prefix = {input.data(), complete};
  return true;
}

inline constexpr uint32_t kFnv1a32Basis = 2166136261u;
inline uint32_t fnv1a32(std::string_view bytes,
                        uint32_t seed = kFnv1a32Basis) noexcept {
  for (const unsigned char c : bytes) seed = (seed ^ c) * 16777619u;
  return seed;
}

// Decimal digits only, 1..UINT32_MAX; zero is the invalid/absent sentinel.
inline uint32_t positiveUint32(std::string_view text) noexcept {
  if (text.empty()) return 0;
  uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return 0;
    const auto digit = static_cast<uint32_t>(c - '0');
    if (value > UINT32_MAX / 10 ||
        (value == UINT32_MAX / 10 && digit > UINT32_MAX % 10))
      return 0;
    value = value * 10 + digit;
  }
  return value;
}
}  // namespace values
