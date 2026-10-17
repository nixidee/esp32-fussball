#include "store_views.h"

#include <algorithm>
#include <cstdlib>

#include "football_files.h"

namespace football::view {
namespace {

// Stable insertion sort of at most cfg::kStoreMatches indices; no heap
// buffer (std::stable_sort may allocate one).
template <class Index, class Less>
void stableSort(Index* first, unsigned count, Less less) {
  for (unsigned i = 1; i < count; ++i) {
    const Index value = first[i];
    unsigned j = i;
    for (; j > 0 && less(value, first[j - 1]); --j) first[j] = first[j - 1];
    first[j] = value;
  }
}

bool goals(const StoredMatch& m) {
  return m.principal.valid() && m.principal.home + m.principal.away > 0;
}

// Activity rank of "One page"; lower first.
int rank(const StoredMatch& m) {
  if (running(m.phase)) return goals(m) ? 0 : 1;
  return m.phase == Phase::kFinished ? 2 : 3;
}

int64_t distance(const StoredMatch& m, int64_t utc) {
  return std::llabs(utc - int64_t{m.kickoff});
}

bool detailSlot(const MatchStore& store, const StoredMatch& m) {
  return store.hasDetail(m);
}

int conferencePin(const MatchStore& store, const List& list, bool enabled) {
  if (enabled)
    for (unsigned i = 0; i < list.count; ++i)
      if (store.match(list.index[i]).isOwn()) return list.index[i];
  return -1;
}

// Preserve the original page count and kickoff order. Fill a sparse final
// page from the back: at most one row per donor for three or more pages,
// or two from the first of two pages. Never make the final page larger than
// its predecessor. A repeated pin is outside this unique-match partition.
struct ConferenceSplit {
  unsigned pages = 0, slots = 0, moved = 0;
  ConferenceSplit(unsigned count, unsigned capacity) : slots(capacity) {
    if (!count || !slots) return;
    pages = (count + slots - 1) / slots;
    const unsigned last = count - (pages - 1) * slots;
    if (pages == 2)
      moved = std::min(2u, (slots - last) / 2);
    else if (pages > 2 && last < slots)
      moved = std::min(pages - 1, slots - last - 1);
  }
  unsigned first(unsigned page) const {
    if (pages == 2) return page ? slots - moved : 0;
    const unsigned donors_begin = pages - 1 - moved;
    return page * slots - (page > donors_begin ? page - donors_begin : 0);
  }
  unsigned size(unsigned page, unsigned count) const {
    const unsigned begin = first(page);
    return page + 1 < pages ? first(page + 1) - begin : count - begin;
  }
};

}  // namespace

void Shown::add(const settings::Competition& entry) {
  if (!entry.active || entry.id == 0) return;
  add(Source{entry.id, entry.primary.provider,
             decimalId(entry.primary.team.view())});
}

void Shown::add(const Source& source) {
  if (source.competition == 0 || this->source(source.competition) ||
      count_ >= sources_.size())
    return;
  sources_[count_++] = source;
}

const Source* Shown::source(uint8_t competition) const {
  for (uint8_t i = 0; i < count_; ++i)
    if (sources_[i].competition == competition) return &sources_[i];
  return nullptr;
}

bool Shown::shows(const StoredMatch& match) const {
  const auto* s = source(match.competition);
  return s != nullptr && s->provider == match.provider;
}

bool started(Phase phase) {
  return running(phase) || phase == Phase::kFinished ||
         phase == Phase::kAbandoned;
}

ConferencePage conferencePage(const MatchStore& store, const List& list,
                              const ConferenceRequest& request) {
  ConferencePage page;
  const unsigned per_page =
      std::min<unsigned>(request.per_page, kConferenceRowsMax);
  if (list.count == 0 || per_page == 0) return page;
  const int pinned = conferencePin(store, list, request.pin_own);
  std::array<uint8_t, cfg::kStoreMatches> others{};
  unsigned count = 0;
  for (unsigned i = 0; i < list.count; ++i)
    if (list.index[i] != pinned) others[count++] = list.index[i];
  const unsigned slots = per_page - (pinned >= 0 ? 1 : 0);
  unsigned first = 0, rows = slots;
  page.pages = 1;
  if (request.mode == cfg::ConferenceMode::kOnePage && !request.manual) {
    stableSort(others.data(), count, [&](uint8_t a, uint8_t b) {
      const auto& x = store.match(a);
      const auto& y = store.match(b);
      const int rx = rank(x), ry = rank(y);
      if (rx != ry) return rx < ry;
      return rx == 0 && x.score_changed > y.score_changed;
    });
  } else if (slots > 0 && count > 0) {
    const ConferenceSplit split(count, slots);
    page.pages = static_cast<uint8_t>(std::min(split.pages, 255u));
    page.page = static_cast<uint8_t>(
        request.manual ? std::min<unsigned>(request.page, page.pages - 1u)
                       : request.page % page.pages);
    first = split.first(page.page);
    rows = split.size(page.page, count);
  }
  if (pinned >= 0) page.rows[page.row_count++] = static_cast<uint8_t>(pinned);
  for (unsigned i = first; i < count && i - first < rows; ++i)
    page.rows[page.row_count++] = others[i];
  return page;
}

int conferencePageOf(const MatchStore& store, const List& list,
                     const ConferenceRequest& request, uint8_t store_index) {
  const unsigned per_page =
      std::min<unsigned>(request.per_page, kConferenceRowsMax);
  if (!list.count || !per_page) return -1;
  const int pinned = conferencePin(store, list, request.pin_own);
  if (pinned == store_index) return 0;
  const unsigned count = list.count - (pinned >= 0);
  const ConferenceSplit split(count, per_page - (pinned >= 0));
  unsigned ordinal = 0;
  for (unsigned i = 0; i < list.count; ++i) {
    if (list.index[i] == pinned) continue;
    if (list.index[i] == store_index) {
      for (unsigned page = 0; page < split.pages; ++page)
        if (ordinal < split.first(page) + split.size(page, count))
          return static_cast<int>(page);
      return -1;
    }
    ++ordinal;
  }
  return -1;
}

int ownForOverview(const MatchStore& store, const Shown& shown, int64_t utc) {
  int best = -1;
  bool best_live = false;
  int64_t best_distance = 0;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (!m.isOwn() || !shown.shows(m)) continue;
    const bool live = running(m.phase);
    const int64_t d = m.kickoff ? distance(m, utc) : INT64_MAX;
    if (best < 0 || (live && !best_live) ||
        (live == best_live && d < best_distance)) {
      best = static_cast<int>(i);
      best_live = live;
      best_distance = d;
    }
  }
  return best;
}

uint16_t matchCount(const MatchStore& store, const Shown& shown,
                    uint8_t competition) {
  uint16_t count = 0;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (m.competition == competition && shown.shows(m)) ++count;
  }
  return count;
}

void ownSides(const MatchStore& store, const StoredMatch& match,
              const Shown& shown, bool& home, bool& away) {
  home = away = false;
  const auto* s = shown.source(match.competition);
  if (!match.isOwn() || s == nullptr || s->club == 0) return;
  const auto club = [&](uint8_t team) {
    return team < cfg::kStoreTeams && store.teamAt(team).id == s->club &&
           store.teamAt(team).provider == match.provider;
  };
  home = club(match.home);
  away = club(match.away);
}

namespace {
// The owner's replacement of a stored team's short name, nullptr when none.
const char* replaced(const MatchStore& store, uint8_t team,
                     const persist::ShortNames* names) {
  if (!names || team >= cfg::kStoreTeams) return nullptr;
  const auto& t = store.teamAt(team);
  return t.id != 0 ? names->find(t.provider, t.id) : nullptr;
}
}  // namespace

const char* teamName(const MatchStore& store, const StoredMatch& match,
                     bool home, const persist::ShortNames* names) {
  const uint8_t team = home ? match.home : match.away;
  if (const char* name = replaced(store, team, names)) return name;
  if (team < cfg::kStoreTeams && store.teamAt(team).id != 0 &&
      !store.teamAt(team).short_name.empty())
    return store.teamAt(team).short_name.data;
  if (detailSlot(store, match)) {
    const auto& own = store.detailAt(match.detail);
    return home ? own.home_name.data : own.away_name.data;
  }
  return "";
}

const char* fullName(const MatchStore& store, const StoredMatch& match,
                     bool home, const persist::ShortNames* names) {
  if (detailSlot(store, match)) {
    const auto& own = store.detailAt(match.detail);
    const auto& name = home ? own.home_name : own.away_name;
    if (!name.empty()) return name.data;
  }
  return teamName(store, match, home, names);
}

const char* rowName(const MatchStore& store, const StoredRow& row,
                    const persist::ShortNames* names) {
  if (const char* name = replaced(store, row.team, names)) return name;
  if (row.team < cfg::kStoreTeams)
    return store.teamAt(row.team).short_name.data;
  return "";
}

Table table(const MatchStore& store, uint8_t competition) {
  Table out;
  for (std::size_t i = 0; i < store.rowCount() && out.count < out.row.size();
       ++i) {
    const auto& r = store.row(i);
    if (r.competition != competition) continue;
    if ((r.flags & kRowOwn) && out.own < 0) out.own = out.count;
    out.row[out.count++] = static_cast<uint8_t>(i);
  }
  return out;
}

Events events(const MatchStore& store, const StoredMatch& match) {
  Events out;
  if (!detailSlot(store, match)) return out;
  const auto& own = store.detailAt(match.detail);
  // Sort key per kept event: shootout, minute (inherited when unknown),
  // added minute.
  std::array<uint32_t, cfg::kMatchEvents> key{};
  uint8_t minute = 0;
  for (uint8_t i = 0; i < own.event_count && i < own.events.size(); ++i) {
    const uint8_t at = own.events[i];
    const auto& e = store.eventAt(at);
    if (e.minute != kNoValue) minute = e.minute;
    if (e.flags & kEventRemoved) continue;
    const uint8_t added =
        e.minute != kNoValue && e.added != kNoValue ? e.added : 0;
    key[out.count] = (e.flags & kEventShootout ? 1u << 16 : 0u) |
                     uint32_t{minute} << 8 | added;
    out.index[out.count++] = at;
  }
  std::array<uint8_t, cfg::kMatchEvents> order{};
  for (uint8_t i = 0; i < out.count; ++i) order[i] = i;
  stableSort(order.data(), out.count,
             [&](uint8_t a, uint8_t b) { return key[a] < key[b]; });
  const auto unsorted = out.index;
  for (uint8_t i = 0; i < out.count; ++i) out.index[i] = unsorted[order[i]];
  return out;
}

}  // namespace football::view
