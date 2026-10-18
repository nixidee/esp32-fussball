// Firmware update, settings import/export, reset, restart and WiFi actions.
let firmwareFile = null, firmwareMeta = null;
function decodeFirmware(buffer, expected, schema, max, total) {
    const a = new Uint8Array(buffer), v = new DataView(buffer);
    if (total < 380 || total > max || a.length < 380)
        throw Error(t('Datei oder Wert ist zu groß.'));
    const text = (offset, count) => new TextDecoder().decode(a.subarray(offset, offset + count)).split('\0')[0];
    if (a[0] !== 0xe9 || v.getUint32(32, true) !== 0xabcd5432 || v.getUint32(288, true) !== 0x46424c31 || v.getUint32(292, true) !== schema.firmware_identity_version || text(296, 48) !== expected.target || text(344, 32) !== expected.layout || v.getUint32(376, true) < schema.config_version)
        throw Error(t('Datei passt nicht zum Gerät oder ist beschädigt.'));
    const id = [...a.subarray(176, 208)].map(n => n.toString(16).padStart(2, '0')).join('');
    if (/^0{64}$/.test(id))
        throw Error(t('Datei passt nicht zum Gerät oder ist beschädigt.'));
    return {
        id, version: text(48, 32), target: text(296, 48), layout: text(344, 32), format: v.getUint32(376, true)
    };
}
function bindSystem() {
    $('firmwareFile').onchange = async (e) => {
        firmwareFile = e.target.files[0] || null;
        firmwareMeta = null;
        $('otaUpload').disabled = true;
        $('otaCheck').textContent = '';
        if (!firmwareFile)
            return;
        try {
            if (!S.status || !S.schema.ota_max)
                throw Error(t('Nicht verfügbar'));
            firmwareMeta = decodeFirmware(await firmwareFile.slice(0, 1024).arrayBuffer(), S.status, S.schema, S.schema.ota_max, firmwareFile.size);
            $('otaCheck').textContent = t('Datei geprüft') + ': ' + firmwareMeta.version + ' · ' + kb(firmwareFile.size) + ' · ' + firmwareMeta.id;
            $('otaUpload').disabled = S.ota?.busy === true;
        }
        catch (e) {
            $('otaCheck').textContent = e.message;
        }
    };
    $('importFile').onchange = async e => {
        const file = e.target.files[0]; e.target.value = '';
        if (!file) return;
        try {
            if (S.pendingApply) throw Error(t('Speicherergebnis zuerst prüfen.'));
            if (file.size > S.schema.max_apply_body * 2) throw Error(t('Datei oder Wert ist zu groß.'));
            const imported = JSON.parse(await file.text());
            if (imported.version !== S.schema.config_version || !imported.desired ||
                Object.keys(imported).some(k => !['version', 'desired', 'adopted_names'].includes(k)) ||
                Object.keys(imported.desired).some(k => !['settings', 'competitions', 'screens'].includes(k)))
                throw Error(t('Datei passt nicht zum Gerät oder ist beschädigt.'));
            const fields = imported.desired.settings;
            const model = validateImport({...fields, competitions: imported.desired.competitions,
                screen_policy: imported.desired.screens});
            const collection = validateAdoptedNames(imported.adopted_names);
            const clipped = namesAdoptionChanges(collection);
            confirmAction('Einstellungen importieren',
                'Import lädt Konfiguration und Namen in den gemeinsamen Entwurf. Erst Speichern übernimmt sie.', () => {
                    S.draft = model;
                    S.sec = Object.fromEntries(SECRETS.filter(k => typeof fields[k] === 'string').map(k => [k,
                        {mode: fields[k] === '' ? 'clear' : 'change', value: fields[k]}]));
                    S.pendingNames = collection; S.tried = true; S.server = null; S.saveNotice = null;
                    render();
                }, 'In Entwurf übernehmen', clipped.length ? `<ul>${clipped.map(x => `<li>${esc(x)}</li>`).join('')}</ul>` : '');
        } catch (error) { toast(error.message, true); }
    };
    paintOta();
}
async function exportSettings() {
    const include = $('exportSecrets').checked;
    if (include && !confirm(t('Diese Datei enthält Geheimnisse. Sicher aufbewahren.'))) return;
    const data = await mutate('export', {secrets: include});
    downloadJson(data, 'fussball-settings.json');
}
function paintOta() {
    const u = S.ota;
    if (!$('otaState') || !u)
        return;
    const names = {
        upload: 'Übertragen', boot: 'Gerät startet neu, warte auf Antwort …', trial: 'Neue Firmware läuft im Test. Bestätigung abwarten.', done: 'Update bestätigt.', rollback: 'Gerät ist zur vorherigen Firmware zurückgekehrt.', unknown: 'Updatezustand ungeklärt.', failed: 'Update fehlgeschlagen.'
    };
    $('otaState').textContent = t(names[u.state] || 'Update') + (u.state === 'upload' ? ' ' + u.progress + ' %' : '') + (u.version ? '\n' + u.version : '') + (OTA_CODE[u.code] ? '\n' + t(OTA_CODE[u.code][0]) + ' ' + t(OTA_CODE[u.code][1]) + codeText(u.code) : '') + (u.error ? '\n' + u.error : '');
    $('otaProgress').hidden = u.state !== 'upload';
    $('otaProgress').firstElementChild.style.width = (u.progress || 0) + '%';
    $('otaUpload').disabled = u.busy || !firmwareMeta;
}
async function runOta() {
    if (!firmwareFile || !firmwareMeta || S.ota?.busy)
        return;
    const file = firmwareFile;
    S.ota = {
        busy: true, state: 'upload', progress: 0
    };
    $('firmwareFile').disabled = true;
    paintOta();
    try {
        const before = await raw('status'), meta = decodeFirmware(await file.slice(0, 1024).arrayBuffer(), before, S.schema, S.schema.ota_max, file.size);
        await authenticatedUpload('ota', file, p => {
            S.ota.progress = p;
            paintOta();
        });
        S.ota.state = 'boot';
        paintOta();
        const deadline = Date.now() + 120000, start = Date.now();
        let reboot = false, trial = false, sawDisconnect = false;
        await sleep(1800);
        while (Date.now() < deadline) {
            try {
                const s = await raw('status');
                S.status = s;
                reboot = reboot || s.uptime_s < before.uptime_s || sawDisconnect;
                if (s.firmware_id === meta.id) {
                    if (s.trial) {
                        trial = true;
                        S.ota.state = 'trial';
                    }
                    else if (meta.id !== before.firmware_id || reboot || trial) {
                        S.ota.state = 'done';
                        S.ota.version = s.version;
                        break;
                    }
                    else if (Date.now() - start > 15000) {
                        S.ota.state = 'unknown';
                        S.ota.code = 'ota_no_restart';
                        break;
                    }
                }
                else if (s.firmware_id === before.firmware_id && reboot && !s.trial && Date.now() - start > 12000) {
                    S.ota.state = 'rollback';
                    S.ota.version = s.version;
                    break;
                }
                else if (reboot && !s.trial && s.firmware_id !== meta.id && s.firmware_id !== before.firmware_id) {
                    S.ota.state = 'unknown';
                    S.ota.code = 'ota_other_firmware';
                    break;
                }
                paintStatus();
            }
            catch {
                sawDisconnect = true;
            }
            await sleep(1800);
        }
        if (S.ota.state === 'boot' || S.ota.state === 'trial') {
            S.ota.code = S.ota.state === 'boot' ? 'ota_no_answer' : 'ota_trial_open';
            S.ota.state = 'unknown';
        }
    }
    catch (e) {
        S.ota.state = 'failed';
        S.ota.error = e.message;
        throw e;
    }
    finally {
        S.ota.busy = false;
        if ($('firmwareFile'))
            $('firmwareFile').disabled = false;
        paintOta();
    }
}
async function waitForDevice(before, reset = false) {
    const deadline = Date.now() + 90000;
    await sleep(2000);
    while (Date.now() < deadline) {
        try {
            const s = await raw('status');
            if (!before || s.uptime_s < before.uptime_s) {
                S.status = s;
                await authenticate();
                const next = await readModel();
                if (reset)
                    fresh(next);
                else
                    mergeBaseline(next);
                S.images = await raw('images');
                render();
                toast(t('Gerät ist wieder erreichbar.'));
                return;
            }
        }
        catch {
        }
        await sleep(1500);
    }
    throw Error(t('Gerät nicht erreichbar. Es wird erneut versucht …'));
}
async function reboot() {
    const before = await raw('status');
    await mutate('reboot', {});
    toast(t('Gerät startet neu, warte auf Antwort …'));
    await waitForDevice(before);
}
function factoryDialog(formatStorage = false) {
    const hostname = S.saved.hostname, ap = S.status?.ap_ip, d = dialog(t('Werkseinstellungen'), `<p>${ht('Löscht Einstellungen einschließlich WLAN und Zugang, Abrufzähler und gespeicherte Vereinsnamen. Danach startet der Einrichtungsmodus. Die unveränderliche Namensvorlage der Firmware bleibt verfügbar.')}</p><label class="switch"><input type="checkbox" id="resetImages"><span class="track"></span>${ht('Eigene Bilder ebenfalls löschen')}</label><label class="switch"><input type="checkbox" id="resetStorage" ${formatStorage ? 'checked' : ''}><span class="track"></span>${ht('Dateispeicher vollständig formatieren (auch Standardbilder)')}</label><p class="help">${ht('Standardbilder benötigen danach USB-uploadfs.')}</p><div class="field"><label for="resetName">${ht('Zum Bestätigen Gerätenamen eingeben')}: <b>${esc(hostname)}</b></label><div class="ctl"><input id="resetName" autocomplete="off" class="w-m"></div></div><p>${ht('Nach dem Reset mit dem Setup-WLAN verbinden und diese Adresse öffnen.')} ${ap && ap !== '0.0.0.0' ? `<a href="http://${esc(ap)}">http://${esc(ap)}</a>` : ''}</p><p class="dlg-err" id="resetError"></p>`, button('Abbrechen', 'close') + button('Zurücksetzen', 'resetConfirm', 'disabled', 'danger'));
    const btn = d.querySelector('[data-act=resetConfirm]');
    $('resetName').oninput = e => btn.disabled = e.target.value !== hostname;
    $('resetImages').disabled = formatStorage;
    $('resetStorage').onchange = e => {
        $('resetImages').disabled = e.target.checked;
    };
    d.querySelector('[data-act=close]').onclick = () => d.close();
    btn.onclick = async () => {
        btn.disabled = true;
        const before = S.status;
        try {
            await mutate('reset', {
                confirm: 'reset', images: $('resetImages').checked, format_storage: $('resetStorage').checked
            });
            S.token = '';
            S.session = null;
            closeDebug();
            d.close();
            toast(t('Nach dem Reset mit dem Setup-WLAN verbinden und diese Adresse öffnen.'));
            await waitForDevice(before, true);
        }
        catch (e) {
            if (d.open) {
                $('resetError').textContent = e.message;
                btn.disabled = $('resetName').value !== hostname;
            }
            else
                toast(e.message, true);
        }
    };
}
async function scanWifi() {
    if (S.scan === 'busy')
        return;
    S.scan = 'busy';
    render();
    try {
        try {
            await mutate('wifi/scan', {});
        }
        catch (e) {
            if (e.status !== 409 || e.code !== 'busy')
                throw e;
        }
        const deadline = Date.now() + 90000;
        for (;;) {
            const data = await raw('wifi');
            if (!data.busy) {
                if (data.error_code)
                    throw Error(t({
                        scan_start: 'Aktion momentan nicht möglich.', scan_result: 'Antwort des Geräts ist nicht lesbar.', net_lost: 'WLAN-Verbindung verloren.', timeout: 'Zeitüberschreitung. Erneut versuchen.'
                    }[data.error_code] || 'WLAN-Suche fehlgeschlagen. Erneut suchen; bleibt der Fehler, Gerät neu starten.') + codeText(data.error_code));
                S.scan = data.networks;
                break;
            }
            if (Date.now() > deadline)
                throw Error(t('Zeitüberschreitung. Erneut versuchen.'));
            await sleep(750);
        }
    }
    catch (e) {
        S.scan = null;
        throw e;
    }
    finally {
        if (S.page === 'settings')
            render();
    }
}
