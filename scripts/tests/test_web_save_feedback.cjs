'use strict';
// Actual current client, events and C++ schema/config fixture. Atomic Apply
// replaces retired per-record partial-write expectations; correlated response
// loss, read-only retries, imports and accessible feedback remain covered.
const fs = require('node:fs'), assert = require('node:assert/strict'),
      {harness} = require('./web_config_harness.cjs');
let h, U, element, results = [];
const copy = x => JSON.parse(JSON.stringify(x)),
      dirty = () => copy(U.changes()).map(c => c.path);
function style(severity) {
  if (severity === 'info') {
    assert.equal(element('toast').className, 'toast');
    assert.equal(element('toast').attributes.role, 'status');
    assert.match(element('toast').innerHTML, /aria-hidden="true"/);
    assert.match(element('toast').textContent, /Bestätigung:/);
    return;
  }
  assert.equal(U.S.saveNotice.severity, severity);
  const html = element('savebarIn').innerHTML;
  assert(html.includes('notice ' + (severity === 'warning' ? 'warn' : 'err')));
  assert(html.includes(
      'role="' + (severity === 'error' ? 'alert' : 'status') + '"'));
  assert.match(html, /aria-hidden="true"/);
  assert.match(html, severity === 'warning' ? /Warnung:/ : /Fehler:/);
  assert.equal(element('toast').innerHTML, '');
}
async function test(name, run) {
  h = harness(process.argv[2]);
  ({U, element} = h);
  await run();
  results.push({name, passed: true, requests: copy(h.calls), dirty: dirty()});
  console.log('PASS ' + name);
}
const edit = () => {
  U.S.draft.screen_policy.frame = true;
};
async function importDraft() {
  const backup = {
    version: h.data.schema.config_version,
    desired: copy(h.state.config.desired),
    adopted_names: {
      format: h.data.schema.names_format,
      version: h.data.schema.names_source_version,
      clubs: [{
        key: 'club',
        de: 'FC Bayern',
        en: 'Bayern',
        short: 'FCB',
        league: 'league',
        ids: {espn: [40]},
        aliases: ['original']
      }],
      exceptions: [{provider: 2, id: 40, name: ''}]
    }
  };
  backup.desired.settings.night_brightness = 20;
  backup.desired.screens.frame = true;
  const text = JSON.stringify(backup);
  U.bindSystem();
  await element('importFile').onchange({
    target: {
      value: 'fixture',
      files: [{size: Buffer.byteLength(text), text: async () => text}]
    }
  });
  const confirm = element('dialog [data-act=confirm]');
  assert.equal(typeof confirm.onclick, 'function');
  await confirm.onclick({target: confirm});
  return backup;
}
(async () => {
  await test(
      'healthy atomic save gives a neutral confirmation and clears all changes',
      async () => {
        edit();
        await h.click();
        style('info');
        assert.equal(h.state.writes, 1);
        assert.equal(dirty().length, 0);
        assert.deepEqual(copy(U.validate(U.S.draft)), []);
        assert.equal(U.S.pendingApply, null);
        assert.equal(element('savebar').hidden, true);
      });
  await test(
      'acknowledged write retries only busy verification GETs', async () => {
        h.state.mode = 'busy-after';
        edit();
        await h.click();
        style('warning');
        assert.equal(h.state.writes, 1);
        assert.equal(dirty().length, 0);
        assert(U.S.pendingApply.accepted);
        assert.equal(h.calls.filter(c => c.code === 'busy').length, 3);
        assert.deepEqual(h.delays.filter(x => x < 1000), [250, 500]);
        assert(
            element('savebarIn').innerHTML.includes('data-act="saveRefresh"'));
        assert(!element('savebarIn').innerHTML.includes('data-act="save"'));
      });
  await test(
      'read-only refresh preserves edits made after acknowledgement',
      async () => {
        h.state.mode = 'busy-after';
        edit();
        await h.click();
        U.S.draft.night_brightness = 21;
        h.state.mode = 'ok';
        await h.click('saveRefresh');
        assert.equal(h.state.writes, 1);
        assert.equal(U.S.draft.night_brightness, 21);
        assert.deepEqual(dirty(), ['night_brightness']);
        assert.equal(U.S.saveNotice, null);
        style('info');
      });
  await test(
      'busy once during verification retries GET then confirms once',
      async () => {
        edit();
        h.state.mode = 'busy-once';
        await h.click();
        style('info');
        assert.equal(h.state.writes, 1);
        assert.equal(h.calls.filter(c => c.code === 'busy').length, 1);
        assert(h.delays.includes(250));
      });
  await test(
      'explicit rejection remains red and preserves every draft section',
      async () => {
        h.state.mode = 'reject';
        edit();
        U.S.draft.night_brightness = 20;
        U.S.draft.competitions.push(h.entry());
        const before = copy(h.state.config);
        await h.click();
        style('error');
        assert.equal(h.state.writes, 1);
        assert.deepEqual(h.state.config, before);
        for (const field
                 of ['night_brightness', 'competitions', 'screen_policy.frame'])
          assert(dirty().includes(field));
        assert.equal(U.S.pendingApply, null);
      });
  await test(
      'unknown NVS result blocks every resend and keeps the full draft',
      async () => {
        h.state.mode = 'unknown';
        edit();
        await h.click();
        style('error');
        assert(U.S.pendingApply && !U.S.pendingApply.accepted);
        assert.match(U.S.saveNotice.message, /Speicherergebnis unklar/);
        h.state.mode = 'ok';
        await h.click();
        assert.equal(h.state.writes, 1);
        assert(U.S.pendingApply);
        assert.deepEqual(dirty(), ['screen_policy.frame']);
      });
  await test(
      'lost acknowledgement resolves by exact Apply ID using GET only',
      async () => {
        h.state.mode = 'lost';
        edit();
        await h.click();
        style('error');
        assert.equal(h.state.writes, 1);
        assert(U.S.pendingApply && !U.S.pendingApply.accepted);
        h.state.mode = 'ok';
        await h.click('saveRefresh');
        style('info');
        assert.equal(h.state.writes, 1);
        assert.equal(U.S.pendingApply, null);
        assert.equal(dirty().length, 0);
      });
  await test(
      'malformed aggregate receipt is unconfirmed until exact GET resolves it',
      async () => {
        h.state.mode = 'bad-receipt';
        edit();
        await h.click();
        style('error');
        assert.equal(h.state.writes, 1);
        assert(U.S.pendingApply && !U.S.pendingApply.accepted);
        h.state.mode = 'ok';
        await h.click('saveRefresh');
        assert.equal(h.state.writes, 1);
        assert.equal(U.S.pendingApply, null);
        assert.equal(dirty().length, 0);
      });
  await test(
      'non-busy verification failures remain red without read retry or mutation',
      async () => {
        edit();
        await U.writeModel();
        h.state.mode = 'memory-read';
        try {
          await U.refreshSave();
          assert.fail();
        } catch (error) {
          assert.equal(error.code, 'memory');
        }
        style('error');
        assert.equal(h.calls.filter(c => c.code === 'memory').length, 1);
        assert.equal(h.state.writes, 1);
        assert(U.S.pendingApply.accepted);
        assert.match(U.S.saveNotice.message, /Gespeichert/);
      });
  await test(
      'session error after exact readback never claims the accepted save was rejected',
      async () => {
        h.state.hook = call => call.endpoint === 'session' ?
            h.response(
                503,
                {code: 'service_failed', error: 'controlled session failure'}) :
            undefined;
        edit();
        await h.click();
        style('error');
        assert.equal(h.state.writes, 1);
        assert.equal(dirty().length, 0);
        assert.match(U.S.saveNotice.message, /Gespeichert/);
        assert(!U.S.saveNotice.message.includes('Speichern wurde abgelehnt'));
      });
  await test(
      'import stages all sections and full metadata without any write',
      async () => {
        const backup = await importDraft();
        assert.equal(h.state.writes, 0);
        assert.equal(element('dialog').open, false);
        assert.equal(U.S.draft.night_brightness, 20);
        assert.equal(U.S.draft.screen_policy.frame, true);
        assert.deepEqual(
            copy(U.S.pendingNames),
            copy(U.validateAdoptedNames(backup.adopted_names)));
        assert(dirty().includes('adopted_names'));
      });
  await test(
      'rejected imported configuration retains its complete names and other changes',
      async () => {
        const backup = await importDraft();
        h.state.mode = 'reject';
        await h.click();
        style('error');
        assert.equal(h.state.writes, 1);
        assert.deepEqual(
            copy(U.S.pendingNames),
            copy(U.validateAdoptedNames(backup.adopted_names)));
        assert(dirty().includes('night_brightness'));
        assert(dirty().includes('screen_policy.frame'));
      });
  await test(
      'imported Apply is one write and acknowledged verification never resends metadata',
      async () => {
        await importDraft();
        h.state.mode = 'busy-after';
        await h.click();
        style('warning');
        assert.equal(h.state.writes, 1);
        assert.equal(U.S.pendingNames, null);
        const sent = h.calls.find(c => c.method === 'PUT').body;
        assert(sent.adopted_names.texts);
        assert.equal(
            sent.adopted_names.version, h.data.schema.names_wire_version);
        h.state.mode = 'ok';
        await h.click('saveRefresh');
        assert.equal(h.state.writes, 1);
        assert.equal(dirty().length, 0);
        style('info');
      });
  await test(
      'current export includes complete adopted collection in its one envelope',
      async () => {
        h.state.names = {
          format: h.data.schema.names_format,
          version: h.data.schema.names_source_version,
          clubs: [{
            key: 'club',
            de: 'Verein',
            ids: {espn: [40]},
            aliases: ['original']
          }],
          exceptions: []
        };
        await U.exportSettings();
        const exported = JSON.parse(await h.state.blob.text());
        assert.equal(exported.version, h.data.schema.config_version);
        assert.deepEqual(exported.desired, h.state.config.desired);
        assert.deepEqual(exported.adopted_names, h.state.names);
        assert.equal(h.state.writes, 0);
      });
  await test('severity is conveyed by text icon role and translated labels', () => {
    for (const [severity, word] of [
             ['error', 'Error'], ['warning', 'Warning'],
             ['info', 'Confirmation']]) {
      U.S.draft.language = 1;
      U.toast('Example', severity);
      assert.match(element('toast').innerHTML, /aria-hidden="true"/);
      assert(element('toast').textContent.includes(word + ':'));
      assert.equal(
          element('toast').attributes.role,
          severity === 'error' ? 'alert' : 'status');
    }
    for (
        const text of
            ['Status erneut prüfen',
             'Gespeichert. Die Nachprüfung ist noch offen. Status erneut prüfen; Speichern muss nicht wiederholt werden.',
             'Speicherergebnis unklar. Nicht erneut speichern. Status prüfen; bei unklarem Speicherzustand Gerät neu starten.'])
      assert(U.W[text], text);
  });
  if (process.argv[3])
    fs.writeFileSync(
        process.argv[3],
        JSON.stringify(
            {
              scope:
                  'Actual script/current-codec fixture, controlled HTTP/DOM; no browser/device evidence',
              passed: results.length,
              results
            },
            null, 2) +
            '\n');
  console.log(
      results.length + ' current save-feedback/import/export cases passed');
})().catch(error => {
  console.error(error);
  process.exitCode = 1
});
