/* The Infrarrojo app's page in the board's portal (docs/PORTAL-PAGES.md):
 * the devices and their buttons, sending from the browser, learning (the
 * board listens and the captures show up here), an air conditioner's state,
 * and importing or exporting codes.
 *
 * The devices are /ir/<name>.json, the same files the app reads and writes
 * (their format is at the top of apps/infrarrojo/main/ir_store.c). Sending
 * and listening go through two files the app follows while it is open
 * (ir_portal.c): the page writes /data/ir_pedido.json with an id, the app
 * does it, deletes it and answers in /data/ir_estado.json with that id.
 *
 * SmartIR's library is read from the card's pack, infrarrojo_p4.pak, with
 * Range requests: the index once, and the JSON of the air conditioner that
 * is open (its modes, fans and temperatures). Codes pasted or imported are
 * converted here: Broadlink base64 or hex, Pronto, Tuya, or a raw list. */
const P = window.P4OS;
const APP_ID = 'aos.infrarrojo';
const DIR = '/ir';
const REQ = '/data/ir_pedido.json';
const STATE = '/data/ir_estado.json';
const PACK = '/apps/infrarrojo_p4.pak';

if (!document.getElementById('css-infrarrojo')) {
  const st = document.createElement('style');
  st.id = 'css-infrarrojo';
  st.textContent = `
.irkeys { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
.irkeys .btn { min-width: 64px; }
.irkeys .btn.raw { border-style: dashed; }
.irdev h3 { margin: 0; font-size: 17px; }
.irdev .sub { color: var(--dim); font-size: 13px; }
.irtab { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 14px; }
.irtab td { padding: 4px 4px; border-top: 1px solid var(--line); vertical-align: middle; }
.irtab input, .irtab select { width: 100%; box-sizing: border-box; }
.irtab .code { color: var(--dim); font-family: ui-monospace, monospace; font-size: 12px; }
.irtab .x { width: 1%; white-space: nowrap; }
.ircap { border-top: 1px solid var(--line); padding: 10px 0; }
.ircap .t { font-weight: 600; }
.ircap .d { color: var(--dim); font-size: 13px; }
.irwave { width: 100%; height: 46px; display: block; margin: 6px 0; }
.irac { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin-top: 10px; }
.irac .tset { display: inline-flex; gap: 10px; align-items: center; white-space: nowrap; }
.irac .temp { font-size: 34px; font-weight: 650; font-variant-numeric: tabular-nums; min-width: 90px; text-align: center; }
.irgrid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; }
.irgrid label { display: flex; flex-direction: column; gap: 4px; font-size: 13px; color: var(--dim); }
.irdot { display: inline-block; width: 10px; height: 10px; border-radius: 50%; background: var(--line); margin-right: 6px; }
.irdot.on { background: var(--red); }
@media (max-width: 560px) {
  .irtab tr { display: grid; grid-template-columns: 1fr 1fr; gap: 4px; padding: 6px 0; border-top: 1px solid var(--line); }
  .irtab td { border: 0; }
  .irtab td.code, .irtab td.x { grid-column: 1 / -1; }
}
textarea.irpaste { width: 100%; box-sizing: border-box; font-family: ui-monospace, monospace; font-size: 12px; }
`;
  document.head.append(st);
}

/* --------------------------------------------------------------- codes */

const PROTOS = [
  ['NEC', 'NEC'], ['NECext', 'NEC extendido'], ['NEC32', 'NEC 32 bits'], ['Samsung', 'Samsung'],
  ['Sony12', 'Sony SIRC 12'], ['Sony15', 'Sony SIRC 15'], ['Sony20', 'Sony SIRC 20'], ['JVC', 'JVC'],
  ['Panasonic', 'Panasonic'], ['Kaseikyo', 'Kaseikyo'], ['RC5', 'Philips RC5'], ['RC6', 'Philips RC6'],
];
const ROLES = [
  ['', 'Otro botón'], ['power', 'Encender/apagar'], ['power_on', 'Encender'], ['power_off', 'Apagar'],
  ['input', 'Fuente'], ['mute', 'Silencio'], ['vol_up', 'Volumen +'], ['vol_down', 'Volumen −'],
  ['ch_up', 'Canal +'], ['ch_down', 'Canal −'], ['up', 'Arriba'], ['down', 'Abajo'], ['left', 'Izquierda'],
  ['right', 'Derecha'], ['ok', 'OK'], ['back', 'Volver'], ['home', 'Inicio'], ['menu', 'Menú'], ['info', 'Info'],
  ['guide', 'Guía'], ['play', 'Reproducir'], ['pause', 'Pausa'], ['play_pause', 'Reproducir/pausa'],
  ['stop', 'Detener'], ['rew', 'Retroceder'], ['ffwd', 'Adelantar'], ['prev', 'Anterior'], ['next', 'Siguiente'],
  ['rec', 'Grabar'], ['red', 'Rojo'], ['green', 'Verde'], ['yellow', 'Amarillo'], ['blue', 'Azul'],
  ...'1234567890'.split('').map(d => ['d' + d, d]),
];
const KINDS = [['tv', 'Tele'], ['audio', 'Audio'], ['ac', 'Aire'], ['fan', 'Ventilador'], ['light', 'Luz'], ['other', 'Otro']];
const WORDS = {
  cool: 'Frío', heat: 'Calor', dry: 'Seco', fan_only: 'Ventilación', auto: 'Auto', heat_cool: 'Frío/calor',
  low: 'Bajo', lowest: 'Mínimo', mid: 'Medio', medium: 'Medio', middle: 'Medio', high: 'Alto', highest: 'Máximo',
  maximum: 'Máximo', quiet: 'Silencioso', silent: 'Silencioso', turbo: 'Turbo', boost: 'Turbo', eco: 'Eco',
  sleep: 'Noche', none: 'Ninguno', normal: 'Normal', on: 'Sí', off: 'No', swing: 'Oscila', fixed: 'Fijo',
  top: 'Arriba', bottom: 'Abajo', up: 'Arriba', down: 'Abajo',
};
const word = k => WORDS[String(k).toLowerCase()] || k;
const hex = (v, w = 2) => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(w, '0');
const codeText = b => b.proto ? `${b.proto}  dir ${hex(b.addr)}  cmd ${hex(b.cmd)}`
  : b.raw ? `crudo · ${b.raw.length} duraciones · ${Math.round((b.freq || 38000) / 1000)} kHz` : 'sin código';

const b64bytes = s => {
  s = s.trim().replace(/\s+/g, '');
  s += '='.repeat((4 - s.length % 4) % 4);
  const bin = atob(s);
  return Uint8Array.from(bin, c => c.charCodeAt(0));
};
function broadlink(bytes) {
  if (bytes.length < 4 || bytes[0] !== 0x26) throw new Error('no es un código IR de Broadlink');
  const len = bytes[2] | bytes[3] << 8, p = bytes.subarray(4, 4 + len), out = [];
  for (let i = 0; i < p.length;) {
    let v = p[i++];
    if (v === 0) { if (i + 1 >= p.length) break; v = p[i] << 8 | p[i + 1]; i += 2; }
    out.push(Math.round(v * 8192 / 269));
  }
  return out;
}
function tuya(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length;) {
    const h = bytes[i++];
    let len = h >> 5, dist = h & 31;
    if (!len) { for (let k = 0; k <= dist; k++) out.push(bytes[i++]); continue; }
    if (len === 7) len += bytes[i++];
    len += 2;
    dist = (dist << 8 | bytes[i++]) + 1;
    for (let k = 0; k < len; k++) out.push(out[out.length - dist]);
  }
  const us = [];
  for (let k = 0; k + 1 < out.length; k += 2) us.push(out[k] | out[k + 1] << 8);
  return us;
}
function pronto(text) {
  const w = text.trim().split(/[\s,]+/).map(x => parseInt(x, 16));
  if (w[0] !== 0 || w.length !== 4 + 2 * (w[2] + w[3])) throw new Error('Pronto mal formado');
  const period = w[1] * 0.241246;
  return { raw: w.slice(4).map(x => Math.round(x * period)), freq: Math.round(1e6 / period) };
}
/* any code as { raw, freq }, guessing its encoding when none is given */
function toRaw(code, enc) {
  const t = String(code).trim();
  const tries = enc ? [enc] : [];
  if (/^\[|^-?\d+([\s,]+-?\d+)+$/.test(t)) tries.push('Raw');
  if (/^0000[\s,]/.test(t)) tries.push('Pronto');
  tries.push('Base64', 'Tuya', 'Hex', 'Raw', 'Pronto');
  for (const e of tries) {
    try {
      let raw, freq = 38000;
      if (e === 'Raw') raw = (t.startsWith('[') ? JSON.parse(t) : t.split(/[\s,]+/)).map(v => Math.abs(Math.round(+v)));
      else if (e === 'Pronto') ({ raw, freq } = pronto(t));
      else if (e === 'Base64') raw = broadlink(b64bytes(t));
      else if (e === 'Hex') raw = broadlink(Uint8Array.from(t.replace(/\s/g, '').match(/../g).map(x => parseInt(x, 16))));
      else if (e === 'Tuya') raw = tuya(b64bytes(t));
      if (raw && raw.length >= 4 && raw.every(v => v > 0 && v < 1e6)) return { raw, freq };
    } catch (_) { /* the next one */ }
  }
  throw new Error('no reconozco el formato del código');
}

/* ----------------------------------------------------------- the pack */

let packIdx = null;
async function range(from, to) {
  const r = await fetch(P.fsUrl(PACK), { headers: { Range: `bytes=${from}-${to}` } });
  if (!r.ok) throw new Error('sin la base de códigos');
  return new DataView(await r.arrayBuffer());
}
async function packIndex() {
  if (packIdx) return packIdx;
  const h = await range(0, 15);
  if (h.getUint32(0, true) !== 0x31505249) throw new Error('la base de códigos no es válida');
  const n = h.getUint32(4, true), so = h.getUint32(8, true), sl = h.getUint32(12, true);
  const idx = await range(16, 16 + 16 * n - 1), strs = await range(so, so + sl - 1);
  const dec = new TextDecoder();
  const str = o => { let e = o; while (e < sl && strs.getUint8(e)) e++; return dec.decode(new Uint8Array(strs.buffer, strs.byteOffset + o, e - o)); };
  const list = [];
  for (let i = 0; i < n; i++) {
    const at = 16 * i;
    list.push({ id: idx.getUint16(at, true), cls: idx.getUint8(at + 2), brand: str(idx.getUint32(at + 4, true)),
      models: str(idx.getUint32(at + 8, true)), blob: idx.getUint32(at + 12, true) });
  }
  return (packIdx = list);
}
async function packJson(id) {
  const list = await packIndex();
  const d = list.find(x => x.id === id);
  if (!d) throw new Error('ese aire no está en la base');
  const jl = (await range(d.blob, d.blob + 3)).getUint32(0, true);
  const j = await range(d.blob + 4, d.blob + 4 + jl - 1);
  return { dev: d, js: JSON.parse(new TextDecoder().decode(new Uint8Array(j.buffer, j.byteOffset, jl))) };
}

/* ------------------------------------------------------- the requests */

let nextId = Date.now() % 100000;
let queue = Promise.resolve();
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function readState() {
  try { const t = await P.fsText(STATE); return t ? JSON.parse(t) : null; } catch (_) { return null; }
}
/* one at a time: a second request written before the app read the first
 * would replace it */
function ask(op, extra = {}) {
  const run = async () => {
    const id = ++nextId;
    await P.fsMkdir('/data').catch(() => {});
    await P.fsPut(REQ, JSON.stringify({ id, op, ...extra }));
    for (let t = 0; t < 12; t++) {
      await sleep(300);
      const s = await readState();
      if (s && s.id === id) {
        if (!s.ok) throw new Error(s.msg || 'no se pudo');
        return s;
      }
    }
    throw new Error('la app no contesta: ¿está abierta en la placa?');
  };
  const p = queue.then(run, run);
  queue = p.catch(() => {});
  return p;
}

/* ----------------------------------------------------------- drawing */

function wave(raw) {
  const c = document.createElement('canvas');
  c.className = 'irwave';
  requestAnimationFrame(() => {
    const W = c.clientWidth || 600, H = 46, dpr = window.devicePixelRatio || 1;
    c.width = W * dpr; c.height = H * dpr;
    const g = c.getContext('2d');
    g.scale(dpr, dpr);
    const gaps = raw.filter((v, i) => i & 1 && v >= 30000).length;
    const total = raw.reduce((s, v, i) => s + (i & 1 && v >= 30000 ? 0 : v), 0) || 1;
    const k = (W - gaps * 14) / total;
    let x = 0;
    g.fillStyle = getComputedStyle(document.body).getPropertyValue('--red') || '#ff453a';
    raw.forEach((v, i) => {
      if (i & 1 && v >= 30000) { x += 14; return; }
      if (!(i & 1)) g.fillRect(x, 6, Math.max(1, v * k), H - 8);
      x += v * k;
    });
  });
  return c;
}

const select = (opts, val) => {
  const s = P.h('select', {}, ...opts.map(([v, t]) => P.h('option', { value: v }, t)));
  s.value = val ?? '';
  return s;
};

P.registerPage({
  id: 'infrarrojo',
  name: 'Infrarrojo',
  icon: '◉',
  render(main) {
    const { h, put } = P;
    let gone = false, listening = false, lastSeq = 0, timer = null, keepAlive = null;
    let devices = [];
    const status = h('div', { class: 'card pad' }, h('span', { class: 'muted' }, 'Cargando…'));
    const devBox = h('div', {});
    const capBox = h('div', {});
    const learnBtn = h('button', { class: 'btn pri' }, 'Escuchar');

    const fail = e => P.toast(e.message, true);
    const openApp = () => P.openApp(APP_ID).then(() => P.toast('Abierta en la placa')).catch(fail);

    async function loadDevices() {
      const files = (await P.fsList(DIR)).filter(f => !f.dir && f.name.endsWith('.json') && !f.name.startsWith('.'));
      const out = [];
      for (const f of files) {
        try { out.push({ file: f.name, d: JSON.parse(await P.fsText(`${DIR}/${f.name}`)) }); }
        catch (e) { out.push({ file: f.name, err: e.message }); }
      }
      out.sort((a, b) => String(a.d?.name || a.file).localeCompare(String(b.d?.name || b.file)));
      return out;
    }

    async function save(dev) {
      await P.fsMkdir(DIR).catch(() => {});
      await P.fsPut(`${DIR}/${dev.file}`, JSON.stringify(dev.d, null, 2) + '\n');
    }

    const send = (dev, i, btn) => {
      btn?.classList.add('pri');
      ask('send', { file: dev.file, button: i }).then(() => P.toast('Mandado: ' + dev.d.buttons[i].name)).catch(fail)
        .finally(() => btn?.classList.remove('pri'));
    };

    function editor(dev, box) {
      const d = dev.d;
      const rows = (d.buttons || []).map((b, i) => {
        const name = h('input', { value: b.name || '' });
        const role = select(ROLES, b.role || '');
        name.onchange = () => { b.name = name.value; };
        role.onchange = () => { b.role = role.value; if (!name.value) { name.value = ROLES.find(r => r[0] === role.value)[1]; b.name = name.value; } };
        return h('tr', {},
          h('td', {}, name), h('td', {}, role), h('td', { class: 'code' }, codeText(b)),
          h('td', { class: 'x' },
            h('button', { class: 'btn', title: 'Mandar', onclick: () => send(dev, i) }, '▶'), ' ',
            h('button', { class: 'btn', title: 'Subir', onclick: () => { if (i) { d.buttons.splice(i - 1, 0, d.buttons.splice(i, 1)[0]); editor(dev, box); } } }, '↑'), ' ',
            h('button', { class: 'btn red', title: 'Borrar', onclick: () => { d.buttons.splice(i, 1); editor(dev, box); } }, '✕')));
      });
      const nm = h('input', { value: d.name || '' });
      const kind = select(KINDS, d.kind || 'other');
      put(box,
        h('div', { class: 'irgrid' },
          h('label', {}, 'Nombre', nm),
          d.smartir ? null : h('label', {}, 'Tipo', kind)),
        rows.length ? h('table', { class: 'irtab' }, h('tbody', {}, rows)) : h('p', { class: 'muted' }, 'Sin botones.'),
        h('div', { class: 'btns', style: 'margin-top:10px' },
          h('button', { class: 'btn pri', onclick: async () => {
            d.name = nm.value.trim() || d.name;
            if (!d.smartir) d.kind = kind.value;
            try { await save(dev); P.toast('Guardado'); refresh(); } catch (e) { fail(e); }
          } }, 'Guardar'),
          h('button', { class: 'btn', onclick: () => refresh() }, 'Descartar'),
          h('button', { class: 'btn', onclick: () => P.saveBlob(new Blob([JSON.stringify(d, null, 2)], { type: 'application/json' }), dev.file) }, 'Bajar el JSON'),
          h('button', { class: 'btn red', onclick: async () => {
            if (!confirm(`¿Borrar ${d.name} y sus botones?`)) return;
            try { await P.fsDelete(`${DIR}/${dev.file}`); refresh(); } catch (e) { fail(e); }
          } }, 'Borrar aparato')));
    }

    async function acPanel(dev, box) {
      const d = dev.d;
      d.state = d.state || { on: false, mode: '', levels: [], temp: 24 };
      let pj;
      try { pj = await packJson(d.smartir); } catch (e) { put(box, h('p', { class: 'note' }, e.message)); return; }
      const cmds = pj.js.commands || {};
      const modes = Object.keys(cmds).filter(k => k !== 'off' && k !== 'on');
      const draw = () => {
        const st = d.state;
        if (!modes.includes(st.mode)) st.mode = modes[0];
        /* the levels down to the temperatures, fixing what the file lacks */
        let node = cmds[st.mode];
        const selects = [];
        const lists = [['presetModes', 'Modo especial'], ['fanModes', 'Ventilador'], ['swingModes', 'Oscilación']].filter(([k]) => pj.js[k]);
        const used = new Set();
        for (let l = 0; node && typeof node === 'object' && !Array.isArray(node) && l < 4; l++) {
          const keys = Object.keys(node);
          if (!keys.length || !isNaN(parseFloat(keys[0]))) break;
          let label = 'Opción';
          for (const [k, t] of lists) if (!used.has(k) && pj.js[k].includes(keys[0])) { label = t; used.add(k); break; }
          if (label === 'Opción') for (const [k, t] of lists) if (!used.has(k)) { label = t; used.add(k); break; }
          if (!keys.includes(st.levels[l])) st.levels[l] = keys[0];
          const s = select(keys.map(k => [k, word(k)]), st.levels[l]);
          const lv = l;
          s.onchange = () => { st.levels[lv] = s.value; st.on = true; apply(); };
          selects.push(h('label', {}, label, s));
          node = node[st.levels[l]];
        }
        st.levels.length = selects.length;
        const temps = node && typeof node === 'object' ? Object.keys(node).map(parseFloat).filter(v => !isNaN(v)).sort((a, b) => a - b) : [];
        if (temps.length && !temps.includes(st.temp)) st.temp = temps.reduce((a, b) => Math.abs(b - st.temp) < Math.abs(a - st.temp) ? b : a);
        const step = dir => {
          const i = temps.indexOf(st.temp) + dir;
          if (i >= 0 && i < temps.length) { st.temp = temps[i]; st.on = true; apply(); }
        };
        const mode = select(modes.map(m => [m, word(m)]), st.mode);
        mode.onchange = () => { st.mode = mode.value; st.on = true; apply(); };
        put(box,
          h('div', { class: 'irac' },
            h('button', { class: 'btn' + (st.on ? ' pri' : ''), onclick: () => { st.on = !st.on; apply(); } }, st.on ? '⏻ Encendido' : '⏻ Apagado'),
            temps.length ? h('span', { class: 'tset' },
              h('button', { class: 'btn', onclick: () => step(-1) }, '−'),
              h('span', { class: 'temp' }, String(st.temp).replace('.', ',') + '°'),
              h('button', { class: 'btn', onclick: () => step(1) }, '+')) : null),
          h('div', { class: 'irgrid', style: 'margin-top:10px' }, h('label', {}, 'Modo', mode), ...selects),
          h('p', { class: 'note' }, `${pj.dev.brand} · ${pj.dev.models} · código ${d.smartir} de SmartIR (licencia MIT). Cada cambio se guarda y se manda con el estado entero.`));
      };
      let pending = null;
      const apply = () => {
        draw();
        clearTimeout(pending);
        pending = setTimeout(async () => {
          try { await save(dev); await ask('ac', { file: dev.file }); P.toast(d.state.on ? 'Mandado' : 'Apagado'); }
          catch (e) { fail(e); }
        }, 500);
      };
      draw();
    }

    function devCard(dev) {
      if (dev.err) return h('div', { class: 'card pad irdev' }, h('h3', {}, dev.file), h('p', { class: 'note' }, 'No se pudo leer: ' + dev.err));
      const d = dev.d;
      const body = h('div', {});
      const keys = h('div', { class: 'irkeys' }, (d.buttons || []).map((b, i) => {
        const k = h('button', { class: 'btn' + (b.proto ? '' : ' raw'), title: codeText(b) }, b.name || '?');
        k.onclick = () => send(dev, i, k);
        return k;
      }));
      const kind = KINDS.find(k => k[0] === d.kind)?.[1] || 'Otro';
      const editBtn = h('button', { class: 'btn' }, 'Editar');
      editBtn.onclick = () => { editBtn.remove(); editor(dev, body); };
      const card = h('div', { class: 'card pad irdev' },
        h('div', { class: 'row' },
          h('div', { class: 'grow' }, h('h3', {}, d.name || dev.file),
            h('div', { class: 'sub' }, `${kind} · ${d.smartir ? 'SmartIR ' + d.smartir : (d.buttons || []).length + ' botones'} · ir/${dev.file}`)),
          editBtn),
        d.smartir ? null : keys, body);
      if (d.smartir) {
        const ac = h('div', {});
        card.insertBefore(ac, body);
        acPanel(dev, ac);
        if ((d.buttons || []).length) card.insertBefore(keys, body);
      }
      return card;
    }

    async function refresh() {
      try { devices = await loadDevices(); } catch (e) { fail(e); devices = []; }
      if (gone) return;
      put(devBox, devices.length ? devices.map(devCard)
        : h('div', { class: 'card pad' }, h('p', { class: 'muted' }, 'Todavía no hay aparatos. Creá uno abajo, importalo, o agregalo desde Códigos en la placa.')));
    }

    /* learning */
    function drawCaptures(st) {
      const caps = st?.captures || [];
      put(capBox, caps.length ? caps.map(c => {
        const nm = h('input', { placeholder: 'Nombre del botón' });
        const to = select([...devices.filter(x => x.d && !x.err).map(x => [x.file, x.d.name]), ['', 'Aparato nuevo…']], devices[0]?.file || '');
        return h('div', { class: 'ircap' },
          h('div', { class: 't' }, `#${c.seq} · ${c.text}`),
          h('div', { class: 'd' }, c.desc),
          wave(c.raw || []),
          h('div', { class: 'irgrid' }, h('label', {}, 'Nombre', nm), h('label', {}, 'Guardar en', to)),
          h('div', { class: 'btns', style: 'margin-top:8px' },
            h('button', { class: 'btn pri', onclick: async () => {
              const b = c.proto ? { name: nm.value || 'Botón', proto: c.proto, addr: c.addr, cmd: c.cmd, ...(c.vendor ? { vendor: c.vendor } : {}) }
                : { name: nm.value || 'Botón', freq: c.freq || 38000, raw: c.raw };
              try {
                let dev = devices.find(x => x.file === to.value);
                if (!dev) {
                  const name = prompt('Nombre del aparato nuevo', 'Aparato nuevo');
                  if (!name) return;
                  dev = { file: P.fsSlug(name, 40, 'aparato') + '.json', d: { name, kind: 'tv', buttons: [] } };
                }
                dev.d.buttons = dev.d.buttons || [];
                dev.d.buttons.push(b);
                await save(dev);
                P.toast(`Guardado en ${dev.d.name}`);
                refresh();
              } catch (e) { fail(e); }
            } }, 'Guardar'),
            h('button', { class: 'btn', onclick: () => ask('raw', { freq: c.freq, raw: c.raw }).then(() => P.toast('Mandado')).catch(fail) }, 'Mandar'),
            h('button', { class: 'btn', onclick: () => navigator.clipboard?.writeText(JSON.stringify(c.raw)).then(() => P.toast('Copiado')) }, 'Copiar las duraciones')));
      }) : h('p', { class: 'muted' }, listening ? 'Apuntá el control al receptor y apretá un botón.' : 'Tocá Escuchar y apretá un botón del control.'));
    }

    async function poll() {
      const st = await readState();
      if (gone) return;
      if (st) {
        put(status,
          h('div', {}, h('span', { class: 'irdot' + (st.listening ? ' on' : '') }),
            st.listening ? 'La placa está escuchando' : 'La placa no está escuchando'),
          h('div', { class: 'muted small' }, `Receptor en GPIO${st.rx}, LED en GPIO${st.tx} (se cambian en la app, pestaña Cableado)`),
          h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: openApp }, 'Abrir la app en la placa')));
        const seq = st.captures?.[0]?.seq || 0;
        if (seq !== lastSeq) { lastSeq = seq; drawCaptures(st); }
        if (listening && !st.listening) stopLearn(false);
      } else {
        put(status, h('p', { class: 'muted' }, 'La app todavía no habló: abrila en la placa.'),
          h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: openApp }, 'Abrir la app en la placa')));
      }
    }

    function stopLearn(tell = true) {
      listening = false;
      learnBtn.textContent = 'Escuchar';
      learnBtn.classList.add('pri');
      clearInterval(keepAlive);
      if (tell) ask('stop').catch(() => {});
    }
    learnBtn.onclick = async () => {
      if (listening) { stopLearn(); return; }
      try {
        await ask('learn');
        listening = true;
        learnBtn.textContent = 'Parar';
        learnBtn.classList.remove('pri');
        /* the app listens 30 s per request: keep asking while this is open */
        keepAlive = setInterval(() => ask('learn').catch(() => {}), 20000);
        drawCaptures(await readState());
      } catch (e) { fail(e); }
    };

    /* a new device, an import, a code by hand */
    const newName = h('input', { placeholder: 'Nombre (Tele del living)' });
    const newKind = select(KINDS.filter(k => k[0] !== 'ac'), 'tv');
    const create = async () => {
      const name = newName.value.trim();
      if (!name) { P.toast('Falta el nombre', true); return; }
      let file = P.fsSlug(name, 40, 'aparato') + '.json';
      for (let i = 2; devices.some(x => x.file === file); i++) file = `${P.fsSlug(name, 36, 'aparato')}-${i}.json`;
      try { await save({ file, d: { name, kind: newKind.value, buttons: [] } }); newName.value = ''; refresh(); } catch (e) { fail(e); }
    };

    const fileIn = h('input', { type: 'file', accept: '.json,application/json' });
    fileIn.onchange = async () => {
      const f = fileIn.files[0];
      if (!f) return;
      try {
        const j = JSON.parse(await f.text());
        let d;
        if (Array.isArray(j.buttons)) d = j;                          /* one of ours */
        else if (j.commands) {                                         /* SmartIR */
          if (j.operationModes) throw new Error('un aire de SmartIR se agrega desde Códigos en la placa (la base ya lo trae)');
          d = { name: `${j.manufacturer || 'SmartIR'} ${(j.supportedModels || [])[0] || ''}`.trim(), kind: 'tv', buttons: [] };
          const walk = (node, path) => {
            if (typeof node === 'string') {
              try { const { raw, freq } = toRaw(node, j.commandsEncoding); d.buttons.push({ name: path.join(' · ') || '?', freq, raw }); }
              catch (_) { /* skipped */ }
            } else if (Array.isArray(node)) { if (node.length) walk(node[0], path); }
            else if (node && typeof node === 'object') for (const [k, v] of Object.entries(node)) walk(v, [...path, k]);
          };
          walk(j.commands, []);
        } else throw new Error('no es un aparato ni un archivo de SmartIR');
        let file = P.fsSlug(d.name, 40, 'aparato') + '.json';
        for (let i = 2; devices.some(x => x.file === file); i++) file = `${P.fsSlug(d.name, 36, 'aparato')}-${i}.json`;
        await save({ file, d });
        P.toast(`Importado: ${d.name}, ${d.buttons.length} botones`);
        refresh();
      } catch (e) { fail(e); }
      fileIn.value = '';
    };

    const paste = h('textarea', { class: 'irpaste', rows: 3, placeholder: 'Un código: Broadlink en base64, Pronto (0000 006D …), Tuya, o duraciones [9000, 4500, 560, …]' });
    const proto = select(PROTOS, 'NEC');
    const addr = h('input', { placeholder: '04' }), cmd = h('input', { placeholder: '08' });
    const handCode = () => {
      const a = parseInt(addr.value || '0', 16), c = parseInt(cmd.value || '0', 16);
      if (isNaN(a) || isNaN(c)) throw new Error('dirección y comando en hexa');
      return { proto: proto.value, addr: a, cmd: c };
    };

    put(main,
      h('h1', {}, 'Infrarrojo'),
      status,
      h('h2', {}, 'Aparatos'),
      devBox,
      h('h2', {}, 'Aprender'),
      h('div', { class: 'card pad' },
        h('div', { class: 'row' },
          h('div', { class: 'grow note' }, 'La placa escucha con su receptor y cada botón que apretás aparece acá, decodificado.'),
          learnBtn),
        capBox),
      h('h2', {}, 'Agregar'),
      h('div', { class: 'card pad' },
        h('div', { class: 'irgrid' }, h('label', {}, 'Aparato nuevo', newName), h('label', {}, 'Tipo', newKind)),
        h('div', { class: 'btns', style: 'margin-top:10px' }, h('button', { class: 'btn pri', onclick: create }, 'Crear')),
        h('p', { class: 'note' }, 'Importar un aparato (un JSON bajado de acá) o un archivo de SmartIR de televisor, ventilador o luz:'),
        fileIn),
      h('h2', {}, 'Mandar un código'),
      h('div', { class: 'card pad' },
        h('div', { class: 'irgrid' }, h('label', {}, 'Protocolo', proto), h('label', {}, 'Dirección (hexa)', addr), h('label', {}, 'Comando (hexa)', cmd)),
        h('div', { class: 'btns', style: 'margin-top:10px' },
          h('button', { class: 'btn pri', onclick: () => { try { ask('code', handCode()).then(() => P.toast('Mandado')).catch(fail); } catch (e) { fail(e); } } }, 'Mandar')),
        paste,
        h('div', { class: 'btns', style: 'margin-top:10px' },
          h('button', { class: 'btn', onclick: () => {
            try { const { raw, freq } = toRaw(paste.value); ask('raw', { raw, freq }).then(() => P.toast(`Mandado: ${raw.length} duraciones`)).catch(fail); }
            catch (e) { fail(e); }
          } }, 'Mandar el código pegado'))),
      h('p', { class: 'note' }, 'Los códigos de la base son de SmartIR (licencia MIT, © 2019 Vassilis Panos, 2024 Li Tin O\'ve Weedle). El receptor va a 3,3 V y el LED con un transistor: el cableado está en la pestaña Cableado de la app.'));

    refresh();
    poll();
    drawCaptures(null);
    timer = setInterval(poll, 900);
    return () => { gone = true; clearInterval(timer); clearInterval(keepAlive); if (listening) ask('stop').catch(() => {}); };
  },
});
