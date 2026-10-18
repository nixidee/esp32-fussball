#include "team_name_catalogue.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "json_stream.h"

namespace football::names::catalogue {
namespace {
// No pointers or constructors: the complete payload remains in read-only flash.
struct Record {
  uint16_t key, league, de, en, short_name;
  uint16_t aliases, espn_ids, openligadb_ids;
  uint8_t alias_count, espn_count, openligadb_count, providers;
};
static_assert(sizeof(Record) == 20);
#include "team_name_asset.h"

std::string_view text(uint16_t offset) {
  return reinterpret_cast<const char*>(kStrings + offset);
}

}  // namespace

std::size_t count() { return std::size(kClubs); }
const char* sourceSha256() { return kSourceSha256; }
std::size_t payloadBytes() { return kPayloadBytes; }

ClubView at(std::size_t index) {
  if (index >= count()) return {};
  const auto& club = kClubs[index];
  return {text(club.key),
          text(club.league),
          text(club.de),
          text(club.en),
          text(club.short_name),
          {kIds + club.espn_ids, club.espn_count},
          {kIds + club.openligadb_ids, club.openligadb_count},
          index};
}
std::size_t ClubView::aliasCount() const {
  return index < count() ? kClubs[index].alias_count : 0;
}
std::string_view ClubView::alias(std::size_t alias_index) const {
  if (alias_index >= aliasCount()) return {};
  return text(kAliases[kClubs[index].aliases + alias_index]);
}
std::size_t findKey(std::string_view key) {
  for (std::size_t i = 0; i < count(); ++i)
    if (text(kClubs[i].key) == key) return i;
  return kNotFound;
}
std::size_t findId(std::string_view provider, uint32_t id) {
  if (provider != "espn" && provider != "openligadb") return kNotFound;
  for (std::size_t i = 0; i < count(); ++i) {
    const auto& club = kClubs[i];
    const auto values =
        provider == "espn"
            ? std::span{kIds + club.espn_ids, club.espn_count}
            : std::span{kIds + club.openligadb_ids, club.openligadb_count};
    for (uint32_t candidate : values)
      if (candidate == id) return i;
  }
  return kNotFound;
}
std::size_t uniqueExact(std::string_view original_name) {
  if (original_name.empty()) return kNotFound;
  auto match = kNotFound;
  for (std::size_t i = 0; i < count(); ++i) {
    const auto club = at(i);
    bool found = club.de == original_name || club.en == original_name;
    for (std::size_t alias = 0; !found && alias < club.aliasCount(); ++alias)
      found = club.alias(alias) == original_name;
    if (found) {
      if (match != kNotFound) return kAmbiguous;
      match = i;
    }
  }
  return match;
}

bool streamJson(Write write, void* context) {
  if (!write) return false;
  struct Sink {
    Write write;
    void* context;
  } sink{write, context};
  ::json::Writer out({&sink, [](std::string_view bytes, void* raw) {
                        const auto& sink = *static_cast<Sink*>(raw);
                        while (!bytes.empty()) {
                          const auto count = std::min(
                              bytes.size(), cfg::kNamesStreamChunkBytes);
                          if (!sink.write(sink.context, bytes.data(), count))
                            return false;
                          bytes.remove_prefix(count);
                        }
                        return true;
                      }});
  const auto ids = [&](const char* provider, std::span<const uint32_t> values) {
    if (!out.key(provider) || !out.array()) return false;
    for (const auto id : values)
      if (!out.integer(id)) return false;
    return out.end();
  };
  if (!out.object() || !out.key("format") ||
      !out.string(cfg::kTeamNamesJsonFormat) || !out.key("version") ||
      !out.integer(cfg::kTeamNamesJsonVersion) || !out.key("clubs") ||
      !out.array())
    return false;
  for (std::size_t i = 0; i < count(); ++i) {
    const auto club = at(i);
    if (!out.object() || !out.key("key") || !out.string(club.key) ||
        !out.key("league") || !out.string(club.league) || !out.key("de") ||
        !out.string(club.de) || !out.key("en") || !out.string(club.en) ||
        !out.key("short") || !out.string(club.short_name) || !out.key("ids") ||
        !out.object())
      return false;
    const auto providers = kClubs[i].providers;
    if ((providers & 1) && !ids("espn", club.espn_ids)) return false;
    if ((providers & 2) && !ids("openligadb", club.openligadb_ids))
      return false;
    if (!out.end() || !out.key("aliases") || !out.array()) return false;
    for (std::size_t alias = 0; alias < club.aliasCount(); ++alias)
      if (!out.string(club.alias(alias))) return false;
    if (!out.end() || !out.end()) return false;
  }
  return out.end() && out.end() && out.finish();
}
}  // namespace football::names::catalogue
