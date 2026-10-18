#include "settings_json.h"

#include <charconv>
#include <limits>
#include <type_traits>

#include "football_model.h"
#include "screen_json.h"
#include "typed_json.h"
#include "value_utils.h"

namespace web {
namespace {
#define SCALARS(X)           \
  X(external_antenna)        \
  X(debug_status_log)        \
  X(debug_status_interval_s) \
  X(espn_opt_in)             \
  X(demo)                    \
  X(night_enabled)           \
  X(ip_badge_ap_permanent)   \
  X(ip_badge_bottom)         \
  X(browser_debug)           \
  X(overview_screenshot)     \
  X(language)                \
  X(brightness)              \
  X(night_brightness)        \
  X(rotation)                \
  X(scroll_reset_s)          \
  X(scroll_repeat_ms)        \
  X(night_start)             \
  X(night_end)               \
  X(ip_badge_s)
#define TEXTS(X) \
  X(wifi_ssid)   \
  X(hostname)    \
  X(time_zone)   \
  X(ntp_server)
#define SECRETS(X)    \
  X(wifi_password)    \
  X(ap_password)      \
  X(admin_password)   \
  X(api_football_key) \
  X(football_data_key)

using json::Kind;
using settings::FieldPath;
using settings::Reason;
constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool ssidJson(json::Writer& out, std::string_view bytes) {
  values::Utf8 utf8;
  bool valid = true;
  for (const unsigned char c : bytes)
    if (!utf8.accept(c)) {
      valid = false;
      break;
    }
  if (valid && utf8.complete()) return out.string(bytes);
  char encoded[44];
  std::size_t used = 0;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t a = static_cast<unsigned char>(bytes[i]);
    const uint32_t b =
        i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
    const uint32_t c =
        i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
    encoded[used++] = kBase64[a >> 2];
    encoded[used++] = kBase64[((a & 3) << 4) | (b >> 4)];
    encoded[used++] =
        i + 1 < bytes.size() ? kBase64[((b & 15) << 2) | (c >> 6)] : '=';
    encoded[used++] = i + 2 < bytes.size() ? kBase64[c & 63] : '=';
  }
  return out.object() && out.key("base64") && out.string({encoded, used}) &&
         out.end();
}

struct Decoder : detail::Decoder {
  using detail::Decoder::Decoder;
  bool ssid(settings::Bytes<cfg::kWifiSsidMaxBytes>& out,
            const FieldPath& path) {
    if (token.kind != Kind::kObject) return bytes(out, path);
    const char* keys[] = {"base64"};
    bool present = false;
    return object(
               token, path, keys,
               [&](unsigned, const FieldPath& at) {
                 present = true;
                 if (token.kind != Kind::kString)
                   return reject(at, Reason::kType);
                 if (token.truncated || token.text.size() > 44)
                   return reject(at, Reason::kTooLong);
                 const auto encoded = token.text;
                 if (encoded.size() % 4) return reject(at, Reason::kInvalid);
                 char decoded[cfg::kWifiSsidMaxBytes];
                 std::size_t count = 0;
                 auto digit = [](char c) {
                   for (int i = 0; i < 64; ++i)
                     if (kBase64[i] == c) return i;
                   return -1;
                 };
                 for (std::size_t i = 0; i < encoded.size(); i += 4) {
                   const int a = digit(encoded[i]), b = digit(encoded[i + 1]);
                   const int c =
                       encoded[i + 2] == '=' ? 0 : digit(encoded[i + 2]);
                   const int e =
                       encoded[i + 3] == '=' ? 0 : digit(encoded[i + 3]);
                   const bool pad2 = encoded[i + 2] == '=',
                              pad1 = encoded[i + 3] == '=';
                   if (a < 0 || b < 0 || c < 0 || e < 0 || (pad2 && !pad1) ||
                       ((pad1 || pad2) && i + 4 != encoded.size()) ||
                       (pad2 && (b & 15)) || (pad1 && !pad2 && (c & 3)))
                     return reject(at, Reason::kInvalid);
                   const std::size_t length = pad2 ? 1 : pad1 ? 2 : 3;
                   if (count + length > sizeof(decoded))
                     return reject(at, Reason::kTooLong);
                   decoded[count++] = static_cast<char>((a << 2) | (b >> 4));
                   if (length > 1)
                     decoded[count++] = static_cast<char>((b << 4) | (c >> 2));
                   if (length > 2)
                     decoded[count++] = static_cast<char>((c << 6) | e);
                 }
                 return out.assign({decoded, count});
               }) &&
           (present || reject(path.with("base64"), Reason::kRequired));
  }
  template <bool IsSsid, std::size_t N>
  bool textValue(settings::Bytes<N>& out, const FieldPath& path) {
    if constexpr (IsSsid)
      return ssid(out, path);
    else
      return bytes(out, path);
  }
};

bool limit(Decoder& d, settings::RequestLimit& out, const FieldPath& path,
           settings::ProviderPlan* plan = nullptr) {
  const char* keys[] = {"daily_requests", "window_requests", "window_s",
                        "intervals"};
  const char* simple[] = {"daily_requests", "window_requests", "window_s"};
  auto field = [&](unsigned field, const FieldPath& at) {
    if (field == 0) return d.scalar(out.daily, at);
    if (field == 1) return d.scalar(out.window_requests, at);
    if (field == 2) return d.scalar(out.window_s, at);
    if (d.token.kind == Kind::kNull) return true;
    const char* types[cfg::kDataTypeCount];
    for (std::size_t i = 0; i < cfg::kDataTypeCount; ++i)
      types[i] = settings::dataTypeName(static_cast<cfg::DataType>(i));
    return d.object(d.token, at, types,
                    [&](unsigned type, const FieldPath& where) {
                      return d.scalar(plan->interval_s[type], where);
                    });
  };
  return plan ? d.object(d.token, path, keys, field)
              : d.object(d.token, path, simple, field);
}

bool settingsObject(Decoder& d, const json::Token& first,
                    settings::Model& model, const settings::Limits& limits,
                    const FieldPath& path) {
#define KEY(name) #name,
#define MARKER(name) #name "_set",
  const char* keys[] = {SCALARS(KEY) TEXTS(KEY) SECRETS(KEY) "input_roles",
                        "providers", "global_limit", SECRETS(MARKER)};
#undef KEY
#undef MARKER
  if (!d.object(first, path, keys, [&](unsigned field, const FieldPath& at) {
        unsigned index = 0;
#define SCALAR(name) \
  if (field == index++) return d.scalar(model.name, at);
        SCALARS(SCALAR)
#undef SCALAR
#define TEXT(name)      \
  if (field == index++) \
    return d.textValue<std::string_view(#name) == "wifi_ssid">(model.name, at);
        TEXTS(TEXT)
#undef TEXT
#define SECRET(name) \
  if (field == index++) return d.bytes(model.name, at);
        SECRETS(SECRET)
#undef SECRET
        if (field == index++) {
          if (d.token.kind == Kind::kNull) return true;
          return d.array(d.token, at, model.input_roles.size(),
                         [&](unsigned i, const FieldPath& where) {
                           uint8_t role = 0;
                           if (!d.scalar(role, where, false)) return false;
                           model.input_roles[i] =
                               static_cast<cfg::InputRole>(role);
                           return true;
                         });
        }
        if (field == index++) {
          if (d.token.kind == Kind::kNull) return true;
          return d.array(d.token, at, model.providers.size(),
                         [&](unsigned i, const FieldPath& where) {
                           return limit(d, model.providers[i].limit, where,
                                        &model.providers[i]);
                         });
        }
        if (field == index++)
          return d.token.kind == Kind::kNull ||
                 limit(d, model.global_limit, at);
        bool marker = false;
        return d.scalar(marker, at, false);
      }))
    return false;
  settings::Violation invalid;
  return settings::validate(model, nullptr, &invalid, limits) ||
         d.reject(path.with(invalid.path.data()), invalid.reason);
}

bool competitionArray(Decoder& d, const json::Token& first,
                      settings::Competitions& list, const FieldPath& path) {
  if (first.kind != Kind::kArray) return d.reject(path, Reason::kType);
  list.entries.clear();
  return d.array(
      first, path, cfg::kCompetitionMax, [&](unsigned, const FieldPath& at) {
        settings::Competition entry;
        const char* keys[] = {"id",      "incarnation",      "active",
                              "primary", "competition_name", "club",
                              "display"};
        if (!d.object(
                d.token, at, keys, [&](unsigned field, const FieldPath& where) {
                  if (field == 0) return d.scalar(entry.id, where);
                  if (field == 1) return d.scalar(entry.incarnation, where);
                  if (field == 2) return d.scalar(entry.active, where);
                  if (field == 4) return d.bytes(entry.competition_name, where);
                  if (field == 6) {
                    d.result = applyCompetitionPreferencesObject(
                        d.reader, d.token, entry.display, d.violation, where);
                    return d.result == SettingsDecode::kOk;
                  }
                  if (d.token.kind == Kind::kNull) return true;
                  if (field == 5) {
                    const char* club[] = {"team", "name", "own_special",
                                          "group"};
                    return d.object(d.token, where, club,
                                    [&](unsigned part, const FieldPath& at) {
                                      if (part == 0)
                                        return d.scalar(entry.club.team, at);
                                      if (part == 1)
                                        return d.bytes(entry.club.name, at);
                                      if (part == 2)
                                        return d.scalar(entry.club.own_special,
                                                        at);
                                      return d.scalar(entry.club.group, at);
                                    });
                  }
                  const char* source[] = {"provider", "competition", "season"};
                  return d.object(
                      d.token, where, source,
                      [&](unsigned part, const FieldPath& at) {
                        if (part == 1)
                          return d.bytes(entry.primary.competition, at);
                        if (part == 2) return d.bytes(entry.primary.season, at);
                        uint8_t provider =
                            static_cast<uint8_t>(entry.primary.provider);
                        if (!d.scalar(provider, at)) return false;
                        entry.primary.provider =
                            static_cast<cfg::Provider>(provider);
                        return true;
                      });
                }))
          return false;
        return list.entries.push(entry);
      });
}

bool limitJson(json::Writer& out, const settings::RequestLimit& limit) {
  return out.key("daily_requests") && out.integer(limit.daily) &&
         out.key("window_requests") && out.integer(limit.window_requests) &&
         out.key("window_s") && out.integer(limit.window_s);
}
template <class T>
bool scalarJson(json::Writer& out, T value) {
  if constexpr (std::is_same_v<T, bool>)
    return out.boolean(value);
  else
    return out.integer(value);
}
}  // namespace

SettingsDecode applySettingsObject(json::Reader& reader,
                                   const json::Token& token,
                                   settings::Model& model,
                                   settings::Violation* violation,
                                   const settings::Limits& limits,
                                   const FieldPath& path) {
  Decoder d{reader, violation};
  settingsObject(d, token, model, limits, path);
  return d.result;
}
SettingsDecode applyCompetitionArray(json::Reader& reader,
                                     const json::Token& token,
                                     settings::Competitions& list,
                                     settings::Violation* violation,
                                     const FieldPath& path) {
  Decoder d{reader, violation};
  competitionArray(d, token, list, path);
  return d.result;
}
SettingsDecode exportOptions(json::Input input, bool& secrets,
                             settings::Violation* violation) {
  json::Reader reader(input);
  Decoder d{reader, violation};
  bool candidate = false;
  const char* keys[] = {"secrets"};
  if (!d.next()) return d.result;
  if (!d.object(d.token, {}, keys, [&](unsigned, const FieldPath& at) {
        return d.scalar(candidate, at);
      }))
    return d.result;
  if (!reader.finish()) return SettingsDecode::kSyntax;
  secrets = candidate;
  return SettingsDecode::kOk;
}
SettingsDecode applyJson(json::Input input, settings::Model& model,
                         settings::Violation* violation,
                         const settings::Limits& limits) {
  json::Reader reader(input);
  json::Token token;
  if (!reader.next(token) || token.kind != Kind::kObject)
    return SettingsDecode::kSyntax;
  auto candidate = model;
  const auto result =
      applySettingsObject(reader, token, candidate, violation, limits);
  if (result != SettingsDecode::kOk) return result;
  if (!reader.finish()) return SettingsDecode::kSyntax;
  model = candidate;
  return SettingsDecode::kOk;
}
SettingsDecode applyCompetitionsJson(json::Input input,
                                     settings::Competitions& list,
                                     settings::Violation* violation) {
  json::Reader reader(input);
  Decoder d{reader, violation};
  if (!d.next() || d.token.kind != Kind::kObject)
    return SettingsDecode::kSyntax;
  auto candidate = list;
  const char* keys[] = {"competitions"};
  if (!d.object(d.token, {}, keys, [&](unsigned, const FieldPath& at) {
        return d.token.kind == Kind::kNull ||
               competitionArray(d, d.token, candidate, at);
      }))
    return d.result;
  if (!reader.finish()) return SettingsDecode::kSyntax;
  list = candidate;
  return SettingsDecode::kOk;
}

bool ssidValueJson(json::Writer& out, std::string_view raw_bytes) {
  return ssidJson(out, raw_bytes);
}
bool settingsJson(json::Writer& out, const settings::Model& m, bool secrets) {
#define PUT(name) \
  if (!out.key(#name) || !scalarJson(out, m.name)) return false;
  SCALARS(PUT)
#undef PUT
#define TEXT(name)                                        \
  if (!out.key(#name)) return false;                      \
  if constexpr (std::string_view(#name) == "wifi_ssid") { \
    if (!ssidJson(out, m.name.view())) return false;      \
  } else if (!out.string(m.name.view()))                  \
    return false;
  TEXTS(TEXT)
#undef TEXT
#define SECRET(name)                                                \
  if (!out.key(#name "_set") || !out.boolean(!m.name.empty()) ||    \
      (secrets && (!out.key(#name) || !out.string(m.name.view())))) \
    return false;
  SECRETS(SECRET)
#undef SECRET
  if (!out.key("input_roles") || !out.array()) return false;
  for (const auto role : m.input_roles)
    if (!out.integer(static_cast<unsigned>(role))) return false;
  if (!out.end() || !out.key("providers") || !out.array()) return false;
  for (const auto& plan : m.providers) {
    if (!out.object() || !limitJson(out, plan.limit) || !out.key("intervals") ||
        !out.object())
      return false;
    for (std::size_t type = 0; type < cfg::kDataTypeCount; ++type)
      if (!out.key(settings::dataTypeName(static_cast<cfg::DataType>(type))) ||
          !out.integer(plan.interval_s[type]))
        return false;
    if (!out.end() || !out.end()) return false;
  }
  return out.end() && out.key("global_limit") && out.object() &&
         limitJson(out, m.global_limit) && out.end();
}
bool competitionValueJson(json::Writer& out,
                          const settings::Competition& entry) {
  const auto& source = entry.primary;
  return out.object() && out.key("id") && out.integer(entry.id) &&
         out.key("incarnation") && out.integer(entry.incarnation) &&
         out.key("active") && out.boolean(entry.active) && out.key("primary") &&
         out.object() && out.key("provider") &&
         out.integer(static_cast<unsigned>(source.provider)) &&
         out.key("competition") && out.string(source.competition.view()) &&
         out.key("season") && out.string(source.season.view()) && out.end() &&
         out.key("competition_name") &&
         out.string(entry.competition_name.view()) && out.key("club") &&
         out.object() && out.key("team") && out.integer(entry.club.team) &&
         out.key("name") && out.string(entry.club.name.view()) &&
         out.key("own_special") && out.boolean(entry.club.own_special) &&
         out.key("group") && out.integer(entry.club.group) && out.end() &&
         out.key("display") && out.object() &&
         competitionPreferencesJson(out, entry.display) && out.end() &&
         out.end();
}
bool competitionArrayJson(json::Writer& out,
                          const settings::Competitions& list) {
  if (!out.array()) return false;
  for (const auto& entry : list.entries)
    if (!competitionValueJson(out, entry)) return false;
  return out.end();
}

bool competitionsJson(json::Writer& out, const settings::Competitions& list) {
  return out.key("competitions") && competitionArrayJson(out, list);
}

bool schemaJson(json::Writer& out, const settings::Limits& limits,
                bool backlight) {
  auto integer = [&](const char* name, uint64_t value) {
    return out.key(name) && out.integer(value);
  };
  if (!integer("version", cfg::kAppConfigFormatVersion) ||
      !integer("firmware_identity_version", cfg::kFirmwareIdentityVersion) ||
      !integer("max_body", cfg::kApiBodyBytes) || !out.key("limits") ||
      !out.object() || !integer("competitions", cfg::kCompetitionMax) ||
      !integer("images", cfg::kImageLibraryMax) || !out.end() ||
      !integer("inputs", limits.inputs) || !out.key("backlight") ||
      !out.boolean(backlight) || !integer("image_max", cfg::kImageMaxBytes) ||
      !integer("selection_entries", cfg::kSelectionEntries) ||
      !integer("team_id_max", UINT32_MAX) ||
      !out.key("selection_country_default") ||
      !out.string(cfg::kSelectionCountryDefault) ||
      !integer("image_quota", cfg::kImageTotalBytes) ||
      !integer("replacement_workspace", cfg::kFileReplacementWorkspaceBytes) ||
      !out.key("client") || !out.object())
    return false;
  for (const auto& entry : cfg::kWebClientPolicy)
    if (!integer(entry.key, entry.value)) return false;
  if (!out.end() || !out.key("time_zones") || !out.array()) return false;
  for (const auto zone : cfg::kTimeZones)
    if (!out.string(zone.label)) return false;
  if (!out.end() || !out.key("providers") || !out.array()) return false;
  for (unsigned provider = 0; provider < cfg::kProviderCount; ++provider)
    if (!out.string(
            football::providerName(static_cast<cfg::Provider>(provider))))
      return false;
  if (!out.end() || !out.key("competition_defaults") ||
      !competitionValueJson(out, settings::Competition{}) ||
      !out.key("defaults") || !out.object() ||
      !settingsJson(out, settings::initialValues({}, nullptr, limits)) ||
      !out.end() || !out.key("text") || !out.object())
    return false;
  using settings::Model;
#define MAX(name) \
  if (!integer(#name, decltype(Model::name)::kCapacity)) return false;
  TEXTS(MAX)
  SECRETS(MAX)
#undef MAX
  if (!out.key("competitions") || !out.object() ||
      !integer("competition",
               decltype(settings::Source::competition)::kCapacity) ||
      !integer("season", decltype(settings::Source::season)::kCapacity) ||
      !integer("team", 10) ||
      !integer("competition_name",
               decltype(settings::Competition::competition_name)::kCapacity) ||
      !integer("club_name",
               decltype(settings::ClubPreference::name)::kCapacity) ||
      !out.end() || !out.end() || !out.key("bounds") || !out.object())
    return false;
  auto range = [&](const char* name, uint64_t low, uint64_t high) {
    return out.key(name) && out.array() && out.integer(low) &&
           out.integer(high) && out.end();
  };
  auto bounded = [&](const char* name, cfg::Range bounds) {
    return range(name, bounds.min, bounds.max);
  };
  if (!range("debug_status_interval_s", cfg::kDebugStatusIntervalMinS,
             cfg::kDebugStatusIntervalMaxS) ||
      !bounded("brightness", cfg::kBrightnessRange) ||
      !bounded("night_brightness", cfg::kBrightnessRange) ||
      !bounded("rotation", cfg::kRotationRange) ||
      !bounded("scroll_reset_s", cfg::kReturnTimeRange) ||
      !bounded("scroll_repeat_ms", cfg::kScrollRepeatRange) ||
      !bounded("night_start", cfg::kDayMinutesRange) ||
      !bounded("night_end", cfg::kDayMinutesRange) ||
      !bounded("ip_badge_s", cfg::kReturnTimeRange) ||
      !bounded("daily_requests", cfg::kDailyRequestsRange) ||
      !bounded("window_requests", cfg::kWindowRequestsRange) ||
      !bounded("window_s", cfg::kWindowRange) ||
      !range("interval_s", cfg::kIntervalMinS, cfg::kIntervalMaxS) ||
      !out.end() || !out.key("data_types") || !out.array())
    return false;
  for (std::size_t type = 0; type < cfg::kDataTypeCount; ++type)
    if (!out.string(settings::dataTypeName(static_cast<cfg::DataType>(type))))
      return false;
  return out.end();
}
}  // namespace web
