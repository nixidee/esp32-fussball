#include "settings_json.h"

#include <cstring>
#include <iterator>
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
  X(table_page_s);            \
  X(night_start);             \
  X(night_end);               \
  X(ip_badge_s);              \
  X(window_before);           \
  X(window_after);
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
// JSON keys of the competition screen switches, in cfg::CompetitionScreen
// order.
constexpr const char* kScreenKeys[] = {"own_match", "conference", "matchday",
                                       "table"};
static_assert(std::size(kScreenKeys) == cfg::kCompetitionScreenCount);
void limitJson(JsonObject out, const settings::RequestLimit& limit) {
  out["daily_requests"] = limit.daily;
  out["window_requests"] = limit.window_requests;
  out["window_s"] = limit.window_s;
}
// Absent keys keep the value in 'target'.
bool applyLimit(const Context& c, const FieldPath& at, JsonVariantConst row,
                settings::RequestLimit& target,
                std::initializer_list<const char*> names) {
  return onlyKeys(c, at, row, names) &&
         scalar(c, at.with("daily_requests"), row["daily_requests"],
                target.daily) &&
         scalar(c, at.with("window_requests"), row["window_requests"],
                target.window_requests) &&
         scalar(c, at.with("window_s"), row["window_s"], target.window_s);
}
// A provider list (index = cfg::Provider) changes the first plans; the plans
// after it keep theirs, and absent keys keep their values.
bool applyPlans(const Context& c, JsonVariantConst rows,
                settings::ProviderPlans& plans) {
  if (rows.isNull()) return true;
  const auto list = FieldPath().key("providers");
  if (!listOf(c, list, rows, plans.size())) return false;
  unsigned n = 0;
  for (JsonVariantConst row : rows.as<JsonArrayConst>()) {
    const auto at = list.withIndex(n);
    auto& plan = plans[n++];
    if (!applyLimit(c, at, row, plan.limit,
                    {"daily_requests", "window_requests", "window_s",
                     "intervals"}))
      return false;
    const auto intervals = row["intervals"];
    if (intervals.isNull()) continue;
    const auto where = at.with("intervals");
    if (!intervals.is<JsonObjectConst>()) return c.reject(where, Reason::kType);
    for (JsonPairConst entry : intervals.as<JsonObjectConst>()) {
      bool known = false;
      for (std::size_t t = 0; t < cfg::kDataTypeCount && !known; ++t) {
        if (strcmp(entry.key().c_str(),
                   settings::dataTypeName(static_cast<cfg::DataType>(t))))
          continue;
        known = true;
        if (!scalar(c, where.with(entry.key().c_str()), entry.value(),
                    plan.interval_s[t]))
          return false;
      }
      if (!known)
        return c.reject(where.with(entry.key().c_str()), Reason::kUnknownKey);
    }
  }
  return true;
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
  out["matchday_mode"] = static_cast<unsigned>(m.matchday_mode);
  // Every stored slot; the schema's "inputs" says how many the device has.
  auto roles = out["input_roles"].to<JsonArray>();
  for (const auto role : m.input_roles) roles.add(static_cast<unsigned>(role));
  auto providers = out["providers"].to<JsonArray>();
  for (const auto& plan : m.providers) {
    auto item = providers.add<JsonObject>();
    limitJson(item, plan.limit);
    auto intervals = item["intervals"].to<JsonObject>();
    for (std::size_t t = 0; t < cfg::kDataTypeCount; ++t)
      intervals[settings::dataTypeName(static_cast<cfg::DataType>(t))] =
          plan.interval_s[t];
  }
  limitJson(out["global_limit"].to<JsonObject>(), m.global_limit);
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
         {"slides", "screens", "idle_screen", "matchday_screen",
          "own_match_screen", "conference_mode", "matchday_mode", "input_roles",
          "providers", "global_limit"})
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
  mode = static_cast<uint8_t>(m.matchday_mode);
  if (!scalar(c, FieldPath().key("matchday_mode"), in["matchday_mode"], mode))
    return false;
  m.matchday_mode = static_cast<cfg::MatchdayMode>(mode);
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
  if (!applyPlans(c, in["providers"], m.providers)) return false;
  if (!in["global_limit"].isNull() &&
      !applyLimit(c, FieldPath().key("global_limit"), in["global_limit"],
                  m.global_limit,
                  {"daily_requests", "window_requests", "window_s"}))
    return false;
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
    // An export of earlier firmware lists fewer screens: the later ones
    // keep their values.
    const std::size_t listed = in["screens"].size();
    if (listed > m.screens.size()) return c.reject(list, Reason::kTooMany);
    if (listed == 0) return c.reject(list, Reason::kInvalid);
    for (unsigned i = 0; i < listed; ++i) {
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
  return settings::validate(m, nullptr, violation, limits);
}
void competitionsJson(JsonObject out, const settings::Competitions& list) {
  auto competitions = out["competitions"].to<JsonArray>();
  for (const auto& entry : list.entries) {
    auto item = competitions.add<JsonObject>();
    item["id"] = entry.id;
    item["active"] = entry.active;
    sourceJson(item["primary"].to<JsonObject>(), entry.primary);
    item["competition_name"] = std::string_view(entry.competition_name.view());
    item["team_name"] = std::string_view(entry.team_name.view());
    auto screens = item["screens"].to<JsonObject>();
    for (std::size_t i = 0; i < cfg::kCompetitionScreenCount; ++i)
      screens[kScreenKeys[i]] =
          entry.uses(static_cast<cfg::CompetitionScreen>(i));
  }
}
bool applyCompetitionsJson(JsonObjectConst in, settings::Competitions& list,
                           settings::Violation* violation) {
  const Context c{violation};
  if (!onlyKeys(c, FieldPath(), in, {"competitions"})) return false;
  // A list in the input replaces the whole list; absent keys of an entry
  // take the defaults of a new entry, an absent or 0 id makes it new.
  if (!in["competitions"].isNull()) {
    const auto at_list = FieldPath().key("competitions");
    const auto rows = in["competitions"];
    if (!listOf(c, at_list, rows, list.entries.kCapacity)) return false;
    list.entries.clear();
    unsigned n = 0;
    for (JsonVariantConst row : rows.as<JsonArrayConst>()) {
      const auto at = at_list.withIndex(n++);
      settings::Competition entry;
      if (!onlyKeys(c, at, row,
                    {"id", "active", "primary", "competition_name", "team_name",
                     "screens"}) ||
          !scalar(c, at.with("id"), row["id"], entry.id) ||
          !scalar(c, at.with("active"), row["active"], entry.active) ||
          !applySource(c, at.with("primary"), row["primary"], entry.primary) ||
          !bytes(c, at.with("competition_name"), row["competition_name"],
                 entry.competition_name) ||
          !bytes(c, at.with("team_name"), row["team_name"], entry.team_name))
        return false;
      const auto screens = row["screens"];
      if (!screens.isNull()) {
        const auto where = at.with("screens");
        if (!onlyKeys(c, where, screens,
                      {kScreenKeys[0], kScreenKeys[1], kScreenKeys[2],
                       kScreenKeys[3]}))
          return false;
        for (std::size_t i = 0; i < cfg::kCompetitionScreenCount; ++i) {
          bool on = entry.uses(static_cast<cfg::CompetitionScreen>(i));
          if (!scalar(c, where.with(kScreenKeys[i]), screens[kScreenKeys[i]],
                      on))
            return false;
          const auto bit = static_cast<uint8_t>(1u << i);
          entry.screens = static_cast<uint8_t>(on ? entry.screens | bit
                                                  : entry.screens & ~bit);
        }
      }
      list.entries.push(entry);
    }
  }
  return true;
}
void schemaJson(JsonObject out, const settings::Limits& limits) {
  out["version"] = cfg::kSettingsFormatVersion;
  out["max_body"] = cfg::kApiBodyBytes;
  auto lists = out["limits"].to<JsonObject>();
  lists["competitions"] = cfg::kCompetitionMax;
  lists["slides"] = cfg::kSlideMax;
  lists["images"] = cfg::kImageLibraryMax;
  lists["screens"] = cfg::kScreenCount;
  // Display order of the screens (indices into "screens").
  auto order = out["screen_order"].to<JsonArray>();
  for (const auto screen : cfg::kScreenOrder)
    order.add(static_cast<unsigned>(screen));
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
  bounded("table_page_s", cfg::kTablePageRange);
  bounded("night_start", cfg::kDayMinutesRange);
  bounded("night_end", cfg::kDayMinutesRange);
  bounded("ip_badge_s", cfg::kReturnTimeRange);
  bounded("window_before", cfg::kMatchWindowRange);
  bounded("window_after", cfg::kMatchWindowRange);
  bounded("daily_requests", cfg::kDailyRequestsRange);
  bounded("window_requests", cfg::kWindowRequestsRange);
  bounded("window_s", cfg::kWindowRange);
  range("interval_s", static_cast<int>(cfg::kIntervalMinS),
        static_cast<int>(cfg::kIntervalMaxS));
  auto types = out["data_types"].to<JsonArray>();
  for (std::size_t t = 0; t < cfg::kDataTypeCount; ++t)
    types.add(settings::dataTypeName(static_cast<cfg::DataType>(t)));
  auto screen_keys = out["competition_screens"].to<JsonArray>();
  for (const char* key : kScreenKeys) screen_keys.add(key);
}
}  // namespace web
