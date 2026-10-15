// Host checks of the competition files and the short-name file
// (football::persist) and of the short-name replacement in the views. The
// matches, events, rows and names below are written by the agent; they
// prove only the file format and its checks as implemented, not that a
// device shows the data after a reboot (the owner's device acceptance).

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "app_config.h"
#include "football_files.h"
#include "match_store.h"
#include "store_views.h"

namespace {

using football::Clock;
using football::kMatchOwn;
using football::kNoIndex;
using football::MatchStore;
using football::Phase;
using football::StoredEvent;
using football::StoredMatch;
using football::StoredRow;
namespace persist = football::persist;
namespace view = football::view;

constexpr uint8_t kLeague = 3, kCup = 5;
constexpr auto kOlg = cfg::Provider::kOpenLigaDb;
constexpr auto kEspn = cfg::Provider::kEspn;
constexpr int64_t kSaturday = 1792195200;  // 2026-10-17 00:00 UTC
constexpr Clock kNow{kSaturday + 8 * 3600, 5000};

MatchStore store;

// An in-memory file.
struct Buffer {
  std::vector<uint8_t> bytes;
  std::size_t at = 0;
};
bool put(const void* bytes, std::size_t size, void* context) {
  auto& b = *static_cast<Buffer*>(context);
  const auto* p = static_cast<const uint8_t*>(bytes);
  b.bytes.insert(b.bytes.end(), p, p + size);
  return true;
}
bool get(void* bytes, std::size_t size, void* context) {
  auto& b = *static_cast<Buffer*>(context);
  if (b.at + size > b.bytes.size()) return false;
  std::memcpy(bytes, b.bytes.data() + b.at, size);
  b.at += size;
  return true;
}
bool failingPut(const void*, std::size_t, void*) { return false; }

// Replaces the trailing CRC after a deliberate change of the file.
void reseal(Buffer& b) {
  const std::size_t body = b.bytes.size() - sizeof(uint32_t);
  const uint32_t crc = persist::crc32(0, b.bytes.data(), body);
  std::memcpy(b.bytes.data() + body, &crc, sizeof(crc));
}

StoredMatch* add(uint8_t competition, uint32_t id, uint32_t home,
                 uint32_t away, int64_t kickoff, Phase phase,
                 bool own = false, std::string_view label = {}) {
  StoredMatch m;
  m.id = id;
  m.competition = competition;
  m.provider = kOlg;
  m.round = 7;
  m.kickoff = static_cast<uint32_t>(kickoff);
  m.phase = phase;
  char name[16];
  snprintf(name, sizeof(name), "Team %u", static_cast<unsigned>(home));
  m.home = store.team(competition, kOlg, home, name);
  snprintf(name, sizeof(name), "Team %u", static_cast<unsigned>(away));
  m.away = store.team(competition, kOlg, away, name);
  if (!label.empty()) m.label = store.label(label);
  if (own) m.flags = kMatchOwn;
  return store.upsert(m, kNow, own ? "Own Club Full" : "",
                      own ? "Guest Club Full" : "");
}

StoredEvent goal(uint32_t identity, uint8_t minute, const char* player) {
  StoredEvent e;
  e.identity = identity;
  e.revision = identity;
  e.minute = minute;
  e.actor = football::Side::kHome;
  e.player.assign(player);
  return e;
}

void row(uint8_t competition, uint32_t team, uint8_t position,
         int16_t points) {
  StoredRow r;
  r.competition = competition;
  r.team = store.team(competition, kOlg, team, "");
  r.position = position;
  r.points = points;
  TEST_ASSERT_TRUE(store.appendRow(r));
}

// Competition kLeague: a scheduled match with a round label, an own
// finished match with two events, a two-row table and a notice; one match
// of kCup that the league file must not contain.
void fill() {
  add(kLeague, 101, 10, 11, kSaturday + 13 * 3600, Phase::kScheduled, false,
      "7. Spieltag");
  StoredMatch* own =
      add(kLeague, 102, 40, 12, kSaturday + 4 * 3600, Phase::kFinished, true);
  TEST_ASSERT_NOT_NULL(own);
  own->principal = {2, 1};
  TEST_ASSERT_TRUE(store.beginEvents(*own));
  store.addEvent(goal(1, 12, "Scorer One"));
  store.addEvent(goal(2, 80, "Scorer Two"));
  store.endEvents();
  add(kCup, 201, 50, 51, kSaturday + 15 * 3600, Phase::kScheduled);
  store.beginTable(kLeague);
  row(kLeague, 40, 1, 19);
  row(kLeague, 10, 2, 17);
  TEST_ASSERT_TRUE(store.commitTable(true));
  store.flag(kLeague, football::kNoticeEvents, true);
}

persist::Saved saved() {
  persist::Saved s;
  s.competition = kLeague;
  s.route_key = 0x1234abcd;
  s.saved_utc = kNow.utc;
  auto& primary = s.source;
  TEST_ASSERT_TRUE(primary.resolved.season.assign("2026"));
  primary.resolved.season_utc = static_cast<uint32_t>(kNow.utc - 7200);
  primary.resolved.round = 7;
  primary.resolved.day_count = 2;
  primary.resolved.days[0] = 20261017;
  primary.resolved.days[1] = 20261018;
  primary.done = {kNow.utc - 600, kNow.utc - 60};  // fixtures, table
  return s;
}

Buffer encoded() {
  Buffer b;
  const std::size_t size = persist::encode(store, saved(), put, &b);
  TEST_ASSERT_EQUAL(b.bytes.size(), size);
  return b;
}

const StoredMatch& found(uint32_t id) {
  const StoredMatch* m = store.find(kLeague, kOlg, id);
  TEST_ASSERT_NOT_NULL(m);
  return *m;
}

std::string_view teamOf(uint8_t index) {
  TEST_ASSERT_NOT_EQUAL(kNoIndex, index);
  return store.teamAt(index).short_name.data;
}

bool checks(Buffer b) {
  b.at = 0;
  persist::Saved out;
  return persist::check(get, &b, out);
}

}  // namespace

void setUp() { store.clear(); }
void tearDown() {}

// ---------------------------------------------------------- competition

void test_counting_pass_matches_the_written_size() {
  fill();
  const std::size_t counted = persist::encode(store, saved(), nullptr, nullptr);
  TEST_ASSERT_TRUE(counted > 0);
  TEST_ASSERT_EQUAL(counted, encoded().bytes.size());
  TEST_ASSERT_EQUAL(0, persist::encode(store, saved(), failingPut, nullptr));
}

void test_round_trip_restores_the_competition() {
  fill();
  Buffer b = encoded();

  persist::Saved header;
  TEST_ASSERT_TRUE(persist::check(get, &b, header));
  TEST_ASSERT_EQUAL_UINT8(kLeague, header.competition);
  TEST_ASSERT_EQUAL_UINT32(0x1234abcd, header.route_key);
  TEST_ASSERT_EQUAL_INT64(kNow.utc, header.saved_utc);
  TEST_ASSERT_EQUAL_UINT8(football::kNoticeEvents, header.notices);
  const auto& primary = header.source;
  TEST_ASSERT_TRUE(primary.resolved.season.view() == "2026");
  // The season's resolution time (OQ-259) comes back with it.
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kNow.utc - 7200),
                           primary.resolved.season_utc);
  TEST_ASSERT_EQUAL_UINT8(7, primary.resolved.round);
  TEST_ASSERT_EQUAL_UINT8(2, primary.resolved.day_count);
  TEST_ASSERT_EQUAL_UINT32(20261018, primary.resolved.days[1]);
  TEST_ASSERT_EQUAL_INT64(kNow.utc - 600, primary.done[0]);
  TEST_ASSERT_EQUAL_INT64(kNow.utc - 60, primary.done[1]);

  // Into a store that already holds another competition's teams: the
  // file-local indices are mapped to new store indices.
  store.clear();
  add(kCup, 301, 60, 61, kSaturday, Phase::kScheduled);
  b.at = 0;
  const Clock later{kNow.utc + 600, 20};
  TEST_ASSERT_TRUE(persist::load(get, &b, store, later));

  TEST_ASSERT_EQUAL(3, store.matchCount());
  TEST_ASSERT_NULL(store.find(kCup, kOlg, 201));
  TEST_ASSERT_NOT_NULL(store.find(kCup, kOlg, 301));
  const StoredMatch& next = found(101);
  TEST_ASSERT_TRUE(teamOf(next.home) == "Team 10");
  TEST_ASSERT_TRUE(teamOf(next.away) == "Team 11");
  TEST_ASSERT_EQUAL_UINT32(kSaturday + 13 * 3600, next.kickoff);
  TEST_ASSERT_EQUAL_UINT8(7, next.round);
  TEST_ASSERT_NOT_EQUAL(kNoIndex, next.label);
  TEST_ASSERT_EQUAL_STRING("7. Spieltag", store.labelAt(next.label).data);

  const StoredMatch& own = found(102);
  TEST_ASSERT_TRUE(own.isOwn());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Phase::kFinished),
                          static_cast<uint8_t>(own.phase));
  TEST_ASSERT_EQUAL_UINT8(2, own.principal.home);
  TEST_ASSERT_EQUAL_UINT8(1, own.principal.away);
  TEST_ASSERT_EQUAL_UINT32(0, own.score_changed);  // monotonic: not kept
  TEST_ASSERT_NOT_EQUAL(kNoIndex, own.own);
  const auto& slot = store.ownAt(own.own);
  TEST_ASSERT_EQUAL_STRING("Own Club Full", slot.home_name.data);
  TEST_ASSERT_EQUAL_STRING("Guest Club Full", slot.away_name.data);
  TEST_ASSERT_EQUAL_UINT8(2, slot.event_count);
  TEST_ASSERT_EQUAL_STRING("Scorer One",
                           store.eventAt(slot.events[0]).player.data);
  TEST_ASSERT_EQUAL_UINT8(80, store.eventAt(slot.events[1]).minute);

  std::size_t rows = 0;
  for (std::size_t i = 0; i < store.rowCount(); ++i) {
    const StoredRow& r = store.row(i);
    if (r.competition != kLeague) continue;
    ++rows;
    TEST_ASSERT_TRUE(teamOf(r.team) == (r.position == 1 ? "Team 40"
                                                        : "Team 10"));
  }
  TEST_ASSERT_EQUAL(2, rows);
  TEST_ASSERT_TRUE((store.notices(kLeague) & football::kNoticeEvents) != 0);
}

void test_a_changed_byte_or_a_short_file_is_rejected() {
  fill();
  const Buffer good = encoded();
  TEST_ASSERT_TRUE(checks(good));
  for (const std::size_t at :
       {std::size_t{0}, good.bytes.size() / 2, good.bytes.size() - 1}) {
    Buffer bad = good;
    bad.bytes[at] ^= 0x40;
    TEST_ASSERT_FALSE(checks(bad));
  }
  Buffer short_file = good;
  short_file.bytes.resize(good.bytes.size() - 1);
  TEST_ASSERT_FALSE(checks(short_file));

  // A failed load keeps a table the store already holds.
  Buffer bad = good;
  bad.bytes[good.bytes.size() - 1] ^= 0x01;  // CRC
  store.clear();
  store.beginTable(kLeague);
  row(kLeague, 99, 1, 3);
  TEST_ASSERT_TRUE(store.commitTable(true));
  TEST_ASSERT_FALSE(persist::load(get, &bad, store, kNow));
  TEST_ASSERT_EQUAL(1, store.rowCount());
  TEST_ASSERT_TRUE(teamOf(store.row(0).team) == "");
}

void test_header_version_and_counts_are_checked() {
  fill();
  const Buffer good = encoded();
  // Header: magic (4), layout (4), version (2), competition (1), notices
  // (1), route key (4), saved UTC (8), then the counts of teams, labels,
  // matches and rows.
  struct Patch {
    std::size_t at;
    uint8_t value;
  };
  for (const Patch p :
       {Patch{4, 0x00},   // layout of another build
        Patch{8, 0x02},   // version 2 (before OQ-248)
        Patch{10, 0x00},  // competition 0
        Patch{24, static_cast<uint8_t>(cfg::kStoreTeams + 1)},
        Patch{26, static_cast<uint8_t>(cfg::kStoreMatches + 1)}}) {
    Buffer bad = good;
    if (bad.bytes[p.at] == p.value) bad.bytes[p.at] ^= 0x01;
    else bad.bytes[p.at] = p.value;
    reseal(bad);
    TEST_ASSERT_FALSE(checks(bad));
  }
  Buffer resealed = good;
  reseal(resealed);
  TEST_ASSERT_TRUE(checks(resealed));
}

// ---------------------------------------------------------- short names

void test_short_names_set_find_remove_and_full() {
  persist::ShortNames names;
  TEST_ASSERT_FALSE(names.set(kOlg, 0, "Zero"));
  TEST_ASSERT_TRUE(names.set(kOlg, 40, "Own"));
  TEST_ASSERT_TRUE(names.set(kEspn, 40, "Other provider"));
  TEST_ASSERT_EQUAL_STRING("Own", names.find(kOlg, 40));
  TEST_ASSERT_EQUAL_STRING("Other provider", names.find(kEspn, 40));
  TEST_ASSERT_NULL(names.find(kOlg, 41));
  TEST_ASSERT_TRUE(names.set(kOlg, 40, "Own Club"));
  TEST_ASSERT_EQUAL_STRING("Own Club", names.find(kOlg, 40));
  TEST_ASSERT_EQUAL(2, names.count());
  TEST_ASSERT_TRUE(names.set(kOlg, 40, ""));
  TEST_ASSERT_NULL(names.find(kOlg, 40));
  TEST_ASSERT_TRUE(names.set(kOlg, 41, ""));  // removing none is fine
  TEST_ASSERT_EQUAL(1, names.count());

  for (uint32_t id = 1; names.count() < cfg::kShortNamesMax; ++id)
    TEST_ASSERT_TRUE(names.set(kOlg, 1000 + id, "Name"));
  TEST_ASSERT_FALSE(names.set(kOlg, 5000, "One too many"));
  TEST_ASSERT_TRUE(names.set(kEspn, 40, "Still changeable"));
  TEST_ASSERT_EQUAL_STRING("Still changeable", names.find(kEspn, 40));
}

void test_short_name_file_round_trip_and_corruption() {
  persist::ShortNames names;
  TEST_ASSERT_TRUE(names.set(kOlg, 40, "Own Club"));
  TEST_ASSERT_TRUE(names.set(kEspn, 7, "Seven"));
  Buffer b;
  TEST_ASSERT_EQUAL(names.encode(put, &b), b.bytes.size());

  persist::ShortNames back;
  TEST_ASSERT_TRUE(back.decode(get, &b));
  TEST_ASSERT_EQUAL(2, back.count());
  TEST_ASSERT_EQUAL_STRING("Own Club", back.find(kOlg, 40));
  TEST_ASSERT_EQUAL_STRING("Seven", back.find(kEspn, 7));

  // An invalid file leaves the table empty.
  Buffer bad = b;
  bad.at = 0;
  bad.bytes[bad.bytes.size() / 2] ^= 0x20;
  TEST_ASSERT_FALSE(back.decode(get, &bad));
  TEST_ASSERT_EQUAL(0, back.count());
  Buffer short_file = b;
  short_file.at = 0;
  short_file.bytes.resize(short_file.bytes.size() - 2);
  TEST_ASSERT_FALSE(back.decode(get, &short_file));

  // The file has its own version (OQ-260): 1, while the competition files
  // are at 3; another version is not read.
  TEST_ASSERT_EQUAL_HEX8(1, b.bytes[8]);
  fill();
  TEST_ASSERT_EQUAL_HEX8(3, encoded().bytes[8]);
  Buffer other = b;
  other.at = 0;
  other.bytes[8] = 2;
  TEST_ASSERT_FALSE(back.decode(get, &other));

  persist::ShortNames empty;
  Buffer none;
  empty.encode(put, &none);
  TEST_ASSERT_TRUE(back.decode(get, &none));
  TEST_ASSERT_EQUAL(0, back.count());
}

// ---------------------------------------------------------------- views

void test_views_use_the_replacement_short_name() {
  fill();
  persist::ShortNames names;
  TEST_ASSERT_TRUE(names.set(kOlg, 10, "Ten"));
  const StoredMatch& next = found(101);
  TEST_ASSERT_EQUAL_STRING("Team 10", view::teamName(store, next, true));
  TEST_ASSERT_EQUAL_STRING("Ten", view::teamName(store, next, true, &names));
  TEST_ASSERT_EQUAL_STRING("Team 11", view::teamName(store, next, false,
                                                     &names));
  // An own match shows its full names.
  TEST_ASSERT_EQUAL_STRING("Own Club Full",
                           view::fullName(store, found(102), true, &names));

  for (std::size_t i = 0; i < store.rowCount(); ++i) {
    const StoredRow& r = store.row(i);
    if (r.position != 2) continue;
    TEST_ASSERT_EQUAL_STRING("Team 10", view::rowName(store, r));
    TEST_ASSERT_EQUAL_STRING("Ten", view::rowName(store, r, &names));
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_counting_pass_matches_the_written_size);
  RUN_TEST(test_round_trip_restores_the_competition);
  RUN_TEST(test_a_changed_byte_or_a_short_file_is_rejected);
  RUN_TEST(test_header_version_and_counts_are_checked);
  RUN_TEST(test_short_names_set_find_remove_and_full);
  RUN_TEST(test_short_name_file_round_trip_and_corruption);
  RUN_TEST(test_views_use_the_replacement_short_name);
  return UNITY_END();
}
