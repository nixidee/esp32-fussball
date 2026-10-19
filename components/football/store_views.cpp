#include "store_views.h"

#include <algorithm>
#include <cstdlib>

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

int conferencePin(const MatchStore& store, const List& list, bool enabled,
                  uint8_t competition) {
  if (enabled)
    for (unsigned i = 0; i < list.count; ++i)
      if (store.isOwn(competition, store.match(list.index[i])))
        return list.index[i];
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

void Shown::add(const settings::CompetitionRoute& entry) {
  if (!entry.active || entry.id == 0) return;
  add(Source{entry.id});
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

bool Shown::shows(const MatchStore& store, const StoredMatch& match) const {
  for (unsigned i = 0; i < count_; ++i)
    if (store.includes(sources_[i].competition, match.source)) return true;
  return false;
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
  const int pinned =
      conferencePin(store, list, request.pin_own, request.competition);
  const unsigned count = list.count - (pinned >= 0 ? 1 : 0);
  const unsigned slots = per_page - (pinned >= 0 ? 1 : 0);
  page.pages = 1;
  if (pinned >= 0)
    page.rows[page.row_count++] = static_cast<MatchIndex>(pinned);
  if (request.mode == cfg::ConferenceMode::kOnePage && !request.manual) {
    // Only the visible rows need ranking. Keep their stable top-N order in
    // the bounded output, rather than copying every retained match index.
    const auto before = [&](MatchIndex a, MatchIndex b) {
      const auto& x = store.match(a);
      const auto& y = store.match(b);
      const int rx = rank(x), ry = rank(y);
      if (rx != ry) return rx < ry;
      return rx == 0 && x.score_changed > y.score_changed;
    };
    for (unsigned i = 0; i < list.count; ++i) {
      const auto index = list.index[i];
      if (index == pinned) continue;
      unsigned at = pinned >= 0 ? 1 : 0;
      while (at < page.row_count && !before(index, page.rows[at])) ++at;
      if (at >= per_page) continue;
      for (unsigned j = std::min<unsigned>(page.row_count, per_page - 1);
           j > at; --j)
        page.rows[j] = page.rows[j - 1];
      page.rows[at] = index;
      page.row_count = std::min<unsigned>(page.row_count + 1, per_page);
    }
  } else if (slots > 0 && count > 0) {
    const ConferenceSplit split(count, slots);
    page.pages = static_cast<uint16_t>(split.pages);
    page.page = static_cast<uint16_t>(
        request.manual ? std::min<unsigned>(request.page, page.pages - 1u)
                       : request.page % page.pages);
    const unsigned first = split.first(page.page);
    const unsigned rows = split.size(page.page, count);
    unsigned ordinal = 0;
    for (unsigned i = 0; i < list.count && ordinal < first + rows; ++i) {
      if (list.index[i] == pinned) continue;
      if (ordinal++ >= first) page.rows[page.row_count++] = list.index[i];
    }
  }
  return page;
}

int conferencePageOf(const MatchStore& store, const List& list,
                     const ConferenceRequest& request, MatchIndex store_index) {
  const unsigned per_page =
      std::min<unsigned>(request.per_page, kConferenceRowsMax);
  if (!list.count || !per_page) return -1;
  const int pinned =
      conferencePin(store, list, request.pin_own, request.competition);
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

OverviewMatch ownForOverview(const MatchStore& store, const Shown& shown,
                             int64_t utc) {
  OverviewMatch best;
  bool best_live = false;
  int64_t best_distance = 0;
  for (unsigned view = 0; view < shown.count(); ++view) {
    const auto competition = shown.at(view).competition;
    for (std::size_t i = 0; i < store.matchCount(); ++i) {
      const auto& m = store.match(i);
      if (!store.isOwn(competition, m)) continue;
      const bool live = running(m.phase);
      const int64_t d = m.kickoff ? distance(m, utc) : INT64_MAX;
      if (best.index == kNoMatch || (live && !best_live) ||
          (live == best_live && d < best_distance)) {
        best = {static_cast<MatchIndex>(i), competition};
        best_live = live;
        best_distance = d;
      }
    }
  }
  return best;
}

uint16_t matchCount(const MatchStore& store, const Shown& shown,
                    uint8_t competition) {
  uint16_t count = 0;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (shown.source(competition) && store.includes(competition, m.source))
      ++count;
  }
  return count;
}

const char* teamName(const MatchStore& store, const StoredMatch& match,
                     bool home) {
  const TeamIndex team = home ? match.home : match.away;
  if (team < cfg::kStoreTeams && store.teamAt(team).id != 0 &&
      !store.teamAt(team).name.empty())
    return store.teamAt(team).name.data;
  return "";
}

const char* rowName(const MatchStore& store, const StoredRow& row) {
  if (row.team < cfg::kStoreTeams && store.teamAt(row.team).id != 0)
    return store.teamAt(row.team).name.data;
  return "";
}

TableGroups tableGroups(const MatchStore& store, uint8_t competition) {
  TableGroups out;
  const auto source = store.currentSource(competition);
  for (std::size_t i = 0; i < store.groupCount(); ++i)
    if (store.group(i).source == source)
      out.ordinal[out.count++] = store.group(i).ordinal;
  // Pure/demo callers can still supply a single table without group metadata.
  if (!out.count)
    for (std::size_t i = 0; i < store.rowCount(); ++i)
      if (store.row(i).source == source) {
        out.ordinal[out.count++] = kNoIndex;
        break;
      }
  return out;
}
Table table(const MatchStore& store, uint8_t competition, uint8_t group) {
  Table out;
  const auto source = store.currentSource(competition);
  if (group == kNoIndex) {
    const auto groups = tableGroups(store, competition);
    if (groups.count) group = groups.ordinal[0];
    for (std::size_t i = 0; i < store.rowCount(); ++i)
      if (store.row(i).source == source &&
          store.isOwn(competition, store.row(i))) {
        group = store.row(i).group;
        break;
      }
  }
  out.group = group;
  for (std::size_t i = 0; i < store.groupCount(); ++i) {
    const auto& metadata = store.group(i);
    if (metadata.source == source && metadata.ordinal == group)
      out.label = metadata.label;
  }
  for (std::size_t i = 0; i < store.rowCount() && out.count < out.row.size();
       ++i) {
    const auto& row = store.row(i);
    if (row.source != source || row.group != group) continue;
    if (store.isOwn(competition, row) && out.own < 0) out.own = out.count;
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
             [&](MatchIndex a, MatchIndex b) { return key[a] < key[b]; });
  const auto unsorted = out.index;
  for (uint8_t i = 0; i < out.count; ++i) out.index[i] = unsorted[order[i]];
  return out;
}

Shootout shootout(const MatchStore& store, const StoredMatch& match) {
  Shootout out;
  if (!detailSlot(store, match)) return out;
  for (uint8_t at = 0; at < cfg::kStoreEvents; ++at) {
    const auto& event = store.eventAt(at);
    if (event.slot != match.detail || !(event.flags & kEventShootout) ||
        (event.flags & kEventRemoved))
      continue;
    uint8_t position = out.count;
    while (position &&
           store.eventAt(out.index[position - 1]).ordinal > event.ordinal) {
      out.index[position] = out.index[position - 1];
      --position;
    }
    out.index[position] = at;
    ++out.count;
  }
  return out;
}

}  // namespace football::view
