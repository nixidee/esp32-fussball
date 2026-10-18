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

namespace football::view {

// The provider whose matches a competition shows. Club membership belongs to
// MatchStore; screen eligibility belongs only to screen policy.
struct Source {
  uint8_t competition = 0;
  cfg::Provider provider = cfg::Provider::kOpenLigaDb;
};

// The shown source of every active competition: its provider's matches.
// Matches of other providers and of competitions not added stay hidden.
class Shown {
 public:
  // Adds an active entry (inactive ones are skipped).
  void add(const settings::CompetitionRoute& entry);
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
  std::array<MatchIndex, cfg::kStoreMatches> index{};
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
                     const ConferenceRequest& request, MatchIndex store_index);

// The own match of the Web UI overview: a running one first, else the one
// with the kickoff closest to 'utc' (unknown kickoffs last); -1 when none.
int ownForOverview(const MatchStore& store, const Shown& shown, int64_t utc);
// Shown matches of 'competition'.
uint16_t matchCount(const MatchStore& store, const Shown& shown,
                    uint8_t competition);

// Borrow the original26-byte provider fallback from the shared directory;
// "" when unknown. Adopted visible labels resolve outside the football lock.
const char* teamName(const MatchStore& store, const StoredMatch& match,
                     bool home);
// Borrow the same original directory fallback for a table row; "" if absent.
const char* rowName(const MatchStore& store, const StoredRow& row);

// Rows of the table of 'competition' in stored order and the own row
// (-1 = none).
struct Table {
  std::array<uint8_t, cfg::kStoreTableRows> row{};
  uint8_t count = 0;
  int own = -1;
  uint8_t group = kNoIndex, label = kNoIndex;
};
struct TableGroups {
  std::array<uint8_t, cfg::kStoreTableGroups> ordinal{};
  uint8_t count = 0;
};
TableGroups tableGroups(const MatchStore& store, uint8_t competition);
// Unspecified group prefers the own club's group, else the first. Explicit
// ordinals, including zero, address exactly one provider table.
Table table(const MatchStore& store, uint8_t competition,
            uint8_t group = kNoIndex);

// Shown events of a detailed match (removal markers left out) in match order:
// regular time before the shootout, then by minute and added minute; an event
// without minute keeps the minute of the event before it in the stored list.
// Equal keys keep the stored order.
struct Events {
  std::array<uint8_t, cfg::kMatchEvents> index{};  // event pool indices
  uint8_t count = 0;
};
Events events(const MatchStore& store, const StoredMatch& match);
// All reported, retained shootout attempts; separate from the twelve
// highlights. Pool exhaustion remains visible through match flags and
// competition notice.
struct Shootout {
  std::array<uint8_t, cfg::kStoreEvents> index{};
  uint8_t count = 0;
};
Shootout shootout(const MatchStore& store, const StoredMatch& match);

}  // namespace football::view
