// Host acceptance of the actual codec and shipped source; fixtures stay on
// host.
#include "team_name_store.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include "team_name_catalogue.h"

namespace st = football::names::store;
namespace cat = football::names::catalogue;
using cfg::Provider;
namespace {
bool track_allocations = false;
std::size_t codec_allocations = 0;
}  // namespace
void* operator new(std::size_t n) {
  if (track_allocations) ++codec_allocations;
  if (auto p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct Memory {
  std::array<uint8_t, cfg::kTeamNamesFileBytes> bytes{};
  std::size_t used = 0, reads = 0, writes = 0, fail_read = SIZE_MAX,
              fail_write = SIZE_MAX, max_read = 0;
  static bool read(std::size_t at, void* out, std::size_t n, void* ctx) {
    auto& m = *static_cast<Memory*>(ctx);
    if (m.reads++ == m.fail_read || at > m.used || n > m.used - at)
      return false;
    m.max_read = std::max(m.max_read, n);
    std::memcpy(out, m.bytes.data() + at, n);
    return true;
  }
  static bool write(std::size_t at, const void* data, std::size_t n,
                    void* ctx) {
    auto& m = *static_cast<Memory*>(ctx);
    if (m.writes++ == m.fail_write || at > m.bytes.size() ||
        n > m.bytes.size() - at)
      return false;
    std::memcpy(m.bytes.data() + at, data, n);
    m.used = std::max(m.used, at + n);
    return true;
  }
  st::IO io() { return {this, read, write}; }
};
std::string clipped(std::string_view input, std::size_t n) {
  std::string_view out;
  assert(st::adopt(input, n, out));
  return std::string(out);
}
std::string text(const st::Reader& r, uint16_t ref) {
  char out[256];
  std::size_t n;
  assert(r.text(ref, out, sizeof(out), n));
  return {out, n};
}
struct Bound {
  uint32_t id;
  uint16_t club;
};
struct Fixture {
  std::vector<std::size_t> order;
  std::array<std::vector<Bound>, cfg::kProviderCount> ids;
  st::Counts counts;
  Fixture() {
    for (std::size_t i = 0; i < cat::count(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(),
              [](auto a, auto b) { return cat::at(a).key < cat::at(b).key; });
    counts.clubs = uint16_t(order.size());
    counts.exceptions = 48;
    for (std::size_t i = 0; i < order.size(); ++i) {
      const auto c = cat::at(order[i]);
      counts.aliases += c.aliasCount();
      for (const auto id : c.openligadb_ids)
        ids[0].push_back({id, uint16_t(i)});
      for (const auto id : c.espn_ids) ids[2].push_back({id, uint16_t(i)});
    }
    for (std::size_t p = 0; p < ids.size(); ++p) {
      std::sort(ids[p].begin(), ids[p].end(),
                [](auto a, auto b) { return a.id < b.id; });
      counts.bindings[p] = uint16_t(ids[p].size());
    }
  }
};
std::size_t encode(Memory& m, const Fixture& f, bool maximal = false) {
  st::Writer w;
  if (!w.begin(m.io(), f.counts, 17, 0xdeadbeef)) return 0;
  for (const auto i : f.order)
    if (!w.key(cat::at(i).key)) return 0;
  for (std::size_t p = 0; p < f.ids.size(); ++p)
    for (auto b : f.ids[p])
      if (!w.binding(static_cast<Provider>(p), b.id, b.club)) return 0;
  char de[27], en[27], short_name[4];
  for (std::size_t i = 0; i < f.order.size(); ++i) {
    const auto c = cat::at(f.order[i]);
    std::snprintf(de, sizeof(de), "%023zuDE", i);
    std::snprintf(en, sizeof(en), "%023zuEN", i);
    std::snprintf(short_name, sizeof(short_name), "%03zu", i);
    const st::Fields fields{
        {true, maximal ? std::string_view(de) : c.de},
        {true, maximal ? std::string_view(en) : c.en},
        {true, maximal ? std::string_view(short_name) : c.short_name},
        {true, c.league}};
    if (!w.club(uint16_t(i), fields)) return 0;
    for (std::size_t a = 0; a < c.aliasCount(); ++a)
      if (!w.alias(uint16_t(i), c.alias(a))) return 0;
  }
  for (unsigned p = 0; p < 4; ++p)
    for (unsigned i = 0; i < 12; ++i) {
      char exception[4];
      std::snprintf(exception, sizeof(exception), "%03u", 600 + p * 12 + i);
      if (!w.exception(static_cast<Provider>(p), 100000 + p * 100 + i,
                       maximal ? std::string_view(exception) : "ABCDE"))
        return 0;
    }
  return w.finish();
}
void recheck(Memory& m, const Fixture& f) {
  st::Reader r;
  assert(r.open(m.io(), m.used));
  assert(r.generation() == 17 && r.sourceFingerprint() == 0xdeadbeef);
  const auto c = r.counts();
  assert(c.clubs == 147 && c.aliases == 113 && c.exceptions == 48);
  assert(c.bindings[0] + c.bindings[2] == 288);
  std::size_t aliases = 0;
  for (std::size_t i = 0; i < f.order.size(); ++i) {
    const auto expected = cat::at(f.order[i]);
    char key[st::kKeyBytes + 1];
    uint16_t found = st::kNone;
    st::Club actual;
    assert(r.key(uint16_t(i), key) && std::string_view(key) == expected.key);
    assert(r.findKey(expected.key, found) == st::Lookup::kFound && found == i);
    assert(r.club(uint16_t(i), actual) && actual.present == 15);
    assert(text(r, actual.de) == clipped(expected.de, cfg::kTeamNameBytes));
    assert(text(r, actual.en) == clipped(expected.en, cfg::kTeamNameBytes));
    assert(text(r, actual.short_name) ==
           clipped(expected.short_name, cfg::kTeamShortBytes));
    assert(text(r, actual.league) == expected.league);
    for (std::size_t a = 0; a < expected.aliasCount(); ++a) {
      st::Alias alias;
      assert(r.alias(uint16_t(aliases++), alias) && alias.club == i &&
             text(r, alias.text) == expected.alias(a));
    }
  }
  for (std::size_t p = 0; p < 4; ++p) {
    for (std::size_t i = 0; i < f.ids[p].size(); ++i) {
      st::Binding binding;
      assert(r.binding(static_cast<Provider>(p), uint16_t(i), binding));
      assert(binding.id == f.ids[p][i].id && binding.club == f.ids[p][i].club);
      uint16_t found;
      const auto before = m.reads;
      assert(r.findId(static_cast<Provider>(p), binding.id, found) ==
                 st::Lookup::kFound &&
             found == binding.club);
      assert(m.reads - before <= 16 * 10 + 10);
    }
    for (unsigned i = 0; i < 12; ++i) {
      uint16_t ref;
      assert(r.findException(static_cast<Provider>(p), 100000 + p * 100 + i,
                             ref) == st::Lookup::kFound &&
             text(r, ref) == "ABC");
    }
  }
  uint16_t none;
  assert(r.findKey("no-such-club", none) == st::Lookup::kMissing);
  assert(r.findId(Provider::kApiFootball, 99, none) == st::Lookup::kMissing);
}
void checkedReceipt(Memory& original) {
  st::Reader checked;
  assert(checked.open(original.io(), original.used));
  st::Reader reused;
  const auto before = original.reads;
  checked.detach();
  assert(reused.reuse(original.io(), checked, original.used));
  assert(original.reads == before + 1);
  Memory changed = original;
  changed.bytes[16] ^= 1;
  st::Reader rejected;
  assert(!rejected.reuse(changed.io(), checked, changed.used));
}

uint32_t crc(uint32_t value, uint8_t byte) {
  value = ~value;
  value ^= byte;
  for (unsigned i = 0; i < 8; ++i)
    value = (value >> 1) ^ (0xedb88320u & -(value & 1));
  return ~value;
}
void fixCrc(Memory& m) {
  for (unsigned i = 92; i < 96; ++i) m.bytes[i] = 0;
  uint32_t value = 0;
  for (std::size_t i = 0; i < m.used; ++i) value = crc(value, m.bytes[i]);
  for (unsigned i = 0; i < 4; ++i) m.bytes[92 + i] = uint8_t(value >> (8 * i));
}
void edgeCases() {
  std::string_view prefix;
  assert(st::adopt("abäx", 3, prefix) && prefix == "ab");
  assert(st::adopt("äx", 3, prefix) && prefix == "äx");
  assert(st::adopt("🙂x", 3, prefix) && prefix.empty());
  assert(!st::adopt(std::string_view("\xc0\x80", 2), 3, prefix));
  assert(!st::adopt(std::string_view("a\0b", 3), 3, prefix));
  Memory m;
  st::Counts counts;
  counts.clubs = 1;
  counts.aliases = 1;
  counts.bindings = {1, 1, 1, 1};
  st::Writer w;
  assert(w.begin(m.io(), counts, 1, 2));
  assert(w.key("full-key"));
  for (unsigned p = 0; p < 4; ++p)
    assert(w.binding(static_cast<Provider>(p), UINT32_MAX, 0));
  std::string long_text(5000, 'L');
  assert(w.club(0, {{true, ""}, {}, {true, "long"}, {true, long_text}}));
  assert(w.alias(0, long_text));
  assert(w.finish());
  st::Reader r;
  assert(r.open(m.io(), m.used));
  st::Club row;
  assert(r.club(0, row) && row.present == 13 && row.de && !row.en &&
         text(r, row.de).empty());
  std::size_t streamed = 0;
  assert(r.streamText(
             row.league,
             [](const void* data, std::size_t n, void* ctx) {
               for (std::size_t i = 0; i < n; ++i)
                 assert(static_cast<const char*>(data)[i] == 'L');
               *static_cast<std::size_t*>(ctx) += n;
               return true;
             },
             &streamed) &&
         streamed == long_text.size());
  st::Alias a;
  assert(r.alias(0, a) && a.text == row.league);
  for (unsigned p = 0; p < 4; ++p) {
    uint16_t found;
    assert(r.findId(static_cast<Provider>(p), UINT32_MAX, found) ==
               st::Lookup::kFound &&
           found == 0);
  }
  Memory bad;
  st::Writer over;
  counts = {};
  counts.clubs = 1;
  assert(over.begin(bad.io(), counts, 1, 0));
  assert(over.key("a"));
  std::string huge(cfg::kTeamNamesFileBytes, 'Q');
  assert(!over.club(0, {{}, {}, {}, {true, huge}}));
  assert(over.finish() == 0);
  Memory duplicate;
  st::Writer d;
  counts.clubs = 2;
  assert(d.begin(duplicate.io(), counts, 1, 0));
  assert(d.key("a"));
  assert(!d.key("a"));
  assert(d.finish() == 0);
  Memory unordered;
  st::Writer u;
  counts = {};
  counts.clubs = 1;
  counts.bindings[0] = 2;
  assert(u.begin(unordered.io(), counts, 1, 0));
  assert(u.key("a"));
  assert(u.binding(Provider::kOpenLigaDb, 5, 0));
  assert(!u.binding(Provider::kOpenLigaDb, 4, 0));
  assert(!u.finish());
}
int main() {
  Fixture fixture;
  Memory committed;
  track_allocations = true;
  const auto actual = encode(committed, fixture);
  track_allocations = false;
  const auto candidate_reads = committed.reads;
  assert(actual && codec_allocations == 0);
  recheck(committed, fixture);
  checkedReceipt(committed);
  Memory maximal;
  const auto maximal_bytes = encode(maximal, fixture, true);
  assert(maximal_bytes && maximal_bytes <= cfg::kTeamNamesFileBytes);
  st::Reader maximal_reader;
  assert(maximal_reader.open(maximal.io(), maximal.used));
  edgeCases();
  // Every byte is covered by CRC; the old committed source is never passed as
  // candidate.
  for (std::size_t i = 0; i < committed.used; ++i) {
    Memory damaged = committed;
    damaged.bytes[i] ^= 0x40;
    st::Reader r;
    assert(!r.open(damaged.io(), damaged.used));
  }
  for (std::size_t n = 0; n < committed.used; ++n) {
    st::Reader r;
    assert(!r.open(committed.io(), n));
  }
  // Recomputed checksums cannot hide unsupported versions, malformed offsets,
  // field flags, invalid refs, or an invalid UTF-8 pool.
  const std::size_t record = 96;
  const std::array<std::size_t, 9> hostile{
      4, 6, 24, 30, 40, 52, record, record + 8, committed.used - 1};
  for (auto at : hostile) {
    Memory damaged = committed;
    damaged.bytes[at] ^= 0x80;
    fixCrc(damaged);
    st::Reader r;
    assert(!r.open(damaged.io(), damaged.used));
  }
  const auto old = committed.bytes;
  for (std::size_t i = 0; i < committed.writes; ++i) {
    Memory candidate;
    candidate.fail_write = i;
    assert(encode(candidate, fixture) == 0);
    assert(committed.bytes == old);
  }
  // Representative candidate read failures include deduplication and
  // validation.
  for (std::size_t i : {std::size_t(0), std::size_t(1), std::size_t(50),
                        std::size_t(500), candidate_reads / 2}) {
    Memory candidate;
    candidate.fail_read = i;
    assert(encode(candidate, fixture) == 0);
    assert(committed.bytes == old);
  }
  std::cout << "PASS clubs=147 aliases=113 bindings=288 exceptions=48 bytes="
            << actual << " maximal=" << maximal_bytes
            << " Reader=" << sizeof(st::Reader)
            << " Writer=" << sizeof(st::Writer)
            << " max_read=" << committed.max_read
            << " codec_heap_allocations=" << codec_allocations << '\n';
}
