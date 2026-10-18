"""Actual GET/backup handlers must prepare names before starting HTTP output.

The names owner has its own real-file/OOM/index replay. Here only that owner,
session and settings adapters are controlled; handlers, writer and JSON are real.
"""
from pathlib import Path
import subprocess
import tempfile

from test_team_names_http import block

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "components/web/web_service.cpp").read_text()
code = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include "app_config.h"
#include "json_stream.h"
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=1;
struct httpd_req_t {std::string output; int chunks=0, final=0, failure=0;};
struct Request {explicit Request(httpd_req_t*){}};
bool held=false, busy=false, body_broken=false;
int prepare_result=ESP_OK, write_result=ESP_OK, live=0, prepared=0, written=0;
struct Fault {};
namespace fault {constexpr Fault kBusy{},kBodyIncomplete{},kJson{};}
namespace network {struct HeavyGuard {
 bool admitted=!busy;
 HeavyGuard(){if(admitted){assert(!held);held=true;}}
 ~HeavyGuard(){if(admitted)held=false;}
 explicit operator bool()const{return admitted;}
};}
namespace settings {
struct Violation {};
struct AppConfig {int model=0,competitions=0,screens=0;};
struct NamesRef {uint8_t slot=1;uint32_t generation=4,length=123,crc=42;};
NamesRef namesReference(){assert(held);return{};}
void snapshot(AppConfig&){assert(held);}
}
namespace football::names {struct AdoptedExport {
 bool ready=false;
 AdoptedExport(){++live;}
 ~AdoptedExport(){--live;}
 int prepare(){assert(held);++prepared;ready=prepare_result==ESP_OK;return prepare_result;}
 int write(json::Writer& out){
  assert(held&&ready);ready=false;++written;
  if(!out.object()||!out.field("payload",std::string(2048,'x')))return ESP_FAIL;
  if(write_result!=ESP_OK)return write_result;
  return out.end()?ESP_OK:ESP_FAIL;
 }
};}
bool sendReady(httpd_req_t*){assert(held);return true;}
int httpd_resp_set_type(httpd_req_t*,const char*){return ESP_OK;}
int httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){return ESP_OK;}
int httpd_resp_send_chunk(httpd_req_t* req,const char* bytes,std::size_t size){
 assert(held);++req->chunks;if(!size){++req->final;return ESP_OK;}
 req->output.append(bytes,size);return ESP_OK;
}
int error(httpd_req_t* req,const Fault&,const char* =nullptr,const json::ErrorInfo* =nullptr){req->failure=409;return ESP_FAIL;}
int failed(httpd_req_t* req,int failure){req->failure=failure;return ESP_FAIL;}
int rejected(httpd_req_t* req,const settings::Violation&){req->failure=422;return ESP_FAIL;}
const Fault* admitSession(httpd_req_t*){return nullptr;}
const Fault* checkJsonBody(httpd_req_t*){return nullptr;}
struct JsonBody {explicit JsonBody(httpd_req_t*){} int input(){return 0;}bool complete(){return true;}const json::ErrorInfo* diagnostic(){return nullptr;}};
enum class SettingsDecode {kOk,kViolation,kSyntax};
SettingsDecode exportOptions(int,bool&,settings::Violation*){return SettingsDecode::kOk;}
bool settingsJson(json::Writer&,int,bool){return true;}
bool competitionsJson(json::Writer& out,int){return out.key("competitions")&&out.array()&&out.end();}
bool screenPolicyJson(json::Writer&,int){return true;}
'''
code += block(source, "class Writer {") + ";\n"
code += "template<class Encode>\n" + block(source, "esp_err_t streamJson(")
code += block(source, "esp_err_t getAdoptedNames(")
code += block(source, "esp_err_t exportSettings(")
code += r'''
int main(){
 for(auto handler:{getAdoptedNames,exportSettings}){
  prepared=written=0;prepare_result=ESP_ERR_NO_MEM;write_result=ESP_OK;
  httpd_req_t oom;assert(handler(&oom)==ESP_FAIL&&oom.failure==ESP_ERR_NO_MEM);
  assert(oom.chunks==0&&oom.final==0&&oom.output.empty()&&written==0&&prepared==1&&!live&&!held);
  prepare_result=ESP_OK;httpd_req_t ok;
  assert(handler(&ok)==ESP_OK&&ok.final==1&&ok.failure==0&&!live&&!held);
  assert(ok.output.find("\"adopted_names\":{")!=std::string::npos);
  write_result=ESP_FAIL;httpd_req_t io;
  assert(handler(&io)==ESP_FAIL&&io.final==0&&!io.output.empty()&&!live&&!held);
  busy=true;httpd_req_t admission;
  const int previous=prepared;assert(handler(&admission)==ESP_FAIL&&prepared==previous);
  assert(admission.chunks==0&&!live&&!held);busy=false;
 }
 std::cout<<"GET/backup names: preflight OOM before bytes, complete success, partial I/O without success terminator and busy release PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix="web-names-export-") as temp:
    folder = Path(temp)
    cpp, binary = folder / "replay.cpp", folder / "replay"
    cpp.write_text(code)
    subprocess.run([
        "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I" + str(ROOT / "include"),
        "-I" + str(ROOT / "components/json/include"), str(cpp),
        str(ROOT / "components/json/json_stream.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
