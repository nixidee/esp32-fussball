#!/usr/bin/env python3
"""Replay the actual worker lane branches with the real request scheduler."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'components/football/football_service.cpp').read_text()
worker = source[source.index('void worker(void*) {'):]
def block(pattern):
    match = re.search(pattern, worker)
    assert match, pattern
    start = match.start()
    at = worker.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (worker[end] == '{') - (worker[end] == '}')
        end += 1
    return worker[start:end]
interactive = block(r'if \(selecting\.load\(\) && \(!automatic_turn \|\| !decision\.run\)\)')
automatic = block(r'if \(decision\.run\)')
assert worker.index('decision = scheduler.next(inputs)') < worker.index(interactive)
assert re.search(r'if \(!model\.demo && !settings::footballPaused\(\)\)', worker)
program = r'''
#include "provider_plan.h"
#include "provider_settings.h"
#include "provider_client.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace football;
namespace network { struct Status {uint32_t epoch=1;}; Status status(){return {};} }
struct Case {
 schedule::Scheduler scheduler;
 settings::ProviderSettings model;
 MatchStore store;
 schedule::Inputs inputs;
 std::atomic<bool> selecting{true};
 std::atomic<uint32_t> original_query{0};
 bool automatic_turn=false,terminal=false,refused=false,auto_refused=false,frame=false;
 uint32_t generation=1,settings_seen=1;
 int64_t utc=1000000;
 std::vector<char> trace;
 unsigned auto_tries=0;
 Case(){
  model.providers=settings::providerPlansDefault();
  for(auto& provider:model.providers){provider.limit={};provider.interval_s[static_cast<unsigned>(cfg::DataType::kLive)]=60;}
  SourceKey key;assert(key.assign(cfg::Provider::kEspn,"ger.1","2026"));
  assert(store.bind(1,1,0,{utc,1000}));const auto source=store.resolve(1,key,{utc,1000});assert(source);
  StoredMatch live;live.id=1;live.source=store.sourceSlot(source);live.kickoff=utc;live.phase=Phase::kLive;
  store.upsert(live,{utc,1000});
  inputs.plans=&model.providers;inputs.store=&store;inputs.daily_left.fill(schedule::kUnlimited);
  inputs.scopes[0].key.source=source;inputs.scopes[0].competition=1;inputs.scopes[0].provider=cfg::Provider::kEspn;inputs.scopes[0].types=1u<<static_cast<unsigned>(cfg::DataType::kLive);
 }
 RequestStep stepSelection(const settings::ProviderSettings&,uint32_t,uint32_t,int64_t,int64_t&){return send('I');}
 RequestStep stepOriginalNames(const settings::ProviderSettings&,uint32_t,uint32_t,int64_t&){return send('O');}
 RequestStep send(char lane){
  if(terminal){selecting=false;return {true,false};}
  if(refused)return {false,false};
  if(scheduler.admissibleAt(cfg::Provider::kOpenLigaDb,inputs.clock.mono_s)>inputs.clock.mono_s)return {false,false};
  scheduler.sent(cfg::Provider::kOpenLigaDb,inputs.clock.mono_s);trace.push_back(lane);return {true,true};
 }
 void runJob(const schedule::Job& job,const settings::ProviderSettings&,uint32_t,uint32_t,uint32_t){
  ++auto_tries;
  if(auto_refused){scheduler.finished(job,schedule::Outcome::kDeferred,inputs.clock.mono_s);return;}
  scheduler.sent(job.provider,inputs.clock.mono_s);scheduler.finished(job,schedule::Outcome::kOk,inputs.clock.mono_s);trace.push_back('A');
 }
 void run(unsigned loops){
  for(unsigned i=0;i<loops;++i){
   inputs.clock={utc+i,1000+i};
   scheduler.configure(model.providers,model.global_limit);
   schedule::Decision decision{};if(!frame)decision=scheduler.next(inputs);
   int64_t selection_wake=INT64_MAX;
   INTERACTIVE_BRANCH
   AUTOMATIC_BRANCH
  }
 }
 unsigned count(char lane)const{unsigned n=0;for(char value:trace)n+=value==lane;return n;}
};
int main(){
 Case continuous;continuous.run(400);assert(continuous.count('I')>10&&continuous.count('A')>=6);assert(continuous.trace.front()=='I'&&continuous.trace[1]=='A');
 Case window;window.scheduler.sent(cfg::Provider::kEspn,1000);window.model.providers[static_cast<unsigned>(cfg::Provider::kEspn)].limit.window_requests=1;window.model.providers[static_cast<unsigned>(cfg::Provider::kEspn)].limit.window_s=600;window.scheduler.configure(window.model.providers,{});window.scheduler.sent(cfg::Provider::kEspn,1000);window.automatic_turn=true;window.run(100);assert(window.count('A')==0&&window.count('I')>5);
 Case budget;budget.inputs.daily_left[static_cast<unsigned>(cfg::Provider::kEspn)]=0;budget.automatic_turn=true;budget.run(100);assert(budget.count('A')==0&&budget.count('I')>5);
 Case frame;frame.frame=true;frame.automatic_turn=true;frame.run(100);assert(frame.count('A')==0&&frame.count('I')>5);
 Case terminal;terminal.terminal=true;terminal.run(1);assert(terminal.count('A')==1&&terminal.count('I')==0);
 Case refusal;refusal.refused=true;refusal.run(1);assert(refusal.count('A')==1&&refusal.count('I')==0);
 Case auto_busy;auto_busy.automatic_turn=true;auto_busy.auto_refused=true;auto_busy.run(100);assert(auto_busy.auto_tries>0&&auto_busy.count('I')>5&&auto_busy.count('A')==0);
 Case originals;originals.original_query=1;originals.run(400);assert(originals.count('O')>10&&originals.count('A')>=6&&originals.count('I')==0);
 std::puts("PASS actual worker branches +real Scheduler: continuous interactive/originals cannot starve due Live; window/budget/frame/autoBusy do not stall interactive; terminal/refused steps do not spend a send");
}
'''.replace('INTERACTIVE_BRANCH', interactive).replace('AUTOMATIC_BRANCH', automatic)
with tempfile.TemporaryDirectory(prefix='worker-fairness-',dir='/tmp') as directory:
    folder=Path(directory)
    (folder/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;inline constexpr int ESP_OK=0;\n')
    unit=folder/'fairness.cpp';unit.write_text(program)
    command=['clang++','-std=c++20','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(folder)]
    for path in ('include','components/football/include','components/settings/include','components/timekeeping/include','components/json/include'):
        command+=['-I',str(root/path)]
    for path in ('components/football/source_index.cpp','components/football/football_model.cpp','components/football/match_store.cpp','components/football/request_scheduler.cpp','components/football/store_views.cpp','components/football/matchday.cpp','components/timekeeping/civil_time.cpp','components/settings/settings_model.cpp','components/settings/settings_record.cpp','components/settings/screen_policy.cpp','components/network/wifi_policy.cpp'):
        command.append(str(root/path))
    command+=['-I',str(root/'components/network/include'),str(unit),'-o',str(folder/'check')]
    subprocess.run(command,check=True,cwd=root)
    subprocess.run([str(folder/'check')],check=True,cwd=root)
