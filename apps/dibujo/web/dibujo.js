/* Dibujo's page in the board's portal (docs/PORTAL-PAGES.md).
 *
 * The drawings live in /dibujo on the card as .dib files: layers of 64x64
 * tiles, written by apps/dibujo/main/dib_doc.c (its header has the format,
 * the contract with this file). The page reads them as they are:
 *
 *   - the gallery shows the preview inside each file, read with a Range
 *     request (the first 51 KB, not the whole drawing);
 *   - a drawing opened here is put together in the browser from its layers,
 *     at full size, and goes down as a PNG made by the browser; its layers
 *     can be hidden for that PNG without touching the file;
 *   - the PNGs the app exported, to see and download;
 *   - pictures uploaded to /dibujo/importar, which the app offers in
 *     "Importar una imagen". They are made baseline JPEG (or PNG when they
 *     have transparency) and no bigger than 1280 px here, because the board's
 *     decoder does not take progressive JPEGs and a drawing is never bigger.
 *
 * Only window.P4OS (version 1). */
const P = window.P4OS;
const { h, put, toast, fmtBytes, fmtDate, fsList, fsBytes, fsPut, fsDelete, fsMkdir, fsUrl, fsSlug, saveBlob, openApp } = P;

const DIR = '/dibujo', IMP = '/dibujo/importar', TILE = 64, PREVIEW = 160;

if (!document.getElementById('css-dibujo')) {
  const st = document.createElement('style');
  st.id = 'css-dibujo';
  st.textContent = `
.djgrid { display: grid; gap: 12px; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); }
.djcard { background: var(--card); border-radius: 14px; padding: 10px; cursor: pointer; display: flex; flex-direction: column; align-items: center; gap: 6px; border: 1px solid transparent; min-width: 0; }
.djcard:hover, .djcard.on { border-color: var(--accent); }
.djcard canvas { width: 128px; height: 128px; object-fit: contain; border-radius: 6px; }
.djcard canvas[width="1"] { visibility: hidden; }
.djcard b { font-weight: 600; }
.djview { display: grid; grid-template-columns: minmax(0, 1fr) minmax(220px, 300px); gap: 16px; align-items: start; }
.djbig { max-width: 100%; max-height: 75vh; width: auto; height: auto; margin: 0 auto; border-radius: 10px; display: block;
  background: repeating-conic-gradient(#d0d0d4 0% 25%, #fff 0% 50%) 0 0 / 24px 24px; }
.djside { display: flex; flex-direction: column; gap: 12px; min-width: 0; }
.djlayer { display: flex; align-items: center; gap: 10px; padding: 6px 0; border-bottom: 1px solid var(--line); }
.djlayer:last-child { border-bottom: 0; }
.djlayer label { display: flex; align-items: center; gap: 8px; cursor: pointer; flex: 1; min-width: 0; }
.djthumb { width: 48px; height: 48px; object-fit: cover; border-radius: 6px; flex: none;
  background: repeating-conic-gradient(#d0d0d4 0% 25%, #fff 0% 50%) 0 0 / 12px 12px; }
.djrow { flex-wrap: wrap; }
.djrow .grow { min-width: 140px; }
.djell { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
@media (max-width: 760px) { .djview { grid-template-columns: 1fr; } }
`;
  document.head.append(st);
}

/* ---- the .dib file ---- */

const u16 = (dv, o) => dv.getUint16(o, true), u32 = (dv, o) => dv.getUint32(o, true);

/* The header and the preview, from the start of a file. */
function djHead(buf) {
  const u = new Uint8Array(buf), dv = new DataView(buf);
  if (u.length < 20 || String.fromCharCode(u[0], u[1], u[2], u[3]) !== 'DIB1') return null;
  const d = { w: u16(dv, 4), h: u16(dv, 6), nl: u[8], active: u[9], ng: u[10], bg: u32(dv, 12), pw: u16(dv, 16), ph: u16(dv, 18) };
  if (d.pw > PREVIEW || d.ph > PREVIEW || !d.pw || !d.ph) return null;
  return d;
}

function djPreview(buf, d, canvas) {
  canvas.width = d.pw;
  canvas.height = d.ph;
  const ctx = canvas.getContext('2d'), img = ctx.createImageData(d.pw, d.ph), dv = new DataView(buf);
  for (let i = 0; i < d.pw * d.ph && 20 + i * 2 + 1 < buf.byteLength; i++) {
    const c = u16(dv, 20 + i * 2);
    img.data[i * 4] = ((c >> 11) & 31) * 255 / 31;
    img.data[i * 4 + 1] = ((c >> 5) & 63) * 255 / 63;
    img.data[i * 4 + 2] = (c & 31) * 255 / 31;
    img.data[i * 4 + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
}

/* The whole file: layers with their packed tiles (unpacked when drawn). */
function djParse(buf) {
  const d = djHead(buf);
  if (!d) return null;
  const u = new Uint8Array(buf), dv = new DataView(buf), dec = new TextDecoder();
  let p = 20 + d.pw * d.ph * 2 + d.ng * 4;
  d.layers = [];
  for (let i = 0; i < d.nl; i++) {
    if (p + 32 > u.length) return null;
    const raw = u.subarray(p, p + 24), z = raw.indexOf(0);
    const L = { name: dec.decode(raw.subarray(0, z < 0 ? 24 : z)), opacity: u[p + 24], visible: !!u[p + 25], tiles: [] };
    const n = u32(dv, p + 28);
    p += 32;
    for (let k = 0; k < n; k++) {
      const t = u16(dv, p), sz = u32(dv, p + 2);
      p += 6;
      if (p + sz > u.length) return null;
      L.tiles.push([t, u.subarray(p, p + sz)]);
      p += sz;
    }
    d.layers.push(L);
  }
  return d;
}

/* PackBits over 32-bit pixels, as dib_doc.c packs them. */
function djUnpack(src, out) {
  const dv = new DataView(src.buffer, src.byteOffset, src.byteLength);
  let p = 0, i = 0;
  while (p < src.length && i < out.length) {
    const c = src[p++];
    if (c < 128) {
      for (let k = 0; k <= c && i < out.length; k++, p += 4) out[i++] = dv.getUint32(p, true);
    } else {
      const v = dv.getUint32(p, true);
      p += 4;
      for (let k = 0; k < c - 126 && i < out.length; k++) out[i++] = v;
    }
  }
}

/* Straight-alpha "over" of the visible layers, background included, as
 * dib_compose_row_rgba does: what the app's PNG would be. */
function djCompose(d, hidden) {
  const W = d.w, H = d.h, tw = Math.ceil(W / TILE);
  const px = new Float32Array(W * H * 4);
  const ba = (d.bg >>> 24) / 255;
  for (let i = 0; i < W * H; i++) {
    px[i * 4] = (d.bg >> 16) & 255; px[i * 4 + 1] = (d.bg >> 8) & 255; px[i * 4 + 2] = d.bg & 255; px[i * 4 + 3] = ba;
  }
  const tile = new Uint32Array(TILE * TILE);
  d.layers.forEach((L, li) => {
    if (!L.visible || hidden.has(li) || !L.opacity) return;
    const op = L.opacity / 255;
    for (const [t, data] of L.tiles) {
      djUnpack(data, tile);
      const x0 = (t % tw) * TILE, y0 = Math.floor(t / tw) * TILE;
      for (let y = 0; y < TILE && y0 + y < H; y++) {
        for (let x = 0; x < TILE && x0 + x < W; x++) {
          const s = tile[y * TILE + x], sa = (s >>> 24) / 255 * op;
          if (!sa) continue;
          const o = ((y0 + y) * W + x0 + x) * 4, da = px[o + 3], oa = sa + da * (1 - sa);
          for (let c = 0; c < 3; c++) {
            const sc = (s >> (16 - 8 * c)) & 255;
            px[o + c] = (sc * sa + px[o + c] * da * (1 - sa)) / oa;
          }
          px[o + 3] = oa;
        }
      }
    }
  });
  const img = new ImageData(W, H);
  for (let i = 0; i < W * H * 4; i += 4) {
    img.data[i] = px[i]; img.data[i + 1] = px[i + 1]; img.data[i + 2] = px[i + 2]; img.data[i + 3] = Math.round(px[i + 3] * 255);
  }
  return img;
}

const prettyName = f => { const m = /^dibujo(\d+)\.dib$/.exec(f); return m ? 'Dibujo ' + m[1] : f.replace(/\.dib$/, ''); };

/* A picture the board can import: baseline JPEG (PNG if it has
 * transparency), at most 1280 px on its longest side. */
async function djForBoard(file) {
  const bmp = await createImageBitmap(file);
  const k = Math.min(1, 1280 / Math.max(bmp.width, bmp.height));
  const c = document.createElement('canvas');
  c.width = Math.round(bmp.width * k);
  c.height = Math.round(bmp.height * k);
  c.getContext('2d').drawImage(bmp, 0, 0, c.width, c.height);
  const png = file.type === 'image/png' || file.type === 'image/gif' || file.type === 'image/webp';
  const blob = await new Promise(r => c.toBlob(r, png ? 'image/png' : 'image/jpeg', 0.92));
  return { blob, ext: png ? '.png' : '.jpg', w: c.width, h: c.height };
}

/* ---- the page ---- */

function render(main) {
  let alive = true;
  const grid = h('div', { class: 'djgrid' }, h('p', { class: 'muted' }, 'Cargando…'));
  const viewer = h('div', { class: 'card pad', style: 'display:none' });
  const exported = h('div', {}), imports = h('div', {});
  const up = h('input', { type: 'file', accept: 'image/*', multiple: true, style: 'display:none' });
  const prog = h('div', { class: 'small muted' });

  async function preview(name, canvas, sub) {
    try {
      const r = await fetch(fsUrl(DIR + '/' + name), { headers: { Range: 'bytes=0-' + (20 + PREVIEW * PREVIEW * 2 - 1) } });
      const buf = await r.arrayBuffer(), d = djHead(buf);
      if (!d) throw new Error('no se lee');
      djPreview(buf, d, canvas);
      sub.textContent = `${d.w} × ${d.h} · ${d.nl} ${d.nl === 1 ? 'capa' : 'capas'}`;
    } catch (e) { sub.textContent = e.message; sub.className = 'bad small'; }
  }

  async function open(name, card) {
    grid.querySelectorAll('.djcard').forEach(c => c.classList.toggle('on', c === card));
    viewer.style.display = '';
    put(viewer, h('p', { class: 'muted' }, 'Armando el dibujo…'));
    let d;
    try { d = djParse(await fsBytes(DIR + '/' + name)); } catch (e) { put(viewer, h('p', { class: 'bad' }, e.message)); return; }
    if (!alive) return;
    if (!d) { put(viewer, h('p', { class: 'bad' }, name + ' no se lee')); return; }
    const hidden = new Set(), big = h('canvas', { class: 'djbig' });
    big.width = d.w;
    big.height = d.h;
    const draw = () => big.getContext('2d').putImageData(djCompose(d, hidden), 0, 0);
    draw();
    const base = name.replace(/\.dib$/, '');
    const layers = d.layers.map((L, i) => i).reverse().map(i => {
      const L = d.layers[i];
      const thumb = h('canvas', { class: 'djthumb' });
      thumb.width = d.w; thumb.height = d.h;
      thumb.getContext('2d').putImageData(djCompose({ ...d, bg: 0, layers: [{ ...L, visible: true, opacity: 255 }] }, new Set()), 0, 0);
      const cb = h('input', { type: 'checkbox', checked: L.visible, disabled: !L.visible,
        onchange: e => { if (e.target.checked) hidden.delete(i); else hidden.add(i); draw(); } });
      return h('div', { class: 'djlayer' }, thumb,
        h('label', {}, cb, h('div', { class: 'grow', style: 'min-width:0' }, h('div', { class: 'djell' }, L.name || 'Capa'),
          h('div', { class: 'muted small' }, (L.visible ? Math.round(L.opacity * 100 / 255) + ' %' : 'oculta en la placa')))));
    });
    put(viewer, h('div', { class: 'djview' }, big,
      h('div', { class: 'djside' },
        h('h3', {}, prettyName(name)),
        h('div', { class: 'muted small' }, `${d.w} × ${d.h} px · fondo ${(d.bg >>> 24) ? '#' + (d.bg & 0xFFFFFF).toString(16).padStart(6, '0') : 'transparente'}`),
        h('div', { class: 'btns' },
          h('button', { class: 'btn pri', onclick: () => big.toBlob(b => saveBlob(b, base + '.png'), 'image/png') }, 'Bajar PNG'),
          h('button', { class: 'btn', onclick: async () => saveBlob(new Blob([await fsBytes(DIR + '/' + name)]), name) }, 'Bajar .dib'),
          h('button', { class: 'btn', onclick: async () => { try { await openApp('aos.dibujo'); } catch (e) { toast(e.message, true); } } }, 'Abrir la app')),
        h('div', {}, h('div', { class: 'muted small', style: 'margin-bottom:4px' }, 'Capas (destildá una para sacarla del PNG; el archivo no cambia)'), layers),
        h('div', { class: 'btns' }, h('button', { class: 'btn red', onclick: async () => {
          if (!confirm('¿Borrar ' + prettyName(name) + ' de la tarjeta? No se puede deshacer.')) return;
          try { await fsDelete(DIR + '/' + name); toast('Borrado'); viewer.style.display = 'none'; load(); } catch (e) { toast(e.message, true); }
        } }, 'Borrar de la tarjeta')))));
  }

  function fileRow(dir, a, extra) {
    return h('div', { class: 'row djrow' },
      h('img', { class: 'djthumb', src: fsUrl(dir + '/' + a.name) + '&_=' + (a.mtime || a.size), alt: '', loading: 'lazy' }),
      h('div', { class: 'grow' }, h('div', { class: 'mono djell' }, a.name),
        h('div', { class: 'muted small' }, fmtBytes(a.size) + (a.mtime ? ' · ' + fmtDate(a.mtime) : ''))),
      h('div', { class: 'btns' }, extra, h('a', { class: 'btn', href: fsUrl(dir + '/' + a.name, true) }, 'Bajar'),
        h('button', { class: 'btn red', onclick: async () => {
          if (!confirm('¿Borrar ' + a.name + '?')) return;
          try { await fsDelete(dir + '/' + a.name); load(); } catch (e) { toast(e.message, true); }
        } }, 'Borrar')));
  }

  async function load() {
    let files = [], imp = [];
    try { files = await fsList(DIR); imp = await fsList(IMP); } catch (e) { put(grid, h('p', { class: 'bad' }, e.message)); return; }
    if (!alive) return;
    const dibs = files.filter(a => !a.dir && !a.name.startsWith('.') && /\.dib$/i.test(a.name))
      .sort((a, b) => (b.mtime || 0) - (a.mtime || 0) || a.name.localeCompare(b.name, 'es', { numeric: true }));
    put(grid, dibs.length ? dibs.map(a => {
      const c = h('canvas', { width: 1, height: 1 }), sub = h('span', { class: 'muted small' }, '…');
      const card = h('div', { class: 'djcard', onclick: () => open(a.name, card) }, c, h('b', {}, prettyName(a.name)), sub,
        h('span', { class: 'muted small' }, a.mtime ? fmtDate(a.mtime) : ''));
      preview(a.name, c, sub);
      return card;
    }) : h('p', { class: 'muted' }, 'Todavía no hay dibujos. Se hacen en la placa, con la app Dibujo.'));
    const pngs = files.filter(a => !a.dir && !a.name.startsWith('.') && /\.png$/i.test(a.name));
    put(exported, pngs.length ? pngs.map(a => fileRow(DIR, a)) : h('p', { class: 'muted small' }, 'Ninguno todavía: en la app, Menú → Exportar PNG.'));
    const pics = imp.filter(a => !a.dir && !a.name.startsWith('.') && /\.(jpe?g|png|bmp)$/i.test(a.name));
    put(imports, pics.length ? pics.map(a => fileRow(IMP, a)) : h('p', { class: 'muted small' }, 'No hay imágenes subidas.'));
  }

  up.onchange = async () => {
    const list = [...up.files];
    up.value = '';
    try { await fsMkdir(IMP); } catch (e) { /* already there */ }
    for (const f of list) {
      try {
        prog.textContent = 'Preparando ' + f.name + '…';
        const r = await djForBoard(f);
        const name = fsSlug(f.name.replace(/\.[^.]*$/, ''), 40, 'imagen') + r.ext;
        await fsPut(IMP + '/' + name, r.blob, x => { prog.textContent = `Subiendo ${name}: ${Math.round(x * 100)} %`; });
        toast(`${name} (${r.w} × ${r.h}) subida`);
      } catch (e) { toast(f.name + ': ' + e.message, true); }
    }
    prog.textContent = '';
    load();
  };

  put(main, h('h1', {}, 'Dibujo'),
    h('div', { class: 'card pad' }, h('h3', {}, 'Dibujos'), grid),
    viewer,
    h('div', { class: 'card pad' }, h('h3', {}, 'Exportados'), exported),
    h('div', { class: 'card pad' }, h('h3', {}, 'Imágenes para importar'),
      h('p', { class: 'muted small' }, 'En la app: Menú → Importar una imagen. Van a una capa nueva, abajo de todo, para calcar o pintar encima.'),
      imports,
      h('div', { class: 'btns', style: 'margin-top:8px' }, h('button', { class: 'btn pri', onclick: () => up.click() }, 'Subir imágenes'), up), prog),
    h('p', { class: 'note' }, 'Los dibujos están en /dibujo de la tarjeta, con sus capas (.dib). El PNG que se baja acá lo arma el navegador, igual al que exporta la app.'));
  load();
  return () => { alive = false; };
}

P.registerPage({ id: 'dibujo', name: 'Dibujo', icon: '✎', render });
