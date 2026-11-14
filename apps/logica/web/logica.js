/* The Logic app's page in the board's portal (docs/PORTAL-PAGES.md): the
 * logic analyser on the computer's screen.
 *
 * It talks to the app through the live channel (apps/logica/main/la_live.c):
 * "state" (JSON) twice a second; the capture asked for in pieces ("data
 * <seq> <from>": runs of samples), and the decoders' annotations and
 * packets the same way ("ann", "pkt"). It sends the settings as the lines
 * of the app's own file (rate=, trig=, dec2=...) and commands (capture,
 * stop, probe, load, save). The decoders run on the board; the page draws,
 * measures and exports.
 *
 * Files: the captures are sigrok sessions in the card's /logica, which the
 * page can also open by itself (a zip, deflated or not) and build (.sr for
 * PulseView, VCD, CSV). fx2lafw, the open firmware the cheap FX2 analysers
 * need, goes to /logica/fx2lafw: as .fw files or as sigrok's .tar.gz, which
 * the page unpacks in the browser. Texts in Spanish through tr()
 * (web.logica.lang). */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const APP = 'aos.logica';
const LIVE = 'live?app=' + APP;
const DIR = '/logica';
const FWDIR = '/logica/fx2lafw';
const NCH = 8;
const NDEC = 6;

const CH_COL = ['#30D158', '#E0B800', '#FF9F0A', '#FF6961', '#BF5AF2', '#40C8E0', '#5E9EFF', '#FF7AA2'];
const K_COL = ['#2F5FB3', '#9C7A00', '#22803C', '#1D6B3A', '#B3261E', '#7A3E9D', '#4A4F57'];
const DEC_NAMES = ['UART', 'I2C', 'SPI', '1-Wire', 'CAN', 'WS2812', 'NEC IR', 'DMX512', 'Modbus RTU', N_('Paralelo')];
const DEC_PINS = [['RX/TX'], ['SCL', 'SDA'], ['CLK', 'MOSI', 'MISO', 'CS'], ['DQ'], ['RX'], ['DIN'], ['IR'], ['DMX'], ['A/B (RO)'],
  [N_('Reloj'), 'D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7']];
const DEC_NEED = (t, p) => t === 1 ? true : t === 2 ? p === 0 : t === 9 ? p === 1 : p === 0;
const TRIG = [['x', 'X'], ['0', '0'], ['1', '1'], ['r', N_('↑ sube')], ['f', N_('↓ baja')], ['e', N_('↕ cambia')]];
const STATES = [N_('Lista'), N_('Conectando con el analizador...'), N_('Esperando el disparo...'), N_('Capturando...'), N_('Decodificando...'), N_('Archivo...')];
const UART_B = [0, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 250000, 460800, 921600, 1000000];
const CAN_B = [50000, 100000, 125000, 250000, 500000, 800000, 1000000];
const SAMPLES = [10000, 50000, 100000, 250000, 500000, 1000000, 2000000, 4000000, 8000000, 16000000];

const css = `
.lg-bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.lg-bar select,.lg-bar input{max-width:100%}
.lg-cv{width:100%;display:block;border-radius:10px;background:#0b0e12;touch-action:none;cursor:crosshair;user-select:none}
.lg-hint{font-size:13px;color:var(--dim);min-height:1.3em}
.lg-trig{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:6px}
.lg-trig label{display:flex;gap:6px;align-items:center;font-size:14px}
.lg-tab{width:100%;border-collapse:collapse;font-size:14px}
.lg-tab td{padding:5px 6px;border-top:1px solid var(--line);vertical-align:top}
.lg-tab tr{cursor:pointer}
.lg-tab tr:hover td{background:var(--card2)}
.lg-tab .err{color:var(--red)}
.lg-mono{font-family:ui-monospace,Menlo,monospace}
.lg-dec{display:grid;grid-template-columns:repeat(auto-fill,minmax(170px,1fr));gap:8px;margin-top:8px}
.lg-dec label{display:flex;flex-direction:column;gap:3px;font-size:13px;color:var(--dim)}
.lg-meas{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;font-size:14px}
.lg-prog{height:6px;border-radius:3px;background:var(--card2);overflow:hidden}
.lg-prog>div{height:100%;background:var(--accent);width:0}
.lg-row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.lg-chn{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:6px}
.lg-chn input[type=text]{width:100%}
@media (max-width:600px){.lg-tab td:nth-child(2){display:none}}
`;

/* ---- numbers ---- */

function num3(v) {
  const s = v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2);
  return s.includes('.') ? s.replace(/\.?0+$/, '') : s;
}
function fmtTime(s) {
  const a = Math.abs(s), g = s < 0 ? '-' : '';
  if (a === 0) return '0 s';
  if (a >= 0.9995) return g + num3(a) + ' s';
  if (a >= 0.9995e-3) return g + num3(a * 1e3) + ' ms';
  if (a >= 0.9995e-6) return g + num3(a * 1e6) + ' µs';
  return g + num3(a * 1e9) + ' ns';
}
function fmtFreq(hz) {
  if (hz >= 0.9995e6) return num3(hz / 1e6) + ' MHz';
  if (hz >= 0.9995e3) return num3(hz / 1e3) + ' kHz';
  return num3(hz) + ' Hz';
}
function fmtCount(n) {
  if (n >= 1e6) return (n % 1e6 ? (n / 1e6).toFixed(1) : n / 1e6) + 'M';
  if (n >= 1e3) return (n % 1e3 ? (n / 1e3).toFixed(1) : n / 1e3) + 'k';
  return String(n);
}
const fmtRate = hz => fmtFreq(hz);

/* ---- a capture in the browser ---- */

function summarise(cap) {
  const b = cap.buf, n = cap.n;
  const n1 = Math.ceil(n / 64), s1 = new Uint8Array(n1 * 2);
  for (let k = 0; k < n1; k++) {
    let o = 0, a = 255;
    const e = Math.min(n, k * 64 + 64);
    for (let i = k * 64; i < e; i++) { o |= b[i]; a &= b[i]; }
    s1[2 * k] = o; s1[2 * k + 1] = a;
  }
  const n2 = Math.ceil(n / 4096), s2 = new Uint8Array(n2 * 2);
  for (let k = 0; k < n2; k++) {
    let o = 0, a = 255;
    const e = Math.min(n1, k * 64 + 64);
    for (let j = k * 64; j < e; j++) { o |= s1[2 * j]; a &= s1[2 * j + 1]; }
    s2[2 * k] = o; s2[2 * k + 1] = a;
  }
  cap.s1 = s1; cap.s2 = s2;
}

function range(cap, a, b) {
  const buf = cap.buf, s1 = cap.s1, s2 = cap.s2;
  let o = 0, an = 255, i = a;
  b = Math.min(b, cap.n);
  while (i < b && i % 64) { o |= buf[i]; an &= buf[i]; i++; }
  while (i + 64 <= b && i % 4096) { const k = i >> 6; o |= s1[2 * k]; an &= s1[2 * k + 1]; i += 64; }
  while (i + 4096 <= b) { const k = Math.floor(i / 4096); o |= s2[2 * k]; an &= s2[2 * k + 1]; i += 4096; }
  while (i + 64 <= b) { const k = i >> 6; o |= s1[2 * k]; an &= s1[2 * k + 1]; i += 64; }
  while (i < b) { o |= buf[i]; an &= buf[i]; i++; }
  return [o, an];
}

function measure(cap, ch, a, b) {
  const m = 1 << ch, buf = cap.buf, ts = 1 / cap.rate;
  const r = { rises: 0, falls: 0, hi: [], lo: [], high: 0, freq: 0, duty: 0 };
  if (b - a < 2) return r;
  let v = buf[a] & m, pos = a, lastR = -1, lastF = -1, firstR = -1;
  for (let i = a + 1; i < b; i++) {
    const nv = buf[i] & m;
    if (nv === v) continue;
    if (v) r.high += i - pos;
    if (nv) { r.rises++; if (firstR < 0) firstR = i; if (lastF >= 0) r.lo.push((i - lastF) * ts); lastR = i; }
    else { r.falls++; if (lastR >= 0) r.hi.push((i - lastR) * ts); lastF = i; }
    v = nv; pos = i;
  }
  if (v) r.high += b - pos;
  r.high /= (b - a);
  if (r.rises >= 2) {
    r.freq = (r.rises - 1) / ((lastR - firstR) * ts);
    if (r.hi.length) r.duty = Math.min(1, r.hi.reduce((x, y) => x + y, 0) / r.hi.length * r.freq);
  }
  return r;
}

/* ---- sigrok sessions: zip, CRC-32, reading and writing ---- */

const CRC = (() => { const t = new Uint32Array(256); for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[i] = c >>> 0; } return t; })();
function crc32(u8) { let c = 0xFFFFFFFF; for (let i = 0; i < u8.length; i++) c = CRC[(c ^ u8[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

function zipStore(files) {
  const parts = [], cd = [];
  let off = 0;
  const enc = new TextEncoder();
  for (const [name, data] of files) {
    const nb = enc.encode(name), d = typeof data === 'string' ? enc.encode(data) : data, crc = crc32(d);
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034B50, true); h.setUint16(4, 20, true); h.setUint16(12, 0x21, true);
    h.setUint32(14, crc, true); h.setUint32(18, d.length, true); h.setUint32(22, d.length, true); h.setUint16(26, nb.length, true);
    parts.push(h.buffer, nb, d);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014B50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(14, 0x21, true);
    c.setUint32(16, crc, true); c.setUint32(20, d.length, true); c.setUint32(24, d.length, true); c.setUint16(28, nb.length, true);
    c.setUint32(42, off, true);
    cd.push(c.buffer, nb);
    off += 30 + nb.length + d.length;
  }
  const cdlen = cd.reduce((s, x) => s + x.byteLength, 0);
  const e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054B50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
  e.setUint32(12, cdlen, true); e.setUint32(16, off, true);
  return new Blob([...parts, ...cd, e.buffer], { type: 'application/zip' });
}

async function inflateRaw(u8) {
  const s = new Blob([u8]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(s).arrayBuffer());
}

async function unzip(buf) {
  const dv = new DataView(buf), u8 = new Uint8Array(buf), out = {};
  let eo = -1;
  for (let i = buf.byteLength - 22; i >= Math.max(0, buf.byteLength - 65557); i--) if (dv.getUint32(i, true) === 0x06054B50) { eo = i; break; }
  if (eo < 0) throw new Error(tr('No es un zip'));
  const n = dv.getUint16(eo + 10, true);
  let p = dv.getUint32(eo + 16, true);
  const dec = new TextDecoder();
  for (let k = 0; k < n; k++) {
    if (dv.getUint32(p, true) !== 0x02014B50) break;
    const method = dv.getUint16(p + 10, true), csize = dv.getUint32(p + 20, true);
    const nl = dv.getUint16(p + 28, true), xl = dv.getUint16(p + 30, true), cl = dv.getUint16(p + 32, true);
    const lo = dv.getUint32(p + 42, true), name = dec.decode(u8.subarray(p + 46, p + 46 + nl));
    const d0 = lo + 30 + dv.getUint16(lo + 26, true) + dv.getUint16(lo + 28, true);
    const raw = u8.subarray(d0, d0 + csize);
    out[name] = method === 8 ? await inflateRaw(raw) : raw;
    p += 46 + nl + xl + cl;
  }
  return out;
}

function iniGet(text, sect, key) {
  const lines = text.split(/\r?\n/);
  let inS = false;
  for (const l of lines) {
    if (l.startsWith('[')) { inS = l === `[${sect}]`; continue; }
    if (inS && l.startsWith(key + '=')) return l.slice(key.length + 1);
  }
  return '';
}

function parseRate(s) {
  const m = /([\d.]+)\s*([kKMG]?)/.exec(s || '');
  if (!m) return 0;
  return Math.round(parseFloat(m[1]) * ({ k: 1e3, K: 1e3, M: 1e6, G: 1e9 }[m[2]] || 1));
}

async function srRead(buf, name) {
  const z = await unzip(buf), dec = new TextDecoder();
  if (!z.metadata) throw new Error(tr('No es una sesión de sigrok (.sr)'));
  const meta = dec.decode(z.metadata);
  const rate = parseRate(iniGet(meta, 'device 1', 'samplerate'));
  const unit = parseInt(iniGet(meta, 'device 1', 'unitsize') || '1', 10);
  const cf = iniGet(meta, 'device 1', 'capturefile') || 'logic-1';
  const chunks = [];
  for (let i = 1; z[`${cf}-${i}`]; i++) chunks.push(z[`${cf}-${i}`]);
  if (!chunks.length && z[cf]) chunks.push(z[cf]);
  const total = chunks.reduce((s, c) => s + c.length, 0) / unit;
  const out = new Uint8Array(total);
  let at = 0;
  for (const c of chunks) for (let i = 0; i < c.length; i += unit) out[at++] = c[i];
  const names = [];
  for (let i = 0; i < NCH; i++) names.push(iniGet(meta, 'device 1', 'probe' + (i + 1)) || 'D' + i);
  let trig = -1;
  if (z.p4os) { const m = /trigger=(-?\d+)/.exec(dec.decode(z.p4os)); if (m) trig = parseInt(m[1], 10); }
  return { buf: out, n: total, rate, trig, names, src: name, local: true };
}

function srBuild(cap, names) {
  const files = [['version', '2']];
  let meta = `[global]\nsigrok version=0.5.2\n\n[device 1]\ncapturefile=logic-1\ntotal probes=${NCH}\n` +
    `samplerate=${cap.rate % 1e6 === 0 ? cap.rate / 1e6 + ' MHz' : cap.rate % 1e3 === 0 ? cap.rate / 1e3 + ' kHz' : cap.rate + ' Hz'}\ntotal analog=0\n`;
  for (let i = 0; i < NCH; i++) meta += `probe${i + 1}=${names[i] || 'D' + i}\n`;
  files.push(['metadata', meta + 'unitsize=1\n']);
  const CH = 4 << 20;
  for (let i = 0, k = 1; i < cap.n; i += CH, k++) files.push([`logic-1-${k}`, cap.buf.subarray(i, Math.min(cap.n, i + CH))]);
  files.push(['p4os', `trigger=${cap.trig}\nsource=${cap.src}\n`]);
  return zipStore(files);
}

function vcdBuild(cap, names, a, b) {
  const ids = '!"#$%&\'(';
  const ts = cap.rate >= 1e9 ? 1e-12 : 1e-9;
  let s = `$date ${new Date().toISOString()} $end\n$version P4OS Lógica $end\n$timescale ${ts === 1e-9 ? '1 ns' : '1 ps'} $end\n$scope module p4os $end\n`;
  for (let i = 0; i < NCH; i++) s += `$var wire 1 ${ids[i]} ${(names[i] || 'D' + i).replace(/\s+/g, '_')} $end\n`;
  s += '$upscope $end\n$enddefinitions $end\n';
  const parts = [s];
  const t = i => Math.round((i - a) / cap.rate / ts);
  let prev = -1, chunk = '';
  for (let i = a; i < b; i++) {
    const v = cap.buf[i];
    if (v === prev) continue;
    let line = `#${t(i)}\n`;
    for (let c = 0; c < NCH; c++) if (prev < 0 || ((v ^ prev) >> c & 1)) line += `${v >> c & 1}${ids[c]}\n`;
    chunk += line;
    if (chunk.length > 65536) { parts.push(chunk); chunk = ''; }
    prev = v;
  }
  parts.push(chunk, `#${t(b)}\n`);
  return new Blob(parts, { type: 'text/plain' });
}

/* ---- sigrok's firmware tarball, unpacked in the browser ---- */

async function untarGz(file) {
  const s = file.stream().pipeThrough(new DecompressionStream('gzip'));
  const u8 = new Uint8Array(await new Response(s).arrayBuffer());
  const out = [], dec = new TextDecoder();
  for (let p = 0; p + 512 <= u8.length;) {
    const name = dec.decode(u8.subarray(p, p + 100)).replace(/\0.*$/s, '');
    if (!name) break;
    const size = parseInt(dec.decode(u8.subarray(p + 124, p + 136)).replace(/\0.*$/s, '').trim() || '0', 8);
    const type = u8[p + 156];
    if ((type === 0 || type === 48) && /fx2lafw-[\w-]+\.fw$/.test(name)) out.push([name.split('/').pop(), u8.slice(p + 512, p + 512 + size)]);
    p += 512 + Math.ceil(size / 512) * 512;
  }
  return out;
}

/* ---- the decoders' settings, as the app's file has them ---- */

function parseCfg(text) {
  const cfg = { names: [], show: '11111111', trig: 'xxxxxxxx', dec: [] };
  for (const l of (text || '').split('\n')) {
    const i = l.indexOf('=');
    if (i < 0) continue;
    const k = l.slice(0, i), v = l.slice(i + 1);
    if (/^name\d$/.test(k)) cfg.names[+k[4]] = v;
    else if (/^dec\d$/.test(k)) {
      const f = v.split(',').map(x => parseInt(x, 10));
      cfg.dec[+k[3]] = { type: f[0], on: !!f[1], ch: f.slice(2, 11), baud: f[11], bits: f[12], parity: String.fromCharCode(f[13] || 78),
        stop: f[14], mode: f[15], lsb: !!f[16], inv: !!f[17], fmt: f[18] };
    } else cfg[k] = v;
  }
  return cfg;
}
const decLine = (i, d) => `dec${i}=${d.type},${d.on ? 1 : 0},${d.ch.join(',')},${d.baud | 0},${d.bits | 0},${(d.parity || 'N').charCodeAt(0)},${d.stop | 0},${d.mode | 0},${d.lsb ? 1 : 0},${d.inv ? 1 : 0},${d.fmt | 0}`;
function decDefaults(t) {
  const d = { type: t, on: true, ch: Array(9).fill(-1), baud: 0, bits: 8, parity: 'N', stop: 1, mode: 0, lsb: false, inv: false, fmt: 0 };
  if (t === 0) { d.fmt = 2; d.ch[0] = 0; }
  else if (t === 1) { d.ch[0] = 0; d.ch[1] = 1; }
  else if (t === 2) { d.ch = [0, 1, 2, 3, -1, -1, -1, -1, -1]; }
  else if (t === 4) { d.baud = 500000; d.ch[0] = 0; }
  else if (t === 7) { d.baud = 250000; d.stop = 2; d.ch[0] = 0; d.fmt = 1; }
  else if (t === 8) { d.baud = 9600; d.ch[0] = 0; }
  else if (t === 9) { d.ch = [-1, 0, 1, 2, 3, 4, 5, 6, 7]; }
  else d.ch[0] = 0;
  return d;
}

/* ===================================================================== */

P.registerPage({
  id: 'logica',
  name: tr('Lógica'),
  icon: '⎍',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-logica')) document.head.append(h('style', { id: 'css-logica' }, css));

    let st = null, cfg = parseCfg(''), cap = null, anns = [], pkts = [], annSeq = -1, capSeq = -1, gone = false;
    let local = false, busyFetch = false, decEdit = -1, decDraft = null, tabFilter = -1, tabQuery = '', tabPage = 0;
    const view = { off: 0, spp: 1, ca: -1, cb: -1 };
    const send = text => P.api(LIVE, { method: 'POST', body: text }).catch(e => P.toast(e.message, true));

    /* -- the controls -- */
    const devSel = h('select', { onchange: () => send('dev ' + devSel.value) });
    const rateSel = h('select', { onchange: () => send('rate=' + rateSel.value) });
    const sampSel = h('select', { onchange: () => send('samples=' + sampSel.value) });
    const preSel = h('select', { onchange: () => send('pre=' + preSel.value) }, ...[0, 10, 25, 50, 75, 90].map(v => h('option', { value: v }, tr('%s antes', v + ' %'))));
    const repChk = h('input', { type: 'checkbox', onchange: () => send('repeat=' + (repChk.checked ? 1 : 0)) });
    const capBtn = h('button', { class: 'btn pri', onclick: () => send(st && st.busy ? 'stop' : 'capture') }, tr('Capturar'));
    const msg = h('div', { class: 'lg-hint' });
    const prog = h('div', { class: 'lg-prog' }, h('div'));
    const trigSels = [];
    const trigBox = h('div', { class: 'lg-trig' });
    for (let c = 0; c < NCH; c++) {
      const s = h('select', { onchange: () => send('trig=' + trigSels.map(x => x.value).join('')) }, ...TRIG.map(([v, t]) => h('option', { value: v }, tr(t))));
      trigSels.push(s);
      trigBox.append(h('label', {}, h('span', { class: 'lg-mono', style: `color:${CH_COL[c]}` }, 'D' + c), s));
    }
    const devInfo = h('div', { class: 'note' });

    /* -- the view -- */
    const cv = h('canvas', { class: 'lg-cv', height: 420 });
    const hint = h('div', { class: 'lg-hint' }, tr('Rueda o pellizco: zoom · arrastrar: moverse · clic: cursores A y B · doble clic: acercar · Esc: sacar los cursores'));
    const readout = h('div', { class: 'lg-hint lg-mono' });
    const srcInfo = h('div', { class: 'lg-row' });

    /* -- panels -- */
    const decBox = h('div');
    const chBox = h('div', { class: 'lg-chn' });
    const tabBox = h('div');
    const measBox = h('div');
    const filesBox = h('div');
    const fwBox = h('div');

    put(main,
      h('h1', {}, tr('Lógica')),
      h('div', { class: 'card pad' },
        h('div', { class: 'lg-bar' },
          h('b', {}, tr('Analizador')), devSel,
          h('button', { class: 'btn', onclick: () => { send('probe'); P.toast(tr('Preguntando a los puertos serie USB...')); } }, tr('Buscar SUMP')),
          h('button', { class: 'btn', onclick: () => P.openApp(APP).then(() => P.toast(tr('Abierta en la placa'))) }, tr('Abrir en la placa'))),
        devInfo,
        h('div', { class: 'lg-bar', style: 'margin-top:8px' },
          rateSel, sampSel, preSel, h('label', {}, repChk, ' ', tr('repetir')), capBtn),
        h('details', { style: 'margin-top:8px' }, h('summary', {}, tr('Disparo (todas las condiciones juntas)')), trigBox,
          h('button', { class: 'btn', style: 'margin-top:6px', onclick: () => send('trig=xxxxxxxx') }, tr('Sin disparo'))),
        prog, msg),
      h('div', { class: 'card pad' }, srcInfo, cv, readout, hint,
        h('div', { class: 'lg-row', style: 'margin-top:6px' },
          h('button', { class: 'btn', onclick: () => zoomBy(0.5) }, '−'),
          h('button', { class: 'btn', onclick: () => zoomBy(2) }, '+'),
          h('button', { class: 'btn', onclick: () => fit() }, tr('Todo')),
          h('button', { class: 'btn', onclick: () => toTrig() }, tr('Al disparo')),
          h('button', { class: 'btn', onclick: () => { view.ca = view.cb = -1; draw(); showMeas(); } }, tr('Sacar cursores')))),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Decodificadores')), decBox),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Paquetes')), tabBox),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Medir')), measBox),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Canales')), chBox),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Capturas')), filesBox),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Firmware fx2lafw (analizadores FX2)')), fwBox));

    /* ================= the view ================= */

    const LBL = 110, RULER = 24, CHH = 30, DECH = 26;
    let rows = [];
    const plotW = () => cv.clientWidth - LBL;
    const sAt = x => view.off + (x - LBL) * view.spp;
    const xOf = s => LBL + (s - view.off) / view.spp;
    const names = () => (cap && cap.local && cap.names) ? cap.names : cfg.names;

    function layout() {
      rows = [];
      const show = cfg.show || '11111111';
      let y = RULER + 2;
      for (let c = 0; c < NCH; c++) if (cap && cap.local || show[c] !== '0') { rows.push({ y, h: CHH, ch: c }); y += CHH; }
      if (!(cap && cap.local)) for (let r = 0; r < NDEC; r++) if (cfg.dec[r] && cfg.dec[r].on) { rows.push({ y, h: DECH, dec: r }); y += DECH; }
      return y + 4;
    }

    function clamp() {
      if (!cap || !cap.n) return;
      const pw = plotW(), maxspp = Math.max(cap.n / pw, 1 / 32);
      view.spp = Math.min(Math.max(view.spp, 1 / 32), maxspp);
      const vis = view.spp * pw;
      view.off = Math.min(Math.max(view.off, -vis * 0.05), Math.max(-vis * 0.05, cap.n - vis * 0.95));
    }
    function fit() { if (!cap) return; view.spp = cap.n / plotW(); view.off = 0; clamp(); draw(); showMeas(); }
    function zoomBy(f, x) {
      if (!cap) return;
      x = x ?? LBL + plotW() / 2;
      const at = sAt(x);
      view.spp /= f; clamp(); view.off = at - (x - LBL) * view.spp; clamp(); draw(); showMeas();
    }
    function toTrig() { if (cap && cap.trig >= 0) { view.off = cap.trig - plotW() / 2 * view.spp; clamp(); draw(); } }
    function goTo(s0, s1) {
      const span = Math.max(1, s1 - s0), want = span * 1.6 / plotW();
      if (want > view.spp || span / view.spp < 20) view.spp = want;
      view.off = (s0 + s1) / 2 - plotW() / 2 * view.spp; clamp(); draw(); showMeas();
      cv.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    }

    function draw() {
      const dpr = window.devicePixelRatio || 1, W = cv.clientWidth;
      const H = layout();
      cv.style.height = H + 'px';
      if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
      const g = cv.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.fillStyle = '#0b0e12'; g.fillRect(0, 0, W, H);
      g.fillStyle = '#15191f'; g.fillRect(0, 0, W, RULER);
      g.fillStyle = '#14181e'; g.fillRect(0, RULER, LBL, H - RULER);
      g.font = '12px system-ui,sans-serif';
      g.textBaseline = 'middle';
      const nm = names();
      for (const r of rows) {
        g.fillStyle = '#1a1f26'; g.fillRect(LBL, r.y + r.h - 1, W - LBL, 1);
        if (r.ch !== undefined) {
          g.fillStyle = CH_COL[r.ch]; g.fillRect(0, r.y + 3, 4, r.h - 6);
          g.fillStyle = '#fff'; g.fillText(`D${r.ch} ${nm[r.ch] || ''}`.slice(0, 16), 10, r.y + r.h / 2);
        } else {
          g.fillStyle = '#8e8e93'; g.fillRect(0, r.y + 3, 4, r.h - 6);
          g.fillStyle = '#c7c7cc'; g.fillText(tr(DEC_NAMES[cfg.dec[r.dec].type] || '?'), 10, r.y + r.h / 2);
        }
      }
      if (!cap || !cap.n) {
        g.fillStyle = '#8e8e93'; g.font = '16px system-ui,sans-serif';
        g.fillText(gone ? tr('La app no está abierta en la placa: tocá "Abrir en la placa", o abrí un archivo.') : tr('Sin captura todavía.'), LBL + 16, (RULER + H) / 2);
        return;
      }
      /* ruler */
      const t0 = cap.trig >= 0 ? cap.trig : 0;
      let step = 110 * view.spp / cap.rate;
      const p10 = Math.pow(10, Math.floor(Math.log10(step))), m = step / p10;
      step = (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p10;
      const ta = (sAt(LBL) - t0) / cap.rate, tb = (sAt(W) - t0) / cap.rate;
      g.font = '11px system-ui,sans-serif';
      for (let t = Math.ceil(ta / step) * step, k = 0; t <= tb && k < 60; t += step, k++) {
        const x = Math.round(xOf(t * cap.rate + t0)) + 0.5;
        if (x < LBL) continue;
        g.fillStyle = '#1e242c'; g.fillRect(x, RULER, 1, H - RULER);
        g.fillStyle = '#6e7781'; g.fillRect(x, RULER - 6, 1, 6);
        g.fillStyle = '#8e8e93'; g.fillText(fmtTime(Math.abs(t) < step * 1e-3 ? 0 : t), x + 3, 10);
      }
      /* the channels, column by column */
      const last = new Array(NCH).fill(-1);
      let prevIa = -1, o = 0, a = 0, lastv = 0;
      for (let x = LBL; x < W; x++) {
        const sa = sAt(x), sb = sAt(x + 1);
        if (sb <= 0) continue;
        if (sa >= cap.n) break;
        const ia = Math.max(0, Math.floor(sa)), ib = Math.min(cap.n, Math.max(ia + 1, Math.floor(sb)));
        if (ia !== prevIa) {
          if (ib - ia === 1) o = a = cap.buf[ia]; else [o, a] = range(cap, ia, ib);
          lastv = cap.buf[ib - 1]; prevIa = ia;
        }
        for (const r of rows) {
          if (r.ch === undefined) continue;
          const c = r.ch, yh = r.y + 5, yl = r.y + r.h - 5;
          g.fillStyle = CH_COL[c];
          const bo = o >> c & 1, ba = a >> c & 1;
          if (bo === ba) {
            if (last[c] >= 0 && last[c] !== bo) g.fillRect(x, yh, 1, yl - yh + 1);
            g.fillRect(x, bo ? yh : yl - 1, 1, 2);
            last[c] = bo;
          } else { g.fillRect(x, yh, 1, yl - yh + 1); last[c] = lastv >> c & 1; }
        }
      }
      /* annotations */
      g.font = '11px ui-monospace,Menlo,monospace';
      const s0 = sAt(LBL), s1 = sAt(W);
      for (const r of rows) {
        if (r.dec === undefined) continue;
        const list = anns[r.dec] || [];
        let lo = 0, hi = list.length;
        while (lo < hi) { const mid = (lo + hi) >> 1; if (list[mid].s1 <= s0) lo = mid + 1; else hi = mid; }
        let lastX = -10;
        for (let i = lo; i < list.length; i++) {
          const an = list[i];
          if (an.s0 > s1) break;
          let x0 = Math.max(LBL, Math.floor(xOf(an.s0))), x1 = Math.min(W, Math.floor(xOf(an.s1)));
          if (x1 <= x0) x1 = x0 + 1;
          if (x1 <= lastX) continue;
          if (x0 <= lastX) x0 = lastX + 1;
          if (x1 <= x0) x1 = x0 + 1;
          g.fillStyle = K_COL[an.kind] || K_COL[0];
          g.fillRect(x0, r.y + 3, Math.max(1, x1 - x0 - (x1 - x0 > 3 ? 1 : 0)), r.h - 6);
          lastX = x1;
          const w = x1 - x0 - 4;
          if (w > 12) {
            let t = an.t, tw = g.measureText(t).width;
            while (t.length > 1 && tw > w) { t = t.slice(0, -1); tw = g.measureText(t).width; }
            g.fillStyle = '#fff';
            g.fillText(t, x0 + Math.max(2, (x1 - x0 - tw) / 2), r.y + r.h / 2);
          }
        }
      }
      /* trigger and cursors */
      if (cap.trig >= 0) {
        const x = Math.round(xOf(cap.trig));
        if (x >= LBL && x < W) { g.fillStyle = '#FF9F0A'; for (let y = RULER; y < H; y += 8) g.fillRect(x, y, 1, 4); g.fillRect(x - 4, RULER - 6, 9, 6); }
      }
      [[view.ca, '#40C8E0', 'A'], [view.cb, '#FF375F', 'B']].forEach(([c, col, n]) => {
        if (c < 0) return;
        const x = Math.round(xOf(c));
        if (x < LBL || x >= W) return;
        g.fillStyle = col; g.fillRect(x, 0, 1, H); g.fillRect(x - 8, 0, 17, 16);
        g.fillStyle = '#000'; g.font = 'bold 11px system-ui'; g.fillText(n, x - 3, 8);
      });
    }

    /* -- pointer -- */
    const ptrs = new Map();
    let dragFrom = null, pinch0 = null, moved = false, lastClick = 0;
    const px = e => { const r = cv.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
    cv.addEventListener('pointerdown', e => {
      cv.setPointerCapture(e.pointerId);
      ptrs.set(e.pointerId, px(e));
      moved = false;
      if (ptrs.size === 1) dragFrom = { x: px(e)[0], off: view.off };
      if (ptrs.size === 2) {
        const [p1, p2] = [...ptrs.values()];
        pinch0 = { d: Math.abs(p1[0] - p2[0]) || 1, spp: view.spp, at: sAt((p1[0] + p2[0]) / 2), mx: (p1[0] + p2[0]) / 2 };
      }
    });
    cv.addEventListener('pointermove', e => {
      const [x, y] = px(e);
      if (!ptrs.has(e.pointerId)) { hover(x, y); return; }
      ptrs.set(e.pointerId, [x, y]);
      if (!cap) return;
      if (ptrs.size >= 2 && pinch0) {
        const [p1, p2] = [...ptrs.values()];
        const d = Math.abs(p1[0] - p2[0]) || 1, mx = (p1[0] + p2[0]) / 2;
        view.spp = pinch0.spp * pinch0.d / d; clamp();
        view.off = pinch0.at - (mx - LBL) * view.spp; clamp();
        moved = true; draw();
      } else if (dragFrom && Math.abs(x - dragFrom.x) > 3) {
        view.off = dragFrom.off - (x - dragFrom.x) * view.spp; clamp();
        moved = true; draw();
      }
    });
    const up = e => {
      ptrs.delete(e.pointerId);
      if (ptrs.size < 2) pinch0 = null;
      if (ptrs.size) return;
      const [x, y] = px(e);
      if (!moved && cap && x > LBL) {
        const now = Date.now();
        if (now - lastClick < 300) { zoomBy(2.5, x); lastClick = 0; return; }
        lastClick = now;
        const r = rows.find(r => y >= r.y && y < r.y + r.h);
        if (r && r.dec !== undefined) {
          const s = sAt(x), p = pkts.find(p => p.row === r.dec && p.s0 <= s && s <= p.s1);
          if (p) { readout.textContent = p.t; return; }
        }
        const s = Math.max(0, Math.min(cap.n - 1, Math.round(sAt(x))));
        if (view.ca < 0) view.ca = s; else if (view.cb < 0) view.cb = s;
        else if (Math.abs(view.ca - s) < Math.abs(view.cb - s)) view.ca = s; else view.cb = s;
        draw(); showMeas(); hover(x, y);
      }
      dragFrom = null; showMeas();
    };
    cv.addEventListener('pointerup', up);
    cv.addEventListener('pointercancel', up);
    cv.addEventListener('wheel', e => {
      if (!cap) return;
      e.preventDefault();
      const [x] = px(e);
      if (Math.abs(e.deltaX) > Math.abs(e.deltaY) || e.shiftKey) { view.off += (e.deltaX || e.deltaY) * view.spp; clamp(); draw(); }
      else zoomBy(Math.exp(-e.deltaY * 0.002), x);
    }, { passive: false });
    const onKey = e => { if (e.key === 'Escape' && document.activeElement.tagName !== 'INPUT') { view.ca = view.cb = -1; draw(); showMeas(); } };
    window.addEventListener('keydown', onKey);

    function hover(x, y) {
      if (!cap || x < LBL) return;
      const s = Math.max(0, Math.min(cap.n - 1, Math.floor(sAt(x))));
      const t0 = cap.trig >= 0 ? cap.trig : 0, v = cap.buf[s];
      let txt = `t = ${fmtTime((s - t0) / cap.rate)} · ${tr('muestra')} ${s} · ` + [...Array(NCH)].map((_, c) => v >> c & 1).join('');
      if (view.ca >= 0 && view.cb >= 0) {
        const dt = Math.abs(view.cb - view.ca) / cap.rate;
        txt += ` · A-B ${fmtTime(dt)}` + (dt > 0 ? ` (${fmtFreq(1 / dt)})` : '');
      }
      const r = rows.find(r => y >= r.y && y < r.y + r.h);
      if (r && r.dec !== undefined) {
        const an = (anns[r.dec] || []).find(a => a.s0 <= s && s <= a.s1);
        if (an) txt += ` · ${an.t}`;
      }
      readout.textContent = txt;
    }

    /* ================= talking to the app ================= */

    async function pieces(kind, seq, onPiece) {
      let from = 0, total = 1;
      while (from < total) {
        await send(`${kind} ${seq} ${from}`);
        let got = null;
        for (let k = 0; k < 40 && !got; k++) {
          await new Promise(r => setTimeout(r, k ? 60 : 30));
          const r = await fetch('/api/' + LIVE + '&key=' + kind, { cache: 'no-store' });
          if (!r.ok) continue;
          const b = await r.arrayBuffer(), dv = new DataView(b);
          if (b.byteLength < 20 || dv.getUint32(4, true) !== seq || dv.getUint32(12, true) !== from) continue;
          got = { b, dv };
        }
        if (!got) throw new Error(tr('La placa no mandó la captura'));
        total = got.dv.getUint32(8, true);
        const to = got.dv.getUint32(16, true);
        onPiece(got, from, to, total);
        if (to <= from && from < total) throw new Error(tr('La captura vino cortada'));
        from = to;
      }
    }

    async function fetchCapture(c) {
      const buf = new Uint8Array(c.n);
      await pieces('data', c.seq, ({ b }, from, to) => {
        const u8 = new Uint8Array(b);
        let i = from, p = 32;
        while (i < to && p < u8.length) {
          const v = u8[p++];
          let run = 0, sh = 0, x;
          do { x = u8[p++]; run += (x & 0x7F) * Math.pow(2, sh); sh += 7; } while (x & 0x80);
          buf.fill(v, i, i + run);
          i += run;
        }
        msg.textContent = tr('Bajando la captura: %s', Math.round(to * 100 / c.n) + ' %');
      });
      const nc = { buf, n: c.n, rate: c.rate, trig: c.trig, src: c.src, seq: c.seq };
      summarise(nc);
      return nc;
    }

    async function fetchAnns(seq) {
      const dec = new TextDecoder(), per = [...Array(NDEC)].map(() => []), pk = [];
      for (const kind of ['ann', 'pkt'])
        await pieces(kind, seq, ({ b, dv }, from, to) => {
          const u8 = new Uint8Array(b);
          let p = 20;
          for (let i = from; i < to; i++) {
            const s0 = dv.getUint32(p, true), s1 = dv.getUint32(p + 4, true), row = u8[p + 8], k = u8[p + 9], l = u8[p + 10];
            const t = dec.decode(u8.subarray(p + 11, p + 11 + l));
            p += 11 + l;
            if (kind === 'ann') { if (per[row]) per[row].push({ s0, s1, kind: k, t }); }
            else pk.push({ s0, s1, row, err: !!k, t });
          }
        });
      return { per, pk };
    }

    async function sync() {
      if (busyFetch || local || !st) return;
      busyFetch = true;
      try {
        if (st.cap.ok && st.cap.n && st.cap.seq !== capSeq) {
          const c = await fetchCapture(st.cap);
          cap = c; capSeq = c.seq; annSeq = -1; anns = []; pkts = [];
          view.ca = view.cb = -1;
          fit();
          showSrc();
        }
        if (st.an.ok && st.an.seq !== annSeq && cap && !cap.local) {
          const a = await fetchAnns(st.an.seq);
          anns = a.per; pkts = a.pk; annSeq = st.an.seq;
          draw(); showTable();
          if (decEdit < 0) showDecs();     /* the summaries of this decode */
        }
      } catch (e) { P.toast(e.message, true); }
      busyFetch = false;
      showState();
    }

    function opts(sel, list, cur) {
      const key = list.map(x => x[0]).join(',');
      if (sel.dataset.key !== key) { sel.dataset.key = key; put(sel, ...list.map(([v, t]) => h('option', { value: v }, t))); }
      if (document.activeElement !== sel) sel.value = String(cur);
    }

    function showState() {
      if (!st) return;
      const d = st.devs[st.dev] || {};
      opts(devSel, st.devs.map((x, i) => [i, x.name]), st.dev);
      devInfo.textContent = d.detail || '';
      if (!st.host && d.drv !== 0) devInfo.textContent += ' · ' + tr('el host USB está apagado (Ajustes, USB)');
      opts(rateSel, st.rates.map(r => [r, fmtRate(r)]), cfg.rate);
      const maxS = d.max_samples || st.max_samples;
      const ss = SAMPLES.filter(s => s <= maxS);
      if (d.max_samples && !ss.includes(d.max_samples)) ss.push(d.max_samples);
      if (cfg.samples && !ss.includes(+cfg.samples)) ss.push(+cfg.samples);
      opts(sampSel, ss.sort((a, b) => a - b).map(s => [s, tr('%s muestras', fmtCount(s))]), cfg.samples);
      if (document.activeElement !== preSel) preSel.value = cfg.pre;
      repChk.checked = cfg.repeat === '1';
      (cfg.trig || '').split('').forEach((c, i) => { if (trigSels[i] && document.activeElement !== trigSels[i]) trigSels[i].value = c; });
      capBtn.textContent = st.busy ? tr('Detener') : tr('Capturar');
      capBtn.className = 'btn ' + (st.busy ? 'red' : 'pri');
      const pct = st.total ? Math.min(100, Math.round(st.prog * 100 / st.total)) : 0;
      prog.firstChild.style.width = (st.busy ? pct : 0) + '%';
      if (!busyFetch) {
        msg.textContent = st.busy ? tr(STATES[st.state] || '') + (st.total ? ` ${pct} %` : '') : st.msg;
        msg.style.color = !st.busy && st.err ? 'var(--red)' : '';
      }
    }

    function showSrc() {
      if (!cap) { put(srcInfo); return; }
      const t = `${cap.src} · ${tr('%s muestras', fmtCount(cap.n))} · ${fmtRate(cap.rate)} · ${fmtTime(cap.n / cap.rate)}`;
      put(srcInfo, h('b', {}, local ? tr('Archivo en la computadora') : tr('Captura de la placa')), h('span', { class: 'muted' }, t),
        local ? h('button', { class: 'btn', onclick: () => { local = false; capSeq = -1; annSeq = -1; sync(); } }, tr('Volver a la placa')) : null,
        h('button', { class: 'btn', onclick: () => P.saveBlob(srBuild(cap, names()), (cap.src || 'captura').replace(/\.sr$/, '') + '.sr') }, tr('Bajar .sr')),
        h('button', { class: 'btn', onclick: () => { const [a, b] = visible(true); P.saveBlob(vcdBuild(cap, names(), a, b), 'captura.vcd'); } }, tr('Bajar VCD')),
        h('button', { class: 'btn', onclick: csvSamples }, tr('CSV de lo visible')));
    }

    function visible(cursors) {
      if (cursors && view.ca >= 0 && view.cb >= 0) return [Math.min(view.ca, view.cb), Math.max(view.ca, view.cb)];
      return [Math.max(0, Math.floor(sAt(LBL))), Math.min(cap.n, Math.ceil(sAt(cv.clientWidth)))];
    }

    function csvSamples() {
      const [a, b] = visible(true);
      if (b - a > 2e6) { P.toast(tr('Demasiadas muestras: acercá la vista o poné los cursores'), true); return; }
      const nm = names(), out = ['t_s,' + [...Array(NCH)].map((_, c) => nm[c] || 'D' + c).join(',') + '\n'];
      let prev = -1;
      for (let i = a; i < b; i++) {
        const v = cap.buf[i];
        if (v === prev && i !== b - 1) continue;
        out.push(((i - a) / cap.rate).toPrecision(9) + ',' + [...Array(NCH)].map((_, c) => v >> c & 1).join(',') + '\n');
        prev = v;
      }
      P.saveBlob(new Blob(out, { type: 'text/csv' }), 'captura.csv');
    }

    /* ================= the panels ================= */

    function showDecs() {
      const list = h('div');
      for (let i = 0; i < NDEC; i++) {
        const d = cfg.dec[i] || decDefaults(0), desc = (st && st.decs[i]) ? st.decs[i].desc : '';
        const sum = st && st.an.summary[i];
        list.append(h('div', { class: 'row' },
          h('input', { type: 'checkbox', checked: d.on, onchange: e => { d.on = e.target.checked; send(decLine(i, d)); } }),
          h('div', { class: 'grow' }, h('div', {}, `${i + 1}. ${desc}`), d.on && sum ? h('div', { class: 'small muted' }, sum) : null),
          h('button', { class: 'btn', onclick: () => { decEdit = decEdit === i ? -1 : i; decDraft = JSON.parse(JSON.stringify(cfg.dec[i] || decDefaults(0))); showDecs(); } },
            decEdit === i ? tr('Cerrar') : tr('Editar'))));
        if (decEdit === i) list.append(decEditor(i));
      }
      put(decBox, list, h('p', { class: 'note' }, tr('Los decodificadores corren en la placa: al aplicar, decodifica de nuevo la captura que tiene.')));
    }

    function decEditor(i) {
      const d = decDraft, nm = cfg.names;
      const sel = (val, list, on) => h('select', { onchange: e => on(e.target.value) }, ...list.map(([v, t]) => h('option', { value: v, selected: String(v) === String(val) }, t)));
      const f = (label, el) => h('label', {}, label, el);
      const chans = [[-1, '—'], ...[...Array(NCH)].map((_, c) => [c, `D${c} ${nm[c] || ''}`])];
      const fields = [f(tr('Protocolo'), sel(d.type, DEC_NAMES.map((n, k) => [k, tr(n)]), v => { decDraft = decDefaults(+v); decDraft.ch[0] = d.ch[0] >= 0 && +v !== 9 ? d.ch[0] : decDraft.ch[0]; showDecs(); }))];
      DEC_PINS[d.type].forEach((p, k) => fields.push(f(tr(p) + (DEC_NEED(d.type, k) ? '' : tr(' (optativo)')), sel(d.ch[k], chans, v => { d.ch[k] = +v; }))));
      if (d.type === 0 || d.type === 8) {
        fields.push(f(tr('Velocidad'), sel(d.baud, UART_B.map(b => [b, b ? String(b) : tr('Sola (la adivina)')]), v => { d.baud = +v; })));
        if (d.type === 0) fields.push(f(tr('Bits de datos'), sel(d.bits, [5, 6, 7, 8, 9].map(b => [b, b]), v => { d.bits = +v; })));
        fields.push(f(tr('Paridad'), sel(d.parity, [['N', tr('Ninguna')], ['E', tr('Par')], ['O', tr('Impar')]], v => { d.parity = v; })));
        fields.push(f(tr('Bits de parada'), sel(d.stop, [[1, '1'], [2, '2']], v => { d.stop = +v; })));
        fields.push(f(tr('Línea'), sel(d.inv ? 1 : 0, [[0, tr('Normal')], [1, tr('Invertida')]], v => { d.inv = v === '1'; })));
      } else if (d.type === 4) {
        fields.push(f(tr('Velocidad del bus'), sel(d.baud, CAN_B.map(b => [b, fmtCount(b) + ' bit/s']), v => { d.baud = +v; })));
      } else if (d.type === 2) {
        fields.push(f(tr('Modo (CPOL y CPHA)'), sel(d.mode, [0, 1, 2, 3].map(m => [m, m]), v => { d.mode = +v; })));
        fields.push(f(tr('Bits por palabra'), sel(d.bits, [4, 8, 9, 10, 12, 16].map(b => [b, b]), v => { d.bits = +v; })));
        fields.push(f(tr('Orden'), sel(d.lsb ? 1 : 0, [[0, tr('MSB primero')], [1, tr('LSB primero')]], v => { d.lsb = v === '1'; })));
        fields.push(f('CS', sel(d.inv ? 1 : 0, [[0, tr('Activo en 0')], [1, tr('Activo en 1')]], v => { d.inv = v === '1'; })));
      } else if (d.type === 6) {
        fields.push(f(tr('Nivel con portadora'), sel(d.inv ? 1 : 0, [[0, tr('0 (receptor)')], [1, tr('1 (LED)')]], v => { d.inv = v === '1'; })));
      }
      if (![5, 6, 8].includes(d.type))
        fields.push(f(tr('Valores en'), sel(d.fmt, [[0, tr('Hexa')], [1, tr('Decimal')], [2, 'ASCII'], [3, tr('Binario')]], v => { d.fmt = +v; })));
      return h('div', { class: 'card2 pad', style: 'border-radius:10px;background:var(--card2);margin:6px 0' },
        h('div', { class: 'lg-dec' }, ...fields),
        h('div', { class: 'lg-row', style: 'margin-top:8px' },
          h('button', { class: 'btn pri', onclick: () => { d.on = true; cfg.dec[i] = d; send(decLine(i, d)); decEdit = -1; showDecs(); } }, tr('Aplicar'))));
    }

    function showTable() {
      if (!pkts.length) { put(tabBox, h('p', { class: 'note' }, cap && cap.local ? tr('Los paquetes los arma la placa: abrí el archivo en la placa para decodificarlo.') : tr('No hay paquetes: encendé un decodificador.'))); return; }
      const q = tabQuery.toLowerCase();
      const list = pkts.filter(p => (tabFilter < 0 || p.row === tabFilter) && (!q || p.t.toLowerCase().includes(q)));
      const PAGE = 200, pages = Math.max(1, Math.ceil(list.length / PAGE));
      tabPage = Math.min(tabPage, pages - 1);
      const t0 = cap && cap.trig >= 0 ? cap.trig : 0, rate = cap ? cap.rate : 1;
      const rowsEl = list.slice(tabPage * PAGE, tabPage * PAGE + PAGE).map(p =>
        h('tr', { onclick: () => goTo(p.s0, p.s1) },
          h('td', { class: 'lg-mono' }, fmtTime((p.s0 - t0) / rate)),
          h('td', {}, `${p.row + 1} ${tr(DEC_NAMES[(cfg.dec[p.row] || {}).type] || '')}`),
          h('td', { class: 'lg-mono' + (p.err ? ' err' : '') }, p.t)));
      const filt = h('select', { onchange: e => { tabFilter = +e.target.value; tabPage = 0; showTable(); } },
        h('option', { value: -1 }, tr('Todos')),
        ...cfg.dec.map((d, i) => d && d.on ? h('option', { value: i, selected: tabFilter === i }, `${i + 1} ${tr(DEC_NAMES[d.type])}`) : null));
      const search = h('input', { type: 'text', placeholder: tr('Buscar'), value: tabQuery, oninput: e => { tabQuery = e.target.value; tabPage = 0; clearTimeout(search.t); search.t = setTimeout(showTable, 250); } });
      put(tabBox,
        h('div', { class: 'lg-row' }, filt, search,
          h('span', { class: 'muted' }, tr('%s paquetes', list.length)),
          tabPage > 0 ? h('button', { class: 'btn', onclick: () => { tabPage--; showTable(); } }, '‹') : null,
          pages > 1 ? h('span', { class: 'muted' }, `${tabPage + 1}/${pages}`) : null,
          tabPage < pages - 1 ? h('button', { class: 'btn', onclick: () => { tabPage++; showTable(); } }, '›') : null,
          h('button', { class: 'btn', onclick: () => {
            const out = ['t_s,end_s,decoder,error,text\n', ...list.map(p => `${(p.s0 - t0) / rate},${(p.s1 - t0) / rate},${DEC_NAMES[(cfg.dec[p.row] || {}).type] || ''},${p.err ? 1 : 0},"${p.t.replace(/"/g, '""')}"\n`)];
            P.saveBlob(new Blob(out, { type: 'text/csv' }), 'paquetes.csv');
          } }, tr('Bajar CSV'))),
        h('table', { class: 'lg-tab' }, ...rowsEl));
      if (document.activeElement && document.activeElement.placeholder === tr('Buscar')) { search.focus(); search.setSelectionRange(search.value.length, search.value.length); }
    }

    let measCh = 0;
    function showMeas() {
      if (!cap) { put(measBox, h('p', { class: 'note' }, tr('No hay captura.'))); return; }
      const [a, b] = visible(true), m = measure(cap, measCh, a, b);
      const stats = l => l.length ? `${fmtTime(Math.min(...l))} / ${fmtTime(l.reduce((x, y) => x + y, 0) / l.length)} / ${fmtTime(Math.max(...l))}` : '—';
      const nm = names();
      put(measBox,
        h('div', { class: 'lg-row' }, h('select', { onchange: e => { measCh = +e.target.value; showMeas(); } },
          ...[...Array(NCH)].map((_, c) => h('option', { value: c, selected: c === measCh }, `D${c} ${nm[c] || ''}`))),
          h('span', { class: 'muted' }, view.ca >= 0 && view.cb >= 0 ? tr('entre los cursores A y B') : tr('lo que se ve'))),
        h('div', { class: 'lg-meas', style: 'margin-top:8px' },
          h('span', { class: 'muted' }, tr('Tramo')), h('span', {}, `${fmtTime((b - a) / cap.rate)} · ${tr('%s muestras', b - a)}`),
          h('span', { class: 'muted' }, tr('Flancos')), h('span', {}, tr('%s de subida, %s de bajada', m.rises, m.falls)),
          h('span', { class: 'muted' }, tr('Frecuencia')), h('span', {}, m.freq ? `${fmtFreq(m.freq)} (${fmtTime(1 / m.freq)})` : '—'),
          h('span', { class: 'muted' }, tr('Ciclo de trabajo')), h('span', {}, m.freq ? (m.duty * 100).toFixed(1) + ' %' : '—'),
          h('span', { class: 'muted' }, tr('En 1 (mín/prom/máx)')), h('span', {}, stats(m.hi)),
          h('span', { class: 'muted' }, tr('En 0 (mín/prom/máx)')), h('span', {}, stats(m.lo)),
          h('span', { class: 'muted' }, tr('En 1')), h('span', {}, (m.high * 100).toFixed(1) + ' %')));
    }

    function showChannels() {
      const nm = cfg.names, show = (cfg.show || '11111111').split('');
      put(chBox, ...[...Array(NCH)].map((_, c) => h('label', {},
        h('div', { class: 'lg-row' },
          h('input', { type: 'checkbox', checked: show[c] !== '0', onchange: e => { show[c] = e.target.checked ? '1' : '0'; cfg.show = show.join(''); send('show=' + cfg.show); draw(); } }),
          h('span', { class: 'lg-mono', style: `color:${CH_COL[c]}` }, 'D' + c)),
        h('input', { type: 'text', maxlength: 11, value: nm[c] || '', onchange: e => { const v = e.target.value.replace(/[\r\n=]/g, '').slice(0, 11); cfg.names[c] = v; send(`name${c}=${v}`); draw(); } }))));
    }

    async function showFiles() {
      let fl = [];
      try { fl = (await P.fsList(DIR)).filter(f => !f.dir && f.name.endsWith('.sr') && !f.name.startsWith('.')); } catch (e) { put(filesBox, h('div', { class: 'note' }, e.message)); return; }
      fl.sort((a, b) => b.name.localeCompare(a.name));
      const inp = h('input', { type: 'file', accept: '.sr', style: 'display:none', onchange: async e => {
        const f = e.target.files[0];
        if (!f) return;
        try { cap = await srRead(await f.arrayBuffer(), f.name); summarise(cap); local = true; anns = []; pkts = []; view.ca = view.cb = -1; fit(); showSrc(); showTable(); }
        catch (err) { P.toast(err.message, true); }
      } });
      const upl = h('input', { type: 'file', accept: '.sr', style: 'display:none', onchange: async e => {
        const f = e.target.files[0];
        if (!f) return;
        try { await P.fsMkdir(DIR); await P.fsPut(`${DIR}/${P.fsSlug(f.name.replace(/\.sr$/, ''), 40, 'captura')}.sr`, f); P.toast(tr('Subida')); showFiles(); }
        catch (err) { P.toast(err.message, true); }
      } });
      put(filesBox,
        h('div', { class: 'lg-row' },
          h('button', { class: 'btn', onclick: () => send(`save ${DIR}/cap_${new Date().toISOString().replace(/[-:]/g, '').replace('T', '_').slice(0, 15)}.sr`).then(() => setTimeout(showFiles, 2500)) }, tr('Guardar la captura de la placa')),
          h('button', { class: 'btn', onclick: () => inp.click() }, tr('Ver un .sr de la computadora')),
          h('button', { class: 'btn', onclick: () => upl.click() }, tr('Subir un .sr a la placa')), inp, upl),
        fl.length ? h('table', { class: 'lg-tab' }, ...fl.map(f => h('tr', {},
          h('td', {}, f.name), h('td', { class: 'muted' }, P.fmtBytes(f.size)),
          h('td', { style: 'text-align:right;white-space:nowrap' },
            h('button', { class: 'btn', onclick: e => { e.stopPropagation(); local = false; send(`load ${DIR}/${f.name}`); P.toast(tr('Abriendo en la placa')); } }, tr('Abrir en la placa')),
            ' ',
            h('button', { class: 'btn', onclick: async e => {
              e.stopPropagation();
              try { cap = await srRead(await P.fsBytes(`${DIR}/${f.name}`), f.name); summarise(cap); local = true; anns = []; pkts = []; view.ca = view.cb = -1; fit(); showSrc(); showTable(); }
              catch (err) { P.toast(err.message, true); }
            } }, tr('Ver acá')),
            ' ', h('a', { class: 'btn', href: P.fsUrl(`${DIR}/${f.name}`, true) }, tr('Bajar')),
            ' ', h('button', { class: 'btn red', onclick: async e => {
              e.stopPropagation();
              if (!confirm(tr('¿Borrar %s?', f.name))) return;
              try { await P.fsDelete(`${DIR}/${f.name}`); await P.fsDelete(`${DIR}/${f.name.replace(/\.sr$/, '.csv')}`).catch(() => {}); showFiles(); } catch (err) { P.toast(err.message, true); }
            } }, tr('Borrar')))))) : h('p', { class: 'note' }, tr('Todavía no hay capturas guardadas en la tarjeta.')),
        h('p', { class: 'note' }, tr('Son sesiones de sigrok: PulseView las abre tal cual (Archivo, Abrir), y la placa abre las que guarda PulseView.')));
    }

    async function showFw() {
      let fl = [];
      try { fl = (await P.fsList(FWDIR)).filter(f => !f.dir && /\.(fw|ihx|hex)$/.test(f.name)); } catch (e) { /* no folder yet */ }
      const inp = h('input', { type: 'file', multiple: true, accept: '.fw,.ihx,.hex,.gz,.tgz', style: 'display:none', onchange: async e => {
        try {
          let files = [];
          for (const f of e.target.files) {
            if (/\.(tar\.gz|tgz|gz)$/.test(f.name)) files = files.concat(await untarGz(f));
            else files.push([f.name, new Uint8Array(await f.arrayBuffer())]);
          }
          if (!files.length) throw new Error(tr('No encontré ningún fx2lafw-*.fw ahí adentro'));
          await P.fsMkdir(DIR); await P.fsMkdir(FWDIR);
          for (const [n, d] of files) await P.fsPut(`${FWDIR}/${n}`, new Blob([d]));
          P.toast(tr('%s archivos de firmware en la tarjeta', files.length));
          send('rescan');
          showFw();
        } catch (err) { P.toast(err.message, true); }
      } });
      put(fwBox,
        h('p', { class: 'note' }, tr('Los analizadores "24 MHz 8 canales" (clones del Saleae Logic, con un Cypress FX2LP) no tienen programa propio: la app les carga fx2lafw, el firmware libre de sigrok, cada vez que se enchufan. Bajá "sigrok-firmware-fx2lafw" (el .tar.gz de binarios) y subilo acá tal cual: la página saca los .fw y los pone en la carpeta logica/fx2lafw.')),
        h('div', { class: 'lg-row' },
          h('button', { class: 'btn pri', onclick: () => inp.click() }, tr('Subir el .tar.gz o los .fw')), inp,
          h('a', { class: 'btn', href: 'https://sigrok.org/wiki/Fx2lafw', target: '_blank', rel: 'noopener' }, tr('fx2lafw en sigrok.org')),
          h('a', { class: 'btn', href: 'https://sigrok.org/download/binary/sigrok-firmware-fx2lafw/', target: '_blank', rel: 'noopener' }, tr('Descargas'))),
        fl.length ? h('p', { class: 'small' }, tr('En la tarjeta: %s', fl.map(f => f.name).join(', '))) : h('p', { class: 'small muted' }, tr('Todavía no hay firmware en la tarjeta.')),
        h('p', { class: 'note' }, tr('Conexión: el analizador va al host USB de la placa (Ajustes, USB): en los pines 25/27 corre a alta velocidad y llega a 24 MHz; en 21/23 hasta 1 MHz. Medí señales de 3,3 V (de 5 V sólo si tu analizador dice que las aguanta) y uní su masa con la del circuito.')));
    }

    /* ================= the loop ================= */

    let lastCfg = '';
    async function tick() {
      try {
        const s = await P.api(LIVE + '&key=state');
        st = s; gone = false;
        if (s.cfg !== lastCfg) {
          lastCfg = s.cfg;
          cfg = parseCfg(s.cfg);
          showDecs(); showChannels(); draw();
        }
        showState();
        sync();
      } catch (e) {
        if (!gone) { gone = true; st = null; msg.textContent = tr('La app Lógica no está abierta en la placa.'); draw(); }
      }
    }
    tick(); showFiles(); showFw(); showMeas(); showTable(); draw();
    const timer = setInterval(tick, 500);
    const onResize = () => { clamp(); draw(); };
    window.addEventListener('resize', onResize);
    return () => { clearInterval(timer); window.removeEventListener('resize', onResize); window.removeEventListener('keydown', onKey); };
  },
});
