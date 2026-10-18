'use strict';
// Actual production image actions/reconciliation, controlled HTTP and DOM.
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm'), assert = require('node:assert/strict');
const {readWebSource, productionFunctions} = require('./web_source.cjs');
const root = path.resolve(__dirname, '../..'), {script} = readWebSource();
const functions = productionFunctions(script, [
  'imageName', 'imageAllowance', 'imageIds', 'libraryEntries', 'screenMeaning', 'sameObject',
  'sameScreenPolicy', 'screenConflict', 'competitionPayload', 'mergeValues', 'prepareScreenModel', 'mergeBaseline',
  'pruneMissing', 'reconcileImages', 'imageDeleteFailure', 'deleteImage', 'deleteAllImages',
]);
const types = ['single', 'conference', 'table', 'crest', 'matchday', 'images'];
function model() {
  return {hostname: 'saved', competitions: [], screen_policy: {
    competitions: [], slides: [1, 9], appearance: Object.fromEntries(types.map(type => [type, {image: type === 'table' ? 1 : 0}]))
  }};
}
function setup(language = 0) {
  const writes = [], reads = [], result = {textContent: ''};
  const server = model();
  const S = {saved: structuredClone(server), draft: structuredClone(server), schema: {limits: {images: 10}, image_quota: 327680, image_max: 32768, replacement_workspace: 40960}, images: {items: [{id: 1}, {id: 2}]}};
  S.draft.hostname = 'unsaved'; S.draft.screen_policy.appearance.conference.image = 7;
  const context = {S, console, structuredClone, Set, Map, Error,
    clone: structuredClone, eq: (a, b) => JSON.stringify(a) === JSON.stringify(b),
    SP_TYPES: types, IMAGE_ROLES: [['crest', 'crest'], ['background', 'background']],
    codeText: value => value ? ` [${value}]` : '',
    $: id => id === 'deleteResults' ? result : null, render() {}, toast() {},
    tf: (key, value) => key.replace('%', value),
    async raw(endpoint) { reads.push(endpoint); return structuredClone(S.images); },
    async readModel() { reads.push('settings'); return structuredClone(server); },
    async load(endpoint) { reads.push(endpoint); },
    async mutate(endpoint, body) { writes.push({endpoint, body}); if (context.failure) throw context.failure; return {ok: true}; },
  };
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(root, 'web/js/i18n.js'), 'utf8') + '\nglobalThis.translations=W;', context);
  context.t = text => language ? context.translations[text] || text : text;
  vm.runInContext(functions + '\nglobalThis.actions={deleteImage,deleteAllImages,imageAllowance};', context);
  return {context, S, server, writes, reads, result};
}
function failure(data) { return Object.assign(Error('original error'), {data, status: 500, code: 'service_failed'}); }
function clearServer(server, id) {
  server.screen_policy.slides = server.screen_policy.slides.filter(value => value !== id);
  for (const style of Object.values(server.screen_policy.appearance)) if (style.image === id) style.image = 0;
}
(async () => {
  for (const language of [0, 1]) {
    const t = setup(language);
    t.context.mutate = async (endpoint, body) => {
      t.writes.push({endpoint, body}); clearServer(t.server, body.id);
      throw failure({references_cleared: true, settings_saved: true, file_deleted: false});
    };
    await assert.rejects(t.context.actions.deleteImage(1), error => {
      assert.match(error.message, language ? /references were saved.*file was not deleted/i : /Bildverweise wurden gespeichert.*nicht gelöscht/);
      assert(error.message.includes('service_failed')); return true;
    });
    assert.equal(t.writes.length, 1); assert.equal(t.S.saved.screen_policy.appearance.table.image, 0);
    assert.equal(t.S.draft.hostname, 'unsaved'); assert.equal(t.S.draft.screen_policy.appearance.conference.image, 7);
    assert.deepEqual(Array.from(t.S.draft.screen_policy.slides), [9]);
  }
  {
    const t = setup(); const before = JSON.stringify(t.S.draft);
    t.context.failure = failure({outcome_unknown: true, references_cleared: false});
    await assert.rejects(t.context.actions.deleteImage(1), /nicht erneut löschen/);
    assert.equal(t.writes.length, 1); assert.equal(JSON.stringify(t.S.draft), before);
    assert(t.reads.every(endpoint => ['settings', 'images', 'status'].includes(endpoint)));
  }
  {
    const t = setup(1); const before = JSON.stringify(t.S.draft);
    t.context.failure = failure({outcome_unknown: true});
    await t.context.actions.deleteAllImages();
    assert.equal(t.writes.length, 1); assert.equal(t.writes[0].endpoint, 'images/delete');
    assert.match(t.result.textContent, /do not delete again/);
    assert.equal(JSON.stringify(t.S.draft), before);
  }
  {
    const t = setup();
    t.context.mutate = async (endpoint, body) => {t.writes.push({endpoint, body}); clearServer(t.server, body.id); return {ok: true, already_missing: true};};
    t.S.draft.screen_policy.appearance.crest.image = 1;
    await t.context.actions.deleteImage(1);
    assert.equal(t.S.draft.screen_policy.appearance.crest.image, 0);
    assert.equal(t.S.draft.screen_policy.appearance.conference.image, 7);
    assert.equal(t.S.draft.hostname, 'unsaved'); assert.equal(t.writes.length, 1);
  }
  {
    const t = setup();t.S.status={fs_total:100000,fs_used:59040};
    assert.equal(t.context.actions.imageAllowance(),32768,'one full workspace admits a candidate without a second reserve');
    t.S.status.fs_used++;
    assert.equal(t.context.actions.imageAllowance(),0,'insufficient workspace rejects before upload');
    t.S.status.fs_used=59040;t.S.images.used=327680-12000;
    assert.equal(t.context.actions.imageAllowance(),12000,'logical image quota remains independent');
  }
  console.log('6 image receipt scenarios PASS; actual actions/reconciliation/translations; no layout/device claim');
})().catch(error => {console.error(error); process.exitCode = 1;});
