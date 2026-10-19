'use strict';
// Owner-visible draft transitions use the real handlers and current firmware
// schema. Freeze input snapshots to prove edits cannot change another draft.
const assert = require('node:assert/strict'), vm = require('node:vm');
const {harness} = require('./web_config_harness.cjs');
const {readWebSource,autonomousBoundary} = require('./web_source.cjs');
const h = harness(), {U, events} = h;
// Inspect a separate, uninstrumented production scope. No private mutable
// feature state or test hook may escape into the actual bundle.
const source=readWebSource().script, isolated={...h.context};
delete isolated.__featureTests; delete isolated.UI;
isolated.document={...h.context.document,addEventListener(){}};
const clean=vm.createContext(isolated);
vm.runInContext(source.slice(0,autonomousBoundary(source)),clean);
assert.equal(vm.runInContext('typeof wizard',clean),'undefined');
assert.equal(vm.runInContext('typeof names',clean),'undefined');
assert.equal(vm.runInContext('typeof ED',clean),'undefined');
assert.equal(vm.runInContext('typeof __featureTests',clean),'undefined');
assert.equal(vm.runInContext('typeof openWizard + typeof openNames + typeof editorBusy',clean),'functionfunctionfunction');
const {draftAction, describeField, decodeInput, F} = vm.runInContext('({draftAction,describeField,decodeInput,F})', h.context);
const copy = value => JSON.parse(JSON.stringify(value));
function freeze(value) { if (value && typeof value === 'object') {Object.freeze(value); Object.values(value).forEach(freeze);} return value; }
const before = copy(U.S.draft), frozen = freeze(copy(before));
let next = draftAction(frozen, {type:'field', path:'screen_policy.phases.own.images[0]', value:true});
assert.equal(next.screen_policy.phases.own.images[0],true);
assert.equal(next.screen_policy.phases.own.images[1],true);
assert.deepEqual(copy(frozen),before);
assert.equal(next.competitions,frozen.competitions);
for (const value of [null, false, true]) {
 next=draftAction(next,{type:'field',path:'competitions[0].display.own.single[0]',value});
 assert.equal(next.competitions[0].display.own.single[0],value);
 assert.deepEqual(copy(frozen),before);
}
const ids=[7,3,7];
next={...next,screen_policy:{...next.screen_policy,slides:ids}};
next=draftAction(next,{type:'slide-move',index:2,direction:-1});
assert.deepEqual(copy(next.screen_policy.slides),[7,7,3]);
next=draftAction(next,{type:'slide-delete',index:1});
assert.deepEqual(copy(next.screen_policy.slides),[7,3]);
assert.deepEqual(ids,[7,3,7]);
assert.throws(()=>draftAction(next,{type:'slide-add',id:9,maximum:2}));
const rawCases=[['num','',false,''],['num','42',false,42],['policybool','',false,null],['policybool','false',false,false],['policybool','true',false,true],['time','23:59',false,1439],['bool','',true,true],['colour','#123abc',false,0x123abc],['club-id','00040',false,40],['club-id','1e2',false,0]];
for (const [kind,raw,checked,expected] of rawCases) assert.equal(decodeInput(kind,raw,checked,U.S.schema.team_id_max),expected);
for (const [path,type,options,kind,bound,unit] of [
 ['brightness','number',null,'num',U.S.schema.bounds.brightness,'%'],
 ['scroll_repeat_ms','number',null,'num',U.S.schema.bounds.scroll_repeat_ms,'ms'],
 ['language','select',[[0,'DE'],[1,'EN']],'num',null,''],
 ['time_zone','select',[['UTC','UTC']],'str',null,''],
 ['competitions[0].display.own.single[0]','select',[[null,'shared'],[true,'yes'],[false,'no']],'policybool',null,'']
]) {
 const descriptor=describeField(U.S.draft,U.S.schema,path,type,'help',options,false,null);
 assert.equal(descriptor.kind,kind); assert.equal(descriptor.unit,unit);
 assert.deepEqual(copy(descriptor.bound),bound===undefined?null:bound);
 const html=F(descriptor);assert(html.includes('data-kind="'+kind+'"'));
}
// Equivalent decoded objects may arrive in a different JSON key order. The
// shared equality owner must retain array order and avoid false dirty rows.
const original=copy(U.S.draft), reordered=copy(U.S.draft);
reordered.competitions[0].primary=Object.fromEntries(Object.entries(reordered.competitions[0].primary).reverse());
U.S.draft=reordered;
assert(U.sameObject(original,reordered));assert.equal(U.changes().length,0);
assert(U.compSaved(reordered.competitions[0]));
assert(!U.sameObject([1,2],[2,1]));assert(!U.sameObject({a:null},{}));
assert(!U.sameObject({a:undefined},{b:undefined}));assert(!U.sameObject(false,null));
for(const language of [0,1]) {
 U.S.draft.language=language; U.S.status.settings_source='damaged';
 U.paintStatus();
 const text=h.element('warnings').innerHTML;
 assert(text.includes(language?'Stored settings are damaged':'Gespeicherte Einstellungen sind beschädigt'));
 assert.equal((text.match(language?/Stored settings are damaged/g:/Gespeicherte Einstellungen sind beschädigt/g)||[]).length,1);
 U.S.status.settings_source='outcome_unknown';U.paintStatus();
 assert(h.element('warnings').innerHTML.includes(language?'Save outcome is unknown':'Speicherergebnis unklar'));
}
U.S.draft=original;U.S.status.settings_source='stored';
// Render actual prepared views while application models are unavailable.
// Frozen input also proves render cannot edit the draft/dialog state.
const currentSchema=U.S.schema, currentDraft=U.S.draft;
const entry=currentDraft.competitions[0];
const wizardView=freeze({step:1,noClub:false,list:[],search:['',''],listSeason:'',country:'',busy:false,more:false,wait:false,waitText:'',error:'',entry:copy(entry)});
const collection=freeze({clubs:[{key:'view',de:'View club',ids:{},aliases:[]}],exceptions:[]});
U.S.schema=null;U.S.draft=null;
const beforeCalls=h.calls.length;
assert(U.wizardMarkup(wizardView,currentSchema,[{name:'OpenLigaDB',enabled:true,description:'public'}],'').includes('id="wizardId"'));
const namesView=U.namesMarkup(freeze({selected:'view',aliasIndex:0,competition:0,hasConfig:false,busy:false,error:''}),collection,collection.clubs[0],[],currentSchema,false);
assert(namesView.body.includes('View club'));assert(namesView.body.includes('nameImport'));
assert(U.imageEditorMarkup(freeze({width:240,height:240,round:true,label:'View image',replacing:true,hasImage:true,busy:false,zoom:100,x:12.75,y:-8.5,opacity:100,base:'#101820',sample:true})).includes('value="12.75"'));
assert.equal(h.calls.length,beforeCalls);
U.S.schema=currentSchema;U.S.draft=currentDraft;
const saved=copy(U.S.saved);
events.input({target:{dataset:{path:'hostname',kind:'str'},value:'new-draft',checked:false}});
assert.equal(U.S.draft.hostname,'new-draft');assert.deepEqual(copy(U.S.saved),saved);
assert(U.changes().some(row=>row.path==='hostname'));
console.log('actual immutable draft transitions, inheritance/auto invariant, ordered duplicate slides, bounds/types/units, raw decoding and unsaved baseline preservation PASS');
