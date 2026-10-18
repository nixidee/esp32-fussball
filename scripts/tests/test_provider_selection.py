#!/usr/bin/env python3
"""Real selection adapter and full-original string callbacks; no network mock claims."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
UNIT = r'''
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "provider_adapter.h"
using namespace football;
using namespace football::provider;
struct Input {std::string value;size_t at=0;static int read(void*p){auto&s=*static_cast<Input*>(p);return s.at<s.value.size()?static_cast<unsigned char>(s.value[s.at++]):-1;}};
struct Rows {std::vector<CompetitionSelection> competitions;std::vector<TeamSelection> teams;size_t count()const{return competitions.size()+teams.size();}std::string encoded;json::Writer writer{{this,[](std::string_view s,void*p){static_cast<Rows*>(p)->encoded+=s;return true;}}};
OriginalsSink originals{this,[](void*p){return static_cast<Rows*>(p)->writer.object();},[](uint32_t id,void*p){return static_cast<Rows*>(p)->writer.field("id",id);},[](OriginalField f,void*p){auto&w=static_cast<Rows*>(p)->writer;return w.key(f==OriginalField::kName?"name":"short_name")&&w.beginString();},[](std::string_view s,void*p){return static_cast<Rows*>(p)->writer.stringChunk(s);},[](OriginalField,void*p){return static_cast<Rows*>(p)->writer.endString();},[](void*p){return static_cast<Rows*>(p)->writer.end();}};
};
ParseResult parse(std::string body,cfg::Provider provider,bool teams,Rows& rows,bool originals=false){
Input input{std::move(body)};json::Reader reader({&input,Input::read});Sink sink{&rows};
sink.competition_selection=[](const CompetitionSelection&s,void*p){static_cast<Rows*>(p)->competitions.push_back(s);return true;};
sink.team_selection=[](const TeamSelection&s,void*p){static_cast<Rows*>(p)->teams.push_back(s);return true;};
if(originals){rows.writer.array();sink.originals=&rows.originals;}
auto result=readSelectionResponse(reader,{{provider,"ger.1","2026"},teams?Operation::kTeams:Operation::kCompetitions,0,0,0,0,0,{}},sink);
if(originals&&result.status==Status::kOk)assert(rows.writer.end()&&rows.writer.finish());return result;}
void good(std::string body,cfg::Provider provider,bool teams,size_t count){Rows rows;auto result=parse(body,provider,teams,rows);assert(result.status==Status::kOk&&result.document_complete&&rows.count()==count);}
void bad(std::string body,cfg::Provider provider,bool teams){Rows rows;auto result=parse(body,provider,teams,rows);assert(result.status!=Status::kOk&&!result.metadata.metadata_complete);}
int main(int argc,char**argv){
SelectionPlan preparation;
assert(prepareSelection({cfg::Provider::kOpenLigaDb,"bl1",""},Operation::kTeams,0,preparation)&&preparation.discover&&preparation.season.empty());
assert(prepareSelection({cfg::Provider::kApiFootball,"78","2026"},Operation::kTeams,0,preparation)&&!preparation.discover&&std::string_view(preparation.season.data)=="2026");
assert(prepareSelection({cfg::Provider::kEspn,"ger.1",""},Operation::kOriginalNames,0,preparation)&&!preparation.discover);
assert(!prepareSelection({cfg::Provider::kOpenLigaDb,"bl1",""},Operation::kCompetitions,0,preparation));
assert(prepareSelection({cfg::Provider::kOpenLigaDb,"bl1","2026"},Operation::kCompetitions,0,preparation)&&std::string_view(preparation.season.data)=="2026");
assert(!prepareSelection({cfg::Provider::kEspn,"ger.1","2026"},Operation::kFixtures,0,preparation));

good(R"([{"teamName":"Bayern","shortName":null,"teamId":40}])",cfg::Provider::kOpenLigaDb,true,1);
good(R"([{"leagueSeason":2026,"leagueName":"Bundesliga","sport":{"sportId":1},"leagueShortcut":"bl1"},{"leagueSeason":2026,"leagueName":"Other","sport":{"sportId":2},"leagueShortcut":"other"}])",cfg::Provider::kOpenLigaDb,false,1);
good(R"({"response":[{"team":{"code":"FCB","name":"Bayern","id":40}}],"results":1,"errors":[],"paging":{"total":1,"current":1}})",cfg::Provider::kApiFootball,true,1);
good(R"({"response":[{"seasons":[{"current":false,"year":2025},{"current":true,"year":2026}],"league":{"name":"Bundesliga","id":78}}],"errors":{},"results":1})",cfg::Provider::kApiFootball,false,1);
good(R"({"competitions":[{"currentSeason":{"startDate":"2026-08-01"},"name":"Bundesliga","id":2002}],"count":1})",cfg::Provider::kFootballData,false,1);
good(R"({"teams":[{"shortName":"FCB","name":"Bayern","id":40}],"count":1})",cfg::Provider::kFootballData,true,1);
good(R"({"leagues":[{"name":"Bundesliga","slug":"ger.1"}]})",cfg::Provider::kEspn,false,1);
good(R"({"sports":[{"leagues":[{"teams":[{"team":{"displayName":"Bayern","id":"40","abbreviation":"FCB"}}],"slug":"ger.1"}]}]})",cfg::Provider::kEspn,true,1);
for(auto provider:{cfg::Provider::kApiFootball,cfg::Provider::kFootballData,cfg::Provider::kEspn})bad("{}",provider,true);
good("[]",cfg::Provider::kOpenLigaDb,true,0);
good(R"({"response":[],"results":0,"errors":[]})",cfg::Provider::kApiFootball,true,0);
bad(R"([{"teamId":0,"teamName":"Bayern"}])",cfg::Provider::kOpenLigaDb,true);
bad(R"([{"teamId":40,"teamId":41,"teamName":"Bayern"}])",cfg::Provider::kOpenLigaDb,true);
bad(R"([{"teamId":40,"teamName":"Bayern"}] garbage)",cfg::Provider::kOpenLigaDb,true);
bad(R"({"response":[],"results":1,"errors":[]})",cfg::Provider::kApiFootball,true);
bad(R"({"response":[],"errors":{"token":"bad"}})",cfg::Provider::kApiFootball,true);
bad(R"({"response":[],"errors":[],"paging":{"current":1,"total":2}})",cfg::Provider::kApiFootball,true);
bad(R"({"sports":[{"leagues":[{"teams":[],"slug":"other"}]}]})",cfg::Provider::kEspn,true);
for(const auto* id:{"0","4294967296","\"abc\"","-1","1.5"})
 bad(std::string("[{\"teamId\":")+id+",\"teamName\":\"Invalid\"}]",cfg::Provider::kOpenLigaDb,true);
Rows full_id;auto full=parse(R"([{"teamId":4294967295,"teamName":"Maximum"}])",cfg::Provider::kOpenLigaDb,true,full_id);
assert(full.status==Status::kOk&&full_id.teams.size()==1&&full_id.teams[0].id==UINT32_MAX);
std::string longname;for(int i=0;i<10000;++i)longname+="ä😀";
Rows originals;auto result=parse("[{\"shortName\":null,\"teamName\":\""+longname+"\",\"teamId\":40}]",cfg::Provider::kOpenLigaDb,true,originals,true);
assert(result.status==Status::kOk&&originals.encoded=="[{\"name\":\""+longname+"\",\"id\":40}]");
bad("[{\"teamId\":40,\"teamName\":\""+longname+"\\u0000\"}]",cfg::Provider::kOpenLigaDb,true);
Rows empty;result=parse(R"([{"teamName":"","teamId":40,"shortName":""}])",cfg::Provider::kOpenLigaDb,true,empty,true);assert(result.status==Status::kOk&&empty.encoded.find("\"short_name\":\"\"")!=std::string::npos);
for(int i=1;i<argc;++i){std::ifstream file(argv[i]);assert(file);std::string body{std::istreambuf_iterator<char>(file),{}};Rows rows;auto result=parse(body,cfg::Provider::kEspn,true,rows,true);assert(result.status==Status::kOk&&!rows.teams.empty());std::cout<<argv[i]<<": "<<body.size()<<" bytes, "<<rows.count()<<" teams, full originals PASS\n";}
std::cout<<"typed selection/full-original callbacks: four providers, complete empty, duplicates, metadata, suffix, 60KiB UTF8, NUL and null fields PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='provider-selection-') as folder:
    build = Path(folder)
    unit = build / 'selection.cpp'
    unit.write_text(UNIT)
    binary = build / 'selection'
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-g',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I'+str(ROOT/'include'), '-I'+str(ROOT/'components/football/include'),
        '-I'+str(ROOT/'components/json/include'), '-I'+str(ROOT/'components/timekeeping/include'), '-I'+str(ROOT/'components/settings/include'), str(unit),
        str(ROOT/'components/football/provider_selection.cpp'),
        str(ROOT/'components/football/provider_adapter.cpp'),
        str(ROOT/'components/json/json_stream.cpp'), '-o',str(binary)],check=True)
    fixtures = sorted(ROOT.glob('aidocs/Reviews/evidence_2026-10-17/**/espn-ger1-teams.json'))
    subprocess.run([str(binary), *map(str,fixtures)], check=True)
