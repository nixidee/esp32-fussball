// Selected labels only: no saved-club collection or original-text archive.
// Adopted values are read through the bounded Reader's I/O callbacks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "football_model.h"
#include "team_name_store.h"

namespace football::names {
struct Effective {
  Text<cfg::kTeamNameBytes> name;
  Text<cfg::kTeamShortBytes> short_name;
};

// Only already-adopted provider bindings are active. The immutable catalogue
// is a matching/prefill source, never an implicit activation path.
bool resolve(const store::Reader* adopted, cfg::Provider provider, uint32_t id,
             std::string_view original_name, std::string_view original_short,
             uint8_t language, Effective& out);

// Retrieves only explicit adopted fields; absent values remain absent so a
// visible cache can reuse fresh provider fallback without retaining a copy.
bool adopted(const store::Reader*, cfg::Provider, uint32_t, uint8_t language,
             Effective&, bool& name_present, bool& short_present);
}  // namespace football::names
