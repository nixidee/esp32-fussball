// Canonical content projections for the common sequence. This module owns no
// independent traversal or clock; screen_sequence is the sole navigation
// policy.
#pragma once
#include "screen_sequence.h"
#include "store_views.h"

namespace ui {
using NavigationState = sequence::State;
struct NavigationData {
  const football::MatchStore& store;
  const football::view::Shown& shown;
  const settings::Competitions& competitions;
  football::Clock clock;
  football::DateContext dates;
  uint8_t loaded_matches = 0, loaded_tables = 0, failed = 0;
  bool configured = false, break_proven = false;
};
struct NavigationContext {
  const settings::screen_policy::Policy& policy;
  const NavigationData& data;
  football::selection::Input selection;
  football::selection::Selection selected;
  NavigationContext(const settings::screen_policy::Policy& rules,
                    const NavigationData& facts)
      : policy(rules),
        data(facts),
        selection(rules, facts.competitions, facts.store, facts.clock,
                  facts.dates, facts.break_proven),
        selected(football::selection::select(selection)) {}
  sequence::Input input();
};
sequence::Content content(const sequence::Entry& entry, sequence::PageMode mode,
                          uint16_t visible_page, void* context);
football::view::List conferenceMembers(const NavigationContext& context,
                                       uint8_t competition);
football::ConferencePage conferencePage(const NavigationContext& context,
                                        const sequence::Entry& entry,
                                        sequence::PageMode mode, uint16_t page);
struct TableSelection {
  uint8_t competition = 0;
  int first = 0, count = 0, total = 0;
  bool fault = false;
};
TableSelection tableSelection(const NavigationContext& context,
                              const sequence::Entry& entry,
                              sequence::PageMode mode, uint16_t page);
struct MatchdaySelection {
  football::selection::Round round;
  uint16_t page = 0, pages = 1;
  bool available = false;
};
MatchdaySelection matchdaySelection(const NavigationContext& context,
                                    const sequence::Entry& entry,
                                    sequence::PageMode mode, uint16_t page);
football::view::List roundMembers(const NavigationContext& context,
                                  const football::selection::Round& round);
}  // namespace ui
