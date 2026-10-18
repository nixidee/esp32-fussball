#!/usr/bin/env python3
"""Actual RAM route owner and discovery/NVS boundary, with controlled SDK I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "components/football/football_service.cpp").read_text()


def function(signature):
    start = source.index(signature)
    at = source.index("{", start)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


sdk = r'''
#pragma once
#include <cstring>
#include <vector>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NVS_NOT_FOUND=1;
using nvs_handle_t=int;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
inline std::vector<unsigned char> durable,pending;
inline bool fail_open=false,fail_read=false,fail_set=false,fail_commit=false;
inline unsigned writes=0,commits=0;
inline int nvs_open(const char*,int,nvs_handle_t* out){*out=1;return fail_open?ESP_FAIL:ESP_OK;}
inline void nvs_close(nvs_handle_t){}
inline int nvs_get_blob(nvs_handle_t,const char*,void* out,size_t* size){
 if(fail_read)return ESP_FAIL;if(durable.empty())return ESP_ERR_NVS_NOT_FOUND;
 if(*size<durable.size())return ESP_FAIL;*size=durable.size();std::memcpy(out,durable.data(),*size);return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t,const char*,const void* bytes,size_t size){
 ++writes;if(fail_set)return ESP_FAIL;const auto* p=static_cast<const unsigned char*>(bytes);pending.assign(p,p+size);return ESP_OK;
}
inline int nvs_commit(nvs_handle_t){++commits;if(fail_commit)return ESP_FAIL;durable=pending;return ESP_OK;}
'''

program = r'''
#include "sdk_test.h"
#include "football_service.h"
#include "provider_coverage.h"
#include "provider_settings.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
namespace network {struct HeavyGuard{explicit operator bool()const{return true;}};struct Status{uint32_t epoch=1;};Status status(){return {};}}
namespace events {enum class Event{kDataUpdated};void post(Event){}}
namespace timekeeping {int64_t monotonicMs(){return 10000;}}
namespace football {
MatchStore store;
settings::CompetitionRoutes routes;
plan::Planner planner;
schedule::Scheduler scheduler;
settings::screen_policy::Eligibility eligibility;
coverage::EvidenceArray coverage_evidence;
coverage::Discovery discovery;
struct DiscoveryCursor {uint8_t route=0,season=0;};
DiscoveryCursor discovery_progress;
uint64_t discovery_binding=0,coverage_binding=1;
bool discovery_retire=false,coverage_streaming=false,sent=false,demo_mode=false,configured=false;
Cause failure_cause=Cause::kNone;
uint8_t route_count=0,error_routes=0;
uint32_t revision=0,published_generation=0;
view::Shown shown;
CompetitionStates states;
Clock demo_clock{};
demo::Bindings demo_bindings;
demo::Scenario demo_scenario=demo::Scenario::kOwn;
uint8_t demo_step=0;
constexpr uint8_t kNoDemoCommand=255;
std::atomic<uint8_t> demo_command=kNoDemoCommand;
std::atomic<bool> busy=false;
Clock nowClock(){return {1792238400,10};}
void lock(){}void unlock(){}
uint32_t routeKey(const settings::CompetitionRoute& route){return route.incarnation*8+static_cast<unsigned>(route.primary.provider);}
bool usesAny(const settings::CompetitionRoute& route,uint8_t providers){return providers&(1u<<static_cast<unsigned>(route.primary.provider));}
ACTUAL_SLOT
std::array<Slot,cfg::kCompetitionMax> slots;
ACTUAL_SLOT_LOOKUP
ACTUAL_COVERAGE_LOOKUP
ACTUAL_BIND_COVERAGE
ACTUAL_PUBLISH
ACTUAL_APPLY_ROUTES
ACTUAL_LOAD
ACTUAL_STORE
ACTUAL_RESERVE
ACTUAL_FINISH
enum class HttpPurpose{kDiscovery};
unsigned sends=0;
cfg::Provider last_provider=cfg::kProviderDefault;
std::string last_season;
esp_err_t get(const provider::RequestSpec& spec,const settings::ProviderSettings&,uint32_t,uint32_t,uint32_t,
              const provider::Sink& sink,provider::ParseResult& parsed,HttpPurpose,uint8_t,const network::HeavyGuard*){
 sent=false;
 if(!reserveDiscovery(1792238400/86400)){failure_cause=Cause::kCounter;return ESP_FAIL;}
 sent=true;++sends;last_provider=spec.source.provider;last_season=spec.source.season;
 struct Input{std::string body;size_t at=0;static int read(void* raw){auto& in=*static_cast<Input*>(raw);return in.at<in.body.size()?static_cast<unsigned char>(in.body[in.at++]):-1;}};
 Input input{"[{\"matchID\":1,\"leagueShortcut\":\"bl1\",\"leagueSeason\":"+last_season+",\"matchDateTimeUTC\":\"2026-09-01T12:00Z\"},{\"matchID\":2,\"leagueShortcut\":\"bl1\",\"leagueSeason\":"+last_season+",\"matchDateTimeUTC\":\"2026-12-01T12:00Z\"}]"};
 json::Reader reader({&input,Input::read});parsed=provider::readResponse(reader,spec,sink);
 return parsed.status==provider::Status::kOk?ESP_OK:ESP_FAIL;
}
ACTUAL_STEP
void reset(){
 durable.clear();pending.clear();fail_open=fail_read=fail_set=fail_commit=false;writes=commits=sends=0;
 routes.clear();route_count=0;slots={};store.clear();coverage_evidence={};discovery.load(budget::Load::kMissing);
 discovery_progress={};discovery_binding=0;coverage_binding=1;discovery_retire=false;failure_cause=Cause::kNone;
 eligibility={};eligibility.selection.automatic_break=true;eligibility.break_days=21;
}
void route(cfg::Provider provider,uint8_t id){settings::CompetitionRoute value;value.id=id;value.incarnation=id;value.primary.provider=provider;value.primary.competition.assign(provider==cfg::Provider::kEspn?"aus.1":"bl1");value.primary.season.assign("2026");routes.entries.push(value);route_count=routes.entries.size();}
}
int main(){using namespace football;settings::ProviderSettings model;model.espn_opt_in=true;
 // Actual source application exposes unsupported separately from unknown;
 // same-source reload preserves successful and failed supported datasets.
 reset();route(cfg::Provider::kApiFootball,1);applyRoutesLocked(model,1,false);
 assert(store.datasetState(1,MatchStore::Dataset::kEvents)==DatasetState::kUnavailable);
 assert(store.datasetState(1,MatchStore::Dataset::kFixtures)==DatasetState::kUnknown);
 assert(store.datasetStatus(1,MatchStore::Dataset::kEvents).cause==Cause::kNone);
 store.recordDataset(1,MatchStore::Dataset::kTable,100,200,true,Cause::kNone);
 store.recordDataset(1,MatchStore::Dataset::kFixtures,300,400,false,Cause::kHttp,503);
 applyRoutesLocked(model,2,false);
 assert(store.datasetStatus(1,MatchStore::Dataset::kTable).last_success_ms==200);
 assert(store.datasetStatus(1,MatchStore::Dataset::kFixtures).http==503);
 routes.entries[0].primary.provider=cfg::Provider::kOpenLigaDb;applyRoutesLocked(model,3,false);
 assert(store.datasetState(1,MatchStore::Dataset::kEvents)==DatasetState::kUnknown);
 assert(!store.datasetStatus(1,MatchStore::Dataset::kTable).last_success_ms);
 store.datasetResult(1,MatchStore::Dataset::kEvents,true);applyRoutesLocked(model,4,false);
 assert(store.datasetState(1,MatchStore::Dataset::kEvents)==DatasetState::kComplete);
 routes.entries[0].primary.provider=cfg::Provider::kFootballData;applyRoutesLocked(model,5,false);
 assert(store.datasetState(1,MatchStore::Dataset::kEvents)==DatasetState::kUnavailable);
 routes.entries[0].primary.provider=cfg::Provider::kEspn;applyRoutesLocked(model,6,false);
 assert(store.datasetState(1,MatchStore::Dataset::kEvents)==DatasetState::kUnknown);
 model.demo=true;applyRoutesLocked(model,7,true);
 assert(store.datasetState(1,MatchStore::Dataset::kEvents)==DatasetState::kComplete);model.demo=false;
 // ESPN alone cannot spend an extra discovery request or claim a break.
 reset();route(cfg::Provider::kEspn,1);bindCoverage();
 assert(!stepDiscovery(model,1,1,1792238400)&&sends==0&&writes==0);
 assert(coverage::assess(coverage_evidence,routes,1792238400,21).verdict==coverage::Verdict::kUnknown);
 // A following supported source still uses the same normal typed parser.
 route(cfg::Provider::kOpenLigaDb,2);bindCoverage();
 assert(stepDiscovery(model,1,1,1792238400)&&sends==1&&last_provider==cfg::Provider::kOpenLigaDb&&last_season=="2026");
 assert(discovery_progress.route==1&&discovery_progress.season==1&&durable.size()==36);
 const auto reserved=discovery.stored().requests.reserved;assert(reserved==cfg::kBudgetReservationRequests);
 loadDiscovery();assert(!discovery_progress.route&&!discovery_progress.season);
 assert(stepDiscovery(model,1,1,1792238400)&&sends==2&&last_season=="2026");
 assert(discovery.stored().requests.reserved>reserved); // reboot forfeits unused reservation
 // Changed inputs cannot inherit this run, even if retirement persistence fails.
 ++coverage_binding;fail_set=true;
 assert(!stepDiscovery(model,2,2,1792238400)&&sends==2&&discovery.stored().active);
 fail_set=false;fail_commit=true;
 assert(!stepDiscovery(model,2,2,1792238400)&&sends==2&&discovery.stored().active);
 fail_commit=false;
 assert(!stepDiscovery(model,2,2,1792238400)&&sends==2&&!discovery.stored().active);
 coverage::DiscoveryRecord unused;assert(discovery.start(1792238400/86400,unused)==coverage::Discovery::Admission::kUsedUp);
 // Guard writes must succeed before the real HTTP send; no optimistic credits.
 reset();route(cfg::Provider::kOpenLigaDb,1);bindCoverage();fail_set=true;
 assert(!stepDiscovery(model,1,1,1792238400)&&sends==0&&!discovery.stored().active);
 fail_set=false;fail_commit=true;
 assert(!stepDiscovery(model,1,1,1792238400)&&sends==0&&!discovery.stored().active);
 fail_commit=false;
 assert(stepDiscovery(model,1,1,1792238400)&&sends==1);
 // A damaged NVS record blocks the observed day; no cursor is reconstructed.
 durable.back()^=1;loadDiscovery();coverage::DiscoveryRecord candidate;
 assert(!discovery.canPersist());assert(discovery.start(20744,candidate)==coverage::Discovery::Admission::kUsedUp);
 assert(discovery.start(20745,candidate)==coverage::Discovery::Admission::kStore);
 assert(candidate.active&&candidate.runs.day==20745&&candidate.requests.reserved==0);
 std::printf("PASS actual route reload/capability, zero ESPN discovery, typed supported source, RAM-only reboot traversal, source retirement, NVS set/commit faults. Guard=%zu Record=%zu Discovery=%zu\n",coverage::record::kGuardBytes,sizeof(coverage::DiscoveryRecord),sizeof(coverage::Discovery));
}
'''
slot_at = source.index("struct Slot {")
replacements = {
    "ACTUAL_SLOT": source[slot_at:source.index("\n};", slot_at) + 3],
    "ACTUAL_SLOT_LOOKUP": function("const Slot* slotOf("),
    "ACTUAL_COVERAGE_LOOKUP": function("coverage::Evidence* coverageOf(" + "uint8_t id) {"),
    "ACTUAL_BIND_COVERAGE": function("void bindCoverage()"),
    "ACTUAL_PUBLISH": function("void publish()"),
    "ACTUAL_APPLY_ROUTES": function("void applyRoutesLocked("),
    "ACTUAL_LOAD": function("void loadDiscovery()"),
    "ACTUAL_STORE": function("bool storeDiscovery("),
    "ACTUAL_RESERVE": function("bool reserveDiscovery(uint32_t utc_day) {"),
    "ACTUAL_FINISH": function("bool finishDiscovery()"),
    "ACTUAL_STEP": function("bool stepDiscovery("),
}
for key, value in sorted(replacements.items(), key=lambda item: -len(item[0])):
    program = program.replace(key, value)


with tempfile.TemporaryDirectory(prefix="discovery-owner-", dir="/tmp") as directory:
    folder = Path(directory)
    (folder / "sdk_test.h").write_text(sdk)
    (folder / "esp_err.h").write_text('#include "sdk_test.h"\n')
    (folder / "test.cpp").write_text(program)
    command = ["clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(folder)]
    for path in (root / "include", *sorted((root / "components").glob("*/include"))):
        command += ["-I", str(path)]
    files = ["football/fixture_retention", "football/football_model", "football/match_store",
             "football/matchday", "football/store_views", "football/store_mappers",
             "football/provider_plan", "football/provider_readers", "football/provider_adapter",
             "football/provider_selection", "football/provider_coverage", "football/schedule_coverage", "football/request_scheduler",
             "football/screen_selection", "football/demo_scenarios", "football/budget_ledger",
             "settings/settings_model", "settings/settings_record", "settings/screen_policy",
             "network/wifi_policy", "json/json_stream", "timekeeping/civil_time"]
    command += [str(root / "components" / (name + ".cpp")) for name in files]
    binary = folder / "test"
    subprocess.run([*command, str(folder / "test.cpp"), "-o", str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
