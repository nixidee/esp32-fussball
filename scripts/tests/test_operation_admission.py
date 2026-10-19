#!/usr/bin/env python3
"""Actual exclusive owner and provider request lifetime with controlled HTTP."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'components/football/football_service.cpp').read_text()
start = source.index('RequestOutcome get(const provider::RequestSpec&')
end = source.index('\n// Canonical mapping', start)
request = source[start:end]
prelude = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>
#include "operation_admission.h"
#include "provider_client.h"
static_assert(!std::is_copy_constructible_v<operations::Guard> &&
              !std::is_copy_assignable_v<operations::Guard> &&
              !std::is_move_constructible_v<operations::Guard>);
namespace settings {unsigned gates=0;bool pause=false,pause_after_check=false;
 bool footballPaused(){return pause||(pause_after_check&&++gates>1);}}
namespace timekeeping {int64_t monotonicMs(){return 1000;}}
const char* esp_err_to_name(esp_err_t){return "controlled";}
bool transport_done=false;unsigned diagnostic_checks=0;
template<class... T> void diagnostic(const T&...){if(transport_done){operations::Guard probe;assert(!probe);++diagnostic_checks;}}
#define ESP_LOGW(...) diagnostic(__VA_ARGS__)
#define ESP_LOGD(...) diagnostic(__VA_ARGS__)
#define ESP_LOGI(...) diagnostic(__VA_ARGS__)
namespace football {
enum class HttpPurpose {kAutomatic,kSetup,kDiscovery};
std::atomic<uint32_t> original_query=0;
namespace originals {uint64_t deadlineMs(uint32_t){return 2000;}}
FetchError fetchErrorOf(esp_err_t){return FetchError::kOther;}
struct Interrupted {const char* why="changed";Cause cause=Cause::kSettings;bool active=false;explicit operator bool()const{return active;}};
bool changed=false;
Interrupted interrupted(uint32_t,uint32_t){return {"changed",Cause::kSettings,changed};}
struct SendContext {cfg::Provider provider;const settings::ProviderSettings& model;uint32_t generation,settings_generation,epoch;HttpPurpose purpose;uint8_t competition;RequestOutcome& outcome;};
esp_err_t beginSend(void* raw){auto& result=static_cast<SendContext*>(raw)->outcome;result.sent=true;result.sent_ms=1000;return ESP_OK;} void endSend(void*){}
unsigned sends=0;int mode=0;
esp_err_t fetchProvider(const provider::RequestSpec&,const provider::Sink&,const settings::ProviderSettings&,uint32_t,provider::ParseResult& parsed,FetchResult* result,const SendHooks* hooks){
 operations::Guard probe;assert(!probe);++sends;transport_done=true;parsed.metadata.season.assign("2026");
 assert(hooks&&hooks->begin&&hooks->end);
 assert(hooks->deadline_ms==(original_query?2000:0));
 result->status=mode==1?503:200;result->paused=mode==2;
 if(mode==3){result->admission_error=ESP_ERR_INVALID_STATE;static_cast<SendContext*>(hooks->context)->outcome.fail(ESP_ERR_INVALID_STATE,Cause::kSettings);}
 else if(mode!=2)hooks->begin(hooks->context);
 if(mode==4)changed=true;
 return mode==1||mode==4?ESP_FAIL:ESP_OK;
}
'''
main = r'''
}
int main(){using namespace football;
 settings::ProviderSettings model;provider::RequestSpec spec;spec.source={cfg::Provider::kOpenLigaDb,"bl1","2026"};provider::Sink sink;
 const auto reset=[](int failure){mode=failure;sends=0;diagnostic_checks=0;transport_done=false;changed=false;settings::gates=0;settings::pause=settings::pause_after_check=false;original_query=0;};
 RequestOutcome last;
 const auto run=[&](const operations::Guard* borrowed=nullptr,HttpPurpose purpose=HttpPurpose::kAutomatic){const auto result=get(spec,model,1,1,1,sink,purpose,0,borrowed);last=result;return result.code;};
 for(int failure=0;failure<5;++failure){
  reset(failure);const auto error=run();assert(sends==1);
  assert((failure==0)==(error==ESP_OK));
  assert(std::string_view(last.parsed.metadata.season.data)=="2026");
  assert(last.sent==(failure!=2&&failure!=3));
  assert(last.http==(failure==1?503:0));
  assert(last.sent_ms==(last.sent?1000:0));
  assert(diagnostic_checks>0||failure==4);transport_done=false;
  operations::Guard available;assert(available);
 }
 const auto preserved=last;
 reset(0);settings::pause=true;assert(run()!=ESP_OK&&!last.sent&&last.http==0&&last.sent_ms==0&&last.cause==Cause::kBusy);
 assert(preserved.sent&&preserved.http==0&&preserved.sent_ms==1000);
 reset(0);
 {operations::Guard outer;assert(outer);assert(run(&outer)==ESP_OK);transport_done=false;operations::Guard still_held;assert(!still_held);}
 {operations::Guard available;assert(available);}
 reset(0);
 {operations::Guard other;assert(other);assert(run()!=ESP_OK&&sends==0);operations::Guard refused;assert(!refused);assert(run(&refused)!=ESP_OK&&sends==0);operations::Guard still_held;assert(!still_held);}
 {operations::Guard available;assert(available);}
 reset(0);settings::pause_after_check=true;assert(run()!=ESP_OK&&sends==0);
 {operations::Guard available;assert(available);}
 reset(0);changed=true;assert(run()!=ESP_OK&&sends==0);
 {operations::Guard available;assert(available);}
 reset(0);spec.source.provider=cfg::Provider::kApiFootball;assert(run()!=ESP_OK&&sends==0&&last.cause==Cause::kProviderOff);spec.source.provider=cfg::Provider::kOpenLigaDb;
 reset(0);settings::pause=true;original_query=7;assert(run(nullptr,HttpPurpose::kSetup)==ESP_OK&&sends==1);transport_done=false;
 {operations::Guard available;assert(available);}
 std::atomic<unsigned> owners=0,acquired=0;std::atomic<bool> start=false;
 std::vector<std::thread> threads;
 for(unsigned i=0;i<4;++i)threads.emplace_back([&]{while(!start.load())std::this_thread::yield();for(unsigned pass=0;pass<2000;++pass){operations::Guard guard;if(!guard)continue;assert(owners.fetch_add(1)==0);++acquired;std::this_thread::yield();assert(owners.fetch_sub(1)==1);}});
 start=true;for(auto& thread:threads)thread.join();assert(acquired>0&&owners==0);
 {operations::Guard available;assert(available);}
 std::puts("PASS actual operation owner: exclusive contention, refused destructor preserves owner, provider own/borrowed lifetime, busy/paused/stale/off/admission/fetch returns, interactive deadline and admission held through diagnostics; no target timing claim");
}
'''
with tempfile.TemporaryDirectory(prefix='operation-owner-',dir='/tmp') as directory:
    folder=Path(directory)
    (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\ninline constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=1,ESP_ERR_TIMEOUT=2;\n')
    unit=folder/'check.cpp';unit.write_text(prelude+request+main)
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I'+str(folder),'-I'+str(ROOT/'include')]
    command+=['-I'+str(p) for p in (ROOT/'components').glob('*/include')]
    files=('system/operation_admission.cpp','football/football_model.cpp','settings/settings_model.cpp','settings/screen_policy.cpp','network/wifi_policy.cpp','timekeeping/civil_time.cpp')
    command += [str(unit),*(str(ROOT/'components'/name) for name in files),'-o',str(folder/'check')]
    subprocess.run(command,check=True)
    subprocess.run([str(folder/'check')],check=True)
