// Competition files and the short-name file (pure, native-tested; the
// football service does the file I/O through the callbacks).
//
// A competition file holds what one competition's requests built up, so a
// reboot shows it without a request: its matches with their teams and
// round labels, its table, the own matches' full names and events, its
// notices, what its source resolved (season, round, match days) and when
// each kept data type last completed (UTC). Layout: header, teams, round
// labels, matches (an own match is followed by its names and events),
// table rows, CRC-32 of everything before it. Structures are stored as
// they are in RAM, so a file is only valid for the build layout it names
// (kLayout); any change to a stored structure or enum needs a new
// kVersion. Team and label indices are file-local; the monotonic time of
// a score change is not kept.
//
// The short-name file holds the owner's replacements for team short names
// (ShortNames), per provider and team id. It has its own version and
// layout (kNamesVersion, kNamesLayout; OQ-260), so a change of the
// competition files keeps it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_config.h"
#include "match_store.h"
#include "provider_plan.h"

namespace football::persist {

// Data types whose last completed run a file keeps: fixtures, table (the
// live types follow the live windows).
inline constexpr std::size_t kKeptTypes = 2;
static_assert(static_cast<std::size_t>(cfg::DataType::kFixtures) == 0 &&
              static_cast<std::size_t>(cfg::DataType::kTable) == 1);

// Streams bytes out of or into a file; false on an I/O error.
using Write = bool (*)(const void* bytes, std::size_t size, void* context);
using Read = bool (*)(void* bytes, std::size_t size, void* context);

uint32_t crc32(uint32_t crc, const void* bytes, std::size_t size);

struct SavedSource {
  plan::Resolved resolved;
  // UTC second each kept data type last completed, 0 = never.
  std::array<int64_t, kKeptTypes> done{};
};

struct Saved {
  uint8_t competition = 0;
  uint32_t route_key = 0;  // the service's key of the competition's source
  int64_t saved_utc = 0;   // 0 = no valid clock when written
  uint8_t notices = 0;
  SavedSource source;
};

// Writes the file of 'saved.competition' from 'store'; 'write' null only
// counts. Returns its size, 0 when a write failed.
std::size_t encode(const MatchStore& store, const Saved& saved, Write write,
                   void* context);
// Reads a whole file and checks it (format, layout, counts within the
// store's capacities, indices, CRC) without touching a store; 'out' gets
// its header.
bool check(Read read, void* context, Saved& out);
// Loads a checked file (read again from its start) into 'store': teams,
// labels and matches through the store's own functions, the table, the
// own matches' events and the notices. False on a read error or a file
// that differs from the checked one; what was loaded stays.
bool load(Read read, void* context, MatchStore& store, const Clock& clock);

struct ShortName {
  uint32_t id = 0;  // provider team id, 0 = free entry
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  Text<26> name;
};

class ShortNames {
 public:
  // The replacement of a team's short name, nullptr when none.
  const char* find(cfg::Provider provider, uint32_t id) const;
  // Sets the replacement (an empty name removes it); false for id 0 or a
  // full table.
  bool set(cfg::Provider provider, uint32_t id, std::string_view name);
  std::size_t count() const;
  const ShortName& at(std::size_t i) const { return names_[i]; }
  void clear() { names_ = {}; }
  // Exchanges the tables element by element (no 1.5 KB temporary).
  void swap(ShortNames& other) { names_.swap(other.names_); }
  // The short-name file; decode() reads it in place and leaves the table
  // empty when the file is not valid.
  std::size_t encode(Write write, void* context) const;
  bool decode(Read read, void* context);

 private:
  std::array<ShortName, cfg::kShortNamesMax> names_{};
};

}  // namespace football::persist
