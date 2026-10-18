#include "command_json.h"

#include "typed_json.h"

namespace web {
SettingsDecode readDisplayCommand(json::Input input, DisplayCommand& command,
                                  settings::Violation& violation) {
  json::Reader reader(input);
  detail::Decoder decoder(reader, &violation);
  DisplayCommand candidate;
  const char* keys[] = {"action", "config_revision"};
  if (!decoder.next() ||
      !decoder.object(
          decoder.token, {}, keys,
          [&](unsigned field, const auto& path) {
            if (field == 1)
              return decoder.scalar(candidate.config_revision, path, false);
            if (decoder.token.kind == json::Kind::kNull)
              return decoder.reject(path, settings::Reason::kRequired);
            if (decoder.token.kind != json::Kind::kString)
              return decoder.reject(path, settings::Reason::kType);
            const auto action = decoder.token.text;
            if (!decoder.token.truncated && action == "next")
              candidate.action = ui::Action::kNext;
            else if (!decoder.token.truncated && action == "ip")
              candidate.action = ui::Action::kShowIp;
            else if (!decoder.token.truncated && action == "auto_next")
              candidate.action = ui::Action::kAutomaticNext;
            else
              return decoder.reject(path, settings::Reason::kInvalid);
            return true;
          },
          3))
    return decoder.result;
  if (!reader.finish()) return SettingsDecode::kSyntax;
  command = candidate;
  return SettingsDecode::kOk;
}
}  // namespace web
