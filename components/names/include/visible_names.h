// Names needed by one visible projection, not a second team directory.
#pragma once
#include <array>
#include <span>

#include "effective_names.h"
#include "names_ref.h"

namespace football::names {
inline constexpr std::size_t kVisibleNames =
    cfg::kVisibleMatchesRange.max * 2 > cfg::kTableWindowRange.max
        ? cfg::kVisibleMatchesRange.max * 2
        : cfg::kTableWindowRange.max;
struct Target {
  uint32_t id = 0;
  cfg::Provider provider = cfg::kProviderDefault;
  char* output = nullptr;
  std::size_t capacity = 0;
  char* short_output = nullptr;
  std::size_t short_capacity = 0;
};
struct Targets {
  std::array<Target, kVisibleNames> entries{};
  std::size_t count = 0;
  void add(const Target& target) {
    if (target.id && count < entries.size()) entries[count++] = target;
  }
  std::span<Target> span() { return {entries.data(), count}; }
};
// Cache only adopted fields. A missing override uses the newly projected
// provider fallback every frame; no raw/effective all-team duplication.
struct VisibleCache {
  struct Entry {
    uint32_t id = 0;
    cfg::Provider provider = cfg::kProviderDefault;
    bool name_present = false, short_present = false;
    Effective labels;
  };
  std::array<Entry, kVisibleNames> entries{};
  settings::NamesRef reference;
  uint8_t language = 0;
  std::size_t count = 0;
};
}  // namespace football::names
