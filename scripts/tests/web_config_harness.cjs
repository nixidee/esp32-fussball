'use strict';
// Shared controlled HTTP/DOM around the full current production script and
// actual C++ schema/config writers. This is neither a browser nor device API.
const fs = require('node:fs'), path = require('node:path'),
      vm = require('node:vm');
const assert = require('node:assert/strict'),
      {execFileSync} = require('node:child_process');
const {readWebSource, autonomousBoundary, featureProbes} = require('./web_source.cjs');
const root = path.resolve(__dirname, '../..'),
      copy = value =>
          value === undefined ? undefined : JSON.parse(JSON.stringify(value));
let generated;
function fixture(filename) {
  const value = filename ?
      JSON.parse(fs.readFileSync(filename, 'utf8')) :
      (generated ||= JSON.parse(execFileSync(
           'python3', [path.join(__dirname, 'web_config_fixture.py')],
           {encoding: 'utf8'})));
  assert(
      value.schema && value.config?.desired,
      'current C++ schema/config fixture required');
  return copy(value);
}
function harness(filename) {
  const data = fixture(filename), elements = new Map(), events = {}, calls = [],
        delays = [];
  function element(id) {
    if (!elements.has(id)) {
      let markup = '', text = '';
      elements.set(id, {
        id,
        get innerHTML() {
          return markup
        },
        set innerHTML(value) {
          markup = String(value);
          text = markup.replace(/<[^>]*>/g, '')
        },
        get textContent() {
          return text
        },
        set textContent(value) {
          text = String(value);
          markup = text
        },
        value: '',
        dataset: {},
        style: {},
        attributes: {},
        hidden: false,
        disabled: false,
        checked: false,
        classList: {toggle() {}},
        querySelector(selector) {
          return element(id + ' ' + selector)
        },
        querySelectorAll() {
          return []
        },
        remove() {},
        firstElementChild: {style: {}},
        close() {
          this.open = false
        },
        showModal() {
          this.open = true
        },
        focus() {},
        setAttribute(k, v) {
          this.attributes[k] = v
        },
        isConnected: true
      });
    }
    return elements.get(id);
  }
  const response = (status, body) => ({
    ok: status >= 200 && status < 300,
    status,
    json: async () => copy(body)
  });
  const state = {
    config: copy(data.config),
    mode: 'ok',
    writes: 0,
    reads: 0,
    nextIds: [47, 9, 199, 61, 12, 244, 3, 88],
    incarnation: 901,
    names: {
      format: data.schema.names_format,
      version: data.schema.names_source_version,
      clubs: [],
      exceptions: []
    },
    status: {
      version: 'test',
      target: 'xiao_esp32c6_gc9a01',
      layout: '4mb_ota_littlefs_v1',
      firmware_id: 'a'.repeat(64),
      ip: '192.0.2.1',
      ap_ip: '192.168.4.1',
      network: 'connected',
      ap: false,
      time_valid: true,
      uptime_s: 60,
      last_update_ms: 0,
      screen: 0,
      screen_reason: 'idle',
      provider_busy: false,
      provider_started: true,
      own_match: {present: false},
      settings_source: 'stored',
      fs_used: 100000,
      fs_total: 458752
    },
    images: {
      items: [],
      roles: [],
      used: 0,
      quota: data.schema.image_quota,
      max: data.schema.limits.images
    },
    hook: null
  };
  async function fetchApi(url, options = {}) {
    const endpoint = String(url).split('/api/v1/')[1],
          method = options.method || 'GET',
          body =
              options.body === undefined ? undefined : JSON.parse(options.body);
    const call = {endpoint, method, body};
    calls.push(call);
    if (state.hook) {
      const value = await state.hook(call);
      if (value !== undefined) return value;
    }
    const reject = (code, status = 409) => {
      call.code = code;
      call.status = status;
      return response(status, {code, error: 'controlled ' + code});
    };
    if (method === 'GET') {
      if (endpoint === 'config') {
        state.reads++;
        if (state.mode === 'busy-read' ||
            state.mode === 'busy-once' && state.reads === 1)
          return reject('busy');
        if (state.mode === 'memory-read') return reject('memory', 507);
        return response(200, state.config);
      }
      if (endpoint === 'schema') return response(200, data.schema);
      if (endpoint === 'session')
        return response(200, {valid: true, password_set: false});
      if (endpoint === 'status') return response(200, state.status);
      if (endpoint === 'images') return response(200, state.images);
      if (endpoint === 'team-names')
        return response(
            200,
            {adopted_names: state.names, names_ref: state.config.names_ref});
      throw Error('retired/unhandled GET ' + endpoint);
    }
    if (endpoint === 'session') return response(200, {token: 'controlled'});
    if (endpoint === 'export')
      return response(200, {
        version: state.config.version,
        desired: state.config.desired,
        adopted_names: state.names
      });
    if (endpoint === 'logout') return response(200, {ok: true});
    assert.equal(
        endpoint, 'config', 'only current Common Apply may persist config');
    assert.equal(method, 'PUT');
    state.writes++;
    assert(
        !JSON.stringify(body).includes('_screenKey'),
        'browser-only identity leaked');
    assert(
        Buffer.byteLength(JSON.stringify(body)) <= data.schema.max_apply_body);
    if (state.mode === 'reject') return reject('settings_changed');
    if (state.mode === 'unknown') return reject('outcome_unknown');
    if (body.expected_revision !== state.config.revision)
      return reject('settings_changed');
    const previous = state.config.desired.settings,
          desired = copy(body.desired);
    for (const marker of Object.keys(previous).filter(
             key => key.endsWith('_set'))) {
      const secret = marker.slice(0, -4);
      desired.settings[marker] = secret in desired.settings ?
          desired.settings[secret] !== '' :
          previous[marker];
      delete desired.settings[secret];
    }
    state.config = {
      ...state.config,
      revision: state.config.revision + 1,
      apply_id: body.apply_id,
      desired
    };
    state.config.desired.competitions.forEach(c => {
      if (!c.id) {
        c.id = state.nextIds.shift();
        c.incarnation = state.incarnation++;
      }
    });
    if (body.adopted_names) state.names = copy(body.adopted_names);
    if (state.mode === 'lost') throw Error('controlled lost acknowledgement');
    if (state.mode === 'bad-receipt') return response(200, {ok: true});
    if (state.mode === 'busy-after') state.mode = 'busy-read';
    return response(200, state.config);
  }
  const context = {
    console,
    TextEncoder,
    TextDecoder,
    Blob,
    AbortController,
    Uint8Array,
    DataView,
    ArrayBuffer,
    Map,
    Set,
    Date,
    Promise,
    Math,
    JSON,
    Number,
    String,
    Object,
    Error,
    Array,
    RegExp,
    structuredClone,
    crypto: require('node:crypto').webcrypto,
    scrollY: 0,
    scrollTo() {},
    setTimeout(fn, ms) {
      delays.push(ms);
      const timer = setTimeout(fn, ms < 1000 ? 0 : ms);
      if (ms >= 1000) timer.unref();
      return timer
    },
    clearTimeout,
    document: {
      getElementById: element,
      querySelectorAll() {
        return []
      },
      querySelector() {
        return null
      },
      addEventListener(name, fn) {
        events[name] = fn
      },
      documentElement: {dataset: {}},
      createElement(type) {
        const e = element('new-' + type);
        e.click = () => {};
        return e;
      }
    },
    URL: {
      createObjectURL(blob) {
        state.blob = blob;
        return 'blob:controlled'
      },
      revokeObjectURL() {}
    },
    localStorage: {
      getItem() {
        return 'auto'
      },
      setItem() {}
    },
    location: {
      host: 'fussball.local',
      hostname: 'fussball.local',
      protocol: 'http:',
      assign() {}
    },
    window: {addEventListener() {}},
    fetch: fetchApi,
    confirm: () => true
  };
  const probed = featureProbes(readWebSource().script), script = probed.script,
        boundary = autonomousBoundary(script), names = probed.publicNames;
  vm.createContext(context);
  vm.runInContext(
      script.slice(0, boundary) +
          `\nglobalThis.UI={...globalThis.__featureTests,S,W,CAUSE,ACT,ApiError,get,set,${
              names.join(',')}};render=()=>renderBar();paintErrors=()=>{};`,
      context);
  const U = context.UI;
  Object.assign(U.S, {
    schema: copy(data.schema),
    status: copy(state.status),
    images: copy(state.images),
    session: {valid: true, password_set: false},
    token: 'controlled',
    page: 'display',
    saving: false
  });
  U.fresh(U.modelFromConfig(copy(state.config)));
  async function click(action = 'save') {
    const button = {
      dataset: {act: action},
      disabled: false,
      isConnected: true,
      closest(selector) {
        return selector === '[data-act]' ? this : null
      }
    };
    await events.click({target: button, preventDefault() {}});
  }
  return {
    U,
    context,
    data,
    state,
    calls,
    delays,
    events,
    element,
    copy,
    response,
    click,
    model: () => U.modelFromConfig(copy(data.config)),
    entry: (id = 0, incarnation = 0) =>
        ({...copy(data.config.desired.competitions[0]), id, incarnation})
  };
}
module.exports = {
  harness,
  fixture,
  copy
};
