#include "command_json.h"

#include <charconv>

namespace web {
CommandDecode readDisplayCommand(json::Input input, DisplayCommand& command,
                                 settings::Violation& violation) {
  json::Reader reader(input);
  json::Token token;
  char text[16];
  const char* names[] = {"action", "config_revision"};
  json::Fields fields(names, std::size(names));
  DisplayCommand candidate;
  auto reject = [&](settings::Reason reason, const char* key = nullptr) {
    violation.set(key ? settings::FieldPath().key(key) : settings::FieldPath(),
                  reason);
    return CommandDecode::kViolation;
  };
  if (!reader.next(token) || token.kind != json::Kind::kObject)
    return CommandDecode::kSyntax;
  for (;;) {
    if (!reader.next(token, text, sizeof(text))) return CommandDecode::kSyntax;
    if (token.kind == json::Kind::kEndObject) break;
    const int field = fields.accept(token);
    if (field == -1) return reject(settings::Reason::kUnknownKey);
    if (field == -2)
      return reject(settings::Reason::kDuplicate,
                    token.text == names[1] ? names[1] : names[0]);
    const char* key = names[field];
    if (!reader.next(token, text, sizeof(text))) return CommandDecode::kSyntax;
    if (token.kind == json::Kind::kNull)
      return reject(settings::Reason::kRequired, key);
    if (field == 1) {
      if (token.kind != json::Kind::kNumber ||
          token.text.find_first_of(".eE") != std::string_view::npos)
        return reject(settings::Reason::kType, key);
      uint32_t value = 0;
      const auto parsed = std::from_chars(
          token.text.data(), token.text.data() + token.text.size(), value);
      if (token.truncated || parsed.ec != std::errc{} ||
          parsed.ptr != token.text.data() + token.text.size())
        return reject(settings::Reason::kRange, key);
      candidate.config_revision = value;
      continue;
    }
    if (token.kind != json::Kind::kString)
      return reject(settings::Reason::kType, key);
    if (token.truncated) return reject(settings::Reason::kInvalid, key);
    if (token.text == "next")
      candidate.action = ui::Action::kNext;
    else if (token.text == "ip")
      candidate.action = ui::Action::kShowIp;
    else if (token.text == "auto_next")
      candidate.action = ui::Action::kAutomaticNext;
    else
      return reject(settings::Reason::kInvalid, key);
  }
  if (!reader.finish()) return CommandDecode::kSyntax;
  for (unsigned field = 0; field < std::size(names); ++field)
    if (!fields.contains(field))
      return reject(settings::Reason::kRequired, names[field]);
  command = candidate;
  return CommandDecode::kOk;
}
}  // namespace web
