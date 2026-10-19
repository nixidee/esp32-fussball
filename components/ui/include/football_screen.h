#pragma once
#include <array>
#include <cstdint>
#include <new>
#include <type_traits>

#include "app_config.h"
#include "safe_area.h"
#include "settings_model.h"
namespace ui {
// Semantic colour of a text, pill or ring. The view maps it to the screen's
// palette (docs/UI.md "Colours and night").
enum class Tone : uint8_t { kText, kDim, kAccent, kWarn, kError, kNeutral };
// Screen layout the view draws (docs/UI.md "Screens").
enum class Layout : uint8_t { kImage, kOwnMatch, kConference, kTable, kCard };
enum class RimKind : uint8_t { kNone, kMatch, kRing, kSegments, kScroll };
// Information on the rim of a round panel; rect panels draw no rim.
struct Rim {
  RimKind kind = RimKind::kNone;
  Tone tone = Tone::kAccent;  // kRing
  // kMatch: played minute (-1 = no progress), finished match (dim ring),
  // extra time as a second inner lap, and the half-time mark.
  uint16_t progress_degrees = 0, extra_degrees = 0;
  bool progress_known = false, extra_known = false, gap = false;
  Tone progress_tone = Tone::kAccent, top = Tone::kDim, bottom = Tone::kDim;
  uint8_t done = 0, current = 0;             // kSegments: three steps
  uint16_t first = 0, count = 0, total = 0;  // kScroll: visible rows
};
struct MatchRow {
  char home[41]{}, away[41]{}, centre[12]{};
  char home_short[4]{}, away_short[4]{};
  Tone centre_tone = Tone::kText;
  // Own club name in accent; own match as a dark row; a recent goal as an
  // accent row.
  bool own_home = false, own_away = false, own = false, goal = false;
};
enum class Attempt : uint8_t {
  kUnknown,
  kHomeScored,
  kHomeMissed,
  kAwayScored,
  kAwayMissed,
  kUnknownScored,
  kUnknownMissed,
  kHomeUnknown,
  kAwayUnknown
};
struct Shootout {
  std::array<Attempt, 10> attempts{};
  char caption[48]{};
  uint8_t count = 0;
  bool active = false;
};
struct OwnMatch {
  char home[41]{}, away[41]{}, centre[12]{}, pill[24]{}, highlight[96]{};
  char home_short[4]{}, away_short[4]{};
  Tone pill_tone = Tone::kAccent, pill_text = Tone::kAccent,
       highlight_tone = Tone::kText;
  bool own_home = false, own_away = false, large_centre = true;
  Shootout shootout;
};
struct TableRow {
  // Signed table values accept -10000..10000; difference also shows a '+'
  // for positive values. Both need six characters plus the terminator.
  char name[41]{}, position[4]{}, played[4]{}, difference[7]{}, points[7]{};
  char name_short[4]{};
  bool own = false;
};
// Full-screen state: title, up to two lines (an accent line stands out) and
// two hints.
struct Card {
  char title[48]{};
  char lines[2][48]{};
  char hints[2][48]{};
  bool accent[2]{};
  uint8_t line_count = 0, hint_count = 0;
};
struct ConferenceContent {
  std::array<MatchRow, cfg::kVisibleMatchesRange.max> rows{};
  uint8_t count = 0, slots = 0;
};
struct TableContent {
  std::array<TableRow, cfg::kTableWindowRange.max> rows{};
  // Points, goal difference and played, in display order.
  char columns[3][6]{};
  uint8_t count = 0;
};
struct ViewModel {
  ViewModel() noexcept : empty_(0) {}
  Layout layout() const { return layout_; }
  // Begin the selected payload's lifetime before any output targets refer to
  // it. Common scene metadata survives; no other payload remains live.
  void setLayout(Layout next) {
    switch (next) {
      case Layout::kOwnMatch: new (&own) OwnMatch(); break;
      case Layout::kConference: new (&conference) ConferenceContent(); break;
      case Layout::kTable: new (&table) TableContent(); break;
      case Layout::kCard: new (&card) Card(); break;
      case Layout::kImage: new (&empty_) uint8_t{0}; break;
    }
    layout_ = next;
  }
  screen::Type screen = screen::Type::kCrest;
  settings::ScreenStyle style;
  // Own match: competition · round; Konferenz: title with page; table:
  // competition name.
  char caption[80]{};
  union {
    OwnMatch own;
    ConferenceContent conference;
    TableContent table;
    Card card;
    uint8_t empty_;
  };
  char status[64]{};
  Tone status_tone = Tone::kText;
  // The status is a temporary pill over the bottom of the content
  // (OQ-284); false: a permanent line ("Demo").
  bool status_pill = false;
  // IP chip: one line (station IP) or two (setup network name and IP).
  char badge[2][40]{};
  uint8_t badge_lines = 0;
  bool badge_bottom = false;
  Rim rim;
  // Konferenz pages or slideshow position (dots); pages 0 or 1 = no dots.
  uint16_t page = 0, pages = 0;
  char image[64]{};
  // Image-service attempt, including fallback/retry of an unchanged file path.
  uint32_t image_revision = 0;

 private:
  Layout layout_ = Layout::kImage;
};
static_assert(std::is_trivially_copyable_v<ViewModel> &&
              std::is_trivially_destructible_v<ViewModel>);
bool render(const ViewModel& model, const geometry::SafeArea& safe_area);
}  // namespace ui
