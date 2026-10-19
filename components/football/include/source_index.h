// Exact resolved provider sources. Configuration IDs and selected clubs never
// enter this identity. The MatchStore owner releases a source only after its
// facts are no longer needed; there is no implicit eviction or persistence.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "football_model.h"

namespace football {
struct SourceId {
  uint32_t value = 0;
  explicit operator bool() const { return value != 0; }
  bool operator==(const SourceId&) const = default;
};
struct MatchKey {
  uint32_t id = 0;
  SourceId source;
  bool valid() const { return id != 0 && bool(source); }
  bool operator==(const MatchKey&) const = default;
};
using SourceSlot = uint8_t;
inline constexpr SourceSlot kNoSource = UINT8_MAX;

struct SourceKey {
  cfg::Provider provider = cfg::kProviderDefault;
  values::Bytes<cfg::kCompetitionIdMaxBytes> competition;
  values::Bytes<cfg::kSeasonMaxBytes> season;
  // Both fields are exact, nonempty identities. An unresolved automatic
  // season is a request preference, never a stored source. Failure is atomic.
  bool assign(cfg::Provider, std::string_view competition,
              std::string_view resolved_season);
  bool valid() const;
  bool operator==(const SourceKey&) const = default;
};

class SourceIndex {
 public:
  // Existing exact identity wins even when capacity/ID space is exhausted.
  // Otherwise zero means invalid key/full/exhausted: no existing source moves.
  SourceId intern(const SourceKey&);
  SourceId find(const SourceKey&) const;
  SourceSlot slot(SourceId) const;
  const SourceKey* key(SourceSlot) const;
  SourceId id(SourceSlot) const;
  void release(SourceId);
  // Never resets the runtime ID sequence: delayed IDs cannot alias a later
  // source even after a store reset or physical slot reuse within this boot.
  void clear();

 private:
  struct Entry {
    SourceKey key;
    SourceId id;
  };
  std::array<Entry, cfg::kStoreSources> entries_{};
  uint32_t allocated_ = 0;
};
static_assert(cfg::kStoreSources < kNoSource);
}  // namespace football
