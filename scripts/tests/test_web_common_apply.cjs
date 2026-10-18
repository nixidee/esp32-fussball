'use strict';
// Actual client Apply/reconciliation functions; synthetic serialized HTTP.
// This proves response-loss and draft behavior, not browser/device acceptance.
const {readWebSource, productionFunctions} = require('./web_source.cjs');
const vm = require('node:vm'), assert = require('node:assert/strict');
const {script} = readWebSource();
const functions = productionFunctions(script, ['secretMode','payload','modelFromConfig',
    'applicationPayload','applyField','sameObject','mergeValues','acceptApply',
    'saveFailure','readModel','verifySaved','writeModel','mergeBaseline',
    'expandNamesWire','cloneNamesCollection','packAdoptedNames','nameProviders','adoptedPrefix','adoptedShortPrefix','validateAdoptedNames','namesAdoptionChanges','keepNamesDraft','originalClubMatch','adoptOriginalName']);
const copy = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));
let writes, mode, wire;
class ApiError extends Error { constructor(status, data) { super(data?.code || status); this.code = data?.code || status; this.status = typeof status === 'number' ? status : undefined; } }
const context = vm.createContext({console, TextEncoder, TextDecoder, Uint8Array, Set,
    bytes: value=>new TextEncoder().encode(value).length, crypto: require('node:crypto').webcrypto, ApiError, JSON,
    prepareScreenModel: m => m, competitionPayload: copy,
    screenPayload: m => copy(m.screen_policy), t: x => x, tf:(x,v)=>x.replace('%',v),
    Map,renderBar(){}, authenticate: async()=>{},
    settingsRead: async path => { assert.equal(path,'config'); if(mode==='busy-read')throw new ApiError('busy'); return copy(wire); },
    mutate: async(path, body, method) => {
        assert.equal(path,'config'); assert.equal(method,'PUT'); writes++;
        assert.equal(body.expected_revision,wire.revision);
        if(mode==='reject')throw new ApiError('settings_changed');
        if(mode==='uncertain')throw new ApiError('outcome_unknown');
        wire = {...wire, revision:wire.revision+1,apply_id:body.apply_id,desired:copy(body.desired)};
        wire.desired.competitions.forEach((c,i)=>{ if(!c.id){c.id=i+1;c.incarnation=101+i;} });
        if(mode==='lost')throw Error('lost response');
        return copy(wire);
    }});
vm.runInContext(`const S = {schema:{config_version:1,max_apply_body:200000,short_name_max:3,names_name_max:26,names_key_max:63,names_format:'football-team-names',names_source_version:1,names_wire_version:2,names_file_bytes:16384,provider_keys:['openligadb','api-football','espn','football-data']}};
const SECRETS=['admin_password'];const NAME_PROVIDERS=['openligadb','api-football','espn','football-data'];
const PROVIDERS=NAME_PROVIDERS;const clone=x=>x===undefined?undefined:JSON.parse(JSON.stringify(x));
${functions}
globalThis.U={S,modelFromConfig,writeModel,verifySaved,saveFailure,applicationPayload,validateAdoptedNames,namesAdoptionChanges,keepNamesDraft,packAdoptedNames,cloneNamesCollection,originalClubMatch,adoptOriginalName};`,context);
const U=context.U;
function reset(){writes=0;mode='ok';wire={version:1,revision:2,apply_id:'0000000000000001',names_ref:{slot:0,generation:0,length:0,crc:0},desired:{settings:{brightness:90,admin_password_set:false},competitions:[],screens:{frame:false}}};U.S.saved=U.modelFromConfig(copy(wire));U.S.draft=copy(U.S.saved);U.S.sec={};U.S.pendingNames=null;U.S.pendingApply=null;U.S.saveProgress=null;}
let checks=0;async function test(title,run){reset();await run();checks++;console.log('PASS '+title);}
(async()=>{
 await test('one coherent write carries all sections and names',async()=>{U.S.draft.brightness=70;U.S.draft.competitions=[{id:0,incarnation:0,club:{team:40,group:1},display:{}}];U.S.pendingNames={format:'football-team-names',version:1,clubs:[],exceptions:[]};await U.writeModel();assert.equal(writes,1);assert.equal(U.S.saved.brightness,70);assert.equal(U.S.saved.competitions[0].id,1);assert.equal(U.S.pendingNames,null);await U.verifySaved();assert.equal(writes,1);assert.equal(U.S.pendingApply,null);});
 await test('loss of response is resolved by Apply ID using GET only',async()=>{mode='lost';U.S.draft.brightness=71;await assert.rejects(U.writeModel());assert(U.S.pendingApply&&!U.S.pendingApply.accepted);mode='ok';await U.verifySaved();assert.equal(writes,1);assert.equal(U.S.saved.brightness,71);assert.equal(U.S.pendingApply,null);});
 await test('uncertain NVS never permits another mutation or assumes old means absent',async()=>{mode='uncertain';U.S.draft.brightness=72;await assert.rejects(U.writeModel());await assert.rejects(U.writeModel());mode='ok';await assert.rejects(U.verifySaved());assert.equal(writes,1);assert.equal(U.S.draft.brightness,72);assert(U.S.pendingApply);});
 await test('explicit stale rejection preserves whole draft and clears pending token',async()=>{mode='reject';U.S.draft.brightness=73;U.S.pendingNames={clubs:[]};await assert.rejects(U.writeModel());assert.equal(U.S.pendingApply,null);assert.equal(U.S.saved.brightness,90);assert.equal(U.S.draft.brightness,73);assert(U.S.pendingNames);});
 await test('acknowledged write with busy read is never repeated',async()=>{U.S.draft.brightness=74;await U.writeModel();mode='busy-read';try{await U.verifySaved();assert.fail();}catch(error){U.saveFailure(error);}assert.equal(U.S.saveNotice.severity,'warning');await assert.rejects(U.writeModel());assert.equal(writes,1);mode='ok';await U.verifySaved();assert.equal(writes,1);});
 await test('edits after unknown reply survive outcome resolution',async()=>{mode='lost';U.S.draft.brightness=75;await assert.rejects(U.writeModel());U.S.draft.brightness=76;mode='ok';await U.verifySaved();assert.equal(U.S.saved.brightness,75);assert.equal(U.S.draft.brightness,76);assert.equal(writes,1);});
 await test('newer other Apply resolves an uncertain request as conflict without dropping draft',async()=>{mode='lost';U.S.draft.brightness=75;await assert.rejects(U.writeModel());U.S.draft.brightness=76;wire.revision++;wire.apply_id='00000000000000ff';wire.desired.settings.brightness=85;mode='ok';await assert.rejects(U.verifySaved(),error=>error.status===409&&error.code==='settings_changed');assert.equal(U.S.pendingApply,null);assert.equal(U.S.saved.brightness,85);assert.equal(U.S.draft.brightness,76);assert.equal(writes,1);});
 await test('redacted flags and revision never become desired settings',async()=>{U.S.sec.admin_password={mode:'change',value:'changed'};await U.writeModel();assert.equal(wire.desired.settings.admin_password,'changed');assert(!('admin_password_set' in wire.desired.settings));assert(!('_revision' in wire.desired.settings));assert(!('screen_policy' in wire.desired.settings));});
 await test('names validate full Unicode and duplicate identities before clipping',()=>{const names={format:'football-team-names',version:1,clubs:[{key:'club',de:'Lang😀'.repeat(12),short:'Bayern',ids:{espn:[40]},aliases:['full original']}],exceptions:[{provider:2,id:40,name:''}]};const valid=U.validateAdoptedNames(names);assert.equal(valid.clubs[0].de,names.clubs[0].de);assert.equal(U.namesAdoptionChanges(valid).length,2);assert.throws(()=>U.validateAdoptedNames({...names,clubs:[...names.clubs,{key:'other',ids:{espn:[40]}}]}));assert.throws(()=>U.validateAdoptedNames({...names,clubs:[{key:'club',de:'ok\ud800'}]}));});
 await test('successive editor events preserve all changed letters and other metadata',()=>{const n={collection:{format:'football-team-names',version:1,clubs:[{key:'club',de:'A',league:'league',ids:{espn:[40]},aliases:['original']}],exceptions:[]}};n.collection.clubs[0].de='Ba';U.keepNamesDraft(n);n.collection.clubs[0].de='Bayern';U.keepNamesDraft(n);assert.equal(U.S.pendingNames.clubs[0].de,'Bayern');assert.deepEqual(copy(U.S.pendingNames.clubs[0].aliases),['original']);});
 await test('shared long metadata is sent once and real firmware exports roundtrip',()=>{const text='L'.repeat(8136);const names={format:'football-team-names',version:1,clubs:[{key:'club',league:text,ids:{},aliases:Array(2000).fill(text)}],exceptions:[]};const packed=U.packAdoptedNames(names);assert(JSON.stringify(packed).length<24000);const roundtrip=U.validateAdoptedNames(packed);assert.equal(roundtrip.clubs[0].aliases.length,2000);assert.equal(roundtrip.clubs[0].aliases[1999],text);for(const filename of process.argv.slice(2)){const exported=JSON.parse(require('node:fs').readFileSync(filename,'utf8'));const expanded=U.validateAdoptedNames(exported);assert.deepEqual(copy(U.validateAdoptedNames(U.packAdoptedNames(expanded))),copy(expanded));}});
 await test('original name matching uses full aliases and exact IDs before clipping',()=>{const prefix='Same long name '.repeat(4);const collection={clubs:[{key:'a',de:prefix+'A',ids:{espn:[40]},aliases:[prefix+'alias']},{key:'b',de:prefix+'B',ids:{},aliases:[]}]};assert.equal(U.originalClubMatch(collection,2,{id:40,name:prefix+'B'}),'a');assert.equal(U.originalClubMatch(collection,2,{id:41,name:prefix+'B'}),'b');assert.equal(U.originalClubMatch(collection,2,{id:41,name:prefix}),'');collection.clubs[1].aliases=[prefix+'alias'];assert.equal(U.originalClubMatch(collection,2,{id:41,name:prefix+'alias'}),'');});
 await test('explicit original adoption changes only selected collection draft and preserves groups',()=>{const n={collection:{format:'football-team-names',version:1,clubs:[{key:'club',de:'Prior',short:'XYZ',ids:{espn:[40],openligadb:[9]},aliases:['alias']}],exceptions:[]},originals:{source:{provider:2},items:[{id:40,name:'Full provider name '+ '😀'.repeat(20)}]}};U.S.draft.competitions=[{id:1,club:{team:40,group:2}}];const before=copy(U.S.draft.competitions);U.adoptOriginalName(n,0,'club','en');assert.equal(n.collection.clubs[0].de,'Prior');assert.equal(n.collection.clubs[0].short,'XYZ');assert.equal(n.collection.clubs[0].en,n.originals.items[0].name);assert.deepEqual(copy(n.collection.clubs[0].ids.openligadb),[9]);assert.deepEqual(copy(U.S.draft.competitions),before);assert.equal(writes,0);assert.throws(()=>U.adoptOriginalName(n,0,'__new__','de'));});
 console.log(checks+' common Apply/client draft behavior cases passed');
})().catch(error=>{console.error(error);process.exitCode=1});
