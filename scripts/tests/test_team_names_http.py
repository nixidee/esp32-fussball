"""Exercise the actual preset handler/writer and catalogue at the HTTP boundary.

Only the SDK request, deadline and shared admission are adapted. The output is
compared with the complete maintained source; no device HTTP is claimed.
"""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def block(source, signature):
    start = source.index(signature)
    depth = 0
    for at in range(source.index("{", start), len(source)):
        depth += (source[at] == "{") - (source[at] == "}")
        if depth == 0:
            return source[start:at + 1]
    raise RuntimeError(signature)


def main():
    spec = importlib.util.spec_from_file_location(
        "embed_team_names", ROOT / "scripts/embed_team_names.py")
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    source = (ROOT / "components/web/web_service.cpp").read_text()
    writer = block(source, "class Writer {") + ";\n"
    handler = block(source, "esp_err_t getTeamNamesPreset(")
    harness = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "app_config.h"
#include "team_name_catalogue.h"
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
static bool busy = false, held = false;
namespace fault { constexpr int kBusy = 409; }
struct httpd_req_t {
  std::string body, type;
  std::map<std::string,std::string> headers;
  std::vector<std::size_t> chunks;
  int fail_call = -1, calls = 0, final_chunks = 0;
  std::size_t expire_after = SIZE_MAX;
};
struct Request { explicit Request(httpd_req_t*) {} };
namespace network {
struct HeavyGuard {
  bool admitted = !busy;
  HeavyGuard() { if (admitted) { assert(!held); held = true; } }
  ~HeavyGuard() { if (admitted) held = false; }
  explicit operator bool() const { return admitted; }
};
}
bool sendReady(httpd_req_t* req) {
  assert(held);
  return req->body.size() < req->expire_after;
}
esp_err_t error(httpd_req_t*, int why) { return why; }
esp_err_t httpd_resp_set_type(httpd_req_t* req, const char* type) {
  req->type = type; return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t* req, const char* key, const char* value) {
  req->headers[key] = value; return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* data, std::size_t size) {
  assert(held);
  if (req->calls++ == req->fail_call) return ESP_FAIL;
  if (size == 0) { ++req->final_chunks; return ESP_OK; }
  assert(size <= 512);
  req->chunks.push_back(size);
  req->body.append(data, size);
  return ESP_OK;
}
'''
    harness += writer + handler
    harness += r'''
int main() {
  busy = true;
  httpd_req_t rejected;
  assert(getTeamNamesPreset(&rejected) == 409);
  assert(rejected.body.empty() && rejected.calls == 0 && !held);
  busy = false;
  httpd_req_t success;
  assert(getTeamNamesPreset(&success) == ESP_OK && !held);
  assert(success.type == "application/json");
  assert(success.headers["Cache-Control"] == "no-store");
  assert(success.headers["X-Team-Names-Source"] ==
         football::names::catalogue::sourceSha256());
  assert(success.final_chunks == 1 && success.body.size() > 8192);
  for (int position : {0, 1, 7}) {
    httpd_req_t failed;
    failed.fail_call = position;
    assert(getTeamNamesPreset(&failed) == ESP_FAIL && !held);
    assert(failed.final_chunks == 0);
    assert(failed.calls == position + 1);
    assert(success.body.compare(0, failed.body.size(), failed.body) == 0);
  }
  httpd_req_t expired;
  expired.expire_after = 512;
  assert(getTeamNamesPreset(&expired) == ESP_FAIL && !held);
  assert(expired.body.size() == 512 && expired.final_chunks == 0);
  httpd_req_t retry;
  assert(getTeamNamesPreset(&retry) == ESP_OK && !held);
  assert(retry.body == success.body && retry.final_chunks == 1);
  std::cout << success.body;
}
'''
    with tempfile.TemporaryDirectory(prefix="football-names-http-") as temp:
        directory = Path(temp)
        generator.prepare_header(ROOT / "include/team_names.json", directory)
        cpp, binary = directory / "http.cpp", directory / "http"
        cpp.write_text(harness)
        subprocess.run([
            "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
            "-I" + str(ROOT / "components/names/include"),
            "-I" + str(ROOT / "include"),
            "-I" + str(directory / "team_name_asset"), str(cpp),
            str(ROOT / "components/names/team_name_catalogue.cpp"),
            str(ROOT / "components/json/json_stream.cpp"),
            "-I" + str(ROOT / "components/json/include"),
            "-o", str(binary),
        ], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True)
        decoded = json.loads(result.stdout)
        assert decoded == json.loads((ROOT / "include/team_names.json").read_text())
        print("PASS: complete source HTTP round trip, admission, bounded chunks, "
              "three failed sends, deadline abort and retry; "
              f"{len(decoded['clubs'])} clubs, {len(result.stdout)} JSON bytes")


if __name__ == "__main__":
    main()
