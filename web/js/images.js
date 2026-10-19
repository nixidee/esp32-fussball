// Image library, role ownership, composition, editing and image actions.
function imageName(id) {
    return id ? tf('Bild %', id) : t('Wappen');
}
function imageUrl(id, stock = 'background') {
    const item = S.images?.items?.find(x => x.id === id);
    return '/api/v1/images/file?' + (id ? 'id=' + id + '&r=' + (item?.revision || 0) : 'stock=' + stock + '&r=' + imageRole(stock).revision);
}
function thumb(id, stock = 'background') {
    return `<img src="${esc(imageUrl(id, stock))}" alt="${esc(id ? imageName(id) : t('Standardbild'))}" loading="lazy" data-image>`;
}
function imageOptions(stock = 'Standard-Hintergrund', selected = 0) {
    const rows = (S.images?.items || []).map(i => [i.id, imageName(i.id)]);
    if (selected > 0 && !rows.some(([id]) => id === selected))
        rows.push([selected, imageName(selected) + ' · ' + t('Datei fehlt')]);
    return [[0, t(stock)], ...rows];
}
function imageIds(...models) {
    return [
        ...new Set(models.flatMap(m => m?.screen_policy ? [...m.screen_policy.slides, ...Object.values(m.screen_policy.appearance).map(s => s.image)] : []))
    ].filter(id => Number.isInteger(id) && id > 0 && id <= S.schema.limits.images);
}
function libraryEntries(items = S.images?.items || []) {
    return [
        ...items, ...imageIds(S.saved, S.draft).filter(id => !items.some(i => i.id === id)).sort((a, b) => a - b).map(id => ({
            id, missing: true
        }))
    ];
}
function screenUses(id, m = S.draft) {
    const result = [];
    for (const type of SP_SPORTS) {
        const s = m.screen_policy.appearance[type];
        if ((s.background || id > 0) && s.image === id)
            result.push(t('Hintergrundbild') + ' ' + t(SP_TYPE_LABEL[type]));
    }
    return result;
}
function slideUses(id, m = S.draft) {
    const result = [];
    m.screen_policy.slides.forEach((x, i) => {
        if (x === id)
            result.push(t('Bilderfolge') + ' ' + (i + 1));
    });
    return result;
}
function uses(id, m = S.draft) {
    return [...screenUses(id, m), ...slideUses(id, m)];
}
const IMAGE_ROLES = [['crest', 'Standard-Wappen'], ['background', 'Standard-Hintergrund']];
function imageRole(name) {
    return S.images?.roles?.find(r => r.name === name) || {
        name, replaced: false, bytes: 0, revision: 0
    };
}
function roleName(name) {
    return t(IMAGE_ROLES.find(r => r[0] === name)?.[1] || name);
}
function roleUses(name, m = S.draft) {
    if (name === 'background')
        return screenUses(0, m);
    return [t('Wappen'), ...(m.screen_policy.frame_crest ? [t('Bilderrahmen')] : [])];
}
function imagesPage() {
    if (S.editor)
        return editorPage();
    const inv = S.images || {
        items: [], used: 0, quota: S.schema.image_quota, max: S.schema.limits.images
    }, entries = libraryEntries(inv.items), free = Math.max(0, inv.max - entries.length), tiles = entries.map(im => {
        const u = [...new Set([...uses(im.id), ...uses(im.id, S.saved)])];
        return `<div class="card tile"><div class="th">${im.missing ? '' : thumb(im.id)}</div><b>${esc(imageName(im.id))}</b><p class="small muted num">${im.missing ? ht('Datei fehlt') : kb(im.bytes)} · #${im.id}</p><div class="uses">${u.length ? u.map(x => `<span class="tag acc">${esc(x)}</span>`).join('') : `<span class="tag">${ht('Nicht verwendet')}</span>`}</div><div class="row">${button('Ersetzen', 'imageEdit', `data-id="${im.id}"`)}${button('Löschen', 'imageDelete', `data-id="${im.id}"`, 'ghost danger')}</div></div>`;
    }).join('');
    return head('Bilder', 'Bilder liegen auf dem Gerät und können mehrfach verwendet werden. Verwendungen werden unter Anzeige gespeichert. Fehlende Dateien behalten ihren Bildplatz, solange sie ausgewählt sind.') + `<div id="imageWarning"></div><div class="card quota"><div class="section-head"><b class="num">${kb(inv.used)} ${ht('von')} ${kb(inv.quota)} ${ht('belegt')}</b><span class="small muted">${inv.items.length} / ${inv.max} ${ht('Dateien')} · ${esc(tf('Freie Bildplätze: %', free))} · ${esc(tf('Höchstens % je Bild', kb(S.schema.image_max)))}</span></div><div class="meter"><i style="width:${Math.min(100, inv.used / inv.quota * 100)}%"></i></div></div><div class="gallery" style="margin-top:16px">${tiles}<button class="card tile add" data-act="imageEdit" data-id="0" ${free === 0 ? 'disabled' : ''}><div><b style="font-size:1.6rem">+</b><br><b>${ht('Neues Bild')}</b><br><span class="small muted">${ht(free === 0 ? 'Keine freien Bildplätze. Bilder ersetzen oder löschen.' : 'Foto oder Grafik zuschneiden')}</span></div></button>${IMAGE_ROLES.map(([kind, name]) => {
        const r = imageRole(kind), u = roleUses(kind);
        return `<div class="card tile"><div class="th">${thumb(0, kind)}</div><b>${ht(name)}</b><p class="small muted num">${r.replaced ? esc(tf('Ersetzt · %', kb(r.bytes))) : ht('eingebaut')}</p><div class="uses">${u.length ? u.map(x => `<span class="tag acc">${esc(x)}</span>`).join('') : `<span class="tag">${ht('Nicht verwendet')}</span>`}</div><div class="row">${button('Ersetzen', 'roleEdit', `data-role="${kind}"`)}${button('Aus Bibliothek übernehmen', 'roleCopy', `data-role="${kind}" ${inv.items.length ? '' : 'disabled'}`)}${button('Werksbild wiederherstellen', 'roleReset', `data-role="${kind}" ${r.replaced ? '' : 'disabled'}`, 'ghost')}</div></div>`;
    }).join('')}</div>`;
}
function imageAllowance() {
    if (!S.images)
        return 0;
    const old = S.editor?.role ? imageRole(S.editor.role).bytes : S.images.items.find(i => i.id === S.editor?.id)?.bytes || 0;
    const library = Math.max(0, S.schema.image_quota - ((S.images.used || 0) - old));
    const free = S.status ? Math.max(0, S.status.fs_total - S.status.fs_used) : 0;
    return free >= S.schema.replacement_workspace ? Math.min(library, free, S.schema.image_max) : 0;
}
function jpegBaseline(bytes) {
    if (bytes[0] !== 255 || bytes[1] !== 216)
        return false;
    for (let i = 2; i + 3 < bytes.length;) {
        if (bytes[i] !== 255)
            return false;
        const marker = bytes[i + 1];
        if (marker === 192)
            return true;
        if (marker === 194)
            return false;
        if (marker === 218)
            return false;
        const length = bytes[i + 2] * 256 + bytes[i + 3];
        if (length < 2)
            return false;
        i += length + 2;
    }
    return false;
}
async function decodeImage(file) {
    if (typeof createImageBitmap === 'function')
        return createImageBitmap(file);
    return new Promise((resolve, reject) => {
        const url = URL.createObjectURL(file), im = new Image();
        im.onload = () => {
            URL.revokeObjectURL(url);
            resolve(im);
        };
        im.onerror = () => {
            URL.revokeObjectURL(url);
            reject(Error('decode'));
        };
        im.src = url;
    });
}
function createImageEditor(config, {
    t, tf, ht, esc, $, head, button, roleName, imageName, imageUrl,
    imageIds, imageAllowance, jpegBaseline, decodeImage, kb, raw, authenticatedUpload, load, render, toast, CLIENT
}) {
    const ED = {
        bmp: null, zoom: 100, x: 0, y: 0, opacity: 100, base: '#101820', sample: true, blob: null, quality: null, error: '', busy: false, serial: 0, decodeSerial: 0
    };
    function closeEditor() {
        if (ED.bmp?.close)
            ED.bmp.close();
        ED.bmp = null;
        ED.serial++;
        ED.blob = null;
        config.editor = null;
    }
    async function openEditor(id, role = null) {
        closeEditor();
        config.editor = {
            id, role
        };
        Object.assign(ED, {
            zoom: 100, x: 0, y: 0, opacity: 100, base: '#101820', sample: true, blob: null, quality: null, error: '', busy: false
        });
        const editor = config.editor, serial = ++ED.decodeSerial;
        render();
        if (!id && !role)
            return;
        try {
            const path = imageUrl(id, role || 'background').slice('/api/v1/'.length), bytes = await raw(path, undefined, 'GET', '', true), next = await decodeImage(new Blob([bytes], {
                type: 'image/jpeg'
            }));
            if (config.editor !== editor || serial !== ED.decodeSerial || ED.busy) {
                if (next.close)
                    next.close();
                return;
            }
            ED.bmp = next;
            render();
        }
        catch {
            if (config.editor !== editor || serial !== ED.decodeSerial)
                return;
            ED.error = t('Gespeichertes Bild konnte nicht geladen werden. Wähle eine lokale Bilddatei; das gespeicherte Bild bleibt erhalten.');
            if ($('editorError'))
                $('editorError').textContent = ED.error;
        }
    }
    function imageEditorMarkup(view) {
        return head('Bild bearbeiten', 'Zuschnitt in Geräteauflösung. Ziehen verschiebt, Mausrad oder zwei Finger zoomen.', button('Zurück zur Übersicht', 'editorClose', view.busy ? 'disabled' : '')) + `<p class="small muted" style="margin-bottom:16px">${view.width} × ${view.height} · ${esc(view.label)}</p><div class="editor"><div><div class="canvas-wrap ${view.round ? '' : 'rect'}" id="canvasWrap" style="aspect-ratio:${view.width}/${view.height}"><canvas id="editorCanvas"></canvas></div></div><div style="display:grid;gap:14px;align-content:start"><div class="field"><label for="editorFile">${ht('Datei')}</label><input id="editorFile" type="file" accept="image/*" ${view.busy ? 'disabled' : ''}><div class="field-error on" id="editorError">${esc(view.error)}</div></div><div class="field"><label for="zoomNumber">${ht('Zoom')}</label><div class="ctl"><input id="zoomRange" type="range" min="10" max="400" value="${view.zoom}"><input id="zoomNumber" type="number" class="w-s" min="10" max="400" value="${view.zoom}"><span class="unit">%</span>${button('Einpassen', 'imageFit')}${button('Füllen', 'imageFill')}</div></div><div class="field"><span class="lbl">${ht('Bildposition')}</span><div class="cols"><div><label for="imageX">${ht('Horizontal (px)')}</label><input id="imageX" type="number" step="any" class="w-m" value="${view.x}" ${view.busy ? 'disabled' : ''}></div><div><label for="imageY">${ht('Vertikal (px)')}</label><input id="imageY" type="number" step="any" class="w-m" value="${view.y}" ${view.busy ? 'disabled' : ''}></div></div><p class="help">${ht('0 zentriert das Bild. Positive Werte verschieben nach rechts oder unten, negative nach links oder oben; Pixel beziehen sich auf die Geräteauflösung.')}</p></div><div class="field"><label for="editorOpacity">${ht('Deckkraft')}</label><div class="ctl"><input id="editorOpacity" type="range" min="0" max="100" value="${view.opacity}"><span class="num" id="opacityLabel">${view.opacity} %</span></div></div><div class="field"><label for="editorBase">${ht('Grundfarbe')}</label><div class="colors"><input id="editorBase" type="color" value="${view.base}"></div></div><label class="switch"><input id="editorSample" type="checkbox" ${view.sample ? 'checked' : ''}><span class="track"></span>${ht('Beispieltext einblenden (wird nicht gespeichert)')}</label><div class="notice info" id="editorSize">${ht(view.hasImage ? 'Größe wird berechnet …' : 'Wähle eine Bilddatei.')}</div><div class="progress" id="imageProgress" hidden><i></i></div><p id="imageProgressText" class="small num"></p><div class="row">${button(view.replacing ? 'Ersetzen' : 'Hochladen', 'imageUpload', 'id="imageUpload" disabled', 'primary')}${button('Abbrechen', 'editorClose', view.busy ? 'disabled' : '')}</div></div></div>`;
    }
    function editorPage() {
        return imageEditorMarkup({width: config.schema.width, height: config.schema.height,
            round: config.schema.round, label: config.editor.role ? roleName(config.editor.role) : config.editor.id ? imageName(config.editor.id) : t('Neues Bild'),
            replacing: !!(config.editor.id || config.editor.role), hasImage: !!ED.bmp,
            busy: ED.busy, zoom: ED.zoom, x: ED.x, y: ED.y, opacity: ED.opacity,
            base: ED.base, sample: ED.sample});
    }
    function composition(sample = false) {
        const cv = document.createElement('canvas');
        cv.width = config.schema.width;
        cv.height = config.schema.height;
        const g = cv.getContext('2d');
        g.fillStyle = ED.base;
        g.fillRect(0, 0, cv.width, cv.height);
        if (ED.bmp) {
            const scale = Math.max(cv.width / ED.bmp.width, cv.height / ED.bmp.height) * ED.zoom / 100;
            g.globalAlpha = ED.opacity / 100;
            g.drawImage(ED.bmp, (cv.width - ED.bmp.width * scale) / 2 + ED.x, (cv.height - ED.bmp.height * scale) / 2 + ED.y, ED.bmp.width * scale, ED.bmp.height * scale);
            g.globalAlpha = 1;
        }
        if (sample) {
            g.textAlign = 'center';
            g.fillStyle = '#ffffff';
            g.font = '700 ' + Math.round(cv.width * .19) + 'px system-ui';
            g.fillText('2 : 1', cv.width / 2, cv.height * .55);
            g.fillStyle = '#49cba0';
            g.font = '600 ' + Math.round(cv.width * .075) + 'px system-ui';
            g.fillText('67′', cv.width / 2, cv.height * .7);
        }
        return cv;
    }
    function drawEditor() {
        for (const [id, value] of [['imageX', ED.x], ['imageY', ED.y]])
            if ($(id))
                $(id).value = value;
        const cv = $('editorCanvas');
        if (!cv)
            return;
        cv.width = config.schema.width;
        cv.height = config.schema.height;
        const g = cv.getContext('2d');
        g.drawImage(composition(ED.sample), 0, 0);
    }
    let imageSizeTimer;
    async function prepareImage() {
        const serial = ++ED.serial;
        ED.blob = null;
        ED.quality = null;
        if ($('imageUpload'))
            $('imageUpload').disabled = true;
        if (!ED.bmp || !config.editor)
            return;
        const cv = composition(false);
        let blob = null, quality = null;
        for (let q = 90; q >= 30; q -= 5) {
            blob = await new Promise(resolve => cv.toBlob(resolve, 'image/jpeg', q / 100));
            if (serial !== ED.serial || !config.editor)
                return;
            if (blob && blob.size <= config.schema.image_max) {
                quality = q;
                break;
            }
        }
        if (!blob)
            throw Error(t('Datei kann nicht gelesen werden. Das bisherige Bild bleibt erhalten.'));
        let error = '';
        if (quality === null)
            error = t('Bild auch bei Qualität 30 % zu groß. Weniger Details, stärker zoomen oder Deckkraft senken.');
        else if (blob.size > imageAllowance())
            error = t('Bild überschreitet die Speicherquote einschließlich Reserve.');
        else if (!jpegBaseline(new Uint8Array(await blob.arrayBuffer())))
            error = t('Datei passt nicht zum Gerät oder ist beschädigt.');
        if (serial !== ED.serial || !config.editor)
            return;
        ED.blob = error ? null : blob;
        ED.quality = quality;
        const el = $('editorSize');
        if (el) {
            el.className = 'notice ' + (error ? 'err' : 'info');
            el.innerHTML = `<b class="num">${kb(blob.size)} · ${ht('JPEG-Qualität')}: ${quality === null ? '—' : quality + ' %'}</b><span>${ht('Frei für dieses Bild')}: ${kb(imageAllowance())}${error ? '<br>' + esc(error) : ''}</span>`;
        }
        if ($('imageUpload'))
            $('imageUpload').disabled = !ED.blob || ED.busy;
    }
    function scheduleImage() {
        ED.blob = null;
        ED.serial++;
        clearTimeout(imageSizeTimer);
        if ($('imageUpload'))
            $('imageUpload').disabled = true;
        imageSizeTimer = setTimeout(() => prepareImage().catch(e => {
            if ($('editorError'))
                $('editorError').textContent = e.message;
        }), CLIENT.image_input_debounce_ms);
    }
    function fitImage(fill) {
        if (!ED.bmp)
            return;
        const max = Math.max(config.schema.width / ED.bmp.width, config.schema.height / ED.bmp.height), min = Math.min(config.schema.width / ED.bmp.width, config.schema.height / ED.bmp.height);
        ED.zoom = fill ? 100 : Math.max(10, Math.min(400, Math.round(min / max * 100)));
        ED.x = ED.y = 0;
        render();
    }
    function bindEditor() {
        drawEditor();
        scheduleImage();
        for (const [id, key] of [['imageX', 'x'], ['imageY', 'y']])
            $(id).onchange = e => {
                if (ED.busy)
                    return;
                const value = Number(e.target.value);
                if (e.target.value === '' || !Number.isFinite(value)) {
                    e.target.value = ED[key];
                    toast(t('Eine gültige Pixelzahl eingeben. Die bisherige Position bleibt erhalten.'), 'warning');
                    return;
                }
                ED[key] = value;
                drawEditor();
                scheduleImage();
            };
        const wrap = $('canvasWrap'), pts = new Map();
        let pinch = null;
        const zoom = z => {
            if (ED.busy || !Number.isFinite(z))
                return;
            ED.zoom = Math.max(10, Math.min(400, Math.round(z)));
            $('zoomRange').value = ED.zoom;
            $('zoomNumber').value = ED.zoom;
            drawEditor();
            scheduleImage();
        };
        $('zoomRange').oninput = e => zoom(Number(e.target.value));
        $('zoomNumber').onchange = e => {
            if (e.target.value === '') {
                e.target.value = ED.zoom;
                return;
            }
            zoom(Number(e.target.value));
        };
        $('editorOpacity').oninput = e => {
            if (ED.busy)
                return;
            ED.opacity = Number(e.target.value);
            $('opacityLabel').textContent = ED.opacity + ' %';
            drawEditor();
            scheduleImage();
        };
        $('editorBase').oninput = e => {
            if (ED.busy)
                return;
            ED.base = e.target.value;
            drawEditor();
            scheduleImage();
        };
        $('editorSample').onchange = e => {
            if (ED.busy)
                return;
            ED.sample = e.target.checked;
            drawEditor();
        };
        $('editorFile').onchange = async (e) => {
            const file = e.target.files[0];
            if (!file || ED.busy)
                return;
            const previous = ED.bmp, editor = config.editor, serial = ++ED.decodeSerial;
            try {
                const next = await decodeImage(file);
                if (config.editor !== editor || serial !== ED.decodeSerial || ED.busy) {
                    if (next.close)
                        next.close();
                    return;
                }
                ED.bmp = next;
                if (previous?.close)
                    previous.close();
                ED.x = ED.y = 0;
                ED.zoom = 100;
                ED.error = '';
                render();
            }
            catch {
                if (config.editor !== editor || serial !== ED.decodeSerial)
                    return;
                ED.error = t('Datei kann nicht gelesen werden. Das bisherige Bild bleibt erhalten.');
                if ($('editorError'))
                    $('editorError').textContent = ED.error;
            }
        };
        wrap.addEventListener('wheel', e => {
            e.preventDefault();
            zoom(ED.zoom * (e.deltaY < 0 ? 1.06 : 1 / 1.06));
        }, {
            passive: false
        });
        wrap.onpointerdown = e => {
            if (ED.busy)
                return;
            wrap.setPointerCapture(e.pointerId);
            pts.set(e.pointerId, [e.clientX, e.clientY]);
            if (pts.size === 2) {
                const [a, b] = [...pts.values()];
                pinch = Math.hypot(a[0] - b[0], a[1] - b[1]);
            }
        };
        wrap.onpointermove = e => {
            if (ED.busy || !pts.has(e.pointerId))
                return;
            const prev = pts.get(e.pointerId);
            pts.set(e.pointerId, [e.clientX, e.clientY]);
            if (pts.size === 2) {
                const [a, b] = [...pts.values()], distance = Math.hypot(a[0] - b[0], a[1] - b[1]);
                if (pinch > 0)
                    zoom(ED.zoom * distance / pinch);
                pinch = distance;
            }
            else {
                const rect = wrap.getBoundingClientRect();
                ED.x += (e.clientX - prev[0]) * config.schema.width / rect.width;
                ED.y += (e.clientY - prev[1]) * config.schema.height / rect.height;
                drawEditor();
                scheduleImage();
            }
        };
        wrap.onpointerup = wrap.onpointercancel = e => {
            pts.delete(e.pointerId);
            pinch = null;
        };
    }
    async function uploadImage() {
        if (ED.busy || !config.editor)
            return;
        if (!ED.blob)
            throw Error(t('Wähle eine Bilddatei.'));
        const editor = config.editor, blob = ED.blob;
        ED.busy = true;
        document.querySelectorAll('#main input,#main button').forEach(el => el.disabled = true);
        $('imageProgress').hidden = false;
        try {
            await load('status', false);
            if (blob.size > imageAllowance())
                throw Error(t('Bild überschreitet die Speicherquote einschließlich Reserve.'));
            await authenticatedUpload('images/upload', blob, p => {
                if ($('imageProgress'))
                    $('imageProgress').firstElementChild.style.width = p + '%';
                if ($('imageProgressText'))
                    $('imageProgressText').textContent = t('Übertragen') + ' ' + p + ' %';
            }, editor.role ? {
                'X-Image-Role': editor.role
            } : editor.id ? {
                'X-Image-Id': editor.id
            } : {
                'X-Image-Reserved': String(imageIds(config.draft).reduce((mask, id) => mask | (1 << id), 0))
            });
            config.images = await raw('images');
            await load('status', false);
            closeEditor();
            render();
            toast(t(editor.role ? 'Standardbild ersetzt.' : 'Bild gespeichert. Verwendung unter Anzeige festlegen.'));
        }
        catch (e) {
            ED.error = e.message;
            if ($('editorError'))
                $('editorError').textContent = e.message;
            throw e;
        }
        finally {
            ED.busy = false;
            if (config.editor)
                render();
        }
    }

    function editorBusy() { return ED.busy; }

    return Object.freeze({openEditor, closeEditor, editorPage, bindEditor, fitImage, uploadImage, editorBusy});
} // End private ImageEditor scope.
const {openEditor, closeEditor, editorPage, bindEditor, fitImage, uploadImage, editorBusy} = createImageEditor({
    get schema() { return S.schema; },
    get images() { return S.images; },
    set images(value) { S.images = value; },
    get editor() { return S.editor; },
    set editor(value) { S.editor = value; },
    get status() { return S.status; },
    get draft() { return S.draft; }
}, {
    CLIENT,
    t,
    tf,
    ht,
    esc,
    $,
    head,
    button,
    roleName,
    imageName,
    imageUrl,
    imageIds,
    imageAllowance,
    jpegBaseline,
    decodeImage,
    kb,
    raw,
    authenticatedUpload,
    load,
    render,
    toast
});
async function reconcileImages(removed = [], allowed = removed) {
    const [next, inventory] = await Promise.all([readModel(), raw('images')]);
    mergeBaseline(next, allowed);
    S.images = inventory;
    const keep = new Set(Array.from({
        length: S.schema.limits.images
    }, (_, i) => i + 1));
    for (const id of removed)
        keep.delete(id);
    pruneMissing(S.draft, keep);
    await load('status', false);
    render();
}
function imageDeleteFailure(error) {
    const result = error.data || {};
    const message = result.outcome_unknown
        ? 'Ob die Bildverweise gespeichert wurden, ist unklar. Gerät neu starten und Zustand prüfen; nicht erneut löschen.'
        : result.references_cleared && result.settings_saved && !result.file_deleted
            ? 'Bildverweise wurden gespeichert. Die Datei wurde nicht gelöscht; das Löschen kann erneut versucht werden.'
            : '';
    if (message)
        error.message = t(message) + codeText([error.code, error.status ? 'HTTP ' + error.status : ''].filter(Boolean).join(' · '));
    return error;
}
async function deleteImage(id) {
    let failure = null;
    try {
        await mutate('images/delete', {
            id
        });
    }
    catch (e) {
        failure = imageDeleteFailure(e);
    }
    try {
        await reconcileImages(!failure || failure.data?.file_deleted || failure.data?.already_missing ? [id] : [], [id]);
    }
    catch (e) {
        if (failure)
            failure.message += ' ' + e.message;
        else {
            toast(t('Bild gelöscht.') + ' ' + e.message, true);
            return;
        }
    }
    if (failure)
        throw failure;
    toast(t('Bild gelöscht.'));
}
async function changeRole(role, action, id) {
    let failure = null;
    try {
        await mutate('images/role', action === 'copy' ? {
            role, action, id
        } : {
            role, action
        });
    }
    catch (e) {
        failure = e;
    }
    try {
        S.images = await raw('images');
        await load('status', false);
        render();
    }
    catch (e) {
        if (failure)
            failure.message += ' ' + e.message;
        else
            throw e;
    }
    if (failure)
        throw failure;
    toast(t(action === 'copy' ? 'Standardbild ersetzt.' : 'Werksbild wiederhergestellt.'));
}
async function deleteAllImages() {
    const ids = libraryEntries((await raw('images')).items).map(i => i.id), result = [], removed = [], attempted = [];
    let outcomeUnknown = false;
    for (const id of ids) {
        attempted.push(id);
        try {
            await mutate('images/delete', {
                id
            });
            removed.push(id);
            result.push(imageName(id) + ': ' + t('Bild gelöscht.'));
        }
        catch (e) {
            const failure = imageDeleteFailure(e);
            if (failure.data?.file_deleted || failure.data?.already_missing)
                removed.push(id);
            result.push(imageName(id) + ': ' + failure.message);
            if (failure.data?.outcome_unknown) {
                outcomeUnknown = true;
                break;
            }
        }
    }
    for (const [kind, name] of outcomeUnknown ? [] : IMAGE_ROLES) {
        try {
            await mutate('images/role', {
                role: kind, action: 'reset'
            });
            result.push(t(name) + ': ' + t('Werksbild wiederhergestellt.'));
        }
        catch (e) {
            result.push(t(name) + ': ' + e.message);
        }
    }
    try {
        await reconcileImages(removed, attempted);
    }
    catch (e) {
        result.push(e.message);
    }
    if ($('deleteResults'))
        $('deleteResults').textContent = result.join('\n');
    else
        toast(result.join('\n'));
}
