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

  // How it works: input strip heads, output heads, routing as lines (interactive).
  G.routing = function (host) {
    const W = 920, H = 500, iy = 34, oy = 372;
    const ix0 = (W - SW * INS.length) / 2;
    const ox = [112, 220, 328, 484, 592, 700];
    let body = `<rect width="${W}" height="${H}" fill="${C.bg}"/>`;
    body += pane(ix0 - 6, iy - 6, SW * INS.length + 12, 202, C.in) + T(ix0, iy - 14, 'Inputs', { size: 12.5, fill: C.in, weight: 700 });
    body += pane(ox[0] - 6, oy - 6, SW * 3 + 12, 78, C.hw) + pane(ox[3] - 6, oy - 6, SW * 3 + 12, 78, C.virt);
    body += T(ox[0], oy + 94, 'Hardware Outputs', { size: 12.5, fill: C.hw, weight: 700 });
    body += T(ox[3], oy + 94, 'Virtual Outputs', { size: 12.5, fill: C.virt, weight: 700 });
    let wires = '';
    INS.forEach((s, i) => s.routes.forEach(o => {
      const x1 = ix0 + i * SW + SW / 2, y1 = iy + 192, x2 = ox[o] + SW / 2, y2 = oy - 8, my = (y1 + y2) / 2;
      wires += `<path class="w${s.muted ? ' dash' : ''}" data-i="${i}" data-o="${o}" d="M${f1(x1)} ${y1}C${f1(x1)} ${my} ${f1(x2)} ${my} ${f1(x2)} ${y2}" fill="none" stroke="${OUTS[o].hw ? C.hw : C.virt}"/>`;
    }));
    body += `<g>${wires}</g>`;
    INS.forEach((s, i) => {
      const x = ix0 + i * SW;
      body += `<g class="node" data-i="${i}" tabindex="0">${inputHead(x, iy, s, OUTS)}` +
        (i ? L(x, iy + 4, x, iy + 186, C.outline) : '') + `<rect class="hl" x="${x + 1}" y="${iy + 1}" width="${SW - 2}" height="188" rx="4" fill="none"/></g>`;
    });
    OUTS.forEach((b, o) => {
      body += `<g class="node" data-o="${o}" tabindex="0">${outputHead(ox[o], oy, b)}` +
        (o % 3 ? L(ox[o], oy + 4, ox[o], oy + 62, C.outline) : '') + `<rect class="hl" x="${ox[o] + 1}" y="${oy + 1}" width="${SW - 2}" height="64" rx="4" fill="none"/></g>`;
    });
    host.innerHTML = svg(W, H, body, 'Routing of the example: which input strips feed which outputs');
    const el = host.querySelector('svg'), paths = [...el.querySelectorAll('path.w')];
    el.querySelectorAll('g.node').forEach(n => {
      const key = n.dataset.i !== undefined ? ['i', n.dataset.i] : ['o', n.dataset.o];
      const on = () => { el.classList.add('focus'); paths.forEach(p => p.classList.toggle('on', p.dataset[key[0]] === key[1])); };
      const off = () => { el.classList.remove('focus'); paths.forEach(p => p.classList.remove('on')); };
      n.addEventListener('mouseenter', on); n.addEventListener('mouseleave', off);
      n.addEventListener('focus', on); n.addEventListener('blur', off);
      n.addEventListener('touchstart', on, { passive: true });
    });
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
    b += eqGraph(44, 56, W - 72, 262, MIC_EQ, { dbLabels: true, sel: true });
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
    host.innerHTML = svg(W, H, b, "Hydra's large EQ window: curve with four bands, one column of controls per band, a column to add a band");
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
