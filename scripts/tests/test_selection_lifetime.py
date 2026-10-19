#!/usr/bin/env python3
"""Actual selection owner/callback/worker-step lifetime with controlled HTTP."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'components/football/football_service.cpp').read_text()
def function(signature):
    start=source.index(signature);at=source.index('{',start);depth=1;end=at+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
def structure(name):
    start=source.index('struct '+name+' {');at=source.index('{',start);depth=1;end=at+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]+';'
assert 'selectionDeadline(),' in source and 'selectionAlive(run.query)' in source
assert 'selection_boot_nonce = esp_random();' in source
prelude=r'''
#include "football_service.h"
#include "provider_plan.h"
#include "provider_settings.h"
#include "provider_client.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <type_traits>
namespace settings {uint32_t current=1;uint32_t generation(){return current;}}
namespace timekeeping {int64_t now=1000;int64_t monotonicMs(){return now;}}
namespace network {struct Status{uint32_t epoch=1;};Status status(){return {};}}
namespace ota {bool trial(){return false;}}
namespace football {
void*mutex=reinterpret_cast<void*>(1);int depth=0;
void lock(){assert(depth==0);++depth;}void unlock(){assert(depth==1);--depth;}
bool reset_fence=false;
std::atomic<bool> selecting=false,busy=false;
std::atomic<uint32_t>original_query=0;
SelectionRequest selection_request;
SelectionStatus selection_state{};
uint32_t selection_boot_nonce=0x12345678,selection_counter=0,selection_settings_generation=0;
int64_t selection_deadline_ms=0;bool selection_cancelled=false;
schedule::Scheduler scheduler;
RequestOutcome requestFailure(esp_err_t error){RequestOutcome result;result.code=error;result.cause=error==ESP_OK?Cause::kNone:Cause::kBadResponse;return result;}
plan::Fetch fetchOf(const RequestOutcome& result){return result.code==ESP_OK?plan::Fetch::kOk:plan::Fetch::kFailed;}
enum class HttpPurpose{kSetup};
int mode=0;unsigned sends=0;
RequestOutcome get(const provider::RequestSpec& spec,const settings::ProviderSettings&,uint32_t,uint32_t,uint32_t,const provider::Sink&sink,HttpPurpose){
 assert(spec.operation==provider::Operation::kTeams&&spec.source.provider==cfg::Provider::kEspn&&spec.source.competition=="ger.1"&&spec.source.season=="2026"&&!spec.upcoming&&std::string_view(spec.country)=="Germany");
 RequestOutcome result;result.sent=true;++sends;assert(sink.cancelled&&!sink.cancelled(sink.context));
 if(mode==1)assert(cancelSelection(selection_state.query)==ESP_OK);
 if(mode==2)timekeeping::now=selection_deadline_ms;
 if(mode==3)++settings::current;
 TeamSelection row;row.id=91;row.name.assign("Owner result");
 if(mode){assert(sink.cancelled(sink.context));assert(!sink.team_selection(row,sink.context));result.fail(ESP_ERR_INVALID_STATE,Cause::kSettings);return result;}
 assert(sink.team_selection(row,sink.context));return result;
}
'''
parts=[structure('SelectionRows'),'SelectionRows selections;']
parts += [function(x) for x in ('bool selectionCurrentLocked(', 'bool selectionAlive(', 'int64_t selectionDeadline(')]
parts += [structure('SelectContext'),function('template <std::size_t N>\nvoid fold('),function('template <class Row>\nbool selectionRow('),function('void finishSelection('),structure('SelectionRun'),'SelectionRun run;',function('void endSelection('),function('void setSelectionWait('),function('RequestStep stepSelection('),function('esp_err_t start(const SelectionRequest&'),function('esp_err_t select(const SelectionRequest&'),function('esp_err_t selectionStatus('),function('template <class Row>\nbool readSelectionRow('),function('bool selectionAt(SelectionQuery query, uint16_t index, uint32_t revision,\n                 CompetitionSelection& out)'),function('bool selectionAt(SelectionQuery query, uint16_t index, uint32_t revision,\n                 TeamSelection& out)'),function('esp_err_t cancelSelection(')]
tests=r'''
void reset(){selection_state={};selection_request={};run={};selecting=false;busy=false;original_query=0;selection_counter=0;selection_settings_generation=0;selection_deadline_ms=0;selection_cancelled=false;selection_boot_nonce=0x12345678;settings::current=1;timekeeping::now=1000;mode=0;sends=0;scheduler.reset();}
SelectionRequest request(){SelectionRequest out;out.provider=cfg::Provider::kEspn;out.competition.assign("ger.1");out.season.assign("2026");out.teams=true;out.country.assign("Germany");return out;}
SelectionQuery begin(){SelectionQuery query=99;assert(select(request(),query)==ESP_OK&&query&&selecting&&selection_state.busy);return query;}
void step(){settings::ProviderSettings model;int64_t wake=INT64_MAX;assert(stepSelection(model,1,settings::current,1792238400,wake).progressed);}
} // namespace football
int main(){using namespace football;
 reset();const auto a=begin();assert(a==(SelectionQuery{0x12345678}<<32|1));
 SelectionQuery rejected=99;assert(select(request(),rejected)==ESP_ERR_NOT_FINISHED&&rejected==0);
 const auto saved_request=request();SelectContext old{&saved_request,a};TeamSelection row;row.id=40;row.name.assign("A result");
 assert(selectionRow(row,&old));finishSelection(a,requestFailure(ESP_OK));
 SelectionStatus out{};assert(selectionStatus(a,out)==ESP_OK&&out.query==a&&!out.busy&&out.count==1);
 const auto revision=out.revision;TeamSelection value;assert(selectionAt(a,0,revision,value)&&std::string_view(value.name.data)=="A result");
 CompetitionSelection wrong;wrong.name.assign("Unchanged");
 assert(!selectionAt(a,0,revision,wrong)&&std::string_view(wrong.name.data)=="Unchanged");
 const auto b=begin();assert(b!=a);
 out.query=999;assert(selectionStatus(a,out)==ESP_ERR_NOT_FOUND&&out.query==999);
 value.name.assign("unchanged");assert(!selectionAt(a,0,revision,value)&&std::string_view(value.name.data)=="unchanged");
 assert(!selectionRow(row,&old));finishSelection(a,requestFailure(ESP_FAIL));endSelection(a,requestFailure(ESP_FAIL));setSelectionWait(a,Cadence::kRetry,9999);
 assert(selecting&&selection_state.busy&&selection_state.query==b&&selection_state.count==0&&selection_state.wait==Cadence::kNone);
 assert(cancelSelection(a)==ESP_ERR_NOT_FOUND&&selectionAlive(b));
 assert(cancelSelection(b)==ESP_OK&&!selectionAlive(b)&&selecting&&selection_state.busy);
 rejected=99;assert(select(request(),rejected)==ESP_ERR_NOT_FINISHED&&rejected==0);
 step();assert(!selecting&&!selection_state.busy);assert(selectionStatus(b,out)==ESP_ERR_NOT_FOUND);
 const auto c=begin();assert(c!=b);step();assert(selectionStatus(c,out)==ESP_OK&&out.count==1&&out.query==c);
 for(int cancellation:{1,2,3}){reset();const auto query=begin();mode=cancellation;step();assert(sends==1&&!selecting&&!selection_state.busy&&selection_state.count==0);assert(selectionStatus(query,out)==ESP_ERR_NOT_FOUND);}
 reset();const auto lease=begin();const auto initial=selection_deadline_ms;
 timekeeping::now=initial-1;assert(selectionStatus(lease,out)==ESP_OK&&selection_deadline_ms==timekeeping::now+cfg::kOperationDeadlineMs);
 const auto renewed=selection_deadline_ms;assert(selectionStatus(lease+1,out)==ESP_ERR_NOT_FOUND&&selection_deadline_ms==renewed);
 timekeeping::now=renewed;assert(selectionStatus(lease,out)==ESP_ERR_NOT_FOUND&&selection_deadline_ms==renewed&&selecting);step();assert(!selecting&&!selection_state.busy&&sends==0);
 reset();const auto waiting=begin();settings::ProviderSettings model;model.providers=settings::providerPlansDefault();for(auto& provider:model.providers)provider.limit={};auto& limit=model.providers[static_cast<unsigned>(cfg::Provider::kEspn)].limit;limit.window_requests=1;limit.window_s=600;scheduler.configure(model.providers,{});scheduler.sent(cfg::Provider::kEspn,timekeeping::now/1000);
 int64_t wake=INT64_MAX;assert(!stepSelection(model,1,settings::current,1792238400,wake).progressed&&run.active&&wake>selection_deadline_ms&&selectionDeadline()==selection_deadline_ms);
 for(unsigned poll=0;poll<4;++poll){timekeeping::now+=cfg::kOperationDeadlineMs-1;assert(selectionStatus(waiting,out)==ESP_OK);assert(!stepSelection(model,1,settings::current,1792238400,wake).progressed);}
 assert(selecting&&sends==0);timekeeping::now=selection_deadline_ms;assert(stepSelection(model,1,settings::current,1792238400,wake).progressed&&!selecting&&sends==0);
 reset();const auto before_reboot=begin();finishSelection(before_reboot,requestFailure(ESP_OK));selection_boot_nonce=0x87654321;selection_counter=0;const auto after_reboot=begin();assert(before_reboot!=after_reboot&&selectionStatus(before_reboot,out)==ESP_ERR_NOT_FOUND);
 finishSelection(after_reboot,requestFailure(ESP_OK));selection_counter=UINT32_MAX;rejected=99;assert(select(request(),rejected)==ESP_ERR_INVALID_STATE&&rejected==0&&selection_state.query==after_reboot);
 // Reuse every pool row across purposes; kind checks precede union access.
 reset();auto competitions=request();competitions.teams=false;SelectionQuery list;
 assert(select(competitions,list)==ESP_OK);SelectContext page{&competitions,list};
 for(unsigned i=0;i<cfg::kSelectionEntries;++i){CompetitionSelection item;item.id.assign(std::to_string(i+1));item.name.assign("Competition");assert(selectionRow(item,&page));}
 CompetitionSelection extra;extra.id.assign("extra");extra.name.assign("Extra");assert(selectionRow(extra,&page));
 finishSelection(list,requestFailure(ESP_OK));assert(selection_state.more&&selection_state.count==cfg::kSelectionEntries);
 CompetitionSelection last;assert(selectionAt(list,cfg::kSelectionEntries-1,selection_state.revision,last)&&std::string_view(last.id.data)=="64");
 const auto teams=begin();const auto team_request=request();SelectContext team_page{&team_request,teams};
 TeamSelection maximum;maximum.id=UINT32_MAX;maximum.name.assign("Maximum");assert(selectionRow(maximum,&team_page));
 finishSelection(teams,requestFailure(ESP_OK));TeamSelection numeric;assert(selectionAt(teams,0,selection_state.revision,numeric)&&numeric.id==UINT32_MAX);
 last.name.assign("Unchanged");assert(!selectionAt(teams,0,selection_state.revision,last)&&std::string_view(last.name.data)=="Unchanged");
 std::puts("PASS actual selection API/callback/worker: browserA/B isolation, late-row/end guards, cancel+ack, generation, reboot nonce, no-wrap, sink cancellation/expiry and matching-owner idle renewal through provider-window waits");
}
'''
with tempfile.TemporaryDirectory(prefix='selection-lifetime-',dir='/tmp') as directory:
    folder=Path(directory)
    (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\ninline constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NOT_FOUND=1,ESP_ERR_NOT_FINISHED=2,ESP_ERR_INVALID_STATE=3,ESP_ERR_INVALID_ARG=4,ESP_ERR_INVALID_RESPONSE=5;\n')
    unit=folder/'selection.cpp';unit.write_text(prelude+'\n'.join(parts)+tests)
    command=['clang++','-std=c++20','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(folder)]
    for path in ('include','components/football/include','components/settings/include','components/timekeeping/include','components/json/include','components/network/include'):command+=['-I',str(root/path)]
    for path in ('components/football/provider_adapter.cpp','components/football/football_model.cpp','components/football/match_store.cpp','components/football/source_index.cpp','components/football/request_scheduler.cpp','components/football/store_views.cpp','components/football/matchday.cpp','components/timekeeping/civil_time.cpp','components/settings/settings_model.cpp','components/settings/settings_record.cpp','components/settings/screen_policy.cpp','components/network/wifi_policy.cpp'):command.append(str(root/path))
    subprocess.run([*command,str(unit),'-o',str(folder/'check')],check=True,cwd=root)
    subprocess.run([str(folder/'check')],check=True,cwd=root)
