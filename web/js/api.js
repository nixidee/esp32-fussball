// HTTP/session admission, binary uploads and device snapshot loading.
class ApiError extends Error {
    constructor(status, data) {
        const text = ERROR[data?.code], stage = data?.code === 'reset_partial' ? {
            settings: 'Einstellungsspeicher', counters: 'Abrufzähler', format: 'Formatierung', images: 'Eigene Bilder', adopted_names: 'Vereinsnamen'
        }[data.stage] : '', position = Number.isSafeInteger(data?.json_offset) && data.json_offset >= 0 ? ' · ' + tf('JSON-Byteposition: %', data.json_offset) : '';
        super((text ? t(text) : t('Aktion fehlgeschlagen. Seite neu laden und erneut versuchen.')) + (stage ? ' · ' + t(stage) : '') + (!text || status >= 500 ? codeText([data?.code, 'HTTP ' + status].filter(Boolean).join(' · ')) : '') + position);
        this.status = status;
        this.data = data || {};
        this.field = data?.field;
        this.code = data?.code;
    }
}
async function raw(path, body, method = 'GET', base = '', binary = false, options = {}) {
    const controller = new AbortController(), timeout = setTimeout(() => controller.abort(), options.timeoutMs || 15000);
    const cancelled = () => controller.abort();
    if (options.signal?.aborted) cancelled();
    options.signal?.addEventListener('abort', cancelled, {once: true});
    try {
        const headers = {};
        if (S.token && !base)
            headers['X-CSRF-Token'] = S.token;
        if (body !== undefined)
            headers['Content-Type'] = 'application/json';
        const res = await fetch(base + '/api/v1/' + path, {
            method, headers, body: body === undefined ? undefined : JSON.stringify(body), signal: controller.signal, cache: 'no-store'
        });
        if (res.ok && binary)
            return await res.arrayBuffer();
        let data;
        try {
            data = await res.json();
        }
        catch {
            throw Error(t('Antwort des Geräts ist nicht lesbar.'));
        }
        if (!res.ok)
            throw new ApiError(res.status, data);
        return data;
    }
    catch (e) {
        if (e instanceof ApiError || e.message === t('Antwort des Geräts ist nicht lesbar.'))
            throw e;
        const failure = Error(t('Verbindung fehlgeschlagen.'));
        if (binary)
            failure.code = e.name === 'AbortError' ? 'request_timeout' : 'connection';
        throw failure;
    }
    finally {
        clearTimeout(timeout);
        options.signal?.removeEventListener('abort', cancelled);
    }
}
let authWait = null;
async function authenticate() {
    S.session = await raw('session');
    if (!S.session.password_set && !S.session.valid) {
        const result = await raw('session', {}, 'POST');
        S.token = result.token;
        S.session = {
            password_set: false, valid: true
        };
    }
    renderHeader();
}
function login() {
    if (authWait)
        return authWait;
    authWait = new Promise((resolve, reject) => {
        const d = dialog(t('Anmelden'), `<p>${ht('Zum Ändern anmelden')}</p><form id="loginForm"><div class="field"><label for="loginPassword">${ht('Passwort')}</label><div class="ctl"><input type="password" id="loginPassword" autocomplete="current-password" class="w-l" required></div></div><p id="loginError" class="dlg-err"></p><button class="btn primary" style="margin-top:14px">${ht('Anmelden')}</button></form>`);
        let ok = false;
        d.onclose = () => {
            d.onclose = null;
            authWait = null;
            if (ok)
                resolve();
            else
                reject(Error(t('Anmeldung abgebrochen.')));
        };
        $('loginForm').onsubmit = async (e) => {
            e.preventDefault();
            const btn = e.target.querySelector('button');
            btn.disabled = true;
            try {
                const result = await raw('session', {
                    password: $('loginPassword').value
                }, 'POST');
                S.token = result.token;
                await authenticate();
                ok = true;
                d.close();
            }
            catch (err) {
                $('loginError').textContent = err.message;
                btn.disabled = false;
            }
        };
        $('loginPassword').focus();
    });
    return authWait;
}
async function mutate(path, body = {}, method = 'POST', binary = false, options = {}) {
    if (!S.session?.valid) {
        await authenticate();
        if (S.session.password_set && !S.session.valid)
            await login();
    }
    try {
        return await raw(path, body, method, '', binary, options);
    }
    catch (e) {
        if (e.status === 401 && e.code === 'session') {
            S.token = '';
            await authenticate();
            if (S.session.password_set)
                await login();
            return raw(path, body, method, '', binary, options);
        }
        throw e;
    }
}
async function load(key, rerender = true) {
    if (S.loading[key])
        return;
    S.loading[key] = true;
    try {
        if (key === 'session')
            await authenticate();
        else {
            const result = key === 'settings' ? await readModel() : await raw(key);
            if (key === 'schema')
                S.schema = result;
            else if (key === 'settings') {
                if (!S.saved)
                    fresh(result);
            }
            else
                S[key] = result;
        }
        delete S.errors[key];
        document.querySelector(`[data-load="${key}"]`)?.remove();
        if (rerender)
            render();
        else if (key === 'status')
            paintStatus();
    }
    catch (e) {
        S.errors[key] = e.message;
        if (rerender)
            render();
        else
            renderHeader();
    }
    finally {
        S.loading[key] = false;
    }
}
// Busy means the shared operation slot is temporarily occupied. Only these
// read-only snapshots may retry; a successful write is never replayed here.
async function settingsRead(path) {
    for (let retry = 0;; retry++) {
        try {
            return await raw(path);
        }
        catch (error) {
            if (error.code !== 'busy' || retry === 2)
                throw error;
            await sleep(250 * (retry + 1));
        }
    }
}
function upload(path, file, progress, headers = {}) {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/v1/' + path);
        xhr.timeout = 180000;
        xhr.setRequestHeader('X-CSRF-Token', S.token);
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');
        for (const [k, v] of Object.entries(headers))
            xhr.setRequestHeader(k, v);
        xhr.upload.onprogress = e => {
            if (e.lengthComputable)
                progress(Math.round(e.loaded / e.total * 100));
        };
        xhr.onload = () => {
            let result;
            try {
                result = JSON.parse(xhr.responseText);
            }
            catch {
                reject(Error(t('Antwort des Geräts ist nicht lesbar.')));
                return;
            }
            if (xhr.status >= 200 && xhr.status < 300)
                resolve(result);
            else
                reject(new ApiError(xhr.status, result));
        };
        xhr.onerror = () => reject(Error(t('Verbindung fehlgeschlagen.')));
        xhr.ontimeout = () => reject(Error(t('Zeitüberschreitung. Erneut versuchen.')));
        xhr.send(file);
    });
}
async function authenticatedUpload(path, file, progress, headers = {}) {
    if (!S.session?.valid) {
        await authenticate();
        if (S.session.password_set && !S.session.valid)
            await login();
    }
    try {
        return await upload(path, file, progress, headers);
    }
    catch (e) {
        if (e.status === 401 && e.code === 'session') {
            S.token = '';
            await authenticate();
            if (S.session.password_set)
                await login();
            return upload(path, file, progress, headers);
        }
        throw e;
    }
}

// SSIDs are up to 32 arbitrary bytes. JSON strings cover valid UTF-8; the
// explicit base64 object is also editable when bytes have no text spelling.
function ssidBase64(bytes) {
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    let out = '';
    for (let i = 0; i < bytes.length; i += 3) {
        const a = bytes[i], b = bytes[i + 1] || 0, c = bytes[i + 2] || 0;
        out += alphabet[a >> 2] + alphabet[(a & 3) << 4 | b >> 4];
        out += i + 1 < bytes.length ? alphabet[(b & 15) << 2 | c >> 6] : '=';
        out += i + 2 < bytes.length ? alphabet[c & 63] : '=';
    }
    return out;
}
function ssidBytes(value) {
    if (typeof value === 'string') {
        const raw = new TextEncoder().encode(value);
        return new TextDecoder('utf-8', {fatal: true, ignoreBOM: true}).decode(raw) === value ? raw : null;
    }
    if (!value || typeof value !== 'object' || Array.isArray(value) ||
        Object.keys(value).length !== 1 || typeof value.base64 !== 'string')
        return null;
    const text = value.base64;
    if (text.length > 44 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(text))
        return null;
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    const bytes = [];
    for (let i = 0; i < text.length; i += 4) {
        const a = alphabet.indexOf(text[i]), b = alphabet.indexOf(text[i + 1]);
        const c = alphabet.indexOf(text[i + 2]), d = alphabet.indexOf(text[i + 3]);
        bytes.push(a << 2 | b >> 4);
        if (c >= 0)
            bytes.push((b & 15) << 4 | c >> 2);
        if (d >= 0)
            bytes.push((c & 3) << 6 | d);
    }
    const decoded = Uint8Array.from(bytes);
    return ssidBase64(decoded) === text ? decoded : null;
}
function ssidValue(base64) {
    const value = {base64}, raw = ssidBytes(value);
    if (!raw)
        return value;
    try {
        return new TextDecoder('utf-8', {fatal: true, ignoreBOM: true}).decode(raw);
    }
    catch {
        return value;
    }
}
function ssidEditor(value) {
    if (value && typeof value === 'object' && typeof value.base64 === 'string')
        return {base64: true, value: value.base64};
    // HTML text input normalises line endings and cannot retain NUL. Derive
    // their editable representation directly from the model, with no shadow.
    if (typeof value === 'string' && /[\x00-\x1f\x7f]/.test(value))
        return {base64: true, value: ssidBase64(new TextEncoder().encode(value))};
    return {base64: false, value};
}
