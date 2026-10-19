#!/usr/bin/env python3
"""Actual bounded activity owner versus an independent union-of-intervals oracle."""
from pathlib import Path
import random, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
NOW=1800000000
rng=random.Random(108)
def window(row):
    source,kickoff,observed,finish,phase,estimated=row
    if phase==4 or (phase==3 and not observed): return None
    basis=kickoff or observed
    if phase==2:
        if not finish:return None
        start=max(0,kickoff-lead*60) if kickoff else finish
        return start,max(start,finish)
    confirmed=phase==1 and not estimated or phase==3
    if not basis or not kickoff and not confirmed:return None
    return (max(0,kickoff-lead*60) if kickoff else basis,
            basis+(hard if confirmed else expected_duration)*60)
def expected(rows,source,target,now,end):
    intervals=sorted(w for i,row in enumerate(rows) if row[0]==source and (not target or i+1==target) and (w:=window(row)))
    merged=[]
    for left,right in intervals:
        if merged and left<=merged[-1][1]:merged[-1][1]=max(merged[-1][1],right)
        else:merged.append([left,right])
    current=[w for w in merged if w[0]<=now<w[1]]
    future=[w[0] for w in merged if w[0]>now]
    ended=[w[1] for w in merged if w[1]<=now]
    overlap=[max(0,min(right,end)-max(left,now)) for left,right in merged]
    return (int(bool(current)), current[0][0] if current else 0,min(future,default=0),max(ended,default=0),sum(overlap),sum(v>0 for v in overlap))
cases=[]
for count in [0,1,2,8,64,128,256,512]:
    for variant in range(12):
        rows=[]
        for i in range(count):
            observed=NOW+rng.randrange(-50000,50000)
            kickoff=0 if variant%3==0 else NOW+rng.randrange(-100000,100000)
            phase=rng.randrange(5)
            if variant==1:phase=0;kickoff=NOW+i*20000 # disjoint
            if variant==2:phase=1;kickoff=NOW+(i%4)*60 # equal starts/overlap chain
            rows.append((rng.randrange(2),kickoff,observed,observed+rng.randrange(1,20000) if phase==2 else 0,phase,rng.randrange(2)))
        source=variant%2;target=rng.randrange(1,count+1) if count and variant>=9 else 0
        now=NOW+rng.randrange(-50000,50000);end=(now//86400+1)*86400
        cases.append((rows,source,target,now,end))
with tempfile.TemporaryDirectory(prefix='football-activity-') as temporary:
    binary=Path(temporary)/'check'
    command=['clang++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    command += ['-I'+str(ROOT/'include')]
    command += ['-I'+str(ROOT/'components'/c/'include') for c in ['football','settings','timekeeping','network','json']]
    command += [str(ROOT/p) for p in ['scripts/tests/fixtures/scheduler_activity_host.cpp','components/football/match_store.cpp','components/football/source_index.cpp','components/football/football_model.cpp','components/settings/settings_model.cpp','components/settings/screen_policy.cpp','components/network/wifi_policy.cpp','components/timekeeping/civil_time.cpp']]
    subprocess.run(command+['-o',str(binary)],check=True)
    text=str(len(cases))+'\n'
    for rows,source,target,now,end in cases:
        text+=' '.join(map(str,[len(rows),source,target,now,end]))+'\n'
        text+=''.join(' '.join(map(str,row))+'\n' for row in rows)
    result=subprocess.run([str(binary)],input=text,text=True,capture_output=True,check=True)
    lines=result.stdout.splitlines()
    policy,*values=lines[0].split();assert policy=='policy'
    lead,expected_duration,hard=map(int,values)
    actual=[tuple(map(int,line.split())) for line in lines[1:]]
    assert len(actual)==len(cases)
    for case,found in zip(cases,actual):
        wanted=expected(*case)
        assert found==wanted,(len(case[0]),case[1:],found,wanted)
print('PASS96 actual activity/oracle cases:0..512 rows, disjoint/overlapping/repeated/unknown/out-of-order windows, final boundaries, source/target isolation; ASan+UBSan')
