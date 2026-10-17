#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "app_config.h"
#include "screen_types.h"

namespace settings {
struct Competitions;
}

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

struct Association {
  // The competition service assigns incarnation and owns its lifetime.
  // Reusing a small id must receive a different nonzero incarnation.
  uint32_t incarnation = 0;
  uint8_t id = 0;  // id=0 and incarnation=0 denote an unused slot.
  bool own_special = true;
  bool operator==(const Association&) const noexcept = default;
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
using CompetitionRules = std::array<Override, cfg::kCompetitionMax>;
using SportRules = std::array<CompetitionRules, screen::kSportTypeCount>;

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

struct Policy {
  // Build bounded snapshots in place; avoid a complete constant template per
  // global instance in firmware flash.
  Policy() noexcept {}
  std::array<Association, cfg::kCompetitionMax> competitions{};
  SharedRules shared = initialRules();
  std::array<SportRules, kPhaseCount> overrides{};
  // One bit per sportIndex(), not per stored Type identity.
  std::array<uint8_t, kPhaseCount> expanded{};
  std::array<Appearance, screen::kTypeCount> appearance{};
  std::array<screen::Type, screen::kTypeCount> order = cfg::kScreenOrderDefault;
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
  kExpanded,
  kCompetition,
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
  kId,
  kIncarnation,
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

// Unknown, removed or reincarnated identities inherit shared values. Expansion
// off ignores, and never changes, its retained overrides.
Rule resolve(const Policy& policy, Phase phase, screen::Type type,
             uint8_t competition_id = 0, uint32_t incarnation = 0) noexcept;
std::size_t associationIndex(const Policy& policy, uint8_t competition_id,
                             uint32_t incarnation) noexcept;
bool expanded(const Policy& policy, Phase phase, screen::Type type) noexcept;

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
SelectionRules selectionRules(const Policy& policy,
                              const Competitions& competitions) noexcept;
Eligibility eligibility(const Policy& policy,
                        const Competitions& competitions) noexcept;
static_assert(sizeof(SelectionRules) <= 12 && sizeof(Eligibility) <= 56);

// Also used by manual-only sport entries while automatic subpage rotation is
// active. Page boundaries divide milliseconds, retaining the whole visit.
uint32_t effectiveVisitMs(const Rule& rule, uint16_t pages) noexcept;
uint32_t pageBoundaryMs(uint32_t total_ms, uint16_t pages,
                        uint16_t boundary) noexcept;

}  // namespace settings::screen_policy
