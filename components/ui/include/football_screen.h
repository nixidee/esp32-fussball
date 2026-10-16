#pragma once
#include <array>
#include <cstdint>

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
  int16_t minute = -1;
  bool finished = false, extra = false, gap = false;
  uint8_t done = 0, current = 0;            // kSegments: three steps
  uint8_t first = 0, count = 0, total = 0;  // kScroll: visible rows
};
struct MatchRow {
  char home[41]{}, away[41]{}, centre[12]{};
  Tone centre_tone = Tone::kText;
  // Own club name in accent; own match as a dark row; a recent goal as an
  // accent row.
  bool own_home = false, own_away = false, own = false, goal = false;
};
struct OwnMatch {
  char home[41]{}, away[41]{}, centre[12]{}, pill[24]{}, highlight[96]{};
  Tone pill_tone = Tone::kAccent, pill_text = Tone::kAccent,
       highlight_tone = Tone::kText;
  bool own_home = false, own_away = false;
};
struct TableRow {
  // Signed table values accept -10000..10000; difference also shows a '+'
  // for positive values. Both need six characters plus the terminator.
  char name[41]{}, position[4]{}, played[4]{}, difference[7]{}, points[7]{};
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
struct ViewModel {
  cfg::Screen screen = cfg::Screen::kCrest;
  settings::ScreenStyle style;
  Layout layout = Layout::kImage;
  // Own match: competition · round; Konferenz: title with page; table:
  // competition name.
  char caption[64]{};
  OwnMatch own;
  std::array<MatchRow, cfg::kVisibleMatchesRange.max> matches{};
  std::array<TableRow, cfg::kTableWindowRange.max> table{};
  // Rows shown and, for the Konferenz, fixed row slots while paging.
  uint8_t row_count = 0, slots = 0;
  // Table column titles: points, goal difference, played.
  char columns[3][6]{};
  Card card;
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
  uint8_t page = 0, pages = 0;
  char image[64]{};
  // Image-service attempt, including fallback/retry of an unchanged file path.
  uint32_t image_revision = 0;
  bool night = false;
  // Night colour level 0..255 towards the night background; 255 = colours
  // unchanged (a backlight dims instead).
  uint8_t night_colour = 255;
};
bool render(const ViewModel& model, const geometry::SafeArea& safe_area);
}  // namespace ui
