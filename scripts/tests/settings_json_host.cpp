// Production stream settings/competition codecs. No copied decoding rules.
#include <cassert>
#include <iostream>
#include <string>

#include "settings_json.h"

struct Input {
  std::string_view text;
  std::size_t at = 0;
  json::Input input() {
    return {this, [](void* context) {
              auto& self = *static_cast<Input*>(context);
              return self.at < self.text.size()
                         ? static_cast<unsigned char>(self.text[self.at++])
                         : -1;
            }};
  }
};
struct Output {
  std::string text;
  std::size_t fail_after = SIZE_MAX;
  json::Output output() {
    return {this, [](std::string_view bytes, void* context) {
              auto& self = *static_cast<Output*>(context);
              if (self.text.size() + bytes.size() > self.fail_after)
                return false;
              self.text.append(bytes);
              return true;
            }};
  }
};
web::SettingsDecode patch(std::string_view text, settings::Model& model,
                          settings::Violation* violation = nullptr) {
  Input source{text};
  return web::applyJson(source.input(), model, violation);
}
std::string saved(const settings::Model& model) {
  Output sink;
  json::Writer writer(sink.output());
  assert(writer.object() && web::settingsJson(writer, model, true) &&
         writer.end() && writer.finish());
  return sink.text;
}
int main() {
  using web::SettingsDecode;
  auto initial = settings::initialValues({});
  auto model = initial;
  model.admin_password.assign("private password");
  model.api_football_key.assign("key");
  model.overview_screenshot = true;
  auto restored = initial;
  assert(patch(saved(model), restored) == SettingsDecode::kOk &&
         restored == model);
  for (unsigned byte = 0; byte <= 255; ++byte) {
    const std::string ssid(32, static_cast<char>(byte));
    model.wifi_ssid.assign(ssid);
    restored = initial;
    const auto text = saved(model);
    assert(patch(text, restored) == SettingsDecode::kOk);
    assert(restored == model);
    if (byte == 255)
      assert(text.find("\"wifi_ssid\":{\"base64\"") != std::string::npos);
  }
  model = initial;
  model.wifi_ssid.assign(std::string_view("A\0B", 3));
  const auto nul = saved(model);
  assert(nul.find("A\\u0000B") != std::string::npos);
  assert(patch(nul, restored) == SettingsDecode::kOk && restored == model);

  const struct {
    const char* text;
    const char* field;
    settings::Reason reason;
  } rejected[] = {
      {"{\"brightness\":5,\"brightness\":6}", "brightness",
       settings::Reason::kDuplicate},
      {"{\"providers\":[{\"daily_requests\":1,\"daily_requests\":2}]}",
       "providers[0].daily_requests", settings::Reason::kDuplicate},
      {"{\"providers\":[{\"intervals\":{\"live\":10,\"live\":20}}]}",
       "providers[0].intervals.live", settings::Reason::kDuplicate},
      {"{\"brightness\":5,\"unknown\":6}", "unknown",
       settings::Reason::kUnknownKey},
      {"{\"brightness\":1.0}", "brightness", settings::Reason::kType},
      {"{\"brightness\":256}", "brightness", settings::Reason::kRange},
      {"{\"brightness\":-1}", "brightness", settings::Reason::kRange},
      {"{\"wifi_ssid\":{}}", "wifi_ssid.base64", settings::Reason::kRequired},
      {"{\"wifi_ssid\":{\"base64\":\"/x==\"}}", "wifi_ssid.base64",
       settings::Reason::kInvalid},
      {"{\"wifi_ssid\":{\"base64\":\"////=\"}}", "wifi_ssid.base64",
       settings::Reason::kInvalid},
      {"{\"wifi_ssid\":{\"base64\":\"/w==\",\"base64\":\"AA==\"}}",
       "wifi_ssid.base64", settings::Reason::kDuplicate},
  };
  for (const auto& item : rejected) {
    model = initial;
    settings::Violation violation;
    assert(patch(item.text, model, &violation) == SettingsDecode::kViolation);
    assert(model == initial);
    assert(violation.path.data() == std::string(item.field));
    assert(violation.reason == item.reason);
  }
  for (const auto* text : {"{\"brightness\":5", "{\"brightness\":5}{}",
                           "{\"wifi_ssid\":\"\xc0\x80\"}"}) {
    model = initial;
    assert(patch(text, model) == SettingsDecode::kSyntax && model == initial);
  }
  const auto large =
      std::string("{\"api_football_key\":\"") + std::string(97, 'x') + "\"}";
  settings::Violation violation;
  assert(patch(large, model, &violation) == SettingsDecode::kViolation);
  assert(violation.reason == settings::Reason::kTooLong);
  model = initial;
  assert(
      patch(
          "{\"brightness\":null,\"providers\":[{\"intervals\":{\"live\":30}}]}",
          model) == SettingsDecode::kOk);
  assert(model.brightness == initial.brightness &&
         model.providers[0].interval_s[2] == 30);

  settings::Competitions list;
  settings::initialCompetitions({.competition = "bl1", .team = "40"}, list);
  Output sink;
  json::Writer writer(sink.output());
  assert(writer.object() && web::competitionsJson(writer, list) &&
         writer.end() && writer.finish());
  settings::Competitions decoded;
  Input input{sink.text};
  assert(web::applyCompetitionsJson(input.input(), decoded) ==
         SettingsDecode::kOk);
  // Allocation cursors belong to the NVS owner; only entries travel on the
  // wire.
  assert(list.entries == decoded.entries);
  for (const auto* team : {"4294967296", "-1"}) {
    const auto body = std::string(
                          "{\"competitions\":[{\"primary\":{\"competition\":"
                          "\"bl1\"},\"club\":{\"team\":") +
                      team + "}}]}";
    Input source{body};
    const auto before = list;
    assert(web::applyCompetitionsJson(source.input(), list, &violation) ==
           SettingsDecode::kViolation);
    assert(list == before);
    assert(violation.path.data() == std::string("competitions[0].club.team"));
    assert(violation.reason == settings::Reason::kRange);
  }
  // Composition retains the caller's precise common-apply field prefix.
  {
    Input input{R"({"brightness":256})"};
    json::Reader reader(input.input());
    json::Token token;
    assert(reader.next(token));
    auto candidate = initial;
    assert(web::applySettingsObject(
               reader, token, candidate, &violation, {},
               settings::FieldPath().key("desired").with("settings")) ==
           SettingsDecode::kViolation);
    assert(violation.path.data() == std::string("desired.settings.brightness"));
  }
  for (const auto* text : {"{}", "{\"secrets\":false}", "{\"secrets\":null}"}) {
    Input input{text};
    bool secrets = true;
    assert(web::exportOptions(input.input(), secrets) == SettingsDecode::kOk);
    assert(!secrets);
  }
  {
    Input input{R"({"secrets":true})"};
    bool secrets = false;
    assert(web::exportOptions(input.input(), secrets) == SettingsDecode::kOk);
    assert(secrets);
  }
  for (const auto* text : {"{\"secrets\":true,\"secrets\":false}",
                           "{\"unknown\":true}", "{\"secrets\":1}"}) {
    Input input{text};
    bool secrets = false;
    assert(web::exportOptions(input.input(), secrets, &violation) ==
           SettingsDecode::kViolation);
    assert(!secrets);
  }
  {
    Input input{R"({"secrets":true}false)"};
    bool secrets = false;
    assert(web::exportOptions(input.input(), secrets) ==
           SettingsDecode::kSyntax);
    assert(!secrets);
  }
  Output public_settings;
  json::Writer public_writer(public_settings.output());
  assert(public_writer.object() && web::settingsJson(public_writer, model) &&
         public_writer.end() && public_writer.finish());
  assert(public_settings.text.find("\"api_football_key\":") ==
         std::string::npos);
  assert(public_settings.text.find("\"api_football_key_set\":") !=
         std::string::npos);
  Output schema;
  json::Writer metadata(schema.output());
  assert(metadata.object() && web::schemaJson(metadata, {}, false) &&
         metadata.end() && metadata.finish());
  assert(schema.text.find("\"backlight\":false") != std::string::npos);
  assert(schema.text.find(
             "\"version\":" + std::to_string(cfg::kAppConfigFormatVersion) +
             ",") != std::string::npos);
  assert(schema.text.find("\"firmware_identity_version\":" +
                          std::to_string(cfg::kFirmwareIdentityVersion) +
                          ",") != std::string::npos);
  Output full;
  full.fail_after = 5;
  json::Writer interrupted(full.output());
  assert(interrupted.object() && !web::settingsJson(interrupted, initial));
  std::cout << "Stream settings: 256 raw SSIDs, NUL, nested duplicates, "
               "syntax/rollback, competition identities, schema and output "
               "failure PASS\n";
}
