// The single application state, shared value helpers and page/field metadata.
const $ = id => document.getElementById(id), clone = o => o === undefined ? undefined : JSON.parse(JSON.stringify(o)), eq = (a, b) => JSON.stringify(a) === JSON.stringify(b), bytes = s => new TextEncoder().encode(s).length, esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
}[c]));
const S = {
    page: 'overview', schema: null, saved: null, draft: null, status: null, images: null, token: '', session: null, sec: {}, tried: false, server: null, expanded: false, saving: false, confirm: null, scan: null, editor: null, ota: null, screenshot: false, errors: {}, loading: {}, jobs: new Set(), socket: null, open: null
};
const PAGES = {
    overview: 'Übersicht', competitions: 'Wettbewerbe', display: 'Anzeige', images: 'Bilder', settings: 'Einstellungen', debug: 'Debug', system: 'System'
}, PROVIDERS = ['OpenLigaDB', 'API-Football', 'ESPN', 'football-data.org'], SCREENS = [
    'Einzelspiel', 'Konferenz', 'Tabelle', 'Wappen', 'Spieltag', 'Bilderfolge'
], ROLES = ['Aus', 'Pflichttaste', 'Hoch', 'Runter', 'Blättern mit Richtungswechsel'], ROLE_HELP = [
    '', '', 'Konferenz und Spieltag: vorherige Seite. Tabelle: nach oben blättern; gehalten wiederholt.', 'Konferenz und Spieltag: nächste Seite. Tabelle: nach unten blättern; gehalten wiederholt.', 'Gehalten: blättert in der aktuellen Richtung (Konferenz und Spieltag seitenweise, Tabelle zeilenweise). Loslassen wechselt die Richtung; Start ist abwärts.'
], SECRETS = ['wifi_password', 'ap_password', 'admin_password', 'api_football_key', 'football_data_key'];
const LABEL = {
    short_names: 'Kurznamen', wifi_ssid: 'WLAN-Name', wifi_password: 'WLAN-Passwort', ap_password: 'Passwort für das Setup-WLAN', hostname: 'Gerätename', external_antenna: 'Externe Antenne verwenden', admin_password: 'Admin-Passwort', api_football_key: 'Schlüssel', football_data_key: 'Schlüssel', time_zone: 'Zeitzone', ntp_server: 'Zeitserver', language: 'Sprache von Display und Oberfläche', brightness: 'Tageshelligkeit', night_brightness: 'Abdunkeln auf', night_enabled: 'Nachtmodus', night_start: 'Nacht ab', night_end: 'Nacht bis', rotation: 'Drehung', manual_return_s: 'Nach Tastendruck zurück zur Automatik', scroll_repeat_ms: 'Scroll-Schritt bei gehaltener Taste', scroll_reset_s: 'Scroll-Richtung zurücksetzen nach', slideshow_s: 'Bildwechsel', visible_matches: 'Spiele gleichzeitig', table_window: 'Tabellenzeilen', table_page_s: 'Tabelle weiterschalten nach', window_before: 'Vorlauf vor dem ersten Anstoß', window_after: 'Nachlauf nach dem letzten Abpfiff', backgrounds: 'Hintergrundbilder auf allen Bildschirmen', pin_own_match: 'Eigenes Spiel in der Konferenz anheften', whole_matchday: 'Eigenes Spiel den ganzen Spieltag zeigen', conference_mode: 'Konferenz-Modus', matchday_mode: 'Angezeigter Spieltag', input_roles: 'Tasten', demo: 'Demo-Modus', espn_opt_in: 'ESPN verwenden', overview_screenshot: 'Screenshot zusätzlich auf Übersicht anzeigen', browser_debug: 'Live-Protokoll im Browser erlauben', debug_status_log: 'Status regelmäßig in die serielle Konsole schreiben', debug_status_interval_s: 'Statusintervall', ip_badge_s: 'Nach dem Start einblenden für', ip_badge_ap_permanent: 'Im Einrichtungsmodus dauerhaft zeigen', ip_badge_bottom: 'Unten statt oben', daily_requests: 'Höchstens Abrufe pro Tag', window_requests: 'Höchstens Abrufe im Zeitfenster', window_s: 'Zeitfenster', intervals: 'Abrufintervalle', providers: 'Datenanbieter', global_limit: 'Gesamtlimit', screens: 'Bildschirme', id: 'Kennung', fixtures: 'Spielplan', table: 'Tabelle', live: 'Live', live_table: 'Live-Tabelle', events: 'Ereignisse eigenes Spiel', own_match: 'Eigenes Spiel', conference: 'Konferenz', matchday: 'Spieltag', idle_screen: 'Wenn kein Spiel läuft', matchday_screen: 'Wenn andere Spiele laufen', own_match_screen: 'Wenn das eigene Spiel läuft', competition_name: 'Wettbewerb', club: 'Verein', name: 'Vereinsname', group: 'Vereinsgruppe', display: 'Anzeige', own_special: 'Eigene Sonderbehandlung', enabled: 'Anzeigen', background: 'Hintergrundbild', image: 'Bild', colour: 'Grundfarbe', text: 'Text', accent: 'Akzent', text_scale: 'Textgröße', active: 'Aktiv', primary: 'Quelle', provider: 'Anbieter', competition: 'Wettbewerb-ID', team: 'Verein-ID', season: 'Saison'
};
const t = k => S.draft?.language === 1 ? W[k] || k : k, tf = (k, v) => t(k).replace('%', v), ht = k => esc(t(k)), sleep = ms => new Promise(r => setTimeout(r, ms));
const parts = p => p.replace(/\[(\d+)\]/g, '.$1').split('.'), get = (o, p) => parts(p).reduce((v, k) => v?.[k], o), set = (o, p, v) => {
    const a = parts(p), k = a.pop();
    (a.length ? get(o, a.join('.')) : o)[k] = v;
}, colour = n => '#' + Number(n).toString(16).padStart(6, '0'), kb = n => (Number(n || 0) / 1024).toFixed(1) + ' KB', dt = n => n ? new Date(n * 1000).toLocaleString(S.draft?.language === 1 ? 'en-GB' : 'de-DE') : '—';
function pageOf(p) {
    if (/^(demo|overview_screenshot|browser_debug|debug_status_log|debug_status_interval_s)$/.test(p))
        return 'debug';
    if (p === 'screen_policy.frame')
        return 'overview';
    if (/^competitions\[\d+\]\.display/.test(p))
        return 'display';
    if (p.startsWith('screen_policy'))
        return /\.own_special$/.test(p) ? 'competitions' : 'display';
    return /^competitions/.test(p) ? 'competitions' : /^(idle_screen|matchday_screen|own_match_screen|screens|slides|brightness|night_|rotation|visible_matches|table_window|table_page|manual_return|scroll_|slideshow|window_|ip_badge|backgrounds|pin_own_match|whole_matchday|conference_mode|matchday_mode|input_roles)/.test(p) ? 'display' : 'settings';
}
// Request limits are rows per provider (index = provider); the competition
// lists and screen rules share the one application-config draft.
const LIMITS = ['providers'], LIMIT_KEYS = ['daily_requests', 'window_requests', 'window_s'], LISTS = ['competitions'];
// Settings, competitions, screen policy and adopted names apply together.
function labelOf(p) {
    if (p.startsWith('screen_policy') || /^competitions\[\d+\]\.display/.test(p)) {
        const a = parts(p);
        return a.map((k, i) => /^\d+$/.test(k) ? i === a.length - 1 && Number(k) <= 2 && SP_TYPES.includes(a[i - 1]) ? t(['Freigeben', 'Automatisch', 'Besuchszeit'][k]) : String(Number(k) + 1) : t(({
            screen_policy: 'Anzeige', phases: 'Phasen', appearance: 'Darstellung', competitions: 'Wettbewerbe', own_special: 'Eigene Sonderbehandlung', own_before_min: 'Eigene Spiele: Vorlauf', own_after_min: 'Eigene Spiele: Nachlauf', other_before_min: 'Andere Spiele: Vorlauf', other_after_min: 'Andere Spiele: Nachlauf', frame: 'Bilderrahmen', frame_crest: 'Wappen im Bilderrahmen', automatic_break: 'Automatische Saisonpause', break_days: 'Pausenschwelle in Tagen', per_image_s: 'Bildwechsel', order: 'Gemeinsame Reihenfolge', per_competition: 'Abweichungen je Wettbewerb', only_one_own_single: 'Nur ein eigenes Einzelspiel'
        })[k] || SP_PHASE_LABEL[k] || SP_TYPE_LABEL[k] || LABEL[k] || k)).join(' · ');
    }
    const a = parts(p);
    return a.map((k, i) => /^\d+$/.test(k) ? LIMITS.includes(a[i - 1]) ? PROVIDERS[k] ?? String(Number(k) + 1) : String(Number(k) + 1) : t(LABEL[k] || PAGES[k] || k)).join(' · ');
}
