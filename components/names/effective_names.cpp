#include "effective_names.h"

#include <cstdio>
#include <cstring>

namespace football::names {
namespace {
struct References {
  uint16_t name = 0, short_name = 0;
};
bool references(const store::Reader* reader, cfg::Provider provider,
                uint32_t id, uint8_t language, References& out) {
  out = {};
  if (id == 0 || static_cast<std::size_t>(provider) >= cfg::kProviderCount ||
      language > 1)
    return false;
  if (reader == nullptr) return true;
  uint16_t club;
  const auto found = reader->findId(provider, id, club);
  if (found == store::Lookup::kError) return false;
  if (found == store::Lookup::kFound) {
    store::Club row;
    if (!reader->club(club, row)) return false;
    out.name = language == 0 ? row.de : row.en;
    out.short_name = row.short_name;
  }
  uint16_t exception;
  const auto overridden = reader->findException(provider, id, exception);
  if (overridden == store::Lookup::kError) return false;
  if (overridden == store::Lookup::kFound) out.short_name = exception;
  return true;
}
template <std::size_t N>
bool original(Text<N>& out, std::string_view input) {
  std::string_view prefix;
  if (!store::adopt(input, N, prefix)) return false;
  out.assign(prefix);
  return true;
}
template <std::size_t N>
bool selected(const store::Reader& reader, uint16_t reference, Text<N>& out) {
  std::size_t length = 0;
  return reader.text(reference, out.data, sizeof(out.data), length);
}
}  // namespace

bool adopted(const store::Reader* reader, cfg::Provider provider, uint32_t id,
             uint8_t language, Effective& output, bool& name_present,
             bool& short_present) {
  References refs;
  if (!references(reader, provider, id, language, refs)) return false;
  name_present = refs.name != 0;
  short_present = refs.short_name != 0;
  output = {};
  return (!name_present || selected(*reader, refs.name, output.name)) &&
         (!short_present ||
          selected(*reader, refs.short_name, output.short_name));
}

bool resolve(const store::Reader* reader, cfg::Provider provider, uint32_t id,
             std::string_view original_name, std::string_view original_short,
             uint8_t language, Effective& out) {
  References refs;
  if (!references(reader, provider, id, language, refs)) return false;
  char identity[11];
  snprintf(identity, sizeof(identity), "%lu", static_cast<unsigned long>(id));
  const auto full = !original_name.empty()    ? original_name
                    : !original_short.empty() ? original_short
                                              : std::string_view(identity);
  const auto short_text = !original_short.empty() ? original_short : full;
  // Validate complete raw inputs even when saved fields win. Prefix clipping
  // must never hide malformed UTF-8 or an embedded NUL past the visible prefix.
  if (!original(out.name, full) || !original(out.short_name, short_text))
    return false;
  if (refs.name && !selected(*reader, refs.name, out.name)) return false;
  if (refs.short_name && !selected(*reader, refs.short_name, out.short_name))
    return false;
  return true;
}

}  // namespace football::names
