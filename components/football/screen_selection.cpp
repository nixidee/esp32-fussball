#include "screen_selection.h"

#include <algorithm>

namespace football::selection {
namespace {

bool belongs(const Input& input, const settings::CompetitionRoute& c,
             const StoredMatch& m) {
  return c.active && c.id != 0 && input.store.includes(c.id, m.source);
}

bool eligibleDate(const StoredMatch& m) {
  return m.kickoff != 0 && m.phase != Phase::kPostponed &&
         m.phase != Phase::kCancelled && m.phase != Phase::kAbandoned;
}

bool verifiedRound(const MatchStore& store, const StoredMatch& m) {
  if (m.round != 0) return true;
  return (m.flags & kMatchRoundLabel) && m.label < cfg::kStoreRoundLabels &&
         !store.labelAt(m.label).empty();
}

bool sameVerifiedRound(const MatchStore& store, const StoredMatch& a,
                       const StoredMatch& b) {
  if (a.source != b.source || !verifiedRound(store, a) ||
      !verifiedRound(store, b))
    return false;
  if (a.round != 0 || b.round != 0)
    return a.round != 0 && b.round != 0 && a.round == b.round;
  return store.labelAt(a.label) == store.labelAt(b.label);
}

Window basicWindow(const Input& input, const settings::CompetitionRoute& c,
                   const StoredMatch& m) {
  Window out;
  const auto timing = matchTiming(m);
  if (!belongs(input, c, m) || input.clock.utc <= 0 || !timing.valid)
    return out;
  out.own_special = input.store.isOwn(c.id, m) && ownSpecial(input, c);
  const auto before = out.own_special ? input.rules.own_before_min
                                      : input.rules.other_before_min;
  const auto after =
      out.own_special ? input.rules.own_after_min : input.rules.other_after_min;
  out.begin =
      m.kickoff != 0 ? timing.basis - int64_t{before} * 60 : timing.basis;
  out.end = timing.end + (timing.finished ? int64_t{after} * 60 : 0);
  out.reported_running = timing.reported_running &&
                         input.clock.utc >= timing.basis &&
                         input.clock.utc < timing.end;
  out.active = input.clock.utc >= out.begin && input.clock.utc < out.end;
  return out;
}

const settings::CompetitionRoute* ownCompetition(const Input& input,
                                                 const StoredMatch& m) {
  const auto order =
      settings::screen_policy::competitionOrder(input.competitions);
  for (unsigned slot = 0; slot < order.count; ++slot) {
    const auto& c = input.competitions.entries[order.slots[slot]];
    if (belongs(input, c, m) && input.store.isOwn(c.id, m) &&
        ownSpecial(input, c))
      return &c;
  }
  return nullptr;
}

// Enumerates date clusters without a retained group table. Verified rounds
// may be noncontiguous in kickoff order; fallback groups have no round claim.
template <class Visit>
void clusters(const Input& input, const settings::CompetitionRoute& c,
              Visit visit) {
  const auto& store = input.store;
  const auto flush = [&](Round& cluster) {
    if (cluster.valid()) visit(cluster);
    cluster = {};
  };
  for (std::size_t seed = 0; seed < store.matchCount(); ++seed) {
    const auto& anchor = store.match(seed);
    if (!belongs(input, c, anchor) || !eligibleDate(anchor) ||
        !verifiedRound(store, anchor))
      continue;
    bool seen = false;
    for (std::size_t i = 0; i < seed && !seen; ++i) {
      const auto& prior = store.match(i);
      seen = belongs(input, c, prior) && eligibleDate(prior) &&
             sameVerifiedRound(store, anchor, prior);
    }
    if (seen) continue;
    Round cluster;
    for (std::size_t i = seed; i < store.matchCount(); ++i) {
      const auto& m = store.match(i);
      if (!belongs(input, c, m) || !eligibleDate(m) ||
          !sameVerifiedRound(store, anchor, m))
        continue;
      const auto day = civilDay(m.kickoff, input.dates);
      if (cluster.valid() &&
          (day < cluster.last_day || day - cluster.last_day > 2))
        flush(cluster);
      if (!cluster.valid()) cluster = {input.store.key(m), day, day, true};
      cluster.last_day = day;
    }
    flush(cluster);
  }
  Round fallback;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (!belongs(input, c, m) || !eligibleDate(m) || verifiedRound(store, m))
      continue;
    const auto day = civilDay(m.kickoff, input.dates);
    if (fallback.valid() &&
        (day < fallback.last_day || day - fallback.last_day > 2 ||
         day - fallback.first_day > 3))
      flush(fallback);
    if (!fallback.valid()) fallback = {input.store.key(m), day, day, false};
    fallback.last_day = day;
  }
  flush(fallback);
}

Single activeSingle(const Input& input, const StoredMatch& m) {
  Content content = Content::kBefore;
  if (m.phase == Phase::kFinished)
    content = Content::kAfter;
  else if (running(m.phase) || m.phase == Phase::kSuspended)
    content = Content::kLive;
  return {input.store.key(m), content};
}

Single staticSingle(const Input& input, const settings::CompetitionRoute& c) {
  if (!c.hasClub()) return {};
  const StoredMatch* result = nullptr;
  const StoredMatch* preview = nullptr;
  for (std::size_t i = 0; i < input.store.matchCount(); ++i) {
    const auto& m = input.store.match(i);
    if (!belongs(input, c, m) || !input.store.isOwn(c.id, m)) continue;
    if (resultHeld(m, input.clock.utc, input.dates) &&
        (!result || m.finished_at > result->finished_at))
      result = &m;
    if ((m.phase == Phase::kScheduled || m.phase == Phase::kUnknown) &&
        m.kickoff != 0 && m.kickoff > input.clock.utc &&
        (!preview || m.kickoff < preview->kickoff))
      preview = &m;
  }
  if (result) return {input.store.key(*result), Content::kResult};
  if (preview) return {input.store.key(*preview), Content::kPreview};
  return {};
}

}  // namespace

const StoredMatch* find(const Input& input, const MatchKey& match) {
  return match.valid() ? input.store.find(match) : nullptr;
}

bool ownSpecial(const Input& input, const settings::CompetitionRoute& c) {
  if (!c.active || !c.hasClub()) return false;
  for (std::size_t slot = 0; slot < input.competitions.entries.size(); ++slot) {
    const auto& entry = input.competitions.entries[slot];
    if (entry.id == c.id && entry.incarnation == c.incarnation)
      return (input.rules.own_special & (1u << slot)) != 0;
  }
  return false;
}

Window window(const Input& input, const settings::CompetitionRoute& c,
              const StoredMatch& m) {
  auto out = basicWindow(input, c, m);
  if (!out.own_special || out.end == 0) return out;
  // A midpoint is valid only between an observed/estimated final whistle
  // of a finished own match and the next own kickoff. Clamp to the overlap;
  // the configured before/after windows can only become shorter.
  const StoredMatch* previous = nullptr;
  const StoredMatch* next = nullptr;
  for (std::size_t i = 0; i < input.store.matchCount(); ++i) {
    const auto& other = input.store.match(i);
    if (input.store.key(other) == input.store.key(m) ||
        !ownCompetition(input, other))
      continue;
    if (m.kickoff != 0 && other.phase == Phase::kFinished &&
        other.finished_at != 0 && other.finished_at <= m.kickoff &&
        (!previous || other.finished_at > previous->finished_at))
      previous = &other;
    if (m.phase == Phase::kFinished && m.finished_at != 0 &&
        other.kickoff > m.finished_at && eligibleDate(other) &&
        (!next || other.kickoff < next->kickoff))
      next = &other;
  }
  if (previous) {
    const auto old =
        basicWindow(input, *ownCompetition(input, *previous), *previous);
    if (out.begin < old.end) {
      const int64_t midpoint = int64_t{previous->finished_at} +
                               (int64_t{m.kickoff} - previous->finished_at) / 2;
      out.begin = std::max(out.begin, std::clamp(midpoint, out.begin, old.end));
    }
  }
  if (next) {
    const auto upcoming =
        basicWindow(input, *ownCompetition(input, *next), *next);
    if (upcoming.begin < out.end) {
      const int64_t midpoint =
          int64_t{m.finished_at} + (int64_t{next->kickoff} - m.finished_at) / 2;
      out.end =
          std::min(out.end, std::clamp(midpoint, upcoming.begin, out.end));
    }
  }
  out.active = input.clock.utc >= out.begin && input.clock.utc < out.end;
  return out;
}

Selection select(const Input& input) {
  Selection out;
  if (input.clock.utc <= 0) return out;
  MatchKey buffered;
  uint8_t buffered_competition = 0;
  bool other_active = false;
  const auto priority =
      settings::screen_policy::competitionOrder(input.competitions);
  for (unsigned slot = 0; slot < priority.count; ++slot) {
    const auto& c = input.competitions.entries[priority.slots[slot]];
    if (!c.active || c.id == 0) continue;
    for (std::size_t i = 0; i < input.store.matchCount(); ++i) {
      const auto& m = input.store.match(i);
      if (!belongs(input, c, m)) continue;
      const auto period = window(input, c, m);
      if (!period.active) continue;
      if (!period.own_special) {
        other_active = true;
      } else if (period.reported_running) {
        return {ScreenPhase::kOwn, c.id, input.store.key(m)};
      } else if (!buffered.valid()) {
        buffered = input.store.key(m);
        buffered_competition = c.id;
      }
    }
  }
  if (buffered.valid())
    return {ScreenPhase::kOwn, buffered_competition, buffered};
  if (other_active) return {ScreenPhase::kOtherGames, 0, {}};
  for (const auto& c : input.competitions.entries)
    if (c.active && phasePeriod(input, c).valid())
      return {ScreenPhase::kMatchday, 0, {}};
  out.phase = input.rules.automatic_break && input.break_proven
                  ? ScreenPhase::kBreak
                  : ScreenPhase::kIdle;
  return out;
}

Single single(const Input& input, const Selection& selected,
              const settings::CompetitionRoute& c) {
  if (!c.active || c.id == 0 || input.clock.utc <= 0) return {};
  if (selected.phase == ScreenPhase::kOwn) {
    if (input.rules.only_one_own_single) {
      const auto* focused = find(input, selected.focus);
      return selected.competition == c.id && focused &&
                     belongs(input, c, *focused) &&
                     window(input, c, *focused).active
                 ? activeSingle(input, *focused)
                 : Single{};
    }
    const StoredMatch* buffered = nullptr;
    for (std::size_t i = 0; i < input.store.matchCount(); ++i) {
      const auto& match = input.store.match(i);
      if (!belongs(input, c, match)) continue;
      const auto period = window(input, c, match);
      if (!period.active || !period.own_special) continue;
      if (period.reported_running) return activeSingle(input, match);
      if (!buffered) buffered = &match;
    }
    return buffered ? activeSingle(input, *buffered) : Single{};
  }
  if (selected.phase == ScreenPhase::kOtherGames) {
    const StoredMatch* live = nullptr;
    const StoredMatch* candidate = nullptr;
    unsigned live_count = 0, candidate_count = 0;
    for (std::size_t i = 0; i < input.store.matchCount(); ++i) {
      const auto& m = input.store.match(i);
      if (!belongs(input, c, m)) continue;
      const auto period = window(input, c, m);
      if (!period.active) continue;
      candidate = &m;
      ++candidate_count;
      if (period.reported_running) {
        live = &m;
        ++live_count;
      }
    }
    if (live_count == 1) return activeSingle(input, *live);
    if (live_count == 0 && candidate_count == 1)
      return activeSingle(input, *candidate);
  }
  return staticSingle(input, c);
}

Round phasePeriod(const Input& input, const settings::CompetitionRoute& c) {
  Round out;
  if (!c.active || input.clock.utc <= 0) return out;
  const auto today = civilDay(input.clock.utc, input.dates);
  clusters(input, c, [&](const Round& candidate) {
    if (today >= candidate.first_day && today <= candidate.last_day &&
        (!out.valid() || candidate.first_day < out.first_day))
      out = candidate;
  });
  return out;
}

Round round(const Input& input, const settings::CompetitionRoute& c,
            cfg::MatchdayMode mode) {
  Round active, held, upcoming;
  if (!c.active || input.clock.utc <= 0) return {};
  const auto today = civilDay(input.clock.utc, input.dates);
  clusters(input, c, [&](const Round& candidate) {
    if (candidate.first_day <= today && candidate.last_day >= today) {
      if (!active.valid() || candidate.first_day < active.first_day)
        active = candidate;
    } else if (candidate.last_day < today && candidate.last_day + 2 > today) {
      if (!held.valid() || candidate.last_day > held.last_day) held = candidate;
    } else if (candidate.first_day > today &&
               (!upcoming.valid() ||
                candidate.first_day < upcoming.first_day)) {
      upcoming = candidate;
    }
  });
  if (active.valid()) return active;
  if (mode == cfg::MatchdayMode::kCurrent && held.valid()) return held;
  return upcoming;
}

bool roundContains(const Input& input, const Round& selected,
                   const StoredMatch& m) {
  const auto* anchor = find(input, selected.anchor);
  if (!anchor || m.source != anchor->source) return false;
  if (selected.verified) return sameVerifiedRound(input.store, *anchor, m);
  if (verifiedRound(input.store, m) || m.kickoff == 0) return false;
  const auto day = civilDay(m.kickoff, input.dates);
  return day >= selected.first_day && day <= selected.last_day;
}

RoundReach rounds(const Input& input, const settings::CompetitionRoute& c,
                  cfg::MatchdayMode mode) {
  RoundReach out;
  if (!c.active || input.clock.utc <= 0) return out;
  const auto chosen = round(input, c, mode);
  const auto today = civilDay(input.clock.utc, input.dates);
  Round previous, next;
  const auto same = [&](const Round& a, const Round& b) {
    const auto* match = find(input, b.anchor);
    return a.valid() && match && roundContains(input, a, *match);
  };
  const auto before = [&](const Round& a, const Round& b) {
    if (a.first_day != b.first_day) return a.first_day < b.first_day;
    const auto* first = find(input, a.anchor);
    const auto* second = find(input, b.anchor);
    return first && second && first->kickoff < second->kickoff;
  };
  clusters(input, c, [&](const Round& candidate) {
    if (same(chosen, candidate)) return;
    if (candidate.last_day < today && candidate.last_day + 2 > today &&
        (!chosen.valid() || candidate.first_day < chosen.first_day) &&
        (!previous.valid() || candidate.last_day > previous.last_day))
      previous = candidate;
    if (chosen.valid() && before(chosen, candidate) &&
        (!next.valid() || before(candidate, next)))
      next = candidate;
  });
  if (previous.valid()) out.round[out.count++] = previous;
  if (chosen.valid()) {
    out.selected = static_cast<int8_t>(out.count);
    out.round[out.count++] = chosen;
  }
  if (next.valid() && !same(previous, next)) out.round[out.count++] = next;
  return out;
}

}  // namespace football::selection
