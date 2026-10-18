#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "screen_types.h"

namespace settings {
struct Competitions;
struct Competition;
struct CompetitionRoutes;
}  // namespace settings

namespace settings::screen_policy {

enum class Phase : uint8_t { kOwn, kOtherGames, kMatchday, kIdle, kBreak };
inline constexpr std::size_t kPhaseCount = 5;
constexpr std::size_t index(Phase phase) noexcept {
  return static_cast<std::size_t>(phase);
}
constexpr bool valid(Phase phase) noexcept {
  return index(phase) < kPhaseCount;
}

struct Rule {
  uint16_t visit_s = cfg::kScreenVisitRange.min;
  bool enabled = false;
  bool automatic = false;
  constexpr bool automaticallyReachable() const noexcept {
    return enabled && automatic;
  }
  bool operator==(const Rule&) const noexcept = default;
};

enum OverrideField : uint8_t {
  kEnabled = 1 << 0,
  kAutomatic = 1 << 1,
  kVisit = 1 << 2,
};
inline constexpr uint8_t kOverrideFields = kEnabled | kAutomatic | kVisit;
inline constexpr uint8_t kRuleFlags = kEnabled | kAutomatic;

struct Override {
  uint16_t visit_s = cfg::kScreenVisitRange.min;
  uint8_t values =
      0;  // kEnabled / kAutomatic, independently of field presence.
  uint8_t fields =
      0;  // Explicit per-field overrides, including explicit false.
  bool operator==(const Override&) const noexcept = default;
};

// The competition owns its preferences: identity cannot drift into a second
// association table. Null/field-presence semantics retain dormant overrides.
struct CompetitionPreferences {
  std::array<std::array<Override, screen::kSportTypeCount>, kPhaseCount>
      overrides{};
  bool operator==(const CompetitionPreferences&) const noexcept = default;
};

struct Appearance {
  uint32_t colour = cfg::kThemeBackgroundDefault;
  uint32_t text = cfg::kThemeTextDefault;
  uint32_t accent = cfg::kThemeAccentDefault;
  uint8_t text_scale = cfg::kTextScaleDefault;
  uint8_t image = cfg::kStockImageId;
  bool background = cfg::kBackgroundsDefault;
  bool dots = cfg::kScreenDotsDefault;
  bool operator==(const Appearance&) const noexcept = default;
};

using PhaseRules = std::array<Rule, screen::kTypeCount>;
using SharedRules = std::array<PhaseRules, kPhaseCount>;

constexpr SharedRules initialRules() noexcept {
  static_assert(std::size(cfg::kScreenAutomaticDefault) == kPhaseCount);
  SharedRules rules{};
  for (std::size_t phase = 0; phase < rules.size(); ++phase)
    for (std::size_t type = 0; type < screen::kTypeCount; ++type) {
      auto& rule = rules[phase][type];
      const auto kind = static_cast<screen::Type>(type);
      const bool sport = screen::sporting(kind);
      rule.visit_s = cfg::kScreenVisitDefaultS[type];
      rule.enabled = sport ? cfg::kScreenSportsEnabledDefault
                           : cfg::kScreenImagesEnabledDefault;
      rule.automatic = !sport || cfg::kScreenAutomaticDefault[phase] == kind;
    }
  return rules;
}

struct Options {
  std::array<Appearance, screen::kTypeCount> appearance{};
  std::array<uint8_t, cfg::kSlideMax> slides{};
  uint16_t break_days = cfg::kScreenBreakDaysDefault;
  uint16_t own_before_min = cfg::kMatchWindowDefaultMinutes;
  uint16_t own_after_min = cfg::kMatchWindowDefaultMinutes;
  uint16_t other_before_min = cfg::kMatchWindowDefaultMinutes;
  uint16_t other_after_min = cfg::kMatchWindowDefaultMinutes;
  uint16_t manual_return_s = cfg::kManualReturnDefaultS;
  uint16_t per_image_s = cfg::kSlideshowDefaultS;
  uint8_t slide_count = 0;
  uint8_t visible_matches = cfg::kVisibleMatchesDefault;
  uint8_t table_window = cfg::kTableWindowDefault;
  cfg::ConferenceMode conference_mode = cfg::kConferenceModeDefault;
  cfg::MatchdayMode matchday_mode = cfg::kMatchdayModeDefault;
  bool backgrounds = cfg::kBackgroundsDefault;
  bool pin_own_match = cfg::kPinOwnMatchDefault;
  bool frame = cfg::kScreenFrameDefault;
  bool frame_crest = cfg::kScreenFrameCrestDefault;
  bool automatic_break = cfg::kScreenBreakDefault;
  bool only_one_own_single = cfg::kOnlyOneOwnSingleDefault;
  bool operator==(const Options&) const noexcept = default;
};

struct Policy : Options {
  SharedRules shared = initialRules();
  // One sportIndex bit per phase: activate retained competition overrides.
  std::array<uint8_t, kPhaseCount> per_competition{};
  std::array<screen::Type, screen::kTypeCount> order = cfg::kScreenOrderDefault;
  bool operator==(const Policy&) const noexcept = default;
};

static_assert(sizeof(Rule) == 4 && sizeof(Override) == 4);
static_assert(sizeof(Policy) <= cfg::kScreenPolicyMaxBytes);

struct Limits {
  uint8_t visible_matches_max =
      static_cast<uint8_t>(cfg::kVisibleMatchesRange.max);
  uint8_t table_window_max = static_cast<uint8_t>(cfg::kTableWindowRange.max);
};

// Structured location for a future API's actionable error path; no strings or
// dynamically allocated field-path construction are needed in the pure model.
enum class Field : uint8_t {
  kNone,
  kShared,
  kOverride,
  kPerCompetition,
  kOrder,
  kAppearance,
  kSlides,
  kBreakDays,
  kOwnBefore,
  kOwnAfter,
  kOtherBefore,
  kOtherAfter,
  kManualReturn,
  kPerImage,
  kVisibleMatches,
  kTableWindow,
  kConferenceMode,
  kMatchdayMode,
};
enum class Reason : uint8_t {
  kNone,
  kRange,
  kUnknownBits,
  kDuplicate,
  kRequired,
  kImageAutomatic,
  kImageMultiple,
  kUnusedSlot,
};
enum class Property : uint8_t {
  kNone,
  kVisit,
  kValues,
  kFields,
  kColour,
  kText,
  kAccent,
  kTextScale,
  kImage,
};
struct Error {
  Field field = Field::kNone;
  Reason reason = Reason::kNone;
  Property property = Property::kNone;
  uint8_t phase = 0;
  uint8_t type = 0;
  uint8_t competition = 0;
  bool operator==(const Error&) const noexcept = default;
};

bool validate(const Policy& policy, Error* error = nullptr,
              const Limits& limits = {}) noexcept;

// Resolve the shared fields with the preferences of this exact competition.
// A null competition resolves the global image/shared rule only.
Rule resolve(const Policy&, Phase, screen::Type,
             const Competition* competition = nullptr) noexcept;
bool perCompetition(const Policy&, Phase, screen::Type) noexcept;
bool validate(const CompetitionPreferences&, Error* error = nullptr,
              uint8_t competition = 0) noexcept;

struct CompetitionOrder {
  std::array<uint8_t, cfg::kCompetitionMax> slots{};
  uint8_t count = 0;
};
// Explicit group blocks inherit the earliest member's priority. Members keep
// their configured order; text and provider names never establish membership.
CompetitionOrder competitionOrder(const Competitions&) noexcept;
CompetitionOrder competitionOrder(const CompetitionRoutes&) noexcept;

struct ProgramEntry {
  Rule rule;
  screen::Type type = screen::Type::kCrest;
  uint8_t competition = 0;
  bool operator==(const ProgramEntry&) const noexcept = default;
};
inline constexpr std::size_t kProgramMax =
    screen::kSportTypeCount * cfg::kCompetitionMax + 2;
struct Program {
  std::array<ProgramEntry, kProgramMax> entries{};
  uint8_t count = 0;
  bool operator==(const Program&) const noexcept = default;
};
// The one normalized effective list. Disabled entries are absent; manual-only
// sport entries remain present with automatic=false. Images are global.
void program(const Policy&, const Competitions&, Phase, Program&) noexcept;
static_assert(sizeof(ProgramEntry) == 6 && sizeof(Program) <= 208);

// Small derived values for classification/provider consumers. They never own
// the complete rule matrix or retain a Policy pointer. Bits follow the current
// competition-list positions and must be refreshed with that same snapshot.
struct SelectionRules {
  uint16_t own_before_min = cfg::kMatchWindowDefaultMinutes;
  uint16_t own_after_min = cfg::kMatchWindowDefaultMinutes;
  uint16_t other_before_min = cfg::kMatchWindowDefaultMinutes;
  uint16_t other_after_min = cfg::kMatchWindowDefaultMinutes;
  uint8_t own_special = 0;
  bool automatic_break = cfg::kScreenBreakDefault;
  bool only_one_own_single = cfg::kOnlyOneOwnSingleDefault;
};
struct Eligibility {
  SelectionRules selection;
  // One bit per stored screen::Type. Manual-only entries are enabled too.
  std::array<std::array<uint8_t, cfg::kCompetitionMax>, kPhaseCount> enabled{};
  uint16_t break_days = cfg::kScreenBreakDaysDefault;
  bool frame = cfg::kScreenFrameDefault;
  bool allows(Phase phase, screen::Type type, std::size_t slot) const noexcept {
    return valid(phase) && screen::sporting(type) &&
           slot < cfg::kCompetitionMax &&
           (enabled[index(phase)][slot] & (1u << screen::index(type)));
  }
};
SelectionRules selectionRules(const Options&, const Competitions&) noexcept;
SelectionRules selectionRules(const Options&,
                              const CompetitionRoutes&) noexcept;
Eligibility eligibility(const Policy& policy,
                        const Competitions& competitions) noexcept;
Eligibility eligibility(const Policy&, const CompetitionRoutes&,
                        const Competitions* configured = nullptr) noexcept;
static_assert(sizeof(SelectionRules) <= 12 && sizeof(Eligibility) <= 56);

// Also used by manual-only sport entries while automatic subpage rotation is
// active. Page boundaries divide milliseconds, retaining the whole visit.
uint32_t effectiveVisitMs(const Rule& rule, uint16_t pages) noexcept;
uint32_t pageBoundaryMs(uint32_t total_ms, uint16_t pages,
                        uint16_t boundary) noexcept;

}  // namespace settings::screen_policy
