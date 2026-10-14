// Read side of the compact store (docs/DATA_FLOW.md, "Views from the
// store"): which rows the screens and the Web UI show, the Konferenz, the
// own match, a league table and the own match's events. The views keep no
// data of their own; they return store indices into small fixed lists on
// the caller's stack. Pure: no clock, settings store, hardware or LVGL, so
// the native tests compile it.

#pragma once

#include <array>
#include <cstdint>

#include "app_config.h"
#include "conference.h"
#include "match_store.h"
#include "settings_model.h"

namespace football::persist {
class ShortNames;  // football_files.h
}

namespace football::view {

// The provider whose matches a competition shows, the own club's id there
// (0 = no own club) and the screens that show the competition (bits of
// cfg::CompetitionScreen, OQ-192).
struct Source {
  uint8_t competition = 0;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  uint32_t club = 0;
  uint8_t screens = cfg::kCompetitionScreensDefault;
  bool uses(cfg::CompetitionScreen screen) const {
    return (screens >> static_cast<unsigned>(screen)) & 1;
  }
};

// The shown source of every active competition. A competition shows its
// primary provider's matches; while kNoticeFallbackRoute is set and the
// store holds matches of the fallback provider in it, the fallback's
// instead (the primary's stored rows stay hidden until the route ends).
// Matches of other providers and of competitions not added stay hidden.
class Shown {
 public:
  // Adds an active entry (inactive ones are skipped).
  void add(const settings::Competition& entry, const MatchStore& store);
  // Adds a source directly (demo).
  void add(const Source& source);
  const Source* source(uint8_t competition) const;
  bool shows(const StoredMatch& match) const;
  std::size_t count() const { return count_; }
  const Source& at(std::size_t i) const { return sources_[i]; }
  // The sources whose competition uses 'screen' (a copy of about 100 B).
  Shown only(cfg::CompetitionScreen screen) const;

 private:
  std::array<Source, cfg::kCompetitionMax> sources_{};
  uint8_t count_ = 0;
};

// Store indices in kickoff order.
struct List {
  std::array<uint8_t, cfg::kStoreMatches> index{};
  uint16_t count = 0;
};

// Kickoff known and not postponed, cancelled or abandoned.
bool scheduled(const StoredMatch& match);
// Kicked off: running, finished or abandoned.
bool started(Phase phase);
// End of a match's window in UTC seconds: the end of its live window
// (schedule::liveWindow(), at most kickoff + kUnknownMatchEndMinutes) plus
// 'after' minutes; 0 without a live window.
int64_t matchEnd(const StoredMatch& match, uint16_t after);
// From kickoff - 'before' minutes to matchEnd(match, after).
bool matchWindow(const StoredMatch& match, int64_t utc, uint16_t before,
                 uint16_t after);
// Same competition, provider and round (round number, else round label,
// else kickoff day in UTC).
bool sameRound(const StoredMatch& a, const StoredMatch& b);

// Konferenz members at 'utc' among the shown matches, in kickoff order
// (equal kickoff keeps store order). A block is all matches with the same
// kickoff, across competitions, from kickoff - before. Every match of a
// block stays until the block ends (latest matchEnd of the block) or the
// next block starts; a match still running at the next block's start stays
// until its own matchEnd. With whole_matchday an own match that has kicked
// off is added while a match of its round is shown.
List conferenceMembers(const MatchStore& store, const Shown& shown,
                       int64_t utc, const ConferenceRules& rules);
// Every shown match (demo mode).
List conferenceAll(const MatchStore& store, const Shown& shown);
// The first matchday of 'competition' among its shown matches with a
// kickoff (the MatchStore::matchday() gap rule): the current matchday
// until expire() drops it (OQ-202), then the next one. Kickoff order.
List matchday(const MatchStore& store, const Shown& shown,
              uint8_t competition);
// Rows of one Konferenz page (store indices). per_page includes a pinned
// own match, which stands in row 1 of every page. "One page" ranks the
// others by activity: running with the latest score change first, goalless
// running by kickoff, then finished, then not started; all other cases use
// kickoff order.
ConferencePage conferencePage(const MatchStore& store, const List& list,
                              const ConferenceRequest& request);

// The own match of the own-match screen: the first own match in its match
// window (in demo mode any own match counts as in its window), else the
// own match with the kickoff closest to 'utc'; -1 when none is shown.
// 'in_window' tells whether the chosen one is in its window.
int ownForScreen(const MatchStore& store, const Shown& shown, int64_t utc,
                 uint16_t before, uint16_t after, bool demo, bool& in_window);
// The own match of the Web UI overview: a running one first, else the one
// with the kickoff closest to 'utc' (unknown kickoffs last); -1 when none.
int ownForOverview(const MatchStore& store, const Shown& shown, int64_t utc);
// Index of the 'ordinal'-th own match of 'competition' from 'provider'
// (published fixtures of the pair picker); -1 when there is none.
int ownAt(const MatchStore& store, uint8_t competition, cfg::Provider provider,
          uint16_t ordinal);
// Shown matches of 'competition'.
uint16_t matchCount(const MatchStore& store, const Shown& shown,
                    uint8_t competition);

// Which side of a match is the own club (team id = source club).
void ownSides(const MatchStore& store, const StoredMatch& match,
              const Shown& shown, bool& home, bool& away);
// Display name of a side: the owner's replacement from 'names', else the
// directory's short name, else the own slot's full name; "" when unknown.
const char* teamName(const MatchStore& store, const StoredMatch& match,
                     bool home, const persist::ShortNames* names = nullptr);
// Full name of a side of an own match, else teamName().
const char* fullName(const MatchStore& store, const StoredMatch& match,
                     bool home, const persist::ShortNames* names = nullptr);
// Team name of a table row: the owner's replacement, else the directory's
// short name; "" when unknown.
const char* rowName(const MatchStore& store, const StoredRow& row,
                    const persist::ShortNames* names = nullptr);

// Rows of the table of 'competition' in stored order and the own row
// (-1 = none).
struct Table {
  std::array<uint8_t, cfg::kStoreTableRows> row{};
  uint8_t count = 0;
  int own = -1;
};
Table table(const MatchStore& store, uint8_t competition);

// Shown events of an own match (removal markers and hidden fallback events
// left out, MatchStore::eventShown()) in match order: regular time before
// the shootout, then by minute and added minute; an event without minute
// keeps the minute of the event before it in the stored list. Equal keys
// keep the stored order.
struct Events {
  std::array<uint8_t, cfg::kMatchEvents> index{};  // event pool indices
  uint8_t count = 0;
};
Events events(const MatchStore& store, const StoredMatch& match);

// Demo data (store cleared first): one running own match, 2:1 in minute
// 67 without kickoff, and a five-row table in 'competition', all from
// OpenLigaDB; shown with Source{competition, kOpenLigaDb}.
void demo(MatchStore& store, uint8_t competition);

}  // namespace football::view
