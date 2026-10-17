#include "screen_json.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string_view>
#include <type_traits>

namespace web {
namespace {
using namespace ArduinoJson;
namespace policy = settings::screen_policy;
using settings::FieldPath;
using settings::Reason;

constexpr std::array<const char*, policy::kPhaseCount> kPhases{
    "own", "other", "matchday", "idle", "break"};
// Stored Type order, not display order.
constexpr std::array<const char*, screen::kTypeCount> kTypes{
    "single", "conference", "table", "crest", "matchday", "images"};
constexpr std::array<screen::Type, screen::kSportTypeCount> kSports{
    screen::Type::kLiveSingle, screen::Type::kLiveMulti, screen::Type::kTable,
    screen::Type::kMatchday};

bool same(JsonString value, std::string_view literal) {
  return !value.isNull() &&
         std::string_view(value.c_str(), value.size()) == literal;
}

struct Context {
  settings::Violation* violation;
  bool reject(const FieldPath& path, Reason reason) const {
    if (violation) violation->set(path, reason);
    return false;
  }
};

bool keys(const Context& c, const FieldPath& path, JsonVariantConst value,
          std::initializer_list<const char*> required,
          std::initializer_list<const char*> optional = {}) {
  if (!value.is<JsonObjectConst>()) return c.reject(path, Reason::kType);
  const auto object = value.as<JsonObjectConst>();
  for (JsonPairConst pair : object) {
    bool known = false;
    for (const auto* name : required) known |= same(pair.key(), name);
    for (const auto* name : optional) known |= same(pair.key(), name);
    if (!known)
      return c.reject(path.with(pair.key().c_str()), Reason::kUnknownKey);
  }
  for (const auto* name : required)
    if (object[name].isUnbound())
      return c.reject(path.with(name), Reason::kRequired);
  return true;
}

template <std::size_t N>
bool namedKeys(const Context& c, const FieldPath& path, JsonVariantConst value,
               const std::array<const char*, N>& names) {
  if (!value.is<JsonObjectConst>()) return c.reject(path, Reason::kType);
  const auto object = value.as<JsonObjectConst>();
  for (JsonPairConst pair : object) {
    bool known = false;
    for (const auto* name : names) known |= same(pair.key(), name);
    if (!known)
      return c.reject(path.with(pair.key().c_str()), Reason::kUnknownKey);
  }
  for (const auto* name : names)
    if (object[name].isUnbound())
      return c.reject(path.with(name), Reason::kRequired);
  return true;
}

template <class T>
bool scalar(const Context& c, const FieldPath& path, JsonVariantConst value,
            T& out) {
  if (!value.is<T>())
    return c.reject(path, !std::is_same_v<T, bool> && value.is<long long>()
                              ? Reason::kRange
                              : Reason::kType);
  out = value.as<T>();
  return true;
}

bool array(const Context& c, const FieldPath& path, JsonVariantConst value,
           std::size_t capacity, bool exact = false) {
  if (!value.is<JsonArrayConst>()) return c.reject(path, Reason::kType);
  if (value.size() > capacity) return c.reject(path, Reason::kTooMany);
  return !exact || value.size() == capacity ||
         c.reject(path, Reason::kRequired);
}

bool type(const Context& c, const FieldPath& path, JsonVariantConst value,
          screen::Type& out, bool sporting_only = false) {
  if (!value.is<const char*>()) return c.reject(path, Reason::kType);
  for (unsigned i = 0; i < kTypes.size(); ++i)
    if (same(value.as<JsonString>(), kTypes[i])) {
      out = static_cast<screen::Type>(i);
      return !sporting_only || screen::sporting(out) ||
             c.reject(path, Reason::kInvalid);
    }
  return c.reject(path, Reason::kInvalid);
}

bool rule(const Context& c, const FieldPath& path, JsonVariantConst value,
          policy::Rule& out) {
  return array(c, path, value, 3, true) &&
         scalar(c, path.withIndex(0), value[0], out.enabled) &&
         scalar(c, path.withIndex(1), value[1], out.automatic) &&
         scalar(c, path.withIndex(2), value[2], out.visit_s);
}

bool overrideRule(const Context& c, const FieldPath& path,
                  JsonVariantConst value, policy::Override& out) {
  if (!array(c, path, value, 3, true)) return false;
  out = {};
  for (unsigned slot = 0; slot < 3; ++slot) {
    const auto field = value[slot];
    if (field.isNull()) continue;
    const auto bit = static_cast<uint8_t>(1u << slot);
    out.fields |= bit;
    if (slot == 2) {
      if (!scalar(c, path.withIndex(slot), field, out.visit_s)) return false;
    } else {
      bool enabled = false;
      if (!scalar(c, path.withIndex(slot), field, enabled)) return false;
      if (enabled) out.values |= bit;
    }
  }
  return true;
}

void tuple(JsonArray out, const policy::Rule& value) {
  out.add(value.enabled);
  out.add(value.automatic);
  out.add(value.visit_s);
}

void tuple(JsonArray out, const policy::Override& value) {
  for (unsigned slot = 0; slot < 3; ++slot) {
    const auto bit = static_cast<uint8_t>(1u << slot);
    if (!(value.fields & bit))
      out.add(nullptr);
    else if (slot == 2)
      out.add(value.visit_s);
    else
      out.add((value.values & bit) != 0);
  }
}

// Keep the independently bounded parsing stages out of the caller's frame.
// Inlining all three stages more than doubles its C6 stack frame.
[[gnu::noinline]] bool phases(const Context& c, JsonVariantConst value,
                              policy::Policy& out) {
  const auto path = FieldPath().key("phases");
  if (!namedKeys(c, path, value, kPhases)) return false;
  for (unsigned p = 0; p < kPhases.size(); ++p) {
    const auto at = path.with(kPhases[p]);
    const auto row = value[kPhases[p]];
    if (!keys(c, at, row,
              {"single", "conference", "table", "crest", "matchday", "images",
               "expanded"}) ||
        !array(c, at.with("expanded"), row["expanded"], kSports.size()))
      return false;
    for (unsigned t = 0; t < kTypes.size(); ++t)
      if (!rule(c, at.with(kTypes[t]), row[kTypes[t]], out.shared[p][t]))
        return false;
    out.expanded[p] = 0;
    unsigned item = 0;
    for (JsonVariantConst entry : row["expanded"].as<JsonArrayConst>()) {
      const auto where = at.with("expanded").withIndex(item++);
      screen::Type kind{};
      if (!type(c, where, entry, kind, true)) return false;
      const auto bit = static_cast<uint8_t>(1u << screen::sportIndex(kind));
      if (out.expanded[p] & bit) return c.reject(where, Reason::kDuplicate);
      out.expanded[p] |= bit;
    }
  }
  return true;
}

[[gnu::noinline]] bool competitions(const Context& c, JsonVariantConst value,
                                    policy::Policy& out) {
  const auto path = FieldPath().key("competitions");
  if (!array(c, path, value, out.competitions.size())) return false;
  // A complete replacement. Rules are indexed by their accompanying identity,
  // never accidentally inherited when the service reuses an id later.
  for (auto& association : out.competitions) association = {};
  for (auto& phase : out.overrides)
    for (auto& sport : phase)
      for (auto& override : sport) override = {};
  unsigned i = 0;
  for (JsonVariantConst row : value.as<JsonArrayConst>()) {
    const auto at = path.withIndex(i);
    auto& association = out.competitions[i];
    if (!keys(c, at, row,
              {"id", "incarnation", "own_special", "own", "other", "matchday",
               "idle", "break"}) ||
        !scalar(c, at.with("id"), row["id"], association.id) ||
        !scalar(c, at.with("incarnation"), row["incarnation"],
                association.incarnation) ||
        !scalar(c, at.with("own_special"), row["own_special"],
                association.own_special))
      return false;
    // An entry in the JSON must name a real association. The model's all-zero
    // unused slots are intentionally omitted from the wire representation.
    if (!association.id) return c.reject(at.with("id"), Reason::kRange);
    if (!association.incarnation)
      return c.reject(at.with("incarnation"), Reason::kRange);
    for (unsigned p = 0; p < kPhases.size(); ++p) {
      const auto where = at.with(kPhases[p]);
      const auto rules = row[kPhases[p]];
      if (!keys(c, where, rules, {"single", "conference", "table", "matchday"}))
        return false;
      for (unsigned t = 0; t < kSports.size(); ++t) {
        const auto* name = kTypes[screen::index(kSports[t])];
        if (!overrideRule(c, where.with(name), rules[name],
                          out.overrides[p][t][i]))
          return false;
      }
    }
    ++i;
  }
  return true;
}

[[gnu::noinline]] bool appearances(const Context& c, JsonVariantConst value,
                                   policy::Policy& out) {
  const auto path = FieldPath().key("appearance");
  if (!namedKeys(c, path, value, kTypes)) return false;
  for (unsigned t = 0; t < kTypes.size(); ++t) {
    const auto at = path.with(kTypes[t]);
    const auto row = value[kTypes[t]];
    auto& target = out.appearance[t];
    if (!keys(c, at, row,
              {"colour", "text", "accent", "text_scale", "image", "background",
               "dots"}))
      return false;
    // One path temporary per statement; a chained expression would retain
    // seven FieldPath temporaries until the end of that whole expression.
#define STYLE_GET(name) \
  if (!scalar(c, at.with(#name), row[#name], target.name)) return false;
    STYLE_GET(colour)
    STYLE_GET(text)
    STYLE_GET(accent)
    STYLE_GET(text_scale)
    STYLE_GET(image)
    STYLE_GET(background)
    STYLE_GET(dots)
#undef STYLE_GET
  }
  return true;
}

FieldPath errorPath(const policy::Error& error) {
  const unsigned p = error.phase;
  const unsigned t = error.type;
  switch (error.field) {
    case policy::Field::kShared:
      return FieldPath()
          .key("phases")
          .key(kPhases[p])
          .key(kTypes[t])
          .index(error.property == policy::Property::kVisit ? 2 : 1);
    case policy::Field::kOverride:
      return FieldPath()
          .key("competitions")
          .index(error.competition)
          .key(kPhases[p])
          .key(kTypes[t])
          .index(error.property == policy::Property::kVisit ? 2 : 0);
    case policy::Field::kExpanded:
      return FieldPath().key("phases").key(kPhases[p]).key("expanded");
    case policy::Field::kCompetition:
      return FieldPath()
          .key("competitions")
          .index(error.competition)
          .key(error.property == policy::Property::kId ? "id" : "incarnation");
    case policy::Field::kOrder: return FieldPath().key("order").index(t);
    case policy::Field::kAppearance: {
      const char* name = "image";
      switch (error.property) {
        case policy::Property::kColour: name = "colour"; break;
        case policy::Property::kText: name = "text"; break;
        case policy::Property::kAccent: name = "accent"; break;
        case policy::Property::kTextScale: name = "text_scale"; break;
        default: break;
      }
      return FieldPath().key("appearance").key(kTypes[t]).key(name);
    }
    case policy::Field::kSlides:
      return FieldPath().key("slides").index(error.competition);
    case policy::Field::kBreakDays: return FieldPath().key("break_days");
    case policy::Field::kOwnBefore: return FieldPath().key("own_before_min");
    case policy::Field::kOwnAfter: return FieldPath().key("own_after_min");
    case policy::Field::kOtherBefore:
      return FieldPath().key("other_before_min");
    case policy::Field::kOtherAfter: return FieldPath().key("other_after_min");
    case policy::Field::kManualReturn:
      return FieldPath().key("manual_return_s");
    case policy::Field::kPerImage: return FieldPath().key("per_image_s");
    case policy::Field::kVisibleMatches:
      return FieldPath().key("visible_matches");
    case policy::Field::kTableWindow: return FieldPath().key("table_window");
    case policy::Field::kConferenceMode:
      return FieldPath().key("conference_mode");
    case policy::Field::kMatchdayMode: return FieldPath().key("matchday_mode");
    default: return {};
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

// These options are required in complete GET/PUT representations; there are no
// separate defaults in the JSON layer.
#define OPTIONS(X)    \
  X(break_days)       \
  X(own_before_min)   \
  X(own_after_min)    \
  X(other_before_min) \
  X(other_after_min)  \
  X(manual_return_s)  \
  X(per_image_s)      \
  X(visible_matches)  \
  X(table_window)     \
  X(backgrounds)      \
  X(pin_own_match)    \
  X(frame)            \
  X(frame_crest)      \
  X(automatic_break)

}  // namespace

void screenPolicyJson(JsonObject out, const policy::Policy& model,
                      uint32_t generation) {
  out["generation"] = generation;
#define PUT(name) out[#name] = model.name;
  OPTIONS(PUT)
#undef PUT
  out["conference_mode"] = static_cast<unsigned>(model.conference_mode);
  out["matchday_mode"] = static_cast<unsigned>(model.matchday_mode);
  auto order = out["order"].to<JsonArray>();
  for (const auto kind : model.order) order.add(kTypes[screen::index(kind)]);
  auto phases = out["phases"].to<JsonObject>();
  for (unsigned p = 0; p < kPhases.size(); ++p) {
    auto phase = phases[kPhases[p]].to<JsonObject>();
    for (unsigned t = 0; t < kTypes.size(); ++t)
      tuple(phase[kTypes[t]].to<JsonArray>(), model.shared[p][t]);
    auto expanded = phase["expanded"].to<JsonArray>();
    for (unsigned t = 0; t < kSports.size(); ++t)
      if (model.expanded[p] & (1u << t))
        expanded.add(kTypes[screen::index(kSports[t])]);
  }
  auto competitions = out["competitions"].to<JsonArray>();
  for (unsigned i = 0; i < model.competitions.size(); ++i) {
    const auto& association = model.competitions[i];
    if (!association.id) continue;
    auto row = competitions.add<JsonObject>();
    row["id"] = association.id;
    row["incarnation"] = association.incarnation;
    row["own_special"] = association.own_special;
    for (unsigned p = 0; p < kPhases.size(); ++p) {
      auto phase = row[kPhases[p]].to<JsonObject>();
      for (unsigned t = 0; t < kSports.size(); ++t)
        tuple(phase[kTypes[screen::index(kSports[t])]].to<JsonArray>(),
              model.overrides[p][t][i]);
    }
  }
  auto appearance = out["appearance"].to<JsonObject>();
  for (unsigned t = 0; t < kTypes.size(); ++t) {
    auto row = appearance[kTypes[t]].to<JsonObject>();
    const auto& style = model.appearance[t];
    row["colour"] = style.colour;
    row["text"] = style.text;
    row["accent"] = style.accent;
    row["text_scale"] = style.text_scale;
    row["image"] = style.image;
    row["background"] = style.background;
    row["dots"] = style.dots;
  }
  auto slides = out["slides"].to<JsonArray>();
  for (unsigned i = 0; i < model.slide_count; ++i) slides.add(model.slides[i]);
}

bool applyScreenPolicyJson(JsonObjectConst in, policy::Policy& out,
                           uint32_t& generation, settings::Violation* violation,
                           const policy::Limits& limits) {
  const Context c{violation};
  generation = 0;
#define NAME(name) #name,
  if (!keys(c, FieldPath(), in,
            {OPTIONS(NAME) "conference_mode", "matchday_mode", "order",
             "phases", "competitions", "appearance", "slides", "generation"}))
    return false;
#undef NAME
  if (!scalar(c, FieldPath().key("generation"), in["generation"], generation))
    return false;
#define GET(name) \
  if (!scalar(c, FieldPath().key(#name), in[#name], out.name)) return false;
  OPTIONS(GET)
#undef GET
  uint8_t mode = 0;
  if (!scalar(c, FieldPath().key("conference_mode"), in["conference_mode"],
              mode))
    return false;
  out.conference_mode = static_cast<cfg::ConferenceMode>(mode);
  if (!scalar(c, FieldPath().key("matchday_mode"), in["matchday_mode"], mode))
    return false;
  out.matchday_mode = static_cast<cfg::MatchdayMode>(mode);
  const auto order_path = FieldPath().key("order");
  if (!array(c, order_path, in["order"], out.order.size(), true)) return false;
  for (unsigned i = 0; i < out.order.size(); ++i)
    if (!type(c, order_path.withIndex(i), in["order"][i], out.order[i]))
      return false;
  if (!phases(c, in["phases"], out) ||
      !competitions(c, in["competitions"], out) ||
      !appearances(c, in["appearance"], out))
    return false;
  const auto slides_path = FieldPath().key("slides");
  if (!array(c, slides_path, in["slides"], out.slides.size())) return false;
  out.slides = {};
  out.slide_count = 0;
  for (JsonVariantConst item : in["slides"].as<JsonArrayConst>()) {
    if (!scalar(c, slides_path.withIndex(out.slide_count), item,
                out.slides[out.slide_count]))
      return false;
    ++out.slide_count;
  }
  policy::Error error;
  if (policy::validate(out, &error, limits)) return true;
  if (violation) screenPolicyViolation(error, *violation);
  return false;
}

void screenPolicyViolation(const policy::Error& error,
                           settings::Violation& violation) {
  violation.set(errorPath(error), errorReason(error.reason));
}

void screenPolicySchemaJson(JsonObject out, const policy::Limits& limits) {
  out["version"] = cfg::kScreenPolicyFormatVersion;
  out["max_body"] = cfg::kApiBodyBytes;
  auto phases = out["phases"].to<JsonArray>();
  for (const auto* name : kPhases) phases.add(name);
  auto types = out["types"].to<JsonArray>();
  for (const auto* name : kTypes) types.add(name);
  auto sports = out["sports"].to<JsonArray>();
  for (const auto kind : kSports) sports.add(kTypes[screen::index(kind)]);
  auto fields = out["rule_fields"].to<JsonArray>();
  for (const auto* name : {"enabled", "automatic", "visit_s"}) fields.add(name);
  out["override_null"] = "inherit_shared_field";
  out["complete_put"] = true;
  out["generation_required"] = true;
  out["max_competitions"] = cfg::kCompetitionMax;
  out["max_slides"] = cfg::kSlideMax;
  auto bounds = out["bounds"].to<JsonObject>();
  auto range = [&](const char* name, uint16_t low, uint16_t high) {
    auto values = bounds[name].to<JsonArray>();
    values.add(low);
    values.add(high);
  };
  auto bounded = [&](const char* name, cfg::Range value) {
    range(name, value.min, value.max);
  };
  bounded("visit_s", cfg::kScreenVisitRange);
  bounded("break_days", cfg::kScreenBreakDaysRange);
  bounded("buffer_min", cfg::kMatchWindowRange);
  bounded("manual_return_s", cfg::kReturnTimeRange);
  bounded("per_image_s", cfg::kSlideshowRange);
  bounded("text_scale", cfg::kTextScaleRange);
  range("visible_matches", cfg::kVisibleMatchesRange.min,
        std::min<uint16_t>(cfg::kVisibleMatchesRange.max,
                           limits.visible_matches_max));
  range(
      "table_window", cfg::kTableWindowRange.min,
      std::min<uint16_t>(cfg::kTableWindowRange.max, limits.table_window_max));
  range("image", 0, cfg::kImageLibraryMax);
  range("conference_mode", 0,
        static_cast<uint8_t>(cfg::ConferenceMode::kOnePage));
  range("matchday_mode", 0, static_cast<uint8_t>(cfg::MatchdayMode::kNext));
}

#undef OPTIONS
}  // namespace web
