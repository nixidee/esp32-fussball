// Pure common screen sequence and visible-time accounting. Borrowed settings
// and a short-lived content probe replace retained lists or copied models.
#pragma once

#include <cstdint>

#include "input_policy.h"
#include "screen_selection.h"

namespace ui::sequence {
namespace policy = settings::screen_policy;

struct Entry {
  screen::Type type = static_cast<screen::Type>(0xff);
  uint8_t competition = 0;
  uint32_t incarnation = 0;
  bool valid() const {
    return screen::valid(type) &&
           (screen::sporting(type) ? competition != 0
                                   : competition == 0 && incarnation == 0);
  }
  bool operator==(const Entry&) const = default;
};

enum class PageMode : uint8_t { kAutomatic, kManualAutomatic, kManualHeld };
enum class ContentKind : uint8_t { kEmpty, kData, kError };
struct Content {
  ContentKind kind = ContentKind::kEmpty;
  uint16_t pages = 1;
  // On first Up/Down, map the currently visible automatic position into the
  // complete manual projection BEFORE moving one step. Otherwise ignored.
  uint16_t anchor_page = 0;
};
// Content only: never repeat enable/mask logic here. kError is an eligible
// actionable error card; data with a warning remain kData; confirmed empty
// content is kEmpty. Return at least one page for eligible content.
//
// Automatic/ManualAutomatic expose normal automatic pages: Conference
// One-page stays one page and an own-centred table keeps its automatic view.
// ManualHeld exposes COMPLETE navigation: each full-table row position, or
// a flattened sequence of held/current/next sporting rounds and their pages.
// No row/round may disappear merely because the controller counts pages.
// visible_page refers to the preceding projection and allows anchor_page to
// map that position when entering ManualHeld. At an expanded projection's
// boundary the controller moves to another competition of this same type.
//
// For image entries visible_page is the configured image-list position (frame
// includes optional crest first). Error/fallback presentation remains eligible.
using Probe = Content (*)(const Entry&, PageMode, uint16_t visible_page,
                          void* context);

struct Input {
  const policy::Options& rules;
  const policy::Program& program;
  const settings::CompetitionRoutes& competitions;
  const football::selection::Selection& selected;
  Probe probe = nullptr;
  void* context = nullptr;
};

// No repository pointers and no retained catalog. Time uses monotonic ms.
// The ordinary image continuation cursor is separate from frame browsing.
struct State {
  Entry entry;
  Entry visible_entry;  // last confirmed output, while a new one is pending
  football::selection::MatchKey focus;
  int64_t observed_at = 0, input_at = 0;
  uint32_t elapsed_ms = 0, total_ms = 0, image_elapsed_ms = 0;
  uint32_t revision = 0;
  uint16_t page = 0, pages = 1, visible_page = 0;
  policy::Phase phase = policy::Phase::kIdle;
  uint8_t image_cursor = 0, image_count = 0, image_id = 0, next_image = 0;
  uint8_t visible_image = 0;
  bool initialized = false, manual = false, paged = false, frame = false;
  bool presented = false, scroll_down = true;
};

struct View {
  Entry entry;
  uint32_t revision = 0, elapsed_ms = 0, total_ms = 0, return_left_ms = 0;
  uint16_t page = 0, pages = 1;
  uint8_t image = 0;
  bool empty = true, manual = false, paged = false, frame = false;
};

// Starts at the first eligible automatic entry; frame instead starts with a
// permissible already visible image or optional crest then library order.
void reset(State& state, const Input& input, int64_t now);
// Phase changes end manual and restart first automatic. Same-phase own focus
// changes replace a visible Single immediately while retaining visit time.
// Timed sporting visits retain their page count and duration when content
// grows; extra pages join the next visit. Shrink still removes invalid pages.
// Up/Down's complete held projection always exposes the current content.
void synchronize(State& state, const Input& input, int64_t now);
void applyAction(State& state, const Input& input, Action action, int64_t now);
// Confirm only a matching successfully displayed output. Stale confirmations
// do nothing. Unseen pages/images earn no time, and a late tick never skips
// through unseen outputs. Repeated confirmation does not reset a visible clock.
// A settled image-error card earns its finite visit, but does not consume the
// ordinary slideshow continuation cursor.
void presented(State& state, uint32_t revision, int64_t now,
               bool image_content = true);
View view(const State& state, const Input& input, int64_t now);

// Array-free NORMAL profile sequence access; frame uses View's image pages.
// Also useful for later position indicators.
// Automatic has at most 34 entries; manual at most 32 sporting entries.
uint8_t count(const Input& input, bool manual);
Entry first(const Input& input, bool manual);
Entry adjacent(const Input& input, const Entry& visible, bool manual,
               int direction);

static_assert(sizeof(Entry) <= 8 && sizeof(State) <= 128 && sizeof(View) <= 40);

}  // namespace ui::sequence
