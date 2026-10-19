'use strict';
// Production draft/group/rule functions. No browser or device claim.
const {readWebSource, productionFunctions} = require('./web_source.cjs');
const vm = require('node:vm'), assert = require('node:assert/strict');
const {script} = readWebSource();
const schema = require('./web_config_harness.cjs').fixture().schema;
const source = productionFunctions(script, ['blankCompetitionDisplay','prepareScreenModel',
  'screenKey','newScreenKey','screenPayload','competitionPayload','spEffective',
  'validateCompetitionDisplay','applyClubGroup','competitionOrder','groupName','validClubId',
  'validId','validate','decimalIdInput','editDraft','draftAction','dispatchDraft']);
const context = vm.createContext({TextEncoder, Uint8Array, console});
vm.runInContext(`const clone=x=>JSON.parse(JSON.stringify(x));
const S={draft:null,schema:{providers:[0,1,2,3],data_types:[],defaults:{},inputs:0,
  text:{competitions:{competition:32,season:16,competition_name:32,club_name:32}},
  limits:{competitions:8},bounds:{daily_requests:[0,65535],window_requests:[0,65535],window_s:[0,65535]},
  time_zones:['UTC'],screen_policy:{bounds:{visit_s:[15,43200]}}},sec:{}};
const SP_PHASES=['own','other','matchday','idle','break'],SP_SPORTS=['single','conference','table','matchday'];
const SP_TYPES=['single','conference','table','crest','matchday','images'];
const t=x=>x, bytes=x=>new TextEncoder().encode(x).length, SECRETS=[],LIMIT_KEYS=['daily_requests','window_requests','window_s'],LIMITS=['providers'],ROLES=[];
const parts=p=>p.replace(/\\[(\\d+)\\]/g,'.$1').split('.'),get=(o,p)=>parts(p).reduce((v,k)=>v?.[k],o);
let screenDraftSerial=0;
${source}
globalThis.U={S,blankCompetitionDisplay,competitionPayload,screenPayload,spEffective,
validateCompetitionDisplay,applyClubGroup,competitionOrder,groupName,validClubId,validate,decimalIdInput};`,context);
const U=context.U, copy=x=>JSON.parse(JSON.stringify(x));
// Obtain domain limits from the current C++ schema, not copied stale maxima.
U.S.schema.provider_setup=schema.provider_setup;
U.S.schema.team_id_max=schema.team_id_max;
U.S.schema.competition_id_max=schema.competition_id_max;
function row(id,name='Same name'){return {id,incarnation:id,active:true,primary:{provider:0,competition:'league',season:''},competition_name:'League',club:{team:id,name,own_special:true,group:0},display:U.blankCompetitionDisplay()};}
function draft(rows){U.S.draft={language:0,time_zone:'UTC',hostname:'football',ntp_server:'pool.ntp.org',global_limit:{daily_requests:0,window_requests:0,window_s:0},providers:[0,1,2,3].map(()=>({daily_requests:0,window_requests:0,window_s:0,intervals:{}})),competitions:rows};}
let count=0;function test(title,run){draft([row(1),row(2),row(3),row(4)]);run();count++;console.log('PASS '+title);}
test('only explicit club groups alter priority, independent of labels',()=>{assert.deepEqual(copy(U.competitionOrder()),[0,1,2,3]);const group=U.applyClubGroup(0,[0,2]);assert.equal(group,1);assert.deepEqual(copy(U.competitionOrder()),[0,2,1,3]);U.S.draft.competitions[0].club.name='Renamed';assert.equal(U.groupName(1),'Renamed');assert.deepEqual(copy(U.competitionOrder()),[0,2,1,3]);assert.equal(U.S.draft.competitions[1].club.group,0);});
test('editing a group preserves unrelated memberships and sources',()=>{const before=copy(U.S.draft.competitions.map(c=>c.primary));U.applyClubGroup(0,[0,2]);U.applyClubGroup(0,[1,3]);U.applyClubGroup(1,[0,3]);assert.deepEqual(copy(U.S.draft.competitions.map(c=>c.club.group)),[1,2,0,1]);assert.deepEqual(copy(U.S.draft.competitions.map(c=>c.primary)),before);});
test('invalid group changes reject before modifying the draft',()=>{const before=copy(U.S.draft);assert.throws(()=>U.applyClubGroup(0,[0]));assert.throws(()=>U.applyClubGroup(0,[0,0]));U.S.draft.competitions[1].club.team=0;const noClub=copy(U.S.draft);assert.throws(()=>U.applyClubGroup(0,[0,1]));assert.deepEqual(copy(U.S.draft),noClub);assert.equal(before.competitions[0].club.group,0);});
test('nullable fields inherit independently and dormant values remain in payload',()=>{const c=U.S.draft.competitions[0];U.S.draft.screen_policy={phases:{idle:{table:[true,true,90],per_competition:[]}}};c.display.idle.table=[false,null,27];assert.deepEqual(copy(U.spEffective(c.display,'idle','table')),[false,true,27]);assert.equal(U.validateCompetitionDisplay(c.display,0).length,0);const wire=copy(U.competitionPayload(U.S.draft.competitions));assert.deepEqual(wire[0].display.idle.table,[false,null,27]);U.S.draft.screen_policy.phases.idle.per_competition=['table'];assert.deepEqual(copy(U.competitionPayload(U.S.draft.competitions))[0].display,wire[0].display);});
test('club numeric boundaries and display errors point to the edited field',()=>{for(const id of [1,4294967295])assert(U.validClubId(id));assert.equal(U.decimalIdInput('00016',schema.team_id_max),16);assert(!U.validClubId('00016'));for(const id of [0,-1,4294967296,'abc','1.0',' 1'])assert(!U.validClubId(id));U.S.draft.competitions[0].club.team=4294967295;assert.equal(U.validate(U.S.draft).filter(x=>x.path.startsWith('competitions')).length,0);U.S.draft.competitions[0].club.team='16';assert(U.validate(U.S.draft).some(x=>x.path==='competitions[0].club.team'));U.S.draft.competitions[0].display.idle.table[2]=43201;assert.equal(U.validateCompetitionDisplay(U.S.draft.competitions[0].display,0)[0].path,'competitions[0].display.idle.table[2]');});
console.log(count+' display/group client cases passed');
