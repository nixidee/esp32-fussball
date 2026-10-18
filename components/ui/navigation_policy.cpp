#include "navigation_policy.h"

#include <algorithm>

namespace ui {
namespace {
using Type = screen::Type;
using Mode = sequence::PageMode;
using Kind = sequence::ContentKind;

Kind absent(const NavigationContext& context, const sequence::Entry& entry) {
  const auto& facts = context.data;
  const auto status = contentStatus(facts.store, entry.competition, entry.type,
                                    facts.clock.utc);
  return status.state == football::DatasetState::kComplete &&
                 status.cause == football::Cause::kNone
             ? Kind::kEmpty
             : Kind::kError;
}
uint16_t pages(unsigned count, unsigned per_page) {
  return static_cast<uint16_t>(std::max(1u, (count + per_page - 1) / per_page));
}
}  // namespace

sequence::Input NavigationContext::input() {
  return {policy, *program, data.competitions, selected, content, this};
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
    if (active) out.index[out.count++] = static_cast<football::MatchIndex>(i);
  }
  return out;
}

football::view::List roundMembers(const NavigationContext& context,
                                  const football::selection::Round& round) {
  football::view::List out;
  const auto& store = context.data.store;
  for (std::size_t i = 0; i < store.matchCount(); ++i)
    if (football::selection::roundContains(context.selection, round,
                                           store.match(i)))
      out.index[out.count++] = static_cast<football::MatchIndex>(i);
  return out;
}

static MatchdaySelection selectMatchday(const NavigationContext& context,
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
  return static_cast<NavigationContext*>(raw)
      ->prepare(entry, mode, visible_page)
      .content;
}
const PreparedContent& NavigationContext::prepare(const sequence::Entry& entry,
                                                  Mode mode,
                                                  uint16_t visible_page) {
  const auto status =
      data.store.datasetStatus(entry.competition, contentDataset(entry.type));
  const uint32_t proofs = static_cast<uint32_t>(status.state) |
                          static_cast<uint32_t>(status.cause) << 8 |
                          uint32_t{data.store.notices(entry.competition)} << 16;
  if (prepared_.valid && prepared_.entry == entry && prepared_.mode == mode &&
      prepared_.page == visible_page &&
      match_count_ == data.store.matchCount() &&
      row_count_ == data.store.rowCount() && proofs_ == proofs)
    return prepared_;
  auto& out = prepared_;
  out = {};
  out.entry = entry;
  out.mode = mode;
  out.page = visible_page;
  out.valid = true;
  match_count_ = data.store.matchCount();
  row_count_ = data.store.rowCount();
  proofs_ = proofs;
  const auto& context = *this;
  if (!screen::sporting(entry.type)) {
    out.content = {Kind::kData, 1};
    return out;
  }
  const auto* c = context.data.competitions.find(entry.competition);
  if (!c) return out;
  unsigned count = 0;
  uint16_t anchor = 0;
  if (entry.type == Type::kLiveSingle) {
    if (context.policy.only_one_own_single &&
        context.selected.phase == settings::screen_policy::Phase::kOwn &&
        context.selected.focus.competition != entry.competition)
      return out;
    const auto single =
        football::selection::single(context.selection, context.selected, *c);
    out.single = football::selection::find(context.selection, single.match);
    count = out.single != nullptr;
  } else if (entry.type == Type::kLiveMulti) {
    const auto members = conferenceMembers(context, entry.competition);
    if (mode == Mode::kAutomatic && members.count == 1 &&
        context.selected.phase == settings::screen_policy::Phase::kOwn) {
      const auto& only = context.data.store.match(members.index[0]);
      if (only.id == context.selected.focus.id &&
          only.competition == context.selected.focus.competition)
        for (unsigned i = 0; i < context.program->count; ++i) {
          const auto& row = context.program->entries[i];
          if (row.type == Type::kLiveSingle &&
              row.competition == entry.competition && row.rule.automatic)
            return out;  // The same focused Single already owns this visit.
        }
    }
    football::ConferenceRequest request;
    request.mode = policy.conference_mode;
    request.pin_own = policy.pin_own_match;
    request.manual = mode == Mode::kManualHeld;
    request.per_page = policy.visible_matches;
    request.page = visible_page;
    const auto page =
        football::view::conferencePage(data.store, members, request);
    out.matches = page;
    out.total = members.count;
    if (members.count == 1) out.single = &data.store.match(members.index[0]);
    for (unsigned i = 0; i < members.count; ++i)
      if (data.store.isOwn(data.store.match(members.index[i]))) {
        out.own = members.index[i];
        break;
      }
    count = page.row_count ? page.pages : 0;
    anchor = visible_page;
    if (mode == Mode::kManualHeld &&
        context.policy.conference_mode == cfg::ConferenceMode::kOnePage) {
      request.manual = false;
      const auto automatic =
          football::view::conferencePage(data.store, members, request);
      int pinned = -1;
      if (context.policy.pin_own_match)
        for (unsigned i = 0; i < members.count && pinned < 0; ++i)
          if (data.store.isOwn(data.store.match(members.index[i])))
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
    const auto groups =
        football::view::tableGroups(data.store, entry.competition);
    const auto preferred = football::view::table(data.store, entry.competition);
    football::view::Table automatic_table, held_table;
    unsigned automatic_pages = 0, held_pages = 0, automatic_first = 0,
             held_first = 0, automatic_anchor = 0, own_anchor = 0;
    for (unsigned i = 0; i < groups.count; ++i) {
      const auto table = football::view::table(data.store, entry.competition,
                                               groups.ordinal[i]);
      const unsigned visible =
          std::min<unsigned>(policy.table_window, table.count);
      if (!visible) continue;
      const unsigned last = table.count - visible;
      const unsigned auto_count = pages(table.count, visible);
      const unsigned held_count = last + 1;
      if (table.group == preferred.group && preferred.own >= 0)
        own_anchor = held_pages +
                     std::clamp(preferred.own - static_cast<int>(visible) / 2,
                                0, static_cast<int>(last));
      if (visible_page >= automatic_pages) {
        automatic_table = table;
        automatic_first = std::min<unsigned>(
            (visible_page - automatic_pages) * visible, last);
        automatic_anchor = held_pages + automatic_first;
      }
      if (visible_page >= held_pages) {
        held_table = table;
        held_first = std::min<unsigned>(visible_page - held_pages, last);
      }
      automatic_pages += auto_count;
      held_pages += held_count;
    }
    const bool own = preferred.own >= 0;
    anchor = static_cast<uint16_t>(own ? own_anchor : automatic_anchor);
    const auto& table = mode == Mode::kManualHeld ? held_table
                        : own                     ? preferred
                                                  : automatic_table;
    const unsigned visible =
        std::min<unsigned>(policy.table_window, table.count);
    if (visible) {
      count = mode == Mode::kManualHeld ? held_pages
              : own                     ? 1
                                        : automatic_pages;
      const int first =
          mode == Mode::kManualHeld ? held_first
          : own ? std::clamp(preferred.own - static_cast<int>(visible) / 2, 0,
                             static_cast<int>(table.count - visible))
                : automatic_first;
      out.table = {entry.competition,         table.group, table.label, first,
                   static_cast<int>(visible), table.count, false};
      for (unsigned i = 0; i < visible; ++i)
        out.table_rows[i] = table.row[first + i];
    } else
      out.table = {entry.competition,
                   football::kNoIndex,
                   football::kNoIndex,
                   0,
                   0,
                   0,
                   absent(context, entry) == Kind::kError};
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
      out.matchday = selectMatchday(context, entry, mode, visible_page);
      count = out.matchday.available ? out.matchday.pages : 0;
    }
    if (mode == Mode::kManualHeld)
      out.matchday = selectMatchday(context, entry, mode, visible_page);
    if (out.matchday.available) {
      const auto members = roundMembers(context, out.matchday.round);
      out.total = members.count;
      out.matchday.page =
          std::min<uint16_t>(out.matchday.page, out.matchday.pages - 1);
      out.first = out.matchday.page * policy.visible_matches;
      const auto rows =
          std::min<unsigned>(policy.visible_matches, members.count - out.first);
      out.matches.row_count = rows;
      for (unsigned i = 0; i < rows; ++i)
        out.matches.rows[i] = members.index[out.first + i];
    }
  }
  out.content = {count ? Kind::kData : absent(context, entry),
                 static_cast<uint16_t>(std::max(1u, count)), anchor};
  return out;
}
uint8_t CalendarFacts::find(int64_t utc) const {
  for (uint8_t i = 0; i < count; ++i)
    if (instants[i].utc == utc) return i;
  return kMissing;
}
bool CalendarFacts::civilAt(uint8_t index, std::tm& out) const {
  if (index >= count) return false;
  const auto& instant = instants[index];
  const time_t at = static_cast<time_t>(instant.utc + instant.offset_s);
  return gmtime_r(&at, &out) != nullptr;
}
PresentationFacts presentationFacts(const NavigationState& state,
                                    NavigationContext& context, int64_t now,
                                    int64_t fetched_ms, int64_t retry_ms) {
  PresentationFacts facts;
  facts.sequence = sequence::view(state, context.input(), now);
  facts.phase = context.selected.phase;
  for (const auto& entry : context.data.competitions.entries)
    facts.active |= entry.active;
  if (!facts.sequence.empty && screen::sporting(facts.sequence.entry.type)) {
    const auto mode = state.paged    ? sequence::PageMode::kManualHeld
                      : state.manual ? sequence::PageMode::kManualAutomatic
                                     : sequence::PageMode::kAutomatic;
    facts.page = &context.prepare(state.entry, mode, state.page);
  }
  auto& calendar = facts.calendar;
  const auto& dates = context.data.dates;
  const auto utc = context.data.clock.utc;
  const auto capture = [&](int64_t at) -> uint8_t {
    if (at <= 0) return CalendarFacts::kMissing;
    const auto known = calendar.find(at);
    if (known != CalendarFacts::kMissing) return known;
    if (calendar.count == calendar.instants.size())
      return CalendarFacts::kMissing;
    const auto index = calendar.count++;
    calendar.instants[index] = {at, dates.offset_at
                                        ? dates.offset_at(at, dates.context)
                                        : dates.fallback_offset_s};
    return index;
  };
  capture(utc);
  if (facts.page) {
    if (facts.page->single) capture(facts.page->single->kickoff);
    for (unsigned i = 0; i < facts.page->matches.row_count; ++i)
      capture(context.data.store.match(facts.page->matches.rows[i]).kickoff);
  }
  if (utc > 0) {
    const auto as_of = [&](int64_t fetched) {
      return fetched ? capture(utc - (now - fetched) / 1000)
                     : CalendarFacts::kMissing;
    };
    calendar.fetched = as_of(fetched_ms);
    if (facts.page)
      calendar.dataset = as_of(contentStatus(context.data.store,
                                             facts.sequence.entry.competition,
                                             facts.sequence.entry.type, utc)
                                   .last_success_ms);
    if (retry_ms > now)
      calendar.retry = capture(utc + (retry_ms - now + 999) / 1000);
  }
  return facts;
}

}  // namespace ui
