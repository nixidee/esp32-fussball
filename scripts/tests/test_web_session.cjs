'use strict';
// Full current production JS; controlled Fetch/XHR and login UI boundary.
// An explicit expired-session rejection permits one replay, ambiguous writes
// never do. The same assertions cover JSON, screenshot binary and image upload.
const assert = require('node:assert/strict');
const {harness} = require('./web_config_harness.cjs');
const h = harness(), {U, context, response} = h;
let state, passed = 0;
function reset(options = {}) {
    state = {operations: [], sessions: [], logins: 0, replies: [], ...options};
    U.S.session = {valid: true, password_set: false};
    U.S.token = 'initial';
    context.login = async () => {
        ++state.logins;
        if (state.cancel) throw Error('login cancelled');
        U.S.token = 'password';
        U.S.session = {valid: true, password_set: true};
    };
}
function operation(call) {
    state.operations.push(call);
    const reply = state.replies.shift() || {status: 200, data: {ok: true}};
    return reply;
}
context.fetch = async (url, options) => {
    if (url.endsWith('/session')) {
        state.sessions.push(options.method);
        return options.method === 'GET' ?
            response(200, {valid: false, password_set: !!state.password}) :
            response(200, {token: 'renewed'});
    }
    const reply = operation({url, options, token: options.headers['X-CSRF-Token']});
    if (reply.failure) throw Object.assign(Error('controlled lost connection'), {name: reply.failure});
    if (reply.status === 200 && state.binary)
        return {ok: true, status: 200, arrayBuffer: async () => new ArrayBuffer(4)};
    return response(reply.status, reply.data);
};
context.XMLHttpRequest = class {
    constructor() { this.headers = {}; this.upload = {}; }
    open(method, url) { this.method = method; this.url = url; }
    setRequestHeader(key, value) { this.headers[key] = value; }
    send(file) {
        const reply = operation({url: this.url, file, token: this.headers['X-CSRF-Token'], headers: this.headers});
        queueMicrotask(() => {
            if (reply.failure) {
                (reply.failure === 'AbortError' ? this.ontimeout : this.onerror)();
                return;
            }
            this.status = reply.status;
            this.responseText = JSON.stringify(reply.data);
            this.onload();
        });
    }
};
async function run() {
    for (const mode of ['json', 'binary', 'upload']) {
        const body = {expected_revision: 7}, file = new Blob(['image content']);
        const request = () => mode === 'upload' ?
            U.authenticatedUpload('images/upload', file, () => {}, {'X-Image-Id': '3'}) :
            U.mutate('protected', body, 'PUT', mode === 'binary');
        function verifyOperation(count) {
            assert.equal(state.operations.length, count);
            for (const call of state.operations) {
                if (mode === 'upload') {
                    assert.equal(call.file, file);
                    assert.equal(call.headers['Content-Type'], 'application/octet-stream');
                    assert.equal(call.headers['X-Image-Id'], '3');
                } else {
                    assert.equal(call.options.body, JSON.stringify(body));
                    assert.equal(call.options.method, 'PUT');
                }
            }
        }
        reset({binary: mode === 'binary'});
        await request(); verifyOperation(1);
        assert.equal(state.sessions.length, 0); ++passed;

        reset({binary: mode === 'binary'}); U.S.session.valid = false;
        await request(); verifyOperation(1);
        assert.deepEqual(state.sessions, ['GET', 'POST']);
        assert.equal(state.operations[0].token, 'renewed'); ++passed;

        reset({binary: mode === 'binary', replies: [{status: 401, data: {code: 'session'}}]});
        await request(); verifyOperation(2);
        assert.deepEqual(state.sessions, ['GET', 'POST']);
        assert.equal(state.operations[1].token, 'renewed'); ++passed;

        reset({replies: Array.from({length: 2}, () => ({status: 401, data: {code: 'session'}}))});
        await assert.rejects(request(), error => error.code === 'session');
        verifyOperation(2); assert.deepEqual(state.sessions, ['GET', 'POST']); ++passed;

        reset({binary: mode === 'binary', password: true}); U.S.session.valid = false;
        await request(); verifyOperation(1);
        assert.deepEqual(state.sessions, ['GET']); assert.equal(state.logins, 1);
        assert.equal(state.operations[0].token, 'password'); ++passed;

        reset({password: true, cancel: true}); U.S.session.valid = false;
        await assert.rejects(request(), /login cancelled/);
        verifyOperation(0); assert.equal(state.logins, 1); ++passed;

        for (const [status, code] of [[401, 'password'], [403, 'origin'], [409, 'busy'], [507, 'memory']]) {
            reset({replies: [{status, data: {code}}]});
            await assert.rejects(request(), error => error.status === status && error.code === code);
            verifyOperation(1); assert.equal(state.sessions.length, 0); ++passed;
        }
        for (const failure of ['AbortError', 'Error']) {
            reset({replies: [{failure}]});
            await assert.rejects(request());
            verifyOperation(1); assert.equal(state.sessions.length, 0); ++passed;
        }
        console.log('PASS ' + mode + ': session admission/one renewal/password/cancel/exact transport/no ambiguous replay');
    }
    console.log(passed + ' current transport/session cases passed; actual browser/device acceptance separate');
}
run().catch(error => { console.error(error); process.exitCode = 1; });
