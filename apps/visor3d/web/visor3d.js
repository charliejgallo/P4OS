/* Visor 3D's page in the board's portal (docs/PORTAL-PAGES.md), moved out
 * of the firmware's app.js on 2026-10-04 as it was: tools/install_apps.sh
 * puts this file in the card's /web and the portal loads it from there.
 * window.P4OS (version 1) is all it uses, and its styles come with it. */
const P = window.P4OS;
const { main, h, put, $, api, post, toast, row, fmtBytes, fmtDate, fsPut, fsList, fsDelete, fsMkdir, fsUrl,
  fsText, fsBytes, openApp, saveBlob, fsSlug } = P;

if (!document.getElementById('css-visor3d')) {
  const st = document.createElement('style');
  st.id = 'css-visor3d';
  st.textContent = `
.v3cols { display: grid; grid-template-columns: minmax(0, 1fr) minmax(280px, 480px); gap: 18px; align-items: start; }
.v3view { position: sticky; top: 16px; }
.v3cv { width: 100%; aspect-ratio: 1; display: block; border-radius: var(--radius); background: #4a505e; touch-action: none; cursor: grab; }
.v3color { width: 64px; height: 36px; border: 0; background: none; padding: 0; cursor: pointer; }
@media (max-width: 900px) { .v3cols { grid-template-columns: 1fr; } .v3view { position: static; } }
`;
  document.head.append(st);
}

/* ---- Visor 3D ---- */
/* Models for the Visor 3D app (apps/visor3d), in /3d on the card. The
 * firmware knows nothing of 3D: an STL can go up as it is (the viewer welds it
 * and reduces it while loading), and STL, OBJ and GLB become an M3D here, in
 * the browser, as AmoledOS's /3d page did (modelos.html):
 *   "M3D1", nv, nt, flags (u32 LE); nv*3 f32; nt*3 u32; flags&1: nt u16 RGB565. Y up.
 * The colours are kept, and the model is reduced only past the viewer's
 * budget: V3_BUDGET triangles (v3_mesh.h) and MAX_CL vertices, over which
 * v3_mesh.c refuses an M3D. Every loose piece is turned to face outwards,
 * as tools/obj2m3d.py does: the viewer skips the back faces of a model it
 * takes as closed, and a piece wound inwards (eyes, a mouth) vanished. */
const V3_DIR = '/3d', V3_BUDGET = 150000, V3_MAXV = 131072, V3_BASE = '#a8b8d0';

function v3Rgb565(r, g, b) {             /* 0..1 */
  const c = v => Math.max(0, Math.min(255, Math.round(v * 255)));
  return ((c(r) >> 3) << 11) | ((c(g) >> 2) << 5) | (c(b) >> 3);
}

/* The readers give { pos: Float32Array (x,y,z per corner, 9 per triangle),
 * col: Uint16Array|null (RGB565 per triangle, 0xFFFF for none) }, Y up. */
function v3ReadSTL(buf) {
  const dv = new DataView(buf), n = buf.byteLength >= 84 ? dv.getUint32(80, true) : 0;
  if (84 + n * 50 === buf.byteLength) {
    const out = new Float32Array(n * 9);
    for (let i = 0; i < n; i++) {
      const o = 84 + i * 50 + 12;
      for (let k = 0; k < 3; k++) {
        const b = o + k * 12, q = i * 9 + k * 3;
        out[q] = dv.getFloat32(b, true); out[q + 1] = dv.getFloat32(b + 8, true); out[q + 2] = -dv.getFloat32(b + 4, true);  /* Z up -> Y up */
      }
    }
    return { pos: out, col: null };
  }
  const txt = new TextDecoder().decode(buf), re = /vertex\s+(\S+)\s+(\S+)\s+(\S+)/g, pos = [];
  let m;
  while ((m = re.exec(txt))) pos.push(+m[1], +m[3], -m[2]);
  pos.length -= pos.length % 9;          /* half a triangle at the end: out */
  return { pos: new Float32Array(pos), col: null };
}

function v3ReadMTL(txt) {
  const mats = {};
  let cur = null;
  for (const l of txt.split(/\r?\n/)) {
    const p = l.trim().split(/\s+/);
    if (p[0] === 'newmtl') { cur = p.slice(1).join(' '); mats[cur] = null; }
    else if (p[0] === 'Kd' && cur !== null) mats[cur] = [+p[1], +p[2], +p[3]];
  }
  return mats;
}

function v3ReadOBJ(txt, mats) {
  const v = [], pos = [], col = [];
  let color = null, any = false;
  for (const l of txt.split(/\r?\n/)) {
    if (l.startsWith('v ')) {
      const p = l.trim().split(/\s+/);
      v.push(+p[1], +p[2], +p[3]);
    } else if (l.startsWith('usemtl')) {
      const k = l.trim().split(/\s+/).slice(1).join(' ');
      color = mats && mats[k] ? mats[k] : null;
    } else if (l.startsWith('f ')) {
      const nv = v.length / 3;
      const idx = l.trim().split(/\s+/).slice(1).map(s => { const i = parseInt(s.split('/')[0], 10); return i < 0 ? nv + i : i - 1; });
      for (let k = 1; k + 1 < idx.length; k++) {           /* a fan: polygon -> triangles */
        for (const i of [idx[0], idx[k], idx[k + 1]]) pos.push(v[i * 3] || 0, v[i * 3 + 1] || 0, v[i * 3 + 2] || 0);
        col.push(color ? v3Rgb565(color[0], color[1], color[2]) : 0xFFFF);
        if (color) any = true;
      }
    }
  }
  return { pos: new Float32Array(pos), col: any ? Uint16Array.from(col) : null };
}

/* glTF 2.0 binary: the scene with its transforms, every triangle primitive,
 * and each material's base colour (linear in glTF: to sRGB for the screen). */
function v3ReadGLB(buf) {
  const dv = new DataView(buf);
  if (buf.byteLength < 20 || dv.getUint32(0, true) !== 0x46546C67) throw new Error('no es un GLB');
  let off = 12, json = null, bin = null;
  while (off + 8 <= buf.byteLength) {
    const len = dv.getUint32(off, true), type = dv.getUint32(off + 4, true);
    if (type === 0x4E4F534A) json = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, off + 8, len)));
    else if (type === 0x004E4942) bin = new DataView(buf, off + 8, len);
    off += 8 + len;
  }
  if (!json || !bin) throw new Error('no es un GLB');
  if ((json.extensionsUsed || []).includes('KHR_draco_mesh_compression')) throw new Error('un GLB comprimido con Draco no se puede leer');
  const accessor = i => {
    const a = json.accessors[i], bv = json.bufferViews[a.bufferView];
    const n = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 }[a.type];
    const sz = { 5126: 4, 5125: 4, 5123: 2, 5122: 2, 5121: 1, 5120: 1 }[a.componentType];
    const stride = bv.byteStride || n * sz, base = (bv.byteOffset || 0) + (a.byteOffset || 0);
    const rd = { 5126: o => bin.getFloat32(o, true), 5125: o => bin.getUint32(o, true), 5123: o => bin.getUint16(o, true),
      5122: o => bin.getInt16(o, true), 5121: o => bin.getUint8(o), 5120: o => bin.getInt8(o) }[a.componentType];
    const out = new Float64Array(a.count * n);
    for (let k = 0; k < a.count; k++) for (let c = 0; c < n; c++) out[k * n + c] = rd(base + k * stride + c * sz);
    return out;
  };
  const mul = (a, b) => {                /* 4x4, column-major */
    const r = new Array(16).fill(0);
    for (let i = 0; i < 4; i++) for (let j = 0; j < 4; j++) for (let k = 0; k < 4; k++) r[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
    return r;
  };
  const local = nd => {
    if (nd.matrix) return nd.matrix.slice();
    const [tx, ty, tz] = nd.translation || [0, 0, 0], [x, y, z, w] = nd.rotation || [0, 0, 0, 1], [sx, sy, sz] = nd.scale || [1, 1, 1];
    return [(1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0,
            (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0,
            (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0, tx, ty, tz, 1];
  };
  const srgb = v => Math.pow(Math.max(0, v), 1 / 2.2);
  const pos = [], col = [];
  let any = false;
  const visit = (ni, parent) => {
    const nd = json.nodes[ni], m = mul(parent, local(nd));
    if (nd.mesh !== undefined) for (const pr of json.meshes[nd.mesh].primitives) {
      if ((pr.mode !== undefined && pr.mode !== 4) || pr.attributes.POSITION === undefined) continue;
      const p = accessor(pr.attributes.POSITION);
      const idx = pr.indices !== undefined ? accessor(pr.indices) : Float64Array.from({ length: p.length / 3 }, (_, i) => i);
      let c = 0xFFFF;
      const mat = pr.material !== undefined ? json.materials[pr.material] : null;
      const f = mat && mat.pbrMetallicRoughness && mat.pbrMetallicRoughness.baseColorFactor;
      if (f) { c = v3Rgb565(srgb(f[0]), srgb(f[1]), srgb(f[2])); any = true; }
      for (let k = 0; k + 2 < idx.length; k += 3) {
        for (const i of [idx[k], idx[k + 1], idx[k + 2]]) {
          const x = p[i * 3], y = p[i * 3 + 1], z = p[i * 3 + 2];
          pos.push(m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]);
        }
        col.push(c);
      }
    }
    for (const ch of nd.children || []) visit(ch, m);
  };
  const scene = json.scenes ? json.scenes[json.scene || 0] : { nodes: json.nodes.map((_, i) => i) };
  for (const n of scene.nodes) visit(n, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  return { pos: new Float32Array(pos), col: any ? Uint16Array.from(col) : null };
}

/* An M3D from the card, for the preview. */
function v3ReadM3D(buf) {
  const dv = new DataView(buf);
  if (buf.byteLength < 16 || dv.getUint32(0, true) !== 0x3144334D) throw new Error('no es un M3D');
  const nv = dv.getUint32(4, true), nt = dv.getUint32(8, true), fl = dv.getUint32(12, true);
  const o1 = 16 + nv * 12, o2 = o1 + nt * 12;
  if (o2 + (fl & 1 ? nt * 2 : 0) > buf.byteLength) throw new Error('el M3D está cortado');
  const v = new Float32Array(buf.slice(16, o1)), idx = new Uint32Array(buf.slice(o1, o2));
  return { v, idx, col: fl & 1 ? new Uint16Array(buf.slice(o2, o2 + nt * 2)) : null, nt, nv };
}

/* Weld and reduce: the corners grouped in a grid of G cells a side over the
 * box; those in one cell are one vertex (their average), and a triangle with
 * two corners in one cell goes. A big G only welds, a small one reduces. The
 * cell goes with the COLOUR: pieces of different colours are not melted
 * together (Mila's black pupil against the yellow eye came out misshapen).
 * pid is the place (the cell whatever the colour) of each vertex, so the
 * pieces can be told apart as a mesh, not by colour. */
function v3Cluster(src, G) {
  const pos = src.pos, nt = pos.length / 9;
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < pos.length; i += 3) for (let a = 0; a < 3; a++) { const p = pos[i + a]; if (p < lo[a]) lo[a] = p; if (p > hi[a]) hi[a] = p; }
  const inv = [0, 1, 2].map(a => hi[a] > lo[a] ? G / (hi[a] - lo[a]) : 0);
  const cells = new Map(), places = new Map(), colours = new Map();
  const sum = [], pid = [], idx = [], col = [];
  const q = (i, a) => Math.min(G - 1, Math.max(0, Math.floor((pos[i + a] - lo[a]) * inv[a])));
  const cell = (i, c) => {
    const pk = (q(i, 0) * 4096 + q(i, 1)) * 4096 + q(i, 2);
    let ci = colours.get(c);
    if (ci === undefined) { ci = colours.size; colours.set(c, ci); }
    const k = pk * 65536 + ci;
    let v = cells.get(k);
    if (v === undefined) {
      v = sum.length / 4; cells.set(k, v); sum.push(0, 0, 0, 0);
      let p = places.get(pk);
      if (p === undefined) { p = places.size; places.set(pk, p); }
      pid.push(p);
    }
    sum[v * 4] += pos[i]; sum[v * 4 + 1] += pos[i + 1]; sum[v * 4 + 2] += pos[i + 2]; sum[v * 4 + 3]++;
    return v;
  };
  for (let t = 0; t < nt; t++) {
    const c = src.col ? src.col[t] : 0;
    const a = cell(t * 9, c), b = cell(t * 9 + 3, c), d = cell(t * 9 + 6, c);
    if (a === b || b === d || a === d) continue;
    idx.push(a, b, d);
    if (src.col) col.push(src.col[t]);
  }
  const nv = sum.length / 4, v = new Float32Array(nv * 3);
  for (let c = 0; c < nv; c++) for (let a = 0; a < 3; a++) v[c * 3 + a] = sum[c * 4 + a] / sum[c * 4 + 3];
  return { v, idx: Uint32Array.from(idx), col: src.col ? Uint16Array.from(col) : null, pid: Int32Array.from(pid), np: places.size, nt: idx.length / 3, nv };
}

/* The finest grid that fits the budget: only welded if it already does. */
function v3Reduce(src, cap) {
  const fits = r => r.nt <= cap && r.nv <= V3_MAXV;
  const all = v3Cluster(src, 4096);
  if (fits(all)) return all;
  let lo = 8, hi = 4095, best = null;
  for (let it = 0; it < 13 && lo <= hi; it++) {                /* binary search over G */
    const G = (lo + hi) >> 1, r = v3Cluster(src, G);
    if (fits(r)) { best = r; lo = G + 1; } else hi = G - 1;
  }
  return best || v3Cluster(src, 8);
}

/* obj2m3d.py's orient(): every loose piece (connected through shared places)
 * wound outwards. A closed piece is judged by its signed volume, an open one
 * by its normals against the direction from the model's centre. */
function v3Orient(m) {
  const { v, idx, pid, np } = m, nt = idx.length / 3;
  const par = new Int32Array(np);
  for (let i = 0; i < np; i++) par[i] = i;
  const find = a => { while (par[a] !== a) { par[a] = par[par[a]]; a = par[a]; } return a; };
  for (let t = 0; t < nt; t++) {
    const a = find(pid[idx[t * 3]]);
    for (let k = 1; k < 3; k++) { const b = find(pid[idx[t * 3 + k]]); if (b !== a) par[b] = a; }
  }
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < v.length; i += 3) for (let a = 0; a < 3; a++) { lo[a] = Math.min(lo[a], v[i + a]); hi[a] = Math.max(hi[a], v[i + a]); }
  const c = [0, 1, 2].map(a => (lo[a] + hi[a]) / 2);
  const vol = new Float64Array(np), dir = new Float64Array(np), used = new Uint8Array(np), edges = new Map();
  for (let t = 0; t < nt; t++) {
    const ia = idx[t * 3] * 3, ib = idx[t * 3 + 1] * 3, id = idx[t * 3 + 2] * 3;
    const ax = v[ia], ay = v[ia + 1], az = v[ia + 2], bx = v[ib], by = v[ib + 1], bz = v[ib + 2], dx = v[id], dy = v[id + 1], dz = v[id + 2];
    const r = find(pid[idx[t * 3]]);
    used[r] = 1;
    vol[r] += ax * (by * dz - bz * dy) - ay * (bx * dz - bz * dx) + az * (bx * dy - by * dx);
    const ux = bx - ax, uy = by - ay, uz = bz - az, wx = dx - ax, wy = dy - ay, wz = dz - az;
    const nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
    dir[r] += nx * ((ax + bx + dx) / 3 - c[0]) + ny * ((ay + by + dy) / 3 - c[1]) + nz * ((az + bz + dz) / 3 - c[2]);
    const p = [pid[idx[t * 3]], pid[idx[t * 3 + 1]], pid[idx[t * 3 + 2]]];
    for (let k = 0; k < 3; k++) {
      const e0 = p[k], e1 = p[(k + 1) % 3], key = e0 < e1 ? e0 * np + e1 : e1 * np + e0;
      edges.set(key, (edges.get(key) || 0) + 1);
    }
  }
  const open = new Uint8Array(np);
  for (const [k, n] of edges) if (n === 1) open[find(Math.floor(k / np))] = 1;
  let pieces = 0, nopen = 0, flipped = 0;
  for (let r = 0; r < np; r++) if (used[r]) { pieces++; if (open[r]) nopen++; }
  for (let t = 0; t < nt; t++) {
    const r = find(pid[idx[t * 3]]);
    if ((open[r] ? dir[r] : vol[r]) < 0) { const k = idx[t * 3 + 1]; idx[t * 3 + 1] = idx[t * 3 + 2]; idx[t * 3 + 2] = k; flipped++; }
  }
  return { pieces, open: nopen, flipped };
}

function v3ToM3D(m, def) {
  const nv = m.v.length / 3, nt = m.idx.length / 3;
  let col = m.col;
  if (col) col = col.map(c => c === 0xFFFF ? def : c);
  else if (def !== null) col = new Uint16Array(nt).fill(def);
  const buf = new ArrayBuffer(16 + nv * 12 + nt * 12 + (col ? nt * 2 : 0)), dv = new DataView(buf);
  [0x4D, 0x33, 0x44, 0x31].forEach((b, i) => dv.setUint8(i, b));
  dv.setUint32(4, nv, true); dv.setUint32(8, nt, true); dv.setUint32(12, col ? 1 : 0, true);
  let o = 16;
  for (let i = 0; i < nv * 3; i++, o += 4) dv.setFloat32(o, m.v[i], true);
  for (let i = 0; i < nt * 3; i++, o += 4) dv.setUint32(o, m.idx[i], true);
  if (col) for (let i = 0; i < nt; i++, o += 2) dv.setUint16(o, col[i], true);
  return buf;
}

/* The preview: a z-buffer, flat faces, and the board's light (v3_raster.c):
 * a headlight plus a little from above, both sides lit, on the viewer's
 * default background. */
function v3Render(cv, m, view, baseHex) {
  const W = cv.width, H = cv.height, g = cv.getContext('2d');
  const img = g.createImageData(W, H), out = new Uint32Array(img.data.buffer);
  out.fill(0xFF5E504A);                                        /* #4a505e, the viewer's grey: a black cat reads on it */
  if (m && m.idx.length) {
    const v = m.v, nv = v.length / 3, nt = m.idx.length / 3, idx = m.idx, col = m.col;
    if (!m.ctr) {
      let cx = 0, cy = 0, cz = 0, r = 0;
      for (let i = 0; i < nv; i++) { cx += v[i * 3]; cy += v[i * 3 + 1]; cz += v[i * 3 + 2]; }
      cx /= nv; cy /= nv; cz /= nv;
      for (let i = 0; i < nv; i++) r = Math.max(r, Math.hypot(v[i * 3] - cx, v[i * 3 + 1] - cy, v[i * 3 + 2] - cz));
      m.ctr = [cx, cy, cz]; m.rad = r || 1;
    }
    const [cx, cy, cz] = m.ctr, r = m.rad;
    const cyw = Math.cos(view.yaw), syw = Math.sin(view.yaw), cp = Math.cos(view.pitch), sp = Math.sin(view.pitch);
    const f = Math.min(W, H) * 0.36 * 3 * (view.zoom || 1), P = new Float32Array(nv * 3);
    for (let i = 0; i < nv; i++) {
      const x = (v[i * 3] - cx) / r, y = (v[i * 3 + 1] - cy) / r, z = (v[i * 3 + 2] - cz) / r;
      const x1 = cyw * x + syw * z, z1 = -syw * x + cyw * z, y2 = cp * y - sp * z1, z2 = sp * y + cp * z1, d = 3 - z2;
      P[i * 3] = W / 2 + x1 * f / d; P[i * 3 + 1] = H / 2 - y2 * f / d; P[i * 3 + 2] = d;
    }
    const zb = new Float32Array(W * H).fill(1e9);
    const base = parseInt(baseHex.slice(1), 16);
    for (let t = 0; t < nt; t++) {
      const a = idx[t * 3], b = idx[t * 3 + 1], d = idx[t * 3 + 2];
      const ax = P[a * 3], ay = P[a * 3 + 1], az = P[a * 3 + 2], bx = P[b * 3], by = P[b * 3 + 1], bz = P[b * 3 + 2], dx = P[d * 3], dy = P[d * 3 + 1], dz = P[d * 3 + 2];
      const area = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
      if (area === 0) continue;
      const x0 = Math.max(0, Math.floor(Math.min(ax, bx, dx))), x1 = Math.min(W - 1, Math.ceil(Math.max(ax, bx, dx)));
      const y0 = Math.max(0, Math.floor(Math.min(ay, by, dy))), y1 = Math.min(H - 1, Math.ceil(Math.max(ay, by, dy)));
      if (x0 > x1 || y0 > y1) continue;
      const ux = v[b * 3] - v[a * 3], uy = v[b * 3 + 1] - v[a * 3 + 1], uz = v[b * 3 + 2] - v[a * 3 + 2];
      const wx = v[d * 3] - v[a * 3], wy = v[d * 3 + 1] - v[a * 3 + 1], wz = v[d * 3 + 2] - v[a * 3 + 2];
      let nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
      const l = Math.hypot(nx, ny, nz) || 1; nx /= l; ny /= l; nz /= l;
      const nz1 = -syw * nx + cyw * nz, ny2 = cp * ny - sp * nz1, nz2 = sp * ny + cp * nz1;
      const li = Math.min(1, 0.2 + 0.7 * Math.abs(nz2) + 0.15 * Math.max(0, ny2));
      const c = col && col[t] !== 0xFFFF ? col[t] : -1;
      const R = c >= 0 ? ((c >> 11) & 31) * 8.23 : base >> 16, G = c >= 0 ? ((c >> 5) & 63) * 4.05 : (base >> 8) & 255, B = c >= 0 ? (c & 31) * 8.23 : base & 255;
      const pix = (0xFF000000 | ((B * li) << 16) | ((G * li) << 8) | (R * li)) >>> 0;
      const inv = 1 / area;
      for (let y = y0; y <= y1; y++) {
        const py = y + 0.5;
        for (let x = x0; x <= x1; x++) {
          const px = x + 0.5;
          const w0 = ((bx - px) * (dy - py) - (by - py) * (dx - px)) * inv;
          if (w0 < 0) continue;
          const w1 = ((dx - px) * (ay - py) - (dy - py) * (ax - px)) * inv;
          if (w1 < 0 || w0 + w1 > 1) continue;
          const z = w0 * az + w1 * bz + (1 - w0 - w1) * dz, k = y * W + x;
          if (z < zb[k]) { zb[k] = z; out[k] = pix; }
        }
      }
    }
  }
  g.putImageData(img, 0, 0);
}

let v3View = { yaw: 0.6, pitch: 0.35, zoom: 1, spin: true }, v3Loop = 0;
async function pageVisor3d() {
  let src = null, model = null, name = '', rawStl = null, raw = null, stats = null, files = [];
  const list = h('div', { class: 'card' });
  const cv = h('canvas', { class: 'v3cv', width: 480, height: 480 });
  const info = h('div', { class: 'note', style: 'white-space:pre-line;margin:12px 0 0' });
  const bar = h('div', { class: 'prog', style: 'display:none' }, h('div'));
  const cap = h('input', { type: 'range', min: 5000, max: V3_BUDGET, step: 5000, value: V3_BUDGET });
  const capv = h('b', {}, V3_BUDGET.toLocaleString('es-AR'));
  const color = h('input', { type: 'color', value: V3_BASE, class: 'v3color' });
  const conv = h('button', { class: 'btn pri', disabled: true, onclick: () => save(false) }, 'Convertir y guardar en la placa');
  const rawBtn = h('button', { class: 'btn', style: 'display:none', onclick: () => save(true) }, 'Subir el STL tal cual');
  const input = h('input', { type: 'file', multiple: true, accept: '.stl,.obj,.mtl,.glb', style: 'display:none', onchange: e => read([...e.target.files]) });
  const drop = h('div', { class: 'drop', style: 'margin-top:0' }, 'Soltá acá un STL, un OBJ (con su .mtl) o un GLB, o ',
    h('a', { href: '#3d', onclick: e => { e.preventDefault(); input.click(); } }, 'elegilo'), '.', input);
  drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); read([...e.dataTransfer.files]); };
  const say = (t, bad) => { info.textContent = t; info.className = 'note' + (bad ? ' bad' : ''); };
  const wait = () => new Promise(r => setTimeout(r, 30));      /* the words show before a long reduce */
  const draw = () => v3Render(cv, model, v3View, color.value);

  async function load() {
    try { files = (await fsList(V3_DIR)).filter(f => !f.dir && /\.(stl|m3d)$/i.test(f.name)).sort((a, b) => a.name.localeCompare(b.name)); }
    catch (e) { put(list, h('div', { class: 'row bad' }, e.message)); return; }
    put(list, files.length ? files.map(f => h('div', { class: 'row' },
      h('div', { class: 'ico' }, /\.m3d$/i.test(f.name) ? '◆' : '◇'),
      h('div', { class: 'grow' }, h('div', { class: 'mono ell' }, f.name), h('div', { class: 'muted small' }, fmtBytes(f.size) + (f.mtime ? ' · ' + fmtDate(f.mtime) : ''))),
      h('div', { class: 'btns' },
        h('button', { class: 'btn', onclick: () => preview(f) }, 'Ver'),
        h('a', { class: 'btn', href: fsUrl(V3_DIR + '/' + f.name, true) }, 'Bajar'),
        h('button', { class: 'btn red', onclick: async () => {
          if (!confirm(`¿Borrar ${f.name} de la placa?`)) return;
          try { await fsDelete(V3_DIR + '/' + f.name); toast('Borrado ' + f.name); load(); } catch (e) { toast(e.message, true); }
        } }, 'Borrar')))) :
      h('div', { class: 'row muted' }, 'Todavía no hay modelos en la placa.'));
  }

  /* one on the card, drawn as the viewer will (an STL welded and, past the budget, reduced) */
  async function preview(f) {
    src = null; rawStl = null; conv.disabled = true; rawBtn.style.display = 'none';
    say('Leyendo ' + f.name + '…');
    try {
      const buf = await fsBytes(V3_DIR + '/' + f.name);
      await wait();
      if (/\.m3d$/i.test(f.name)) { model = v3ReadM3D(buf); say(`${f.name}: ${model.nt.toLocaleString('es-AR')} triángulos, ${model.nv.toLocaleString('es-AR')} vértices${model.col ? ', con colores' : ''}.`); }
      else {
        const s = v3ReadSTL(buf), n = s.pos.length / 9;
        say(`${f.name}: soldando ${n.toLocaleString('es-AR')} triángulos…`); await wait();
        model = v3Reduce(s, V3_BUDGET);
        say(`${f.name}: ${n.toLocaleString('es-AR')} triángulos en el archivo` + (model.nt < n - n / 50 ? `, ${model.nt.toLocaleString('es-AR')} al reducirlo` : '') + '.');
      }
      v3View = { yaw: 0.6, pitch: 0.35, zoom: 1, spin: true };
      draw();
    } catch (e) { say(e.message || String(e), true); }
  }

  async function read(fs) {
    src = null; model = null; rawStl = null; raw = null; conv.disabled = true; rawBtn.style.display = 'none';
    if (!fs.length) { say(''); draw(); return; }
    say('Leyendo…');
    try {
      const mtl = fs.find(f => /\.mtl$/i.test(f.name)), m = fs.find(f => /\.(stl|obj|glb)$/i.test(f.name));
      if (!m) throw new Error('hace falta un .stl, un .obj o un .glb');
      name = fsSlug(m.name.replace(/\.[^.]+$/, ''), 40, 'modelo');
      const ext = m.name.split('.').pop().toLowerCase();
      if (ext === 'stl') { rawStl = await m.arrayBuffer(); src = v3ReadSTL(rawStl); raw = m.name; }
      else if (ext === 'obj') src = v3ReadOBJ(await m.text(), mtl ? v3ReadMTL(await mtl.text()) : null);
      else src = v3ReadGLB(await m.arrayBuffer());
      if (!src.pos.length) throw new Error('no tiene triángulos');
      await process();
      conv.disabled = false;
      if (rawStl) rawBtn.style.display = '';
    } catch (e) { say('No se pudo leer: ' + (e.message || e), true); draw(); }
  }

  async function process() {
    if (!src) return;
    const n = src.pos.length / 9, c = +cap.value;
    say(n > c ? 'Reduciendo…' : 'Soldando…');
    await wait();
    model = v3Reduce(src, c);
    stats = v3Orient(model);
    const fmt = x => x.toLocaleString('es-AR');
    say(`${fmt(n)} triángulos en el archivo → ${fmt(model.nt)}${n > c ? ' al reducir' : ', sin reducir'}, ${fmt(model.nv)} vértices${src.col ? ', con colores' : ''}.\n` +
      `${fmt(stats.pieces)} ${stats.pieces === 1 ? 'pieza' : 'piezas'} (${fmt(stats.open)} abiertas), ${fmt(stats.flipped)} triángulos dados vuelta hacia afuera.`);
    v3View = { yaw: 0.6, pitch: 0.35, zoom: 1, spin: true };
    draw();
  }

  async function save(asIs) {
    const file = name + (asIs ? '.stl' : '.m3d');
    if (files.some(f => f.name === file) && !confirm(`Ya hay un ${file} en la placa. ¿Reemplazarlo?`)) return;
    let body;
    if (asIs) body = rawStl;
    else {
      const c = parseInt(color.value.slice(1), 16);
      const def = v3Rgb565((c >> 16) / 255, ((c >> 8) & 255) / 255, (c & 255) / 255);
      body = v3ToM3D(model, src.col || color.value !== V3_BASE ? def : null);
    }
    conv.disabled = true;
    bar.style.display = ''; bar.firstChild.style.width = '0';
    try {
      await fsMkdir(V3_DIR);
      await fsPut(V3_DIR + '/' + file, body, f => { bar.firstChild.style.width = (f * 100) + '%'; });
      toast('Guardado ' + V3_DIR + '/' + file + ' (' + fmtBytes(body.byteLength) + ')');
      load();
    } catch (e) { toast(e.message, true); }
    conv.disabled = false;
    bar.style.display = 'none';
  }

  cap.oninput = () => { capv.textContent = (+cap.value).toLocaleString('es-AR'); };
  cap.onchange = () => { if (src) process(); };
  color.oninput = draw;
  let drag = null;
  cv.onpointerdown = e => { drag = { x: e.clientX, y: e.clientY }; v3View.spin = false; cv.setPointerCapture(e.pointerId); };
  cv.onpointermove = e => {
    if (!drag) return;
    v3View.yaw += (e.clientX - drag.x) * 0.01; v3View.pitch = Math.max(-1.5, Math.min(1.5, v3View.pitch + (e.clientY - drag.y) * 0.01));
    drag = { x: e.clientX, y: e.clientY }; draw();
  };
  cv.onpointerup = cv.onpointercancel = () => { drag = null; };
  cv.onwheel = e => { e.preventDefault(); v3View.zoom = Math.max(0.4, Math.min(4, v3View.zoom * Math.exp(-e.deltaY * 0.0015))); draw(); };
  cv.ondblclick = () => { v3View = { yaw: 0.6, pitch: 0.35, zoom: 1, spin: true }; draw(); };

  put(main, h('h1', {}, 'Visor 3D'),
    h('div', { class: 'btns', style: 'margin-bottom:14px' },
      h('button', { class: 'btn pri', onclick: () => openApp('demo.visor3d').then(() => toast('Abriendo el Visor 3D en la placa'), e => toast(e.message, true)) }, 'Abrir el Visor 3D en la placa'),
      h('span', { class: 'muted small' }, 'Los modelos van en /3d de la tarjeta, STL o M3D.')),
    list,
    h('h2', {}, 'Agregar un modelo'),
    h('div', { class: 'v3cols' },
      h('div', { class: 'card pad' }, drop,
        h('label', { class: 'f' }, 'Triángulos como máximo: ', capv),
        cap,
        h('p', { class: 'note', style: 'margin:4px 0 0' }, 'El visor dibuja hasta 150 000; más que eso lo reduciría él mismo al abrirlo. Menos triángulos es más fluido al girar.'),
        h('label', { class: 'f' }, 'Color, si el archivo no trae'), color,
        info, bar,
        h('div', { class: 'btns', style: 'margin-top:14px' }, conv, rawBtn)),
      h('div', { class: 'v3view' }, cv, h('p', { class: 'note' }, 'Vista previa, con la luz de la placa: arrastrá para girar, la rueda acerca, doble clic vuelve al principio.'))),
    h('p', { class: 'note' }, 'Todo se hace en este navegador: el STL, el OBJ o el GLB se leen, se sueldan, cada pieza suelta se orienta hacia afuera (el visor no dibuja la cara de atrás de un modelo cerrado) y se guarda un .m3d con sus colores. De un GLB se toma el color base de cada material; las texturas no.'));
  draw();
  load();
  cancelAnimationFrame(v3Loop);
  let last = 0, cost = 16;
  const spin = ts => {
    if (location.hash !== '#3d' || !cv.isConnected) return;
    if (v3View.spin && model && !document.hidden && ts - last > Math.max(33, cost * 2)) {
      const t0 = performance.now();
      v3View.yaw += 0.03; draw();
      cost = performance.now() - t0; last = ts;
    }
    v3Loop = requestAnimationFrame(spin);
  };
  v3Loop = requestAnimationFrame(spin);
}

P.registerPage({ id: '3d', name: 'Visor 3D', icon: '◆', render: () => { pageVisor3d(); } });
