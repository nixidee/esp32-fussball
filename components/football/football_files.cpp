#include "football_files.h"

#include <cstring>
#include <initializer_list>

namespace football::persist {
namespace {

constexpr uint32_t kMagic = 0x31434246;       // "FBC1"
constexpr uint32_t kNamesMagic = 0x314e4246;  // "FBN1"
// 2: one source per competition (OQ-247); 3: no core data, season time
// (OQ-248, OQ-259).
constexpr uint16_t kVersion = 3;
// The short-name file has its own version and layout (OQ-260): a change of
// the competition files keeps the owner's short names.
constexpr uint16_t kNamesVersion = 1;

struct Header {
  uint32_t magic, layout;
  uint16_t version;
  uint8_t competition, notices;
  uint32_t route_key;
  int64_t saved_utc;
  uint8_t teams, labels, matches, rows;
  SavedSource source;
};
// Follows an own match: its full names and event count; the events follow.
struct OwnBlock {
  Text<cfg::kNameBytes> home, away;
  uint8_t events;
};
struct NamesHeader {
  uint32_t magic, layout;
  uint16_t version, count;
};

constexpr uint32_t mix(uint32_t h, std::size_t value) {
  for (int i = 0; i < 4; ++i) {
    h ^= static_cast<uint8_t>(value >> (8 * i));
    h *= 16777619u;
  }
  return h;
}
// The sizes of every stored structure: a build with another layout cannot
// read the file.
constexpr uint32_t kLayout = [] {
  uint32_t h = 2166136261u;
  for (const std::size_t v :
       {sizeof(Header), sizeof(OwnBlock), sizeof(StoredTeam), sizeof(Text<31>),
        sizeof(StoredMatch), sizeof(StoredEvent), sizeof(StoredRow),
        sizeof(SavedSource), cfg::kMatchEvents})
    h = mix(h, v);
  return h;
}();
constexpr uint32_t kNamesLayout = [] {
  uint32_t h = 2166136261u;
  for (const std::size_t v : {sizeof(NamesHeader), sizeof(ShortName)})
    h = mix(h, v);
  return h;
}();

template <std::size_t N>
bool terminated(const Text<N>& text) {
  return text.data[N] == 0;
}
bool provider(cfg::Provider p) {
  return static_cast<std::size_t>(p) < cfg::kProviderCount;
}
bool index(uint8_t i, uint8_t count) { return i == kNoIndex || i < count; }
bool side(Side s) { return static_cast<uint8_t>(s) <= 2; }

struct Out {
  Write write;
  void* context;
  uint32_t crc = 0;
  std::size_t size = 0;
  bool ok = true;
  void put(const void* bytes, std::size_t n) {
    crc = crc32(crc, bytes, n);
    size += n;
    if (ok && write != nullptr && !write(bytes, n, context)) ok = false;
  }
  std::size_t end() {
    const uint32_t value = crc;
    size += sizeof(value);
    if (ok && write != nullptr && !write(&value, sizeof(value), context))
      ok = false;
    return ok ? size : 0;
  }
};
struct In {
  Read read;
  void* context;
  uint32_t crc = 0;
  bool get(void* bytes, std::size_t n) {
    if (!read(bytes, n, context)) return false;
    crc = crc32(crc, bytes, n);
    return true;
  }
  bool end() {
    uint32_t value = 0;
    return read(&value, sizeof(value), context) && value == crc;
  }
};

bool validMatch(const StoredMatch& m, const Header& h) {
  return m.id != 0 && m.competition == h.competition && provider(m.provider) &&
         index(m.home, h.teams) && index(m.away, h.teams) &&
         index(m.label, h.labels) &&
         static_cast<uint8_t>(m.phase) <=
             static_cast<uint8_t>(Phase::kAbandoned);
}
bool validEvent(const StoredEvent& e) {
  return static_cast<uint8_t>(e.type) <= static_cast<uint8_t>(EventType::kVar) &&
         side(e.actor) && side(e.beneficiary) && terminated(e.player) &&
         terminated(e.player_in);
}

// Reads one competition file; 'store' null only checks it.
bool parse(In& in, Saved& out, MatchStore* store, const Clock& clock) {
  Header h;
  if (!in.get(&h, sizeof(h))) return false;
  if (h.magic != kMagic || h.version != kVersion || h.layout != kLayout ||
      h.competition == 0 || h.teams > cfg::kStoreTeams ||
      h.labels > cfg::kStoreRoundLabels || h.matches > cfg::kStoreMatches ||
      h.rows > cfg::kStoreTableRows)
    return false;
  if (h.source.resolved.day_count > plan::kEspnDays ||
      h.source.resolved.season.length > h.source.resolved.season.kCapacity)
    return false;
  // File index -> store index (load only).
  std::array<uint8_t, cfg::kStoreTeams> teams;
  std::array<uint8_t, cfg::kStoreRoundLabels> labels;
  for (uint8_t i = 0; i < h.teams; ++i) {
    StoredTeam t;
    if (!in.get(&t, sizeof(t))) return false;
    if (t.id == 0 || !provider(t.provider) || !terminated(t.short_name))
      return false;
    if (store != nullptr)
      teams[i] = store->team(h.competition, t.provider, t.id,
                             t.short_name.data);
  }
  for (uint8_t i = 0; i < h.labels; ++i) {
    Text<31> label;
    if (!in.get(&label, sizeof(label))) return false;
    if (label.empty() || !terminated(label)) return false;
    if (store != nullptr) labels[i] = store->label(label.data);
  }
  const auto team = [&teams](uint8_t i) {
    return i == kNoIndex ? kNoIndex : teams[i];
  };
  for (uint8_t i = 0; i < h.matches; ++i) {
    StoredMatch m;
    if (!in.get(&m, sizeof(m)) || !validMatch(m, h)) return false;
    OwnBlock own;
    const bool block = m.own != kNoIndex;
    if (block) {
      if (!in.get(&own, sizeof(own)) || !terminated(own.home) ||
          !terminated(own.away) || own.events > cfg::kMatchEvents)
        return false;
    }
    StoredMatch* stored = nullptr;
    const uint8_t cut = m.flags & (kMatchEventsTruncated | kMatchEventsPoolCut);
    if (store != nullptr) {
      m.home = team(m.home);
      m.away = team(m.away);
      m.label = m.label == kNoIndex ? kNoIndex : labels[m.label];
      m.own = kNoIndex;
      m.score_changed = 0;
      stored = block ? store->upsert(m, clock, own.home.data, own.away.data)
                     : store->upsert(m, clock);
    }
    for (uint8_t k = 0; block && k < own.events; ++k) {
      StoredEvent e;
      if (!in.get(&e, sizeof(e)) || !validEvent(e)) return false;
      if (stored != nullptr) store->restoreEvent(*stored, e);
    }
    if (stored != nullptr && stored->own != kNoIndex) stored->flags |= cut;
  }
  if (store != nullptr && h.rows > 0) store->beginTable(h.competition);
  bool rows_ok = true;
  for (uint8_t i = 0; i < h.rows && rows_ok; ++i) {
    StoredRow r;
    rows_ok = in.get(&r, sizeof(r)) && r.competition == h.competition &&
              index(r.team, h.teams);
    if (rows_ok && store != nullptr) {
      r.team = team(r.team);
      store->appendRow(r);
    }
  }
  const bool ok = rows_ok && in.end();
  if (store != nullptr && h.rows > 0) store->commitTable(ok);
  if (!ok) return false;
  if (store != nullptr) store->flag(h.competition, h.notices, true);
  out.competition = h.competition;
  out.route_key = h.route_key;
  out.saved_utc = h.saved_utc;
  out.notices = h.notices;
  out.source = h.source;
  return true;
}

}  // namespace

uint32_t crc32(uint32_t crc, const void* bytes, std::size_t size) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
  }
  return ~crc;
}

std::size_t encode(const MatchStore& store, const Saved& saved, Write write,
                   void* context) {
  const uint8_t competition = saved.competition;
  // Store index -> file index, and the store indices in file order.
  std::array<uint8_t, cfg::kStoreTeams> team_map, team_list;
  std::array<uint8_t, cfg::kStoreRoundLabels> label_map, label_list;
  team_map.fill(kNoIndex);
  label_map.fill(kNoIndex);
  uint8_t teams = 0, labels = 0, matches = 0, rows = 0;
  const auto addTeam = [&](uint8_t t) {
    if (t >= cfg::kStoreTeams || store.teamAt(t).id == 0) return;
    if (team_map[t] == kNoIndex) {
      team_map[t] = teams;
      team_list[teams++] = t;
    }
  };
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (m.competition != competition) continue;
    ++matches;
    addTeam(m.home);
    addTeam(m.away);
    if (m.label < cfg::kStoreRoundLabels && !store.labelAt(m.label).empty() &&
        label_map[m.label] == kNoIndex) {
      label_map[m.label] = labels;
      label_list[labels++] = m.label;
    }
  }
  for (std::size_t i = 0; i < store.rowCount(); ++i)
    if (store.row(i).competition == competition) {
      ++rows;
      addTeam(store.row(i).team);
    }
  const auto team = [&team_map](uint8_t t) {
    return t < cfg::kStoreTeams ? team_map[t] : kNoIndex;
  };

  Out out{write, context};
  Header h;
  std::memset(static_cast<void*>(&h), 0, sizeof(h));
  h.magic = kMagic;
  h.layout = kLayout;
  h.version = kVersion;
  h.competition = competition;
  h.notices = store.notices(competition);
  h.route_key = saved.route_key;
  h.saved_utc = saved.saved_utc;
  h.teams = teams;
  h.labels = labels;
  h.matches = matches;
  h.rows = rows;
  h.source = saved.source;
  out.put(&h, sizeof(h));
  for (uint8_t i = 0; i < teams; ++i) {
    const StoredTeam t = store.teamAt(team_list[i]);
    out.put(&t, sizeof(t));
  }
  for (uint8_t i = 0; i < labels; ++i)
    out.put(&store.labelAt(label_list[i]), sizeof(Text<31>));
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& stored = store.match(i);
    if (stored.competition != competition) continue;
    StoredMatch m = stored;
    m.home = team(m.home);
    m.away = team(m.away);
    m.label = m.label < cfg::kStoreRoundLabels ? label_map[m.label] : kNoIndex;
    m.score_changed = 0;
    const bool block = stored.own < cfg::kStoreOwnMatches &&
                       store.ownAt(stored.own).match == stored.id;
    m.own = block ? 0 : kNoIndex;
    out.put(&m, sizeof(m));
    if (!block) continue;
    const auto& own = store.ownAt(stored.own);
    OwnBlock b;
    std::memset(static_cast<void*>(&b), 0, sizeof(b));
    b.home = own.home_name;
    b.away = own.away_name;
    b.events = own.event_count;
    out.put(&b, sizeof(b));
    for (uint8_t k = 0; k < own.event_count; ++k)
      out.put(&store.eventAt(own.events[k]), sizeof(StoredEvent));
  }
  for (std::size_t i = 0; i < store.rowCount(); ++i) {
    if (store.row(i).competition != competition) continue;
    StoredRow r = store.row(i);
    r.team = team(r.team);
    out.put(&r, sizeof(r));
  }
  return out.end();
}

bool check(Read read, void* context, Saved& out) {
  In in{read, context};
  return parse(in, out, nullptr, {});
}

bool load(Read read, void* context, MatchStore& store, const Clock& clock) {
  In in{read, context};
  Saved saved;
  return parse(in, saved, &store, clock);
}

const char* ShortNames::find(cfg::Provider provider, uint32_t id) const {
  if (id == 0) return nullptr;
  for (const auto& n : names_)
    if (n.id == id && n.provider == provider) return n.name.data;
  return nullptr;
}

bool ShortNames::set(cfg::Provider provider, uint32_t id,
                     std::string_view name) {
  if (id == 0) return false;
  ShortName* free = nullptr;
  for (auto& n : names_) {
    if (n.id == id && n.provider == provider) {
      if (name.empty())
        n = {};
      else
        n.name.assign(name);
      return true;
    }
    if (n.id == 0 && free == nullptr) free = &n;
  }
  if (name.empty()) return true;
  if (free == nullptr) return false;
  free->id = id;
  free->provider = provider;
  free->name.assign(name);
  return true;
}

std::size_t ShortNames::count() const {
  std::size_t n = 0;
  for (const auto& e : names_) n += e.id != 0;
  return n;
}

std::size_t ShortNames::encode(Write write, void* context) const {
  Out out{write, context};
  NamesHeader h;
  std::memset(static_cast<void*>(&h), 0, sizeof(h));
  h.magic = kNamesMagic;
  h.layout = kNamesLayout;
  h.version = kNamesVersion;
  h.count = static_cast<uint16_t>(count());
  out.put(&h, sizeof(h));
  for (const auto& n : names_)
    if (n.id != 0) out.put(&n, sizeof(n));
  return out.end();
}

bool ShortNames::decode(Read read, void* context) {
  // Read in place (no 1.5 KB copy on the stack); an invalid file leaves the
  // table empty.
  clear();
  In in{read, context};
  NamesHeader h;
  bool ok = in.get(&h, sizeof(h)) && h.magic == kNamesMagic &&
            h.layout == kNamesLayout && h.version == kNamesVersion &&
            h.count <= names_.size();
  for (uint16_t i = 0; ok && i < h.count; ++i) {
    auto& n = names_[i];
    ok = in.get(&n, sizeof(n)) && n.id != 0 && provider(n.provider) &&
         !n.name.empty() && terminated(n.name);
  }
  ok = ok && in.end();
  if (!ok) clear();
  return ok;
}

}  // namespace football::persist
