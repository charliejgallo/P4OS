/* The CAN app's page in the board's portal (docs/PORTAL-PAGES.md): the spy
 * live, sending, the recordings, and the files the app reads.
 *
 * The page and the app meet in two files of /data, with no firmware of
 * their own:
 *   /data/can_cmd.txt    the page's orders, a line each: "watch",
 *                        "send 123#1122", "rec on", "rec off", "clear",
 *                        "reload". The app reads it within a second and
 *                        deletes it; the page writes the next batch only
 *                        once it is gone.
 *   /data/can_live.json  the app's snapshot: the state, every id with its
 *                        last data and the bytes that just changed, the
 *                        last frames. Written once a second for 15 s after
 *                        each "watch", which the page repeats while open.
 * So the app must be open on the board (in front or not); the bus is
 * opened there. The recordings are the CSVs in /can (SavvyCAN's format),
 * the saved frames /can/enviar.txt and the signals /can/senales.dbc or any
 * .dbc of /can/dbc, as /data/can_dbc.txt says (written by the app and by
 * this page: "file dbc/x.dbc", "show bus" or "show 0B4 1C4").
 *
 * The .dbc is read here too, with the same rules as the app's reader
 * (can_dbc.c: BO_, SG_ with multiplexing, SG_MUL_VAL_, VAL_), and the
 * signals are decoded from the app's snapshot: the last frame of each id,
 * and of the multiplexed messages the last frame of each multiplexor value
 * ("mux"). The OBD-II scanner runs in the app ("obd on", "obd on29",
 * "obd off"); the snapshot brings its answers' bytes ("obd") and the page
 * decodes them with the same J1979 formulas as can_obd.c. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const LOC = { es: 'es-AR', en: 'en-US', de: 'de-DE' }[P.lang ? P.lang() : 'es'] || 'es-AR';
const APP_ID = 'aos.can';
const CMD = '/data/can_cmd.txt';
const LIVE = '/data/can_live.json';
const DIR = '/can';
const SAVED = DIR + '/enviar.txt';
const DBC = DIR + '/senales.dbc';
const DBCDIR = DIR + '/dbc';
const SEL = '/data/can_dbc.txt';
const SHOW_MAX = 64;

if (!document.getElementById('css-can')) {
  const st = document.createElement('style');
  st.id = 'css-can';
  st.textContent = `
.cnsum { display: flex; gap: 22px; flex-wrap: wrap; align-items: baseline; }
.cnsum b { font-size: 24px; font-variant-numeric: tabular-nums; }
.cndot { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; background: var(--dim); }
.cntbl { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
.cntbl th { text-align: left; font-weight: 500; color: var(--dim); font-size: 13px; padding: 8px 12px; }
.cntbl td { padding: 6px 12px; border-top: 1px solid var(--line); white-space: nowrap; }
.cntbl td.num { text-align: right; color: var(--dim); }
.cntbl .id { color: var(--accent); }
.cnb { display: inline-block; min-width: 2.2ch; padding: 1px 3px; margin-right: 3px; border-radius: 4px; }
.cnb.hot { background: var(--orange); color: #000; }
.cnlast { max-height: 320px; overflow: auto; padding: 8px 14px; line-height: 1.55; white-space: pre; word-break: normal; }
.cnlast .tx { color: var(--accent); }
.cnin { flex: 1; min-width: 180px; font: inherit; padding: 9px 12px; border-radius: 10px; border: 1px solid var(--line); background: var(--card2); color: inherit; }
.cnedit { width: 100%; min-height: 180px; box-sizing: border-box; border: 0; border-top: 1px solid var(--line); padding: 12px 14px; background: transparent; color: inherit; resize: vertical; }
.cnscroll { overflow-x: auto; }
.cnwarn { color: var(--orange); }
.cnpick { max-height: 360px; overflow: auto; }
.cnpick label { display: flex; gap: 10px; align-items: center; padding: 6px 14px; border-top: 1px solid var(--line); cursor: pointer; }
.cnpick label .mono { min-width: 9ch; color: var(--accent); }
.cnseen { color: var(--green); font-size: 13px; }
.cnval { text-align: right; color: var(--accent); }
`;
  document.head.append(st);
}

const STATE = { active: [N_('activo'), 'var(--green)'], warning: [N_('con avisos'), '#ffd60a'], passive: [N_('pasivo'), 'var(--orange)'], bus_off: [N_('fuera del bus'), 'var(--red)'] };
const MODE = { listen: N_('Solo escucha'), normal: N_('Normal'), selftest: N_('Autoprueba') };
const rate = r => r % 1000000 === 0 ? `${r / 1000000} Mbit/s` : r % 1000 === 0 ? `${r / 1000} kbit/s` : `${r} bit/s`;
const when = t => t ? new Date(t * 1000).toLocaleString(LOC, { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }) : '';
/* candump's form: 123#11223344, 18FEF100#FF00, 7DF#R */
const FRAME_RE = /^([0-9a-f]{1,8})#(r[0-8]?|([0-9a-f]{2}[.:]?){0,8})$/i;

/* ---- the .dbc, as can_dbc.c reads it ---- */
function parseDbc(text) {
  const msgs = [], byKey = new Map();
  let cur = null, inStr = false;
  const quotes = l => { let n = 0; for (let i = 0; i < l.length; i++) { if (l[i] === '\\') i++; else if (l[i] === '"') n++; } return n; };
  const findSig = (m, name) => m ? m.sigs.find(g => g.name === name) : null;
  const key = (id, ext) => (ext ? 'x' : 's') + id;
  const idOf = raw => { const ext = raw >= 0x80000000 || (raw & 0x1FFFFFFF) > 0x7FF; return [raw & 0x1FFFFFFF, ext]; };
  for (const line of text.split('\n')) {
    const t = line.trim();
    if (inStr) { if (quotes(t) & 1) inStr = false; continue; }
    let m;
    if ((m = /^BO_\s+(\d+)\s+([^\s:]+)\s*:\s*(\d+)/.exec(t))) {
      const raw = Number(m[1]);
      cur = null;
      if (m[2] === 'VECTOR__INDEPENDENT_SIG_MSG') continue;
      const [id, ext] = idOf(raw >>> 0);
      cur = { id: id >>> 0, ext, name: m[2], dlc: +m[3], sigs: [], mux: null };
      msgs.push(cur);
      byKey.set(key(cur.id, ext), cur);
    } else if ((m = /^SG_\s+(\S+)\s*(M|m(\d+)(M?))?\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(([^,]+),([^)]+)\)\s*\[[^\]]*\]\s*"([^"]*)"/.exec(t))) {
      if (!cur) continue;
      const g = { name: m[1], start: +m[5], len: +m[6], motorola: m[7] === '0', signed: m[8] === '-',
        scale: parseFloat(m[9]), offset: parseFloat(m[10]), unit: m[11], isMux: m[2] === 'M' || m[4] === 'M',
        muxed: m[3] !== undefined, lo: +(m[3] || 0), hi: +(m[3] || 0), muxsig: null, vals: null };
      if (g.len < 1 || g.len > 64) continue;
      if (m[2] === 'M' && !cur.mux) cur.mux = g;
      cur.sigs.push(g);
    } else if (t) {
      cur = null;
      if ((m = /^VAL_\s+(\d+)\s+(\S+)\s+(.*)$/.exec(t))) {
        const [id, ext] = idOf(Number(m[1]) >>> 0);
        const g = findSig(byKey.get(key(id, ext)), m[2]);
        if (g && !g.vals) {
          g.vals = new Map();
          for (const v of m[3].matchAll(/(-?\d+)\s+"([^"]*)"/g)) g.vals.set(+v[1], v[2]);
        }
      } else if ((m = /^SG_MUL_VAL_\s+(\d+)\s+(\S+)\s+(\S+)\s+(-?\d+)-(-?\d+)/.exec(t))) {
        const [id, ext] = idOf(Number(m[1]) >>> 0);
        const msg = byKey.get(key(id, ext));
        const g = findSig(msg, m[2]), mx = findSig(msg, m[3]);
        if (g && mx && g !== mx) Object.assign(g, { muxed: true, muxsig: mx, lo: +m[4], hi: +m[5] });
      } else if (quotes(t) & 1) inStr = true;
    }
  }
  for (const msg of msgs) for (const g of msg.sigs) { g.msg = msg; if (g.muxed && !g.muxsig) g.muxsig = msg.mux; }
  return { msgs, byKey, find: (id, ext) => byKey.get(key(id, ext)) };
}

/* a signal's raw value from the frame's bytes (a Number: exact up to 2^53) */
function sigRaw(g, d) {
  let raw = 0, pos = g.start;
  for (let i = 0; i < g.len; i++) {
    const p = g.motorola ? pos : g.start + i;
    if ((p >> 3) >= d.length) return null;
    const b = d[p >> 3] >> (p & 7) & 1;
    if (g.motorola) { raw = raw * 2 + b; pos = (pos & 7) === 0 ? pos + 15 : pos - 1; }
    else raw += b * 2 ** i;
  }
  if (g.signed && raw >= 2 ** (g.len - 1)) raw -= 2 ** g.len;
  return raw;
}
function sigPresent(g, d, depth = 0) {
  if (!g.muxed || !g.muxsig || g.muxsig === g) return true;
  if (depth < 4 && !sigPresent(g.muxsig, d, depth + 1)) return false;
  const r = sigRaw(g.muxsig, d);
  return r !== null && r >= g.lo && r <= g.hi;
}
const hexBytes = s => s.split(' ').filter(Boolean).map(b => parseInt(b, 16));
const fmtNum = v => Math.abs(v) >= 1000 || Number.isInteger(v) ? String(Math.round(v)) : v.toFixed(Math.abs(v) >= 10 ? 1 : 2);
const idText = (id, ext) => id.toString(16).toUpperCase().padStart(ext ? 8 : 3, '0');

/* ---- OBD-II, mode 01: the formulas of SAE J1979, as can_obd.c ---- */
const PCT = 100 / 255, TRIM = 100 / 128;
const OBD = [
  [0x01, 4, 'status', N_('Luz de falla y códigos'), '', 1, 0], [0x04, 1, 0, N_('Carga calculada'), '%', PCT, 0],
  [0x05, 1, 0, N_('Refrigerante'), '°C', 1, -40], [0x06, 1, 0, N_('Corrección corta, banco 1'), '%', TRIM, -100],
  [0x07, 1, 0, N_('Corrección larga, banco 1'), '%', TRIM, -100], [0x08, 1, 0, N_('Corrección corta, banco 2'), '%', TRIM, -100],
  [0x09, 1, 0, N_('Corrección larga, banco 2'), '%', TRIM, -100], [0x0A, 1, 0, N_('Presión de combustible'), 'kPa', 3, 0],
  [0x0B, 1, 0, N_('Presión del múltiple'), 'kPa', 1, 0], [0x0C, 2, 0, N_('Régimen del motor'), 'rpm', 0.25, 0],
  [0x0D, 1, 0, N_('Velocidad'), 'km/h', 1, 0], [0x0E, 1, 0, N_('Avance del encendido'), '°', 0.5, -64],
  [0x0F, 1, 0, N_('Aire de admisión'), '°C', 1, -40], [0x10, 2, 0, N_('Caudal de aire (MAF)'), 'g/s', 0.01, 0],
  [0x11, 1, 0, N_('Mariposa'), '%', PCT, 0], [0x14, 1, 0, N_('Sonda de oxígeno, banco 1, 1'), 'V', 0.005, 0],
  [0x15, 1, 0, N_('Sonda de oxígeno, banco 1, 2'), 'V', 0.005, 0], [0x1C, 1, 'std', N_('Norma OBD'), '', 1, 0],
  [0x1F, 2, 0, N_('Desde el arranque'), 's', 1, 0], [0x21, 2, 0, N_('Recorrido con la luz de falla'), 'km', 1, 0],
  [0x22, 2, 0, N_('Presión del riel (relativa)'), 'kPa', 0.079, 0], [0x23, 2, 0, N_('Presión del riel'), 'kPa', 10, 0],
  [0x2C, 1, 0, N_('EGR comandada'), '%', PCT, 0], [0x2D, 1, 0, N_('Error de la EGR'), '%', TRIM, -100],
  [0x2E, 1, 0, N_('Purga del cánister'), '%', PCT, 0], [0x2F, 1, 0, N_('Nivel de combustible'), '%', PCT, 0],
  [0x30, 1, 0, N_('Calentamientos desde el borrado'), '', 1, 0], [0x31, 2, 0, N_('Recorrido desde el borrado'), 'km', 1, 0],
  [0x33, 1, 0, N_('Presión barométrica'), 'kPa', 1, 0], [0x3C, 2, 0, N_('Catalizador, banco 1, 1'), '°C', 0.1, -40],
  [0x3D, 2, 0, N_('Catalizador, banco 2, 1'), '°C', 0.1, -40], [0x42, 2, 0, N_('Tensión del módulo'), 'V', 0.001, 0],
  [0x43, 2, 0, N_('Carga absoluta'), '%', PCT, 0], [0x44, 2, 0, N_('Lambda comandada'), '', 1 / 32768, 0],
  [0x45, 1, 0, N_('Mariposa relativa'), '%', PCT, 0], [0x46, 1, 0, N_('Temperatura ambiente'), '°C', 1, -40],
  [0x47, 1, 0, N_('Mariposa absoluta B'), '%', PCT, 0], [0x49, 1, 0, N_('Pedal D'), '%', PCT, 0],
  [0x4A, 1, 0, N_('Pedal E'), '%', PCT, 0], [0x4C, 1, 0, N_('Mariposa comandada'), '%', PCT, 0],
  [0x4D, 2, 0, N_('Tiempo con la luz de falla'), 'min', 1, 0], [0x4E, 2, 0, N_('Tiempo desde el borrado'), 'min', 1, 0],
  [0x51, 1, 'fuel', N_('Combustible'), '', 1, 0], [0x52, 1, 0, N_('Etanol'), '%', PCT, 0],
  [0x59, 2, 0, N_('Presión del riel (absoluta)'), 'kPa', 10, 0], [0x5A, 1, 0, N_('Pedal relativo'), '%', PCT, 0],
  [0x5B, 1, 0, N_('Batería del híbrido'), '%', PCT, 0], [0x5C, 1, 0, N_('Aceite'), '°C', 1, -40],
  [0x5D, 2, 0, N_('Inyección'), '°', 1 / 128, -210], [0x5E, 2, 0, N_('Consumo'), 'L/h', 0.05, 0],
  [0x61, 1, 0, N_('Par pedido'), '%', 1, -125], [0x62, 1, 0, N_('Par real'), '%', 1, -125],
  [0x63, 2, 0, N_('Par de referencia'), 'Nm', 1, 0], [0xA6, 4, 0, N_('Odómetro'), 'km', 0.1, 0],
];
const OBD_BY = new Map(OBD.map(r => [r[0], r]));
const FUEL = [N_('sin dato'), N_('nafta'), N_('metanol'), N_('etanol'), N_('diésel'), N_('GLP'), N_('GNC'), N_('propano'), N_('eléctrico')];
const OBDSTD = ['', 'OBD-II (CARB)', 'OBD (EPA)', 'OBD + OBD-II', 'OBD-I', N_('sin OBD'), 'EOBD', 'EOBD + OBD-II', 'EOBD + OBD', 'EOBD + OBD + OBD-II', 'JOBD', 'JOBD + OBD-II', 'JOBD + EOBD', 'JOBD + EOBD + OBD-II'];
function obdValue(pid, bytes) {
  const r = OBD_BY.get(pid);
  if (!r) return bytes.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
  const [, nb, kind, , unit, k, o] = r;
  if (bytes.length < (kind === 'status' ? 1 : nb)) return '?';
  const a = bytes[0];
  if (kind === 'status') return tr(a & 0x80 ? 'encendida · %d códigos' : 'apagada · %d códigos', a & 0x7F);
  if (kind === 'fuel') return a < FUEL.length ? tr(FUEL[a]) : tr('código %d', a);
  if (kind === 'std') return a === 5 ? tr(OBDSTD[5]) : a > 0 && a < OBDSTD.length ? OBDSTD[a] : tr('código %d', a);
  let raw = 0;
  for (let i = 0; i < nb; i++) raw = raw * 256 + bytes[i];
  const v = raw * k + o;
  const dec = unit === 'rpm' ? 0 : k >= 0.99 && Number.isInteger(o) ? 0 : k === PCT || k === TRIM || k >= 0.09 ? 1 : unit ? 2 : 3;
  return v.toLocaleString(LOC, { minimumFractionDigits: dec, maximumFractionDigits: dec }) + (unit ? ' ' + unit : '');
}

P.registerPage({
  id: 'can',
  name: tr('CAN'),
  icon: '⇆',
  render(main) {
    const { h, put } = P;
    const sum = h('div', { class: 'card pad' });
    const ids = h('div', { class: 'card cnscroll' });
    const last = h('div', { class: 'card mono cnlast' });
    const send = h('div', { class: 'card pad' });
    const recs = h('div', { class: 'card' });
    const files = h('div', { class: 'card' });
    let live = null, lastN = -1, staleFor = 0, gone = false;
    let queue = [], pending = false;

    /* ---- orders to the app ---- */
    const order = line => { if (!queue.includes(line) || line.startsWith('send')) queue.push(line); pump(); };
    async function pump() {
      if (pending || !queue.length || gone) return;
      pending = true;
      try {
        await P.fsMkdir('/data').catch(() => {});
        while (!gone && await P.fsText(CMD) !== null) await new Promise(r => setTimeout(r, 400));
        const batch = queue.splice(0, 16);
        await P.fsPut(CMD, batch.join('\n') + '\n');
      } catch (e) { P.toast(e.message, true); }
      pending = false;
      if (queue.length) setTimeout(pump, 300);
    }

    /* ---- the state ---- */
    const openBtn = () => h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast(tr('Abierta en la placa'))).catch(e => P.toast(e.message, true)) }, tr('Abrir CAN en la placa'));
    /* the numbers change every second, the buttons only with the state: they
     * are redrawn apart, so a click is not lost under a redraw */
    const sumInfo = h('div', {}), sumBtns = h('div', { class: 'btns', style: 'margin-top:12px' });
    put(sum, sumInfo, sumBtns);
    let btnKey = '';
    function drawSummary() {
      const stale = !live || staleFor >= 5;
      const key = stale ? 'stale' : `${live.open}|${!!live.rec}`;
      if (key !== btnKey) {
        btnKey = key;
        put(sumBtns,
          !stale && live.open && (live.rec
            ? h('button', { class: 'btn red', onclick: () => order('rec off') }, tr('Parar la grabación'))
            : h('button', { class: 'btn', onclick: () => order('rec on') }, tr('Grabar'))),
          !stale && live.open && h('button', { class: 'btn', onclick: () => order('clear') }, tr('Limpiar')),
          openBtn());
      }
      if (stale) {
        put(sumInfo,
          h('div', { class: 'row', style: 'padding:0' }, h('span', { class: 'cndot' }), h('div', { class: 'grow' }, live ? tr('La app dejó de contestar.') : tr('Esperando a la app…'))),
          h('p', { class: 'note' }, tr('La página ve el bus a través de la app CAN: tiene que estar abierta en la placa (al frente o atrás). El bus se conecta desde ahí.')));
        return;
      }
      const L = live;
      const [sname0, scolor] = L.open ? (STATE[L.state] || [L.state, 'var(--red)']) : [N_('desconectado'), 'var(--dim)'];
      const sname = tr(sname0);
      const head = L.open
        ? `${rate(L.rate)} · ${MODE[L.mode] ? tr(MODE[L.mode]) : L.mode} · ${L.tx === L.rx ? 'GPIO' + L.tx : `TX ${L.tx} · RX ${L.rx}`}`
        : tr('Sin bus: se conecta desde la app, tocando la conexión.');
      put(sumInfo,
        h('div', { class: 'row', style: 'padding:0' }, h('span', { class: 'cndot', style: `background:${scolor}` }), h('div', { class: 'grow' }, h('b', {}, sname), ' · ', head)),
        L.open && h('div', { class: 'cnsum', style: 'margin-top:10px' },
          h('div', {}, h('b', {}, String(Math.round(L.fps))), ' ', h('span', { class: 'muted small' }, tr('tramas/s'))),
          h('div', {}, h('b', {}, Math.round(L.load * 100) + ' %'), ' ', h('span', { class: 'muted small' }, tr('de carga'))),
          h('div', {}, h('b', {}, String(L.rxn)), ' ', h('span', { class: 'muted small' }, tr('recibidas'))),
          h('div', {}, h('b', {}, String(L.txn)), ' ', h('span', { class: 'muted small' }, tr('enviadas'))),
          h('div', {}, h('b', {}, String(L.drop)), ' ', h('span', { class: 'muted small' }, tr('perdidas'))),
          h('div', {}, h('b', {}, `${L.txe} / ${L.rxe}`), ' ', h('span', { class: 'muted small' }, tr('errores TX / RX')))),
        L.rec && h('p', { class: 'note' }, tr('Grabando %s: %d tramas.', L.rec, L.recn)),
        L.replay && h('p', { class: 'note' }, tr('Repitiendo %s.', L.replay)));
    }

    function drawSpy() {
      if (!live || !live.ids?.length) {
        put(ids, h('div', { class: 'row muted' }, live?.open ? tr('Esperando tramas…') : tr('Sin tramas.')));
        put(last);
        return;
      }
      put(ids, h('table', { class: 'cntbl' },
        h('tr', {}, h('th', {}, tr('id')), h('th', {}, tr('n')), h('th', {}, tr('datos')), h('th', { style: 'text-align:right' }, tr('por s')), h('th', { style: 'text-align:right' }, tr('vistas'))),
        live.ids.slice().sort((a, b) => a[1] - b[1] || parseInt(a[0], 16) - parseInt(b[0], 16)).map(([id, ext, len, data, recent, hz, n]) => h('tr', {},
          h('td', { class: 'mono id' }, id + (ext ? ' ·29' : '')),
          h('td', { class: 'num' }, String(len)),
          h('td', { class: 'mono' }, data.split(' ').filter(Boolean).map((b, i) => h('span', { class: 'cnb' + (recent >> i & 1 ? ' hot' : '') }, b))),
          h('td', { class: 'num' }, hz.toFixed(1)),
          h('td', { class: 'num' }, String(n))))));
      put(last, live.last.slice().reverse().map(([t, id, fl, data]) =>
        h('div', { class: fl & 4 ? 'tx' : '' }, `${(t / 1000).toFixed(3).padStart(9)}  ${fl & 4 ? '>' : ' '} ${id.padEnd(8)}  ${data}`)));
    }

    async function poll() {
      let t = null;
      try { t = await P.fsText(LIVE); } catch { /* not there yet */ }
      if (gone) return;
      let j = null;
      try { j = t ? JSON.parse(t) : null; } catch { /* caught half written: next time */ }
      if (j && j.n !== lastN) { live = j; lastN = j.n; staleFor = 0; }
      else staleFor++;
      drawSummary();
      drawSpy();
      drawSendHint();
      drawSigVals();
      drawObd();
    }

    /* ---- the signals: which .dbc, which of its messages, their values ---- */
    const sigHead = h('div', {}), sigPick = h('div', { class: 'cnpick' }), sigVals = h('div', { class: 'cnscroll' });
    const sigBox = h('div', { class: 'card' }, sigHead, sigPick);
    const search = h('input', { class: 'cnin', placeholder: tr('Buscar un mensaje por id o nombre'), spellcheck: false });
    search.addEventListener('input', () => drawPick());
    let sel = { file: 'senales.dbc', bus: true, show: [] }, dbc = null, dbcFile = null, dbcErr = '', dbcFiles = [];
    const showKey = (id, ext) => (ext ? 'x' : 's') + id;

    async function loadSel() {
      let t = null;
      try { t = await P.fsText(SEL); } catch { /* none yet */ }
      sel = { file: 'senales.dbc', bus: true, show: [] };
      for (const l of (t || '').split('\n')) {
        const m = /^\s*(file|show)\s+(.+?)\s*$/.exec(l);
        if (!m) continue;
        if (m[1] === 'file' && !m[2].startsWith('/') && !m[2].includes('..')) sel.file = m[2];
        if (m[1] === 'show') {
          sel.bus = m[2] === 'bus';
          sel.show = sel.bus ? [] : m[2].split(/\s+/).filter(x => /^[0-9a-f]{1,8}$/i.test(x)).slice(0, SHOW_MAX)
            .map(x => showKey(parseInt(x, 16), x.length > 3 || parseInt(x, 16) > 0x7FF));
          if (!sel.show.length) sel.bus = true;
        }
      }
    }
    async function saveSel() {
      const ids = sel.show.map(k => idText(Number(k.slice(1)), k[0] === 'x'));
      const text = '# CAN: el .dbc de la pestana Senales (en <tarjeta>/can) y los mensajes que muestra.\n' +
        '# show bus: los que pasan por el bus. show 0B4 1C4: esos ids.\n' +
        `file ${sel.file}\nshow ${sel.bus || !ids.length ? 'bus' : ids.join(' ')}\n`;
      try { await P.fsMkdir('/data').catch(() => {}); await P.fsPut(SEL, text); order('reload'); }
      catch (e) { P.toast(e.message, true); }
    }
    async function loadFiles() {
      let top = [], sub = [];
      try { top = await P.fsList(DIR); sub = await P.fsList(DBCDIR); } catch (e) { P.toast(e.message, true); }
      if (gone) return;
      dbcFiles = [];
      const own = top.find(f => !f.dir && f.name === 'senales.dbc');
      if (own) dbcFiles.push({ path: 'senales.dbc', size: own.size });
      for (const f of sub.filter(f => !f.dir && !f.name.startsWith('.') && /\.dbc$/i.test(f.name)).sort((a, b) => a.name.localeCompare(b.name)))
        dbcFiles.push({ path: 'dbc/' + f.name, size: f.size });
      drawSigHead();
    }
    async function loadDbc(force) {
      if (!force && dbcFile === sel.file) return;
      dbcFile = sel.file;
      dbc = null;
      dbcErr = '';
      let t = null;
      try { t = await P.fsText(`${DIR}/${sel.file}`); } catch (e) { dbcErr = e.message; }
      if (gone) return;
      if (t === null && !dbcErr) dbcErr = tr('No está can/%s', sel.file);
      if (t !== null) dbc = parseDbc(t);
      drawSigHead();
      drawPick();
      drawSigVals();
    }
    async function upload(list) {
      try {
        await P.fsMkdir(DIR).catch(() => {});
        await P.fsMkdir(DBCDIR).catch(() => {});
        for (const f of list) {
          if (!/\.dbc$/i.test(f.name)) { P.toast(tr('%s no es un .dbc', f.name), true); continue; }
          await P.fsPut(`${DBCDIR}/${f.name.replace(/[^\w.+-]/g, "_")}`, f);
        }
        P.toast(tr('Subido a can/dbc'));
      } catch (e) { P.toast(e.message, true); }
      loadFiles();
    }
    const fileIn = h('input', { type: 'file', accept: '.dbc', multiple: true, style: 'display:none' });
    fileIn.addEventListener('change', () => { if (fileIn.dbcFiles.length) upload([...fileIn.dbcFiles]); fileIn.value = ''; });
    function drawSigHead() {
      const nsig = dbc ? dbc.msgs.reduce((n, m) => n + m.sigs.length, 0) : 0;
      const pick = h('select', { class: 'cnin', onchange: async e => {
        sel = { file: e.target.value, bus: true, show: [] };
        await saveSel();
        loadDbc(true);
      } }, (dbcFiles.some(f => f.path === sel.file) ? dbcFiles : [{ path: sel.file, size: 0 }, ...dbcFiles]).map(f =>
        h('option', { value: f.path, selected: f.path === sel.file }, f.size ? `${f.path} · ${P.fmtBytes(f.size)}` : f.path)));
      put(sigHead,
        h('div', { class: 'row', style: 'gap:10px;flex-wrap:wrap' }, pick,
          h('button', { class: 'btn', onclick: () => fileIn.click() }, tr('Subir .dbc')),
          sel.file.startsWith('dbc/') && h('button', { class: 'btn red', onclick: async () => {
            if (!confirm(tr('¿Borrar %s de la tarjeta? No se puede deshacer.', sel.file))) return;
            try { await P.fsDelete(`${DIR}/${sel.file}`); sel = { file: 'senales.dbc', bus: true, show: [] }; await saveSel(); loadFiles(); loadDbc(true); }
            catch (e) { P.toast(e.message, true); }
          } }, tr('Borrar')), fileIn),
        h('p', { class: 'note', style: 'padding:0 14px' }, dbcErr || (dbc ? tr('%d mensajes, %d señales. La placa muestra los mensajes elegidos acá, o los que pasan por el bus.', dbc.msgs.length, nsig) : tr('Leyendo…'))),
        h('p', { class: 'note', style: 'padding:0 14px' }, tr('Los .dbc van en la carpeta can/dbc de la tarjeta. Bibliotecas abiertas: opendbc de comma.ai (github.com/commaai/opendbc, carpeta opendbc/dbc) y nberlette/canbus (carpeta dbc, con OBD-II y J1939 genéricos); las dos con licencia MIT.')),
        dbc && h('div', { class: 'row', style: 'gap:14px;flex-wrap:wrap' },
          h('label', {}, h('input', { type: 'radio', name: 'cnshow', checked: sel.bus, onchange: () => { sel.bus = true; saveSel(); drawSigVals(); } }), ' ', tr('Los que pasan por el bus')),
          h('label', {}, h('input', { type: 'radio', name: 'cnshow', checked: !sel.bus, onchange: () => {
            if (!sel.show.length) { P.toast(tr('Marcá los mensajes que querés ver'), true); sel.bus = true; drawSigHead(); return; }
            sel.bus = false; saveSel(); drawSigVals();
          } }), ' ', tr('Elegidos: %d', sel.show.length)),
          search));
    }
    function seenSet() {
      const s = new Set();
      if (live && staleFor < 5) for (const [id, ext] of live.ids || []) s.add(showKey(parseInt(id, 16), !!ext));
      return s;
    }
    function drawPick() {
      if (!dbc) { put(sigPick); return; }
      const q = search.value.trim().toLowerCase(), seen = seenSet();
      const list = dbc.msgs.filter(m => !q || m.name.toLowerCase().includes(q) || idText(m.id, m.ext).toLowerCase().includes(q))
        .sort((a, b) => seen.has(showKey(b.id, b.ext)) - seen.has(showKey(a.id, a.ext)) || a.ext - b.ext || a.id - b.id);
      put(sigPick, list.slice(0, 400).map(m => {
        const k = showKey(m.id, m.ext);
        return h('label', {},
          h('input', { type: 'checkbox', checked: sel.show.includes(k), onchange: e => {
            if (e.target.checked) {
              if (sel.show.length >= SHOW_MAX) { e.target.checked = false; P.toast(tr('Hasta %d mensajes', SHOW_MAX), true); return; }
              sel.show.push(k);
            } else sel.show = sel.show.filter(x => x !== k);
            sel.bus = !sel.show.length;
            saveSel();
            drawSigHead();
            drawSigVals();
          } }),
          h('span', { class: 'mono' }, idText(m.id, m.ext)), h('span', { class: 'grow' }, m.name),
          h('span', { class: 'muted small' }, tr('%d señales', m.sigs.length)),
          seen.has(k) && h('span', { class: 'cnseen' }, tr('en el bus')));
      }), list.length > 400 && h('div', { class: 'row muted' }, tr('Y %d más: buscá para acotar.', list.length - 400)));
    }
    function drawSigVals() {
      if (!dbc) { put(sigVals); return; }
      const fresh = live && staleFor < 5;
      const frames = new Map(), muxed = new Map();
      if (fresh) {
        for (const [id, ext, , data] of live.ids || []) frames.set(showKey(parseInt(id, 16), !!ext), hexBytes(data));
        for (const [id, ext, data] of live.mux || []) {
          const k = showKey(parseInt(id, 16), !!ext);
          if (!muxed.has(k)) muxed.set(k, []);
          muxed.get(k).push(hexBytes(data));
        }
      }
      const msgs = dbc.msgs.filter(m => sel.bus ? frames.has(showKey(m.id, m.ext)) : sel.show.includes(showKey(m.id, m.ext)))
        .sort((a, b) => a.ext - b.ext || a.id - b.id);
      if (!msgs.length) {
        put(sigVals, h('div', { class: 'row muted' }, sel.bus ? tr('Ningún mensaje del archivo pasa por el bus (o la app no está mirando).') : tr('Sin mensajes elegidos.')));
        return;
      }
      const value = g => {
        const k = showKey(g.msg.id, g.msg.ext);
        const cands = g.muxed ? [...(muxed.get(k) || []), frames.get(k)].filter(Boolean) : [frames.get(k)].filter(Boolean);
        const d = cands.find(d => sigPresent(g, d));
        if (!d) return '—';
        const raw = sigRaw(g, d);
        if (raw === null) return '—';
        if (g.vals?.has(raw)) return g.vals.get(raw);
        return fmtNum(raw * g.scale + g.offset) + (g.unit ? ' ' + g.unit : '');
      };
      let rows = 0;
      /* P.h takes one level of arrays: the rows go flat */
      put(sigVals, h('table', { class: 'cntbl' }, msgs.flatMap(m => rows > 600 ? [] : [
        h('tr', {}, h('th', { colspan: 2 }, h('span', { class: 'mono id' }, idText(m.id, m.ext)), ' · ', m.name)),
        ...m.sigs.map(g => { rows++; return h('tr', {}, h('td', {}, g.name), h('td', { class: 'mono cnval' }, value(g))); })])));
    }

    /* ---- OBD-II ---- */
    const obdInfo = h('div', {}), obdTable = h('div', { class: 'cnscroll' });
    const obdBox = h('div', { class: 'card' },
      h('div', { class: 'pad' }, obdInfo,
        h('div', { class: 'btns', style: 'margin-top:12px' },
          h('button', { class: 'btn pri', onclick: () => order('obd on') }, tr('Preguntar (11 bits)')),
          h('button', { class: 'btn', onclick: () => order('obd on29') }, tr('Preguntar (29 bits)')),
          h('button', { class: 'btn', onclick: () => order('obd off') }, tr('Parar'))),
        h('p', { class: 'note' }, tr('Pregunta los PIDs del modo 01 de SAE J1979 a 7DF (o 18DB33F1): primero el PID 00, el mapa de los que conoce el auto, y después esos, en vuelta. Necesita el modo Normal, que se elige en la placa: la app abre siempre en Solo escucha. En muchos autos modernos el conector OBD da sólo diagnóstico, porque un gateway separa los buses internos.'))),
      obdTable);
    const OBDST = { idle: N_('Detenido'), maps: N_('Preguntando qué PIDs conoce…'), live: N_('En vivo'), noanswer: N_('Nadie contestó'), notnormal: N_('Detenido: hace falta el modo Normal'), noack: N_('Detenido: nadie confirma lo que manda') };
    function drawObd() {
      const o = live && staleFor < 5 ? live.obd : null;
      if (!o) { put(obdInfo, h('div', { class: 'muted' }, tr('Sin datos de la app.'))); put(obdTable); return; }
      const mode = live.open ? live.mode : 'off';
      put(obdInfo,
        h('div', {}, h('b', { style: o.on ? 'color:var(--green)' : '' }, tr(OBDST[o.st] || o.st)),
          o.ecus.length ? ' · ' + tr('contestan %s · %d respuestas', o.ecus.join(', '), o.answers) : ''),
        mode !== 'normal' && h('p', { class: 'note cnwarn' }, mode === 'off' ? tr('El bus está desconectado en la app.') : tr('La app está en %s: para preguntar hace falta Normal, y se cambia en la placa.', tr(MODE[mode] || mode))));
      const rows = o.v.slice().sort((a, b) => a[0] - b[0]);
      put(obdTable, rows.length ? h('table', { class: 'cntbl' },
        h('tr', {}, h('th', {}, tr('PID')), h('th', {}, tr('qué')), h('th', { style: 'text-align:right' }, tr('valor')), h('th', {}, tr('de')), h('th', { style: 'text-align:right' }, tr('hace'))),
        rows.map(([pid, ecu, data, age]) => {
          const def = OBD_BY.get(pid);
          return h('tr', {},
            h('td', { class: 'mono id' }, pid.toString(16).toUpperCase().padStart(2, '0')),
            h('td', {}, def ? tr(def[3]) : tr('sin fórmula')),
            h('td', { class: 'mono cnval' }, obdValue(pid, hexBytes(data))),
            h('td', { class: 'mono muted' }, o.ecus[ecu] || ''),
            h('td', { class: 'num' }, age < 1000 ? '' : `${Math.round(age / 1000)} s`));
        })) : null);
    }

    /* ---- sending ---- */
    const input = h('input', { class: 'cnin mono', placeholder: '7DF#02010C', spellcheck: false });
    const hint = h('p', { class: 'note' });
    const savedList = h('div', {});
    function drawSendHint() {
      const m = live && staleFor < 5 ? (live.open ? live.mode : 'off') : 'none';
      hint.className = 'note' + (m === 'normal' ? '' : ' cnwarn');
      hint.textContent = {
        none: tr('La app no está contestando: lo que mandes se manda cuando vuelva.'),
        off: tr('El bus está desconectado en la app.'),
        listen: tr('La app está en Solo escucha: no sale nada. El modo se cambia en la placa.'),
        selftest: tr('Autoprueba: vuelve sólo a la placa, no sale al bus.'),
        normal: tr('Normal: sale al bus. Nunca al de un auto en marcha.'),
      }[m];
    }
    const sendOne = fr => {
      fr = fr.trim();
      if (!FRAME_RE.test(fr)) { P.toast(tr('Como en candump: 123#11223344, 18FEF100#FF00 o 7DF#R'), true); return; }
      order('send ' + fr);
      P.toast(tr('Mandada a la app: %s', fr));
    };
    function drawSaved(text) {
      const rows = (text || '').split('\n').map(l => l.trim()).filter(l => l && !l.startsWith('#'))
        .map(l => { const [fr, per, ...name] = l.split(/\s+/); return { fr, per: +per || 0, name: name.join(' ') }; })
        .filter(r => FRAME_RE.test(r.fr));
      put(savedList, rows.length ? rows.map(r => h('div', { class: 'row' },
        h('div', { class: 'grow' }, h('span', { class: 'mono' }, r.fr), ' ', h('span', { class: 'muted small' }, [r.name, r.per ? tr('cada %d ms (las periódicas se arrancan en la placa)', r.per) : ''].filter(Boolean).join(' · '))),
        h('button', { class: 'btn', onclick: () => sendOne(r.fr) }, tr('Mandar')))) : h('div', { class: 'row muted' }, tr('Sin tramas guardadas.')));
    }
    put(send,
      h('div', { class: 'row', style: 'gap:10px;flex-wrap:wrap' }, input,
        h('button', { class: 'btn pri', onclick: () => sendOne(input.value) }, tr('Mandar'))),
      hint,
      h('h3', {}, tr('Guardadas en la placa')),
      savedList);
    input.addEventListener('keydown', e => { if (e.key === 'Enter') sendOne(input.value); });

    /* ---- the recordings ---- */
    async function loadRecs() {
      let fl;
      try { fl = await P.fsList(DIR); } catch (e) { put(recs, h('div', { class: 'row bad' }, e.message)); return; }
      if (gone) return;
      const csv = fl.filter(f => !f.dir && !f.name.startsWith('.') && /\.csv$/i.test(f.name))
        .sort((a, b) => b.name.localeCompare(a.name));
      put(recs, csv.length ? csv.map(f => h('div', { class: 'row' },
        h('div', { class: 'grow' }, h('span', { class: 'mono' }, f.name), ' ', h('span', { class: 'muted small' }, `${when(f.mtime)} · ${P.fmtBytes(f.size)}`)),
        h('div', { class: 'btns' },
          h('a', { class: 'btn', href: P.fsUrl(`${DIR}/${f.name}`, true) }, tr('Bajar')),
          h('button', { class: 'btn red', onclick: async () => {
            if (live?.rec === f.name) { P.toast(tr('Se está grabando: pararla primero'), true); return; }
            if (!confirm(tr('¿Borrar %s? No se puede deshacer.', f.name))) return;
            try { await P.fsDelete(`${DIR}/${f.name}`); P.toast(tr('Borrada')); loadRecs(); } catch (e) { P.toast(e.message, true); }
          } }, tr('Borrar')))))
        : h('div', { class: 'row muted' }, tr('Todavía no hay grabaciones: se graba desde la placa o con el botón de arriba.')));
    }

    /* ---- the files the app reads ---- */
    function editor(path, title, about, after) {
      const ta = h('textarea', { class: 'cnedit mono', spellcheck: false });
      const box = h('div', {},
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('b', {}, title), h('div', { class: 'muted small' }, about)),
          h('button', { class: 'btn', onclick: async () => {
            try {
              await P.fsMkdir(DIR).catch(() => {});
              await P.fsPut(path, ta.value.endsWith('\n') ? ta.value : ta.value + '\n');
              order('reload');
              P.toast(tr('Guardado: la app lo vuelve a leer'));
              after?.(ta.value);
            } catch (e) { P.toast(e.message, true); }
          } }, tr('Guardar'))),
        ta);
      P.fsText(path).then(t => { ta.value = t ?? ''; after?.(t); }).catch(e => P.toast(e.message, true));
      return box;
    }
    put(files,
      editor(SAVED, tr('Tramas guardadas'), tr('can/enviar.txt: una por línea, la trama, el período en ms (0 a mano) y un nombre.'), drawSaved),
      editor(DBC, tr('Señales propias'), tr('can/senales.dbc: un .dbc propio (BO_, SG_, VAL_). Los bajados van en can/dbc y se eligen arriba.'), () => { if (sel.file === 'senales.dbc' && dbcFile !== null) loadDbc(true); }));

    put(main,
      h('h1', {}, tr('CAN')), sum,
      h('h2', {}, tr('Por id')), ids,
      h('h2', {}, tr('Señales')), sigBox,
      h('div', { class: 'card' }, sigVals),
      h('h2', {}, tr('OBD-II')), obdBox,
      h('h2', {}, tr('Últimas tramas')), last,
      h('h2', {}, tr('Mandar')), send,
      h('h2', {}, tr('Grabaciones')), recs,
      h('h2', {}, tr('Archivos de la app')), files);

    drawSummary();
    drawSendHint();
    order('watch');
    poll();
    loadRecs();
    loadFiles();
    loadSel().then(() => loadDbc(true));
    const tPoll = setInterval(poll, 1000);
    const tWatch = setInterval(() => order('watch'), 5000);
    const tRecs = setInterval(loadRecs, 10000);
    return () => { gone = true; clearInterval(tPoll); clearInterval(tWatch); clearInterval(tRecs); };
  },
});
