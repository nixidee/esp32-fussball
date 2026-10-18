// Actionable service errors, operating states and diagnostic formatting.
const ERROR = {
    json: 'JSON-Daten sind ungültig. Eingabe prüfen und erneut versuchen.', body_incomplete: 'Übertragung der Anfrage wurde unterbrochen. Verbindung prüfen und erneut versuchen.',
    demo_disabled: 'Demo-Modus zuerst einschalten und speichern.', origin: 'Zugriff über diese Adresse nicht erlaubt. Öffne die lokale Geräteadresse.', password: 'Passwort wurde abgelehnt.', session: 'Zum Ändern anmelden', trial: 'Das Gerät testet ein Update. Änderungen sind nach dem Test wieder möglich.', busy: 'Das Gerät ist beschäftigt. Warte kurz und versuche es erneut.', changed: 'Der Abruf wurde durch eine andere Anfrage geändert. Erneut versuchen.', memory: 'Speicher reicht nicht aus.', storage_full: 'Bildspeicher ist voll.', size: 'Datei oder Wert ist zu groß.', content: 'Datei passt nicht zum Gerät oder ist beschädigt.', timeout: 'Zeitüberschreitung. Erneut versuchen.', not_found: 'Nicht verfügbar', state: 'Aktion momentan nicht möglich.', invalid: 'Ungültiger Wert', range: 'Wert außerhalb des erlaubten Bereichs', type: 'Falscher Datentyp', unknown_key: 'Unbekanntes Feld', too_long: 'Text ist zu lang', too_many: 'Zu viele Einträge', required: 'Wert wird benötigt', duplicate: 'Doppelter Eintrag', conflict: 'Werte passen nicht zusammen', none_enabled: 'Mindestens einen Bildschirm einschalten', reset_partial: 'Werkseinstellungen unvollständig; Daten können bereits geändert sein. Erneut zurücksetzen. Bei unlesbarem Speicher die vollständige Formatierung auswählen.'
};
const CADENCE = {
    live: 'Spiel läuft', prematch: 'Vor dem Anpfiff', postmatch: 'Nach dem Abpfiff', idle: 'Kein Spiel im Zeitfenster', window: 'Spielfenster beginnt', budget: 'Tageslimit knapp: Abrufe seltener oder ausgelassen', budget_used: 'Tageslimit erreicht', limit: 'Abruflimit im Zeitfenster', retry: 'Neuer Versuch nach einem Fehler', waiting: 'Wartet auf WLAN und Uhrzeit', update_test: 'Update-Test läuft'
};
const NETR = {
    connecting: ['Verbinde …', 'Connecting …'], not_found: ['Netz nicht gefunden', 'Network not found'], password: ['Passwort falsch?', 'Wrong password?'], setup_ap: ['Setup-Netz aktiv', 'Setup network on']
};
// Local service failures are separate from the station connection state.
const NET_INIT = {
    antenna: 'Antennenschalter', network_stack: 'Netzwerkbasis', station_interface: 'WLAN-Schnittstelle', ap_interface: 'Setup-Schnittstelle', ap_address: 'Setup-IP', wifi_driver: 'WLAN-Treiber', wifi_configuration: 'WLAN-Konfiguration', wifi_events: 'WLAN-Ereignisse', radio: 'WLAN-Start', portal_dns: 'Setup-DNS', mdns: 'Namensdienst', hostname: 'Gerätename', http_service: 'Web-Ankündigung'
};
function networkFailure(s, english = S.draft?.language === 1) {
    const n = s?.network_init;
    if (!n?.error)
        return null;
    const tx = k => english ? W[k] || k : k, phase = tx(NET_INIT[n.step] || 'Netzwerkdienst'), hasIp = [s.network === 'connected' ? s.ip : '', s.ap ? s.ap_ip : ''].some(ip => ip && ip !== '0.0.0.0'), useIp = hasIp && ['portal_dns', 'mdns', 'hostname', 'http_service'].includes(n.step), action = tx(useIp ? 'IP im Browser öffnen' : 'Neuer Versuch läuft'), retry = n.retry_ms > 0 ? tx('Neuer Versuch') + ' ' + monoClock(n.retry_ms) : tx('Neuer Versuch läuft'), code = n.code || '';
    return {
        phase, action, code, pill: (code ? code + ' · ' : '') + tx(useIp ? 'IP nutzen' : 'neuer Versuch'), title: tx('Netzwerkstart gestört'), message: phase + ': ' + tx(n.error_name === 'ESP_ERR_NO_MEM' ? 'Speicher reicht nicht aus.' : 'Lokaler Netzwerkdienst konnte nicht gestartet oder aktualisiert werden.') + ' ' + (useIp ? tx('Im Browser die IP-Adresse des Geräts verwenden.') + ' ' : '') + tx('Automatischer neuer Versuch. Bleibt der Fehler, Gerät neu starten und Fehlercode melden.') + ' ' + retry + (code ? ' ' + tx('Fehlercode') + ': ' + code : '') + (n.error_name ? ' (' + n.error_name + ')' : '')
    };
}
const IMAGE_CAUSE = {
    missing: 'Bilddatei fehlt.', unavailable: 'Bild nicht lesbar.', invalid: 'Bilddatei nicht nutzbar.', timeout: 'Bildprüfung hat zu lange gedauert.', source: 'Bildquelle konnte nicht übernommen werden.', decode: 'Bild konnte beim Zeichnen nicht gelesen werden.', memory: 'Speicher reicht nicht aus.'
};
function imageFailure(s, english = S.draft?.language === 1) {
    const im = s?.image;
    if (!im?.active || !im.reason)
        return null;
    const tx = k => english ? W[k] || k : k, role = tx(im.role === 'crest' ? 'Standard-Wappen' : 'Standard-Hintergrund'), desired = im.id ? tx('Bild %').replace('%', im.id) : role, kind = k => tx(k === 'library' ? 'Bibliotheksbild' : k === 'replacement' ? 'Ersatzdatei' : k === 'factory' ? 'Werksbild' : 'Kein Bild'), fallback = im.actual ? tx('Ersatz aktiv') + ': ' + (im.actual === 'factory' ? kind(im.actual) : role) : tx('Kein verwendbares Bild; Fußballinhalte bleiben sichtbar.'), action = tx(im.reason === 'unavailable' || im.reason === 'memory' ? 'Gerät neu starten. Bleibt der Fehler, Fehlercode melden.' : 'Unter Bilder das gewünschte Bild ersetzen oder das Werksbild wiederherstellen.'), retry = im.retry_ms > 0 ? tx('Neuer Versuch') + ' ' + monoClock(im.retry_ms) : tx('Neuer Versuch läuft'), code = im.code || '';
    return {
        code, desired, kind: kind(im.failed), fallback, action, pill: (code ? code + ' · ' : '') + tx(['missing', 'invalid'].includes(im.reason) ? 'Bild ersetzen' : 'Web prüfen'), message: desired + ' (' + kind(im.failed) + '): ' + tx(IMAGE_CAUSE[im.reason] || 'Bild kann nicht angezeigt werden.') + ' ' + fallback + ' ' + action + ' ' + (!im.actual && im.failed === 'factory' && ['missing', 'invalid'].includes(im.reason) ? tx('Werksbilder mit USB-uploadfs wiederherstellen; dabei können eigene Bilder verloren gehen.') + ' ' : '') + retry + (code ? ' ' + tx('Fehlercode') + ': ' + code : '') + (im.error_name ? ' (' + im.error_name + ')' : '')
    };
}
const SOURCE = {
    rejected: 'Gespeicherte Einstellungen waren ungültig. Das Gerät nutzt Startwerte; Speichern überschreibt die gespeicherten Einstellungen.', damaged: 'Gespeicherte Einstellungen sind beschädigt. Das Gerät nutzt Startwerte; Speichern überschreibt die gespeicherten Einstellungen.', outdated: 'Einstellungen stammen aus einer älteren Firmware und wurden verworfen. Bitte neu einrichten.', unreadable: 'Gespeicherte Einstellungen konnten nicht gelesen werden. Das Gerät nutzt Startwerte; Speichern ist bis zum Zurücksetzen gesperrt.', no_storage: 'Einstellungsspeicher nicht verfügbar. Das Gerät nutzt Startwerte; Änderungen werden nicht gespeichert.'
};
const COMP_SOURCE = {
    rejected: 'Gespeicherte Wettbewerbe waren ungültig. Das Gerät nutzt Startwerte; Speichern überschreibt die gespeicherten Wettbewerbe.', damaged: 'Gespeicherte Wettbewerbe sind beschädigt. Das Gerät nutzt Startwerte; Speichern überschreibt die gespeicherten Wettbewerbe.', outdated: 'Wettbewerbe stammen aus einer älteren Firmware und wurden verworfen. Bitte neu einrichten.', unreadable: 'Gespeicherte Wettbewerbe konnten nicht gelesen werden. Das Gerät nutzt Startwerte; Speichern ist bis zum Zurücksetzen gesperrt.', no_storage: SOURCE.no_storage
};
// Cause and what to do per failure key of the REST API (docs/API.md "Error codes").
const CAUSE = {
    provider_off: ['Anbieter nicht aktiviert.', 'Schlüssel oder Freigabe unter Einstellungen speichern.'], budget_day: [
        'Tagesbudget verbraucht.', 'Neue Abrufe ab 0:00 UTC; Tageslimit des Anbieters unter Einstellungen prüfen.'
    ], busy: ['Gerät war zu lange beschäftigt.', 'Kurz warten und erneut versuchen.'], net_lost: ['WLAN-Verbindung verloren.', 'WLAN-Empfang prüfen.'], time_invalid: ['Uhrzeit noch ungültig.', 'Internetzugang prüfen; die Uhrzeit kommt vom Zeitserver.'], settings_changed: ['Einstellungen während des Abrufs geändert.', 'Erneut versuchen.'], rate_limited: ['Anbieter begrenzt Anfragen.', 'Später erneut versuchen.'], auth: ['Anbieter hat den Zugang abgelehnt.', 'Gespeicherten Schlüssel unter Einstellungen prüfen.'], not_found: ['Wettbewerb oder Saison beim Anbieter nicht gefunden.', 'Wettbewerb bearbeiten und neu auswählen.'], http_error: [
        'Anbieter hat einen Fehler gemeldet.', 'Später erneut versuchen; bleibt der Fehler, einen anderen Anbieter wählen.'
    ], connect: ['Verbindung zum Anbieter fehlgeschlagen.', 'Internetzugang prüfen.'], bad_response: ['Anbieterantwort unvollständig oder unlesbar.', 'Bleibt der Fehler, einen anderen Anbieter wählen.'], too_large: ['Anbieterantwort zu groß für das Gerät.', 'Einen anderen Anbieter für diesen Wettbewerb wählen.'], memory: ['Speicher reicht nicht aus.', 'Bleibt der Fehler, Gerät neu starten und Fehlercode melden.'], other: ['Abruf fehlgeschlagen.', 'Bleibt der Fehler, Fehlercode melden.'], unmaintained: ['Anbieter trägt keine Ergebnisse ein.', 'Einen anderen Anbieter für diesen Wettbewerb wählen.'], no_season: [
        'Anbieter meldet keine laufende Saison.', 'Saison im Wettbewerb prüfen oder auf die neue Saison warten.'
    ], counter: [
        'Abrufzähler konnte nicht gespeichert werden.', 'Gerät neu starten; bleibt der Fehler, Fehlercode melden.'
    ], store_full: [
        'Nicht alle Daten passen in den Gerätespeicher.', 'Weniger Wettbewerbe oder Wettbewerbe mit weniger Mannschaften auswählen.'
    ]
};
const NOTICE_CAUSES = ['unmaintained', 'store_full'], MISSING = {
    matches: 'Spielliste', table: 'Tabelle', teams: 'Mannschaftsnamen', events: 'Spielereignisse'
};
// Unresolved update outcomes of runOta(): cause and what to do per client code.
const OTA_CODE = {
    ota_no_restart: [
        'Die gewählte Firmware läuft bereits; kein Neustart erkannt.', 'Version unter System prüfen; für ein Update eine andere Firmware-Datei wählen.'
    ], ota_other_firmware: [
        'Nach dem Neustart läuft eine andere als die gewählte Firmware.', 'Version unter System prüfen und Update wiederholen.'
    ], ota_no_answer: [
        'Update nicht innerhalb von 2 Minuten abgeschlossen: kein Neustart oder keine Antwort des Geräts.', 'Seite neu laden, Version unter System prüfen und Update bei Bedarf wiederholen.'
    ], ota_trial_open: [
        'Neue Firmware läuft noch im Test; die Bestätigung blieb aus.', 'Einige Minuten warten und Seite neu laden.'
    ]
};
const codeText = code => ' ' + t('Fehlercode') + ': ' + code;
function causeMessage(key, code, missing) {
    const c = CAUSE[key] || CAUSE.other, gone = missing?.length ? ' ' + t('Betroffen') + ': ' + missing.map(k => t(MISSING[k] || k)).join(', ') + '.' : '';
    return t(c[0]) + gone + ' ' + t(c[1]) + (code ? codeText(code) : '');
}
// Local HH:MM of a monotonic device time in ms.
function monoClock(ms) {
    const d = new Date(Date.now() - (S.status?.uptime_s || 0) * 1000 + ms), z = n => String(n).padStart(2, '0');
    return z(d.getHours()) + ':' + z(d.getMinutes());
}
// Next scheduled provider round and its reason (/status, monotonic ms like last_update_ms).
function nextRequestText() {
    if (S.status?.football_paused)
        return t('Automatische Fußballabfragen sind im Bilderrahmen pausiert.');
    const s = S.status, why = CADENCE[s?.next_request_reason];
    if (!why)
        return '';
    if (!s.next_request_ms)
        return t('Nächster Abruf') + ': ' + t(why);
    const sec = Math.round(s.next_request_ms / 1000 - s.uptime_s), d = new Date(Date.now() + sec * 1000), z = n => String(n).padStart(2, '0');
    return t('Nächster Abruf') + ': ' + (sec <= 0 ? t('jetzt') : sec < 120 ? tf('in % s', sec) : tf('um %', z(d.getHours()) + ':' + z(d.getMinutes()))) + ' · ' + t(why);
}
function heapMinimum(s) {
    const scope = s.heap_min_scope, known = (scope === 'lifetime' || scope === 'interval') && Number.isSafeInteger(s.heap_min) && s.heap_min >= 0;
    return [
        scope === 'lifetime' ? 'Kleinster freier Speicher seit Start' : scope === 'interval' ? 'Kleinster freier Speicher im Messintervall' : 'Kleinster freier Speicher', known ? kb(s.heap_min) : t('Nicht verfügbar')
    ];
}
function taskMinimum(r) {
    const free = Number.isSafeInteger(r.stack_min_free) && r.stack_min_free >= 0, usable = Number.isSafeInteger(r.stack_usable_bytes) && r.stack_usable_bytes > 0;
    return (r.name || '#' + r.id) + ': ' + (free ? r.stack_min_free + ' B' : t('Nicht verfügbar')) + ' / ' + (usable ? r.stack_usable_bytes + ' B' : t('Nicht verfügbar')) + (free && usable ? ' (' + (100 * r.stack_min_free / r.stack_usable_bytes).toFixed(1) + ' %)' : '');
}
function firmwareUsage(s) {
    const bytes = s.app_bytes, max = S.schema?.ota_max;
    return Number.isSafeInteger(bytes) && bytes > 0 && Number.isSafeInteger(max) && max > 0 ? kb(bytes) + ' / ' + kb(max) + ' (' + (100 * bytes / max).toFixed(1) + ' %)' : t('Nicht verfügbar');
}
