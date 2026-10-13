// Konferenz membership and page layout (docs/UI.md, "Konferenz"). Pure: no
// clock, settings store, hardware or LVGL; host-tested in `native`.
#pragma once
#include <array>
#include <cstdint>

#include "app_config.h"
#include "football_model.h"

namespace football {
struct ConferenceRules {
  uint16_t before = 0, after = 0;  // minutes
  bool whole_matchday = false;
};
// Snapshot indices in kickoff order; equal kickoff keeps snapshot order.
struct ConferenceList {
  std::array<uint8_t, cfg::kMatches> index{};
  uint8_t count = 0;
};
// Matches of the Konferenz at `utc`. A block is all matches with the same
// kickoff, across competitions, from kickoff - before. Every match of a block
// stays until the block ends (latest matchEnd of the block) or the next block
// starts; a match still running at the next block's start stays until its
// own matchEnd. With whole_matchday an own match that has kicked off is added
// while a match of its round is shown.
ConferenceList conferenceMembers(const Snapshot& data, int64_t utc,
                                 const ConferenceRules& rules);
// Every match in kickoff order (demo mode).
ConferenceList conferenceAll(const Snapshot& data);

inline constexpr std::size_t kConferenceRowsMax = 9;
struct ConferenceRequest {
  cfg::ConferenceMode mode = cfg::kConferenceModeDefault;
  bool pin_own = cfg::kPinOwnMatchDefault;
  // Kickoff-order paging by the user, in either mode.
  bool manual = false;
  uint8_t per_page = 1;
  // Wrapped by the page count when automatic, clamped when manual.
  unsigned page = 0;
};
struct ConferencePage {
  std::array<uint8_t, kConferenceRowsMax> rows{};  // snapshot indices
  uint8_t row_count = 0, page = 0, pages = 0;
};
// Rows of one page. per_page includes a pinned own match, which stands in
// row 1 of every page. "One page" ranks the others by activity: running with
// the latest score change first, goalless running by kickoff, then finished,
// then not started; all other cases use kickoff order.
ConferencePage conferencePage(const Snapshot& data, const ConferenceList& list,
                              const ConferenceRequest& request);
}  // namespace football
