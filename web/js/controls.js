// Shared fields, dialogs, notifications and page layout primitives.
function feedback(message, severity) {
    const [icon, label] = severity === 'error' ? ['✕', 'Fehler'] : severity === 'warning' ? ['⚠', 'Warnung'] : ['✓', 'Bestätigung'];
    return `<span aria-hidden="true">${icon}</span> <span><b>${ht(label)}:</b> ${esc(message)}</span>`;
}
function toast(message, severity = false) {
    severity = severity === true ? 'error' : severity || 'info';
    const el = $('toast'), top = !!el.showPopover, hide = () => {
        if (!top)
            el.hidden = true;
        else if (el.matches(':popover-open'))
            el.hidePopover();
    };
    el.setAttribute('role', severity === 'error' ? 'alert' : 'status');
    el.innerHTML = feedback(message, severity);
    el.className = 'toast' + (severity === 'info' ? '' : ' ' + severity);
    hide();
    el.hidden = false;
    if (top)
        el.showPopover();
    clearTimeout(toast.timer);
    toast.timer = setTimeout(hide, severity === 'info' ? 6500 : 14000);
}
function dialog(title, body, actions = '', wide = false) {
    const d = $('dialog');
    if (d.open)
        d.close();
    d.oncancel = null;
    d.className = wide ? 'wide' : '';
    d.innerHTML = `<div class="dh"><h2>${esc(title)}</h2><button class="btn ghost" data-close aria-label="${ht('Schließen')}">×</button></div><div class="db">${body}</div><div class="df">${actions}</div>`;
    d.querySelector('[data-close]').onclick = () => d.close();
    d.showModal();
    return d;
}
function button(text, act, extra = '', cls = '', why = '') {
    const off = extra.includes('disabled');
    if (S.saving && !off)
        extra += ' disabled';
    const b = `<button class="btn ${cls}" data-act="${act}" ${extra}${off && why ? ` title="${ht(why)}"` : ''}>${ht(text)}</button>`;
    return off && why ? `<span class="button-reason">${b}<span class="why">${ht(why)}</span></span>` : b;
}
function confirmAction(title, message, run, word = 'Übernehmen', extra = '') {
    const d = dialog(t(title), `<p>${esc(t(message))}</p>${extra}`, button('Abbrechen', 'close') + button(word, 'confirm', '', 'danger'));
    d.querySelector('[data-act=close]').onclick = () => d.close();
    d.querySelector('[data-act=confirm]').onclick = async (e) => {
        e.target.disabled = true;
        try {
            await run();
            d.close();
        }
        catch (err) {
            e.target.disabled = false;
            if (!err.saveFeedback)
                toast(err.message, err.severity || true);
        }
    };
}
function opts(rows, value) {
    return rows.map(([v, n]) => `<option value="${esc(v)}" ${eq(v, value) ? 'selected' : ''}>${esc(n)}</option>`).join('');
}
function helpButton(text, path = '') {
    return text ? `<button type="button" class="btn ghost policy-help" data-act="policyHelp" data-path="${esc(path)}" data-help="${ht(text)}" aria-label="${ht('Hilfe')}">?</button>` : '';
}

function F(path, type = 'text', help = '', options = null, disabled = false, label = null) {
    disabled = disabled || S.saving;
    const value = get(S.draft, path), ssid = path === 'wifi_ssid' ? ssidEditor(value) : null, id = 'f-' + path.replace(/[^\w-]/g, '-'), name = t(label || LABEL[parts(path).at(-1)] || path) + (ssid?.base64 ? ' (Base64)' : ''), bound = type === 'number' ? /^(screen_policy\.|competitions\[\d+\]\.display\.)/.test(path) ? screenBound(path) : S.schema.bounds[path] || S.schema.bounds[path.replace(/\[\d+\]$/, '')] || S.schema.bounds[parts(path).at(-1)] || (/text_scale$/.test(path) ? S.schema.text_scale : null) : null;
    let control;
    if (type === 'switch')
        control = `<label class="switch"><input id="${id}" type="checkbox" data-path="${path}" data-refresh ${value ? 'checked' : ''} ${disabled ? 'disabled' : ''}><span class="track"></span><span>${esc(name)}</span></label>`;
    else if (type === 'select')
        control = `<select id="${id}" data-path="${path}" data-kind="${typeof value === 'number' ? 'num' : 'str'}" data-refresh class="w-m" ${disabled ? 'disabled' : ''}>${opts(options || [], value)}</select>`;
    else if (type === 'colour')
        control = `<input id="${id}" type="color" data-path="${path}" data-kind="colour" value="${colour(value)}" ${disabled ? 'disabled' : ''}>`;
    else {
        const max = ssid?.base64 ? 44 : S.schema.text[path], v = ssid?.base64 ? ssid.value : type === 'time' && Number.isInteger(value) ? String(Math.floor(value / 60)).padStart(2, '0') + ':' + String(value % 60).padStart(2, '0') : value;
        control = `<input id="${id}" type="${type === 'number' ? 'number' : type === 'time' ? 'time' : 'text'}" class="${type === 'number' || type === 'time' ? 'w-s' : 'w-l'}" data-path="${path}" data-kind="${ssid?.base64 ? 'ssid-base64' : type === 'number' ? 'num' : type === 'time' ? 'time' : 'str'}" value="${esc(v)}" ${bound ? `min="${bound[0]}" max="${bound[1]}" step="1"` : ''} ${max ? `maxlength="${max}"` : ''} ${disabled ? 'disabled' : ''}>`;
    }
    const hint = [
        bound ? t('Erlaubter Bereich') + ': ' + bound.join('–') : '', ssid?.base64 ? t('WLAN-Name als Base64: höchstens 32 Originalbytes. Base64 direkt bearbeiten oder ein gefundenes Netzwerk auswählen.') : S.schema.text[path] ? tf('Höchstens % Byte UTF-8', S.schema.text[path]) : ''
    ].filter(Boolean).join(' ');
    return `<div class="field ${disabled ? 'off' : ''}" data-field="${path}">${type === 'switch' ? '' : `<div class="field-label"><label for="${id}">${esc(name)}</label>${helpButton(help, path)}</div>`}<div class="ctl">${control}${type === 'switch' ? helpButton(help, path) : ''}${type === 'number' ? `<span class="unit">${/brightness|text_scale/.test(path) ? '%' : /window_before|window_after|_min$/.test(path) ? 'min' : /scroll_repeat_ms/.test(path) ? 'ms' : (/_s(\[\d+\])?$/.test(path) || /^screen_policy\.(phases|competitions).*\[2\]$/.test(path)) ? 's' : ''}</span>` : ''}</div>${hint ? `<div class="help">${esc(hint)}</div>` : ''}<div class="field-error" data-error="${path}"></div></div>`;
}
function SEC(key, help = '') {
    const state = secretMode(key), field = state.mode === 'change' ? `<div class="ctl"><input type="password" id="f-${key}" class="w-l" data-secret="${key}" value="${esc(state.value)}" autocomplete="new-password"></div><p class="help">${ht('Leeres Feld behält den gespeicherten Wert.')}</p>` : '';
    return `<div class="field" data-field="${key}"><div class="field-label"><span class="lbl">${ht(LABEL[key])}</span>${helpButton(help, key)}</div><span class="help">${ht(state.mode === 'clear' ? 'Wird beim Speichern entfernt' : S.saved[key + '_set'] ? 'Gespeichert' : 'Nicht gesetzt')}</span><div class="row">${button('Ändern', 'secret', `data-key="${key}" data-mode="change"`)}${button(state.mode === 'keep' ? 'Entfernen' : 'Behalten', 'secret', `data-key="${key}" data-mode="${state.mode === 'keep' ? 'clear' : 'keep'}"`)}</div>${field}<div class="field-error" data-error="${key}"></div></div>`;
}
function section(name, help, body, action = '') {
    return `<section class="section"><div class="section-head"><div><h2>${ht(name)}</h2>${help ? `<p class="help">${ht(help)}</p>` : ''}</div>${action}</div>${body}</section>`;
}
function expert(name, body) {
    return `<details class="expert" data-expert-key="${esc(name)}" ${S.expertOpen?.has(name) ? 'open' : ''}><summary>${ht('Experte')}<span>${ht(name)}</span></summary><div class="cols">${body}</div></details>`;
}
function head(name, help = '', action = '') {
    return `<div class="page-head"><div><h1>${ht(name)}</h1>${help ? `<p>${ht(help)}</p>` : ''}</div>${action}</div>`;
}
