'use strict';
const {readWebSource, autonomousBoundary, productionFunctions}=require('./web_source.cjs');

// Production dialog/name functions with controlled DOM and pending HTTP.
// Native cancel follows the browser's default close unless prevented. This is
// state/race evidence, not a browser-layout or device-acceptance claim.
// Run: node scripts/tests/test_web_names_close.cjs [optional HTML snapshot]
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const {html,script}=readWebSource(process.argv[2]||path.resolve(__dirname,'../../web/index.html'));
const functionNames=['validClubId','button','confirmAction','dialog','openNames','loadNames','drawNames','nameProviders','adoptedPrefix','adoptedShortPrefix','shortAdoptionPreview','expandNamesWire','validateAdoptedNames','keepNamesDraft','namesAdoptionChanges','editorNameTeams'];
const firmwareSchema=require('./web_config_harness.cjs').fixture().schema;
const source=productionFunctions(script,functionNames);
const deferred=()=>{let resolve,reject;const promise=new Promise((yes,no)=>{resolve=yes;reject=no});return{promise,resolve,reject};};
const rows=name=>({items:[{provider:0,id:42,name,short_name:'ABC'}]});
const adopted=()=>({names_ref:{},adopted_names:{format:'football-team-names',version:1,clubs:[{key:'club',de:'Adopted',short:'',ids:{openligadb:[42]},aliases:[]}],exceptions:[]}});

function setup(){
 const pending=[],nameReads=[],nodes=new Map();let markup='';
 const node=key=>{if(!nodes.has(key))nodes.set(key,{value:'',dataset:{},checked:false,disabled:false,events:{},addEventListener(type,fn){this.events[type]=fn;}});return nodes.get(key);};
 const dialog={open:false,className:'',oncancel:null,
  get innerHTML(){return markup;},set innerHTML(value){markup=value;nodes.clear();
   for(const match of value.matchAll(/<input[^>]*data-name-(inherit|field)="([^"]+)"[^>]*>/g)){
    const key=`[data-name-${match[1]}="${match[2]}"]`,el=node(key);el.dataset[match[1]==='inherit'?'nameInherit':'nameField']=match[2];el.checked=/\bchecked\b/.test(match[0]);el.disabled=/\bdisabled\b/.test(match[0]);el.value=match[0].match(/value="([^"]*)"/)?.[1]||'';
   }},
  close(){this.open=false;},showModal(){this.open=true;},
  querySelector:node,querySelectorAll(selector){return [...nodes.values()].filter(el=>selector==='[data-name-inherit]'?'nameInherit'in el.dataset:selector==='[data-name-field]'?'nameField'in el.dataset:false);}
 };
 const context={S:{schema:{...firmwareSchema,short_name_max:3,names_name_max:26,names_key_max:63,names_format:'football-team-names',names_source_version:1,names_wire_version:2,provider_keys:['openligadb','api-football','espn','football-data']},saved:{_namesRef:{}},pendingNames:null},PROVIDERS:['OL','AF','ESPN','FD'],TextEncoder,TextDecoder,structuredClone,bytes:x=>new TextEncoder().encode(x).length,
  t:x=>x,tf:(x,v)=>x.replace('%',v),ht:x=>x,esc:x=>String(x??''),button:()=>'<button data-act="close">Close</button>',compName:c=>c.competition_name,clone:structuredClone,sameObject:(a,b)=>JSON.stringify(a)===JSON.stringify(b),screenConflict:()=>Error('changed'),renderBar(){},
  $:id=>id==='dialog'?dialog:node(id),
  settingsRead(url){const request={url,...deferred()};if(url==='team-names')nameReads.push(request);else{request.names=nameReads.shift();pending.push(request);}return request.promise;}
 };
 vm.createContext(context);vm.runInContext('let names=null;'+source+';globalThis.UI={openNames,pending:()=>S.pendingNames};',context,{filename:'production name/dialog functions'});
 return{dialog,pending,node,draft:()=>context.UI.pending(),
  open(id=1){return context.UI.openNames({id,competition_name:'Competition '+id});},
  failTeams(){pending[0].names.resolve(adopted());pending[0].reject(Error('runtime teams unavailable'));},
  failNames(){pending[0].names.reject(Error('names file unavailable'));pending[0].resolve(rows('Provider'));},
  importNames(){return node('nameImport').onchange({target:{files:[{size:100,text:async()=>JSON.stringify(adopted().adopted_names)}]}});},
  confirmImport(){return dialog.querySelector('[data-act=confirm]').onclick({target:{disabled:false}});},
  headerClose(){dialog.querySelector('[data-close]').onclick();},
  explicitClose(){dialog.querySelector('[data-act=close]').onclick();},
  cancel(){let prevented=false;dialog.oncancel?.({preventDefault(){prevented=true;}});if(!prevented)dialog.close();},
  reply(index,name='Current club'){pending[index].names.resolve(adopted());pending[index].resolve(rows(name));}
 };
}

const watchdog=setTimeout(()=>{throw Error('Pending name-read test never completed');},5000);
let passed=0,failed=0;
async function test(name,run){try{await run(setup());passed++;console.log('PASS '+name);}catch(error){failed++;console.error('FAIL '+name+'\n'+error.stack);}}
(async()=>{
 await test('header X closes a pending names dialog permanently when its GET succeeds',async t=>{const waiting=t.open();assert.equal(t.dialog.open,true);t.headerClose();assert.equal(t.dialog.open,false);t.reply(0);await waiting;assert.equal(t.dialog.open,false,'A late GET must not reopen the owner-closed dialog');});
 await test('native Escape cancel closes a pending names dialog permanently when its GET succeeds',async t=>{const waiting=t.open();t.cancel();assert.equal(t.dialog.open,false);t.reply(0);await waiting;assert.equal(t.dialog.open,false,'A late GET must not reopen the Escape-closed dialog');});
 await test('explicit bottom Close already drops the late successful GET',async t=>{const waiting=t.open();t.explicitClose();assert.equal(t.dialog.open,false);t.reply(0);await waiting;assert.equal(t.dialog.open,false);});
 await test('a failed GET after header X also leaves the dialog closed',async t=>{const waiting=t.open();t.headerClose();t.pending[0].reject(Error('request failed'));t.pending[0].names.resolve(adopted());await waiting;assert.equal(t.dialog.open,false,'A late failure must not reopen the dialog to show its error');});
 await test('an old GET cannot overwrite a newer names dialog after its own GET completed',async t=>{const first=t.open(1),second=t.open(2);assert.equal(t.pending[0].url,'teams?competition=1');assert.equal(t.pending[1].url,'teams?competition=2');t.reply(1,'New competition club');await second;const current=t.dialog.innerHTML;t.reply(0,'Obsolete competition club');await first;assert.equal(t.dialog.open,true);assert.equal(t.dialog.innerHTML,current);assert(current.includes('Competition 2'));assert(current.includes('New competition club'));assert(!current.includes('Obsolete competition club'));});
 await test('close and immediate reopen preserve the new pending dialog when the old GET arrives first',async t=>{const first=t.open(1);t.headerClose();const second=t.open(2),current=t.dialog.innerHTML;t.reply(0,'Obsolete competition club');await first;assert.equal(t.dialog.open,true);assert.equal(t.dialog.innerHTML,current);assert(current.includes('Competition 2'));assert(!current.includes('Obsolete competition club'));t.reply(1,'New competition club');await second;assert(t.dialog.innerHTML.includes('New competition club'));});
 await test('Escape followed by reopening the same competition does not reuse the cancelled visit',async t=>{const first=t.open(1);t.cancel();const second=t.open(1);t.reply(1,'Reopened current club');await second;const current=t.dialog.innerHTML;t.reply(0,'Cancelled old club');await first;assert.equal(t.dialog.open,true);assert.equal(t.dialog.innerHTML,current);assert(current.includes('Reopened current club'));assert(!current.includes('Cancelled old club'));});
 await test('runtime team read failure keeps the adopted collection editable without changing its draft',async t=>{const waiting=t.open();t.failTeams();await waiting;assert(t.dialog.open);assert(t.dialog.innerHTML.includes('Adopted'),'Adopted fields must stay available independently of volatile teams');assert(t.dialog.innerHTML.includes('runtime teams unavailable'));assert.equal(t.draft(),null);});
 await test('unreadable names retain explicit collection import while original adoption and export stay disabled',async t=>{const waiting=t.open();t.failNames();await waiting;assert(t.dialog.open);assert(t.dialog.innerHTML.includes('names file unavailable'));assert(t.dialog.innerHTML.includes('nameImport'));assert(/<button[^>]+data-act="nameOriginalLoad"[^>]+disabled/.test(t.dialog.innerHTML));assert(/<button[^>]+data-act="nameExport"[^>]+disabled/.test(t.dialog.innerHTML));assert.equal(t.draft(),null);await t.importNames();assert.equal(t.draft(),null,'Import still needs explicit draft replacement');await t.confirmImport();assert.equal(t.draft().clubs[0].de,'Adopted');assert.equal(t.dialog.open,false);});
 await test('inheritance and explicit empty remain separate draft states without a write',async t=>{const waiting=t.open();t.reply(0);await waiting;assert(t.node('[data-name-inherit="en"]').checked);assert(t.node('[data-name-field="en"]').disabled);assert.equal(t.node('[data-name-inherit="short"]').checked,false);assert.equal(t.node('[data-name-field="short"]').value,'');const de=t.node('[data-name-inherit="de"]');de.checked=true;de.onchange();assert(!('de'in t.draft().clubs[0]));const en=t.node('[data-name-inherit="en"]');en.checked=false;en.onchange();assert.equal(t.draft().clubs[0].en,'');assert.equal(t.node('[data-name-field="en"]').disabled,false);});
 console.log(JSON.stringify({passed,failed,source_sha256:crypto.createHash('sha256').update(html).digest('hex'),scope:'actual dialog/name functions; controlled DOM/HTTP/default cancel close; no browser/device acceptance'}));
 clearTimeout(watchdog);
 if(failed)process.exitCode=1;
})().catch(error=>{clearTimeout(watchdog);console.error(error);process.exitCode=1;});
