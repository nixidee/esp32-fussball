#include <cassert>
#include <iostream>
#include <string>

#include "request_json.h"
struct Source {
  std::string_view text;
  std::size_t at = 0;
  json::Input input() {
    return {this, [](void* raw) {
              auto& self = *static_cast<Source*>(raw);
              return self.at == self.text.size()
                         ? -1
                         : uint8_t(self.text[self.at++]);
            }};
  }
};
template <class T, class Decode>
void reject(std::string_view text, Decode decode) {
  Source source{text};
  T before{}, candidate = before;
  settings::Violation why;
  assert(decode(source.input(), candidate, why) != web::SettingsDecode::kOk);
}
int main() {
  settings::Violation why;
  for (auto text : {"{}", " {} \n"}) {
    Source source{text};
    assert(web::readEmpty(source.input(), why) == web::SettingsDecode::kOk);
  }
  for (auto text : {R"({"ignored":1})", "{} false", "[]"}) {
    Source source{text};
    assert(web::readEmpty(source.input(), why) != web::SettingsDecode::kOk);
  }
  for (auto text :
       {R"({"confirm":"reset","images":false,"format_storage":true})",
        R"({"format_storage":true,"\u0063onfirm":"reset","images":false})"}) {
    Source source{text};
    web::ResetRequest command;
    assert(web::readReset(source.input(), command, why) ==
               web::SettingsDecode::kOk &&
           command.format_storage && !command.images);
  }
  for (auto text :
       {R"({"confirm":"reset","confirm":"reset"})",
        R"({"confirm":"reset\u0000"})", R"({"confirm":"reset","images":null})",
        R"({"confirm":"repair"})", R"({"images":true})"})
    reject<web::ResetRequest>(text, web::readReset);
  for (auto text : {R"({"id":1})", R"({"\u0069d":10})"}) {
    Source source{text};
    uint8_t id;
    assert(web::readImageId(source.input(), id, why) ==
               web::SettingsDecode::kOk &&
           id > 0);
  }
  for (auto text :
       {R"({"id":0})", R"({"id":11})", R"({"id":4294967296})",
        R"({"id":1,"id":2})", R"({"id":1e0})", R"({"id":1,"ignored":0})"})
    reject<uint8_t>(text, web::readImageId);
  for (auto text : {R"({"id":1,"action":"copy","role":"crest"})",
                    R"({"role":"background","action":"reset"})"}) {
    Source source{text};
    web::ImageRoleRequest command;
    assert(web::readImageRole(source.input(), command, why) ==
           web::SettingsDecode::kOk);
  }
  for (auto text : {R"({"role":"crest","action":"copy"})",
                    R"({"role":"crest\u0000","action":"reset"})",
                    R"({"role":"crest","action":"reset","role":"background"})"})
    reject<web::ImageRoleRequest>(text, web::readImageRole);
  Source login{R"({"password":"abc\u0000def"})"};
  web::LoginRequest account;
  assert(web::readLogin(login.input(), account, why) !=
         web::SettingsDecode::kOk);
  for (
      auto text :
      {R"({"provider":0,"teams":false,"season":"2026","competition":{},"country":[1]})",
       R"({"country":[1],"competition":{},"season":"2026","teams":false,"provider":0})"}) {
    Source source{text};
    football::SelectionRequest query;
    assert(web::readSelection(source.input(), query, why) ==
               web::SettingsDecode::kOk &&
           query.competition.empty() && query.country.empty());
  }
  for (
      auto text :
      {R"({"provider":0,"teams":true,"competition":"bl1","season":"2026","search":"Bayern"})",
       R"({"search":"Bayern","season":"2026","competition":"bl1","teams":true,"provider":0})"}) {
    Source source{text};
    football::SelectionRequest query;
    assert(web::readSelection(source.input(), query, why) ==
               web::SettingsDecode::kOk &&
           std::string_view(query.competition.data) == "bl1");
  }
  for (
      auto text :
      {R"({"provider":0,"teams":true,"competition":{}})",
       R"({"provider":0,"teams":true,"competition":"bl1","competition":"bl2"})",
       R"({"provider":0,"teams":true,"competition":"bl1\u0000"})",
       R"({"provider":0,"teams":true,"competition":"bl1","search":"ok"}garbage)"})
    reject<football::SelectionRequest>(text, web::readSelection);
  for (auto text :
       {R"({"provider":1,"teams":true,"competition":"39","season":""})",
        R"({"provider":1,"teams":true,"competition":"39","season":"2026"})"}) {
    Source source{text};
    football::SelectionRequest query;
    assert(web::readSelection(source.input(), query, why) ==
           web::SettingsDecode::kOk);
  }
  for (auto text : {R"({"query":"ffffffffffffffff"})",
                    R"({"\u0071uery":"0000000100000001"})"}) {
    Source source{text};
    uint64_t query = 0;
    assert(web::readSelectionQuery(source.input(), query, why) ==
               web::SettingsDecode::kOk &&
           query != 0);
  }
  for (auto text :
       {R"({})", R"({"query":"0000000000000000"})",
        R"({"query":"000000000000000A"})", R"({"query":1})", R"({"query":"1"})",
        R"({"query":"0000000100000001","query":"0000000100000002"})",
        R"({"query":"0000000100000001","extra":1})",
        R"({"query":"0000000100000001"}false)"}) {
    Source source{text};
    uint64_t query = 123;
    assert(web::readSelectionQuery(source.input(), query, why) !=
               web::SettingsDecode::kOk &&
           query == 123);
  }
  std::cout << "typed command replay: reset safety, duplicate/unknown aliases, "
               "IDs, used picker fields, completion PASS\n";
}
