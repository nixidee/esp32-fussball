// Actual aggregate codec. Draft mutation is permitted; no Apply receipt is
// published until the complete envelope, nested schemas and suffix are valid.
#include <cassert>
#include <iostream>
#include <string>

#include "config_json.h"
#include "screen_json.h"
struct Input {
  std::string_view text;
  std::size_t at = 0;
  json::Input input() {
    return {this, [](void* raw) {
              auto& self = *static_cast<Input*>(raw);
              return self.at == self.text.size()
                         ? -1
                         : uint8_t(self.text[self.at++]);
            }};
  }
};
struct Output {
  std::string text;
  json::Output output() {
    return {this, [](std::string_view bytes, void* raw) {
              static_cast<Output*>(raw)->text.append(bytes);
              return true;
            }};
  }
};
std::string desired(const settings::AppConfig& config) {
  Output sink;
  json::Writer writer(sink.output());
  assert(writer.object() && writer.key("settings") && writer.object() &&
         web::settingsJson(writer, config.model, true) && writer.end() &&
         web::competitionsJson(writer, config.competitions) &&
         writer.key("screens") && writer.object() &&
         web::screenPolicyJson(writer, config.screens) && writer.end() &&
         writer.end() && writer.finish());
  return sink.text;
}
int main() {
  settings::AppConfig config;
  config.model = settings::initialValues({});
  settings::Competition competition;
  competition.primary.competition.assign("bl1");
  competition.primary.season.assign("2026");
  competition.club.team = 40;
  competition.club.group = 1;
  config.competitions.entries.push(competition);
  config.model.wifi_ssid.assign(std::string_view("raw\0ssid", 8));
  const auto body = desired(config);
  for (const auto& text :
       {"{\"expected_revision\":42,\"apply_id\":\"123456789abcdef0\","
        "\"desired\":" +
            body + "}",
        "{\"desired\":" + body +
            ",\"apply_id\":\"123456789abcdef0\",\"expected_revision\":42}"}) {
    Input input{text};
    settings::AppConfig candidate;
    web::ApplyRequest request;
    settings::Violation why;
    assert(web::readApply(input.input(), candidate, request, why, nullptr,
                          nullptr) == web::SettingsDecode::kOk);
    assert(request.expected_revision == 42 &&
           request.apply_id == 0x123456789abcdef0 && !request.names_changed);
    assert(candidate.model == config.model &&
           candidate.competitions.entries[0].club.team == 40);
    assert(candidate.screens == config.screens &&
           candidate.last_apply_id == request.apply_id);
  }
  for (const auto& text : std::initializer_list<std::string>{
           "{\"expected_revision\":42,\"apply_id\":\"0000000000000000\","
           "\"desired\":" +
               body + "}",
           "{\"expected_revision\":42,\"apply_id\":\"123456789abcdef0\","
           "\"desired\":" +
               body + ",\"expected_revision\":42}",
           "{\"expected_revision\":4294967296,\"apply_id\":"
           "\"123456789abcdef0\",\"desired\":" +
               body + "}",
           "{\"expected_revision\":42,\"apply_id\":\"123456789abcdef0\","
           "\"desired\":" +
               body + ",\"unknown\":0}",
           "{\"expected_revision\":42,\"apply_id\":\"123456789abcdef0\","
           "\"desired\":" +
               body + "}false",
           "{\"expected_revision\":42,\"apply_id\":\"123456789abcdef0\","
           "\"desired\":{}}"}) {
    Input input{text};
    settings::AppConfig candidate;
    web::ApplyRequest request;
    request.apply_id = 7;
    settings::Violation why;
    assert(web::readApply(input.input(), candidate, request, why, nullptr,
                          nullptr) != web::SettingsDecode::kOk);
    assert(request.apply_id == 7);
  }
  char id[17];
  web::correlationIdText(UINT64_MAX, id);
  assert(std::string_view(id) == "ffffffffffffffff");
  std::cout << "aggregate Apply grammar: arbitrary order, full desired, raw "
               "SSID, correlation and no receipt on invalid request PASS\n";
}
