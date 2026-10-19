#!/usr/bin/env python3
"""Actual discovery traversal/assessment/normal callback with real parser and credits."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'components/football/football_service.cpp').read_text()
def function(signature):
    start = source.index(signature)
    at = source.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
prelude = r'''
#include "esp_err.h"
#include <atomic>
#include <fstream>
#include <iterator>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "provider_coverage.h"
#include "provider_settings.h"
#include "provider_client.h"
namespace operations {struct Guard {explicit operator bool() const{return true;}};}
namespace network {struct State {uint32_t epoch=1;};State status(){return {};}}
namespace events {enum class Event{kDataUpdated};void post(Event){} }
namespace timekeeping {int64_t monotonicMs(){return 1000000;}}
namespace football {
MatchStore store;
settings::CompetitionRoutes routes;
settings::screen_policy::Eligibility eligibility;
coverage::EvidenceArray coverage_evidence;
coverage::Discovery discovery;
struct {uint8_t source=0;} discovery_progress;
uint64_t discovery_binding=0,coverage_binding=1;
uint8_t route_count=0;
bool discovery_retire=false,coverage_streaming=false,sent=false,persist_ok=true;
std::atomic<bool> busy=false;
Cause failure_cause=Cause::kNone;
std::vector<std::string> requests;
plan::Request request;
struct {bool acceptsMatch(const provider::Fixture&) const{return true;} bool consume(const provider::Fixture&,std::span<const provider::Event>){return true;}} planner;
Clock nowClock(){return {1792238400,1000};}
void lock(){} void unlock(){} void publish(){}
struct {uint32_t admissibleAt(cfg::Provider,uint32_t now){return now;}} scheduler;
bool storeDiscovery(const coverage::DiscoveryRecord& record){std::array<uint8_t,coverage::record::kGuardBytes> bytes;return persist_ok&&coverage::record::encodeGuard(record,bytes)==bytes.size();}
enum class HttpPurpose{kDiscovery};
bool reserveDiscovery(uint32_t utc_day);
bool finishDiscovery();
RequestOutcome get(const provider::RequestSpec& spec,const settings::ProviderSettings&,uint32_t,uint32_t,uint32_t,const provider::Sink& sink,HttpPurpose,uint8_t,const operations::Guard*){
 RequestOutcome result;
 if(!reserveDiscovery(nowClock().utc/86400)){result.fail(ESP_FAIL,Cause::kCounter);return result;}
 result.sent=true;requests.emplace_back(spec.source.season);
 const auto row=[&](unsigned id,const char* date){return std::string("{\"matchID\":")+std::to_string(id)+",\"leagueShortcut\":\"bl1\",\"leagueSeason\":"+std::string(spec.source.season)+",\"matchDateTimeUTC\":\""+date+"\"}";};
 const std::string bytes="["+row(1,"2026-09-01T12:00Z")+","+row(2,"2026-12-01T12:00Z")+"]";
 struct Input {const std::string& bytes;std::size_t at=0;} input{bytes};
 json::Reader reader({&input,[](void* raw){auto& in=*static_cast<Input*>(raw);return in.at<in.bytes.size()?static_cast<unsigned char>(in.bytes[in.at++]):-1;}});
 result.parsed=provider::readResponse(reader,spec,sink);
 result.code=result.parsed.status==provider::Status::kOk?ESP_OK:ESP_FAIL;return result;
}
'''
main = r'''
}
int main(int argc,char** argv){using namespace football;
 assert(argc==2);
 settings::ProviderSettings model;
 SourceKey current;assert(current.assign(cfg::Provider::kOpenLigaDb,"bl1","2026"));
 SourceKey next;assert(coverage::seasonSource(current,1,next));
 for(uint8_t i=0;i<cfg::kCompetitionMax;++i){
  settings::CompetitionRoute entry;entry.id=i+1;entry.incarnation=i+1;entry.primary.competition.assign("bl1");entry.primary.season.assign("2026");entry.club.team=100+i;
  assert(routes.entries.push(entry));assert(store.bind(entry.id,entry.incarnation,entry.club.team,nowClock()));assert(store.resolve(entry.id,current,nowClock()));
 }
 route_count=routes.entries.size();eligibility.selection.automatic_break=true;eligibility.break_days=21;
 discovery.load(budget::Load::kMissing);
 assert(bindCoverage());assert(!bindCoverage());
 assert(stepDiscovery(model,1,1,nowClock().utc));assert(stepDiscovery(model,1,1,nowClock().utc));
 assert(!stepDiscovery(model,1,1,nowClock().utc));
 assert((requests==std::vector<std::string>{"2026","2027"}));
 assert(discovery.stored().requests.reserved>=requests.size());assert(!discovery.stored().active);
 assert(assessCoverage(nowClock().utc,21).verdict==coverage::Verdict::kBreak);
 request.spec.source={current.provider,current.competition.view(),current.season.view()};
 provider::Fixture row;row.source_matches=true;row.time_definite=true;row.facts.kickoff=nowClock().utc;
 assert(consumeLocked(row,{},&routes.entries[0]));
 assert(coverage::find(coverage_evidence,current)->span.reason==coverage::Reason::kTimingChanged);
 assert(coverage::find(coverage_evidence,next)->span.reason==coverage::Reason::kTimingChanged);
 assert(assessCoverage(nowClock().utc,21).verdict==coverage::Verdict::kUnknown);
 // Equal exact source after removing/recreating a configured view retains facts.
 ++routes.entries[0].incarnation;assert(store.bind(1,routes.entries[0].incarnation,100,nowClock()));assert(store.resolve(1,current,nowClock()));assert(!bindCoverage());
 // A2027 explicit preference overlaps one proof, adding2028 only once.
 auto& moved=routes.entries[7];moved.primary.season.assign("2027");assert(store.resolve(moved.id,next,nowClock()));assert(bindCoverage());
 unsigned count=0;for(const auto& proof:coverage_evidence)count+=proof.source.valid();assert(count==3);
 assert(assessCoverage(nowClock().utc,21).verdict==coverage::Verdict::kUnknown);
 // A failed durable credit reservation forbids the HTTP send.
 discovery.load(budget::Load::kMissing);discovery_progress={};discovery_binding=0;requests.clear();persist_ok=false;
 assert(!stepDiscovery(model,1,1,nowClock().utc));assert(requests.empty());
 std::ifstream file(argv[1]);assert(file);const std::string recorded{std::istreambuf_iterator<char>(file),{}};
 coverage::Query query;assert(coverage::discoveryQuery(current,nowClock().utc,21,query));
 coverage::Response response;response.begin(query,nowClock().utc);
 struct Recorded {const std::string& bytes;std::size_t at=0;} recorded_input{recorded};
 json::Reader reader({&recorded_input,[](void* raw){auto& in=*static_cast<Recorded*>(raw);return in.at<in.bytes.size()?static_cast<unsigned char>(in.bytes[in.at++]):-1;}});
 provider::Sink sink;sink.context=&response;sink.fixture=[](const provider::Fixture& row,std::span<const provider::Event>,void* raw){static_cast<coverage::Response*>(raw)->observe(row);return true;};
 const auto parsed=provider::readResponse(reader,query.http.spec,sink);response.parsed(parsed);
 assert(parsed.rows==306&&parsed.status==provider::Status::kOk);
 assert(response.finish(true,nowClock().utc).completeness==coverage::Completeness::kComplete);
 std::puts("PASS real306-row coverage and actual discovery: eight views/two exact requests, overlapping seasons, shared read proof, callback family invalidation, incarnation reuse and failed-credit no-send");
}
'''
with tempfile.TemporaryDirectory(prefix='source-coverage-worker-') as directory:
    folder=Path(directory)
    (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;inline constexpr int ESP_OK=0,ESP_FAIL=-1;\n')
    signatures=('coverage::Requirements coverageRequirements()', 'bool bindCoverage() {',
                'coverage::Assessment assessCoverage(uint32_t utc, uint16_t days) {',
                'uint8_t coverageRoute(', 'bool reserveDiscovery(uint32_t utc_day) {',
                'bool finishDiscovery() {', 'bool consumeLocked(', 'bool stepDiscovery(')
    unit=folder/'worker.cpp';unit.write_text(prelude+'\n'.join(function(s) for s in signatures)+main)
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(folder)]
    for part in ('include','components/football/include','components/settings/include','components/json/include','components/timekeeping/include','components/network/include'):command+=['-I'+str(ROOT/part)]
    files=('components/football/football_model.cpp','components/football/source_index.cpp','components/football/match_store.cpp','components/football/schedule_coverage.cpp','components/football/provider_coverage.cpp','components/football/provider_adapter.cpp','components/football/provider_readers.cpp','components/football/provider_selection.cpp','components/football/budget_ledger.cpp','components/football/matchday.cpp','components/settings/settings_model.cpp','components/settings/settings_record.cpp','components/settings/screen_policy.cpp','components/network/wifi_policy.cpp','components/timekeeping/civil_time.cpp','components/json/json_stream.cpp')
    command += [str(unit), *(str(ROOT/f) for f in files),'-o',str(folder/'check')]
    subprocess.run(command,check=True)
    subprocess.run([str(folder/'check'),str(ROOT/'aidocs/Reviews/evidence_2026-10-17/provider-public/openliga-season.json')],check=True)
