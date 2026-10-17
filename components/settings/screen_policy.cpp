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
    if (policy.expanded[phase] >> screen::kSportTypeCount)
      return check.fail(Field::kExpanded, Reason::kUnknownBits, Property::kNone,
                        phase);
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
      const auto sport = screen::sportIndex(kind);
      if (sport == screen::kSportTypeCount) continue;
      for (unsigned competition = 0; competition < cfg::kCompetitionMax;
           ++competition) {
        const Override& value = policy.overrides[phase][sport][competition];
        if (value.fields & ~kOverrideFields)
          return check.fail(Field::kOverride, Reason::kUnknownBits,
                            Property::kFields, phase, type, competition);
        if (value.values & ~kRuleFlags)
          return check.fail(Field::kOverride, Reason::kUnknownBits,
                            Property::kValues, phase, type, competition);
        if (!cfg::kScreenVisitRange.contains(value.visit_s))
          return check.fail(Field::kOverride, Reason::kRange, Property::kVisit,
                            phase, type, competition);
        if (value.fields != 0 && policy.competitions[competition].id == 0)
          return check.fail(Field::kOverride, Reason::kUnusedSlot,
                            Property::kFields, phase, type, competition);
      }
    }
  }
  return true;
}

bool validateAssociations(const Policy& policy, const Check& check) noexcept {
  for (unsigned i = 0; i < policy.competitions.size(); ++i) {
    const auto& entry = policy.competitions[i];
    if ((entry.id == 0) != (entry.incarnation == 0))
      return check.fail(Field::kCompetition, Reason::kRequired,
                        entry.id == 0 ? Property::kId : Property::kIncarnation,
                        0, 0, i);
    if (entry.id == 0) continue;
    for (unsigned previous = 0; previous < i; ++previous) {
      const auto& other = policy.competitions[previous];
      if (entry.id == other.id)
        return check.fail(Field::kCompetition, Reason::kDuplicate,
                          Property::kId, 0, 0, i);
      if (entry.incarnation == other.incarnation)
        return check.fail(Field::kCompetition, Reason::kDuplicate,
                          Property::kIncarnation, 0, 0, i);
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
  return validateAssociations(policy, check) && validateRules(policy, check) &&
         validateAppearance(policy, check);
}

std::size_t associationIndex(const Policy& policy, uint8_t competition_id,
                             uint32_t incarnation) noexcept {
  if (competition_id != 0 && incarnation != 0)
    for (std::size_t i = 0; i < policy.competitions.size(); ++i) {
      const auto& entry = policy.competitions[i];
      if (entry.id == competition_id && entry.incarnation == incarnation)
        return i;
    }
  return policy.competitions.size();
}

bool expanded(const Policy& policy, Phase phase, screen::Type type) noexcept {
  return valid(phase) && screen::sporting(type) &&
         (policy.expanded[index(phase)] & (1u << screen::sportIndex(type)));
}

Rule resolve(const Policy& policy, Phase phase, screen::Type type,
             uint8_t competition_id, uint32_t incarnation) noexcept {
  if (!valid(phase) || !screen::valid(type)) return {};
  Rule rule = policy.shared[index(phase)][screen::index(type)];
  const auto competition =
      associationIndex(policy, competition_id, incarnation);
  if (!expanded(policy, phase, type) || competition == cfg::kCompetitionMax)
    return rule;
  const auto& value =
      policy.overrides[index(phase)][screen::sportIndex(type)][competition];
  if (value.fields & kEnabled) rule.enabled = (value.values & kEnabled) != 0;
  if (value.fields & kAutomatic)
    rule.automatic = (value.values & kAutomatic) != 0;
  if (value.fields & kVisit) rule.visit_s = value.visit_s;
  return rule;
}

SelectionRules selectionRules(const Policy& policy,
                              const Competitions& competitions) noexcept {
  SelectionRules out{policy.own_before_min,
                     policy.own_after_min,
                     policy.other_before_min,
                     policy.other_after_min,
                     0,
                     policy.automatic_break};
  for (std::size_t slot = 0; slot < competitions.entries.size(); ++slot) {
    const auto& c = competitions.entries[slot];
    const auto at = associationIndex(policy, c.id, c.incarnation);
    const bool special = at == policy.competitions.size()
                             ? Association{}.own_special
                             : policy.competitions[at].own_special;
    if (special) out.own_special |= static_cast<uint8_t>(1u << slot);
  }
  return out;
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
        if (screen::sporting(kind) && resolve(policy, static_cast<Phase>(phase),
                                              kind, c.id, c.incarnation)
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
