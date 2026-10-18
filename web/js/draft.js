// The single settings draft, validation, save receipts and baseline reconciliation.
function secretMode(key) {
    return S.sec[key] || {
        mode: 'keep', value: ''
    };
}
function payload(m = S.draft, sec = S.sec) {
    const out = clone(m);
    delete out.screen_policy;
    for (const key of SECRETS) {
        delete out[key];
        const state = sec[key];
        if (state?.mode === 'clear')
            out[key] = '';
        else if (state?.mode === 'change' && state.value !== '')
            out[key] = state.value;
        delete out[key + '_set'];
    }
    return out;
}
function fresh(model) {
    prepareScreenModel(model);
    S.saveNotice = null;
    S.saveProgress = null;
    S.pendingNames = null;
    S.pendingApply = null;
    S.saved = clone(model);
    S.draft = clone(model);
    S.sec = {};
    S.tried = false;
    S.server = null;
    S.expanded = false;
    S.open = null;
}
function changes() {
    if (!S.saved)
        return [];
    const rows = [];
    function walk(a, b, p) {
        if (p.split('.').some(key => key.startsWith('_')) || eq(a, b))
            return;
        if ((p === 'input_roles' || LIMITS.includes(p) || p.startsWith('screen_policy.')) && Array.isArray(a) && Array.isArray(b) && a.length === b.length) {
            a.forEach((v, i) => walk(v, b[i], `${p}[${i}]`));
            return;
        }
        if (a && b && typeof a === 'object' && typeof b === 'object' && !Array.isArray(a) && !Array.isArray(b)) {
            for (const k of new Set([...Object.keys(a), ...Object.keys(b)]))
                walk(a[k], b[k], p ? p + '.' + k : k);
        }
        else
            rows.push({
                path: p, old: a, value: b
            });
    }
    walk(S.saved, S.draft, '');
    for (const key of SECRETS) {
        const s = secretMode(key);
        if (s.mode === 'clear' || s.mode === 'change' && s.value !== '')
            rows.push({
                path: key, secret: s.mode
            });
    }
    if (S.pendingNames)
        rows.push({
            path: 'adopted_names', old: null, value: S.pendingNames
        });
    return rows;
}
function validId(v) {
    return !/[\x00-\x1f\x7f]|^ | $/.test(v);
}
// provider ids: no control characters, no space at either end
function validate(m, sec = S.sec) {
    const issues = [], add = (path, key) => issues.push({
        path, message: t(key)
    }), text = (path, limit) => {
        const v = get(m, path);
        if (path === 'wifi_ssid') {
            const raw = ssidBytes(v);
            if (!raw)
                add(path, 'Ungültiger Base64-WLAN-Name');
            else if (raw.length > limit)
                add(path, 'Text ist zu lang');
            return !!raw && raw.length <= limit;
        }
        if (typeof v !== 'string') {
            add(path, 'Falscher Datentyp');
            return false;
        }
        if (bytes(v) > limit) {
            add(path, 'Text ist zu lang');
            return false;
        }
        return true;
    }, range = (path, a) => {
        const n = get(m, path);
        if (n === '') {
            add(path, 'Wert fehlt');
            return;
        }
        if (typeof n !== 'number' || !Number.isInteger(n))
            add(path, 'Falscher Datentyp');
        else if (n < a[0] || n > a[1])
            add(path, 'Wert außerhalb des erlaubten Bereichs');
    }, id = v => typeof v === 'string' && validId(v), dns = v => typeof v === 'string' && v.length > 0 && v.split('.').every(x => x.length <= 63 && /^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$/.test(x));
    if (!m || !S.schema)
        return issues;
    const sc = S.schema;
    for (const [p, a] of Object.entries(sc.bounds)) {
        if (LIMIT_KEYS.includes(p) || p === 'interval_s')
            continue;
        const v = get(m, p);
        if (Array.isArray(v))
            v.forEach((_, i) => range(`${p}[${i}]`, a));
        else
            range(p, a);
    }
    range('language', [0, 1]);
    if (Number.isInteger(sc.inputs) && Array.isArray(m.input_roles)) {
        m.input_roles.forEach((_, i) => range(`input_roles[${i}]`, [0, ROLES.length - 1]));
        if (sc.inputs > 0 && m.input_roles.slice(0, sc.inputs).filter(r => r === 1).length !== 1)
            add('input_roles', 'Genau eine eingeschaltete Taste muss die Pflichttaste sein.');
    }
    for (const [p, n] of Object.entries(sc.text)) {
        if (typeof n === 'number' && !SECRETS.includes(p))
            text(p, n);
    }
    const limit = (l, p) => {
        if (!l || typeof l !== 'object' || Array.isArray(l)) {
            add(p, 'Falscher Datentyp');
            return false;
        }
        for (const k of LIMIT_KEYS)
            range(`${p}.${k}`, sc.bounds[k]);
        if (Number.isInteger(l.window_requests) && Number.isInteger(l.window_s) && !l.window_requests !== !l.window_s)
            add(`${p}.${l.window_requests ? 'window_s' : 'window_requests'}`, 'Werte passen nicht zusammen');
        return true;
    };
    if (!Array.isArray(m.providers) || m.providers.length !== sc.providers.length)
        add('providers', 'Ungültiger Wert');
    else
        m.providers.forEach((r, i) => {
            const p = `providers[${i}]`;
            if (!limit(r, p))
                return;
            if (!r.intervals || typeof r.intervals !== 'object' || Array.isArray(r.intervals)) {
                add(p + '.intervals', 'Falscher Datentyp');
                return;
            }
            for (const k of sc.data_types)
                range(`${p}.intervals.${k}`, sc.bounds.interval_s);
        });
    limit(m.global_limit, 'global_limit');
    const effective = p => {
        const s = sec[p];
        return s?.mode === 'clear' ? '' : s?.mode === 'change' && s.value !== '' ? s.value : undefined;
    };
    for (const key of SECRETS) {
        const v = effective(key);
        if (v === undefined)
            continue;
        if (typeof v !== 'string') {
            add(key, 'Falscher Datentyp');
            continue;
        }
        if (bytes(v) > sc.text[key])
            add(key, 'Text ist zu lang');
        if (v.includes('\0'))
            add(key, 'Ungültiger Wert');
        if (key === 'wifi_password' && v !== '' && !(/^[\x20-\x7e]{8,63}$/.test(v) || /^[0-9a-fA-F]{64}$/.test(v)))
            add(key, 'WLAN-Passwort: 8–63 ASCII-Zeichen oder 64 Hex-Ziffern; leer für offenes WLAN.');
        if (key === 'ap_password' && v !== '' && !/^[\x20-\x7e]{8,63}$/.test(v))
            add(key, 'Setup-Passwort: 8–63 ASCII-Zeichen; leer für offenes WLAN.');
        if (/_key$/.test(key) && !/^[\x21-\x7e]*$/.test(v))
            add(key, 'API-Schlüssel: nur sichtbare ASCII-Zeichen ohne Leerzeichen.');
    }
    if (typeof m.hostname !== 'string' || !dns(m.hostname) || m.hostname.includes('.'))
        add('hostname', 'Gerätename: Buchstaben, Ziffern und Bindestrich, ohne Bindestrich am Rand.');
    if (!dns(m.ntp_server))
        add('ntp_server', 'Zeitserver: gültige DNS-Namen oder IPv4-Adresse.');
    if (!sc.time_zones.includes(m.time_zone))
        add('time_zone', 'Ungültiger Wert');
    for (const [key, max] of Object.entries(sc.limits)) {
        const p = key === 'images' ? null : key;
        if (p && (!Array.isArray(m[p]) || m[p].length > max))
            add(p, 'Zu viele Einträge');
    }
    const source = (s, p) => {
        if (!s || typeof s !== 'object') {
            add(p, 'Falscher Datentyp');
            return;
        }
        range(p + '.provider', [0, sc.providers.length - 1]);
        for (const k of ['competition', 'season'])
            text(p + '.' + k, sc.text.competitions[k]);
        if (!id(s.competition))
            add(p + '.competition', 'IDs: ohne Steuerzeichen und ohne Leerzeichen am Anfang oder Ende.');
        if (typeof s.season === 'string' && s.season !== '' && (!/^\d+$/.test(s.season) || s.provider !== 0 && s.season.length !== 4))
            add(p + '.season', 'Saison: nur Ziffern, außer OpenLigaDB genau vier.');
    };
    (m.competitions || []).forEach((c, i) => {
        const p = `competitions[${i}]`;
        source(c.primary, p + '.primary');
        for (const [path, max] of [[p + '.competition_name', sc.text.competitions.competition_name],
                                  [p + '.club.name', sc.text.competitions.club_name]])
            if (text(path, max) && /[\x00-\x1f\x7f]/.test(get(m, path)))
                add(path, 'Ungültiger Wert');
        if (!c.club || typeof c.club !== 'object' || Array.isArray(c.club))
            add(p + '.club', 'Falscher Datentyp');
        else {
            range(p + '.club.team', [0, 4294967295]);
            range(p + '.club.group', [0, sc.limits.competitions]);
            if (typeof c.club.own_special !== 'boolean')
                add(p + '.club.own_special', 'Falscher Datentyp');
            if (c.club.group && !c.club.team)
                add(p + '.club.group', 'Eine Vereinsgruppe benötigt einen ausgewählten Verein.');
        }
        issues.push(...validateCompetitionDisplay(c.display, i));
        if (!c.primary?.competition)
            add(p + '.primary.competition', 'Wert wird benötigt');
        range(p + '.id', [0, 255]);
        if (c.id && m.competitions.findIndex(x => x.id === c.id) < i)
            add(p + '.id', 'Doppelter Eintrag');
    });
    if (m.screen_policy)
        issues.push(...validateScreenPolicy(m.screen_policy, m.competitions, true));
    const defaults = sc.defaults;
    for (const [k, v] of Object.entries(defaults))
        if (typeof v === 'boolean' && typeof m[k] !== 'boolean')
            add(k, 'Falscher Datentyp');
    for (const [i, c] of (m.competitions || []).entries())
        for (const k of ['active'])
            if (typeof c[k] !== 'boolean')
                add(`competitions[${i}].${k}`, 'Falscher Datentyp');
    return issues;
}
function modelFromConfig(wire) {
    const d = wire?.desired;
    if (wire?.version !== S.schema.config_version || !Number.isInteger(wire.revision) ||
        !/^[0-9a-f]{16}$/.test(wire.apply_id) || !d?.settings ||
        !Array.isArray(d.competitions) || !d.screens || !wire.names_ref)
        throw Error(t('Antwort des Geräts ist nicht lesbar.'));
    return prepareScreenModel({...clone(d.settings), competitions: clone(d.competitions),
        screen_policy: clone(d.screens), _revision: wire.revision,
        _applyId: wire.apply_id, _namesRef: clone(wire.names_ref)});
}
function applicationPayload(model = S.draft, secrets = S.sec) {
    const general = payload(model, secrets);
    delete general.competitions;
    for (const key of Object.keys(general))
        if (key.startsWith('_')) delete general[key];
    return {settings: general, competitions: competitionPayload(model.competitions),
        screens: screenPayload(model)};
}
function applyField(path) {
    return path?.replace(/^desired\.settings\.?/, '').replace(/^desired\.screens/, 'screen_policy')
        .replace(/^desired\./, '');
}
function mergeValues(before, draft, current) {
    if (sameObject(before, draft)) return clone(current);
    if (before && draft && current && typeof before === 'object' &&
        !Array.isArray(before) && !Array.isArray(draft) && !Array.isArray(current)) {
        const result = {};
        for (const key of new Set([...Object.keys(draft), ...Object.keys(current)])) {
            if (current[key] === undefined) result[key] = clone(draft[key]);
            else if (draft[key] === undefined) result[key] = clone(current[key]);
            else result[key] = mergeValues(before[key], draft[key], current[key]);
        }
        return result;
    }
    return clone(draft);
}
function acceptApply(wire, pending) {
    const next = modelFromConfig(wire);
    if (next._applyId !== pending.id || next._revision <= pending.expected)
        throw Error(t('Antwort des Geräts ist nicht lesbar.'));
    // New identities are assigned by the config owner once. Draft-only keys
    // correlate the acknowledged list without an independent association map.
    next.competitions.forEach((c, i) => {
        if (pending.model.competitions[i]?._screenKey)
            c._screenKey = pending.model.competitions[i]._screenKey;
    });
    S.draft = prepareScreenModel(mergeValues(pending.model, S.draft, next));
    S.saved = clone(next);
    if (sameObject(S.sec, pending.secrets)) S.sec = {};
    if (sameObject(S.pendingNames, pending.names)) S.pendingNames = null;
    pending.accepted = true;
    S.saveProgress.accepted = 1;
    if (S.status) {
        S.status.settings_source = S.status.competitions_source =
            S.status.screen_policy_source = 'stored';
    }
}
function saveFailure(error) {
    error.field = applyField(error.field);
    const accepted = S.pendingApply?.accepted;
    const unresolved = S.pendingApply && !accepted;
    error.severity = accepted && error.code === 'busy' ? 'warning' : 'error';
    error.saveFeedback = true;
    const prefix = accepted
        ? 'Gespeichert. Die Nachprüfung ist noch offen. Status erneut prüfen; Speichern muss nicht wiederholt werden.'
        : unresolved
        ? 'Speicherergebnis unklar. Nicht erneut speichern. Status prüfen; bei unklarem Speicherzustand Gerät neu starten.'
        : 'Speichern wurde abgelehnt. Alle Änderungen bleiben im Entwurf.';
    error.message = t(prefix) + ' ' + error.message;
    S.saveNotice = {severity: error.severity, message: error.message};
    return error;
}
async function readModel() {
    return modelFromConfig(await settingsRead('config'));
}
async function verifySaved() {
    if (S.saveProgress) S.saveProgress.stage = 'read';
    const wire = await settingsRead('config'), pending = S.pendingApply;
    if (pending && !pending.accepted) {
        if (wire.apply_id !== pending.id) {
            if (wire.revision > pending.expected) {
                // The old expected revision can no longer commit, even if its
                // request was delayed. Show the current baseline and retain
                // the draft; another Apply requires a separate user action.
                S.pendingApply = null;
                mergeBaseline(modelFromConfig(wire));
                throw new ApiError(409, {code: 'settings_changed'});
            }
            throw Error(t('Speicherergebnis noch unklar. Gerät neu starten und Status erneut prüfen.'));
        }
        acceptApply(wire, pending);
    }
    mergeBaseline(modelFromConfig(wire));
    await authenticate();
    S.pendingApply = null;
    S.saveNotice = null;
}
async function refreshSave() {
    if (S.saving) return;
    S.saving = true;
    renderBar();
    try {
        await verifySaved();
        toast(t(changes().length ? 'Status geprüft. Noch offene Änderungen können gespeichert werden.' : 'Gespeichert'));
    } catch (error) { throw saveFailure(error); }
    finally { S.saving = false; render(); }
}
async function writeModel() {
    if (S.pendingApply) throw Error(t('Speicherergebnis zuerst prüfen.'));
    const random = crypto.getRandomValues(new Uint8Array(8));
    if (random.every(x => x === 0)) random[0] = 1;
    const id = Array.from(random, x => x.toString(16).padStart(2, '0')).join('');
    const pending = {id, expected: S.saved._revision, model: clone(S.draft),
        secrets: clone(S.sec), names: S.pendingNames ? cloneNamesCollection(S.pendingNames) : null, accepted: false};
    const body = {expected_revision: pending.expected, apply_id: id,
        desired: applicationPayload(pending.model, pending.secrets)};
    if (pending.names) body.adopted_names = packAdoptedNames(pending.names);
    if (bytes(JSON.stringify(body)) > S.schema.max_apply_body)
        throw Error(t('Datei oder Wert ist zu groß.'));
    S.pendingApply = pending;
    S.saveProgress = {accepted: 0, total: 1, stage: 'write'};
    try {
        const result = await mutate('config', body, 'PUT');
        acceptApply(result, pending);
    } catch (error) {
        // Explicit rejections happened before publication. A transport error,
        // unreadable success or uncertain NVS result can never trigger resend.
        if (error instanceof ApiError && error.code !== 'outcome_unknown')
            S.pendingApply = null;
        throw error;
    }
}
async function save(networkConfirmed = false, sourceConfirmed = false) {
    if (S.saving) return;
    if (S.pendingApply) return refreshSave();
    S.tried = true;
    S.server = null;
    S.saveNotice = null;
    if (validate(S.draft).length) {
        S.expanded = true; paintErrors(); renderBar(); return;
    }
    if (!changes().length) return;
    const source = S.status?.settings_source;
    if (['rejected', 'damaged'].includes(source) && !sourceConfirmed) {
        confirmAction('Gespeicherte Einstellungen überschreiben?', t(SOURCE[source]),
            () => save(networkConfirmed, true), 'Überschreiben und speichern');
        return;
    }
    const general = payload();
    const networkChange = S.saved.hostname !== S.draft.hostname ||
        !eq(S.saved.wifi_ssid, S.draft.wifi_ssid) ||
        S.saved.external_antenna !== S.draft.external_antenna ||
        general.wifi_password !== undefined || general.ap_password !== undefined;
    if (networkChange && !networkConfirmed) {
        const address = 'http://' + S.draft.hostname + '.local';
        confirmAction('WLAN oder Gerätename geändert',
            'Die Funkverbindung wird neu aufgebaut. Falls die Seite getrennt wird, öffne eine der folgenden Adressen.',
            () => save(true, sourceConfirmed), 'Speichern und verbinden',
            `<p><a href="${esc(address)}">${esc(address)}</a>${[S.status?.ip, S.status?.ap_ip].filter(ip => ip && ip !== '0.0.0.0').map(ip => `<br><a href="http://${esc(ip)}">http://${esc(ip)}</a>`).join('')}</p>`);
        return;
    }
    S.saving = true;
    document.querySelectorAll('#main input,#main select,#main button,#nav button,#auth').forEach(el => el.disabled = true);
    renderBar();
    try {
        await writeModel();
        if (networkChange) {
            toast(t('Netzwerkänderung gespeichert. Neue Adresse öffnen, wenn die Verbindung abbricht.'));
            S.saveNotice = {severity: 'warning', message: t('Gespeichert. Die Nachprüfung ist noch offen. Status erneut prüfen; Speichern muss nicht wiederholt werden.')};
            const host = S.saved.hostname + '.local';
            if (location.hostname.endsWith('.local') && location.hostname !== host)
                location.assign('http://' + host);
        } else {
            await verifySaved();
            toast(t('Gespeichert'));
        }
    } catch (error) {
        error.field = applyField(error.field);
        if (error.field) {
            S.server = {path: error.field, message: error.message};
            S.expanded = true; jump(error.field);
        }
        throw saveFailure(error);
    } finally { S.saving = false; render(); }
}
function mergeBaseline(next) {
    S.draft = prepareScreenModel(mergeValues(S.saved, S.draft, next));
    S.saved = clone(next);
}
function pruneMissing(m, present) {
    for (const s of Object.values(m.screen_policy.appearance))
        if (s.image && !present.has(s.image))
            s.image = 0;
    m.screen_policy.slides = m.screen_policy.slides.filter(id => present.has(id));
}
function validateImport(data) {
    const reject = (path, key) => {
        throw Error(labelOf(path) + ': ' + t(key));
    };
    if (!data || typeof data !== 'object' || Array.isArray(data))
        reject('', 'Falscher Datentyp');
    const allowed = new Set([...Object.keys(S.schema.defaults), ...SECRETS, ...LISTS, 'screen_policy']), m = clone(S.saved), secret = {};
    const object = (value, path, keys) => {
        if (!value || typeof value !== 'object' || Array.isArray(value))
            reject(path, 'Falscher Datentyp');
        for (const k of Object.keys(value))
            if (!keys.includes(k))
                reject(path + '.' + k, 'Unbekanntes Feld');
    };
    for (const [k, v] of Object.entries(data)) {
        if (!allowed.has(k))
            reject(k, 'Unbekanntes Feld');
        if (SECRETS.some(p => p + '_set' === k)) {
            if (typeof v !== 'boolean')
                reject(k, 'Falscher Datentyp');
            continue;
        }
        if (SECRETS.includes(k)) {
            if (v !== null && typeof v !== 'string')
                reject(k, 'Falscher Datentyp');
            if (v !== null)
                secret[k] = {
                    mode: v === '' ? 'clear' : 'change', value: v
                };
            continue;
        }
        if (k === 'screen_policy') {
            m.screen_policy = clone(v);
            continue;
        }
        if (v === null)
            continue;
        if (k === 'wifi_ssid') {
            const bytes = ssidBytes(v);
            if (!bytes)
                reject(k, 'Ungültiger Base64-WLAN-Name');
            if (bytes.length > S.schema.text.wifi_ssid)
                reject(k, 'Text ist zu lang');
            m[k] = typeof v === 'string' ? v : ssidValue(v.base64);
            continue;
        }
        const initial = LISTS.includes(k) ? [] : S.schema.defaults[k];
        if (typeof initial !== 'object' && typeof v !== typeof initial)
            reject(k, 'Falscher Datentyp');
        if (Array.isArray(initial) && !Array.isArray(v))
            reject(k, 'Falscher Datentyp');
        if (k === 'competitions') {
            m[k] = v.map((c, i) => {
                const p = `competitions[${i}]`;
                object(c, p, [
                    'id', 'incarnation', 'active', 'primary', 'competition_name', 'club', 'display'
                ]);
                const row = blankCompetition();
                for (const [key, val] of Object.entries(c)) {
                    if (val === null)
                        continue;
                    if (key === 'primary' || key === 'club') {
                        object(val, p + '.' + key, key === 'primary'
                            ? ['provider', 'competition', 'season']
                            : ['team', 'name', 'own_special', 'group']);
                        for (const [sk, sv] of Object.entries(val))
                            if (sv !== null)
                                row[key][sk] = sv;
                    }
                    else
                        row[key] = clone(val);
                }
                return row;
            });
        }
        else if (k === 'providers') {
            if (v.length > m.providers.length)
                reject(k, 'Ungültiger Wert');
            const known = o => Object.fromEntries(Object.entries(o).filter(([key, val]) => key !== 'intervals' && val !== null));
            m[k] = m.providers.map((x, i) => {
                if (i >= v.length)
                    return x;
                const row = v[i], p = `providers[${i}]`;
                object(row, p, [...LIMIT_KEYS, 'intervals']);
                const out = {
                    ...x, ...known(row)
                };
                if (row.intervals != null) {
                    object(row.intervals, p + '.intervals', S.schema.data_types);
                    out.intervals = {
                        ...x.intervals, ...known(row.intervals)
                    };
                }
                return out;
            });
        }
        else if (k === 'global_limit') {
            object(v, k, LIMIT_KEYS);
            m[k] = {
                ...m[k], ...Object.fromEntries(Object.entries(v).filter(([, val]) => val !== null))
            };
        }
        else
            m[k] = v;
    }
    prepareScreenImport(m, data);
    const issues = validate(m, secret);
    if (issues.length)
        throw Error(labelOf(issues[0].path) + ': ' + issues[0].message);
    return m; // the caller checks the size of every request part
}
