#include "request_json.h"

#include "typed_json.h"

namespace web {
namespace {
using json::Kind;
using settings::Reason;
struct Object : detail::Decoder {
  using detail::Decoder::Decoder;
  bool reject(const char* key, Reason reason) {
    return detail::Decoder::reject(settings::FieldPath().key(key), reason);
  }
  template <std::size_t N, class Consume>
  SettingsDecode read(const char* const (&keys)[N], uint64_t required,
                      Consume consume) {
    if (!next() || !object(
                       token, {}, keys,
                       [&](unsigned field, const auto&) {
                         return consume(field, keys[field]);
                       },
                       required))
      return result;
    return reader.finish() ? result : SettingsDecode::kSyntax;
  }
  bool string(const char* key, std::string_view& out) {
    if (token.kind != Kind::kString) return reject(key, Reason::kType);
    if (token.truncated) return reject(key, Reason::kTooLong);
    if (!values::isUtf8Text(token.text)) return reject(key, Reason::kInvalid);
    out = token.text;
    return true;
  }
  template <std::size_t N>
  bool string(const char* key, settings::Bytes<N>& out) {
    std::string_view text;
    return string(key, text) &&
           (out.assign(text) || reject(key, Reason::kTooLong));
  }
  bool boolean(const char* key, bool& out) {
    return scalar(out, settings::FieldPath().key(key), false);
  }
  template <class T>
  bool integer(const char* key, T& out, uint32_t max, uint32_t min = 0) {
    if (!scalar(out, settings::FieldPath().key(key), false)) return false;
    return (out >= min && out <= max) || reject(key, Reason::kRange);
  }
};
template <class T, class Decode>
SettingsDecode command(json::Input input, T& out, settings::Violation& why,
                       Decode decode) {
  json::Reader reader(input);
  Object object(reader, &why);
  T candidate{};
  const auto result = decode(object, candidate);
  if (result == SettingsDecode::kOk) out = candidate;
  return result;
}
}  // namespace
SettingsDecode readEmpty(json::Input input, settings::Violation& why) {
  json::Reader reader(input);
  detail::Decoder object(reader, &why);
  if (!object.next()) return object.result;
  if (object.token.kind != Kind::kObject) {
    object.reject({}, Reason::kType);
    return object.result;
  }
  if (!object.next()) return object.result;
  if (object.token.kind != Kind::kEndObject) {
    object.reject({}, Reason::kUnknownKey);
    return object.result;
  }
  return reader.finish() ? SettingsDecode::kOk : SettingsDecode::kSyntax;
}
SettingsDecode readLogin(json::Input input, LoginRequest& out,
                         settings::Violation& why) {
  return command(input, out, why, [](Object& d, LoginRequest& value) {
    const char* keys[] = {"password"};
    return d.read(keys, 0, [&](unsigned, const char* key) {
      return d.string(key, value.password);
    });
  });
}
SettingsDecode readReset(json::Input input, ResetRequest& out,
                         settings::Violation& why) {
  return command(input, out, why, [](Object& d, ResetRequest& value) {
    const char* keys[] = {"confirm", "images", "format_storage"};
    return d.read(keys, 1, [&](unsigned id, const char* key) {
      if (id == 1) return d.boolean(key, value.images);
      if (id == 2) return d.boolean(key, value.format_storage);
      std::string_view text;
      return d.string(key, text) &&
             (text == "reset" || d.reject(key, Reason::kInvalid));
    });
  });
}
SettingsDecode readDemo(json::Input input, DemoRequest& out,
                        settings::Violation& why) {
  DemoRequest candidate;
  const auto result =
      command(input, candidate, why, [](Object& d, DemoRequest& value) {
        const char* keys[] = {"scenario", "step"};
        return d.read(keys, 3, [&](unsigned id, const char* key) {
          return id == 0 ? d.integer(key, value.scenario,
                                     football::demo::kScenarioCount - 1)
                         : d.integer(key, value.step, UINT8_MAX);
        });
      });
  if (result != SettingsDecode::kOk) return result;
  if (candidate.step <
      football::demo::steps(
          static_cast<football::demo::Scenario>(candidate.scenario))) {
    out = candidate;
    return result;
  }
  why.set(settings::FieldPath().key("step"), Reason::kRange);
  return SettingsDecode::kViolation;
}
SettingsDecode readImageId(json::Input input, uint8_t& out,
                           settings::Violation& why) {
  return command(input, out, why, [](Object& d, uint8_t& value) {
    const char* keys[] = {"id"};
    return d.read(keys, 1, [&](unsigned, const char* key) {
      return d.integer(key, value, cfg::kImageLibraryMax, 1);
    });
  });
}
SettingsDecode readImageRole(json::Input input, ImageRoleRequest& out,
                             settings::Violation& why) {
  bool have_id = false;
  ImageRoleRequest candidate;
  const auto result =
      command(input, candidate, why, [&](Object& d, ImageRoleRequest& value) {
        const char* keys[] = {"role", "action", "id"};
        return d.read(keys, 3, [&](unsigned id, const char* key) {
          if (id == 2) {
            have_id = true;
            return d.integer(key, value.id, cfg::kImageLibraryMax, 1);
          }
          std::string_view text;
          if (!d.string(key, text)) return false;
          if (id == 0) {
            if (text == "crest")
              value.role = cfg::kStockCrest;
            else if (text == "background")
              value.role = cfg::kStockBackground;
            else
              return d.reject(key, Reason::kInvalid);
          } else {
            value.copy = text == "copy";
            if (!value.copy && text != "reset")
              return d.reject(key, Reason::kInvalid);
          }
          return true;
        });
      });
  if (result != SettingsDecode::kOk) return result;
  if (!candidate.copy || have_id) {
    out = candidate;
    return result;
  }
  why.set(settings::FieldPath().key("id"), Reason::kRequired);
  return SettingsDecode::kViolation;
}
SettingsDecode readSelectionQuery(json::Input input, uint64_t& out,
                                  settings::Violation& why) {
  return command(input, out, why, [](Object& d, uint64_t& value) {
    const char* keys[] = {"query"};
    return d.read(keys, 1, [&](unsigned, const char*) {
      return d.correlation(value, settings::FieldPath().key("query"));
    });
  });
}
SettingsDecode readSelection(json::Input input, football::SelectionRequest& out,
                             settings::Violation& why) {
  return command(
      input, out, why, [](Object& d, football::SelectionRequest& value) {
        // Stage optional text so arbitrary field order never changes which
        // values apply. Unused picker fields retain the established stale-form
        // semantics.
        struct Text {
          settings::Bytes<cfg::kSelectionTextMaxBytes> value;
          Kind kind = Kind::kNull;
          bool truncated = false;
        } text[4];
        uint8_t provider = 0;
        const char* keys[] = {"provider", "teams",  "offset", "competition",
                              "season",   "search", "country"};
        auto result = d.read(keys, 3, [&](unsigned id, const char* key) {
          if (id == 0) return d.integer(key, provider, cfg::kProviderCount - 1);
          if (id == 1) return d.boolean(key, value.teams);
          if (id == 2) return d.integer(key, value.offset, UINT16_MAX);
          auto& field = text[id - 3];
          field.kind = d.token.kind;
          field.truncated = d.token.truncated;
          if (d.token.kind == Kind::kString) field.value.assign(d.token.text);
          return d.reader.skip(d.token);
        });
        if (result != SettingsDecode::kOk) return result;
        value.provider = static_cast<cfg::Provider>(provider);
        const auto fields =
            settings::selectionFields(value.provider, value.teams);
        const bool used[] = {bool(fields & settings::kSelectionCompetition),
                             bool(fields & settings::kSelectionSeason),
                             bool(fields & settings::kSelectionSearch),
                             bool(fields & settings::kSelectionCountry)};
        for (unsigned i = 0; i < 4; ++i) {
          if (!used[i] || text[i].kind == Kind::kNull) continue;
          const auto* key = keys[i + 3];
          if (text[i].kind != Kind::kString) {
            d.reject(key, Reason::kType);
            return d.result;
          }
          const std::size_t maximum[] = {
              cfg::kCompetitionIdMaxBytes, cfg::kSeasonMaxBytes,
              cfg::kSelectionTextMaxBytes, cfg::kSelectionTextMaxBytes};
          if (text[i].truncated || text[i].value.view().size() > maximum[i]) {
            d.reject(key, Reason::kTooLong);
            return d.result;
          }
          if (!values::isUtf8Text(text[i].value.view())) {
            d.reject(key, Reason::kInvalid);
            return d.result;
          }
        }
        auto assign = [&](auto& target, unsigned i) {
          if (used[i]) target.assign(text[i].value.view());
          return true;
        };
        if (!assign(value.competition, 0) || !assign(value.season, 1) ||
            !assign(value.search, 2) || !assign(value.country, 3))
          return d.result;
        if (!settings::validateSelection(value.provider, value.teams,
                                         value.competition.data,
                                         value.season.data, d.violation))
          return SettingsDecode::kViolation;
        return d.result;
      });
}
}  // namespace web
