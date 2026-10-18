// Competition list editing and the provider selection wizard.
// Entries are identified by their device id; a new entry has id 0 until saved.
function compSaved(c) {
    return !!c?.id && eq(c, S.saved?.competitions.find(x => x.id === c.id));
}
function compName(c) {
    return c?.competition_name || c?.primary.competition || t('Unbekannt');
}
function competitions() {
    const a = S.draft.competitions, limit = S.schema.limits.competitions, rows = a.map((c, i) => {
        const open = S.open === i, saved = compSaved(c);
        return `<div class="card item ${c.active ? '' : 'inactive'}" data-field="competitions[${i}]"><div class="pos">${i + 1}</div><div class="item-main"><div class="row"><h3>${esc(compName(c))}</h3>${saved ? '' : `<span class="tag">${ht('Nicht gespeichert')}</span>`}</div><div class="meta"><span>${PROVIDERS[c.primary.provider]}</span><span>${c.club.team ? esc(c.club.name || c.club.team) : ht('Ohne Verein')}</span></div>${open ? `<div class="meta"><span>${ht('Saison')}: ${esc(c.primary.season || t('Automatisch'))}</span></div>${S.draft.screen_policy ? screenField(`competitions[${i}].club.own_special`, 'switch', 'Bevorzugt Spiele des ausgewählten Vereins in der eigenen Phase und in den zugehörigen Ansichten. Ohne Sonderbehandlung gelten dieselben Regeln wie für andere Spiele; der Verein bleibt für Zuordnung und Tabellenmarkierung erhalten.', null, !c.club.team, 'Eigene Sonderbehandlung') : ''}${saved ? '' : `<p class="help">${ht('Noch nicht gespeichert (neu oder geändert). Drücke unten „Speichern“: Erst dann ruft das Gerät diesen Wettbewerb ab.')}</p>`}<div class="row">${button('Nach oben', 'compMove', `data-i="${i}" data-dir="-1" ${i === 0 ? 'disabled' : ''}`, 'icon')}${button('Nach unten', 'compMove', `data-i="${i}" data-dir="1" ${i === a.length - 1 ? 'disabled' : ''}`, 'icon')}${button('Bearbeiten', 'compEdit', `data-i="${i}"`)}${button('Vereinsnamen', 'compNames', `data-i="${i}"`)}${button('Entfernen', 'compDelete', `data-i="${i}"`, 'ghost danger')}</div>` : ''}<div class="field-error" data-error="competitions[${i}]"></div></div><div class="item-side">${F(`competitions[${i}].active`, 'switch')}${button(open ? 'Schließen' : 'Details', 'compOpen', `data-i="${i}" aria-expanded="${open}"`)}</div></div>`;
    });
    let previousGroup = 0;
    const items = competitionOrder().map(i => {
        const group = a[i].club.group;
        const heading = group && group !== previousGroup ? `<div class="card row"><b>${ht('Vereinsgruppe')}: ${esc(groupName(group))}</b>${button('Gruppe bearbeiten', 'clubGroup', `data-group="${group}"`)}</div>` : '';
        previousGroup = group;
        return heading + rows[i];
    }).join('');
    return head('Wettbewerbe', 'Die Wettbewerbsreihenfolge bestimmt den Vorrang eigener Spiele und die Reihenfolge innerhalb jeder Sportart.', button('Vereinsnamen verwalten', 'namesManage') + button('Wettbewerb hinzufügen', 'compNew', a.length >= limit ? 'disabled' : '', 'primary', a.length >= limit ? tf('Höchstens % Wettbewerbe. Entferne zuerst einen.', limit) : '') + (a.length > 1 ? button('Wettbewerbe über Verein gruppieren', 'clubGroup') : '')) + `<div class="list">${items || `<p class="card muted">${ht('Noch keine Wettbewerbe. Wähle Anbieter, Wettbewerb und optional einen Verein.')}</p>`}</div>`;
}
function providerDescription(i) {
    return [
        'Ohne Schlüssel, Spiele und Tabellen; keine Live-Minuten.', 'Schlüssel erforderlich, Spiele, Tabellen und Live-Angaben; Tagesbudget.', 'Inoffizielle Schnittstelle; Freigabe erforderlich, Live-Angaben je Wettbewerb.', 'Schlüssel erforderlich, Spiele und Tabellen; Live-Angaben je Tarif.'
    ][i];
}
function providerEnabled(i) {
    return i === 0 || i === 1 && S.saved.api_football_key_set || i === 2 && S.saved.espn_opt_in || i === 3 && S.saved.football_data_key_set;
}
let wizard = null;
function blankCompetition() { return clone(S.schema.competition_defaults); }
function validClubId(value) {
    return (typeof value === 'number' || typeof value === 'string' && /^\d+$/.test(value)) &&
        Number.isInteger(Number(value)) && Number(value) > 0 && Number(value) <= 4294967295;
}
function blankSource(provider = 0) {
    return {
        ...clone(S.schema.competition_defaults.primary), provider
    };
}
// A new competition appears on every screen (the firmware default).
function openWizard(index = null) {
    if (wizard) closeWizard(wizard);
    const entry = index === null ? blankCompetition() : clone(S.draft.competitions[index]);
    wizard = {
        index, entry, step: 0, noClub: index !== null && !entry.club.team, list: [], offset: 0, more: false, search: ['', ''], listSeason: '', country: S.schema.selection_country_default || '', busy: false, error: '', activeQuery: null, cancelled: false
    };
    drawWizard();
}
function drawWizard() {
    const w = wizard;
    if (!w)
        return;
    const source = w.entry.primary, steps = ['Anbieter', 'Wettbewerb', 'Verein', 'Zusammenfassung'];
    let body = `<div class="wizsteps">${steps.map((s, i) => `<span class="${w.step === i ? 'on' : w.step > i ? 'done' : ''}">${ht(s)}</span>`).join('')}</div>`;
    if (w.step === 0) {
        body += `<div class="choices">${PROVIDERS.map((name, i) => {
            const enabled = providerEnabled(i);
            return `<button class="choice ${enabled ? '' : 'dis'}" data-wprovider="${i}" ${enabled ? '' : 'disabled'}><input type="radio" ${source.provider === i ? 'checked' : ''}><b>${name}</b><small>${ht(providerDescription(i))}${!enabled ? ' ' + ht('Unter Einstellungen aktivieren und speichern.') : ''}</small></button>`;
        }).join('')}</div><p class="help">${ht('Unter Einstellungen aktivieren und speichern.')}</p>`;
    }
    else if (w.step === 1 || w.step === 2) {
        const teams = w.step === 2, current = teams ? w.entry.club.team || '' : source.competition;
        body += `<h3>${PROVIDERS[source.provider]} · ${ht(teams ? 'Verein' : 'Wettbewerb')}</h3><div class="cols"><div class="field"><label for="wizardSearch">${ht('Suche')}</label><div class="ctl"><input id="wizardSearch" type="search" class="w-l" value="${esc(w.search[w.step - 1])}" maxlength="${S.schema.selection_text_max}"></div><p class="help">${ht('Tippen filtert die geladene Liste; Enter oder „Suchen“ durchsucht die ganze Liste des Anbieters.')}</p></div>${!teams && source.provider === 0 ? `<div class="field"><label for="wizardListSeason">${ht('Saison')}</label><div class="ctl"><input id="wizardListSeason" class="w-s" inputmode="numeric" value="${esc(w.listSeason)}" placeholder="${ht('laufende')}" maxlength="${S.schema.text.competitions.season}"></div><p class="help">${ht('Leer: laufende Saison. Eine andere Saison wird mit dem Wettbewerb gespeichert.')}</p></div>` : ''}${!teams && source.provider === 1 ? `<div class="field"><label for="wizardCountry">${ht('Land')}</label><div class="ctl"><input id="wizardCountry" class="w-m" value="${esc(w.country)}" maxlength="${S.schema.selection_text_max}"></div></div>` : ''}</div><div class="row"><button class="btn" id="wizardLoad" ${w.busy ? 'disabled' : ''}>${w.busy ? ht('Daten werden geladen') : ht(w.search[w.step - 1] ? 'Suchen' : 'Listen laden')}</button>${w.more ? `<button class="btn" id="wizardMore" ${w.busy ? 'disabled' : ''}>${ht('Weitere laden')}</button>` : ''}</div>${w.busy && w.wait ? `<p class="help" id="wizardWait">${esc(selectionWait(w))}</p>` : ''}<div class="pick">${w.list.map((item, i) => `<button class="choice" data-wpick="${i}" ${w.busy ? 'disabled' : ''}><input type="radio" ${String(item.id) === String(current) && (teams || !source.season || String(item.season) === source.season) ? 'checked' : ''}><b>${esc(item.name || item.id)}</b><small>${esc(item.id)}${item.season ? ' · ' + esc(item.season) : ''}</small></button>`).join('') || `<p class="help">${ht(w.busy ? 'Daten werden geladen' : 'Keine Treffer.')}</p>`}<p class="help" id="wizardNoHit" hidden>${ht('Keine Treffer.')}</p></div>${teams ? `<div class="choices"><button class="choice" id="wizardNoClub" ${w.busy ? 'disabled' : ''}><input type="radio" ${w.noClub ? 'checked' : ''}><b>${ht('Ohne Verein')}</b><small>${ht('Kein eigenes Spiel; die Spiele erscheinen in der Konferenz, die Tabelle ohne markierte Zeile.')}</small></button></div>` : ''}<details class="expert"><summary>${ht('Experte')}</summary><div class="field"><label for="wizardId">${ht(teams ? 'Verein-ID' : 'Wettbewerb-ID')}</label><div class="ctl"><input id="wizardId" class="w-l" value="${esc(current)}" maxlength="${S.schema.text.competitions[teams ? 'team' : 'competition']}"></div><p class="help">${ht('IDs: ohne Steuerzeichen und ohne Leerzeichen am Anfang oder Ende.')}</p></div>${!teams && source.provider === 0 ? '' : `<div class="field"><label for="wizardSeason">${ht('Saison überschreiben')}</label><div class="ctl"><input id="wizardSeason" class="w-m" value="${esc(source.season)}" maxlength="${S.schema.text.competitions.season}"></div><p class="help">${ht('Leer: laufende Saison, das Gerät ermittelt sie selbst.')}</p></div>`}</details>`;
    }
    else {
        body += `<div class="card"><h3>${esc(w.entry.competition_name || source.competition)}</h3><p>${w.entry.club.team ? esc(w.entry.club.name || w.entry.club.team) : ht('Ohne Verein')}</p><p class="muted">${PROVIDERS[source.provider]} · ${ht('Saison')}: ${esc(source.season || t('Automatisch'))}</p></div>`;
        if (w.entry.club.group)
            body += `<div class="notice warn"><b>${ht('Vereinsgruppe')}: ${esc(groupName(w.entry.club.group))}</b><p>${ht('Diese Zuordnung bleibt bei einem Vereinswechsel bestehen. Prüfe die zusammengehörigen Wettbewerbe vor dem Übernehmen.')}</p>${button('Aus Gruppe lösen', 'wizardUngroup')}</div>`;
        body += `<div class="cols">${[
            ['competition_name', w.entry.competition_name, 'Wettbewerb'],
            ...(w.entry.club.team ? [['club_name', w.entry.club.name, 'Verein']] : [])
        ].map(([key, value, label]) => `<div class="field"><label for="wizard-${key}">${ht(label)}</label><div class="ctl"><input id="wizard-${key}" class="w-l" value="${esc(value)}" maxlength="${S.schema.text.competitions[key]}"></div><p class="help">${esc(tf('Höchstens % Byte UTF-8', S.schema.text.competitions[key]))}</p></div>`).join('')}</div>`;
    }
    body += `<p class="dlg-err" id="wizardError">${esc(w.error)}</p>`;
    const d = dialog(t(w.index === null ? 'Wettbewerb hinzufügen' : 'Bearbeiten'), body, `<span class="why" id="wizardWhy"></span>` + button('Abbrechen', 'close') + button('Zurück', 'wizardBack', w.busy || w.step === 0 ? 'disabled' : '') + button(w.step === 3 ? 'Übernehmen' : 'Weiter', 'wizardNext', w.busy ? 'disabled' : '', 'primary'), true);
    const close = () => closeWizard(w);
    d.querySelector('[data-close]').onclick = close;
    d.querySelector('[data-act=close]').onclick = close;
    d.oncancel = close;
    d.querySelector('[data-act=wizardBack]').onclick = () => {
        w.step--;
        w.list = [];
        w.offset = 0;
        w.more = false;
        w.error = '';
        drawWizard();
    };
    d.querySelector('[data-act=wizardNext]').onclick = () => wizardNext();
    d.querySelectorAll('[data-wprovider]').forEach(el => el.onclick = () => {
        source.provider = Number(el.dataset.wprovider);
        source.competition = '';
        w.entry.club.team = 0;
        source.season = '';
        w.listSeason = '';
        w.entry.competition_name = '';
        w.entry.club.name = '';
        w.search = ['', ''];
        w.step = 1;
        w.list = [];
        w.offset = 0;
        w.more = false;
        w.error = '';
        drawWizard();
    });
    d.querySelectorAll('[data-wpick]').forEach(el => el.onclick = () => {
        const item = w.list[Number(el.dataset.wpick)];
        if (w.step === 1) {
            if (source.competition !== item.id) {
                w.entry.club.team = 0;
                w.entry.club.name = '';
            }
            source.competition = item.id;
            if (source.provider === 1)
                w.teamSeason = {
                    competition: item.id, season: String(item.season || '')
                };
            source.season = source.provider === 0 && w.listSeason && String(item.season) === w.listSeason ? String(item.season || '') : source.provider <= 1 ? '' : source.season;
            w.entry.competition_name = item.name || item.id;
        }
        else {
            w.entry.club.team = Number(item.id);
            w.noClub = false;
            w.entry.club.name = item.name || item.id;
        }
        w.error = '';
        drawWizard();
    });
    if ($('wizardSearch')) {
        $('wizardSearch').oninput = e => {
            w.search[w.step - 1] = e.target.value;
            wizardFilter();
        };
        $('wizardSearch').onkeydown = e => {
            if (e.key === 'Enter') {
                e.preventDefault();
                runSelection(false);
            }
        };
    }
    if ($('wizardCountry'))
        $('wizardCountry').oninput = e => {
            w.country = e.target.value;
        };
    if ($('wizardId'))
        $('wizardId').oninput = e => {
            const key = w.step === 2 ? 'team' : 'competition';
            if (key === 'competition' && source.competition !== e.target.value) {
                w.entry.club.team = 0;
                w.entry.competition_name = '';
                w.entry.club.name = '';
            }
            if (key === 'team')
                w.entry.club.team = e.target.value;
            else
                source[key] = e.target.value;
            if (key === 'team') {
                w.noClub = false;
                w.entry.club.name = '';
            }
            wizardCheck();
        };
    if ($('wizardSeason'))
        $('wizardSeason').oninput = e => {
            source.season = e.target.value;
            wizardCheck();
        };
    if ($('wizardListSeason'))
        $('wizardListSeason').oninput = e => {
            w.listSeason = e.target.value.trim();
        };
    if ($('wizardLoad'))
        $('wizardLoad').onclick = () => runSelection(false);
    if ($('wizardMore'))
        $('wizardMore').onclick = () => runSelection(true);
    if ($('wizardNoClub'))
        $('wizardNoClub').onclick = () => {
            w.noClub = true;
            w.entry.club.team = 0;
            w.entry.club.name = '';
            w.error = '';
            drawWizard();
        };
    d.querySelector('[data-act=wizardUngroup]')?.addEventListener('click', () => {
        w.entry.club.group = 0;
        drawWizard();
    });
    for (const key of ['competition_name', 'club_name'])
        if ($('wizard-' + key))
            $('wizard-' + key).oninput = e => {
                if (key === 'club_name')
                    w.entry.club.name = e.target.value;
                else
                    w.entry.competition_name = e.target.value;
            };
    wizardFilter();
    wizardCheck();
    if (w.busy)
        d.querySelectorAll('input,select,button:not([data-close]):not([data-act=close])').forEach(el => el.disabled = true);
}
// Reason why "Weiter" is not possible yet; the same checks run in wizardNext.
function wizardWhy() {
    const w = wizard, s = w.entry.primary;
    if (w.step === 0 && !providerEnabled(s.provider))
        return t('Wähle einen aktivierten Anbieter.');
    if (w.step === 1 || w.step === 2) {
        const key = w.step === 1 ? 'competition' : 'team';
        if (key === 'team' && w.noClub)
            return '';
        if (key === 'team' ? !validClubId(w.entry.club.team) : !s[key] || !validId(s[key]))
            return t(w.step === 1 ? 'Wähle zuerst einen Wettbewerb aus der Liste oder gib unter „Experte“ eine ID ein.' : 'Wähle zuerst einen Verein aus der Liste oder gib unter „Experte“ eine ID ein.');
    }
    return '';
}
function wizardCheck() {
    const b = document.querySelector('#dialog [data-act=wizardNext]'), why = wizard?.busy ? '' : wizardWhy();
    if (!b)
        return;
    b.disabled = !!wizard?.busy || !!why;
    b.title = why;
    if ($('wizardWhy'))
        $('wizardWhy').textContent = why;
}
function wizardNext() {
    const w = wizard, s = w.entry.primary;
    w.error = '';
    if (w.step === 0) {
        if (!providerEnabled(s.provider)) {
            w.error = t('Unter Einstellungen aktivieren und speichern.');
            drawWizard();
            return;
        }
        w.listSeason = s.provider === 0 ? s.season : '';
        w.step = 1;
    }
    else if (w.step === 1 || w.step === 2) {
        const key = w.step === 1 ? 'competition' : 'team';
        if (!(key === 'team' && w.noClub) && (key === 'team' ? !validClubId(w.entry.club.team) : !s[key] || !validId(s[key]))) {
            w.error = t('IDs: ohne Steuerzeichen und ohne Leerzeichen am Anfang oder Ende.');
            drawWizard();
            return;
        }
        w.step++;
    }
    else {
        const index = w.index === null ? S.draft.competitions.length : w.index, m = clone(S.draft);
        w.entry.club.team = Number(w.entry.club.team);
        m.competitions[index] = clone(w.entry);
        const problems = validate(m).filter(e => e.path.startsWith(`competitions[${index}]`));
        if (problems.length) {
            w.error = problems.map(e => labelOf(e.path) + ': ' + e.message).join(' ');
            drawWizard();
            return;
        }
        S.draft.competitions[index] = clone(w.entry);
        prepareScreenModel(S.draft);
        closeWizard(w);
        render();
        return;
    }
    w.list = [];
    w.offset = 0;
    w.more = false;
    drawWizard();
}
// Hides loaded entries whose name does not contain the search text; the
// device search covers entries that are not loaded.
function wizardFilter() {
    const w = wizard;
    if (!w || !$('wizardSearch'))
        return;
    const q = w.search[w.step - 1].toLowerCase();
    let shown = 0;
    document.querySelectorAll('[data-wpick]').forEach(el => {
        const item = w.list[Number(el.dataset.wpick)], hit = !q || String(item?.name || item?.id || '').toLowerCase().includes(q);
        el.hidden = !hit;
        shown += hit;
    });
    if ($('wizardNoHit'))
        $('wizardNoHit').hidden = shown > 0 || !w.list.length;
    if ($('wizardLoad') && !w.busy)
        $('wizardLoad').textContent = t(q ? 'Suchen' : 'Listen laden');
}
// The selection waits for its provider's request limit: until when, and the code of the cause.
function selectionWait(w) {
    const d = new Date(w.wait.until), z = n => String(n).padStart(2, '0');
    return tf('Wartet auf Anfragelimit bis %', z(d.getHours()) + ':' + z(d.getMinutes()) + ':' + z(d.getSeconds())) + ' (Code ' + w.wait.code + ')';
}
// Closing an editor cancels only its own correlated query. A lost cancellation
// reply cannot modify a later query; the server's idle-read lease also expires.
async function cancelWizardQuery(w) {
    const query = w.activeQuery;
    w.activeQuery = null;
    if (!query) return;
    try {
        // Do not open a login dialog after the owner has closed this one.
        await raw('data/select/cancel', {query}, 'POST');
    } catch (error) {
        if (error.code !== 'changed') toast(error.message, true);
    }
}
function closeWizard(w = wizard) {
    if (!w || wizard !== w) return;
    w.cancelled = true;
    wizard = null;
    void cancelWizardQuery(w);
    $('dialog').close();
}
async function awaitSelection(query, current, onWait) {
    // Allow season discovery, list retrieval and intervening automatic work
    // using the device's published per-request deadline.
    const allowance = S.schema.operation_deadline_ms * 5;
    let deadline = Date.now() + allowance;
    while (current()) {
        const data = await raw('selection?query=' + query);
        if (!current()) return null;
        if (data.query !== query) throw Error(t(ERROR.changed));
        if (!data.busy) return data;
        if (data.wait_s > 0)
            deadline = Math.max(deadline, Date.now() + data.wait_s * 1000 + allowance);
        onWait?.(data);
        if (Date.now() > deadline)
            throw Error(t('Zeitüberschreitung. Erneut versuchen.'));
        await sleep(750);
    }
    return null;
}
async function startSelection(body, current) {
    for (let retry = 0; retry < 3 && current(); retry++) {
        try {
            const receipt = await mutate('data/select', body);
            if (typeof receipt.query !== 'string' || !/^[0-9a-f]{16}$/.test(receipt.query) || /^0+$/.test(receipt.query))
                throw Error(t('Antwort des Geräts ist nicht lesbar.'));
            return receipt.query;
        } catch (error) {
            // Only an explicit busy rejection is safe to retry. Never repeat
            // an unacknowledged start or inspect another browser's query.
            if (error.status !== 409 || error.code !== 'busy') throw error;
            if (retry === 2) throw error;
            await sleep(1000);
        }
    }
    return null;
}
async function runSelection(more) {
    const w = wizard;
    if (!w || w.busy)
        return;
    const s = w.entry.primary, teams = w.step === 2, offset = more ? w.offset : 0; // More pages continue the query of the loaded list, not the edited fields.
    const query = more && w.query ? w.query : {
        provider: s.provider, teams, competition: s.competition, season: !teams && s.provider === 0 ? w.listSeason : s.season || (s.provider === 1 && w.teamSeason?.competition === s.competition ? w.teamSeason.season : ''), search: w.search[w.step - 1], country: w.country
    };
    if (bytes(query.search) > S.schema.selection_text_max || bytes(query.country) > S.schema.selection_text_max) {
        w.error = t('Text ist zu lang');
        drawWizard();
        return;
    }
    if (teams && !query.competition) {
        w.error = t('Wert wird benötigt');
        drawWizard();
        return;
    }
    w.busy = true;
    w.error = '';
    drawWizard();
    try {
        const current = () => wizard === w && !w.cancelled;
        const id = await startSelection({...query, offset}, current);
        if (!id) return;
        w.activeQuery = id;
        if (!current()) { await cancelWizardQuery(w); return; }
        const result = await awaitSelection(id, current, data => {
            const wait = data.wait_s > 0 ? {
                code: data.wait, until: Date.now() + data.wait_s * 1000
            } : null;
            if (wizard === w && (wait?.code !== w.wait?.code || Math.abs((wait?.until || 0) - (w.wait?.until || 0)) > 2000)) {
                w.wait = wait;
                drawWizard();
            }
        });
        if (!result) return;
        w.activeQuery = null;
        if (result.error_code)
            throw Error(causeMessage(result.error_code, result.code));
        if (wizard !== w)
            return;
        const rows = result.items || [];
        w.list = more ? [...w.list, ...rows.filter(item => !w.list.some(x => x.id === item.id && x.season === item.season))] : rows;
        w.query = query;
        w.offset = offset + rows.length;
        w.more = result.more === true;
    }
    catch (e) {
        if (wizard === w)
            w.error = e.message;
    }
    finally {
        if (w.activeQuery) await cancelWizardQuery(w);
        w.busy = false;
        w.wait = null;
        if (wizard === w) drawWizard();
    }
}
function moveCompetition(index, dir) {
    const next = index + dir, a = S.draft.competitions;
    if (next < 0 || next >= a.length)
        return;
    [a[index], a[next]] = [a[next], a[index]];
    prepareScreenModel(S.draft);
    if (S.open === index)
        S.open = next;
    else if (S.open === next)
        S.open = index;
    render();
}
function removeCompetition(index) {
    const c = S.draft.competitions[index];
    S.draft.competitions.splice(index, 1);
    prepareScreenModel(S.draft);
    S.open = null;
    render();
}
// Group membership is an explicit competition preference, independent of text.
function groupName(group, model = S.draft) {
    const first = model.competitions.find(c => c.club.group === group);
    return first ? first.club.name || String(first.club.team) : '';
}
function competitionOrder(model = S.draft) {
    const order = [], seen = new Set(), entries = model.competitions;
    for (let i = 0; i < entries.length; ++i) {
        if (seen.has(i))
            continue;
        const group = entries[i].club.group;
        for (let member = i; member < entries.length; ++member)
            if (!seen.has(member) && (member === i || group && entries[member].club.group === group)) {
                order.push(member);
                seen.add(member);
            }
    }
    return order;
}
function applyClubGroup(group, members) {
    const rows = S.draft.competitions;
    if (members.length < 2 || new Set(members).size !== members.length ||
        members.some(i => !Number.isInteger(i) || !rows[i]?.club.team))
        throw Error(t('Mindestens zwei Wettbewerbe mit ausgewähltem Verein wählen.'));
    if (!group) {
        const used = new Set(rows.map(c => c.club.group));
        for (let id = 1; id <= S.schema.limits.competitions; ++id)
            if (!used.has(id)) {
                group = id;
                break;
            }
    }
    if (!group)
        throw Error(t('Zu viele Einträge'));
    rows.forEach((row, index) => {
        if (members.includes(index))
            row.club.group = group;
        else if (row.club.group === group)
            row.club.group = 0;
    });
    return group;
}
function openClubGroup(group = 0) {
    const rows = S.draft.competitions;
    const d = dialog(t('Wettbewerbe über Verein gruppieren'), `<p>${ht('Wähle die Wettbewerbe desselben Vereins ausdrücklich aus. Die Gruppe erhält die höchste Priorität ihrer Mitglieder; die Mitglieder behalten ihre Reihenfolge. Namen verändern keine Gruppierung.')}</p><div class="choices">${rows.map((c, i) => `<label class="choice"><input type="checkbox" data-group-member="${i}" ${group && c.club.group === group ? 'checked' : ''} ${c.club.team ? '' : 'disabled'}><span><b>${esc(compName(c))}</b><small>${esc(c.club.team ? c.club.name || c.club.team : t('Ohne Verein'))}${c.club.group ? ' · ' + esc(groupName(c.club.group)) : ''}</small></span></label>`).join('')}</div><p class="help">${ht('Die Auswahl ändert die Gruppen im Entwurf. Übernehmen bestätigt den Entwurf; erst Speichern auf der Seite übernimmt ihn auf dem Gerät.')}</p><p id="groupError" class="dlg-err"></p>`, button('Abbrechen', 'close') + (group ? button('Gruppe auflösen', 'groupDissolve', '', 'danger') : '') + button('Übernehmen', 'groupApply', '', 'primary'));
    d.querySelector('[data-act=close]').onclick = () => d.close();
    d.querySelector('[data-act=groupApply]').onclick = () => {
        const members = [...d.querySelectorAll('[data-group-member]:checked')].map(el => Number(el.dataset.groupMember));
        try {
            applyClubGroup(group, members);
            d.close();
            render();
        }
        catch (error) {
            d.querySelector('#groupError').textContent = error.message;
        }
    };
    const dissolve = d.querySelector('[data-act=groupDissolve]');
    if (dissolve)
        dissolve.onclick = () => {
            rows.forEach(c => { if (c.club.group === group)
                c.club.group = 0; });
            d.close();
            render();
        };
}
