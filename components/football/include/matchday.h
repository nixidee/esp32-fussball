// Matchdays (pure logic): the one place of the gap rule (OQ-201, OQ-224),
// the hold rule (OQ-202, OQ-268), "over" (OQ-271) and the current/next
// selection of the Matchday screen (OQ-244, OQ-267, OQ-269, OQ-275). Store
// expiry, the store views, the request planner and the UI use it. No I/O.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "match_store.h"

namespace football::matchday {

// True when 'next' starts a new matchday after 'previous' (ascending UTC
// seconds of kickoffs or provider day starts): at least kMatchdayGapHours
// apart.
bool starts(uint32_t previous, uint32_t next);

// Over (OQ-271): the local date of 'last_kickoff' has ended at 'utc'.
bool over(uint32_t last_kickoff, int64_t utc, int32_t local_offset_s);

// Passed (OQ-202, OQ-268): the local day after that date has ended too. The
// store drops a passed matchday and no view shows it.
bool passed(uint32_t last_kickoff, int64_t utc, int32_t local_offset_s);
// Previous/current/next derived date clusters when the provider gives no
// distinct round sections. Overflow is explicit; no partial window is complete.
std::size_t retainedDays(const uint32_t* days, std::size_t count, int64_t utc,
                         uint32_t* out, std::size_t capacity, bool* complete);

// One matchday of a competition: its matches lie in the store index range
// [begin, end) (kickoff order; matches of other competitions or filtered
// ones may lie in between).
struct Span {
  MatchIndex begin = 0, end = 0;
  uint32_t first = 0, last = 0;  // first and last kickoff (UTC seconds)
};

// The next matchday of 'competition' at or after store index 'from' among
// its matches with a known kickoff that 'use(match)' accepts; false if none.
template <class Use>
bool next(const MatchStore& store, uint8_t competition, Use use,
          std::size_t from, Span& out) {
  bool open = false;
  for (std::size_t i = from; i < store.matchCount(); ++i) {
    const auto& m = store.match(i);
    if (m.competition != competition || m.kickoff == 0 || !use(m)) continue;
    if (open && starts(out.last, m.kickoff)) return true;
    if (!open) {
      out = {static_cast<MatchIndex>(i), 0, m.kickoff, 0};
      open = true;
    }
    out.last = m.kickoff;
    out.end = static_cast<MatchIndex>(i + 1);
  }
  return open;
}

// The matchdays of one competition the Matchday screen can show: from the
// first one not passed up to the one after the selected one (held,
// selected, next; OQ-269). 'selected' is the one the automatic cycle shows:
// "current" the first not passed, "next" the first not over; -1 when
// "next" finds none (OQ-275).
struct Reach {
  std::array<Span, 3> span{};
  uint8_t count = 0;
  int8_t selected = -1;
};

template <class Use>
Reach reach(const MatchStore& store, uint8_t competition, Use use,
            cfg::MatchdayMode mode, int64_t utc, int32_t local_offset_s) {
  Reach out;
  Span span;
  for (std::size_t from = 0;
       out.count < out.span.size() && next(store, competition, use, from, span);
       from = span.end) {
    if (passed(span.last, utc, local_offset_s)) continue;
    if (out.selected >= 0 && out.count > out.selected + 1) break;
    if (out.selected < 0 && (mode == cfg::MatchdayMode::kCurrent ||
                             !over(span.last, utc, local_offset_s)))
      out.selected = static_cast<int8_t>(out.count);
    out.span[out.count++] = span;
  }
  return out;
}

}  // namespace football::matchday
