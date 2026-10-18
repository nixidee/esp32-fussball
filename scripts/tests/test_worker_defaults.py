#!/usr/bin/env python3
"""Prove the flash-image retirement preserves actual worker-owned defaults."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'components/football/football_service.cpp').read_text()
parts=[]
for kind,name in (('WorkerInputs','inputs'),('WorkerRoutes','routes'),('WorkerCoverage','coverage_evidence')):
    match=re.search(r'struct '+kind+r'\b.*?\};\s*'+kind+r' '+name+r';',source,re.S)
    assert match,kind
    parts.append(match.group())
program=r'''
#include "provider_plan.h"
#include "schedule_coverage.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace football;
ACTUAL_WORKER_DEFAULTS
static schedule::Inputs normal_inputs;
static settings::CompetitionRoutes normal_routes;
static coverage::EvidenceArray normal_coverage;
static_assert(sizeof(inputs)==sizeof(normal_inputs));
static_assert(sizeof(routes)==sizeof(normal_routes));
static_assert(sizeof(coverage_evidence)==sizeof(normal_coverage));
int main(){
 assert(!std::memcmp(&inputs,&normal_inputs,sizeof(inputs)));
 assert(!std::memcmp(&routes,&normal_routes,sizeof(routes)));
 assert(!std::memcmp(&coverage_evidence,&normal_coverage,sizeof(coverage_evidence)));
 assert(!inputs.plans&&inputs.global_daily_left==UINT32_MAX&&routes.entries.empty());
 for(auto left:inputs.daily_left)assert(left==0); // filled before scheduler admission
 for(const auto& route:routes.entries.items)assert(route.club.own_special);
 for(const auto& row:coverage_evidence){
  assert(!row.id&&row.blocked==coverage::Reason::kUnknown);
  for(const auto& span:row.seasons)
   assert(span.reason==coverage::Reason::kMissingMetadata&&span.completeness==coverage::Completeness::kUnknown);
 }
 std::puts("PASS actual startup worker defaults: exact default bytes/sizes/limits/club preference/evidence state; no extra owner or payload");
}
'''.replace('ACTUAL_WORKER_DEFAULTS','\n'.join(parts))
with tempfile.TemporaryDirectory(prefix='worker-defaults-',dir='/tmp') as directory:
    folder=Path(directory);unit=folder/'defaults.cpp';unit.write_text(program)
    command=['clang++','-std=c++20','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    for path in (root/'include',*sorted((root/'components').glob('*/include'))):command+=['-I',str(path)]
    subprocess.run([*command,str(unit),'-o',str(folder/'check')],check=True,cwd=root)
    subprocess.run([str(folder/'check')],check=True,cwd=root)
