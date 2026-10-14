#include "store_views.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

#include "football_files.h"
#include "request_scheduler.h"

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

// Start of the first block of shown matches after 'kickoff'; INT64_MAX if
// there is none.
int64_t nextBlockStart(const MatchStore& store, const Shown& shown,
                       int64_t kickoff, uint16_t before) {
  int64_t next = INT64_MAX;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (shown.shows(m) && scheduled(m) && m.kickoff > kickoff)
      next = std::min<int64_t>(next, int64_t{m.kickoff} - before * 60);
  }
  return next;
}

int64_t blockEnd(const MatchStore& store, const Shown& shown, uint32_t kickoff,
                 uint16_t after) {
  int64_t end = 0;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (shown.shows(m) && scheduled(m) && m.kickoff == kickoff)
      end = std::max(end, matchEnd(m, after));
  }
  return end;
}

bool member(const MatchStore& store, const Shown& shown, const StoredMatch& m,
            int64_t utc, const ConferenceRules& rules) {
  if (!scheduled(m) || utc < int64_t{m.kickoff} - rules.before * 60)
    return false;
  const int64_t next = nextBlockStart(store, shown, m.kickoff, rules.before);
  if (utc < std::min(blockEnd(store, shown, m.kickoff, rules.after), next))
    return true;
  // An overrun: still running when the next block started.
  const bool overran =
      m.finished_at ? int64_t{m.finished_at} > next : running(m.phase);
  return utc >= next && overran && utc < matchEnd(m, rules.after);
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

bool ownSlot(const MatchStore& store, const StoredMatch& m) {
  return m.isOwn() && m.own < cfg::kStoreOwnMatches &&
         store.ownAt(m.own).match == m.id &&
         store.ownAt(m.own).competition == m.competition;
}

}  // namespace

void Shown::add(const settings::Competition& entry, const MatchStore& store) {
  if (!entry.active || entry.id == 0) return;
  Source source{entry.id, entry.primary.provider,
                decimalId(entry.primary.team.view()), entry.screens};
  if (entry.fallback_active &&
      (store.notices(entry.id) & kNoticeFallbackRoute) != 0) {
    for (std::size_t i = 0; i < store.matchCount(); ++i) {
      const auto& m = store.match(i);
      if (m.competition == entry.id && m.provider == entry.fallback.provider) {
        source.provider = entry.fallback.provider;
        source.club = decimalId(entry.fallback.team.view());
        break;
      }
    }
  }
  add(source);
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

Shown Shown::only(cfg::CompetitionScreen screen) const {
  Shown out;
  for (uint8_t i = 0; i < count_; ++i)
    if (sources_[i].uses(screen)) out.sources_[out.count_++] = sources_[i];
  return out;
}

bool Shown::shows(const StoredMatch& match) const {
  const auto* s = source(match.competition);
  return s != nullptr && s->provider == match.provider;
}

bool scheduled(const StoredMatch& m) {
  return m.kickoff != 0 && m.phase != Phase::kPostponed &&
         m.phase != Phase::kCancelled && m.phase != Phase::kAbandoned;
}

bool started(Phase phase) {
  return running(phase) || phase == Phase::kFinished ||
         phase == Phase::kAbandoned;
}

int64_t matchEnd(const StoredMatch& m, uint16_t after) {
  uint32_t start = 0, end = 0;
  if (!schedule::liveWindow(m, start, end)) return 0;
  const int64_t latest =
      int64_t{m.kickoff} + int64_t{cfg::kUnknownMatchEndMinutes} * 60;
  return std::min<int64_t>(end, latest) + after * 60;
}

bool matchWindow(const StoredMatch& m, int64_t utc, uint16_t before,
                 uint16_t after) {
  if (!scheduled(m)) return false;
  return utc >= int64_t{m.kickoff} - before * 60 && utc < matchEnd(m, after);
}

bool sameRound(const StoredMatch& a, const StoredMatch& b) {
  if (a.competition != b.competition || a.provider != b.provider) return false;
  if (a.round != 0 && b.round != 0) return a.round == b.round;
  if (a.label != kNoIndex && b.label != kNoIndex) return a.label == b.label;
  return a.kickoff != 0 && b.kickoff != 0 &&
         a.kickoff / 86400 == b.kickoff / 86400;
}

List conferenceMembers(const MatchStore& store, const Shown& shown,
                       int64_t utc, const ConferenceRules& rules) {
  List list;
  std::array<bool, cfg::kStoreMatches> in{};
  const std::size_t n = store.matchCount();
  for (std::size_t i = 0; i < n; ++i)
    in[i] = shown.shows(store.match(i)) &&
            member(store, shown, store.match(i), utc, rules);
  if (rules.whole_matchday) {
    for (std::size_t i = 0; i < n; ++i) {
      const auto& own = store.match(i);
      if (in[i] || !own.isOwn() || !shown.shows(own) || !scheduled(own) ||
          utc < int64_t{own.kickoff})
        continue;
      for (std::size_t j = 0; j < n && !in[i]; ++j)
        in[i] = j != i && in[j] && !store.match(j).isOwn() &&
                sameRound(own, store.match(j));
    }
  }
  // The store is sorted by kickoff (unknown kickoffs last).
  for (std::size_t i = 0; i < n; ++i)
    if (in[i]) list.index[list.count++] = static_cast<uint8_t>(i);
  return list;
}

List conferenceAll(const MatchStore& store, const Shown& shown) {
  List list;
  for (std::size_t i = 0; i < store.matchCount(); ++i)
    if (shown.shows(store.match(i)))
      list.index[list.count++] = static_cast<uint8_t>(i);
  return list;
}

List matchday(const MatchStore& store, const Shown& shown,
              uint8_t competition) {
  List list;
  uint32_t last = 0;
  // The store is sorted by kickoff (unknown kickoffs last).
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (m.competition != competition || m.kickoff == 0 || !shown.shows(m))
      continue;
    if (list.count &&
        m.kickoff - last >= cfg::kMatchdayGapHours * 3600u)
      break;
    last = m.kickoff;
    list.index[list.count++] = static_cast<uint8_t>(i);
  }
  return list;
}

ConferencePage conferencePage(const MatchStore& store, const List& list,
                              const ConferenceRequest& request) {
  ConferencePage page;
  const unsigned per_page =
      std::min<unsigned>(request.per_page, kConferenceRowsMax);
  if (list.count == 0 || per_page == 0) return page;
  int pinned = -1;
  if (request.pin_own)
    for (unsigned i = 0; i < list.count && pinned < 0; ++i)
      if (store.match(list.index[i]).isOwn()) pinned = list.index[i];
  std::array<uint8_t, cfg::kStoreMatches> others{};
  unsigned count = 0;
  for (unsigned i = 0; i < list.count; ++i)
    if (list.index[i] != pinned) others[count++] = list.index[i];
  const unsigned slots = per_page - (pinned >= 0 ? 1 : 0);
  unsigned first = 0;
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
    const unsigned pages = (count + slots - 1) / slots;
    page.pages = static_cast<uint8_t>(std::min(pages, 255u));
    page.page = static_cast<uint8_t>(
        request.manual ? std::min<unsigned>(request.page, page.pages - 1u)
                       : request.page % page.pages);
    first = page.page * slots;
  }
  if (pinned >= 0) page.rows[page.row_count++] = static_cast<uint8_t>(pinned);
  for (unsigned i = first; i < count && page.row_count < per_page; ++i)
    page.rows[page.row_count++] = others[i];
  return page;
}

int ownForScreen(const MatchStore& store, const Shown& shown, int64_t utc,
                 uint16_t before, uint16_t after, bool demo, bool& in_window) {
  int best = -1;
  in_window = false;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (!m.isOwn() || !shown.shows(m)) continue;
    const bool active = demo || matchWindow(m, utc, before, after);
    const auto* chosen = best < 0 ? nullptr : &store.match(best);
    if (chosen == nullptr || (active && !in_window) ||
        (!in_window && m.kickoff != 0 && chosen->kickoff != 0 &&
         distance(m, utc) < distance(*chosen, utc))) {
      best = static_cast<int>(i);
      in_window |= active;
    }
  }
  return best;
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

int ownAt(const MatchStore& store, uint8_t competition, cfg::Provider provider,
          uint16_t ordinal) {
  uint16_t seen = 0;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (m.competition != competition || !m.isOwn() || m.provider != provider)
      continue;
    if (seen++ == ordinal) return static_cast<int>(i);
  }
  return -1;
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
  if (ownSlot(store, match)) {
    const auto& own = store.ownAt(match.own);
    return home ? own.home_name.data : own.away_name.data;
  }
  return "";
}

const char* fullName(const MatchStore& store, const StoredMatch& match,
                     bool home, const persist::ShortNames* names) {
  if (ownSlot(store, match)) {
    const auto& own = store.ownAt(match.own);
    const auto& name = home ? own.home_name : own.away_name;
    if (!name.empty()) return name.data;
  }
  return teamName(store, match, home, names);
}

const char* rowName(const MatchStore& store, const StoredRow& row,
                    const persist::ShortNames* names) {
  if (const char* name = replaced(store, row.team, names)) return name;
  if (row.team < cfg::kStoreTeams) return store.teamAt(row.team).short_name.data;
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
  if (!ownSlot(store, match)) return out;
  const auto& own = store.ownAt(match.own);
  // Sort key per kept event: shootout, minute (inherited when unknown),
  // added minute.
  std::array<uint32_t, cfg::kMatchEvents> key{};
  uint8_t minute = 0;
  for (uint8_t i = 0; i < own.event_count && i < own.events.size(); ++i) {
    const uint8_t at = own.events[i];
    const auto& e = store.eventAt(at);
    if (e.minute != kNoValue) minute = e.minute;
    if ((e.flags & kEventRemoved) || !store.eventShown(match.own, e)) continue;
    const uint8_t added = e.minute != kNoValue && e.added != kNoValue
                              ? e.added
                              : 0;
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

void demo(MatchStore& store, uint8_t competition) {
  store.clear();
  // Clock 0: no kickoff and no recent score change (no goal flash).
  const Clock clock{};
  StoredMatch m;
  m.id = 1;
  m.competition = competition;
  m.home = store.team(competition, cfg::Provider::kOpenLigaDb, 10, "Bayern");
  m.away = store.team(competition, cfg::Provider::kOpenLigaDb, 11, "Essen");
  m.phase = Phase::kLive;
  m.minute = 67;
  m.principal = {2, 1};
  m.flags = kMatchOwn;
  store.upsert(m, clock, "FC Bayern München", "Rot-Weiss Essen");
  static constexpr const char* kClubs[] = {"Demo United", "Demo City",
                                           "FC Bayern München",
                                           "Demo Athletic", "Rot-Weiss Essen"};
  store.beginTable(competition);
  for (uint8_t i = 0; i < 5; ++i) {
    StoredRow r;
    r.team = store.team(competition, cfg::Provider::kOpenLigaDb, i + 1u,
                        kClubs[i]);
    r.position = i + 1;
    r.points = static_cast<int16_t>(30 - i * 3);
    r.flags = i == 2 ? kRowOwn : 0;
    store.appendRow(r);
  }
  store.commitTable(true);
}

}  // namespace football::view
