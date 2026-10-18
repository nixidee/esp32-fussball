'use strict';
// Current aggregate API and actual C++ schema fixture. Retained general UI,
// diagnostics, network and image assertions from the earlier suite are mapped
// to their current owners. Legacy backup conversion/ShortNames and the removed
// global screen arrays are intentionally retired. Common Apply receipt/fault
// coverage lives in test_web_common_apply and test_web_save_feedback; attached
// programme/appearance controls live in test_web_screen_policy.
const fs = require('node:fs'), path = require('node:path'),
      assert = require('node:assert/strict');
const {harness} = require('./web_config_harness.cjs');
let h, U, context, events, element, model, passed = 0;
function setup() {
  h = harness(process.argv[2]);
  ({U, context, events, element} = h);
  model = h.model();
}
const networkFaultStatus = (step = 'mdns') => ({
  ready: false,
  step,
  error: 257,
  error_name: 'ESP_ERR_NO_MEM',
  code: step === 'mdns'     ? 'NET E11' :
      step === 'portal_dns' ? 'NET E10' :
                              'NET E09',
  retry_ms: 12000
});
const imageFaultStatus = (actual = 'factory', reason = 'invalid') => ({
  active: true,
  pending: false,
  id: 2,
  role: 'background',
  actual,
  failed: 'library',
  reason,
  error: 263,
  error_name: 'ESP_ERR_INVALID_RESPONSE',
  code: 'IMG E03',
  revision: 12,
  attempt: 34,
  retry_ms: 12000
});
const copy = () => structuredClone(model),
      errs = (m, sec = {}) => U.validate(m, sec);
function entry(id = 'bl1', key = 1) {
  const row = h.entry(key, key);
  row.primary.competition = id;
  return row;
}
function test(name, fn) {
  setup();
  fn();
  passed++;
  console.log('PASS ' + name);
}

test(
    'valid representative settings', () => assert.equal(errs(model).length, 0));
test(
    'diagnostics distinguish lifetime interval and unavailable heap minima',
    () => {
      assert.deepEqual(
          Array.from(
              U.heapMinimum({heap_min_scope: 'lifetime', heap_min: 40960})),
          ['Kleinster freier Speicher seit Start', '40.0 KB']);
      assert.equal(
          U.heapMinimum({heap_min_scope: 'interval', heap_min: 32768})[0],
          'Kleinster freier Speicher im Messintervall');
      for (const s
               of [{heap_min_scope: 'unavailable', heap_min: null},
                   {heap_min_scope: 'lifetime', heap_min: null},
                   {heap_min: 40960}])
        assert.equal(U.heapMinimum(s)[1], 'Nicht verfügbar');
    });
test(
    'diagnostics show usable stack capacity and leave unknown capacity explicit',
    () => {
      assert.equal(
          U.taskMinimum({
            name: 'provider',
            stack_min_free: 3200,
            stack_usable_bytes: 12800
          }),
          'provider: 3200 B / 12800 B (25.0 %)');
      assert.equal(
          U.taskMinimum(
              {name: '', id: 7, stack_min_free: 1000, stack_usable_bytes: 0}),
          '#7: 1000 B / Nicht verfügbar');
      assert.equal(
          U.taskMinimum(
              {name: 'x', stack_min_free: null, stack_usable_bytes: 1000}),
          'x: Nicht verfügbar / 1000 B');
    });
test(
    'diagnostics show firmware slot occupancy without converting unknown bytes to zero',
    () => {
      const cap = U.S.schema.ota_max;
      U.S.schema.ota_max = 2000;
      assert.equal(
          U.firmwareUsage({app_bytes: 1700}), '1.7 KB / 2.0 KB (85.0 %)');
      assert.equal(U.firmwareUsage({app_bytes: null}), 'Nicht verfügbar');
      U.S.schema.ota_max = 0;
      assert.equal(U.firmwareUsage({app_bytes: 1700}), 'Nicht verfügbar');
      U.S.schema.ota_max = cap;
    });
test('top-level setter and nested setter', () => {
  const m = copy();
  U.set(m, 'hostname', 'test');
  U.set(m, 'screen_policy.appearance.conference.text_scale', 120);
  assert.equal(m.hostname, 'test');
  assert.equal(m.screen_policy.appearance.conference.text_scale, 120)
});
test('empty numeric input remains empty via event', () => {
  events.input({
    target:
        {dataset: {path: 'brightness', kind: 'num'}, type: 'number', value: ''}
  });
  assert.equal(U.S.draft.brightness, '');
  assert(errs(U.S.draft).some(e => e.path === 'brightness'));
  U.S.draft.brightness = 100
});
test('save lock rejects pending edit events', () => {
  U.S.saving = true;
  events.input(
      {target: {dataset: {path: 'hostname'}, type: 'text', value: 'lost'}});
  assert.equal(U.S.draft.hostname, 'fussball');
  U.S.saving = false
});
test('UTF-8 byte length enforced', () => {
  const m = copy();
  m.wifi_ssid = 'ä'.repeat(17);
  assert(errs(m).some(e => e.path === 'wifi_ssid'))
});
test('paused competition still requires competition; club optional', () => {
  const m = copy(), x = entry();
  x.active = false;
  x.primary.competition = '';
  m.competitions = [x];
  assert(errs(m).some(e => e.path === 'competitions[0].primary.competition'));
  x.primary.competition = 'bl1';
  x.club.team = 0;
  assert.equal(errs(m).length, 0)
});
test('API-Football empty season allowed (current season)', () => {
  const m = copy(), x = entry();
  x.primary.provider = 1;
  x.primary.season = '';
  m.competitions = [x];
  assert.equal(errs(m).length, 0);
  x.active = false;
  assert.equal(errs(m).length, 0);
  x.primary.season = '25';
  assert(errs(m).some(e => e.path === 'competitions[0].primary.season'))
});
test(
    'secret empty-edit keeps; explicit clear removes; no values in change list',
    () => {
      U.S.sec = {wifi_password: {mode: 'change', value: ''}};
      assert(!('wifi_password' in U.payload()));
      assert.equal(U.changes().length, 0);
      U.S.sec.wifi_password = {mode: 'change', value: 'verysecret'};
      assert(U.changes().every(x => !JSON.stringify(x).includes('verysecret')));
      U.S.sec.wifi_password = {mode: 'clear', value: ''};
      assert.equal(U.payload().wifi_password, '');
      U.S.sec = {}
    });
test(
    'API header spaces rejected',
    () => assert(errs(model, {
                   api_football_key: {mode: 'change', value: 'abc def'}
                 }).some(e => e.path === 'api_football_key')));
test('competition ids unique', () => {
  const m = copy();
  m.competitions = [entry('a'), entry('b')];
  assert(errs(m).some(e => e.path === 'competitions[1].id'));
  m.competitions[1].id = 0;
  assert.equal(errs(m).length, 0)
});
test('competition move swaps the order', () => {
  U.S.draft.competitions = [entry('a', 1), entry('b', 2)];
  U.moveCompetition(0, 1);
  assert.equal(U.S.draft.competitions[0].primary.competition, 'b')
});
test('image-side saved merge preserves unrelated draft leaf and arrays', () => {
  U.S.saved = copy();
  U.S.draft = copy();
  U.S.draft.hostname = 'draft';
  U.S.draft.screen_policy.slides = [1, 2];
  const n = copy();
  n.brightness = 80;
  U.mergeBaseline(n);
  assert.equal(U.S.draft.hostname, 'draft');
  assert.equal(U.S.draft.brightness, 80);
  assert.deepEqual(Array.from(U.S.draft.screen_policy.slides), [1, 2])
});
test('missing images pruned without unrelated draft loss', () => {
  U.S.draft.screen_policy.slides = [1, 2];
  U.S.draft.hostname = 'draft';
  U.S.draft.screen_policy.appearance.single.image = 2;
  U.S.draft.screen_policy.appearance.conference.image = 1;
  U.pruneMissing(U.S.draft, new Set([2]));
  assert.equal(U.S.draft.screen_policy.appearance.conference.image, 0);
  assert.equal(U.S.draft.screen_policy.appearance.single.image, 2);
  assert.deepEqual(Array.from(U.S.draft.screen_policy.slides), [2]);
  assert.equal(U.S.draft.hostname, 'draft')
});
test('baseline JPEG accepted; progressive rejected', () => {
  assert(U.jpegBaseline(Uint8Array.from([255, 216, 255, 192, 0, 17])));
  assert(!U.jpegBaseline(Uint8Array.from([255, 216, 255, 194, 0, 17])))
});
test('strict import rejects unknown key and scalar type', () => {
  assert.throws(() => U.validateImport({hostnme: 'typo'}));
  assert.throws(() => U.validateImport({brightness: '10'}));
  assert.throws(() => U.validateImport({
    competitions: [{primary: {provider: 0, competition: 'bl1', surprise: 1}}]
  }))
});
test('import checks shape and UTF-8/name constraints', () => {
  assert.throws(() => U.validateImport({competitions: [null]}));
  assert.throws(() => U.validateImport({screens: []}));
  assert.throws(() => U.validateImport({wifi_ssid: 'ä'.repeat(17)}))
});
test(
    'six production page functions render both languages without simulator data',
    () => {
      for (const language of [0, 1]) {
        U.S.draft.language = language;
        for (const [name, fn] of Object.entries({
               overview: U.overview,
               competitions: U.competitions,
               display: U.displayPage,
               images: U.imagesPage,
               settings: U.settingsPage,
               system: U.systemPage
             })) {
          const html = fn();
          assert(html.includes('<h1>'), name);
          assert(!/S\.mock|1948|Mockup|Heimnetz|Beispielmotiv/.test(html), name)
        }
      }
      U.S.draft.language = 0
    });
test('failed status dominates header connection state', () => {
  U.S.errors.status = 'offline';
  U.renderHeader();
  assert(element('connection').textContent.includes('nicht erreichbar'));
  assert.equal(element('connection').className, 'chip err');
  delete U.S.errors.status
});
test('request limits are checked per provider', () => {
  const m = copy();
  m.providers[2].window_requests = 10;
  m.providers[3].daily_requests = 65536;
  m.providers[0].intervals.live = 5;
  const e = errs(m).map(x => x.path);
  assert.deepEqual([...e].sort(), [
    'providers[0].intervals.live', 'providers[2].window_s',
    'providers[3].daily_requests'
  ])
});
test('global limit window needs both values', () => {
  const m = copy();
  m.global_limit.window_s = 60;
  assert.deepEqual(
      [...errs(m).map(x => x.path)], ['global_limit.window_requests']);
  m.global_limit.window_requests = 30;
  assert.equal(errs(m).length, 0)
});
test('request limit change is listed per provider', () => {
  U.S.draft.providers[1].daily_requests = 50;
  assert.deepEqual(
      [...U.changes().map(r => r.path)], ['providers[1].daily_requests']);
  U.S.draft.providers[1].daily_requests = 100
});
test('every provider card has day and window limits', () => {
  const html = U.settingsPage();
  for (let i = 0; i < 4; i++)
    for (const k of ['daily_requests', 'window_requests', 'window_s'])
      assert(html.includes(`data-path="providers[${i}].${k}"`));
  assert(
      !html.includes('api_daily_budget') && !html.includes('request_spacing_s'))
});
test(
    'imported provider rows replace only their first entries and given keys',
    () => {
      const p =
          U.validateImport({
             providers:
                 [{window_requests: 9, window_s: 60}, {intervals: {live: 30}}]
           }).providers;
      assert.deepEqual(
          [
            p[0].daily_requests, p[0].window_requests, p[0].window_s,
            p[1].daily_requests, p[1].intervals.live, p[1].intervals.fixtures,
            p[2].window_s
          ],
          [0, 9, 60, 100, 30, 28800, 0]);
      assert.throws(() => U.validateImport({providers: [{}, {}, {}, {}, {}]}));
      assert.throws(
          () => U.validateImport({providers: [{daily_requests: '5'}]}));
      assert.throws(() => U.validateImport({providers: [{spacing: 5}]}));
      assert.throws(
          () => U.validateImport({providers: [{intervals: {often: 5}}]}))
    });
test('every firmware failure key has a Web cause and action', () => {
  const src = fs.readFileSync(
            path.join(
                __dirname, '../../components/football/football_model.cpp'),
            'utf8'),
        body = src.slice(
            src.indexOf('const char* causeKey('),
            src.indexOf('uint8_t causeNumber(')),
        keys = [...body.matchAll(/return "([a-z_]+)";/g)].map(m => m[1]);
  assert(keys.length >= 18);
  for (const k of keys) {
    const c = U.CAUSE[k];
    assert(c && c[0] && c[1], k);
    for (const text of c) assert(U.W[text], text)
  }
});
test(
    'failing competitions show cause, action, code and other providers', () => {
      const saved = U.S.saved, status = U.S.status;
      U.S.saved = {
        ...structuredClone(model),
        api_football_key_set: true,
        competitions: [
          {...entry('ucl', 1), competition_name: 'Champions League'},
          entry('bl1', 2)
        ]
      };
      U.S.status = {
        ...status,
        time_valid: true,
        own_match: {
          present: true,
          competition: 2,
          home: 'FC',
          away: 'SV',
          phase: 'scheduled',
          kickoff: 1760536800
        },
        last_update_ms: 5000,
        uptime_s: 60,
        competitions: [
          {
            id: 1,
            matches: 32,
            provider: 'openligadb',
            cause: 'unmaintained',
            code: 'OLG E16',
            error_provider: 'openligadb',
            last_update_ms: 5000
          },
          {
            id: 2,
            matches: 0,
            provider: 'api-football',
            cause: 'auth',
            code: 'APF E08·403',
            error_provider: 'api-football',
            http: 403
          }
        ]
      };
      U.paintStatus();
      const html = element('competitionStatus').innerHTML;
      for (const text
               of ['Anbieter trägt keine Ergebnisse ein.',
                   'Andere aktivierte Anbieter: API-Football',
                   'Fehlercode: OLG E16',
                   'Gespeicherten Schlüssel unter Einstellungen prüfen.',
                   'Fehlercode: APF E08·403', 'tag warn', 'tag err'])
        assert(html.includes(text), text);
      assert(!/Geräteprotokoll/.test(html + element('warnings').innerHTML));
      assert(element('ownStatus').innerHTML.includes('FC'));
      assert(element('ownStatus').innerHTML.includes('SV'));
      U.S.saved = saved;
      U.S.status = status
    });
test(
    'local network failure keeps working WiFi and gives phase cause action code and retry in both languages',
    () => {
      const status = U.S.status, draft = U.S.draft;
      U.S.draft = copy();
      U.S.status = {
        ...status,
        network: 'connected',
        ip: '192.0.2.1',
        time_valid: true,
        network_init: networkFaultStatus(),
        uptime_s: 1
      };
      for (const [language, phase, cause, action] of [
               [
                 0, 'Namensdienst', 'Speicher reicht nicht aus.',
                 'Im Browser die IP-Adresse des Geräts verwenden.'
               ],
               [
                 1, 'Local name service', 'Not enough memory.',
                 'Use the device\'s IP address in the browser.'
               ]]) {
        U.S.draft.language = language;
        U.paintStatus();
        const text = element('warnings').innerHTML;
        for (const expected
                 of [phase, cause, action, 'NET E11', 'ESP_ERR_NO_MEM']) {
          assert(
              U.networkFailure(U.S.status).message.includes(expected),
              expected);
          assert(text.includes(expected.replaceAll('\'', '&#39;')), expected);
        }
        assert.equal(
            element('connection').textContent,
            language ? 'Connected' : 'Verbunden');
        assert(element('networkWarning').innerHTML.includes('NET E11'));
        assert(!text.includes('Kein WLAN'));
      }
      assert(U.networkFailure(U.S.status).message.includes('Retry'));
      U.S.status = status;
      U.S.draft = draft;
    });
test(
    'initial network error and recovery preserve draft and actual status advice',
    () => {
      U.S.draft.hostname = 'unsaved';
      U.S.status = {
        ...U.S.status,
        screen: 0,
        network: 'connecting',
        time_valid: false,
        ap: false,
        last_update_ms: 0,
        screen_starting: false,
        network_init: networkFaultStatus('radio')
      };
      U.paintStatus();
      assert(element('warnings').innerHTML.includes('NET E09'));
      assert(element('networkWarning').innerHTML.includes('NET E09'));
      U.S.status = {
        ...U.S.status,
        network: 'connected',
        time_valid: true,
        network_init: {
          ready: true,
          step: '',
          error: 0,
          error_name: 'ESP_OK',
          code: '',
          retry_ms: 0
        }
      };
      U.paintStatus();
      assert(!element('warnings').innerHTML.includes('NET E'));
      assert.equal(element('networkWarning').innerHTML, '');
      assert.equal(U.S.draft.hostname, 'unsaved');
      assert(U.previewMarkup().includes('Bildschirmfoto'));
    });
test('setup network fault retains access instructions and IP', () => {
  U.S.status = {
    ...U.S.status,
    screen: 3,
    screen_reason: 'setup',
    ap: true,
    ap_ip: '192.168.4.1',
    network_init: networkFaultStatus('portal_dns')
  };
  U.paintStatus();
  const text = element('setupChecklist').innerHTML;
  assert(text.includes('WLAN unter Einstellungen'));
  assert(text.includes('Wettbewerb und Verein'));
  assert(element('warnings').innerHTML.includes('NET E10'));
  assert.equal(U.S.status.ap_ip, '192.168.4.1');
});
test(
    'local service failure preserves loaded match and avoids stale-IP advice',
    () => {
      U.S.status = {
        ...U.S.status,
        network: 'connected',
        time_valid: true,
        last_update_ms: 5000,
        network_init: networkFaultStatus(),
        own_match: {
          present: true,
          competition: 23,
          home: 'FC',
          away: 'SV',
          phase: 'live',
          home_goals: 2,
          away_goals: 1,
          minute: 12
        }
      };
      U.paintStatus();
      assert(element('ownStatus').innerHTML.includes('2 : 1'));
      assert(element('networkWarning').innerHTML.includes('NET E11'));
      const failure = U.networkFailure(
          {...U.S.status, network: 'connecting', ap: false}, false);
      assert(!failure.action.includes('IP'));
      assert(!failure.message.includes('IP-Adresse'));
    });
test(
    'all image failure kinds give translated cause action code fallback and retry',
    () => {
      const status = U.S.status;
      U.S.status = {...status, uptime_s: 1};
      for (const [reason, code] of [
               ['missing', '01'], ['unavailable', '02'], ['invalid', '03'],
               ['timeout', '04'], ['source', '05'], ['decode', '06'],
               ['memory', '07']])
        for (const english of [false, true]) {
          const f = U.imageFailure(
              {
                ...U.S.status,
                image: {...imageFaultStatus(), reason, code: 'IMG E' + code}
              },
              english);
          for (const value
                   of ['IMG E' + code,
                       english ? 'Fallback active' : 'Ersatz aktiv',
                       ['unavailable', 'memory'].includes(reason) ?
                           english ? 'Restart the device' :
                                     'Gerät neu starten' :
                           english ? 'Under Images' :
                                     'Unter Bilder',
                       english ? 'Retry' : 'Neuer Versuch'])
            assert(f.message.includes(value), value);
          assert(f.message.includes(english ? 'Factory image' : 'Werksbild'));
        }
      assert.equal(
          U.imageFailure({image: {active: false, reason: 'missing'}}), null);
      assert.equal(U.imageFailure({image: {active: true, reason: ''}}), null);
      U.S.status = status;
    });
test('actual image fallback keeps match and both persistent warnings', () => {
  U.S.draft.hostname = 'unsaved';
  U.S.status = {
    ...U.S.status,
    screen: 0,
    screen_starting: false,
    network: 'connected',
    time_valid: true,
    last_update_ms: 5000,
    network_init: networkFaultStatus(),
    image: imageFaultStatus(),
    own_match: {
      present: true,
      competition: 23,
      home: 'FC',
      away: 'SV',
      phase: 'live',
      home_goals: 2,
      away_goals: 1,
      minute: 12
    }
  };
  for (const language of [0, 1]) {
    U.S.saved.language = U.S.draft.language = language;
    U.paintStatus();
    assert(element('ownStatus').innerHTML.includes('2 : 1'));
    for (const code of ['NET E11', 'IMG E03'])
      assert(element('warnings').innerHTML.includes(code));
    assert(element('diagnosticStatus').innerHTML.includes('IMG E03'));
    assert.equal(U.S.draft.hostname, 'unsaved');
  }
});
test(
    'all-image failure keeps football and actionable persistent repair advice',
    () => {
      U.S.status = {
        ...U.S.status,
        screen: 3,
        screen_starting: false,
        network: 'connected',
        time_valid: true,
        last_update_ms: 5000,
        image: imageFaultStatus(''),
        own_match: {
          present: true,
          competition: 23,
          home: 'FC',
          away: 'SV',
          phase: 'live',
          home_goals: 2,
          away_goals: 1,
          minute: 12
        }
      };
      for (const language of [0, 1]) {
        U.S.saved.language = U.S.draft.language = language;
        U.paintStatus();
        assert(element('ownStatus').innerHTML.includes('2 : 1'));
        const text = element('imageWarning').innerHTML;
        assert(text.includes('IMG E03'));
        assert(text.includes(language ? 'Under Images' : 'Unter Bilder'));
        assert(text.includes(language ? 'Retry' : 'Neuer Versuch'));
        assert(U.previewMarkup().includes(
            language ? 'screenshot' : 'Bildschirmfoto'));
      }
    });
test(
    'image recovery clears warnings and keeps desired slideshow positions',
    () => {
      U.S.saved.screen_policy.slides = [0, 2, 9, 2];
      U.S.images = {items: []};
      U.S.status = {
        ...U.S.status,
        screen: 3,
        screen_starting: false,
        network: 'connected',
        time_valid: true,
        last_update_ms: 5000,
        uptime_s: 60,
        image: imageFaultStatus('')
      };
      U.paintStatus();
      assert(element('warnings').innerHTML.includes('IMG E03'));
      U.S.status.image = {
        ...imageFaultStatus('library', ''),
        error: 0,
        error_name: 'ESP_OK',
        code: '',
        attempt: 35,
        retry_ms: 0
      };
      U.paintStatus();
      assert(!element('warnings').innerHTML.includes('IMG E03'));
      assert.equal(element('imageWarning').innerHTML, '');
      assert.deepEqual(
          Array.from(U.S.saved.screen_policy.slides), [0, 2, 9, 2]);
    });

test('names read errors stay actionable while pending or recovery do not claim failure', () => {
  for (const language of [0,1]) {
    U.S.saved.language=U.S.draft.language=language;
    U.S.status={...U.S.status,network:'connected',time_valid:true,
      names:{pending:false,error:263,code:'NAM E01'}};
    U.paintStatus();
    const warning=element('warnings').innerHTML;
    assert(warning.includes('NAM E01'));
    assert(warning.includes(language ? 'Under Competitions' : 'Unter Wettbewerbe'));
    U.S.status.names={pending:true,error:12,code:''}; U.paintStatus();
    assert(!element('warnings').innerHTML.includes('NAM E01'));
    U.S.status.names={pending:false,error:0,code:''}; U.paintStatus();
    assert(!element('warnings').innerHTML.includes('NAM E01'));
  }
});

test('general and screen fields share contextual help without repeating long prose', () => {
  const markup=U.settingsPage();
  const text='Höchstens so viele Abrufe je Tag;';
  assert(markup.includes('data-act="policyHelp"'));
  // Each provider retains its complete guidance on the help button, not as a
  // repeated visible paragraph. Both API boundaries remain schema-derived.
  assert(!/<div class="help">[^<]*Höchstens so viele Abrufe je Tag;/.test(markup));
  assert(markup.includes('data-help="'+text));
  assert(markup.includes('0–65535'));
  assert(markup.includes('data-path="providers[0].daily_requests"'));
});

function firmware() {
  const a = new Uint8Array(1024), v = new DataView(a.buffer),
        write = (o, s) => a.set(new TextEncoder().encode(s), o);
  a[0] = 0xe9;
  v.setUint32(32, 0xabcd5432, true);
  v.setUint32(288, 0x46424c31, true);
  v.setUint32(292, 2, true);
  write(48, 'test');
  write(296, 'xiao_esp32c6_gc9a01');
  write(344, '4mb_ota_littlefs_v1');
  v.setUint32(376, 1, true);
  a.fill(0x31, 176, 208);
  return a;
}
test(
    'OTA aggregate format descriptor target layout size and full ELF identity validated',
    () => {
      const a = firmware();
      assert.equal(
          U.decodeFirmware(
               a.buffer, U.S.status, U.S.schema, U.S.schema.ota_max, 100000)
              .id,
          '31'.repeat(32));
      a[296] = 88;
      assert.throws(
          () => U.decodeFirmware(
              a.buffer, U.S.status, U.S.schema, U.S.schema.ota_max, 100000));
      assert.throws(
          () => U.decodeFirmware(
              firmware().buffer, U.S.status, U.S.schema, 90000, 100000));
      const legacy = firmware();
      new DataView(legacy.buffer).setUint32(292, 1, true);
      new DataView(legacy.buffer).setUint32(376, 11, true);
      assert.throws(
          () => U.decodeFirmware(
              legacy.buffer, U.S.status, U.S.schema, U.S.schema.ota_max,
              100000));
    });
test(
    'replacement excludes old bytes from logical quota and consumes one physical workspace',
    () => {
      U.S.editor = {id: 1};
      U.S.images = {
        ...U.S.images,
        items: [{id: 1, bytes: 20000}],
        used: U.S.schema.image_quota - 680
      };
      U.S.status.fs_total = 100000;
      U.S.status.fs_used = 100000 - U.S.schema.image_reserve;
      assert.equal(U.imageAllowance(), 20680);
      U.S.status.fs_used++;
      assert.equal(U.imageAllowance(), 0);
    });
test(
    'missing gallery retains saved draft and disabled selections without thumbnails',
    () => {
      U.S.saved.screen_policy.slides = [9, 9];
      U.S.saved.screen_policy.appearance.single.image = 2;
      U.S.saved.screen_policy.appearance.single.background = false;
      U.S.saved.screen_policy.phases.own.single[0] = false;
      U.S.draft.screen_policy.slides = [7];
      U.S.draft.screen_policy.appearance.conference.image = 9;
      U.S.draft.screen_policy.appearance.conference.background = false;
      U.S.images = {
        ...U.S.images,
        items: [{id: 1, bytes: 1200, revision: 3}],
        used: 1200
      };
      assert.deepEqual(Array.from(U.libraryEntries(), x => x.id), [1, 2, 7, 9]);
      for (const language of [0, 1]) {
        U.S.draft.language = language;
        const html = U.imagesPage();
        assert.equal(
            (html.match(/data-act="imageEdit" data-id="9"/g) || []).length, 1);
        for (const id of [2, 7, 9])
          for (const action of ['imageEdit', 'imageDelete'])
            assert(html.includes(`data-act="${action}" data-id="${id}"`));
        assert.equal(
            (html.match(
                 new RegExp(language ? 'File missing' : 'Datei fehlt', 'g')) ||
             []).length,
            3);
        assert(html.includes('1 / 10'));
        assert(!html.includes('/api/v1/images/file?id=9'));
        assert(
            html.includes(language ? 'Background image' : 'Hintergrundbild'));
      }
    });
test(
    'missing selected background is retained while new choices offer existing files',
    () => {
      U.S.draft.screen_policy.appearance.single.image = 9;
      U.S.draft.screen_policy.appearance.single.background = false;
      U.S.draft.screen_policy.slides = [7];
      U.S.images = {...U.S.images, items: [{id: 1, bytes: 1200}], used: 1200};
      for (const language of [0, 1]) {
        U.S.draft.language = language;
        assert.deepEqual(
            Array.from(U.imageOptions('Standard-Hintergrund', 9), r => r[0]),
            [0, 1, 9]);
        U.S.screenAppearance = 'single';
        const html = U.screenAppearanceMarkup();
        assert(html.includes('value="9" selected'));
        assert(!html.includes('value="7"'));
        let choices = '';
        context.confirmAction = (_title, _message, _run, _word, extra) => {
          choices = extra;
        };
        U.ACT.roleCopy({dataset: {role: 'background'}});
        assert(
            choices.includes('value="1"') && !choices.includes('value="7"') &&
            !choices.includes('value="9"'));
      }
      assert.equal(U.S.draft.screen_policy.appearance.single.image, 9);
      assert.deepEqual(Array.from(U.S.draft.screen_policy.slides), [7]);
    });
test(
    'ten desired missing IDs reserve all slots while replace and release stay available',
    () => {
      U.S.saved.screen_policy.slides = [1, 2, 3, 4, 5];
      U.S.draft.screen_policy.slides = [6, 7, 8, 9, 10];
      for (const language of [0, 1]) {
        U.S.draft.language = language;
        const html = U.imagesPage();
        assert(/data-act="imageEdit" data-id="0" disabled/.test(html));
        assert(html.includes(
            language ? 'Free image slots: 0' : 'Freie Bildplätze: 0'));
        assert(html.includes('0 / 10'));
        assert(/data-act="imageEdit" data-id="1">/.test(html));
        assert(/data-act="imageDelete" data-id="10">/.test(html));
      }
    });
test(
    'free slots count actual files and selected IDs once across both models',
    () => {
      U.S.saved.screen_policy.slides = [1, 9];
      U.S.draft.screen_policy.slides = [2, 9];
      U.S.images = {
        ...U.S.images,
        items: [{id: 1, bytes: 100}, {id: 2, bytes: 100}, {id: 3, bytes: 100}],
        used: 300
      };
      const html = U.imagesPage();
      assert(html.includes('Freie Bildplätze: 6'));
      assert(html.includes('3 / 10 Dateien'));
      assert(!/data-act="imageEdit" data-id="0" disabled/.test(html));
    });
async function asyncTest(name, run) {
  setup();
  await run();
  passed++;
  console.log('PASS ' + name);
}
(async () => {
  await asyncTest(
      'password-free session opens silently before mutations', async () => {
        let posts = 0;
        h.state.hook = call => call.endpoint === 'session' ?
            h.response(
                200,
                call.method === 'POST' ? (posts++, {token: 'silent'}) :
                                         {password_set: false, valid: false}) :
            undefined;
        await U.authenticate();
        assert.equal(posts, 1);
        assert.equal(U.S.token, 'silent');
        assert(element('auth').hidden);
      });
  await asyncTest(
      'origin errors retain status and code without prompting login',
      async () => {
        h.state.hook = () =>
            h.response(403, {code: 'origin', error: 'controlled origin'});
        await assert.rejects(
            U.raw('config', {}, 'PUT'),
            e => e.status === 403 && e.code === 'origin');
      });
  await asyncTest(
      'status failure retains unsaved draft and recovery clears its load error',
      async () => {
        U.S.draft.hostname = 'unsaved';
        h.state.hook = call => {
          if (call.endpoint === 'status') throw Error('offline');
        };
        await U.load('status', false);
        assert.equal(U.S.draft.hostname, 'unsaved');
        assert(U.S.errors.status);
        h.state.hook = null;
        await U.load('status', false);
        assert(!U.S.errors.status);
        assert.equal(U.S.draft.hostname, 'unsaved');
      });
  await asyncTest(
      'image reconciliation uses GET only and clears solely explicitly deleted IDs',
      async () => {
        U.S.saved.screen_policy.slides = [2, 9];
        U.S.saved.screen_policy.appearance.single.image = 9;
        U.S.draft = structuredClone(U.S.saved);
        U.S.draft.hostname = 'unsaved';
        U.S.draft.screen_policy.appearance.conference.image = 2;
        context.readModel = async () => structuredClone(U.S.saved);
        await U.reconcileImages();
        assert.deepEqual(Array.from(U.S.draft.screen_policy.slides), [2, 9]);
        assert.equal(U.S.draft.screen_policy.appearance.single.image, 9);
        await U.reconcileImages([2]);
        assert.deepEqual(Array.from(U.S.draft.screen_policy.slides), [9]);
        assert.equal(U.S.draft.screen_policy.appearance.single.image, 9);
        assert.equal(U.S.draft.screen_policy.appearance.conference.image, 0);
        assert.equal(U.S.draft.hostname, 'unsaved');
        assert(h.calls.every(c => c.method === 'GET'));
      });
  await asyncTest(
      'new upload reserves latest current draft only and sends no settings',
      async () => {
        U.S.saved.screen_policy.slides = [1];
        U.S.draft.screen_policy.slides = [2, 2, 0];
        U.S.draft.screen_policy.appearance.single.image = 3;
        const sent = [];
        context.authenticatedUpload = async (path, file, progress, headers) => {
          sent.push({path, headers});
          progress(100);
        };
        let changed = false;
        h.state.hook = call => {
          if (call.endpoint === 'status' && !changed) {
            changed = true;
            U.S.draft.screen_policy.appearance.single.image = 5;
            U.S.draft.screen_policy.appearance.conference.image = 10;
          }
        };
        await U.openEditor(0);
        U.S.draft.screen_policy.slides = [4, 0, 4];
        U.ED.blob = new Blob([new Uint8Array(100)]);
        await U.uploadImage();
        assert.equal(sent.length, 1);
        assert.equal(sent[0].path, 'images/upload');
        assert.deepEqual({...sent[0].headers}, {'X-Image-Reserved': '1072'});
        assert.deepEqual(Array.from(U.S.saved.screen_policy.slides), [1]);
        assert.deepEqual(Array.from(U.S.draft.screen_policy.slides), [4, 0, 4]);
        assert.equal(U.S.draft.screen_policy.appearance.single.image, 5);
        assert.equal(h.state.writes, 0);
      });
  setup();
  U.S.status.competitions = [{id: U.S.saved.competitions[0].id, provider:'openligadb',cause:'',datasets:{matches:{state:'complete',cause:'',last_success_ms:3000},table:{state:'partial',cause:'auth',http:403,code:'OLG E08·403',last_success_ms:1000},details:{state:'partial',cause:'bad_response',code:'OLG E12',last_success_ms:0}}}];
  U.paintStatus();
  const datasetHtml = element('competitionStatus').innerHTML;
  assert(datasetHtml.includes('Tabelle') && datasetHtml.includes('Spieldetails'));
  assert(datasetHtml.includes('OLG E08·403') && datasetHtml.includes('OLG E12'));
  assert(datasetHtml.includes('Letzter erfolgreicher Abruf') && datasetHtml.includes('Gestört'));
  passed++; console.log('PASS distinct table/details failures remain visible beside successful fixtures');
  console.log(
      passed +
      ' current UI/diagnostic/image cases passed; no layout/device claim');
})().catch(error => {
  console.error(error);
  process.exitCode = 1
});
