/* Pixel Art's page in the board's portal (docs/PORTAL-PAGES.md), moved out
 * of the firmware's app.js on 2026-10-04 as it was: tools/install_apps.sh
 * puts this file in the card's /web and the portal loads it from there.
 * window.P4OS (version 1) is all it uses, and its styles come with it. */
const P = window.P4OS;
const { main, h, put, $, api, post, toast, row, fmtBytes, fmtDate, fsPut, fsList, fsDelete, fsMkdir, fsUrl,
  fsText, fsBytes, openApp, saveBlob, fsSlug } = P;

if (!document.getElementById('css-pixel')) {
  const st = document.createElement('style');
  st.id = 'css-pixel';
  st.textContent = `
.pxgrid { display: grid; gap: 12px; grid-template-columns: repeat(auto-fill, minmax(160px, 1fr)); }
.pxslot { background: var(--card); border-radius: 14px; padding: 12px; cursor: pointer; display: flex; flex-direction: column; align-items: center; gap: 6px;
  border: 1px solid transparent; }
.pxslot:hover { border-color: var(--accent); }
.pxslot canvas { width: 128px; height: 128px; image-rendering: pixelated; border-radius: 8px; background: #000; }
.pxplus { width: 128px; height: 128px; border-radius: 8px; border: 2px dashed var(--line); display: flex; align-items: center; justify-content: center; font-size: 40px; color: var(--dim); }
.pxacts { min-height: 30px; justify-content: center; }
.pxacts .btn { padding: 4px 10px; font-size: 12.5px; }
.pxthumb { width: 48px; height: 48px; image-rendering: pixelated; border-radius: 6px; background: #000; flex: none; object-fit: contain; }
.pxcols { display: grid; grid-template-columns: minmax(280px, 560px) minmax(260px, 1fr); gap: 18px; align-items: start; }
.pxcv { width: 100%; aspect-ratio: 1; display: block; border-radius: 10px; image-rendering: pixelated; touch-action: none; cursor: crosshair; background: #000; }
.pxside { display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.pxpal { display: grid; grid-template-columns: repeat(8, 1fr); gap: 6px; max-width: 340px; }
.pxpal i { display: block; aspect-ratio: 1; border-radius: 8px; cursor: pointer; border: 2px solid var(--line); }
.pxpal i.on { border-color: var(--text); outline: 2px solid var(--accent); }
.pxstrip { display: flex; gap: 8px; overflow-x: auto; padding: 4px 2px 8px; }
.pxframe { flex: none; position: relative; cursor: pointer; }
.pxframe canvas { width: 64px; height: 64px; border-radius: 6px; image-rendering: pixelated; display: block; border: 2px solid transparent; background: #000; }
.pxframe.on canvas { border-color: var(--accent); }
.pxframe span { position: absolute; left: 4px; top: 4px; font-size: 11px; background: rgba(0,0,0,.6); color: #fff; padding: 1px 5px; border-radius: 6px; }
@media (max-width: 800px) { .pxcols { grid-template-columns: 1fr; } }
`;
  document.head.append(st);
}

/* ---- Pixel Art ---- */
/* The canvases of the Pixel Art app (apps/pixel), in /pixel on the card:
 * lienzo1.pix .. lienzo12.pix, and what the app exported (lienzoN.gif,
 * lienzoN-F.png). Ported from AmoledOS's /pixel page (pixel.html), with the
 * P4's twelve slots and its 32x32 and 64x64. The .pix format is the contract
 * with apps/pixel/main/px_file.h, written on both sides on purpose: the
 * firmware only keeps bytes. The app notices a file changing on the card
 * (every 3 s) and reloads it, so what is saved here shows on the board.
 *   "PIX1", size, frames, ncolors, flags, delay u16, 0 u16, palette RGB888, frames*size*size indices */
const PX_DIR = '/pixel', PX_SLOTS = 12, PX_FRAMES = 16, PX_SIZES = [8, 16, 32, 64];
const PX_PAL = [
  [0x00, 0x00, 0x00], [0xFF, 0xFF, 0xFF], [0xB0, 0xB0, 0xB8], [0x5A, 0x5A, 0x64], [0xFF, 0x3B, 0x30], [0xA8, 0x14, 0x1E], [0xFF, 0x8A, 0x1E], [0xC8, 0x50, 0x00],
  [0xFF, 0xD6, 0x0A], [0xFF, 0xF4, 0x8C], [0x30, 0xD1, 0x58], [0x14, 0x7A, 0x32], [0xA6, 0xF0, 0x5A], [0x00, 0xC8, 0xBE], [0x5A, 0xC8, 0xFA], [0x0A, 0x84, 0xFF],
  [0x10, 0x3C, 0xA0], [0xAF, 0x52, 0xDE], [0x5E, 0x1E, 0x8C], [0xFF, 0x2D, 0x95], [0xFF, 0xB3, 0xC8], [0x8B, 0x46, 0x18], [0x50, 0x28, 0x0A], [0xC8, 0x8A, 0x46],
  [0xE6, 0xB8, 0x8A], [0xFF, 0xDC, 0xB4], [0xD2, 0x96, 0x6E], [0x8C, 0x5A, 0x3C], [0xFF, 0xE4, 0xE1], [0xC8, 0xE6, 0xFF], [0xD4, 0xA0, 0x17], [0x2A, 0x2A, 0x32]];
const pxCss = i => { const c = PX_PAL[i]; return `rgb(${c[0]},${c[1]},${c[2]})`; };
const pxSlotName = n => 'lienzo' + n + '.pix';

/* px_nearest(): weighted like the eye, green counts most */
function pxNearest(c) {
  let best = 0, bd = 1e12;
  PX_PAL.forEach((q, i) => { const d = 2 * (c[0] - q[0]) ** 2 + 4 * (c[1] - q[1]) ** 2 + 3 * (c[2] - q[2]) ** 2; if (d < bd) { bd = d; best = i; } });
  return best;
}
function pxParse(buf) {
  const b = new Uint8Array(buf);
  if (b.length < 12 || String.fromCharCode(b[0], b[1], b[2], b[3]) !== 'PIX1') return null;
  const size = b[4], frames = b[5], ncol = b[6] || 256;
  if (!PX_SIZES.includes(size) || frames < 1 || frames > PX_FRAMES) return null;
  const cells = size * size;
  if (b.length < 12 + ncol * 3 + frames * cells) return null;
  const delay = b[8] | (b[9] << 8), pal = [];
  let p = 12;
  for (let i = 0; i < ncol; i++, p += 3) pal.push([b[p], b[p + 1], b[p + 2]]);
  /* a foreign palette mapped onto ours by nearest colour, as the app does */
  const map = pal.map(pxNearest), same = ncol === PX_PAL.length && map.every((m, i) => m === i);
  const fr = [];
  for (let f = 0; f < frames; f++, p += cells) {
    const a = new Uint8Array(cells);
    for (let i = 0; i < cells; i++) { const v = b[p + i]; a[i] = v < ncol ? (same ? v : map[v]) : 0; }
    fr.push(a);
  }
  return { size, frames: fr, delay: delay >= 50 && delay <= 2000 ? delay : 200 };
}
function pxBuild(doc) {
  const cells = doc.size * doc.size, out = new Uint8Array(12 + PX_PAL.length * 3 + doc.frames.length * cells);
  out.set([80, 73, 88, 49, doc.size, doc.frames.length, PX_PAL.length, 0, doc.delay & 255, doc.delay >> 8, 0, 0]);
  let p = 12;
  PX_PAL.forEach(c => { out.set(c, p); p += 3; });
  doc.frames.forEach(f => { out.set(f, p); p += cells; });
  return out;
}
function pxPaint(canvas, d, f, grid) {
  const c = canvas.getContext('2d'), cell = canvas.width / d.size, inset = grid && cell >= 6 ? 1 : 0;
  c.fillStyle = inset ? '#3a3a3e' : '#000';
  c.fillRect(0, 0, canvas.width, canvas.height);
  const px = d.frames[f];
  for (let y = 0; y < d.size; y++) for (let x = 0; x < d.size; x++) {
    c.fillStyle = pxCss(px[y * d.size + x]);
    c.fillRect(Math.round(x * cell) + inset, Math.round(y * cell) + inset, Math.round((x + 1) * cell) - Math.round(x * cell) - inset, Math.round((y + 1) * cell) - Math.round(y * cell) - inset);
  }
}
/* the exports: 512 px a side as the app writes them, or the size chosen */
const pxScale = (d, side) => Math.max(1, Math.round(side / d.size));
function pxPng(d, f, side) {
  const s = pxScale(d, side), c = h('canvas', { width: d.size * s, height: d.size * s });
  pxPaint(c, d, f, false);
  return new Promise(r => c.toBlob(r, 'image/png'));
}
/* GIF: LZW as the format asks, as px_export.c does */
function pxGif(d, side) {
  const esc = pxScale(d, side), w = d.size * esc, out = [];
  const u16 = v => out.push(v & 255, v >> 8);
  out.push(71, 73, 70, 56, 57, 97);                            /* GIF89a */
  u16(w); u16(w); out.push(0xF4, 0, 0);                         /* global table of 32 colours */
  PX_PAL.forEach(c => out.push(c[0], c[1], c[2]));
  if (d.frames.length > 1) out.push(0x21, 0xFF, 0x0B, 78, 69, 84, 83, 67, 65, 80, 69, 50, 46, 48, 3, 1, 0, 0, 0);
  const cs = Math.max(2, Math.round(d.delay / 10));
  const px = new Uint8Array(w * w);
  d.frames.forEach(f => {
    out.push(0x21, 0xF9, 4, 4, cs & 255, cs >> 8, 0, 0);
    out.push(0x2C); u16(0); u16(0); u16(w); u16(w); out.push(0);
    out.push(5);                                                 /* min code size */
    for (let y = 0; y < w; y++) for (let x = 0; x < w; x++) px[y * w + x] = f[((y / esc) | 0) * d.size + ((x / esc) | 0)];
    pxLzw(px, 5, out);
    out.push(0);
  });
  out.push(0x3B);
  return new Blob([new Uint8Array(out)], { type: 'image/gif' });
}
function pxLzw(px, min, out) {
  const clear = 1 << min, eoi = clear + 1;
  let width = min + 1, free = eoi + 1, dict = new Map(), acc = 0, nbits = 0, block = [];
  const emit = code => {
    acc |= code << nbits; nbits += width;
    while (nbits >= 8) {
      block.push(acc & 255); acc >>>= 8; nbits -= 8;
      if (block.length === 255) { out.push(255); for (const b of block) out.push(b); block = []; }
    }
  };
  emit(clear);
  let prefix = px[0];
  for (let i = 1; i < px.length; i++) {
    const c = px[i], key = (prefix << 8) | c, hit = dict.get(key);
    if (hit !== undefined) { prefix = hit; continue; }
    emit(prefix);
    if (free < 4096) { dict.set(key, free++); if (free > (1 << width) && width < 12) width++; }
    else { emit(clear); dict = new Map(); width = min + 1; free = eoi + 1; }
    prefix = c;
  }
  emit(prefix);
  emit(eoi);
  if (nbits > 0) block.push(acc & 255);
  if (block.length) { out.push(block.length); for (const b of block) out.push(b); }
}
/* A picture from the computer as one frame: fitted into the smallest size
 * that holds it (64 at most), centred on black, each pixel to the nearest
 * colour of the palette. */
async function pxFromImage(file) {
  const bmp = await createImageBitmap(file);
  const size = PX_SIZES.find(s => s >= Math.max(bmp.width, bmp.height)) || 64;
  const k = Math.min(size / bmp.width, size / bmp.height), w = Math.max(1, Math.round(bmp.width * k)), hh = Math.max(1, Math.round(bmp.height * k));
  const c = h('canvas', { width: size, height: size }), g = c.getContext('2d');
  g.imageSmoothingEnabled = k < 1;
  g.drawImage(bmp, (size - w) >> 1, (size - hh) >> 1, w, hh);
  const d = g.getImageData(0, 0, size, size).data, f = new Uint8Array(size * size);
  for (let i = 0; i < f.length; i++) f[i] = d[i * 4 + 3] < 128 ? 0 : pxNearest([d[i * 4], d[i * 4 + 1], d[i * 4 + 2]]);
  return { size, delay: 200, frames: [f] };
}

let pxPlayT;
async function pagePixel() {
  let files = [], doc = null, slot = 0, cur = 0, tool = 'pen', color = 1, dirty = false, undo = [], playing = null, painting = false, lastCell = -1, side = 512;
  const gallery = h('div', {}), editor = h('div', { style: 'display:none' });
  const slots = h('div', { class: 'pxgrid' }), exports = h('div', { class: 'card' });
  const cv = h('canvas', { class: 'pxcv', width: 512, height: 512 }), ctx = cv.getContext('2d');
  const title = h('b', {}), status = h('span', { class: 'muted small' });
  const pal = h('div', { class: 'pxpal' }), strip = h('div', { class: 'pxstrip' });
  const delay = h('input', { type: 'range', min: 50, max: 1000, step: 10 }), delayv = h('span', { class: 'muted small', style: 'min-width:60px;text-align:right' });
  const playBtn = h('button', { class: 'btn' }, '▶');
  const undoBtn = h('button', { class: 'btn' }, 'Deshacer'), delFrame = h('button', { class: 'btn red' }, 'Borrar cuadro');
  const sizeSel = h('select', { style: 'width:auto', onchange: e => { side = +e.target.value; } },
    [256, 512, 1024].map(s => h('option', { value: s, selected: s === side }, s + ' px')));
  const tools = { pen: h('button', { class: 'btn pri' }, 'Lápiz'), fill: h('button', { class: 'btn' }, 'Rellenar'), pick: h('button', { class: 'btn' }, 'Tomar color') };
  const say = (t, cls) => { status.textContent = t; status.className = 'small ' + (cls || 'muted'); };

  /* ---- the gallery ---- */
  async function read(name) { const d = pxParse(await fsBytes(PX_DIR + '/' + name)); if (!d) throw new Error(name + ' no se lee'); return d; }
  async function load() {
    try { files = await fsList(PX_DIR); } catch (e) { put(slots, h('p', { class: 'bad' }, e.message)); return; }
    const anim = [];
    put(slots, Array.from({ length: PX_SLOTS }, (_, i) => {
      const n = i + 1, f = files.find(a => a.name === pxSlotName(n));
      if (!f) return h('div', { class: 'pxslot', onclick: () => ask(n) }, h('div', { class: 'pxplus' }, '+'), h('b', {}, 'Lienzo ' + n), h('span', { class: 'muted small' }, 'vacío'));
      const c = h('canvas', { width: 128, height: 128 }), sub = h('span', { class: 'muted small' }, '…');
      const acts = h('div', { class: 'btns pxacts', onclick: e => e.stopPropagation() });
      read(f.name).then(d => {
        pxPaint(c, d, 0, false);
        sub.textContent = `${d.size}×${d.size} · ${d.frames.length} ${d.frames.length === 1 ? 'cuadro' : 'cuadros'}`;
        if (d.frames.length > 1) anim.push({ c, d, f: 0, t: 0 });
        put(acts, h('button', { class: 'btn', onclick: async () => saveBlob(await pxPng(d, 0, side), `lienzo${n}.png`) }, 'PNG'),
          d.frames.length > 1 ? h('button', { class: 'btn', onclick: () => saveBlob(pxGif(d, side), `lienzo${n}.gif`) }, 'GIF') : null);
      }).catch(e => { sub.textContent = e.message; sub.className = 'bad small'; });
      return h('div', { class: 'pxslot', onclick: () => open(n) }, c, h('b', {}, 'Lienzo ' + n), sub, acts);
    }));
    /* the thumbnails of the animations play, each at its own speed */
    clearInterval(pxPlayT);
    pxPlayT = setInterval(() => {
      if (location.hash !== '#pixel' || !slots.isConnected) { clearInterval(pxPlayT); return; }
      if (gallery.style.display === 'none' || document.hidden) return;
      const now = performance.now();
      for (const a of anim) if (now - a.t >= a.d.delay) { a.f = (a.f + 1) % a.d.frames.length; a.t = now; pxPaint(a.c, a.d, a.f, false); }
    }, 50);
    const ex = files.filter(a => !a.dir && /\.(png|gif)$/i.test(a.name)).sort((a, b) => a.name.localeCompare(b.name, 'es', { numeric: true }));
    put(exports, ex.length ? ex.map(a => h('div', { class: 'row' },
      h('img', { class: 'pxthumb', src: fsUrl(PX_DIR + '/' + a.name) + '&_=' + (a.mtime || a.size), alt: '', loading: 'lazy' }),
      h('div', { class: 'grow' }, h('div', { class: 'mono ell' }, a.name), h('div', { class: 'muted small' }, fmtBytes(a.size) + (a.mtime ? ' · ' + fmtDate(a.mtime) : ''))),
      h('div', { class: 'btns' }, h('a', { class: 'btn', href: fsUrl(PX_DIR + '/' + a.name, true) }, 'Bajar'),
        h('button', { class: 'btn red', onclick: async () => {
          if (!confirm(`¿Borrar ${a.name}?`)) return;
          try { await fsDelete(PX_DIR + '/' + a.name); load(); } catch (e) { toast(e.message, true); }
        } }, 'Borrar')))) :
      h('div', { class: 'row muted' }, 'Todavía no hay nada exportado. Desde el menú de la app en la placa, o con PNG y GIF de acá.'));
  }
  /* an empty slot asks for the size */
  function ask(n) {
    const close = () => md.remove();
    const md = h('div', { class: 'modal', onclick: e => { if (e.target === md) close(); } },
      h('div', { class: 'box', style: 'width:min(420px,100%)' }, h('div', { class: 'head' }, h('b', { class: 'grow' }, 'Lienzo ' + n + ' nuevo')),
        h('div', { class: 'pad btns', style: 'justify-content:center' }, PX_SIZES.map(s => h('button', { class: 'btn pri', style: 'padding:16px 20px;font-size:17px', onclick: () => {
          close(); edit(n, { size: s, delay: 200, frames: [new Uint8Array(s * s)] }, true);
        } }, s + ' × ' + s))),
        h('div', { class: 'foot' }, h('button', { class: 'btn', onclick: close }, 'Cancelar'))));
    document.body.append(md);
  }
  async function open(n) {
    try { edit(n, await read(pxSlotName(n)), false); } catch (e) { toast(e.message, true); }
  }
  /* what comes from the computer: a .pix as it is, a picture as one frame, into the first free slot */
  async function uploadPx(fl) {
    const list = [...fl];
    for (const f of list) {
      const n = Array.from({ length: PX_SLOTS }, (_, i) => i + 1).find(k => !files.some(a => a.name === pxSlotName(k)));
      if (!n) { toast('No hay lienzos libres: borrá uno primero', true); return; }
      try {
        if (/\.pix$/i.test(f.name)) {
          const buf = await f.arrayBuffer(), d = pxParse(buf);
          if (!d) throw new Error('no es un .pix que se pueda leer');
          await fsMkdir(PX_DIR);
          await fsPut(PX_DIR + '/' + pxSlotName(n), pxBuild(d));
          files.push({ name: pxSlotName(n) });
          toast(f.name + ' → lienzo ' + n);
        } else if (/^image\//.test(f.type)) {
          edit(n, await pxFromImage(f), true);
          say(`${f.name}, pasado a la paleta: guardalo si te gusta`);
          return;
        } else throw new Error('sólo .pix o imágenes');
      } catch (e) { toast(f.name + ': ' + e.message, true); return; }
    }
    load();
  }

  /* ---- the editor ---- */
  function redraw() {
    pxPaint(cv, doc, cur, true);
    paintStrip();
    title.textContent = `Lienzo ${slot} · ${doc.size}×${doc.size} · cuadro ${cur + 1}/${doc.frames.length}`;
    delay.value = doc.delay; delayv.textContent = doc.delay + ' ms';
    delFrame.disabled = doc.frames.length < 2;
    undoBtn.disabled = !undo.length;
    if (dirty && !status.textContent) say('cambios sin guardar');
  }
  function paintStrip() {
    put(strip, doc.frames.map((f, i) => {
      const c = h('canvas', { width: 64, height: 64 });
      pxPaint(c, doc, i, false);
      return h('div', { class: 'pxframe' + (i === cur ? ' on' : ''), onclick: () => { stop(); cur = i; undo = []; redraw(); } }, c, h('span', {}, i + 1));
    }));
    const on = strip.querySelector('.on');
    if (on) on.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }
  const mark = () => { dirty = true; say('cambios sin guardar'); };
  const keepUndo = () => { undo.push(new Uint8Array(doc.frames[cur])); if (undo.length > 50) undo.shift(); undoBtn.disabled = false; };
  function cellOf(ev) {
    const r = cv.getBoundingClientRect();
    const x = Math.floor((ev.clientX - r.left) / r.width * doc.size), y = Math.floor((ev.clientY - r.top) / r.height * doc.size);
    return x < 0 || y < 0 || x >= doc.size || y >= doc.size ? null : { x, y, i: y * doc.size + x };
  }
  function fill(f, x, y, c) {
    const n = doc.size, px = doc.frames[f], from = px[y * n + x];
    if (from === c) return;
    const st = [y * n + x];
    px[y * n + x] = c;
    while (st.length) {
      const i = st.pop(), cx = i % n, cy = (i / n) | 0;
      for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
        const nx = cx + dx, ny = cy + dy;
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
        const j = ny * n + nx;
        if (px[j] === from) { px[j] = c; st.push(j); }
      }
    }
  }
  /* a stroke joined cell to cell, as the app does: a fast mouse draws a line and not dots */
  function stroke(c) {
    const px = doc.frames[cur], cell = cv.width / doc.size, inset = cell >= 6 ? 1 : 0;
    const pts = [];
    if (lastCell >= 0) {
      let x0 = lastCell % doc.size, y0 = (lastCell / doc.size) | 0;
      const dx = Math.abs(c.x - x0), dy = -Math.abs(c.y - y0), sx = x0 < c.x ? 1 : -1, sy = y0 < c.y ? 1 : -1;
      let err = dx + dy;
      for (;;) { pts.push(y0 * doc.size + x0); if (x0 === c.x && y0 === c.y) break; const e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; } }
    } else pts.push(c.i);
    lastCell = c.i;
    for (const i of pts) {
      if (px[i] === color) continue;
      px[i] = color;
      const x = i % doc.size, y = (i / doc.size) | 0;
      ctx.fillStyle = pxCss(color);
      ctx.fillRect(Math.round(x * cell) + inset, Math.round(y * cell) + inset, Math.round((x + 1) * cell) - Math.round(x * cell) - inset, Math.round((y + 1) * cell) - Math.round(y * cell) - inset);
      mark();
    }
  }
  cv.onpointerdown = ev => {
    if (!doc) return;
    stop();
    const c = cellOf(ev);
    if (!c) return;
    cv.setPointerCapture(ev.pointerId);
    const px = doc.frames[cur];
    if (tool === 'pick') { pick(px[c.i]); return; }
    if (tool === 'fill') { if (px[c.i] !== color) { keepUndo(); fill(cur, c.x, c.y, color); mark(); redraw(); } return; }
    keepUndo();
    painting = true; lastCell = -1;
    stroke(c);
  };
  cv.onpointermove = ev => { if (painting) { const c = cellOf(ev); if (c && c.i !== lastCell) stroke(c); } };
  cv.onpointerup = cv.onpointercancel = () => { if (painting) { painting = false; paintStrip(); } };
  function setTool(t) { tool = t; for (const k in tools) tools[k].className = 'btn' + (k === t ? ' pri' : ''); }
  function pick(i) { color = i; [...pal.children].forEach((s, k) => s.classList.toggle('on', k === i)); if (tool === 'pick') setTool('pen'); }
  for (const k in tools) tools[k].onclick = () => setTool(k);
  put(pal, PX_PAL.map((_, i) => h('i', { style: 'background:' + pxCss(i), title: i, onclick: () => pick(i) })));
  undoBtn.onclick = () => { if (!undo.length) return; doc.frames[cur] = undo.pop(); mark(); redraw(); };
  function insert(f) {
    stop();
    if (doc.frames.length >= PX_FRAMES) { toast('16 cuadros como máximo', true); return; }
    doc.frames.splice(cur + 1, 0, f); cur++; undo = []; mark(); redraw();
  }
  delFrame.onclick = () => { stop(); if (doc.frames.length < 2) return; doc.frames.splice(cur, 1); cur = Math.min(cur, doc.frames.length - 1); undo = []; mark(); redraw(); };
  delay.oninput = () => { doc.delay = +delay.value; delayv.textContent = doc.delay + ' ms'; mark(); if (playing) { stop(); play(); } };
  function play() {
    if (doc.frames.length < 2) { toast('Hace falta más de un cuadro', true); return; }
    playBtn.textContent = '■';
    playing = setInterval(() => { if (!cv.isConnected) { stop(); return; } cur = (cur + 1) % doc.frames.length; redraw(); }, doc.delay);
  }
  function stop() { if (!playing) return; clearInterval(playing); playing = null; playBtn.textContent = '▶'; }
  playBtn.onclick = () => { if (playing) stop(); else play(); };
  async function save() {
    stop();
    say('guardando…');
    try {
      await fsMkdir(PX_DIR);
      await fsPut(PX_DIR + '/' + pxSlotName(slot), pxBuild(doc));
      dirty = false;
      say('guardado en la placa', 'ok');
    } catch (e) { say('no se pudo guardar: ' + e.message, 'bad'); }
  }
  async function delCanvas() {
    stop();
    if (!confirm(`¿Borrar el lienzo ${slot} de la placa?`)) return;
    try { await fsDelete(PX_DIR + '/' + pxSlotName(slot)).catch(e => { if (!/no existe|No such/.test(e.message)) throw e; }); dirty = false; toast('Lienzo ' + slot + ' borrado'); back(true); }
    catch (e) { toast(e.message, true); }
  }
  function edit(n, d, isNew) {
    doc = d; slot = n; cur = 0; undo = []; dirty = isNew; say('');
    setTool('pen'); pick(color);
    gallery.style.display = 'none'; editor.style.display = '';
    redraw();
    window.scrollTo(0, 0);
  }
  function back(force) {
    if (dirty && !force && !confirm('Hay cambios sin guardar. ¿Salir igual?')) return;
    stop(); doc = null; dirty = false;
    editor.style.display = 'none'; gallery.style.display = '';
    load();
  }

  const input = h('input', { type: 'file', multiple: true, accept: '.pix,image/*', style: 'display:none', onchange: e => uploadPx(e.target.files) });
  const drop = h('div', { class: 'drop' }, 'Soltá acá un .pix, o una imagen para pasarla a la paleta (a 8, 16, 32 o 64 de lado)');
  drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); uploadPx(e.dataTransfer.files); };
  put(gallery, h('div', { class: 'btns', style: 'margin-bottom:14px' },
      h('button', { class: 'btn pri', onclick: () => openApp('aos.pixel').then(() => toast('Abriendo Pixel Art en la placa'), e => toast(e.message, true)) }, 'Abrir Pixel Art en la placa'),
      h('button', { class: 'btn', onclick: () => input.click() }, 'Subir…'), input,
      h('span', { class: 'muted small' }, 'PNG y GIF de:'), sizeSel),
    slots, drop, h('h2', {}, 'Exportados en la tarjeta'), exports);
  put(editor, h('div', { class: 'btns', style: 'margin-bottom:14px' }, h('button', { class: 'btn', onclick: () => back(false) }, '← Lienzos'), title, status),
    h('div', { class: 'pxcols' },
      h('div', { style: 'min-width:0' }, cv),
      h('div', { class: 'pxside' },
        h('div', { class: 'btns' }, tools.pen, tools.fill, tools.pick, undoBtn),
        pal,
        h('div', { class: 'card pad' }, h('h2', { style: 'margin:0 0 8px' }, 'Cuadros'), strip,
          h('div', { class: 'btns', style: 'margin-top:8px' },
            h('button', { class: 'btn', onclick: () => insert(new Uint8Array(doc.frames[cur])) }, 'Duplicar'),
            h('button', { class: 'btn', onclick: () => insert(new Uint8Array(doc.size * doc.size)) }, 'En blanco'),
            h('button', { class: 'btn', onclick: () => { stop(); keepUndo(); doc.frames[cur].fill(0); mark(); redraw(); } }, 'Limpiar'), delFrame),
          h('div', { class: 'btns', style: 'margin-top:12px;flex-wrap:nowrap' }, h('span', { class: 'muted small' }, 'Velocidad'), delay, delayv, playBtn)),
        h('div', { class: 'btns' },
          h('button', { class: 'btn pri', onclick: save }, 'Guardar en la placa'),
          h('button', { class: 'btn', onclick: async () => saveBlob(await pxPng(doc, cur, side), `lienzo${slot}-${cur + 1}.png`) }, 'PNG'),
          h('button', { class: 'btn', onclick: () => saveBlob(pxGif(doc, side), `lienzo${slot}.gif`) }, 'GIF'),
          h('button', { class: 'btn', onclick: () => saveBlob(new Blob([pxBuild(doc)]), pxSlotName(slot)) }, '.pix'),
          h('button', { class: 'btn red', onclick: delCanvas }, 'Borrar de la placa')))));
  put(main, h('h1', {}, 'Pixel Art'), gallery, editor,
    h('p', { class: 'note' }, 'Los .pix viven en /pixel de la tarjeta, doce lienzos de 8, 16, 32 o 64 de lado con hasta 16 cuadros. La app los relee sola cuando cambian: si la tenés abierta en ese lienzo, se actualiza en la pantalla. Los de 8 y 16 pasan tal cual a un reloj con AmoledOS.'));
  window.onbeforeunload = () => dirty && location.hash === '#pixel' ? 'Hay cambios sin guardar' : undefined;
  load();
}

P.registerPage({ id: 'pixel', name: 'Pixel Art', icon: '▚', render: () => { pagePixel(); } });
