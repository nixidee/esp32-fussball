#pragma once

#include <cstddef>
#include <cstdint>

namespace screen {

// These identities are stored, not sequence positions. Keep the five existing
// identities unchanged; the independent slideshow appends the sixth identity.
enum class Type : uint8_t {
  kLiveSingle = 0,
  kLiveMulti = 1,
  kTable = 2,
  kCrest = 3,
  kMatchday = 4,
  kSlideshow = 5,
};

inline constexpr std::size_t kTypeCount = 6;
inline constexpr std::size_t kSportTypeCount = 4;

constexpr std::size_t index(Type type) noexcept {
  return static_cast<std::size_t>(type);
}

constexpr bool valid(Type type) noexcept { return index(type) < kTypeCount; }

constexpr bool sporting(Type type) noexcept {
  return type == Type::kLiveSingle || type == Type::kLiveMulti ||
         type == Type::kTable || type == Type::kMatchday;
}

// Compact competition-rule index; image types have no competition overrides.
constexpr std::size_t sportIndex(Type type) noexcept {
  switch (type) {
    case Type::kLiveSingle: return 0;
    case Type::kLiveMulti: return 1;
    case Type::kTable: return 2;
    case Type::kMatchday: return 3;
    default: return kSportTypeCount;
  }
}

}  // namespace screen
