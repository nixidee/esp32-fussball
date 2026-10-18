#!/usr/bin/env python3
"""Actual D3 owner/encoder/selection parser/channel; HTTP/transport are seams.

No provider network, HTTP server or target scheduler acceptance is implied.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from test_original_names_channel import FREERTOS, EVENTS

ROOT = Path(__file__).resolve().parents[2]
service = (ROOT / "components/football/football_service.cpp").read_text()
owner = service[service.index("struct OriginalsRun {"):service.index("// Why nothing runs now")]
begin = service[service.index("esp_err_t beginOriginalNames("):service.index("\nesp_err_t selectionStatus(")]
prelude = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include "original_names_channel.h"
#include "provider_adapter.h"
#include "provider_settings.h"
int64_t esp_timer_get_time(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
namespace settings {
uint32_t current_generation=1,current_data=1;
Competition entry;
uint32_t generation(){return current_generation;}
uint32_t dataGeneration(){return current_data;}
bool competitionById(uint8_t id,Competition&out){if(id!=entry.id)return false;out=entry;return true;}
}
namespace network {struct Status {uint32_t epoch=1;};Status status(){return {};}}
namespace ota {bool trial(){return false;}}
namespace timekeeping {int64_t monotonicMs(){return esp_timer_get_time()/1000;}}
namespace football {
std::mutex owned;std::mutex*mutex=&owned;
void lock(){owned.lock();}void unlock(){owned.unlock();}
bool reset_fence=false;
std::atomic<uint32_t> original_query{0};
std::atomic<bool> selecting=false,busy=false;
uint32_t original_generation=0,original_settings_generation=0,original_epoch=0;
struct {uint32_t generation=0,epoch=0;bool active=false;}run;
struct {uint32_t hold=0;uint32_t admissibleAt(cfg::Provider,uint32_t now){return hold?hold:now;}}scheduler;
enum class HttpPurpose{kSetup};
namespace plan {enum class Fetch{kBusy,kDone};}
plan::Fetch fetchOf(esp_err_t error){return error==ESP_ERR_NOT_FINISHED?plan::Fetch::kBusy:plan::Fetch::kDone;}
std::string response;bool busy_once=false;unsigned requests=0;
esp_err_t get(const provider::RequestSpec&request,const settings::ProviderSettings&,uint32_t generation,
 uint32_t settings_generation,uint32_t,const provider::Sink&sink,provider::ParseResult&parsed,HttpPurpose){
 assert(generation==original_generation&&settings_generation==original_settings_generation);
 if(busy_once){busy_once=false;return ESP_ERR_NOT_FINISHED;}++requests;
 const std::string season=R"([{"matchID":1,"leagueShortcut":"bl1","leagueSeason":2026,"team1":{"teamId":40,"teamName":"FC"},"team2":{"teamId":41,"teamName":"SV"}}])";
 const auto&body=request.operation==provider::Operation::kCurrentSeason?season:response;
 struct Input {const std::string&text;std::size_t at=0;const provider::Sink&sink;}input{body,0,sink};
 json::Reader reader({&input,[](void*p){auto&v=*static_cast<Input*>(p);
  if(v.sink.cancelled&&v.sink.cancelled(v.sink.context))return -1;
  return v.at<v.text.size()?static_cast<unsigned char>(v.text[v.at++]):-1;}});
 parsed=provider::readResponse(reader,request,sink);
 return parsed.status==provider::Status::kOk&&parsed.document_complete?ESP_OK:ESP_ERR_INVALID_RESPONSE;
}
'''
tests = r'''
uint32_t start(std::string body,bool season=true){
 response=std::move(body);settings::entry={};settings::entry.id=7;
 settings::entry.primary.provider=cfg::Provider::kOpenLigaDb;
 assert(settings::entry.primary.competition.assign("bl1"));
 if(season)assert(settings::entry.primary.season.assign("2026"));
 uint32_t id=0;assert(beginOriginalNames(7,id)==ESP_OK&&selecting&&id);return id;
}
void worker(){settings::ProviderSettings model;int64_t wake=0;while(selecting){
 stepOriginalNames(model,settings::dataGeneration(),settings::generation(),wake);}}
std::pair<originals::State,std::string>consume(uint32_t id){
 originals::Read part;std::string body;char bytes[37];
 do{part=originals::read(id,bytes,sizeof(bytes),100);body.append(bytes,part.bytes);}
 while(part.state==originals::State::kPending||part.state==originals::State::kStreaming);
 originals::release(id);return {part.state,body};
}
bool jsonDocument(std::string_view body){struct Input{std::string_view text;size_t at=0;}input{body};
 json::Reader reader({&input,[](void*p){auto&v=*static_cast<Input*>(p);return v.at<v.text.size()?static_cast<unsigned char>(v.text[v.at++]):-1;}});
 json::Token token;while(reader.next(token)&&token.kind!=json::Kind::kEnd){}return reader.finish();}
}
int main(){using namespace football;
 // Full source text, non-ASCII, quote escaping and nullable short values survive.
 std::string label;for(int i=0;i<6000;i++)label+="ä😀";
 auto id=start("[{\"teamId\":40,\"teamName\":\""+label+"\",\"shortName\":null},{\"teamName\":\"B\\\" C\",\"teamId\":41,\"shortName\":\"\"}]");
 std::thread producer(worker);auto [state,body]=consume(id);producer.join();
 assert(state==originals::State::kComplete&&jsonDocument(body)&&body.find(label)!=std::string::npos);
 assert(body.find("\"short_name\":\"\"")!=std::string::npos&&body.find("\"short_name\":null")==std::string::npos);
 assert(!selecting&&!original_query&&body.find("\"season\":\"2026\"")!=std::string::npos);
 // A valid row followed by malformed transport data is never a complete answer.
 id=start("[{\"teamId\":40,\"teamName\":\""+label+"\"}] trailing");
 producer=std::thread(worker);auto broken=consume(id);producer.join();
 assert(broken.first==originals::State::kFailed&&!jsonDocument(broken.second));
 // Missing identity and a NUL beyond the retained prefix cannot be adopted.
 for(const auto&bad:{std::string("[{\"teamName\":\"missing id\"}]"), "[{\"teamId\":40,\"teamName\":\""+label+"\\u0000\"}]"}){
 id=start(bad);producer=std::thread(worker);broken=consume(id);producer.join();
 assert(broken.first==originals::State::kFailed&&!jsonDocument(broken.second));}
 // Cancellation before claim, stale settings and refused first admission send nothing.
 auto before=requests;id=start("[]");originals::cancel(id);worker();assert(requests==before&&!selecting);
 id=start("[]");settings::current_generation++;worker();assert(requests==before);assert(consume(id).first==originals::State::kFailed);
 id=start("[]");busy_once=true;settings::ProviderSettings model;int64_t wake=0;
 assert(!stepOriginalNames(model,settings::dataGeneration(),settings::generation(),wake));assert(requests==before&&selecting);
 producer=std::thread(worker);auto empty=consume(id);producer.join();assert(empty.first==originals::State::kComplete&&jsonDocument(empty.second));
 // Season discovery uses the same owner before its one explicit list request.
 before=requests;id=start("[]",false);producer=std::thread(worker);empty=consume(id);producer.join();
 assert(requests==before+2&&empty.first==originals::State::kComplete&&empty.second.find("2026")!=std::string::npos);
 // Cancel while producing a large label; slot releases only after worker ACK.
 id=start("[{\"teamId\":40,\"teamName\":\""+label+"\"}]");producer=std::thread(worker);
 char prefix[1];assert(originals::read(id,prefix,1,100).bytes);originals::cancel(id);producer.join();assert(!selecting&&!original_query);
 std::puts("actual originals owner+encoder+parser+channel: 8 integration scenarios PASS; no network/device claim");
}
'''

with tempfile.TemporaryDirectory(prefix="football-original-integration-") as folder:
    scratch = Path(folder)
    (scratch / "freertos").mkdir()
    (scratch / "freertos/FreeRTOS.h").write_text(FREERTOS)
    (scratch / "freertos/event_groups.h").write_text(EVENTS)
    (scratch / "esp_err.h").write_text(
        "#pragma once\nusing esp_err_t=int;\ninline constexpr int ESP_OK=0,ESP_FAIL=-1,"
        "ESP_ERR_INVALID_ARG=0x102,ESP_ERR_INVALID_STATE=0x103,ESP_ERR_NO_MEM=0x101,"
        "ESP_ERR_TIMEOUT=0x107,ESP_ERR_INVALID_RESPONSE=0x108,ESP_ERR_NOT_FINISHED=0x109,ESP_ERR_NOT_FOUND=0x105;\n")
    (scratch / "esp_timer.h").write_text("#pragma once\n#include<cstdint>\nint64_t esp_timer_get_time();\n")
    fixture = scratch / "originals.cpp"
    fixture.write_text(prelude + owner + begin + tests)
    includes = [scratch, ROOT / "include", ROOT / "components/football"]
    includes.extend((ROOT / "components").glob("*/include"))
    command = ["clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread"]
    command.extend("-I" + str(path) for path in includes)
    command += [str(fixture), str(ROOT / "components/football/provider_selection.cpp"),
                str(ROOT / "components/football/provider_adapter.cpp"),
                str(ROOT / "components/football/provider_readers.cpp"),
                str(ROOT / "components/football/football_model.cpp"),
                str(ROOT / "components/timekeeping/civil_time.cpp"),
                str(ROOT / "components/football/match_store.cpp"),
                str(ROOT / "components/football/matchday.cpp"),
                str(ROOT / "components/football/original_names_channel.cpp"),
                str(ROOT / "components/json/json_stream.cpp"), "-o", str(scratch / "originals")]
    subprocess.run(command, check=True)
    subprocess.run([str(scratch / "originals")], check=True, timeout=20,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
