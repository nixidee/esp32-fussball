#include "screen_json.h"

#include <algorithm>
#include <array>
#include <type_traits>

#include "typed_json.h"

namespace web {
namespace {
namespace policy = settings::screen_policy;
using detail::Decoder;
using json::Kind;
using settings::FieldPath;
using settings::Reason;
constexpr const char* kPhases[] = {"own", "other", "matchday", "idle", "break"};
constexpr const char* kTypes[] = {"single", "conference", "table",
                                  "crest",  "matchday",   "images"};
constexpr const char* kSports[] = {"single", "conference", "table", "matchday"};
template <std::size_t N>
constexpr uint64_t all(const char* const (&)[N]) {
  return (uint64_t{1} << N) - 1;
}

template <class T>
bool scalar(Decoder& d, T& out, const FieldPath& path) {
  return d.scalar(out, path, false);
}
bool type(Decoder& d, screen::Type& out, const FieldPath& path) {
  if (d.token.kind == Kind::kNull) return d.reject(path, Reason::kRequired);
  if (d.token.kind != Kind::kString) return d.reject(path, Reason::kType);
  for (unsigned i = 0; i < std::size(kTypes); ++i)
    if (!d.token.truncated && d.token.text == kTypes[i]) {
      out = static_cast<screen::Type>(i);
      return true;
    }
  return d.reject(path, Reason::kInvalid);
}
bool tuple(Decoder& d, policy::Rule& out, const FieldPath& path) {
  return d.array(
      d.token, path, 3,
      [&](unsigned field, const FieldPath& at) {
        if (d.token.kind == Kind::kNull) return d.reject(at, Reason::kType);
        if (field == 0) return scalar(d, out.enabled, at);
        if (field == 1) return scalar(d, out.automatic, at);
        return scalar(d, out.visit_s, at);
      },
      true);
}
bool tuple(Decoder& d, policy::Override& out, const FieldPath& path) {
  out = {};
  return d.array(
      d.token, path, 3,
      [&](unsigned field, const FieldPath& at) {
        if (d.token.kind == Kind::kNull) return true;
        const auto bit = static_cast<uint8_t>(1u << field);
        out.fields |= bit;
        if (field == 2) return scalar(d, out.visit_s, at);
        bool value = false;
        if (!scalar(d, value, at)) return false;
        if (value) out.values |= bit;
        return true;
      },
      true);
}
bool tuple(json::Writer& out, const policy::Rule& value) {
  return out.array() && out.boolean(value.enabled) &&
         out.boolean(value.automatic) && out.integer(value.visit_s) &&
         out.end();
}
bool tuple(json::Writer& out, const policy::Override& value) {
  if (!out.array()) return false;
  for (unsigned field = 0; field < 3; ++field) {
    const auto bit = static_cast<uint8_t>(1u << field);
    if (!(value.fields & bit)) {
      if (!out.null()) return false;
    } else if (field == 2) {
      if (!out.integer(value.visit_s)) return false;
    } else if (!out.boolean((value.values & bit) != 0))
      return false;
  }
  return out.end();
}
template <class T>
bool field(json::Writer& out, const char* key, T value) {
  if (!out.key(key)) return false;
  if constexpr (std::is_same_v<T, bool>)
    return out.boolean(value);
  else
    return out.integer(static_cast<uint64_t>(value));
}

// Keep this declarative field list one entry per line.
// clang-format off
#define OPTIONS(X) \
  X(break_days) \
  X(own_before_min) \
  X(own_after_min) \
  X(other_before_min) \
  X(other_after_min) \
  X(manual_return_s) \
  X(per_image_s) \
  X(visible_matches) \
  X(table_window) \
  X(backgrounds) \
  X(pin_own_match) \
  X(frame) \
  X(frame_crest) \
  X(automatic_break) \
  X(only_one_own_single)
// clang-format on

bool phases(Decoder& d, policy::Policy& out, const FieldPath& path) {
  return d.object(
      d.token, path, kPhases,
      [&](unsigned phase, const FieldPath& at) {
        const char* keys[] = {"single",         "conference", "table",
                              "crest",          "matchday",   "images",
                              "per_competition"};
        return d.object(
            d.token, at, keys,
            [&](unsigned field, const FieldPath& where) {
              if (field < std::size(kTypes))
                return tuple(d, out.shared[phase][field], where);
              out.per_competition[phase] = 0;
              return d.array(d.token, where, screen::kSportTypeCount,
                             [&](unsigned, const FieldPath& item) {
                               screen::Type kind{};
                               if (!type(d, kind, item)) return false;
                               if (!screen::sporting(kind))
                                 return d.reject(item, Reason::kInvalid);
                               const auto bit = static_cast<uint8_t>(
                                   1u << screen::sportIndex(kind));
                               if (out.per_competition[phase] & bit)
                                 return d.reject(item, Reason::kDuplicate);
                               out.per_competition[phase] |= bit;
                               return true;
                             });
            },
            all(keys));
      },
      all(kPhases));
}
bool appearance(Decoder& d, policy::Policy& out, const FieldPath& path) {
  return d.object(
      d.token, path, kTypes,
      [&](unsigned type, const FieldPath& at) {
        auto& value = out.appearance[type];
        const char* keys[] = {"colour", "text",       "accent", "text_scale",
                              "image",  "background", "dots"};
        return d.object(
            d.token, at, keys,
            [&](unsigned field, const FieldPath& where) {
              switch (field) {
                case 0: return scalar(d, value.colour, where);
                case 1: return scalar(d, value.text, where);
                case 2: return scalar(d, value.accent, where);
                case 3: return scalar(d, value.text_scale, where);
                case 4: return scalar(d, value.image, where);
                case 5: return scalar(d, value.background, where);
                default: return scalar(d, value.dots, where);
              }
            },
            all(keys));
      },
      all(kTypes));
}
FieldPath errorPath(const policy::Error& error) {
  auto path = FieldPath();
  switch (error.field) {
    case policy::Field::kShared:
      path.key("phases").key(kPhases[error.phase]).key(kTypes[error.type]);
      if (error.property == policy::Property::kVisit)
        path.index(2);
      else if (error.reason == policy::Reason::kImageAutomatic)
        path.index(1);
      return path;
    case policy::Field::kOverride:
      path.key("competitions")
          .index(error.competition)
          .key("display")
          .key(kPhases[error.phase])
          .key(kSports[error.type]);
      if (error.property == policy::Property::kVisit) path.index(2);
      return path;
    case policy::Field::kPerCompetition:
      return path.key("phases")
          .key(kPhases[error.phase])
          .key("per_competition");
    case policy::Field::kOrder: return path.key("order").index(error.type);
    case policy::Field::kSlides:
      return path.key("slides").index(error.competition);
    case policy::Field::kAppearance:
      path.key("appearance").key(kTypes[error.type]);
      switch (error.property) {
        case policy::Property::kColour: return path.key("colour");
        case policy::Property::kText: return path.key("text");
        case policy::Property::kAccent: return path.key("accent");
        case policy::Property::kTextScale: return path.key("text_scale");
        case policy::Property::kImage: return path.key("image");
        default: return path;
      }
    case policy::Field::kBreakDays: return path.key("break_days");
    case policy::Field::kOwnBefore: return path.key("own_before_min");
    case policy::Field::kOwnAfter: return path.key("own_after_min");
    case policy::Field::kOtherBefore: return path.key("other_before_min");
    case policy::Field::kOtherAfter: return path.key("other_after_min");
    case policy::Field::kManualReturn: return path.key("manual_return_s");
    case policy::Field::kPerImage: return path.key("per_image_s");
    case policy::Field::kVisibleMatches: return path.key("visible_matches");
    case policy::Field::kTableWindow: return path.key("table_window");
    case policy::Field::kConferenceMode: return path.key("conference_mode");
    case policy::Field::kMatchdayMode: return path.key("matchday_mode");
    default: return path;
  }
}
Reason errorReason(policy::Reason reason) {
  switch (reason) {
    case policy::Reason::kRange: return Reason::kRange;
    case policy::Reason::kDuplicate: return Reason::kDuplicate;
    case policy::Reason::kRequired: return Reason::kRequired;
    case policy::Reason::kImageAutomatic:
    case policy::Reason::kImageMultiple:
    case policy::Reason::kUnusedSlot: return Reason::kConflict;
    default: return Reason::kInvalid;
  }
}
}  // namespace

SettingsDecode applyScreenPolicyObject(json::Reader& reader,
                                       const json::Token& token,
                                       policy::Policy& out,
                                       settings::Violation* violation,
                                       const policy::Limits& limits,
                                       const FieldPath& path) {
  Decoder d(reader, violation);
#define NAME(name) #name,
  const char* keys[] = {OPTIONS(NAME) "conference_mode",
                        "matchday_mode",
                        "order",
                        "phases",
                        "appearance",
                        "slides"};
#undef NAME
  if (!d.object(
          token, path, keys,
          [&](unsigned field, const FieldPath& at) {
            unsigned index = 0;
#define GET(name) \
  if (field == index++) return scalar(d, out.name, at);
            OPTIONS(GET)
#undef GET
            if (field == index++) {
              uint8_t mode = 0;
              if (!scalar(d, mode, at)) return false;
              out.conference_mode = static_cast<cfg::ConferenceMode>(mode);
              return true;
            }
            if (field == index++) {
              uint8_t mode = 0;
              if (!scalar(d, mode, at)) return false;
              out.matchday_mode = static_cast<cfg::MatchdayMode>(mode);
              return true;
            }
            if (field == index++)
              return d.array(
                  d.token, at, out.order.size(),
                  [&](unsigned i, const FieldPath& where) {
                    return type(d, out.order[i], where);
                  },
                  true);
            if (field == index++) return phases(d, out, at);
            if (field == index++) return appearance(d, out, at);
            out.slides = {};
            out.slide_count = 0;
            return d.array(d.token, at, out.slides.size(),
                           [&](unsigned i, const FieldPath& where) {
                             if (!scalar(d, out.slides[i], where)) return false;
                             ++out.slide_count;
                             return true;
                           });
          },
          all(keys)))
    return d.result;
  policy::Error error;
  if (!policy::validate(out, &error, limits)) {
    settings::Violation invalid;
    screenPolicyViolation(error, invalid);
    d.reject(path.with(invalid.path.data()), invalid.reason);
  }
  return d.result;
}
SettingsDecode applyCompetitionPreferencesObject(
    json::Reader& reader, const json::Token& token,
    policy::CompetitionPreferences& out, settings::Violation* violation,
    const FieldPath& path) {
  Decoder d(reader, violation);
  if (!d.object(
          token, path, kPhases,
          [&](unsigned phase, const FieldPath& at) {
            return d.object(
                d.token, at, kSports,
                [&](unsigned type, const FieldPath& where) {
                  return tuple(d, out.overrides[phase][type], where);
                },
                all(kSports));
          },
          all(kPhases)))
    return d.result;
  policy::Error error;
  if (!policy::validate(out, &error)) {
    auto at = path.with(kPhases[error.phase]).with(kSports[error.type]);
    if (error.property == policy::Property::kVisit) at.index(2);
    d.reject(at, errorReason(error.reason));
  }
  return d.result;
}
SettingsDecode applyScreenPolicyJson(json::Input input, policy::Policy& out,
                                     settings::Violation* violation,
                                     const policy::Limits& limits) {
  json::Reader reader(input);
  json::Token token;
  if (!reader.next(token)) return SettingsDecode::kSyntax;
  const auto result =
      applyScreenPolicyObject(reader, token, out, violation, limits);
  if (result != SettingsDecode::kOk) return result;
  return reader.finish() ? SettingsDecode::kOk : SettingsDecode::kSyntax;
}
void screenPolicyViolation(const policy::Error& error,
                           settings::Violation& violation) {
  violation.set(errorPath(error), errorReason(error.reason));
}
bool competitionPreferencesJson(
    json::Writer& out, const policy::CompetitionPreferences& preferences) {
  for (unsigned phase = 0; phase < std::size(kPhases); ++phase) {
    if (!out.key(kPhases[phase]) || !out.object()) return false;
    for (unsigned type = 0; type < std::size(kSports); ++type)
      if (!out.key(kSports[type]) ||
          !tuple(out, preferences.overrides[phase][type]))
        return false;
    if (!out.end()) return false;
  }
  return true;
}

bool screenPolicyJson(json::Writer& out, const policy::Policy& model) {
#define PUT(name) \
  if (!field(out, #name, model.name)) return false;
  OPTIONS(PUT)
#undef PUT
  if (!field(out, "conference_mode", model.conference_mode) ||
      !field(out, "matchday_mode", model.matchday_mode) || !out.key("order") ||
      !out.array())
    return false;
  for (const auto kind : model.order)
    if (!out.string(kTypes[screen::index(kind)])) return false;
  if (!out.end() || !out.key("phases") || !out.object()) return false;
  for (unsigned p = 0; p < std::size(kPhases); ++p) {
    if (!out.key(kPhases[p]) || !out.object()) return false;
    for (unsigned t = 0; t < std::size(kTypes); ++t)
      if (!out.key(kTypes[t]) || !tuple(out, model.shared[p][t])) return false;
    if (!out.key("per_competition") || !out.array()) return false;
    for (unsigned t = 0; t < std::size(kSports); ++t)
      if ((model.per_competition[p] & (1u << t)) && !out.string(kSports[t]))
        return false;
    if (!out.end() || !out.end()) return false;
  }
  if (!out.end() || !out.key("appearance") || !out.object()) return false;
  for (unsigned t = 0; t < std::size(kTypes); ++t) {
    const auto& style = model.appearance[t];
    if (!out.key(kTypes[t]) || !out.object() ||
        !field(out, "colour", style.colour) ||
        !field(out, "text", style.text) ||
        !field(out, "accent", style.accent) ||
        !field(out, "text_scale", style.text_scale) ||
        !field(out, "image", style.image) ||
        !field(out, "background", style.background) ||
        !field(out, "dots", style.dots) || !out.end())
      return false;
  }
  if (!out.end() || !out.key("slides") || !out.array()) return false;
  for (unsigned i = 0; i < model.slide_count; ++i)
    if (!out.integer(model.slides[i])) return false;
  return out.end();
}

bool screenPolicySchemaJson(json::Writer& out, const policy::Limits& limits) {
  if (!field(out, "version", cfg::kAppConfigFormatVersion) ||
      !field(out, "max_body", cfg::kApiBodyBytes) || !out.key("phases") ||
      !out.array())
    return false;
  for (const auto* name : kPhases)
    if (!out.string(name)) return false;
  if (!out.end() || !out.key("types") || !out.array()) return false;
  for (const auto* name : kTypes)
    if (!out.string(name)) return false;
  if (!out.end() || !out.key("sports") || !out.array()) return false;
  for (const auto* name : kSports)
    if (!out.string(name)) return false;
  if (!out.end() || !out.key("rule_fields") || !out.array()) return false;
  for (const auto* name : {"enabled", "automatic", "visit_s"})
    if (!out.string(name)) return false;
  if (!out.end() || !out.key("override_null") ||
      !out.string("inherit_shared_field") ||
      !field(out, "complete_put", true) ||
      !field(out, "max_competitions", cfg::kCompetitionMax) ||
      !field(out, "max_slides", cfg::kSlideMax) || !out.key("bounds") ||
      !out.object())
    return false;
  auto range = [&](const char* name, uint16_t low, uint16_t high) {
    return out.key(name) && out.array() && out.integer(low) &&
           out.integer(high) && out.end();
  };
  auto bounded = [&](const char* name, cfg::Range value) {
    return range(name, value.min, value.max);
  };
  return bounded("visit_s", cfg::kScreenVisitRange) &&
         bounded("break_days", cfg::kScreenBreakDaysRange) &&
         bounded("buffer_min", cfg::kMatchWindowRange) &&
         bounded("manual_return_s", cfg::kReturnTimeRange) &&
         bounded("per_image_s", cfg::kSlideshowRange) &&
         bounded("text_scale", cfg::kTextScaleRange) &&
         range("visible_matches", cfg::kVisibleMatchesRange.min,
               std::min<uint16_t>(cfg::kVisibleMatchesRange.max,
                                  limits.visible_matches_max)) &&
         range("table_window", cfg::kTableWindowRange.min,
               std::min<uint16_t>(cfg::kTableWindowRange.max,
                                  limits.table_window_max)) &&
         range("image", 0, cfg::kImageLibraryMax) &&
         range("conference_mode", 0,
               static_cast<uint8_t>(cfg::ConferenceMode::kOnePage)) &&
         range("matchday_mode", 0,
               static_cast<uint8_t>(cfg::MatchdayMode::kNext)) &&
         out.end();
}

#undef OPTIONS
}  // namespace web
