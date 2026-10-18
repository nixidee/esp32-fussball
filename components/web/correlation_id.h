// Exact JSON/URL representation of 64-bit operation identities. JavaScript
// cannot preserve every uint64 value as a number; the wire uses lower-case hex.
#pragma once
#include <cstdint>
#include <string_view>

namespace web {
inline bool parseCorrelationId(std::string_view text, uint64_t& out) {
  if (text.size() != 16) return false;
  uint64_t value = 0;
  for (const char c : text) {
    const int digit = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                             : -1;
    if (digit < 0) return false;
    value = (value << 4) | static_cast<unsigned>(digit);
  }
  if (!value) return false;
  out = value;
  return true;
}
inline void correlationIdText(uint64_t id, char (&text)[17]) {
  constexpr char hex[] = "0123456789abcdef";
  text[16] = 0;
  for (std::size_t i = 16; i > 0; --i, id >>= 4) text[i - 1] = hex[id & 15];
}
}  // namespace web
