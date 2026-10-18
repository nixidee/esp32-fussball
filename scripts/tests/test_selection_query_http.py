"""Replay actual correlated selector routes/codecs at the HTTP boundary.

Controlled SDK/service adapters exercise missing/stale IDs, partial result
replacement, strict cancel bodies and receipts. Worker lifetime is tested
separately through the actual service, not inferred from these adapters.
"""
from pathlib import Path
import subprocess
import tempfile
from test_team_names_http import block

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "components/web/web_service.cpp").read_text()
code = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include "football_service.h"
#include "request_json.h"
#include "correlation_id.h"
using web::readSelection;
using web::readSelectionQuery;
using web::SettingsDecode;
using web::parseCorrelationId;
using web::correlationIdText;
struct httpd_req_t {std::string query,body,output;int status=0,chunks=0,final=0;std::size_t offset=SIZE_MAX,content_len=0;};
std::size_t body_read=0;
bool body_broken=false;
struct Request {explicit Request(httpd_req_t* req){body_read=0;body_broken=false;req->content_len=req->body.size();}};
struct Fault {int status;};
namespace fault {constexpr Fault kRejected{400},kChanged{409},kSelectionBusy{409},kJson{400},kBodyIncomplete{400};}
int error(httpd_req_t* req,const Fault& fault,const char* =nullptr,const json::ErrorInfo* info=nullptr){req->status=fault.status;if(info)req->offset=info->offset;return ESP_FAIL;}
int failed(httpd_req_t* req,int){req->status=500;return ESP_FAIL;}
int rejected(httpd_req_t* req,const settings::Violation&){req->status=422;return ESP_FAIL;}
const Fault* admitSession(httpd_req_t*){return nullptr;}
const Fault* checkJsonBody(httpd_req_t*){return nullptr;}
int readBody(httpd_req_t* req,char* out,std::size_t maximum){
 const auto count=std::min(maximum,req->body.size()-body_read);
 std::memcpy(out,req->body.data()+body_read,count);body_read+=count;return static_cast<int>(count);
}
// ACTUAL_JSON_BODY
bool sendReady(httpd_req_t*){return true;}
int httpd_resp_set_type(httpd_req_t*,const char*){return ESP_OK;}
int httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){return ESP_OK;}
int httpd_resp_send_chunk(httpd_req_t* req,const char* bytes,std::size_t size){
 ++req->chunks;if(!size){++req->final;return ESP_OK;}req->output.append(bytes,size);return ESP_OK;
}
int httpd_req_get_url_query_str(httpd_req_t* req,char* out,std::size_t size){
 if(req->query.empty()||req->query.size()>=size)return ESP_FAIL;
 std::memcpy(out,req->query.c_str(),req->query.size()+1);return ESP_OK;
}
int httpd_query_key_value(const char* query,const char* key,char* out,std::size_t size){
 const std::string prefix=std::string(key)+"=";
 if(std::string_view(query).substr(0,prefix.size())!=prefix)return ESP_FAIL;
 const auto value=std::string_view(query).substr(prefix.size());if(value.size()>=size)return ESP_FAIL;
 std::memcpy(out,value.data(),value.size());out[value.size()]=0;return ESP_OK;
}
namespace timekeeping {int64_t monotonicMs(){return 100;}}
namespace football {
SelectionQuery current=UINT64_C(0x1234567800000001);
bool busy=false;unsigned starts=0,cancels=0,reads=0;int break_at=-1;
esp_err_t select(const SelectionRequest&,SelectionQuery& query){++starts;if(busy)return ESP_ERR_NOT_FINISHED;query=current;return ESP_OK;}
esp_err_t selectionStatus(SelectionQuery query,SelectionStatus& out){
 ++reads;if(query!=current)return ESP_ERR_NOT_FOUND;
 out={};out.query=query;out.revision=7;out.count=8;return ESP_OK;
}
bool selectionAt(SelectionQuery query,uint16_t i,uint32_t revision,Selection& out){
 assert(query==UINT64_C(0x1234567800000001)&&revision==7);
 if(i==break_at){++current;return false;}out.id.assign(std::to_string(i+1));out.name.assign(std::string(64,'W'));return true;
}
esp_err_t cancelSelection(SelectionQuery query){++cancels;return query==current?ESP_OK:ESP_ERR_NOT_FOUND;}
const char* cadenceName(Cadence){return "none";}
}
'''
code = code.replace("// ACTUAL_JSON_BODY", block(source, "class JsonBody {") + ";\n")
code += block(source, "class Writer {") + ";\n"
code += "template<class Encode>\n" + block(source, "esp_err_t streamJson(")
code += block(source, "esp_err_t success(")
code += "template<class T,class Decode>\n" + block(source, "bool readCommand(")
for name in ["getSelection", "dataSelect", "cancelSelection"]:
    code += block(source, "esp_err_t " + name + "(")
code += r'''
int main(){
 for(const auto* query:{"","query=1","query=0000000000000000","query=123456780000000A","query=1234567800000002"}){
  httpd_req_t req;req.query=query;assert(getSelection(&req)==ESP_FAIL);
  assert(req.chunks==0&&req.output.empty()&&req.final==0);
  assert(req.status==(std::strlen(query)==22&&query[21]=='2'?409:400));
 }
 httpd_req_t good;good.query="query=1234567800000001";
 assert(getSelection(&good)==ESP_OK&&good.final==1&&good.output.find("\"query\":\"1234567800000001\"")!=good.output.npos);
 football::break_at=5;httpd_req_t replaced;replaced.query=good.query;
 assert(getSelection(&replaced)==ESP_FAIL&&replaced.final==0&&!replaced.output.empty());
 football::current=UINT64_C(0x1234567800000001);football::break_at=-1;
 httpd_req_t start;start.body=R"({"provider":0,"teams":false})";
 assert(dataSelect(&start)==ESP_OK&&start.final==1&&start.output.find("1234567800000001")!=start.output.npos);
 football::busy=true;httpd_req_t blocked;blocked.body=start.body;
 assert(dataSelect(&blocked)==ESP_FAIL&&blocked.status==409&&blocked.chunks==0);football::busy=false;
 for(const auto* body:{R"({"query":"1234567800000001"})",R"({"query":"1234567800000002"})"}){
  httpd_req_t cancel;cancel.body=body;const bool matching=cancel.body.find("00000001")!=cancel.body.npos;
  assert((cancelSelection(&cancel)==ESP_OK)==matching);
  assert(matching?cancel.final==1:cancel.status==409&&cancel.final==0);
 }
 for(const auto* body:{R"({"query":"1234567800000001","query":"1234567800000001"})",R"({"query":"1234567800000001"}false)",R"({"query":1})"}){
  const auto before=football::cancels;httpd_req_t invalid;invalid.body=body;
  assert(cancelSelection(&invalid)==ESP_FAIL&&football::cancels==before&&invalid.final==0);
  if(invalid.body.back()=='e')assert(invalid.offset==invalid.body.find("false"));
 }
 std::cout<<"selection HTTP: exact receipts/IDs, preflight stale rejection, no foreign stream completion, strict cancellation and explicit busy PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix="selection-query-http-") as temp:
    folder = Path(temp)
    (folder / "esp_err.h").write_text(
        "#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_FAIL=-1,"
        "ESP_ERR_NOT_FOUND=1,ESP_ERR_NOT_FINISHED=2;\n")
    cpp, binary = folder / "replay.cpp", folder / "replay"
    cpp.write_text(code)
    command = ["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-ffunction-sections",
               "-I" + str(folder)]
    for name in ["include", "components/json/include", "components/web",
                 "components/football/include", "components/settings/include", "components/timekeeping/include"]:
        command += ["-I" + str(ROOT / name)]
    for name in ["components/json/json_stream.cpp", "components/web/request_json.cpp",
                 "components/settings/settings_model.cpp", "components/timekeeping/civil_time.cpp",
                 "components/football/football_model.cpp", "components/football/demo_scenarios.cpp",
                 "components/football/match_store.cpp"]:
        command += [str(ROOT / name)]
    subprocess.run(command + [str(cpp), "-Wl,-dead_strip", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
