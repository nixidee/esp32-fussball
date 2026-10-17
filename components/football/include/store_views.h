// Read side of the compact store (docs/DATA_FLOW.md, "Views from the
// store"): which rows the screens and the Web UI show, the Konferenz, the
// own match, a league table and detailed matches' events. The views keep no
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
// (0 = no own club). Screen eligibility belongs only to screen policy.
struct Source {
  uint8_t competition = 0;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
  uint32_t club = 0;
};

// The shown source of every active competition: its provider's matches.
// Matches of other providers and of competitions not added stay hidden.
class Shown {
 public:
  // Adds an active entry (inactive ones are skipped).
  void add(const settings::Competition& entry);
  // Adds a source directly (demo).
  void add(const Source& source);
  const Source* source(uint8_t competition) const;
  bool shows(const StoredMatch& match) const;
  std::size_t count() const { return count_; }
  const Source& at(std::size_t i) const { return sources_[i]; }

 private:
  std::array<Source, cfg::kCompetitionMax> sources_{};
  uint8_t count_ = 0;
};

// Store indices in kickoff order.
struct List {
  std::array<uint8_t, cfg::kStoreMatches> index{};
  uint16_t count = 0;
};

// Kicked off: running, finished or abandoned.
bool started(Phase phase);

// Rows of one Konferenz page (store indices). per_page includes a pinned
// own match, which stands in row 1 of every page. "One page" ranks the
// others by activity: running with the latest score change first, goalless
// running by kickoff, then finished, then not started; all other cases use
// kickoff order. Keep the original page count; redistribute a sparse final
// page from the back, at most one match per earlier page (two for two pages).
ConferencePage conferencePage(const MatchStore& store, const List& list,
                              const ConferenceRequest& request);
// The actual redistributed kickoff-order page containing a store row,
// independently of One-page activity ranking; pinned row -> 0, absent -> -1.
int conferencePageOf(const MatchStore& store, const List& list,
                     const ConferenceRequest& request, uint8_t store_index);

// The own match of the Web UI overview: a running one first, else the one
// with the kickoff closest to 'utc' (unknown kickoffs last); -1 when none.
int ownForOverview(const MatchStore& store, const Shown& shown, int64_t utc);
// Shown matches of 'competition'.
uint16_t matchCount(const MatchStore& store, const Shown& shown,
                    uint8_t competition);

// Which side of a match is the own club (team id = source club).
void ownSides(const MatchStore& store, const StoredMatch& match,
              const Shown& shown, bool& home, bool& away);
// Display name of a side: the owner's replacement from 'names', else the
// directory's short name, else the detail slot's full name; "" when unknown.
const char* teamName(const MatchStore& store, const StoredMatch& match,
                     bool home, const persist::ShortNames* names = nullptr);
// Full name of a side with match details, else teamName().
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

// Shown events of a detailed match (removal markers left out) in match order:
// regular time before the shootout, then by minute and added minute; an event
// without minute keeps the minute of the event before it in the stored list.
// Equal keys keep the stored order.
struct Events {
  std::array<uint8_t, cfg::kMatchEvents> index{};  // event pool indices
  uint8_t count = 0;
};
Events events(const MatchStore& store, const StoredMatch& match);

}  // namespace football::view
