// Page composition, navigation and publication of device status to the DOM.
const PHASE = {
    unknown: 'Unbekannt', scheduled: 'Geplant', live: 'Live', half_time: 'Halbzeit', extra_time: 'Verlängerung', penalties: 'Elfmeterschießen', finished: 'Beendet', postponed: 'Verschoben', cancelled: 'Abgesagt', abandoned: 'Abgebrochen', suspended: 'Unterbrochen'
};
function loadingState() {
    const s = S.status;
    return s?.provider_started === false || !s?.last_update_ms && !s?.competitions?.some(r => r.cause);
}
function ownMarkup() {
    const o = S.status?.own_match;
    if (!o?.present)
        return `<p class="muted">${ht(loadingState() ? 'Daten werden geladen' : 'Keine eigenen Spiele verfügbar')}</p>`;
    const goals = o.home_goals !== undefined && o.away_goals !== undefined ? `<strong class="num" style="font-size:2.6rem">${o.home_goals} : ${o.away_goals}</strong>` : `<b class="num">${ht('Anstoß')} ${esc(dt(o.kickoff))}</b>`;
    return `<div class="row"><b>${esc(o.home)}</b><span>–</span><b>${esc(o.away)}</b></div>${goals}<p>${ht(PHASE[o.phase] || 'Unbekannt')}${o.minute !== undefined ? ' · ' + o.minute + '′' : ''}</p>`;
}
function previewMarkup() {
    const s = S.status;
    if (!s)
        return `<p class="muted">${ht('Daten werden geladen')}</p>`;
    return `<p><b>${ht(s.screen_starting ? 'Startbildschirm' : SCREENS[s.screen] || 'Unbekannt')}</b></p><p class="help">${ht('Die tatsächliche Geräteausgabe kannst du als Bildschirmfoto herunterladen.')}</p>`;
}
function reasonText() {
    const s = S.status, m = S.saved, o = s.own_match;
    switch (s.screen_reason) {
        case 'manual': {
            const delay = m.screen_policy.manual_return_s;
            return delay ? tf('Von Hand gewählt · zurück zur Automatik % s nach dem letzten Tastendruck.', delay) : t('Von Hand gewählt · bleibt bis zum nächsten Tastendruck.');
        }
        case 'setup': return t('Einrichtung: Es ist noch kein aktiver Wettbewerb gespeichert.');
        case 'own_match': return t(m.demo ? 'Demo-Modus: Das eigene Spiel wird simuliert.' : 'Das eigene Spiel läuft oder liegt im Vorlauf oder Nachlauf.');
        case 'other_games': return t('Spiele der Wettbewerbe laufen oder liegen im Vorlauf oder Nachlauf.');
        case 'matchday': return t('Spieltag');
        case 'break': return t('Saisonpause');
        case 'frame': return t('Bilderrahmen');
        case 'idle': return t('Kein Spiel läuft.') + (o?.present && o.phase === 'scheduled' && o.kickoff ? ' ' + tf('Nächstes eigenes Spiel: %', dt(o.kickoff)) : '');
    }
    return t('Unbekannt');
}
function overview() {
    return head('Übersicht') + (S.draft.screen_policy ? `<div class="card" style="margin-bottom:16px">${screenField('screen_policy.frame', 'switch', 'Bilderrahmen zeigt ausschließlich Wappen und ausgewählte Bilder. Nach dem Speichern starten keine automatischen Fußballabfragen oder Fortsetzungen mehr; eine bereits gesendete Anfrage wird noch abgeschlossen. Profile und Daten bleiben erhalten. Beim Verlassen werden nur fällige Abrufe innerhalb der Anbieterlimits nachgeholt. Der Wettbewerbsassistent darf ausdrücklich gestartete Einrichtungslisten weiter laden.', null, false, 'Bilderrahmen')}</div>` : '') + `<div class="grid2"><div class="card"><h2>${ht('Display jetzt')}</h2><div id="statusPreview" style="margin-block:14px"></div><p id="screenReason"></p></div><div class="card" style="display:grid;gap:12px;align-content:start"><h2>${ht('Eigenes Spiel')}</h2><div id="ownStatus"></div><div class="row">${button('Daten abrufen', 'refresh')}${button('Nächster Bildschirm', 'next')}${button('IP-Adresse anzeigen', 'ip')}${S.saved.overview_screenshot ? screenshotButton() : ''}</div></div></div>` + section('Datenstand', '', `<p id="dataAge" class="num muted"></p><p id="nextRequest" class="num muted"></p><div id="competitionStatus" class="list"></div><div id="warnings"></div>`) + `<div id="setupChecklist"></div>`;
}
function providerSettings(index) {
    const key = 'provider-' + index;
    const credentials = index === 1 ? SEC('api_football_key')
        : index === 3 ? SEC('football_data_key')
        : index === 2 ? draftField('espn_opt_in', 'switch', 'ESPN ist eine inoffizielle Schnittstelle. Freigabe erfolgt ausdrücklich.') : '';
    const limits = [
        draftField(`providers[${index}].daily_requests`, 'number', 'Höchstens so viele Abrufe je Tag; der Tag wechselt um 00:00 UTC. Reicht der Rest nicht bis zum Tageswechsel, ruft das Gerät zuerst Live-Tabelle, Tabelle, Spielplan und Live-Stände seltener oder gar nicht ab, das eigene Spiel zuletzt; sind die Abrufe aufgebraucht, ruft es den Anbieter erst nach dem Tageswechsel wieder ab. 0 = keine Tagesgrenze.'),
        draftField(`providers[${index}].window_requests`, 'number', 'Höchstens so viele Abrufe innerhalb des Zeitfensters, z. B. 10 Abrufe je 60 s. Das Gerät überschreitet sie in keinem Zeitfenster. Beide Werte 0 = keine Grenze.'),
        draftField(`providers[${index}].window_s`, 'number', 'Länge des Zeitfensters. 0 nur zusammen mit 0 Abrufen.')
    ].join('');
    return `<details class="expert" data-expert-key="${key}" ${S.expertOpen?.has(key) ? 'open' : ''}>
        <summary>${esc(PROVIDERS[index])}<span>${ht(providerDescription(index))}</span></summary>
        ${credentials ? `<div class="cols">${credentials}</div>` : ''}
        <div class="cols">${limits}</div>
    </details>`;
}
function settingsPage() {
    const scan = S.scan === 'busy' ? `<span class="spin"></span>${ht('Suche läuft …')}`
        : Array.isArray(S.scan) ? `<div class="pick">${S.scan.map(n =>
            `<button class="choice" data-act="ssid" data-v="${esc(n.ssid)}" style="grid-template-columns:1fr auto">
                <span>${esc(n.ssid)}</span><span class="small muted num">${n.rssi} dBm ${n.secure ? '' : ht('Offen')}</span>
            </button>`).join('') || `<p class="muted">${ht('Keine Netzwerke gefunden.')}</p>`}</div>` : '';
    const wifi = `<div class="cols">
        ${draftField('wifi_ssid')}
        ${SEC('wifi_password', 'WLAN-Passwort: 8–63 ASCII-Zeichen oder 64 Hex-Ziffern; leer für offenes WLAN.')}
        <div style="grid-column:1/-1">
            ${button('Netzwerke suchen', 'scan', S.scan === 'busy' ? 'disabled' : '')}
            ${scan ? `<div style="margin-top:10px">${scan}</div>` : ''}
        </div>
        ${draftField('hostname', 'text', 'Gerätename: Buchstaben, Ziffern und Bindestrich, ohne Bindestrich am Rand.')}
        ${SEC('ap_password', 'Setup-Passwort: 8–63 ASCII-Zeichen; leer für offenes WLAN.')}
        ${draftField('external_antenna', 'switch', 'Nur aktivieren, wenn eine Antenne am Stecker angeschlossen ist.', null, !S.schema.antenna)}
    </div>`;
    const providers = `<div class="provider-settings">${[0, 1, 3, 2].map(providerSettings).join('')}</div>`;
    const clock = `<div class="cols">
        ${draftField('time_zone', 'select', '', S.schema.time_zones.map(z => [z, z]))}
        ${draftField('ntp_server', 'text', 'Zeitserver: gültige DNS-Namen oder IPv4-Adresse.')}
        ${draftField('language', 'select', '', [[0, t('Deutsch')], [1, 'English']])}
    </div>`;
    return head('Einstellungen')
        + section('WLAN', '', wifi)
        + section('Datenanbieter', '', providers)
        + section('Zeit und Sprache', '', clock)
        + section('Zugang', 'Ohne Admin-Passwort kann jeder im WLAN Einstellungen ändern. Lesen bleibt mit Passwort möglich.', SEC('admin_password'));
}
function systemPage() {
    return head('System') + section('Firmware', '', `<dl class="kv" id="firmwareStatus"></dl><div class="card" style="display:grid;gap:12px"><h3>${ht('Update installieren')}</h3><p class="help">${ht('Passende firmware.bin wählen. Profil, Speicherlayout, Format und Größe werden geprüft; die Firmware entscheidet abschließend.')}</p><input type="file" id="firmwareFile" accept=".bin"><div id="otaCheck" class="help"></div><div class="progress" id="otaProgress" hidden><i></i></div><div id="otaState" class="status-line"></div>${button('Update', 'otaUpload', 'id="otaUpload" disabled', 'primary')}</div>`) + section('Sicherung', 'Die Sicherung enthält Einstellungen, Wettbewerbe, Anzeigeprogramme und die vollständige Namenssammlung; Bilder sind nicht enthalten.', `<label class="switch"><input type="checkbox" id="exportSecrets"><span class="track"></span>${ht('Passwörter und Schlüssel einschließen')}</label><p class="help">${ht('Diese Datei enthält Geheimnisse. Sicher aufbewahren.')}</p><div class="row">${button('Einstellungen exportieren', 'export')}<label class="btn" for="importFile">${ht('Einstellungen importieren')}</label><input type="file" id="importFile" accept="application/json,.json" hidden></div>`) + section('Neustart', '', button('Gerät neu starten', 'reboot')) + section('Wartung', '', `<div class="grid2"><div class="card" style="display:grid;gap:12px"><h3>${ht('Bildspeicher reparieren')}</h3><p class="help">${ht('Vollständige Reparatur erfordert bestätigte Werkseinstellungen. WLAN, Zugang, Abrufzähler, gespeicherte Vereinsnamen und alle Dateien werden gelöscht. Standardbilder benötigen danach USB-uploadfs.')}</p>${button('Bildspeicher reparieren', 'repair', '', 'danger')}</div><div class="card" style="display:grid;gap:12px"><h3>${ht('Alle eigenen Bilder löschen')}</h3><p class="help">${ht('Jede Datei der Bibliothek und jede Auswahl einer fehlenden Datei wird einzeln gelöscht. Wappen und Standard-Hintergrund zeigen danach wieder die Werksbilder. Teilfehler bleiben sichtbar.')}</p>${button('Alle eigenen Bilder löschen', 'imagesDeleteAll', '', 'danger')}<p id="deleteResults" class="status-line small"></p></div><div class="card" style="display:grid;gap:12px"><h3>${ht('Werkseinstellungen')}</h3><p class="help">${ht('Löscht Einstellungen einschließlich WLAN und Zugang, Abrufzähler und gespeicherte Vereinsnamen. Danach startet der Einrichtungsmodus. Die unveränderliche Namensvorlage der Firmware bleibt verfügbar.')}</p>${button('Zurücksetzen', 'factory', '', 'danger')}</div></div>`);
}
function renderHeader() {
    const status = S.status, session = S.session;
    $('brand').textContent = S.saved?.hostname || 'Fussball Display';
    $('deviceAddress').textContent = status?.ip && status.ip !== '0.0.0.0' ? status.ip : location.host;
    $('connection').textContent = t(S.errors.status ? 'Gerät nicht erreichbar. Es wird erneut versucht …' : !status ? 'Verbinde …' : status.ap ? 'Einrichtung' : status.network === 'connected' ? 'Verbunden' : 'WLAN getrennt');
    $('connection').className = 'chip ' + (S.errors.status ? 'err' : status?.ap ? 'warn' : status?.network === 'connected' ? 'ok' : 'warn');
    $('theme').textContent = t(theme === 'auto' ? 'Automatisch' : theme === 'dark' ? 'Dunkel' : 'Hell');
    $('theme').title = $('theme').textContent;
    $('auth').hidden = !session?.password_set;
    $('auth').textContent = t(session?.valid ? 'Abmelden' : 'Anmelden');
    document.documentElement.lang = S.draft?.language === 1 ? 'en' : 'de';
    renderNav();
}
function renderNav() {
    const changed = changes(), issues = S.tried ? validate(S.draft) : [];
    $('nav').innerHTML = Object.entries(PAGES).map(([p, name]) => `<button data-act="go" data-page="${p}" ${p === S.page ? 'aria-current="page"' : ''}>${ht(name)}${issues.some(x => pageOf(x.path) === p) ? '<i class="dot e"></i>' : changed.some(x => pageOf(x.path) === p) ? '<i class="dot"></i>' : ''}</button>`).join('');
}
function loadCards() {
    return Object.entries(S.errors).map(([key, message]) => `<div class="card load-card" data-load="${key}"><b>${ht({
        schema: 'Schema', settings: 'Einstellungen', status: 'Status', images: 'Bilder', session: 'Sitzung'
    }[key] || 'System')}</b><p class="muted">${esc(message)}</p>${button('Erneut versuchen', 'retry', `data-key="${key}"`)}</div>`).join('');
}
function render() {
    renderHeader();
    const y = scrollY;
    for (const el of document.querySelectorAll('#main details[data-expert-key]')) {
        S.expertOpen ??= new Set();
        if (el.open)
            S.expertOpen.add(el.dataset.expertKey);
        else
            S.expertOpen.delete(el.dataset.expertKey);
    }
    if (!S.schema || !S.draft) {
        $('main').innerHTML = loadCards() + `<div class="card">${ht('Daten werden geladen')}</div>`;
        renderBar();
        return;
    }
    if (S.page === 'display')
        S.policyOpen = new Set([...document.querySelectorAll('#main details[data-policy-block][open]')].map(el => el.dataset.policyBlock));
    $('main').innerHTML = loadCards() + ({
        overview, competitions, display: displayPage, images: imagesPage, settings: settingsPage, debug: debugPage, system: systemPage
    }[S.page])();
    document.querySelectorAll('[data-image]').forEach(im => im.onerror = () => {
        im.replaceWith(Object.assign(document.createElement('span'), {
            textContent: t('Bildvorschau nicht verfügbar.'), className: 'help'
        }));
    });
    if (S.page === 'images' && S.editor)
        bindEditor();
    if (S.page === 'system')
        bindSystem();
    if (S.page === 'debug')
        $('liveDetails').ontoggle = e => {
            if (!e.target.open)
                closeDebug();
        };
    paintStatus();
    paintErrors();
    renderBar();
    scrollTo(0, y);
}
function paintErrors() {
    const errors = [...(S.tried ? validate(S.draft) : []), ...(S.server ? [S.server] : [])];
    document.querySelectorAll('[data-error]').forEach(el => {
        const p = el.dataset.error, rows = errors.filter(e => e.path === p || e.path.startsWith(p + '.'));
        el.textContent = rows.map(e => e.message).join(' ');
        el.classList.toggle('on', rows.length > 0);
        el.closest('.field')?.classList.toggle('bad', rows.length > 0);
    });
}
function showValue(v, p = '') {
    if (/^input_roles\[/.test(p) && ROLES[v])
        return t(ROLES[v]);
    if (p === 'screen_policy.conference_mode' && Number.isInteger(v))
        return t(['Alle Seiten', 'Eine Seite'][v] ?? String(v));
    if (p === 'screen_policy.matchday_mode' && Number.isInteger(v))
        return t(['Aktueller Spieltag', 'Nächster Spieltag'][v] ?? String(v));
    if (typeof v === 'boolean')
        return t(v ? 'Aktiv' : 'Keine');
    if (v && typeof v === 'object')
        return Array.isArray(v) ? String(v.length) : t('Änderung');
    return String(v ?? '—');
}
function renderBar() {
    const c = changes(), errors = [...(S.tried ? validate(S.draft) : []), ...(S.server ? [S.server] : [])], notice = S.saveNotice;
    $('savebar').hidden = !c.length && !errors.length && !notice;
    if ($('savebar').hidden)
        return;
    $('savebarIn').innerHTML = (notice ? `<div class="notice ${notice.severity === 'warning' ? 'warn' : 'err'} save-feedback" role="${notice.severity === 'error' ? 'alert' : 'status'}"><p>${feedback(notice.message, notice.severity)}</p><div>${button('Status erneut prüfen', 'saveRefresh', S.saving ? 'disabled' : '')}</div></div>` : '') + (!c.length && !errors.length ? '' : `<div class="top"><b ${errors.length ? 'style="color:var(--err)"' : ''}>${errors.length ? errors.length + ' ' + ht('Fehler verhindern das Speichern') : c.length + ' ' + ht('Änderungen nicht gespeichert')}</b>${button(S.expanded ? 'Liste ausblenden' : 'Liste zeigen', 'expand')}${button('Verwerfen', 'discard', S.saving ? 'disabled' : '')}${button(S.saving ? 'Speichert …' : 'Speichern', 'save', S.saving || S.pendingApply || !c.length ? 'disabled' : '', 'primary')}</div>${S.expanded || errors.length ? `<ul>${errors.map(e => `<li class="e"><button data-act="jump" data-path="${esc(e.path)}">${ht(PAGES[pageOf(e.path)])} · ${esc(labelOf(e.path))}</button><span>${esc(e.message)}</span></li>`).join('')}${S.expanded ? c.map(x => `<li><button data-act="jump" data-path="${esc(x.path)}">${ht(PAGES[pageOf(x.path)])} · ${esc(labelOf(x.path))}</button>${x.secret ? `<span>${ht(x.secret === 'clear' ? 'Geheimnis wird entfernt' : 'Geheimnis wird geändert')}</span>` : `<span class="old">${esc(showValue(x.old, x.path))}</span> → <span>${esc(showValue(x.value, x.path))}</span>`}</li>`).join('') : ''}</ul>` : ''}`);
}
function statusCompetitions() {
    if (!S.status?.demo)
        return S.saved.competitions;
    return (S.status.competitions || []).map((r, i) => {
        const saved = r.bound ? S.saved.competitions.find(c => c.id === r.id && c.incarnation === r.incarnation) : null;
        return {
            id: r.id, active: true, competition_name: 'Demo ' + (i + 1) + (saved ? ' · ' + compName(saved) : ''), primary: {
                competition: ''
            }
        };
    });
}
function paintStatus() {
    renderHeader();
    const s = S.status;
    if (!s)
        return;
    const failure = networkFailure(s), imageFault = imageFailure(s), preview = $('statusPreview');
    if (preview) {
        const html = previewMarkup();
        if (preview.html !== html) {
            preview.innerHTML = html;
            preview.html = html;
        }
    }
    if ($('screenReason'))
        $('screenReason').textContent = reasonText();
    if ($('ownStatus'))
        $('ownStatus').innerHTML = ownMarkup();
    if ($('dataAge'))
        $('dataAge').textContent = t('Datenalter') + ': ' + (s.last_update_ms ? Math.max(0, Math.floor(s.uptime_s - s.last_update_ms / 1000)) + ' s' : t(loadingState() ? 'Daten werden geladen' : 'Noch keine Daten')) + ' · ' + t(s.provider_busy ? 'Abruf läuft' : s.data_stale ? 'Veraltet' : 'Aktuell');
    if ($('nextRequest'))
        $('nextRequest').textContent = nextRequestText();
    if ($('competitionStatus'))
        $('competitionStatus').innerHTML = statusCompetitions().map((c, i) => {
            const r = s.competitions?.find(x => x.id === c.id);
            const failures = Object.entries(r?.datasets || {}).filter(([, data]) => data.cause);
            const datasetErrors = failures.map(([key, data]) => `<p class="meta"><b>${ht({matches:'Spiele',table:'Tabelle',details:'Spieldetails'}[key] || key)}</b> · ${esc(causeMessage(data.cause, data.code))}${data.last_success_ms ? ' · ' + ht('Letzter erfolgreicher Abruf') + ' ' + monoClock(data.last_success_ms) : ''}</p>`).join('');
            const bad = c.active && (!!r?.cause || failures.length > 0), kept = r?.cause === 'unmaintained', full = r?.cause === 'store_full', notice = kept || full, other = PROVIDERS.filter((_, p) => providerEnabled(p) && S.schema.providers[p] !== r?.error_provider), hint = kept ? ' ' + (other.length ? t('Andere aktivierte Anbieter:') + ' ' + other.join(', ') : t('Unter Einstellungen einen weiteren Anbieter aktivieren.')) : '';
            return `<div class="card item"><div class="pos">${i + 1}</div><div class="item-main"><b>${esc(c.competition_name || c.primary.competition)}</b><p class="meta">${esc(PROVIDERS[S.schema.providers.indexOf(r?.provider)] || r?.provider || '—')} · ${r?.matches ?? 0} ${ht('Spiele')}${r?.last_update_ms ? ' · ' + ht('Stand') + ' ' + monoClock(r.last_update_ms) : ''}</p>${bad ? datasetErrors || `<p class="meta">${esc(causeMessage(r.cause, r.code, full ? r.missing : null) + hint)}</p>` : ''}</div><span class="tag ${bad ? notice ? 'warn' : 'err' : 'acc'}">${ht(!c.active ? 'Keine' : bad ? kept ? 'Nicht gepflegt' : full ? 'Unvollständig' : 'Gestört' : loadingState() ? 'Daten werden geladen' : s.data_stale ? 'Veraltet' : 'Aktuell')}</span></div>`;
        }).join('');
    if ($('warnings')) {
        const warn = [];
        if (SOURCE[s.settings_source]) warn.push(t(SOURCE[s.settings_source]));
        if (failure)
            warn.push(failure.message);
        if (imageFault)
            warn.push(imageFault.message);
        if (s.names?.code)
            warn.push(t('Gespeicherte Vereinsnamen konnten nicht gelesen werden. Unter Wettbewerbe die Namenssammlung erneut öffnen und bei Bedarf die Sicherung importieren. Anbieterwerte bleiben sichtbar.') + ' · ' + s.names.code);
        if (s.network && s.network !== 'connected')
            warn.push(t('Kein WLAN') + ': ' + t((NETR[s.network_reason] || NETR.connecting)[0]));
        if (!s.time_valid)
            warn.push(t('Zeit noch ungültig'));
        if (loadingState())
            warn.push(t('Daten werden geladen. Der erste Abruf kann länger dauern.'));
        $('warnings').innerHTML = warn.map(x => `<div class="notice warn"><p>${esc(x)}</p></div>`).join('');
    }
    if ($('setupChecklist'))
        $('setupChecklist').innerHTML = s.ap ? section('Einrichtung abschließen', '', `<ol><li>${ht('WLAN unter Einstellungen auswählen und speichern.')}</li><li>${ht('Wettbewerb und Verein auswählen und speichern.')}</li><li>${ht('Auf gültige Uhrzeit und den ersten Datenabruf warten.')}</li></ol>`) : '';
    if ($('networkWarning')) {
        $('networkWarning').innerHTML = failure ? `<div class="notice warn"><p>${esc(failure.message)}</p></div>` : '';
    }
    if ($('imageWarning'))
        $('imageWarning').innerHTML = imageFault ? `<div class="notice warn"><p>${esc(imageFault.message)}</p></div>` : '';
    const kv = rows => rows.map(([a, b]) => `<dt>${ht(a)}</dt><dd class="num">${esc(b)}</dd>`).join('');
    if ($('firmwareStatus'))
        $('firmwareStatus').innerHTML = kv([
            ['Version', s.version], ['Gerät', s.target + ' · ' + s.layout], ['Einstellungsformat', S.schema.version], ['Build-Kennung', s.firmware_id || '—'], ['Firmware-Speicherplatz', firmwareUsage(s)]
        ]);
    if ($('diagnosticStatus'))
        $('diagnosticStatus').innerHTML = kv([
            ['Uhrzeit gültig', t(s.time_valid ? 'Aktuell' : 'Zeit noch ungültig')], ['WLAN', s.ip], [
                'Lokale Netzwerkdienste', (failure ? failure.phase + (s.network_init?.code ? ' · ' + s.network_init.code : '') : '') || (s.network_init?.ready ? t('Aktuell') : t(s.network_init ? 'Wird gestartet' : 'Nicht verfügbar'))
            ], ['Signalstärke', s.rssi + ' dBm'], ['Läuft seit', s.uptime_s + ' s'], [
                'Letzter Neustart', t({
                    'power-on': 'Stromversorgung eingeschaltet', 'external pin': 'Externer Neustart-Pin', 'software restart': 'Software-Neustart', panic: 'Programmabsturz', 'interrupt watchdog': 'Überwachung der Unterbrechungen', 'task watchdog': 'Überwachung der Aufgaben', 'other watchdog': 'Weitere Überwachung', 'deep sleep': 'Aufwachen aus Tiefschlaf', brownout: 'Versorgungsspannung zu niedrig', 'eFuse error': 'Fehler im dauerhaften Chip-Speicher', 'power glitch': 'Störung der Stromversorgung', 'CPU lockup': 'Prozessor blockiert', SDIO: 'SDIO', USB: 'USB', JTAG: 'JTAG'
                }[s.reset_reason] || 'Unbekannt')
            ], ['Freier interner Speicher', kb(s.heap_free)], heapMinimum(s), ['Größter freier Block', kb(s.heap_largest)], [
                'Bilddarstellung', imageFault ? imageFault.desired + ' · ' + imageFault.fallback + ' · ' + imageFault.code : s.image?.active ? t(s.image.pending ? 'Wird geprüft' : s.image.actual ? 'Aktuell' : 'Kein verwendbares Bild') : t('ausgeschaltet')
            ], ['Bildspeicher', kb(s.fs_used) + ' / ' + kb(s.fs_total)], ['Abrufe seit Neustart', (s.requests || []).map((n, i) => PROVIDERS[i] + ' ' + n).join(' · ')], ['Update-Test', t(s.trial ? 'Aktiv' : 'Keine')], ['Aufgaben und freie Stack-Minima', (s.tasks || []).map(taskMinimum).join(' · ')]
        ]);
    paintOta();
    paintScreenshot();
    paintDemoNavigation();
}
function jump(path) {
    S.page = pageOf(path);
    closeEditor();
    const a = parts(path);
    if (a[0] === 'screen_policy') {
        if (a[1] === 'phases')
            S.screenPhase = a[2];
        else if (a[1] === 'appearance')
            S.screenAppearance = a[2];
    }
    else if (a[0] === 'competitions') {
        S.open = Number(a[1]);
        if (a[2] === 'display' && SP_PHASES.includes(a[3])) {
            S.screenPhase = a[3];
            S.policyOpen ||= new Set();
            if (SP_SPORTS.includes(a[4]))
                S.policyOpen.add(a[3] + '-' + a[4]);
        }
    }
    render();
    const el = [...document.querySelectorAll('[data-field]')].find(e => e.dataset.field === path) || [...document.querySelectorAll('[data-field]')].find(e => path.startsWith(e.dataset.field + '.'));
    if (el) {
        el.closest('details')?.setAttribute('open', '');
        el.scrollIntoView({
            block: 'center'
        });
        el.querySelector('input,select,button')?.focus({
            preventScroll: true
        });
    }
    else if (path.startsWith('competitions[')) {
        const index = Number(path.match(/\d+/)[0]);
        openWizard(index);
    }
}
