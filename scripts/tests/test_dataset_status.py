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
#include "provider_settings.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#define ESP_LOGW(...) ((void)0)
namespace network {struct HeavyGuard { explicit operator bool() const{return true;} };}
namespace events {enum class Event{kDataUpdated};void post(Event){} }
namespace timekeeping {int64_t instant=0;int64_t monotonicMs(){return instant;}}
namespace football {
struct Fault {esp_err_t error=ESP_OK;Cause cause=Cause::kNone;uint16_t http=0;bool sent=true;int64_t attempt=0,finished=0;schedule::Outcome outcome=schedule::Outcome::kOk;} fault;
MatchStore store;
settings::CompetitionRoutes routes;
view::Shown shown;
CompetitionStates states;
uint32_t revision=0;
uint8_t error_routes=0,route_count=1;
bool demo_mode=false,coverage_streaming=false;
Clock demo_clock{};
std::atomic<bool> busy=false;
bool sent=false;
int64_t sent_ms=0;
plan::Request request;
Clock nowClock(){return {1792238400,static_cast<uint32_t>(timekeeping::instant/1000)};}
void lock(){} void unlock(){}
plan::Fetch fetchOf(esp_err_t error){return error==ESP_OK?plan::Fetch::kOk:sent?plan::Fetch::kFailed:plan::Fetch::kBusy;}
Cause causeOf(esp_err_t){return fault.cause;}
uint16_t httpOf(esp_err_t){return fault.http;}
esp_err_t stop(esp_err_t error,Cause cause){fault.cause=cause;return error;}
namespace coverage {struct Evidence{};enum class Reason{kPartialResponse};void invalidate(Evidence&,Reason){} }
coverage::Evidence* coverageOf(uint8_t){return nullptr;}
struct Planner {
 bool request(const schedule::Job&,const settings::CompetitionRoute&,MatchStore&,Clock,plan::Request&){return true;}
 bool response(const provider::ParseResult&){return true;}
 provider::Status responseFailure() const{return provider::Status::kOk;}
 plan::Completion complete(plan::Fetch,const Clock&){return {fault.outcome,false,false};}
} planner;
struct Scheduler {void finished(const schedule::Job&,schedule::Outcome,uint32_t,bool=false){}} scheduler;
provider::Sink plannerSink(const settings::CompetitionRoute&){return {};}
enum class HttpPurpose {kAutomatic};
esp_err_t get(const provider::RequestSpec&,const settings::ProviderSettings&,uint32_t,uint32_t,uint32_t,const provider::Sink&,provider::ParseResult&,HttpPurpose,uint8_t,const network::HeavyGuard*){
 sent=fault.sent;sent_ms=fault.attempt;timekeeping::instant=fault.finished;return fault.error;
}
'''
main = r'''
} // namespace football
int main(){using namespace football;
 settings::CompetitionRoute route;route.id=1;route.incarnation=1;route.primary.competition.assign("bl1");routes.entries.push(route);
 slots[0].id=1;slots[0].active=true;settings::ProviderSettings model;
 const auto job=[](cfg::DataType type){schedule::Job out;out.slot=0;out.competition=1;out.provider=cfg::Provider::kOpenLigaDb;out.type=type;return out;};
 fault={ESP_OK,Cause::kNone,0,true,100,200,schedule::Outcome::kOk};runJob(job(cfg::DataType::kTable),model,1,1,1);
 fault={ESP_FAIL,Cause::kHttp,503,true,300,400,schedule::Outcome::kFailed};runJob(job(cfg::DataType::kTable),model,1,1,1);
 fault={ESP_OK,Cause::kNone,0,true,500,600,schedule::Outcome::kOk};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 auto table=store.datasetStatus(1,MatchStore::Dataset::kTable);auto matches=store.datasetStatus(1,MatchStore::Dataset::kFixtures);
 assert(table.last_attempt_ms==300&&table.last_success_ms==200&&table.http==503&&table.cause==Cause::kHttp&&table.state==DatasetState::kPartial);
 assert(matches.last_attempt_ms==500&&matches.last_success_ms==600&&matches.cause==Cause::kNone);
 assert(states[0].cause==Cause::kHttp&&states[0].http==503&&error_routes==1&&states[0].fetched_ms==600);
 assert(schedule::datasetStale(store,1,MatchStore::Dataset::kTable,nowClock().utc,600,route.primary.provider));
 assert(!schedule::datasetStale(store,1,MatchStore::Dataset::kFixtures,nowClock().utc,600,route.primary.provider));
 fault={ESP_ERR_TIMEOUT,Cause::kBusy,0,false,0,700,schedule::Outcome::kDeferred};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 matches=store.datasetStatus(1,MatchStore::Dataset::kFixtures);assert(matches.last_attempt_ms==500&&matches.last_success_ms==600&&matches.cause==Cause::kNone);
 assert(states[0].cause==Cause::kHttp&&table.last_success_ms==200);
 fault={ESP_FAIL,Cause::kConnect,0,true,800,900,schedule::Outcome::kFailed};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 fault={ESP_OK,Cause::kNone,0,true,1000,1100,schedule::Outcome::kOk};runJob(job(cfg::DataType::kTable),model,1,1,1);
 assert(states[0].cause==Cause::kConnect&&states[0].http==0&&error_routes==1);
 table=store.datasetStatus(1,MatchStore::Dataset::kTable);assert(table.last_success_ms==1100&&table.cause==Cause::kNone&&table.state==DatasetState::kComplete);
 fault={ESP_OK,Cause::kNone,0,true,1200,1300,schedule::Outcome::kOk};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 assert(states[0].cause==Cause::kNone&&error_routes==0);
 // A table refused before HTTP starts has no invented provider attempt.
 fault={ESP_ERR_TIMEOUT,Cause::kBusy,0,false,0,1400,schedule::Outcome::kDeferred};runJob(job(cfg::DataType::kTable),model,1,1,1);
 table=store.datasetStatus(1,MatchStore::Dataset::kTable);assert(table.last_attempt_ms==1000&&table.last_success_ms==1100&&table.state==DatasetState::kComplete);
 store.flag(1,kNoticeTableTeams|kNoticeEvents,true);
 assert(store.datasetNotice(1,MatchStore::Dataset::kTable,nowClock().utc)==Cause::kStoreFull);
 assert(store.datasetNotice(1,MatchStore::Dataset::kEvents,nowClock().utc)==Cause::kStoreFull);
 assert(store.datasetNotice(1,MatchStore::Dataset::kFixtures,nowClock().utc)==Cause::kNone);
 // A successful HTTP finish cannot turn a capacity-truncated table into
 // complete data; an unrelated Fixtures success does not mask this either.
 fault={ESP_OK,Cause::kNone,0,true,1500,1600,schedule::Outcome::kOk};runJob(job(cfg::DataType::kTable),model,1,1,1);
 assert(store.datasetState(1,MatchStore::Dataset::kTable)==DatasetState::kPartial);
 fault={ESP_OK,Cause::kNone,0,true,1700,1800,schedule::Outcome::kOk};runJob(job(cfg::DataType::kFixtures),model,1,1,1);
 assert(store.datasetState(1,MatchStore::Dataset::kFixtures)==DatasetState::kComplete);
 assert(store.datasetState(1,MatchStore::Dataset::kTable)==DatasetState::kPartial);
 store.flag(1,kNoticeTableTeams,false);
 fault={ESP_OK,Cause::kNone,0,true,1900,2000,schedule::Outcome::kOk};runJob(job(cfg::DataType::kTable),model,1,1,1);
 assert(store.datasetState(1,MatchStore::Dataset::kTable)==DatasetState::kComplete);
 const int64_t long_uptime=5000000000LL; // beyond the old49-day uint32ms wrap
 StoredMatch live;live.id=1;live.competition=1;live.provider=route.primary.provider;live.kickoff=nowClock().utc;live.phase=Phase::kLive;
 store.upsert(live,nowClock());
 store.recordDataset(1,MatchStore::Dataset::kFixtures,long_uptime-100,long_uptime,true,Cause::kNone);
 assert(!schedule::datasetStale(store,1,MatchStore::Dataset::kFixtures,nowClock().utc,long_uptime+cfg::kDataStaleS*1000LL,route.primary.provider));
 assert(schedule::datasetStale(store,1,MatchStore::Dataset::kFixtures,nowClock().utc,long_uptime+cfg::kDataStaleS*1000LL+1,route.primary.provider));
 assert(!schedule::datasetStale(store,1,MatchStore::Dataset::kTable,nowClock().utc,long_uptime+cfg::kDataStaleS*1000LL+1,route.primary.provider));
 assert(!schedule::datasetStale(store,1,MatchStore::Dataset::kFixtures,nowClock().utc,long_uptime+cfg::kDataStaleS*1000LL+1,cfg::Provider::kEspn));
 assert(store.datasetNotice(1,MatchStore::Dataset::kFixtures,nowClock().utc+cfg::kUnmaintainedAfterS+1)==Cause::kUnmaintained);
 std::printf("PASS actual runJob/publish: Table503→FixturesOK preserves Tableerror+timestamps; inverse failure preserved; busy never becomes HTTP attempt; same-dataset recovery clears only itself. DatasetStatus=%zu Store=%zu Slot=%zu\n",sizeof(DatasetStatus),sizeof(MatchStore),sizeof(Slot));
}
'''
with tempfile.TemporaryDirectory(prefix="football-dataset-status-") as directory:
    scratch = Path(directory)
    (scratch / "esp_err.h").write_text("#pragma once\nusing esp_err_t=int;\ninline constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=1,ESP_ERR_INVALID_RESPONSE=2,ESP_ERR_INVALID_SIZE=3,ESP_ERR_NO_MEM=4;\n")
    unit = scratch / "test.cpp"
    unit.write_text(prelude + slot + '\nstd::array<Slot,cfg::kCompetitionMax> slots;\n' +
                    function('void publish() {') + '\n' + function('void runJob(') + main)
    command = ['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(scratch)]
    for path in ('include', 'components/football/include', 'components/settings/include',
                 'components/json/include', 'components/timekeeping/include'):
        command += ['-I', str(root / path)]
    command += [str(unit)]
    for path in ('components/football/football_model.cpp', 'components/football/match_store.cpp',
                 'components/football/matchday.cpp', 'components/football/store_views.cpp', 'components/football/request_scheduler.cpp', 'components/timekeeping/civil_time.cpp'):
        command += [str(root / path)]
    binary = scratch / 'dataset'
    subprocess.run([*command, '-o', str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
