/* The EEPROM app's page in the board's portal (docs/PORTAL-PAGES.md): the
 * versions of each chip, a hex view, the differences between two versions,
 * downloading and uploading .bin files, and asking the board to read the
 * chip or to write a version into it.
 *
 * The page and the app meet on the card, in /eeprom (ee_portal.c):
 *   /eeprom/chip.json      the chip chosen on the board and where it hangs
 *   /eeprom/pedido.json    what the page asks: {"id","op":"leer"|"escribir"|"chip",...}
 *   /eeprom/estado.json    the app's answer, with its progress
 *   /eeprom/<chip>/*.bin   the versions, each with a .json beside it
 * The app looks for a request every second while it is open, so the page
 * opens it first. A write from here is the same as one from the screen: a
 * copy of what the chip had is kept first, and everything is read back.
 * The browser computes an upload's CRC-32, MD5 and SHA-256 itself: the
 * portal is plain HTTP, where crypto.subtle does not exist. */
const P = window.P4OS;
const DIR = '/eeprom';
const APP_ID = 'aos.eeprom';
const CHIPS = ['24LC01', '24LC02', '24LC04', '24LC08', '24LC16', '24LC32', '24LC64', '24LC128', '24LC256', '24LC512',
  '24LC1025', 'AT24CM01', '25LC010', '25LC020', '25LC040', '25LC080', '25LC160', '25LC320', '25LC640', '25LC128',
  '25LC256', '25LC512', '25LC1024', 'M95M02', '25Q40', '25Q80', '25Q16', '25Q32', '25Q64', '25Q128',
  '93C46', '93C56', '93C66', '93C76', '93C86'];

if (!document.getElementById('css-eeprom')) {
  const st = document.createElement('style');
  st.id = 'css-eeprom';
  st.textContent = `
.eehead { display: flex; gap: 16px; align-items: baseline; flex-wrap: wrap; }
.eehead b { font-size: 22px; }
.eestat { margin-top: 10px; }
.eever { display: grid; grid-template-columns: auto 1fr auto; gap: 4px 12px; align-items: center; padding: 12px 18px; border-top: 1px solid var(--line); }
.eever:first-child { border-top: 0; }
.eever .nm { font-weight: 600; }
.eever .sub { grid-column: 2 / 4; color: var(--dim); font-size: 13px; overflow-wrap: anywhere; }
.eever .btns { grid-column: 1 / 4; }
.eever.sel { background: color-mix(in srgb, var(--accent) 12%, transparent); }
.eesrc { font-size: 12px; padding: 2px 9px; border-radius: 999px; background: var(--card2); }
.eesrc.antes { color: var(--orange); } .eesrc.escrita { color: var(--green); } .eesrc.portal { color: #40c8e0; }
.eesrc.lectura { color: var(--accent); } .eesrc.vista { color: #bf5af2; }
.eeab { display: flex; gap: 6px; }
.eeab label { display: flex; gap: 4px; align-items: center; font-size: 13px; color: var(--dim); cursor: pointer; }
.eehex { position: relative; height: 440px; overflow: auto; border-top: 1px solid var(--line); }
.eehex .in { position: absolute; left: 0; right: 0; }
.eehex .ln { white-space: pre; font-family: ui-monospace, "JetBrains Mono", Menlo, monospace; font-size: 13px; line-height: 20px; height: 20px; padding: 0 14px; }
.eehex .ad { color: var(--dim); }
.eehex .z { color: var(--dim); opacity: .6; }
.eehex .d { color: var(--red); background: color-mix(in srgb, var(--red) 18%, transparent); }
.eehex .h { background: color-mix(in srgb, var(--orange) 45%, transparent); }
.eetools { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; padding: 12px 18px; }
.eetools input[type=text] { width: 160px; }
.eesums { font-size: 12px; }
.eeranges { max-height: 160px; overflow: auto; padding: 0 18px 12px; }
.eeranges a { margin-right: 10px; font-family: ui-monospace, Menlo, monospace; font-size: 13px; }
`;
  document.head.append(st);
}

/* ---- CRC-32, MD5, SHA-256 (the same three the app writes) ---- */

const CRC_T = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
function crc32(b) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < b.length; i++) c = CRC_T[(c ^ b[i]) & 255] ^ (c >>> 8);
  return ((c ^ 0xFFFFFFFF) >>> 0).toString(16).padStart(8, '0');
}

function pad64(b, bigEndian) {
  const n = b.length, total = ((n + 8) >> 6) + 1 << 6;
  const m = new Uint8Array(total);
  m.set(b);
  m[n] = 0x80;
  const bits = n * 8, lo = bits >>> 0, hi = Math.floor(bits / 4294967296);
  const dv = new DataView(m.buffer);
  if (bigEndian) { dv.setUint32(total - 8, hi); dv.setUint32(total - 4, lo); }
  else { dv.setUint32(total - 8, lo, true); dv.setUint32(total - 4, hi, true); }
  return dv;
}

const MD5_S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
const MD5_K = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) >>> 0);
function md5(b) {
  const dv = pad64(b, false);
  let h = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
  const w = new Array(16);
  for (let o = 0; o < dv.byteLength; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(o + 4 * i, true);
    let [a, bb, c, d] = h;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) { f = (bb & c) | (~bb & d); g = i; }
      else if (i < 32) { f = (d & bb) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = bb ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (bb | ~d); g = (7 * i) & 15; }
      const t = d; d = c; c = bb;
      const x = (a + f + MD5_K[i] + w[g]) >>> 0, s = MD5_S[(i >> 4) * 4 + (i & 3)];
      bb = (bb + ((x << s) | (x >>> (32 - s)))) >>> 0;
      a = t;
    }
    h = [(h[0] + a) >>> 0, (h[1] + bb) >>> 0, (h[2] + c) >>> 0, (h[3] + d) >>> 0];
  }
  return h.map(v => [0, 8, 16, 24].map(s => ((v >>> s) & 255).toString(16).padStart(2, '0')).join('')).join('');
}

const SHA_K = [0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb,
  0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f,
  0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2];
function sha256(b) {
  const dv = pad64(b, true);
  const h = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
  const w = new Uint32Array(64), ror = (x, n) => (x >>> n) | (x << (32 - n));
  for (let o = 0; o < dv.byteLength; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(o + 4 * i);
    for (let i = 16; i < 64; i++) {
      const s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let [a, bb, c, d, e, f, g, hh] = h;
    for (let i = 0; i < 64; i++) {
      const t1 = (hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i]) >>> 0;
      const t2 = ((ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c))) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = bb; bb = a; a = (t1 + t2) >>> 0;
    }
    [a, bb, c, d, e, f, g, hh].forEach((v, i) => { h[i] = (h[i] + v) >>> 0; });
  }
  return h.map(v => v.toString(16).padStart(8, '0')).join('');
}

/* ---- small helpers ---- */

const hex2 = v => v.toString(16).toUpperCase().padStart(2, '0');
const sizeText = n => n >= 1048576 && !(n % 1048576) ? n / 1048576 + ' MB' : n >= 1024 && !(n % 1024) ? n / 1024 + ' KB' : n + ' B';
const SRC = { lectura: 'lectura', antes: 'antes de escribir', escrita: 'escrita', portal: 'del portal', vista: 'guardada' };
const stamp = () => {
  const d = new Date(), p = n => String(n).padStart(2, '0');
  return { file: `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`,
    date: `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}` };
};
const readJson = async path => { try { const t = await P.fsText(path); return t ? JSON.parse(t) : null; } catch { return null; } };

P.registerPage({
  id: 'eeprom',
  name: 'EEPROM',
  icon: '▦',
  render(main) {
    const { h, put } = P;
    let gone = false, board = null, chip = '', vers = [], a = null, b = null, view = null, reqId = null, poll = 0;
    const head = h('div', { class: 'card pad' });
    const listCard = h('div', { class: 'card' });
    const viewCard = h('div', { class: 'card' });
    const pick = h('select', { onchange: () => { chip = pick.value; a = b = null; view = null; loadList(); showView(); } });

    /* ---- the board's side ---- */

    const status = h('div', { class: 'eestat' });
    const bar = h('div', { class: 'prog', style: 'display:none' }, h('div'));
    const drawHead = () => {
      const c = board;
      put(head,
        h('div', { class: 'eehead' },
          h('b', {}, c ? c.chip : 'EEPROM'),
          c ? h('span', { class: 'muted' }, `${c.familia} · ${sizeText(c.tamano)} · ${c.conexion}`) : h('span', { class: 'muted' }, 'La app todavía no escribió su chip.json: abrila en la placa.'),
          c && c.abierta ? h('span', { class: 'pill' }, c.ocupada ? 'trabajando' : 'abierta') : null,
          c && c.cambios ? h('span', { class: 'pill', style: 'color:var(--orange)' }, `${c.cambios} bytes sin escribir en la placa`) : null),
        h('div', { class: 'btns', style: 'margin-top:12px' },
          h('button', { class: 'btn pri', onclick: () => ask({ op: 'leer' }, 'Leer el chip') }, 'Leer el chip ahora'),
          h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast('Abierta en la placa')).catch(e => P.toast(e.message, true)) }, 'Abrir la app'),
          h('label', { class: 'muted small' }, 'Chip en la placa: ',
            h('select', { onchange: e => ask({ op: 'chip', chip: e.target.value }, 'Cambiar el chip') },
              ...CHIPS.map(n => h('option', { value: n, selected: c && c.chip === n ? '' : null }, n))))),
        status, bar);
    };
    const refreshBoard = async () => {
      const c = await readJson(DIR + '/chip.json');
      const changed = JSON.stringify(c) !== JSON.stringify(board);
      board = c;
      if (changed) drawHead();
      if (c && !chip) { chip = c.chip; await loadFolders(); loadList(); }
    };

    /* A request: open the app, leave the file, follow its answer. */
    const ask = async (req, what) => {
      if (req.op === 'escribir' && !confirm(`Escribir ${req.archivo} en el ${req.chip}? Antes la placa guarda una copia de lo que tiene, y después lo verifica.`)) return;
      reqId = Math.random().toString(36).slice(2, 10);
      try {
        await P.openApp(APP_ID);
        await P.fsMkdir(DIR);
        await P.fsPut(DIR + '/pedido.json', JSON.stringify({ id: reqId, ...req }));
      } catch (e) { P.toast(e.message, true); reqId = null; return; }
      put(status, h('span', { class: 'muted' }, `${what}: esperando a la app…`));
      bar.style.display = '';
      bar.firstChild.style.width = '0';
      clearInterval(poll);
      const t0 = Date.now();
      poll = setInterval(async () => {
        if (gone) return clearInterval(poll);
        const s = await readJson(DIR + '/estado.json');
        if (!s || s.id !== reqId) {
          if (Date.now() - t0 > 8000) { clearInterval(poll); put(status, h('span', { class: 'bad' }, 'La app no contestó. ¿Está la placa encendida?')); bar.style.display = 'none'; }
          return;
        }
        bar.firstChild.style.width = Math.round((s.progreso || 0) * 100) + '%';
        if (s.estado === 'trabajando') { put(status, h('span', {}, `${what}: ${s.fase || 'trabajando'} ${Math.round((s.progreso || 0) * 100)} %`)); return; }
        clearInterval(poll);
        bar.style.display = 'none';
        reqId = null;
        const ok = s.estado === 'listo';
        put(status, h('span', { class: ok ? 'ok' : 'bad' }, (ok ? '✓ ' : '✗ ') + (s.mensaje || (ok ? 'Listo.' : 'Falló.')).replace(/\n/g, ' ')));
        await refreshBoard();
        if (req.op === 'chip') { chip = req.chip; await loadFolders(); }
        await loadList();
        if (ok && s.archivo && req.op === 'leer') { a = null; b = s.archivo; showView(); }
      }, 700);
    };

    /* ---- the versions ---- */

    const loadFolders = async () => {
      let dirs = [];
      try { dirs = (await P.fsList(DIR)).filter(e => e.dir && !e.name.startsWith('.')).map(e => e.name); } catch { }
      if (chip && !dirs.includes(chip)) dirs.push(chip);
      dirs.sort();
      put(pick, ...dirs.map(n => h('option', { value: n, selected: n === chip ? '' : null }, n)));
    };

    const loadList = async () => {
      if (!chip) { put(listCard, h('div', { class: 'pad muted' }, 'Elegí un chip.')); return; }
      let files = [];
      try { files = (await P.fsList(`${DIR}/${chip}`)).filter(f => !f.dir && /\.bin$/i.test(f.name) && !f.name.startsWith('.')); } catch { }
      files.sort((x, y) => y.name.localeCompare(x.name));
      vers = await Promise.all(files.map(async f => ({ ...f, meta: await readJson(`${DIR}/${chip}/${f.name.replace(/\.bin$/i, '.json')}`) || {} })));
      if (gone) return;
      drawList();
    };

    const drawList = () => {
      const up = h('input', { type: 'file', accept: '.bin,application/octet-stream', style: 'display:none', onchange: e => upload(e.target.files[0]) });
      const rows = vers.map(v => {
        const m = v.meta, src = m.source || '';
        return h('div', { class: 'eever' + (v.name === b || v.name === a ? ' sel' : '') },
          h('span', { class: 'eesrc ' + src }, SRC[src] || src || 'archivo'),
          h('span', { class: 'nm' }, m.date || v.name.replace(/\.bin$/i, '')),
          h('span', { class: 'eeab' },
            h('label', { title: 'Comparar desde esta' }, h('input', { type: 'radio', name: 'eeA', checked: v.name === a ? '' : null, onchange: () => { a = v.name; showView(); drawList(); } }), 'A'),
            h('label', { title: 'Ver esta' }, h('input', { type: 'radio', name: 'eeB', checked: v.name === b ? '' : null, onchange: () => { b = v.name; showView(); drawList(); } }), 'B')),
          h('div', { class: 'sub' }, `${sizeText(v.size)} · CRC32 ${m.crc32 || '—'}${m.note ? ' · ' + m.note : ''}`),
          h('div', { class: 'btns' },
            h('button', { class: 'btn', onclick: () => { b = v.name; showView(); drawList(); } }, 'Ver'),
            h('a', { class: 'btn', href: P.fsUrl(`${DIR}/${chip}/${v.name}`, true), download: `${chip}-${v.name}` }, 'Bajar'),
            h('button', { class: 'btn', onclick: () => editNote(v) }, 'Nota'),
            h('button', { class: 'btn', onclick: () => ask({ op: 'escribir', chip, archivo: v.name }, 'Escribir ' + v.name),
              disabled: board && board.chip === chip && v.size === board.tamano ? null : '',
              title: board && board.chip !== chip ? 'En la placa está elegido otro chip' : '' }, 'Escribir en el chip'),
            h('button', { class: 'btn red', onclick: () => remove(v) }, 'Borrar')));
      });
      put(listCard,
        h('div', { class: 'eetools' },
          h('b', {}, 'Versiones'), pick,
          h('span', { class: 'muted small' }, `${vers.length} en ${DIR}/${chip}`),
          h('span', { style: 'flex:1' }),
          h('button', { class: 'btn', onclick: () => up.click() }, 'Subir un .bin'), up),
        ...(rows.length ? rows : [h('div', { class: 'pad muted' }, 'Todavía no hay versiones de este chip. Cada lectura en la placa guarda una.')]));
    };

    const editNote = async v => {
      const note = prompt('Nota de ' + v.name, v.meta.note || '');
      if (note === null) return;
      const m = { ...v.meta, note };
      try { await P.fsPut(`${DIR}/${chip}/${v.name.replace(/\.bin$/i, '.json')}`, JSON.stringify(m) + '\n'); loadList(); }
      catch (e) { P.toast(e.message, true); }
    };

    const remove = async v => {
      if (!confirm(`Borrar ${v.name} de la tarjeta? No se puede deshacer.`)) return;
      try {
        await P.fsDelete(`${DIR}/${chip}/${v.name}`);
        await P.fsDelete(`${DIR}/${chip}/${v.name.replace(/\.bin$/i, '.json')}`).catch(() => {});
        if (a === v.name) a = null;
        if (b === v.name) { b = null; showView(); }
        loadList();
      } catch (e) { P.toast(e.message, true); }
    };

    const upload = async file => {
      if (!file || !chip) return;
      const bytes = new Uint8Array(await file.arrayBuffer());
      const want = board && board.chip === chip ? board.tamano : null;
      if (want && bytes.length !== want && !confirm(`${file.name} tiene ${sizeText(bytes.length)} y el ${chip} ${sizeText(want)}: no se va a poder escribir. ¿Subirlo igual?`)) return;
      const st = stamp(), name = `${st.file}-portal.bin`;
      const meta = { chip, size: bytes.length, date: st.date, source: 'portal', note: 'subido: ' + file.name,
        crc32: crc32(bytes), md5: md5(bytes), sha256: sha256(bytes) };
      try {
        await P.fsMkdir(DIR);
        await P.fsMkdir(`${DIR}/${chip}`);
        await P.fsPut(`${DIR}/${chip}/${name}`, new Blob([bytes]), f => put(status, h('span', { class: 'muted' }, `Subiendo ${Math.round(f * 100)} %`)));
        await P.fsPut(`${DIR}/${chip}/${name.replace(/\.bin$/, '.json')}`, JSON.stringify(meta) + '\n');
        put(status, h('span', { class: 'ok' }, `✓ Subido como ${name}`));
        b = name;
        await loadList();
        showView();
      } catch (e) { P.toast(e.message, true); }
    };

    /* ---- the hex view and the differences ---- */

    const cache = new Map();
    const bytesOf = async name => {
      const key = chip + '/' + name;
      if (!cache.has(key)) cache.set(key, new Uint8Array(await P.fsBytes(`${DIR}/${key}`)));
      return cache.get(key);
    };

    const showView = async () => {
      if (!b) { put(viewCard, h('div', { class: 'pad muted' }, 'Tocá Ver en una versión para verla en hexa; con A y B, las diferencias.')); return; }
      put(viewCard, h('div', { class: 'pad muted' }, 'Cargando…'));
      let B, A = null;
      try { B = await bytesOf(b); if (a && a !== b) A = await bytesOf(a); }
      catch (e) { put(viewCard, h('div', { class: 'pad bad' }, e.message)); return; }
      if (gone) return;
      const diff = [];
      let ndiff = 0;
      if (A) {
        const n = Math.max(A.length, B.length);
        for (let i = 0; i < n; i++) {
          if (A[i] === B[i]) continue;
          ndiff++;
          const last = diff[diff.length - 1];
          if (last && last[1] === i) last[1] = i + 1; else diff.push([i, i + 1]);
        }
      }
      view = { A, B, hit: -1, hitLen: 0 };
      const rowsN = Math.ceil(B.length / 16), RH = 20;
      const inner = h('div', { class: 'in' });
      const spacer = h('div', { style: `height:${rowsN * RH + 8}px` });
      const box = h('div', { class: 'eehex' }, spacer, inner);
      const adig = B.length > 0x10000 ? 6 : 4;
      const draw = () => {
        const first = Math.floor(box.scrollTop / RH), n = Math.ceil(box.clientHeight / RH) + 2;
        inner.style.top = first * RH + 4 + 'px';
        const lines = [];
        for (let r = first; r < Math.min(rowsN, first + n); r++) {
          let hx = '', as = '';
          for (let j = 0; j < 16; j++) {
            const i = r * 16 + j;
            if (i >= B.length) { hx += '   '; continue; }
            const v = B[i], d = A && A[i] !== v, hit = view.hit >= 0 && i >= view.hit && i < view.hit + view.hitLen;
            const cls = hit ? 'h' : d ? 'd' : v === 0 || v === 255 ? 'z' : '';
            const ch = v >= 32 && v < 127 ? String.fromCharCode(v).replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c])) : '.';
            hx += (j === 8 ? '  ' : ' ') + (cls ? `<span class="${cls}">${hex2(v)}</span>` : hex2(v));
            as += cls ? `<span class="${cls}">${ch}</span>` : ch;
          }
          lines.push(`<div class="ln"><span class="ad">${r.toString(16).toUpperCase().padStart(adig - 1, '0')}0</span> ${hx}  ${as}</div>`);
        }
        inner.innerHTML = lines.join('');
      };
      const go = (i, len) => {
        view.hit = i; view.hitLen = len || 1;
        box.scrollTop = Math.max(0, Math.floor(i / 16) * RH - box.clientHeight / 3);
        draw();
      };
      box.addEventListener('scroll', draw);
      const addr = h('input', { type: 'text', placeholder: 'dirección (hexa)', onkeydown: e => { if (e.key === 'Enter') { const v = parseInt(addr.value.replace(/^0x/i, ''), 16); if (v >= 0 && v < B.length) go(v); } } });
      const find = h('input', { type: 'text', placeholder: 'texto o DE AD BE EF' });
      const search = () => {
        const q = find.value.trim();
        if (!q) return;
        const hexq = /^([0-9a-f]{2}[\s,]*)+$/i.test(q) && q.replace(/[\s,]/g, '').length % 2 === 0;
        const pat = hexq ? q.replace(/[\s,]/g, '').match(/../g).map(x => parseInt(x, 16)) : [...new TextEncoder().encode(q)];
        for (let k = 0, from = view.hit + 1; k < B.length; k++) {
          const i = (from + k) % B.length;
          let m = i + pat.length <= B.length;
          for (let j = 0; m && j < pat.length; j++) m = B[i + j] === pat[j];
          if (m) return go(i, pat.length);
        }
        P.toast('No está', true);
      };
      find.addEventListener('keydown', e => { if (e.key === 'Enter') search(); });
      const sums = h('div', { class: 'pad mono eesums muted' }, `CRC32 ${crc32(B)} · MD5 ${md5(B)} · SHA-256 ${sha256(B)}`);
      put(viewCard,
        h('div', { class: 'eetools' },
          h('b', {}, b), A ? h('span', { class: 'muted' }, `comparada con ${a}: `, h('span', { class: ndiff ? 'bad' : 'ok' }, ndiff ? `${ndiff} bytes distintos en ${diff.length} tramos` : 'iguales')) : null,
          h('span', { style: 'flex:1' }), addr, find, h('button', { class: 'btn', onclick: search }, 'Buscar'),
          h('a', { class: 'btn', href: P.fsUrl(`${DIR}/${chip}/${b}`, true), download: `${chip}-${b}` }, 'Bajar')),
        diff.length ? h('div', { class: 'eeranges' }, ...diff.slice(0, 300).map(([s, e]) =>
          h('a', { href: '#eeprom', onclick: ev => { ev.preventDefault(); go(s, e - s); } }, `${s.toString(16).toUpperCase()}${e - s > 1 ? '–' + (e - 1).toString(16).toUpperCase() : ''}`)),
          diff.length > 300 ? h('span', { class: 'muted small' }, `y ${diff.length - 300} tramos más`) : null) : null,
        box, sums);
      requestAnimationFrame(draw);
    };

    put(main,
      h('h1', {}, 'EEPROM'),
      head,
      h('p', { class: 'note' }, 'Las versiones están en la tarjeta, en /eeprom/<chip>. Cada lectura en la placa guarda una; antes de cada escritura se guarda lo que había.'),
      listCard,
      h('div', { style: 'height:16px' }),
      viewCard);
    drawHead();
    put(listCard, h('div', { class: 'pad muted' }, 'Cargando…'));
    showView();
    refreshBoard().then(async () => { if (!chip) { await loadFolders(); chip = pick.value; loadList(); } });
    const t = setInterval(() => { if (!reqId) refreshBoard(); }, 4000);
    return () => { gone = true; clearInterval(t); clearInterval(poll); };
  },
});
