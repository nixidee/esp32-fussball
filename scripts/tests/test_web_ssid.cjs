'use strict';
// Production draft/editor functions with controlled DOM/transport boundaries.
// This proves byte retention and request payloads, not browser layout.
const assert = require('node:assert/strict');
const vm = require('node:vm');
const {readWebSource} = require('./web_source.cjs');
const {script, boundary} = readWebSource();
const events = {}, elements = new Map();
const element = id => {
    if (!elements.has(id)) elements.set(id, {
        id, innerHTML: '', textContent: '', dataset: {}, style: {},
        classList: {toggle() {}}, querySelectorAll() {return [];},
        querySelector() {return null;}, setAttribute() {},
    });
    return elements.get(id);
};
const context = {
    console, TextEncoder, TextDecoder, Uint8Array, Uint8ClampedArray,
    DataView, ArrayBuffer, Map, Set, Date, Promise, Math, JSON, Number,
    String, Object, Error, Array, RegExp, AbortController, Blob,
    setTimeout, clearTimeout,
    document: {getElementById: element, querySelectorAll() {return [];},
        querySelector() {return null;}, documentElement: {dataset: {}},
        addEventListener(name, fn) {events[name] = fn;}},
    localStorage: {getItem() {return 'auto';}, setItem() {}},
    location: {host: 'example.local', hostname: 'example.local', protocol: 'http:'},
    window: {addEventListener() {}},
};
vm.createContext(context);
vm.runInContext(script.slice(0, boundary) + `
    globalThis.UI = {S, W, F, ssidBase64, ssidBytes, ssidEditor,
        payload, validate, validateImport, changes, ssidValue};
    paintErrors = renderNav = renderBar = () => {};
`, context);
const U = context.UI, copy = x => JSON.parse(JSON.stringify(x));
const limits = {daily_requests: [0, 1000], window_requests: [0, 1000], window_s: [0, 3600]};
U.S.schema = {
    bounds: limits, text: {wifi_ssid: 32}, inputs: 0,
    providers: [], data_types: [], limits: {}, time_zones: ['UTC'],
    defaults: {wifi_ssid: '', language: 0},
};
const initial = {
    wifi_ssid: '', hostname: 'football', ntp_server: 'pool.ntp.org',
    language: 0, time_zone: 'UTC', providers: [], competitions: [],
    global_limit: {daily_requests: 0, window_requests: 0, window_s: 0},
};
function reset(value) {
    U.S.saved = {...copy(initial), wifi_ssid: copy(value)};
    U.S.draft = copy(U.S.saved);
    U.S.sec = {};
    U.S.saving = false;
}
let passed = 0;
function test(name, fn) {fn(); ++passed; console.log('PASS', name);}

test('every raw byte has canonical editable base64 with an unchanged payload', () => {
    for (let byte = 0; byte <= 255; ++byte) {
        const bytes = Uint8Array.from({length: 32}, () => byte);
        const base64 = Buffer.from(bytes).toString('base64');
        assert.equal(U.ssidBase64(bytes), base64);
        assert.deepEqual(Array.from(U.ssidBytes({base64})), Array.from(bytes));
        reset({base64});
        assert.deepEqual(copy(U.payload().wifi_ssid), {base64});
        assert.equal(U.validate(U.S.draft).length, 0);
        assert.equal(U.changes().length, 0);
    }
});
test('ordinary UTF-8 remains text and its encoded byte limit applies', () => {
    reset('Fußball ⚽');
    assert.deepEqual(copy(U.ssidEditor(U.S.draft.wifi_ssid)), {base64: false, value: 'Fußball ⚽'});
    assert(!U.F('wifi_ssid').includes('ssid-base64'));
    assert.equal(U.validate(U.S.draft).length, 0);
    U.S.draft.wifi_ssid = 'ä'.repeat(17);
    assert(U.validate(U.S.draft).some(x => x.path === 'wifi_ssid'));
});
test('NUL and line endings are derived losslessly without changing the draft', () => {
    for (const value of ['A\0B', 'A\r\nB']) {
        reset(value);
        const expected = Buffer.from(value).toString('base64');
        assert.deepEqual(copy(U.ssidEditor(value)), {base64: true, value: expected});
        const markup = U.F('wifi_ssid');
        assert(markup.includes('data-kind="ssid-base64"'));
        assert(markup.includes('value="' + expected + '"'));
        assert(markup.includes('(Base64)'));
        assert(!markup.includes('\0'));
        assert.equal(U.S.draft.wifi_ssid, value);
        assert.equal(U.changes().length, 0);
    }
});
test('the real input listener edits the one draft and sends exactly its bytes', () => {
    reset({base64: '/w=='});
    events.input({target: {dataset: {path: 'wifi_ssid', kind: 'ssid-base64'}, value: 'AP8='}});
    assert.deepEqual(copy(U.S.draft.wifi_ssid), {base64: 'AP8='});
    assert.deepEqual(Array.from(U.ssidBytes(U.payload().wifi_ssid)), [0, 255]);
    assert(U.changes().some(x => x.path.startsWith('wifi_ssid')));
    assert.equal(U.S.saved.wifi_ssid.base64, '/w==');
});
test('invalid padding hidden fields and oversize SSIDs fail before a save', () => {
    for (const value of [{base64: '/x=='}, {base64: 'a'}, {base64: 'AA==\n'},
        {base64: 'AA==', extra: true}, {base64: Buffer.alloc(33).toString('base64')}]) {
        reset(value);
        assert(U.validate(U.S.draft).some(x => x.path === 'wifi_ssid'));
        assert.throws(() => U.validateImport({wifi_ssid: value}));
    }
});
test('valid text entered as base64 uses the same wire representation as readback', () => {
    for (const value of ['plain', 'A\0B', '\ufeffleading BOM']) {
        reset({base64: '/w=='});
        const base64 = Buffer.from(value).toString('base64');
        events.input({target: {dataset: {path: 'wifi_ssid', kind: 'ssid-base64'}, value: base64}});
        assert.equal(U.S.draft.wifi_ssid, value);
        assert.equal(U.payload().wifi_ssid, value);
        assert.equal(U.validateImport({wifi_ssid: {base64}}).wifi_ssid, value);
    }
    assert.equal(U.ssidBytes('\ud800'), null);
});
test('SSID help has an English translation', () => {
    reset({base64: '/w=='});
    U.S.draft.language = 1;
    assert(U.F('wifi_ssid').includes('32 original bytes'));
    assert(U.W['Ungültiger Base64-WLAN-Name']);
});
console.log(passed + ' lossless SSID client cases passed');
