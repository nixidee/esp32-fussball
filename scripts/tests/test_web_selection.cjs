'use strict';
// Actual current bundle/schema, wizard/dialog and HTTP client. Only the DOM,
// device responses and browser timing are controlled; no device claim.
const assert = require('node:assert/strict'), vm = require('node:vm');
const {harness} = require('./web_config_harness.cjs');
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return {promise, resolve, reject};
};
const first = '1234567800000001', second = '1234567800000002';
function setup() {
  const h = harness(), calls = [], starts = [], reads = [];
  const decorate = node => {
    if (node.decorated) return node;
    node.decorated = true;
    node.addEventListener = () => {};
    const query = node.querySelector;
    node.querySelector = selector => decorate(query.call(node, selector));
    return node;
  };
  const byId = h.context.document.getElementById;
  h.context.document.getElementById = id => {
    if (id.startsWith('wizard') && !h.element('dialog').innerHTML.includes('id="'+id+'"')) return null;
    return decorate(byId(id));
  };
  h.context.fetch = async (url, options = {}) => {
    const path = url.split('/api/v1/')[1], body = options.body && JSON.parse(options.body);
    calls.push({path, body, method: options.method});
    if (path === 'data/select') {
      const next = starts.shift(); assert(next, 'Unexpected query start');
      return await next();
    }
    if (path === 'data/select/cancel') return h.response(200, {ok:true});
    assert(path.startsWith('selection?query='), path);
    const next = reads.shift(); assert(next, 'Unexpected result read');
    return await next(path);
  };
  const open = () => {
    h.U.openWizard();
    const w = vm.runInContext('wizard', h.context);
    w.step = 1;
    h.U.drawWizard();
    return w;
  };
  return {...h, calls, starts, reads, open,
    headerClose: () => h.element('dialog').querySelector('[data-close]').onclick(),
    escape: () => { h.element('dialog').oncancel(); h.element('dialog').close(); },
    result: (query, name) => h.response(200, {query,busy:false,more:false,items:[{id:'bl1',season:'2026',name}]}),
  };
}
const settle = async () => { for (let i=0;i<12;i++) await Promise.resolve(); };
const watchdog = setTimeout(() => { throw Error('Selection race test did not finish'); }, 15000);
let passed = 0;
async function test(name, run) { await run(setup()); ++passed; console.log('PASS ' + name); }
(async () => {
  await test('each result read names the acknowledged query and adopts only matching rows', async h => {
    const w=h.open(); h.starts.push(async()=>h.response(200,{query:first}));
    h.reads.push(async path=>{assert.equal(path,'selection?query='+first);return h.result(first,'Own response');});
    await h.U.runSelection(false);
    assert.equal(w.list[0].name,'Own response');
    assert.equal(h.calls.filter(x=>x.path==='data/select/cancel').length,0);
  });
  await test('a foreign complete result is rejected without replacing loaded entries', async h => {
    const w=h.open(); w.list=[{id:'keep',name:'Kept'}];
    h.starts.push(async()=>h.response(200,{query:first}));
    h.reads.push(async()=>h.result(second,'Foreign response'));
    await h.U.runSelection(false);
    assert.equal(w.list[0].name,'Kept');assert(w.error);
    assert.equal(h.calls.at(-1).path,'data/select/cancel');assert.equal(h.calls.at(-1).body.query,first);
  });
  await test('superseded-query HTTP conflict retains the wizard draft and old rows', async h => {
    const w=h.open();w.list=[{id:'keep',name:'Kept'}];
    h.starts.push(async()=>h.response(200,{query:first}));
    h.reads.push(async()=>h.response(409,{code:'changed'}));
    await h.U.runSelection(false);assert.equal(w.list[0].name,'Kept');assert(w.error);assert.equal(w.busy,false);
  });
  for (const action of ['headerClose','escape']) {
    await test(action+' cancels a pending owner and late GET cannot reopen or adopt', async h => {
      const w=h.open(), reply=deferred();h.starts.push(async()=>h.response(200,{query:first}));
      h.reads.push(()=>reply.promise);const work=h.U.runSelection(false);await settle();
      assert.equal(w.activeQuery,first);h[action]();await settle();
      reply.resolve(h.result(first,'Late old response'));await work;
      assert.equal(h.element('dialog').open,false);assert.equal(w.list.length,0);
      assert.equal(h.calls.filter(x=>x.path==='data/select/cancel').length,1);
    });
  }
  await test('closing before start acknowledgement cancels its eventual ID without reading results', async h => {
    const w=h.open(), reply=deferred();h.starts.push(()=>reply.promise);
    const work=h.U.runSelection(false);await settle();h.headerClose();
    reply.resolve(h.response(200,{query:first}));await work;
    assert.equal(w.list.length,0);assert.equal(h.calls.length,2);
    assert.equal(h.calls[1].path,'data/select/cancel');assert.equal(h.calls[1].body.query,first);
  });
  await test('an unacknowledged start is never resent or replaced by an unrelated GET', async h => {
    const w=h.open();h.starts.push(async()=>{throw Error('lost receipt');});
    await h.U.runSelection(false);assert.equal(h.calls.length,1);assert(w.error);assert.equal(w.list.length,0);
  });
  await test('explicit busy rejection retries boundedly without reading another browser query', async h => {
    const w=h.open();h.starts.push(async()=>h.response(409,{code:'busy'}));
    h.starts.push(async()=>h.response(200,{query:first}));h.reads.push(async()=>h.result(first,'After busy'));
    await h.U.runSelection(false);assert.equal(w.list[0].name,'After busy');
    assert.deepEqual(h.calls.map(x=>x.path),['data/select','data/select','selection?query='+first]);
  });
  await test('close and reopen keep the new query independent of a late old result', async h => {
    const old=h.open(), oldReply=deferred();h.starts.push(async()=>h.response(200,{query:first}));
    h.reads.push(()=>oldReply.promise);const previous=h.U.runSelection(false);await settle();h.headerClose();
    const current=h.open();h.starts.push(async()=>h.response(200,{query:second}));h.reads.push(async()=>h.result(second,'New response'));
    await h.U.runSelection(false);oldReply.resolve(h.result(first,'Obsolete'));await previous;
    assert.equal(current.list[0].name,'New response');assert.equal(old.list.length,0);assert(h.element('dialog').open);
  });
  await test('repeated busy rejections stop without a result read or accepted write', async h => {
    const w=h.open();
    for(let i=0;i<h.data.schema.client.busy_attempts;i++)
      h.starts.push(async()=>h.response(409,{code:'busy'}));
    await h.U.runSelection(false);
    assert(w.error);assert.equal(w.busy,false);assert.equal(w.list.length,0);
    assert.equal(h.calls.length,h.data.schema.client.busy_attempts);
    assert(h.calls.every(call=>call.path==='data/select'));
  });
  await test('closing on a busy refusal prevents every later retry', async h => {
    const w=h.open();
    h.starts.push(async()=>{h.headerClose();return h.response(409,{code:'busy'});});
    await h.U.runSelection(false);
    assert.equal(h.calls.length,1);assert.equal(w.list.length,0);
    assert.equal(h.element('dialog').open,false);
  });
  await test('team results preserve maximum numeric identity without text conversion', async h => {
    const w=h.open();w.step=2;w.entry.primary.competition='bl1';h.U.drawWizard();
    h.starts.push(async()=>h.response(200,{query:first}));
    h.reads.push(async()=>h.response(200,{query:first,busy:false,items:[{id:4294967295,name:'Maximum'}]}));
    await h.U.runSelection(false);
    assert.equal(w.error,'');assert.equal(w.list[0].id,4294967295);
    assert.equal(typeof w.list[0].id,'number');
  });
  await test('invalid or textual team identity cannot replace an existing list', async h => {
    for(const id of ['40',0,-1,4294967296,1.5,null]) {
      const w=h.open();w.step=2;w.entry.primary.competition='bl1';w.list=[{id:40,name:'Retained'}];h.U.drawWizard();
      h.starts.push(async()=>h.response(200,{query:first}));
      h.reads.push(async()=>h.response(200,{query:first,busy:false,items:[{id,name:'Invalid'}]}));
      await h.U.runSelection(false);assert(w.error);assert.equal(w.list[0].name,'Retained');h.headerClose();
    }
  });
  await test('malformed item arrays are errors while confirmed empty remains valid', async h => {
    for(const items of [undefined,null,{},[null],[{id:40,name:7}]]) {
      const w=h.open();w.step=2;w.entry.primary.competition='bl1';h.U.drawWizard();
      h.starts.push(async()=>h.response(200,{query:first}));
      h.reads.push(async()=>h.response(200,{query:first,busy:false,items}));
      await h.U.runSelection(false);assert(w.error);assert.equal(w.list.length,0);h.headerClose();
    }
    const w=h.open();w.step=2;w.entry.primary.competition='bl1';h.U.drawWizard();
    h.starts.push(async()=>h.response(200,{query:first}));
    h.reads.push(async()=>h.response(200,{query:first,busy:false,items:[]}));
    await h.U.runSelection(false);assert.equal(w.error,'');assert.equal(w.list.length,0);
  });
  console.log(passed+' correlated wizard/client race cases passed');
  clearTimeout(watchdog);
})().catch(error=>{clearTimeout(watchdog);console.error(error);process.exitCode=1;});
