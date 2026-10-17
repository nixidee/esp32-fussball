#include "navigation_policy.h"

#include <algorithm>

namespace ui {
namespace {
using Type = screen::Type;
using Mode = sequence::PageMode;
using Kind = sequence::ContentKind;

Kind absent(const NavigationContext& context, const sequence::Entry& entry) {
  const auto& facts = context.data;
  for (std::size_t i = 0; i < facts.competitions.entries.size(); ++i) {
    if (facts.competitions.entries[i].id != entry.competition) continue;
    const auto loaded =
        entry.type == Type::kTable ? facts.loaded_tables : facts.loaded_matches;
    return (facts.failed & (1u << i)) || !(loaded & (1u << i)) ? Kind::kError
                                                               : Kind::kEmpty;
  }
  return Kind::kEmpty;
}
uint16_t pages(unsigned count, unsigned per_page) {
  return static_cast<uint16_t>(std::max(1u, (count + per_page - 1) / per_page));
}
}  // namespace

sequence::Input NavigationContext::input() {
  return {policy, data.competitions, selected, content, this};
}

football::view::List conferenceMembers(const NavigationContext& context,
                                       uint8_t competition) {
  football::view::List out;
  const auto* c = context.data.competitions.find(competition);
  if (!c) return out;
  const auto& store = context.data.store;
  for (std::size_t i = 0; i < store.matchCount(); ++i) {
    const auto& match = store.match(i);
    if (match.competition != competition ||
        match.provider != c->primary.provider)
      continue;
    auto window = football::selection::window(context.selection, *c, match);
    bool active = window.active;
    // A kickoff block stays complete while one of its games is in-window.
    // Own/other buffers and the bounded unknown-kickoff live case use the
    // same selector as phase classification, never a second timing rule.
    if (!active && match.kickoff &&
        match.phase != football::Phase::kPostponed &&
        match.phase != football::Phase::kCancelled &&
        match.phase != football::Phase::kAbandoned)
      for (std::size_t j = 0; !active && j < store.matchCount(); ++j) {
        const auto& other = store.match(j);
        if (other.competition == competition &&
            other.provider == match.provider && other.kickoff == match.kickoff)
          active =
              football::selection::window(context.selection, *c, other).active;
      }
    if (active) out.index[out.count++] = static_cast<uint8_t>(i);
  }
  return out;
}

football::ConferencePage conferencePage(const NavigationContext& context,
                                        const sequence::Entry& entry, Mode mode,
                                        uint16_t page) {
  const auto members = conferenceMembers(context, entry.competition);
  football::ConferenceRequest request;
  request.mode = context.policy.conference_mode;
  request.pin_own = context.policy.pin_own_match;
  request.manual = mode == Mode::kManualHeld;
  request.per_page = context.policy.visible_matches;
  request.page = page;
  return football::view::conferencePage(context.data.store, members, request);
}

TableSelection tableSelection(const NavigationContext& context,
                              const sequence::Entry& entry, Mode mode,
                              uint16_t page) {
  const auto table =
      football::view::table(context.data.store, entry.competition);
  const int count = std::min<int>(context.policy.table_window, table.count);
  const int last = std::max(0, table.count - count);
  const int first = mode == Mode::kManualHeld ? std::min<int>(page, last)
                    : table.own >= 0
                        ? std::clamp(table.own - count / 2, 0, last)
                        : std::min<int>(page * count, last);
  return {entry.competition, first, count, table.count,
          table.count == 0 && absent(context, entry) == Kind::kError};
}

football::view::List roundMembers(const NavigationContext& context,
                                  const football::selection::Round& round) {
  football::view::List out;
  const auto& store = context.data.store;
  for (std::size_t i = 0; i < store.matchCount(); ++i)
    if (football::selection::roundContains(context.selection, round,
                                           store.match(i)))
      out.index[out.count++] = static_cast<uint8_t>(i);
  return out;
}

MatchdaySelection matchdaySelection(const NavigationContext& context,
                                    const sequence::Entry& entry, Mode mode,
                                    uint16_t page) {
  const auto* c = context.data.competitions.find(entry.competition);
  if (!c) return {};
  const auto per_page = std::max<unsigned>(1, context.policy.visible_matches);
  if (mode != Mode::kManualHeld) {
    auto round = football::selection::round(context.selection, *c,
                                            context.policy.matchday_mode);
    if (!round.valid() && mode == Mode::kManualAutomatic) {
      const auto reach = football::selection::rounds(
          context.selection, *c, context.policy.matchday_mode);
      if (reach.count) round = reach.round[reach.count - 1];
    }
    const auto list = roundMembers(context, round);
    return {round, page, pages(list.count, per_page), list.count != 0};
  }
  const auto reach = football::selection::rounds(context.selection, *c,
                                                 context.policy.matchday_mode);
  for (unsigned i = 0; i < reach.count; ++i) {
    const auto list = roundMembers(context, reach.round[i]);
    const auto count = pages(list.count, per_page);
    if (page < count) return {reach.round[i], page, count, list.count != 0};
    page -= count;
  }
  return {};
}

sequence::Content content(const sequence::Entry& entry, Mode mode,
                          uint16_t visible_page, void* raw) {
  const auto& context = *static_cast<NavigationContext*>(raw);
  if (!screen::sporting(entry.type)) return {Kind::kData, 1};
  const auto* c = context.data.competitions.find(entry.competition);
  if (!c) return {};
  unsigned count = 0;
  uint16_t anchor = 0;
  if (entry.type == Type::kLiveSingle) {
    if (context.selected.phase == settings::screen_policy::Phase::kOwn &&
        context.selected.focus.competition != entry.competition)
      return {};
    const auto single =
        football::selection::single(context.selection, context.selected, *c);
    count = single.match.valid();
  } else if (entry.type == Type::kLiveMulti) {
    const auto page = conferencePage(context, entry, mode, visible_page);
    count = page.row_count ? page.pages : 0;
    anchor = visible_page;
    if (mode == Mode::kManualHeld &&
        context.policy.conference_mode == cfg::ConferenceMode::kOnePage) {
      const auto automatic =
          conferencePage(context, entry, Mode::kAutomatic, visible_page);
      const auto members = conferenceMembers(context, entry.competition);
      int pinned = -1;
      if (context.policy.pin_own_match)
        for (unsigned i = 0; i < members.count && pinned < 0; ++i)
          if (context.data.store.match(members.index[i]).isOwn())
            pinned = members.index[i];
      football::ConferenceRequest held;
      held.pin_own = context.policy.pin_own_match;
      held.per_page = context.policy.visible_matches;
      for (unsigned row = 0; row < automatic.row_count; ++row) {
        if (automatic.rows[row] == pinned) continue;
        const int page = football::view::conferencePageOf(
            context.data.store, members, held, automatic.rows[row]);
        if (page < 0) continue;
        anchor = static_cast<uint16_t>(page);
        break;
      }
    }
  } else if (entry.type == Type::kTable) {
    const auto table =
        football::view::table(context.data.store, entry.competition);
    const unsigned visible =
        std::min<unsigned>(context.policy.table_window, table.count);
    if (visible) {
      count = mode == Mode::kManualHeld ? table.count - visible + 1
              : table.own >= 0          ? 1
                                        : pages(table.count, visible);
      const int last = std::max<int>(0, table.count - visible);
      anchor = static_cast<uint16_t>(
          table.own >= 0 ? std::clamp<int>(table.own - visible / 2, 0, last)
                         : std::min<unsigned>(visible_page * visible, last));
    }
  } else if (entry.type == Type::kMatchday) {
    if (mode == Mode::kManualHeld) {
      const auto reach = football::selection::rounds(
          context.selection, *c, context.policy.matchday_mode);
      for (unsigned i = 0; i < reach.count; ++i) {
        const auto list = roundMembers(context, reach.round[i]);
        if (static_cast<int>(i) == reach.selected ||
            (reach.selected < 0 && i + 1 == reach.count))
          anchor = static_cast<uint16_t>(count + visible_page);
        count += pages(list.count, context.policy.visible_matches);
      }
    } else {
      const auto chosen = matchdaySelection(context, entry, mode, visible_page);
      count = chosen.available ? chosen.pages : 0;
    }
  }
  return {count ? Kind::kData : absent(context, entry),
          static_cast<uint16_t>(std::max(1u, count)), anchor};
}
}  // namespace ui
