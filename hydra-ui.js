/* Hydra UI drawn as vector graphics for the website (sharp at any size).
   Colours, sizes and shapes follow the app (src/ui/Theme.*, Widgets.cpp, Strips.cpp, Inspector.cpp, RoomPages.cpp,
   LatencyView.cpp, DspView.cpp; Default theme). Every <div data-g="name" data-o="..."> is filled on load.
   The example values match the example layout of the screenshot tool (tools/Shots.cpp). */
(function () {
  'use strict';
  const C = {
    bg: '#0a0c10', panel: '#13171e', panel2: '#191e27', raised: '#212733', outline: '#262d3a',
    text: '#e8ebf1', dim: '#8f98a9', faint: '#5a6273',
    in: '#35d0f4', hw: '#a288ff', virt: '#ff6aa6', mute: '#ff4d5e', solo: '#ffc23f', good: '#3ee08f', warn: '#ffa53d',
    eq: '#35d0f4', gate: '#ffc23f', comp: '#ff8f4d', lim: '#ff6a7d', corr: '#a78bfa', phase: '#ff6ac1',
    bands: ['#ff5f6d', '#ffd84a', '#35c8f4', '#ffa53d', '#a47cff', '#3ee08f', '#ff6ac1', '#5b7cff']
  };
  const FONT = '"Segoe UI Variable Text","Segoe UI",system-ui,sans-serif';
  const MONO = '"Cascadia Mono",Consolas,ui-monospace,monospace';
  const f1 = v => Math.round(v * 10) / 10;
  const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');

  /* ---------- primitives ---------- */
  const T = (x, y, s, o = {}) => `<text x="${f1(x)}" y="${f1(y)}" font-size="${o.size || 11}" fill="${o.fill || C.text}"` +
    `${o.weight ? ` font-weight="${o.weight}"` : ''}${o.anchor ? ` text-anchor="${o.anchor}"` : ''}` +
    `${o.mono ? ` font-family='${MONO}'` : ''}${o.ls ? ` letter-spacing="${o.ls}"` : ''}${o.op ? ` opacity="${o.op}"` : ''}>${esc(s)}</text>`;
  const R = (x, y, w, h, r, fill, o = {}) => `<rect x="${f1(x)}" y="${f1(y)}" width="${f1(w)}" height="${f1(h)}" rx="${r}"` +
    ` fill="${fill}"${o.stroke ? ` stroke="${o.stroke}" stroke-width="${o.sw || 1}"` : ''}${o.op ? ` opacity="${o.op}"` : ''}/>`;
  const L = (x1, y1, x2, y2, c, w = 1, o = '') => `<line x1="${f1(x1)}" y1="${f1(y1)}" x2="${f1(x2)}" y2="${f1(y2)}" stroke="${c}" stroke-width="${w}" ${o}/>`;
  const svg = (w, h, body, label) => `<svg viewBox="0 0 ${w} ${h}" xmlns="http://www.w3.org/2000/svg" role="img"` +
    `${label ? ` aria-label="${esc(label)}"` : ''} font-family='${FONT}'>` +
    `<style>.l{fill:none;stroke:currentColor;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round}.f{fill:currentColor}</style>${body}</svg>`;
  const mix = (a, b, t) => {   // colour a -> b
    const p = c => [1, 3, 5].map(i => parseInt(c.slice(i, i + 2), 16));
    const A = p(a), B = p(b);
    return '#' + A.map((v, i) => Math.round(v + (B[i] - v) * t).toString(16).padStart(2, '0')).join('');
  };

  /* ---------- Hydra's output icons (ui/ChannelIcons.cpp, 24 x 24) ---------- */
  const r_ = (x, y, w, h, r, c) => `<rect class="${c}" x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}"/>`;
  const c_ = (x, y, r, c) => `<circle class="${c}" cx="${x}" cy="${y}" r="${r}"/>`;
  const e_ = (x, y, rx, ry) => `<ellipse class="f" cx="${x}" cy="${y}" rx="${rx}" ry="${ry}"/>`;
  const p_ = (d, c = 'l') => `<path class="${c}" d="${d}"/>`;
  const ICONS = {
    speakers: r_(5.5, 2.5, 13, 19, 2.5, 'l') + c_(12, 15, 4.2, 'l') + c_(12, 7, 1.7, 'f'),
    headphones: p_('M4 13A8 8 0 0 1 20 13') + r_(2.5, 12, 5, 8.5, 1.6, 'f') + r_(16.5, 12, 5, 8.5, 1.6, 'f'),
    tv: r_(2.5, 7, 19, 13, 2, 'l') + p_('M8 2.5L12 7M16 2.5L12 7'),
    discord: p_('M6.5 6.5Q12 4.5 17.5 6.5Q21.5 11 21 17Q19 19 16 19.5L15 17.5Q12 18.3 9 17.5L8 19.5Q5 19 3 17Q2.5 11 6.5 6.5Z') + e_(9, 12.5, 1.7, 2) + e_(15, 12.5, 1.7, 2),
    obs: c_(12, 12, 10, 'l') + c_(12, 7.4, 2.6, 'f') + c_(15.98, 14.3, 2.6, 'f') + c_(8.02, 14.3, 2.6, 'f'),
    recording: c_(12, 12, 9.5, 'l') + c_(12, 12, 5, 'f'),
    stream: c_(12, 12, 2.2, 'f') + p_('M16.24 8.49A5.5 5.5 0 0 1 16.24 15.51M7.76 15.51A5.5 5.5 0 0 1 7.76 8.49M18.93 6.26A9 9 0 0 1 18.93 17.74M5.07 17.74A9 9 0 0 1 5.07 6.26')
  };
  const icon = (id, x, y, size, col) => `<g transform="translate(${f1(x)} ${f1(y)}) scale(${size / 24})" color="${col}">${ICONS[id] || ''}</g>`;

  /* ---------- controls ---------- */
  // Knob (Widgets.cpp Knob::paint): dark track arc, value arc in the accent, raised disc, white pointer.
  function knob(cx, cy, rad, norm, accent, o = {}) {
    const a0 = -135, a1 = 135, ang = a0 + norm * (a1 - a0);
    const start = o.bipolar ? 0 : a0;
    const pt = (a, r) => [cx + Math.sin(a * Math.PI / 180) * r, cy - Math.cos(a * Math.PI / 180) * r];
    const arc = (from, to, r) => {
      const [x1, y1] = pt(from, r), [x2, y2] = pt(to, r);
      return `M${f1(x1)} ${f1(y1)}A${r} ${r} 0 ${to - from > 180 ? 1 : 0} 1 ${f1(x2)} ${f1(y2)}`;
    };
    const ra = rad - 2, inner = rad - 7;
    let s = `<path d="${arc(a0, a1, ra)}" fill="none" stroke="${C.bg}" stroke-width="3.5" stroke-linecap="round"/>`;
    if (Math.abs(ang - start) > 1) s += `<path d="${arc(Math.min(start, ang), Math.max(start, ang), ra)}" fill="none" stroke="${accent}" stroke-width="3.5" stroke-linecap="round"/>`;
    s += `<circle cx="${cx}" cy="${cy}" r="${inner}" fill="${C.raised}"/>`;
    const [px1, py1] = pt(ang, inner * 0.25), [px2, py2] = pt(ang, inner - 2);
    s += L(px1, py1, px2, py2, C.text, 2, 'stroke-linecap="round"');
    if (o.label) s += T(cx, cy + rad + 13, o.label, { size: 8.5, fill: C.dim, anchor: 'middle', ls: '.04em' });
    if (o.value) s += T(cx, cy + rad + (o.label ? 25 : 13), o.value, { size: 9, fill: C.dim, anchor: 'middle' });
    return s;
  }
  const toggle = (x, y, col, on = true) => R(x, y, 30, 16, 8, on ? col : C.raised) + `<circle cx="${on ? x + 22 : x + 8}" cy="${y + 8}" r="5.5" fill="${C.bg}"/>`;
  const trash = (x, y) => R(x, y, 20, 18, 4, C.raised) +
    `<g fill="none" stroke="${C.dim}" stroke-width="1.2" stroke-linecap="round"><path d="M${x + 6} ${y + 5.5}h8M${x + 8.5} ${y + 5.5}v-1.3h3v1.3M${x + 7} ${y + 6.5}l.6 7h4.8l.6-7"/></g>`;
  const grip = (x, y) => [0, 1].map(i => [0, 1, 2].map(j => `<circle cx="${x + i * 4}" cy="${y + j * 4}" r="1" fill="${C.faint}"/>`).join('')).join('');
  const button = (x, y, w, h, label, o = {}) => R(x, y, w, h, 4, o.fill || C.raised) +
    T(x + w / 2, y + h / 2 + 3.4, label, { size: o.size || 8.5, fill: o.color || C.dim, anchor: 'middle', weight: 700, ls: '.04em' });

  /* ---------- strips (Strips.cpp) ---------- */
  const SW = 108;
  const SKEW = Math.log(0.5) / Math.log(45 / 72);
  const dbPos = db => db <= -60 ? 0 : Math.pow(Math.min(1, (db + 60) / 72), SKEW);
  const routeBtn = (x, y, id, on, col) => R(x, y, 29, 21, 5, on ? mix(C.raised, col, 0.75) : C.raised, on ? { stroke: mix(col, '#ffffff', 0.25) } : {}) +
    icon(id, x + 7.5, y + 3.5, 14, on ? '#ffffff' : C.dim);
  const chipRow = (x, y, w, text) => T(x + 10, y + 13, text, { size: 10.5 }) +
    `<path d="M${x + w - 19} ${y + 8}h8l-4 5z" fill="${C.dim}"/>` + L(x + 8, y + 22, x + w - 8, y + 22, C.outline);

  /** Head of an input strip: number, name, source, gain/pan, routing buttons (3 per row, H then V). */
  function inputHead(x, y, s, outs, o = {}) {
    let g = R(x, y, SW, 190, 0, C.panel);
    g += R(x + 8, y + 8, 22, 22, 5, mix(C.panel, C.in, 0.22)) + T(x + 19, y + 23, s.n, { size: 11.5, fill: C.in, anchor: 'middle', weight: 700 });
    g += T(x + 38, y + 23.5, s.name, { size: 12 });
    g += chipRow(x, y + 36, SW, s.src);
    g += knob(x + 30, y + 84, 17, s.gainN ?? 0.5, C.in, { label: 'GAIN', bipolar: true });
    g += knob(x + 78, y + 84, 17, s.pan ?? 0.5, C.in, { label: 'PAN', bipolar: true });
    g += L(x + 8, y + 124, x + SW - 8, y + 124, C.outline);
    outs.forEach((b, i) => {
      const row = b.hw ? 0 : 1, col = outs.filter(q => q.hw === b.hw).indexOf(b);
      g += routeBtn(x + 9 + col * 31, y + 131 + row * 29, b.icon, s.routes.includes(i), b.hw ? C.hw : C.virt);
    });
    g += L(x + 8, y + 186, x + SW - 8, y + 186, C.outline);
    return g;
  }
  /** Head of an output strip: icon badge, name, device. */
  function outputHead(x, y, b) {
    const col = b.hw ? C.hw : C.virt;
    return R(x, y, SW, 66, 0, C.panel) +
      R(x + 8, y + 8, 22, 22, 5, mix(C.panel, col, 0.25)) + icon(b.icon, x + 12, y + 12, 14, col) +
      T(x + 38, y + 23.5, b.name, { size: 12 }) + chipRow(x, y + 36, SW, b.dev);
  }
  /** Fader with scale and meters (Strips.cpp), from y0 to y1. */
  function faderBlock(x, y0, y1, db, lvl, col, muted) {
    let g = '';
    const yy = d => y1 - dbPos(d) * (y1 - y0);
    [[12, '12'], [6, '6'], [0, '0'], [-6, '-6'], [-12, '-12'], [-24, '-24'], [-40, '-40'], [-60, '-inf']].forEach(([d, t]) => {
      g += T(x + 22, yy(d) + 3, t, { size: 7.5, fill: C.faint, anchor: 'end' }) + L(x + 24, yy(d), x + 28, yy(d), C.faint, 0.8);
    });
    g += R(x + 38, y0, 4, y1 - y0, 2, '#050608');
    g += R(x + 38, yy(db), 4, y1 - yy(db), 2, muted ? C.faint : col, { op: muted ? 0.5 : 0.95 });
    [0, 1].forEach(k => {
      const mx = x + 58 + k * 9, l = lvl - k * 0.8;
      g += R(mx, y0, 6, y1 - y0, 1.5, '#050608');
      if (l > -60) {
        const top = yy(l);
        g += R(mx, top, 6, y1 - top, 1.5, muted ? '#3a404c' : C.good);
        g += L(mx, top - 3, mx + 6, top - 3, '#c9ced6', 1);
      }
    });
    const cy = yy(db);
    g += `<rect x="${x + 25}" y="${f1(cy - 7)}" width="30" height="14" rx="3" fill="url(#cap)"/>` + L(x + 28, cy, x + 52, cy, col, 1.6);
    return g;
  }
  const capGrad = `<defs><linearGradient id="cap" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f1f3f6"/><stop offset="1" stop-color="#9da4ae"/></linearGradient></defs>`;
  function busStrip(x, y, b, o) {
    const col = b.hw ? C.hw : C.virt, H = o.h || 420;
    let g = outputHead(x, y, b) + R(x, y + 66, SW, H - 66, 0, C.panel);
    g += button(x + 10, y + 72, SW - 20, 18, 'SEL', o.sel ? { fill: col, color: C.bg } : {});
    g += faderBlock(x, y + 108, y + H - 74, b.db, b.lvl, col, false);
    g += T(x + SW / 2, y + H - 56, (b.db > 0 ? '+' : '') + b.db.toFixed(1) + ' dB', { size: 10, anchor: 'middle' });
    g += button(x + 10, y + H - 46, SW - 20, 16, 'MONO');
    g += button(x + 10, y + H - 26, SW - 20, 18, 'MUTE');
    return g;
  }

  /* ---------- example layout (tools/Shots.cpp) ---------- */
  const OUTS = [
    { name: 'Speakers', icon: 'speakers', dev: 'Master 1/2', hw: true, db: 0, lvl: -9 },
    { name: 'Headphones', icon: 'headphones', dev: 'Master 3/4', hw: true, db: -3, lvl: -12 },
    { name: 'TV', icon: 'tv', dev: 'Samsung TV ...', hw: true, db: -6, lvl: -17 },
    { name: 'Stream', icon: 'obs', dev: 'Hydra Virtual 1', hw: false, db: 0, lvl: -10 },
    { name: 'Discord', icon: 'discord', dev: 'Hydra Virtual 2', hw: false, db: 0, lvl: -14 },
    { name: 'Recording', icon: 'recording', dev: 'Hydra Virtual 3', hw: false, db: -1.5, lvl: -12 }
  ];
  const INS = [
    { n: 1, name: 'Mic', src: 'Master 1/2', routes: [0, 3] },
    { n: 2, name: 'Headset', src: 'Headset Mic...', routes: [1, 3], gainN: 0.48 },
    { n: 3, name: 'Guitar', src: 'Master 3/4', routes: [0, 1], gainN: 0.45, pan: 0.38 },
    { n: 4, name: 'Chrome', src: 'App: chrome', routes: [0, 1] },
    { n: 5, name: 'Discord', src: 'App: Discord', routes: [1, 4] },
    { n: 6, name: 'Game', src: 'App: Game', routes: [0, 1, 3], gainN: 0.47 },
    { n: 7, name: 'Spotify', src: 'App: Spotify', routes: [0, 1, 3], gainN: 0.42 },
    { n: 8, name: 'Phone', src: 'Hydra Link Ph...', routes: [2], muted: true }
  ];
  const pane = (x, y, w, h, col) => R(x, y, w, h, 8, C.panel) + `<path d="M${x + 8} ${y}h${w - 16}" stroke="${col}" stroke-width="2.5" stroke-linecap="round"/>`;

  /* ---------- graphics ---------- */
  const G = {};

  /* ---------- animation helper: runs frame(t) only while the graphic is on screen ---------- */
  const reduce = matchMedia('(prefers-reduced-motion: reduce)');
  function whileVisible(host, frame, fps = 30) {
    let vis = false, raf = 0, last = 0;
    const loop = ts => {
      raf = 0;
      if (!host.isConnected) { vis = false; io.disconnect(); return; }
      if (ts - last >= 1000 / fps - 2) { last = ts; frame(ts / 1000); }
      if (vis) raf = requestAnimationFrame(loop);
    };
    const io = new IntersectionObserver(es => {
      vis = es[es.length - 1].isIntersecting && !reduce.matches;
      host.classList.toggle('run', vis);
      if (vis && !raf) raf = requestAnimationFrame(loop);
    });
    io.observe(host);
    frame(1.3);
  }
  // Deterministic "audio": level in dBFS of a source kind at time t (s).
  function srcLevel(kind, t, seed) {
    const s = Math.sin, k = seed * 1.7;
    if (kind === 'voice') { const env = Math.abs(s(t * 2.9 + k) * s(t * 1.13 + k * 2)); return env < 0.18 ? -60 : -30 + 22 * env + 2 * s(t * 23 + k); }
    if (kind === 'music') return -15 + 2.5 * s(t * 6.3 + k) * s(t * 1.7) + 1.5 * Math.abs(s(t * 9.1 + k));
    if (kind === 'game') return -19 + 5 * Math.abs(s(t * 0.9 + k)) + 2.5 * s(t * 13 + k) * Math.abs(s(t * 0.37));
    if (kind === 'chat') { const env = Math.abs(s(t * 2.2 + 2 + k) * s(t * 0.41 + k)); return env < 0.3 ? -60 : -32 + 18 * env + 2 * s(t * 19); }
    return -60;
  }
  const dbSum = list => { const p = list.reduce((a, d) => a + (d > -60 ? Math.pow(10, d / 10) : 0), 0); return p > 0 ? 10 * Math.log10(p) : -60; };

  // How it works: four input strips and four output strips like in Hydra. Click routing buttons / MUTE, drag faders.
  G.routing = function (host) {
    const ins = [
      { n: 1, name: 'Mic', src: 'Master 1/2', kind: 'voice', db: 0, routes: new Set([2, 3]), muted: false, gainN: 0.5 },
      { n: 2, name: 'Game', src: 'App: Game', kind: 'game', db: -4, routes: new Set([1, 2]), muted: false, gainN: 0.47 },
      { n: 3, name: 'Spotify', src: 'App: Spotify', kind: 'music', db: -9, routes: new Set([0, 2]), muted: false, gainN: 0.42 },
      { n: 4, name: 'Discord', src: 'App: Discord', kind: 'chat', db: 0, routes: new Set([1, 3]), muted: false }
    ];
    const outs = [
      { name: 'Speakers', icon: 'speakers', dev: 'Master 1/2', hw: true, db: 0, muted: false },
      { name: 'Headphones', icon: 'headphones', dev: 'Master 3/4', hw: true, db: -3, muted: false },
      { name: 'Stream', icon: 'obs', dev: 'Hydra Virtual 1', hw: false, db: 0, muted: false },
      { name: 'Recording', icon: 'recording', dev: 'Hydra Virtual 2', hw: false, db: -1.5, muted: false }
    ];
    const HS = 440, TOP = 150, PAD = 14, GAP = 18;
    const ix = i => PAD + i * SW;
    const ox = o => PAD + 4 * SW + GAP + o * SW + (o >= 2 ? GAP : 0);
    const W = ox(3) + SW + PAD, H = TOP + HS + PAD;
    const fy0i = TOP + 206, fy1 = TOP + HS - 74, fy0o = TOP + 104;
    const colOf = o => outs[o].hw ? C.hw : C.virt;
    const btnX = (i, o) => ix(i) + 9 + (o % 2) * 31 + 14.5, btnY = o => TOP + 131 + (o >= 2 ? 29 : 0);
    const portX = (i, o) => ox(o) + 22 + i * 21;
    const level = { i: ins.map(() => -60), o: outs.map(() => -60) };
    let hover = null, fresh = null;

    const wirePath = (i, o) => {
      const x1 = btnX(i, o), x2 = portX(i, o), h = 14 + 0.16 * Math.abs(x2 - x1);
      return `M${f1(x1)} ${TOP}C${f1(x1)} ${f1(TOP - h)} ${f1(x2)} ${f1(TOP - h)} ${f1(x2)} ${TOP}`;
    };
    const meterIds = (key, x, y0, y1) => [0, 1].map(k => {
      const mx = x + 58 + k * 9;
      return R(mx, y0, 6, y1 - y0, 1.5, '#050608') + `<rect id="${key}m${k}" x="${mx}" y="${y1}" width="6" height="0" rx="1.5" fill="${C.good}"/>` +
        `<rect id="${key}p${k}" x="${mx}" y="${y1}" width="6" height="1" fill="#c9ced6" opacity="0"/>`;
    }).join('');
    function fader(key, x, y0, y1, db, col, muted) {
      const yy = d => y1 - dbPos(d) * (y1 - y0);
      let g = `<g class="fd" data-k="${key}" data-y0="${y0}" data-y1="${y1}">` + R(x + 4, y0 - 8, 52, y1 - y0 + 16, 0, 'transparent');
      [[12, '12'], [6, '6'], [0, '0'], [-6, '-6'], [-12, '-12'], [-24, '-24'], [-40, '-40'], [-60, '-inf']].forEach(([d, t]) => {
        g += T(x + 22, yy(d) + 3, t, { size: 7.5, fill: C.faint, anchor: 'end' }) + L(x + 24, yy(d), x + 28, yy(d), C.faint, 0.8);
      });
      g += R(x + 38, y0, 4, y1 - y0, 2, '#050608') + R(x + 38, yy(db), 4, y1 - yy(db), 2, muted ? C.faint : col, { op: muted ? 0.5 : 0.95 });
      const cy = yy(db);
      g += `<rect x="${x + 25}" y="${f1(cy - 7)}" width="30" height="14" rx="3" fill="url(#cap)"/>` + L(x + 28, cy, x + 52, cy, col, 1.6) + '</g>';
      return g + meterIds(key, x, y0, y1);
    }
    const muteBtn = (key, x, y, w, on) => `<g class="mb" data-k="${key}">` +
      button(x, y, w, 18, 'MUTE', on ? { fill: C.mute, color: '#ffffff' } : {}) + '</g>';
    const dbText = (x, y, db) => T(x + SW / 2, y, (db > 0 ? '+' : '') + (db <= -60 ? '-inf' : db.toFixed(1)) + ' dB', { size: 10, anchor: 'middle' });

    function render() {
      let body = capGrad + `<rect width="${W}" height="${H}" fill="${C.bg}"/>`;
      body += T(ix(0), 26, 'Inputs', { size: 12.5, fill: C.in, weight: 700 }) +
        T(ox(0), 26, 'Hardware Outputs', { size: 12.5, fill: C.hw, weight: 700 }) + T(ox(2), 26, 'Virtual Outputs', { size: 12.5, fill: C.virt, weight: 700 });
      // wires (behind the strips): one base line + one animated "flow" line each
      let wires = '';
      ins.forEach((s, i) => s.routes.forEach(o => {
        const d = wirePath(i, o), m = s.muted || outs[o].muted ? ' muted' : '', nw = fresh === i + ':' + o ? ' new' : '';
        wires += `<path class="w${m}${nw}" data-i="${i}" data-o="${o}" d="${d}" stroke="${colOf(o)}"/>` +
          `<path class="w flow${m}" data-i="${i}" data-o="${o}" d="${d}" stroke="${mix(colOf(o), '#ffffff', 0.35)}" id="f${i}_${o}"/>`;
      }));
      body += `<g style="pointer-events:none">${wires}</g>`;
      body += pane(ix(0) - 4, TOP - 4, 4 * SW + 8, HS + 8, C.in) + pane(ox(0) - 4, TOP - 4, 2 * SW + 8, HS + 8, C.hw) + pane(ox(2) - 4, TOP - 4, 2 * SW + 8, HS + 8, C.virt);
      ins.forEach((s, i) => {
        const x = ix(i), key = 'i' + i;
        let g = `<g data-si="${key}">` + R(x, TOP, SW, HS, 0, C.panel);
        s.routes.forEach(o => { g += L(btnX(i, o), TOP, btnX(i, o), btnY(o), colOf(o), 1.4, `opacity="${s.muted ? 0.12 : 0.3}"`); });
        g += R(x + 8, TOP + 8, 22, 22, 5, mix(C.panel, C.in, 0.22)) + T(x + 19, TOP + 23, s.n, { size: 11.5, fill: C.in, anchor: 'middle', weight: 700 });
        g += T(x + 38, TOP + 23.5, s.name, { size: 12 }) + chipRow(x, TOP + 36, SW, s.src);
        g += knob(x + 30, TOP + 84, 17, s.gainN ?? 0.5, C.in, { label: 'GAIN', bipolar: true }) + knob(x + 78, TOP + 84, 17, 0.5, C.in, { label: 'PAN', bipolar: true });
        g += L(x + 8, TOP + 124, x + SW - 8, TOP + 124, C.outline);
        outs.forEach((b, o) => {
          g += `<g class="rb" data-i="${i}" data-o="${o}"><title>${esc(s.name + ' → ' + b.name)}</title>` +
            routeBtn(x + 9 + (o % 2) * 31, btnY(o), b.icon, s.routes.has(o), colOf(o)) + '</g>';
        });
        g += L(x + 8, TOP + 188, x + SW - 8, TOP + 188, C.outline);
        g += fader(key, x, fy0i, fy1, s.db, C.in, s.muted) + dbText(x, TOP + HS - 56, s.db);
        g += button(x + 10, TOP + HS - 46, 42, 16, 'MONO') + button(x + 56, TOP + HS - 46, 42, 16, 'SOLO') + muteBtn(key, x + 10, TOP + HS - 26, SW - 20, s.muted);
        if (i) g += L(x, TOP + 4, x, TOP + HS - 4, C.outline);
        body += g + '</g>';
      });
      outs.forEach((b, o) => {
        const x = ox(o), key = 'o' + o, col = colOf(o);
        let g = `<g data-si="${key}">` + outputHead(x, TOP, b) + R(x, TOP + 66, SW, HS - 66, 0, C.panel);
        ins.forEach((s, i) => { if (s.routes.has(o)) g += `<circle cx="${f1(portX(i, o))}" cy="${TOP + 1.5}" r="2.5" fill="${col}"/>`; });
        g += button(x + 10, TOP + 72, SW - 20, 18, 'SEL');
        g += fader(key, x, fy0o, fy1, b.db, col, b.muted) + dbText(x, TOP + HS - 56, b.db);
        g += button(x + 10, TOP + HS - 46, SW - 20, 16, 'MONO') + muteBtn(key, x + 10, TOP + HS - 26, SW - 20, b.muted);
        if (o % 2) g += L(x, TOP + 4, x, TOP + HS - 4, C.outline);
        body += g + '</g>';
      });
      host.innerHTML = svg(W, H, body, 'Interactive routing: four input strips feed hardware and virtual outputs; click the routing buttons to change it');
      fresh = null;
      applyHover();
      step(lastT);
    }
    function applyHover() {
      const el = host.querySelector('svg');
      if (!el) return;
      el.classList.toggle('focus', !!hover);
      el.querySelectorAll('path.w').forEach(p => p.classList.toggle('on', !!hover && (hover[0] === 'i' ? p.dataset.i === hover.slice(1) : p.dataset.o === hover.slice(1))));
    }
    const peaks = {};
    function setMeter(key, y0, db, muted) {
      [0, 1].forEach(k => {
        const m = document.getElementById(key + 'm' + k), p = document.getElementById(key + 'p' + k);
        if (!m) return;
        const l = db - k * 0.8, top = l > -60 ? fy1 - dbPos(l) * (fy1 - y0) : fy1;
        m.setAttribute('y', f1(top)); m.setAttribute('height', f1(fy1 - top));
        m.setAttribute('fill', muted ? '#3a404c' : C.good);
        const pk = Math.min((peaks[key + k] ?? fy1) + 0.6, fy1, top);
        peaks[key + k] = pk;
        p.setAttribute('y', f1(pk - 3)); p.setAttribute('opacity', pk < fy1 - 1 ? 1 : 0);
      });
    }
    let lastT = 1.3;
    function step(t) {
      lastT = t;
      ins.forEach((s, i) => { level.i[i] = srcLevel(s.kind, t, i); });
      const post = ins.map((s, i) => s.db <= -60 ? -60 : level.i[i] + s.db);
      outs.forEach((b, o) => {
        const sum = dbSum(ins.map((s, i) => s.routes.has(o) && !s.muted ? post[i] : -60));
        level.o[o] = sum <= -60 || b.db <= -60 ? -60 : sum + b.db;
      });
      ins.forEach((s, i) => setMeter('i' + i, fy0i, post[i], s.muted));
      outs.forEach((b, o) => setMeter('o' + o, fy0o, level.o[o], b.muted));
      // flow lines are lit only while their input carries sound
      ins.forEach((s, i) => s.routes.forEach(o => {
        const f = document.getElementById(`f${i}_${o}`);
        if (f) f.style.opacity = post[i] > -50 ? '' : '0';
      }));
    }

    // interaction (delegated, survives re-renders)
    const keyOf = k => ({ list: k[0] === 'i' ? ins : outs, idx: +k.slice(1) });
    let drag = null;
    host.addEventListener('pointerover', e => { const g = e.target.closest('[data-si]'); const h = g ? g.dataset.si : null; if (h !== hover && !drag) { hover = h; applyHover(); } });
    host.addEventListener('pointerleave', () => { hover = null; applyHover(); });
    const svgY = e => { const el = host.querySelector('svg'), pt = el.createSVGPoint(); pt.x = e.clientX; pt.y = e.clientY; return pt.matrixTransform(el.getScreenCTM().inverse()).y; };
    const dbAt = (y, y0, y1) => {
      const n = Math.max(0, Math.min(1, (y1 - y) / (y1 - y0)));
      if (n <= 0.002) return -60;
      return Math.round(Math.max(-60, Math.min(12, Math.pow(n, 1 / SKEW) * 72 - 60)) * 2) / 2;
    };
    host.addEventListener('pointerdown', e => {
      const rb = e.target.closest('.rb'), mb = e.target.closest('.mb'), fd = e.target.closest('.fd');
      if (rb) {
        const s = ins[+rb.dataset.i], o = +rb.dataset.o;
        if (s.routes.has(o)) s.routes.delete(o); else { s.routes.add(o); fresh = rb.dataset.i + ':' + o; }
        render();
      } else if (mb) {
        const { list, idx } = keyOf(mb.dataset.k); list[idx].muted = !list[idx].muted; render();
      } else if (fd) {
        drag = { k: fd.dataset.k, y0: +fd.dataset.y0, y1: +fd.dataset.y1, raf: 0 };
        host.setPointerCapture(e.pointerId);
        e.preventDefault();
        move(e);
      }
    });
    function move(e) {
      if (!drag) return;
      const db = dbAt(svgY(e), drag.y0, drag.y1), { list, idx } = keyOf(drag.k);
      if (db === list[idx].db) return;
      list[idx].db = db;
      if (!drag.raf) drag.raf = requestAnimationFrame(() => { if (drag) drag.raf = 0; render(); });
    }
    host.addEventListener('pointermove', move);
    const end = () => { drag = null; };
    host.addEventListener('pointerup', end); host.addEventListener('pointercancel', end);
    host.addEventListener('dblclick', e => {   // double click on a fader = 0 dB, like in Hydra
      const fd = e.target.closest('.fd'); if (!fd) return;
      const { list, idx } = keyOf(fd.dataset.k); list[idx].db = 0; render();
    });
    render();
    whileVisible(host, step);
  };

  // Strip heads of some inputs (data-o = indices) or outputs (data-o="out:3,4,5").
  G.heads = function (host, opt) {
    if (opt.startsWith('out:')) {
      const list = opt.slice(4).split(',').map(Number);
      let b = '';
      list.forEach((o, k) => { b += outputHead(k * SW, 0, OUTS[o]) + (k ? L(k * SW, 4, k * SW, 62, C.outline) : ''); });
      host.innerHTML = svg(SW * list.length, 66, R(0, 0, SW * list.length, 66, 8, C.panel) + b, 'Output strips of Hydra');
      return;
    }
    const list = opt.split(',').map(Number);
    let b = '';
    list.forEach((i, k) => { b += inputHead(k * SW, 0, INS[i], OUTS) + (k ? L(k * SW, 4, k * SW, 186, C.outline) : ''); });
    host.innerHTML = svg(SW * list.length, 190, b, 'Input strips of Hydra: ' + list.map(i => INS[i].src).join(', '));
  };

  // Output strips with SEL (separate mix per output).
  G.sel = function (host) {
    const H = 400;
    let b = capGrad + R(0, 0, SW * 4 + 24, H + 12, 8, C.bg);
    b += pane(6, 6, SW * 2, H, C.hw) + pane(SW * 2 + 18, 6, SW * 2, H, C.virt);
    b += busStrip(6, 6, OUTS[0], { h: H }) + busStrip(6 + SW, 6, OUTS[1], { h: H, sel: true }) + L(6 + SW, 10, 6 + SW, H, C.outline);
    b += busStrip(SW * 2 + 18, 6, OUTS[3], { h: H }) + busStrip(SW * 3 + 18, 6, OUTS[4], { h: H }) + L(SW * 3 + 18, 10, SW * 3 + 18, H, C.outline);
    host.innerHTML = svg(SW * 4 + 24, H + 12, b, 'Output strips with SEL: Headphones selected');
  };

  // Top bar status (MainComponent TopBar): logo, device status, DSP meter.
  G.status = function (host) {
    const W = 760, H = 46;
    let b = R(0, 0, W, H, 8, C.bg);
    [[8, 14, 6, 18], [16, 9, 6, 23], [24, 15, 6, 17]].forEach(([x, y, w, h], i) => { b += R(x + 6, y, w, h, 3, ['#4bc0f5', '#6c8cfa', '#8f7cff'][i]); });
    b += T(46, 28, 'HYDRA', { size: 13, weight: 700, ls: '.06em' }) + T(98, 28, 'MIXER', { size: 13, fill: C.dim, ls: '.06em' });
    b += R(160, 8, 590, 30, 7, C.panel) + `<circle cx="176" cy="23" r="4" fill="${C.good}"/>`;
    b += T(188, 27, 'ASIO  -  Focusrite USB ASIO  -  48000 Hz  -  64 smp  -  10.5 ms I/O', { size: 10.5 });
    b += T(600, 27, 'DSP', { size: 9.5, fill: C.dim }) + R(624, 20, 80, 6, 3, '#050608') + R(624, 20, 4, 6, 3, C.good) + T(740, 27, '3.4%', { size: 9.5, anchor: 'end' });
    host.innerHTML = svg(W, H, b, 'Hydra status: ASIO, 48000 Hz, 64 samples, 10.5 ms I/O, DSP 3.4 %');
  };

  /* EQ maths: RBJ biquads, magnitude in dB (ParamEq.h types: Bell, Notch, Band Pass, High Cut, Low Cut, Low Shelf, High Shelf) */
  function bandDb(type, g, f0, q, f) {
    const fs = 48000, w0 = 2 * Math.PI * f0 / fs, cs = Math.cos(w0), sn = Math.sin(w0), al = sn / (2 * q), A = Math.pow(10, g / 40);
    let b0, b1, b2, a0, a1, a2;
    if (type === 'bell') { b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; }
    else if (type === 'lowcut') { b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
    else if (type === 'highcut') { b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; a0 = 1 + al; a1 = -2 * cs; a2 = 1 - al; }
    else {   // shelves
      const sq = 2 * Math.sqrt(A) * al, hi = type === 'highshelf' ? -1 : 1;
      b0 = A * ((A + 1) - hi * (A - 1) * cs + sq); b1 = 2 * hi * A * ((A - 1) - hi * (A + 1) * cs); b2 = A * ((A + 1) - hi * (A - 1) * cs - sq);
      a0 = (A + 1) + hi * (A - 1) * cs + sq; a1 = -2 * hi * ((A - 1) + hi * (A + 1) * cs); a2 = (A + 1) + hi * (A - 1) * cs - sq;
    }
    const w = 2 * Math.PI * f / fs, c1 = Math.cos(w), s1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
    const nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
  }
  const MIC_EQ = [
    { type: 'lowcut', g: 0, f: 90, q: 0.7, t: 'Low Cut' }, { type: 'bell', g: -3, f: 320, q: 1.2, t: 'Bell' },
    { type: 'bell', g: 2.5, f: 3200, q: 0.9, t: 'Bell' }, { type: 'highshelf', g: 1.5, f: 9000, q: 0.7, t: 'High Shelf' }
  ];
  const fmtHz = f => f >= 1000 ? (f / 1000).toFixed(2).replace(/\.?0+$/, '') + ' kHz' : f.toFixed(1) + ' Hz';
  /** EQ graph: grid, one coloured area per band, the summed curve and the band points. */
  function eqGraph(x, y, w, h, bands, o = {}) {
    const lo = 20, hi = 20000, range = 18;
    const fx = f => x + w * Math.log(f / lo) / Math.log(hi / lo);
    const fy = d => y + h / 2 - d / range * (h / 2);
    let g = R(x, y, w, h, 4, '#0c0f14');
    [50, 100, 200, 500, 1000, 2000, 5000, 10000].forEach(f => {
      g += L(fx(f), y, fx(f), y + h, '#1c222c');
      if (o.labels !== false) g += T(fx(f), y + h - 4, f >= 1000 ? f / 1000 + 'k' : f, { size: o.small ? 7 : 9, fill: C.faint, anchor: 'middle' });
    });
    [-12, -6, 6, 12].forEach(d => { g += L(x, fy(d), x + w, fy(d), '#1c222c'); });
    if (o.dbLabels) [18, 12, 6, 0, -6, -12, -18].forEach(d => { g += T(x - 6, fy(d) + 3, (d > 0 ? '+' : '') + d, { size: 9, fill: C.faint, anchor: 'end' }); });
    g += L(x, fy(0), x + w, fy(0), '#2a3240');
    if (o.spec) {   // live spectrum after the EQ (Inspector.cpp: accent gradient + thin line, own dB scale on the right)
      g += `<defs><linearGradient id="spg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${C.in}" stop-opacity=".30"/><stop offset="1" stop-color="${C.in}" stop-opacity=".03"/></linearGradient></defs>`;
      g += `<path class="spec" d="" fill="url(#spg)" stroke="${C.in}" stroke-opacity=".5" stroke-width="1.2"/>`;
      [-20, -40, -60, -80].forEach(d => { g += T(x + w + 5, y + h * (-d / 90) + 3, d, { size: 8, fill: C.faint }); });
    }
    const N = 160, fs = [...Array(N + 1)].map((_, i) => lo * Math.pow(hi / lo, i / N));
    bands.forEach((b, k) => {
      const pts = fs.map(f => [fx(f), fy(Math.max(-range, Math.min(range, bandDb(b.type, b.g, b.f, b.q, f))))]);
      g += `<path d="M${f1(x)} ${f1(fy(0))}${pts.map(p => `L${f1(p[0])} ${f1(p[1])}`).join('')}L${f1(x + w)} ${f1(fy(0))}Z" fill="${C.bands[k]}" opacity=".16"/>`;
    });
    const sum = fs.map(f => [fx(f), fy(Math.max(-range - 2, Math.min(range, bands.reduce((a, b) => a + bandDb(b.type, b.g, b.f, b.q, f), 0))))]);
    g += `<clipPath id="eqc${x}${y}"><rect x="${x}" y="${y}" width="${w}" height="${h}" rx="4"/></clipPath>`;
    g += `<path clip-path="url(#eqc${x}${y})" d="M${sum.map(p => `${f1(p[0])} ${f1(p[1])}`).join('L')}" fill="none" stroke="#e8ebf1" stroke-width="${o.small ? 1.6 : 2}"/>`;
    bands.forEach((b, k) => {
      const px = fx(b.f), py = fy(b.type.endsWith('cut') ? 0 : b.g), r = o.small ? 5.5 : 8;
      if (k === 0 && o.sel) g += `<circle cx="${f1(px)}" cy="${f1(py)}" r="${r + 4}" fill="${C.bands[0]}" opacity=".3"/>`;
      g += `<circle cx="${f1(px)}" cy="${f1(py)}" r="${r}" fill="${C.bands[k]}" stroke="${k === 0 && o.sel ? '#fff' : 'none'}" stroke-width="1.2"/>` +
        T(px, py + (o.small ? 2.6 : 3.4), k + 1, { size: o.small ? 7 : 9, fill: C.bg, anchor: 'middle', weight: 700 });
    });
    return g;
  }
  // Filter type glyphs (the column of type buttons in the EQ block).
  const TYPES = ['M2 9Q7 1 12 9', 'M2 4H5Q7 4 7 9Q7 4 9 4H12', 'M2 9Q7 -1 12 9', 'M2 4H7Q9 4 12 9', 'M2 9Q5 4 7 4H12', 'M2 9H5Q7 9 9 4H12', 'M2 9Q5 9 7 4H12'];
  function typeColumn(x, y, active, col, h = 19) {
    let g = '';
    TYPES.forEach((d, i) => {
      const on = i === active;
      g += R(x, y + i * (h + 2), 26, h, 4, on ? mix(C.raised, col, 0.25) : C.raised, on ? { stroke: col } : {}) +
        `<path transform="translate(${x + 6} ${y + i * (h + 2) + h / 2 - 6.5})" d="${d}" fill="none" stroke="${on ? col : C.dim}" stroke-width="1.3" stroke-linecap="round"/>`;
    });
    return g;
  }
  /** One effect block: card, coloured top line, grip, title, "..." / toggle / trash. */
  function block(x, y, w, h, title, col, o = {}) {
    let g = R(x, y, w, h, 7, C.panel2) + `<path d="M${x + 7} ${y + 0.5}h${w - 14}" stroke="${col}" stroke-width="2.5" stroke-linecap="round"/>`;
    g += grip(x + 12, y + 14) + T(x + 24, y + 22, title, { size: 10, fill: col, weight: 700, ls: '.05em' });
    let bx = x + w - 10;
    if (o.trash !== false) { bx -= 20; g += trash(bx, y + 9); bx -= 6; }
    if (o.toggle !== false) { bx -= 30; g += toggle(bx, y + 10, col); bx -= 6; }
    if (o.dots) { bx -= 22; g += button(bx, y + 9, 20, 18, '...'); bx -= 6; }
    if (o.expand) { bx -= 20; g += `<path d="M${bx + 6} ${y + 22}l8-8m-4 0h4v4m-8 0v4h4" fill="none" stroke="${C.dim}" stroke-width="1.3"/>`; }
    return g;
  }

  // Inspector: effect chain of the mic, with a VST3 plugin among the built-in effects.
  G.chain = function (host) {
    const W = 1130, H = 252, y = 34, h = 206;
    let b = R(0, 0, W, H, 10, C.panel) + T(14, 21, 'IN 1', { size: 11, fill: C.in, weight: 700 }) + T(46, 21, 'Mic', { size: 11, weight: 600 }) + button(80, 8, 62, 18, 'BYPASS');
    let x = 10;
    b += block(x, y, 160, h, 'DENOISER', C.good) + knob(x + 80, y + 98, 26, 0.85, C.good, { label: 'AMOUNT' }) +
      T(x + 80, y + h - 12, 'Low (10 ms)', { size: 8.5, fill: C.faint, anchor: 'middle' });
    x += 168;
    b += block(x, y, 356, h, 'EQUALIZER', C.eq, { dots: true, expand: true }) + typeColumn(x + 12, y + 38, 4, C.bands[0], 20);
    b += eqGraph(x + 46, y + 38, 190, 154, MIC_EQ, { small: true, sel: true });
    b += knob(x + 264, y + 92, 18, 0.5, C.bands[0], { label: 'GAIN', value: '0.0 dB', bipolar: true }) +
      knob(x + 302, y + 92, 18, 0.25, C.bands[0], { label: 'FREQ', value: '90 Hz' }) + knob(x + 340 - 4, y + 92, 18, 0.3, C.bands[0], { label: 'Q', value: '0.7' });
    b += R(x + 248, y + 160, 82, 22, 5, C.bands[0]) + T(x + 289, y + 175, 'BAND 1 ON', { size: 9, fill: C.bg, anchor: 'middle', weight: 700 });
    x += 364;
    b += block(x, y, 150, h, 'GATE', C.gate, { dots: true }) + knob(x + 64, y + 104, 26, 0.3, C.gate, { label: 'THRESH' }) +
      R(x + 130, y + 40, 6, h - 64, 3, '#050608') + R(x + 130, y + 112, 6, h - 136, 3, C.gate) + T(x + 133, y + h - 10, 'OPEN', { size: 7.5, fill: C.faint, anchor: 'middle' });
    x += 158;
    b += R(x, y, 158, h, 7, C.panel2, { stroke: mix(C.panel2, C.in, 0.45) }) + grip(x + 12, y + 14) +
      T(x + 24, y + 22, 'VST3', { size: 10, fill: C.faint, weight: 700 }) + T(x + 24, y + 44, 'Your plugin', { size: 12.5, weight: 700 }) +
      T(x + 24, y + 60, 'any VST3 effect', { size: 10, fill: C.dim });
    b += `<g transform="translate(${x + 49} ${y + 86})"><rect width="60" height="40" rx="5" fill="${C.raised}"/>` +
      [10, 22, 34, 46].map((k, i) => `<rect x="${k}" y="${26 - [10, 18, 13, 7][i]}" width="5" height="${[10, 18, 13, 7][i]}" rx="1.5" fill="${C.in}" opacity=".8"/>`).join('') + `</g>`;
    b += button(x + 12, y + h - 32, 40, 22, 'ON', { fill: mix(C.raised, C.in, 0.35), color: C.text }) + button(x + 55, y + h - 32, 48, 22, 'EDIT', { color: C.text }) + trash(x + 126, y + h - 30);
    x += 166;
    b += block(x, y, 200, h, 'COMPRESSOR', C.comp, { dots: true }) + knob(x + 58, y + 98, 25, 0.62, C.comp, { label: 'THRESH' }) +
      knob(x + 142, y + 98, 25, 0.3, C.comp, { label: 'RATIO' }) + T(x + 12, y + h - 12, 'GR', { size: 8, fill: C.faint }) +
      R(x + 30, y + h - 20, 158, 8, 3, '#050608') + R(x + 30, y + h - 20, 26, 8, 3, C.comp, { op: 0.8 });
    x += 208;
    b += `<circle cx="${x + 18}" cy="${y + h / 2}" r="13" fill="${mix(C.panel, C.in, 0.2)}"/>` + L(x + 12, y + h / 2, x + 24, y + h / 2, C.in, 2) + L(x + 18, y + h / 2 - 6, x + 18, y + h / 2 + 6, C.in, 2);
    host.innerHTML = svg(W, H, b, 'Effect chain of the mic: denoiser, equalizer, gate, a VST3 plugin, compressor, and a plus button to add more');
  };

  // Large EQ window.
  G.eq = function (host) {
    const W = 900, H = 520;
    let b = R(0, 0, W, H, 10, C.bg) + T(18, 26, 'IN 1', { size: 12, fill: C.in, weight: 700 }) + T(52, 26, 'Mic', { size: 12, fill: C.in, weight: 700 });
    b += R(W - 220, 12, 96, 24, 5, C.raised) + T(W - 172, 28, 'EQ ...', { size: 9.5, anchor: 'middle', weight: 600 });
    b += R(W - 116, 12, 100, 24, 5, C.eq) + T(W - 66, 28, 'EQ ON', { size: 9.5, fill: C.bg, anchor: 'middle', weight: 700 });
    b += R(14, 46, W - 28, 286, 6, '#0d1015');
    const gx = 44, gy = 56, gw = W - 84, gh = 262;
    b += eqGraph(gx, gy, gw, gh, MIC_EQ, { dbLabels: true, sel: true, spec: true });
    b += T(W - 28, 52, 'dBFS', { size: 7.5, fill: C.faint, anchor: 'middle' });
    MIC_EQ.forEach((band, k) => {
      const x = 14 + k * 176, y = 344, col = C.bands[k];
      b += R(x, y, 168, 164, 7, C.panel2, k === 0 ? { stroke: col, sw: 1.4 } : { stroke: mix(C.panel2, col, 0.35) });
      b += `<circle cx="${x + 14}" cy="${y + 15}" r="4.5" fill="${col}"/>` + T(x + 25, y + 19, 'BAND ' + (k + 1), { size: 10, fill: col, weight: 700 });
      b += toggle(x + 108, y + 7, col) + trash(x + 142, y + 6);
      b += typeColumn(x + 8, y + 30, ['bell', 'notch', 'bp', 'highcut', 'lowcut', 'lowshelf', 'highshelf'].indexOf(band.type), col, 16);
      const gn = 0.5 + band.g / 36;
      b += knob(x + 58, y + 82, 15, gn, band.type.endsWith('cut') ? C.faint : col, { label: 'GAIN', value: band.g.toFixed(1) + ' dB', bipolar: true });
      b += knob(x + 98, y + 82, 15, Math.log(band.f / 20) / Math.log(1000), col, { label: 'FREQ', value: fmtHz(band.f) });
      b += knob(x + 138, y + 82, 15, Math.log(band.q / 0.1) / Math.log(1000), col, { label: 'Q', value: band.q.toFixed(1) });
    });
    b += R(14 + 4 * 176, 344, W - 28 - 4 * 176, 164, 7, C.panel2) + `<circle cx="${14 + 4 * 176 + 14}" cy="359" r="4.5" fill="${C.faint}"/>` +
      T(14 + 4 * 176 + 25, 363, 'BAND 5', { size: 10, fill: C.faint, weight: 700 }) + T(14 + 4 * 176 + (W - 28 - 4 * 176) / 2, 430, '+  Add band', { size: 10, fill: C.dim, anchor: 'middle' });
    host.innerHTML = svg(W, H, b, "Hydra's large EQ window: live spectrum, curve with four bands, one column of controls per band, a column to add a band");
    // Live spectrum of a voice after the EQ: harmonics of a moving pitch, formants, syllable envelope.
    const spec = host.querySelector('path.spec'), N = 150, lo = 20, hi = 20000;
    const fsx = [...Array(N + 1)].map((_, i) => lo * Math.pow(hi / lo, i / N));
    const eqDb = fsx.map(f => MIC_EQ.reduce((a, q) => a + bandDb(q.type, q.g, q.f, q.q, f), 0));
    whileVisible(host, t => {
      const f0 = 128 + 14 * Math.sin(t * 1.7), env = 0.35 + 0.65 * Math.abs(Math.sin(t * 2.4) * Math.sin(t * 0.9 + 1));
      const form = [520 + 140 * Math.sin(t * 1.1), 1500 + 400 * Math.sin(t * 0.7 + 2), 2700];
      const pts = fsx.map((f, i) => {
        const oct = Math.log2(f / 300);
        let d = -34 - 6 * oct * oct * (oct < 0 ? 1.6 : 0.55);
        form.forEach((fm, k) => { d += (7 - k * 2) / (1 + Math.pow(Math.log2(f / fm) * 3.2, 2)); });
        if (f < 4000) d += 7 * Math.pow(Math.abs(Math.cos(Math.PI * f / f0)), 6) - 4;   // harmonics
        d += 2.2 * Math.sin(i * 1.9 + t * 9) + 1.6 * Math.sin(i * 0.7 - t * 13);         // FFT jitter
        d = d - (1 - env) * 22 + eqDb[i];
        const yy = gy + gh * Math.min(1, Math.max(0, -d / 90));
        return f1(gx + gw * i / N) + ' ' + f1(yy);
      });
      spec.setAttribute('d', `M${gx} ${gy + gh}L${pts.join('L')}L${gx + gw} ${gy + gh}Z`);
    });
  };

  // Room correction: waterfall 3D (RoomPages.cpp paintWaterfall) of a synthetic room with a few modes.
  G.waterfall = function (host) {
    const W = 820, H = 430, lo = 20, hi = 400, top = 3, floorDb = -45, slices = 18, span = 300;
    const modes = [[38, 9, 3.5, 0.95], [56, 6, 4, 0.8], [87, 7.5, 5, 0.9], [124, 4, 6, 0.6], [171, 5, 9, 0.7], [243, 3, 12, 0.45]];
    const dips = [[72, -7, 5], [148, -6, 8], [310, -4, 20]];
    const lev = (f, t) => {
      let base = -6 + 2 * Math.sin(Math.log(f) * 5.3), dec = 0.32;
      modes.forEach(([fm, a, bw, ring]) => { const r = 1 / (1 + ((f - fm) / bw) ** 2); base += a * r; dec -= 0.27 * ring * r; });
      dips.forEach(([fd, a, bw]) => { base += a / (1 + ((f - fd) / bw) ** 2); });
      return base - Math.max(0.035, dec) * t - (t > 0 ? 3 : 0);
    };
    const area = { x: 58, y: 18, w: W - 58 - 74, h: H - 18 - 34 };
    const ox = 0.2 * area.w, oy = 0.36 * area.h;
    const front = { x: area.x, y: area.y + oy, w: area.w - ox, h: area.h - oy };
    const fx = f => front.x + front.w * Math.log(f / lo) / Math.log(hi / lo);
    const fy = d => front.y + front.h * (top - Math.max(floorDb, Math.min(top, d))) / (top - floorDb);
    const shift = s => { const d = 1 - s / (slices - 1); return [ox * d, -oy * d]; };
    const stops = ['#ff5f6d', '#ffa53d', '#ffd84a', '#3ee08f', '#35c8f4', '#5b7cff'];
    const tcol = t => { const x = Math.max(0, Math.min(1, t)) * (stops.length - 1), i = Math.min(stops.length - 2, Math.floor(x)); return mix(stops[i], stops[i + 1], x - i); };
    let b = R(0, 0, W, H, 10, C.panel);
    const back = shift(0);
    for (let d = 0; d >= floorDb; d -= 15) {
      b += L(front.x + back[0], fy(d) + back[1], front.x + front.w + back[0], fy(d) + back[1], C.outline, 1) + L(front.x, fy(d), front.x + back[0], fy(d) + back[1], C.outline, 0.8);
      b += T(front.x - 8, fy(d) + 3.5, d + ' dB', { size: 10, fill: C.faint, anchor: 'end' });
    }
    const fb = front.y + front.h;
    b += `<path d="M${front.x} ${fb}H${front.x + front.w}l${f1(back[0])} ${f1(back[1])}H${f1(front.x + back[0])}Z" fill="${C.panel2}"/>`;
    const N = 140, fs = [...Array(N + 1)].map((_, i) => lo * Math.pow(hi / lo, i / N));
    b += '<defs>';
    for (let s = 0; s < slices; s++) {
      const c = tcol(s / (slices - 1));
      b += `<linearGradient id="wf${s}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${mix('#000000', c, 0.62)}"/><stop offset="1" stop-color="${mix(C.panel2, c, 0.12)}"/></linearGradient>`;
    }
    b += '</defs>';
    for (let s = 0; s < slices; s++) {
      const [dx, dy] = shift(s), t = s / (slices - 1) * span, c = tcol(s / (slices - 1));
      const pts = fs.map(f => `${f1(fx(f) + dx)} ${f1(fy(lev(f, t)) + dy)}`);
      b += `<path d="M${f1(fx(lo) + dx)} ${f1(fb + dy)}L${pts.join('L')}L${f1(fx(hi) + dx)} ${f1(fb + dy)}Z" fill="url(#wf${s})"/>`;
      b += `<path d="M${pts.join('L')}" fill="none" stroke="${c}" stroke-width="${s === slices - 1 ? 1.6 : 1.1}" opacity=".95"/>`;
    }
    [20, 30, 50, 100, 200, 300].forEach(f => { b += L(fx(f), fb, fx(f), fb + 5, C.faint) + T(fx(f), fb + 17, f + ' Hz', { size: 10, fill: C.faint, anchor: 'middle' }); });
    for (let t = 0; t <= span; t += 50) {
      const d = 1 - t / span;
      b += T(front.x + front.w + ox * d + 8, fb - oy * d + 4, t + ' ms', { size: 10, fill: C.faint });
    }
    host.innerHTML = svg(W, H, b, 'Waterfall 3D of a room measurement: the room modes ring longer than the rest');
  };

  // Recorder (Inspector, "REC Recorder"): the recording card.
  G.recorder = function (host) {
    const W = 820, H = 236;
    let b = R(0, 0, W, H, 10, C.panel) + T(14, 21, 'REC', { size: 11, fill: C.mute, weight: 700 }) + T(48, 21, 'Recorder', { size: 11, weight: 600 });
    // Record source
    b += R(10, 32, 160, 194, 7, C.panel2) + `<path d="M17 32.5h146" stroke="${C.virt}" stroke-width="2.5" stroke-linecap="round"/>`;
    b += T(22, 54, 'RECORD SOURCE', { size: 9.5, fill: C.virt, weight: 700, ls: '.04em' }) + T(22, 76, 'VIRTUAL (B)', { size: 8, fill: C.virt, weight: 700 });
    b += button(22, 84, 38, 22, 'V1', { fill: C.virt, color: C.bg, size: 10 }) + button(64, 84, 38, 22, 'V2', { size: 10 }) + button(106, 84, 38, 22, 'V3', { size: 10 });
    b += T(22, 126, 'Records V1 after its fader', { size: 8.5, fill: C.dim }) + T(22, 138, 'into Hydra Recordings', { size: 8.5, fill: C.dim });
    b += button(22, 188, 64, 24, 'Folder...', { color: C.text, size: 9 }) + button(92, 188, 66, 24, 'WAV 24', { color: C.text, size: 9 });
    // Recording
    const x = 178, w = W - x - 10;
    b += R(x, 32, w, 194, 7, C.panel2) + `<path d="M${x + 7} 32.5h${w - 14}" stroke="${C.mute}" stroke-width="2.5" stroke-linecap="round"/>`;
    b += T(x + 12, 54, 'RECORDING', { size: 9.5, fill: C.mute, weight: 700, ls: '.04em' });
    const by = 64;
    b += R(x + 12, by, 40, 28, 5, mix(C.raised, C.mute, 0.3), { stroke: C.mute }) + `<circle cx="${x + 32}" cy="${by + 14}" r="7" fill="${C.mute}"/>`;
    b += R(x + 56, by, 40, 28, 5, C.raised) + `<path d="M${x + 70} ${by + 7}l13 7-13 7z" fill="${C.good}"/>`;
    b += R(x + 100, by, 40, 28, 5, C.raised) + R(x + 114, by + 8, 12, 12, 2, C.dim);
    b += R(x + 150, by, 40, 28, 5, C.raised) + `<path d="M${x + 162} ${by + 12}a8 6 0 0 1 14-2l2-3v6h-6l2-2a6 4 0 0 0-10 1M${x + 178} ${by + 16}a8 6 0 0 1-14 2l-2 3v-6h6l-2 2a6 4 0 0 0 10-1" fill="${C.good}"/>`;
    b += R(x + 194, by, 34, 28, 5, C.raised, { op: 0.6 }) + `<g opacity=".5">${trash(x + 201, by + 5)}</g>`;
    b += button(x + 236, by + 2, 60, 24, 'Cut', { fill: C.panel, color: C.faint, size: 9 }) + button(x + 300, by + 2, 60, 24, 'Reset', { color: C.dim, size: 9 });
    b += T(x + 374, by + 21, '00:42.6', { size: 19, mono: true, weight: 600 }) + T(x + w - 12, by + 19, 'Recording V1', { size: 9.5, fill: C.mute, anchor: 'end' });
    // waveform: 200 peaks/s in the app; here a deterministic speech-like envelope
    const wx = x + 12, wy = 102, ww = w - 24, wh = 84, mid = wy + wh / 2;
    b += R(wx, wy, ww, wh, 5, '#080a0d');
    let peaks = '';
    const n = 230, filled = 0.82;
    for (let i = 0; i < n * filled; i++) {
      const t = i / n, env = 0.25 + 0.55 * Math.abs(Math.sin(t * 19.7) * Math.sin(t * 6.1 + 1)) + 0.18 * Math.abs(Math.sin(t * 71.3));
      const a = Math.min(1, env * (0.75 + 0.25 * Math.sin(i * 2.7))) * (wh / 2 - 6);
      peaks += `M${f1(wx + 6 + i * (ww - 12) / n)} ${f1(mid - a)}v${f1(2 * a)}`;
    }
    b += `<path d="${peaks}" stroke="${C.virt}" stroke-width="1.6" opacity=".85"/>`;
    const ph = wx + 6 + n * filled * (ww - 12) / n;
    b += L(ph, wy + 4, ph, wy + wh - 4, C.mute, 1.5);
    b += R(wx + 14, wy, 3, wh, 1.5, C.text, { op: 0.75 }) + R(ph + 26, wy, 3, wh, 1.5, C.text, { op: 0.35 });
    b += T(x + 12, 210, 'Hydra_V1_2026-11-09_20-14-03.wav', { size: 9.5, fill: C.dim });
    b += button(x + w - 252, 196, 84, 22, 'Open File...', { color: C.text, size: 9 }) + button(x + w - 162, 196, 76, 22, 'Last Take', { color: C.text, size: 9 }) +
      button(x + w - 80, 196, 68, 22, 'Show', { color: C.text, size: 9 });
    host.innerHTML = svg(W, H, b, "Hydra's recorder: records V1, transport, time, waveform with trim handles");
  };

  // Latency Finder (LatencyView.cpp): matrix of all paths and the chosen path with its stages.
  G.latency = function (host) {
    const W = 860, H = 470;
    const legend = [['Input device', C.in], ['Denoiser', C.good], ['Plugin', C.warn], ['Sync delay', '#ff6aa6'], ['Output device', C.hw]];
    let b = R(0, 0, W, H, 10, C.bg) + T(18, 28, 'Latency Finder', { size: 14, weight: 700 });
    let lx = W - 16;
    legend.slice().reverse().forEach(([t, c]) => { lx -= t.length * 6 + 26; b += R(lx, 18, 11, 11, 3, c) + T(lx + 16, 28, t, { size: 10, fill: C.dim }); });
    b += T(18, 58, 'TOTAL LATENCY PER INPUT AND OUTPUT', { size: 9.5, fill: C.dim, weight: 700, ls: '.04em' }) + T(232, 58, '(click a cell)', { size: 9.5, fill: C.dim, weight: 600 });
    const cols = [['H1', 'Speakers', true], ['H2', 'Headphones', true], ['V1', 'Stream', false], ['V2', 'Discord', false]];
    const rows = [['IN 1', 'Mic', [15.4, null, 24.1, null]], ['IN 2', 'Headset', [null, 21.9, 30.6, null]], ['IN 4', 'Chrome', [16.2, 16.4, null, null]], ['IN 5', 'Discord', [null, 16.9, null, 25.6]]];
    const cx0 = 200, cw = 150;
    cols.forEach(([c, n, hw], k) => { b += T(cx0 + k * cw + cw / 2, 88, c + '  ' + n, { size: 10.5, fill: hw ? C.hw : C.virt, weight: 700, anchor: 'middle' }); });
    rows.forEach(([c, n, v], r) => {
      const y = 100 + r * 34;
      if (r % 2 === 0) b += R(20, y, cx0 - 30 + cw * 4 + 14, 30, 6, C.panel);
      b += T(30, y + 19.5, c, { size: 10.5, weight: 700 }) + T(64, y + 19.5, n, { size: 10.5, weight: 700 });
      v.forEach((ms, k) => {
        const x = cx0 + k * cw + 10;
        if (ms == null) { b += `<circle cx="${x + cw / 2 - 10}" cy="${y + 15}" r="1.6" fill="${C.faint}"/>`; return; }
        const sel = r === 0 && k === 2;
        b += R(x, y + 3, cw - 20, 24, 5, sel ? mix(C.panel, C.in, 0.25) : C.panel2, { stroke: sel ? C.in : C.outline, sw: sel ? 1.5 : 1 }) +
          T(x + (cw - 20) / 2, y + 19, ms.toFixed(1) + ' ms', { size: 10.5, anchor: 'middle', weight: 600, fill: sel ? C.text : C.dim });
      });
    });
    // path of IN 1 -> V1
    b += T(18, 264, 'PATH', { size: 9.5, fill: C.dim, weight: 700, ls: '.04em' }) + T(56, 264, 'IN 1 Mic  →  V1 Stream: 24.1 ms', { size: 9.5, fill: C.text, weight: 600 });
    const stages = [['Input device', 'ASIO: Master 1/2', 2.7, C.in], ['Denoiser', 'Low (10 ms)', 10, C.good], ['Mix', 'V1 Stream', 0, C.faint], ['Output device', 'Hydra Virtual 1 (cable)', 11.4, C.hw]];
    let x = 18, total = 0;
    stages.forEach(([t, d, ms, c], i) => {
      total += ms;
      const w = i === 2 ? 120 : 186;
      b += R(x, 278, w, 66, 7, mix(C.bg, c, 0.12), { stroke: mix(C.bg, c, 0.7), sw: 1.3 });
      b += T(x + 12, 298, t, { size: 10.5, weight: 700 }) + T(x + 12, 314, d, { size: 9.5, fill: C.dim }) +
        T(x + 12, 334, '+' + ms.toFixed(1) + ' ms', { size: 10, fill: c === C.faint ? C.dim : c, weight: 700 }) + T(x + w - 10, 334, 'at ' + total.toFixed(1) + ' ms', { size: 9, fill: C.faint, anchor: 'end' });
      x += w;
      if (i < stages.length - 1) { b += `<path d="M${x + 4} 311l8 0m-3-3 3 3-3 3" stroke="${C.faint}" fill="none" stroke-width="1.3"/>`; x += 16; }
    });
    // latency share
    b += L(18, 366, W - 18, 366, C.outline) + T(18, 392, 'Latency share', { size: 12, weight: 700 });
    let sx = 18;
    const sw = W - 36;
    stages.filter(s => s[2] > 0).forEach(([t, , ms, c]) => {
      const w = sw * ms / total;
      b += R(sx, 404, w - 3, 22, 4, c, { op: 0.85 });
      b += T(sx + 8, 444, t + ' ' + Math.round(ms / total * 100) + ' %', { size: 9.5, fill: C.dim });
      sx += w;
    });
    host.innerHTML = svg(W, H, b, 'Latency Finder: matrix of all input-to-output paths, the path of the mic to the stream with its stages, and the share of each stage');
  };

  // DSP Load (DspView.cpp): total, biggest consumers, one row per channel.
  G.dsp = function (host) {
    const W = 860, H = 470;
    const cols = [['DENOISER', C.good], ['EQ', C.eq], ['GATE', C.gate], ['COMP', '#8d6cff'], ['PLUGINS', C.warn], ['LIMITER', C.lim], ['REST', C.faint]];
    const rows = [
      ['IN 1', 'Mic', C.in, [1.82, 0.12, 0.05, 0.07, null, null, 0.03]], ['IN 2', 'Headset', C.in, [null, null, null, null, null, null, 0.02]],
      ['IN 3', 'Guitar', C.in, [null, null, null, null, 0.64, null, 0.02]], ['IN 6', 'Game', C.in, [null, null, null, null, null, null, 0.02]],
      ['H1', 'Speakers', C.hw, [null, 0.09, null, null, null, 0.06, 0.02]], ['H2', 'Headphones', C.hw, [null, 0.09, null, null, null, 0.06, 0.02]],
      ['V1', 'Stream', C.virt, [null, null, null, 0.06, null, 0.05, 0.02]]
    ];
    let b = R(0, 0, W, H, 10, C.bg) + T(18, 28, 'DSP Load', { size: 14, weight: 700 });
    b += T(18, 76, '3.40 %', { size: 26, mono: true, weight: 500 }) + R(190, 62, 300, 10, 5, C.panel2) + R(190, 62, 300 * 0.034 * 4, 10, 5, C.good) +
      L(190 + 300 * 0.041 * 4, 58, 190 + 300 * 0.041 * 4, 76, C.text, 1.5) + T(512, 71, 'peak 4.10 %', { size: 10, fill: C.dim });
    b += T(18, 110, 'BIGGEST CONSUMERS', { size: 9.5, fill: C.dim, weight: 700, ls: '.04em' });
    [['IN 1 Mic · Denoiser', '1.82 %', C.good], ['IN 3 Guitar · Plugins', '0.64 %', C.warn], ['IN 1 Mic · EQ', '0.12 %', C.eq]].forEach(([t, v, c], i) => {
      const x = 18 + i * 220;
      b += R(x, 120, 210, 26, 6, C.panel) + R(x + 8, 128, 4, 10, 2, c) + T(x + 20, 137, t, { size: 10 }) + T(x + 200, 137, v, { size: 10, mono: true, anchor: 'end', fill: C.dim });
    });
    b += L(18, 162, W - 18, 162, C.outline);
    const cx0 = 260, cw = 84;
    b += T(30, 186, 'CHANNEL', { size: 9, fill: C.dim, weight: 700 }) + T(168, 186, 'TOTAL', { size: 9, fill: C.dim, weight: 700 });
    cols.forEach(([t, c], k) => { b += R(cx0 + k * cw, 177, 3, 12, 1.5, c) + T(cx0 + k * cw + 8, 186, t, { size: 8.5, fill: C.dim, weight: 700 }); });
    rows.forEach(([code, n, c, v], r) => {
      const y = 196 + r * 36, tot = v.reduce((a, q) => a + (q || 0), 0);
      if (r % 2 === 0) b += R(18, y, W - 36, 32, 6, C.panel);
      b += T(30, y + 21, code, { size: 10.5, fill: c, weight: 700 }) + T(30 + code.length * 7 + 8, y + 21, n, { size: 10.5, fill: c, weight: 700 });
      b += T(168, y + 21, tot.toFixed(2) + ' %', { size: 10.5, mono: true }) + R(214, y + 13, 36, 5, 2.5, '#050608') + R(214, y + 13, Math.min(36, tot * 18), 5, 2.5, c);
      v.forEach((q, k) => {
        const x = cx0 + k * cw;
        if (q == null) { b += T(x + 36, y + 20, '-', { size: 10, fill: C.faint, anchor: 'middle' }); return; }
        const col = cols[k][1], heat = Math.min(1, q / 1.5);
        b += R(x, y + 4, cw - 8, 24, 5, mix(C.panel2, col, 0.12 + heat * 0.3), { stroke: mix(C.panel2, col, 0.4) }) +
          T(x + (cw - 8) / 2, y + 20, q.toFixed(2) + ' %', { size: 10, mono: true, anchor: 'middle', fill: heat > 0.3 ? C.text : C.dim });
      });
    });
    host.innerHTML = svg(W, H, b, 'DSP Load: 3.4 % in total, biggest consumers, the cost of each effect per channel');
  };

  /* ---------- effect settings windows (Inspector.cpp AdvancedPanel: cards with a caption, knobs, live bars) ---------- */
  const card = (x, y, w, h, title) => R(x, y, w, h, 4, C.panel2, { stroke: C.outline }) + T(x + 12, y + 17, title, { size: 11, fill: C.dim, weight: 700 });
  const bar = (x, y, w, label, val, col, txt) => T(x, y + 10, label, { size: 8.5, fill: C.dim, weight: 700, ls: '.04em' }) +
    R(x + 58, y + 2, w - 110, 9, 4.5, '#050608') + R(x + 58, y + 2, Math.max(4, (w - 110) * val), 9, 4.5, col) + T(x + w, y + 10, txt, { size: 9, fill: C.dim, anchor: 'end' });
  const knobRow = (x, y, w, list, col) => list.map(([lab, n, v, bi], i) => knob(x + w * (i + 0.5) / list.length, y, 21, n, col, { label: lab, value: v, bipolar: bi })).join('');
  const FX = {};
  FX.gate = () => {
    const W = 660, H = 300, col = C.gate;
    let b = R(0, 0, W, H, 0, C.bg) + card(14, 14, W - 28, 130, 'Gate') + toggle(W - 58, 18, col);
    b += knobRow(24, 72, W - 48, [['THRESHOLD', 0.32, '-42 dB'], ['RANGE', 0.7, '-60 dB'], ['ATTACK', 0.12, '1.0 ms'], ['HOLD', 0.45, '120 ms'], ['RELEASE', 0.4, '150 ms']], col);
    b += card(14, 152, 160, 134, 'Sidechain filter') + toggle(128, 156, col, false) + knob(94, 208, 21, 0.25, col, { label: 'FREQ', value: '100 Hz' });
    b += card(182, 152, 160, 134, 'Voice Key') + toggle(296, 156, col) + knob(262, 208, 21, 0.6, col, { label: 'VOICE THR', value: '60 %' });
    b += card(350, 152, W - 364, 134, 'Live') + bar(364, 190, W - 392, 'STATE', 1, col, 'OPEN') + bar(364, 216, W - 392, 'VOICE', 0.86, col, '86 %') +
      bar(364, 242, W - 392, 'LEVEL', 0.62, C.good, '-23 dB') + L(364 + 58 + (W - 502) * 0.45, 240, 364 + 58 + (W - 502) * 0.45, 255, C.text, 1.5);
    return [W, H, b, 'Gate window: threshold, range, attack, hold, release, sidechain filter, Voice Key and live state'];
  };
  FX.denoiser = () => {
    const W = 660, H = 262, col = C.good;
    let b = R(0, 0, W, H, 0, C.bg) + card(14, 14, W - 28, 108, 'Latency mode') + toggle(W - 58, 18, col);
    ['Ultra Low', 'Low', 'Aligned', 'Native HQ'].forEach((t, i) => {
      const w = (W - 52) / 4, x = 26 + i * w;
      b += R(x, 42, w - 4, 28, 5, i === 1 ? mix(C.raised, col, 0.3) : C.raised, i === 1 ? { stroke: col } : {}) + T(x + (w - 4) / 2, 60, t, { size: 10.5, anchor: 'middle', weight: 600, fill: i === 1 ? C.text : C.dim });
    });
    b += T(26, 92, '+10 ms. RNNoise decides how much noise each band has, Hydra applies it with its own', { size: 10, fill: C.dim }) +
      T(26, 107, 'zero-latency filter bank. Ultra Low adds nothing, Native HQ is RNNoise as designed (30 ms).', { size: 10, fill: C.dim });
    b += card(14, 130, 160, 118, 'Mix') + knob(94, 180, 22, 0.85, col, { label: 'AMOUNT', value: '85 %' });
    b += card(182, 130, W - 196, 118, 'Live') + bar(196, 168, W - 224, 'VOICE', 0.78, col, '78 %') + bar(196, 196, W - 224, 'REMOVED', 0.7, C.in, '-48 dB') +
      T(196, 232, 'Fans, keyboard and room noise are removed between and under the words.', { size: 9.5, fill: C.faint });
    return [W, H, b, 'Denoiser window: latency mode Low, amount 85 %, voice probability'];
  };
  FX.comp = () => {
    const W = 660, H = 300, col = C.comp, cw = 214, lw = W - 28 - cw - 8;
    let b = R(0, 0, W, H, 0, C.bg) + card(14, 14, lw, 130, 'Compressor') + toggle(14 + lw - 44, 18, col);
    b += knobRow(20, 72, lw - 12, [['INPUT', 0.5, '0.0 dB', true], ['THRESHOLD', 0.62, '-18 dB'], ['RATIO', 0.3, '3.0:1'], ['KNEE', 0.25, '6 dB']], col);
    b += card(14, 152, lw, 134, 'Timing and gain') + R(14 + lw - 92, 156, 84, 20, 4, mix(C.raised, col, 0.3), { stroke: col }) + T(14 + lw - 50, 170, 'AUTO GAIN', { size: 8.5, anchor: 'middle', weight: 700 });
    b += knobRow(20, 208, lw - 12, [['ATTACK', 0.2, '5 ms'], ['RELEASE', 0.4, '120 ms'], ['MAKEUP', 0.6, '+4.0 dB']], col);
    const cx = 14 + lw + 8;
    b += card(cx, 14, cw, 272, 'Curve');
    const px = cx + 16, py = 36, ps = cw - 32, toX = d => px + ps * (d + 60) / 60, toY = d => py + ps - ps * (d + 60) / 60;
    b += R(px, py, ps, ps, 3, '#0c0f14');
    [-48, -36, -24, -12].forEach(d => { b += L(toX(d), py, toX(d), py + ps, '#1c222c') + L(px, toY(d), px + ps, toY(d), '#1c222c'); });
    b += L(toX(-60), toY(-60), toX(0), toY(0), C.faint, 1, 'stroke-dasharray="3 3"');
    const thr = -18, ratio = 3, knee = 6, mk = 4, out = d => {
      const o = d - thr;
      const y = 2 * o < -knee ? d : 2 * Math.abs(o) <= knee ? d + (1 / ratio - 1) * Math.pow(o + knee / 2, 2) / (2 * knee) : thr + o / ratio;
      return Math.min(0, y + mk);
    };
    b += `<path d="M${[...Array(61)].map((_, i) => { const d = -60 + i; return f1(toX(d)) + ' ' + f1(toY(out(d))); }).join('L')}" fill="none" stroke="${col}" stroke-width="2"/>`;
    b += `<circle cx="${f1(toX(-9))}" cy="${f1(toY(out(-9)))}" r="4.5" fill="${C.text}"/>`;
    b += T(px, py + ps + 18, 'GR', { size: 8.5, fill: C.faint }) + R(px + 20, py + ps + 10, ps - 20, 8, 3, '#050608') + R(px + 20, py + ps + 10, (ps - 20) * 0.3, 8, 3, col, { op: 0.85 }) +
      T(px + ps, py + ps + 34, '-6.0 dB', { size: 9, fill: C.dim, anchor: 'end' });
    return [W, H, b, 'Compressor window: input, threshold, ratio, knee, attack, release, makeup, auto gain and the curve'];
  };
  FX.limiter = () => {
    const W = 420, H = 236;
    let b = R(0, 0, W, H, 0, C.bg) + block(14, 14, 190, 208, 'LIMITER', C.lim) + knob(109, 112, 30, 0.85, C.lim, { label: 'CEILING', value: '-1.0 dB' });
    b += T(109, 200, 'GR', { size: 8, fill: C.faint, anchor: 'middle' }) + R(34, 206, 150, 6, 3, '#050608') + R(34, 206, 22, 6, 3, C.lim);
    b += T(222, 52, 'No extra window: the limiter', { size: 11, fill: C.dim }) + T(222, 70, 'is a block in the chain with', { size: 11, fill: C.dim }) +
      T(222, 88, 'one control, the ceiling.', { size: 11, fill: C.dim }) + T(222, 124, 'Nothing goes over it, with', { size: 11, fill: C.dim }) + T(222, 142, 'no added latency.', { size: 11, fill: C.dim });
    return [W, H, b, 'Limiter block with its ceiling'];
  };
  FX.phase = () => {
    const W = 420, H = 200;
    let b = R(0, 0, W, H, 0, C.bg) + block(14, 14, 160, 172, 'PHASE', C.phase);
    b += `<circle cx="94" cy="100" r="30" fill="${mix(C.raised, C.phase, 0.3)}" stroke="${C.phase}" stroke-width="1.5"/>` +
      `<circle cx="94" cy="100" r="13" fill="none" stroke="${C.text}" stroke-width="2.2"/>` + L(78, 118, 110, 82, C.text, 2.2, 'stroke-linecap="round"') +
      T(94, 156, 'INVERTED', { size: 9, fill: C.phase, anchor: 'middle', weight: 700, ls: '.05em' });
    const wave = (y, inv, col) => `<path d="M${[...Array(61)].map((_, i) => f1(196 + i * 3.4) + ' ' + f1(y - (inv ? -1 : 1) * 18 * Math.sin(i / 60 * Math.PI * 4))).join('L')}" fill="none" stroke="${col}" stroke-width="2"/>`;
    b += T(196, 40, 'IN', { size: 8.5, fill: C.dim, weight: 700 }) + wave(70, false, C.dim) + T(196, 120, 'OUT', { size: 8.5, fill: C.dim, weight: 700 }) + wave(150, true, C.phase);
    return [W, H, b, 'Phase block: polarity inverted'];
  };
  FX.vst = () => {
    const W = 660, H = 290;
    let b = R(0, 0, W, H, 0, C.bg) + R(0, 0, W, 34, 0, C.panel) + T(14, 22, 'IN 1', { size: 11, fill: C.in, weight: 700 }) + T(46, 22, 'Mic  ·  Your plugin (VST3)', { size: 11, weight: 600 });
    b += button(W - 210, 8, 50, 18, 'ON', { fill: mix(C.raised, C.in, 0.35), color: C.text }) + button(W - 154, 8, 80, 18, 'PRESETS', { color: C.text }) + button(W - 68, 8, 56, 18, 'CLOSE', { color: C.text });
    // the plugin's own editor (every plugin looks different; neutral stand-in)
    b += `<defs><linearGradient id="vstbg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#242a35"/><stop offset="1" stop-color="#171b22"/></linearGradient></defs>`;
    b += R(14, 46, W - 28, H - 60, 8, 'url(#vstbg)', { stroke: '#323a48' }) + T(32, 74, "The plugin's own editor", { size: 13, weight: 700 }) +
      T(32, 92, 'Hydra opens it in a window, saves its state in the session and skips plugins that crash while scanning.', { size: 9.5, fill: C.dim });
    const sx = 32, sy = 110, sw = 360, sh = 150;
    b += R(sx, sy, sw, sh, 5, '#0c0f14');
    let p = '';
    for (let i = 0; i <= 90; i++) { const f = i / 90; p += `L${f1(sx + f * sw)} ${f1(sy + sh - 10 - (sh - 30) * (0.55 + 0.3 * Math.sin(f * 9) * Math.exp(-f * 2) - 0.4 * f * f) * (0.8 + 0.2 * Math.sin(i * 1.7)))}`; }
    b += `<path d="M${sx} ${sy + sh}${p}L${sx + sw} ${sy + sh}Z" fill="${C.warn}" opacity=".22"/><path d="M${p.slice(1)}" fill="none" stroke="${C.warn}" stroke-width="1.5"/>`;
    [['DRIVE', 0.6], ['TONE', 0.45], ['MIX', 0.8]].forEach(([l, n], i) => { b += knob(450 + i * 64, 175, 24, n, C.warn, { label: l }); });
    return [W, H, b, 'Window of a VST3 plugin in Hydra with the plugin editor'];
  };
  FX.correction = () => {
    const W = 680, H = 330, col = C.corr;
    let b = R(0, 0, W, H, 0, C.bg) + T(16, 26, 'Room Correction', { size: 13, weight: 700 }) + T(140, 26, 'H1 Speakers', { size: 11, fill: C.hw, weight: 700 });
    ['Correction', 'Calibrate', 'Analysis', 'Edit'].forEach((t, i) => { b += R(300 + i * 92, 10, 86, 24, 5, i === 0 ? mix(C.raised, col, 0.3) : C.raised, i === 0 ? { stroke: col } : {}) + T(343 + i * 92, 26, t, { size: 10, anchor: 'middle', weight: 600, fill: i === 0 ? C.text : C.dim }); });
    const x = 50, y = 50, w = W - 66, h = 210, lo = 20, hi = 20000, rg = 15;
    const fx = f => x + w * Math.log(f / lo) / Math.log(hi / lo), fy = d => y + h / 2 - d / rg * (h / 2);
    b += R(x, y, w, h, 4, '#0c0f14');
    [50, 100, 200, 500, 1000, 2000, 5000, 10000].forEach(f => { b += L(fx(f), y, fx(f), y + h, '#1c222c') + T(fx(f), y + h + 14, f >= 1000 ? f / 1000 + 'k' : f, { size: 8.5, fill: C.faint, anchor: 'middle' }); });
    [-10, -5, 0, 5, 10].forEach(d => { b += L(x, fy(d), x + w, fy(d), d ? '#1c222c' : '#2a3240') + T(x - 6, fy(d) + 3, (d > 0 ? '+' : '') + d, { size: 8.5, fill: C.faint, anchor: 'end' }); });
    const cut = [['bell', -7, 44, 6], ['bell', -6, 88, 5], ['bell', -3.5, 230, 3], ['bell', 2.5, 64, 4]];
    const meas = f => 3 + 7 / (1 + Math.pow(Math.log2(f / 44) * 6, 2)) + 6 / (1 + Math.pow(Math.log2(f / 88) * 5, 2)) + 3.5 / (1 + Math.pow(Math.log2(f / 230) * 3, 2)) -
      5 / (1 + Math.pow(Math.log2(f / 64) * 5, 2)) - 1.2 * Math.sin(Math.log(f) * 7) * (f > 400 ? 0.6 : 1) - (f < 32 ? (32 - f) * 0.5 : 0) - (f > 9000 ? (f - 9000) / 1600 : 0) - 3;
    const N = 170, fs = [...Array(N + 1)].map((_, i) => lo * Math.pow(hi / lo, i / N));
    const corr = f => f > 500 ? 0 : cut.reduce((a, [t, g, f0, q]) => a + bandDb(t, g, f0, q, f), 0);
    const path = fn => 'M' + fs.map(f => f1(fx(f)) + ' ' + f1(fy(Math.max(-rg, Math.min(rg, fn(f)))))).join('L');
    b += L(x, fy(0), x + w, fy(0), C.good, 1.2, 'stroke-dasharray="5 4" opacity=".7"');
    b += `<path d="${path(meas)}" fill="none" stroke="${C.dim}" stroke-width="1.4" opacity=".8"/>`;
    b += `<path d="${path(corr)}" fill="none" stroke="${col}" stroke-width="2"/>`;
    b += `<path d="${path(f => meas(f) + corr(f))}" fill="none" stroke="${C.text}" stroke-width="2"/>`;
    [['Measured', C.dim], ['Correction', col], ['Result', C.text], ['Target', C.good]].forEach(([t, c], i) => { b += R(x + i * 110, 292, 14, 3, 1.5, c) + T(x + 20 + i * 110, 297, t, { size: 10, fill: C.dim }); });
    b += T(W - 16, 297, '4 bands  ·  L/R aligned 0.42 ms', { size: 10, fill: C.dim, anchor: 'end' });
    return [W, H, b, 'Room correction window: measured response, correction, result and target'];
  };
  // Headphone Correction window (ui/HeadphoneView.cpp): your headphones, "should sound like", AMOUNT/GAIN/AUTO MAKEUP, curves.
  FX.headphone = () => {
    const W = 800, H = 400, col = '#7c9cff', orange = '#ffa53d', green = C.good;
    let b = R(0, 0, W, H, 0, C.bg);
    const list = (x, title, rows, sel) => {
      let g = T(x, 26, title, { size: 9.5, fill: C.dim, weight: 700, ls: '.04em' }) + R(x, 36, 176, 24, 3, '#07090c', { stroke: C.outline }) + T(x + 9, 52, 'Search', { size: 10, fill: C.faint });
      g += R(x, 68, 176, H - 82, 6, C.panel);
      let y = 74;
      rows.forEach(r => {
        if (r[0] === '#') { g += T(x + 8, y + 15, r.slice(1), { size: 8.5, fill: C.dim, weight: 700 }); y += 22; return; }
        const [n, src] = r.split('|'), on = n === sel;
        if (on) g += R(x + 4, y, 168, 22, 5, mix(C.panel, col, 0.3));
        g += T(x + 12, y + 15, n, { size: 10, weight: on ? 700 : 400 }) + T(x + 164, y + 15, src, { size: 7.5, fill: C.faint, anchor: 'end' });
        y += 24;
      });
      return g;
    };
    b += list(14, 'YOUR HEADPHONES', ['DT 770 Pro 80 Ohm|Kuulokenurkka', 'DT 770 Pro 250 Ohm|oratory1990', 'DT 880|oratory1990', 'DT 990 Pro|oratory1990', '#BOSE', 'QuietComfort 45|oratory1990', '#CORSAIR', 'HS80 RGB Wireless|Rtings', '#SENNHEISER', 'HD 560S|oratory1990', 'HD 600|oratory1990', 'HD 650|oratory1990'], 'DT 770 Pro 80 Ohm');
    b += list(200, 'SHOULD SOUND LIKE', ['#TARGETS', 'Neutral (Harman)|target', 'Harman without bass|target', 'Diffuse field|target', '#SONY', 'WH-1000XM5|oratory1990', '#STEELSERIES', 'Arctis 7 (2019)|Rtings', 'Arctis 7 Wireless (2017)|Rtings', 'Arctis Nova Pro|Rtings'], 'Arctis 7 Wireless (2017)');
    const gx = 392;
    b += button(gx, 14, 150, 30, 'Add headphones…', { size: 10.5 });
    b += knob(gx + 186, 36, 15, 1, col, { label: 'AMOUNT', value: '100 %' }) + knob(gx + 240, 36, 15, 0.5, col, { label: 'GAIN', value: '0.0 dB', bipolar: true });
    b += button(gx + 274, 24, 80, 22, 'AUTO MAKEUP', { size: 8.5 }) + toggle(W - 42, 18, col);
    const x = gx + 26, y = 92, w = W - x - 18, h = 230, lo = 20, hi = 20000, rg = 10;
    const fx = f => x + w * Math.log(f / lo) / Math.log(hi / lo), fy = d => y + h / 2 - d / rg * (h / 2);
    b += R(gx, 82, W - gx - 10, h + 34, 6, '#0c0f14');
    [50, 100, 200, 500, 1000, 2000, 5000, 10000].forEach(f => { b += L(fx(f), y, fx(f), y + h, '#1c222c') + T(fx(f), y + h + 14, f >= 1000 ? f / 1000 + 'k' : f, { size: 8.5, fill: C.faint, anchor: 'middle' }); });
    [-10, -5, 0, 5, 10].forEach(d => { b += L(x, fy(d), x + w, fy(d), d ? '#1c222c' : '#2a3240') + T(x - 6, fy(d) + 3, (d > 0 ? '+' : '') + d, { size: 8.5, fill: C.faint, anchor: 'end' }); });
    const bell = (f, f0, g, q) => g / (1 + Math.pow(Math.log2(f / f0) * q, 2));
    const hp = f => -2.5 + bell(f, 45, 3.8, 1.2) + bell(f, 90, -2.2, 4) + bell(f, 190, -3.8, 3) + bell(f, 1300, 1.6, 3) + bell(f, 6200, -6, 4) + bell(f, 8500, 4, 2) + bell(f, 11000, 3, 3);
    const tgt = f => -3.6 + bell(f, 22, -2.5, 2) + bell(f, 150, 7.4, 1.4) + bell(f, 700, -1.6, 2) + bell(f, 2600, 1.5, 3) + bell(f, 5000, -5, 5) + bell(f, 11000, 3.5, 2) + bell(f, 15000, 1, 2);
    const N = 180, fs = [...Array(N + 1)].map((_, i) => lo * Math.pow(hi / lo, i / N));
    const cor = f => (tgt(f) - hp(f)) * (f > 10000 ? 0.4 : 1);
    const pts = fn => fs.map(f => f1(fx(f)) + ' ' + f1(fy(Math.max(-rg, Math.min(rg, fn(f)))))).join('L');
    b += `<path d="M${f1(x)} ${f1(fy(0))}L${pts(cor)}L${f1(x + w)} ${f1(fy(0))}Z" fill="${col}" opacity=".16"/>`;
    b += `<path d="M${pts(hp)}" fill="none" stroke="${orange}" stroke-width="1.5"/>`;
    b += `<path d="M${pts(tgt)}" fill="none" stroke="${C.dim}" stroke-width="1.4" stroke-dasharray="5 4"/>`;
    b += `<path d="M${pts(cor)}" fill="none" stroke="${col}" stroke-width="2"/>`;
    b += `<path d="M${pts(f => hp(f) + cor(f) + 0.25 * Math.sin(Math.log(f) * 9))}" fill="none" stroke="${green}" stroke-width="1.8"/>`;
    [['DT 770 Pro 80 Ohm', orange, ''], ['Target: Arctis 7 Wireless (2017)', C.dim, 'd'], ['Correction', col, ''], ['Result', green, '']].forEach(([t, c, d], i) => {
      const lx = x + 8 + (i % 2) * 150, ly = y + 6 + Math.floor(i / 2) * 16;
      b += (d ? L(lx, ly, lx + 14, ly, c, 2, 'stroke-dasharray="4 3"') : R(lx, ly - 1.5, 14, 3, 1.5, c)) + T(lx + 20, ly + 3.5, t, { size: 9, weight: 600 });
    });
    b += T(gx, H - 30, '9 filters (zero latency), deviation 3.3 → 0.3 dB. Loudness −5.7 dB.', { size: 10 });
    b += T(gx, H - 14, 'Built-in measurements: AutoEq project (MIT), oratory1990, Rtings, Kuulokenurkka and others.', { size: 8, fill: C.faint });
    return [W, H, b, 'Headphone Correction window: DT 770 Pro 80 Ohm should sound like the Arctis 7 Wireless; headphone curve, target, correction and result'];
  };
  // Stem Separator window (Inspector.cpp StemsPanel): latency mode, latency + GPU load, one fader per stem with M/S, models.
  FX.stems = () => {
    const W = 680, H = 420, col = '#56e0c8';
    const stems = [['VOCALS', '#ff6ac1', 0, false], ['DRUMS', '#ffa53d', -6, false], ['BASS', '#8d6cff', 0, false], ['GUITAR', '#ffd84a', 0, true], ['REST', '#35c8f4', -3, false]];
    let b = R(0, 0, W, H, 0, C.bg) + card(14, 14, W - 28, 104, 'Latency mode') + toggle(W - 58, 18, col);
    b += R(24, 38, W - 48, 28, 6, '#07090c');
    ['Live', 'Fast', 'Balanced', 'Best'].forEach((t, i) => {
      const w = (W - 52) / 4, x = 26 + i * w, on = i === 3;
      b += (on ? R(x, 40, w - 2, 24, 5, col) : '') + T(x + w / 2, 56, t, { size: 10.5, anchor: 'middle', weight: on ? 700 : 400, fill: on ? '#0a0c10' : C.dim });
    });
    b += T(24, 92, '3.6 s', { size: 15, weight: 700 }) + T(24, 108, 'Best quality, 6 stems', { size: 9.5, fill: C.dim });
    b += T(390, 96, 'GPU', { size: 9.5, fill: C.dim }) + R(420, 89, 190, 8, 4, '#050608') + R(420, 89, 190 * 0.48, 8, 4, col) + T(W - 24, 97, '48 %', { size: 10, anchor: 'end' });
    b += card(14, 126, W - 28, 214, 'Stems');
    const y0 = 166, y1 = 300, yy = d => y1 - dbPos(d) * (y1 - y0);
    stems.forEach(([n, c, db, muted], i) => {
      const cw = (W - 28) / 5, cx = 14 + cw * i + cw / 2, fade = muted ? 0.35 : 1;
      b += T(cx, 154, n, { size: 9.5, weight: 700, anchor: 'middle', fill: muted ? mix(c, C.panel2, 0.5) : c });
      [[12, '12'], [0, '0'], [-12, '-12'], [-40, '-40'], [-60, '-inf']].forEach(([d, t]) => { b += T(cx - 16, yy(d) + 3, t, { size: 7, fill: C.faint, anchor: 'end' }); });
      b += R(cx - 10, y0, 5, y1 - y0, 2.5, '#050608') + R(cx - 10, yy(db), 5, y1 - yy(db), 2.5, c, { op: 0.9 * fade });
      b += R(cx - 20, yy(db) - 7, 25, 14, 3, '#c9ced8', { op: muted ? 0.45 : 1 }) + R(cx - 18, yy(db) - 1, 21, 2, 1, c);
      const lvl = muted ? 0 : [0.7, 0.62, 0.55, 0, 0.5][i];
      b += R(cx + 10, y0, 5, y1 - y0, 2.5, '#050608') + (lvl ? R(cx + 10, y1 - (y1 - y0) * lvl, 5, (y1 - y0) * lvl, 2.5, c, { op: 0.75 }) : '');
      b += button(cx - 30, 310, 28, 20, 'M', { fill: muted ? C.mute : C.raised, color: muted ? '#ffffff' : C.dim, size: 9.5 }) + button(cx + 2, 310, 28, 20, 'S', { size: 9.5 });
    });
    b += card(14, 348, W - 28, 58, 'Models');
    b += T(24, 384, 'BS-RoFormer SW, 6 stems  ·  Fast, Balanced, Best', { size: 10, weight: 600 }) + T(24, 398, 'HT-Demucs, 4 stems  ·  Live', { size: 10, weight: 600 });
    b += button(W - 196, 376, 46, 18, 'AUTO', { fill: col, color: '#0a0c10' }) + R(W - 140, 383, 110, 5, 2.5, '#050608') + R(W - 140, 383, 60, 5, 2.5, mix(col, C.panel2, 0.4)) + `<circle cx="${W - 80}" cy="385.5" r="6" fill="${C.dim}"/>`;
    return [W, H, b, 'Stem Separator window: latency mode Best, 3.6 s latency, 48 % GPU, faders for vocals, drums, bass, guitar (muted) and rest, the two models'];
  };
  // Segmented choice (Inspector.cpp ChoiceBar): dark track, the chosen entry filled in the effect colour.
  const seg = (x, y, w, h, list, on, col) => R(x, y, w, h, 6, '#07090c') + list.map((t, i) => {
    const sw = w / list.length, sx = x + i * sw, a = i === on;
    return (a ? R(sx + 2, y + 2, sw - 4, h - 4, 5, col) : '') + T(sx + sw / 2, y + h / 2 + 3.8, t, { size: 10.5, anchor: 'middle', weight: a ? 700 : 400, fill: a ? '#0a0c10' : C.dim });
  }).join('');
  const C_UTIL = '#ff6ac1', C_TP = '#c77dff';
  // Utility window (Inspector.cpp UtilityPanel): phase + channel, width, bass mono, output.
  FX.utility = () => {
    const W = 700, H = 300, col = C_UTIL;
    let b = R(0, 0, W, H, 0, C.bg) + card(14, 14, 376, 130, 'Phase and channel');
    b += R(24, 66, 56, 30, 6, col) + `<circle cx="52" cy="81" r="9" fill="none" stroke="#0a0c10" stroke-width="2.2"/>` + L(42, 93, 62, 69, '#0a0c10', 2.2, 'stroke-linecap="round"');
    b += seg(92, 66, 288, 30, ['Left', 'Stereo', 'Right', 'Swap'], 1, col);
    b += card(398, 14, 288, 130, 'Width') + button(520, 20, 52, 18, 'MONO') + toggle(646, 19, col) + knob(542, 76, 26, 0.58, col, { label: 'WIDTH', value: '140 %' });
    b += card(14, 152, 200, 134, 'Bass Mono') + toggle(170, 157, col, false) + knob(114, 210, 24, 0.3, col, { label: 'FREQUENCY', value: '120 Hz' });
    b += card(222, 152, 464, 134, 'Output') + button(636, 158, 40, 18, 'DC') +
      knob(400, 210, 24, 0.5, col, { label: 'BALANCE', value: '0.0 %', bipolar: true }) + knob(500, 210, 24, 0.5, col, { label: 'GAIN', value: '0.0 dB', bipolar: true });
    return [W, H, b, 'Utility window: phase, channel Left/Stereo/Right/Swap, width 140 %, bass mono, balance, gain, DC filter'];
  };
  // Transpose window (Inspector.cpp TransposePanel): pitch, quality, key detection.
  FX.transpose = () => {
    const W = 720, H = 330, col = C_TP;
    let b = R(0, 0, W, H, 0, C.bg) + card(14, 14, 230, 130, 'Pitch');
    b += knob(72, 70, 26, 0.54, col, { label: 'SEMITONES', value: '+2 st', bipolar: true }) + knob(184, 70, 26, 0.28, col, { label: 'FINE', value: '-22.5 ct', bipolar: true });
    b += card(252, 14, 454, 130, 'Quality') + toggle(666, 19, col) + seg(262, 40, 434, 28, ['Fast', 'Normal', 'High'], 1, col);
    b += button(262, 80, 130, 22, 'KEEP TIMBRE') + T(262, 128, 'Latency while shifting: 200 ms', { size: 10, fill: C.dim });
    b += card(14, 152, 692, 166, 'Key') + button(24, 206, 130, 26, 'FIND NEW KEY') + button(24, 242, 130, 26, 'ADD PART');
    b += R(166, 176, 128, 132, 6, mix(C.panel2, col, 0.1), { stroke: col }) + T(230, 238, 'B', { size: 34, weight: 700, anchor: 'middle' }) +
      T(230, 260, 'MINOR', { size: 10, fill: col, weight: 700, anchor: 'middle' }) + T(230, 278, '81 %', { size: 10.5, weight: 700, anchor: 'middle' }) + T(230, 298, 'song: A minor', { size: 8.5, fill: C.faint, anchor: 'middle' });
    b += R(302, 176, 128, 132, 6, '#0e1117') + T(366, 230, '+22.5', { size: 22, weight: 700, anchor: 'middle' }) + T(366, 248, 'ct, TUNED', { size: 9.5, fill: C.good, weight: 700, anchor: 'middle' }) +
      button(312, 274, 108, 24, 'AUTO FINETUNE', { size: 8 });
    b += T(444, 196, 'B minor', { size: 11, weight: 700 }) + T(444, 210, '1st  81 %', { size: 9.5, fill: col }) + T(444, 240, 'D major', { size: 11, weight: 700, fill: C.dim }) + T(444, 254, '2nd  15 %', { size: 9.5, fill: C.faint });
    const tiles = (y, notes, heard) => notes.map((n, i) => { const h = heard[i]; return R(552 + i * 20, y, 16, 34, 4, h ? mix(C.panel2, col, 0.55) : '#0e1117', h ? { stroke: col } : {}) + T(560 + i * 20, y + 21, n, { size: 7.5, anchor: 'middle', fill: h ? C.text : C.faint, weight: i ? 400 : 700 }); }).join('');
    b += tiles(178, ['B', 'C#', 'D', 'E', 'F#', 'G', 'A'], [1, 0, 1, 1, 1, 1, 1]) + tiles(222, ['D', 'E', 'F#', 'G', 'A', 'B', 'C#'], [1, 1, 1, 1, 1, 1, 0]);
    b += T(444, 298, 'Filled = heard  ·  also heard: Bb  ·  2 parts of the song', { size: 9, fill: C.faint });
    return [W, H, b, 'Transpose window: +2 semitones, -22.5 cents, quality Normal (200 ms), detected key B minor 81 %, song tuned +22.5 cents'];
  };

  /* ---------- wiki: device picker, controls, channel menu ---------- */
  const menu = (x, y, w, items, hi = -1) => {
    const h = items.reduce((a, t) => a + (t === '-' ? 9 : 22), 8);
    let g = R(x + 3, y + 4, w, h, 7, '#000', { op: 0.35 }) + R(x, y, w, h, 7, '#1b2029', { stroke: C.outline });
    let yy = y + 4;
    items.forEach((t, i) => {
      if (t === '-') { g += L(x + 10, yy + 4.5, x + w - 10, yy + 4.5, C.outline); yy += 9; return; }
      if (i === hi) g += R(x + 4, yy, w - 8, 22, 4, mix('#1b2029', C.in, 0.25));
      g += T(x + 14, yy + 15, t, { size: 10.5, fill: i === hi ? C.text : '#c9ced8' });
      yy += 22;
    });
    return g;
  };
  const row = (x, y, w, t, o = {}) => (o.sel ? R(x, y, w, 22, 4, mix(C.panel2, C.in, 0.22), { stroke: mix(C.in, C.panel2, 0.4) }) : '') +
    (o.dot ? `<circle cx="${x + 10}" cy="${y + 11}" r="3" fill="${C.in}"/>` : '') + T(x + 20, y + 15, t, { size: 10.5, fill: o.sel ? C.text : '#c9ced8', weight: o.sel ? 600 : 400 }) +
    (o.tag ? R(x + w - 8 - o.tag.length * 6.2 - 8, y + 4, o.tag.length * 6.2 + 8, 14, 7, C.raised) + T(x + w - 12 - o.tag.length * 3.1, y + 14.5, o.tag, { size: 8, fill: C.dim, anchor: 'middle', weight: 700 }) : '') +
    (o.tick ? `<path d="M${x + w - 22} ${y + 11}l4 4 7-8" fill="none" stroke="${C.good}" stroke-width="1.8" stroke-linecap="round"/>` : '');
  G.picker = function (host) {
    const W = 780, H = 360;
    let b = R(0, 0, W, H, 10, C.bg) + R(0, 0, W, 38, 10, C.panel) + R(0, 28, W, 10, 0, C.panel);
    b += T(16, 24, 'Input source', { size: 13, weight: 700 }) + T(110, 24, 'IN 1  Mic', { size: 11, fill: C.in, weight: 700 });
    b += `<circle cx="${W - 30}" cy="19" r="9" fill="none" stroke="${C.dim}" stroke-width="1.3"/>` + T(W - 30, 23, 'i', { size: 11, fill: C.dim, anchor: 'middle', weight: 700 });
    const col = (x, w, title) => card(x, 48, w, 262, title);
    b += col(12, 236, 'Connection') + col(256, 270, 'Device') + col(534, 234, 'Channels');
    const grp = (x, y, t) => T(x + 12, y + 14, t, { size: 8, fill: C.faint, weight: 700, ls: '.1em' });
    let y = 70;
    [['HARDWARE'], ['Master device', { sel: true, dot: true }], ['Other ASIO Devices'], ['VIRTUAL CABLE'], ['Virtual Cable'], ['WINDOWS'], ['Recording devices'],
      ['SOFTWARE'], ['Application'], ['Loopback'], ['NETWORK'], ['Hydra Link']].forEach(([t, o]) => {
      if (t === t.toUpperCase()) { b += grp(12, y, t); y += 17; } else { b += row(20, y, 220, t, o || {}); y += 22; }
    });
    b += T(268, 88, 'Show all devices', { size: 10, fill: C.dim }) + toggle(484, 76, C.in, false);
    b += row(264, 100, 254, 'Focusrite USB ASIO', { sel: true, dot: true });
    b += T(270, 146, 'The master device comes first.', { size: 9.5, fill: C.faint }) + T(270, 160, 'Other ASIO drivers are listed', { size: 9.5, fill: C.faint }) + T(270, 174, 'under "Other ASIO Devices".', { size: 9.5, fill: C.faint });
    y = 76;
    [['None'], ['Mono 1', { tag: 'IN 1' }], ['Mono 2'], ['Stereo 1/2', { sel: true, dot: true, tick: true }], ['Stereo 3/4', { tag: 'IN 3' }], ['Stereo 5/6'], ['Stereo 7/8']].forEach(([t, o]) => { b += row(542, y, 218, t, o || {}); y += 24; });
    b += button(W - 150, 322, 64, 26, 'REFRESH', { color: C.text }) + button(W - 78, 322, 64, 26, 'DONE', { fill: C.in, color: '#0a0c10' });
    b += T(14, 340, 'Click = use it right away  ·  double-click = use it and close', { size: 10, fill: C.dim });
    host.innerHTML = svg(W, H, b, 'Device picker: connection type, device and channels in three columns');
  };
  G.controls = function (host) {
    const W = 780, H = 250;
    let b = R(0, 0, W, H, 10, C.bg);
    const cap = (x, t1, t2) => T(x, 214, t1, { size: 11, weight: 700, anchor: 'middle' }) + T(x, 230, t2, { size: 9.5, fill: C.dim, anchor: 'middle' });
    // 1 knob: drag up/down
    b += knob(90, 96, 34, 0.62, C.in, { label: 'GAIN', value: '+4.5 dB', bipolar: true });
    b += `<path d="M146 66v56M140 74l6-8 6 8M140 114l6 8 6-8" fill="none" stroke="${C.dim}" stroke-width="1.5" stroke-linecap="round"/>` + cap(100, 'Drag', 'Shift = fine  ·  double-click = default');
    // 2 fader: click on the track
    b += capGrad + faderBlock(206, 30, 180, -6, -14, C.in, false);
    b += `<circle cx="246" cy="66" r="7" fill="none" stroke="${C.solo}" stroke-width="1.6"/><path d="M246 76v20" stroke="${C.solo}" stroke-width="1.4" stroke-dasharray="3 3"/>` + cap(250, 'Click the track', 'the fader glides there');
    // 3 value field
    b += R(352, 82, 110, 28, 5, '#07090c', { stroke: C.in }) + T(366, 101, '-6', { size: 13, mono: true }) + L(384, 88, 384, 104, C.text, 1.2);
    b += T(407, 140, '-6 · 2.5k · -inf', { size: 9.5, fill: C.faint, anchor: 'middle' }) + cap(407, 'Click the value', 'and type a number');
    // 4 wheel
    b += R(510, 56, 40, 64, 20, C.raised, { stroke: C.outline }) + R(527, 68, 6, 16, 3, C.dim) + L(530, 92, 530, 120, C.outline);
    b += `<path d="M562 70a14 14 0 0 1 0 28" fill="none" stroke="${C.dim}" stroke-width="1.5"/><path d="M558 94l4 5 5-4" fill="none" stroke="${C.dim}" stroke-width="1.5"/>` + cap(530, 'Mouse wheel', 'Ctrl + wheel always adjusts');
    // 5 right-click menu
    b += menu(612, 22, 156, ['MIDI Learn', 'Clear MIDI Mapping', 'MIDI Range (calibrate)', '-', 'Assign Keyboard Shortcut...', 'Reset to Default'], 0) + cap(690, 'Right-click', 'MIDI, shortcuts, reset');
    host.innerHTML = svg(W, H, b, 'How controls work: drag, click the track, type a value, mouse wheel, right-click menu');
  };
  G.order = function (host) {
    const W = 780, H = 300;
    let b = R(0, 0, W, H, 10, C.bg);
    b += menu(14, 14, 190, ['Rename', '-', 'Copy', 'Cut', 'Paste', 'Paste as new channel', 'Duplicate', 'Delete...', '-', 'Icon', 'Change Order...', 'Add Input'], 10);
    b += T(110, 290, 'Right-click a strip', { size: 10, fill: C.dim, anchor: 'middle' });
    const bx = 232;
    b += R(bx, 14, W - bx - 14, 36, 7, C.panel) + T(bx + 14, 37, 'Change order: drag the strips, the trash can removes one', { size: 10.5, fill: C.dim }) +
      button(W - 160, 21, 66, 22, 'DISCARD', { color: C.text }) + button(W - 88, 21, 60, 22, 'DONE', { fill: C.in, color: '#0a0c10' });
    const mini = (x, y, n, name, o = {}) => {
      let g = (o.drag ? R(x + 4, y + 6, 96, 216, 8, '#000', { op: 0.45 }) : '') + R(x, y, 96, 216, 8, C.panel, { stroke: o.drag ? C.in : C.outline, sw: o.drag ? 1.6 : 1 });
      g += R(x + 8, y + 10, 20, 20, 5, mix(C.panel, C.in, 0.22)) + T(x + 18, y + 24, n, { size: 10.5, fill: C.in, anchor: 'middle', weight: 700 }) + T(x + 34, y + 24, name, { size: 11 });
      g += grip(x + 44, y + 100) + trash(x + 38, y + 186);
      [0, 1, 2].forEach(i => { g += R(x + 12 + i * 26, y + 42, 20, 14, 4, C.raised); });
      g += R(x + 46, y + 66, 4, 104, 2, '#050608') + R(x + 38, y + 112, 20, 10, 3, '#c9ced8');
      return g;
    };
    b += mini(bx + 4, 66, 1, 'Mic') + mini(bx + 108, 66, 2, 'Headset');
    b += R(bx + 212, 66, 3, 216, 1.5, C.in) + mini(bx + 222, 66, 3, 'Chrome');
    b += `<g transform="rotate(-3 ${bx + 378} 170)">${mini(bx + 330, 52, 7, 'Spotify', { drag: true })}</g>`;
    b += mini(bx + 440, 66, 4, 'Game');
    host.innerHTML = svg(W, H, b, 'Strip context menu and the Change Order mode: a strip is being dragged between two others');
  };

  G.fxwin = function (host, k) {
    if (k === 'eq') return G.eq(host);
    const r = FX[k] && FX[k]();
    if (r) host.innerHTML = svg(r[0], r[1], r[2], r[3]);
  };

  /* ---------- Hydra Link: two PCs and a phone ---------- */
  G.link = function (host) {
    const W = 820, H = 380, anim = !reduce.matches;
    let b = R(0, 0, W, H, 10, C.bg);
    const monitor = (x, y, title, sub, col) => {
      let g = R(x, y, 230, 140, 9, '#1b2029', { stroke: '#2e3644' }) + R(x + 8, y + 8, 214, 116, 4, C.panel);
      g += R(x + 14, y + 14, 38, 6, 3, col, { op: 0.9 }) + T(x + 216, y + 21, 'HYDRA', { size: 7, fill: C.faint, anchor: 'end', weight: 700, ls: '.08em' });
      for (let i = 0; i < 8; i++) {   // tiny strips with meters
        const sx = x + 16 + i * 25.5, hgt = 28 + ((i * 37) % 44), c = i < 5 ? C.in : i < 7 ? C.hw : C.virt;
        g += R(sx, y + 28, 22, 90, 2, C.panel2) + R(sx + 3, y + 32, 16, 3, 1.5, c, { op: 0.8 }) + R(sx + 7, y + 114 - hgt, 3, hgt, 1.5, C.good) + R(sx + 12, y + 114 - hgt + 4, 3, hgt - 4, 1.5, C.good);
      }
      g += `<path d="M${x + 100} ${y + 140}l-8 22h46l-8-22z" fill="#1b2029"/>` + R(x + 80, y + 160, 70, 6, 3, '#262d3a');
      g += T(x + 115, y + 190, title, { size: 13, weight: 700, anchor: 'middle' }) + T(x + 115, y + 207, sub, { size: 10, fill: C.dim, anchor: 'middle' });
      return g;
    };
    b += monitor(30, 40, 'Gaming PC', 'mic, game, chat', C.in) + monitor(W - 260, 40, 'Streaming PC', 'OBS, recording', C.virt);
    // router
    const rx = W / 2, ry = 110;
    b += R(rx - 44, ry - 16, 88, 32, 8, '#1b2029', { stroke: '#2e3644' }) + [0, 1, 2].map(i => `<circle cx="${rx - 18 + i * 12}" cy="${ry}" r="2.5" fill="${C.good}"/>`).join('') +
      L(rx - 28, ry - 16, rx - 34, ry - 34, '#2e3644', 3, 'stroke-linecap="round"') + L(rx + 28, ry - 16, rx + 34, ry - 34, '#2e3644', 3, 'stroke-linecap="round"') +
      T(rx, ry + 34, 'Router', { size: 10, fill: C.dim, anchor: 'middle' });
    // phone
    const px = rx - 34, py = 222;
    b += R(px, py, 68, 128, 12, '#1b2029', { stroke: '#2e3644' }) + R(px + 5, py + 9, 58, 110, 7, C.panel);
    [0, 1, 2].forEach(i => { b += R(px + 10, py + 18 + i * 32, 48, 26, 4, C.panel2) + R(px + 15, py + 24 + i * 32, 22, 3, 1.5, C.dim, { op: 0.6 }) + R(px + 15, py + 33 + i * 32, 38 - i * 9, 4, 2, C.good); });
    b += T(rx + 46, py + 60, 'Phone', { size: 13, weight: 700 }) + T(rx + 46, py + 77, 'Hydra Link app', { size: 10, fill: C.dim });
    // links
    const cableL = `M260 ${ry}H${rx - 44}`, cableR = `M${rx + 44} ${ry}H${W - 260}`;
    b += `<path d="${cableL}" stroke="#3a4352" stroke-width="5" stroke-linecap="round"/><path d="${cableR}" stroke="#3a4352" stroke-width="5" stroke-linecap="round"/>`;
    b += T((260 + rx - 44) / 2, ry - 14, 'LAN cable', { size: 10.5, weight: 600, anchor: 'middle' }) + T((260 + rx - 44) / 2, ry + 22, 'PCM 24 · lossless · ~3 ms', { size: 9.5, fill: C.dim, anchor: 'middle' });
    b += T((rx + 44 + W - 260) / 2, ry - 14, 'LAN cable', { size: 10.5, weight: 600, anchor: 'middle' }) + T((rx + 44 + W - 260) / 2, ry + 22, 'found by itself', { size: 9.5, fill: C.dim, anchor: 'middle' });
    [8, 15, 22].forEach((r, i) => { const cy = py - 8, d = r * 0.7; b += `<path d="M${f1(rx - d)} ${f1(cy - d)}A${r} ${r} 0 0 1 ${f1(rx + d)} ${f1(cy - d)}" fill="none" stroke="${C.in}" stroke-width="2.2" stroke-linecap="round" opacity="${0.95 - i * 0.2}"/>`; });
    b += T(rx - 52, py - 26, 'Wi-Fi', { size: 10.5, weight: 600, anchor: 'end' }) + T(rx - 52, py - 11, 'Opus · listen or send the mic', { size: 9.5, fill: C.dim, anchor: 'end' });
    const wifi = `M${rx} ${ry + 16}V${py}`;
    b += `<path d="${wifi}" stroke="${C.in}" stroke-width="1.4" stroke-dasharray="3 5" opacity=".6"/>`;
    // moving packets
    if (anim) {
      const pk = (d, col, dur, begin, rev) => `<circle r="3.4" fill="${col}"><animateMotion dur="${dur}s" begin="${begin}s" repeatCount="indefinite" path="${d}"${rev ? ' keyPoints="1;0" keyTimes="0;1" calcMode="linear"' : ''}/></circle>`;
      [0, 0.45, 0.9].forEach(t => { b += pk(cableL, C.in, 1.35, t) + pk(cableR, C.virt, 1.35, t + 0.2) + pk(wifi, C.in, 1.6, t * 1.2); });
      b += pk(wifi, C.good, 1.6, 0.6, true);
    }
    host.innerHTML = svg(W, H, b, 'Hydra Link: a gaming PC and a streaming PC connected by cable through the router, a phone over Wi-Fi');
  };

  /* ---------- Voicemeeter import: what goes where ---------- */
  G.vm = function (host) {
    const W = 820, H = 368;
    let b = R(0, 0, W, H, 10, C.bg);
    // left: the saved settings file
    b += R(20, 20, 250, 328, 10, C.panel, { stroke: C.outline });
    b += `<path d="M38 38h22l8 8v26H38z" fill="none" stroke="${C.dim}" stroke-width="1.6"/><path d="M60 38v8h8" fill="none" stroke="${C.dim}" stroke-width="1.6"/>` +
      T(42, 66, 'XML', { size: 6.5, fill: C.dim, weight: 700 }) + T(80, 52, 'Voicemeeter settings', { size: 12, weight: 700 }) + T(80, 68, 'Menu → Save Settings', { size: 9.5, fill: C.dim });
    const rowsL = ['Strip 1 · Mic (ASIO 1/2)', 'Strip 2 · Music', 'Virtual input · Game', 'Bus A1 · Speakers', 'Bus B1 · Stream', 'EQ, gate, comp, denoiser', 'MIDI mapping'];
    const rowsR = [['IN 1', 'Mic · Master 1/2', C.in], ['IN 2', 'Music', C.in], ['IN 3', 'Game · placeholder', C.in], ['H1', 'Speakers', C.hw], ['V1', 'Stream', C.virt], ['FX', 'blocks in the chain', C.eq], ['MIDI', 'merged with Hydra’s', C.solo]];
    const ry = i => 96 + i * 36;
    rowsL.forEach((t, i) => { b += R(34, ry(i), 222, 28, 6, C.panel2) + T(46, ry(i) + 18, t, { size: 10.5, fill: C.dim }); });
    // right: Hydra
    b += R(W - 270, 20, 250, 328, 10, C.panel, { stroke: C.outline });
    [[8, 14, 6, 18], [16, 9, 6, 23], [24, 15, 6, 17]].forEach(([x, y, w, h], i) => { b += R(W - 256 + x, 30 + y, w, h, 3, ['#4bc0f5', '#6c8cfa', '#8f7cff'][i]); });
    b += T(W - 218, 52, 'Hydra Mixer', { size: 12, weight: 700 }) + T(W - 218, 68, 'Backup & Transfer → Import', { size: 9.5, fill: C.dim });
    rowsR.forEach(([c, t, col], i) => {
      b += R(W - 256, ry(i), 222, 28, 6, C.panel2, { stroke: mix(C.panel2, col, 0.35) }) + R(W - 248, ry(i) + 6, 34, 16, 4, mix(C.panel2, col, 0.25)) +
        T(W - 231, ry(i) + 18, c, { size: 9, fill: col, weight: 700, anchor: 'middle' }) + T(W - 204, ry(i) + 18, t, { size: 10.5 });    });
    // connectors
    rowsL.forEach((_, i) => {
      const y = ry(i) + 14, col = rowsR[i][2];
      b += `<path d="M258 ${y}C${W / 2 - 40} ${y} ${W / 2 + 40} ${y} ${W - 258} ${y}" fill="none" stroke="${col}" stroke-width="1.6" opacity=".55"/>` +
        `<path d="M${W - 266} ${y - 4}l8 4-8 4z" fill="${col}" opacity=".8"/>`;
    });
    b += R(W / 2 - 56, 186, 112, 28, 14, mix(C.panel, C.in, 0.25), { stroke: C.in }) + T(W / 2, 204, 'Import', { size: 11.5, weight: 700, anchor: 'middle' });
    b += T(W / 2, 40, 'Your Hydra session is backed up first', { size: 9.5, fill: C.faint, anchor: 'middle' });
    host.innerHTML = svg(W, H, b, 'Voicemeeter import: strips become inputs, A buses hardware outputs, B buses virtual outputs, effects become blocks, MIDI mappings are merged');
  };

  function renderAll(root) {
    (root || document).querySelectorAll('[data-g]').forEach(el => {
      if (el.dataset.done) return;
      const fn = G[el.dataset.g];
      if (fn) { fn(el, el.dataset.o || ''); el.dataset.done = '1'; }
    });
  }
  window.HydraUI = { renderAll };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => renderAll()); else renderAll();
})();
