'use strict';
// The actual current config/schema, full production event handlers and one
// coherent Common Apply replace the retired multi-record policy choreography.
const assert = require('node:assert/strict'),
      {harness} = require('./web_config_harness.cjs');
let h, U, events, element, passed = 0;
const phases = ['own', 'other', 'matchday', 'idle', 'break'],
      types = ['single', 'conference', 'table', 'crest', 'matchday', 'images'],
      sports = ['single', 'conference', 'table', 'matchday'];
const copy = x => JSON.parse(JSON.stringify(x));
async function test(name, fn) {
  h = harness(process.argv[2]);
  ({U, events, element} = h);
  await fn();
  passed++;
  console.log('PASS ' + name);
}
(async () => {
  await test('strict policy validates the complete owner profiles', () => {
    assert.equal(U.validate(U.S.draft).length, 0);
    assert.equal(
        U.validateScreenPolicy(
             U.screenPayload(), h.state.config.desired.competitions)
            .length,
        0);
    const bad = U.screenPayload();
    bad.phases.idle.table = [true];
    assert(U.validateScreenPolicy(bad, h.state.config.desired.competitions)
               .length);
    bad.phases.idle.table = [true, false, 43201];
    assert(U.validateScreenPolicy(bad, h.state.config.desired.competitions)
               .some(x => x.path === 'screen_policy.phases.idle.table[2]'));
    bad.typo = true;
    assert(U.validateScreenPolicy(bad, h.state.config.desired.competitions)
               .some(x => x.path === 'screen_policy.typo'));
  });
  await test(
      'manual-only visit stays saved and image multiples are strict', () => {
        const p = U.S.draft.screen_policy;
        p.phases.own.table = [true, false, 321];
        assert(!U.screenPhaseMarkup().includes(
            'data-path="screen_policy.phases.own.table[2]"'));
        assert.equal(p.phases.own.table[2], 321);
        p.phases.own.images = [true, true, 61];
        assert(U.validate(U.S.draft).some(
            x => x.path === 'screen_policy.phases.own.images[2]'));
        p.phases.own.images[2] = 60;
        assert.equal(U.validate(U.S.draft).length, 0);
      });
  await test(
      'null inheritance and explicit false survive splitting and collapse',
      () => {
        const row = () => U.screenRow(0);
        row().own.single = [false, null, null];
        U.S.draft.screen_policy.phases.own.per_competition = ['single'];
        U.ACT.policySplit({dataset: {phase: 'own', type: 'single'}});
        assert.deepEqual(copy(row().own.single), [false, null, null]);
        U.ACT.policySplit({dataset: {phase: 'own', type: 'single'}});
        assert.deepEqual(copy(row().own.single), [false, null, null]);
        const html = U.screenPhaseMarkup();
        assert(html.includes('data-kind="policybool"'));
        const path = U.screenRowPath(0) + '.own.single[0]';
        events.input({
          target: {
            dataset: {path, kind: 'policybool'},
            type: 'select-one',
            value: ''
          }
        });
        assert.equal(row().own.single[0], null);
        events.input({
          target: {
            dataset: {path, kind: 'policybool'},
            type: 'select-one',
            value: 'false'
          }
        });
        assert.equal(row().own.single[0], false);
        row().own.single[0] = null;
        const visit = U.screenRowPath(0) + '.own.single[2]';
        events.input({target: {dataset: {inherit: visit}, checked: false}});
        assert.equal(row().own.single[2], 30);
        events.input({target: {dataset: {inherit: visit}, checked: true}});
        assert.equal(row().own.single[2], null);
      });
  await test(
      'a device without backlight offers no brightness or night controls and retains rotation IP and inputs',
      () => {
        U.S.schema.backlight = false;
        U.S.draft.night_enabled = true;
        const markup = U.displayPage();
        for (const key
                 of ['brightness', 'night_enabled', 'night_start', 'night_end',
                     'night_brightness'])
          assert(!markup.includes('data-path="' + key + '"'), key);
        for (const key
                 of ['rotation', 'ip_badge_s', 'ip_badge_bottom',
                     'ip_badge_ap_permanent', 'scroll_repeat_ms',
                     'input_roles[0]', 'input_roles[1]', 'input_roles[2]'])
          assert(markup.includes('data-path="' + key + '"'), key);
        assert(!markup.includes('20 % Deckkraft'));
        assert(!markup.includes('<div></div>'));
        assert(markup.includes('Ausrichtung'));
      });
  await test(
      'a backlight device retains hardware night brightness rotation IP and input settings',
      async () => {
        U.S.schema.backlight = true;
        const markup = U.displayPage();
        for (const key
                 of ['brightness', 'night_enabled', 'night_start', 'night_end',
                     'night_brightness', 'rotation', 'ip_badge_s',
                     'ip_badge_bottom', 'ip_badge_ap_permanent',
                     'scroll_repeat_ms', 'input_roles[0]', 'input_roles[1]',
                     'input_roles[2]'])
          assert(markup.includes('data-path="' + key + '"'), key);
        U.S.draft.input_roles[0] = 0;
        assert(U.validate(U.S.draft).some(e => e.path === 'input_roles'));
        U.S.draft.input_roles[0] = 1;
        U.S.draft.night_brightness = 20;
        U.S.draft.rotation = 2;
        U.S.draft.ip_badge_s = 120;
        await U.save();
        assert.equal(h.state.config.desired.settings.night_brightness, 20);
        assert.equal(h.state.config.desired.settings.rotation, 2);
        assert.equal(h.state.config.desired.settings.ip_badge_s, 120);
        assert.deepEqual(
            h.state.config.desired.settings.input_roles,
            h.data.config.desired.settings.input_roles);
        assert(!h.calls.some(
            c => c.endpoint === 'screen-settings' && c.method === 'PUT'));
      });
  await test(
      'hardware night controls explain the clock and bounds and disable only their inactive values',
      () => {
        U.S.schema.backlight = true;
        U.S.draft.night_enabled = false;
        const markup = U.displayPage();
        for (const key of ['night_start', 'night_end', 'night_brightness'])
          assert(
              new RegExp('<input[^>]+data-path="' + key + '"[^>]*disabled')
                  .test(markup),
              key);
        for (const key of ['brightness', 'night_enabled', 'rotation'])
          assert(
              !new RegExp(
                   '<(?:input|select)[^>]+data-path="' + key + '"[^>]*disabled')
                   .test(markup),
              key);
        assert(markup.includes('gültige Gerätezeit'));
        assert(markup.includes(
            'Farben, Hintergrund und Bilder bleiben unverändert'));
        assert(markup.includes('0 bis 100 %'));
        U.S.draft.language = 1;
        const english = U.displayPage();
        for (const text
                 of ['Changes only the display backlight',
                     'Requires a valid device clock',
                     'Equal times give an empty night window'])
          assert(english.includes(text), text);
      });
  await test(
      'frame lives on overview and new options use five compact phases', () => {
        const overview = U.overview(), display = U.displayPage();
        assert(overview.includes('data-path="screen_policy.frame"'));
        assert(!overview.includes('data-act=\"screenshot\"'));
        U.S.saved.overview_screenshot = true;
        assert(U.overview().includes('Bildschirmfoto herunterladen'));
        assert(display.includes('role="tablist"'));
        assert.equal(
            (display.match(/data-act="policyPhase"/g) || []).length, 5);
        assert.equal(
            display.includes('data-path="brightness"'),
            U.S.schema.backlight === true);
        assert(
            display.includes('data-path="input_roles[0]"') ===
            Number.isInteger(U.S.schema.inputs));
        assert(!display.includes('data-path="idle_screen"'));
        assert(!display.includes('data-path="whole_matchday"'));
        assert(!display.includes('data-disc='));
      });
  await test(
      'one Apply contains general settings competitions shared rules and attached overrides',
      async () => {
        U.S.draft.night_brightness = 20;
        U.S.draft.screen_policy.frame = true;
        U.screenRow(0).idle.table = [false, null, 99];
        await U.writeModel();
        assert.equal(h.state.writes, 1);
        const body = h.calls.find(x => x.method === 'PUT').body;
        assert.equal(body.desired.settings.night_brightness, 20);
        assert(!('screen_policy' in body.desired.settings));
        assert(!('_revision' in body.desired.settings));
        assert.deepEqual(
            copy(body.desired.competitions[0].display.idle.table),
            [false, null, 99]);
        assert.equal(body.desired.screens.frame, true);
        assert(!JSON.stringify(body).includes('_screenKey'));
      });
  await test(
      'new identical competitions retain attached preferences through nonsequential identities',
      async () => {
        U.S.draft.competitions.push(h.entry(), h.entry());
        U.prepareScreenModel(U.S.draft);
        U.screenRow(1).own.single = [false, null, 123];
        U.screenRow(2).own.single = [true, false, 234];
        await U.writeModel();
        assert.equal(U.S.saved.competitions[1].id, 47);
        assert.equal(U.S.saved.competitions[2].id, 9);
        assert.deepEqual(
            copy(h.state.config.desired.competitions[1].display.own.single),
            [false, null, 123]);
        assert.deepEqual(
            copy(h.state.config.desired.competitions[2].display.own.single),
            [true, false, 234]);
        assert.equal(U.changes().length, 0);
      });
  await test(
      'reorder and delete carry surviving club and display preferences together',
      async () => {
        U.S.draft.competitions.push(h.entry(31, 812));
        U.S.draft.competitions[0].club.own_special = false;
        U.screenRow(1).idle.table[2] = 99;
        await U.writeModel();
        await U.verifySaved();
        U.moveCompetition(1, -1);
        U.removeCompetition(1);
        assert.equal(U.S.draft.competitions[0].id, 31);
        assert.equal(U.screenRow(0).idle.table[2], 99);
        await U.save();
        assert.deepEqual(
            h.state.config.desired.competitions.map(c => c.id), [31]);
        assert.equal(
            h.state.config.desired.competitions[0].display.idle.table[2], 99);
        assert.equal(U.changes().length, 0);
      });
  await test(
      'frame-only saves the complete initial list in one Apply', async () => {
        U.S.draft.screen_policy.frame = true;
        await U.save();
        assert.deepEqual(
            h.calls.filter(c => c.method === 'PUT').map(c => c.endpoint),
            ['config']);
        assert.equal(h.state.config.desired.screens.frame, true);
        assert.equal(h.state.config.desired.competitions.length, 1);
      });
  await test(
      'rejected Apply preserves every section instead of partially publishing',
      async () => {
        const before = copy(h.state.config);
        U.S.draft.night_brightness = 20;
        U.S.draft.screen_policy.frame = true;
        U.S.draft.competitions.push(h.entry());
        h.state.mode = 'reject';
        await assert.rejects(U.writeModel());
        assert.deepEqual(h.state.config, before);
        assert.equal(
            U.S.saved.night_brightness,
            before.desired.settings.night_brightness);
        assert(U.changes().some(c => c.path === 'night_brightness'));
        assert(U.changes().some(c => c.path === 'screen_policy.frame'));
        assert(U.changes().some(c => c.path === 'competitions'));
      });
  await test(
      'foreign coherent revision rejects the draft without adopting another programme',
      async () => {
        U.S.draft.screen_policy.frame = true;
        h.state.config.revision++;
        h.state.config.desired.screens.frame_crest = false;
        await assert.rejects(
            U.writeModel(), e => e.code === 'settings_changed');
        assert.equal(h.state.writes, 1);
        assert.equal(
            U.S.saved.screen_policy.frame_crest,
            h.data.config.desired.screens.frame_crest);
        assert.equal(U.S.draft.screen_policy.frame, true);
      });
  await test(
      'adopted collection metadata is accepted using actual published schema',
      () => {
        const n = {
          format: h.data.schema.names_format,
          version: h.data.schema.names_source_version,
          clubs: [{
            key: 'club',
            de: 'Verein',
            short: 'FCB',
            league: 'league',
            ids: {espn: [40]},
            aliases: ['original']
          }],
          exceptions: [{provider: 2, id: 40, name: ''}]
        };
        const wire = U.packAdoptedNames(n);
        assert.equal(wire.version, h.data.schema.names_wire_version);
        assert.deepEqual(
            copy(U.validateAdoptedNames(wire)),
            copy(U.validateAdoptedNames(n)));
      });
  await test(
      'maximum competition matrix uses aggregate transport limit with no browser identity',
      () => {
        U.S.draft.competitions = Array.from(
            {length: h.data.schema.limits.competitions},
            (_, i) => h.entry(i + 1, 100 + i));
        U.prepareScreenModel(U.S.draft);
        for (const c of U.S.draft.competitions)
          for (const phase of phases)
            for (const type of sports)
              c.display[phase][type] = [false, false, 43200];
        for (const phase of phases)
          U.S.draft.screen_policy.phases[phase].per_competition = copy(sports);
        assert.equal(U.validate(U.S.draft).length, 0);
        const wire = JSON.stringify(U.applicationPayload());
        assert(!wire.includes('_screenKey'));
        assert(Buffer.byteLength(wire) < h.data.schema.max_apply_body);
      });
  await test(
      'every contextual programme and competition help has English translation',
      () => {
        const decode = s => s.replace(/&quot;/g, '"')
                                .replace(/&#39;/g, '\'')
                                .replace(/&amp;/g, '&')
                                .replace(/&lt;/g, '<')
                                .replace(/&gt;/g, '>'),
              helps = new Set();
        for (const phase of phases) {
          U.S.screenPhase = phase;
          U.S.draft.screen_policy.phases[phase].images = [true, true, 60];
          U.S.draft.screen_policy.phases[phase].per_competition = copy(sports);
          for (const t of sports)
            U.S.draft.screen_policy.phases[phase][t][1] = true;
          for (const type of types) {
            U.S.screenAppearance = type;
            U.S.draft.screen_policy.automatic_break = true;
            for (const html of [U.displayPage(), U.overview()])
              for (const match of html.matchAll(/data-help="([^"]+)"/g))
                helps.add(decode(match[1]));
          }
        }
        U.S.open = 0;
        for (const match of U.competitions().matchAll(/data-help="([^"]+)"/g))
          helps.add(decode(match[1]));
        assert(helps.size > 20);
        for (const help of helps) assert(U.W[help], help);
        assert.equal(U.W.Ja, 'Yes');
        assert.equal(U.W.Nein, 'No');
      });
  console.log(
      passed +
      ' current screen-policy/config cases passed; no layout/device claim');
})().catch(e => {
  console.error(e);
  process.exitCode = 1
});
