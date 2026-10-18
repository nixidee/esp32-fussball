// Exact immutable adopted-names version referenced by application config.
#pragma once
#include <cstdint>

namespace settings {
struct NamesRef {
  uint8_t slot = 0;  // 0 = none; 1/2 = immutable names versions
  uint32_t generation = 0;
  uint16_t length = 0;
  uint32_t crc = 0;
  bool operator==(const NamesRef&) const = default;
};
}  // namespace settings
