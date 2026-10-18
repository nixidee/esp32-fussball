#include "config_json.h"

#include "screen_json.h"
#include "typed_json.h"

namespace web {
namespace {
using json::Kind;
using settings::FieldPath;
using settings::Reason;
struct Decoder : detail::Decoder {
  using detail::Decoder::Decoder;
  const settings::Limits* limits = nullptr;
  bool nested(SettingsDecode status) {
    if (status == SettingsDecode::kOk) return true;
    result = status;
    return false;
  }
  bool desired(settings::AppConfig& draft, const FieldPath& path) {
    const char* fields[] = {"settings", "competitions", "screens"};
    return object(
        token, path, fields,
        [&](int id, const FieldPath& at) {
          if (id == 0)
            return nested(applySettingsObject(reader, token, draft.model,
                                              violation, *limits, at));
          if (id == 1)
            return nested(applyCompetitionArray(
                reader, token, draft.competitions, violation, at));
          return nested(applyScreenPolicyObject(
              reader, token, draft.screens, violation,
              {limits->visible_matches_max, limits->table_window_max}, at));
        },
        7);
  }
  bool revision(uint32_t& out, const FieldPath& path) {
    return scalar(out, path, false);
  }
};
}  // namespace
SettingsDecode readApply(json::Input input, settings::AppConfig& draft,
                         ApplyRequest& request, settings::Violation& why,
                         ReadNames names, void* context,
                         const settings::Limits& limits) {
  json::Reader reader(input);
  Decoder d(reader, &why);
  d.limits = &limits;
  ApplyRequest candidate;
  const char* fields[] = {"expected_revision", "apply_id", "desired",
                          "adopted_names"};
  if (!d.next() ||
      !d.object(
          d.token, {}, fields,
          [&](int id, const FieldPath& path) {
            if (id == 0) return d.revision(candidate.expected_revision, path);
            if (id == 1) return d.correlation(candidate.apply_id, path);
            if (id == 2) return d.desired(draft, path);
            if (!names) return d.reject(path, Reason::kInvalid);
            candidate.names_changed = true;
            return d.nested(names(d.reader, d.token, context, why));
          },
          7))
    return d.result;
  if (!d.reader.finish()) return SettingsDecode::kSyntax;
  draft.last_apply_id = candidate.apply_id;
  request = candidate;
  return SettingsDecode::kOk;
}
bool configJson(json::Writer& out, const settings::AppConfig& config,
                bool secrets) {
  char correlation[17];
  correlationIdText(config.last_apply_id, correlation);
  const auto& names = config.names;
  return out.key("version") && out.integer(cfg::kAppConfigFormatVersion) &&
         out.key("revision") && out.integer(config.revision) &&
         out.key("apply_id") && out.string(correlation) && out.key("desired") &&
         out.object() && out.key("settings") && out.object() &&
         settingsJson(out, config.model, secrets) && out.end() &&
         competitionsJson(out, config.competitions) && out.key("screens") &&
         out.object() && screenPolicyJson(out, config.screens) && out.end() &&
         out.end() && out.key("names_ref") && out.object() && out.key("slot") &&
         out.integer(names.slot) && out.key("generation") &&
         out.integer(names.generation) && out.key("length") &&
         out.integer(names.length) && out.key("crc") &&
         out.integer(names.crc) && out.end();
}
}  // namespace web
