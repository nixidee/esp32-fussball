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
prelude = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
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
namespace settings { uint32_t generation=5; uint32_t dataGeneration(){return generation;} }
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
 football::Stream complete_season(&season,5,http.body_limit,1000);assert(document(complete_season)&&complete_season.complete());
 Client previous_cap{season.body};previous_cap.chunk=1024;
 football::Stream clipped(&previous_cap,5,cfg::kProviderBodyBytes,1000);assert(!document(clipped)&&clipped.tooLarge());
 for(std::size_t chunk:{1,3,1024}){Client client{R"({"unknown":["😀",true,123]})"};client.chunk=chunk;
 football::Stream stream(&client,5,client.body.size(),1000);assert(document(stream)&&stream.complete()&&!stream.failed());}
 Client exact{"null"};football::Stream limit(&exact,5,3,1000);assert(!document(limit)&&limit.failed()&&limit.tooLarge()&&!limit.complete());
 Client broken{"null"};broken.complete=false;football::Stream eof(&broken,5,4,1000);assert(document(eof)&&!eof.complete());
 Client faulty{"null"};faulty.fail=true;football::Stream failed(&faulty,5,4,1000);assert(!document(failed)&&failed.failed());
 Client payload{"null"};football::Stream stale(&payload,5,4,1000);settings::generation=6;assert(!document(stale)&&stale.failed());settings::generation=5;
 Client same{"null"};football::Stream changed(&same,5,4,1000);network::epoch++;assert(!document(changed)&&changed.failed());network::epoch--;
 Client absent{"null"};football::Stream offline(&absent,5,4,1000);network::online=false;assert(!document(offline)&&offline.failed());network::online=true;
 Client slow{"null"};football::Stream deadline(&slow,5,4,1000);timekeeping::now=1000;assert(!document(deadline)&&deadline.failed());timekeeping::now=0;
 Client tail{"{} trailing"};football::Stream suffix(&tail,5,64,1000);assert(!document(suffix));
 Client unread{"{}       "};unread.chunk=1024;football::Stream buffered(&unread,5,64,1000);auto input=buffered.input();assert(input.read(input.context)=='{');assert(!buffered.complete());
 bool cancelled=false;football::provider::Sink consumer{&cancelled};consumer.cancelled=[](void*p){return *static_cast<bool*>(p);};
 Client abandoned{"null"};football::Stream closed(&abandoned,5,4,1000,&consumer);cancelled=true;assert(!document(closed)&&closed.failed());
 std::cout<<"actual provider Stream: real299845B season + adapter1MiB bound; old256KiB cap reproduced; chunks, EOF, suffix, stale/network/deadline/fault PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='football-provider-transport-') as folder:
    build = Path(folder)
    unit = build / 'stream.cpp'
    unit.write_text(prelude + stream + main)
    command = ['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I', str(ROOT / 'include'), '-I', str(ROOT / 'components/json/include'), '-I', str(ROOT / 'components/football/include'),
               '-I', str(ROOT / 'components/settings/include'), '-I', str(ROOT / 'components/timekeeping/include'),
               str(unit), str(ROOT / 'components/json/json_stream.cpp'), str(ROOT / 'components/football/provider_adapter.cpp'), '-o', str(build / 'stream')]
    subprocess.run(command, check=True)
    subprocess.run([str(build / 'stream'), str(ROOT / 'aidocs/Reviews/evidence_2026-10-17/provider-public/openliga-season.json')], check=True)
