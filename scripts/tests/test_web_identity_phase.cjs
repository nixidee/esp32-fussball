'use strict';
// Reproduced manual input through the real wizard and actual canonical phase
// keys. Controlled DOM is not a real-browser/device claim.
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const fs = require('node:fs'), os = require('node:os'), path = require('node:path');
const {harness} = require('./web_config_harness.cjs');
const h = harness(), {U} = h;
// The existing harness does not model dialog event registration.
for (const id of ['dialog [data-act=wizardUngroup]']) h.element(id).addEventListener = () => {};
h.context.document.getElementById = id => id.startsWith('wizard') &&
    !h.element('dialog').innerHTML.includes('id="'+id+'"') ? null : h.element(id);
U.openWizard();
// Invoke actual provider/competition/club wizard navigation. The controlled
// elements only provide event receivers; no second input implementation.
U.wizardNext();
h.element('wizardId').oninput({target:{value:'bl1'}});
U.wizardNext();
for (const raw of ['40','00016','4294967295']) {
    h.element('wizardId').oninput({target:{value:raw}});
    assert.equal(U.wizardWhy(), '', raw);
}
for (const raw of ['', '0', '-1', '+1', '4294967296', '1.0', '1e2', ' 1', '1 ', 'abc']) {
    h.element('wizardId').oninput({target:{value:raw}});
    assert.notEqual(U.wizardWhy(), '', raw);
}
h.element('wizardId').oninput({target:{value:'00040'}});
U.wizardNext();
U.wizardNext();
assert.equal(U.S.draft.competitions.at(-1).club.team,40);
assert.equal(U.validClubId('40'),false);
assert.equal(U.validClubId(40),true);
console.log('PASS actual wizard raw decimal boundary and typed draft acceptance');
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'football-phase-'));
try {
    const root=path.resolve(__dirname,'../..');
    fs.writeFileSync(path.join(dir,'phase.cpp'),`#include <cstdio>\n#include "football_model.h"\nint main(){for(unsigned p=0;p<=static_cast<unsigned>(football::Phase::kSuspended);++p) std::puts(football::phaseName(static_cast<football::Phase>(p)));}`);
    execFileSync('clang++',['-std=c++20','-ffunction-sections','-fdata-sections','-I'+root+'/components/football/include','-I'+root+'/components/settings/include','-I'+root+'/components/timekeeping/include','-I'+root+'/include',path.join(dir,'phase.cpp'),root+'/components/football/football_model.cpp','-Wl,-dead_strip','-o',path.join(dir,'phase')]);
    const keys=execFileSync(path.join(dir,'phase'),{encoding:'utf8'}).trim().split('\n');
    const {script}=require('./web_source.cjs').readWebSource();
    const vm=require('node:vm');
    for(const language of [0,1]) {
        U.S.draft.language=language;
        for(const phase of keys) {
            U.S.status.own_match={present:true,phase,home:'Home',away:'Away',home_goals:1,away_goals:0};
            assert(vm.runInContext(`Object.hasOwn(PHASE,${JSON.stringify(phase)})`,h.context),phase);
            const markup=U.ownMarkup();
            assert(markup.includes('1 : 0'),phase);
            if(phase!=='unknown') assert(!markup.includes(language?'Unknown':'Unbekannt'),phase);
        }
        U.S.status.own_match.phase='future_unknown_phase';
        assert(U.ownMarkup().includes(language?'Unknown':'Unbekannt'));
    }
    console.log('PASS every actual C++ phase has DE/EN presentation and retains scores/unknown fallback');
} finally { fs.rmSync(dir,{recursive:true,force:true}); }
