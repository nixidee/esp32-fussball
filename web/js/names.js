// Adopted collection editor. All changes belong to the common application draft;
// reading a catalogue or fetching provider originals never changes club groups.
function nameProviders() { return S.schema.provider_keys; }
function adoptedPrefix(value, limit) {
    let out = '';
    for (const c of value) {
        if (bytes(out + c) > limit) break;
        out += c;
    }
    return out;
}
function adoptedShortPrefix(value) { return adoptedPrefix(value, S.schema.short_name_max); }
function shortAdoptionPreview(value) { return tf('Gespeichert wird der UTF-8-Präfix: „%“.', adoptedShortPrefix(value)); }
function expandNamesWire(input) {
    if (input?.version !== S.schema.names_wire_version) return input;
    const texts = input.texts, used = new Set();
    if (!texts || typeof texts !== 'object' || Array.isArray(texts)) throw Error(t('Ungültige Namenssammlung. Format, Kennungen und doppelte Zuordnungen prüfen.'));
    for (const key of Object.keys(texts))
        if (!/^[1-9][0-9]*$/.test(key) || Number(key) > S.schema.names_file_bytes || typeof texts[key] !== 'string')
            throw Error(t('Ungültiger Wert'));
    const text = (id, limit = 0) => {
        if (!Number.isInteger(id) || !Object.prototype.hasOwnProperty.call(texts, id)) throw Error(t('Ungültiger Wert'));
        if (limit && bytes(texts[id]) > limit) throw Error(t('Ungültiger Wert'));
        used.add(String(id)); return texts[id];
    };
    const clubs = input.clubs.map(club => ({...club,
        ...Object.fromEntries(['de','en','short','league'].filter(key => key in club).map(key => [key, text(club[key], key === 'league' ? 0 : key === 'short' ? S.schema.short_name_max : S.schema.names_name_max)])),
        aliases: (club.aliases || []).map(id => text(id))}));
    const exceptions = (input.exceptions || []).map(row => ({...row, name: text(row.name, S.schema.short_name_max)}));
    if (Object.keys(texts).length !== used.size) throw Error(t('Ungültiger Wert'));
    const result = {...input, version: S.schema.names_source_version, clubs, exceptions};
    delete result.texts;
    return result;
}
function cloneNamesCollection(input) {
    return {...input, clubs: input.clubs.map(club => ({...club,
        ids: Object.fromEntries(Object.entries(club.ids || {}).map(([key, ids]) => [key, [...ids]])),
        aliases: [...(club.aliases || [])]})), exceptions: (input.exceptions || []).map(row => ({...row}))};
}
function packAdoptedNames(input) {
    const collection = validateAdoptedNames(input), texts = {}, known = new Map();
    const intern = (value, limit) => {
        const text = limit ? adoptedPrefix(value, limit) : value;
        if (!known.has(text)) { const id = known.size + 1; known.set(text, id); texts[id] = text; }
        return known.get(text);
    };
    return {...collection, version: S.schema.names_wire_version, texts,
        clubs: collection.clubs.map(club => ({...club,
            ...Object.fromEntries(['de','en','short','league'].filter(key => key in club).map(key =>
                [key, intern(club[key], key === 'league' ? 0 : key === 'short' ? S.schema.short_name_max : S.schema.names_name_max)])),
            aliases: club.aliases.map(text => intern(text, 0))})),
        exceptions: collection.exceptions.map(row => ({...row, name: intern(row.name, S.schema.short_name_max)}))};
}
function validateAdoptedNames(input) {
    input = expandNamesWire(input);
    const fail = () => { throw Error(t('Ungültige Namenssammlung. Format, Kennungen und doppelte Zuordnungen prüfen.')); };
    const object = (value, allowed) => {
        if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).some(k => !allowed.includes(k))) fail();
    };
    const text = value => {
        if (typeof value !== 'string' || value.includes('\0') ||
            new TextDecoder('utf-8', {fatal: true, ignoreBOM: true}).decode(new TextEncoder().encode(value)) !== value) fail();
        return value;
    };
    const id = value => { if (!validClubId(value)) fail(); };
    object(input, ['format', 'version', 'clubs', 'exceptions']);
    if (input.format !== S.schema.names_format || input.version !== S.schema.names_source_version || !Array.isArray(input.clubs)) fail();
    if ('exceptions' in input && !Array.isArray(input.exceptions)) fail();
    const result = {...input, clubs: [...input.clubs], exceptions: input.exceptions == null ? [] : [...input.exceptions]}, keys = new Set(), bound = new Set(), exceptions = new Set();
    result.exceptions ??= [];
    if (!Array.isArray(result.exceptions)) fail();
    for (let index = 0; index < result.clubs.length; ++index) {
        const original = result.clubs[index];
        object(original, ['key', 'de', 'en', 'short', 'league', 'ids', 'aliases']);
        const club = result.clubs[index] = {...original};
        object(club, ['key', 'de', 'en', 'short', 'league', 'ids', 'aliases']);
        text(club.key);
        if (!club.key || bytes(club.key) > S.schema.names_key_max || keys.has(club.key)) fail();
        keys.add(club.key);
        for (const field of ['de', 'en', 'short', 'league'])
            if (field in club) text(club[field]);
        if ('ids' in club) object(club.ids, nameProviders());
        club.ids = {...(club.ids || {})};
        object(club.ids, nameProviders());
        for (const [provider, ids] of Object.entries(club.ids)) {
            if (!Array.isArray(ids)) fail();
            club.ids[provider] = [...ids];
            for (const value of ids) {
                id(value);
                const key = provider + ':' + value;
                if (bound.has(key)) fail();
                bound.add(key);
            }
        }
        if ('aliases' in club && !Array.isArray(club.aliases)) fail();
        club.aliases = club.aliases == null ? [] : [...club.aliases];
        if (!Array.isArray(club.aliases)) fail();
        club.aliases.forEach(text);
    }
    for (let i = 0; i < result.exceptions.length; ++i) {
        const exception = result.exceptions[i] = {...result.exceptions[i]};
        object(exception, ['provider', 'id', 'name']);
        if (!Number.isInteger(exception.provider) || !nameProviders()[exception.provider]) fail();
        id(exception.id); text(exception.name);
        const key = exception.provider + ':' + exception.id;
        if (exceptions.has(key)) fail();
        exceptions.add(key);
    }
    return result;
}
function namesAdoptionChanges(collection) {
    return collection.clubs.flatMap(club => ['de', 'en', 'short'].filter(k =>
        typeof club[k] === 'string' && club[k] !== adoptedPrefix(club[k], k === 'short' ? S.schema.short_name_max : S.schema.names_name_max))
        .map(k => `${club.key} · ${k}: ${club[k]} → ${adoptedPrefix(club[k], k === 'short' ? S.schema.short_name_max : S.schema.names_name_max)}`))
        .concat(collection.exceptions.filter(x => x.name !== adoptedShortPrefix(x.name))
            .map(x => `${PROVIDERS[x.provider]} · ${x.id}: ${x.name} → ${adoptedShortPrefix(x.name)}`));
}
function keepNamesDraft(n) {
    const validated = validateAdoptedNames(n.collection);
    S.pendingNames = validated;
    renderBar();
}
function originalClubMatch(collection, provider, team) {
    const key = nameProviders()[provider];
    const bound = collection.clubs.filter(club => (club.ids[key] || []).includes(team.id));
    if (bound.length) return bound.length === 1 ? bound[0].key : '';
    // Compare complete source values and aliases before any display clipping.
    const exact = collection.clubs.filter(club => [club.de, club.en, ...(club.aliases || [])].includes(team.name));
    return exact.length === 1 ? exact[0].key : '';
}
function adoptOriginalName(n, index, target, language) {
    const team = n.originals?.items[index], provider = n.originals?.source.provider;
    if (!team || !['de', 'en'].includes(language)) throw Error(t('Ungültiger Wert'));
    const collection = cloneNamesCollection(n.collection), binding = nameProviders()[provider];
    const bound = collection.clubs.find(club => (club.ids[binding] || []).includes(team.id));
    let club = collection.clubs.find(club => club.key === target);
    if (target === '__new__') {
        const key = binding + ':' + team.id;
        if (collection.clubs.some(club => club.key === key)) throw Error(t('Doppelter Eintrag'));
        club = {key, ids: {}, aliases: []}; collection.clubs.push(club);
    }
    if (!club || (bound && bound.key !== club.key)) throw Error(t('Werte passen nicht zusammen'));
    club[language] = team.name;
    if ('short_name' in team) club.short = team.short_name;
    if (!(club.ids[binding] || []).includes(team.id)) club.ids[binding] = [...(club.ids[binding] || []), team.id];
    n.collection = validateAdoptedNames(collection);
    n.selected = club.key;
    keepNamesDraft(n);
}
function editorNameTeams(n) {
    const teams = new Map(), add = team => {
        const key = team.provider + ':' + team.id;
        if (!teams.has(key)) teams.set(key, team);
    };
    n.items.forEach(add);
    for (const club of n.collection?.clubs || [])
        nameProviders().forEach((provider, index) => (club.ids[provider] || []).forEach(id =>
            add({provider: index, id, name: club.de || club.en || club.key, short_name: club.short || ''})));
    for (const row of n.collection?.exceptions || []) add({provider: row.provider, id: row.id, name: '', short_name: row.name});
    return [...teams.values()];
}
function downloadJson(data, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2)], {type: 'application/json'}));
    const a = document.createElement('a'); a.href = url; a.download = filename; a.click();
    setTimeout(() => URL.revokeObjectURL(url), CLIENT.object_url_release_ms);
}

function createNamesEditor(config, {
    settingsRead, sameObject, screenConflict, validateAdoptedNames, clone, compName, t, tf, ht, esc, $, dialog, button,
    adoptedPrefix, nameProviders, editorNameTeams, originalClubMatch, keepNamesDraft, adoptOriginalName, shortAdoptionPreview, downloadJson, packAdoptedNames, namesAdoptionChanges, confirmAction, mutate, validClubId, PROVIDERS
}) {
    let names = null;
    async function loadNames(n) {
        const [adoptedResult, teams] = await Promise.allSettled([settingsRead('team-names'),
            n.competition ? settingsRead('teams?competition=' + n.competition) : Promise.resolve({items: []})]);
        if (names !== n) return;
        if (adoptedResult.status === 'rejected') throw adoptedResult.reason;
        const adopted = adoptedResult.value;
        if (!sameObject(adopted.names_ref, config.saved._namesRef)) throw screenConflict();
        n.collection = validateAdoptedNames(config.pendingNames || adopted.adopted_names);
        if (teams.status === 'fulfilled') n.items = teams.value.items || [];
        else n.error = teams.reason.message;
    }
    async function openNames(c) {
        const n = names = {competition: c?.id || 0, config: clone(c), title: c ? compName(c) : t('Vereinsnamen'),
            items: [], busy: true, error: '', collection: null, selected: ''};
        drawNames();
        try { await loadNames(n); } catch (error) { n.error = error.message; }
        if (names !== n) return;
        n.busy = false; drawNames();
    }
    async function loadNamesPreset(n) {
        n.busy = true; n.error = ''; drawNames();
        try {
            const preset = validateAdoptedNames(await settingsRead('team-names/preset'));
            if (names !== n) return;
            n.preset = preset;
        } finally {
            if (names === n) { n.busy = false; drawNames(); }
        }
    }
    async function loadOriginalNames(n) {
        n.busy = true; n.error = ''; n.queryAbort = new AbortController(); drawNames();
        try {
            const result = await mutate('team-names/originals?competition=' + n.competition, {}, 'POST', false,
                {signal: n.queryAbort.signal, timeoutMs: config.schema.operation_deadline_ms + config.schema.socket_timeout_ms});
            if (names !== n) return;
            const source = n.config.primary;
            if (result.source?.provider !== source.provider || result.source?.competition !== source.competition ||
                (source.season && result.source.season !== source.season) || !Array.isArray(result.items)) throw screenConflict();
            const ids = new Set();
            for (const team of result.items) {
                if (!validClubId(team.id) || ids.has(team.id) ||
                    typeof team.name !== 'string' || ('short_name' in team && typeof team.short_name !== 'string')) throw Error(t('Ungültiger Wert'));
                ids.add(team.id);
            }
            n.originals = result; n.originalIndex = 0;
            n.originalTarget = result.items.length ? originalClubMatch(n.collection, source.provider, result.items[0]) : '';
        } finally {
            if (names === n) { n.queryAbort = null; n.busy = false; drawNames(); }
        }
    }
    function namesMarkup(n, collection, club, teams, schema, canQuery) {
        const options = collection?.clubs.map(x => `<option value="${esc(x.key)}" ${x === club ? 'selected' : ''}>${esc(adoptedPrefix(x.de || x.en || x.key, schema.names_name_max))}</option>`).join('') || '';
        const clubFields = club ? ['de', 'en', 'short', 'league'].map(key => `<div class="field"><label for="nameField-${key}">${esc({de: 'Deutsch', en: 'English', short: t('Kurzname'), league: t('Wettbewerb')}[key])}</label><div class="ctl"><input type="text" class="w-l" id="nameField-${key}" data-name-field="${key}" value="${esc(club[key] ?? '')}" ${key in club ? '' : 'disabled'}></div><label><input type="checkbox" data-name-inherit="${key}" ${key in club ? '' : 'checked'}>${ht(key === 'league' ? 'Kein Metadatenwert' : 'Name des Anbieters verwenden')}</label><p class="help" data-name-preview="${key}">${esc(['de','en','short'].includes(key) ? adoptedPrefix(club[key] || '', key === 'short' ? schema.short_name_max : schema.names_name_max) : '')}</p></div>`).join('') : '';
        const aliasEditor = club ? `<select id="nameAliasSelect" aria-label="${ht('Alias')}">${club.aliases.map((text, index) => `<option value="${index}" ${index === n.aliasIndex ? 'selected' : ''}>${index + 1} · ${esc(adoptedPrefix(text, schema.names_name_max))}</option>`).join('')}</select><textarea aria-label="${ht('Vollständiger Alias')}" id="nameAliasValue" rows="3">${esc(club.aliases[n.aliasIndex] || '')}</textarea>${button('Alias hinzufügen', 'nameAliasAdd')}${button('Alias entfernen', 'nameAliasRemove', club.aliases.length ? '' : 'disabled')}` : '';
        const bindingFields = club ? schema.provider_keys.map((key, i) => `<div class="field"><label for="nameProvider-${key}">${esc(PROVIDERS[i])} · ID</label><div class="ctl"><input type="text" class="w-l" id="nameProvider-${key}" data-name-provider="${key}" value="${esc((club.ids[key] || []).join(', '))}" inputmode="numeric"></div></div>`).join('') : '';
        const teamOptions = teams.map((x, i) => `<option value="${i}" ${n.exceptionIdentity === x.provider + ':' + x.id ? 'selected' : ''}>${esc(x.name || x.short_name || x.id)} · ${esc(x.id)}</option>`).join('');
        const preset = n.preset ? `<select id="namePresetClub" aria-label="${ht('Verein aus Vorlage')}">${n.preset.clubs.map(x => `<option value="${esc(x.key)}">${esc(adoptedPrefix(x.de || x.key, schema.names_name_max))}</option>`).join('')}</select>${button('Aus Vorlage übernehmen', 'namePresetUse')}` : button('Namensvorlage laden', 'namePresetLoad');
        const original = n.originals?.items[n.originalIndex || 0];
        const originalEditor = n.competition ? `<details data-name-section="originals"><summary>${ht('Originalnamen des Anbieters')}</summary><p class="help">${ht('Ruft vollständige Originalnamen für den gespeicherten Wettbewerb ab. Der Abruf zählt zum Anbieterlimit. Nur eine ausdrückliche Übernahme ändert den Entwurf; Speichern übernimmt ihn dauerhaft.')}</p>${button('Originalnamen abrufen', 'nameOriginalLoad', canQuery ? '' : 'disabled')}<p class="help">${canQuery ? '' : ht('Geänderte Wettbewerbsquelle zuerst speichern.')}</p>${original ? `<label for="nameOriginalTeam">${ht('Originalname')}</label><select id="nameOriginalTeam">${n.originals.items.map((team, index) => `<option value="${index}" ${index === (n.originalIndex || 0) ? 'selected' : ''}>${esc(adoptedPrefix(team.name, schema.names_name_max))} · ${team.id}</option>`).join('')}</select><textarea aria-label="${ht('Vollständiger Originalname')}" readonly rows="3">${esc(original.name)}</textarea><p>${esc(original.short_name ?? '')}</p><label for="nameOriginalTarget">${ht('In Verein übernehmen')}</label><select id="nameOriginalTarget"><option value="">${ht('Bitte wählen')}</option><option value="__new__" ${n.originalTarget === '__new__' ? 'selected' : ''}>${ht('Neuer Verein')}</option>${collection.clubs.map(club => `<option value="${esc(club.key)}" ${n.originalTarget === club.key ? 'selected' : ''}>${esc(club.de || club.en || club.key)}</option>`).join('')}</select><select id="nameOriginalLanguage" aria-label="${ht('Sprache')}"><option value="de">Deutsch</option><option value="en">English</option></select>${button('Originalwerte in Entwurf übernehmen', 'nameOriginalUse')}` : n.originals ? `<p>${ht('Keine Teams gemeldet')}</p>` : ''}</details>` : '';
        return {title: t('Vereinsnamen') + (n.hasConfig ? ' · ' + n.title : ''), body:
            `<p class="help">${ht('Namensänderungen bleiben im gemeinsamen Entwurf. Erst Speichern übernimmt sie. Vereinsgruppen werden dadurch nicht verändert.')}</p><p class="help">${ht('Anbieterwert verwenden entfernt die eigene Vorgabe. Ein eigener leerer Wert bleibt ausdrücklich leer.')}</p>
            ${collection ? `<div class="field"><label for="nameClub">${ht('Verein')}</label><select id="nameClub">${options}</select></div><div class="grid">${clubFields}</div><details data-name-section="identities"><summary>${ht('Kennungen und Aliasse')}</summary><div class="grid">${bindingFields}</div>${aliasEditor}</details>
            <div class="actions">${preset}</div><details data-name-section="exceptions"><summary>${ht('Kurznamen-Ausnahmen')}</summary><select id="nameTeam" aria-label="${ht('Verein')}">${teamOptions}</select><input type="text" class="w-l" id="nameException" aria-label="${ht('Kurzname')}" value=""><label><input id="nameExceptionInherit" type="checkbox">${ht('Name des Anbieters verwenden')}</label><p class="help" id="nameExceptionPreview"></p>${button('In Entwurf übernehmen', 'nameExceptionApply')}</details>` : ''}
            ${originalEditor}<div class="actions"><label class="btn">${ht('Namenssammlung importieren')}<input id="nameImport" type="file" accept="application/json,.json" hidden></label>${button('Namenssammlung exportieren', 'nameExport', collection ? '' : 'disabled')}</div>
            <p class="dlg-err">${esc(n.busy ? t('Daten werden geladen') : n.error)}</p>`};
    }
    function drawNames() {
        const n = names;
        if (!n) return;
        const previous = $('dialog');
        const oldSections = previous?.querySelectorAll('details[data-name-section]') || [];
        if (oldSections.length) n.openSections = [...oldSections].filter(section => section.open).map(section => section.dataset.nameSection);
        const oldScroll = previous?.querySelector('.db')?.scrollTop || 0;
        const collection = n.collection, club = collection?.clubs.find(x => x.key === n.selected) || collection?.clubs[0];
        n.selected = club?.key || '';
        n.aliasIndex = Math.min(n.aliasIndex || 0, Math.max(0, (club?.aliases.length || 0) - 1));
        const teams = editorNameTeams(n);
        const savedSource = config.saved?.competitions?.find(c => c.id === n.competition)?.primary;
        const canQuery = !!collection && n.competition && sameObject(savedSource, n.config?.primary);
        const markup = namesMarkup({selected: n.selected, aliasIndex: n.aliasIndex,
            exceptionIdentity: n.exceptionIdentity, preset: n.preset, competition: n.competition,
            originals: n.originals, originalIndex: n.originalIndex, originalTarget: n.originalTarget,
            title: n.title, hasConfig: !!n.config, busy: n.busy, error: n.error},
            collection, club, teams, {names_name_max: config.schema.names_name_max,
                short_name_max: config.schema.short_name_max, provider_keys: config.schema.provider_keys}, canQuery);
        const d = dialog(markup.title, markup.body, button('Schließen', 'close'), true);
        d.querySelectorAll('details[data-name-section]').forEach(section => {
            section.open = (n.openSections || []).includes(section.dataset.nameSection);
        });
        const body = d.querySelector('.db'); if (body) body.scrollTop = oldScroll;
        const close = () => { if (names === n) { names = null; n.queryAbort?.abort(); } d.close(); };
        d.querySelector('[data-close]').onclick = close;
        d.querySelector('[data-act=close]').onclick = close;
        d.oncancel = () => { if (names === n) { names = null; n.queryAbort?.abort(); } };
        d.querySelectorAll('input,select,textarea,button').forEach(el => { if (n.busy && !el.matches('[data-close],[data-act=close]')) el.disabled = true; });
        const act = (key, callback) => d.querySelector(`[data-act=${key}]`)?.addEventListener('click', async () => {
            try { await callback(); } catch (error) { if (names === n) { n.error = error.message; drawNames(); } }
        });
        $('nameClub')?.addEventListener('change', e => { n.selected = e.target.value; drawNames(); });
        d.querySelectorAll('[data-name-inherit]').forEach(el => el.onchange = () => {
            const key = el.dataset.nameInherit;
            if (el.checked) delete club[key]; else club[key] = '';
            keepNamesDraft(n); drawNames();
        });
        d.querySelectorAll('[data-name-field]').forEach(el => el.oninput = () => {
            const key = el.dataset.nameField;
            club[key] = el.value;
            try { keepNamesDraft(n); n.error = ''; } catch (error) { n.error = error.message; }
            const preview = d.querySelector(`[data-name-preview=${key}]`);
            preview.textContent = key === 'league' ? '' : adoptedPrefix(el.value, key === 'short' ? config.schema.short_name_max : config.schema.names_name_max);
        });
        d.querySelectorAll('[data-name-provider]').forEach(el => el.onchange = () => {
            const ids = el.value.trim() ? el.value.split(',').map(x => Number(x.trim())) : [];
            const prior = club.ids[el.dataset.nameProvider];
            club.ids[el.dataset.nameProvider] = ids;
            try { keepNamesDraft(n); n.error = ''; } catch (error) { if (prior === undefined) delete club.ids[el.dataset.nameProvider]; else club.ids[el.dataset.nameProvider] = prior; n.error = error.message; }
            drawNames();
        });
        $('nameAliasSelect')?.addEventListener('change', e => { n.aliasIndex = Number(e.target.value); drawNames(); });
        $('nameAliasValue')?.addEventListener('change', e => { if (club.aliases.length) { club.aliases[n.aliasIndex] = e.target.value; keepNamesDraft(n); } });
        act('nameAliasAdd', () => { club.aliases.push(''); n.aliasIndex = club.aliases.length - 1; keepNamesDraft(n); drawNames(); });
        act('nameAliasRemove', () => { club.aliases.splice(n.aliasIndex, 1); keepNamesDraft(n); drawNames(); });
        const exception = () => {
            const team = teams[Number($('nameTeam')?.value || 0)];
            if (!team) return;
            const value = collection.exceptions.find(x => x.provider === team.provider && x.id === team.id);
            $('nameException').value = value?.name || '';
            $('nameExceptionInherit').checked = value === undefined;
            $('nameException').disabled = value === undefined || n.busy;
            $('nameExceptionPreview').textContent = shortAdoptionPreview($('nameException').value);
        };
        $('nameTeam')?.addEventListener('change', () => { const team = teams[Number($('nameTeam').value || 0)]; n.exceptionIdentity = team ? team.provider + ':' + team.id : ''; exception(); });
        $('nameExceptionInherit')?.addEventListener('change', e => $('nameException').disabled = e.target.checked || n.busy);
        $('nameException')?.addEventListener('input', e => $('nameExceptionPreview').textContent = shortAdoptionPreview(e.target.value));
        exception();
        act('nameExceptionApply', () => {
            const team = teams[Number($('nameTeam').value || 0)];
            if (!team) return;
            const text = $('nameException').value;
            collection.exceptions = collection.exceptions.filter(x => x.provider !== team.provider || x.id !== team.id);
            if (!$('nameExceptionInherit').checked) collection.exceptions.push({provider: team.provider, id: team.id, name: text});
            keepNamesDraft(n); drawNames();
        });
        act('nameOriginalLoad', () => loadOriginalNames(n));
        $('nameOriginalTeam')?.addEventListener('change', e => { n.originalIndex = Number(e.target.value); n.originalTarget = originalClubMatch(collection, n.originals.source.provider, n.originals.items[n.originalIndex]); drawNames(); });
        $('nameOriginalTarget')?.addEventListener('change', e => { n.originalTarget = e.target.value; });
        act('nameOriginalUse', () => { adoptOriginalName(n, n.originalIndex || 0, $('nameOriginalTarget').value, $('nameOriginalLanguage').value); drawNames(); });
        act('namePresetLoad', () => loadNamesPreset(n));
        act('namePresetUse', () => {
            const selected = n.preset.clubs.find(x => x.key === $('namePresetClub').value);
            const remaining = collection.clubs.filter(x => x.key !== selected.key);
            // Validate all provider identities before replacing the selected club.
            n.collection = validateAdoptedNames({...collection, clubs: [...remaining, selected]});
            n.selected = selected.key; keepNamesDraft(n); drawNames();
        });
        act('nameExport', () => downloadJson(packAdoptedNames(collection), 'football-team-names.json'));
        $('nameImport').onchange = async e => {
            const file = e.target.files[0]; if (!file) return;
            try {
                if (file.size > config.schema.max_apply_body) throw Error(t('Datei oder Wert ist zu groß.'));
                const imported = validateAdoptedNames(JSON.parse(await file.text()));
                if (names !== n) return;
                const clipped = namesAdoptionChanges(imported);
                confirmAction('Namenssammlung importieren', 'Ersetzt die Namenssammlung im Entwurf. Erst Speichern übernimmt sie.', () => {
                    n.collection = imported; n.selected = ''; keepNamesDraft(n); names = null;
                }, 'In Entwurf übernehmen', clipped.length ? `<ul>${clipped.map(x => `<li>${esc(x)}</li>`).join('')}</ul>` : '');
            } catch (error) { if (names === n) { n.error = error.message; drawNames(); } }
        };
    }

    return Object.freeze({openNames});
} // End private NamesEditor scope.
const {openNames} = createNamesEditor({
    get schema() { return S.schema; },
    get saved() { return S.saved; },
    get pendingNames() { return S.pendingNames; }
}, {
    PROVIDERS,
    settingsRead,
    sameObject,
    screenConflict,
    validateAdoptedNames,
    clone,
    compName,
    t,
    tf,
    ht,
    esc,
    $,
    dialog,
    button,
    adoptedPrefix,
    nameProviders,
    editorNameTeams,
    originalClubMatch,
    keepNamesDraft,
    adoptOriginalName,
    shortAdoptionPreview,
    downloadJson,
    packAdoptedNames,
    namesAdoptionChanges,
    confirmAction,
    mutate,
    validClubId
});
