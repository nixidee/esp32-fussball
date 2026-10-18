// Demo controls, device screenshots and the optional diagnostic stream.
// Scenario controls change runtime demo state only; they never join the saved draft.
const DEMO_DE = {
    'Own match': 'Eigenes Spiel', 'Ordinary match': 'Gewöhnliches Einzelspiel', 'Five-match conference': 'Konferenz mit fünf Spielen', 'Extra time and shootout': 'Verlängerung und Elfmeterschießen', 'Rounds and rescheduled fixture': 'Runden und Nachholspiel', 'Proven synthetic break': 'Simulierter Pausennachweis', 'Empty successful response': 'Erfolgreich geladene leere Daten', 'Before kickoff': 'Vor Anstoß', 'First booking': 'Erste Verwarnung', 'Second booking': 'Zweite Verwarnung', 'Half time': 'Halbzeit', 'Goal and two own matches': 'Tor und zwei eigene Spiele', 'Justified interruption': 'Begründete Unterbrechung', 'Live without kickoff': 'Live ohne Anstoßzeit', 'Final whistle': 'Abpfiff', 'Result on following day': 'Ergebnis am Folgetag', 'First goal': 'Erstes Tor', 'Finished post-window': 'Beendet im Nachlauf', 'Foreign result outside window': 'Fremdes Ergebnis außerhalb des Zeitfensters', 'Five live matches': 'Fünf laufende Spiele', 'Mixed live and finished': 'Laufende und beendete Spiele', 'Extra time': 'Verlängerung', 'Shootout begins': 'Elfmeterschießen beginnt', 'Reported scored attempts': 'Gemeldete verwandelte Versuche', 'Final shootout with visible event limit': 'Beendetes Elfmeterschießen mit Ereignisgrenze', 'Current round with later rescheduled fixture': 'Aktuelle Runde mit späterem Nachholspiel', 'Held round and next round': 'Gehaltene und nächste Runde', 'Next cluster of an earlier round': 'Nächster Datumsblock einer früheren Runde', 'Two competitions with a gap over 21 days': 'Zwei Wettbewerbe mit mehr als 21 Tagen Abstand', 'No matches and no table': 'Keine Spiele und keine Tabelle'
};
const demoText = text => S.draft?.language === 1 ? text : DEMO_DE[text] || text;
function demoBindingsMarkup(d) {
    return (d.bindings || []).map((b, i) => {
        const saved = b.saved ? S.saved.competitions.find(c => c.id === b.competition && c.incarnation === b.incarnation) : null;
        return '<span>Demo ' + (i + 1) + ': ' + esc(saved ? compName(saved) : t('Gemeinsame Regeln')) + '</span>';
    }).join(' · ');
}
function demoMarkup() {
    if (!S.saved.demo)
        return '';
    const d = S.demo;
    if (!d)
        return `<div class="field" style="grid-column:1/-1">${button('Szenarien laden', 'demoReload')}</div>`;
    const scenario = d.scenarios.find(row => row.id === (S.demoPick ?? d.scenario)) || d.scenarios[0], step = Math.min(S.demoPickStep ?? d.step, scenario.steps.length - 1), off = S.demoBusy || d.pending || !d.enabled;
    return `<div class="field" style="grid-column:1/-1"><span class="lbl">${ht('Demo-Szenarien')}</span><p class="help">${demoBindingsMarkup(d)}</p><p class="help">${ht('Steuert simulierte Spieldaten und eine separate Testzeit. Die ersten beiden gespeicherten Wettbewerbe liefern Reihenfolge und Anzeige-Regeln. Fehlt eine gespeicherte Zuordnung, gelten die gemeinsamen Regeln. Systemuhr und echte Anbieter werden nicht verändert; Szenarien werden nicht gespeichert.')} <button class="btn ghost policy-help" data-act="demoHelp" aria-label="${ht('Hilfe')}">?</button></p>${!d.enabled ? `<p class="help">${ht('Demo-Modus zuerst einschalten und speichern.')}</p>` : ''}<div class="cols"><div class="field"><label for="demoScenario">${ht('Szenario')}</label><select id="demoScenario" ${off ? 'disabled' : ''}>${opts(d.scenarios.map(row => [row.id, demoText(row.title)]), scenario.id)}</select></div><div class="field"><label for="demoStep">${ht('Schritt')}</label><select id="demoStep" ${off ? 'disabled' : ''}>${opts(scenario.steps.map((name, i) => [i, (i + 1) + ' / ' + scenario.steps.length + ' · ' + demoText(name)]), step)}</select></div></div><div class="row">${button('Vorheriger Schritt', 'demoPrevious', off || step === 0 ? 'disabled' : '')}${button('Schritt anzeigen', 'demoApply', off ? 'disabled' : '', 'primary')}${button('Nächster Schritt', 'demoNext', off || step + 1 >= scenario.steps.length ? 'disabled' : '')}${button('Aktualisieren', 'demoReload', S.demoBusy ? 'disabled' : '')}</div><p class="help" aria-live="polite">${esc(d.pending ? t('Schritt wird übernommen …') : demoText(d.scenarios.find(row => row.id === d.scenario)?.steps[d.step] || ''))}${d.enabled && d.utc ? ' · ' + esc(dt(d.utc)) : ''}</p></div>`;
}
async function applyDemo(delta = 0) {
    if (S.demoBusy)
        return;
    const d = S.demo, scenario = Number($('demoScenario').value), row = d.scenarios.find(item => item.id === scenario);
    if (!row)
        throw Error(t('Ungültiger Wert'));
    const step = Math.max(0, Math.min(row.steps.length - 1, Number($('demoStep').value) + delta));
    S.demoPick = scenario;
    S.demoPickStep = step;
    S.demoBusy = true;
    render();
    try {
        await mutate('demo', {
            scenario, step
        });
        for (let attempt = 0; attempt < 20; attempt++) {
            const current = await raw('demo');
            S.demo = current;
            if (!current.pending && current.scenario === scenario && current.step === step) {
                await load('status', false);
                return;
            }
            await sleep(250);
        }
        throw Error(t('Demo-Schritt noch nicht bestätigt. Aktualisieren und erneut prüfen.'));
    }
    finally {
        S.demoBusy = false;
        render();
    }
}
function screenshotButton() {
    return button(S.screenshot ? 'Bildschirmfoto wird erstellt …' : 'Bildschirmfoto herunterladen', 'screenshot', `id="screenshot" ${S.screenshot ? 'disabled' : ''}`);
}
function demoNavigationState() {
    const v = S.status;
    if (!S.saved.demo)
        return t('Demo-Modus zuerst einschalten und speichern.');
    if (!v?.demo)
        return t('Demo wird vorbereitet. Status aktualisieren.');
    const phase = S.schema.screen_policy?.phases?.[v.screen_phase] || SP_PHASES[v.screen_phase];
    return [
        t(SP_PHASE_LABEL[phase] || 'Unbekannt'), t(SCREENS[v.screen] || 'Unbekannt'), t(v.screen_manual ? 'Manuelle Anzeige' : v.screen_frame ? 'Bilderrahmen' : 'Automatik'), !v.screen_frame && Number.isInteger(v.screen_automatic_entries) ? tf('Automatische Einträge: %', v.screen_automatic_entries) : ''
    ].filter(Boolean).join(' · ');
}
function paintDemoNavigation() {
    if ($('demoNavigationState'))
        $('demoNavigationState').textContent = demoNavigationState();
    const b = $('demoAutoNext');
    if (b) {
        b.disabled = S.saving || S.demoAdvanceBusy || S.demoBusy || !S.saved.demo || !S.status?.demo;
        b.textContent = t(S.status?.screen_manual ? 'Zur Automatik zurückkehren' : 'Automatik weiterschalten');
    }
}
function debugPage() {
    const m = S.draft;
    return head('Debug') + section('Bildschirmfoto', '', `<div class="row">${screenshotButton()}${button('?', 'screenshotHelp', `aria-label="${ht('Hilfe zum Bildschirmfoto')}"`, 'icon')}</div>` + F('overview_screenshot', 'switch', 'Zeigt den Screenshot-Button zusätzlich auf der Übersicht. Hier unter Debug bleibt er immer erreichbar.')) + section('Demo', '', F('demo', 'switch', 'Zeigt simulierte Spiele statt echter Daten. Nach dem Speichern sind die Demo-Steuerungen verfügbar.') + `<div class="row">${button(S.status?.screen_manual ? 'Zur Automatik zurückkehren' : 'Automatik weiterschalten', 'demoAutoNext', `id="demoAutoNext" ${!S.saved.demo || !S.status?.demo || S.demoAdvanceBusy || S.demoBusy ? 'disabled' : ''}`)}</div><p class="help" id="demoNavigationState" aria-live="polite">${esc(demoNavigationState())}</p><p class="help">${ht('Wechselt zur nächsten automatischen Unterseite oder zum nächsten Bild, danach zur nächsten Bildschirmart oder zum nächsten Wettbewerb. Eine manuelle Anzeige kehrt zuerst zur Automatik zurück. Gespeicherte Zeiten und Demo-Schritt bleiben erhalten.')}</p>` + demoMarkup()) + expert('Protokolle', F('debug_status_log', 'switch', 'Nur zur Fehlersuche: Das Gerät schreibt seinen Zustand über das USB-Kabel an einen angeschlossenen Computer.') + F('debug_status_interval_s', 'number', 'Abstand zwischen zwei Statuszeilen.', null, !m.debug_status_log) + F('browser_debug', 'switch', 'Erlaubt den Live-Protokollstream im Browser. Ausschalten lehnt diese Verbindung ab; Screenshot und Demo bleiben unabhängig.') + `<details class="expert" id="liveDetails"><summary>${ht('Live-Protokoll')}</summary><p class="help">${ht('Live-Protokoll im Browser erlauben aktivieren und speichern.')}</p>${button('Stream verbinden', 'debug')}<pre class="log" id="debugOutput"></pre></details>`) + expert('Gerätewerte', `<div id="networkWarning"></div><dl class="kv" id="diagnosticStatus"></dl>`);
}
async function demoAutoNext() {
    if (S.demoAdvanceBusy || S.demoBusy)
        return;
    if (!S.saved.demo || !S.status?.demo)
        throw Error(t('Demo-Modus zuerst einschalten und speichern.'));
    S.demoAdvanceBusy = true;
    paintDemoNavigation();
    try {
        await mutate('display', {
            action: 'auto_next', config_revision: S.saved._revision
        });
        toast(t(S.status?.screen_manual ? 'Rückkehr zur Automatik angefordert.' : 'Automatischer Wechsel angefordert.'));
        await sleep(500);
        await load('status', false);
    }
    finally {
        S.demoAdvanceBusy = false;
        paintDemoNavigation();
    }
}
const SCREENSHOT_HELP = 'Zeichnet den aktuellen Bildschirm einmal neu und lädt das Ergebnis als PNG herunter. Die Anzeige kann dabei bis zu 5 s pausieren. Die PNG-Datei bleibt ohne zusätzliche Drehung in der aktuellen Leserichtung des Displays. Außerhalb eines runden Displays bleibt das Bild transparent. Erfasst die neu gezeichnete Ausgabe, keine Fehler im LCD selbst. Anmeldung wie bei anderen Geräteaktionen; ohne Admin-Passwort automatisch. Es wird keine Datei auf dem Gerät gespeichert.';
function screenshotFailure(kind) {
    const row = {
        timeout: ['Bildschirmfoto hat zu lange gedauert. WLAN-Empfang prüfen und erneut versuchen.', 'SCREEN E01'], connection: [
            'Bildschirmfoto wurde nicht vollständig übertragen. WLAN-Verbindung prüfen und erneut herunterladen.', 'SCREEN E02'
        ], format: [
            'Bildschirmfoto ist unvollständig oder hat ein unbekanntes Format. Seite neu laden und erneut versuchen; bleibt der Fehler, Fehlercode melden.', 'SCREEN E03'
        ], png: [
            'Der Browser konnte keine PNG-Datei erstellen. Seite neu laden oder einen anderen Browser verwenden.', 'SCREEN E04'
        ]
    }[kind], failure = Error(t(row[0]) + codeText(row[1]));
    return failure;
}
function decodeScreenshot(buffer) {
    const bad = () => {
        throw screenshotFailure('format');
    };
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 20)
        bad();
    const view = new DataView(buffer), width = view.getUint16(4, true), height = view.getUint16(6, true), rotation = view.getUint8(8), round = view.getUint8(9), schema = S.schema;
    if (view.getUint32(0, false) !== 0x46425331 || !width || !height || rotation > 3 || round > 1 || view.getUint8(10) !== 1 || view.getUint8(11) !== 20 || !schema || width !== (rotation % 2 ? schema.height : schema.width) || height !== (rotation % 2 ? schema.width : schema.height) || Boolean(round) !== Boolean(schema.round))
        bad();
    const pixels = new Uint8ClampedArray(width * height * 4), covered = new Uint8Array(width * height);
    let at = 12, count = 0, ended = false;
    while (at < view.byteLength) {
        if (view.byteLength - at < 8)
            bad();
        const x = view.getUint16(at, true), y = view.getUint16(at + 2, true), w = view.getUint16(at + 4, true), h = view.getUint16(at + 6, true);
        at += 8;
        if (!x && !y && !w && !h) {
            if (at !== view.byteLength || count !== width * height)
                bad();
            ended = true;
            break;
        }
        if (!w || !h || h > 20 || x + w > width || y + h > height || view.byteLength - at < w * h * 2)
            bad();
        for (let dy = 0; dy < h; dy++)
            for (let dx = 0; dx < w; dx++) {
                const px = x + dx, py = y + dy, index = py * width + px;
                if (covered[index])
                    bad();
                covered[index] = 1;
                count++;
                const value = view.getUint16(at, true);
                at += 2;
                const out = index * 4;
                pixels[out] = (value >>> 11) * 255 / 31;
                pixels[out + 1] = ((value >>> 5) & 63) * 255 / 63;
                pixels[out + 2] = (value & 31) * 255 / 31;
                pixels[out + 3] = !round || ((2 * px + 1 - width) / width) ** 2 + ((2 * py + 1 - height) / height) ** 2 <= 1 ? 255 : 0;
            }
    }
    if (!ended)
        bad();
    return {
        width, height, rotation, round: Boolean(round), pixels
    };
}
async function screenshotPng(frame) {
    try {
        const canvas = document.createElement('canvas');
        canvas.width = frame.width;
        canvas.height = frame.height;
        const context = canvas.getContext('2d');
        if (!context)
            throw Error();
        const image = context.createImageData(frame.width, frame.height);
        image.data.set(frame.pixels);
        context.putImageData(image, 0, 0);
        return await new Promise((resolve, reject) => canvas.toBlob(blob => blob ? resolve(blob) : reject(Error()), 'image/png'));
    }
    catch {
        throw screenshotFailure('png');
    }
}
function screenshotFilename(date = new Date()) {
    const z = n => String(n).padStart(2, '0');
    return 'fussball-' + date.getFullYear() + '-' + z(date.getMonth() + 1) + '-' + z(date.getDate()) + '_' + z(date.getHours()) + '-' + z(date.getMinutes()) + '-' + z(date.getSeconds()) + '.png';
}
function paintScreenshot() {
    const button = $('screenshot');
    if (button) {
        button.disabled = S.screenshot || S.saving;
        button.textContent = t(S.screenshot ? 'Bildschirmfoto wird erstellt …' : 'Bildschirmfoto herunterladen');
    }
}
function screenshotHelp() {
    const d = dialog(t('Bildschirmfoto'), `<p>${ht(SCREENSHOT_HELP)}</p>`, button('Schließen', 'close'));
    d.querySelector('[data-act=close]').onclick = () => d.close();
}
async function downloadScreenshot() {
    if (S.screenshot)
        return;
    S.screenshot = true;
    paintScreenshot();
    try {
        const frame = decodeScreenshot(await mutate('screenshot', {}, 'POST', true)), blob = await screenshotPng(frame), url = URL.createObjectURL(blob), a = document.createElement('a');
        a.href = url;
        a.download = screenshotFilename();
        try {
            a.click();
        }
        finally {
            setTimeout(() => URL.revokeObjectURL(url), 1000);
        }
    }
    catch (e) {
        if (e instanceof ApiError) {
            const message = {
                screenshot_unavailable: 'Das Display ist noch nicht bereit oder unterstützt kein Bildschirmfoto. Kurz warten und erneut versuchen; bleibt der Fehler, Fehlercode melden.', busy: 'Das Gerät führt gerade eine andere aufwendige Aktion aus. Kurz warten und das Bildschirmfoto erneut herunterladen.', memory: 'Für das Bildschirmfoto reicht der Gerätespeicher gerade nicht. Andere Vorgänge abwarten und erneut versuchen; bleibt der Fehler, Gerät neu starten und Fehlercode melden.', timeout: 'Bildschirmfoto hat zu lange gedauert. WLAN-Empfang prüfen und erneut versuchen.', internal: 'Das Gerät konnte kein Bildschirmfoto erstellen. Erneut versuchen; bleibt der Fehler, Gerät neu starten und Fehlercode melden.'
            }[e.code];
            throw Error((message ? t(message) : e.message) + codeText([e.code, 'HTTP ' + e.status].filter(Boolean).join(' · ')));
        }
        if (e.code === 'request_timeout')
            throw screenshotFailure('timeout');
        if (e.code === 'connection')
            throw screenshotFailure('connection');
        if (e.message === t('Antwort des Geräts ist nicht lesbar.'))
            throw screenshotFailure('format');
        throw e;
    }
    finally {
        S.screenshot = false;
        paintScreenshot();
    }
}
function closeDebug() {
    if (S.socket) {
        S.socket.onclose = null;
        S.socket.close();
        S.socket = null;
    }
}
async function openDebug() {
    if (!S.saved.browser_debug)
        throw Error(t('Unter Debug aktivieren und speichern.'));
    await authenticate();
    if (!S.session.valid)
        await login();
    closeDebug();
    if ($('liveDetails'))
        $('liveDetails').open = true;
    const sock = new WebSocket((location.protocol === 'https:' ? 'wss:' : 'ws:') + '//' + location.host + '/api/v1/debug?token=' + encodeURIComponent(S.token));
    S.socket = sock;
    sock.onmessage = e => {
        if ($('debugOutput')) {
            try {
                $('debugOutput').textContent = JSON.stringify(JSON.parse(e.data), null, 2);
            }
            catch {
                $('debugOutput').textContent = t('Antwort des Geräts ist nicht lesbar.');
            }
        }
    };
    sock.onclose = sock.onerror = () => {
        if ($('debugOutput'))
            $('debugOutput').textContent = t('Stream geschlossen. Erneut öffnen.');
    };
}
