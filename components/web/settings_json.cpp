#include "settings_json.h"

#include <cstring>

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
template <class T>
bool scalar(JsonVariantConst v, T& target) {
  if (v.isNull()) return true;
  if (!v.is<T>()) return false;
  target = v.as<T>();
  return true;
}
template <std::size_t N>
bool bytes(JsonVariantConst v, settings::Bytes<N>& target) {
  if (v.isNull()) return true;
  if (!v.is<const char*>()) return false;
  auto s = v.as<JsonString>();
  return target.assign({s.c_str(), s.size()});
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
  auto routes = out["routes"].to<JsonArray>();
  for (const auto& route : m.routes) {
    auto item = routes.add<JsonObject>();
    item["enabled"] = route.enabled;
    item["provider"] = static_cast<unsigned>(route.provider);
    item["competition"] = std::string_view(route.competition.view());
    item["season"] = std::string_view(route.season.view());
    item["team"] = std::string_view(route.team.view());
  }
  auto fallbacks = out["fallback_routes"].to<JsonArray>();
  for (const auto& route : m.fallback_routes) {
    auto item = fallbacks.add<JsonObject>();
    item["enabled"] = route.enabled;
    item["provider"] = static_cast<unsigned>(route.provider);
    item["competition"] = std::string_view(route.competition.view());
    item["season"] = std::string_view(route.season.view());
    item["team"] = std::string_view(route.team.view());
  }
  auto mappings = out["fixture_mappings"].to<JsonArray>();
  for (const auto& mapping : m.fixture_mappings) {
    auto item = mappings.add<JsonObject>();
    item["enabled"] = mapping.enabled;
    item["route"] = mapping.route;
    item["swapped"] = mapping.swapped;
    item["primary"] = std::string_view(mapping.primary.view());
    item["secondary"] = std::string_view(mapping.secondary.view());
  }
  auto screens = out["screens"].to<JsonArray>();
  for (const auto& style : m.screens) {
    auto item = screens.add<JsonObject>();
    item["enabled"] = style.enabled;
    item["background"] = style.background;
    item["colour"] = style.colour;
    item["text"] = style.text;
    item["accent"] = style.accent;
    item["text_scale"] = style.text_scale;
  }
}
bool applyJson(JsonObjectConst in, settings::Model& m) {
  auto keys = [](JsonVariantConst row,
                 std::initializer_list<const char*> names) {
    if (!row.is<JsonObjectConst>()) return false;
    for (JsonPairConst entry : row.as<JsonObjectConst>()) {
      bool known = false;
      for (const char* name : names)
        known |= !strcmp(entry.key().c_str(), name);
      if (!known) return false;
    }
    return true;
  };
  for (JsonPairConst entry : in) {
    const auto* name = entry.key().c_str();
    bool known = false;
#define KNOWN(member) known |= strcmp(name, #member) == 0;
    SCALARS(KNOWN)
    TEXTS(KNOWN)
    SECRETS(KNOWN)
#undef KNOWN
    for (const char* other :
         {"routes", "fallback_routes", "fixture_mappings", "screens",
          "idle_screen", "matchday_screen", "own_match_screen"})
      known |= strcmp(name, other) == 0;
    // Redacted exports carry only these boolean markers; they never clear a
    // key.
    bool marker = false;
#define MARKER(member) marker |= strcmp(name, #member "_set") == 0;
    SECRETS(MARKER)
#undef MARKER
    if (marker && entry.value().is<bool>()) continue;
    if (!known) return false;
  }
#define GET(name) \
  if (!scalar(in[#name], m.name)) return false;
  SCALARS(GET)
#undef GET
#define GETTEXT(name) \
  if (!bytes(in[#name], m.name)) return false;
  TEXTS(GETTEXT)
  SECRETS(GETTEXT)
#undef GETTEXT
  for (auto pair : {std::pair{"idle_screen", &m.idle_screen},
                    std::pair{"matchday_screen", &m.matchday_screen},
                    std::pair{"own_match_screen", &m.own_match_screen}}) {
    if (in[pair.first].isNull()) continue;
    if (!in[pair.first].is<uint8_t>()) return false;
    *pair.second = static_cast<cfg::Screen>(in[pair.first].as<uint8_t>());
  }
  for (const char* name : {"routes", "fallback_routes"}) {
    if (in[name].isNull()) continue;
    auto& target = !strcmp(name, "routes") ? m.routes : m.fallback_routes;
    if (!in[name].is<JsonArrayConst>() || in[name].size() != target.size())
      return false;
    for (unsigned i = 0; i < target.size(); ++i) {
      auto row = in[name][i];
      if (!keys(row, {"enabled", "provider", "competition", "season", "team"}))
        return false;
      auto& route = target[i];
      uint8_t p = static_cast<uint8_t>(route.provider);
      if (!scalar(row["enabled"], route.enabled) ||
          !scalar(row["provider"], p) ||
          !bytes(row["competition"], route.competition) ||
          !bytes(row["season"], route.season) ||
          !bytes(row["team"], route.team))
        return false;
      route.provider = static_cast<cfg::Provider>(p);
    }
  }
  if (!in["fixture_mappings"].isNull()) {
    if (!in["fixture_mappings"].is<JsonArrayConst>() ||
        in["fixture_mappings"].size() != m.fixture_mappings.size())
      return false;
    for (unsigned i = 0; i < m.fixture_mappings.size(); ++i) {
      const auto row = in["fixture_mappings"][i];
      auto& mapping = m.fixture_mappings[i];
      if (!keys(row, {"enabled", "route", "swapped", "primary", "secondary"}) ||
          !scalar(row["enabled"], mapping.enabled) ||
          !scalar(row["route"], mapping.route) ||
          !scalar(row["swapped"], mapping.swapped) ||
          !bytes(row["primary"], mapping.primary) ||
          !bytes(row["secondary"], mapping.secondary))
        return false;
    }
  }
  if (!in["screens"].isNull()) {
    if (!in["screens"].is<JsonArrayConst>() ||
        in["screens"].size() != m.screens.size())
      return false;
    for (unsigned i = 0; i < m.screens.size(); ++i) {
      auto row = in["screens"][i];
      auto& s = m.screens[i];
      if (!keys(row, {"enabled", "background", "colour", "text", "accent",
                      "text_scale"}) ||
          !scalar(row["enabled"], s.enabled) ||
          !scalar(row["background"], s.background) ||
          !scalar(row["colour"], s.colour) || !scalar(row["text"], s.text) ||
          !scalar(row["accent"], s.accent) ||
          !scalar(row["text_scale"], s.text_scale))
        return false;
    }
  }
  return settings::validate(m);
}
void schemaJson(JsonObject out) {
  out["version"] = cfg::kSettingsFormatVersion;
  out["max_body"] = cfg::kApiBodyBytes;
  out["route_count"] = cfg::kRouteCount;
  out["fixture_mappings"] = cfg::kFixtureMappings;
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
  settingsJson(defaults, settings::initialValues({}));
  auto bounds = out["bounds"].to<JsonObject>();
  auto range = [&](const char* name, int min, int max) {
    auto row = bounds[name].to<JsonArray>();
    row.add(min);
    row.add(max);
  };
  range("debug_status_interval_s", cfg::kDebugStatusIntervalMinS,
        cfg::kDebugStatusIntervalMaxS);
  auto bounded = [&](const char* name, cfg::Range limits) {
    range(name, limits.min, limits.max);
  };
  bounded("brightness", cfg::kBrightnessRange);
  bounded("night_brightness", cfg::kBrightnessRange);
  bounded("rotation", cfg::kRotationRange);
  bounded("visible_matches", cfg::kVisibleMatchesRange);
  bounded("table_window", cfg::kTableWindowRange);
  bounded("manual_return_s", cfg::kReturnTimeRange);
  bounded("scroll_reset_s", cfg::kReturnTimeRange);
  bounded("scroll_repeat_ms", cfg::kScrollRepeatRange);
  bounded("slideshow_s", cfg::kSlideshowRange);
  bounded("night_start", cfg::kDayMinutesRange);
  bounded("night_end", cfg::kDayMinutesRange);
  bounded("ip_badge_s", cfg::kReturnTimeRange);
  bounded("window_before", cfg::kMatchWindowRange);
  bounded("window_after", cfg::kMatchWindowRange);
  bounded("api_daily_budget", cfg::kProviderBudgetRange);
}
}  // namespace web
