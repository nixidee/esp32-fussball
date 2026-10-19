// Screen-policy draft bindings, validation, display settings and controls.
// One aggregate draft owns shared rules and competition-attached overrides.
// Browser keys never leave this document.
const SP_TYPES = [
    'single', 'conference', 'table', 'crest', 'matchday', 'images'
], SP_SPORTS = ['single', 'conference', 'table', 'matchday'], SP_PHASES = ['own', 'other', 'matchday', 'idle', 'break'];
const SP_TYPE_LABEL = {
    single: 'Einzelspiel', conference: 'Konferenz', table: 'Tabelle', crest: 'Wappen', matchday: 'Spieltag', images: 'Bilder'
}, SP_PHASE_LABEL = {
    own: 'Eigenes Spiel', other: 'Andere Spiele', matchday: 'Spieltag', idle: 'Ruhe', break: 'Saisonpause'
};
const SP_NUMBERS = [
    'break_days', 'own_before_min', 'own_after_min', 'other_before_min', 'other_after_min', 'manual_return_s', 'per_image_s', 'visible_matches', 'table_window', 'conference_mode', 'matchday_mode'
], SP_BOOLEANS = ['backgrounds', 'pin_own_match', 'frame', 'frame_crest', 'automatic_break', 'only_one_own_single'];
let screenDraftSerial = 0;
function newScreenKey() {
    return 'draft-' + (++screenDraftSerial);
}
function screenKey(c) {
    return c._screenKey || (c.id && c.incarnation ? 'stored-' + c.id + '-' + c.incarnation : newScreenKey());
}
function blankCompetitionDisplay() {
    return Object.fromEntries(SP_PHASES.map(phase => [phase,
        Object.fromEntries(SP_SPORTS.map(type => [type, [null, null, null]]))]));
}
function prepareScreenModel(m) {
    for (const c of m.competitions || []) {
        c._screenKey = screenKey(c);
        c.display ||= blankCompetitionDisplay();
    }
    return m;
}
function screenPayload(m = S.draft) {
    return m.screen_policy ? clone(m.screen_policy) : null;
}
function competitionPayload(rows) {
    return rows.map(c => {
        const row = clone(c);
        delete row._screenKey;
        return row;
    });
}
function screenConflict() {
    const e = Error(t('Konfiguration wurde zwischenzeitlich geändert. Status aktualisieren und den Entwurf prüfen. [WEB S01]'));
    e.field = 'screen_policy';
    return e;
}
function screenRow(i, m = S.draft) { return m.competitions[i]?.display; }
function screenRowPath(i) { return 'competitions[' + i + '].display'; }
function spEffective(row, phase, type) {
    const shared = S.draft.screen_policy.phases[phase][type];
    return row[phase][type].map((v, i) => v === null ? shared[i] : v);
}
function validateScreenPolicy(p, competitions, internal = false) {
    const issues = [], add = (path, text) => issues.push({
        path: 'screen_policy' + (path ? '.' + path : ''), message: t(text)
    }), sc = S.schema.screen_policy;
    if (!sc) {
        add('', 'Antwort des Geräts ist nicht lesbar.');
        return issues;
    }
    const object = (v, path, required, extra = []) => {
        if (!v || typeof v !== 'object' || Array.isArray(v)) {
            add(path, 'Falscher Datentyp');
            return false;
        }
        for (const k of Object.keys(v))
            if (!required.includes(k) && !extra.includes(k))
                add(path ? path + '.' + k : k, 'Unbekanntes Feld');
        for (const k of required)
            if (!(k in v))
                add(path ? path + '.' + k : k, 'Wert wird benötigt');
        return true;
    }, arr = (v, path, max, exact = false) => {
        if (!Array.isArray(v)) {
            add(path, 'Falscher Datentyp');
            return false;
        }
        if (v.length > max)
            add(path, 'Zu viele Einträge');
        else if (exact && v.length !== max)
            add(path, 'Wert wird benötigt');
        return true;
    }, number = (v, path, bounds) => {
        if (!Number.isInteger(v))
            add(path, 'Falscher Datentyp');
        else if (v < bounds[0] || v > bounds[1])
            add(path, 'Wert außerhalb des erlaubten Bereichs');
    }, bool = (v, path) => {
        if (typeof v !== 'boolean')
            add(path, 'Falscher Datentyp');
    }, tuple = (v, path, nullable) => {
        if (!arr(v, path, 3, true))
            return;
        v.forEach((x, i) => {
            if (nullable && x === null)
                return;
            if (i < 2)
                bool(x, path + '[' + i + ']');
            else
                number(x, path + '[2]', sc.bounds.visit_s);
        });
    };
    if (!object(p, '', [
        ...SP_NUMBERS, ...SP_BOOLEANS, 'order', 'phases', 'appearance', 'slides'
    ]))
        return issues;
    for (const k of SP_NUMBERS)
        number(p[k], k, sc.bounds[k] || sc.bounds[k.endsWith('_min') ? 'buffer_min' : k]);
    for (const k of SP_BOOLEANS)
        bool(p[k], k);
    if (arr(p.order, 'order', 6, true)) {
        const seen = new Set();
        p.order.forEach((x, i) => {
            if (!SP_TYPES.includes(x))
                add('order[' + i + ']', 'Ungültiger Wert');
            if (seen.has(x))
                add('order[' + i + ']', 'Doppelter Eintrag');
            seen.add(x);
        });
    }
    if (object(p.phases, 'phases', SP_PHASES))
        for (const phase of SP_PHASES) {
            const r = p.phases[phase], at = 'phases.' + phase;
            if (!object(r, at, [...SP_TYPES, 'per_competition']))
                continue;
            for (const type of SP_TYPES) {
                tuple(r[type], at + '.' + type, false);
                if (!Array.isArray(r[type]))
                    continue;
                if (!SP_SPORTS.includes(type) && r[type][0] === true && r[type][1] !== true)
                    add(at + '.' + type + '[1]', 'Bilder und Wappen laufen bei Freigabe automatisch.');
                if (type === 'images' && r[type][0] === true && Number.isInteger(r[type][2]) && Number.isInteger(p.per_image_s) && p.per_image_s > 0 && r[type][2] % p.per_image_s !== 0)
                    add(at + '.' + type + '[2]', 'Die Besuchszeit muss ein ganzzahliges Vielfaches der Bildwechselzeit sein.');
            }
            if (arr(r.per_competition, at + '.per_competition', 4)) {
                const seen = new Set();
                r.per_competition.forEach((x, i) => {
                    if (!SP_SPORTS.includes(x))
                        add(at + '.per_competition[' + i + ']', 'Ungültiger Wert');
                    if (seen.has(x))
                        add(at + '.per_competition[' + i + ']', 'Doppelter Eintrag');
                    seen.add(x);
                });
            }
        }
    if (object(p.appearance, 'appearance', SP_TYPES))
        for (const type of SP_TYPES) {
            const a = p.appearance[type], at = 'appearance.' + type;
            if (!object(a, at, [
                'colour', 'text', 'accent', 'text_scale', 'image', 'background', 'dots'
            ]))
                continue;
            for (const k of ['colour', 'text', 'accent'])
                number(a[k], at + '.' + k, [0, 16777215]);
            number(a.text_scale, at + '.text_scale', sc.bounds.text_scale);
            number(a.image, at + '.image', type === 'crest' ? [0, 0] : sc.bounds.image);
            bool(a.background, at + '.background');
            bool(a.dots, at + '.dots');
        }
    if (arr(p.slides, 'slides', sc.max_slides))
        p.slides.forEach((id, i) => number(id, 'slides[' + i + ']', [1, sc.bounds.image[1]]));
    return issues;
}
function screenBound(path) {
    const bounds = S.schema.screen_policy.bounds, key = parts(path).at(-1);
    return key === '2' ? bounds.visit_s : bounds[key] || (key.endsWith('_min') ? bounds.buffer_min : null);
}
function screenField(path, type = 'number', help = '', options = null, disabled = false, label = null) {
    const bound = screenBound(path), key = parts(path).at(-1);
    return `<div class="policy-control">${draftField(path, type, help, options, disabled, label, {bound, unit: key === '2' ? 's' : FIELD_UNITS[key] || ''})}</div>`;
}
const SP_VISIT_HELP = 'Besuchszeit in Sekunden: 15 bis 43.200. Sportansichten erhalten mindestens 15 Sekunden insgesamt und 5 Sekunden je Unterseite. Auch bei nur manuell erreichbaren Sporteinträgen wirkt die gespeicherte Zeit, bis du selbst Auf/Ab benutzt.', SP_AUTO_HELP = 'Automatisch: wird in dieser Phase in der gemeinsamen Artenfolge besucht. Ausgeschaltet bleibt ein freigegebener Sporteintrag über Weiter/Zurück erreichbar. Wappen und Bilder werden dabei übersprungen. Die gespeicherte Besuchszeit gilt auch für automatische Unterseiten eines nur manuell erreichbaren Eintrags, bis du selbst Auf/Ab benutzt.', SP_ENABLED_HELP = 'Freigabe für diese Phase. Ausgeschaltete Einträge erscheinen weder automatisch noch manuell. Gespeicherte Zeiten und Abweichungen bleiben erhalten.';
function screenDisclosureHead(key, title) {
    return `<details class="expert" data-policy-block="${key}" ${S.policyOpen?.has(key) ? 'open' : ''}><summary>${ht(title)}</summary>`;
}
function screenPhaseMarkup() {
    const p = S.draft.screen_policy, phase = S.screenPhase || 'own', prefix = 'screen_policy.phases.' + phase;
    return `<div class="policy-tabs" role="tablist" aria-label="${ht('Phase')}">${SP_PHASES.map(k => `<button class="btn ${k === phase ? 'primary' : ''}" role="tab" aria-selected="${k === phase}" data-act="policyPhase" data-phase="${k}">${ht(SP_PHASE_LABEL[k])}</button>`).join('')}</div><div class="policy-rules">${p.order.map(type => {
        const row = p.phases[phase][type], path = prefix + '.' + type, sport = SP_SPORTS.includes(type), per_competition = p.phases[phase].per_competition.includes(type);
        return `<div class="card policy-rule" data-field="${path}"><div class="policy-rule-main"><h3>${ht(SP_TYPE_LABEL[type])}</h3>${screenField(path + '[0]', 'switch', SP_ENABLED_HELP, null, false, 'Freigeben')}${sport && row[0] ? screenField(path + '[1]', 'switch', SP_AUTO_HELP, null, false, 'Automatisch') : ''}${row[0] && row[1] ? screenField(path + '[2]', 'number', type === 'images' ? 'Besuch der Bilderfolge: 15 bis 43.200 Sekunden und ein ganzzahliges Vielfaches der Bildwechselzeit. Die Bildwechselzeit gilt getrennt.' : SP_VISIT_HELP, null, false, 'Besuchszeit') : ''}</div>${sport ? `<details class="policy-overrides" data-policy-block="${phase}-${type}" ${S.policyOpen?.has(phase + '-' + type) ? 'open' : ''}><summary>${ht('Abweichungen je Wettbewerb')}${per_competition ? ' · ' + ht('Aktiv') : ''}</summary><div class="row">${button(per_competition ? 'Gemeinsame Regeln verwenden' : 'Je Wettbewerb aufteilen', 'policySplit', `data-phase="${phase}" data-type="${type}"`)}<button class="btn ghost policy-help" data-act="policyHelp" data-help="${esc('Aufteilen erlaubt eigene Freigabe, Automatik und Besuchszeit je Wettbewerb. „Gemeinsam“ erbt dieses eine Feld. Zurück zu gemeinsamen Regeln schaltet Abweichungen nur aus; ihre gespeicherten Werte bleiben erhalten.')}" aria-label="${ht('Hilfe')}">?</button></div>${per_competition ? S.draft.competitions.map((c, i) => screenOverrideMarkup(i, phase, type)).join('') || `<p class="help">${ht('Zuerst einen Wettbewerb hinzufügen.')}</p>` : ''}</details>` : ''}<div class="field-error" data-error="${path}"></div></div>`;
    }).join('')}</div>`;
}
function screenOverrideMarkup(i, phase, type) {
    const c = S.draft.competitions[i], r = screenRow(i), path = screenRowPath(i) + '.' + phase + '.' + type, effective = spEffective(r, phase, type);
    return `<div class="policy-competition" data-field="${path}"><b>${esc(compName(c))}</b><div class="policy-override-fields">${[0, 1, 2].map(slot => {
        if (slot === 1 && !effective[0] || slot === 2 && (!effective[0] || !effective[1]))
            return '';
        const inherited = r[phase][type][slot] === null, label = ['Freigeben', 'Automatisch', 'Besuchszeit'][slot], field = path + '[' + slot + ']', selection = slot < 2 ? screenField(field, 'select', slot ? SP_AUTO_HELP : SP_ENABLED_HELP, [[null, t('Gemeinsam')], [true, t('Ja')], [false, t('Nein')]], false, label) : `${screenField(field, 'number', SP_VISIT_HELP, null, inherited, label)}<label class="switch"><input type="checkbox" data-inherit="${field}" ${inherited ? 'checked' : ''} ${S.saving ? 'disabled' : ''}><span class="track"></span>${ht('Gemeinsam')}</label>`;
        return `<div class="policy-override-field">${selection}<span class="help">${ht('Wirksam')}: ${slot === 2 ? effective[slot] + ' s' : ht(effective[slot] ? 'Ja' : 'Nein')}</span></div>`;
    }).join('')}</div><div class="field-error" data-error="${path}"></div></div>`;
}
function screenOrderMarkup() {
    const a = S.draft.screen_policy.order;
    return `<div class="list policy-order">${a.map((type, i) => `<div class="row"><span class="pos">${i + 1}</span><b>${ht(SP_TYPE_LABEL[type])}</b>${button('Nach oben', 'policyOrder', `data-i="${i}" data-dir="-1" ${i === 0 ? 'disabled' : ''}`, 'icon')}${button('Nach unten', 'policyOrder', `data-i="${i}" data-dir="1" ${i === a.length - 1 ? 'disabled' : ''}`, 'icon')}</div>`).join('')}</div>`;
}
function screenAppearanceMarkup() {
    const p = S.draft.screen_policy;
    return `<div class="policy-tabs" role="tablist" aria-label="${ht('Bildschirmart')}">${p.order.map(type => `<button class="btn ${(S.screenAppearance || 'single') === type ? 'primary' : ''}" role="tab" aria-selected="${(S.screenAppearance || 'single') === type}" data-act="policyAppearance" data-type="${type}">${ht(SP_TYPE_LABEL[type])}</button>`).join('')}</div>` + (() => {
        const type = S.screenAppearance || 'single', path = 'screen_policy.appearance.' + type, s = p.appearance[type];
        return `<div class="cols">${SP_SPORTS.includes(type) ? screenField(path + '.background', 'switch', 'Hintergrundbild für diese Ansicht. Die gemeinsame Hintergrund-Freigabe gilt zusätzlich; die Bildauswahl bleibt beim Ausschalten erhalten.', null, !p.backgrounds, 'Hintergrundbild') + screenField(path + '.image', 'select', 'Bild aus der Bibliothek oder der gemeinsame Standard-Hintergrund.', imageOptions('Standard-Hintergrund', s.image), !p.backgrounds || !s.background, 'Bild') : ''}${screenField(path + '.colour', 'colour', 'Grundfarbe hinter den Inhalten.', null, false, 'Grundfarbe')}${screenField(path + '.text', 'colour', 'Farbe der Texte und Ergebnisse.', null, false, 'Text')}${screenField(path + '.accent', 'colour', 'Farbe für Hervorhebungen und den eigenen Verein.', null, false, 'Akzent')}${screenField(path + '.text_scale', 'number', 'Skaliert Texte innerhalb der vorhandenen Schriftgrößen. Die tatsächliche Lesbarkeit wird auf dem Gerät geprüft.', null, false, 'Textgröße')}${screenField(path + '.dots', 'switch', 'Zeigt Punkte für vorhandene Unterseiten dieser Bildschirmart. Die Punkte stellen keine Wettbewerbe oder automatische Artenfolge dar.', null, false, 'Unterseitenpunkte')}</div>`;
    })();
}
function screenSlidesMarkup() {
    const p = S.draft.screen_policy, full = p.slides.length >= S.schema.screen_policy.max_slides, rows = p.slides.map((id, i) => `<div class="slide" data-field="screen_policy.slides[${i}]"><div class="th">${thumb(id)}</div><span>${i + 1}. ${esc(imageName(id))}</span><div class="row">${button('Nach oben', 'slideMove', `data-i="${i}" data-dir="-1" ${i === 0 ? 'disabled' : ''}`, 'icon')}${button('Nach unten', 'slideMove', `data-i="${i}" data-dir="1" ${i === p.slides.length - 1 ? 'disabled' : ''}`, 'icon')}${button('Entfernen', 'slideDelete', `data-i="${i}"`, 'icon')}</div></div>`).join('');
    return `<div class="slides">${rows || `<p class="help">${ht('Keine Bibliotheksbilder ausgewählt. Das Wappen ist eine eigene Bildschirmart.')}</p>`}</div><div class="ctl"><select id="slideAdd" class="w-m" ${full || !S.images?.items?.length ? 'disabled' : ''}>${opts(imageOptions().slice(1), S.images?.items?.[0]?.id)}</select>${button('Zur Diashow hinzufügen', 'slideAdd', full || !S.images?.items?.length ? 'disabled' : '')}</div>`;
}
function screenOptionsMarkup() {
    const q = 'screen_policy.', p = S.draft.screen_policy;
    return section('Ablauf nach Spielsituation', 'Eine gemeinsame Reihenfolge, fünf Phasen. Es gilt die höchste passende Phase: eigenes Spiel, andere Spiele, Spieltag, Ruhe, optionale Saisonpause.', screenPhaseMarkup()) + `${screenDisclosureHead('order', 'Gemeinsame Reihenfolge')}${screenOrderMarkup()}<p class="help">${ht('Automatische Einträge folgen dieser Reihenfolge; Wettbewerbe innerhalb einer Sportart folgen ihrer Reihenfolge unter Wettbewerbe. Phasenwechsel und Rückkehr zur Automatik starten beim ersten geeigneten Eintrag.')}</p></details>` + `${screenDisclosureHead('timing', 'Zeiten und Sportansichten')}<div class="cols">${screenField(q + 'only_one_own_single', 'switch', 'Begrenzt eigene Einzelspiele auf den fokussierten Wettbewerb. Ausgeschaltet laufen weitere eigene Spiele nach der Wettbewerbs- und Gruppenpriorität in derselben Folge.', null, false, 'Nur ein eigenes Einzelspiel')}${screenField(q + 'manual_return_s', 'number', 'Nach dem letzten Tastendruck zurück zur Automatik. 0 hält die manuelle Anzeige dauerhaft; auch im Bilderrahmen. Im Bilderrahmen beginnt danach das betrachtete Bild mit voller Bildzeit.', '', false, 'Manuelle Rückkehrzeit')}${['own_before_min', 'own_after_min', 'other_before_min', 'other_after_min'].map(k => screenField(q + k, 'number', 'Vor- und Nachlauf in Minuten: 0 bis 180. Eigene Sonderbehandlung gilt nur bei ausgewähltem Verein und eingeschalteter Sonderbehandlung dieses Wettbewerbs.', null, false, {
        own_before_min: 'Eigene Spiele: Vorlauf', own_after_min: 'Eigene Spiele: Nachlauf', other_before_min: 'Andere Spiele: Vorlauf', other_after_min: 'Andere Spiele: Nachlauf'
    }[k])).join('')}${screenField(q + 'automatic_break', 'switch', 'Verwendet die Saisonpausen-Phase nur bei einem vollständigen, frischen Nachweis aller aktiven Wettbewerbe. Unbekannte oder unvollständige Abdeckung beweist keine Pause.', null, false, 'Automatische Saisonpause')}${p.automatic_break ? screenField(q + 'break_days', 'number', 'Die nachgewiesene Zeitspanne zwischen Anstößen muss größer als diese Zahl mal 24 Stunden sein. Sommerzeit ändert die Grenze nicht.', null, false, 'Pausenschwelle in Tagen') : ''}${screenField(q + 'visible_matches', 'number', 'Gemeinsame Höchstzahl je Unterseite von Konferenz und Spieltag, mindestens 2. Beim Anheften zählt das eigene Spiel mit. Übrige Partien werden auf dieselbe Seitenzahl verteilt; kleine letzte Seiten werden soweit möglich aufgefüllt. Die Obergrenze hängt vom Display ab.', null, false, 'Spiele gleichzeitig')}${screenField(q + 'table_window', 'number', 'Tabellenzeilen je Unterseite. Die Obergrenze hängt vom Display ab.', null, false, 'Tabellenzeilen')}${screenField(q + 'conference_mode', 'select', 'Alle Seiten zeigt alle geeigneten Spiele. Eine Seite bevorzugt laufende Spiele und jüngste Tore innerhalb der vorhandenen Plätze.', [[0, t('Alle Seiten')], [1, t('Eine Seite')]], false, 'Konferenz-Modus')}${screenField(q + 'pin_own_match', 'switch', 'Heftet das eigene Spiel in seiner Konferenz an, solange es dort ausgewählt ist. Ohne Anheften bleibt die normale Reihenfolge erhalten.', null, false, 'Eigenes Spiel anheften')}${screenField(q + 'matchday_mode', 'select', 'Aktueller Spieltag behält einen beendeten Spieltag bis Ende des Folgetags seines lokalen Spielendes. Nächster Spieltag nutzt den vorhandenen folgenden Spieltag; fehlende Daten werden nicht erfunden.', [[0, t('Aktueller Spieltag')], [1, t('Nächster Spieltag')]], false, 'Angezeigter Spieltag')}</div></details>` + `${screenDisclosureHead('appearance', 'Farben, Hintergründe und Unterseiten')}${screenField(q + 'backgrounds', 'switch', 'Gemeinsame Freigabe der Hintergrundbilder für die vier Sportansichten. Einzelne Bildauswahlen bleiben erhalten.', null, false, 'Hintergrundbilder auf allen Sportansichten')}${screenAppearanceMarkup()}</details>` + `${screenDisclosureHead('images', 'Bilderfolge und Bilderrahmen')}<div class="cols">${screenField(q + 'per_image_s', 'number', 'Zeit je Bild in Sekunden: 3 bis 3.600. Besuche der Bilderfolge müssen ein ganzzahliges Vielfaches davon sein.', null, false, 'Bildwechsel')}${screenField(q + 'frame_crest', 'switch', 'Nimmt das separate Wappen zusätzlich zu den ausgewählten Bibliotheksbildern in den Bilderrahmen auf.', null, false, 'Wappen im Bilderrahmen')}</div>${screenSlidesMarkup()}</details>`;
}
function prepareScreenImport(m, data) {
    if (Object.prototype.hasOwnProperty.call(data, 'screen_policy')) {
        const issues = validateScreenPolicy(data.screen_policy, m.competitions);
        if (issues.length)
            throw Error(labelOf(issues[0].path) + ': ' + issues[0].message);
    }
    if (Object.prototype.hasOwnProperty.call(data, 'competitions'))
        for (const c of m.competitions) {
            c.id = 0;
            c.incarnation = 0;
            c._screenKey = newScreenKey();
        }
    prepareScreenModel(m);
}
function displayBasics() {
    const m = S.draft, backlight = S.schema.backlight === true;
    const brightnessHelp = 'Helligkeit der Displaybeleuchtung von 0 bis 100 %. 0 schaltet die Beleuchtung aus.';
    const windowHelp = 'Täglicher Beginn und Ende in der lokalen Gerätezeit. Die Startzeit ist eingeschlossen, die Endzeit nicht. Gleiche Zeiten ergeben kein Nachtfenster.';
    const rotation = draftField('rotation', 'select', '', [[0, '0°'], [1, '90°'], [2, '180°'], [3, '270°']]);
    const brightness = backlight ? draftField('brightness', 'number', brightnessHelp) : '';
    const night = backlight ? `<div class="cols">${draftField('night_enabled', 'switch', 'Ändert im eingestellten Zeitfenster ausschließlich die Displaybeleuchtung. Farben, Hintergrund und Bilder bleiben unverändert. Benötigt eine gültige Gerätezeit.')}${draftField('night_brightness', 'number', brightnessHelp, null, !m.night_enabled)}${draftField('night_start', 'time', windowHelp, null, !m.night_enabled)}${draftField('night_end', 'time', windowHelp, null, !m.night_enabled)}</div>` : '';
    return section(backlight ? 'Helligkeit und Ausrichtung' : 'Ausrichtung', '', `<div class="cols">${brightness}${rotation}</div>` + night) + section('IP-Adresse auf dem Display', '', `<div class="cols">${draftField('ip_badge_s', 'number', '0 = dauerhaft.')}${draftField('ip_badge_bottom', 'switch')}${draftField('ip_badge_ap_permanent', 'switch')}</div>`) + inputSection() + expert('Tastenverhalten beim Blättern', draftField('scroll_repeat_ms', 'number', 'Nur Tabelle: Abstand der Blätterschritte, solange eine Taste gehalten wird.') + (scrollInput() ? draftField('scroll_reset_s', 'number', 'So viele Sekunden nach dem letzten Richtungswechsel blättert eine Blättern-Taste wieder abwärts. 0 = nie zurücksetzen.') : ''));
}
function displayPage() {
    return head('Anzeige') + (S.server?.path?.startsWith('screen_policy') ? `<div class="notice err"><p>${esc(S.server.message)}</p>${button('Anzeige neu laden', 'policyReload')}</div>` : '') + (S.draft.screen_policy ? screenOptionsMarkup() : `<p class="notice err">${ht('Die Anzeigeeinstellungen konnten nicht geladen werden. Seite neu laden. [WEB S02]')}</p>`) + displayBasics();
}
function inputRoles() {
    const n = S.schema.inputs, r = S.draft.input_roles;
    return Number.isInteger(n) && Array.isArray(r) ? r.slice(0, n) : null;
}
function scrollInput() {
    const r = inputRoles();
    return !r || r.includes(4);
}
function inputSection() {
    const r = inputRoles();
    if (!r?.length)
        return '';
    return section('Tasten', 'Jede Taste ist abschaltbar. Genau eine Taste ist die Pflichttaste: kurz = nächster Bildschirm, doppelt = vorheriger Bildschirm, lang = zurück zur Automatik.', `<div class="field" data-field="input_roles"><div class="cols">${r.map((v, i) => draftField(`input_roles[${i}]`, 'select', ROLE_HELP[v] || '', ROLES.map((n, k) => [k, t(n)]), false, tf('Taste %', i + 1))).join('')}</div><div class="field-error" data-error="input_roles"></div></div>`);
}
function validateCompetitionDisplay(display, index) {
    const issues = [], prefix = `competitions[${index}].display`;
    const add = (path, text) => issues.push({ path, message: t(text) });
    const object = (value, path, keys) => {
        if (!value || typeof value !== 'object' || Array.isArray(value)) {
            add(path, 'Falscher Datentyp');
            return false;
        }
        for (const key of Object.keys(value))
            if (!keys.includes(key))
                add(path + '.' + key, 'Unbekanntes Feld');
        for (const key of keys)
            if (!(key in value))
                add(path + '.' + key, 'Wert wird benötigt');
        return true;
    };
    if (!object(display, prefix, SP_PHASES))
        return issues;
    for (const phase of SP_PHASES) {
        const at = prefix + '.' + phase;
        if (!object(display[phase], at, SP_SPORTS))
            continue;
        for (const type of SP_SPORTS) {
            const tuple = display[phase][type], path = at + '.' + type;
            if (!Array.isArray(tuple) || tuple.length !== 3) {
                add(path, 'Falscher Datentyp');
                continue;
            }
            tuple.forEach((value, field) => {
                if (value === null)
                    return;
                if (field < 2 && typeof value !== 'boolean' || field === 2 && !Number.isInteger(value))
                    add(path + '[' + field + ']', 'Falscher Datentyp');
                else if (field === 2) {
                    const [low, high] = S.schema.screen_policy.bounds.visit_s;
                    if (value < low || value > high)
                        add(path + '[2]', 'Wert außerhalb des erlaubten Bereichs');
                }
            });
        }
    }
    return issues;
}
