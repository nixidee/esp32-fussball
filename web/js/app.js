// Application wiring, user actions, theme and autonomous polling.
let theme = 'auto';
try {
    theme = localStorage.getItem('fussball-theme') || 'auto';
}
catch {
}
if (!['auto', 'light', 'dark'].includes(theme))
    theme = 'auto';
function applyTheme() {
    if (theme === 'auto')
        delete document.documentElement.dataset.theme;
    else
        document.documentElement.dataset.theme = theme;
    try {
        localStorage.setItem('fussball-theme', theme);
    }
    catch {
    }
    renderHeader();
}
$('theme').onclick = () => {
    theme = ['auto', 'light', 'dark'][(['auto', 'light', 'dark'].indexOf(theme) + 1) % 3];
    applyTheme();
};
$('auth').onclick = async () => {
    try {
        if (S.session?.valid) {
            await raw('logout', {}, 'POST');
            S.token = '';
            S.session.valid = false;
            closeDebug();
            renderHeader();
        }
        else
            await login();
    }
    catch (e) {
        toast(e.message, true);
    }
};
const ACT = {
    namesManage: () => openNames(),
    clubGroup: el => openClubGroup(Number(el.dataset.group || 0)),
    demoAutoNext: () => demoAutoNext(), saveRefresh: () => refreshSave(), demoReload: () => load('demo'), demoApply: () => applyDemo(), demoPrevious: () => applyDemo(-1), demoNext: () => applyDemo(1), demoHelp: () => {
        const d = dialog(t('Hilfe'), `<p>${ht('Steuert simulierte Spieldaten und eine separate Testzeit. Die ersten beiden gespeicherten Wettbewerbe liefern Reihenfolge und Anzeige-Regeln. Fehlt eine gespeicherte Zuordnung, gelten die gemeinsamen Regeln. Systemuhr und echte Anbieter werden nicht verändert; Szenarien werden nicht gespeichert.')}</p><p>${ht('Die Demo ersetzt keine echten Anbieterantworten oder die Prüfung auf dem Gerät. Eine leere Automatik oder deaktivierte Ansicht bleibt auch im Szenario leer oder deaktiviert.')}</p>`, button('Schließen', 'close'));
        d.querySelector('[data-act=close]').onclick = () => d.close();
    }, policyPhase: el => {
        S.screenPhase = el.dataset.phase;
        render();
    }, policyAppearance: el => {
        S.screenAppearance = el.dataset.type;
        render();
    }, policyHelp: el => {
        const d = dialog(t('Hilfe'), `<p>${esc(t(el.dataset.help))}</p>`, button('Schließen', 'close'));
        d.querySelector('[data-act=close]').onclick = () => d.close();
    }, policySplit: el => {
        const a = S.draft.screen_policy.phases[el.dataset.phase].per_competition, type = el.dataset.type, i = a.indexOf(type);
        if (i < 0)
            a.push(type);
        else
            a.splice(i, 1);
        render();
    }, policyOrder: el => {
        const a = S.draft.screen_policy.order, i = Number(el.dataset.i), j = i + Number(el.dataset.dir);
        if (j >= 0 && j < a.length)
            [a[i], a[j]] = [a[j], a[i]];
        render();
    }, policyReload: () => confirmAction('Anzeige neu laden', 'Verwirft nur den Anzeigeentwurf. Andere noch offene Änderungen bleiben erhalten.', async () => {
        const fresh = await readModel(), edited = !sameObject(S.saved.competitions, S.draft.competitions);
        if (!edited)
            S.draft.competitions = clone(fresh.competitions);
        S.saved.competitions = clone(fresh.competitions);
        S.saved.screen_policy = clone(fresh.screen_policy);
        S.draft.screen_policy = clone(fresh.screen_policy);
        prepareScreenModel(S.draft);
        S.server = null;
        render();
    }), go: el => {
        if (ED.busy)
            return;
        closeDebug();
        closeEditor();
        S.page = el.dataset.page;
        render();
        scrollTo(0, 0);
    }, retry: el => load(el.dataset.key), refresh: async () => {
        await mutate('data/refresh', {});
        toast(t('Abruf vorgemerkt.'));
        await load('status', false);
    }, next: async () => {
        await mutate('display', {
            action: 'next', config_revision: S.saved._revision
        });
        toast(t('Aktion an das Display gesendet.'));
        await sleep(CLIENT.action_settle_ms);
        await load('status', false);
    }, ip: async () => {
        await mutate('display', {
            action: 'ip', config_revision: S.saved._revision
        });
        toast(t('Aktion an das Display gesendet.'));
    }, secret: el => {
        S.sec[el.dataset.key] = {
            mode: el.dataset.mode, value: ''
        };
        render();
        if (el.dataset.mode === 'change')
            $('f-' + el.dataset.key)?.focus();
    }, expand: () => {
        S.expanded = !S.expanded;
        renderBar();
    }, discard: () => {
        if (S.pendingApply) {
            toast(t('Speicherergebnis zuerst prüfen.'), true);
            return;
        }
        fresh(S.saved);
        render();
        toast(t('Änderungen verworfen.'));
    }, jump: el => jump(el.dataset.path), save: () => save(), compNew: () => openWizard(), compEdit: el => openWizard(Number(el.dataset.i)), compMove: el => moveCompetition(Number(el.dataset.i), Number(el.dataset.dir)), compNames: el => openNames(S.draft.competitions[Number(el.dataset.i)]), compOpen: el => {
        const i = Number(el.dataset.i);
        S.open = S.open === i ? null : i;
        render();
    }, compDelete: el => {
        const i = Number(el.dataset.i), c = S.draft.competitions[i];
        confirmAction('Wettbewerbe', tf('„%“ entfernen?', compName(c)) + ' ' + t('Wirksam nach „Speichern“.'), () => removeCompetition(i), 'Entfernen');
    }, slideAdd: () => {
        const slides = S.draft.screen_policy.slides;
        if (slides.length >= S.schema.screen_policy.max_slides)
            throw Error(t('Maximale Listenlänge erreicht.'));
        slides.push(Number($('slideAdd').value));
        render();
    }, slideMove: el => {
        const i = Number(el.dataset.i), n = i + Number(el.dataset.dir), a = S.draft.screen_policy.slides;
        if (n >= 0 && n < a.length)
            [a[i], a[n]] = [a[n], a[i]];
        render();
    }, slideDelete: el => {
        S.draft.screen_policy.slides.splice(Number(el.dataset.i), 1);
        render();
    }, imageEdit: el => openEditor(Number(el.dataset.id)), roleEdit: el => openEditor(0, el.dataset.role), roleCopy: el => {
        const kind = el.dataset.role;
        confirmAction('Aus Bibliothek übernehmen', 'Das Bild wird auf dem Gerät kopiert und zählt zum Speicher. Die Bibliothek bleibt unverändert.', () => changeRole(kind, 'copy', Number($('roleSource').value)), 'Übernehmen', `<p><b>${esc(roleName(kind))}</b></p><div class="field"><label for="roleSource">${ht('Bild')}</label><select id="roleSource" class="w-m">${opts(imageOptions().slice(1), S.images?.items?.[0]?.id)}</select></div>`);
    }, roleReset: el => {
        const kind = el.dataset.role;
        confirmAction('Werksbild wiederherstellen', 'Danach erscheint wieder das Werksbild. Die eigene Datei wird sofort gelöscht.', () => changeRole(kind, 'reset'), 'Wiederherstellen', `<p><b>${esc(roleName(kind))}</b></p>`);
    }, editorClose: () => {
        if (ED.busy)
            return;
        closeEditor();
        render();
    }, imageFit: () => fitImage(false), imageFill: () => fitImage(true), imageUpload: () => uploadImage(), imageDelete: el => {
        const id = Number(el.dataset.id), u = [...new Set([...uses(id), ...uses(id, S.saved)])];
        confirmAction('Löschen', 'Danach erscheint dort das Standardbild. Die Datei wird sofort gelöscht.', () => deleteImage(id), 'Löschen', `<p><b>${esc(imageName(id))}</b> · ${ht('Wird verwendet als')}: ${esc(u.length ? u.join(', ') : t('Nicht verwendet'))}</p>`);
    }, scan: () => scanWifi(), ssid: el => {
        S.draft.wifi_ssid = el.dataset.v;
        S.scan = null;
        render();
        toast(t('WLAN-Name übernommen. Passwort prüfen, dann speichern.'));
    }, export: () => exportSettings(), reboot: () => confirmAction('Neustart', 'Das Display ist kurz aus. Die Seite verbindet sich danach erneut.', reboot, 'Gerät neu starten'), repair: () => factoryDialog(true), imagesDeleteAll: () => confirmAction('Alle eigenen Bilder löschen', 'Jede Datei der Bibliothek und jede Auswahl einer fehlenden Datei wird einzeln gelöscht. Wappen und Standard-Hintergrund zeigen danach wieder die Werksbilder. Teilfehler bleiben sichtbar.', deleteAllImages, 'Löschen'), factory: () => factoryDialog(), screenshot: () => downloadScreenshot(), screenshotHelp: () => screenshotHelp(), debug: () => openDebug(), otaUpload: () => runOta()
};
document.addEventListener('click', async (e) => {
    const el = e.target.closest('[data-act]');
    if (!el || el.disabled || el.closest('#dialog'))
        return;
    if (S.saving)
        return;
    e.preventDefault();
    const fn = ACT[el.dataset.act];
    if (!fn)
        return;
    el.disabled = true;
    try {
        await fn(el);
    }
    catch (err) {
        if (!err.saveFeedback)
            toast(err.message, err.severity || true);
    }
    finally {
        if (el.isConnected)
            el.disabled = false;
    }
});
document.addEventListener('input', e => {
    const el = e.target;
    if (S.saving)
        return;
    if (el.dataset.inherit) {
        const path = el.dataset.inherit, a = parts(path);
        set(S.draft, path, el.checked ? null : S.draft.screen_policy.phases[a[3]][a[4]][2]);
        S.server = null;
        render();
        return;
    }
    if (el.dataset.secret) {
        S.sec[el.dataset.secret] = {
            mode: 'change', value: el.value
        };
        if (S.server?.path === el.dataset.secret)
            S.server = null;
        paintErrors();
        renderNav();
        renderBar();
        return;
    }
    const path = el.dataset.path;
    if (!path)
        return;
    let value = el.type === 'checkbox' ? el.checked : el.value;
    if (el.dataset.kind === 'ssid-base64')
        value = ssidValue(value);
    else if (el.dataset.kind === 'policybool')
        value = value === '' ? null : value === 'true';
    else if (el.dataset.kind === 'num')
        value = value === '' ? '' : Number(value);
    else if (el.dataset.kind === 'colour')
        value = parseInt(value.slice(1), 16);
    else if (el.dataset.kind === 'time')
        value = value === '' ? '' : Number(value.slice(0, 2)) * 60 + Number(value.slice(3, 5));
    set(S.draft, path, value);
    if (/^screen_policy\.phases\.[^.]+\.(crest|images)\[0\]$/.test(path) && value === true)
        set(S.draft, path.replace(/\[0\]$/, '[1]'), true);
    if (S.server?.path === path)
        S.server = null;
    paintErrors();
    renderNav();
    renderBar();
});
document.addEventListener('change', e => {
    if (e.target.id === 'demoScenario') {
        S.demoPick = Number(e.target.value);
        S.demoPickStep = 0;
        render();
    }
    else if (e.target.id === 'demoStep')
        S.demoPickStep = Number(e.target.value);
});
document.addEventListener('change', e => {
    if (S.saving)
        return;
    const el = e.target;
    if (el.dataset.path && (el.hasAttribute('data-refresh') || el.dataset.kind === 'colour'))
        render();
});
async function startup() {
    applyTheme();
    render();
    await load('session', false);
    await Promise.allSettled(['schema', 'settings', 'status', 'images'].map(key => load(key, false)));
    render();
    let retryMs = CLIENT.startup_retry_ms;
    for (;;) {
        await sleep(retryMs);
        const failed = Object.keys(S.errors).filter(k => k !== 'status');
        if (failed.length) {
            await Promise.allSettled(failed.map(k => load(k)));
            retryMs = Math.min(CLIENT.startup_retry_max_ms, retryMs * 2);
        }
        else
            retryMs = CLIENT.startup_idle_ms;
    }
}
// Autonomous work starts here.
setInterval(() => load('status', false), CLIENT.status_poll_ms);
window.addEventListener('pagehide', () => {
    closeDebug();
    if (ED.bmp?.close)
        ED.bmp.close();
});
startup().catch(e => toast(e.message, true));
