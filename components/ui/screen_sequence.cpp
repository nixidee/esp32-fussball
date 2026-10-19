#include "screen_sequence.h"

#include <algorithm>

namespace ui::sequence {
namespace {

bool imageType(screen::Type type) {
  return type == screen::Type::kCrest || type == screen::Type::kSlideshow;
}

PageMode mode(const State& state) {
  return !state.manual ? PageMode::kAutomatic
         : state.paged ? PageMode::kManualHeld
                       : PageMode::kManualAutomatic;
}

Content probe(const Input& input, const Entry& entry, PageMode page_mode,
              uint16_t visible_page = 0) {
  if (!input.probe) return {};
  auto out = input.probe(entry, page_mode, visible_page, input.context);
  out.pages = std::max<uint16_t>(out.pages, 1);
  out.anchor_page = std::min<uint16_t>(out.anchor_page, out.pages - 1);
  return out;
}

policy::Rule ruleFor(const Input& input, const Entry& entry) {
  for (unsigned i = 0; i < input.program.count; ++i) {
    const auto& row = input.program.entries[i];
    if (row.type == entry.type && row.competition == entry.competition)
      return row.rule;
  }
  return {};
}

bool eligible(const Input& input, const Entry& entry, PageMode page_mode,
              Content* content = nullptr, uint16_t visible_page = 0) {
  if (!entry.valid() ||
      (page_mode != PageMode::kAutomatic && !screen::sporting(entry.type)))
    return false;
  if (screen::sporting(entry.type)) {
    const auto* c = input.competitions.find(entry.competition);
    if (!c || !c->active || c->incarnation != entry.incarnation) return false;
  } else if (entry.type == screen::Type::kSlideshow &&
             input.rules.slide_count == 0) {
    return false;
  }
  const auto rule = ruleFor(input, entry);
  if (!rule.enabled || (page_mode == PageMode::kAutomatic && !rule.automatic))
    return false;
  const auto found = probe(input, entry, page_mode, visible_page);
  if (found.kind == ContentKind::kEmpty) return false;
  if (content) *content = found;
  return true;
}

template <class Visit>
void rawEntries(const Input& input, Visit visit) {
  for (unsigned position = 0; position < input.program.count; ++position) {
    const auto& row = input.program.entries[position];
    const auto* competition = input.competitions.find(row.competition);
    visit(Entry{row.type, row.competition,
                competition ? competition->incarnation : 0},
          position);
  }
}

Entry adjacentMode(const Input& input, const Entry& visible, PageMode page_mode,
                   int direction, bool same_type) {
  int origin = -1;
  rawEntries(input, [&](const Entry& entry, int position) {
    if (entry == visible) origin = position;
  });
  const int total = input.program.count;
  for (int step = 1; step <= total; ++step) {
    const int position =
        origin < 0 ? (direction >= 0 ? step - 1 : total - step)
                   : (origin + (direction >= 0 ? step : -step) + total) % total;
    const auto& row = input.program.entries[position];
    const auto* competition = input.competitions.find(row.competition);
    const Entry entry{row.type, row.competition,
                      competition ? competition->incarnation : 0};
    if ((!same_type || entry.type == visible.type) &&
        eligible(input, entry, page_mode))
      return entry;
  }
  return {};
}

void invalidate(State& state, int64_t now) {
  if (++state.revision == 0) ++state.revision;
  state.presented = false;
  state.observed_at = now;
}

uint32_t imageTime(const Input& input) {
  return uint32_t{input.rules.per_image_s} * 1000;
}

void enter(State& state, const Input& input, Entry entry, bool manual,
           int64_t now) {
  state.entry = entry;
  state.manual = manual;
  state.paged = false;
  state.page = 0;
  state.elapsed_ms = state.image_elapsed_ms = 0;
  state.image_id = state.image_cursor = state.image_count = 0;
  state.total_ms = 0;
  state.pages = 1;
  if (entry.valid()) {
    const auto content = probe(input, entry, mode(state));
    state.pages = screen::sporting(entry.type) ? content.pages : 1;
    const auto rule = ruleFor(input, entry);
    state.total_ms = policy::effectiveVisitMs(rule, state.pages);
    if (entry.type == screen::Type::kSlideshow && input.rules.slide_count) {
      state.image_count = input.rules.slide_count;
      state.image_cursor = state.next_image % state.image_count;
      state.image_id = input.rules.slides[state.image_cursor];
    }
  }
  invalidate(state, now);
}

uint8_t frameCount(const Input& input) {
  return input.rules.slide_count + (input.rules.frame_crest ? 1 : 0);
}

uint8_t frameImage(const Input& input, uint8_t cursor) {
  if (input.rules.frame_crest && cursor == 0) return cfg::kStockImageId;
  return input.rules.slides[cursor - (input.rules.frame_crest ? 1 : 0)];
}

Entry frameEntry(const Input& input, uint8_t cursor) {
  return {input.rules.frame_crest && cursor == 0 ? screen::Type::kCrest
                                                 : screen::Type::kSlideshow};
}

bool frameEligible(const Input& input, uint8_t cursor) {
  return cursor < frameCount(input) &&
         probe(input, frameEntry(input, cursor), PageMode::kAutomatic, cursor)
                 .kind != ContentKind::kEmpty;
}

int framePosition(const Input& input, uint8_t image) {
  for (uint8_t i = 0; i < frameCount(input); ++i)
    if (frameImage(input, i) == image && frameEligible(input, i)) return i;
  return -1;
}

int adjacentFrame(const Input& input, int origin, int direction) {
  const int count = frameCount(input);
  if (count == 0) return -1;
  if (origin < 0) {
    for (int i = 0; i < count; ++i)
      if (frameEligible(input, i)) return i;
    return -1;
  }
  for (int step = 1; step <= count; ++step) {
    const int i = (origin + (direction >= 0 ? step : -step) + count) % count;
    if (frameEligible(input, i)) return i;
  }
  return -1;
}

void enterFrame(State& state, const Input& input, int cursor, bool manual,
                int64_t now) {
  state.manual = manual;
  state.paged = false;
  state.elapsed_ms = state.image_elapsed_ms = 0;
  state.page = 0;
  state.pages = 1;
  state.image_count = frameCount(input);
  state.image_cursor = cursor < 0 ? 0 : static_cast<uint8_t>(cursor);
  state.entry = cursor < 0 ? Entry{} : frameEntry(input, state.image_cursor);
  state.image_id = cursor < 0 ? 0 : frameImage(input, state.image_cursor);
  state.total_ms = cursor < 0 ? 0 : imageTime(input);
  invalidate(state, now);
}

void automatic(State& state, const Input& input, int64_t now) {
  if (state.frame) {
    int cursor = frameEligible(input, state.image_cursor) &&
                         frameImage(input, state.image_cursor) == state.image_id
                     ? state.image_cursor
                     : framePosition(input, state.image_id);
    if (cursor < 0) cursor = adjacentFrame(input, -1, 1);
    enterFrame(state, input, cursor, false, now);
  } else {
    enter(state, input, first(input, false), false, now);
  }
}

void account(State& state, const Input& input, int64_t now) {
  const int64_t elapsed = std::max<int64_t>(0, now - state.observed_at);
  state.observed_at = now;
  if (!state.entry.valid() || !state.presented ||
      (state.manual && (state.paged || state.frame)))
    return;
  uint32_t limit = state.total_ms;
  const bool images =
      state.frame || state.entry.type == screen::Type::kSlideshow;
  if (images) {
    const auto left = imageTime(input) > state.image_elapsed_ms
                          ? imageTime(input) - state.image_elapsed_ms
                          : 0;
    limit = std::min<uint32_t>(limit, state.elapsed_ms + left);
  } else if (screen::sporting(state.entry.type)) {
    limit = policy::pageBoundaryMs(state.total_ms, state.pages, state.page + 1);
  }
  const uint32_t room = limit > state.elapsed_ms ? limit - state.elapsed_ms : 0;
  const auto credit = static_cast<uint32_t>(std::min<int64_t>(elapsed, room));
  state.elapsed_ms += credit;
  if (images) state.image_elapsed_ms += credit;
}

void progress(State& state, const Input& input, int64_t now) {
  if (!state.entry.valid() || !state.presented ||
      (state.manual && (state.paged || state.frame)))
    return;
  if (state.frame) {
    if (state.image_elapsed_ms >= imageTime(input))
      enterFrame(state, input, adjacentFrame(input, state.image_cursor, 1),
                 false, now);
    return;
  }
  if (state.entry.type == screen::Type::kSlideshow &&
      state.image_elapsed_ms >= imageTime(input) &&
      state.elapsed_ms < state.total_ms) {
    if (state.image_count) {
      state.image_cursor = (state.image_cursor + 1) % state.image_count;
      state.image_id = input.rules.slides[state.image_cursor];
    }
    state.image_elapsed_ms = 0;
    invalidate(state, now);
    return;
  }
  if (screen::sporting(state.entry.type) && state.page + 1 < state.pages &&
      state.elapsed_ms >=
          policy::pageBoundaryMs(state.total_ms, state.pages, state.page + 1)) {
    ++state.page;
    invalidate(state, now);
    return;
  }
  if (state.elapsed_ms >= state.total_ms) {
    const auto next =
        state.manual ? state.entry : adjacent(input, state.entry, false, 1);
    enter(state, input, next, state.manual, now);
  }
}

void pageAction(State& state, const Input& input, int direction, int64_t now) {
  if (!state.manual && state.visible_entry.valid() &&
      eligible(input, state.visible_entry, PageMode::kManualAutomatic))
    state.entry = state.visible_entry;
  if (!screen::sporting(state.entry.type) ||
      state.entry.type == screen::Type::kLiveSingle)
    return;
  const uint16_t shown_page =
      !state.paged && !state.presented && state.visible_entry == state.entry
          ? state.visible_page
          : state.page;
  const auto content =
      probe(input, state.entry, PageMode::kManualHeld, shown_page);
  if (content.kind == ContentKind::kEmpty) return;
  const int origin = state.paged
                         ? std::min<uint16_t>(state.page, content.pages - 1)
                         : content.anchor_page;
  const int next = origin + direction;
  state.manual = state.paged = true;
  state.pages = content.pages;
  if (next >= 0 && next < content.pages) {
    state.page = static_cast<uint16_t>(next);
    invalidate(state, now);
    return;
  }
  const auto target =
      adjacentMode(input, state.entry, PageMode::kManualHeld, direction, true);
  if (target.valid()) {
    enter(state, input, target, true, now);
    const auto target_content = probe(input, target, PageMode::kManualHeld);
    state.pages = target_content.pages;
    state.page = direction > 0 ? 0 : state.pages - 1;
    state.paged = true;
  }
}

}  // namespace

uint8_t count(const Input& input, bool manual) {
  uint8_t out = 0;
  const auto wanted =
      manual ? PageMode::kManualAutomatic : PageMode::kAutomatic;
  rawEntries(input, [&](const Entry& entry, int) {
    if (eligible(input, entry, wanted)) ++out;
  });
  return out;
}

Entry first(const Input& input, bool manual) {
  return adjacentMode(
      input, {}, manual ? PageMode::kManualAutomatic : PageMode::kAutomatic, 1,
      false);
}

Entry adjacent(const Input& input, const Entry& visible, bool manual,
               int direction) {
  return adjacentMode(
      input, visible,
      manual ? PageMode::kManualAutomatic : PageMode::kAutomatic, direction,
      false);
}

void reset(State& state, const Input& input, int64_t now) {
  const Entry visible_entry = state.visible_entry;
  const uint16_t visible_page = state.visible_page;
  const uint8_t image = state.visible_image;
  const bool visible_image =
      visible_entry.valid() && imageType(visible_entry.type);
  const uint8_t next_image = state.next_image;
  const uint32_t revision = state.revision;
  state = {};
  state.next_image = next_image;
  state.revision = revision;
  state.visible_entry = visible_entry;
  state.visible_page = visible_page;
  state.visible_image = image;
  state.initialized = true;
  state.phase = input.selected.phase;
  state.focus = input.selected.focus;
  state.frame = input.rules.frame;
  state.input_at = now;
  if (state.frame) {
    int cursor = visible_image ? framePosition(input, image) : -1;
    if (cursor < 0) cursor = adjacentFrame(input, -1, 1);
    enterFrame(state, input, cursor, false, now);
  } else {
    automatic(state, input, now);
  }
}

static void synchronizeImpl(State& state, const Input& input, int64_t now,
                            bool advance) {
  if (!state.initialized || state.frame != input.rules.frame ||
      (!state.frame && state.phase != input.selected.phase)) {
    reset(state, input, now);
    return;
  }
  if (state.frame) {
    state.phase = input.selected.phase;
    state.focus = input.selected.focus;
    if (!state.entry.valid() || !frameEligible(input, state.image_cursor)) {
      enterFrame(state, input, adjacentFrame(input, -1, 1), false, now);
      return;
    }
  } else {
    if (state.focus != input.selected.focus &&
        state.entry.type == screen::Type::kLiveSingle) {
      Entry target;
      rawEntries(input, [&](const Entry& entry, int) {
        if (entry.type == screen::Type::kLiveSingle &&
            entry.competition == input.selected.competition &&
            eligible(input, entry, mode(state)))
          target = entry;
      });
      if (target.valid()) {
        account(state, input, now);
        state.entry = target;
        invalidate(state, now);
      }
    }
    state.focus = input.selected.focus;
    Content content;
    if (!eligible(input, state.entry, mode(state), &content, state.page)) {
      auto next = adjacentMode(input, state.entry, mode(state), 1, false);
      if (!next.valid() && state.manual) {
        next = first(input, false);
        state.manual = false;
      }
      if (next.valid() || state.entry.valid() || state.manual)
        enter(state, input, next, state.manual, now);
      return;
    }
    if (screen::sporting(state.entry.type)) {
      account(state, input, now);
      // Additional automatic pages belong to the next visit. The current
      // visit keeps its original clock and page boundaries; held navigation
      // remains a complete projection of the currently available content.
      if (content.pages < state.pages || state.paged) {
        state.pages = content.pages;
        if (state.page >= state.pages) {
          state.page = state.pages - 1;
          invalidate(state, now);
        }
      }
    }
  }
  account(state, input, now);
  if (state.manual && input.rules.manual_return_s != 0 &&
      now - state.input_at >= int64_t{input.rules.manual_return_s} * 1000) {
    automatic(state, input, now);
    return;
  }
  if (advance) progress(state, input, now);
}

void synchronize(State& state, const Input& input, int64_t now) {
  synchronizeImpl(state, input, now, true);
}

void applyAction(State& state, const Input& input, Action action, int64_t now) {
  state.input_at = now;
  // Navigation is relative to the currently visible entry, even when its
  // automatic visit would expire at this same timestamp.
  synchronizeImpl(state, input, now, false);
  // The phase/frame reset above must not erase the timestamp of this input.
  state.input_at = now;
  if (action == Action::kAutomaticNext) {
    if (state.manual) {
      state.scroll_down = true;
      automatic(state, input, now);
    } else if (state.entry.valid() && state.presented) {
      // Advance precisely one existing automatic boundary. Do not credit
      // unseen output, skip pages/images or replace the saved timing policy.
      if (state.frame) {
        state.image_elapsed_ms = imageTime(input);
      } else if (state.entry.type == screen::Type::kSlideshow) {
        const auto left = imageTime(input) > state.image_elapsed_ms
                              ? imageTime(input) - state.image_elapsed_ms
                              : 0;
        state.elapsed_ms = std::min(state.total_ms, state.elapsed_ms + left);
        state.image_elapsed_ms = imageTime(input);
      } else if (screen::sporting(state.entry.type)) {
        state.elapsed_ms =
            policy::pageBoundaryMs(state.total_ms, state.pages, state.page + 1);
      } else {
        state.elapsed_ms = state.total_ms;
      }
      progress(state, input, now);
    }
  } else if (action == Action::kDefault) {
    state.scroll_down = true;
    automatic(state, input, now);
  } else if (action == Action::kToggleDirection) {
    state.scroll_down = !state.scroll_down;
  } else if (state.frame &&
             (action == Action::kNext || action == Action::kPrevious ||
              action == Action::kUp || action == Action::kDown)) {
    const int direction =
        action == Action::kNext || action == Action::kDown ? 1 : -1;
    const int origin = !state.manual && imageType(state.visible_entry.type)
                           ? framePosition(input, state.visible_image)
                           : state.image_cursor;
    enterFrame(state, input, adjacentFrame(input, origin, direction), true,
               now);
  } else if (action == Action::kNext || action == Action::kPrevious) {
    const auto origin = !state.manual && state.visible_entry.valid()
                            ? state.visible_entry
                            : state.entry;
    const auto target =
        adjacent(input, origin, true, action == Action::kNext ? 1 : -1);
    if (target.valid()) enter(state, input, target, true, now);
  } else if (action == Action::kUp || action == Action::kDown) {
    pageAction(state, input, action == Action::kDown ? 1 : -1, now);
  }
}

void presented(State& state, uint32_t revision, int64_t now,
               bool image_content) {
  if (revision != state.revision || state.presented || !state.entry.valid())
    return;
  state.presented = true;
  state.observed_at = now;
  state.visible_entry = state.entry;
  state.visible_page = state.page;
  state.visible_image = state.image_id;
  if (image_content && !state.frame &&
      state.entry.type == screen::Type::kSlideshow && state.image_count)
    state.next_image = (state.image_cursor + 1) % state.image_count;
}

View view(const State& state, const Input& input, int64_t now) {
  View out;
  out.entry = state.entry;
  out.revision = state.revision;
  out.elapsed_ms = state.elapsed_ms;
  out.total_ms = state.total_ms;
  out.page = state.page;
  out.pages = state.pages;
  out.image = state.image_id;
  out.empty = !state.entry.valid();
  out.manual = state.manual;
  out.paged = state.paged;
  out.frame = state.frame;
  if (state.frame) {
    out.page = out.pages = 0;
    for (uint8_t i = 0; i < frameCount(input); ++i)
      if (frameEligible(input, i)) {
        if (i < state.image_cursor) ++out.page;
        ++out.pages;
      }
  } else if (state.entry.type == screen::Type::kSlideshow) {
    out.page = state.image_cursor;
    out.pages = state.image_count;
  }
  if (state.manual && input.rules.manual_return_s != 0) {
    const int64_t left = int64_t{input.rules.manual_return_s} * 1000 -
                         std::max<int64_t>(0, now - state.input_at);
    out.return_left_ms = static_cast<uint32_t>(std::max<int64_t>(0, left));
  }
  return out;
}

}  // namespace ui::sequence
