#include "conference.h"

#include <algorithm>
#include <climits>

namespace football {
namespace {
// Stable insertion sort: at most cfg::kMatches entries, no heap buffer
// (std::stable_sort may allocate one).
template <class Less>
void stableSort(uint8_t* first, unsigned count, Less less) {
  for (unsigned i = 1; i < count; ++i) {
    const uint8_t value = first[i];
    unsigned j = i;
    for (; j > 0 && less(value, first[j - 1]); --j) first[j] = first[j - 1];
    first[j] = value;
  }
}
void byKickoff(const Snapshot& data, ConferenceList& list) {
  stableSort(list.index.data(), list.count, [&](uint8_t a, uint8_t b) {
    return data.matches[a].kickoff.value < data.matches[b].kickoff.value;
  });
}
// Start of the first block after `kickoff`; INT64_MAX if there is none.
int64_t nextBlockStart(const Snapshot& data, int64_t kickoff,
                       uint16_t before) {
  int64_t next = INT64_MAX;
  for (unsigned i = 0; i < data.match_count; ++i) {
    const auto& m = data.matches[i];
    if (scheduled(m) && m.kickoff.value > kickoff)
      next = std::min(next, m.kickoff.value - before * 60);
  }
  return next;
}
int64_t blockEnd(const Snapshot& data, int64_t kickoff, uint16_t after) {
  int64_t end = 0;
  for (unsigned i = 0; i < data.match_count; ++i) {
    const auto& m = data.matches[i];
    if (scheduled(m) && m.kickoff.value == kickoff)
      end = std::max(end, matchEnd(m, after));
  }
  return end;
}
bool member(const Snapshot& data, const Match& m, int64_t utc,
            const ConferenceRules& rules) {
  if (!scheduled(m) || utc < m.kickoff.value - rules.before * 60) return false;
  const int64_t next = nextBlockStart(data, m.kickoff.value, rules.before);
  if (utc < std::min(blockEnd(data, m.kickoff.value, rules.after), next))
    return true;
  // An overrun: still running when the next block started.
  const bool overran = m.finished_at.valid() ? m.finished_at.value > next
                                             : running(m);
  return utc >= next && overran && utc < matchEnd(m, rules.after);
}
bool goals(const Match& m) {
  return m.principal.valid() &&
         m.principal.home.value + m.principal.away.value > 0;
}
// Activity rank of "One page"; lower first. A score change unseen since the
// last restart (score_changed_ms 0) sorts last among running matches with
// goals.
int rank(const Match& m) {
  if (running(m)) return goals(m) ? 0 : 1;
  return m.phase == Phase::kFinished ? 2 : 3;
}
}  // namespace

ConferenceList conferenceMembers(const Snapshot& data, int64_t utc,
                                 const ConferenceRules& rules) {
  ConferenceList list;
  std::array<bool, cfg::kMatches> shown{};
  for (unsigned i = 0; i < data.match_count; ++i)
    shown[i] = member(data, data.matches[i], utc, rules);
  if (rules.whole_matchday) {
    for (unsigned i = 0; i < data.match_count; ++i) {
      const auto& own = data.matches[i];
      if (shown[i] || !own.own || !scheduled(own) || utc < own.kickoff.value)
        continue;
      for (unsigned j = 0; j < data.match_count && !shown[i]; ++j)
        shown[i] = j != i && shown[j] && !data.matches[j].own &&
                   sameRound(own, data.matches[j]);
    }
  }
  for (unsigned i = 0; i < data.match_count; ++i)
    if (shown[i]) list.index[list.count++] = static_cast<uint8_t>(i);
  byKickoff(data, list);
  return list;
}

ConferenceList conferenceAll(const Snapshot& data) {
  ConferenceList list;
  for (unsigned i = 0; i < data.match_count; ++i)
    list.index[list.count++] = static_cast<uint8_t>(i);
  byKickoff(data, list);
  return list;
}

ConferencePage conferencePage(const Snapshot& data, const ConferenceList& list,
                              const ConferenceRequest& request) {
  ConferencePage page;
  const unsigned per_page =
      std::min<unsigned>(request.per_page, kConferenceRowsMax);
  if (list.count == 0 || per_page == 0) return page;
  int pinned = -1;
  if (request.pin_own)
    for (unsigned i = 0; i < list.count && pinned < 0; ++i)
      if (data.matches[list.index[i]].own) pinned = list.index[i];
  std::array<uint8_t, cfg::kMatches> others{};
  unsigned count = 0;
  for (unsigned i = 0; i < list.count; ++i)
    if (list.index[i] != pinned) others[count++] = list.index[i];
  const unsigned slots = per_page - (pinned >= 0 ? 1 : 0);
  unsigned first = 0;
  page.pages = 1;
  if (request.mode == cfg::ConferenceMode::kOnePage && !request.manual) {
    stableSort(others.data(), count, [&](uint8_t a, uint8_t b) {
      const auto& x = data.matches[a];
      const auto& y = data.matches[b];
      const int rx = rank(x), ry = rank(y);
      if (rx != ry) return rx < ry;
      return rx == 0 && x.score_changed_ms > y.score_changed_ms;
    });
  } else if (slots > 0 && count > 0) {
    page.pages = static_cast<uint8_t>((count + slots - 1) / slots);
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
}  // namespace football
