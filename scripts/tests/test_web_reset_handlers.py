"""Run the production reset/retired-repair handlers against observable service I/O.

The handler bodies are extracted unchanged; only HTTP, NVS, image/filesystem,
heavy-operation and reboot boundaries are controlled. This establishes ordering,
confirmation and partial-failure reporting, not device filesystem acceptance.
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


PRELUDE = r'''
#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "request_json.h"
#include "json_stream.h"
using web::ResetRequest;
using web::readReset;
using namespace ArduinoJson;
using esp_err_t = int;
constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1;
struct httpd_req_t {};
struct Request { explicit Request(httpd_req_t*) {} };
struct Doc { JsonDocument doc; };
using Reason=settings::Reason;
struct Fault { const char* code; };
namespace fault {
constexpr Fault kBusy{"busy"};
}
std::string body, failure, status;
std::vector<std::string> calls;
JsonDocument response;
bool heavy_available=true, fence_active=false, mounted=true, admitted=true;
void check(bool ok,const char* message) {
  if (!ok) throw std::runtime_error(message);
}
esp_err_t call(const char* stage) {
  calls.emplace_back(stage);
  return failure==stage ? ESP_FAIL : ESP_OK;
}
const char* esp_err_to_name(esp_err_t) { return "CONTROLLED_ERROR"; }
void controlledLog(const char*,const char*,...) {}
#define ESP_LOGW(...) controlledLog(__VA_ARGS__)
bool sameText(JsonString value,const char* expected) {
  return value.c_str()!=nullptr && value.size()==std::strlen(expected) &&
      std::memcmp(value.c_str(),expected,value.size())==0;
}
const Fault* admit(httpd_req_t*,JsonDocument& doc,void* =nullptr,bool =false) {
  if (!admitted) return &fault::kBusy;
  check(!deserializeJson(doc,body),"test request JSON");
  return nullptr;
}
esp_err_t error(httpd_req_t*,const Fault& fault) {
  response.clear();response["code"]=fault.code;return ESP_OK;
}
esp_err_t invalid(httpd_req_t*,Reason,const char*,const char* field) {
  response.clear();response["code"]="invalid";response["field"]=field;return ESP_OK;
}
esp_err_t failed(httpd_req_t*,esp_err_t) {
  response.clear();response["code"]="service_failed";return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t*,const char* value) {
  status=value;return ESP_OK;
}
esp_err_t success(httpd_req_t*) {response.clear();response["ok"]=true;return ESP_OK;}
const Fault* admitSession(httpd_req_t*,void*,bool) {return admitted?nullptr:&fault::kBusy;}
struct BodyInput {std::size_t at=0;static int read(void* raw){
 auto& source=*static_cast<BodyInput*>(raw);return source.at<body.size()?static_cast<uint8_t>(body[source.at++]):-1;}};
template<class T,class Decode>
bool readCommand(httpd_req_t* req,T& value,Decode decode,esp_err_t& reply){
 BodyInput source;settings::Violation why;
 const auto status=decode(json::Input{&source,BodyInput::read},value,why);
 if(status==web::SettingsDecode::kOk)return true;
 reply=invalid(req,why.reason,"rejected",why.path.data());return false;
}
template<class Write>
esp_err_t streamJson(httpd_req_t*,Write write){
 std::string bytes;json::Writer out({&bytes,[](std::string_view text,void* raw){
   static_cast<std::string*>(raw)->append(text);return true;}});
 check(out.object()&&write(out)&&out.end()&&out.finish(),"actual streamed response encoder");
 check(!deserializeJson(response,bytes),"response oracle JSON");return ESP_OK;
}
namespace network {
struct HeavyGuard {
  bool held=heavy_available;
  HeavyGuard(){calls.emplace_back("heavy_begin");}
  ~HeavyGuard(){if(held)calls.emplace_back("heavy_end");}
  explicit operator bool()const{return held;}
};
}
namespace football {
esp_err_t beginReset(){
  const auto err=call("fence_begin");if(err==ESP_OK)fence_active=true;return err;
}
void endReset(){calls.emplace_back("fence_end");fence_active=false;}
namespace names {
void invalidate() {}
esp_err_t erase(){
  const auto err=call("adopted_names");return mounted?err:ESP_FAIL;
}
} // names
namespace budget { esp_err_t erase(){return call("counters");} }
}
namespace settings { esp_err_t reset(){return call("settings");} }
namespace images {
esp_err_t format(){
  const auto err=call("format");if(err==ESP_OK)mounted=true;return err;
}
struct DeleteAllResult { uint8_t deleted=0,already_missing=0; };
esp_err_t removeAll(DeleteAllResult* result){
 const auto error=call("images");
 if(result)*result={uint8_t(error==ESP_OK?2:1),3};
 return error;
}
}
namespace ota { void reboot(){calls.emplace_back("reboot");} }
'''

MAIN = r'''
void prepare(std::string request) {
  body=std::move(request);failure.clear();status.clear();calls.clear();response.clear();
  heavy_available=true;fence_active=false;mounted=true;admitted=true;
}
bool called(const char* stage){return std::find(calls.begin(),calls.end(),stage)!=calls.end();}
void sequence(std::initializer_list<const char*> expected) {
  std::vector<std::string> values;
  for(auto value:expected)values.emplace_back(value);
  check(calls==values,"actual reset service-call order");
}
int main(){try{
  httpd_req_t req;
  prepare(R"({"confirm":"reset","format_storage":true,"images":true})");
  mounted=false;
  check(reset(&req)==ESP_OK,"format reset handler result");
  sequence({"heavy_begin","fence_begin","settings","counters","format","reboot","heavy_end"});
  check(response["ok"].as<bool>()&&fence_active,"successful format stays fenced until reboot");
  check(mounted&&!called("adopted_names")&&!called("images"),"full format works from unmounted state without mounted deletion bypass");

  prepare(R"({"confirm":"reset"})");reset(&req);
  sequence({"heavy_begin","fence_begin","settings","counters","adopted_names","reboot","heavy_end"});
  check(response["ok"].as<bool>()&&!called("format")&&!called("images"),"ordinary factory reset preserves images");

  prepare(R"({"confirm":"reset","images":true})");reset(&req);
  sequence({"heavy_begin","fence_begin","settings","counters","adopted_names","images","reboot","heavy_end"});
  check(response["ok"].as<bool>(),"image deletion option retained");

  for(const auto stage:{"settings","counters","format","adopted_names","images"}){
    prepare(stage==std::string_view("format") ?
      R"({"confirm":"reset","format_storage":true})" :
      R"({"confirm":"reset","images":true})");
    failure=stage;reset(&req);
    check(response["code"]=="reset_partial"&&response["partial"].as<bool>(),"partial reset failure is explicit");
    check(response["stage"]==stage&&status=="500 Internal Server Error","failed stage and HTTP status retained");
    check(!called("reboot")&&!fence_active&&called("fence_end"),"partial reset never reports success/reboots and releases fence");
    check(response["settings_reset"].as<bool>()==(stage!=std::string_view("settings")),"only completed settings stage acknowledged");
    check(response["counters_erased"].as<bool>()==(stage!=std::string_view("settings")&&stage!=std::string_view("counters")),"only completed counter stage acknowledged");
    check(!response["storage_formatted"].as<bool>(),"failed/unattempted format never acknowledged complete");
    check(response["adopted_names_deleted"].as<bool>()==(stage==std::string_view("images")),"partial deletion never advertised complete");
    const std::string message=response["error"].as<const char*>();
    check(message.find("may have changed")!=std::string::npos&&message.find("Retry")!=std::string::npos,"partial state has honest actionable guidance");
    if(stage==std::string_view("images"))
      check(response["image_files_deleted"]==1&&response["image_files_already_missing"]==3&&!response["images_deleted"].as<bool>(),"partial image stage reports actual completed file deletions");
    if(stage==std::string_view("settings"))check(!called("counters")&&!called("format")&&!called("adopted_names"),"settings failure prevents destructive file step");
    if(stage==std::string_view("counters"))check(!called("format")&&!called("adopted_names"),"counter failure prevents destructive file step");
  }

  for(const auto request:{R"({"confirm":"repair","format_storage":true})",R"({"confirm":"reset","format_storage":1})",R"({"confirm":"reset","images":"true"})"}){
    prepare(request);reset(&req);
    check(calls.empty()&&response["code"]=="invalid","invalid confirmation/options never enter destructive workflow");
  }
  prepare(R"({"confirm":"reset","format_storage":true})");heavy_available=false;reset(&req);
  sequence({"heavy_begin"});check(response["code"]=="busy","busy reset does not mutate");
  prepare(R"({"confirm":"reset","format_storage":true})");failure="fence_begin";reset(&req);
  sequence({"heavy_begin","fence_begin","heavy_end"});check(!called("settings"),"fence refusal precedes any NVS mutation");
  prepare(R"({"confirm":"reset","format_storage":true})");admitted=false;reset(&req);
  check(calls.empty(),"admission refusal precedes any destructive call");

  std::cout<<"14 reset scenarios PASS; production handler/typed decoder/shared writer; controlled I/O, no device claim\n";
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
'''


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "components/web/web_service.cpp").read_text()
    assert 'HTTP_POST, repairImages' not in source, "retired repair route has no destructive handler"
    start = source.index("struct ResetProgress {")
    end = source.index("\n// ---- Network, provider data, display", start)
    handlers = source[start:end]
    compiler = shutil.which("clang++")
    if compiler is None:
        raise SystemExit("clang++ is required for the reset handler check")
    with tempfile.TemporaryDirectory(prefix="reset-handlers-") as temporary:
        folder = Path(temporary)
        main_file = folder / "reset.cpp"
        main_file.write_text(PRELUDE + handlers + MAIN)
        # Compile the real command helper/reset decoder without unrelated Demo
        # and provider-selection functions or an alternative validation model.
        codec = (root / "components/web/request_json.cpp").read_text()
        codec = codec[:codec.index("SettingsDecode readDemo(")] + "\n} // namespace web\n"
        (folder / "request.cpp").write_text(codec)
        binary = folder / "check"
        subprocess.run([
            compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I" + str(root / ".pio/libdeps/native/ArduinoJson/src"),
            "-I" + str(root / "include"),
            "-I" + str(root / "components/web"),
            "-I" + str(root / "components/settings/include"),
            "-I" + str(root / "components/football/include"),
            "-I" + str(root / "components/json/include"),
            str(main_file), str(root / "components/json/json_stream.cpp"),
            str(folder / "request.cpp"), "-o", str(binary),
        ], cwd=root, check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = (
            "detect_leaks=" + ("0" if sys.platform == "darwin" else "1")
            + ":abort_on_error=1"
        )
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        subprocess.run([str(binary)], cwd=root, env=environment, check=True)


if __name__ == "__main__":
    main()
