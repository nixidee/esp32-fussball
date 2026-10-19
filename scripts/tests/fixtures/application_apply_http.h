// Controlled HTTP only. Actual application session/settings/names/JSON owners
// are compiled by test_names_config_apply.py. No device or authentication
// claim.
#include "config_json.h"
#include "screen_json.h"
namespace http_test {
using web::ApplyRequest;
using web::configJson;
using web::readApply;
using web::screenPolicyViolation;
using web::SettingsDecode;
using Serial = uint32_t;
struct httpd_req_t {
  std::string body, response, code;
  unsigned status = 0;
  size_t at = 0;
  bool complete = true;
};
struct Request {
  explicit Request(httpd_req_t*) {}
};
struct SessionLock {
  SessionLock() {}
  ~SessionLock() {}
};
struct {
  unsigned revoked = 0;
  void closeOthers(Serial who) {
    assert(who == 7);
    ++revoked;
  }
} sessions;
struct Fault {
  unsigned status;
};
namespace fault {
constexpr Fault kBusy{409}, kBodyIncomplete{400}, kJson{400},
    kSettingsChanged{409}, kSession{401};
}
bool body_broken = false, authorized = true;
const Fault* admitSession(httpd_req_t*, Serial* who = nullptr, bool = false) {
  if (who) *who = 7;
  return authorized ? nullptr : &fault::kSession;
}
const Fault* checkJsonBody(httpd_req_t* req,
                           size_t maximum = cfg::kApiBodyBytes) {
  return req->body.size() > maximum ? &fault::kJson : nullptr;
}
int error(httpd_req_t* req, const Fault& f, const char* = nullptr,
          const json::ErrorInfo* = nullptr) {
  req->status = f.status;
  return ESP_FAIL;
}
int error(httpd_req_t* req, const char*, const char* code, const char*) {
  req->status = 503;
  req->code = code;
  return ESP_FAIL;
}
int failed(httpd_req_t* req, int code, const char* = nullptr) {
  req->status = code == ESP_ERR_NO_MEM ? 507 : 500;
  return ESP_FAIL;
}
int rejected(httpd_req_t* req, const settings::Violation&) {
  req->status = 422;
  return ESP_FAIL;
}
struct JsonBody {
  httpd_req_t* req;
  explicit JsonBody(httpd_req_t* value) : req(value) {}
  json::Input input() {
    return {req, [](void* raw) {
              auto& r = *static_cast<httpd_req_t*>(raw);
              return r.at < r.body.size()
                         ? static_cast<unsigned char>(r.body[r.at++])
                         : -1;
            }};
  }
  bool complete() const { return req->complete && req->at == req->body.size(); }
  const json::ErrorInfo* diagnostic() const { return nullptr; }
};
template <class Encode>
int streamJson(httpd_req_t* req, Encode encode) {
  json::Writer writer({req, [](std::string_view bytes, void* raw) {
                         static_cast<httpd_req_t*>(raw)->response.append(bytes);
                         return true;
                       }});
  return writer.object() && encode(writer) && writer.end() && writer.finish()
             ? ESP_OK
             : ESP_FAIL;
}
// ACTUAL_HTTP_APPLY
std::string body(const char* hostname, int revision_delta = 0) {
  settings::AppConfig config;
  settings::snapshot(config);
  config.model.hostname.assign(hostname);
  config.model.admin_password.assign("synthetic-password");
  Sink sink;
  json::Writer writer({&sink, Sink::write});
  assert(writer.object() &&
         writer.field("expected_revision", config.revision + revision_delta) &&
         writer.field("apply_id", "123456789abcdef0") &&
         writer.key("desired") && writer.object() && writer.key("settings") &&
         writer.object() && web::settingsJson(writer, config.model, true) &&
         writer.end() && web::competitionsJson(writer, config.competitions) &&
         writer.key("screens") && writer.object() &&
         web::screenPolicyJson(writer, config.screens) && writer.end() &&
         writer.end() && writer.end() && writer.finish());
  return sink.bytes;
}
void test() {
  boot();
  const auto before = writes;
  httpd_req_t good;
  good.body = body("http-apply");
  assert(applyConfig(&good) == ESP_OK && writes == before + 1 &&
         sessions.revoked == 1);
  assert(good.response.find("123456789abcdef0") != std::string::npos &&
         good.response.find("synthetic-password") == std::string::npos);
  settings::AppConfig saved;
  settings::snapshot(saved);
  assert(saved.model.hostname.view() == "http-apply");
  for (unsigned variant = 0; variant < 5; ++variant) {
    const auto writes_before = writes;
    httpd_req_t bad;
    bad.body =
        body(variant == 3 ? "bad host" : "ignored", variant == 0 ? 1 : 0);
    if (variant == 1) bad.body += "false";
    if (variant == 2) bad.complete = false;
    if (variant == 4) authorized = false;
    assert(applyConfig(&bad) != ESP_OK && bad.status == (variant == 0   ? 409
                                                         : variant == 3 ? 422
                                                         : variant == 4 ? 401
                                                                        : 400));
    authorized = true;
    assert(writes == writes_before && sessions.revoked == 1);
  }
  httpd_req_t unknown;
  unknown.body = body("http-uncertain");
  ::fault = 1;
  mutated = false;
  assert(applyConfig(&unknown) != ESP_OK && unknown.status == 503 &&
         unknown.code == "outcome_unknown" && sessions.revoked == 1);
  boot();
  settings::snapshot(saved);
  assert(saved.model.hostname.view() == "http-apply");
  std::cout << "actual HTTP Apply+application+NVS: one "
               "write/receipt/redaction/session revocation; "
               "stale/syntax/incomplete/validation/auth reject before "
               "mutation; unknown result preserved PASS\n";
}
}  // namespace http_test
