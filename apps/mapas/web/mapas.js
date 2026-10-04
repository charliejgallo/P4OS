/* Mapas's page in the board's portal (docs/PORTAL-PAGES.md), moved out
 * of the firmware's app.js on 2026-10-04 as it was: tools/install_apps.sh
 * puts this file in the card's /web and the portal loads it from there.
 * window.P4OS (version 1) is all it uses, and its styles come with it. */
const P = window.P4OS;
const { main, h, put, $, api, post, toast, row, fmtBytes, fmtDate, fsPut, fsList, fsDelete, fsMkdir, fsUrl,
  fsText, fsBytes, openApp, saveBlob, fsSlug } = P;

if (!document.getElementById('css-mapas')) {
  const st = document.createElement('style');
  st.id = 'css-mapas';
  st.textContent = `
.mpmap { position: relative; height: min(62vh, 520px); border-radius: var(--radius); overflow: hidden; background: #0b0d11; }
.mpcv { position: absolute; inset: 0; width: 100%; height: 100%; touch-action: none; cursor: grab; }
.mpcv:active { cursor: grabbing; }
.mpzoom { position: absolute; top: 10px; right: 10px; display: flex; flex-direction: column; gap: 6px; }
.mpzoom .btn { width: 38px; height: 38px; padding: 0; font-size: 20px; background: rgba(44,44,46,.9); color: #fff; }
.mpattr { position: absolute; right: 6px; bottom: 4px; font-size: 11px; color: #9aa0a8; background: rgba(0,0,0,.45); padding: 1px 6px; border-radius: 6px; pointer-events: none; }
.mpnote { position: absolute; left: 50%; top: 50%; transform: translate(-50%, -50%); background: rgba(0,0,0,.7); color: #d0d4da; padding: 12px 18px; border-radius: 12px; font-size: 14px; text-align: center; pointer-events: none; max-width: 80%; }
.mpres { display: flex; flex-direction: column; gap: 4px; margin-bottom: 8px; }
.mpresult { text-align: left; background: var(--card); border: 0; padding: 9px 14px; border-radius: 10px; cursor: pointer; }
.mpresult:hover { background: var(--card2); }
`;
  document.head.append(st);
}

/* ---- Mapas ---- */
/* The Mapas app's zones and its maps for use without a connection (apps/mapas),
 * ported from AmoledOS's /mapas page (mapas.html). The firmware knows nothing
 * of maps: the app reads, from /maps on the card,
 *   zones.txt  one zone per line, tab-separated: name, lat and lon x 1e6, zoom x 10 (256-px zoom)
 *   goto.txt   the same without a name: the app goes there and deletes it
 *   *.amp      a zone's tiles, AMP1 (apps/mapas/main/mp_store.h):
 *              "AMP1", n, minz, maxz, flags, W S E N (1e6), name[32], 64,
 *              n x { z, pad[3], x, y, offset, length }, data; sorted by (z, x, y)
 *   *.idx      its names for the search, AIX2 (mp_search.h), byte for byte
 *              what AmoledOS's tools/map_pack.py write_idx() writes.
 * Everything else happens in this browser, online in the user's browser only:
 * OpenFreeMap's tiles for the map and the download, Photon for the search.
 * The watch page drew its map with MapLibre from a CDN; the portal is
 * embedded in the firmware and loads nothing from outside, so the map here
 * is its own: the same MVT the board draws, painted on a canvas.
 *
 * window.P4_MAPAS_TILES = async (z, x, y) => Uint8Array stands in for the
 * network (tests with no internet: tiles from a pack, say); with it set,
 * nothing is asked of OpenFreeMap or Photon. */
const MP_DIR = '/maps', MP_TILEJSON = 'https://tiles.openfreemap.org/planet';
const MP_MAX_TILES = 2500, MP_MINZ = 6, MP_SCREEN = 720;          /* the board's screen, upright */
/* the layers the board draws (mp_mvt.c): the rest stays out of the pack */
const MP_LAYERS = new Set(['water', 'waterway', 'landcover', 'landuse', 'park', 'building', 'aeroway',
  'transportation', 'transportation_name', 'boundary', 'place', 'water_name']);
const mpTest = () => typeof window.P4_MAPAS_TILES === 'function';

let mpTpl = null;
async function mpTemplate() {
  if (!mpTpl) mpTpl = (await (await fetch(MP_TILEJSON)).json()).tiles[0];
  return mpTpl;
}
/* A tile as the network gives it: an empty one for open sea (204/404). */
async function mpFetch(z, x, y) {
  if (mpTest()) return await window.P4_MAPAS_TILES(z, x, y) || new Uint8Array(0);
  const url = (await mpTemplate()).replace('{z}', z).replace('{x}', x).replace('{y}', y);
  const r = await fetch(url);
  if (r.status === 404 || r.status === 204) return new Uint8Array(0);
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return new Uint8Array(await r.arrayBuffer());
}

/* -- MVT, what is needed -- */
class MpReader {
  constructor(u8, a, b) { this.u = u8; this.p = a || 0; this.end = b === undefined ? u8.length : b; }
  varint() {
    let r = 0, s = 0, c;
    do {
      if (this.p >= this.end) throw new Error('mvt');
      c = this.u[this.p++];
      if (s < 28) r |= (c & 0x7f) << s; else r += (c & 0x7f) * Math.pow(2, s);
      s += 7;
    } while (c & 0x80);
    return r;
  }
  /* the next field: { f, w, v } or, for length-delimited, { f, w, a, b } */
  field() {
    const k = this.varint(), f = Math.floor(k / 8), w = k & 7;
    if (w === 0) return { f, w, v: this.varint() };
    if (w === 2) { const n = this.varint(), a = this.p; this.p += n; if (this.p > this.end) throw new Error('mvt'); return { f, w, a, b: this.p }; }
    if (w === 1) { const a = this.p; this.p += 8; return { f, w, a }; }
    if (w === 5) { const a = this.p; this.p += 4; return { f, w, a }; }
    throw new Error('mvt');
  }
  get more() { return this.p < this.end; }
}
const mpDec = new TextDecoder();
const mpZz = v => (v >>> 1) ^ -(v & 1);

/* Only the layers the board draws: a tile is a list of layers, the bytes are copied. */
function mpStrip(u8) {
  const L = new MpReader(u8), parts = [];
  let total = 0;
  while (L.more) {
    const start = L.p, c = L.field();
    if (c.f !== 3 || c.w !== 2) continue;
    const C = new MpReader(u8, c.a, c.b);
    let name = '';
    while (C.more) { const d = C.field(); if (d.f === 1 && d.w === 2) { name = mpDec.decode(u8.subarray(d.a, d.b)); break; } }
    if (MP_LAYERS.has(name)) { parts.push(u8.subarray(start, L.p)); total += L.p - start; }
  }
  const out = new Uint8Array(total);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

/* A layer's features with their tags read: [{ type, tags, geom:[a,b] }] per layer name. */
function mpLayers(u8, wanted) {
  const L = new MpReader(u8), out = [];
  while (L.more) {
    const c = L.field();
    if (c.f !== 3 || c.w !== 2) continue;
    const C = new MpReader(u8, c.a, c.b);
    let name = '', ext = 4096;
    const keys = [], vals = [], feats = [];
    while (C.more) {
      const d = C.field();
      if (d.f === 1 && d.w === 2) name = mpDec.decode(u8.subarray(d.a, d.b));
      else if (d.f === 2 && d.w === 2) feats.push([d.a, d.b]);
      else if (d.f === 3 && d.w === 2) keys.push(mpDec.decode(u8.subarray(d.a, d.b)));
      else if (d.f === 4 && d.w === 2) vals.push([d.a, d.b]);
      else if (d.f === 5 && d.w === 0) ext = d.v;
    }
    if (wanted && !wanted(name)) continue;
    const val = i => {
      if (!vals[i]) return undefined;
      const V = new MpReader(u8, vals[i][0], vals[i][1]);
      while (V.more) {
        const d = V.field();
        if (d.f === 1 && d.w === 2) return mpDec.decode(u8.subarray(d.a, d.b));
        if (d.f === 2 && d.w === 5) return new DataView(u8.buffer, u8.byteOffset + d.a, 4).getFloat32(0, true);
        if (d.f === 3 && d.w === 1) return new DataView(u8.buffer, u8.byteOffset + d.a, 8).getFloat64(0, true);
        if ((d.f === 4 || d.f === 5) && d.w === 0) return d.v;
        if (d.f === 6 && d.w === 0) return mpZz(d.v);
        if (d.f === 7 && d.w === 0) return !!d.v;
      }
      return undefined;
    };
    const fs = [];
    for (const [a, b] of feats) {
      const F = new MpReader(u8, a, b);
      let tags = null, geom = null, type = 0;
      while (F.more) {
        const d = F.field();
        if (d.f === 2 && d.w === 2) tags = [d.a, d.b];
        else if (d.f === 3 && d.w === 0) type = d.v;
        else if (d.f === 4 && d.w === 2) geom = [d.a, d.b];
      }
      if (!geom) continue;
      const t = {};
      if (tags) { const T = new MpReader(u8, tags[0], tags[1]); while (T.more) { const k = T.varint(), v = T.varint(); t[keys[k]] = val(v); } }
      fs.push({ type, tags: t, geom });
    }
    out.push({ name, ext, feats: fs });
  }
  return out;
}
/* The geometry: calls moveTo/lineTo/close in tile units, and gives the first point. */
function mpGeom(u8, geom, ctx) {
  const G = new MpReader(u8, geom[0], geom[1]);
  let x = 0, y = 0, first = null, n = 0;
  while (G.more && n < 200000) {
    const cmd = G.varint(), id = cmd & 7, cnt = cmd >> 3;
    if (id === 7) { if (ctx) ctx.closePath(); continue; }
    for (let i = 0; i < cnt; i++, n++) {
      x += mpZz(G.varint()); y += mpZz(G.varint());
      if (!first) first = [x, y];
      if (ctx) { if (id === 1) ctx.moveTo(x, y); else ctx.lineTo(x, y); }
    }
  }
  return first;
}

/* The names of a tile, a point each, for the search index. */
const MP_NAME_LAYERS = { transportation_name: 'calle', place: 'lugar', poi: 'poi', water_name: 'agua',
  park: 'parque', aerodrome_label: 'aeropuerto', mountain_peak: 'cerro' };
function mpNames(u8, z, x, y, onName) {
  for (const L of mpLayers(u8, n => n in MP_NAME_LAYERS)) {
    const kind = MP_NAME_LAYERS[L.name];
    for (const f of L.feats) {
      const nm = f.tags.name || f.tags['name:latin'];
      if (!nm || typeof nm !== 'string') continue;
      /* a point: the middle one of the geometry */
      const G = new MpReader(u8, f.geom[0], f.geom[1]), pts = [];
      let px = 0, py = 0;
      while (G.more && pts.length < 4000) {
        const cmd = G.varint(), id = cmd & 7, n = cmd >> 3;
        if (id === 7) continue;
        for (let i = 0; i < n; i++) { px += mpZz(G.varint()); py += mpZz(G.varint()); pts.push(px, py); }
      }
      if (!pts.length) continue;
      const m = Math.floor(pts.length / 4) * 2, fx = pts[m] / L.ext, fy = pts[m + 1] / L.ext;
      if (kind !== 'calle' && (fx < 0 || fy < 0 || fx >= 1 || fy >= 1)) continue;   /* the repeated edge */
      const n2 = 1 << z, lon = (x + fx) / n2 * 360 - 180;
      const lat = Math.atan(Math.sinh(Math.PI * (1 - 2 * (y + fy) / n2))) * 180 / Math.PI;
      const cl = f.tags.class;
      onName(nm, kind === 'poi' && typeof cl === 'string' && cl ? cl : kind, lat, lon, z);
    }
  }
}
const mpKey = s => s.normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();

/* -- the AIX2 index -- */
/* The same as write_idx() of tools/map_pack.py, byte for byte: the folding is
 * the board's (mp_search_key): Latin-1 letters without accents, lower case,
 * everything else outside ASCII out. */
const MP_FOLD = 'AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty';
const MP_STOP = new Set(['de', 'del', 'la', 'las', 'el', 'los', 'y', 'e', 'a', 'al', 'en', 'of', 'the', 'da', 'do', 'dos']);
const MP_SKIP_KINDS = new Set(['bus']);   /* stops named after a corner: the streets are there already */
const MP_KEY_LEN = 9, MP_BLOCK = 256, MP_MAX_KEYS = 6;
function mpNorm(s) {
  let out = '', sp = true;
  for (let ch of s) {
    const c = ch.codePointAt(0);
    if (c >= 0xC0 && c <= 0xFF) ch = MP_FOLD[c - 0xC0];
    else if (c >= 0x80) continue;
    if (/\s/.test(ch)) { if (!sp) out += ' '; sp = true; continue; }
    sp = false;
    out += ch >= 'A' && ch <= 'Z' ? ch.toLowerCase() : ch;
  }
  return out.replace(/ +$/, '');
}
const mpAlnum = c => (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
function mpWordKeys(n) {
  const out = [];
  for (let i = 0; i < n.length && out.length < MP_MAX_KEYS; i++) {
    if (!mpAlnum(n[i]) || (i && mpAlnum(n[i - 1]))) continue;
    let j = i;
    while (j < n.length && mpAlnum(n[j])) j++;
    const w = n.slice(i, j);
    if (i && (MP_STOP.has(w) || (w.length === 1 && !/[0-9]/.test(w)))) continue;
    out.push([n.slice(i, i + MP_KEY_LEN), i === 0]);
  }
  return out;
}
function mpIndex(entries) {   /* [name, kind, lat, lon] */
  entries = entries.filter(e => !MP_SKIP_KINDS.has(e[1]) && !/^\d+\s*-\s/.test(e[0]));
  const freq = {};
  for (const e of entries) freq[e[1]] = (freq[e[1]] || 0) + 1;
  /* as Python's sorted() with key -frequency: stable, ties in order of appearance */
  let kinds = Object.keys(freq).map((k, i) => [k, freq[k], i]).sort((a, b) => b[1] - a[1] || a[2] - b[2]).map(x => x[0]).slice(0, 255);
  if (!kinds.includes('poi')) kinds = kinds.slice(0, 254).concat(['poi']);
  const ix = new Map(kinds.map((k, i) => [k, i]));
  const enc = new TextEncoder(), names = [], keys = [];
  let len = 0;
  for (const [nm, kind, la, lo] of entries) {
    const n = mpNorm(nm);
    if (!n) continue;
    const ki = ix.has(kind) ? ix.get(kind) : ix.get('poi');
    let nb = enc.encode(nm);
    if (nb.length > 60) nb = enc.encode(new TextDecoder().decode(nb.subarray(0, 60)).replace(/\uFFFD$/, ''));
    const rec = new Uint8Array(9 + nb.length + 1), dv = new DataView(rec.buffer);
    dv.setInt32(0, Math.round(la * 1e6), true);
    dv.setInt32(4, Math.round(lo * 1e6), true);
    rec[8] = ki;
    rec.set(nb, 9);
    const off = len;
    names.push(rec);
    len += rec.length;
    const cls = { lugar: 0, calle: 1, agua: 2 }[kinds[ki]] ?? 3;
    for (const [k, first] of mpWordKeys(n)) keys.push([k, cls, first ? 1 : 0, Math.min(255, n.length), off]);
  }
  /* Python's order over the tuples: the key padded with NUL, then the numbers */
  const kbytes = k => { const b = new Uint8Array(MP_KEY_LEN); for (let i = 0; i < k.length; i++) b[i] = k.charCodeAt(i); return b; };
  for (const c of keys) c[0] = kbytes(c[0]);
  keys.sort((a, b) => {
    for (let i = 0; i < MP_KEY_LEN; i++) if (a[0][i] !== b[0][i]) return a[0][i] - b[0][i];
    return a[1] - b[1] || a[2] - b[2] || a[3] - b[3] || a[4] - b[4];
  });
  const kb = enc.encode(kinds.join('\0') + '\0');
  const nblocks = Math.ceil(keys.length / MP_BLOCK);
  const kindsOff = 40, blocksOff = kindsOff + kb.length, keysOff = blocksOff + nblocks * MP_KEY_LEN, namesOff = keysOff + keys.length * 16;
  const out = new Uint8Array(namesOff + len), dv = new DataView(out.buffer);
  out.set([65, 73, 88, 50], 0);                                  /* AIX2 */
  [entries.length, keys.length, kinds.length, kindsOff, blocksOff, keysOff, namesOff, MP_BLOCK, 0].forEach((v, i) => dv.setUint32(4 + i * 4, v, true));
  out.set(kb, kindsOff);
  for (let b = 0; b < nblocks; b++) out.set(keys[b * MP_BLOCK][0], blocksOff + b * MP_KEY_LEN);
  keys.forEach((c, i) => {
    const o = keysOff + i * 16;
    out.set(c[0], o);
    out[o + 9] = c[1]; out[o + 10] = c[2]; out[o + 11] = c[3];
    dv.setUint32(o + 12, c[4], true);
  });
  let o = namesOff;
  for (const r of names) { out.set(r, o); o += r.length; }
  return out;
}

/* -- the AMP1 pack -- */
/* tiles: [{ z, x, y, d: Uint8Array }] sorted by (z, x, y) */
function mpPack(name, box, tiles) {
  const n = tiles.length;
  let total = 64 + n * 20;
  for (const t of tiles) total += t.d.length;
  const buf = new ArrayBuffer(total), dv = new DataView(buf), u8 = new Uint8Array(buf);
  u8.set([65, 77, 80, 49], 0);                                   /* AMP1 */
  dv.setUint32(4, n, true);
  let minz = 99, maxz = 0;
  for (const t of tiles) { minz = Math.min(minz, t.z); maxz = Math.max(maxz, t.z); }
  u8[8] = n ? minz : 0; u8[9] = maxz;
  dv.setInt32(12, Math.round(box.w * 1e6), true);
  dv.setInt32(16, Math.round(box.s * 1e6), true);
  dv.setInt32(20, Math.round(box.e * 1e6), true);
  dv.setInt32(24, Math.round(box.n * 1e6), true);
  let nb = new TextEncoder().encode(name);
  if (nb.length > 31) {                                          /* not cutting a letter in half */
    let c = 31;
    while (c > 0 && (nb[c] & 0xC0) === 0x80) c--;
    nb = nb.subarray(0, c);
  }
  u8.set(nb, 28);
  dv.setUint32(60, 64, true);
  let off = 64 + n * 20;
  tiles.forEach((t, i) => {
    const o = 64 + i * 20;
    u8[o] = t.z;
    dv.setUint32(o + 4, t.x, true);
    dv.setUint32(o + 8, t.y, true);
    dv.setUint32(o + 12, off, true);
    dv.setUint32(o + 16, t.d.length, true);
    u8.set(t.d, off);
    off += t.d.length;
  });
  return buf;
}
/* A pack read back: its header and a map "z/x/y" -> tile. */
function mpUnpack(buf) {
  const dv = new DataView(buf), u8 = new Uint8Array(buf);
  if (dv.getUint32(0, true) !== 0x31504D41) throw new Error('no es un AMP1');
  const n = dv.getUint32(4, true), io = dv.getUint32(60, true), tiles = new Map();
  for (let i = 0; i < n; i++) {
    const o = io + i * 20;
    tiles.set(u8[o] + '/' + dv.getUint32(o + 4, true) + '/' + dv.getUint32(o + 8, true), u8.subarray(dv.getUint32(o + 12, true), dv.getUint32(o + 12, true) + dv.getUint32(o + 16, true)));
  }
  const name = mpDec.decode(u8.subarray(28, 60)).replace(/\0.*$/s, '');
  return { n, minz: u8[8], maxz: u8[9], w: dv.getInt32(12, true) / 1e6, s: dv.getInt32(16, true) / 1e6, e: dv.getInt32(20, true) / 1e6, north: dv.getInt32(24, true) / 1e6, name, tiles };
}

/* -- tiles in a box, zoom by zoom -- */
const mpLon2x = (lo, z) => Math.floor((lo + 180) / 360 * (1 << z));
const mpLat2y = (la, z) => { const r = la * Math.PI / 180; return Math.floor((1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2 * (1 << z)); };
function mpTilesIn(b, minz, maxz) {
  const out = [];
  for (let z = minz; z <= maxz; z++) {
    const n = 1 << z;
    const x0 = Math.max(0, mpLon2x(b.w, z)), x1 = Math.min(n - 1, mpLon2x(b.e, z));
    const y0 = Math.max(0, mpLat2y(b.n, z)), y1 = Math.min(n - 1, mpLat2y(b.s, z));
    for (let x = x0; x <= x1; x++) for (let y = y0; y <= y1; y++) out.push({ z, x, y });
  }
  return out;
}
const mpSlug = s => s.normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 24) || 'zona';

/* -- the map in the page --
 * Web Mercator; the zoom is of 512-px tiles (MapLibre's, which the watch
 * page's arithmetic was written for); the board's is of 256 px, one more.
 * Tiles are OpenFreeMap's (the board's own), drawn in the board's dark
 * style, level z up to 14 and enlarged past it. */
/* the board's style (mp_render.c): colour, the zoom it shows from, its width at z 16 and its thinnest */
const MP_STYLE = {
  bg: '#0b0d11', water: '#0c2946', wood: '#0d2515', grass: '#0f2117', park: '#0f2d1d', resid: '#13161c', indus: '#19171e', misc: '#1b1824',
  building: '#252a34', aeroway: '#181b22', stream: '#15406a', boundary: '#6a5a8c', runway: '#353a45',
  minz: { resid: 99, indus: 10, misc: 12, aeroway: 9, building: 15 },
  roads: { motorway: ['#df7935', 4, 9, 1.4], trunk: ['#cf893b', 5, 8.5, 1.3], primary: ['#b88e44', 7, 8, 1.1], secondary: ['#8b8464', 9, 7, 0.9],
    tertiary: ['#59616f', 11, 6, 0], minor: ['#3f4653', 12.5, 4.6, 0], service: ['#2f3440', 14.5, 2.6, 0], track: ['#454a55', 15, 1.3, 0],
    path: ['#454a55', 15, 1.3, 0], rail: ['#686b75', 10, 1.6, 0], transit: ['#686b75', 10, 1.6, 0] },
};
const MP_ROAD_ORDER = ['path', 'track', 'service', 'minor', 'rail', 'transit', 'tertiary', 'secondary', 'primary', 'trunk', 'motorway'];
/* which names show at which (board) zoom, in what size and colour, the most important first */
const MP_PLACES = { country: [1, 'L', '#e6e6ee', 0, 7], state: [2, 'M', '#b9a9da', 5, 9], province: [2, 'M', '#b9a9da', 5, 9], city: [3, 'L', '#ffffff', 4, 15.5],
  town: [4, 'M', '#f0f0f2', 8, 16], village: [5, 'M', '#d5d9e0', 11, 17], suburb: [6, 'M', '#b4becc', 11.5, 16.5], quarter: [7, 'S', '#9ea9b8', 13.5, 99],
  neighbourhood: [7, 'S', '#9ea9b8', 13.5, 99], hamlet: [8, 'S', '#9ea9b8', 13, 99] };
const MP_FONT = { L: '600 15px', M: '500 13px', S: '12px' };
function mpDecode(u8, z) {
  const b = { fills: {}, roads: {}, lines: { waterway: new Path2D(), boundary: new Path2D(), aeroway: new Path2D() }, labels: [], ext: 4096 };
  const fill = k => b.fills[k] || (b.fills[k] = new Path2D());
  for (const L of mpLayers(u8)) {
    b.ext = L.ext;
    for (const f of L.feats) {
      const t = f.tags, cl = t.class;
      if (L.name === 'place' || L.name === 'water_name') {
        const nm = t.name || t['name:latin'];
        if (!nm || f.type !== 1) continue;
        const p = mpGeom(u8, f.geom, null);
        if (!p || p[0] < 0 || p[1] < 0 || p[0] >= L.ext || p[1] >= L.ext) continue;
        const ps = L.name === 'water_name' ? [6, 'S', '#86b6e6', 0, 99] : MP_PLACES[cl] || [9, 'S', '#9ea9b8', 15, 99];
        b.labels.push({ x: p[0], y: p[1], name: String(nm), st: ps, italic: L.name === 'water_name', rank: ps[0] * 10 + Math.min(9, +t.rank || 5) });
        continue;
      }
      if (f.type === 3) {
        const key = L.name === 'water' ? 'water' : L.name === 'park' ? 'park' : L.name === 'building' ? 'building'
          : L.name === 'aeroway' ? 'aeroway'
          : L.name === 'landcover' ? (cl === 'wood' || cl === 'forest' ? 'wood' : cl === 'grass' || cl === 'farmland' || cl === 'wetland' ? 'grass' : null)
          : L.name === 'landuse' ? (/^(residential|suburb|neighbourhood)$/.test(cl) ? 'resid' : /^(industrial|commercial|retail|railway|garages)$/.test(cl) ? 'indus'
            : /^(cemetery|hospital|school|university|college|stadium|pitch|playground)$/.test(cl) ? 'misc' : null) : null;
        if (key) mpGeom(u8, f.geom, fill(key));
      } else if (f.type === 2) {
        if (L.name === 'transportation') {
          const k = MP_STYLE.roads[cl] ? cl : cl === 'busway' || cl === 'bus_guideway' ? 'minor' : cl === 'ferry' ? null : cl ? 'minor' : null;
          if (!k || (t.brunnel === 'tunnel' && k !== 'motorway' && k !== 'trunk')) continue;
          mpGeom(u8, f.geom, b.roads[k] || (b.roads[k] = new Path2D()));
        } else if (L.name === 'waterway') mpGeom(u8, f.geom, b.lines.waterway);
        else if (L.name === 'boundary') { if (+t.admin_level <= 4 && !t.maritime) mpGeom(u8, f.geom, b.lines.boundary); }
        else if (L.name === 'aeroway') mpGeom(u8, f.geom, b.lines.aeroway);
      }
    }
  }
  return b;
}

function mpMapView(box, onMoveEnd) {
  const cv = h('canvas', { class: 'mpcv' }), g = cv.getContext('2d');
  const note = h('div', { class: 'mpnote', style: 'display:none' });
  const st = { cx: 0.5, cy: 0.5, z: 2 };
  const tiles = new Map();          /* "z/x/y" -> { b } | { err } | { loading } */
  let W = 0, H = 0, dpr = 1, raf = 0, endT = 0, inflight = 0, queue = [], fails = 0, used = 0;
  box.append(cv,
    h('div', { class: 'mpzoom' }, h('button', { class: 'btn', title: 'Acercar', onclick: () => zoomAt(1, W / 2, H / 2) }, '+'), h('button', { class: 'btn', title: 'Alejar', onclick: () => zoomAt(-1, W / 2, H / 2) }, '−')),
    note, h('div', { class: 'mpattr' }, '© OpenStreetMap · OpenMapTiles · OpenFreeMap'));
  const scale = () => 512 * Math.pow(2, st.z);
  const toX = lon => (lon + 180) / 360, toY = lat => { const r = Math.max(-85.0511, Math.min(85.0511, lat)) * Math.PI / 180; return (1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2; };
  const lonOf = x => x * 360 - 180, latOf = y => Math.atan(Math.sinh(Math.PI * (1 - 2 * y))) * 180 / Math.PI;
  const draw = () => { if (!raf) raf = requestAnimationFrame(render); };
  const moved = () => { draw(); clearTimeout(endT); endT = setTimeout(() => onMoveEnd && onMoveEnd(), 350); };
  function clampView() { st.z = Math.max(1, Math.min(17.5, st.z)); st.cx = ((st.cx % 1) + 1) % 1; st.cy = Math.max(0, Math.min(1, st.cy)); }
  function zoomAt(dz, px, py) {
    const s0 = scale(), wx = st.cx + (px - W / 2) / s0, wy = st.cy + (py - H / 2) / s0;
    st.z += dz; clampView();
    const s1 = scale();
    st.cx = wx - (px - W / 2) / s1; st.cy = wy - (py - H / 2) / s1; clampView();
    moved();
  }
  function want(z, x, y) {
    const k = z + '/' + x + '/' + y;
    if (tiles.has(k)) return tiles.get(k);
    const e = { loading: true };
    tiles.set(k, e);
    queue.push([k, z, x, y]);
    pump();
    return e;
  }
  function pump() {
    while (inflight < 6 && queue.length) {
      /* the newest wish first: what is on screen now */
      const [k, z, x, y] = queue.pop();
      inflight++;
      mpFetch(z, x, y).then(u8 => { tiles.set(k, { b: mpDecode(u8, z) }); fails = 0; note.style.display = 'none'; },
        err => { tiles.set(k, { err: true }); if (++fails >= 3) { note.textContent = 'El mapa necesita internet en esta computadora (OpenFreeMap).'; note.style.display = ''; } })
        .finally(() => { inflight--; pump(); draw(); });
    }
    if (queue.length > 64) { for (const [k] of queue.splice(0, queue.length - 64)) tiles.delete(k); }
  }
  function drawTile(b, ox, oy, T, clip) {
    /* the board's zoom at this scale (256-px tiles), and its widths: w16 x 2^((z - 16) x 0.7), not under the thinnest */
    const s = T / b.ext, zb = st.z + 1, wide = (w16, wmin) => Math.max(wmin || 0.5, w16 * Math.pow(2, (zb - 16) * 0.7)) * 0.6 / s;
    g.save();
    g.beginPath(); g.rect(clip[0], clip[1], clip[2], clip[3]); g.clip();
    g.setTransform(dpr * s, 0, 0, dpr * s, dpr * ox, dpr * oy);
    for (const key of ['grass', 'wood', 'resid', 'indus', 'misc', 'park', 'aeroway', 'water', 'building'])
      if (b.fills[key] && zb >= (MP_STYLE.minz[key] || 0)) { g.fillStyle = MP_STYLE[key]; g.fill(b.fills[key]); }
    g.lineCap = 'round'; g.lineJoin = 'round';
    g.strokeStyle = MP_STYLE.stream; g.lineWidth = wide(1.6, 0.5); g.stroke(b.lines.waterway);
    if (zb >= 11) { g.strokeStyle = MP_STYLE.runway; g.lineWidth = wide(10, 0.5); g.stroke(b.lines.aeroway); }
    g.setLineDash([6 / s, 4 / s]); g.strokeStyle = MP_STYLE.boundary; g.lineWidth = 1.1 / s; g.stroke(b.lines.boundary); g.setLineDash([]);
    for (const r of MP_ROAD_ORDER) if (b.roads[r]) {
      const [c, minz, w16, wmin] = MP_STYLE.roads[r];
      if (zb < minz) continue;
      if (r === 'rail' || r === 'transit') g.setLineDash([4 / s, 3 / s]);
      g.strokeStyle = c; g.lineWidth = wide(w16, wmin); g.stroke(b.roads[r]);
      g.setLineDash([]);
    }
    g.restore();
  }
  function render() {
    raf = 0;
    const r = box.getBoundingClientRect();
    dpr = window.devicePixelRatio || 1;
    if (Math.round(r.width) !== W || Math.round(r.height) !== H || cv.width !== Math.round(r.width * dpr)) {
      W = Math.round(r.width); H = Math.round(r.height);
      cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
    }
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = MP_STYLE.bg; g.fillRect(0, 0, W, H);
    const S = scale(), dz = Math.max(0, Math.min(14, Math.floor(st.z))), n = 1 << dz, T = S / n;
    const left = st.cx - W / 2 / S, top = st.cy - H / 2 / S;
    const tx0 = Math.floor(left * n), tx1 = Math.floor((st.cx + W / 2 / S) * n), ty0 = Math.max(0, Math.floor(top * n)), ty1 = Math.min(n - 1, Math.floor((st.cy + H / 2 / S) * n));
    const labels = [];
    used++;
    for (let ty = ty0; ty <= ty1; ty++) for (let tx = tx0; tx <= tx1; tx++) {
      const x = ((tx % n) + n) % n, ox = (tx / n - left) * S, oy = (ty / n - top) * S;
      const e = want(dz, x, ty);
      e.used = used;
      const clip = [ox, oy, T, T];
      if (e.b) {
        drawTile(e.b, ox, oy, T, clip);
        for (const l of e.b.labels) labels.push({ ...l, sx: ox + l.x / e.b.ext * T, sy: oy + l.y / e.b.ext * T });
      } else {
        /* an ancestor already here stands in, enlarged */
        for (let up = 1; up <= 5 && dz - up >= 0; up++) {
          const pz = dz - up, px = x >> up, py = ty >> up, pe = tiles.get(pz + '/' + px + '/' + py);
          if (pe && pe.b) { const PT = T * (1 << up); drawTile(pe.b, ox - (x - (px << up)) * T, oy - (ty - (py << up)) * T, PT, clip); pe.used = used; break; }
        }
      }
    }
    /* names last, the important ones first, none over another */
    labels.sort((a, b) => a.rank - b.rank);
    const boxes = [];
    g.textAlign = 'center'; g.textBaseline = 'middle'; g.lineJoin = 'round';
    const zb = st.z + 1;
    for (const l of labels) {
      if (l.sx < 0 || l.sy < 0 || l.sx > W || l.sy > H || zb < l.st[3] || zb >= l.st[4]) continue;
      g.font = (l.italic ? 'italic ' : '') + MP_FONT[l.st[1]] + ' -apple-system, system-ui, sans-serif';
      const w = g.measureText(l.name).width, bx = [l.sx - w / 2 - 4, l.sy - 9, l.sx + w / 2 + 4, l.sy + 9];
      if (boxes.some(q => bx[0] < q[2] && bx[2] > q[0] && bx[1] < q[3] && bx[3] > q[1]) || boxes.some(q => q.name === l.name && Math.abs(q[0] - bx[0]) < 300)) continue;
      bx.name = l.name; boxes.push(bx);
      g.strokeStyle = 'rgba(11,13,17,.9)'; g.lineWidth = 3; g.strokeText(l.name, l.sx, l.sy);
      g.fillStyle = l.st[2]; g.fillText(l.name, l.sx, l.sy);
    }
    /* forget what has not been drawn in a while */
    if (tiles.size > 400) for (const [k, e] of tiles) if (!e.loading && used - (e.used || 0) > 30) tiles.delete(k);
  }
  /* the finger, the mouse and the wheel */
  const ptrs = new Map();
  let pinch = null;
  cv.onpointerdown = e => { cv.setPointerCapture(e.pointerId); ptrs.set(e.pointerId, [e.offsetX, e.offsetY]); pinch = null; };
  cv.onpointermove = e => {
    const p = ptrs.get(e.pointerId);
    if (!p) return;
    const q = [e.offsetX, e.offsetY];
    if (ptrs.size === 1) { const S = scale(); st.cx -= (q[0] - p[0]) / S; st.cy -= (q[1] - p[1]) / S; clampView(); ptrs.set(e.pointerId, q); moved(); return; }
    ptrs.set(e.pointerId, q);
    const [a, b] = [...ptrs.values()], mid = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2], d = Math.hypot(a[0] - b[0], a[1] - b[1]) || 1;
    if (pinch) {
      const S = scale();
      st.cx -= (mid[0] - pinch.mid[0]) / S; st.cy -= (mid[1] - pinch.mid[1]) / S;
      zoomAt(Math.log2(d / pinch.d), mid[0], mid[1]);
    }
    pinch = { mid, d };
  };
  cv.onpointerup = cv.onpointercancel = e => { ptrs.delete(e.pointerId); pinch = null; };
  cv.onwheel = e => { e.preventDefault(); zoomAt(-e.deltaY * (e.deltaMode ? 0.05 : 0.0022), e.offsetX, e.offsetY); };
  cv.ondblclick = e => zoomAt(1, e.offsetX, e.offsetY);
  new ResizeObserver(draw).observe(box);
  return {
    getCenter: () => ({ lat: latOf(st.cy), lng: lonOf(st.cx) }),
    getZoom: () => st.z,
    width: () => W || box.clientWidth,
    getBounds: () => { const S = scale(), w = W || box.clientWidth, hh = H || box.clientHeight;
      return { w: lonOf(st.cx - w / 2 / S), e: lonOf(st.cx + w / 2 / S), n: latOf(Math.max(0, st.cy - hh / 2 / S)), s: latOf(Math.min(1, st.cy + hh / 2 / S)) }; },
    flyTo: ({ center, zoom }) => { st.cx = toX(center[0]); st.cy = toY(center[1]); if (zoom != null) st.z = zoom; clampView(); moved(); },
    fitBounds: ([[w, n], [e, s]], o) => {
      const x0 = toX(w), x1 = toX(e), y0 = toY(n), y1 = toY(s), ww = W || box.clientWidth, hh = H || box.clientHeight;
      st.cx = (x0 + x1) / 2; st.cy = (y0 + y1) / 2;
      st.z = Math.min((o && o.maxZoom) || 17, Math.log2(Math.min(ww / Math.max(1e-9, (x1 - x0) * 512), hh / Math.max(1e-9, (y1 - y0) * 512))) - 0.1);
      clampView(); moved();
    },
    redraw: draw,
  };
}

/* One download at a time, whatever the page does meanwhile: its words and
 * its bar go to whichever copy of the page is up. */
const mpJob = { running: false, cancel: false, msg: '', bad: false, frac: 0 };
function mpJobShow() {
  const e = document.getElementById('mp-estado'), p = document.getElementById('mp-prog'), c = document.getElementById('mp-cancel'), d = document.getElementById('mp-bajar');
  if (e) { e.textContent = mpJob.msg; e.className = 'note' + (mpJob.bad ? ' bad' : ''); }
  if (p) { p.style.display = mpJob.running ? '' : 'none'; p.firstChild.style.width = (mpJob.frac * 100) + '%'; }
  if (c) c.style.display = mpJob.running ? '' : 'none';
  if (d && mpJob.running) d.disabled = true;
}

let mpView = null, mpLang = null;
async function pageMapas() {
  let zones = [], zonesRaw = null, packs = [];
  const zoneList = h('div', { class: 'card' }), packList = h('div', { class: 'card' });
  const mapBox = h('div', { class: 'mpmap' }), where = h('p', { class: 'note' });
  const q = h('input', { type: 'search', placeholder: 'Buscar un lugar: ciudad, calle, barrio…' });
  const results = h('div', { class: 'mpres' });
  const zName = h('input', { type: 'text', maxlength: 30, placeholder: 'Nombre de la zona' });
  const dName = h('input', { type: 'text', maxlength: 30, placeholder: 'Nombre de la zona' });
  const detail = h('select', {}, [['14', 'Calles, con sus nombres (lo más completo)'], ['13', 'Avenidas y barrios'], ['12', 'Rutas y ciudades']].map(([v, t]) => h('option', { value: v }, t)));
  const est = h('div', { class: 'note' });
  const dlBtn = h('button', { class: 'btn pri', id: 'mp-bajar', onclick: () => download() }, 'Descargar y guardar en la placa');
  const cancelBtn = h('button', { class: 'btn', id: 'mp-cancel', style: 'display:none', onclick: () => { mpJob.cancel = true; } }, 'Cancelar');
  const map = mpMapView(mapBox, () => { paintWhere(); estimate(); });
  mpView = map;

  /* The board's zoom: one more than this map's (256-px tiles against 512),
   * and zoomed out as far as its 720 px need to show what this map shows. */
  const boardZoom = () => Math.max(3, Math.min(18, Math.round((map.getZoom() + 1 + Math.log2(MP_SCREEN / (map.width() || MP_SCREEN))) * 10) / 10));
  function paintWhere() {
    const c = map.getCenter();
    where.textContent = `Centro ${c.lat.toFixed(5)}, ${c.lng.toFixed(5)} · zoom ${boardZoom().toFixed(1).replace('.', ',')} en la placa`;
  }
  const viewBox = () => { const b = map.getBounds(); return { w: Math.max(-180, b.w), e: Math.min(179.9999, b.e), s: Math.max(-85, b.s), n: Math.min(85, b.n) }; };

  /* Photon (komoot's, over OpenStreetMap), which answers with CORS */
  async function search() {
    const s = q.value.trim();
    put(results);
    if (!s) return;
    let feats = [];
    if (!mpTest()) try {
      if (mpLang == null) mpLang = await api('settings').then(x => x.lang, () => '');
      const lang = ['en', 'de', 'fr', 'it'].includes(mpLang) ? mpLang : 'default';
      const r = await fetch('https://photon.komoot.io/api/?limit=6&lang=' + lang + '&q=' + encodeURIComponent(s));
      feats = (await r.json()).features || [];
    } catch { feats = []; }
    if (!feats.length) { put(results, h('p', { class: 'note' }, 'No se encontró nada.')); return; }
    put(results, feats.map(f => {
      const pr = f.properties || {};
      return h('button', { class: 'mpresult', onclick: () => {
        put(results);
        const [lo, la] = f.geometry.coordinates;
        if (pr.extent) map.fitBounds([[pr.extent[0], pr.extent[1]], [pr.extent[2], pr.extent[3]]], { maxZoom: 15 });
        else map.flyTo({ center: [lo, la], zoom: 14 });
        if (!zName.value) zName.value = (pr.name || '').slice(0, 30);
        if (!dName.value) dName.value = (pr.name || '').slice(0, 30);
      } }, pr.name || pr.street || '?', h('span', { class: 'muted small' }, ' ' + [pr.city, pr.state, pr.country].filter(Boolean).join(', ')));
    }));
  }

  /* -- zones -- */
  function parseZones(txt) {
    const out = [];
    for (const l of (txt || '').split(/\r?\n/)) {
      if (!l || l[0] === '#') continue;
      const p = l.split('\t');
      if (p.length < 3 || !p[0] || isNaN(+p[1]) || isNaN(+p[2])) continue;
      out.push({ name: p[0], la: +p[1] / 1e6, lo: +p[2] / 1e6, z: p[3] ? +p[3] / 10 : 15 });
    }
    return out;
  }
  const zonesText = () => '# P4OS - zonas de la app Mapas: nombre, lat y lon x 1e6, zoom x 10\n' +
    zones.map(z => z.name.replace(/[\t\r\n]/g, ' ') + '\t' + Math.round(z.la * 1e6) + '\t' + Math.round(z.lo * 1e6) + '\t' + Math.round(z.z * 10) + '\n').join('');
  async function writeZones(txt) {
    await fsMkdir(MP_DIR);
    await fsPut(MP_DIR + '/zones.txt', txt ?? zonesText());
    zonesRaw = txt ?? zonesText();
  }
  async function showOnBoard(la, lo, z) {
    try {
      await fsMkdir(MP_DIR);
      await fsPut(MP_DIR + '/goto.txt', Math.round(la * 1e6) + '\t' + Math.round(lo * 1e6) + '\t' + Math.round(z * 10) + '\n');
      await openApp('demo.mapas');
      toast('Enviado: la placa abre Mapas ahí');
    } catch (e) { toast(e.message, true); }
  }
  function paintZones() {
    put(zoneList, zones.length ? zones.map((z, i) => h('div', { class: 'row' },
      h('div', { class: 'grow' }, h('div', { class: 'ell' }, z.name), h('div', { class: 'muted small' }, `${z.la.toFixed(4)}, ${z.lo.toFixed(4)} · zoom ${z.z.toFixed(1).replace('.', ',')}`)),
      h('div', { class: 'btns' },
        h('button', { class: 'btn', onclick: () => map.flyTo({ center: [z.lo, z.la], zoom: z.z - 1 - Math.log2(MP_SCREEN / (map.width() || MP_SCREEN)) }) }, 'Ir'),
        h('button', { class: 'btn', onclick: () => showOnBoard(z.la, z.lo, z.z) }, 'En la placa'),
        h('button', { class: 'btn', onclick: async () => {
          const n = prompt('Nuevo nombre', z.name);
          if (!n || !n.trim() || n.trim() === z.name) return;
          z.name = n.trim().slice(0, 30);
          try { await writeZones(); paintZones(); } catch (e) { toast(e.message, true); }
        } }, 'Renombrar'),
        h('button', { class: 'btn red', onclick: async () => {
          if (!confirm(`¿Borrar la zona «${z.name}»?`)) return;
          zones.splice(i, 1);
          try { await writeZones(); } catch (e) { toast(e.message, true); }
          paintZones();
        } }, 'Borrar')))) :
      h('div', { class: 'row muted' }, 'Todavía no hay zonas. Mové el mapa hasta un lugar y guardalo: en la placa aparece en el menú ☰ de la app.'));
  }
  async function loadZones() {
    try { zonesRaw = await fsText(MP_DIR + '/zones.txt'); } catch (e) { toast(e.message, true); zonesRaw = null; }
    zones = parseZones(zonesRaw);
    paintZones();
  }
  async function saveZone() {
    const n = zName.value.trim();
    if (!n) { toast('Primero ponele un nombre', true); return; }
    const c = map.getCenter();
    zones.push({ name: n.slice(0, 30), la: c.lat, lo: c.lng, z: boardZoom() });
    try { await writeZones(); toast('Zona guardada'); } catch (e) { zones.pop(); toast(e.message, true); }
    paintZones();
  }
  /* zones.txt by hand: what is not a zone line is kept as it is */
  function editZones() {
    const ta = h('textarea', { spellcheck: 'false' });
    ta.value = zonesRaw ?? zonesText();
    const close = () => md.remove();
    const md = h('div', { class: 'modal', onclick: e => { if (e.target === md) close(); } },
      h('div', { class: 'box' }, h('div', { class: 'head' }, h('b', { class: 'grow' }, MP_DIR + '/zones.txt'), h('span', { class: 'muted small' }, 'nombre ⇥ lat×1e6 ⇥ lon×1e6 ⇥ zoom×10')), ta,
        h('div', { class: 'foot' }, h('button', { class: 'btn', onclick: close }, 'Cancelar'),
          h('button', { class: 'btn pri', onclick: async () => {
            const z = parseZones(ta.value);
            const bad = ta.value.split(/\r?\n/).filter(l => l && l[0] !== '#' && !parseZones(l).length);
            if (bad.length && !confirm(`${bad.length === 1 ? 'Una línea no es' : bad.length + ' líneas no son'} una zona y la app la${bad.length === 1 ? '' : 's'} va a ignorar:\n${bad.slice(0, 3).join('\n')}\n¿Guardar igual?`)) return;
            try { await writeZones(ta.value.endsWith('\n') ? ta.value : ta.value + '\n'); zones = z; paintZones(); close(); toast('zones.txt guardado'); } catch (e) { toast(e.message, true); }
          } }, 'Guardar'))));
    document.body.append(md);
    ta.focus();
  }

  /* -- the packs: a zone is its parts (name.amp, and name.2.amp... from the watch) and its index (name.idx) -- */
  async function loadPacks() {
    let files;
    try { files = await fsList(MP_DIR); } catch (e) { put(packList, h('div', { class: 'row bad' }, e.message)); return; }
    const groups = {};
    for (const f of files) {
      const m = /^([^.]+)(\.\d+)?\.(amp|idx)$/i.exec(f.name);
      if (!m || f.dir) continue;
      const g = groups[m[1]] || (groups[m[1]] = { files: [], bytes: 0, mtime: 0 });
      g.files.push(f.name); g.bytes += f.size || 0; g.mtime = Math.max(g.mtime, f.mtime || 0);
    }
    packs = Object.keys(groups).sort();
    put(packList, packs.length ? packs.map(k => {
      const g = groups[k];
      return h('div', { class: 'row' },
        h('div', { class: 'grow' }, h('div', { class: 'mono ell' }, k), h('div', { class: 'muted small' }, fmtBytes(g.bytes) + ' · ' + g.files.join(', ') + (g.mtime ? ' · ' + fmtDate(g.mtime) : ''))),
        h('button', { class: 'btn red', onclick: async () => {
          if (!confirm(`¿Borrar «${k}» de la placa?`)) return;
          try { for (const a of g.files) await fsDelete(MP_DIR + '/' + a); toast('Borrado ' + k); } catch (e) { toast(e.message, true); }
          loadPacks();
        } }, 'Borrar'));
    }) : h('div', { class: 'row muted' }, 'No hay mapas guardados en la placa.'),
    files.some(f => f.dir && f.name === 'cache') ? h('div', { class: 'row muted small' }, '/maps/cache: lo visto en línea, que la app guarda sola (se vacía desde su menú).') : null);
  }

  /* The size cannot be known without downloading: a stripped tile of the
   * centre weighs 85 KB and one of the suburbs 14. A sample spread over the
   * zone is measured (12 tiles of the highest zoom, 6 of the next, 3 of the
   * other, 1 of each low level), stripped as in the download, and
   * extrapolated; the index with the sample's names, halved (an avenue is in
   * tens of tiles and once in the index). Against real downloads
   * (2026-09-27): CABA 15.1 estimated / 15.6 real, AMBA 31.5 / 33.8. */
  const samples = new Map();
  let estT = 0, estTok = 0;
  async function measure(t) {
    const k = t.z + '/' + t.x + '/' + t.y;
    if (samples.has(k)) return samples.get(k);
    const u8 = await mpFetch(t.z, t.x, t.y);
    let ib = 0;
    mpNames(u8, t.z, t.x, t.y, (nm, kind) => { ib += 2 * nm.length + kind.length + 24; });
    const m = { b: mpStrip(u8).length, i: ib };
    samples.set(k, m);
    return m;
  }
  const spread = (arr, k) => arr.length <= k ? arr : Array.from({ length: k }, (_, i) => arr[Math.floor((i + 0.5) * arr.length / k)]);
  function estimate() {
    if (!est.isConnected) return;
    const maxz = +detail.value, box = viewBox(), list = mpTilesIn(box, MP_MINZ, maxz), n = list.length, tok = ++estTok;
    clearTimeout(estT);
    dlBtn.disabled = n > MP_MAX_TILES || mpJob.running;
    if (n > MP_MAX_TILES) { est.className = 'note bad'; est.textContent = `${n.toLocaleString('es-AR')} teselas: demasiado grande. Acercá el mapa o elegí menos detalle.`; return; }
    est.className = 'note';
    est.textContent = `${n} teselas del zoom ${MP_MINZ} al ${maxz}: calculando el tamaño…`;
    /* once the map is still: moving it must not set off downloads */
    estT = setTimeout(async () => {
      try {
        const byZ = {};
        for (const t of list) (byZ[t.z] = byZ[t.z] || []).push(t);
        let bytes = 0, idx = 0, measured = 0;
        for (const zs of Object.keys(byZ)) {
          const z = +zs, arr = byZ[zs], k = z === maxz ? 12 : z === maxz - 1 ? 6 : z === maxz - 2 ? 3 : 1;
          const ms = await Promise.all(spread(arr, k).map(measure));
          if (tok !== estTok) return;
          const avg = f => ms.reduce((a, m) => a + f(m), 0) / ms.length;
          bytes += (avg(m => m.b) + 20) * arr.length;
          if (z === maxz) idx = avg(m => m.i) * arr.length * 0.5;
          measured += ms.length;
        }
        est.textContent = `${n} teselas del zoom ${MP_MINZ} al ${maxz}: unos ${fmtBytes(Math.round(bytes + idx))} en la tarjeta (medido sobre ${measured} teselas de muestra).`;
      } catch { if (tok === estTok) est.textContent = `${n} teselas del zoom ${MP_MINZ} al ${maxz}. No se pudo calcular el tamaño (¿sin internet?).`; }
    }, 700);
  }

  /* -- the download -- */
  async function download() {
    const name = dName.value.trim();
    if (!name) { toast('Primero ponele un nombre', true); return; }
    if (mpJob.running) return;
    const maxz = +detail.value, box = viewBox(), list = mpTilesIn(box, MP_MINZ, maxz);
    if (list.length > MP_MAX_TILES) return;
    const centre = map.getCenter(), zb = boardZoom();
    Object.assign(mpJob, { running: true, cancel: false, bad: false, frac: 0, msg: 'Empezando…' });
    mpJobShow();
    const say = (msg, frac) => { mpJob.msg = msg; if (frac != null) mpJob.frac = frac; mpJobShow(); };
    try {
      const done = [], idx = new Map();
      let i = 0, ready = 0;
      const worker = async () => {
        while (i < list.length && !mpJob.cancel) {
          const t = list[i++];
          let u8 = null;
          for (let a = 0; a < 3 && !u8; a++) try { u8 = await mpFetch(t.z, t.x, t.y); } catch { /* again */ }
          if (!u8) throw new Error(`No se pudo bajar la tesela ${t.z}/${t.x}/${t.y}`);
          mpNames(u8, t.z, t.x, t.y, (nm, kind, la, lo, z) => {
            const k = mpKey(nm) + '\u0001' + (kind === 'calle' || kind === 'lugar' || kind === 'agua' ? kind : 'poi:' + kind);
            const prev = idx.get(k);
            if (!prev || z > prev.z) idx.set(k, { nm, kind, la, lo, z });
          });
          done.push({ z: t.z, x: t.x, y: t.y, d: mpStrip(u8) });
          ready++;
          say(`Descargando ${ready} de ${list.length} teselas…`, ready / list.length);
        }
      };
      await Promise.all([1, 2, 3, 4, 5, 6].map(worker));
      if (mpJob.cancel) throw new Error('Cancelado.');
      const r = await mpStore(name, box, done, [...idx.values()].map(v => [v.nm, v.kind, v.la, v.lo]), centre, zb, say);
      say(`Guardado: ${r.tiles} teselas en ${r.file} (${fmtBytes(r.bytes)}), ${r.names} nombres para buscar.`, 1);
    } catch (e) { mpJob.bad = true; say(e.message || String(e)); }
    mpJob.running = false;
    mpJobShow();
    if (dlBtn.isConnected) { estimate(); loadPacks(); loadZones(); }
  }
  /* The tiles of a zone to the card: the pack whole, its index, and the zone
   * in zones.txt so the board's menu goes there; then goto.txt is not
   * written (that would move the board), but the app rescans the packs when
   * it next opens its list. The parts of an older download of the same zone
   * (from the watch, which split at 7.5 MB) go once the new pack is in. */
  async function mpStore(name, box, tiles, names, centre, zb, say) {
    say('Armando el paquete…');
    tiles.sort((a, b) => a.z - b.z || a.x - b.x || a.y - b.y);
    const base = mpSlug(name), pack = mpPack(name, box, tiles), index = mpIndex(names);
    await fsMkdir(MP_DIR);
    say(`Guardando en la placa: ${base}.amp (${fmtBytes(pack.byteLength)})…`, 0);
    await fsPut(MP_DIR + '/' + base + '.amp', new Blob([pack]), f => say(`Guardando en la placa: ${base}.amp (${fmtBytes(pack.byteLength)})… ${Math.round(f * 100)} %`, f));
    say('Guardando el índice de nombres…');
    await fsPut(MP_DIR + '/' + base + '.idx', new Blob([index]));
    for (const f of await fsList(MP_DIR)) if (new RegExp('^' + base + '\\.\\d+\\.amp$').test(f.name)) await fsDelete(MP_DIR + '/' + f.name).catch(() => {});
    const now = parseZones(await fsText(MP_DIR + '/zones.txt').catch(() => null)).filter(z => z.name !== name.slice(0, 30));
    now.push({ name: name.slice(0, 30), la: centre.lat, lo: centre.lng, z: zb });
    zones = now;
    await writeZones();
    if (zoneList.isConnected) paintZones();
    return { tiles: tiles.length, file: base + '.amp', bytes: pack.byteLength, names: new DataView(index.buffer).getUint32(4, true) };
  }
  /* for the tests: the store step alone, with tiles from anywhere */
  window.P4_MAPAS_STORE = (name, box, tiles, names) => mpStore(name, box, tiles, names, { lat: (box.n + box.s) / 2, lng: (box.w + box.e) / 2 }, 15, () => {});

  q.onkeydown = e => { if (e.key === 'Enter') search(); };
  detail.onchange = estimate;
  put(main, h('h1', {}, 'Mapas'),
    h('div', { class: 'btns', style: 'margin-bottom:10px;flex-wrap:nowrap' }, h('div', { style: 'flex:1;min-width:0' }, q), h('button', { class: 'btn', onclick: search }, 'Buscar')),
    results, mapBox, where,
    h('h2', {}, 'Zonas'), zoneList,
    h('div', { class: 'btns', style: 'margin-top:10px' }, h('div', { style: 'flex:1 1 200px' }, zName),
      h('button', { class: 'btn pri', onclick: saveZone }, 'Guardar lo que se ve como zona'),
      h('button', { class: 'btn', onclick: () => { const c = map.getCenter(); showOnBoard(c.lat, c.lng, boardZoom()); } }, 'Mostrarlo en la placa'),
      h('button', { class: 'btn', onclick: editZones }, 'Editar zones.txt')),
    h('h2', {}, 'Sin conexión'), packList,
    h('div', { class: 'card pad', style: 'margin-top:10px' },
      h('label', { class: 'f', style: 'margin-top:0' }, 'Se descarga lo que se ve en el mapa de arriba, con este nombre:'), dName,
      h('label', { class: 'f' }, 'Hasta qué detalle'), detail, est,
      h('div', { class: 'prog', id: 'mp-prog', style: 'display:none' }, h('div')),
      h('div', { class: 'note', id: 'mp-estado' }),
      h('div', { class: 'btns', style: 'margin-top:12px' }, dlBtn, cancelBtn)),
    h('p', { class: 'note' }, 'Todo se hace en este navegador: baja las teselas de OpenFreeMap, les quita lo que la placa no dibuja, arma el índice de nombres para buscar y las guarda en /maps de la tarjeta, un archivo por zona. En la app, lo guardado se usa antes que la red y lo que se ve en línea queda en la tarjeta para la próxima vez. Datos © colaboradores de OpenStreetMap, OpenMapTiles y OpenFreeMap; búsqueda de Photon (komoot).'));
  mpJobShow();
  window.onbeforeunload = () => mpJob.running ? 'Hay una descarga en curso' : undefined;
  await loadZones();
  loadPacks();
  /* where to start: the first zone, or Clima's city, or the Obelisco */
  let centre = [-58.3816, -34.6037], zoom = 11;
  if (zones.length) { centre = [zones[0].lo, zones[0].la]; zoom = zones[0].z - 1 - Math.log2(MP_SCREEN / (mapBox.clientWidth || MP_SCREEN)); }
  else try { const s = await api('settings'); if (s.clima_lat10k || s.clima_lon10k) { centre = [s.clima_lon10k / 1e4, s.clima_lat10k / 1e4]; zoom = 12; } } catch {}
  map.flyTo({ center: centre, zoom });
  paintWhere();
}

P.registerPage({ id: 'mapas', name: 'Mapas', icon: '⌖', render: () => { pageMapas(); } });
