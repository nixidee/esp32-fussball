#include "settings_json.h"

#include <cstring>
#include <type_traits>

#include "hw_target.h"
#include "provider_client.h"
namespace web {
using namespace ArduinoJson;
#define SCALARS(X)            \
  X(external_antenna);        \
  X(debug_status_log);        \
  X(debug_status_interval_s); \
  X(espn_opt_in);             \
  X(demo);                    \
  X(backgrounds);             \
  X(night_enabled);           \
  X(ip_badge_ap_permanent);   \
  X(ip_badge_bottom);         \
  X(browser_debug);           \
  X(pin_own_match);           \
  X(whole_matchday);          \
  X(language);                \
  X(brightness);              \
  X(night_brightness);        \
  X(rotation);                \
  X(visible_matches);         \
  X(table_window);            \
  X(manual_return_s);         \
  X(scroll_reset_s);          \
  X(scroll_repeat_ms);        \
  X(slideshow_s);             \
  X(night_start);             \
  X(night_end);               \
  X(ip_badge_s);              \
  X(window_before);           \
  X(window_after);            \
  X(api_daily_budget);
#define TEXTS(X) \
  X(wifi_ssid);  \
  X(hostname);   \
  X(time_zone);  \
  X(ntp_server);
#define SECRETS(X)     \
  X(wifi_password);    \
  X(ap_password);      \
  X(admin_password);   \
  X(api_football_key); \
  X(football_data_key);
using settings::FieldPath;
using settings::Reason;
// Where the first rejected value is recorded (D7); null: only the verdict.
struct Context {
  settings::Violation* violation;
  bool reject(const FieldPath& at, Reason why) const {
    if (violation) violation->set(at, why);
    return false;
  }
};
// A number that does not fit the target type is out of range, any other
// mismatch is a wrong type.
template <class T>
bool scalar(const Context& c, const FieldPath& at, JsonVariantConst v,
            T& target) {
  if (v.isNull()) return true;
  if (!v.is<T>())
    return c.reject(at, !std::is_same_v<T, bool> && v.is<long long>()
                            ? Reason::kRange
                            : Reason::kType);
  target = v.as<T>();
  return true;
}
template <std::size_t N>
bool bytes(const Context& c, const FieldPath& at, JsonVariantConst v,
           settings::Bytes<N>& target) {
  if (v.isNull()) return true;
  if (!v.is<const char*>()) return c.reject(at, Reason::kType);
  auto s = v.as<JsonString>();
  return target.assign({s.c_str(), s.size()}) || c.reject(at, Reason::kTooLong);
}
// True if 'row' is an object whose keys are all in 'names'.
bool onlyKeys(const Context& c, const FieldPath& at, JsonVariantConst row,
              std::initializer_list<const char*> names) {
  if (!row.is<JsonObjectConst>()) return c.reject(at, Reason::kType);
  for (JsonPairConst entry : row.as<JsonObjectConst>()) {
    bool known = false;
    for (const char* name : names) known |= !strcmp(entry.key().c_str(), name);
    if (!known)
      return c.reject(at.with(entry.key().c_str()), Reason::kUnknownKey);
  }
  return true;
}
// A list in the input: an array of at most 'capacity' entries.
bool listOf(const Context& c, const FieldPath& at, JsonVariantConst rows,
            std::size_t capacity) {
  if (!rows.is<JsonArrayConst>()) return c.reject(at, Reason::kType);
  return rows.size() <= capacity || c.reject(at, Reason::kTooMany);
}
void sourceJson(JsonObject out, const settings::Source& source) {
  out["provider"] = static_cast<unsigned>(source.provider);
  out["competition"] = std::string_view(source.competition.view());
  out["season"] = std::string_view(source.season.view());
  out["team"] = std::string_view(source.team.view());
}
// Absent keys keep the value in 'target'; null is absent.
bool applySource(const Context& c, const FieldPath& at, JsonVariantConst row,
                 settings::Source& target) {
  if (row.isNull()) return true;
  uint8_t provider = static_cast<uint8_t>(target.provider);
  if (!onlyKeys(c, at, row, {"provider", "competition", "season", "team"}) ||
      !scalar(c, at.with("provider"), row["provider"], provider) ||
      !bytes(c, at.with("competition"), row["competition"],
             target.competition) ||
      !bytes(c, at.with("season"), row["season"], target.season) ||
      !bytes(c, at.with("team"), row["team"], target.team))
    return false;
  target.provider = static_cast<cfg::Provider>(provider);
  return true;
}
void settingsJson(JsonObject out, const settings::Model& m, bool secrets) {
#define PUT(name) out[#name] = m.name;
  SCALARS(PUT)
#undef PUT
#define PUTTEXT(name) out[#name] = std::string_view(m.name.view());
  TEXTS(PUTTEXT)
#undef PUTTEXT
#define PUTSECRET(name)                \
  out[#name "_set"] = !m.name.empty(); \
  if (secrets) out[#name] = std::string_view(m.name.view());
  SECRETS(PUTSECRET)
#undef PUTSECRET
  out["idle_screen"] = static_cast<unsigned>(m.idle_screen);
  out["matchday_screen"] = static_cast<unsigned>(m.matchday_screen);
  out["own_match_screen"] = static_cast<unsigned>(m.own_match_screen);
  out["conference_mode"] = static_cast<unsigned>(m.conference_mode);
  // Every stored slot; the schema's "inputs" says how many the device has.
  auto roles = out["input_roles"].to<JsonArray>();
  for (const auto role : m.input_roles) roles.add(static_cast<unsigned>(role));
  auto competitions = out["competitions"].to<JsonArray>();
  for (const auto& entry : m.competitions) {
    auto item = competitions.add<JsonObject>();
    item["active"] = entry.active;
    item["fallback_active"] = entry.fallback_active;
    sourceJson(item["primary"].to<JsonObject>(), entry.primary);
    sourceJson(item["fallback"].to<JsonObject>(), entry.fallback);
    item["competition_name"] = std::string_view(entry.competition_name.view());
    item["team_name"] = std::string_view(entry.team_name.view());
  }
  auto pairs = out["fixture_pairs"].to<JsonArray>();
  for (const auto& pair : m.fixture_pairs) {
    auto item = pairs.add<JsonObject>();
    item["active"] = pair.active;
    item["swapped"] = pair.swapped;
    item["competition"] = pair.competition;
    item["primary_id"] = std::string_view(pair.primary_id.view());
    item["secondary_id"] = std::string_view(pair.secondary_id.view());
  }
  auto slides = out["slides"].to<JsonArray>();
  for (const uint8_t id : m.slides) slides.add(id);
  auto screens = out["screens"].to<JsonArray>();
  for (const auto& style : m.screens) {
    auto item = screens.add<JsonObject>();
    item["enabled"] = style.enabled;
    item["background"] = style.background;
    item["colour"] = style.colour;
    item["text"] = style.text;
    item["accent"] = style.accent;
    item["text_scale"] = style.text_scale;
    item["image"] = style.image;
  }
}
bool applyJson(JsonObjectConst in, settings::Model& m,
               settings::Violation* violation,
               const settings::Limits& limits) {
  const Context c{violation};
  for (JsonPairConst entry : in) {
    const auto* name = entry.key().c_str();
    bool known = false;
#define KNOWN(member) known |= strcmp(name, #member) == 0;
    SCALARS(KNOWN)
    TEXTS(KNOWN)
    SECRETS(KNOWN)
#undef KNOWN
    for (const char* other :
         {"competitions", "fixture_pairs", "slides", "screens",
          "idle_screen", "matchday_screen", "own_match_screen",
          "conference_mode", "input_roles"})
      known |= strcmp(name, other) == 0;
    // Redacted exports carry only these boolean markers; they never clear a
    // key.
    bool marker = false;
#define MARKER(member) marker |= strcmp(name, #member "_set") == 0;
    SECRETS(MARKER)
#undef MARKER
    if (marker && entry.value().is<bool>()) continue;
    if (!known) return c.reject(FieldPath().key(name), Reason::kUnknownKey);
  }
#define GET(name) \
  if (!scalar(c, FieldPath().key(#name), in[#name], m.name)) return false;
  SCALARS(GET)
#undef GET
#define GETTEXT(name) \
  if (!bytes(c, FieldPath().key(#name), in[#name], m.name)) return false;
  TEXTS(GETTEXT)
  SECRETS(GETTEXT)
#undef GETTEXT
  for (auto pair : {std::pair{"idle_screen", &m.idle_screen},
                    std::pair{"matchday_screen", &m.matchday_screen},
                    std::pair{"own_match_screen", &m.own_match_screen}}) {
    if (in[pair.first].isNull()) continue;
    if (!in[pair.first].is<uint8_t>())
      return c.reject(FieldPath().key(pair.first),
                      in[pair.first].is<long long>() ? Reason::kRange
                                                     : Reason::kType);
    *pair.second = static_cast<cfg::Screen>(in[pair.first].as<uint8_t>());
  }
  uint8_t mode = static_cast<uint8_t>(m.conference_mode);
  if (!scalar(c, FieldPath().key("conference_mode"), in["conference_mode"],
              mode))
    return false;
  m.conference_mode = static_cast<cfg::ConferenceMode>(mode);
  // A role list replaces the first slots; the slots after it keep theirs.
  if (!in["input_roles"].isNull()) {
    const auto list = FieldPath().key("input_roles");
    const auto rows = in["input_roles"];
    if (!listOf(c, list, rows, m.input_roles.size())) return false;
    unsigned n = 0;
    for (JsonVariantConst row : rows.as<JsonArrayConst>()) {
      if (!row.is<uint8_t>())
        return c.reject(list.withIndex(n), row.is<long long>() ? Reason::kRange
                                                               : Reason::kType);
      m.input_roles[n++] = static_cast<cfg::InputRole>(row.as<uint8_t>());
    }
  }
  // A list in the input replaces the whole list; absent keys of an entry
  // take the defaults of a new entry.
  if (!in["competitions"].isNull()) {
    const auto list = FieldPath().key("competitions");
    const auto rows = in["competitions"];
    if (!listOf(c, list, rows, m.competitions.kCapacity)) return false;
    m.competitions.clear();
    unsigned n = 0;
    for (JsonVariantConst row : rows.as<JsonArrayConst>()) {
      const auto at = list.withIndex(n++);
      settings::Competition entry;
      if (!onlyKeys(c, at, row,
                    {"active", "fallback_active", "primary", "fallback",
                     "competition_name", "team_name"}) ||
          !scalar(c, at.with("active"), row["active"], entry.active) ||
          !scalar(c, at.with("fallback_active"), row["fallback_active"],
                  entry.fallback_active) ||
          !applySource(c, at.with("primary"), row["primary"], entry.primary) ||
          !applySource(c, at.with("fallback"), row["fallback"],
                       entry.fallback) ||
          !bytes(c, at.with("competition_name"), row["competition_name"],
                 entry.competition_name) ||
          !bytes(c, at.with("team_name"), row["team_name"], entry.team_name))
        return false;
      m.competitions.push(entry);
    }
  }
  if (!in["fixture_pairs"].isNull()) {
    const auto list = FieldPath().key("fixture_pairs");
    const auto rows = in["fixture_pairs"];
    if (!listOf(c, list, rows, m.fixture_pairs.kCapacity)) return false;
    m.fixture_pairs.clear();
    unsigned n = 0;
    for (JsonVariantConst row : rows.as<JsonArrayConst>()) {
      const auto at = list.withIndex(n++);
      settings::FixturePair pair;
      if (!onlyKeys(c, at, row,
                    {"active", "swapped", "competition", "primary_id",
                     "secondary_id"}) ||
          !scalar(c, at.with("active"), row["active"], pair.active) ||
          !scalar(c, at.with("swapped"), row["swapped"], pair.swapped) ||
          !scalar(c, at.with("competition"), row["competition"],
                  pair.competition) ||
          !bytes(c, at.with("primary_id"), row["primary_id"],
                 pair.primary_id) ||
          !bytes(c, at.with("secondary_id"), row["secondary_id"],
                 pair.secondary_id))
        return false;
      m.fixture_pairs.push(pair);
    }
  }
  if (!in["slides"].isNull()) {
    const auto list = FieldPath().key("slides");
    const auto rows = in["slides"];
    if (!listOf(c, list, rows, m.slides.kCapacity)) return false;
    m.slides.clear();
    unsigned n = 0;
    for (JsonVariantConst row : rows.as<JsonArrayConst>()) {
      const auto at = list.withIndex(n++);
      if (!row.is<uint8_t>())
        return c.reject(at, row.is<long long>() ? Reason::kRange
                                                : Reason::kType);
      m.slides.push(row.as<uint8_t>());
    }
  }
  if (!in["screens"].isNull()) {
    const auto list = FieldPath().key("screens");
    if (!in["screens"].is<JsonArrayConst>())
      return c.reject(list, Reason::kType);
    if (in["screens"].size() != m.screens.size())
      return c.reject(list, in["screens"].size() > m.screens.size()
                                ? Reason::kTooMany
                                : Reason::kInvalid);
    for (unsigned i = 0; i < m.screens.size(); ++i) {
      const auto at = list.withIndex(i);
      auto row = in["screens"][i];
      auto& s = m.screens[i];
      if (!onlyKeys(c, at, row,
                    {"enabled", "background", "colour", "text", "accent",
                     "text_scale", "image"}) ||
          !scalar(c, at.with("enabled"), row["enabled"], s.enabled) ||
          !scalar(c, at.with("background"), row["background"], s.background) ||
          !scalar(c, at.with("colour"), row["colour"], s.colour) ||
          !scalar(c, at.with("text"), row["text"], s.text) ||
          !scalar(c, at.with("accent"), row["accent"], s.accent) ||
          !scalar(c, at.with("text_scale"), row["text_scale"], s.text_scale) ||
          !scalar(c, at.with("image"), row["image"], s.image))
        return false;
    }
  }
  return settings::validate(m, nullptr, violation, limits) &&
         settings::checkProviderAccess(m, violation);
}
void schemaJson(JsonObject out, const settings::Limits& limits) {
  out["version"] = cfg::kSettingsFormatVersion;
  out["max_body"] = cfg::kApiBodyBytes;
  auto lists = out["limits"].to<JsonObject>();
  lists["competitions"] = cfg::kCompetitionMax;
  lists["fixture_pairs"] = cfg::kFixtureMappingMax;
  lists["slides"] = cfg::kSlideMax;
  lists["images"] = cfg::kImageLibraryMax;
  lists["screens"] = cfg::kScreenCount;
  // Inputs of this device (the first entries of "input_roles").
  out["inputs"] = limits.inputs;
  out["backlight"] = hw::kTarget.wiring.backlight != hw::kNoPin;
  out["image_max"] = cfg::kImageMaxBytes;
  out["selection_entries"] = cfg::kSelectionEntries;
  out["selection_country_default"] = cfg::kSelectionCountryDefault;
  auto scale = out["text_scale"].to<JsonArray>();
  scale.add(cfg::kTextScaleRange.min);
  scale.add(cfg::kTextScaleRange.max);
  out["image_quota"] = cfg::kImageTotalBytes;
  out["image_reserve"] = cfg::kImageReserveBytes;
  auto zones = out["time_zones"].to<JsonArray>();
  for (auto zone : cfg::kTimeZones) zones.add(zone.label);
  auto providers = out["providers"].to<JsonArray>();
  for (unsigned p = 0; p < cfg::kProviderCount; ++p)
    providers.add(football::providerName(static_cast<cfg::Provider>(p)));
  auto defaults = out["defaults"].to<JsonObject>();
  settingsJson(defaults, settings::initialValues({}, nullptr, limits));
  // Longest accepted text in bytes; list entries are nested like in the
  // settings JSON (W127).
  using settings::Model;
  auto text = out["text"].to<JsonObject>();
#define TEXTMAX(name) text[#name] = decltype(Model::name)::kCapacity;
  TEXTS(TEXTMAX)
  SECRETS(TEXTMAX)
#undef TEXTMAX
  auto entry = text["competitions"].to<JsonObject>();
  entry["competition"] = decltype(settings::Source::competition)::kCapacity;
  entry["season"] = decltype(settings::Source::season)::kCapacity;
  entry["team"] = decltype(settings::Source::team)::kCapacity;
  entry["competition_name"] =
      decltype(settings::Competition::competition_name)::kCapacity;
  entry["team_name"] = decltype(settings::Competition::team_name)::kCapacity;
  auto pair = text["fixture_pairs"].to<JsonObject>();
  pair["primary_id"] = decltype(settings::FixturePair::primary_id)::kCapacity;
  pair["secondary_id"] =
      decltype(settings::FixturePair::secondary_id)::kCapacity;
  auto bounds = out["bounds"].to<JsonObject>();
  auto range = [&](const char* name, int min, int max) {
    auto row = bounds[name].to<JsonArray>();
    row.add(min);
    row.add(max);
  };
  range("debug_status_interval_s", cfg::kDebugStatusIntervalMinS,
        cfg::kDebugStatusIntervalMaxS);
  auto bounded = [&](const char* name, cfg::Range r) {
    range(name, r.min, r.max);
  };
  bounded("brightness", cfg::kBrightnessRange);
  bounded("night_brightness", cfg::kBrightnessRange);
  bounded("rotation", cfg::kRotationRange);
  // The display profile narrows the two layout-dependent maxima (OQ-92).
  bounded("visible_matches",
          {cfg::kVisibleMatchesRange.min, limits.visible_matches_max});
  bounded("table_window",
          {cfg::kTableWindowRange.min, limits.table_window_max});
  bounded("manual_return_s", cfg::kReturnTimeRange);
  bounded("scroll_reset_s", cfg::kReturnTimeRange);
  bounded("scroll_repeat_ms", cfg::kScrollRepeatRange);
  bounded("slideshow_s", cfg::kSlideshowRange);
  bounded("night_start", cfg::kDayMinutesRange);
  bounded("night_end", cfg::kDayMinutesRange);
  bounded("ip_badge_s", cfg::kReturnTimeRange);
  bounded("window_before", cfg::kMatchWindowRange);
  bounded("window_after", cfg::kMatchWindowRange);
  bounded("api_daily_budget", cfg::kApiFootballDailyBudgetRange);
}
}  // namespace web
