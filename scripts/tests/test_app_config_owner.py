#!/usr/bin/env python3
"""Production aggregate/NVS-owner checks with syscall faults, never presets.

The source body is compiled without its include/preset expansion. NVS and task
locks are controlled adapters; serialization, validation and publication are real.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def block(source, signature):
    start = source.index(signature)
    depth = 0
    for index in range(source.index("{", start), len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start:index + 1]
    raise RuntimeError(signature)

source = (ROOT / "components/settings/settings_store.cpp").read_text()
source = source[source.index("namespace settings {"):]
source = source.replace(block(source, "Presets presets()"),
                        "Presets presets() { return {}; }")

stubs = r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=1,
 ESP_ERR_INVALID_ARG=2,ESP_ERR_INVALID_STATE=3,ESP_ERR_INVALID_RESPONSE=4,
 ESP_ERR_TIMEOUT=5,ESP_ERR_NOT_ALLOWED=6,ESP_ERR_INVALID_SIZE=7,
 ESP_ERR_NVS_NOT_FOUND=8,ESP_ERR_NVS_NO_FREE_PAGES=9,
 ESP_ERR_NVS_NEW_VERSION_FOUND=10,ESP_ERR_NVS_REMOVE_FAILED=11,ESP_ERR_NVS_NOT_ENOUGH_SPACE=13;
inline const char* esp_err_to_name(int){return "controlled";}
inline std::vector<std::string> test_messages;
template<class... T>void testLog(const char*,const char* message,const T&...){test_messages.emplace_back(message);}
#define ESP_LOGE(...) testLog(__VA_ARGS__)
#define ESP_LOGI(...) testLog(__VA_ARGS__)
#define ESP_LOGW(...) testLog(__VA_ARGS__)
struct StaticSemaphore_t { std::mutex mutex; std::condition_variable cv; bool held=false; unsigned waiters=0; };
using SemaphoreHandle_t=StaticSemaphore_t*;
constexpr unsigned portMAX_DELAY=UINT32_MAX,pdTRUE=1;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* value){value->held=false;return value;}
inline unsigned xSemaphoreTake(SemaphoreHandle_t value,unsigned wait){
 std::unique_lock lock(value->mutex);
 if(value->held&&wait==0)return 0;
 ++value->waiters;value->cv.notify_all();
 value->cv.wait(lock,[&]{return !value->held;});--value->waiters;
 value->held=true;return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t value){std::lock_guard lock(value->mutex);assert(value->held);value->held=false;value->cv.notify_all();}
namespace events {enum class Event {kSettingsChanged}; inline unsigned count=0;inline int post(Event){++count;return 0;}}
using nvs_handle_t=int;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
std::map<std::string,std::vector<uint8_t>> durable;
unsigned fault=0,writes=0; bool mutated=false,fail_open=false;
inline int nvs_flash_init(){return ESP_OK;}
inline int nvs_flash_erase(){durable.clear();return ESP_OK;}
inline int storage_error=ESP_OK;inline unsigned fill_writes=0;
inline int nvs_open(const char* name,int,nvs_handle_t* out){if(fail_open)return ESP_FAIL;*out=std::strcmp(name,"fill_test")==0?2:1;return ESP_OK;}
inline void nvs_close(nvs_handle_t){}
inline int nvs_set_blob(nvs_handle_t handle,const char* key,const void* data,std::size_t size){
 if(handle==2){++fill_writes;return storage_error==ESP_OK?ESP_ERR_NVS_NOT_ENOUGH_SPACE:storage_error;}
 if(storage_error!=ESP_OK){++writes;return storage_error;}
 ++writes;mutated=true;if(fault==1)return ESP_FAIL;
 const auto* bytes=static_cast<const uint8_t*>(data);durable[key]={bytes,bytes+size};
 return fault==2?ESP_ERR_NVS_REMOVE_FAILED:ESP_OK;
}
inline int nvs_commit(nvs_handle_t){return fault==3?ESP_FAIL:ESP_OK;}
inline int nvs_get_blob(nvs_handle_t,const char* key,void* out,std::size_t* size){
 const auto found=durable.find(key);if(found==durable.end())return ESP_ERR_NVS_NOT_FOUND;
 if(out&&mutated&&fault==5)return ESP_FAIL;
 if(!out){*size=found->second.size();return ESP_OK;}
 if(*size<found->second.size())return ESP_ERR_INVALID_SIZE;
 *size=found->second.size();std::memcpy(out,found->second.data(),*size);
 if(mutated&&fault==4)static_cast<uint8_t*>(out)[*size-1]^=1;
 return ESP_OK;
}
inline int nvs_erase_all(nvs_handle_t handle){if(handle==1)durable.clear();else fill_writes=0;return ESP_OK;}
inline int nvs_set_u8(nvs_handle_t,const char*,uint8_t){return storage_error==ESP_OK?ESP_ERR_NVS_NOT_ENOUGH_SPACE:storage_error;}
struct nvs_stats_t{unsigned used_entries=0,available_entries=0,total_entries=0;};
inline int nvs_get_stats(const char*,nvs_stats_t*){return ESP_OK;}
inline void vTaskDelay(unsigned){}
#define pdMS_TO_TICKS(value) (value)
'''

main = r'''
namespace settings {
void rebootTestOwner(){
 generation_counter=0;data_generation_counter=0;
 record_unread=false;current_source=Origin::kInitial;
 init();
}
}
using namespace settings;
AppConfig observed;
void boot(){fault=0;mutated=false;fail_open=false;rebootTestOwner();snapshot(observed);}
AppConfig populated(){
 AppConfig next; snapshot(next);
 next.model.hostname.assign("coherent");
 Competition entry; entry.id=1;entry.incarnation=1;
 entry.primary.competition.assign("bl1"); entry.club.team=40;
 next.competitions.entries.push(entry);next.competitions.next_id=2;next.competitions.next_incarnation=2;
 next.last_apply_id=UINT64_C(0xfedcba9876543210);
 return next;
}
void recordChecks(){
 durable.clear();boot();auto next=populated();next.revision=77;next.names={2,17,321,0x12345678};
 std::array<uint8_t,app_record::kMaxBytes> bytes{},second{};
 const auto size=app_record::encode(next,bytes);assert(size>0);
 AppConfig read;
 assert(app_record::decode(std::span(bytes).first(size),read)==DecodeResult::kOk);
 assert(read.model==next.model&&read.competitions==next.competitions&&read.screens==next.screens&&read.names==next.names&&read.revision==77&&read.last_apply_id==next.last_apply_id);
 assert(app_record::encode(read,second)==size&&bytes==second);
 for(std::size_t length=0;length<size;++length)
   assert(app_record::decode(std::span(bytes).first(length),read)!=DecodeResult::kOk);
 for(std::size_t offset=0;offset<size;++offset){auto bad=bytes;bad[offset]^=0x80;
   assert(app_record::decode(std::span(bad).first(size),read)!=DecodeResult::kOk);}
 auto bad=bytes;bad[8+12]=3;app_record::seal(std::span(bad).first(size));
 assert(app_record::decode(std::span(bad).first(size),read)==DecodeResult::kInvalidValue);
 next.competitions.entries[0].display.overrides[0][0].fields=128;
 assert(app_record::encode(next,bytes)==0);
 next.competitions.entries[0].display.overrides[0][0].fields=0;
 for(uint8_t id=2;id<=cfg::kCompetitionMax;++id){
   auto entry=next.competitions.entries[0];entry.id=id;entry.incarnation=id;
   entry.primary.competition.assign("league-" + std::to_string(id));
   next.competitions.entries.push(entry);
 }
 next.competitions.next_id=cfg::kCompetitionMax+1;
 next.competitions.next_incarnation=cfg::kCompetitionMax+1;
 assert(app_record::encode(next,bytes)==app_record::kMaxBytes);
 assert(app_record::decode(bytes,read)==DecodeResult::kOk&&read.competitions==next.competitions);

 std::cout<<"record: roundtrip, every truncation/byte corruption, invalid names/attached preferences; max="<<app_record::kMaxBytes<<'\n';
}
void runtimeProjectionChecks(){
 durable.clear();boot();auto next=populated();
 next.model.time_zone.assign(cfg::kTimeZones[1].label);
 next.model.debug_status_log=true;next.model.debug_status_interval_s=cfg::kDebugStatusIntervalMinS;
 next.model.ntp_server.assign("time.example");next.model.ap_password.assign("12345678");
 next.model.wifi_password.assign("12345678");next.model.external_antenna=!cfg::kExternalAntennaDefault;
 next.model.admin_password.assign(std::string(cfg::kAdminPasswordMaxBytes,'w'));
 next.model.browser_debug=true;next.model.language=1;
 for(unsigned byte=0;byte<256;++byte){
   next.model.wifi_ssid.assign(std::string(32,static_cast<char>(byte)));
   assert(apply(next,next.revision)==ESP_OK);
   const auto network=networkSettings();
   assert(network.wifi_ssid==next.model.wifi_ssid&&network.wifi_password==next.model.wifi_password);
   assert(network.ap_password==next.model.ap_password&&network.hostname==next.model.hostname);
   assert(network.ntp_server==next.model.ntp_server&&network.external_antenna==next.model.external_antenna);
   const auto diagnostic=diagnosticSettings();
   assert(diagnostic.enabled&&diagnostic.interval_s==cfg::kDebugStatusIntervalMinS);
   uint32_t web_revision;const auto web=webSettings(&web_revision);
   assert(web_revision==next.revision);
   assert(web.hostname==next.model.hostname&&web.admin_password==next.model.admin_password);
   assert(web.browser_debug==next.model.browser_debug&&web.language==next.model.language);
   assert(&clockZone()==&cfg::kTimeZones[1]);
 }
 std::cout<<"runtime projections:256 raw SSIDs including NUL/invalidUTF8, coherent radio/host/NTP/diagnostics/web/clock PASS; web="<<sizeof(WebSettings)<<'\n';
}
void providerProjectionChecks(){
 durable.clear();boot();auto next=populated();
 next.model.api_football_key.assign(std::string(cfg::kApiFootballKeyMaxBytes,'a'));
 next.model.football_data_key.assign(std::string(cfg::kFootballDataKeyMaxBytes,'b'));
 next.model.espn_opt_in=true;next.model.demo=true;next.model.global_limit.daily=123;
 assert(apply(next,next.revision)==ESP_OK);
 ProviderSettings provider;CompetitionRoutes routes;uint32_t configuration=0,data=0;
 screen_policy::Eligibility rules;
 footballSettings(provider,routes,configuration,data,&rules,
   [](const ProviderSettings& snapshot,CompetitionRoutes& routes,bool saved){
     assert(saved&&snapshot.demo&&snapshot.espn_opt_in&&routes.entries.size()==1);
   });
 assert(configuration==generation()&&data==dataGeneration());
 assert(provider.api_football_key==next.model.api_football_key&&provider.football_data_key==next.model.football_data_key);
 assert(provider.providers==next.model.providers&&provider.global_limit==next.model.global_limit);
 assert(routes.entries[0].id==1&&routes.entries[0].club.team==40);
 for(unsigned value=0;value<cfg::kProviderCount;++value){
   const auto id=static_cast<cfg::Provider>(value);assert(providerUsable(id,provider)==providerUsable(id,next.model));
 }
 const auto af=football::provider::authentication(cfg::Provider::kApiFootball,provider);
 const auto fd=football::provider::authentication(cfg::Provider::kFootballData,provider);
 assert(std::string_view(af.header)=="x-apisports-key"&&af.value==provider.api_football_key.view());
 assert(std::string_view(fd.header)=="X-Auth-Token"&&fd.value==provider.football_data_key.view());
 assert(af.value.data()==provider.api_football_key.data.data()&&fd.value.data()==provider.football_data_key.data.data());
 next.model.hostname.assign("unrelated-view-change");assert(apply(next,next.revision)==ESP_OK);
 ProviderSettings after;footballSettings(after,routes,configuration,data);
 assert(after.api_football_key==provider.api_football_key&&after.providers==provider.providers&&after.demo);
 std::cout<<"provider projection: actual coherent owner getter, max96/64-byte keys, access/auth borrows, plans/global limit and callback PASS; size="<<sizeof(ProviderSettings)<<'\n';
}
void ownerChecks(){
 durable.clear();writes=0;boot();auto next=populated();
 assert(apply(next,0)==ESP_OK);assert(writes==1&&durable.size()==1&&generation()==1);
 snapshot(observed);assert(observed.names==next.names&&observed.last_apply_id==next.last_apply_id&&observed.screens==next.screens);
 assert(apply(next,0)==ESP_ERR_INVALID_STATE&&writes==1);
 boot();assert(observed.revision==1&&observed.model.hostname==next.model.hostname&&observed.names==next.names);
 for(unsigned failure=1;failure<=5;++failure){
   boot();auto candidate=observed;candidate.model.hostname.assign("candidate-" + std::to_string(failure));candidate.last_apply_id++;
   fault=failure;mutated=false;const auto old_revision=observed.revision;
   assert(apply(candidate,old_revision)!=ESP_OK);
   snapshot(observed);assert(observed.revision==old_revision&&observed.model.hostname!=candidate.model.hostname);
   assert(origin()==Origin::kOutcomeUnknown);
   assert(namesReference()==observed.names);
   const auto calls=writes;assert(apply(candidate,old_revision)==ESP_ERR_INVALID_STATE&&writes==calls);
   boot();assert(origin()==Origin::kStored);
   // Adapter may expose the old or the new complete record, never mixed fields.
   if(failure==1)assert(observed.revision==old_revision);
   else assert(observed.revision==old_revision+1&&observed.model.hostname==candidate.model.hostname&&observed.names==candidate.names&&observed.last_apply_id==candidate.last_apply_id);
 }
 boot();auto candidate=observed;candidate.model.hostname.assign("opened");
 fail_open=true;assert(apply(candidate,observed.revision)!=ESP_OK&&origin()==Origin::kStored);
 fail_open=false;
 assert(apply(candidate,observed.revision)==ESP_OK);
 assert(generation()==candidate.revision);
 std::cout<<"owner: one write/coherent reboot/stale reject/5 uncertain failures/read-only/common Apply PASS\n";
}
void namesActivationChecks(){
 durable.clear();writes=0;boot();auto next=populated();const auto old=next;
 next.names={1,1,100,123};
 assert(apply(next,0)==ESP_ERR_INVALID_STATE&&writes==0);
 struct Activation {
   std::mutex mutex;std::condition_variable cv;bool entered=false,release=false;
   unsigned activated=0,preserved=0;
 } activation;
 NamesChange change{old.names,next.names,&activation,
   [](void* raw){auto& state=*static_cast<Activation*>(raw);
     // The whole old config is visible until the names receipt is activated.
     assert(generation()==0&&namesReference()==NamesRef{}&&writes==1);
     std::unique_lock lock(state.mutex);state.entered=true;state.cv.notify_all();
     state.cv.wait(lock,[&]{return state.release;});++state.activated;return ESP_OK;},
   [](void* raw){++static_cast<Activation*>(raw)->preserved;}};
 auto invalid=change;invalid.expected={2,3,100,321};
 assert(apply(next,0,nullptr,nullptr,&invalid)==ESP_ERR_INVALID_STATE&&writes==0);
 auto competing=old;competing.model.hostname.assign("competing");
 int first=ESP_FAIL,second=ESP_FAIL;
 std::thread owner([&]{first=apply(next,0,nullptr,nullptr,&change);});
 {std::unique_lock lock(activation.mutex);
  assert(activation.cv.wait_for(lock,std::chrono::seconds(2),[&]{return activation.entered;}));}
 std::thread writer([&]{second=apply(competing,0);});
 {std::unique_lock lock(write_mutex->mutex);
  assert(write_mutex->cv.wait_for(lock,std::chrono::seconds(2),[&]{return write_mutex->waiters==1;}));}
 assert(generation()==0&&namesReference()==old.names&&writes==1);
 {std::lock_guard lock(activation.mutex);activation.release=true;activation.cv.notify_all();}
 owner.join();writer.join();
 assert(first==ESP_OK&&second==ESP_ERR_INVALID_STATE&&writes==1);
 assert(activation.activated==1&&activation.preserved==0&&generation()==1&&namesReference()==next.names);
 snapshot(observed);assert(observed.model.hostname==next.model.hostname&&observed.last_apply_id==next.last_apply_id);
 // Failed pre-activation after durable write retains both files, old RAM,
 // and the existing unknown-outcome fence until boot reads the exact record.
 auto failed=next;failed.names={2,2,100,456};
 NamesChange failure{next.names,failed.names,&activation,
   [](void*){return ESP_FAIL;},[](void* raw){++static_cast<Activation*>(raw)->preserved;}};
 assert(apply(failed,1,nullptr,nullptr,&failure)==ESP_FAIL);
 assert(origin()==Origin::kOutcomeUnknown&&generation()==1&&namesReference()==next.names&&activation.preserved==1);
 boot();assert(observed.names==failed.names&&observed.revision==2);
 std::cout<<"common Apply: exact prepared lease, activation before publication, competing stale writer, unknown activation/reboot PASS\n";
}
unsigned completions=0;
int failedCleanup(void*){++completions;return ESP_FAIL;}
void cleanupChecks(){
 durable.clear();boot();auto next=populated();next.screens.appearance[0].image=1;
 assert(apply(next,0)==ESP_OK);
 ImageCleanupProgress progress;
 const auto old_revision=next.revision;
 assert(dropMissingImages(0,failedCleanup,nullptr,&progress)==ESP_FAIL);
 snapshot(observed);
 assert(progress.references_cleared&&progress.settings_saved&&!progress.outcome_unknown&&progress.revision==old_revision+1);
 assert(completions==1&&referencedImages()==0&&observed.screens.appearance[0].image==0);
 next=observed;next.screens.appearance[0].image=1;
 assert(apply(next,observed.revision)==ESP_OK);
 fault=2;mutated=false;
 assert(dropMissingImages(0,failedCleanup,nullptr,&progress)!=ESP_OK);
 assert(!progress.references_cleared&&!progress.settings_saved&&progress.outcome_unknown&&completions==1);
 assert(referencedImages()==2);
 std::cout<<"cleanup: committed config + failed completion is partial; uncertain commit skips completion PASS\n";
}
void diagnosticChecks(){
 for(const auto failure:{ESP_ERR_NVS_NOT_ENOUGH_SPACE,ESP_ERR_NO_MEM,ESP_FAIL}){
   durable.clear();boot();auto next=populated();assert(apply(next,0)==ESP_OK);
   const auto before=durable;const auto revision=generation();test_messages.clear();
   storage_error=failure;settings::runNvsFull();storage_error=ESP_OK;
   const bool passed=std::any_of(test_messages.begin(),test_messages.end(),[](const auto& message){return message.starts_with("PASS:");});
   const bool failed=std::any_of(test_messages.begin(),test_messages.end(),[](const auto& message){return message.starts_with("FAIL:");});
   assert(passed==(failure==ESP_ERR_NVS_NOT_ENOUGH_SPACE)&&failed==!passed);
   assert(generation()==revision&&durable==before);boot();assert(observed.revision==revision);
 }
 for(const auto mode:{cfg::SettingsTest::kCorruptRecord,cfg::SettingsTest::kUnknownVersion,cfg::SettingsTest::kShorterRecord,cfg::SettingsTest::kLongerRecord}){
   durable.clear();boot();settings::storePrepared(mode);assert(durable.size()==1);
   boot();assert(origin()!=Origin::kStored&&origin()!=Origin::kInitial);
 }
 durable.clear();boot();const auto before=writes;settings::runSaveLoop();
 assert(writes==before+2000&&generation()==2000);
 snapshot(observed);assert(observed.model.debug_status_interval_s==200);
 std::cout<<"actual device diagnostics: exact NVS full vs memory/IO refusal,4 raw record modes,2000 aggregate Apply loop PASS; SDK faults simulated, no device NVS proof\n";
}
int main(){recordChecks();runtimeProjectionChecks();providerProjectionChecks();ownerChecks();namesActivationChecks();cleanupChecks();diagnosticChecks();}
'''

with tempfile.TemporaryDirectory(prefix="app-config-owner-", dir="/tmp") as tmp:
    folder = Path(tmp)
    (folder / "esp_err.h").write_text(stubs)
    for name in ("esp_log.h", "nvs.h", "freertos/FreeRTOS.h", "freertos/task.h"):
        header = folder / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#pragma once\n#include "esp_err.h"\n')
    diagnostics = ('\n#define kTag kDeviceTestTag\n#include "' +
                   str(ROOT / 'components/settings/settings_test.cpp') +
                   '"\n#undef kTag\n')
    body = '#include "settings_store.h"\n#include "app_config_record.h"\n#include "provider_adapter.h"\n' + source + diagnostics + main
    (folder / "main.cpp").write_text(body)
    includes = [folder, ROOT / "include"]
    includes.extend((ROOT / "components").glob("*/include"))
    command = ["c++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command.extend("-I" + str(path) for path in includes)
    command.append(str(folder / "main.cpp"))
    command.extend(str(ROOT / "components/settings" / name) for name in (
        "app_config_model.cpp", "app_config_record.cpp", "settings_model.cpp",
        "settings_record.cpp", "screen_policy.cpp", "screen_record.cpp", "ui_config.cpp", "runtime_config.cpp"))
    command.append(str(ROOT / "components/timekeeping/civil_time.cpp"))
    command.append(str(ROOT / "components/football/provider_adapter.cpp"))
    binary = folder / "check"
    command += ["-o", str(binary)]
    subprocess.run(command, check=True)
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "detect_leaks=" + ("0" if sys.platform == "darwin" else "1") + ":abort_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    subprocess.run([str(binary)], check=True, env=environment)
