#!/usr/bin/env python3
"""Actual originals HTTP handler/channel; SDK socket and provider are seams."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_original_names_channel import FREERTOS, EVENTS

ROOT = Path(__file__).resolve().parents[2]
web = (ROOT / "components/web/web_service.cpp").read_text()
peer = web[web.index("bool peerConnected("):web.index("// Explicit protected query.")]
handler = web[web.index("esp_err_t queryOriginalNames("):web.index("// The running app owns the immutable source.")]
number = web[web.index("bool parseUnsigned("):web.index("[[gnu::noinline]] bool passwordSet()")]
prelude = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include "original_names_channel.h"
int64_t esp_timer_get_time(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
namespace timekeeping{int64_t monotonicMs(){return esp_timer_get_time()/1000;}}
using settings::Reason;
constexpr int MSG_PEEK=1,MSG_DONTWAIT=2;
enum class Mode{good,provider_error,send_error,closed,pipeline};
Mode mode=Mode::good;std::thread worker;std::string document;unsigned started=0;
struct httpd_req_t{bool authorized=true;std::string query="competition=7",body;unsigned sends=0,finals=0,peeks=0;};
httpd_req_t*current=nullptr;
int64_t request_deadline=0;
struct Request{explicit Request(httpd_req_t*req){current=req;request_deadline=timekeeping::monotonicMs()+cfg::kOperationDeadlineMs;}};
struct Fault{};namespace fault{Fault kBusy;}
const Fault*admitSession(httpd_req_t*req){static Fault denied;return req->authorized?nullptr:&denied;}
esp_err_t error(httpd_req_t*,const Fault&){return 401;}
bool readEmptyCommand(httpd_req_t*,esp_err_t&){return true;}
esp_err_t invalid(httpd_req_t*,Reason,const char*,const char*){return 400;}
esp_err_t failed(httpd_req_t*,esp_err_t,const char* = nullptr);
esp_err_t failed(httpd_req_t*,esp_err_t,const char*){return 500;}
int httpd_req_get_url_query_str(httpd_req_t*req,char*out,size_t cap){if(req->query.empty()||req->query.size()>=cap)return ESP_FAIL;std::memcpy(out,req->query.c_str(),req->query.size()+1);return ESP_OK;}
int httpd_query_key_value(const char*query,const char*key,char*out,size_t cap){std::string prefix=std::string(key)+"=";if(!std::string_view(query).starts_with(prefix)||std::strlen(query+prefix.size())>=cap)return ESP_FAIL;std::strcpy(out,query+prefix.size());return ESP_OK;}
int httpd_req_to_sockfd(httpd_req_t*){return 17;}
int recv(int fd,char*,size_t,int flags){assert(fd==17&&flags==(MSG_PEEK|MSG_DONTWAIT));current->peeks++;if(mode==Mode::closed)return 0;if(mode==Mode::pipeline)return 1;errno=EWOULDBLOCK;return -1;}
void httpd_resp_set_type(httpd_req_t*,const char*){}
void httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){}
bool sendReady(httpd_req_t*){return timekeeping::monotonicMs()<request_deadline;}
int httpd_resp_send_chunk(httpd_req_t*req,const char*bytes,size_t count){if(!count){req->finals++;return ESP_OK;}req->sends++;if(mode==Mode::send_error)return ESP_FAIL;req->body.append(bytes,count);return ESP_OK;}
namespace application{
esp_err_t beginOriginalNames(uint8_t competition,uint32_t&query){
 if(competition!=7)return ESP_ERR_NOT_FOUND;
 settings::Source source;source.competition.assign("bl1");source.season.assign("2026");
 auto error=football::originals::begin(source,query);if(error!=ESP_OK)return error;++started;
 worker=std::thread([id=query]{settings::Source source;assert(football::originals::claim(id,source));
  if(mode==Mode::closed){while(football::originals::active(id))std::this_thread::yield();football::originals::complete(id,ESP_OK);return;}
  const bool sent=football::originals::write(id,document);
  football::originals::complete(id,sent&&mode!=Mode::provider_error?ESP_OK:ESP_FAIL);
 });return ESP_OK;
}
}
'''
tests = r'''
int main(){
 document="{\"items\":[{\"id\":40,\"name\":\""+std::string(20000,'A')+"\"}]}";
 httpd_req_t req;mode=Mode::good;assert(queryOriginalNames(&req)==ESP_OK);worker.join();assert(req.body==document&&req.finals==1&&req.peeks>1);
 req={};mode=Mode::pipeline;assert(queryOriginalNames(&req)==ESP_OK);worker.join();assert(req.body==document&&req.finals==1&&req.peeks>1); // peek never consumes pipeline bytes
 req={};mode=Mode::provider_error;document="{\"items\":[{\"name\":\""+std::string(20000,'B');assert(queryOriginalNames(&req)==ESP_FAIL);worker.join();assert(!req.body.empty()&&req.finals==0);
 req={};mode=Mode::send_error;assert(queryOriginalNames(&req)==ESP_FAIL);worker.join();assert(req.sends==1&&req.finals==0);
 req={};mode=Mode::closed;auto began=timekeeping::monotonicMs();assert(queryOriginalNames(&req)==ESP_FAIL);worker.join();assert(req.sends==0&&req.finals==0&&timekeeping::monotonicMs()-began<1000);
 const auto before=started;
 for(const char*query:{"","competition=0","competition=256","competition=x"}){req={};req.query=query;assert(queryOriginalNames(&req)==400);assert(started==before);}
 req={};req.authorized=false;assert(queryOriginalNames(&req)==401&&started==before);
 req={};req.query="competition=9";assert(queryOriginalNames(&req)==500&&started==before);
 std::puts("actual originals HTTP/channel: complete stream, partial failure, send failure, close before data, pipelining, admission PASS");
}
'''
with tempfile.TemporaryDirectory(prefix="football-original-http-") as folder:
    scratch = Path(folder)
    (scratch / "freertos").mkdir()
    (scratch / "freertos/FreeRTOS.h").write_text(FREERTOS)
    (scratch / "freertos/event_groups.h").write_text(EVENTS)
    (scratch / "esp_err.h").write_text(
        "#pragma once\nusing esp_err_t=int;\ninline constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_ARG=0x102,"
        "ESP_ERR_INVALID_STATE=0x103,ESP_ERR_NO_MEM=0x101,ESP_ERR_TIMEOUT=0x107,ESP_ERR_INVALID_RESPONSE=0x108,"
        "ESP_ERR_NOT_FOUND=0x105,ESP_ERR_NOT_FINISHED=0x109;\n")
    (scratch / "esp_timer.h").write_text("#pragma once\n#include<cstdint>\nint64_t esp_timer_get_time();\n")
    source = scratch / "http.cpp"
    source.write_text(prelude + number + peer + handler + tests)
    command = ["clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread"]
    includes = [scratch, ROOT / "include", ROOT / "components/football"]
    includes.extend((ROOT / "components").glob("*/include"))
    command += ["-I" + str(path) for path in includes]
    command += [str(source), str(ROOT / "components/football/original_names_channel.cpp"), "-o", str(scratch / "http")]
    subprocess.run(command, check=True)
    subprocess.run([str(scratch / "http")], check=True, timeout=20,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
