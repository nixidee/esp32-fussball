'use strict';
const {readWebSource, autonomousBoundary, productionFunctions}=require('./web_source.cjs');
// Actual production functions with controlled DOM/network boundaries. Browser
// layout and the C++ reset execution are verified separately.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'../..'),html=readWebSource(path.join(root,'web/index.html')).html,script=html.match(/<script>([\s\S]*?)<\/script>/)[1],boundary=autonomousBoundary(script);
assert(boundary>0);
const nodes=new Map(),writes=[],messages=[];
function node(id){
 if(nodes.has(id))return nodes.get(id);
 let markup='';
 const item={id,value:'',textContent:'',dataset:{},checked:false,disabled:false,hidden:false,open:false,style:{},classList:{toggle(){}},setAttribute(){},focus(){},close(){this.open=false;},showModal(){this.open=true;},querySelector(selector){return node(id+' '+selector);},querySelectorAll(selector){if(selector==='[data-team]')return [...nodes.values()].filter(x=>x.dataset.team!==undefined);return[];},firstElementChild:{style:{}}};
 Object.defineProperty(item,'innerHTML',{get(){return markup;},set(value){markup=value;if(id==='dialog'){for(const key of [...nodes.keys()])if(key!=='dialog')nodes.delete(key);for(const match of value.matchAll(/<[^>]+\bid="([^"]+)"[^>]*>/g)){const element=node(match[1]),tag=match[0],data=tag.match(/data-team="([^"]+)"/),input=tag.match(/value="([^"]*)"/);element.checked=/\bchecked\b/.test(tag);element.disabled=/\bdisabled\b/.test(tag);element.value=input?input[1]:'';if(data)element.dataset.team=data[1];}}}});
 nodes.set(id,item);return item;
}
const context={console,TextEncoder,TextDecoder,Blob,AbortController,Uint8Array,DataView,ArrayBuffer,Map,Set,Date,Promise,Math,JSON,Number,String,Object,Error,Array,RegExp,structuredClone,scrollY:0,scrollTo(){},setTimeout:()=>1,clearTimeout(){},document:{getElementById:node,querySelectorAll(){return[];},querySelector(){return null;},addEventListener(){},documentElement:{dataset:{}},createElement(){return{click(){}};}},URL:{createObjectURL(){return'blob:controlled';},revokeObjectURL(){}},localStorage:{getItem(){return'auto';},setItem(){}},location:{host:'controlled.local',hostname:'controlled.local',protocol:'http:'},window:{addEventListener(){}},fetch:async()=>{throw Error('unexpected network');},confirm:()=>true};
vm.createContext(context);
vm.runInContext(script.slice(0,boundary)+'\nglobalThis.UI={S,W,ACT,factoryDialog,ApiError};render=()=>{};toast=message=>globalThis.messages.push(message);waitForDevice=async()=>{};closeDebug=()=>{};loadNames=async()=>{};',context,{filename:'production Web script'});
context.messages=messages;
const U=context.UI;
let failure=null;
context.mutate=async(endpoint,body)=>{writes.push({endpoint,body:structuredClone(body)});if(failure)throw failure;return{ok:true};};
function reset(){nodes.clear();writes.length=messages.length=0;failure=null;U.S.saved={hostname:'test-device'};U.S.draft={language:0};U.S.status={ap_ip:'192.0.2.1'};U.S.schema={short_name_max:3,short_names_max:48,providers:['openligadb','api-football','espn','football-data']};}
let passed=0;
async function test(name,run){reset();await run();passed++;console.log('PASS '+name);}
(async()=>{
 await test('repair opens existing explicit reset dialog without a network write',()=>{U.ACT.repair();assert(node('dialog').open);assert(node('resetStorage').checked);assert(node('resetImages').disabled);assert.equal(writes.length,0);assert(node('dialog').innerHTML.includes('WLAN'));assert(node('dialog').innerHTML.includes('Abrufzähler'));assert(node('dialog').innerHTML.includes('gespeicherte Vereinsnamen'));assert(node('dialog').innerHTML.includes('unveränderliche Namensvorlage'));});
 await test('hostname confirmation gates the destructive reset request',async()=>{U.ACT.repair();const button=node('dialog [data-act=resetConfirm]');assert(node('dialog').innerHTML.includes('data-act="resetConfirm" disabled'));node('resetName').oninput({target:{value:'wrong'}});assert(button.disabled);node('resetName').value='test-device';node('resetName').oninput({target:{value:'test-device'}});assert.equal(button.disabled,false);await button.onclick();assert.equal(writes.length,1);assert.equal(writes[0].endpoint,'reset');assert.deepEqual(writes[0].body,{confirm:'reset',images:false,format_storage:true});assert.equal(node('dialog').open,false);});
 await test('ordinary reset starts with format unselected and image-only option available',()=>{U.ACT.factory();assert.equal(node('resetStorage').checked,false);assert.equal(node('resetImages').disabled,false);node('resetStorage').onchange({target:{checked:true}});assert(node('resetImages').disabled);node('resetStorage').onchange({target:{checked:false}});assert.equal(node('resetImages').disabled,false);});
 await test('partial reset stays visible and names the failed stage in both languages',async()=>{for(const language of[0,1]){U.S.draft.language=language;U.ACT.repair();failure=new U.ApiError(500,{code:'reset_partial',stage:'format',settings_reset:true,counters_erased:true});node('resetName').value='test-device';await node('dialog [data-act=resetConfirm]').onclick();assert(node('dialog').open);assert(node('resetError').textContent.includes(language?'Formatting':'Formatierung'));assert(node('resetError').textContent.includes('reset_partial'));assert(node('resetError').textContent.includes(language?'may already have changed':'bereits geändert'));}});
 console.log(passed+' Web factory-reset cases PASS; actual production functions, controlled DOM/network');
})().catch(error=>{console.error(error);process.exitCode=1;});
