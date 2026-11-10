/* Hydra website editor. Loaded only with ?edit (see the loader at the end of index.html).
 *
 * Lets the user change texts, reorder/hide elements and leave comments (with images or sketches)
 * directly on the page. Nothing is written back to index.html: "Export" downloads one JSON file
 * that Claude applies to the source by hand (the page is one file with two languages and generated
 * graphics, a browser-side HTML dump would make a mess of it).
 *
 * Elements are addressed by an anchor (nearest ancestor with id / data-f / data-g) plus the ORIGINAL
 * child indices below it (stamped at load), so addresses survive reordering and reloads.
 * State lives in IndexedDB (images are too big for localStorage) and is reapplied on every load
 * and every wiki re-render (language switch).
 */
(function () {
  'use strict';
  if (window.__hydraEdit) return;
  window.__hydraEdit = true;

  /* ================= storage ================= */
  const DB = 'hydra-edit', STORE = 'kv', KEY = 'state';
  function idb() {
    return new Promise((res, rej) => {
      const r = indexedDB.open(DB, 1);
      r.onupgradeneeded = () => r.result.createObjectStore(STORE);
      r.onsuccess = () => res(r.result);
      r.onerror = () => rej(r.error);
    });
  }
  async function load() {
    try {
      const db = await idb();
      return await new Promise(res => {
        const q = db.transaction(STORE).objectStore(STORE).get(KEY);
        q.onsuccess = () => res(q.result || null);
        q.onerror = () => res(null);
      });
    } catch (e) {
      try { return JSON.parse(localStorage.getItem('hydra-edit-state') || 'null'); } catch (e2) { return null; }
    }
  }
  let saveTimer = 0;
  function save() {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(async () => {
      try {
        const db = await idb();
        db.transaction(STORE, 'readwrite').objectStore(STORE).put(state, KEY);
      } catch (e) {
        try { localStorage.setItem('hydra-edit-state', JSON.stringify(state)); } catch (e2) { toast('Could not save (storage full?)'); }
      }
    }, 150);
  }

  let state = { v: 1, seq: 0, text: [], moves: [], hidden: [], comments: [] };

  /* ================= addressing ================= */
  const orig = new WeakMap();          // element -> original index among its parent's children
  const UI_ATTR = 'data-hx-ui';
  // Language and theme switches always work, also in the edit modes
  const isUi = el => !!(el && el.closest && el.closest('[' + UI_ATTR + '], header.top .lang, #themeBtn'));
  const wikiEl = () => document.getElementById('wiki');
  const navEl = () => document.getElementById('navLinks');
  const lang = () => document.documentElement.lang;

  function stamp(root) {
    const walk = el => {
      [...el.children].forEach((c, i) => {
        if (c.hasAttribute(UI_ATTR)) return;
        if (!orig.has(c)) orig.set(c, i);
        if (!c.hasAttribute('data-g') && c.tagName !== 'SVG' && c.tagName !== 'svg' && c.tagName !== 'TEMPLATE') walk(c);
      });
    };
    walk(root);
  }
  function scopeOf(el) {
    const w = wikiEl(), n = navEl();
    if (w && w.contains(el) && el !== w) return 'wiki-' + (w.dataset.hxLang || lang());
    if (n && n.contains(el) && el !== n) return 'nav-' + (n.dataset.hxLang || lang());
    return 'page';
  }
  function scopeRoot(scope) {
    if (scope === 'page') return document.body;
    const [kind, l] = scope.split('-');
    const r = kind === 'wiki' ? wikiEl() : navEl();
    return r && (r.dataset.hxLang || lang()) === l ? r : null;
  }
  function anchorSel(el) {
    if (el.id && el.id !== 'wiki' && el.id !== 'navLinks') return '#' + el.id;
    if (el.hasAttribute('data-f')) return el.tagName.toLowerCase() + '[data-f="' + el.getAttribute('data-f') + '"]';
    return null;
  }
  function pathOf(el) {
    const scope = scopeOf(el), root = scopeRoot(scope);
    const idx = [];
    let cur = el, anchor = null;
    while (cur && cur !== root && cur !== document.body) {
      const a = anchorSel(cur);
      if (a) { anchor = a; break; }
      if (!orig.has(cur)) return null;
      idx.unshift(orig.get(cur));
      cur = cur.parentElement;
    }
    return { scope, anchor: anchor || '', idx };
  }
  const pkey = p => p.scope + '|' + p.anchor + '|' + p.idx.join('.');
  function resolve(p) {
    const root = scopeRoot(p.scope);
    if (!root) return null;
    let cur = p.anchor ? root.querySelector(p.anchor) : root;
    if (!cur) return null;
    for (const i of p.idx) {
      cur = [...cur.children].find(c => orig.get(c) === i);
      if (!cur) return null;
    }
    return cur;
  }
  function describe(el) {
    if (!el) return '?';
    let s = el.tagName.toLowerCase();
    if (el.id) s += '#' + el.id;
    const cls = [...el.classList].filter(c => !c.startsWith('hx-')).slice(0, 2);
    if (cls.length) s += '.' + cls.join('.');
    if (el.dataset.f) s += '[' + el.dataset.f + ']';
    if (el.dataset.g) s += '{' + el.dataset.g + '}';
    return s;
  }
  function snippet(el, n = 70) {
    if (!el) return '';
    if (el.dataset && el.dataset.g) return '[graphic: ' + el.dataset.g + ']';
    // visible language only
    const c = el.cloneNode(true);
    c.querySelectorAll(lang() === 'de' ? '.en' : '.de').forEach(x => x.remove());
    c.querySelectorAll('svg, [data-g]').forEach(x => x.remove());
    const t = c.textContent.replace(/\s+/g, ' ').trim();
    return t.length > n ? t.slice(0, n - 1) + '…' : t;
  }
  function where(el) {
    const parts = [];
    let cur = el;
    while (cur && cur !== document.body && parts.length < 4) {
      parts.unshift(describe(cur));
      if (cur.id) break;
      cur = cur.parentElement;
    }
    return parts.join(' > ');
  }
  const isVisible = el => !!el && el.getClientRects().length > 0 && getComputedStyle(el).visibility !== 'hidden';

  /* ================= applying state ================= */
  function applyScope(scope) {
    state.text.filter(t => t.p.scope === scope).forEach(t => { const el = resolve(t.p); if (el) { el.innerHTML = t.after; stamp(el); } });
    // moves: last order per parent wins
    const last = new Map();
    state.moves.filter(m => m.p.scope === scope).forEach(m => last.set(pkey(m.p), m));
    last.forEach(m => { const el = resolve(m.p); if (el) reorder(el, m.order); });
    document.querySelectorAll('.hx-removed').forEach(el => { if (scopeOf(el) === scope) el.classList.remove('hx-removed'); });
    state.hidden.filter(h => h.p.scope === scope).forEach(h => { const el = resolve(h.p); if (el) el.classList.add('hx-removed'); });
  }
  function applyAll() {
    applyScope('page');
    ['wiki', 'nav'].forEach(k => applyScope(k + '-' + lang()));
    pins();
  }
  function reorder(parent, order) {
    const kids = [...parent.children].filter(c => orig.has(c));
    const byIdx = new Map(kids.map(c => [orig.get(c), c]));
    const seq = order.map(i => byIdx.get(i)).filter(Boolean);
    kids.forEach(c => { if (!seq.includes(c)) seq.push(c); });
    // keep non-stamped nodes (our UI) where they are: append in order
    seq.forEach(c => parent.appendChild(c));
  }
  const orderOf = parent => [...parent.children].filter(c => orig.has(c)).map(c => orig.get(c));

  /* ================= styles ================= */
  const css = document.createElement('style');
  css.setAttribute(UI_ATTR, '');
  css.textContent = `
  [${UI_ATTR}] { font-family: "Segoe UI Variable", "Segoe UI", system-ui, sans-serif; font-size: 13px; line-height: 1.35; color: #e3e8ee; letter-spacing: 0; text-transform: none; }
  [${UI_ATTR}] * { box-sizing: border-box; }
  .hx-bar { position: fixed; left: 50%; bottom: 16px; transform: translateX(-50%); z-index: 2147483000; display: flex; gap: 4px; align-items: center;
    background: rgba(16,20,26,.96); border: 1px solid #2c3642; border-radius: 14px; padding: 6px; box-shadow: 0 12px 40px rgba(0,0,0,.5); backdrop-filter: blur(8px); max-width: calc(100vw - 16px); flex-wrap: wrap; justify-content: center; }
  .hx-bar button, .hx-pop button, .hx-drawer button, .hx-sel button { font: inherit; color: inherit; background: #1d242d; border: 1px solid #2c3642; border-radius: 9px; padding: 6px 10px; cursor: pointer; white-space: nowrap; }
  .hx-bar button:hover, .hx-pop button:hover, .hx-drawer button:hover, .hx-sel button:hover { border-color: #35c7d9; }
  .hx-bar button.on { background: #35c7d9; color: #061014; border-color: #35c7d9; font-weight: 600; }
  .hx-bar .sep { width: 1px; align-self: stretch; background: #2c3642; margin: 2px 4px; }
  .hx-bar .logo { font-weight: 700; padding: 0 8px 0 6px; color: #35c7d9; }
  .hx-bar kbd { font: 11px Consolas, monospace; opacity: .6; margin-left: 4px; }
  .hx-count { background: #ff6aa6; color: #fff; border-radius: 8px; padding: 0 6px; font-size: 11px; margin-left: 4px; }
  .hx-hover { position: fixed; z-index: 2147482990; pointer-events: none; border: 2px solid #35c7d9; border-radius: 6px; background: rgba(53,199,217,.07); transition: all .06s; }
  .hx-hover.move { border-color: #a288ff; background: rgba(162,136,255,.08); }
  .hx-hover.comment { border-color: #ff6aa6; background: rgba(255,106,166,.07); }
  .hx-hover span { position: absolute; left: -2px; top: -20px; background: #35c7d9; color: #061014; font: 600 11px Consolas, monospace; padding: 2px 6px; border-radius: 4px 4px 4px 0; white-space: nowrap; }
  .hx-hover.move span { background: #a288ff; } .hx-hover.comment span { background: #ff6aa6; color: #fff; }
  .hx-editing { outline: 2px solid #35c7d9 !important; outline-offset: 3px; border-radius: 3px; background: rgba(53,199,217,.06); cursor: text; }
  .hx-changed { box-shadow: inset 0 -2px 0 rgba(242,193,78,.75); }
  .hx-moved { outline: 1px dashed rgba(162,136,255,.7); outline-offset: 2px; }
  body.hx-on .hx-removed { display: revert !important; opacity: .28 !important; outline: 2px dashed #ef5b5b; outline-offset: 2px;
    background-image: repeating-linear-gradient(45deg, rgba(239,91,91,.12) 0 6px, transparent 6px 12px) !important; }
  body.hx-on.hx-preview .hx-removed { display: none !important; }
  body.hx-on.hx-preview .hx-changed, body.hx-on.hx-preview .hx-moved { box-shadow: none; outline: none; }
  body.hx-on.hx-preview .hx-pin { display: none !important; }
  .hx-sel { position: absolute; z-index: 2147482995; pointer-events: none; border: 2px solid #a288ff; border-radius: 6px; }
  .hx-sel .tools { position: absolute; left: -2px; top: -38px; display: flex; gap: 3px; pointer-events: auto; background: rgba(16,20,26,.96); border: 1px solid #2c3642; border-radius: 9px; padding: 3px; }
  .hx-sel .tools button { padding: 3px 8px; border-radius: 6px; }
  .hx-sel .tools .grip { cursor: grab; background: #a288ff; color: #0b0e12; font-weight: 700; border-color: #a288ff; touch-action: none; }
  .hx-sel .tools .lbl { padding: 3px 6px; font: 11px Consolas, monospace; opacity: .75; align-self: center; white-space: nowrap; max-width: 280px; overflow: hidden; text-overflow: ellipsis; }
  .hx-drop { position: fixed; z-index: 2147482996; pointer-events: none; background: #a288ff; border-radius: 3px; box-shadow: 0 0 12px #a288ff; }
  .hx-ghost { position: fixed; z-index: 2147482997; pointer-events: none; opacity: .85; transform: rotate(-1deg); box-shadow: 0 16px 50px rgba(0,0,0,.6); border-radius: 8px; overflow: hidden; outline: 2px solid #a288ff; }
  .hx-dragging { opacity: .35 !important; }
  .hx-pin { position: absolute; z-index: 2147482980; width: 24px; height: 24px; border-radius: 12px 12px 12px 3px; background: #ff6aa6; color: #fff; font: 700 12px system-ui;
    display: flex; align-items: center; justify-content: center; cursor: pointer; box-shadow: 0 3px 10px rgba(0,0,0,.4); border: 2px solid #fff; transform: translate(-6px, -18px); }
  .hx-pin:hover { transform: translate(-6px, -18px) scale(1.12); }
  .hx-pin .tip { display: none; position: absolute; left: 28px; top: -2px; width: 260px; background: #10141a; border: 1px solid #2c3642; border-radius: 9px; padding: 8px 10px; font-weight: 400; text-align: left; color: #e3e8ee; white-space: pre-wrap; }
  .hx-pin:hover .tip { display: block; }
  .hx-pin .tip img { max-width: 100%; border-radius: 5px; margin-top: 6px; display: block; }
  .hx-pop { position: fixed; z-index: 2147483001; width: 380px; max-width: calc(100vw - 16px); background: #10141a; border: 1px solid #2c3642; border-radius: 14px; padding: 12px; box-shadow: 0 20px 60px rgba(0,0,0,.6); }
  .hx-pop h4 { margin: 0 0 6px; font-size: 13px; font-weight: 600; color: #ff6aa6; }
  .hx-pop .ctx { font: 11px Consolas, monospace; opacity: .6; margin-bottom: 8px; word-break: break-all; }
  .hx-pop textarea { width: 100%; min-height: 90px; resize: vertical; background: #0b0e12; color: #e3e8ee; border: 1px solid #2c3642; border-radius: 9px; padding: 8px; font: inherit; }
  .hx-pop textarea:focus { outline: none; border-color: #ff6aa6; }
  .hx-pop .row { display: flex; gap: 6px; margin-top: 8px; flex-wrap: wrap; align-items: center; }
  .hx-pop .row .grow { flex: 1; }
  .hx-pop .primary { background: #ff6aa6; border-color: #ff6aa6; color: #fff; font-weight: 600; }
  .hx-pop .danger { color: #ef5b5b; }
  .hx-pop .imgs { display: flex; gap: 6px; flex-wrap: wrap; margin-top: 8px; }
  .hx-pop .imgs div { position: relative; }
  .hx-pop .imgs img { height: 64px; border-radius: 6px; border: 1px solid #2c3642; display: block; }
  .hx-pop .imgs b { position: absolute; right: -6px; top: -6px; width: 18px; height: 18px; border-radius: 9px; background: #ef5b5b; color: #fff; font-size: 11px; display: flex; align-items: center; justify-content: center; cursor: pointer; }
  .hx-pop .hint { font-size: 11px; opacity: .55; margin-top: 6px; }
  .hx-pop canvas { display: block; width: 100%; background: #fff; border-radius: 8px; touch-action: none; cursor: crosshair; margin-top: 8px; }
  .hx-pop .sw { width: 22px; height: 22px; border-radius: 11px; padding: 0 !important; border: 2px solid #2c3642; }
  .hx-pop .sw.on { border-color: #fff; }
  .hx-pop.textpop textarea { min-height: 60px; }
  .hx-pop.textpop h4 { color: #35c7d9; }
  .hx-pop.textpop .primary { background: #35c7d9; border-color: #35c7d9; color: #061014; }
  .hx-drawer { position: fixed; top: 0; right: 0; bottom: 0; width: 400px; max-width: 100vw; z-index: 2147483002; background: #10141a; border-left: 1px solid #2c3642; box-shadow: -20px 0 60px rgba(0,0,0,.5);
    display: flex; flex-direction: column; transform: translateX(105%); transition: transform .2s; }
  .hx-drawer.open { transform: none; }
  .hx-drawer header { display: flex; align-items: center; gap: 8px; padding: 14px; border-bottom: 1px solid #2c3642; }
  .hx-drawer header b { flex: 1; font-size: 15px; }
  .hx-drawer .list { overflow: auto; flex: 1; padding: 8px 14px 80px; }
  .hx-drawer h5 { margin: 14px 0 6px; font-size: 11px; letter-spacing: .08em; text-transform: uppercase; opacity: .55; }
  .hx-item { border: 1px solid #2c3642; border-radius: 10px; padding: 8px 10px; margin-bottom: 6px; background: #161b22; cursor: pointer; position: relative; }
  .hx-item:hover { border-color: #35c7d9; }
  .hx-item .w { font: 11px Consolas, monospace; opacity: .5; margin-bottom: 3px; padding-right: 22px; word-break: break-all; }
  .hx-item .x { position: absolute; right: 6px; top: 6px; padding: 0 6px !important; border: none !important; background: none !important; opacity: .5; font-size: 15px; }
  .hx-item .x:hover { opacity: 1; color: #ef5b5b; }
  .hx-item del { color: #ef8b8b; text-decoration: line-through; opacity: .8; }
  .hx-item ins { color: #4cd18a; text-decoration: none; }
  .hx-item img { height: 40px; border-radius: 4px; margin: 4px 4px 0 0; }
  .hx-empty { opacity: .5; padding: 30px 0; text-align: center; }
  .hx-toast { position: fixed; left: 50%; bottom: 78px; transform: translateX(-50%); z-index: 2147483003; background: #1d242d; border: 1px solid #2c3642; border-radius: 10px; padding: 8px 14px; box-shadow: 0 10px 30px rgba(0,0,0,.5); transition: opacity .3s; }
  .hx-help { position: fixed; left: 50%; top: 50%; transform: translate(-50%,-50%); z-index: 2147483004; width: 520px; max-width: calc(100vw - 24px); background: #10141a; border: 1px solid #2c3642; border-radius: 16px; padding: 20px 22px; box-shadow: 0 30px 90px rgba(0,0,0,.7); }
  .hx-help h3 { margin: 0 0 10px; color: #35c7d9; font-size: 17px; }
  .hx-help dl { display: grid; grid-template-columns: 110px 1fr; gap: 8px 12px; margin: 0 0 14px; }
  .hx-help dt { font-weight: 600; } .hx-help dd { margin: 0; opacity: .85; }
  .hx-help button { font: inherit; color: #061014; background: #35c7d9; border: 0; border-radius: 9px; padding: 7px 14px; cursor: pointer; font-weight: 600; }
  body.hx-mode-text, body.hx-mode-comment { cursor: crosshair; }
  body.hx-mode-move { cursor: default; }
  body.hx-on { padding-bottom: 70px; }
  `;
  document.head.appendChild(css);

  /* ================= UI helpers ================= */
  function h(tag, attrs, ...kids) {
    const el = document.createElement(tag);
    for (const k in attrs || {}) {
      if (k === 'on') for (const ev in attrs.on) el.addEventListener(ev, attrs.on[ev]);
      else if (k === 'class') el.className = attrs[k];
      else if (k === 'html') el.innerHTML = attrs[k];
      else if (attrs[k] != null) el.setAttribute(k, attrs[k]);
    }
    kids.flat().forEach(c => c != null && el.append(c));
    return el;
  }
  function ui(el) { el.setAttribute(UI_ATTR, ''); document.body.appendChild(el); return el; }
  let toastEl = null, toastT = 0;
  function toast(msg) {
    if (!toastEl) toastEl = ui(h('div', { class: 'hx-toast' }));
    toastEl.textContent = msg; toastEl.style.opacity = 1;
    clearTimeout(toastT); toastT = setTimeout(() => toastEl.style.opacity = 0, 2200);
  }
  const de = () => lang() === 'de';
  const L = (en, dee) => de() ? dee : en;
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  /* ================= toolbar ================= */
  let mode = 'browse';
  const modes = [
    ['browse', () => L('Browse', 'Ansehen'), 'B'],
    ['text', () => L('Edit text', 'Text'), 'T'],
    ['move', () => L('Move', 'Verschieben'), 'M'],
    ['comment', () => L('Comment', 'Kommentar'), 'C']
  ];
  const bar = ui(h('div', { class: 'hx-bar' }));
  const modeBtns = {};
  let countEl, previewBtn;
  function buildBar() {
    bar.replaceChildren(h('span', { class: 'logo' }, '✎ Edit'));
    modes.forEach(([m, label, k]) => {
      modeBtns[m] = h('button', { title: k, on: { click: () => setMode(m) } }, label(), h('kbd', {}, k));
      bar.append(modeBtns[m]);
    });
    bar.append(h('span', { class: 'sep' }));
    previewBtn = h('button', { title: 'P', on: { click: togglePreview } }, L('Preview', 'Vorschau'));
    countEl = h('span', { class: 'hx-count' }, '0');
    bar.append(previewBtn,
      h('button', { title: 'Ctrl+Z', on: { click: undo } }, '↶'),
      h('button', { on: { click: () => drawer.toggle() } }, L('Changes', 'Änderungen'), countEl),
      h('button', { on: { click: exportFile } }, L('Export', 'Exportieren')),
      h('button', { title: L('Help', 'Hilfe'), on: { click: help } }, '?'),
      h('button', { title: L('Close editor', 'Editor schließen'), on: { click: closeEditor } }, '✕'));
    setMode(mode);
    updateCount();
  }
  function updateCount() {
    if (countEl) countEl.textContent = state.text.length + state.moves.length + state.hidden.length + state.comments.length;
  }
  function setMode(m) {
    if (editing) finishEdit(true);
    mode = m;
    Object.entries(modeBtns).forEach(([k, b]) => b.classList.toggle('on', k === m));
    document.body.classList.remove('hx-mode-browse', 'hx-mode-text', 'hx-mode-move', 'hx-mode-comment');
    document.body.classList.add('hx-mode-' + m);
    hover.style.display = 'none';
    select(null);
  }
  function togglePreview() {
    document.body.classList.toggle('hx-preview');
    previewBtn.classList.toggle('on', document.body.classList.contains('hx-preview'));
  }
  function closeEditor() {
    try { sessionStorage.removeItem('hydra-edit'); } catch (e) {}
    const u = new URL(location.href); u.searchParams.delete('edit');
    location.href = u.toString();
  }
  function help() {
    const box = ui(h('div', { class: 'hx-help' }));
    box.innerHTML = de() ? `
      <h3>Website bearbeiten</h3>
      <dl>
        <dt>Ansehen (B)</dt><dd>Seite normal benutzen: Links, Sprache, Features umschalten.</dd>
        <dt>Text (T)</dt><dd>Text anklicken und tippen. Enter = fertig, Shift+Enter = neue Zeile, Esc = verwerfen. Gilt für die gerade angezeigte Sprache.</dd>
        <dt>Verschieben (M)</dt><dd>Element anklicken. Am Griff ⠿ ziehen oder ↑ ↓ benutzen. „▲“ wählt das umgebende Element, „Ausblenden“ schlägt vor, es zu entfernen.</dd>
        <dt>Kommentar (C)</dt><dd>Element anklicken, Notiz schreiben. Bilder per Strg+V einfügen, hochladen oder eine Skizze zeichnen (Grafik-Vorschläge).</dd>
        <dt>Vorschau (P)</dt><dd>Zeigt die Seite ohne Markierungen, Ausgeblendetes verschwindet.</dd>
        <dt>Exportieren</dt><dd>Lädt eine Datei mit allen Änderungen. Gib sie Claude, der überträgt alles in den Quelltext.</dd>
      </dl>
      <p style="opacity:.6;margin:0 0 14px">Alles wird automatisch im Browser gespeichert, auch über Neuladen hinweg.</p>` : `
      <h3>Edit the website</h3>
      <dl>
        <dt>Browse (B)</dt><dd>Use the page normally: links, language, feature tabs.</dd>
        <dt>Edit text (T)</dt><dd>Click a text and type. Enter = done, Shift+Enter = new line, Esc = discard. Applies to the language shown.</dd>
        <dt>Move (M)</dt><dd>Click an element. Drag the ⠿ grip or use ↑ ↓. “▲” selects the surrounding element, “Hide” suggests removing it.</dd>
        <dt>Comment (C)</dt><dd>Click an element and write a note. Paste images (Ctrl+V), upload one or draw a sketch (graphic ideas).</dd>
        <dt>Preview (P)</dt><dd>Shows the page without markers, hidden elements disappear.</dd>
        <dt>Export</dt><dd>Downloads one file with all changes. Give it to Claude, who applies it to the source.</dd>
      </dl>
      <p style="opacity:.6;margin:0 0 14px">Everything is saved in the browser automatically, also across reloads.</p>`;
    const ok = h('button', { on: { click: () => box.remove() } }, 'OK');
    box.append(ok); ok.focus({ preventScroll: true });
  }

  /* ================= hover highlight ================= */
  const hover = ui(h('div', { class: 'hx-hover' }, h('span')));
  hover.style.display = 'none';
  const TEXT_SEL = 'h1,h2,h3,h4,h5,h6,p,li,td,th,dt,dd,figcaption,blockquote,summary,label,.eyebrow,.cap,.pill,.btn,a,button,.stat b,.stat > span,.where span,code,pre,kbd,.grp,.n';
  function textTarget(t) {
    if (!t || isUi(t) || t.closest('[data-g], svg')) return null;
    const lng = t.closest('.en, .de');
    const block = t.closest(TEXT_SEL);
    if (lng && block && block.contains(lng) && lng !== block) {
      // a small language span inside a block: edit the span (clearer), unless the block has only one span pair
      return lng;
    }
    if (lng && !block) return lng;
    return block;
  }
  function movable(el) {
    if (!el || isUi(el) || el === document.body || el === document.documentElement) return false;
    if (el.parentElement && el.parentElement.closest('[data-g]')) return false;
    if (el.closest('svg') && el.tagName.toLowerCase() !== 'svg') return false;
    if (!orig.has(el)) return false;
    if (['home', 'docs', 'wiki', 'navLinks'].includes(el.id)) return false;
    if (el.matches('header.top, footer.site, script, style, template')) return false;
    const sibs = [...el.parentElement.children].filter(c => orig.has(c) && isVisible(c));
    return sibs.length >= 2;
  }
  function moveTarget(t) {
    let cur = t;
    if (cur && cur.closest && cur.closest('[data-g]')) cur = cur.closest('[data-g]');
    while (cur && cur !== document.body) {
      if (movable(cur) && isVisible(cur) && !cur.matches('.en, .de, b, i, span:not([class]), br, strong, em, code, kbd')) return cur;
      cur = cur.parentElement;
    }
    return null;
  }
  function commentTarget(t) {
    if (!t || isUi(t)) return null;
    const g = t.closest('[data-g]'); if (g) return g;
    const img = t.closest('img'); if (img) return img;
    return t.closest(TEXT_SEL + ',.fx,.step,.stat,.shot-frame,.routing,section,article,table,.card,div') || null;
  }
  function targetFor(t) {
    if (mode === 'text') return textTarget(t);
    if (mode === 'move') return moveTarget(t);
    if (mode === 'comment') return commentTarget(t);
    return null;
  }
  function showHover(el) {
    if (!el) { hover.style.display = 'none'; return; }
    const r = el.getBoundingClientRect();
    Object.assign(hover.style, { display: 'block', left: r.left - 3 + 'px', top: r.top - 3 + 'px', width: r.width + 6 + 'px', height: r.height + 6 + 'px' });
    hover.className = 'hx-hover ' + mode;
    hover.firstChild.textContent = describe(el);
    hover.firstChild.style.top = r.top < 24 ? (r.height + 6) + 'px' : '-20px';
  }
  document.addEventListener('mousemove', e => {
    if (mode === 'browse' || dragging || isUi(e.target) || popOpen()) { if (!dragging) hover.style.display = 'none'; return; }
    if (editing && editing.contains(e.target)) { hover.style.display = 'none'; return; }
    showHover(targetFor(e.target));
  }, true);
  window.addEventListener('scroll', () => { hover.style.display = 'none'; }, true);

  /* ================= click routing ================= */
  ['click', 'auxclick', 'dblclick'].forEach(type => document.addEventListener(type, e => {
    if (mode === 'browse' || isUi(e.target)) return;
    if (editing && editing.contains(e.target)) { if (type !== 'dblclick') e.preventDefault(); return; }
    e.preventDefault(); e.stopPropagation(); e.stopImmediatePropagation();
    if (type !== 'click') return;
    const el = targetFor(e.target);
    if (!el) return;
    if (mode === 'text') startEdit(el);
    else if (mode === 'move') select(el);
    else if (mode === 'comment') openComment(null, el);
  }, true));
  ['mousedown', 'pointerdown'].forEach(type => document.addEventListener(type, e => {
    if (mode === 'browse' || isUi(e.target)) return;
    if (editing && editing.contains(e.target)) return;
    if (mode === 'move' || mode === 'comment') e.preventDefault();
    if (mode === 'text' && editing && !editing.contains(e.target)) finishEdit(true);
  }, true));
  // links and <img> must not start native drags while editing
  document.addEventListener('dragstart', e => { if (mode !== 'browse' && !isUi(e.target)) e.preventDefault(); }, true);

  /* ================= keyboard ================= */
  document.addEventListener('keydown', e => {
    if (e.target && (e.target.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName))) return;
    if (document.querySelector('.hx-help')) { if (e.key === 'Escape' || e.key === 'Enter') { document.querySelector('.hx-help').remove(); e.preventDefault(); } return; }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') { e.preventDefault(); undo(); return; }
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    const k = e.key.toLowerCase();
    const m = { b: 'browse', t: 'text', m: 'move', c: 'comment' }[k];
    if (m) { e.preventDefault(); e.stopPropagation(); setMode(m); return; }
    if (k === 'p') { e.preventDefault(); e.stopPropagation(); togglePreview(); return; }
    if (k === 'escape') { if (selected) select(null); else if (drawer.isOpen()) drawer.toggle(false); else setMode('browse'); return; }
    if (selected && (k === 'arrowup' || k === 'arrowleft')) { e.preventDefault(); step(-1); }
    if (selected && (k === 'arrowdown' || k === 'arrowright')) { e.preventDefault(); step(1); }
  }, true);

  /* ================= text editing ================= */
  let editing = null, editBefore = '';
  function startEdit(el) {
    if (editing) finishEdit(true);
    if (el.closest('button, select') || el.matches('button')) { textPopover(el); return; }
    editing = el; editBefore = el.innerHTML;
    el.contentEditable = 'true';
    el.spellcheck = true;
    el.classList.add('hx-editing');
    el.focus({ preventScroll: true });
    // caret at the click position is set by the browser on the next click; place it at the end now
    const r = document.createRange(); r.selectNodeContents(el); r.collapse(false);
    const s = getSelection(); s.removeAllRanges(); s.addRange(r);
    el.addEventListener('keydown', editKeys);
    el.addEventListener('paste', plainPaste);
    el.addEventListener('blur', onEditBlur);
    hover.style.display = 'none';
  }
  function onEditBlur() { setTimeout(() => { if (editing && document.activeElement !== editing) finishEdit(true); }, 0); }
  function editKeys(e) {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); finishEdit(true); }
    else if (e.key === 'Enter' && e.shiftKey) { e.preventDefault(); document.execCommand('insertLineBreak'); }
    else if (e.key === 'Escape') { e.preventDefault(); finishEdit(false); }
    e.stopPropagation();
  }
  function plainPaste(e) {
    e.preventDefault();
    const t = (e.clipboardData || window.clipboardData).getData('text/plain');
    document.execCommand('insertText', false, t);
  }
  function finishEdit(keep) {
    const el = editing; if (!el) return;
    editing = null;
    el.removeEventListener('keydown', editKeys);
    el.removeEventListener('paste', plainPaste);
    el.removeEventListener('blur', onEditBlur);
    el.removeAttribute('contenteditable');
    el.removeAttribute('spellcheck');
    el.classList.remove('hx-editing');
    if (!keep) { el.innerHTML = editBefore; stamp(el); return; }
    commitText(el, editBefore, cleanHtml(el.innerHTML));
  }
  function cleanHtml(s) {
    return s.replace(/&nbsp;/g, ' ').replace(/<br\s*\/?>\s*$/i, '').replace(/\s+(class|contenteditable|spellcheck)="hx-[^"]*"/g, '');
  }
  function commitText(el, before, after) {
    if (after === before) return;
    el.innerHTML = after;
    stamp(el);
    const p = pathOf(el);
    if (!p) { toast('Could not address this element'); return; }
    const k = pkey(p);
    let t = state.text.find(x => pkey(x.p) === k);
    if (t) { t.after = after; if (t.after === t.before) state.text = state.text.filter(x => x !== t); }
    else state.text.push({ id: ++state.seq, p, lang: lang(), where: where(el), before, after, t: Date.now() });
    el.classList.add('hx-changed');
    save(); updateCount(); drawer.refresh();
  }
  function textPopover(el) {
    const before = el.innerHTML;
    const plain = el.innerText;
    const pop = popover('textpop', el);
    const ta = h('textarea', {}, ); ta.value = plain;
    pop.append(h('h4', {}, L('Edit text', 'Text bearbeiten')), h('div', { class: 'ctx' }, where(el)), ta,
      h('div', { class: 'row' }, h('span', { class: 'grow' }),
        h('button', { on: { click: () => closePop() } }, L('Cancel', 'Abbrechen')),
        h('button', { class: 'primary', on: { click: done } }, 'OK')));
    function done() {
      // replace the text nodes only, keep icons/markup around (buttons hold svgs)
      const textNodes = [];
      const w = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
      while (w.nextNode()) if (w.currentNode.nodeValue.trim() && !w.currentNode.parentNode.closest('svg') && isVisible(w.currentNode.parentElement)) textNodes.push(w.currentNode);
      if (textNodes.length) { textNodes[0].nodeValue = ta.value; textNodes.slice(1).forEach(n => n.nodeValue = ''); }
      else el.append(ta.value);
      commitText(el, before, el.innerHTML);
      closePop();
    }
    ta.addEventListener('keydown', e => { if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); done(); } if (e.key === 'Escape') closePop(); });
    ta.focus({ preventScroll: true }); ta.select();
  }

  /* ================= move / hide ================= */
  let selected = null, selBox = null;
  function select(el) {
    selected = el;
    if (selBox) { selBox.remove(); selBox = null; }
    if (!el) return;
    selBox = ui(h('div', { class: 'hx-sel' }));
    const tools = h('div', { class: 'tools' },
      h('button', { class: 'grip', title: L('Drag to move', 'Ziehen zum Verschieben'), on: { pointerdown: startDrag } }, '⠿'),
      h('button', { title: L('Earlier', 'Nach vorne'), on: { click: () => step(-1) } }, '↑'),
      h('button', { title: L('Later', 'Nach hinten'), on: { click: () => step(1) } }, '↓'),
      h('button', { title: L('Select the surrounding element', 'Umgebendes Element wählen'), on: { click: up } }, '▲'),
      h('button', { on: { click: toggleHide } }, el.classList.contains('hx-removed') ? L('Show', 'Einblenden') : L('Hide', 'Ausblenden')),
      h('button', { title: L('Comment', 'Kommentar'), on: { click: () => openComment(null, el) } }, '💬'),
      h('span', { class: 'lbl' }, describe(el) + '  ' + snippet(el, 40)));
    selBox.append(tools);
    placeSel();
  }
  function placeSel() {
    if (!selBox || !selected) return;
    const r = selected.getBoundingClientRect();
    Object.assign(selBox.style, { left: r.left + scrollX - 3 + 'px', top: r.top + scrollY - 3 + 'px', width: r.width + 6 + 'px', height: r.height + 6 + 'px' });
    selBox.firstChild.style.top = r.top < 90 ? (r.height + 10) + 'px' : '-38px';
  }
  function up() {
    let cur = selected && selected.parentElement;
    while (cur && !movable(cur)) cur = cur.parentElement;
    if (cur) select(cur); else toast(L('No larger element', 'Kein größeres Element'));
  }
  function visibleSiblings(el) { return [...el.parentElement.children].filter(c => orig.has(c) && isVisible(c)); }
  function step(d) {
    const el = selected; if (!el) return;
    const sibs = visibleSiblings(el);
    const i = sibs.indexOf(el), j = i + d;
    if (j < 0 || j >= sibs.length) return;
    const before = orderOf(el.parentElement);
    if (d < 0) sibs[j].before(el); else sibs[j].after(el);
    commitMove(el, before);
  }
  function commitMove(el, before) {
    const parent = el.parentElement;
    const after = orderOf(parent);
    if (after.join() === before.join()) { placeSel(); return; }
    const p = pathOf(parent);
    if (!p) { toast('Could not address this element'); return; }
    const labels = ord => ord.map(i => { const c = [...parent.children].find(x => orig.get(x) === i); return describe(c) + ' "' + snippet(c, 40) + '"'; });
    state.moves.push({ id: ++state.seq, p, where: where(parent), item: describe(el) + ' "' + snippet(el, 50) + '"',
      before: labels(before), after: labels(after), beforeOrder: before, order: after, t: Date.now() });
    el.classList.add('hx-moved');
    save(); updateCount(); drawer.refresh();
    placeSel(); pins();
  }
  function toggleHide() {
    const el = selected; if (!el) return;
    const p = pathOf(el); if (!p) return;
    const k = pkey(p);
    const ex = state.hidden.find(x => pkey(x.p) === k);
    if (ex) { state.hidden = state.hidden.filter(x => x !== ex); el.classList.remove('hx-removed'); }
    else { state.hidden.push({ id: ++state.seq, p, where: where(el), item: describe(el) + ' "' + snippet(el, 60) + '"', t: Date.now() }); el.classList.add('hx-removed'); }
    save(); updateCount(); drawer.refresh(); select(el); pins();
  }

  // drag
  let dragging = null;
  function startDrag(e) {
    const el = selected; if (!el) return;
    e.preventDefault(); e.stopPropagation();
    const r = el.getBoundingClientRect();
    const ghost = ui(h('div', { class: 'hx-ghost' }));
    const clone = el.cloneNode(true);
    clone.style.margin = '0'; clone.style.width = r.width + 'px';
    ghost.append(clone);
    Object.assign(ghost.style, { width: Math.min(r.width, 600) + 'px', maxHeight: '260px', left: r.left + 'px', top: r.top + 'px' });
    const drop = ui(h('div', { class: 'hx-drop' }));
    el.classList.add('hx-dragging');
    dragging = { el, ghost, drop, dx: e.clientX - r.left, dy: e.clientY - r.top, before: orderOf(el.parentElement), target: null };
    selBox.style.display = 'none';
    window.addEventListener('pointermove', onDrag, true);
    window.addEventListener('pointerup', endDrag, true);
  }
  function onDrag(e) {
    const d = dragging; if (!d) return;
    d.ghost.style.left = e.clientX - d.dx + 'px';
    d.ghost.style.top = e.clientY - d.dy + 'px';
    // auto-scroll near the window edges
    if (e.clientY < 60) scrollBy(0, -14); else if (e.clientY > innerHeight - 60) scrollBy(0, 14);
    const sibs = visibleSiblings(d.el).filter(c => c !== d.el);
    if (!sibs.length) return;
    const rects = sibs.map(c => c.getBoundingClientRect());
    // horizontal layout if the siblings share rows
    const row = rects.length > 1 && Math.abs(rects[0].top - rects[1].top) < 4;
    let best = 0, bd = Infinity;
    rects.forEach((r, i) => {
      const cx = Math.max(r.left, Math.min(e.clientX, r.right)), cy = Math.max(r.top, Math.min(e.clientY, r.bottom));
      const dd = Math.hypot(e.clientX - cx, e.clientY - cy);
      if (dd < bd) { bd = dd; best = i; }
    });
    const r = rects[best];
    const sameRow = row || (Math.abs(r.top - (rects[best + 1] || rects[best - 1] || r).top) < 4 && rects.length > 1);
    const after = sameRow ? e.clientX > r.left + r.width / 2 : e.clientY > r.top + r.height / 2;
    d.target = { el: sibs[best], after };
    if (sameRow) Object.assign(d.drop.style, { left: (after ? r.right + 2 : r.left - 5) + 'px', top: r.top + 'px', width: '3px', height: r.height + 'px' });
    else Object.assign(d.drop.style, { left: r.left + 'px', top: (after ? r.bottom + 2 : r.top - 5) + 'px', width: r.width + 'px', height: '3px' });
  }
  function endDrag() {
    const d = dragging; if (!d) return;
    dragging = null;
    window.removeEventListener('pointermove', onDrag, true);
    window.removeEventListener('pointerup', endDrag, true);
    d.ghost.remove(); d.drop.remove();
    d.el.classList.remove('hx-dragging');
    if (selBox) selBox.style.display = '';
    if (d.target) { if (d.target.after) d.target.el.after(d.el); else d.target.el.before(d.el); }
    commitMove(d.el, d.before);
  }

  /* ================= comments ================= */
  let pop = null;
  const popOpen = () => !!pop;
  let popNear = null;
  function popover(cls, near) {
    closePop();
    pop = ui(h('div', { class: 'hx-pop ' + cls }));
    popNear = near;
    requestAnimationFrame(placePop);
    return pop;
  }
  // Below the element if it fits, else above, always clamped into the window (above the toolbar)
  function placePop() {
    if (!pop || !popNear) return;
    const r = popNear.getBoundingClientRect();
    const w = pop.offsetWidth, ph = pop.offsetHeight, maxTop = Math.max(8, innerHeight - ph - 70);
    let top = r.bottom + 10;
    if (top > maxTop) top = r.top - ph - 10;
    pop.style.left = Math.min(Math.max(8, r.left), Math.max(8, innerWidth - w - 8)) + 'px';
    pop.style.top = Math.min(Math.max(8, top), maxTop) + 'px';
  }
  function closePop() { if (pop) { pop.remove(); pop = null; } }
  document.addEventListener('pointerdown', e => { if (pop && !pop.contains(e.target) && !isUi(e.target)) closePop(); }, true);

  function openComment(existing, el) {
    let target = el;
    const c = existing ? JSON.parse(JSON.stringify(existing)) : { text: '', images: [] };
    function render() {
      const box = popover('', target);
      const ta = h('textarea', { placeholder: L('Your note … (Ctrl+V pastes an image)', 'Deine Notiz … (Strg+V fügt ein Bild ein)') });
      ta.value = c.text;
      ta.addEventListener('input', () => c.text = ta.value);
      ta.addEventListener('paste', e => {
        const items = [...(e.clipboardData && e.clipboardData.items || [])].filter(i => i.type.startsWith('image/'));
        if (!items.length) return;
        e.preventDefault();
        items.forEach(i => addImage(i.getAsFile()));
      });
      ta.addEventListener('keydown', e => {
        if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); saveIt(); }
        if (e.key === 'Escape') { e.preventDefault(); closePop(); }
        e.stopPropagation();
      });
      const imgs = h('div', { class: 'imgs' });
      c.images.forEach((src, i) => imgs.append(h('div', {}, h('img', { src }), h('b', { title: 'Remove', on: { click: () => { c.images.splice(i, 1); render(); } } }, '×'))));
      const file = h('input', { type: 'file', accept: 'image/*', multiple: '', style: 'display:none' });
      file.addEventListener('change', () => [...file.files].forEach(addImage));
      box.append(
        h('h4', {}, existing ? L('Comment', 'Kommentar') + ' ' + existing.n : L('New comment', 'Neuer Kommentar')),
        h('div', { class: 'ctx' }, describe(target) + (snippet(target, 50) ? ' · “' + snippet(target, 50) + '”' : '')),
        ta, imgs,
        h('div', { class: 'row' },
          h('button', { title: L('Attach the comment to the surrounding element', 'Kommentar an das umgebende Element hängen'), on: { click: widen } }, '▲ ' + L('Larger', 'Größer')),
          h('button', { on: { click: () => file.click() } }, '🖼 ' + L('Image', 'Bild')),
          h('button', { on: { click: sketch } }, '✏ ' + L('Sketch', 'Skizze')),
          file),
        h('div', { class: 'row' },
          existing ? h('button', { class: 'danger', on: { click: del } }, L('Delete', 'Löschen')) : null,
          h('span', { class: 'grow' }),
          h('button', { on: { click: closePop } }, L('Cancel', 'Abbrechen')),
          h('button', { class: 'primary', on: { click: saveIt } }, L('Save', 'Speichern'))),
        h('div', { class: 'hint' }, L('Ctrl+Enter saves', 'Strg+Enter speichert')));
      placePop();
      ta.focus({ preventScroll: true });
      ta.setSelectionRange(ta.value.length, ta.value.length);
    }
    function widen() {
      let cur = target.parentElement;
      while (cur && cur !== document.body && (!orig.has(cur) || cur.getBoundingClientRect().height <= target.getBoundingClientRect().height + 2)) cur = cur.parentElement;
      if (cur && cur !== document.body && orig.has(cur)) { target = cur; render(); showHoverFixed(cur); }
    }
    async function addImage(f) {
      if (!f) return;
      c.images.push(await shrink(f));
      render();
    }
    function sketch() {
      const box = pop;
      const W = 720, H = 420;
      const cv = h('canvas', { width: W, height: H });
      const ctx = cv.getContext('2d');
      ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, W, H);
      ctx.lineCap = 'round'; ctx.lineJoin = 'round';
      let color = '#141b24', size = 3, last = null;
      const colors = ['#141b24', '#35c7d9', '#ff6aa6', '#a288ff', '#4cd18a', '#f2c14e', '#ef5b5b'];
      const sws = colors.map(col => {
        const b = h('button', { class: 'sw' + (col === color ? ' on' : ''), style: 'background:' + col, on: { click: () => { color = col; size = 3; sws.forEach(s => s.classList.remove('on')); b.classList.add('on'); } } });
        return b;
      });
      const eraser = h('button', { on: { click: () => { color = '#ffffff'; size = 18; sws.forEach(s => s.classList.remove('on')); } } }, L('Eraser', 'Radierer'));
      const pt = e => { const r = cv.getBoundingClientRect(); return [(e.clientX - r.left) * W / r.width, (e.clientY - r.top) * H / r.height]; };
      cv.addEventListener('pointerdown', e => { cv.setPointerCapture(e.pointerId); last = pt(e); ctx.fillStyle = color; ctx.beginPath(); ctx.arc(last[0], last[1], size / 2, 0, 7); ctx.fill(); });
      cv.addEventListener('pointermove', e => { if (!last) return; const p = pt(e); ctx.strokeStyle = color; ctx.lineWidth = size; ctx.beginPath(); ctx.moveTo(...last); ctx.lineTo(...p); ctx.stroke(); last = p; });
      cv.addEventListener('pointerup', () => last = null);
      const wrap = h('div', {}, cv, h('div', { class: 'row' }, ...sws, eraser,
        h('button', { on: { click: () => { ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, W, H); } } }, L('Clear', 'Leeren')),
        h('span', { class: 'grow' }),
        h('button', { class: 'primary', on: { click: () => { c.images.push(cv.toDataURL('image/png')); render(); } } }, L('Add sketch', 'Skizze anhängen'))));
      box.style.width = '560px';
      box.querySelectorAll('.row, .hint, .imgs').forEach(x => x.remove());
      box.append(wrap);
      placePop();
    }
    function saveIt() {
      if (!c.text.trim() && !c.images.length) { closePop(); return; }
      if (existing) {
        const e = state.comments.find(x => x.id === existing.id);
        Object.assign(e, { text: c.text, images: c.images });
        if (target !== el) Object.assign(e, { p: pathOf(target), where: where(target), context: snippet(target, 120) });
      } else {
        const p = pathOf(target);
        if (!p) { toast('Could not address this element'); return; }
        const n = state.comments.reduce((m, x) => Math.max(m, x.n), 0) + 1;
        state.comments.push({ id: ++state.seq, n, p, lang: lang(), where: where(target), context: snippet(target, 120), text: c.text, images: c.images, t: Date.now() });
      }
      save(); updateCount(); drawer.refresh(); closePop(); pins();
    }
    function del() {
      state.comments = state.comments.filter(x => x.id !== existing.id);
      save(); updateCount(); drawer.refresh(); closePop(); pins();
    }
    render();
  }
  function showHoverFixed(el) { showHover(el); setTimeout(() => hover.style.display = 'none', 700); }
  function shrink(file) {
    return new Promise(res => {
      const img = new Image();
      const fr = new FileReader();
      fr.onload = () => {
        img.onload = () => {
          const max = 1600, s = Math.min(1, max / Math.max(img.width, img.height));
          if (s === 1 && fr.result.length < 1.5e6) { res(fr.result); return; }
          const cv = document.createElement('canvas');
          cv.width = Math.round(img.width * s); cv.height = Math.round(img.height * s);
          cv.getContext('2d').drawImage(img, 0, 0, cv.width, cv.height);
          const png = cv.toDataURL('image/png');
          res(png.length < 1.5e6 ? png : cv.toDataURL('image/jpeg', 0.88));
        };
        img.src = fr.result;
      };
      fr.readAsDataURL(file);
    });
  }

  /* ================= pins ================= */
  const pinLayer = ui(h('div', { style: 'position:absolute;left:0;top:0;width:0;height:0' }));
  function pins() {
    pinLayer.replaceChildren();
    state.comments.forEach(c => {
      const el = resolve(c.p);
      if (!el || !isVisible(el)) return;
      const r = el.getBoundingClientRect();
      if (!r.width && !r.height) return;
      const tip = h('div', { class: 'tip' }, c.text || '');
      c.images.forEach(src => tip.append(h('img', { src })));
      const pin = h('div', { class: 'hx-pin', on: { click: e => { e.stopPropagation(); openComment(c, el); }, mouseenter: () => showHover(el), mouseleave: () => hover.style.display = 'none' } }, String(c.n), tip);
      pin.style.left = Math.min(r.right + scrollX - 10, document.documentElement.scrollWidth - 30) + 'px';
      pin.style.top = r.top + scrollY + 'px';
      pinLayer.append(pin);
    });
    // mark changed text / moved items
    state.text.forEach(t => { const el = resolve(t.p); if (el) el.classList.add('hx-changed'); });
    placeSel();
  }
  let pinT = 0;
  const repin = () => { clearTimeout(pinT); pinT = setTimeout(pins, 80); };
  window.addEventListener('resize', repin);
  window.addEventListener('hashchange', () => { select(null); setTimeout(pins, 60); });
  window.addEventListener('load', repin);
  new ResizeObserver(repin).observe(document.body);
  document.addEventListener('click', e => { if (e.target.closest && e.target.closest('.lang button, .flist button')) setTimeout(pins, 60); }, false);

  /* ================= drawer ================= */
  const drawer = (() => {
    const el = ui(h('div', { class: 'hx-drawer' }));
    let open = false;
    function toggle(v) { open = v === undefined ? !open : v; el.classList.toggle('open', open); if (open) refresh(); }
    function item(where, body, onJump, onDel) {
      const it = h('div', { class: 'hx-item', on: { click: onJump } }, h('div', { class: 'w' }, where), body,
        h('button', { class: 'x', title: L('Undo this change', 'Änderung zurücknehmen'), on: { click: e => { e.stopPropagation(); onDel(); } } }, '×'));
      return it;
    }
    const stripTags = s => { const d = document.createElement('div'); d.innerHTML = s; d.querySelectorAll(lang() === 'de' ? '.en' : '.de').forEach(x => x.remove()); return d.textContent.replace(/\s+/g, ' ').trim(); };
    function refresh() {
      if (!open) return;
      const list = h('div', { class: 'list' });
      const total = state.text.length + state.moves.length + state.hidden.length + state.comments.length;
      if (!total) list.append(h('div', { class: 'hx-empty' }, L('No changes yet.', 'Noch keine Änderungen.')));
      if (state.comments.length) {
        list.append(h('h5', {}, L('Comments', 'Kommentare')));
        state.comments.slice().sort((a, b) => a.n - b.n).forEach(c => {
          const body = h('div', {}, h('b', {}, '#' + c.n + ' '), c.text || '');
          c.images.forEach(src => body.append(h('img', { src })));
          list.append(item(c.where, body, () => jump(c.p, c.lang, el2 => openComment(c, el2)), () => removeEntry('comments', c.id)));
        });
      }
      if (state.text.length) {
        list.append(h('h5', {}, L('Texts', 'Texte')));
        state.text.forEach(t => list.append(item(t.where + ' · ' + t.lang.toUpperCase(),
          h('div', { html: '<del>' + esc(stripTags(t.before)) + '</del><br><ins>' + esc(stripTags(t.after)) + '</ins>' }),
          () => jump(t.p, t.lang), () => removeEntry('text', t.id))));
      }
      if (state.moves.length) {
        list.append(h('h5', {}, L('Moved', 'Verschoben')));
        state.moves.forEach(m => list.append(item(m.where, h('div', {}, m.item), () => jump(m.p), () => removeEntry('moves', m.id))));
      }
      if (state.hidden.length) {
        list.append(h('h5', {}, L('Hidden (suggested for removal)', 'Ausgeblendet (zum Entfernen vorgeschlagen)')));
        state.hidden.forEach(x => list.append(item(x.where, h('div', {}, x.item), () => jump(x.p), () => removeEntry('hidden', x.id))));
      }
      el.replaceChildren(h('header', {}, h('b', {}, L('Changes', 'Änderungen') + ' (' + total + ')'),
        total ? h('button', { on: { click: clearAll } }, L('Clear all', 'Alles löschen')) : null,
        h('button', { on: { click: () => toggle(false) } }, '✕')), list);
    }
    return { toggle, refresh, isOpen: () => open };
  })();
  function jump(p, lng, then) {
    const go = () => {
      if (p.scope.startsWith('wiki') && document.body.dataset.view !== 'docs') location.hash = p.anchor && p.anchor.startsWith('#') ? p.anchor : '#docs';
      if (p.scope === 'page' && document.body.dataset.view === 'docs' && document.getElementById('home').contains(resolve(p) || document.body)) location.hash = '#top';
      setTimeout(() => {
        let el = resolve(p);
        // feature details are hidden unless selected
        const art = el && el.closest('article[data-f]');
        if (art && art.hidden) { const b = document.querySelector('.flist button[data-f="' + art.dataset.f + '"]'); if (b) b.click(); }
        el = resolve(p);
        if (!el) { toast(L('Element not found', 'Element nicht gefunden')); return; }
        el.scrollIntoView({ block: 'center', behavior: 'instant' });
        setTimeout(() => { showHoverFixed(el); pins(); if (then) then(el); }, 60);
      }, 120);
    };
    const want = lng || (p.scope.includes('-') ? p.scope.split('-')[1] : null);
    if (want && want !== lang()) { const b = document.querySelector('.lang button[data-lang="' + want + '"]'); if (b) b.click(); setTimeout(go, 150); }
    else go();
  }
  function removeEntry(kind, id) {
    state[kind] = state[kind].filter(x => x.id !== id);
    save(); reloadKeepingPlace();
  }
  function clearAll() {
    if (!confirm(L('Delete all changes and comments? This cannot be undone.', 'Alle Änderungen und Kommentare löschen? Das lässt sich nicht rückgängig machen.'))) return;
    state = { v: 1, seq: state.seq, text: [], moves: [], hidden: [], comments: [] };
    save(); reloadKeepingPlace();
  }
  function undo() {
    const all = [...state.text.map(x => ['text', x]), ...state.moves.map(x => ['moves', x]), ...state.hidden.map(x => ['hidden', x]), ...state.comments.map(x => ['comments', x])];
    if (!all.length) { toast(L('Nothing to undo', 'Nichts rückgängig zu machen')); return; }
    all.sort((a, b) => b[1].id - a[1].id);
    const [kind, x] = all[0];
    removeEntry(kind, x.id);
  }
  function reloadKeepingPlace() {
    try { sessionStorage.setItem('hydra-edit-restore', JSON.stringify({ y: scrollY, mode, drawer: drawer.isOpen() })); } catch (e) {}
    setTimeout(() => location.reload(), 200);   // let IndexedDB finish writing
  }

  /* ================= export ================= */
  function exportFile() {
    const out = {
      what: 'Hydra website edits. Apply to docs/index.html (texts, order, hidden) and treat comments/images as change requests.',
      version: 1,
      created: new Date().toISOString(),
      page: location.pathname,
      howToRead: {
        p: 'scope (page | wiki-en | wiki-de | nav-en | nav-de) + anchor (CSS selector of the nearest ancestor with id/data-f, empty = scope root) + idx (ORIGINAL child indices below the anchor, i.e. in index.html before any move)',
        text: 'before/after = innerHTML of the element',
        moves: 'order = new order of the parent\'s children as original indices; only the last entry per parent counts'
      },
      text: state.text, moves: state.moves, hidden: state.hidden, comments: state.comments
    };
    const blob = new Blob([JSON.stringify(out, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    const d = new Date();
    const stamp = d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0') + '_' + String(d.getHours()).padStart(2, '0') + String(d.getMinutes()).padStart(2, '0');
    a.href = URL.createObjectURL(blob);
    a.download = 'hydra-website-edits_' + stamp + '.json';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
    toast(L('Exported. Give the file to Claude.', 'Exportiert. Gib die Datei Claude.'));
  }

  /* ================= wiki re-render (language switch) ================= */
  function watch(rootGetter, kind) {
    const r = rootGetter(); if (!r) return;
    r.dataset.hxLang = lang();
    new MutationObserver(muts => {
      if (!muts.some(m => m.type === 'childList' && m.target === r)) return;
      r.dataset.hxLang = lang();
      stamp(r);
      applyScope(kind + '-' + lang());
      repin();
    }).observe(r, { childList: true });
  }

  /* ================= start ================= */
  (async () => {
    stamp(document.body);
    watch(wikiEl, 'wiki'); watch(navEl, 'nav');
    const s = await load();
    if (s && s.v === 1) state = Object.assign({ v: 1, seq: 0, text: [], moves: [], hidden: [], comments: [] }, s);
    document.body.classList.add('hx-on');
    buildBar();
    applyAll();
    document.querySelectorAll('.lang button').forEach(b => b.addEventListener('click', () => setTimeout(buildBar, 0)));
    let restore = null;
    try { restore = JSON.parse(sessionStorage.getItem('hydra-edit-restore') || 'null'); sessionStorage.removeItem('hydra-edit-restore'); } catch (e) {}
    if (restore) {
      setMode(restore.mode || 'browse');
      setTimeout(() => { scrollTo(0, restore.y || 0); pins(); }, 100);
      if (restore.drawer) drawer.toggle(true);
    } else {
      let seen = false;
      try { seen = localStorage.getItem('hydra-edit-help') === '1'; localStorage.setItem('hydra-edit-help', '1'); } catch (e) {}
      if (!seen) help();
    }
  })();
})();
