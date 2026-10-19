#!/usr/bin/env python3
"""Actual bounded provider Stream with SDK byte/EOF/deadline fault injection.

This exercises the transport-to-shared-Reader boundary, not DNS/TLS or hardware.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'components/football/provider_client.cpp').read_text()
start = source.index('class Stream {')
end = source.index('\n};', start) + 3
stream = source[start:end]
validity_start = source.index('struct RequestValidity {')
validity = source[validity_start:source.index('\n};', validity_start) + 3]
prelude = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <iterator>
#include <string>
#include "json_stream.h"
#include "provider_adapter.h"
struct Client { std::string body; std::size_t at=0, chunk=3; bool complete=true, fail=false; };
using esp_http_client_handle_t=Client*;
int esp_http_client_read(Client* client,char* out,int capacity){
 if(client->fail)return -1;
 const auto count=std::min({std::size_t(capacity),client->chunk,client->body.size()-client->at});
 std::memcpy(out,client->body.data()+client->at,count);client->at+=count;return int(count);
}
bool esp_http_client_is_complete_data_received(Client* c){return c->complete&&c->at==c->body.size();}
namespace settings { uint32_t generation=5; unsigned checks=0;uint32_t dataGeneration(){++checks;return generation;} }
namespace network { struct Status{uint32_t epoch;}; uint32_t epoch=2; bool online=true;
Status status(){return{epoch};}bool ready(){return online;} }
namespace timekeeping { int64_t now=0;int64_t monotonicMs(){return now;} }
namespace football {
'''
main = r'''
}
bool document(football::Stream& stream){json::Reader reader(stream.input());json::Token token;
 do {if(!reader.next(token))return false;}while(token.kind!=json::Kind::kEnd);return true;}
int main(int argc,char** argv){
 assert(argc==2);
 std::ifstream file(argv[1]);assert(file);
 Client season{std::string(std::istreambuf_iterator<char>(file),{})};season.chunk=1024;
 football::provider::RequestSpec query;query.source={cfg::Provider::kOpenLigaDb,"bl1","2026"};
 football::provider::HttpRequest http;assert(football::provider::buildRequest(query,http));
 assert(http.body_limit==cfg::kProviderSeasonBodyBytes&&season.body.size()>cfg::kProviderBodyBytes);
 football::RequestValidity complete_season_validity{5,network::epoch,1000};football::Stream complete_season(&season,complete_season_validity,http.body_limit);assert(document(complete_season)&&complete_season.complete());
 const auto season_checks=settings::checks;
 assert(season_checks < season.body.size()/8);
 std::cout<<"Real season validity evaluations="<<season_checks<<" bytes="<<season.body.size()<<"\n";
 Client previous_cap{season.body};previous_cap.chunk=1024;
 football::RequestValidity clipped_validity{5,network::epoch,1000};football::Stream clipped(&previous_cap,clipped_validity,cfg::kProviderBodyBytes);assert(!document(clipped)&&clipped.tooLarge());
 for(std::size_t chunk:{1,3,1024}){Client client{R"({"unknown":["😀",true,123]})"};client.chunk=chunk;
 football::RequestValidity stream_validity{5,network::epoch,1000};football::Stream stream(&client,stream_validity,client.body.size());assert(document(stream)&&stream.complete()&&!stream.failed());}
 Client exact{"null"};football::RequestValidity limit_validity{5,network::epoch,1000};football::Stream limit(&exact,limit_validity,3);assert(!document(limit)&&limit.failed()&&limit.tooLarge()&&!limit.complete());
 Client broken{"null"};broken.complete=false;football::RequestValidity eof_validity{5,network::epoch,1000};football::Stream eof(&broken,eof_validity,4);assert(document(eof)&&!eof.complete());
 Client faulty{"null"};faulty.fail=true;football::RequestValidity failed_validity{5,network::epoch,1000};football::Stream failed(&faulty,failed_validity,4);assert(!document(failed)&&failed.failed());
 Client payload{"null"};football::RequestValidity stale_validity{5,network::epoch,1000};football::Stream stale(&payload,stale_validity,4);settings::generation=6;assert(!document(stale)&&stale.failed());settings::generation=5;
 Client same{"null"};football::RequestValidity changed_validity{5,network::epoch,1000};football::Stream changed(&same,changed_validity,4);network::epoch++;assert(!document(changed)&&changed.failed());network::epoch--;
 Client absent{"null"};football::RequestValidity offline_validity{5,network::epoch,1000};football::Stream offline(&absent,offline_validity,4);network::online=false;assert(!document(offline)&&offline.failed());network::online=true;
 Client slow{"null"};football::RequestValidity deadline_validity{5,network::epoch,1000};football::Stream deadline(&slow,deadline_validity,4);timekeeping::now=1000;assert(!document(deadline)&&deadline.failed());timekeeping::now=0;
 Client tail{"{} trailing"};football::RequestValidity suffix_validity{5,network::epoch,1000};football::Stream suffix(&tail,suffix_validity,64);assert(!document(suffix));
 Client unread{"{}       "};unread.chunk=1024;football::RequestValidity buffered_validity{5,network::epoch,1000};football::Stream buffered(&unread,buffered_validity,64);auto input=buffered.input();assert(input.read(input.context)=='{');assert(!buffered.complete());
 bool cancelled=false;football::provider::Sink consumer{&cancelled};consumer.cancelled=[](void*p){return *static_cast<bool*>(p);};
 Client abandoned{"null"};football::RequestValidity closed_validity{5,network::epoch,1000,&consumer};football::Stream closed(&abandoned,closed_validity,4);cancelled=true;assert(!document(closed)&&closed.failed());
 // Every possible point inside a check interval preserves a bounded abort.
 unsigned worst_extra=0;long long worst_ns=0;
 for(unsigned fault=0;fault<5;++fault)for(unsigned prefix=0;prefix<cfg::kProviderValidityBytes;++prefix){
  bool stop=false;football::provider::Sink sink{&stop};sink.cancelled=[](void*p){return *static_cast<bool*>(p);};
  Client client{std::string(2048,' ')+"null"};client.chunk=1024;
  football::RequestValidity check{5,network::epoch,1000,&sink};football::Stream stream(&client,check,client.body.size());auto in=stream.input();
  for(unsigned i=0;i<prefix;++i)assert(in.read(in.context)==' ');
  const auto cancel_started=std::chrono::steady_clock::now();
  switch(fault){case 0:stop=true;break;case 1:++settings::generation;break;case 2:++network::epoch;break;case 3:network::online=false;break;case 4:timekeeping::now=1000;break;}
  unsigned extra=0;while(in.read(in.context)>=0)++extra;
  worst_ns=std::max(worst_ns,static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-cancel_started).count()));
  assert(extra<cfg::kProviderValidityBytes&&stream.failed()&&!stream.complete());worst_extra=std::max(worst_extra,extra);
  settings::generation=5;network::epoch=2;network::online=true;timekeeping::now=0;
 }
 assert(worst_extra==cfg::kProviderValidityBytes-1);
 std::cout<<"PASS320 abort positions: worst additional buffered bytes="<<worst_extra<<" host worst_ns="<<worst_ns<<"; final completion rejects expiry/cancellation; wall-time bound needs device measurement\n";
 std::cout<<"actual provider Stream: real299845B season + adapter1MiB bound; old256KiB cap reproduced; chunks, EOF, suffix, stale/network/deadline/fault PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='football-provider-transport-') as folder:
    build = Path(folder)
    unit = build / 'stream.cpp'
    unit.write_text(prelude + validity + stream + main)
    command = ['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I', str(ROOT / 'include'), '-I', str(ROOT / 'components/json/include'), '-I', str(ROOT / 'components/football/include'),
               '-I', str(ROOT / 'components/settings/include'), '-I', str(ROOT / 'components/timekeeping/include'),
               str(unit), str(ROOT / 'components/json/json_stream.cpp'), str(ROOT / 'components/football/provider_adapter.cpp'), '-o', str(build / 'stream')]
    command += ['-I', str(ROOT / 'components/network/include')]
    command += [str(ROOT / file) for file in (
        'components/settings/settings_model.cpp',
        'components/settings/screen_policy.cpp',
        'components/network/wifi_policy.cpp',
        'components/timekeeping/civil_time.cpp')]
    subprocess.run(command, check=True)
    subprocess.run([str(build / 'stream'), str(ROOT / 'aidocs/Reviews/evidence_2026-10-17/provider-public/openliga-season.json')], check=True)
