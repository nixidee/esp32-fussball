#include "screen_policy.h"

#include <algorithm>
#include <utility>

#include "settings_model.h"
namespace settings::screen_policy {
namespace {

class Check {
 public:
  explicit Check(Error* out) : out_(out) {
    if (out_) *out_ = {};
  }
  bool fail(Field field, Reason reason, Property property = Property::kNone,
            unsigned phase = 0, unsigned type = 0,
            unsigned competition = 0) const noexcept {
    if (out_)
      *out_ = {field,
               reason,
               property,
               static_cast<uint8_t>(phase),
               static_cast<uint8_t>(type),
               static_cast<uint8_t>(competition)};
    return false;
  }
  bool range(Field field, cfg::Range bounds, uint16_t value) const noexcept {
    return bounds.contains(value) || fail(field, Reason::kRange);
  }

 private:
  Error* out_;
};

bool validateRules(const Policy& policy, const Check& check) noexcept {
  for (unsigned phase = 0; phase < kPhaseCount; ++phase) {
    if (policy.per_competition[phase] >> screen::kSportTypeCount)
      return check.fail(Field::kPerCompetition, Reason::kUnknownBits,
                        Property::kNone, phase);
    for (unsigned type = 0; type < screen::kTypeCount; ++type) {
      const Rule& rule = policy.shared[phase][type];
      if (!cfg::kScreenVisitRange.contains(rule.visit_s))
        return check.fail(Field::kShared, Reason::kRange, Property::kVisit,
                          phase, type);
      const auto kind = static_cast<screen::Type>(type);
      if (!screen::sporting(kind) && rule.enabled && !rule.automatic)
        return check.fail(Field::kShared, Reason::kImageAutomatic,
                          Property::kValues, phase, type);
      if (kind == screen::Type::kSlideshow && rule.enabled &&
          rule.visit_s % policy.per_image_s != 0)
        return check.fail(Field::kShared, Reason::kImageMultiple,
                          Property::kVisit, phase, type);
    }
  }
  return true;
}

bool validateAppearance(const Policy& policy, const Check& check) noexcept {
  for (unsigned type = 0; type < policy.appearance.size(); ++type) {
    const auto& style = policy.appearance[type];
    for (const auto& [colour, property] :
         {std::pair{style.colour, Property::kColour},
          std::pair{style.text, Property::kText},
          std::pair{style.accent, Property::kAccent}})
      if (colour > 0xffffff)
        return check.fail(Field::kAppearance, Reason::kRange, property, 0,
                          type);
    if (!cfg::kTextScaleRange.contains(style.text_scale))
      return check.fail(Field::kAppearance, Reason::kRange,
                        Property::kTextScale, 0, type);
    if (style.image > cfg::kImageLibraryMax)
      return check.fail(Field::kAppearance, Reason::kRange, Property::kImage, 0,
                        type);
    if (type == screen::index(screen::Type::kCrest) &&
        style.image != cfg::kStockImageId)
      return check.fail(Field::kAppearance, Reason::kRange, Property::kImage, 0,
                        type);
  }
  return true;
}

}  // namespace

bool validate(const Policy& policy, Error* error,
              const Limits& limits) noexcept {
  const Check check(error);
  if (!(check.range(Field::kBreakDays, cfg::kScreenBreakDaysRange,
                    policy.break_days) &&
        check.range(Field::kOwnBefore, cfg::kMatchWindowRange,
                    policy.own_before_min) &&
        check.range(Field::kOwnAfter, cfg::kMatchWindowRange,
                    policy.own_after_min) &&
        check.range(Field::kOtherBefore, cfg::kMatchWindowRange,
                    policy.other_before_min) &&
        check.range(Field::kOtherAfter, cfg::kMatchWindowRange,
                    policy.other_after_min) &&
        check.range(Field::kManualReturn, cfg::kReturnTimeRange,
                    policy.manual_return_s) &&
        check.range(Field::kPerImage, cfg::kSlideshowRange,
                    policy.per_image_s) &&
        check.range(Field::kVisibleMatches,
                    {cfg::kVisibleMatchesRange.min,
                     std::min<uint16_t>(cfg::kVisibleMatchesRange.max,
                                        limits.visible_matches_max)},
                    policy.visible_matches) &&
        check.range(Field::kTableWindow,
                    {cfg::kTableWindowRange.min,
                     std::min<uint16_t>(cfg::kTableWindowRange.max,
                                        limits.table_window_max)},
                    policy.table_window) &&
        check.range(Field::kConferenceMode,
                    {0, static_cast<uint8_t>(cfg::ConferenceMode::kOnePage)},
                    static_cast<uint8_t>(policy.conference_mode)) &&
        check.range(Field::kMatchdayMode,
                    {0, static_cast<uint8_t>(cfg::MatchdayMode::kNext)},
                    static_cast<uint8_t>(policy.matchday_mode))))
    return false;
  if (policy.slide_count > policy.slides.size())
    return check.fail(Field::kSlides, Reason::kRange);
  for (unsigned i = 0; i < policy.slides.size(); ++i) {
    if (i < policy.slide_count) {
      if (policy.slides[i] == 0 || policy.slides[i] > cfg::kImageLibraryMax)
        return check.fail(Field::kSlides, Reason::kRange, Property::kImage, 0,
                          0, i);
    } else if (policy.slides[i] != 0) {
      return check.fail(Field::kSlides, Reason::kUnusedSlot, Property::kImage,
                        0, 0, i);
    }
  }
  uint8_t seen = 0;
  for (unsigned i = 0; i < policy.order.size(); ++i) {
    const auto type = policy.order[i];
    if (!screen::valid(type))
      return check.fail(Field::kOrder, Reason::kRange, Property::kNone, 0, i);
    const auto bit = static_cast<uint8_t>(1u << screen::index(type));
    if (seen & bit)
      return check.fail(Field::kOrder, Reason::kDuplicate, Property::kNone, 0,
                        i);
    seen |= bit;
  }
  return validateRules(policy, check) && validateAppearance(policy, check);
}

bool validate(const CompetitionPreferences& preferences, Error* error,
              uint8_t competition) noexcept {
  const Check check(error);
  for (unsigned phase = 0; phase < kPhaseCount; ++phase)
    for (unsigned type = 0; type < screen::kSportTypeCount; ++type) {
      const auto& value = preferences.overrides[phase][type];
      if (value.fields & ~kOverrideFields)
        return check.fail(Field::kOverride, Reason::kUnknownBits,
                          Property::kFields, phase, type, competition);
      if (value.values & ~kRuleFlags)
        return check.fail(Field::kOverride, Reason::kUnknownBits,
                          Property::kValues, phase, type, competition);
      if (!cfg::kScreenVisitRange.contains(value.visit_s))
        return check.fail(Field::kOverride, Reason::kRange, Property::kVisit,
                          phase, type, competition);
    }
  return true;
}

bool perCompetition(const Policy& policy, Phase phase,
                    screen::Type type) noexcept {
  return valid(phase) && screen::sporting(type) &&
         (policy.per_competition[index(phase)] &
          (1u << screen::sportIndex(type)));
}

Rule resolve(const Policy& policy, Phase phase, screen::Type type,
             const Competition* competition) noexcept {
  if (!valid(phase) || !screen::valid(type)) return {};
  Rule rule = policy.shared[index(phase)][screen::index(type)];
  if (!competition || !perCompetition(policy, phase, type)) return rule;
  const auto& value =
      competition->display.overrides[index(phase)][screen::sportIndex(type)];
  if (value.fields & kEnabled) rule.enabled = (value.values & kEnabled) != 0;
  if (value.fields & kAutomatic)
    rule.automatic = (value.values & kAutomatic) != 0;
  if (value.fields & kVisit) rule.visit_s = value.visit_s;
  return rule;
}

template <class Entries>
CompetitionOrder ordered(const Entries& competitions) noexcept {
  CompetitionOrder out;
  uint8_t emitted = 0;
  for (unsigned slot = 0; slot < competitions.entries.size(); ++slot) {
    if (emitted & (1u << slot)) continue;
    const auto group = competitions.entries[slot].club.group;
    for (unsigned member = slot; member < competitions.entries.size();
         ++member) {
      if ((emitted & (1u << member)) ||
          (member != slot &&
           (!group || competitions.entries[member].club.group != group)))
        continue;
      out.slots[out.count++] = static_cast<uint8_t>(member);
      emitted |= static_cast<uint8_t>(1u << member);
    }
  }
  return out;
}

CompetitionOrder competitionOrder(const Competitions& competitions) noexcept {
  return ordered(competitions);
}
CompetitionOrder competitionOrder(
    const CompetitionRoutes& competitions) noexcept {
  return ordered(competitions);
}

void program(const Policy& policy, const Competitions& competitions,
             Phase phase, Program& out) noexcept {
  out = {};
  if (!valid(phase)) return;
  const auto priority = competitionOrder(competitions);
  for (const auto type : policy.order) {
    if (screen::sporting(type)) {
      for (unsigned position = 0; position < priority.count; ++position) {
        const auto& competition =
            competitions.entries[priority.slots[position]];
        if (!competition.active || !competition.id) continue;
        const auto rule = resolve(policy, phase, type, &competition);
        if (rule.enabled)
          out.entries[out.count++] = {rule, type, competition.id};
      }
    } else {
      const auto rule = resolve(policy, phase, type);
      if (rule.enabled) out.entries[out.count++] = {rule, type, 0};
    }
  }
}

template <class Entries>
SelectionRules selection(const Options& policy,
                         const Entries& competitions) noexcept {
  SelectionRules out{policy.own_before_min,
                     policy.own_after_min,
                     policy.other_before_min,
                     policy.other_after_min,
                     0,
                     policy.automatic_break};
  out.only_one_own_single = policy.only_one_own_single;
  for (std::size_t slot = 0; slot < competitions.entries.size(); ++slot)
    if (competitions.entries[slot].club.own_special)
      out.own_special |= static_cast<uint8_t>(1u << slot);

  return out;
}

SelectionRules selectionRules(const Options& policy,
                              const Competitions& competitions) noexcept {
  return selection(policy, competitions);
}
SelectionRules selectionRules(const Options& policy,
                              const CompetitionRoutes& competitions) noexcept {
  return selection(policy, competitions);
}

Eligibility eligibility(const Policy& policy,
                        const Competitions& competitions) noexcept {
  Eligibility out;
  out.selection = selectionRules(policy, competitions);
  out.break_days = policy.break_days;
  out.frame = policy.frame;
  for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
    for (std::size_t slot = 0; slot < competitions.entries.size(); ++slot) {
      const auto& c = competitions.entries[slot];
      if (!c.active || c.id == 0) continue;
      for (std::size_t type = 0; type < screen::kTypeCount; ++type) {
        const auto kind = static_cast<screen::Type>(type);
        if (screen::sporting(kind) &&
            resolve(policy, static_cast<Phase>(phase), kind, &c).enabled)
          out.enabled[phase][slot] |= static_cast<uint8_t>(1u << type);
      }
    }
  return out;
}

Eligibility eligibility(const Policy& policy, const CompetitionRoutes& routes,
                        const Competitions* configured) noexcept {
  Eligibility out;
  out.selection = selectionRules(policy, routes);
  out.break_days = policy.break_days;
  out.frame = policy.frame;
  for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
    for (std::size_t slot = 0; slot < routes.entries.size(); ++slot) {
      const auto& route = routes.entries[slot];
      if (!route.active || !route.id) continue;
      const auto* preferences =
          configured ? configured->find(route.id) : nullptr;
      if (preferences && preferences->incarnation != route.incarnation)
        preferences = nullptr;
      for (unsigned type = 0; type < screen::kTypeCount; ++type) {
        const auto kind = static_cast<screen::Type>(type);
        if (screen::sporting(kind) &&
            resolve(policy, static_cast<Phase>(phase), kind, preferences)
                .enabled)
          out.enabled[phase][slot] |= static_cast<uint8_t>(1u << type);
      }
    }
  return out;
}

uint32_t effectiveVisitMs(const Rule& rule, uint16_t pages) noexcept {
  return std::max({uint32_t{rule.visit_s}, uint32_t{cfg::kScreenVisitRange.min},
                   uint32_t{pages} * cfg::kScreenMinimumPageS}) *
         1000;
}

uint32_t pageBoundaryMs(uint32_t total_ms, uint16_t pages,
                        uint16_t boundary) noexcept {
  const uint32_t count = std::max<uint16_t>(pages, 1);
  return static_cast<uint32_t>(uint64_t{total_ms} *
                               std::min<uint32_t>(boundary, count) / count);
}

}  // namespace settings::screen_policy
