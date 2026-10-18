// Canonical content projections for the common sequence. This module owns no
// independent traversal or clock; screen_sequence is the sole navigation
// policy.
#pragma once
#include "screen_sequence.h"
#include "store_views.h"
#include "ui_config.h"

namespace ui {
using NavigationState = sequence::State;
struct NavigationContext;
struct NavigationData {
  const football::MatchStore& store;
  const football::view::Shown& shown;
  const settings::CompetitionRoutes& competitions;
  football::Clock clock;
  football::DateContext dates;
  bool configured = false, break_proven = false;
};
inline football::MatchStore::Dataset contentDataset(screen::Type type) {
  return type == screen::Type::kTable
             ? football::MatchStore::Dataset::kTable
             : football::MatchStore::Dataset::kFixtures;
}
inline football::DatasetStatus contentStatus(const football::MatchStore& store,
                                             uint8_t competition,
                                             screen::Type type, int64_t utc) {
  const auto dataset = contentDataset(type);
  auto status = store.datasetStatus(competition, dataset);
  if (status.cause == football::Cause::kNone)
    status.cause = store.datasetNotice(competition, dataset, utc);
  return status;
}
sequence::Content content(const sequence::Entry& entry, sequence::PageMode mode,
                          uint16_t visible_page, void* context);
football::view::List conferenceMembers(const NavigationContext& context,
                                       uint8_t competition);
struct TableSelection {
  uint8_t competition = 0, group = football::kNoIndex,
          label = football::kNoIndex;
  int first = 0, count = 0, total = 0;
  bool fault = false;
};
struct MatchdaySelection {
  football::selection::Round round;
  uint16_t page = 0, pages = 1;
  bool available = false;
};
football::view::List roundMembers(const NavigationContext& context,
                                  const football::selection::Round& round);
// One bounded prepared page, borrowed only within this publication read.
// It never retains the 512-member lists used to derive the page.
struct PreparedContent {
  sequence::Entry entry;
  sequence::PageMode mode = sequence::PageMode::kAutomatic;
  uint16_t page = 0;
  sequence::Content content;
  const football::StoredMatch* single = nullptr;
  football::ConferencePage matches;
  TableSelection table;
  MatchdaySelection matchday;
  std::array<uint8_t, cfg::kTableWindowRange.max> table_rows{};
  football::MatchIndex own = football::kNoMatch;
  uint16_t total = 0, first = 0;
  bool valid = false;
};
struct NavigationContext {
  const settings::screen_policy::Options& policy;
  const NavigationData& data;
  settings::screen_policy::Program demo_program;
  const settings::screen_policy::Program* program;
  football::selection::Input selection;
  football::selection::Selection selected;
  NavigationContext(const settings::UiConfig& configuration,
                    const NavigationData& facts)
      : policy(configuration.screens),
        data(facts),
        program(&configuration.program),
        selection(configuration.screens, facts.competitions, facts.store,
                  facts.clock, facts.dates, facts.break_proven),
        selected(football::selection::select(selection)) {
    if (configuration.device.demo) {
      settings::demoProgram(configuration, facts.competitions, demo_program);
      program = &demo_program;
    }
  }
  sequence::Input input();
  const PreparedContent& prepare(const sequence::Entry&, sequence::PageMode,
                                 uint16_t page);

 private:
  PreparedContent prepared_;
  uint16_t match_count_ = 0, row_count_ = 0;
  uint32_t proofs_ = 0;
};
}  // namespace ui
