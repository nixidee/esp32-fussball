#!/usr/bin/env python3
"""Actual runJob/publish bodies with controlled transport outcomes and real store."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / "components/football/football_service.cpp").read_text()
def function(signature):
    start = source.index(signature)
    at = source.index("{", start)
    depth = 1
    end = at + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]
slot_start = source.index("struct Slot {")
slot = source[slot_start:source.index("\n};", slot_start) + 3]
prelude = r'''
#include "football_service.h"
#include "provider_plan.h"
#include "provider_client.h"
#include "provider_settings.h"
#include "schedule_coverage.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#define ESP_LOGW(...) ((void)0)
namespace operations {struct Guard { explicit operator bool() const{return true;} };}
namespace events {enum class Event{kDataUpdated};void post(Event){} }
namespace timekeeping {int64_t instant=0;int64_t monotonicMs(){return instant;}}
namespace football {
struct Fault {esp_err_t error=ESP_OK;Cause cause=Cause::kNone;uint16_t http=0;bool sent=true;int64_t attempt=0,finished=0;schedule::Outcome outcome=schedule::Outcome::kOk;bool preserve_scope=false;} fault;
void* mutex=reinterpret_cast<void*>(1);
bool reset_fence=false;
demo::Bindings demo_bindings;
void statusLocked(Status&,int64_t,int64_t){}
MatchStore store;
settings::CompetitionRoutes routes;
view::Shown shown;
CompetitionStates states;
uint32_t revision=0;
uint8_t error_routes=0,route_count=1;
bool demo_mode=false,coverage_streaming=false;
Clock demo_clock{};
std::atomic<bool> busy=false;
plan::Request request;
schedule::Inputs inputs;
Clock nowClock(){return {1792238400,static_cast<uint32_t>(timekeeping::instant/1000)};}
void lock(){} void unlock(){}
plan::Fetch fetchOf(const RequestOutcome& result){return result.code==ESP_OK?plan::Fetch::kOk:result.sent?plan::Fetch::kFailed:plan::Fetch::kBusy;}
coverage::EvidenceArray coverage_evidence;
struct Planner {
 bool request(const schedule::Job&,const settings::CompetitionRoute&,MatchStore&,Clock,plan::Request&){return true;}
 bool response(const provider::ParseResult&){return true;}
 provider::Status responseFailure() const{return provider::Status::kOk;}
 Cause responseCause() const{return Cause::kNone;}
 plan::Completion complete(plan::Fetch,const Clock&){return {fault.outcome,false,false,fault.preserve_scope};}
} planner;
struct Scheduler {void finished(const schedule::Job&,schedule::Outcome,uint32_t,bool=false){}} scheduler;
provider::Sink plannerSink(const settings::CompetitionRoute&){return {};}
enum class HttpPurpose {kAutomatic};
RequestOutcome get(const provider::RequestSpec&,const settings::ProviderSettings&,uint32_t,uint32_t,uint32_t,const provider::Sink&,HttpPurpose,uint8_t,const operations::Guard*){
 RequestOutcome result;result.sent=fault.sent;result.sent_ms=fault.attempt;result.code=fault.error;result.cause=fault.cause;result.http=fault.http;timekeeping::instant=fault.finished;return result;
}
'''
main = r'''
} // namespace football
int main(){using namespace football;
 settings::CompetitionRoute route;route.id=1;route.incarnation=1;route.primary.competition.assign("bl1");routes.entries.push(route);
 SourceKey key;assert(key.assign(route.primary.provider,"bl1","2026"));
 assert(store.bind(1,1,0,nowClock()));assert(store.resolve(1,key,nowClock()));
 slots[0].id=1;slots[0].active=true;settings::ProviderSettings model;
 const auto baseline_access=accessOf(model);
 auto changed=model;changed.api_football_key.assign("synthetic-af-key");
 assert(changedProviders(baseline_access,accessOf(changed))==(1u<<static_cast<unsigned>(cfg::Provider::kApiFootball)));
 const auto keyed_access=accessOf(changed);changed.api_football_key.assign("synthetic-af-key-2");
 assert(changedProviders(keyed_access,accessOf(changed))==(1u<<static_cast<unsigned>(cfg::Provider::kApiFootball)));
 changed=model;changed.football_data_key.assign("synthetic-fd-key");
 assert(changedProviders(baseline_access,accessOf(changed))==(1u<<static_cast<unsigned>(cfg::Provider::kFootballData)));
 changed=model;changed.espn_opt_in=!model.espn_opt_in;
 assert(changedProviders(baseline_access,accessOf(changed))==(1u<<static_cast<unsigned>(cfg::Provider::kEspn)));
 changed=model;changed.global_limit.daily=123;changed.demo=!model.demo;
 assert(!changedProviders(baseline_access,accessOf(changed)));

 const auto job=[](cfg::DataType type){schedule::Job out;out.slot=store.currentSource(1);out.key.source=store.sourceId(out.slot);out.competition=1;out.provider=cfg::Provider::kOpenLigaDb;out.type=type;return out;};
 fault={ESP_OK,Cause::kNone,0,true,100,200,schedule::Outcome::kOk};runJob(job(cfg::DataType::kTable),model,1,1,1);
 fault={ESP_FAIL,Cause::kHttp,503,true,300,400,schedule::Outcome::kFailed};runJob(job(cfg::DataType::kTable),model,1,1,1);
 fault={ESP_OK,Cause::kNone,0,true,500,600,schedule::Outcome::kOk};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 auto table=store.datasetStatus(store.currentSource(1),MatchStore::Dataset::kTable);auto matches=store.datasetStatus(store.currentSource(1),MatchStore::Dataset::kFixtures);
 assert(table.last_attempt_ms==300&&table.last_success_ms==200&&table.http==503&&table.cause==Cause::kHttp&&table.state==DatasetState::kPartial);
 assert(matches.last_attempt_ms==500&&matches.last_success_ms==600&&matches.cause==Cause::kNone);
 assert(states[0].cause==Cause::kHttp&&states[0].http==503&&error_routes==1&&states[0].fetched_ms==600);
 assert(schedule::datasetStale(store,1,MatchStore::Dataset::kTable,nowClock().utc,600,route.primary.provider));
 assert(!schedule::datasetStale(store,1,MatchStore::Dataset::kFixtures,nowClock().utc,600,route.primary.provider));
 fault={ESP_ERR_TIMEOUT,Cause::kBusy,0,false,0,700,schedule::Outcome::kDeferred};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 matches=store.datasetStatus(store.currentSource(1),MatchStore::Dataset::kFixtures);assert(matches.last_attempt_ms==500&&matches.last_success_ms==600&&matches.cause==Cause::kNone);
 assert(states[0].cause==Cause::kHttp&&table.last_success_ms==200);
 fault={ESP_FAIL,Cause::kConnect,0,true,800,900,schedule::Outcome::kFailed};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 fault={ESP_OK,Cause::kNone,0,true,1000,1100,schedule::Outcome::kOk};runJob(job(cfg::DataType::kTable),model,1,1,1);
 assert(states[0].cause==Cause::kConnect&&states[0].http==0&&error_routes==1);
 table=store.datasetStatus(store.currentSource(1),MatchStore::Dataset::kTable);assert(table.last_success_ms==1100&&table.cause==Cause::kNone&&table.state==DatasetState::kComplete);
 fault={ESP_OK,Cause::kNone,0,true,1200,1300,schedule::Outcome::kOk};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 assert(states[0].cause==Cause::kNone&&error_routes==0);
 // A table refused before HTTP starts has no invented provider attempt.
 fault={ESP_ERR_TIMEOUT,Cause::kBusy,0,false,0,1400,schedule::Outcome::kDeferred};runJob(job(cfg::DataType::kTable),model,1,1,1);
 table=store.datasetStatus(store.currentSource(1),MatchStore::Dataset::kTable);assert(table.last_attempt_ms==1000&&table.last_success_ms==1100&&table.state==DatasetState::kComplete);
 store.flag(store.currentSource(1),kNoticeTableTeams|kNoticeEvents,true);
 assert(store.datasetNotice(store.currentSource(1),MatchStore::Dataset::kTable,nowClock().utc)==Cause::kStoreFull);
 assert(store.datasetNotice(store.currentSource(1),MatchStore::Dataset::kEvents,nowClock().utc)==Cause::kStoreFull);
 assert(store.datasetNotice(store.currentSource(1),MatchStore::Dataset::kFixtures,nowClock().utc)==Cause::kNone);
 // A successful HTTP finish cannot turn a capacity-truncated table into
 // complete data; an unrelated Fixtures success does not mask this either.
 fault={ESP_OK,Cause::kNone,0,true,1500,1600,schedule::Outcome::kOk};runJob(job(cfg::DataType::kTable),model,1,1,1);
 assert(store.datasetState(store.currentSource(1),MatchStore::Dataset::kTable)==DatasetState::kPartial);
 fault={ESP_OK,Cause::kNone,0,true,1700,1800,schedule::Outcome::kOk};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 assert(store.datasetState(store.currentSource(1),MatchStore::Dataset::kFixtures)==DatasetState::kComplete);
 assert(store.datasetState(store.currentSource(1),MatchStore::Dataset::kTable)==DatasetState::kPartial);
 store.flag(store.currentSource(1),kNoticeTableTeams,false);
 fault={ESP_OK,Cause::kNone,0,true,1900,2000,schedule::Outcome::kOk};runJob(job(cfg::DataType::kTable),model,1,1,1);
 assert(store.datasetState(store.currentSource(1),MatchStore::Dataset::kTable)==DatasetState::kComplete);
 const int64_t long_uptime=5000000000LL; // beyond the old49-day uint32ms wrap
 StoredMatch live;live.id=1;live.source=store.currentSource(1);live.kickoff=nowClock().utc;live.phase=Phase::kLive;
 store.upsert(live,nowClock());
 store.recordDataset(store.currentSource(1),MatchStore::Dataset::kFixtures,long_uptime-100,long_uptime,true,Cause::kNone);
 assert(!schedule::datasetStale(store,1,MatchStore::Dataset::kFixtures,nowClock().utc,long_uptime+cfg::kDataStaleS*1000LL,route.primary.provider));
 assert(schedule::datasetStale(store,1,MatchStore::Dataset::kFixtures,nowClock().utc,long_uptime+cfg::kDataStaleS*1000LL+1,route.primary.provider));
 assert(!schedule::datasetStale(store,1,MatchStore::Dataset::kTable,nowClock().utc,long_uptime+cfg::kDataStaleS*1000LL+1,route.primary.provider));
 assert(!schedule::datasetStale(store,1,MatchStore::Dataset::kFixtures,nowClock().utc,long_uptime+cfg::kDataStaleS*1000LL+1,cfg::Provider::kEspn));
 assert(store.datasetNotice(store.currentSource(1),MatchStore::Dataset::kFixtures,nowClock().utc+cfg::kUnmaintainedAfterS+1)==Cause::kUnmaintained);
 // Different requested summaries of a shared season retain independent errors.
 store.clear();routes.clear();route.primary.provider=cfg::Provider::kEspn;route.primary.competition.assign("ger.1");
 assert(key.assign(cfg::Provider::kEspn,"ger.1","2026"));routes.entries.push(route);
 auto peer=route;peer.id=2;peer.incarnation=2;routes.entries.push(peer);
 assert(store.bind(1,1,0,nowClock())&&store.resolve(1,key,nowClock()));
 assert(store.bind(2,2,0,nowClock())&&store.resolve(2,key,nowClock()));
 route_count=2;slots[1].id=2;slots[1].active=true;
 auto first=job(cfg::DataType::kEvents);first.key={};first.key.competition=1;first.key.incarnation=1;
 first.provider=cfg::Provider::kEspn;first.event_match={101,store.sourceId(store.currentSource(1))};
 auto second=first;second.route=1;second.competition=2;second.key.competition=2;second.key.incarnation=2;second.event_match.id=202;
 inputs.scopes[cfg::kStoreSources].key=first.key;
 inputs.scopes[cfg::kStoreSources].competition=1;
 inputs.scopes[cfg::kStoreSources].event_match=first.event_match;
 inputs.scopes[cfg::kStoreSources+1].key=second.key;
 inputs.scopes[cfg::kStoreSources+1].competition=2;
 inputs.scopes[cfg::kStoreSources+1].event_match=second.event_match;
 assert(store.summaryTarget(1,first.event_match));assert(store.summaryTarget(2,second.event_match));
 fault={ESP_FAIL,Cause::kHttp,503,true,2100,2200,schedule::Outcome::kFailed};runJob(first,model,1,1,1);
 assert(store.viewDatasetStatus(1,MatchStore::Dataset::kEvents).cause==Cause::kHttp);
 assert(store.viewDatasetStatus(2,MatchStore::Dataset::kEvents).cause==Cause::kNone);
 fault={ESP_OK,Cause::kNone,0,true,2300,2400,schedule::Outcome::kOk};runJob(second,model,1,1,1);
 assert(store.viewDatasetStatus(1,MatchStore::Dataset::kEvents).cause==Cause::kHttp);
 assert(store.viewDatasetStatus(2,MatchStore::Dataset::kEvents).cause==Cause::kNone);
 assert(states[0].cause==Cause::kHttp&&states[1].cause==Cause::kNone);
 // New valid target resets only its view; a late previous result is ignored.
 assert(store.summaryTarget(1,second.event_match));
 store.recordSummary(1,first.event_match,2500,2600,true,Cause::kNone);
 assert(store.viewDatasetStatus(1,MatchStore::Dataset::kEvents).last_success_ms==0);
 assert(store.viewDatasetStatus(2,MatchStore::Dataset::kEvents).last_success_ms==2400);
 inputs.scopes[cfg::kStoreSources].event_match=second.event_match;
 fault={ESP_OK,Cause::kNone,0,true,2700,2800,schedule::Outcome::kOk};runJob(second,model,1,1,1);
 assert(store.viewDatasetStatus(1,MatchStore::Dataset::kEvents).last_success_ms==2800);
 assert(store.viewDatasetStatus(2,MatchStore::Dataset::kEvents).last_success_ms==2800);
 assert(states[0].cause==Cause::kNone&&states[1].cause==Cause::kNone);
 store.flag(store.currentSource(1),kNoticeEvents,true);
 fault={ESP_OK,Cause::kNone,0,true,2900,3000,schedule::Outcome::kOk};runJob(second,model,1,1,1);
 assert(store.viewDatasetStatus(1,MatchStore::Dataset::kEvents).state==DatasetState::kPartial);
 assert(store.viewDatasetStatus(2,MatchStore::Dataset::kEvents).state==DatasetState::kPartial);
 store.flag(store.currentSource(1),kNoticeEvents,false);
 fault={ESP_OK,Cause::kNone,0,true,3100,3200,schedule::Outcome::kOk};runJob(second,model,1,1,1);
 assert(store.viewDatasetStatus(1,MatchStore::Dataset::kEvents).state==DatasetState::kComplete);
 assert(store.viewDatasetStatus(2,MatchStore::Dataset::kEvents).state==DatasetState::kComplete);
 // A known-identity-only refresh advances freshness without inventing scope.
 auto live_job=job(cfg::DataType::kLive);live_job.provider=store.provider(store.currentSource(1));
 store.datasetResult(store.currentSource(1),MatchStore::Dataset::kFixtures,false);
 fault={ESP_OK,Cause::kNone,0,true,3300,3400,schedule::Outcome::kOk,true};runJob(live_job,model,1,1,1);
 assert(store.datasetState(store.currentSource(1),MatchStore::Dataset::kFixtures)==DatasetState::kPartial);
 assert(store.datasetStatus(store.currentSource(1),MatchStore::Dataset::kFixtures).last_success_ms==3400);
 store.datasetResult(store.currentSource(1),MatchStore::Dataset::kFixtures,true);
 fault={ESP_OK,Cause::kNone,0,true,3500,3600,schedule::Outcome::kOk,true};runJob(live_job,model,1,1,1);
 assert(store.datasetState(store.currentSource(1),MatchStore::Dataset::kFixtures)==DatasetState::kComplete);
 // Provider provenance comes from the source even without home-team metadata.
 store.clear();shown={};routes.clear();route_count=1;slots={};
 settings::CompetitionRoute away_route;away_route.id=7;away_route.incarnation=7;away_route.primary.provider=cfg::Provider::kEspn;away_route.primary.competition.assign("ger.1");away_route.club.team=40;
 assert(routes.entries.push(away_route));assert(store.bind(7,7,40,nowClock()));
 SourceKey away_source;assert(away_source.assign(cfg::Provider::kEspn,"ger.1","2026"));const auto away_id=store.resolve(7,away_source,nowClock());assert(away_id);
 StoredMatch away;away.id=700;away.source=store.sourceSlot(away_id);away.away=store.team(away.source,40,"Selected away club");away.phase=Phase::kLive;away.kickoff=nowClock().utc-60;
 assert(store.upsert(away,nowClock()));shown.add(away_route);slots[0].id=7;slots[0].active=true;
 const auto projection=overview(nowClock().utc,nullptr,nullptr);
 assert(projection.own.present&&projection.own.competition==7&&projection.own.home_id==0&&projection.own.away_id==40);
 assert(projection.own.provider==cfg::Provider::kEspn&&projection.routes[0].provider==cfg::Provider::kEspn);
 assert(projection.own.home.empty()&&std::string_view{projection.own.away.data}=="Selected away club");
 std::printf("PASS actual overview missing-home provenance and actual runJob/publish: Table503→FixturesOK preserves Tableerror+timestamps; inverse failure preserved; busy never becomes HTTP attempt; same-dataset recovery clears only itself; shared-source distinct summary targets stay independent, stale outcomes ignored, coalesced success updates both. DatasetStatus=%zu Store=%zu Slot=%zu\n",sizeof(DatasetStatus),sizeof(MatchStore),sizeof(Slot));
}
'''
with tempfile.TemporaryDirectory(prefix="football-dataset-status-") as directory:
    scratch = Path(directory)
    (scratch / "esp_err.h").write_text("#pragma once\nusing esp_err_t=int;\ninline constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=1,ESP_ERR_INVALID_RESPONSE=2,ESP_ERR_INVALID_SIZE=3,ESP_ERR_NO_MEM=4;\n")
    unit = scratch / "test.cpp"
    unit.write_text(prelude + slot + '\nstd::array<Slot,cfg::kCompetitionMax> slots;\n' +
                    'using Access=std::array<uint32_t,cfg::kProviderCount>;\n' + function('Access accessOf(') + '\n' + function('uint8_t changedProviders(') + '\n' + function('void publish() {') + '\n' + function('void runJob(') + '\n' + function('Overview overview(') + main)
    command = ['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(scratch)]
    for path in ('include', 'components/football/include', 'components/settings/include',
                 'components/json/include', 'components/timekeeping/include', 'components/network/include'):
        command += ['-I', str(root / path)]
    command += [str(unit)]
    for path in ('components/football/provider_adapter.cpp', 'components/football/football_model.cpp', 'components/football/source_index.cpp', 'components/football/schedule_coverage.cpp', 'components/football/budget_ledger.cpp', 'components/settings/settings_record.cpp', 'components/settings/settings_model.cpp', 'components/settings/screen_policy.cpp', 'components/network/wifi_policy.cpp', 'components/football/match_store.cpp',
                 'components/football/matchday.cpp', 'components/football/store_views.cpp', 'components/football/request_scheduler.cpp', 'components/timekeeping/civil_time.cpp'):
        command += [str(root / path)]
    binary = scratch / 'dataset'
    subprocess.run([*command, '-o', str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
