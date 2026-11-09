/*
 * P4OS - the Modbus app's page in the portal: the device profiles.
 *
 * A profile is a CSV file in /modbus/perfiles on the card (the format is in
 * apps/modbus/main/mb_prof.h and the README): this page lists them, edits
 * one as a table, saves it, makes a new one, uploads and downloads them, and
 * tells the board which to use. The app reads a file again when it changes,
 * and while this page looks it sends the values it reads (or plays) through
 * the live channel (docs/PORTAL-PAGES.md, "Live data"); this page sends
 * "perfil=<file>" the same way to choose one.
 */
const P = window.P4OS;
const { h, put } = P;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;           /* marks a table's names for tools/gen_lang.py; tr() at use */

const APP = 'aos.modbus';
const DIR = '/modbus/perfiles';
const LIVE = 'live?app=' + APP;
const TABLES = ['coil', 'di', 'hr', 'ir'];
const TYPES = ['bit', 'u16', 's16', 'hex', 'u32', 's32', 'f32', 'u32cdab', 's32cdab', 'f32cdab'];
const SIMS = ['', 'fijo', 'seno', 'rampa', 'deriva', 'contador', 'alterna', 'azar'];
const COLS = ['tabla', 'direccion', 'nombre', 'tipo', 'escala', 'unidad', 'acceso', 'min', 'max', 'simulacion', 'valor'];

/* ---- the file ---- */

function splitCsv(line) {
  const out = [];
  let cur = '', q = false;
  for (const c of line) {
    if (c === '"') q = !q;
    else if (c === ',' && !q) { out.push(cur.trim()); cur = ''; }
    else cur += c;
  }
  out.push(cur.trim());
  return out;
}

function parse(text) {
  const p = { nombre: '', nota: '', esclavo: '1', velocidad: '', formato: '', periodo: '1000', pts: [], bad: 0 };
  for (let raw of text.replace(/^﻿/, '').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const c = splitCsv(line);
    const k = c[0].toLowerCase();
    const map = { name: 'nombre', note: 'nota', slave: 'esclavo', baud: 'velocidad', format: 'formato', poll: 'periodo' };
    const key = map[k] || k;
    if (['nombre', 'nota', 'esclavo', 'velocidad', 'formato', 'periodo'].includes(key)) p[key] = c[1] || '';
    else if (TABLES.includes(k) || ['holding', 'input', 'coils', 'discrete', 'entrada', 'entradas'].includes(k)) {
      const t = { holding: 'hr', input: 'ir', coils: 'coil', discrete: 'di', entrada: 'di', entradas: 'di' }[k] || k;
      const pt = {};
      COLS.forEach((name, i) => { pt[name] = c[i] ?? ''; });
      pt.tabla = t;
      p.pts.push(pt);
    } else p.bad++;
  }
  return p;
}

function q(s) { s = String(s ?? ''); return /[,"]/.test(s) ? '"' + s.replace(/"/g, '') + '"' : s; }

function serialize(p) {
  const L = ['# P4OS Modbus profile (apps/modbus/README.md, "Perfiles")', 'nombre,' + q(p.nombre)];
  if (p.nota) L.push('nota,' + q(p.nota));
  L.push('esclavo,' + (p.esclavo || 1));
  if (p.velocidad) L.push('velocidad,' + p.velocidad);
  if (p.formato) L.push('formato,' + p.formato);
  L.push('periodo,' + (p.periodo || 1000));
  L.push('# ' + COLS.join(','));
  for (const pt of p.pts) L.push(COLS.map(c => q(pt[c])).join(','));
  return L.join('\n') + '\n';
}

/* ---- importing a register map from Home Assistant or ESPHome ----
 *
 * What people already have in text: Home Assistant's `modbus:` (sensors,
 * binary_sensors, switches, with address, input_type, data_type, scale,
 * slave) and ESPHome's `modbus_controller` platforms (address,
 * register_type, value_type, a multiply filter). Not a YAML parser: the
 * list items ("- ...") with their keys, by indentation, which is all these
 * files are. A list inside an item (ESPHome's filters) lends its keys to
 * the item. */
function yamlItems(text) {
  const items = [], open = [], keys = [], top = {};
  for (const raw of text.replace(/\r/g, '').split('\n')) {
    const line = raw.replace(/(^|\s)#.*$/, '');
    if (!line.trim()) continue;
    const ind = line.search(/\S/);
    let body = line.trim();
    while (open.length && ind <= open[open.length - 1].ind) open.pop();
    while (keys.length && ind <= keys[keys.length - 1].ind) keys.pop();
    let item = null;
    if (body === '-' || body.startsWith('- ')) {
      item = { ind, ctx: keys.map(k => k.key), f: {}, parent: open.length ? open[open.length - 1] : null };
      items.push(item);
      open.push(item);
      body = body.slice(1).trim();
      if (!body) continue;
    }
    const m = body.match(/^([A-Za-z_][\w.-]*)\s*:\s*(.*)$/);
    if (!m) continue;
    const key = m[1].toLowerCase(), val = m[2].replace(/^["']|["']$/g, '').trim();
    if (!val) { keys.push({ ind: item ? ind + 2 : ind, key }); continue; }
    const target = item || (open.length ? open[open.length - 1] : null);
    if (target) { if (!(key in target.f)) target.f[key] = val; }
    else if (!(key in top)) top[key] = val;
  }
  /* an item with no address inside one with it (ESPHome's filters) lends it its keys */
  for (const it of items)
    if (!('address' in it.f) && it.parent && 'address' in it.parent.f)
      for (const [k, v] of Object.entries(it.f)) if (!(k in it.parent.f)) it.parent.f[k] = v;
  return { items, top };
}

const HA_TYPE = { int16: 's16', uint16: 'u16', int32: 's32', uint32: 'u32', float32: 'f32', int: 's16', uint: 'u16', float: 'f32' };
const EH_TYPE = { u_word: 'u16', s_word: 's16', u_dword: 'u32', s_dword: 's32', fp32: 'f32', u_dword_r: 'u32cdab', s_dword_r: 's32cdab', fp32_r: 'f32cdab' };
const TABLE = { holding: 'hr', input: 'ir', read: 'ir', coil: 'coil', discrete_input: 'di', discrete: 'di' };

function importYaml(text, fname) {
  const p = { nombre: fname.replace(/\.(ya?ml)$/i, ''), nota: '', esclavo: '1', velocidad: '', formato: '', periodo: '1000', pts: [], bad: 0 };
  const esphome = /modbus_controller/.test(text);
  let skipped = 0;
  const { items, top } = yamlItems(text);
  for (const it of [{ f: top, ctx: [] }, ...items]) {
    const f = it.f, ctx = it.ctx.join('/');
    if (f.baudrate || f.baud_rate) p.velocidad = f.baudrate || f.baud_rate;
    if (f.parity) p.formato = '8' + ({ e: 'E', even: 'E', o: 'O', odd: 'O' }[f.parity.toLowerCase()] || 'N') + ((f.stopbits || f.stop_bits) === '2' ? '2' : '1');
    if (/modbus_controller$/.test(ctx) && f.address) { p.esclavo = String(parseInt(f.address)); continue; }
    if (!('address' in f) || it === undefined || !it.ctx.length) continue;
    if (esphome && f.platform && f.platform !== 'modbus_controller') continue;
    const unit = f.slave || f.device_address;
    if (unit && p.pts.length === 0) p.esclavo = String(parseInt(unit));
    const writes = /switch|number|select|output/.test(ctx);
    const bits = /binary_sensor/.test(ctx);
    let tabla = TABLE[(f.register_type || f.input_type || f.write_type || '').toLowerCase()] || (bits ? 'coil' : writes && /switch/.test(ctx) ? 'coil' : 'hr');
    let tipo = 'u16';
    if (tabla === 'coil' || tabla === 'di') tipo = 'bit';
    else if (f.value_type) tipo = EH_TYPE[f.value_type.toLowerCase()] || 'u16';
    else if (f.data_type) {
      tipo = HA_TYPE[f.data_type.toLowerCase()] || 'u16';
      if (/^(word|word_byte)$/.test((f.swap || '').toLowerCase()) && /32/.test(tipo)) tipo += 'cdab';
    }
    if (!['u16', 's16', 'bit', 'u32', 's32', 'f32', 'u32cdab', 's32cdab', 'f32cdab'].includes(tipo)) { skipped++; continue; }
    const addr = parseInt(f.address);
    if (isNaN(addr)) { skipped++; continue; }
    p.pts.push({
      tabla, direccion: String(addr), nombre: (f.name || `${tabla} ${addr}`).replace(/,/g, ' ').slice(0, 31), tipo,
      escala: f.scale || f.multiply || '', unidad: (f.unit_of_measurement || '').replace(/,/g, ' '),
      acceso: writes ? 'rw' : 'r', min: f.min_value || '', max: f.max_value || '', simulacion: '', valor: '',
    });
  }
  p.nota = (esphome ? tr('Importado de ESPHome') : tr('Importado de Home Assistant')) + (skipped ? ' · ' + tr('%s entradas sin traer', skipped) : '');
  return p;
}

function slug(name) {
  const s = (P.fsSlug ? P.fsSlug(name, 40, 'perfil') : name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')) || 'perfil';
  return s + '.csv';
}

/* ---- the page ---- */

function css() {
  if (document.getElementById('css-modbus')) return;
  document.head.appendChild(h('style', { id: 'css-modbus' }, `
    .mbw { display: grid; grid-template-columns: minmax(220px, 300px) 1fr; gap: 16px; align-items: start; }
    @media (max-width: 860px) { .mbw { grid-template-columns: 1fr; } }
    .mbl button.mbi { display: block; width: 100%; text-align: left; padding: 10px 12px; border: 0; border-radius: 10px;
      background: transparent; color: inherit; cursor: pointer; font: inherit; }
    .mbl button.mbi:hover { background: var(--card2); }
    .mbl button.mbi.on { background: var(--card2); outline: 2px solid var(--accent); }
    .mbl .mbn { font-weight: 600; }
    .mbt { width: 100%; border-collapse: collapse; font-size: 13px; }
    .mbt th { text-align: left; color: var(--dim); font-weight: 500; padding: 4px; white-space: nowrap; }
    .mbt td { padding: 2px 3px; border-top: 1px solid var(--line); }
    .mbt input, .mbt select { width: 100%; box-sizing: border-box; padding: 5px 6px; font: inherit;
      border: 1px solid var(--line); border-radius: 6px; background: var(--card); color: inherit; }
    .mbt select { min-width: 72px; }
    .mbt input { min-width: 60px; }
    .mbt input.w { min-width: 150px; }
    .mbt input.n { max-width: 84px; }
    .mbt { min-width: 1080px; }
    .mbt td.v { font-weight: 600; color: var(--orange); white-space: nowrap; min-width: 80px; }
    .mbt td.v.e { color: var(--dim); font-weight: 400; }
    .mbh { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin-bottom: 12px; }
    .mbh label { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--dim); }
    .mbh input { padding: 7px 8px; font: inherit; font-size: 14px; border: 1px solid var(--line); border-radius: 8px;
      background: var(--card); color: var(--fg, inherit); }
    .mbscroll { overflow-x: auto; }
    .mbctl > .card, .mbctl > div > .card { margin-bottom: 14px; }
    .mbstate { font-weight: 600; }
    .mbstate.ok { color: var(--green); }
    .mbstate.bad, .mberr { color: var(--red); }
    .mbok { color: var(--green); font-weight: 600; }
    .mbwarn { color: var(--orange); }
    .mbck { display: inline-flex; align-items: center; gap: 6px; }
    .mbh select { padding: 7px 8px; font: inherit; font-size: 14px; border: 1px solid var(--line); border-radius: 8px;
      background: var(--card); color: inherit; }
    .mbgrid { display: grid; grid-template-columns: repeat(auto-fill, minmax(120px, 1fr)); gap: 6px; margin: 8px 0; }
    .mbcell { display: flex; justify-content: space-between; padding: 6px 10px; border-radius: 8px; background: var(--card2); }
    .mbcell .a { color: var(--orange); font-family: ui-monospace, monospace; font-size: 12px; }
    .mbcell .v { font-family: ui-monospace, monospace; font-weight: 600; }
    .mbcell.on .v { color: var(--green); }
    .mbtraffic { max-height: 520px; overflow-y: auto; display: flex; flex-direction: column; gap: 6px; margin-top: 10px; }
    .mbfr { padding: 6px 10px; border-radius: 8px; background: var(--card2); }
    .mbfr .mono { font-family: ui-monospace, monospace; font-size: 12px; word-break: break-all; }
    .mbfr.tx .mono { color: var(--accent); }
    .mbfr.bad .mono { color: var(--red); }
    .mbfr.warn .mono { color: var(--orange); }
  `));
}

/* ---- the Control tab: the app on the board, driven from here ---- */

const FCS = [[1, N_('01 Coils')], [2, N_('02 Entradas')], [3, N_('03 Holding')], [4, N_('04 Input')], [5, N_('05 Coil')], [6, N_('06 Registro')],
  [15, N_('15 Coils')], [16, N_('16 Registros')], [8, N_('08 Eco')], [17, N_('17 Id')], [43, N_('43 Identificación')]];
const FAULTS = [N_('Normal'), N_('Mudo'), N_('Lento'), N_('CRC malo'), N_('Ocupado (06)'), N_('Falla (04)')];
const BAUDS = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200];
const ROLES = [['maestro', N_('Maestro')], ['esclavo', N_('Esclavo')], ['monitor', N_('Monitor')]];

function control(box, hidden) {
  const send = (...lines) => P.api(LIVE, { method: 'POST', body: lines.join('\n') })
    .then(() => setTimeout(poll, 300))
    .catch(e => P.toast(tr('La app Modbus no está abierta en la placa (%s)', e.message), true));
  let st = null, timer = null, paused = false, lastTraffic = -1;

  /* fields that keep what is typed while the board's state comes in */
  const inp = (attrs = {}) => h('input', { ...attrs });
  const sel = (opts, attrs = {}) => h('select', attrs, ...opts.map(([v, t]) => h('option', { value: v }, t)));
  const setv = (el, v) => { if (document.activeElement !== el && v !== undefined && v !== null) el.value = String(v); };

  const stateLine = h('div', { class: 'mbstate' });
  const openBtn = h('button', { class: 'btn pri', onclick: () => send('abrir=' + (st && st.quiere ? 0 : 1)) });
  const roleBtn = ROLES.map(([, t], i) => h('button', { class: 'btn', onclick: () => send('rol=' + i) }, tr(t)));
  const roleBtns = h('div', { class: 'btns' }, ...roleBtn);
  const port = inp({ list: 'mbports', size: 8 });
  const baud = sel(BAUDS.map(b => [b, b]));
  const fmt = sel(['8N1', '8E1', '8O1', '8N2'].map(f => [f, f]));
  const wait = inp({ size: 5, inputmode: 'numeric' });
  const gap = inp({ size: 4, inputmode: 'numeric' });
  const applyLink = () => send('puerto=' + port.value.trim(), 'baud=' + baud.value, 'formato=' + fmt.value,
    'espera=' + wait.value, 'silencio=' + gap.value);

  /* master */
  const mUnit = inp({ size: 4, inputmode: 'numeric' }), mFc = sel(FCS.map(([v, t]) => [v, tr(t)]));
  const mAddr = inp({ size: 6 }), mN = inp({ size: 5, inputmode: 'numeric' }), mVals = inp({ size: 18, placeholder: tr('valores (escrituras)') });
  const mLive = h('input', { type: 'checkbox', onchange: e => send('vivo=' + (e.target.checked ? 1 : 0), 'periodo=' + mPer.value) });
  const mPer = sel([100, 250, 500, 1000, 2000].map(v => [v, v + ' ms']), { onchange: () => send('periodo=' + mPer.value) });
  const mFmt = sel([['u16', 'U16'], ['s16', 'S16'], ['hex', 'HEX'], ['bin', 'BIN']], { onchange: () => drawMaster(true) });
  const mRes = h('div'), mCounts = h('div', { class: 'muted small' }), mTool = h('div');
  const gwOn = h('input', { type: 'checkbox', onchange: e => send('pasarela=' + (e.target.checked ? 1 : 0)) });
  const gwPort = inp({ size: 6, inputmode: 'numeric' });
  const gwInfo = h('div', { class: 'muted small' });
  const sendReq = () => {
    const fc = +mFc.value, one = fc === 5 || fc === 6 || fc === 8;
    const vals = mVals.value.trim().split(/[\s,;]+/).filter(Boolean);
    const line = one ? `${mUnit.value},${fc},${mAddr.value},${vals[0] ?? mN.value}`
      : `${mUnit.value},${fc},${mAddr.value},${mN.value}${vals.length ? ',' + vals.join(' ') : ''}`;
    send('pedido=' + line);
  };
  const masterCard = h('div', { class: 'card pad' },
    h('h2', { style: 'margin-top:0' }, tr('Maestro')),
    h('div', { class: 'mbh' },
      h('label', {}, tr('Unidad'), mUnit), h('label', {}, tr('Función'), mFc), h('label', {}, tr('Desde'), mAddr),
      h('label', {}, tr('Cantidad'), mN), h('label', {}, tr('Valores'), mVals)),
    h('div', { class: 'btns' },
      h('button', { class: 'btn pri', onclick: sendReq }, tr('Enviar')),
      h('label', { class: 'mbck' }, mLive, tr('En vivo')), mPer, mFmt),
    mRes, mCounts,
    h('div', { class: 'btns', style: 'margin-top:10px' },
      h('button', { class: 'btn', onclick: () => send('herramienta=unidades') }, tr('Buscar unidades')),
      h('button', { class: 'btn', onclick: () => send('herramienta=velocidad') }, tr('Buscar velocidad')),
      h('button', { class: 'btn', onclick: () => send('herramienta=parar') }, tr('Detener')),
      h('button', { class: 'btn', onclick: () => send('borrar=cuentas') }, tr('Borrar cuentas'))),
    mTool,
    h('h3', {}, tr('Pasarela Modbus TCP → RTU')),
    h('div', { class: 'btns' },
      h('label', { class: 'mbck' }, gwOn, tr('Atender clientes TCP')),
      h('label', { class: 'mbck' }, tr('Puerto'), gwPort),
      h('button', { class: 'btn', onclick: () => send('pasarela_puerto=' + gwPort.value) }, tr('Aplicar'))),
    gwInfo,
    h('p', { class: 'note small' }, tr('Un programa de la PC (Modbus Poll, un SCADA, Home Assistant) pide por la red a la placa, y ella le pregunta al equipo por el bus. Si el equipo no contesta, el cliente recibe la excepción 0B.')));

  /* slave */
  const sUnit = inp({ size: 4, inputmode: 'numeric' }), sFault = sel(FAULTS.map((f, i) => [i, tr(f)]), { onchange: () => send('falla=' + sFault.value) });
  const sDelay = inp({ size: 5, inputmode: 'numeric' }), sAnim = h('input', { type: 'checkbox', onchange: e => send('animar=' + (e.target.checked ? 1 : 0)) });
  const sInfo = h('div', { class: 'muted small' });
  const slaveCard = h('div', { class: 'card pad' },
    h('h2', { style: 'margin-top:0' }, tr('Esclavo')),
    h('div', { class: 'mbh' },
      h('label', {}, tr('Unidad'), sUnit), h('label', {}, tr('Falla'), sFault), h('label', {}, tr('Demora (ms)'), sDelay)),
    h('div', { class: 'btns' },
      h('button', { class: 'btn pri', onclick: () => send('unidad=' + sUnit.value, 'demora=' + sDelay.value) }, tr('Aplicar')),
      h('label', { class: 'mbck' }, sAnim, tr('Valores que se mueven')),
      h('button', { class: 'btn', onclick: () => send('borrar=cuentas') }, tr('Borrar cuentas'))),
    sInfo);

  /* monitor */
  const monInfo = h('div'), monTable = h('div', { class: 'mbscroll' });
  const monCard = h('div', { class: 'card pad' },
    h('h2', { style: 'margin-top:0' }, tr('Monitor')), monInfo, monTable,
    h('div', { class: 'btns', style: 'margin-top:10px' }, h('button', { class: 'btn', onclick: () => send('borrar=cuentas') }, tr('Borrar cuentas'))));

  /* traffic */
  const trTable = h('div', { class: 'mbtraffic' });
  const pauseCk = h('input', { type: 'checkbox', onchange: e => { paused = e.target.checked; } });
  const errCk = h('input', { type: 'checkbox', onchange: () => { lastTraffic = -1; pollTraffic(); } });
  const trafficCard = h('div', { class: 'card pad' },
    h('h2', { style: 'margin-top:0' }, tr('Tráfico')),
    h('div', { class: 'btns' },
      h('label', { class: 'mbck' }, pauseCk, tr('Pausa')), h('label', { class: 'mbck' }, errCk, tr('Sólo errores')),
      h('button', { class: 'btn', onclick: () => send('borrar=trafico') }, tr('Borrar'))),
    trTable);

  const linkCard = h('div', { class: 'card pad' },
    h('div', { class: 'row' }, h('div', { class: 'grow' }, stateLine), openBtn),
    roleBtns,
    h('div', { class: 'mbh', style: 'margin-top:12px' },
      h('label', {}, tr('Puerto'), port), h('label', {}, tr('Velocidad'), baud), h('label', {}, tr('Formato'), fmt),
      h('label', {}, tr('Espera (ms)'), wait), h('label', {}, tr('Silencio (ms)'), gap)),
    h('datalist', { id: 'mbports' }, ...['uart.a', 'uart.b', 'usb0', 'usb1'].map(p => h('option', { value: p }))),
    h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: applyLink }, tr('Aplicar conexión')),
      h('button', { class: 'btn', onclick: () => P.openApp(APP).then(() => P.toast(tr('Abierta en la placa'))) }, tr('Abrir la app en la placa'))));
  const roleBox = h('div');
  box.className = 'mbctl';
  put(box, linkCard, roleBox, trafficCard);

  function fmtVal(v) {
    switch (mFmt.value) {
      case 's16': return v > 32767 ? v - 65536 : v;
      case 'hex': return '0x' + v.toString(16).toUpperCase().padStart(4, '0');
      case 'bin': return v.toString(2).padStart(16, '0').replace(/(.{4})/g, '$1 ').trim();
      default: return v;
    }
  }

  let lastMaster = '';
  function drawMaster(force) {
    if (!st) return;
    const m = st.maestro;
    const key = JSON.stringify([m.seq, m.rc, m.herramienta, st.perfil, mFmt.value, m.cuentas]);
    if (key === lastMaster && force !== true) return;
    lastMaster = key;
    if (st.perfil) {
      put(mRes, h('p', { class: 'note' }, tr('Con el perfil "%s" la app lo lee entero: los valores están en la pestaña Perfiles.', st.perfil_nombre),
        ' ', h('button', { class: 'btn small', onclick: () => send('perfil=') }, tr('Pasar a pedido a mano'))));
    } else if (!m.seq) put(mRes, h('p', { class: 'muted' }, tr('Todavía no hay respuesta.')));
    else if (m.rc) put(mRes, h('p', { class: 'mberr' }, m.resultado));
    else {
      const bits = m.fc <= 2;
      put(mRes,
        h('p', { class: 'mbok' }, tr('OK · %s ms', m.cuentas.ultima_ms)),
        m.texto ? h('p', {}, m.texto) : null,
        m.vals.length ? h('div', { class: 'mbgrid' }, ...m.vals.map((v, i) => h('div', { class: 'mbcell' + (bits && v ? ' on' : '') },
          h('span', { class: 'a' }, String(m.desde + i)), h('span', { class: 'v' }, bits ? String(v) : String(fmtVal(v)))))) : null);
    }
    const c = m.cuentas;
    mCounts.textContent = tr('%s pedidos · %s respuestas · %s sin respuesta · %s CRC · %s excepciones · promedio %s ms, máximo %s ms',
      c.pedidos, c.respuestas, c.sin_respuesta, c.crc, c.excepciones, c.promedio_ms, c.maxima_ms);
    const t = m.herramienta;
    put(mTool,
      t.activa ? h('p', { class: 'muted' }, tr('Herramienta en curso: %s de %s', t.pos, t.total)) : null,
      (t.unidades.length || t.velocidades.length) ? h('div', { class: 'btns' },
        ...t.unidades.map(([u, k]) => h('button', { class: 'btn small', onclick: () => { mUnit.value = u; } }, `U${u}${k === 2 ? ' (exc)' : k === 3 ? ' (CRC)' : ''}`)),
        ...t.velocidades.map((v, i) => h('button', { class: 'btn small' + (i < t.limpias ? ' pri' : ''), onclick: () => { const [b, f] = v.split(' '); send('baud=' + b, 'formato=' + f); } }, v))) : null,
      t.velocidades.length > 1 ? h('p', { class: 'note small' }, t.limpias >= 1 && new Set(t.velocidades.slice(0, t.limpias).map(v => v.split(' ')[1])).size === 1
        ? tr('Lo marcado contestó sin errores de línea: ése es el formato. Los otros contestaron con bytes de paridad o de parada malos.')
        : tr('La velocidad es segura; el formato no salió claro (contestaron varios sin errores, o todos con errores). El manual del equipo dice cuál (el estándar pide 8E1).')) : null);
  }

  function drawGateway() {
    const g = st.pasarela;
    if (!g) return;
    gwOn.checked = g.on;
    setv(gwPort, g.puerto);
    gwInfo.className = g.error ? 'mberr small' : 'muted small';
    gwInfo.textContent = g.error || (!g.on ? tr('Apagada.') : tr('Cliente: %s · %s pedidos, %s contestados, %s sin respuesta',
      g.cliente || tr('ninguno'), g.pedidos, g.ok, g.fallas));
  }

  function drawSlave() {
    const e = st.esclavo, c = e.cuentas;
    sInfo.textContent = (st.conexion.tr === 'tcp' ? tr('Modbus TCP en el puerto %s · cliente: %s', st.conexion.tport, (st.pasarela && st.pasarela.cliente) || tr('ninguno')) + ' · ' : '') + tr('%s tramas oídas · %s para esta unidad · %s para otras · %s broadcast · %s CRC malos · %s excepciones',
      c.oidas, c.propias, c.otras, c.broadcast, c.crc, c.excepciones) + (e.ultimo ? ' · ' + tr('Último pedido:') + ' ' + e.ultimo : '');
  }

  function drawMonitor() {
    const m = st.monitor;
    monInfo.textContent = tr('%s tramas · %s bytes · %s con CRC malo · %s bytes de ruido · %s bytes con error de línea', m.tramas, m.bytes, m.crc, m.ruido,
      (m.paridad || 0) + (m.parada || 0));
    put(monTable, m.unidades.length ? h('table', { class: 'mbt', style: 'min-width:0' },
      h('thead', {}, h('tr', {}, ...[tr('Unidad'), tr('Pedidos'), tr('Respuestas'), tr('Sin respuesta'), tr('Excepciones'), 'CRC', tr('Demora'), tr('Último')].map(t => h('th', {}, t)))),
      h('tbody', {}, ...m.unidades.map(u => h('tr', {},
        ...[u.unidad || 'bc', u.pedidos, u.respuestas, u.sin_respuesta, u.excepciones, u.crc, `${u.promedio_ms} / ${u.maxima_ms} ms`, u.ultimo]
          .map((v, i) => h('td', { class: i === 3 && u.sin_respuesta ? 'mbwarn' : '' }, String(v))))))) : null);
  }

  function draw() {
    if (!st) return;
    const open = st.abierto, want = st.quiere;
    stateLine.className = 'mbstate ' + (st.error ? 'bad' : open ? 'ok' : '');
    stateLine.textContent = st.error ? st.error
      : open ? `${tr(ROLES.find(r => r[0] === st.rol)[1])} · ${st.desc}`
      : want ? tr('abriendo…') : tr('cerrado');
    openBtn.textContent = want ? tr('Cerrar') : tr('Abrir');
    openBtn.className = 'btn ' + (want ? 'red' : 'pri');
    ROLES.forEach(([id], i) => { roleBtn[i].className = 'btn' + (st.rol === id ? ' pri' : ''); });
    const k = st.conexion;
    setv(port, k.puerto); setv(baud, k.baud); setv(fmt, k.formato); setv(wait, k.espera); setv(gap, k.silencio);
    const m = st.maestro.pedido;
    setv(mUnit, m.unidad); setv(mFc, m.fc); setv(mAddr, m.desde); setv(mN, m.cantidad);
    const writes = [5, 6, 8, 15, 16].includes(+m.fc);
    if (document.activeElement !== mVals && writes && m.valores.length && !mVals.value) mVals.value = m.valores.join(' ');
    mLive.checked = st.maestro.vivo;
    setv(mPer, st.maestro.periodo);
    const e = st.esclavo;
    setv(sUnit, e.unidad); setv(sFault, e.falla); setv(sDelay, e.demora);
    sAnim.checked = e.animar;
    const card = st.rol === 'maestro' ? masterCard : st.rol === 'esclavo' ? slaveCard : monCard;
    if (roleBox.firstChild !== card) put(roleBox, card);
    if (st.rol === 'maestro') { drawMaster(); drawGateway(); }
    else if (st.rol === 'esclavo') drawSlave();
    else drawMonitor();
  }

  async function poll() {
    if (hidden()) return;
    try {
      st = await P.api(LIVE + '&key=estado');
      draw();
    } catch {
      stateLine.className = 'mbstate';
      stateLine.textContent = tr('La app Modbus no está abierta en la placa.');
    }
  }

  async function pollTraffic() {
    if (hidden() || paused) return;
    let j;
    try { j = await P.api(LIVE + '&key=trafico'); } catch { return; }
    if (j.seq === lastTraffic) return;
    lastTraffic = j.seq;
    const err = f => (f.fl & (0x02 | 0x04 | 0x08)) || f.nota === 1 || f.nota === 2;
    const rows = j.tramas.filter(f => !errCk.checked || err(f));
    put(trTable, ...rows.map(f => {
      const tx = f.fl & 0x01, cls = (f.fl & 0x0A) ? 'bad' : (f.fl & 0x04) || f.nota === 1 || f.nota === 2 ? 'warn' : tx ? 'tx' : '';
      const t = (f.t / 1000).toFixed(3).replace('.', ',');
      const gapTxt = f.silencio_us ? ` +${(f.silencio_us / 1000).toFixed(1).replace('.', ',')} ms` : '';
      const rt = f.rt && (f.fl & 0x20) ? ' · ' + tr('contestó en %s ms', f.rt) : '';
      return h('div', { class: 'mbfr ' + cls },
        h('div', { class: 'mono' }, `${t}${gapTxt} ${f.fl & 0x40 ? '--' : tx ? 'TX' : 'RX'} ${f.hex}`),
        h('div', { class: 'small' }, f.que + rt));
    }));
  }

  return {
    wake() {
      if (timer) return;
      const loop = () => {
        if (hidden()) { timer = null; return; }
        poll();
        pollTraffic();
        timer = setTimeout(loop, 800);
      };
      loop();
    },
  };
}

P.registerPage({
  id: 'modbus',
  name: tr('Modbus'),
  icon: '⇄',
  render(main) {
    css();
    let files = [], sel = null, prof = null, dirty = false, live = null, gone = false;
    const listBox = h('div', { class: 'mbl' });
    const edBox = h('div');
    const fileIn = h('input', { type: 'file', accept: '.csv,text/csv,.yaml,.yml', style: 'display:none', onchange: upload });

    async function loadList() {
      try {
        await P.fsMkdir('/modbus').catch(() => {});
        await P.fsMkdir(DIR).catch(() => {});
        files = (await P.fsList(DIR)).filter(f => !f.dir && f.name.endsWith('.csv') && !f.name.startsWith('.'))
          .map(f => f.name).sort();
      } catch (e) { P.toast(e.message, true); }
      drawList();
    }

    function drawList() {
      put(listBox,
        h('div', { class: 'card pad' },
          h('h2', { style: 'margin-top:0' }, tr('Perfiles')),
          files.length ? null : h('p', { class: 'note' }, tr('No hay perfiles todavía. La app escribe cuatro de ejemplo la primera vez que se abre.')),
          ...files.map(f => h('button', { class: 'mbi' + (f === sel ? ' on' : ''), onclick: () => choose(f) }, h('div', { class: 'mbn' }, f))),
          h('div', { class: 'btns', style: 'margin-top:12px' },
            h('button', { class: 'btn pri', onclick: create }, tr('Nuevo')),
            h('button', { class: 'btn', onclick: () => fileIn.click() }, tr('Subir .csv o importar YAML'))),
          fileIn,
          h('p', { class: 'note small' }, tr('Un perfil dice qué tiene un equipo Modbus y dónde. En Maestro la app lo lee entero; en Esclavo hace de ese equipo.')),
          h('p', { class: 'note small' }, tr('Importar: un YAML de Home Assistant (modbus:) o de ESPHome (modbus_controller) se vuelve un perfil con sus registros, tipos, escalas y unidades.'))));
    }

    async function choose(f) {
      if (dirty && !confirm(tr('Hay cambios sin guardar. ¿Descartarlos?'))) return;
      sel = f;
      dirty = false;
      try {
        const t = await P.fsText(`${DIR}/${f}`);
        prof = parse(t || '');
        if (prof.bad) P.toast(tr('%s líneas sin entender', prof.bad), true);
      } catch (e) { P.toast(e.message, true); prof = null; }
      drawList();
      drawEditor();
    }

    function mark() { dirty = true; }

    function field(label, key, attrs = {}) {
      return h('label', {}, label, h('input', { value: prof[key] ?? '', ...attrs, oninput: e => { prof[key] = e.target.value.trim(); mark(); } }));
    }

    function cell(pt, key, kind) {
      if (kind === 'sel') {
        const opts = key === 'tabla' ? TABLES : key === 'tipo' ? TYPES : key === 'acceso' ? ['r', 'rw'] : SIMS;
        return h('td', {}, h('select', { onchange: e => { pt[key] = e.target.value; mark(); } },
          ...opts.map(o => h('option', { value: o, selected: (pt[key] || (key === 'acceso' ? 'r' : '')) === o ? '' : null }, o || '—'))));
      }
      return h('td', {}, h('input', { class: kind || '', value: pt[key] ?? '', oninput: e => { pt[key] = e.target.value.trim(); mark(); } }));
    }

    function drawEditor() {
      if (!prof) { put(edBox, h('div', { class: 'card pad' }, h('p', { class: 'note' }, tr('Elegí un perfil de la lista, o hacé uno nuevo.')))); return; }
      const rows = prof.pts.map((pt, i) => h('tr', {},
        cell(pt, 'tabla', 'sel'), cell(pt, 'direccion', 'n'), cell(pt, 'nombre', 'w'), cell(pt, 'tipo', 'sel'), cell(pt, 'escala', 'n'),
        cell(pt, 'unidad', 'n'), cell(pt, 'acceso', 'sel'), cell(pt, 'min', 'n'), cell(pt, 'max', 'n'), cell(pt, 'simulacion', 'sel'),
        cell(pt, 'valor', 'n'),
        h('td', { class: 'v e', id: 'mbv' + i }, '—'),
        h('td', {}, h('button', { class: 'btn small', title: tr('Quitar'), onclick: () => { prof.pts.splice(i, 1); mark(); drawEditor(); } }, '✕'))));
      put(edBox, h('div', { class: 'card pad' },
        h('h2', { style: 'margin-top:0' }, prof.nombre || sel),
        h('div', { class: 'muted small', style: 'margin-bottom:10px' }, `${DIR}/${sel}`),
        h('div', { class: 'mbh' },
          field(tr('Nombre'), 'nombre'), field(tr('Nota'), 'nota'),
          field(tr('Esclavo (unidad)'), 'esclavo', { inputmode: 'numeric' }),
          field(tr('Velocidad'), 'velocidad', { placeholder: '9600', inputmode: 'numeric' }),
          field(tr('Formato'), 'formato', { placeholder: '8N1' }),
          field(tr('Leerlo cada (ms)'), 'periodo', { inputmode: 'numeric' })),
        h('div', { class: 'mbscroll' }, h('table', { class: 'mbt' },
          h('thead', {}, h('tr', {}, ...[tr('Tabla'), tr('Dirección'), tr('Nombre'), tr('Tipo'), tr('Escala'), tr('Unidad'), tr('Acceso'),
            tr('Mín'), tr('Máx'), tr('Simulación'), tr('Inicial'), tr('En la placa'), ''].map(t => h('th', {}, t)))),
          h('tbody', {}, ...rows))),
        h('div', { class: 'btns', style: 'margin-top:12px' },
          h('button', { class: 'btn', onclick: addRow }, tr('Agregar punto')),
          h('button', { class: 'btn pri', onclick: save }, tr('Guardar')),
          h('button', { class: 'btn', onclick: useIt }, tr('Usar en la placa')),
          h('button', { class: 'btn', onclick: download }, tr('Descargar')),
          h('button', { class: 'btn', onclick: duplicate }, tr('Duplicar')),
          h('button', { class: 'btn red', onclick: remove }, tr('Borrar'))),
        h('p', { class: 'note small' }, tr('Tablas: coil, di (entradas), hr (holding), ir (input). Tipos de 32 bits: el registro alto primero; los "cdab", el bajo primero. Valor = crudo × escala. La simulación es para cuando la app hace de esclavo: se mueve entre mín y máx (contador: sube máx por hora). La columna "En la placa" muestra lo que la app lee o hace ahora, si está abierta con este perfil.'))));
    }

    function addRow() {
      const last = prof.pts[prof.pts.length - 1];
      const words = last && /32/.test(last.tipo) ? 2 : 1;
      prof.pts.push({ tabla: last ? last.tabla : 'hr', direccion: last ? String((parseInt(last.direccion) || 0) + words) : '0',
        nombre: tr('Valor %s', prof.pts.length + 1), tipo: last && last.tabla !== 'coil' && last.tabla !== 'di' ? 'u16' : (last ? 'bit' : 'u16'),
        escala: '1', unidad: '', acceso: 'r', min: '', max: '', simulacion: '', valor: '' });
      mark();
      drawEditor();
    }

    async function save() {
      if (!prof) return;
      try {
        await P.fsPut(`${DIR}/${sel}`, serialize(prof));
        dirty = false;
        P.toast(tr('Guardado: la app lo vuelve a leer sola'));
      } catch (e) { P.toast(e.message, true); }
    }

    async function useIt() {
      if (dirty) await save();
      try {
        await P.openApp(APP);
        await new Promise(r => setTimeout(r, 1200));
        await P.api(LIVE, { method: 'POST', body: 'perfil=' + sel });
        P.toast(tr('La placa usa %s', prof.nombre || sel));
      } catch (e) { P.toast(e.message, true); }
    }

    function download() {
      P.saveBlob(new Blob([serialize(prof)], { type: 'text/csv' }), sel);
    }

    async function newFile(base, p) {
      let name = slug(base), k = 2;
      while (files.includes(name)) name = slug(base).replace(/\.csv$/, `-${k++}.csv`);
      await P.fsPut(`${DIR}/${name}`, serialize(p));
      await loadList();
      dirty = false;
      await choose(name);
    }

    async function create() {
      const n = prompt(tr('Nombre del perfil nuevo'));
      if (!n) return;
      try { await newFile(n, { nombre: n, nota: '', esclavo: '1', velocidad: '9600', formato: '8N1', periodo: '1000', pts: [] }); }
      catch (e) { P.toast(e.message, true); }
    }

    async function duplicate() {
      const n = prompt(tr('Nombre de la copia'), (prof.nombre || '') + ' 2');
      if (!n) return;
      try { await newFile(n, { ...prof, nombre: n, pts: prof.pts.map(p => ({ ...p })) }); }
      catch (e) { P.toast(e.message, true); }
    }

    async function remove() {
      if (!confirm(tr('¿Borrar %s de la tarjeta? No se puede deshacer.', sel))) return;
      try {
        await P.fsDelete(`${DIR}/${sel}`);
        sel = null; prof = null; dirty = false;
        await loadList();
        drawEditor();
      } catch (e) { P.toast(e.message, true); }
    }

    async function upload(e) {
      const f = e.target.files[0];
      e.target.value = '';
      if (!f) return;
      const text = await f.text();
      const yaml = /\.ya?ml$/i.test(f.name) || /^\s*(modbus|modbus_controller|sensor)\s*:/m.test(text);
      const p = yaml ? importYaml(text, f.name) : parse(text);
      if (!p.pts.length) { P.toast(tr('%s no parece un perfil: no tiene puntos', f.name), true); return; }
      try { await newFile(p.nombre || f.name.replace(/\.csv$/i, ''), p); P.toast(yaml ? tr('Importados %s puntos: revisalos y guardá', p.pts.length) : tr('Subido')); }
      catch (err) { P.toast(err.message, true); }
    }

    /* what the board reads or plays, while this page looks */
    const tick = async () => {
      if (gone) return;
      try {
        const j = await P.api(LIVE + '&key=valores');
        live = j;
        if (prof && j.perfil === sel && !dirty) {
          for (const p of j.pts) {
            const td = document.getElementById('mbv' + p.i);
            if (!td) continue;
            td.textContent = p.e ? '—' : p.v;
            td.className = 'v' + (p.e ? ' e' : '');
          }
        }
      } catch { /* the app is closed or has no profile */ }
      if (!gone) setTimeout(tick, 1000);
    };

    const profBox = h('div', { class: 'mbw' }, listBox, edBox);
    const ctlBox = h('div');
    let view = 'control';
    try { view = localStorage.getItem('mb_view') || 'control'; } catch { /* private window */ }
    const tabs = h('div', { class: 'btns', style: 'margin-bottom:14px' });
    const ctl = control(ctlBox, () => gone || view !== 'control');
    function show(v) {
      view = v;
      try { localStorage.setItem('mb_view', v); } catch { /* fine */ }
      profBox.style.display = v === 'perfiles' ? '' : 'none';
      ctlBox.style.display = v === 'control' ? '' : 'none';
      put(tabs, ...[['control', tr('Control')], ['perfiles', tr('Perfiles')]].map(([id, t]) =>
        h('button', { class: 'btn' + (id === v ? ' pri' : ''), onclick: () => show(id) }, t)));
      if (v === 'control') ctl.wake();
    }
    put(main, h('h1', {}, tr('Modbus')), tabs, ctlBox, profBox);
    show(view);
    drawList();
    drawEditor();
    loadList();
    tick();
    return () => { gone = true; };
  },
});
