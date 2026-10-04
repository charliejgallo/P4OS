/* P4OS portal. One page, one section per hash (#pantalla, #wifi, ...), all
 * over the JSON API in aos_portal.c. No framework: it has to fit the
 * firmware and load on a phone. */
'use strict';

const $ = (s, el = document) => el.querySelector(s);
const main = $('#main');
/* replaceChildren without the null/false holes a conditional leaves */
const put = (el, ...kids) => el.replaceChildren(...kids.flat(Infinity).filter(k => k != null && k !== false));

function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k === 'class') el.className = v;
    else if (k === 'html') el.innerHTML = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat()) if (c != null && c !== false) el.append(c.nodeType ? c : document.createTextNode(c));
  return el;
}

async function api(path, opts = {}) {
  const r = await fetch('/api/' + path, opts);
  const t = await r.text();
  let j;
  try { j = JSON.parse(t); } catch { j = t; }
  if (r.status === 401 && j && j.login) loginScreen();
  if (!r.ok) throw new Error((j && j.error) || r.statusText);
  return j;
}
const post = (path, body) => api(path, { method: 'POST', body: body == null ? '' : JSON.stringify(body) });

let toastT;
function toast(msg, bad) {
  const t = $('#toast');
  t.textContent = msg;
  t.style.color = bad ? 'var(--red)' : '';
  t.classList.add('on');
  clearTimeout(toastT);
  toastT = setTimeout(() => t.classList.remove('on'), 2400);
}

const fmtBytes = b => b < 1024 ? b + ' B' : b < 1048576 ? (b / 1024).toFixed(1).replace('.', ',') + ' KB'
  : b < 1073741824 ? (b / 1048576).toFixed(1).replace('.', ',') + ' MB' : (b / 1073741824).toFixed(1).replace('.', ',') + ' GB';
const fmtUptime = s => s < 3600 ? Math.floor(s / 60) + ' min' : Math.floor(s / 3600) + ' h ' + Math.floor(s % 3600 / 60) + ' min';
const row = (label, value, cls) => h('div', { class: 'row' }, h('div', { class: 'grow' }, label), h('div', { class: 'val ' + (cls || '') }, value));

/* ---- header ---- */
let info = {};
async function refreshInfo() {
  try {
    info = await api('info');
    $('#devname').textContent = info.name;
    $('#devsub').textContent = (info.ip || 'sin red') + ' · ' + info.firmware;
    $('#dot').className = 'dot on';
    document.title = info.name + ' · P4OS';
  } catch {
    $('#dot').className = 'dot off';
    $('#devsub').textContent = 'sin conexión';
  }
}

/* ---- Pantalla ---- */
let live = false, liveT, shotBusy = false;

function pagePantalla() {
  const img = h('img', { alt: 'pantalla', draggable: 'false' });
  const busy = h('div', { class: 'busy' });
  const shot = async () => {
    if (shotBusy) return;
    shotBusy = true;
    busy.classList.add('on');
    try {
      const r = await fetch('/api/screen.bmp?t=' + Date.now());
      if (r.ok) {
        const url = URL.createObjectURL(await r.blob());
        img.onload = () => URL.revokeObjectURL(url);
        img.src = url;
      }
    } catch {}
    busy.classList.remove('on');
    shotBusy = false;
  };
  const after = () => { setTimeout(shot, 350); setTimeout(shot, 1100); };
  /* mouse to screen: a click is a tap, a drag is a drag */
  let down = null;
  const pos = e => {
    const r = img.getBoundingClientRect();
    return { x: Math.round((e.clientX - r.left) * img.naturalWidth / r.width),
             y: Math.round((e.clientY - r.top) * img.naturalHeight / r.height), t: Date.now() };
  };
  img.addEventListener('pointerdown', e => { down = pos(e); img.setPointerCapture(e.pointerId); });
  img.addEventListener('pointerup', async e => {
    if (!down) return;
    const up = pos(e), d = Math.hypot(up.x - down.x, up.y - down.y), ms = Math.min(1500, Math.max(80, up.t - down.t));
    const q = d < 12 ? `x=${down.x}&y=${down.y}&ms=${ms}` : `x=${down.x}&y=${down.y}&x2=${up.x}&y2=${up.y}&ms=${Math.max(150, ms)}`;
    down = null;
    try { await api('touch?' + q, { method: 'POST' }); } catch (err) { toast(err.message, true); }
    after();
  });
  const nav = to => async () => { await api('nav?to=' + to, { method: 'POST' }); setTimeout(shot, 500); setTimeout(shot, 1300); };
  const save = () => {
    const c = document.createElement('canvas');
    c.width = img.naturalWidth; c.height = img.naturalHeight;
    c.getContext('2d').drawImage(img, 0, 0);
    c.toBlob(b => { const a = h('a', { href: URL.createObjectURL(b), download: `${info.name || 'p4os'}-${Date.now()}.png` }); a.click(); });
  };
  const liveBtn = h('button', { class: 'btn', onclick: () => setLive(!live) }, 'En vivo');
  const setLive = on => {
    live = on;
    liveBtn.className = 'btn' + (on ? ' pri' : '');
    clearInterval(liveT);
    if (on) liveT = setInterval(() => { if (!document.hidden && location.hash === '#pantalla') shot(); }, 1200);
  };
  const apps = h('select', { onchange: async e => { if (!e.target.value) return; await api('open?id=' + encodeURIComponent(e.target.value), { method: 'POST' }); e.target.value = ''; after(); } },
    h('option', { value: '' }, 'Abrir una app…'));
  api('apps').then(list => list.sort((a, b) => a.name.localeCompare(b.name)).forEach(a => apps.append(h('option', { value: a.id }, a.name)))).catch(() => {});
  const side = h('div', { class: 'side' },
    h('div', { class: 'btns' },
      h('button', { class: 'btn', onclick: nav('home') }, 'Inicio'),
      h('button', { class: 'btn', onclick: nav('back') }, 'Atrás'),
      h('button', { class: 'btn', onclick: nav('rotate') }, 'Girar'),
      liveBtn,
      h('button', { class: 'btn', onclick: save }, 'Guardar PNG')),
    h('div', { style: 'margin-top:12px' }, apps),
    h('p', { class: 'note' }, 'Un clic es un toque; arrastrar es deslizar. Desde el borde de abajo hacia arriba vas al inicio, como en la placa.'),
    h('h2', {}, 'La placa'),
    h('div', { class: 'card', id: 'infocard' }));
  put(main, h('h1', {}, 'Pantalla'), h('div', { class: 'screen-wrap' }, h('div', { class: 'screen' }, img, busy), side));
  const card = $('#infocard');
  const fill = () => put(card, 
    row('Nombre', info.name + '.local'), row('Placa', info.board), row('Firmware', info.firmware),
    row('Dirección', info.ip || 'sin red'), row('Red', info.ssid ? `${info.ssid} (${info.rssi} dBm)` : '—'),
    row('Encendida hace', fmtUptime(info.uptime_s || 0)),
    row('Memoria libre', `${Math.round((info.heap_internal || 0) / 1024)} KB internos · ${fmtBytes(info.heap_psram || 0)} PSRAM`),
    row('microSD', info.sd_total ? `${fmtBytes(info.sd_free)} libres de ${fmtBytes(info.sd_total)}` : 'sin tarjeta'),
    row('Hora', info.time || 'sin hora'));
  refreshInfo().then(fill);
  shot();
  setLive(live);
}

/* ---- Wi-Fi ---- */
async function pageWifi() {
  const st = h('div', { class: 'card' });
  const list = h('div', { class: 'card' }, h('div', { class: 'row muted' }, 'Tocá Buscar redes.'));
  const form = h('div');
  const load = async () => {
    const w = await api('wifi');
    const color = w.state === 'connected' ? 'ok' : w.state === 'failed' ? 'bad' : 'muted';
    const names = { connected: 'Conectada', connecting: 'Conectando…', failed: 'Sin conexión', off: 'Apagado' };
    put(st, row('Estado', names[w.state] || w.state, color), row('Red', w.ssid || '—'),
      row('Dirección', w.ip || '—'), row('Señal', w.state === 'connected' ? w.rssi + ' dBm' : '—'),
      w.has_credentials ? h('div', { class: 'row click', onclick: async () => { if (!confirm('¿Olvidar esta red?')) return; await post('wifi/forget'); toast('Red olvidada'); load(); } },
        h('div', { class: 'grow bad' }, 'Olvidar esta red')) : null);
  };
  const join = (ssid, secure) => {
    const pass = h('input', { type: 'password', placeholder: 'contraseña', autocomplete: 'off' });
    const go = async () => {
      try { await post('wifi', { ssid, pass: pass.value }); toast('Conectando a ' + ssid + '…'); put(form); setTimeout(load, 4000); }
      catch (e) { toast(e.message, true); }
    };
    put(form, h('h2', {}, 'Unirse a ' + ssid), h('div', { class: 'card pad' },
      secure ? h('div', {}, h('label', { class: 'f' }, 'Contraseña'), pass) : h('p', { class: 'muted' }, 'Red abierta.'),
      h('div', { class: 'btns', style: 'margin-top:14px' }, h('button', { class: 'btn pri', onclick: go }, 'Unirse'),
        h('button', { class: 'btn', onclick: () => put(form) }, 'Cancelar'))));
    if (secure) { pass.focus(); pass.onkeydown = e => { if (e.key === 'Enter') go(); }; }
  };
  const scan = async btn => {
    btn.disabled = true;
    put(list, h('div', { class: 'row muted' }, 'Buscando…'));
    try {
      const aps = await api('wifi/scan');
      put(list, ...(aps.length ? aps.map(a => h('div', { class: 'row click', onclick: () => join(a.ssid, a.secure) },
        h('div', { class: 'grow' }, (a.secure ? '🔒 ' : '') + a.ssid), h('div', { class: 'val' }, a.rssi + ' dBm'))) : [h('div', { class: 'row muted' }, 'No se encontraron redes.')]));
    } catch (e) { put(list, h('div', { class: 'row bad' }, e.message)); }
    btn.disabled = false;
  };
  const manual = () => join(prompt('Nombre de la red (SSID)') || '', true);
  const sb = h('button', { class: 'btn pri', onclick: e => scan(e.target) }, 'Buscar redes');
  put(main, h('h1', {}, 'Wi-Fi'), st, form, h('h2', {}, 'Redes'),
    h('div', { class: 'btns', style: 'margin-bottom:10px' }, sb, h('button', { class: 'btn', onclick: manual }, 'Red oculta…')), list,
    h('p', { class: 'note' }, 'La P4 se conecta por el ESP32-C6 de la placa: sólo 2,4 GHz.'));
  load();
}

/* ---- Home Assistant ---- */
async function pageHa() {
  const st = h('div', { class: 'card' });
  const url = h('input', { type: 'text', placeholder: 'http://192.168.1.10:8123' });
  const token = h('input', { type: 'password', placeholder: 'pegá acá el token de larga duración', autocomplete: 'off' });
  const ents = h('div');
  const names = { unconfigured: 'Sin configurar', waiting_net: 'Esperando el Wi-Fi', connecting: 'Conectando…', auth_failed: 'Token rechazado',
    loading: 'Cargando…', ready: 'Conectado', error: 'Sin conexión' };
  const load = async () => {
    const s = await api('ha');
    if (document.activeElement !== url) url.value = s.url || '';
    put(st, row('Estado', names[s.state] || s.state, s.state === 'ready' ? 'ok' : s.state === 'auth_failed' || s.state === 'error' ? 'bad' : 'muted'),
      row('Nombre de la casa', s.location), row('Token', s.has_token ? 'guardado' : 'falta', s.has_token ? 'ok' : 'warn'),
      row('Entidades', s.entities ? `${s.entities} en ${s.areas} ambientes` : '—'),
      s.error && s.state !== 'ready' ? h('div', { class: 'row warn small' }, s.error) : null);
    return s;
  };
  const save = async () => {
    try { await post('ha', { url: url.value.trim(), token: token.value.trim() }); token.value = ''; toast('Guardado, conectando…'); setTimeout(() => { load(); loadEnts(); }, 2500); }
    catch (e) { toast(e.message, true); }
  };
  const search = h('input', { type: 'search', placeholder: 'Buscar entidad…', oninput: () => drawEnts() });
  let all = [];
  const drawEnts = () => {
    const q = search.value.toLowerCase();
    const by = {};
    for (const e of all) {
      if (q && !(e.name + ' ' + e.id).toLowerCase().includes(q)) continue;
      (by[e.area || 'Sin ambiente'] ||= []).push(e);
    }
    const areas = Object.keys(by).sort((a, b) => a === 'Sin ambiente' ? 1 : b === 'Sin ambiente' ? -1 : a.localeCompare(b));
    put(ents, ...areas.map(a => [h('h2', {}, a), h('div', { class: 'card ent' }, by[a].map(e => {
      const star = h('span', { class: 'star' + (e.fav ? ' on' : ''), title: 'Favorito', onclick: async () => {
        e.fav = !e.fav; star.className = 'star' + (e.fav ? ' on' : ''); star.textContent = e.fav ? '★' : '☆';
        await api(`ha/fav?id=${encodeURIComponent(e.id)}&on=${e.fav ? 1 : 0}`, { method: 'POST' }); } }, e.fav ? '★' : '☆');
      return h('div', { class: 'row' }, star, h('div', { class: 'grow' }, e.name, ' ', h('span', { class: 'muted small' }, e.id)),
        h('span', { class: 'pill' }, e.domain), h('div', { class: 'val', style: 'min-width:90px' }, e.state + (e.unit ? ' ' + e.unit : '')));
    }))]).flat());
  };
  const loadEnts = async () => { all = await api('ha/entities'); drawEnts(); };
  put(main, h('h1', {}, 'Home Assistant'), h('div', { class: 'grid2' },
    h('div', {}, st),
    h('div', { class: 'card pad' },
      h('label', { class: 'f', style: 'margin-top:0' }, 'Dirección'), url,
      h('label', { class: 'f' }, 'Token'), token,
      h('div', { class: 'btns', style: 'margin-top:14px' }, h('button', { class: 'btn pri', onclick: save }, 'Guardar y conectar')),
      h('p', { class: 'note', style: 'margin:12px 0 0' }, 'El token se crea en Home Assistant: tu perfil > Seguridad > Tokens de acceso de larga duración. Queda guardado en la placa; no se vuelve a mostrar.'))),
    h('h2', {}, 'Favoritos'), h('p', { class: 'note' }, 'La estrella elige qué aparece en la página de favoritos y en el widget del inicio, en el orden en que las marcás.'),
    search, ents);
  await load();
  loadEnts();
}

/* ---- Archivos ---- */
let cwd = '/';
const TEXT = /\.(txt|json|csv|log|md|lua|ini|cfg|yaml|yml)$/i;

function editor(path, text) {
  const ta = h('textarea', { spellcheck: 'false' });
  ta.value = text;
  const close = () => m.remove();
  const save = async () => {
    try {
      const r = await fetch('/api/fs/put?path=' + encodeURIComponent(path), { method: 'PUT', body: ta.value });
      if (!r.ok) throw new Error((await r.json()).error);
      toast('Guardado ' + path); close(); pageArchivos();
    } catch (e) { toast(e.message, true); }
  };
  const m = h('div', { class: 'modal', onclick: e => { if (e.target === m) close(); } },
    h('div', { class: 'box' }, h('div', { class: 'head' }, h('b', { class: 'grow' }, path)), ta,
      h('div', { class: 'foot' }, h('button', { class: 'btn', onclick: close }, 'Cancelar'), h('button', { class: 'btn pri', onclick: save }, 'Guardar'))));
  document.body.append(m);
  ta.focus();
  ta.addEventListener('keydown', e => { if ((e.metaKey || e.ctrlKey) && e.key === 's') { e.preventDefault(); save(); } });
}

async function openEditor(path) {
  const r = await fetch('/api/fs/get?path=' + encodeURIComponent(path));
  editor(path, r.ok ? await r.text() : '');
}

function upload(files, bar) {
  const list = [...files];
  return list.reduce((p, f) => p.then(() => new Promise(res => {
    const x = new XMLHttpRequest();
    const dest = (cwd.endsWith('/') ? cwd : cwd + '/') + f.name;
    x.open('PUT', '/api/fs/put?path=' + encodeURIComponent(dest));
    x.upload.onprogress = e => { bar.firstChild.style.width = (e.loaded / e.total * 100) + '%'; bar.dataset.name = f.name; };
    x.onload = () => { if (x.status !== 200) toast(f.name + ': ' + (JSON.parse(x.responseText).error || x.status), true); res(); };
    x.onerror = () => { toast(f.name + ': se cortó', true); res(); };
    x.send(f);
  })), Promise.resolve()).then(() => { toast(list.length === 1 ? 'Subido ' + list[0].name : `Subidos ${list.length} archivos`); pageArchivos(); });
}

async function pageArchivos() {
  let d;
  try { d = await api('fs?path=' + encodeURIComponent(cwd)); }
  catch (e) { if (cwd !== '/') { cwd = '/'; return pageArchivos(); } put(main, h('h1', {}, 'Archivos'), h('p', { class: 'bad' }, e.message)); return; }
  const parts = cwd.split('/').filter(Boolean);
  const go = p => () => { cwd = p; pageArchivos(); };
  const crumbs = h('div', { class: 'crumbs' }, h('a', { href: '#archivos', onclick: go('/') }, 'microSD'),
    parts.map((p, i) => [' / ', h('a', { href: '#archivos', onclick: go('/' + parts.slice(0, i + 1).join('/')) }, p)]));
  const entries = d.entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
  const full = n => (cwd === '/' ? '' : cwd) + '/' + n;
  const del = e => async () => { if (!confirm(`¿Borrar ${e.name}${e.dir ? ' y todo lo que tiene adentro' : ''}?`)) return;
    try { await api('fs/delete?path=' + encodeURIComponent(full(e.name)), { method: 'POST' }); pageArchivos(); } catch (er) { toast(er.message, true); } };
  const ren = e => async () => { const n = prompt('Nuevo nombre', e.name); if (!n || n === e.name) return;
    try { await api(`fs/rename?path=${encodeURIComponent(full(e.name))}&to=${encodeURIComponent(full(n))}`, { method: 'POST' }); pageArchivos(); } catch (er) { toast(er.message, true); } };
  const table = h('div', { class: 'card ftable' }, entries.length ? entries.map(e => h('div', { class: 'row' + (e.dir ? ' click' : ''), onclick: e.dir ? go(full(e.name)) : null },
    h('div', { class: 'ico' }, e.dir ? '📁' : TEXT.test(e.name) ? '📄' : /\.(jpe?g|png|gif|bmp)$/i.test(e.name) ? '🖼' : /\.(mp3|wav)$/i.test(e.name) ? '🎵' : '▫'),
    h('div', { class: 'grow' }, e.name),
    h('div', { class: 'acts', onclick: ev => ev.stopPropagation() },
      !e.dir && TEXT.test(e.name) ? h('button', { class: 'btn', onclick: () => openEditor(full(e.name)) }, 'Editar') : null,
      !e.dir ? h('a', { class: 'btn', href: '/api/fs/get?attachment=1&path=' + encodeURIComponent(full(e.name)) }, 'Bajar') : null,
      h('button', { class: 'btn', onclick: ren(e) }, 'Renombrar'), h('button', { class: 'btn red', onclick: del(e) }, 'Borrar')),
    h('div', { class: 'size' }, e.dir ? '' : fmtBytes(e.size)),
    h('div', { class: 'date' }, e.mtime ? new Date(e.mtime * 1000).toLocaleDateString('es-AR', { day: '2-digit', month: 'short', year: 'numeric' }) : ''),
    h('button', { class: 'btn more', onclick: ev => { ev.stopPropagation(); ev.target.closest('.row').classList.toggle('sel'); } }, '⋯'))) :
    h('div', { class: 'row muted' }, 'Carpeta vacía.'));
  const input = h('input', { type: 'file', multiple: true, style: 'display:none', onchange: e => upload(e.target.files, bar) });
  const bar = h('div', { class: 'prog' }, h('div'));
  const drop = h('div', { class: 'drop' }, 'Soltá archivos acá para subirlos a ', h('b', {}, cwd), bar);
  drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); upload(e.dataTransfer.files, bar); };
  const mkdir = async () => { const n = prompt('Nombre de la carpeta'); if (!n) return;
    try { await api('fs/mkdir?path=' + encodeURIComponent(full(n)), { method: 'POST' }); pageArchivos(); } catch (er) { toast(er.message, true); } };
  put(main, h('h1', {}, 'Archivos'),
    h('div', { class: 'btns', style: 'margin-bottom:14px' },
      h('button', { class: 'btn pri', onclick: () => input.click() }, 'Subir…'), input,
      h('button', { class: 'btn', onclick: mkdir }, 'Nueva carpeta'),
      h('span', { class: 'muted small', style: 'margin-left:8px' }, 'Editar:'),
      ['menu.txt', 'modules.txt', 'ha.txt', 'cameras.txt'].map(f => h('button', { class: 'btn', onclick: () => openEditor('/' + f) }, f)),
      d.total ? h('span', { class: 'muted small', style: 'margin-left:auto' }, `${fmtBytes(d.free)} libres de ${fmtBytes(d.total)}`) : null),
    crumbs, table, drop,
    h('p', { class: 'note' }, 'menu.txt ordena el inicio, modules.txt dice qué hay conectado al header; los dos se aplican al guardar. cameras.txt son las cámaras de la app Cámaras (y su Frigate), que la relee al guardar. El firmware para el Programador va en /firmware.'));
}

/* ---- Terminal ---- */
let termT, termCh = 0, termNext = 0, termLines = [], termFollow = true, termStamps = false, termFilter = '', termPort = null;
const TERM_BAUDS = [9600, 19200, 38400, 57600, 74880, 115200, 230400, 460800, 921600, 1500000, 2000000];
const levelColor = t => t[1] === ' ' && t[2] === '(' ? ({ E: '#ff6b6b', W: '#ffc857', I: '#7bd88f', D: '#8a93a3', V: '#8a93a3' }[t[0]] || '') : '';

async function pageTerminal() {
  const view = h('div', { class: 'log', style: 'height:calc(100vh - 330px);min-height:260px' });
  const status = h('div', { class: 'note' });
  const chSel = h('select', { onchange: e => { termCh = +e.target.value; termNext = 0; termLines = []; put(view); termPort = null; load(true); } },
    h('option', { value: 0, selected: termCh === 0 }, 'Canal A'), h('option', { value: 1, selected: termCh === 1 }, 'Canal B'));
  const portSel = h('select'), baudSel = h('select', {}, TERM_BAUDS.map(b => h('option', { value: b, selected: b === 115200 }, b)));
  const connBtn = h('button', { class: 'btn pri' }, 'Conectar');
  const recBtn = h('button', { class: 'btn' }, 'Grabar');
  const stampBtn = h('button', { class: 'btn' + (termStamps ? ' pri' : ''), onclick: () => { termStamps = !termStamps; stampBtn.className = 'btn' + (termStamps ? ' pri' : ''); redraw(); } }, 'Hora');
  const followBtn = h('button', { class: 'btn pri', onclick: () => { termFollow = true; followBtn.className = 'btn pri'; view.scrollTop = view.scrollHeight; } }, 'Seguir');
  const filter = h('input', { type: 'search', placeholder: 'Filtrar…', value: termFilter, style: 'max-width:240px', oninput: e => { termFilter = e.target.value; redraw(); } });
  const eol = h('select', {}, [['\r\n', 'CR LF'], ['\n', 'LF'], ['\r', 'CR'], ['', 'nada']].map(([v, n]) => h('option', { value: v }, n)));
  const input = h('input', { type: 'text', placeholder: 'Enviar al puerto…', style: 'font-family:ui-monospace,Menlo,monospace' });
  const trigList = h('div', { class: 'card' });
  const trigIn = h('input', { type: 'text', placeholder: 'Avisar cuando aparezca…' });
  let st = null;
  const lineEl = ([t, f, text]) => {
    const d = h('div', {}, (termStamps ? (t / 1000).toFixed(3).padStart(9) + ' ' : '') + text);
    const c = levelColor(text);
    if (f & 1) { d.style.background = '#5c3d00'; d.style.color = '#ffe08a'; }
    else if (f & 2) { d.style.background = '#10305a'; }
    if (c && !(f & 1)) d.style.color = c;
    return d;
  };
  const visible = l => !termFilter || l[2].toLowerCase().includes(termFilter.toLowerCase());
  function redraw() {
    put(view, termLines.filter(visible).slice(-3000).map(lineEl));
    if (termFollow) view.scrollTop = view.scrollHeight;
  }
  view.onscroll = () => {
    const atEnd = view.scrollTop + view.clientHeight >= view.scrollHeight - 20;
    if (!atEnd && termFollow) { termFollow = false; followBtn.className = 'btn'; }
    if (atEnd && !termFollow) { termFollow = true; followBtn.className = 'btn pri'; }
  };
  async function load(full) {
    try { st = await api('serial?ch=' + termCh); } catch { return; }
    if (full || !portSel.options.length) {
      put(portSel, st.ports.map(p => h('option', { value: p.name, selected: st.running ? p.name === st.port : p.name === (termPort || (termCh ? 'uart.a' : 'uart.b')) },
        p.name + (p.holder && !st.running ? ' · lo tiene ' + p.holder : ''))));
      if (st.running) baudSel.value = st.baud;
    }
    connBtn.textContent = st.running ? 'Detener' : 'Conectar';
    connBtn.className = 'btn ' + (st.running ? 'red' : 'pri');
    recBtn.textContent = st.recording ? '● Grabando' : 'Grabar';
    recBtn.className = 'btn' + (st.recording ? ' red' : '');
    const rec = st.rec_path ? '/logs/' + st.rec_path.split('/logs/').pop() : '';
    put(status, st.running ? `${st.desc} · ${st.baud} · ${st.lines} líneas · ${fmtBytes(st.rx)} recibidos` : 'Sin conexión.',
      st.recording && rec ? [' · grabando en ', h('a', { href: '/api/fs/get?attachment=1&path=' + encodeURIComponent(rec) }, rec), ' (' + fmtBytes(st.rec_bytes) + ')'] : null);
    put(trigList, st.triggers.length ? st.triggers.map((t, i) => h('div', { class: 'row' },
      h('div', { class: 'grow', style: 'font-family:ui-monospace,Menlo,monospace;color:#ffe08a;cursor:text', title: 'Tocá para cambiar el texto',
        onclick: () => { const v = prompt('Avisar cuando aparezca…', t.text); if (v && v.trim() && v !== t.text) post('serial/trigger', { index: i, text: v.trim() }).then(() => load()).catch(e => toast(e.message, true)); } },
        t.text, ' ', h('span', { class: 'muted small' }, t.hits === 1 ? '1 vez' : t.hits + ' veces')),
      h('button', { class: 'btn' + (t.notify ? ' pri' : ''), onclick: () => post('serial/trigger', { index: i, flags: (t.notify ? 0 : 1) | (t.beep ? 2 : 0) }).then(() => load()) }, 'Aviso'),
      h('button', { class: 'btn' + (t.beep ? ' pri' : ''), onclick: () => post('serial/trigger', { index: i, flags: (t.notify ? 1 : 0) | (t.beep ? 0 : 2) }).then(() => load()) }, 'Pitido'),
      h('button', { class: 'btn red', onclick: () => post('serial/trigger', { index: i, remove: true }).then(() => load()) }, '✕'))) :
      [h('div', { class: 'row muted' }, 'Sin avisos.')]);
  }
  async function poll() {
    if (location.hash !== '#terminal') return;
    try {
      const r = await api(`serial/lines?ch=${termCh}&from=${termNext}`);
      if (r.next < termNext) { termLines = []; }                 /* the channel restarted */
      termNext = r.next;
      if (r.lines.length) {
        termLines.push(...r.lines);
        if (termLines.length > 6000) termLines = termLines.slice(-5000);
        const add = r.lines.filter(visible);
        if (add.length) {
          for (const l of add) view.append(lineEl(l));
          while (view.childElementCount > 3000) view.firstChild.remove();
          if (termFollow) view.scrollTop = view.scrollHeight;
        }
      }
    } catch {}
    termT = setTimeout(poll, 400);
  }
  connBtn.onclick = async () => {
    try {
      if (st && st.running) await post('serial/stop', { ch: termCh });
      else { termPort = portSel.value; await post('serial/start', { ch: termCh, port: portSel.value, baud: +baudSel.value }); termNext = 0; termLines = []; put(view); }
      load(true);
    } catch (e) { toast(e.message, true); }
  };
  baudSel.onchange = async () => { if (st && st.running) { await post('serial/stop', { ch: termCh }); await post('serial/start', { ch: termCh, port: st.port, baud: +baudSel.value }); load(true); } };
  recBtn.onclick = async () => { try { await post('serial/record', { ch: termCh, on: !st.recording }); setTimeout(load, 400); } catch (e) { toast(e.message, true); } };
  const send = async () => { try { await post('serial/send', { ch: termCh, text: input.value, eol: eol.value }); input.value = ''; } catch (e) { toast(e.message, true); } };
  input.onkeydown = e => { if (e.key === 'Enter') send(); };
  trigIn.onkeydown = async e => { if (e.key === 'Enter' && trigIn.value.trim()) { try { await post('serial/trigger', { add: trigIn.value.trim(), flags: 1 }); trigIn.value = ''; load(); } catch (er) { toast(er.message, true); } } };
  put(main, h('h1', {}, 'Terminal'),
    h('div', { class: 'btns', style: 'margin-bottom:10px' }, chSel, portSel, baudSel, connBtn, recBtn, stampBtn, followBtn, filter,
      h('button', { class: 'btn', onclick: () => { termLines = []; put(view); } }, 'Limpiar')),
    status, view,
    h('div', { class: 'btns', style: 'margin-top:10px;flex-wrap:nowrap' }, eol, h('div', { style: 'flex:1' }, input), h('button', { class: 'btn pri', onclick: send }, 'Enviar')),
    h('h2', {}, 'Avisos'), h('p', { class: 'note' }, 'Textos que se buscan en cada línea de los dos canales, sin importar mayúsculas: la línea se marca en ámbar y, con Aviso o Pitido, la placa avisa (una vez cada 10 s como mucho).'),
    trigList, h('div', { style: 'margin-top:10px;max-width:420px' }, trigIn));
  termNext = 0; termLines = [];
  clearTimeout(termT);
  await load(true);
  poll();
  const stT = setInterval(() => { if (location.hash !== '#terminal') clearInterval(stT); else load(); }, 2000);
}

/* ---- Programador ---- */
const PHASES = { idle: 'Listo', opening: 'Abriendo el puerto', connecting: 'Conectando con la placa', stub: 'Cargando el stub',
  baud: 'Subiendo la velocidad', info: 'Leyendo el chip', erasing: 'Borrando', writing: 'Grabando', verifying: 'Verificando',
  resetting: 'Reiniciando la placa', done: 'Grabación terminada', failed: 'Falló', cancelled: 'Cancelado' };
let flashT, flashSel = '', flashPort = 'uart.b', flashLog = 0;
const fmtMs = ms => { const s = Math.round(ms / 1000); return s < 60 ? s + ' s' : Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); };

async function uploadFolder(files, bar) {
  /* an IDF build folder, subfolders and all, into /firmware/<its name>/ */
  const list = [...files].filter(f => !/(^|\/)\./.test(f.webkitRelativePath || f.name));
  const dirs = new Set();
  for (const f of list) {
    const parts = ('firmware/' + (f.webkitRelativePath || f.name)).split('/');
    for (let i = 1; i < parts.length; i++) dirs.add(parts.slice(0, i).join('/'));
  }
  for (const d of [...dirs].sort((a, b) => a.length - b.length))
    await fetch('/api/fs/mkdir?path=' + encodeURIComponent('/' + d), { method: 'POST' });
  let done = 0;
  for (const f of list) {
    await new Promise(res => {
      const x = new XMLHttpRequest();
      x.open('PUT', '/api/fs/put?path=' + encodeURIComponent('/firmware/' + (f.webkitRelativePath || f.name)));
      x.upload.onprogress = e => { bar.firstChild.style.width = ((done + e.loaded / e.total) / list.length * 100) + '%'; };
      x.onload = x.onerror = () => { done++; res(); };
      x.send(f);
    });
  }
  toast(`Subidos ${list.length} archivos a /firmware`);
}

async function pageProgramador() {
  const target = h('div', { class: 'card' }), job = h('div'), srcs = h('div', { class: 'card' }), log = h('div', { class: 'log', style: 'height:260px' });
  const portSel = h('select', { onchange: e => { flashPort = e.target.value; } });
  const bar = h('div', { class: 'prog' }, h('div'));
  const pickDir = h('input', { type: 'file', webkitdirectory: true, multiple: true, style: 'display:none',
    onchange: async e => { await uploadFolder(e.target.files, bar); load(true); } });
  const pickBin = h('input', { type: 'file', accept: '.bin', multiple: true, style: 'display:none',
    onchange: async e => { cwd = '/firmware'; await fetch('/api/fs/mkdir?path=%2Ffirmware', { method: 'POST' }); await upload(e.target.files, bar); location.hash = '#programador'; load(true); } });
  let last = null, lastSources = '';
  const start = async () => {
    try { await post('flash/start', { path: flashSel, port: flashPort }); flashLog = 0; put(log); load(); }
    catch (e) { toast(e.message, true); }
  };
  const load = async force => {
    let st;
    try { st = await api('flash'); } catch { return; }
    last = st;
    if (!portSel.options.length || force) {
      put(portSel, st.ports.map(p => h('option', { value: p.name, selected: p.name === flashPort },
        p.name + (p.lines ? ' · con EN/BOOT' : ' · sin EN/BOOT') + (p.holder ? ' · lo tiene ' + p.holder : ''))));
    }
    const c = st.chip;
    put(target,
      row('Placa', c ? `${c.name} ${c.revision || ''}` : 'sin detectar', c ? '' : 'muted'),
      c ? row('Flash', c.flash_size ? fmtBytes(c.flash_size) : '—') : null,
      c && c.mac ? row('MAC', c.mac) : null,
      c && c.secure ? h('div', { class: 'row warn' }, 'Flash cifrada o arranque seguro: no se puede grabar desde acá') : null,
      h('div', { class: 'row' }, h('div', { class: 'grow' }, portSel),
        h('button', { class: 'btn', disabled: st.busy, onclick: async () => { try { await post('flash/detect', { port: flashPort }); load(); } catch (e) { toast(e.message, true); } } }, 'Detectar')));
    /* firmware on the card; redrawn only when it changes */
    const sig = JSON.stringify(st.sources.map(x => x.path + x.total)) + flashSel + st.busy;
    if (sig !== lastSources || force) {
      lastSources = sig;
      if (!flashSel && st.sources.length) flashSel = st.sources[0].path;
      put(srcs, st.sources.length ? st.sources.map(x => {
        const on = x.path === flashSel, other = c && x.chip && c.name.toLowerCase().replace(/-/g, '') !== x.chip.toLowerCase();
        return h('div', { class: 'row click' + (on ? ' sel' : ''), style: on ? 'background:var(--card2)' : '', onclick: () => { flashSel = x.path; load(true); } },
          h('div', { class: 'ico' }, on ? '●' : '○'),
          h('div', { class: 'grow' }, h('div', {}, x.name, x.version ? h('span', { class: 'muted small' }, '  ' + x.version) : null),
            h('div', { class: 'small ' + (other ? 'warn' : 'muted') }, (x.chip ? (other ? 'para ' : '') + (x.chip_label || x.chip.toUpperCase()) : 'chip desconocido') + ' · ' +
              x.files.length + (x.files.length === 1 ? ' archivo' : ' archivos') + ' · ' + fmtBytes(x.total)),
            on ? h('div', { class: 'small muted', style: 'font-family:ui-monospace,Menlo,monospace;margin-top:6px' },
              x.files.map(f => h('div', {}, f.offset + '  ' + f.name + '  (' + fmtBytes(f.size) + ')'))) : null));
      }) : [h('div', { class: 'row muted' }, 'No hay firmware en /firmware. Subí una carpeta de build de ESP-IDF o un .bin.')]);
    }
    /* the job */
    const done = st.phase === 'done', failed = st.phase === 'failed' || st.phase === 'cancelled';
    put(job, st.job === 'flash' || st.busy ? h('div', { class: 'card pad' },
      h('div', { class: 'btns' }, h('b', { class: done ? 'ok' : failed ? 'bad' : '' }, PHASES[st.phase] || st.phase),
        h('span', { class: 'muted small' }, st.source + (st.file_count ? ` · archivo ${st.file_index + 1} de ${st.file_count}: ${st.file_name}` : '')),
        st.busy ? h('button', { class: 'btn red', style: 'margin-left:auto', onclick: () => post('flash/cancel') }, 'Cancelar') : null),
      h('div', { class: 'prog', style: 'height:10px;margin-top:12px' }, h('div', { style: `width:${st.percent}%;${done ? 'background:var(--green)' : failed ? 'background:var(--red)' : ''}` })),
      h('div', { class: 'small muted', style: 'margin-top:8px' },
        `${fmtBytes(st.done)} de ${fmtBytes(st.total)} · ${st.bytes_per_s ? fmtBytes(st.bytes_per_s) + '/s' : '—'} · ` +
        (st.busy ? (st.eta_ms ? 'faltan ' + fmtMs(st.eta_ms) : '') : 'en ' + fmtMs(st.elapsed_ms)) + (done && st.verified ? ' · MD5 verificado' : '')),
      failed && st.error ? h('div', { class: 'bad small', style: 'margin-top:6px' }, st.error) : null) : null);
    startBtn.disabled = st.busy || !flashSel;
    /* its log */
    if (st.log_count > flashLog) {
      const l = await api('flash/log?from=' + flashLog);
      flashLog = l.next;
      const stick = log.scrollTop + log.clientHeight >= log.scrollHeight - 30;
      for (const x of l.lines) log.append(h('div', { style: x.l === 3 ? 'color:var(--red)' : x.l === 2 ? 'color:var(--orange)' : x.l === 1 ? 'color:var(--green)' : '' }, x.t));
      if (stick) log.scrollTop = log.scrollHeight;
    }
  };
  const startBtn = h('button', { class: 'btn pri', onclick: start }, 'Grabar');
  put(main, h('h1', {}, 'Programador'),
    h('div', { class: 'grid2' },
      h('div', {}, h('h2', { style: 'margin-top:0' }, 'La placa a programar'), target,
        h('p', { class: 'note' }, 'Conectada al header: TX, RX y GND al puerto, y EN y BOOT a los pines que dice modules.txt (module target … en= boot=).')),
      h('div', {}, h('h2', { style: 'margin-top:0' }, 'Firmware en la tarjeta'), srcs,
        h('div', { class: 'btns', style: 'margin-top:12px' }, startBtn,
          h('button', { class: 'btn', onclick: () => pickDir.click() }, 'Subir carpeta de build…'), pickDir,
          h('button', { class: 'btn', onclick: () => pickBin.click() }, 'Subir .bin…'), pickBin), bar)),
    h('h2', {}, 'Grabación'), job, h('h2', {}, 'Registro del programador'), log);
  flashLog = 0;
  clearInterval(flashT);
  flashT = setInterval(() => { if (location.hash === '#programador') load(); else clearInterval(flashT); }, 700);
  load(true);
}

/* ---- Ajustes ---- */
const TZS = [['Buenos Aires (UTC−3)', '<-03>3'], ['Santiago (UTC−4/−3)', '<-04>4<-03>,M9.1.6/24,M4.1.6/24'], ['Ciudad de México (UTC−6)', 'CST6'],
  ['Nueva York (UTC−5/−4)', 'EST5EDT,M3.2.0,M11.1.0'], ['Madrid (UTC+1/+2)', 'CET-1CEST,M3.5.0,M10.5.0/3'], ['Londres (UTC+0/+1)', 'GMT0BST,M3.5.0/1,M10.5.0/2'], ['UTC', 'UTC0']];

async function pageAjustes() {
  const s = await api('settings');
  const set = async (o, msg) => { try { await post('settings', o); if (msg) toast(msg); } catch (e) { toast(e.message, true); } };
  const name = h('input', { type: 'text', value: s.name });
  const tz = h('select', { onchange: e => set({ tz: e.target.value }, 'Zona horaria cambiada') },
    TZS.map(([n, v]) => h('option', { value: v, selected: v === s.tz }, n)), TZS.some(t => t[1] === s.tz) ? null : h('option', { value: s.tz, selected: true }, s.tz));
  const lang = h('select', { onchange: e => set({ lang: e.target.value }, 'Idioma cambiado') },
    (s.langs.length ? s.langs : [{ code: 'es', name: 'Español' }]).map(l => h('option', { value: l.code, selected: l.code === s.lang }, l.name)));
  const sw = h('div', { class: 'swatches' }, s.wallpapers.map((c, i) => {
    const [a, b] = c.split(',');
    const el = h('div', { class: 'swatch' + (i === s.wallpaper ? ' on' : ''), style: `background:linear-gradient(${a},${b})`,
      onclick: () => { [...sw.children].forEach(x => x.classList.remove('on')); el.classList.add('on'); set({ wallpaper: i }); } });
    return el;
  }));
  const slider = (key, v) => h('input', { type: 'range', min: 1, max: 100, value: v, onchange: e => set({ [key]: +e.target.value }) });
  put(main, h('h1', {}, 'Ajustes'), h('div', { class: 'grid2' },
    h('div', { class: 'card pad' },
      h('label', { class: 'f', style: 'margin-top:0' }, 'Nombre en la red'), h('div', { class: 'btns' }, h('div', { style: 'flex:1' }, name),
        h('button', { class: 'btn pri', onclick: () => set({ name: name.value.trim() }, 'Ahora responde como ' + name.value.trim() + '.local') }, 'Cambiar')),
      h('p', { class: 'note', style: 'margin:6px 0 0' }, 'Letras minúsculas, números y guiones. Es lo que responde como <nombre>.local.'),
      h('label', { class: 'f' }, 'Zona horaria'), tz, h('label', { class: 'f' }, 'Idioma'), lang),
    h('div', { class: 'card pad' },
      h('label', { class: 'f', style: 'margin-top:0' }, 'Orientación'),
      h('div', { class: 'btns' }, h('button', { class: 'btn' + (s.landscape ? '' : ' pri'), onclick: () => { set({ landscape: false }); setTimeout(pageAjustes, 600); } }, 'Vertical'),
        h('button', { class: 'btn' + (s.landscape ? ' pri' : ''), onclick: () => { set({ landscape: true }); setTimeout(pageAjustes, 600); } }, 'Horizontal')),
      h('label', { class: 'f' }, 'Brillo'), slider('brightness', s.brightness),
      h('label', { class: 'f' }, 'Volumen'), slider('volume', s.volume))),
    h('h2', {}, 'Fondo de pantalla'), h('div', { class: 'card pad' }, sw),
    h('h2', {}, 'Seguridad'), h('div', { class: 'card pad' },
      h('p', { style: 'margin-top:0' }, 'La contraseña, las redes de confianza y HTTPS se cambian sólo en la placa: Ajustes, Portal web.'),
      h('div', { class: 'btns' }, h('a', { class: 'btn', href: '/api/tls/ca' }, 'Bajar la autoridad HTTPS de la placa')),
      h('p', { class: 'note' }, 'Instalada una vez como confiable en la Mac (Acceso a Llaveros, "Confiar siempre") o en el iPhone (Ajustes, General, Información, Confianza de certificados), el navegador deja de avisar por https://' + (info.name || 'p4os') + '.local. Sólo sirve para nombres .local y direcciones de la red de casa.')),
    h('h2', {}, 'Sistema'), h('div', { class: 'card pad' }, h('div', { class: 'btns' },
      h('button', { class: 'btn red', onclick: async () => { if (confirm('¿Reiniciar la placa?')) { await post('restart'); toast('Reiniciando…'); } } }, 'Reiniciar'))));
}

/* ---- Registro ----
 * The board's log ring (256 KB), followed live; or the previous boot's tail,
 * which a restart keeps (after a panic, a watchdog, a hang): the way to see
 * what happened with no serial cable, e.g. while the USB port is a keyboard.
 * A filter (text, or /regex/), errors and warnings coloured, a download. */
let logT, logPaused = false;
function pageRegistro() {
  let lines = [], partial = '', from = 0, prev = false, filt = '';
  const pre = h('div', { class: 'log' });
  const MAX = 30000, SHOW = 4000;
  const match = l => {
    if (!filt) return true;
    if (filt.length > 2 && filt[0] === '/' && filt.endsWith('/')) { try { return new RegExp(filt.slice(1, -1), 'i').test(l); } catch { return true; } }
    return l.toLowerCase().includes(filt.toLowerCase());
  };
  const lineEl = l => h('div', { class: /^E \(/.test(l) || /Guru|panic|assert|abort/i.test(l) ? 'le' : /^W \(/.test(l) ? 'lw' : null }, l);
  const render = () => {
    const sel = lines.filter(match);
    pre.replaceChildren(...sel.slice(-SHOW).map(lineEl));
    count.textContent = sel.length + (filt ? ' de ' + lines.length : '') + ' líneas' + (sel.length > SHOW ? ' (se ven las últimas ' + SHOW + ')' : '');
    pre.scrollTop = pre.scrollHeight;
  };
  const addText = t => {
    const parts = (partial + t).split('\n');
    partial = parts.pop();
    const fresh = parts.filter(l => l.length);
    lines.push(...fresh);
    if (lines.length > MAX) lines = lines.slice(-MAX * 0.8);
    return fresh;
  };
  const count = h('span', { class: 'note' });
  const pb = h('button', { class: 'btn', onclick: () => { logPaused = !logPaused; pb.textContent = logPaused ? 'Seguir' : 'Pausar'; } }, logPaused ? 'Seguir' : 'Pausar');
  const fin = h('input', { type: 'text', placeholder: 'filtrar (texto o /regex/)', style: 'flex:1;min-width:160px', oninput: e => { filt = e.target.value; render(); } });
  const which = h('button', { class: 'btn', onclick: () => { prev = !prev; which.textContent = prev ? 'Ver el arranque actual' : 'Ver el arranque anterior'; start(); } }, 'Ver el arranque anterior');
  const dl = h('button', { class: 'btn', onclick: () => {
    const a = h('a', { href: URL.createObjectURL(new Blob([lines.join('\n') + '\n'], { type: 'text/plain' })),
      download: 'p4os-' + (prev ? 'arranque-anterior' : 'registro') + '-' + new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-') + '.txt' });
    a.click();
  } }, 'Descargar');
  const title = h('h1', {}, 'Registro');
  const sub = h('div', { class: 'note', style: 'margin:-6px 0 10px' });
  put(main, title, sub, h('div', { class: 'btns', style: 'margin-bottom:10px;flex-wrap:wrap;gap:8px' }, pb,
    h('button', { class: 'btn', onclick: () => { lines = []; render(); } }, 'Limpiar'), which, dl, fin), count, pre);
  const tick = async () => {
    if (location.hash !== '#registro' || prev) return;
    if (!logPaused) try {
      const r = await fetch('/api/log?from=' + from);
      const t = await r.text();
      from = +r.headers.get('X-Log-Next') || from;
      if (t) {
        const fresh = addText(t).filter(match);
        if (fresh.length) {
          const stick = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 30;
          pre.append(...fresh.map(lineEl));
          while (pre.childElementCount > SHOW) pre.firstChild.remove();
          count.textContent = lines.length + ' líneas';
          if (stick) pre.scrollTop = pre.scrollHeight;
        }
        if (t.length >= 16000) { logT = setTimeout(tick, 50); return; }   /* more waiting: fetch it now */
      }
    } catch {}
    logT = setTimeout(tick, 1000);
  };
  const start = async () => {
    clearTimeout(logT);
    lines = []; partial = ''; from = 0;
    sub.textContent = prev ? 'Lo último que escribió el arranque anterior (se conserva al reiniciar, no al desenchufar).'
                           : 'En vivo. El anillo guarda los últimos 256 KB.';
    if (!prev) { render(); tick(); return; }
    for (let i = 0; i < 8; i++) {
      const r = await fetch('/api/log?prev=1&from=' + from);
      const t = await r.text();
      from = +r.headers.get('X-Log-Next') || from;
      addText(t);
      if (t.length < 16000) break;
    }
    if (partial) { lines.push(partial); partial = ''; }
    if (!lines.length) sub.textContent = 'No hay registro del arranque anterior: la placa se desenchufó, o arrancó un firmware distinto (el anillo cambia de lugar con cada compilación).';
    render();
  };
  start();
}

/* ---- Firmware ----
 * Updates over Wi-Fi (PUT /api/ota): the image goes to the idle slot and the
 * board restarts into it on trial - confirmed after 30 s up, else the
 * previous one comes back by itself. The last panic's dump, for
 * tools/coredump.sh. */
async function pageFirmware() {
  let st, cd;
  try { [st, cd] = await Promise.all([api('ota'), api('coredump')]); }
  catch (e) { put(main, h('h1', {}, 'Firmware'), h('div', { class: 'card pad' }, 'No se pudo leer: ' + e.message)); return; }
  const bar = h('div', { class: 'prog', style: 'margin:12px 0 6px' }, h('div'));
  const msg = h('div', { class: 'note' });
  const file = h('input', { type: 'file', accept: '.bin', style: 'display:none', onchange: e => upload(e.target.files[0]) });
  const restartBtn = h('button', { class: 'btn pri', style: 'display:none', onclick: () => restartInto() }, 'Reiniciar con el firmware nuevo');
  function upload(f) {
    if (!f) return;
    if (!/\.bin$/i.test(f.name)) { toast('Tiene que ser el .bin del firmware (build/rev1_3/p4os.bin)', true); return; }
    const x = new XMLHttpRequest();
    x.open('PUT', '/api/ota');
    x.upload.onprogress = e => { if (e.lengthComputable) { bar.firstChild.style.width = (e.loaded * 100 / e.total).toFixed(1) + '%'; msg.textContent = 'Subiendo ' + fmtBytes(e.loaded) + ' de ' + fmtBytes(e.total) + '…'; } };
    x.onload = () => {
      let j = {};
      try { j = JSON.parse(x.responseText); } catch {}
      if (x.status === 200 && j.ok) { msg.textContent = 'Listo: ' + fmtBytes(j.size) + ' escritos en ' + (j.ms / 1000).toFixed(1).replace('.', ',') + ' s. Arranca a prueba al reiniciar.'; restartBtn.style.display = ''; }
      else { msg.textContent = 'Falló: ' + (j.error || x.statusText); bar.firstChild.style.width = '0%'; }
    };
    x.onerror = () => { msg.textContent = 'Se cortó la conexión'; };
    x.send(f);
  }
  async function restartInto() {
    const before = st.running;
    restartBtn.disabled = true;
    await post('ota/restart').catch(() => {});
    msg.textContent = 'Reiniciando…';
    for (let i = 0; i < 90; i++) {
      await new Promise(r => setTimeout(r, 1000));
      try {
        const s2 = await api('ota');
        if (s2.running === before) { msg.textContent = 'Volvió al firmware anterior: el nuevo no arrancó. Mirá el registro del arranque anterior.'; restartBtn.disabled = false; return; }
        msg.textContent = s2.trial ? 'Corriendo ' + s2.running + ' a prueba: se confirma a los 30 s…' : 'Confirmado: ' + s2.running + ' es el firmware ahora.';
        if (!s2.trial) { setTimeout(pageFirmware, 1500); return; }
      } catch {}
    }
    msg.textContent = 'La placa no contestó en un minuto y medio: mirala.';
  }
  const o = st.other || {};
  put(main, h('h1', {}, 'Firmware'),
    h('div', { class: 'card pad' },
      row('Versión', st.version), row('Compilado', (st.built || '') + ' ' + (st.time || '')), row('IDF', st.idf || ''), st.elf ? row('ELF', st.elf) : null,
      row('Ranura', st.running + (st.trial ? ' (a prueba)' : ''), st.trial ? 'warn' : ''),
      row('La otra ranura', o.slot ? o.slot + (o.version ? ' — ' + o.version + ', ' + (o.built || '') + (o.elf ? ', ELF ' + o.elf : '') + (o.state ? ' (' + o.state + ')' : '') : ' — vacía') : '—')),
    h('h2', {}, 'Actualizar por Wi-Fi'),
    h('div', { class: 'card pad' },
      h('div', { class: 'note' }, 'Elegí build/rev1_3/p4os.bin. Se escribe en la otra ranura mientras sube; al reiniciar arranca a prueba y, si no llega a los 30 s, la placa vuelve sola al firmware de ahora. Desde la terminal: tools/ota.sh.'),
      h('div', { class: 'btns', style: 'margin-top:10px' }, h('button', { class: 'btn', onclick: () => file.click() }, 'Elegir el .bin…'), restartBtn,
        o.version ? h('button', { class: 'btn', onclick: async () => { if (!confirm('¿Arrancar ' + o.slot + ' (' + o.version + ') en el próximo reinicio?')) return; try { await post('ota/other'); toast('Arranca ' + o.slot + ' al reiniciar'); } catch (e) { toast(e.message, true); } } }, 'Volver a la otra ranura') : null),
      file, bar, msg),
    h('h2', {}, 'Último cuelgue'),
    h('div', { class: 'card pad' },
      cd.present ? [
        row('Volcado', fmtBytes(cd.size) + (cd.valid ? '' : ' (dañado)')),
        cd.task ? row('Tarea', cd.task) : null, cd.pc && cd.pc !== '0x00000000' ? row('PC / RA', cd.pc + ' / ' + cd.ra) : null,
        h('div', { class: 'note' }, 'Para ver funciones y líneas: tools/coredump.sh en la Mac. Necesita la ELF del firmware que se colgó: build/elf/<sha>.elf, que tools/ota.sh guarda en cada instalación.'),
        h('div', { class: 'btns', style: 'margin-top:10px' },
          h('a', { class: 'btn', href: '/api/coredump/elf', download: 'p4os-coredump.bin' }, 'Descargar el volcado'),
          h('button', { class: 'btn red', onclick: async () => { if (!confirm('¿Borrar el volcado?')) return; await post('coredump/erase').catch(e => toast(e.message, true)); pageFirmware(); } }, 'Borrar'),
          h('button', { class: 'btn', onclick: () => { location.hash = '#registro'; } }, 'Ver el registro'))]
      : h('div', { class: 'note' }, 'No hay: la placa no se colgó desde que se borró el último.')));
}

/* ---- Banco ---- */
const CH_COLOR = ['#ffd60a', '#40c8e0', '#ff5ad2', '#3b82f6'];
const num = (v, d) => v == null ? '--' : v.toFixed(d).replace('.', ',');
function eng(v, unit) {
  if (v == null || !isFinite(v)) return '--';
  const P = [[1e6, 'M'], [1e3, 'k'], [1, ''], [1e-3, 'm'], [1e-6, 'µ'], [1e-9, 'n']];
  const a = Math.abs(v);
  let k = P.findIndex(([f]) => a >= f * 0.9995);
  if (k < 0 || a === 0) k = 2;
  if (unit !== 'Hz' && unit !== 's' && k < 2) k = 2;
  const s = v / P[k][0], as = Math.abs(s);
  return s.toFixed(as >= 100 ? 1 : as >= 10 ? 2 : 3).replace('.', ',') + ' ' + P[k][1] + unit;
}
/* the knobs go 1-2-5 */
function step125(v, dir) {
  const seq = [1, 2, 5];
  let e = Math.floor(Math.log10(v) + 1e-9);
  const m = v / 10 ** e;
  let i = seq.reduce((b, x, j) => Math.abs(x - m) < Math.abs(seq[b] - m) ? j : b, 0) + dir;
  if (i > 2) { i = 0; e++; }
  if (i < 0) { i = 2; e--; }
  return +(seq[i] * 10 ** e).toPrecision(3);
}
let benchT, waveT, benchShot = 0;
async function pageBanco() {
  let st = null, wave = null, lastWseq = null;
  const scmd = cmd => post('bench/scope', { cmd }).catch(e => toast(e.message, true));
  /* supply */
  const rdV = h('div', { class: 'big' }), rdMode = h('span', { class: 'pill' }), rdIA = h('div', { class: 'mid' }), rdInfo = h('div', { class: 'note' });
  const rdOut = h('button', { class: 'btn' }, 'Salida');
  const vIn = h('input', { type: 'text', placeholder: 'V', style: 'width:90px' }), iIn = h('input', { type: 'text', placeholder: 'A', style: 'width:90px' });
  const parseNum = s => parseFloat(String(s).replace(',', '.'));
  const applyV = () => { const v = parseNum(vIn.value); if (isFinite(v)) post('bench/riden', { v }).then(() => { vIn.value = ''; }).catch(e => toast(e.message, true)); };
  const applyI = () => { const i = parseNum(iIn.value); if (isFinite(i)) post('bench/riden', { i }).then(() => { iIn.value = ''; }).catch(e => toast(e.message, true)); };
  vIn.onkeydown = e => { if (e.key === 'Enter') applyV(); };
  iIn.onkeydown = e => { if (e.key === 'Enter') applyI(); };
  rdOut.onclick = () => st && st.riden.ok && post('bench/riden', { on: !st.riden.on }).catch(e => toast(e.message, true));
  const rdSet = h('div', { class: 'note' });
  /* logger */
  const lgState = h('div', { class: 'mid' }), lgInfo = h('div', { class: 'note' }), lgBtn = h('button', { class: 'btn' }, 'Grabar');
  const lgSeries = h('div', { class: 'btns', style: 'margin-top:8px' });
  const lgIv = h('select', { style: 'width:auto' }, [[500, '0,5 s'], [1000, '1 s'], [2000, '2 s'], [5000, '5 s'], [10000, '10 s'], [30000, '30 s'], [60000, '1 min']].map(([v, n]) => h('option', { value: v }, n)));
  lgIv.onchange = () => post('bench/log', { interval: +lgIv.value }).then(load).catch(e => toast(e.message, true));
  lgBtn.onclick = () => post('bench/log', { start: !st.log.running }).then(() => setTimeout(load, 300)).catch(e => toast(e.message, true));
  /* scope */
  const host = h('input', { type: 'text', placeholder: 'Dirección del osciloscopio (192.168.1.50)', style: 'max-width:320px' });
  const conBtn = h('button', { class: 'btn pri' }, 'Conectar');
  conBtn.onclick = () => post('bench/scope', st && (st.scope.connected || st.scope.connecting) ? { disconnect: true } : { connect: host.value.trim() }).then(() => setTimeout(load, 300)).catch(e => toast(e.message, true));
  const scStatus = h('div', { class: 'note' });
  const canvas = h('canvas', { class: 'scope', width: 1200, height: 800 });
  const chRow = h('div', { class: 'btns' }), tbRow = h('div', { class: 'btns' }), meas = h('div', { class: 'card' });
  const shotImg = h('img', { class: 'shot', alt: '' });
  const shotBtn = h('button', { class: 'btn', onclick: () => post('bench/shot').then(() => toast('Pidiendo la captura…')).catch(e => toast(e.message, true)) }, 'Captura del osciloscopio');
  const ctl = h('div', { class: 'btns' },
    h('button', { class: 'btn green', onclick: () => scmd(':RUN') }, 'Run'),
    h('button', { class: 'btn red', onclick: () => scmd(':STOP') }, 'Stop'),
    h('button', { class: 'btn', onclick: () => scmd(':SINGle') }, 'Single'),
    h('button', { class: 'btn', onclick: () => scmd(':AUToscale') }, 'Auto'),
    h('button', { class: 'btn', onclick: () => scmd(':CLEar') }, 'Borrar'), shotBtn);

  function draw() {
    const c = canvas.getContext('2d'), W = canvas.width, H = canvas.height;
    c.fillStyle = '#050607'; c.fillRect(0, 0, W, H);
    c.strokeStyle = '#1f2a30'; c.lineWidth = 1;
    for (let i = 1; i < 12; i++) { c.beginPath(); c.moveTo(i * W / 12 + .5, 0); c.lineTo(i * W / 12 + .5, H); c.stroke(); }
    for (let i = 1; i < 8; i++) { c.beginPath(); c.moveTo(0, i * H / 8 + .5); c.lineTo(W, i * H / 8 + .5); c.stroke(); }
    c.strokeStyle = '#34464f';
    c.beginPath(); c.moveTo(W / 2 + .5, 0); c.lineTo(W / 2 + .5, H); c.moveTo(0, H / 2 + .5); c.lineTo(W, H / 2 + .5); c.stroke();
    if (!wave) return;
    wave.ch.forEach((ch, k) => {
      if (!ch.on || !ch.v.length || !ch.scale) return;
      /* screen: 8 divisions of scale, the channel's offset moves its zero */
      const y = v => H / 2 - (v + ch.offset) / ch.scale * (H / 8);
      c.strokeStyle = CH_COLOR[k]; c.lineWidth = 2; c.beginPath();
      ch.v.forEach((v, i) => { const x = i * W / (ch.v.length - 1); i ? c.lineTo(x, y(v)) : c.moveTo(x, y(v)); });
      c.stroke();
      c.fillStyle = CH_COLOR[k]; c.font = 'bold 22px -apple-system,sans-serif';
      c.fillText(String(k + 1), 6, Math.max(20, Math.min(H - 6, y(0) + 8)));
      if (wave.trig_source === k + 1) {
        const ty = y(wave.trig_level);
        c.fillStyle = '#ff9f0a'; c.beginPath(); c.moveTo(W, ty); c.lineTo(W - 16, ty - 9); c.lineTo(W - 16, ty + 9); c.fill();
      }
    });
    c.fillStyle = '#8e8e93'; c.font = '20px -apple-system,sans-serif';
    c.fillText(eng(wave.timebase, 's') + '/div', W - 150, H - 12);
  }
  async function pollWave() {
    if (location.hash !== '#banco') return;
    if (st && st.scope.connected) {
      try { wave = await api('bench/wave'); draw(); } catch {}
    }
    waveT = setTimeout(pollWave, st && st.scope.connected ? 150 : 1000);
  }
  function chips() {
    const s = st.scope;
    put(chRow, s.ch.map((ch, k) => h('div', { class: 'chchip', style: `border-color:${ch.on ? CH_COLOR[k] : 'transparent'}` },
      h('button', { class: 'btn', style: ch.on ? `background:${CH_COLOR[k]};color:#000` : '', onclick: () => scmd(`:CHANnel${k + 1}:DISPlay ${ch.on ? 'OFF' : 'ON'}`).then(() => setTimeout(load, 400)) }, 'CH' + (k + 1)),
      h('button', { class: 'btn', onclick: () => scmd(`:CHANnel${k + 1}:SCALe ${step125(ch.scale, -1)}`) }, '−'),
      h('span', { class: 'val' }, eng(ch.scale, 'V')),
      h('button', { class: 'btn', onclick: () => scmd(`:CHANnel${k + 1}:SCALe ${step125(ch.scale, 1)}`) }, '+'))));
    put(tbRow,
      h('span', { class: 'muted' }, 'Tiempo'),
      h('button', { class: 'btn', onclick: () => scmd(`:TIMebase:MAIN:SCALe ${step125(s.timebase, -1)}`) }, '−'),
      h('span', { class: 'val' }, eng(s.timebase, 's') + '/div'),
      h('button', { class: 'btn', onclick: () => scmd(`:TIMebase:MAIN:SCALe ${step125(s.timebase, 1)}`) }, '+'),
      h('span', { class: 'muted', style: 'margin-left:18px' }, 'Disparo CH' + s.trig_source),
      h('button', { class: 'btn', onclick: () => scmd(`:TRIGger:EDGe:LEVel ${(s.trig_level - s.ch[s.trig_source - 1].scale / 5).toPrecision(4)}`) }, '−'),
      h('span', { class: 'val' }, eng(s.trig_level, 'V')),
      h('button', { class: 'btn', onclick: () => scmd(`:TRIGger:EDGe:LEVel ${(s.trig_level + s.ch[s.trig_source - 1].scale / 5).toPrecision(4)}`) }, '+'),
      h('select', { style: 'width:auto', onchange: e => scmd(`:TRIGger:EDGe:SOURce CHANnel${e.target.value}`).then(() => setTimeout(load, 400)) },
        [1, 2, 3, 4].map(n => h('option', { value: n, selected: n === s.trig_source }, 'CH' + n))));
    const on = s.ch.map((c, k) => [c, k]).filter(([c]) => c.on);
    put(meas, on.length ? on.map(([c, k]) => h('div', { class: 'row' },
      h('div', { style: `color:${CH_COLOR[k]};width:50px;font-weight:600` }, 'CH' + (k + 1)),
      ...[['Vpp', c.meas.vpp, 'V'], ['Vmáx', c.meas.vmax, 'V'], ['Vmín', c.meas.vmin, 'V'], ['Vmed', c.meas.vavg, 'V'], ['Vrms', c.meas.vrms, 'V'], ['Frec', c.meas.freq, 'Hz'], ['Período', c.meas.period, 's']]
        .map(([n, v, u]) => h('div', { class: 'grow' }, h('div', { class: 'muted small' }, n), eng(v, u))))) :
      [h('div', { class: 'row muted' }, 'Ningún canal encendido.')]);
  }
  async function load() {
    try { st = await api('bench'); } catch { return; }
    const r = st.riden, l = st.log, s = st.scope;
    /* supply */
    rdV.textContent = r.ok ? num(r.v, 2) + ' V' : '--,-- V';
    rdV.style.color = !r.ok || !r.on ? 'var(--dim)' : r.cc ? 'var(--orange)' : 'var(--green)';
    rdMode.textContent = !r.ok ? '' : !r.on ? 'APAGADA' : r.protect === 1 ? 'OVP' : r.protect === 2 ? 'OCP' : r.cc ? 'CC' : 'CV';
    rdIA.textContent = r.ok ? num(r.i, r.idec) + ' A   ·   ' + num(r.p, 2) + ' W' : '';
    rdOut.textContent = r.ok && r.on ? 'Apagar la salida' : 'Encender la salida';
    rdOut.className = 'btn ' + (r.ok && r.on ? 'red' : 'green');
    rdOut.disabled = !r.ok;
    put(rdSet, r.ok ? `Ajuste ${num(r.v_set, 2)} V · ${num(r.i_set, r.idec)} A · entrada ${num(r.v_in, 2)} V (tope ${num(Math.max(0, r.v_in - 1.01), 2)} V) · ${r.temp} °C · ${num(r.ah, 3)} Ah · ${num(r.wh, 3)} Wh` : '');
    put(rdInfo, (r.model ? 'RD' + r.model + ' · ' : '') + (r.where || '') + (r.error ? ' · ' + r.error : ''));
    if (lastWseq != null && r.wseq !== lastWseq && r.wmsg) toast(r.wmsg, true);
    lastWseq = r.wseq;
    /* logger */
    const e = l.elapsed;
    lgState.textContent = l.running ? `● ${String(Math.floor(e / 3600)).padStart(2, '0')}:${String(Math.floor(e / 60) % 60).padStart(2, '0')}:${String(e % 60).padStart(2, '0')} · ${l.rows} filas` : 'Detenido';
    lgState.style.color = l.running ? 'var(--red)' : '';
    lgBtn.textContent = l.running ? 'Detener' : 'Grabar';
    lgBtn.className = 'btn ' + (l.running ? 'red' : 'pri');
    lgIv.value = l.interval; lgIv.disabled = l.running;
    put(lgInfo, l.error ? h('span', { class: 'bad' }, l.error) : l.path ? ['Archivo: ', h('a', { href: '/api/fs/get?attachment=1&path=' + encodeURIComponent(l.path) }, l.path)] : 'Una fila por toma en un CSV de la carpeta logs de la tarjeta.');
    put(lgSeries, l.series.map((x, i) => h('button', {
      class: 'btn', disabled: l.running,
      style: l.mask & (1 << i) ? `background:${x.color};color:#000` : '',
      onclick: () => post('bench/log', { mask: l.mask ^ (1 << i) }).then(load).catch(er => toast(er.message, true)) }, x.name)));
    /* scope */
    if (!host.matches(':focus') && s.host && !host.value) host.value = s.host;
    conBtn.textContent = s.connected || s.connecting ? 'Desconectar' : 'Conectar';
    conBtn.className = 'btn ' + (s.connected || s.connecting ? '' : 'pri');
    put(scStatus, s.connected ? `${s.idn} · ${s.trig || '--'} · ${s.fps.toFixed(1).replace('.', ',')} trazas/s` : s.connecting ? 'Conectando…' : s.error || 'Sin conectar.');
    if (s.connected) chips(); else { put(chRow); put(tbRow); put(meas); }
    if (s.shot_seq !== benchShot && s.shot_seq) { benchShot = s.shot_seq; shotImg.src = '/api/bench/shot?s=' + s.shot_seq; shotImg.style.display = 'block'; }
  }
  put(main, h('h1', {}, 'Banco'),
    h('div', { class: 'grid2' },
      h('div', { class: 'card pad' }, h('h2', { style: 'margin-top:0' }, 'Fuente Riden'),
        h('div', { style: 'display:flex;align-items:baseline;gap:14px' }, rdV, rdMode), rdIA, rdSet,
        h('div', { class: 'btns', style: 'margin-top:12px' }, rdOut, vIn, h('button', { class: 'btn', onclick: applyV }, 'Fijar V'), iIn, h('button', { class: 'btn', onclick: applyI }, 'Fijar A')),
        rdInfo),
      h('div', { class: 'card pad' }, h('h2', { style: 'margin-top:0' }, 'Registro a CSV'),
        lgState, h('div', { class: 'btns', style: 'margin-top:10px' }, lgBtn, h('span', { class: 'muted' }, 'cada'), lgIv), lgSeries, lgInfo)),
    h('h2', {}, 'Osciloscopio Rigol'),
    h('div', { class: 'btns', style: 'margin-bottom:8px' }, h('div', { style: 'flex:1;max-width:320px' }, host), conBtn), scStatus,
    h('div', { class: 'scopebox' }, canvas), ctl, h('div', { style: 'height:10px' }), chRow, h('div', { style: 'height:8px' }), tbRow,
    h('h2', {}, 'Medidas'), meas, h('h2', {}, 'Captura'), h('p', { class: 'note' }, 'La pantalla del propio osciloscopio, tal cual.'), shotImg,
    h('h2', {}, 'Generador'), h('div', { class: 'card pad muted' }, 'El UNI-T UTG932E habla USBTMC por USB y espera un hub USB con alimentación: el puerto OTG de la placa no da 5 V.'));
  draw();
  clearTimeout(benchT); clearTimeout(waveT);
  await load();
  const tick = () => { if (location.hash !== '#banco') return; load(); benchT = setTimeout(tick, 1000); };
  benchT = setTimeout(tick, 1000);
  pollWave();
}

/* ---- MQTT ---- */
let mqttT;
async function pageMqtt() {
  let st = null, sel = null, field = null, topics = [];
  const numES = v => v == null || !isFinite(v) ? '--' : String(+(+v).toFixed(3)).replace('.', ',');
  const ago = ms => ms < 2000 ? 'recién' : ms < 60000 ? `hace ${Math.round(ms / 1000)} s` : ms < 3600000 ? `hace ${Math.round(ms / 60000)} min` : `hace ${Math.round(ms / 3600000)} h`;
  const STATE = { off: ['Apagado', ''], unconfigured: ['Sin configurar', ''], waiting_net: ['Sin red', 'warn'], connecting: ['Conectando…', 'warn'],
    refused: ['Rechazado', 'bad'], connected: ['Conectado', 'ok'], error: ['Sin conexión', 'bad'] };
  /* connection */
  const f = {};
  for (const k of ['host', 'port', 'user', 'pass', 'client_id', 'sub', 'keepalive']) f[k] = h('input', { type: k === 'pass' ? 'password' : 'text' });
  const en = h('input', { type: 'checkbox' });
  const stLine = h('div', { class: 'mid' }), stNote = h('div', { class: 'note' });
  const save = () => {
    const b = { host: f.host.value.trim(), port: +f.port.value || 1883, user: f.user.value.trim(), client_id: f.client_id.value.trim(),
      sub: f.sub.value.trim() || '#', keepalive: +f.keepalive.value || 30, enabled: en.checked };
    if (f.pass.value) b.pass = f.pass.value;
    post('mqtt/config', b).then(() => { toast('Guardado'); f.pass.value = ''; setTimeout(load, 400); }).catch(e => toast(e.message, true));
  };
  en.onchange = save;
  /* publish */
  const pt = h('input', { type: 'text', placeholder: 'zigbee2mqtt/kitchen_plug/set' });
  const pp = h('textarea', { rows: 3, placeholder: '{"state":"TOGGLE"}', style: 'font-family:ui-monospace,Menlo,monospace' });
  const pq = h('select', { style: 'width:auto' }, h('option', { value: 0 }, 'QoS 0'), h('option', { value: 1 }, 'QoS 1'));
  const pr = h('input', { type: 'checkbox' });
  const pub = () => post('mqtt/publish', { topic: pt.value.trim(), payload: pp.value, qos: +pq.value, retain: pr.checked })
    .then(r => toast(r.queued ? 'En cola: sale cuando haya conexión' : 'Publicado en ' + pt.value.trim())).catch(e => toast(e.message, true));
  /* topics */
  const filt = h('input', { type: 'search', placeholder: 'Filtrar tópicos', style: 'max-width:320px' });
  const count = h('span', { class: 'muted small' });
  const list = h('div', { class: 'card mqlist' }), detail = h('div', { class: 'card pad mqdet' });
  filt.oninput = () => drawList();
  function drawList() {
    const q = filt.value.trim().toLowerCase();
    const shown = topics.filter(t => !q || t.t.toLowerCase().includes(q)).sort((a, b) => a.t < b.t ? -1 : 1);
    count.textContent = `${shown.length} de ${topics.length} tópicos`;
    put(list, shown.slice(0, 400).map(t => h('div', { class: 'row click' + (t.t === sel ? ' sel' : ''), onclick: () => { sel = t.t; field = null; drawList(); loadTopic(); } },
      h('div', { class: 'grow' }, h('div', { class: 'mono' }, t.t), h('div', { class: 'muted small mono ell' }, t.k === 4 ? `binario · ${t.len} bytes` : t.p || '(vacío)')),
      t.r ? h('span', { class: 'pill' }, 'R') : null,
      h('div', { class: 'val small', style: 'width:92px' }, '×' + t.n, h('br'), ago(t.age)))),
      shown.length ? null : h('div', { class: 'row muted' }, topics.length ? 'Ningún tópico coincide.' : 'Todavía no llegó nada.'));
  }
  function chart(fl) {
    const c = h('canvas', { width: 900, height: 260, class: 'scope' });
    const g = c.getContext('2d'), W = c.width, H = c.height, v = fl.v;
    g.fillStyle = '#0b0b0e'; g.fillRect(0, 0, W, H);
    g.strokeStyle = '#26262a'; for (let i = 1; i < 4; i++) { g.beginPath(); g.moveTo(0, i * H / 4 + .5); g.lineTo(W, i * H / 4 + .5); g.stroke(); }
    if (v.length > 1) {
      let lo = Math.min(...v), hi = Math.max(...v); const pad = (hi - lo) * .12 || Math.abs(hi) * .05 + .5; lo -= pad; hi += pad;
      g.strokeStyle = '#a78bfa'; g.lineWidth = 3; g.beginPath();
      v.forEach((x, i) => { const px = i * W / (v.length - 1), py = H - (x - lo) / (hi - lo) * H; i ? g.lineTo(px, py) : g.moveTo(px, py); });
      g.stroke();
      g.fillStyle = '#8e8e93'; g.font = '20px -apple-system,sans-serif';
      g.fillText('máx ' + numES(Math.max(...v)), 8, 24); g.fillText('mín ' + numES(Math.min(...v)), 8, H - 10);
    }
    return c;
  }
  const pretty = s => { try { return JSON.stringify(JSON.parse(s), null, 2); } catch { return s; } };
  async function loadTopic() {
    if (!sel) { put(detail, h('div', { class: 'muted' }, 'Elegí un tópico de la lista.')); return; }
    let t;
    try { t = await api('mqtt/topic?t=' + encodeURIComponent(sel)); } catch (e) { put(detail, h('div', { class: 'muted' }, e.message)); return; }
    const fl = t.fields.find(x => x.name === field) || t.fields[0];
    put(detail, h('div', { class: 'mono', style: 'font-size:17px;word-break:break-all' }, t.topic),
      h('div', { class: 'btns', style: 'margin:10px 0' }, t.retained ? h('span', { class: 'pill', style: 'background:#6d28d9;color:#fff' }, 'Retenido') : null,
        h('span', { class: 'pill' }, 'QoS ' + t.qos), h('span', { class: 'pill' }, t.count + ' mensajes'), h('span', { class: 'pill' }, ago(t.age)), h('span', { class: 'pill' }, fmtBytes(t.len))),
      fl ? [h('div', { class: 'btns' }, t.fields.length > 1 || fl.name ? t.fields.map(x => h('button', { class: 'btn' + (x === fl ? ' pri' : ''), onclick: () => { field = x.name; loadTopic(); } }, x.name || 'valor')) : null),
        h('div', { class: 'big', style: 'color:#a78bfa;margin:6px 0' }, numES(fl.last)), h('div', { class: 'scopebox' }, chart(fl))] : null,
      h('h2', {}, 'Mensaje'), h('pre', { class: 'log', style: 'height:auto;max-height:420px' }, t.kind === 4 ? '(binario)' : pretty(t.payload) + (t.len > t.payload.length ? `\n… (recortado: ${fmtBytes(t.len)} en total)` : '')),
      h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => { pt.value = t.topic; pp.value = t.payload; pr.checked = t.retained; pt.focus(); } }, 'Publicar en este tópico')));
  }
  async function load() {
    try { st = await api('mqtt'); } catch { return; }
    const c = st.config, [name, cls] = STATE[st.state] || [st.state, ''];
    put(stLine, h('span', { class: cls }, '● '), name, c.host ? h('span', { class: 'muted small' }, `  ${c.host}:${c.port}`) : null);
    const s = st.stats;
    put(stNote, (st.error ? st.error + ' · ' : '') + `cliente ${st.client_id} · ${s.rx_msgs} recibidos · ${s.tx_msgs} enviados · ${fmtBytes(s.rx_bytes)} · ${s.connects} conexiones` + (s.ping_ms >= 0 && st.state === 'connected' ? ` · ping ${s.ping_ms} ms` : ''));
    for (const k of ['host', 'port', 'user', 'client_id', 'sub', 'keepalive']) if (!f[k].matches(':focus') && document.activeElement !== f[k] && !f[k].dataset.touched) f[k].value = c[k];
    f.pass.placeholder = c.has_pass ? '(guardada; escribí otra para cambiarla)' : 'sin contraseña';
    en.checked = c.enabled;
    try { topics = (await api('mqtt/topics')).topics; } catch {}
    drawList();
    if (sel) loadTopic();
  }
  for (const k in f) f[k].oninput = () => { f[k].dataset.touched = '1'; };
  const lab = (t, el) => [h('label', { class: 'f' }, t), el];
  put(main, h('h1', {}, 'MQTT'),
    h('div', { class: 'grid2' },
      h('div', { class: 'card pad' }, h('h2', { style: 'margin-top:0' }, 'Conexión'), stLine, stNote,
        h('div', { class: 'mqform' }, h('div', {}, lab('Broker', f.host)), h('div', {}, lab('Puerto', f.port))),
        h('div', { class: 'mqform' }, h('div', {}, lab('Usuario', f.user)), h('div', {}, lab('Contraseña', f.pass))),
        h('div', { class: 'mqform' }, h('div', {}, lab('ID de cliente (vacío: automático)', f.client_id)), h('div', {}, lab('Keepalive (s)', f.keepalive))),
        lab('Suscripción (separá con comas)', f.sub),
        h('div', { class: 'btns', style: 'margin-top:14px' }, h('button', { class: 'btn pri', onclick: () => { for (const k in f) delete f[k].dataset.touched; save(); } }, 'Guardar'),
          h('button', { class: 'btn', onclick: () => post('mqtt/reconnect').then(() => toast('Reconectando…')) }, 'Reconectar'),
          h('label', { class: 'muted' }, en, ' Encendido'))),
      h('div', { class: 'card pad' }, h('h2', { style: 'margin-top:0' }, 'Publicar'), lab('Tópico', pt), lab('Mensaje', pp),
        h('div', { class: 'btns', style: 'margin-top:14px' }, h('button', { class: 'btn pri', onclick: pub }, 'Publicar'), pq, h('label', { class: 'muted' }, pr, ' Retener')),
        h('p', { class: 'note' }, 'QoS 1 espera la confirmación del broker. Un mensaje vacío y retenido borra el retenido del tópico.'))),
    h('h2', {}, 'Tópicos'),
    h('div', { class: 'btns', style: 'margin-bottom:10px' }, filt, count, h('button', { class: 'btn', onclick: () => post('mqtt/clear').then(load) }, 'Vaciar la tabla')),
    h('div', { class: 'mqcols' }, list, detail));
  clearTimeout(mqttT);
  await load();
  loadTopic();
  const tick = () => { if (location.hash !== '#mqtt') return; load(); mqttT = setTimeout(tick, 1500); };
  mqttT = setTimeout(tick, 1500);
}

/* ---- Claude ---- */
let claudeT;
async function pageClaude() {
  const status = h('div', { class: 'card pad' });
  const signin = h('div', { class: 'card pad' });
  const fmtIn = s => { s = Math.max(0, Math.round(s)); return s >= 86400 ? `${Math.floor(s / 86400)} d ${Math.floor(s % 86400 / 3600)} h` : s >= 3600 ? `${Math.floor(s / 3600)} h ${Math.floor(s % 3600 / 60)} min` : `${Math.ceil(s / 60)} min`; };
  const bar = (name, w, now) => h('div', { class: 'row', style: 'flex-direction:column;align-items:stretch;gap:6px' },
    h('div', { class: 'btns', style: 'justify-content:space-between;flex-wrap:nowrap' },
      h('span', {}, name, ' ', h('b', {}, w ? w.pct.toFixed(0) + ' %' : '--')),
      h('span', { class: 'muted small' }, w && w.resets ? 'se renueva en ' + fmtIn(w.resets - now) : '')),
    h('div', { class: 'prog', style: 'height:12px;margin:0' }, h('div', { style: `width:${w ? Math.min(100, w.pct) : 0}%;background:${!w ? 'var(--dim)' : w.pct >= 90 ? 'var(--red)' : w.pct >= 75 ? 'var(--orange)' : '#d97757'}` })));
  async function load() {
    let st;
    try { st = await api('claude'); } catch { return; }
    const inn = st.state === 'ok' || st.state === 'error';
    put(status, h('div', { class: 'btns', style: 'justify-content:space-between' },
        h('div', {}, h('b', {}, inn ? 'Sesión iniciada' : st.state === 'expired' ? 'La sesión venció' : 'Sin sesión'),
          st.fetched ? h('span', { class: 'muted' }, st.now - st.fetched < 60 ? ' · recién leído' : ' · leído hace ' + fmtIn(st.now - st.fetched)) : null),
        inn || st.state === 'expired' ? h('button', { class: 'btn', onclick: async () => { if (confirm('¿Cerrar la sesión de Claude en la placa?')) { await post('claude/logout'); load(); } } }, 'Cerrar sesión') : null),
      st.error ? h('p', { class: 'bad small' }, st.error) : null,
      inn ? [bar('5 horas', st.five_hour, st.now), bar('Semana', st.seven_day, st.now),
        st.seven_day_opus ? bar('Semana, Opus', st.seven_day_opus, st.now) : null,
        st.seven_day_sonnet ? bar('Semana, Sonnet', st.seven_day_sonnet, st.now) : null,
        st.extra_enabled && st.extra_pct != null ? bar('Uso extra', { pct: st.extra_pct }, st.now) : null] : null);
    signin.style.display = inn ? 'none' : '';
    if (st.state === 'waiting_code' || st.state === 'exchanging') step2.style.display = '';   /* back from Claude's page in this same tab */
  }
  const code = h('input', { type: 'text', placeholder: 'Pegá acá el código (código#estado)', style: 'font-family:ui-monospace,Menlo,monospace' });
  const linkp = h('p', { class: 'note' });
  const step2 = h('div', { style: 'display:none;margin-top:14px' }, linkp,
    h('p', {}, '2. En la página de Claude iniciá sesión y autorizá. Te va a mostrar un código: copialo y pegalo acá.'),
    h('div', { class: 'btns', style: 'flex-wrap:nowrap' }, h('div', { style: 'flex:1' }, code),
      h('button', { class: 'btn pri', onclick: async () => {
        try { await post('claude/code', { code: code.value.trim() }); toast('Verificando…'); code.value = ''; setTimeout(load, 1500); setTimeout(load, 4000); }
        catch (e) { toast(e.message, true); }
      } }, 'Listo')));
  put(signin, h('h2', { style: 'margin-top:0' }, 'Iniciar sesión'),
    h('p', { class: 'note' }, 'La placa tiene su propio inicio de sesión con tu cuenta de Claude: no usa ni toca el de Claude Code en ninguna computadora, y los números son los de la cuenta. Los toma del mismo lugar que el /usage de Claude Code, que no es una API pública de Anthropic: puede cambiar sin aviso.'),
    h('p', {}, '1. Abrí la página de inicio de sesión de Claude.'),
    h('button', { class: 'btn pri', onclick: async () => {
      try {
        const r = await post('claude/login');
        put(linkp, 'Si no se abrió una pestaña nueva: ', h('a', { href: r.url, target: '_blank', rel: 'noopener', id: 'claude-login' }, 'abrí la página de Claude acá'), '.');
        window.open(r.url, '_blank', 'noopener'); step2.style.display = ''; code.focus();
      }
      catch (e) { toast(e.message, true); }
    } }, 'Iniciar sesión con Claude'), step2);
  put(main, h('h1', {}, 'Claude'), status, h('div', { style: 'height:16px' }), signin);
  clearTimeout(claudeT);
  await load();
  const tick = () => { if (location.hash !== '#claude') return; load(); claudeT = setTimeout(tick, 5000); };
  claudeT = setTimeout(tick, 5000);
}

/* ---- Macro pad ---- */
/* The layout (aos_macropad.h) edited whole here and sent back with Guardar.
 * The glyphs are Material Design Icons, the same set the board draws with:
 * the browser takes the webfont from jsdelivr when it can, and shows the
 * names when it cannot (the board never needs it). */
const MP_TYPES = [['key', 'Atajo de teclado'], ['text', 'Escribir un texto'], ['seq', 'Secuencia'], ['ha', 'Home Assistant'], ['mqtt', 'Publicar MQTT'], ['app', 'Abrir una app']];
const MP_SWATCHES = ['#0A84FF', '#5E5CE6', '#BF5AF2', '#FF375F', '#FF453A', '#FF9F0A', '#FFD60A', '#30D158', '#10B981', '#40C8E0', '#0E7490', '#A2845E', '#D97757', '#8E8E93', '#3A3A3C', '#F2F2F7'];
const MP_KEYS = ['cmd+c', 'cmd+v', 'cmd+x', 'cmd+z', 'cmd+shift+z', 'cmd+s', 'cmd+space', 'cmd+tab', 'cmd+shift+4', 'cmd+shift+3', 'ctrl+cmd+q',
  'cmd+w', 'cmd+t', 'ctrl+alt+t', 'ctrl+c', 'ctrl+v', 'alt+tab', 'alt+f4', 'win+l', 'win+d', 'ctrl+]', 'play', 'next', 'prev', 'mute', 'volup', 'voldown',
  'brightup', 'brightdown', 'f5', 'enter', 'esc', 'tab', 'up', 'down', 'left', 'right', 'pgup', 'pgdn', 'home', 'end'];
const mpIcon = g => 'mdi mdi-' + String(g || '').toLowerCase().replace(/_/g, '-');
const mpInk = hex => { const v = parseInt(String(hex || '#3A3A3C').slice(1), 16) || 0; return (0.299 * (v >> 16 & 255) + 0.587 * (v >> 8 & 255) + 0.114 * (v & 255)) > 170 ? '#111' : '#fff'; };
const mpWhat = b => !b ? '' : b.type === 'key' ? b.key : b.type === 'text' ? '“' + (b.text || '') + '”' : b.type === 'seq' ? (b.seq || '').split('\n').filter(l => l.trim() && !l.trim().startsWith('#')).length + ' pasos'
  : b.type === 'ha' ? (b.service ? b.service + ' ' : '') + (b.entity || '') : b.type === 'mqtt' ? (b.topic || '') + ' ← ' + (b.payload || '') : b.type === 'app' ? b.app : '';
let mpT, mpIconsOk = false;

async function pageMacropad() {
  let doc = null, glyphs = [], page = 0, sel = -1, dirty = false, version = 0, iconsOk = mpIconsOk, ents = [], apps = [], dragFrom = -1;
  /* the webfont, once its stylesheet is in (fonts.load says nothing before) */
  const probeIcons = () => document.fonts.load('24px "Material Design Icons"', '\u{F018F}').then(f => {
    if (f.length && !iconsOk) { iconsOk = mpIconsOk = true; if (doc) render(); } }).catch(() => {});
  let link = document.getElementById('mdi-css');
  if (!link) {
    link = h('link', { id: 'mdi-css', rel: 'stylesheet', href: 'https://cdn.jsdelivr.net/npm/@mdi/font@7.4.47/css/materialdesignicons.min.css' });
    link.addEventListener('load', probeIcons);
    document.head.append(link);
  } else probeIcons();
  const statusBox = h('div', { class: 'card' });
  const tabs = h('div', { class: 'btns mptabs' });
  const grid = h('div', { class: 'mpgrid' });
  const form = h('div', { class: 'card pad mpform' });
  const saveBtn = h('button', { class: 'btn pri', disabled: true, onclick: () => save() }, 'Guardar');
  const undoBtn = h('button', { class: 'btn', disabled: true, onclick: () => { dirty = false; load(true); } }, 'Descartar cambios');
  const setDirty = () => { dirty = true; saveBtn.disabled = undoBtn.disabled = false; saveBtn.textContent = 'Guardar cambios'; };
  const pages = () => doc.pages;
  const buttons = () => { const p = pages()[page]; if (!p.buttons) p.buttons = []; return p.buttons; };
  const btnAt = i => buttons()[i] || null;
  const setBtn = (i, b) => { const a = buttons(); while (a.length <= i) a.push(null); a[i] = b; while (a.length && !a[a.length - 1]) a.pop(); };
  const icon = (g, cls) => iconsOk ? h('i', { class: mpIcon(g) + ' ' + (cls || '') }) : h('span', { class: 'mpgname ' + (cls || '') }, String(g || '').toLowerCase().replace(/_/g, ' '));

  async function load(force) {
    let r;
    try { r = await api('macropad'); } catch (e) { put(statusBox, h('span', { class: 'bad' }, e.message)); return; }
    showStatus(r);
    if (dirty && !force) return;
    if (!force && doc && r.version === version) return;
    doc = r.layout; glyphs = r.glyphs; version = r.version;
    if (page >= pages().length) page = pages().length - 1;
    dirty = false; saveBtn.disabled = undoBtn.disabled = true; saveBtn.textContent = 'Guardar';
    render();
  }
  function showStatus(r) {
    const usb = { ready: ['La computadora tiene el teclado y el mouse', 'ok'], busy: ['El USB se está preparando…', 'warn'],
      waiting: ['Esperando a la computadora: conectá el puerto OTG', 'warn'], off: ['USB apagado: se prende al abrir el Macro pad o al tocar un atajo', ''] }[r.usb] || ['?', ''];
    put(statusBox, h('div', { class: 'row' }, h('div', { class: 'grow' }, h('b', {}, 'USB · '), h('span', { class: usb[1] }, usb[0])),
        h('div', { class: 'muted small' }, r.saved ? 'en la tarjeta: macropad.json' : h('span', { class: 'warn' }, 'sin tarjeta: sólo en memoria'))),
      r.status && r.status.msg ? h('div', { class: 'row' }, h('div', { class: 'grow' }, 'Último envío: ', h('span', { class: r.status.ok ? 'ok' : 'bad' }, r.status.msg))) : null);
  }
  async function save() {
    try {
      await api('macropad', { method: 'PUT', body: JSON.stringify(doc, null, 1) });
      dirty = false; toast('Guardado: la placa ya lo muestra');
      await load(true);
    } catch (e) { toast(e.message, true); }
  }

  function render() {
    renderTabs(); renderGrid(); renderForm();
  }
  function renderTabs() {
    const ps = pages();
    put(tabs, ps.map((p, i) => h('button', { class: 'btn' + (i === page ? ' pri' : ''), onclick: () => { page = i; sel = -1; render(); } }, p.name || '(sin nombre)')),
      ps.length < 8 ? h('button', { class: 'btn', onclick: () => { ps.push({ name: 'Página ' + (ps.length + 1), buttons: [] }); page = ps.length - 1; sel = -1; setDirty(); render(); } }, '+ Página') : null);
  }
  function tile(i) {
    const b = btnAt(i);
    const t = h('div', { class: 'mptile' + (b ? '' : ' empty') + (i === sel ? ' sel' : ''), draggable: b ? 'true' : 'false',
      onclick: () => { sel = i; if (!b) { setBtn(i, { label: '', glyph: 'KEYBOARD', color: '#0A84FF', type: 'key', key: '' }); setDirty(); } render(); } });
    if (b) {
      t.style.background = b.color || '#3A3A3C';
      t.style.color = mpInk(b.color);
      t.append(icon(b.glyph, 'mpglyph'), h('div', { class: 'mplabel' }, b.label || '(sin nombre)'), h('div', { class: 'mpwhat' }, mpWhat(b)));
    } else t.append(h('div', { class: 'mpplus' }, '+'));
    t.addEventListener('dragstart', e => { dragFrom = i; e.dataTransfer.effectAllowed = 'move'; });
    t.addEventListener('dragover', e => { e.preventDefault(); t.classList.add('over'); });
    t.addEventListener('dragleave', () => t.classList.remove('over'));
    t.addEventListener('drop', e => {
      e.preventDefault(); t.classList.remove('over');
      if (dragFrom < 0 || dragFrom === i) return;
      const arr = buttons();
      while (arr.length < 15) arr.push(null);
      [arr[i], arr[dragFrom]] = [arr[dragFrom], arr[i]];
      while (arr.length && !arr[arr.length - 1]) arr.pop();
      sel = i; dragFrom = -1; setDirty(); render();
    });
    return t;
  }
  function renderGrid() {
    const p = pages()[page];
    put(grid, [...Array(15).keys()].map(tile));
    if (p.auto === 'ha' && !buttons().length)
      grid.prepend(h('div', { class: 'note', style: 'grid-column:1/-1' }, 'Esta página se llena sola con las escenas y luces de Home Assistant la primera vez que la placa conecte con HA. Si agregás botones a mano, deja de hacerlo.'));
  }

  function field(label, input, note) { return h('div', {}, h('label', { class: 'f' }, label), input, note ? h('div', { class: 'note' }, note) : null); }
  function renderForm() {
    const p = pages()[page];
    const pageTools = h('div', {},
      h('h2', { style: 'margin-top:0' }, 'Página'),
      field('Nombre', h('input', { type: 'text', value: p.name || '', maxlength: 20, oninput: e => { p.name = e.target.value; setDirty(); renderTabs(); } })),
      h('div', { class: 'btns', style: 'margin-top:12px' },
        h('button', { class: 'btn', disabled: page === 0, onclick: () => { const ps = pages(); [ps[page - 1], ps[page]] = [ps[page], ps[page - 1]]; page--; setDirty(); render(); } }, '← Mover'),
        h('button', { class: 'btn', disabled: page === pages().length - 1, onclick: () => { const ps = pages(); [ps[page + 1], ps[page]] = [ps[page], ps[page + 1]]; page++; setDirty(); render(); } }, 'Mover →'),
        h('button', { class: 'btn red', disabled: pages().length < 2, onclick: () => { if (!confirm('¿Borrar la página «' + p.name + '» con sus botones?')) return; pages().splice(page, 1); page = Math.max(0, page - 1); sel = -1; setDirty(); render(); } }, 'Borrar página')));
    const b = sel >= 0 ? btnAt(sel) : null;
    if (!b) { put(form, pageTools, h('p', { class: 'note', style: 'margin-top:22px' }, 'Tocá un botón para editarlo, o un lugar vacío para crear uno. Arrastrá los botones para cambiarlos de lugar.')); return; }
    const upd = () => { setDirty(); renderGrid(); };
    const inp = (k, ph, extra) => h('input', Object.assign({ type: 'text', value: b[k] || '', placeholder: ph || '', oninput: e => { b[k] = e.target.value; upd(); } }, extra || {}));
    /* the glyph picker: a search box over the grid of icons */
    const gsearch = h('input', { type: 'search', placeholder: 'Buscar un ícono…' });
    const gbox = h('div', { class: 'mpglyphs' });
    const fillGlyphs = () => put(gbox, glyphs.filter(g => g.toLowerCase().includes(gsearch.value.toLowerCase().replace(/[ -]/g, '_'))).map(g =>
      h('button', { class: 'mpg' + (g === b.glyph ? ' on' : ''), title: g.toLowerCase().replace(/_/g, '-'), onclick: () => { b.glyph = g; upd(); fillGlyphs(); } }, icon(g))));
    gsearch.addEventListener('input', fillGlyphs);
    fillGlyphs();
    const color = h('input', { type: 'color', value: (b.color || '#0A84FF').toLowerCase(), oninput: e => { b.color = e.target.value.toUpperCase(); upd(); } });
    const swatches = h('div', { class: 'swatches' }, MP_SWATCHES.map(c => h('div', { class: 'mpsw' + ((b.color || '').toUpperCase() === c ? ' on' : ''), style: 'background:' + c,
      onclick: () => { b.color = c; color.value = c.toLowerCase(); upd(); renderForm(); } })));
    const typeSel = h('select', { onchange: e => { b.type = e.target.value; upd(); renderForm(); } }, MP_TYPES.map(([v, n]) => h('option', { value: v, selected: b.type === v }, n)));
    let params;
    if (b.type === 'key') {
      const valid = h('div', { class: 'note' });
      const check = async () => {
        if (!b.key) { valid.textContent = 'Modificadores cmd, ctrl, alt, shift con + y una tecla. Medios: play, next, prev, mute, volup, voldown.'; valid.className = 'note'; return; }
        try { const r = await api('macropad/key?name=' + encodeURIComponent(b.key)); valid.textContent = r.valid ? 'La placa conoce esa tecla.' : 'La placa no conoce esa tecla.'; valid.className = 'note ' + (r.valid ? 'ok' : 'bad'); } catch {}
      };
      const k = inp('key', 'cmd+shift+4', { list: 'mp-keys', oninput: e => { b.key = e.target.value.trim(); upd(); check(); } });
      params = [field('Combinación', k), h('datalist', { id: 'mp-keys' }, MP_KEYS.map(v => h('option', { value: v }))), valid];
      check();
    } else if (b.type === 'text') {
      params = [field('Texto', h('textarea', { rows: 3, oninput: e => { b.text = e.target.value; upd(); } }, b.text || ''), 'Se escribe como un teclado de EE. UU.: los acentos y la ñ no llegan.')];
    } else if (b.type === 'seq') {
      params = [field('Pasos', h('textarea', { rows: 9, class: 'mono', spellcheck: 'false', oninput: e => { b.seq = e.target.value; upd(); } }, b.seq || ''),
        h('span', {}, 'Una orden por línea: ', h('code', {}, 'STRING texto'), ' · ', h('code', {}, 'KEY cmd+c'), ' · ', h('code', {}, 'DELAY 300'), ' · ', h('code', {}, 'MOUSE dx dy'), ' · ',
          h('code', {}, 'SCROLL n'), ' · ', h('code', {}, 'CLICK 1|2'), ' · ', h('code', {}, 'REPEAT n'), ' · ', h('code', {}, 'HA entidad'), ' o ', h('code', {}, 'HA dominio.servicio entidad {json}'),
          ' · ', h('code', {}, 'MQTT tópico mensaje'), ' · ', h('code', {}, 'OPEN aos.bench'), '. Las líneas con # son comentarios.'))];
    } else if (b.type === 'ha') {
      const dom = (b.entity || '').split('.')[0];
      const svcs = dom ? [dom + '.toggle', dom + '.turn_on', dom + '.turn_off'].concat(dom === 'scene' || dom === 'script' ? [dom + '.turn_on'] : dom === 'cover' ? ['cover.open_cover', 'cover.close_cover'] : dom === 'button' ? ['button.press'] : dom === 'media_player' ? ['media_player.media_play_pause', 'media_player.volume_up', 'media_player.volume_down'] : []) : [];
      params = [field('Entidad', inp('entity', 'light.taller', { list: 'mp-ents', onchange: e => {
          const en = ents.find(x => x.id === e.target.value);
          if (en && !b.label) { b.label = en.name; }
          const G = { light: 'LIGHTBULB', switch: 'TOGGLE_SWITCH', fan: 'FAN', cover: 'BLINDS', scene: 'AUTO_FIX', script: 'SCRIPT_TEXT', lock: 'LOCK', media_player: 'PLAY_PAUSE', climate: 'THERMOMETER', sensor: 'GAUGE' };
          if (en && G[en.domain]) b.glyph = G[en.domain];
          upd(); renderForm();
        } }), ents.length ? null : 'Home Assistant no está conectado: la lista de entidades aparece cuando la placa lo tenga.'),
        h('datalist', { id: 'mp-ents' }, ents.map(e => h('option', { value: e.id }, e.name + (e.area ? ' · ' + e.area : '')))),
        field('Servicio', inp('service', 'vacío: lo de siempre (prender/apagar, abrir, ejecutar)', { list: 'mp-svc' })), h('datalist', { id: 'mp-svc' }, [...new Set(svcs)].map(v => h('option', { value: v }))),
        field('Datos del servicio (JSON, opcional)', inp('data', '{"brightness_pct": 40}', { class: 'mono' })),
        h('div', { class: 'note' }, 'El botón muestra el estado de la entidad: una luz prendida se ve llena de color; un sensor, su valor.')];
    } else if (b.type === 'mqtt') {
      params = [field('Tópico', inp('topic', 'cmnd/soldador/POWER', { class: 'mono' })), field('Mensaje', inp('payload', 'TOGGLE', { class: 'mono' })),
        field('Tópico de estado (opcional)', inp('state', 'stat/soldador/POWER', { class: 'mono' }), 'Si lo tiene, el botón se prende y apaga con lo que llegue ahí (ON/OFF, 1/0, {"state":"ON"}).'),
        h('div', { class: 'btns', style: 'margin-top:12px' },
          h('label', {}, h('input', { type: 'checkbox', checked: !!b.retain, onchange: e => { b.retain = e.target.checked; upd(); } }), ' Retenido'),
          h('label', { style: 'display:flex;align-items:center;gap:8px' }, 'QoS', h('select', { style: 'width:auto', onchange: e => { b.qos = +e.target.value; upd(); } }, h('option', { value: 0, selected: !b.qos }, '0'), h('option', { value: 1, selected: b.qos === 1 }, '1'))))];
    } else if (b.type === 'app') {
      params = [field('App', h('select', { onchange: e => { b.app = e.target.value; const a = apps.find(x => x.id === b.app); if (a && !b.label) b.label = a.name; upd(); renderForm(); } },
        h('option', { value: '' }, 'Elegí una app…'), apps.filter(a => a.id !== 'aos.macropad').sort((x, y) => x.name.localeCompare(y.name)).map(a => h('option', { value: a.id, selected: a.id === b.app }, a.name))))];
    }
    put(form,
      h('div', { class: 'btns', style: 'justify-content:space-between' }, h('h2', { style: 'margin:0' }, 'Botón ' + (sel + 1)),
        h('div', { class: 'btns' },
          h('button', { class: 'btn', onclick: async () => {
            try { if (dirty) await save(); await api(`macropad/run?page=${page}&slot=${sel}`, { method: 'POST' }); setTimeout(() => load(), 500); setTimeout(() => load(), 1500); }
            catch (e) { toast(e.message, true); }
          } }, 'Probar en la placa'),
          h('button', { class: 'btn', onclick: () => { const i = [...Array(15).keys()].find(k => !btnAt(k)); if (i == null) { toast('La página está llena', true); return; } setBtn(i, JSON.parse(JSON.stringify(b))); sel = i; setDirty(); render(); } }, 'Duplicar'),
          h('button', { class: 'btn red', onclick: () => { setBtn(sel, null); sel = -1; setDirty(); render(); } }, 'Borrar'))),
      field('Nombre', inp('label', 'Copiar', { maxlength: 30 })),
      field('Acción', typeSel), params,
      h('label', { class: 'f' }, 'Color'), h('div', { class: 'btns' }, swatches, color),
      h('label', { class: 'f' }, 'Ícono'), gsearch, gbox,
      h('div', { style: 'height:18px' }), pageTools);
  }
  function rawEditor() {
    const ta = h('textarea', { spellcheck: 'false' });
    ta.value = JSON.stringify(doc, null, 2);
    const close = () => m.remove();
    const m = h('div', { class: 'modal', onclick: e => { if (e.target === m) close(); } },
      h('div', { class: 'box' }, h('div', { class: 'head' }, h('b', { class: 'grow' }, 'macropad.json')), ta,
        h('div', { class: 'foot' }, h('button', { class: 'btn', onclick: close }, 'Cancelar'),
          h('button', { class: 'btn pri', onclick: async () => {
            try { await api('macropad', { method: 'PUT', body: ta.value }); toast('Guardado'); close(); dirty = false; load(true); } catch (e) { toast(e.message, true); }
          } }, 'Guardar'))));
    document.body.append(m);
    ta.focus();
  }
  put(main, h('h1', {}, 'Macro pad'), statusBox,
    h('div', { class: 'btns', style: 'margin:18px 0 12px;justify-content:space-between' }, tabs,
      h('div', { class: 'btns' }, undoBtn, saveBtn,
        h('button', { class: 'btn', onclick: () => rawEditor() }, 'JSON'),
        h('button', { class: 'btn', onclick: async () => { if (!confirm('¿Volver a la botonera de fábrica? Se pierde la actual.')) return; await post('macropad/reset'); dirty = false; load(true); } }, 'De fábrica'))),
    h('div', { class: 'mpcols' }, h('div', {}, grid, h('p', { class: 'note' }, 'En la placa: 3×5 parada, 5×3 acostada, en este mismo orden. Arrastrá para mover; el cambio llega a la placa al guardar.')), form));
  api('ha/entities').then(l => { ents = l.sort((a, b) => a.id.localeCompare(b.id)); if (doc && sel >= 0) renderForm(); }).catch(() => {});
  api('apps').then(l => { apps = l; if (doc && sel >= 0) renderForm(); }).catch(() => {});
  window.onbeforeunload = () => dirty && location.hash === '#macropad' ? 'Hay cambios sin guardar' : undefined;
  await load(true);
  /* ?mp=<page>.<slot> opens that button (a link to one, or a screenshot) */
  const want = new URLSearchParams(location.search).get('mp');
  if (want && doc) {
    const [pg, sl] = want.split('.').map(Number);
    if (pages()[pg]) { page = pg; sel = btnAt(sl) ? sl : -1; render(); }
  }
  clearTimeout(mpT);
  const tick = () => { if (location.hash !== '#macropad') return; load(); mpT = setTimeout(tick, 3000); };
  mpT = setTimeout(tick, 3000);
}

/* ---- Expansión ---- */
const XP_CLS = { '5v': ['#dc2626', '5 V'], '3v3': ['#f97316', '3V3'], gnd: ['#3f3f46', 'GND'], free: ['#16a34a', 'libre'],
  adc: ['#0d9488', 'ADC'], port: ['#2563eb', 'en un puerto'], taken: ['#9333ea', 'tomado'], board: ['#db2777', 'de la placa'],
  care: ['#a16207', 'con cuidado'], rsv: ['#52525b', 'reservado'] };
const xpNum = (v, d) => v.toFixed(d).replace('.', ',');
function xpFmt(q, v) {
  if (v == null) return '--';
  const a = Math.abs(v);
  switch (q) {
    case 0: return xpNum(v, 1) + ' °C';
    case 1: return xpNum(v, 1) + ' %';
    case 2: return xpNum(v, 1) + ' hPa';
    case 3: return xpNum(v, a < 100 ? 1 : 0) + ' lx';
    case 4: return a < 1 ? xpNum(v * 1000, 1) + ' mV' : xpNum(v, 3) + ' V';
    case 5: return a < 1 ? xpNum(v * 1000, 1) + ' mA' : xpNum(v, 3) + ' A';
    case 6: return a < 1 ? xpNum(v * 1000, 0) + ' mW' : xpNum(v, 2) + ' W';
    default: return String(v);
  }
}
const XP_SPAN = [1, 4, 1, 20, 0.05, 0.01, 0.05];
let xpT, xpSel = null, xpPin = null;
async function pageExpansion() {
  let ex = null, se = null, loaded = null, hist = [], histFor = '';
  const dirty = () => loaded !== null && ta.value !== loaded;
  const hex = a => '0x' + a.toString(16).toUpperCase().padStart(2, '0');
  /* header */
  const hdr = h('div', { class: 'xp-hdr' }), pinInfo = h('div', { class: 'card pad xp-pin' });
  const legend = h('div', { class: 'xp-leg' });
  /* sensors */
  const chTitle = h('div', { class: 'muted small' }), chVal = h('div', { class: 'big' }), chRange = h('div', { class: 'note' });
  const canvas = h('canvas', { class: 'xp-chart', width: 1100, height: 260 });
  const portSt = h('div', { class: 'note' }), sens = h('div', { class: 'xp-sens' }), cands = h('div');
  const rescan = h('button', { class: 'btn', onclick: () => post('sensors/rescan').then(() => toast('Buscando sensores…')).catch(e => toast(e.message, true)) }, 'Buscar de nuevo');
  /* ports */
  const ports = h('div', { class: 'grid2' });
  /* modules.txt */
  const ta = h('textarea', { class: 'xp-ta', spellcheck: 'false' }), ck = h('div', { class: 'grow small' });
  const src = h('div', { class: 'note' });
  const save = h('button', { class: 'btn pri' }, 'Guardar');
  const revert = h('button', { class: 'btn' }, 'Descartar cambios');
  const fKind = h('select', { style: 'width:auto' }), fPort = h('select', { style: 'width:auto' }), fAddr = h('select', { style: 'width:auto' }), fOpt = h('select', { style: 'width:auto' });
  const fLine = h('code', { class: 'mono xp-line' });
  const fAdd = h('button', { class: 'btn' }, 'Agregar al texto');

  function freeName(base) {
    const used = new Set((ex ? ex.modules : []).map(m => m.name));
    ta.value.split('\n').forEach(l => { const m = l.trim().match(/^module\s+(\S+)/); if (m) used.add(m[1]); });
    if (!used.has(base)) return base;
    for (let k = 2; ; k++) if (!used.has(base + '_' + k)) return base + '_' + k;
  }
  let ckT;
  function check() {
    clearTimeout(ckT);
    ckT = setTimeout(async () => {
      try { showCheck(await api('expansion/check', { method: 'POST', body: ta.value })); } catch (e) { ck.textContent = e.message; }
    }, 250);
  }
  function showCheck(c) {
    if (!c.ok) { ck.className = 'grow small bad'; ck.textContent = c.error; save.disabled = true; return; }
    save.disabled = false;
    if (c.warnings.length) { ck.className = 'grow small warn'; ck.textContent = 'Se puede guardar, pero: ' + c.warnings.map(w => w.msg).join(' · '); }
    else { ck.className = 'grow small ok'; ck.textContent = `Bien: ${c.ports} puertos y ${c.modules} módulos`; }
  }
  ta.oninput = () => check();
  ta.addEventListener('keydown', e => { if ((e.metaKey || e.ctrlKey) && e.key === 's') { e.preventDefault(); doSave(); } });
  async function doSave() {
    try {
      const r = await api('expansion/save', { method: 'POST', body: ta.value });
      showCheck(r);
      if (!r.saved) { toast(r.error || 'No se guardó', true); return; }
      toast(`Guardado: ${r.ports} puertos, ${r.modules} módulos`);
      loadEx(true);
    } catch (e) { toast(e.message, true); }
  }
  save.onclick = doSave;
  revert.onclick = () => loadEx(true);

  /* the add form */
  function formFill(keep) {
    if (!ex) return;
    const k0 = keep ? fKind.value : '';
    put(fKind, ex.chips.map(c => h('option', { value: c.id }, c.name)), h('option', { value: 'rs485' }, 'RS485'));
    if (k0) fKind.value = k0;
    formKind();
  }
  function formKind() {
    const rs = fKind.value === 'rs485', c = ex.chips.find(x => x.id === fKind.value);
    const kinds = ex.ports.filter(p => p.kind === (rs ? 'uart' : 'i2c'));
    const p0 = fPort.value;
    put(fPort, kinds.map(p => h('option', { value: p.name }, p.name)));
    if (kinds.some(p => p.name === p0)) fPort.value = p0;
    fAddr.style.display = rs ? 'none' : '';
    if (!rs) put(fAddr, c.addrs.map(a => h('option', { value: a }, hex(a))));
    const opts = rs ? [[9600, '9600'], [19200, '19200'], [38400, '38400'], [115200, '115200']]
      : c.shunt ? [[100, 'shunt 0,1 Ω (R100)'], [50, 'shunt 0,05 Ω (R050)'], [10, 'shunt 0,01 Ω (R010)']]
      : c.gain ? [[4096, '±4,096 V'], [6144, '±6,144 V'], [2048, '±2,048 V'], [1024, '±1,024 V']] : [];
    fOpt.style.display = opts.length ? '' : 'none';
    put(fOpt, opts.map(([v, n]) => h('option', { value: v }, n)));
    formLine();
  }
  function formLine() {
    const rs = fKind.value === 'rs485', c = ex.chips.find(x => x.id === fKind.value);
    if (!fPort.value) { fLine.textContent = rs ? 'No hay puertos UART.' : 'No hay puertos I2C.'; fAdd.disabled = true; return; }
    fAdd.disabled = false;
    let l = `module ${freeName(rs ? 'rs485' : c.id)} ${fPort.value}`;
    if (rs) l += ` baud=${fOpt.value}`;
    else {
      l += ` addr=${hex(+fAddr.value)}`;
      if (c.shunt) l += ` shunt=${fOpt.value}`;
      if (c.gain) l += ` gain=${fOpt.value}`;
    }
    fLine.textContent = l;
  }
  fKind.onchange = formKind; fPort.onchange = formLine; fAddr.onchange = formLine; fOpt.onchange = formLine;
  fAdd.onclick = () => { ta.value = ta.value.replace(/\n*$/, '\n') + fLine.textContent + '\n'; check(); formLine(); ta.scrollTop = ta.scrollHeight; };

  function pinShow(p) {
    xpPin = p.pin;
    const [col, name] = XP_CLS[p.cls] || ['#555', p.cls];
    const mods = p.port ? ex.modules.filter(m => m.port === p.port).map(m => m.name) : [];
    put(pinInfo, h('div', { class: 'mid' }, `Pin ${p.pin} · ${p.label}`),
      h('div', { class: 'btns', style: 'margin:8px 0' }, h('span', { class: 'pill', style: `background:${col};color:#fff` }, name), p.flags.map(f => h('span', { class: 'pill' }, f))),
      row('GPIO', p.gpio >= 0 ? 'GPIO' + p.gpio : 'no es un GPIO'),
      p.note ? row('Qué es', p.note) : null,
      p.gpio >= 0 ? row('Lo tiene', p.owner || 'nadie, ahora') : null,
      p.gpio >= 0 ? row('Puerto', p.port ? `${p.port} · ${p.role}` : 'ninguno en modules.txt') : null,
      p.port ? row('Módulos', mods.join(', ') || 'ninguno') : null);
  }
  function drawHeader() {
    const cell = (p, side) => {
      const [col] = XP_CLS[p.cls] || ['#555'];
      const sub = p.owner || (p.port ? `${p.port} ${p.role}` : p.gpio < 0 ? '' : p.flags.join(', '));
      const lab = h('div', { class: 'xp-lab ' + side + (p.pin === xpPin ? ' sel' : ''), onclick: () => { pinShow(p); drawHeader(); } },
        h('div', { class: p.cls === 'gnd' ? 'muted' : '' }, p.label), sub ? h('div', { class: 'xp-sub ' + p.cls }, sub) : null);
      const pad = h('div', { class: 'xp-pad' + (p.pin === 1 ? ' one' : '') + (p.pin === xpPin ? ' sel' : ''), style: `background:${col}`, title: `Pin ${p.pin}`, onclick: () => { pinShow(p); drawHeader(); } },
        p.gpio >= 0 ? String(p.gpio) : p.label.replace('USB ', ''));
      const num = h('div', { class: 'xp-num' }, String(p.pin));
      return side === 'l' ? [lab, num, pad] : [pad, num, lab];
    };
    const rows = [];
    /* as on the board: the even pin on the left, the odd one (5 V's column) on the right */
    for (let r = 0; r < 20; r++) rows.push(cell(ex.header[r * 2 + 1], 'l'), cell(ex.header[r * 2], 'r'));
    put(hdr, h('div', { class: 'xp-body' }), rows);
    const count = {};
    ex.header.forEach(p => { count[p.cls] = (count[p.cls] || 0) + 1; });
    put(legend, Object.entries(XP_CLS).filter(([k]) => count[k]).map(([k, [c, n]]) => h('span', { class: 'xp-key' }, h('i', { style: `background:${c}` }), `${n} ${count[k]}`)));
  }
  function drawPorts() {
    put(ports, ex.ports.map(p => {
      const mods = ex.modules.filter(m => m.port === p.name);
      const det = se ? se.sensors.filter(s => !s.declared && s.port === p.name) : [];
      const kind = p.kind === 'uart' ? `UART · ${p.freq}` : p.kind === 'i2c' ? `I2C · ${p.freq / 1000} kHz` : p.kind === 'spi' ? `SPI · ${p.freq / 1e6} MHz` : 'GPIO';
      return h('div', { class: 'card' },
        h('div', { class: 'row' }, h('b', { class: 'grow', style: 'font-size:19px' }, p.name), h('span', { class: 'muted' }, kind)),
        h('div', { class: 'row' }, h('div', { class: 'btns' }, p.pins.map(x => h('span', { class: 'pill' }, `${x.role} GPIO${x.gpio} · pin ${x.pin}`)))),
        p.owner ? h('div', { class: 'row warn small' }, 'Ahora lo usa: ' + p.owner) : null,
        mods.map(m => h('div', { class: 'row' }, h('div', { class: 'grow' }, h('b', {}, m.name), m.chip && m.chip.toLowerCase() !== m.name ? h('span', { class: 'muted' }, '  ' + m.chip) : null),
          h('code', { class: 'mono muted' }, m.args))),
        !mods.length ? h('div', { class: 'row muted small' }, 'Sin módulos declarados en este puerto.') : null,
        det.length ? h('div', { class: 'row small', style: 'color:#93c5fd' }, 'Detectados ahora: ' + det.map(s => `${s.chip} ${hex(s.addr)}`).join(', ')) : null);
    }));
  }
  async function loadEx(text) {
    try { ex = await api('expansion'); } catch (e) { put(hdr, h('p', { class: 'bad' }, e.message)); return; }
    drawHeader(); drawPorts();
    if (xpPin) { const p = ex.header.find(x => x.pin === xpPin); if (p) pinShow(p); }
    else put(pinInfo, h('div', { class: 'muted' }, 'Tocá un pin para ver qué es, quién lo tiene y a qué puerto pertenece.'));
    if (text || !dirty()) {
      ta.value = loaded = ex.text || '';
      src.textContent = !ex.card ? 'No hay tarjeta: no se puede guardar.' : ex.file ? 'modules.txt, en la raíz de la tarjeta. Se aplica al guardar.'
        : 'No hay modules.txt en la tarjeta: esto es el perfil por defecto. Al guardar se crea el archivo.';
      if (ex.check) showCheck(ex.check);
      formFill(true);
    }
  }

  function drawChart() {
    const c = canvas.getContext('2d'), W = canvas.width, H = canvas.height;
    c.clearRect(0, 0, W, H);
    const vals = hist.filter(v => v != null);
    c.strokeStyle = getComputedStyle(document.body).getPropertyValue('--line'); c.lineWidth = 1;
    for (let i = 1; i < 4; i++) { c.beginPath(); c.moveTo(0, i * H / 4 + .5); c.lineTo(W, i * H / 4 + .5); c.stroke(); }
    if (!vals.length || !xpSel) return;
    let lo = Math.min(...vals), hi = Math.max(...vals);
    const span = hi - lo, ms = XP_SPAN[xpSel.q] || 1;
    if (span < ms) { const m = (hi + lo) / 2; lo = m - ms / 2; hi = m + ms / 2; } else { lo -= span * .1; hi += span * .1; }
    const N = 300, x = i => (N - hist.length + i) * W / (N - 1), y = v => H - (v - lo) / (hi - lo) * H;
    c.strokeStyle = '#94a3b8'; c.lineWidth = 3; c.beginPath();
    let pen = false;
    hist.forEach((v, i) => { if (v == null) { pen = false; return; } pen ? c.lineTo(x(i), y(v)) : c.moveTo(x(i), y(v)); pen = true; });
    c.stroke();
    c.fillStyle = '#8e8e93'; c.font = '20px -apple-system,sans-serif';
    c.fillText(xpFmt(xpSel.q, hi), 8, 22); c.fillText(xpFmt(xpSel.q, lo), 8, H - 8);
    chRange.textContent = `mín ${xpFmt(xpSel.q, Math.min(...vals))} · máx ${xpFmt(xpSel.q, Math.max(...vals))} · últimos ${hist.length >= 120 ? Math.floor(hist.length / 60) + ' min' : hist.length + ' s'}`;
  }
  async function loadHist() {
    if (!se || !xpSel) return;
    const i = se.sensors.findIndex(s => s.name === xpSel.name);
    if (i < 0) return;
    const k = se.sensors[i].values.findIndex(v => v.key === xpSel.key);
    if (k < 0) return;
    try { hist = (await api(`sensors/hist?i=${i}&k=${k}`)).v; } catch { return; }
    drawChart();
  }
  function drawSensors() {
    if (!xpSel || !se.sensors.some(s => s.name === xpSel.name && s.values.some(v => v.key === xpSel.key))) {
      const s = se.sensors.find(s => s.values.some(v => v.q === 0)) || se.sensors.find(s => s.values.length);
      xpSel = s ? { name: s.name, key: (s.values.find(v => v.q === 0) || s.values[0]).key, q: 0 } : null;
    }
    put(portSt, se.ports.map(p => h('div', {}, p.state === 'busy' ? `${p.port}: los pines los tiene ${p.owner}` : p.state === 'err' ? `${p.port}: no se pudo abrir`
      : `${p.port}: ${p.found} sensores` + (p.scan_age_ms != null ? ` · buscó hace ${Math.round(p.scan_age_ms / 1000)} s` : ''))));
    put(sens, se.sensors.length ? se.sensors.map(s => h('div', { class: 'card pad xp-s' },
      h('div', { class: 'btns', style: 'justify-content:space-between' }, h('b', { style: 'font-size:18px' }, s.name),
        h('span', { class: 'muted small' }, `${s.chip} · ${s.port} ${hex(s.addr)} · `, h('span', { style: s.declared ? '' : 'color:#93c5fd' }, s.declared ? 'modules.txt' : 'detectado'))),
      h('div', { class: 'xp-vals' }, s.values.map(v => h('div', { class: 'xp-v' + (xpSel && xpSel.name === s.name && xpSel.key === v.key ? ' sel' : ''),
        onclick: () => { xpSel = { name: s.name, key: v.key, q: v.q }; drawSensors(); loadHist(); } },
        h('div', { class: 'muted small' }, v.name), h('div', { class: 'mid' }, xpFmt(v.q, v.v))))),
      h('div', { class: 'small ' + (s.state === 'ok' ? 'muted' : 'bad') }, s.state === 'ok' ? `${s.reads} lecturas · ${s.errors} errores` : s.msg || 'Esperando la primera lectura'))) :
      h('div', { class: 'card pad muted' }, 'No hay sensores. Conectá uno a i2c.ext: SDA al GPIO21 (pin 15), SCL al GPIO22 (pin 17), 3V3 (pin 18) y GND (pin 19). Los BME280, SHT3x/4x, AHT20, INA219/226 y ADS1115 se reconocen solos.'));
    put(cands, se.candidates.length ? [h('h2', {}, 'Parece que hay'), h('div', { class: 'card' }, se.candidates.map(c => h('div', { class: 'row' },
      h('code', { class: 'mono', style: 'color:#f59e0b' }, hex(c.addr)), h('div', { class: 'grow' }, `¿Es un ${(ex && ex.chips.find(x => x.id === c.chip) || {}).name || c.chip}? `, h('span', { class: 'muted small' }, `${c.port} · ${c.guess || 'no dice qué es'}`)),
      h('button', { class: 'btn', onclick: async () => {
        ta.value = ta.value.replace(/\n*$/, '\n') + `module ${freeName(c.chip)} ${c.port} addr=${hex(c.addr)}\n`; await doSave();
      } }, 'Agregar')))),
      h('p', { class: 'note' }, 'Estos chips no tienen cómo decir qué son sin que alguien les escriba: agregalos a modules.txt si son lo que parecen.')] : []);
    if (xpSel) {
      const s = se.sensors.find(s => s.name === xpSel.name), v = s && s.values.find(v => v.key === xpSel.key);
      if (v) { xpSel.q = v.q; chTitle.textContent = `${s.name} · ${v.name}`; chVal.textContent = xpFmt(v.q, v.v); }
    } else { chTitle.textContent = 'Sin lecturas todavía'; chVal.textContent = '--'; chRange.textContent = ''; }
  }
  async function loadSensors() {
    try { se = await api('sensors'); } catch { return; }
    drawSensors();
    const key = xpSel ? xpSel.name + '/' + xpSel.key : '';
    if (key !== histFor) { histFor = key; hist = []; }
    loadHist();
  }

  put(main, h('h1', {}, 'Expansión'),
    h('div', { class: 'xp-top' },
      h('div', {}, h('h2', { style: 'margin-top:0' }, 'Header J3'), legend, pinInfo, h('div', { class: 'card pad', style: 'margin-top:12px' }, hdr)),
      h('div', { class: 'xp-side' },
        h('h2', { style: 'margin-top:0' }, 'Sensores'),
        h('div', { class: 'card pad' }, chTitle, chVal, chRange, canvas),
        h('div', { class: 'btns', style: 'margin:10px 0' }, rescan, portSt),
        sens, cands)),
    h('h2', {}, 'Puertos y módulos'), ports,
    h('h2', {}, 'modules.txt'), src,
    h('div', { class: 'card' }, ta, h('div', { class: 'row' }, ck, revert, save)),
    h('div', { class: 'card pad', style: 'margin-top:14px' }, h('b', {}, 'Agregar módulo'),
      h('div', { class: 'btns', style: 'margin-top:10px' }, fKind, fPort, fAddr, fOpt, fAdd), h('div', { style: 'margin-top:10px' }, fLine)),
    h('p', { class: 'note' }, 'Sintaxis: port <nombre> <pines> y module <nombre> <puerto> [addr= chip= shunt= gain=]. Los sensores que se reconocen solos no hace falta declararlos; declarados, se leen aunque no se identifiquen (docs/MODULES.md).'));
  await loadEx(true);
  await loadSensors();
  clearTimeout(xpT);
  let n = 0;
  const tick = async () => {
    if (location.hash !== '#expansion') return;
    await loadSensors();
    if (++n % 5 === 0) await loadEx(false);
    xpT = setTimeout(tick, 1000);
  };
  xpT = setTimeout(tick, 1000);
}

/* ---- Inicio ---- */
/* The home screen's menu.txt (components/aos_ui/aos_menu.h) arranged from the
 * computer: the dock, the 4x6 pages, folders, widgets and hidden apps. The
 * file is read with the firmware's grammar (parse() in aos_menu.c) and laid
 * out with place_items()'s rules (aos_home.c), apps the file does not name at
 * the end included, so what shows here is what the board shows upright.
 * Guardar writes it back whole; the board reloads its home screen by itself.
 * Lines kept as they are: the comments (they travel with the line under
 * them) and apps that are named but not installed. */
const IN = { cols: 4, rows: 6, dock: 4, pages: 16, folders: 32, name: 39, fid: 15, id: 39, glyph: 31, wtype: 15, warg: 63,
  line: 191, lines: 256 * 2 + 32, file: 24 * 1024 };
const IN_DEF_DOCK = ['aos.settings', 'aos.files', 'aos.music', 'aos.calc', 'aos.clock', 'aos.photos'];
const IN_WIDGETS = { clock: 'Reloj', ha: 'Home Assistant', sysmon: 'Monitor', claude: 'Claude', weather: 'Clima' };
const IN_SIZES = ['2x2', '4x2', '4x4'];
const IN_PALETTES = [['5E6B80', '3A4150'], ['7B2FF7', 'F107A3'], ['0A84FF', '003C8A'], ['30D158', '0B6E2E'], ['FF9F0A', 'FF375F'],
  ['64D2FF', '5E5CE6'], ['FFD60A', 'FF9F0A'], ['3A3A3C', '1C1C1E'], ['FF453A', '8A0F0A']];
const IN_FILLS = [['v', 'Degradé vertical'], ['d', 'Degradé diagonal'], ['r', 'Radial'], ['s', 'Liso']];
const IN_GROUPS = { g_general: 'General', g_juegos: 'Juegos', g_media: 'Multimedia', g_herram: 'Herramientas', g_red: 'Red',
  g_casa: 'Casa', g_salud: 'Salud', g_tiempo: 'Tiempo', g_varios: 'Varios' };
const inBytes = s => new TextEncoder().encode(s).length;
const inCut = (s, max) => { let a = Array.from(s); while (a.length && inBytes(a.join('')) > max) a.pop(); return a.join(''); };

/* parse() of aos_menu.c, line by line and with its limits. Returns the
 * entries in file order, or the first error the board would stop at (the
 * board then ignores the whole file). */
function inParse(text) {
  const fail = (n, why) => ({ ok: false, err: n ? `línea ${n}: ${why}` : why });
  if (inBytes(text) > IN.file) return fail(0, 'el archivo pasa de 24 KB');
  const entries = [], head = [], ids = new Set(), src = text.split('\n');
  let open = -1, inDock = false, sawDock = false, folders = 0, count = 0, pre = [], started = false;
  if (src[src.length - 1] === '') src.pop();
  for (let i = 0; i < src.length; i++) {
    const n = i + 1;
    if (inBytes(src[i]) > IN.line) return fail(n, 'línea demasiado larga');
    const line = src[i].replace(/[\r \t]+$/, '');
    let cur = line;
    const tok = () => { cur = cur.replace(/^[ \t]+/, ''); if (!cur) return null; const t = cur.match(/^[^ \t]+/)[0]; cur = cur.slice(t.length + 1); return t; };
    const kw = tok();
    if (!kw || kw[0] === '#') { if (kw) (started ? pre : head).push(line); continue; }
    started = true;
    const e = { k: kw, pre, n };
    pre = [];
    const counted = () => ++count > IN.lines;
    if (kw === 'app' || kw === 'hide') {
      e.id = tok();
      if (!e.id) return fail(n, kw + ' sin id de app');
      if (inBytes(e.id) > IN.id) return fail(n, 'id de app demasiado largo');
      if (counted()) return fail(n, 'demasiadas líneas');
      e.folder = kw === 'app' ? open : -1;
      e.dock = kw === 'app' && inDock;
    } else if (kw === 'dock') {
      if (open >= 0 || inDock) return fail(n, 'dock adentro de una carpeta o del dock');
      if (sawDock) return fail(n, 'dos secciones dock');
      inDock = sawDock = true;
    } else if (kw === 'page' || kw === 'widget') {
      if (open >= 0 || inDock) return fail(n, 'page o widget adentro de una carpeta o del dock');
      if (counted()) return fail(n, 'demasiadas líneas');
      if (kw === 'widget') {
        e.type = tok();
        const size = tok(), m = size && size.match(/^([+-]?\d+)x([+-]?\d+)/);
        e.w = m ? +m[1] : 0; e.h = m ? +m[2] : 0;
        if (!e.type || !m || e.w < 1 || e.h < 1 || e.w > 8 || e.h > 8) return fail(n, 'el widget necesita un tipo y un tamaño como 2x2');
        if (inBytes(e.type) > IN.wtype) return fail(n, 'tipo de widget demasiado largo');
        e.arg = inCut(cur.replace(/^[ \t]+/, ''), IN.warg);
      }
    } else if (kw === 'folder') {
      if (open >= 0 || inDock) return fail(n, 'una carpeta adentro de otra o del dock');
      if (folders >= IN.folders) return fail(n, 'más de 32 carpetas');
      if (counted()) return fail(n, 'demasiadas líneas');
      const id = tok(), a = tok(), b = tok(), fl = tok(), glyph = tok(), gc = tok();
      const name = cur.replace(/^[ \t]+/, '');
      if (!gc || !name) return fail(n, 'la carpeta necesita id, dos colores, relleno, glifo, color del glifo y nombre');
      if (inBytes(id) > IN.fid) return fail(n, 'id de carpeta demasiado largo');
      if (!/^[0-9a-fA-F]{6}$/.test(a) || !/^[0-9a-fA-F]{6}$/.test(b)) return fail(n, 'los colores son seis dígitos hexadecimales');
      if (fl.length !== 1 || !'svdr'.includes(fl)) return fail(n, 'el relleno es s, v, d o r');
      if (gc !== 'w' && gc !== 'b') return fail(n, 'el color del glifo es w o b');
      if (ids.has(id)) return fail(n, 'dos carpetas con el mismo id');
      ids.add(id);
      Object.assign(e, { id, a: a.toUpperCase(), b: b.toUpperCase(), fill: fl, glyph: inCut(glyph, IN.glyph), dark: gc === 'b', name: inCut(name, IN.name), idx: folders });
      open = folders++;
    } else if (kw === 'end') {
      if (open < 0 && !inDock) return fail(n, 'end sin carpeta ni dock');
      open = -1; inDock = false;
    } else return fail(n, 'línea desconocida (app, folder, dock, page, widget, end, hide)');
    entries.push(e);
  }
  if (open >= 0 || inDock) return fail(src.length, 'la última carpeta o el dock no tiene end');
  return { ok: true, entries, head, tail: pre, sawDock, folders };
}

/* One page as place_items() fills it: each item where it lands, and the
 * first one that does not fit (it and what follows go to the next page).
 * Apps that are not installed take no cell, as on the board. */
const inGhost = (it, apps) => it.t === 'app' && !apps.has(it.id);
const inSize = it => it.t === 'widget' ? [Math.min(it.w, IN.cols), Math.min(it.h, IN.rows)] : [1, 1];
function inPlace(items, apps) {
  const occ = [...Array(IN.rows)].map(() => Array(IN.cols).fill(false)), pos = new Map();
  let row = 0, col = 0, over = -1;
  for (let i = 0; i < items.length && over < 0; i++) {
    const it = items[i];
    if (inGhost(it, apps)) continue;
    const [w, h] = inSize(it);
    let done = row >= IN.rows;
    if (done) { over = i; break; }
    for (let r = row; r <= IN.rows - h && !done; r++)
      for (let c = r === row ? col : 0; c <= IN.cols - w && !done; c++) {
        let free = true;
        for (let y = 0; y < h && free; y++) for (let x = 0; x < w && free; x++) free = !occ[r + y][c + x];
        if (!free) continue;
        for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) occ[r + y][c + x] = true;
        pos.set(it, { r, c, w, h });
        done = true; row = r; col = c + w;
        if (col >= IN.cols) { col = 0; row++; }
      }
    if (!done) over = i;
  }
  return { pos, over: over === 0 && !pos.size ? -1 : over, occ };
}
/* What overflows a page moves to the start of the next one, as it would on
 * the board. Empty pages stay (a place to drop onto) but are not written. */
function inFlow(pages, apps) {
  const out = [];
  let carry = [];
  const push = pg => {
    for (;;) {
      const { over } = inPlace(pg, apps);
      if (over < 0) { out.push(pg); return; }
      out.push(pg.slice(0, over)); pg = pg.slice(over);
    }
  };
  for (const pg of pages) {
    const all = carry.concat(pg), { over } = inPlace(all, apps);
    if (over < 0) { out.push(all); carry = []; } else { out.push(all.slice(0, over)); carry = all.slice(over); }
  }
  if (carry.length) push(carry);
  return out.length ? out : [[]];
}

/* The file's entries resolved against what is installed, as aos_menu_root(),
 * aos_menu_dock() and place_items() do: hide wins, the first line of an app
 * wins, the default dock and the clock when the file has neither dock nor
 * folders, and every app the file does not place at the end. */
function inBuild(p, apps) {
  const m = { head: p.head, tail: p.tail, dock: [], dockPre: [], hidden: [], hidePre: [], pages: [] };
  const seen = new Set(), folders = [], root = [];
  let carry = [], box = null;      /* box: the comments of the dock or folder being read */
  for (const e of p.entries) if (e.k === 'hide' && !seen.has(e.id)) { seen.add(e.id); m.hidden.push(e.id); }
  for (const e of p.entries) {
    if (e.k === 'hide') { m.hidePre.push(...e.pre); continue; }
    if (e.k === 'dock') { m.dockPre.push(...e.pre); box = m.dockPre; continue; }
    if (e.k === 'end') { (box || carry).push(...e.pre); box = null; continue; }
    if (e.k === 'app' && (e.dock || e.folder >= 0)) {
      box.push(...e.pre);
      if (seen.has(e.id)) continue;
      seen.add(e.id);
      (e.dock ? m.dock : folders[e.folder].apps).push(e.id);
      continue;
    }
    const pre = carry.concat(e.pre);
    carry = [];
    if (e.k === 'app') {
      if (seen.has(e.id)) { carry = pre; continue; }
      seen.add(e.id);
      root.push({ t: 'app', id: e.id, pre });
    } else if (e.k === 'folder') {
      const f = { t: 'folder', id: e.id, a: e.a, b: e.b, fill: e.fill, glyph: e.glyph, dark: e.dark, name: e.name, apps: [], pre };
      folders[e.idx] = f;
      root.push(f);
      box = f.pre;
    } else if (e.k === 'widget') root.push({ t: 'widget', type: e.type, w: e.w, h: e.h, arg: e.arg, pre });
    else root.push({ t: 'page', pre });
  }
  m.tail = carry.concat(m.tail);
  if (!p.sawDock) {
    /* no dock line: the runtime's own, and those apps leave the pages */
    for (const id of IN_DEF_DOCK) if (m.dock.length < IN.dock && apps.has(id) && !m.hidden.includes(id)) m.dock.push(id);
    const d = new Set(m.dock);
    for (let i = root.length - 1; i >= 0; i--) if (root[i].t === 'app' && d.has(root[i].id)) root.splice(i, 1);
    for (const f of folders) if (f) f.apps = f.apps.filter(id => !d.has(id));
    m.dock.forEach(id => seen.add(id));
  }
  if (!p.sawDock && !p.folders) root.unshift({ t: 'widget', type: 'clock', w: 4, h: 2, arg: '', pre: [] });
  for (const id of apps.keys()) if (!seen.has(id)) root.push({ t: 'app', id, pre: [] });
  /* explicit page lines split the list; each part fills pages of its own */
  let seg = [];
  const segs = [];
  for (const it of root) {
    if (it.t === 'page') { segs.push(seg); seg = []; carry = it.pre; continue; }
    if (carry.length) { it.pre = carry.concat(it.pre); carry = []; }
    seg.push(it);
  }
  segs.push(seg);
  m.tail = carry.concat(m.tail);
  for (const s of segs) {
    if (!s.length) continue;
    if (!s.some(it => !inGhost(it, apps)) && m.pages.length) { m.pages[m.pages.length - 1].push(...s); continue; }
    m.pages.push(...inFlow([s], apps));
  }
  if (!m.pages.length) m.pages.push([]);
  return m;
}

function inText(m) {
  const out = m.head.length ? [...m.head] : ['# P4OS home screen - written by the portal (#inicio)'];
  out.push(...m.dockPre, 'dock', ...m.dock.map(id => '  app ' + id), 'end');
  let first = true;
  for (const pg of m.pages) {
    if (!pg.length) continue;
    if (!first) out.push('page');
    first = false;
    for (const it of pg) {
      out.push(...(it.pre || []));
      if (it.t === 'app') out.push('app ' + it.id);
      else if (it.t === 'widget') out.push(`widget ${it.type} ${it.w}x${it.h}` + (it.arg ? ' ' + it.arg : ''));
      else out.push(`folder ${it.id} ${it.a} ${it.b} ${it.fill} ${it.glyph || 'folder'} ${it.dark ? 'b' : 'w'} ${it.name}`, ...it.apps.map(id => '  app ' + id), 'end');
    }
  }
  out.push(...m.hidePre, ...m.hidden.map(id => 'hide ' + id), ...m.tail);
  return out.join('\n') + '\n';
}

/* The limits, before anything is written: errors stop Guardar, warnings don't. */
function inCheck(m, apps, text) {
  const err = [], warn = [];
  const dockOn = m.dock.filter(id => apps.has(id)).length;
  if (dockOn > 6) err.push(`El dock tiene ${dockOn} apps: la placa muestra 4 parada y 6 acostada.`);
  else if (dockOn > IN.dock) warn.push(`El dock tiene ${dockOn} apps: parada la placa muestra las 4 primeras.`);
  const folders = m.pages.flat().filter(it => it.t === 'folder'), fids = new Set();
  if (folders.length > IN.folders) err.push(`Hay ${folders.length} carpetas: la placa lee 32 como mucho.`);
  for (const f of folders) {
    const nm = `La carpeta «${f.name || f.id}»`;
    if (!/^[^ \t]+$/.test(f.id) || inBytes(f.id) > IN.fid) err.push(`${nm}: el id va sin espacios y con hasta 15 caracteres.`);
    if (fids.has(f.id)) err.push(`${nm}: hay dos carpetas con el id ${f.id}.`);
    fids.add(f.id);
    if (!f.name.trim()) err.push(`${nm} no tiene nombre.`);
    if (inBytes(f.name) > IN.name) err.push(`${nm}: el nombre pasa de 39 bytes.`);
    if (!/^[0-9A-F]{6}$/i.test(f.a) || !/^[0-9A-F]{6}$/i.test(f.b)) err.push(`${nm}: los colores son seis dígitos hexadecimales.`);
    if (!/^[^ \t]+$/.test(f.glyph) || inBytes(f.glyph) > IN.glyph) err.push(`${nm}: el glifo no es válido.`);
    const on = f.apps.filter(id => apps.has(id)).length;
    if (on < 2) warn.push(`${nm} tiene ${on === 0 ? 'ninguna app' : 'una sola app'}: al editar el inicio en la placa, ${on === 0 ? 'desaparece' : 'se deshace'}.`);
  }
  for (const w of m.pages.flat().filter(it => it.t === 'widget')) {
    if (!/^[^ \t]+$/.test(w.type) || inBytes(w.type) > IN.wtype) err.push(`El widget «${w.type}»: el tipo va sin espacios y con hasta 15 caracteres.`);
    if (!(w.w >= 1 && w.w <= 8 && w.h >= 1 && w.h <= 8)) err.push(`El widget «${w.type}»: el tamaño va de 1x1 a 8x8.`);
    if (inBytes(w.arg || '') > IN.warg) err.push(`El widget «${w.type}»: el dato pasa de 63 bytes.`);
  }
  const used = m.pages.filter(pg => pg.some(it => !inGhost(it, apps))).length;
  if (used > IN.pages) err.push(`Hay ${used} páginas: la placa muestra 16 como mucho.`);
  if (m.pages.some(pg => !pg.length) && m.pages.length > 1) warn.push('Las páginas vacías no se guardan.');
  const p = inParse(text);
  if (!p.ok) err.push('El archivo no pasaría la lectura de la placa: ' + p.err + '.');
  return { err, warn };
}

/* Icons: the AIC blob the board draws (/api/apps/icon), on its gradient
 * squircle. Apps whose icon is a glyph get their text or initial. */
const IN_PAL = ['FFFFFF', '000000', '1C1C1E', '2C2C2E', '8E8E93', '0A84FF', '30D158', 'FF453A', 'FF9F0A', 'FFD60A', 'BF5AF2', 'FF375F', '40C8E0'];
function inAicOps(b) {
  if (b.length < 4 || b.length > 256 || b[0] !== 65 || b[1] !== 73 || b[2] !== 67 || b[3] !== 1) return null;
  let at = 4;
  const ops = [], u8 = () => b[at++], i8 = () => { const v = b[at++]; return v > 127 ? v - 256 : v; };
  const i16 = () => { const v = b[at] | (b[at + 1] << 8); at += 2; return v > 32767 ? v - 65536 : v; };
  const col = () => { const i = u8(); if (i === 0xFF) { const r = u8(), g = u8(), bb = u8(); return (r << 16) | (g << 8) | bb; } return parseInt(IN_PAL[i] || 'FF00FF', 16); };
  while (at < b.length) {
    const op = u8();
    if (op === 0) break;
    if (op === 1) ops.push({ op: 'rect', align: u8(), x: i8(), y: i8(), w: i8(), h: i8(), r: u8(), c: col(), opa: u8() });
    else if (op === 2) ops.push({ op: 'ring', d: i8(), bw: i8(), c: col(), opa: u8() });
    else if (op === 3) ops.push({ op: 'arc', align: u8(), x: i8(), y: i8(), d: i8(), wt: i8(), wi: i8(), bs: i16(), be: i16(), is: i16(), ie: i16(), rot: i16(), ct: col(), ot: u8(), ci: col(), oi: u8() });
    else if (op === 4) ops.push({ op: 'hand', w: i8(), l: i8(), a: i16(), c: col() });
    else if (op === 5) { const f = u8(), n = u8(); ops.push({ op: 'text', font: f, s: new TextDecoder().decode(b.slice(at, at + n)) }); at += n; }
    else if (op === 6) ops.push({ op: 'rot', a: i16() });
    else if (op === 7) ops.push({ op: 'border', w: i8(), c: col(), opa: u8() });
    else if (op === 8) ops.push({ op: 'grad', c: col(), dir: u8() });
    else if (op === 9 || op === 10) ops.push({ op: op === 9 ? 'into' : 'out' });
    else break;
  }
  return ops;
}
function inRRect(ctx, x, y, w, h, r) {
  r = Math.max(0, Math.min(r, w / 2, h / 2));
  ctx.beginPath(); ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
}
function inDrawIcon(ctx, app, bytes, S) {
  const rgba = (n, o) => `rgba(${n >> 16 & 255},${n >> 8 & 255},${n & 255},${o / 255})`;
  const g = ctx.createLinearGradient(0, 0, 0, S);
  g.addColorStop(0, '#' + (app.color_a || '3A3A3C')); g.addColorStop(1, '#' + (app.color_b || app.color_a || '1C1C1E'));
  ctx.fillStyle = g; inRRect(ctx, 0, 0, S, S, S * 0.225); ctx.fill();
  const ops = bytes && inAicOps(bytes);
  if (!ops) {
    const t = app.text || (app.name || app.id || '?').trim().charAt(0).toUpperCase();
    ctx.fillStyle = '#fff'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.font = `600 ${Math.round(S * (t.length > 1 ? 0.34 : 0.44))}px system-ui, sans-serif`;
    ctx.fillText(t, S / 2, S / 2 + 1);
    return;
  }
  const pct = v => Math.trunc(S * v / 100);
  const dim = v => !v ? 0 : Math.max(1, v > 0 ? Math.trunc(S * v / 100) : Math.trunc(S / -v));
  const rad = v => v === 0xFF ? 1e9 : dim(v > 127 ? v - 256 : v);
  const place = (p, al, x, y, w, h) => {
    const cx = p.x + p.w / 2 - w / 2, cy = p.y + p.h / 2 - h / 2;
    const X = [0, p.x, cx, p.x + p.w - w, p.x, cx, p.x + p.w - w, p.x, p.x + p.w - w][al] ?? cx;
    const Y = [0, p.y, p.y, p.y, p.y + p.h - h, p.y + p.h - h, p.y + p.h - h, cy, cy][al] ?? cy;
    return al >= 1 && al <= 8 ? { x: X + x, y: Y + y, w, h } : { x: cx + x, y: cy + y, w, h };
  };
  const draw = s => {
    ctx.save();
    if (s.rot) { ctx.translate(s.x + s.w / 2, s.y + s.h / 2); ctx.rotate(s.rot / 10 * Math.PI / 180); ctx.translate(-(s.x + s.w / 2), -(s.y + s.h / 2)); }
    if (s.kind === 'rect') {
      const r = Math.min(rad(s.r), Math.min(s.w, s.h) / 2);
      if (s.grad) {
        const gg = s.grad.dir === 2 ? ctx.createLinearGradient(s.x, 0, s.x + s.w, 0) : ctx.createLinearGradient(0, s.y, 0, s.y + s.h);
        gg.addColorStop(0, rgba(s.c, s.opa)); gg.addColorStop(1, rgba(s.grad.c, s.opa)); ctx.fillStyle = gg;
      } else ctx.fillStyle = rgba(s.c, s.opa);
      inRRect(ctx, s.x, s.y, s.w, s.h, r);
      if (s.opa) ctx.fill();
      if (s.border) {
        const bw = s.border.w;
        ctx.strokeStyle = rgba(s.border.c, s.border.opa); ctx.lineWidth = bw;
        inRRect(ctx, s.x + bw / 2, s.y + bw / 2, s.w - bw, s.h - bw, Math.max(0, r - bw / 2)); ctx.stroke();
      }
    } else if (s.kind === 'arc') {
      const cx = s.x + s.w / 2, cy = s.y + s.h / 2, deg = Math.PI / 180;
      ctx.lineCap = 'round';
      if (s.wt && s.ot) { ctx.lineWidth = s.wt; ctx.strokeStyle = rgba(s.ct, s.ot); ctx.beginPath(); ctx.arc(cx, cy, s.w / 2 - s.wt / 2, (s.rot0 + s.bs) * deg, (s.rot0 + s.be) * deg); ctx.stroke(); }
      if (s.wi && s.oi && s.ie !== s.is) { ctx.lineWidth = s.wi; ctx.strokeStyle = rgba(s.ci, s.oi); ctx.beginPath(); ctx.arc(cx, cy, s.w / 2 - s.wi / 2, (s.rot0 + s.is) * deg, (s.rot0 + s.ie) * deg); ctx.stroke(); }
    } else if (s.kind === 'hand') {
      ctx.translate(s.px, s.py); ctx.rotate(s.a / 10 * Math.PI / 180);
      ctx.fillStyle = rgba(s.c, 255); inRRect(ctx, -s.w / 2, -s.l, s.w, s.l, s.w / 2); ctx.fill();
    } else if (s.kind === 'text') {
      ctx.fillStyle = '#fff'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.font = `600 ${Math.round(S * (s.font === 1 ? 0.34 : 0.24))}px system-ui, sans-serif`;
      ctx.fillText(s.s, s.x + s.w / 2, s.y + s.h / 2);
    }
    ctx.restore();
  };
  /* ROT/BORDER/GRAD change the last shape, so each is drawn when the next
   * one starts (LVGL draws in creation order, and so does this) */
  const stack = [];
  let parent = { x: 0, y: 0, w: S, h: S }, last = null, pend = null;
  const flush = () => { if (pend) { draw(pend); pend = null; } };
  for (const o of ops) {
    if (o.op === 'rect') { flush(); pend = last = Object.assign({ kind: 'rect', r: o.r, c: o.c, opa: o.opa }, place(parent, o.align, pct(o.x), pct(o.y), dim(o.w), dim(o.h))); }
    else if (o.op === 'ring') { flush(); const d = dim(o.d); pend = last = Object.assign({ kind: 'rect', r: 0xFF, c: o.c, opa: 0, border: { w: dim(o.bw), c: o.c, opa: o.opa } }, place(parent, 9, 0, 0, d, d)); }
    else if (o.op === 'arc') { flush(); const d = dim(o.d); pend = last = Object.assign({ kind: 'arc', wt: dim(o.wt), wi: dim(o.wi), bs: o.bs, be: o.be, is: o.is, ie: o.ie, rot0: o.rot, ct: o.ct, ot: o.ot, ci: o.ci, oi: o.oi }, place(parent, o.align, pct(o.x), pct(o.y), d, d)); }
    else if (o.op === 'hand') { flush(); pend = last = Object.assign({ kind: 'hand', w: dim(o.w), l: dim(o.l), a: o.a, c: o.c, px: parent.x + parent.w / 2, py: parent.y + parent.h / 2 }, parent); }
    else if (o.op === 'text') { flush(); pend = last = Object.assign({ kind: 'text', s: o.s, font: o.font }, parent); }
    else if (o.op === 'rot') { if (pend) pend.rot = o.a; }
    else if (o.op === 'border') { if (pend && pend.kind === 'rect') pend.border = { w: dim(o.w), c: o.c, opa: o.opa }; }
    else if (o.op === 'grad') { if (pend && pend.kind === 'rect') pend.grad = { c: o.c, dir: o.dir }; }
    else if (o.op === 'into') { flush(); if (last) { stack.push(parent); parent = last; } }
    else if (o.op === 'out') { flush(); if (stack.length) parent = stack.pop(); }
  }
  flush();
}
const inIcons = { blobs: new Map(), urls: new Map(), queue: Promise.resolve() };
function inIconUrl(app, id, S) {
  const key = id + '@' + S;
  if (inIcons.urls.has(key)) return inIcons.urls.get(key);
  const a = app || { id, name: id, color_a: '3A3A3C', color_b: '1C1C1E' };
  if (a.aic && !inIcons.blobs.has(id)) {
    inIcons.blobs.set(id, undefined);
    inIcons.queue = inIcons.queue.then(async () => {
      try { const r = await fetch('/api/apps/icon?id=' + encodeURIComponent(id)); inIcons.blobs.set(id, r.ok ? new Uint8Array(await r.arrayBuffer()) : null); }
      catch { inIcons.blobs.set(id, null); }
      for (const k of [...inIcons.urls.keys()]) if (k.startsWith(id + '@')) inIcons.urls.delete(k);
      document.querySelectorAll('img[data-ic]').forEach(img => { if (img.dataset.ic === id) img.src = inIconUrl(a, id, +img.dataset.sz); });
    });
  }
  const dpr = Math.min(3, window.devicePixelRatio || 1), c = document.createElement('canvas');
  c.width = c.height = Math.round(S * dpr);
  const ctx = c.getContext('2d');
  ctx.scale(dpr, dpr);
  inDrawIcon(ctx, a, inIcons.blobs.get(id), S);
  const url = c.toDataURL();
  if (inIcons.blobs.get(id) !== undefined || !a.aic) inIcons.urls.set(key, url);
  return url;
}
let inGlyphs = null;
function inLoadGlyphs() {
  if (window.AOS_GLIFOS) return Promise.resolve(window.AOS_GLIFOS);
  return inGlyphs ||= new Promise(res => {
    const s = h('script', { src: '/glifos.js' });
    s.onload = () => res(window.AOS_GLIFOS || []);
    s.onerror = () => res([]);
    document.head.append(s);
  });
}
const inGlyphSvg = (name, fill) => {
  const g = (window.AOS_GLIFOS || []).find(x => x.n === name) || (window.AOS_GLIFOS || [])[0];
  return g ? `<svg viewBox="0 0 24 24"><path fill="${fill || '#fff'}" d="${g.d}"/></svg>` : '';
};
const inFillCss = f => f.fill === 's' ? '#' + f.a : f.fill === 'r' ? `radial-gradient(circle, #${f.a}, #${f.b})`
  : `linear-gradient(${f.fill === 'd' ? '135deg' : '180deg'}, #${f.a}, #${f.b})`;

async function pageInicio() {
  let apps = new Map(), m = null, base = '', orig = null, bad = null, sel = null, drag = null, curPage = 0;
  inLoadGlyphs().then(() => { if (m) render(); });
  const status = h('div', { class: 'card' });
  const dockBox = h('div', { class: 'indock' });
  const strip = h('div', { class: 'instrip' });
  const editor = h('div', { class: 'card pad' });
  const hidBox = h('div', { class: 'card inhid' });
  const saveBtn = h('button', { class: 'btn pri', disabled: true, onclick: () => save() }, 'Guardar');
  const undoBtn = h('button', { class: 'btn', disabled: true, onclick: () => { sel = null; load(); } }, 'Descartar cambios');
  const text = () => inText(m);
  const dirty = () => m && text() !== base;
  const name = id => (apps.get(id) || {}).name || id;
  const icon = (id, S) => h('img', { src: inIconUrl(apps.get(id), id, S), 'data-ic': id, 'data-sz': S, width: S, height: S, alt: '', draggable: 'false' });

  async function load() {
    let list, txt = '';
    try {
      list = await api('apps');
      const r = await fetch('/api/fs/get?path=%2Fmenu.txt');
      if (r.ok) txt = await r.text();
      else if (r.status !== 404) throw new Error('no se pudo leer menu.txt');
      orig = r.ok ? txt : null;
    } catch (e) { put(status, h('div', { class: 'row bad' }, e.message)); return; }
    apps = new Map(list.map(a => [a.id, a]));
    const p = inParse(txt);
    bad = p.ok ? null : p.err;
    m = inBuild(p.ok ? p : inParse(''), apps);
    base = bad ? '' : text();       /* a file the board ignores: Guardar can replace it */
    sel = null;
    render();
  }
  async function save() {
    const t = text(), chk = inCheck(m, apps, t);
    if (chk.err.length) { toast(chk.err[0], true); return; }
    try {
      const r0 = await fetch('/api/fs/get?path=%2Fmenu.txt');
      const now = r0.ok ? await r0.text() : null;
      if (now !== orig && !confirm('menu.txt cambió en la placa desde que lo cargaste (¿lo editaste en la pantalla?). ¿Guardar igual y reemplazarlo?')) return;
      const r = await fetch('/api/fs/put?path=%2Fmenu.txt', { method: 'PUT', body: t });
      if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
      const back = await (await fetch('/api/fs/get?path=%2Fmenu.txt')).text();
      if (back !== t) throw new Error('la placa guardó otra cosa');
      orig = base = t;
      bad = null;
      /* read back as the board will: it must come out the same */
      const again = inBuild(inParse(back), apps);
      toast(inText(again) === t ? 'Guardado: la placa ya lo muestra' : 'Guardado, pero al releerlo no queda igual', inText(again) !== t);
      render();
    } catch (e) { toast(e.message, true); }
  }
  function download() {
    if (orig == null) { toast('La tarjeta no tiene menu.txt todavía: la placa usa el orden de siempre', true); return; }
    const d = new Date(), z = n => String(n).padStart(2, '0');
    const a = h('a', { href: URL.createObjectURL(new Blob([orig], { type: 'text/plain' })),
      download: `menu-${info.name || 'p4os'}-${d.getFullYear()}${z(d.getMonth() + 1)}${z(d.getDate())}-${z(d.getHours())}${z(d.getMinutes())}.txt` });
    a.click();
  }
  function rawEditor() {
    const ta = h('textarea', { spellcheck: 'false' });
    ta.value = text();
    const close = () => md.remove();
    const md = h('div', { class: 'modal', onclick: e => { if (e.target === md) close(); } },
      h('div', { class: 'box' }, h('div', { class: 'head' }, h('b', { class: 'grow' }, 'menu.txt'), h('span', { class: 'muted small' }, 'así se va a guardar')), ta,
        h('div', { class: 'foot' }, h('button', { class: 'btn', onclick: close }, 'Cancelar'),
          h('button', { class: 'btn pri', onclick: () => {
            const p = inParse(ta.value);
            if (!p.ok) { toast(p.err, true); return; }
            m = inBuild(p, apps); sel = null; close(); render();
          } }, 'Aplicar'))));
    document.body.append(md);
    ta.focus();
  }

  /* ---- where things are, and moving them ---- */
  const allFolders = () => m.pages.flat().filter(it => it.t === 'folder');
  function whereApp(id) {
    if (m.dock.includes(id)) return { in: 'dock' };
    if (m.hidden.includes(id)) return { in: 'hidden' };
    for (let p = 0; p < m.pages.length; p++)
      for (const it of m.pages[p]) {
        if (it.t === 'app' && it.id === id) return { in: 'page', p, it };
        if (it.t === 'folder' && it.apps.includes(id)) return { in: 'folder', p, f: it };
      }
    return null;
  }
  const pageOf = it => m.pages.findIndex(pg => pg.includes(it));
  /* takes an app (by id) or a page item (folder, widget) out of wherever it is */
  function take(d) {
    if (d.obj) { const p = pageOf(d.obj); if (p >= 0) m.pages[p].splice(m.pages[p].indexOf(d.obj), 1); return d.obj; }
    const w = whereApp(d.id), rm = (a, v) => a.splice(a.indexOf(v), 1);
    if (!w) return { t: 'app', id: d.id, pre: [] };
    if (w.in === 'dock') rm(m.dock, d.id);
    else if (w.in === 'hidden') rm(m.hidden, d.id);
    else if (w.in === 'folder') rm(w.f.apps, d.id);
    else { rm(m.pages[w.p], w.it); return w.it; }
    return { t: 'app', id: d.id, pre: [] };
  }
  const dockOn = () => m.dock.filter(id => apps.has(id)).length;
  /* t: {to:'page', p, before} | {to:'dock', before} | {to:'folder', f, before} | {to:'hidden'} | {to:'newfolder', target} */
  function move(d, t) {
    if (!d || (t.before && (t.before === d.obj || t.before === d.id || (d.id && t.before.t === 'app' && t.before.id === d.id)))) return;
    const isApp = !!d.id;
    if (!isApp && t.to !== 'page') { toast('Ahí sólo van apps', true); return; }
    if (t.to === 'dock' && !m.dock.includes(d.id) && dockOn() >= IN.dock) { toast('El dock admite 4 apps', true); return; }
    if (t.to === 'newfolder' && allFolders().length >= IN.folders) { toast('La placa lee 32 carpetas como mucho', true); return; }
    const it = take(d);
    if (t.to === 'page') {
      while (m.pages.length <= t.p) m.pages.push([]);
      const pg = m.pages[t.p], i = t.before ? pg.indexOf(t.before) : -1;
      pg.splice(i < 0 ? pg.length : i, 0, it);
      curPage = t.p;
    } else if (t.to === 'dock') {
      const i = t.before ? m.dock.indexOf(t.before) : -1;
      m.dock.splice(i < 0 ? m.dock.length : i, 0, d.id);
    } else if (t.to === 'folder') {
      const i = t.before ? t.f.apps.indexOf(t.before) : -1;
      t.f.apps.splice(i < 0 ? t.f.apps.length : i, 0, d.id);
      if (it.pre && it.pre.length) t.f.pre.push(...it.pre);
    } else if (t.to === 'hidden') {
      m.hidden.push(d.id);
      if (it.pre && it.pre.length) m.hidePre.push(...it.pre);
      if (sel && sel.id === d.id) sel = null;
    } else if (t.to === 'newfolder') {
      /* an app dropped on another: a folder with both, where the other was */
      const p = pageOf(t.target), pg = m.pages[p];
      const f = newFolder([t.target.id, d.id]);
      f.pre = (t.target.pre || []).concat(it.pre || []);
      pg.splice(pg.indexOf(t.target), 1, f);
      sel = { obj: f };
    }
    changed();
  }
  function newFolder(ids) {
    let n = 1;
    const used = new Set(allFolders().map(f => f.id));
    while (used.has('f' + n)) n++;
    const pal = IN_PALETTES[allFolders().length % IN_PALETTES.length];
    return { t: 'folder', id: 'f' + n, a: pal[0], b: pal[1], fill: 'v', glyph: 'folder', dark: false, name: 'Carpeta', apps: ids, pre: [] };
  }
  function changed() { m.pages = inFlow(m.pages, apps); render(); }

  /* ---- drag and drop ---- */
  const zone = (e, el, into) => {
    const r = el.getBoundingClientRect(), x = (e.clientX - r.left) / r.width;
    return into && x > 0.28 && x < 0.72 ? 'into' : x < 0.5 ? 'before' : 'after';
  };
  const clear = el => el.classList.remove('before', 'after', 'into', 'over');
  function dragSrc(el, d) {
    el.setAttribute('draggable', 'true');
    if (!el.hasAttribute('tabindex')) {
      el.setAttribute('tabindex', '0');
      el.setAttribute('role', 'button');
      el.addEventListener('keydown', e => { if ((e.key === 'Enter' || e.key === ' ') && e.target === el) { e.preventDefault(); el.click(); } });
    }
    el.addEventListener('dragstart', e => { drag = d; e.dataTransfer.effectAllowed = 'move'; e.dataTransfer.setData('text/plain', d.id || d.obj.t); setTimeout(() => el.classList.add('drag'), 0); e.stopPropagation(); });
    el.addEventListener('dragend', () => { drag = null; el.classList.remove('drag'); document.querySelectorAll('.over,.before,.after,.into').forEach(clear); });
  }
  /* a drop target: where(e) says what a drop there would do, or null */
  function dropOn(el, where) {
    el.addEventListener('dragover', e => {
      if (!drag) return;
      const t = where(e);
      if (!t) return;
      e.preventDefault(); e.stopPropagation();
      clear(el);
      el.classList.add(t.cls || 'over');
    });
    el.addEventListener('dragleave', e => { if (!el.contains(e.relatedTarget)) clear(el); });
    el.addEventListener('drop', e => {
      if (!drag) return;
      const t = where(e);
      clear(el);
      if (!t) return;
      e.preventDefault(); e.stopPropagation();
      const d = drag;
      drag = null;
      move(d, t);
    });
  }
  /* before / after an item of a list: the one after it, or null for the end */
  const nextOf = (list, v) => { const i = list.indexOf(v); return i >= 0 && i + 1 < list.length ? list[i + 1] : null; };

  /* ---- drawing ---- */
  function appTile(id, ctx) {
    const a = apps.get(id);
    const t = h('div', { class: 'intile' + (sel && sel.id === id ? ' sel' : ''), title: `${name(id)} · ${id}`,
      onclick: e => { e.stopPropagation(); sel = { id }; if (ctx.p != null) curPage = ctx.p; render(); } },
      icon(id, 58), h('div', { class: 'inname' }, a ? a.name : id));
    dragSrc(t, { id });
    dropOn(t, e => {
      const z = zone(e, t, ctx.list === 'page' && drag.id && drag.id !== id);
      if (ctx.list === 'page') {
        const it = ctx.it;
        if (z === 'into') return { to: 'newfolder', target: it, cls: 'into' };
        return { to: 'page', p: ctx.p, before: z === 'before' ? it : nextOf(m.pages[ctx.p], it), cls: z };
      }
      if (ctx.list === 'dock') return drag.id ? { to: 'dock', before: z === 'before' ? id : nextOf(m.dock, id), cls: z } : null;
      if (ctx.list === 'folder') return drag.id ? { to: 'folder', f: ctx.f, before: z === 'before' ? id : nextOf(ctx.f.apps, id), cls: z } : null;
      return null;
    });
    return t;
  }
  function folderIcon(f, S) {
    const on = f.apps.filter(id => apps.has(id));
    if (!on.length) return h('div', { class: 'infold glyph', html: inGlyphSvg(f.glyph, f.dark ? '#000' : '#fff') });
    return h('div', { class: 'infold' }, on.slice(0, 9).map(id => icon(id, 13)));
  }
  function folderTile(f, p) {
    const t = h('div', { class: 'intile' + (sel && sel.obj === f ? ' sel' : ''), title: `Carpeta ${f.name} · ${f.apps.length} apps`,
      onclick: e => { e.stopPropagation(); sel = { obj: f }; curPage = p; render(); } },
      folderIcon(f), h('div', { class: 'inname' }, f.name));
    dragSrc(t, { obj: f });
    dropOn(t, e => {
      const z = zone(e, t, !!drag.id);
      if (z === 'into') return { to: 'folder', f, before: null, cls: 'into' };
      return { to: 'page', p, before: z === 'before' ? f : nextOf(m.pages[p], f), cls: z };
    });
    return t;
  }
  function widgetTile(w, p, pos) {
    const t = h('div', { class: 'inwid' + (sel && sel.obj === w ? ' sel' : ''), style: `grid-row:${pos.r + 1}/span ${pos.h};grid-column:${pos.c + 1}/span ${pos.w}`,
      onclick: e => { e.stopPropagation(); sel = { obj: w }; curPage = p; render(); } },
      h('b', {}, IN_WIDGETS[w.type] || w.type), h('span', { class: 'small' }, `widget ${w.w}×${w.h}` + (w.arg ? ' · ' + w.arg : '')));
    dragSrc(t, { obj: w });
    dropOn(t, e => { const z = zone(e, t, false); return { to: 'page', p, before: z === 'before' ? w : nextOf(m.pages[p], w), cls: z }; });
    return t;
  }
  function pageBox(pg, p) {
    const { pos, occ } = inPlace(pg, apps);
    const grid = h('div', { class: 'ingrid' });
    for (const it of pg) {
      const q = pos.get(it);
      if (!q) continue;
      const el = it.t === 'app' ? appTile(it.id, { list: 'page', p, it }) : it.t === 'folder' ? folderTile(it, p) : widgetTile(it, p, q);
      if (it.t !== 'widget') el.style.cssText += `grid-row:${q.r + 1};grid-column:${q.c + 1}`;
      grid.append(el);
    }
    /* empty cells: a drop lands before the first item that comes after them */
    const order = pg.filter(it => pos.has(it));
    for (let r = 0; r < IN.rows; r++)
      for (let c = 0; c < IN.cols; c++) {
        if (occ[r][c]) continue;
        const cell = h('div', { class: 'inempty', style: `grid-row:${r + 1};grid-column:${c + 1}` });
        const after = order.find(it => { const q = pos.get(it); return q.r > r || (q.r === r && q.c > c); }) || null;
        dropOn(cell, () => ({ to: 'page', p, before: after, cls: 'over' }));
        grid.append(cell);
      }
    const visible = pg.some(it => !inGhost(it, apps));
    if (!visible) grid.prepend(h('div', { class: 'inblank' }, 'Página vacía: arrastrá algo acá. Vacía no se guarda.'));
    const ghosts = pg.filter(it => inGhost(it, apps)).length;
    const n = m.pages.length;
    const over = m.pages.slice(0, p + 1).filter(x => x.some(it => !inGhost(it, apps))).length > IN.pages && visible;
    const box = h('div', { class: 'inpage' + (over ? ' bad' : '') },
      h('div', { class: 'inhead' }, h('b', { class: 'grow' }, `Página ${p + 1}`), ghosts ? h('span', { title: 'apps nombradas en menu.txt que no están instaladas' }, `+${ghosts} sin instalar`) : null,
        over ? h('span', {}, 'la placa no la muestra') : null,
        h('button', { class: 'btn', title: 'Mover a la izquierda', disabled: p === 0, onclick: () => { [m.pages[p - 1], m.pages[p]] = [m.pages[p], m.pages[p - 1]]; changed(); } }, '◀'),
        h('button', { class: 'btn', title: 'Mover a la derecha', disabled: p === n - 1, onclick: () => { [m.pages[p + 1], m.pages[p]] = [m.pages[p], m.pages[p + 1]]; changed(); } }, '▶'),
        h('button', { class: 'btn', title: 'Quitar la página', disabled: n === 1 && !visible, onclick: () => {
          if (visible && !confirm(`¿Quitar la página ${p + 1}? Lo que tiene pasa a la página ${p ? p : 2}.`)) return;
          const items = m.pages.splice(p, 1)[0];
          if (!m.pages.length) m.pages.push([]);
          if (p > 0) m.pages[p - 1].push(...items); else m.pages[0].unshift(...items);
          changed();
        } }, '✕')),
      grid);
    dropOn(box, () => ({ to: 'page', p, before: null, cls: 'over' }));
    return box;
  }
  function renderDock() {
    const on = m.dock.filter(id => apps.has(id));
    put(dockBox, on.map(id => appTile(id, { list: 'dock' })), on.length ? null : h('div', { class: 'inhint' }, 'El dock está vacío: arrastrá apps acá (hasta 4).'));
  }
  function renderHidden() {
    const on = m.hidden.filter(id => apps.has(id)), off = m.hidden.length - on.length;
    put(hidBox, on.map(id => {
      const r = h('div', { class: 'row' }, icon(id, 30), h('div', { class: 'grow' }, name(id), ' ', h('span', { class: 'muted small' }, id)),
        h('button', { class: 'btn', onclick: () => move({ id }, { to: 'page', p: m.pages.length - 1, before: null }) }, 'Mostrar'));
      dragSrc(r, { id });
      return r;
    }), on.length ? null : h('div', { class: 'row muted small' }, 'Ninguna. Arrastrá una app acá para sacarla del inicio sin desinstalarla.'),
      off ? h('div', { class: 'row muted small' }, `${off} más ocultas que no están instaladas (se conservan).`) : null);
  }
  const field = (label, input, note) => h('div', {}, h('label', { class: 'f' }, label), input, note ? h('div', { class: 'note' }, note) : null);
  function destinations(cur) {
    const o = [];
    if (cur !== 'dock') o.push(['dock', 'Dock']);
    m.pages.forEach((_, p) => o.push(['p' + p, `Página ${p + 1}`]));
    o.push(['p' + m.pages.length, 'Página nueva']);
    for (const f of allFolders()) if (!(cur === 'folder' && sel.f === f)) o.push(['f:' + f.id, 'Carpeta ' + f.name]);
    if (cur !== 'hidden') o.push(['hidden', 'Ocultas']);
    return o;
  }
  function goTo(d, v) {
    if (v === 'dock') move(d, { to: 'dock', before: null });
    else if (v === 'hidden') move(d, { to: 'hidden' });
    else if (v.startsWith('f:')) move(d, { to: 'folder', f: allFolders().find(f => f.id === v.slice(2)), before: null });
    else move(d, { to: 'page', p: +v.slice(1), before: null });
  }
  /* one step earlier or later in its own list (no drag needed: a phone) */
  function step(d, delta) {
    let list;
    const w = d.id ? whereApp(d.id) : { in: 'page', p: pageOf(d.obj), it: d.obj };
    if (!w) return;
    const v = w.in === 'page' ? w.it : d.id;
    list = w.in === 'dock' ? m.dock : w.in === 'hidden' ? m.hidden : w.in === 'folder' ? w.f.apps : m.pages[w.p];
    const i = list.indexOf(v), j = i + delta;
    if (j < 0 || j >= list.length) {
      if (w.in !== 'page') return;
      const q = w.p + delta;
      if (q < 0) return;
      list.splice(i, 1);
      while (m.pages.length <= q) m.pages.push([]);
      if (delta < 0) m.pages[q].push(v); else m.pages[q].unshift(v);
    } else [list[i], list[j]] = [list[j], list[i]];
    changed();
  }
  function renderEditor() {
    const keep = editor.querySelector('.inlist'), top = keep ? keep.scrollTop : 0;
    renderEditorNow();
    const list = editor.querySelector('.inlist');
    if (list && keep) list.scrollTop = top;
  }
  function renderEditorNow() {
    if (!sel) {
      put(editor, h('h2', { style: 'margin-top:0' }, 'Cómo se usa'),
        h('p', { class: 'small muted', style: 'margin:0' }, 'Arrastrá los íconos para cambiarlos de lugar, entre páginas y al dock. Soltá una app sobre otra para armar una carpeta, o en el centro de una carpeta para meterla. Tocá cualquier cosa para editarla. Nada llega a la placa hasta que tocás Guardar.'));
      return;
    }
    const steps = d => [h('button', { class: 'btn', onclick: () => step(d, -1) }, '◀ Antes'), h('button', { class: 'btn', onclick: () => step(d, 1) }, 'Después ▶')];
    if (sel.id) {
      const id = sel.id, w = whereApp(id);
      if (!w) { sel = null; renderEditorNow(); return; }
      const here = { dock: 'En el dock', hidden: 'Oculta', page: `En la página ${w.p + 1}`, folder: `En la carpeta «${w.f && w.f.name}», página ${w.p + 1}` }[w.in];
      if (w.in === 'folder') sel.f = w.f;
      const dest = h('select', { onchange: e => { if (e.target.value) goTo({ id }, e.target.value); } }, h('option', { value: '' }, 'Mover a…'),
        destinations(w.in).map(([v, n]) => h('option', { value: v }, n)));
      put(editor, h('div', { class: 'btns', style: 'flex-wrap:nowrap' }, icon(id, 58), h('div', {}, h('b', {}, name(id)), h('div', { class: 'muted small mono' }, id), h('div', { class: 'small' }, here))),
        h('div', { class: 'btns', style: 'margin-top:14px' }, w.in === 'hidden' ? null : steps({ id }),
          w.in === 'hidden' ? h('button', { class: 'btn', onclick: () => move({ id }, { to: 'page', p: m.pages.length - 1, before: null }) }, 'Mostrar')
            : h('button', { class: 'btn', onclick: () => move({ id }, { to: 'hidden' }) }, 'Ocultar'),
          w.in === 'folder' ? h('button', { class: 'btn', onclick: () => move({ id }, { to: 'page', p: w.p, before: nextOf(m.pages[w.p], w.f) }) }, 'Sacar de la carpeta') : null),
        h('div', { style: 'margin-top:12px' }, dest),
        w.in === 'hidden' ? h('p', { class: 'note' }, 'Oculta sigue instalada: se abre desde el portal o desde otra app, y no aparece en el inicio.') : null);
      return;
    }
    const it = sel.obj;
    if (pageOf(it) < 0) { sel = null; renderEditorNow(); return; }
    const pageSel = h('select', { onchange: e => { if (e.target.value) goTo({ obj: it }, e.target.value); } }, h('option', { value: '' }, 'Mover a…'),
      m.pages.map((_, p) => h('option', { value: 'p' + p }, `Página ${p + 1}`)), h('option', { value: 'p' + m.pages.length }, 'Página nueva'));
    if (it.t === 'widget') {
      const known = Object.keys(IN_WIDGETS).includes(it.type) ? Object.keys(IN_WIDGETS) : [it.type, ...Object.keys(IN_WIDGETS)];
      const sizes = IN_SIZES.includes(`${it.w}x${it.h}`) ? IN_SIZES : [`${it.w}x${it.h}`, ...IN_SIZES];
      put(editor, h('h2', { style: 'margin-top:0' }, 'Widget'),
        field('Tipo', h('select', { onchange: e => { it.type = e.target.value; changed(); } }, known.map(k => h('option', { value: k, selected: k === it.type }, IN_WIDGETS[k] ? `${IN_WIDGETS[k]} (${k})` : k)))),
        field('Tamaño', h('select', { onchange: e => { [it.w, it.h] = e.target.value.split('x').map(Number); changed(); } }, sizes.map(s => h('option', { value: s, selected: s === `${it.w}x${it.h}` }, s.replace('x', ' × ') + ' celdas')))),
        field('Dato (opcional)', h('input', { type: 'text', value: it.arg || '', placeholder: 'lo que el widget lea', onchange: e => { it.arg = inCut(e.target.value.replace(/[\r\n]/g, ' ').trim(), IN.warg); changed(); } }), 'Hasta 63 bytes. La mayoría de los widgets no lo usan.'),
        h('div', { class: 'btns', style: 'margin-top:14px' }, steps({ obj: it }), pageSel,
          h('button', { class: 'btn red', onclick: () => { take({ obj: it }); sel = null; changed(); } }, 'Quitar widget')));
      return;
    }
    /* a folder */
    const f = it;
    const count = h('span', { class: 'muted small' });
    const setCount = () => { const b = inBytes(f.name); count.textContent = `${b}/39 bytes`; count.className = b > IN.name ? 'bad small' : 'muted small'; };
    const nameIn = h('input', { type: 'text', value: f.name, oninput: e => { f.name = e.target.value.replace(/[\r\n]/g, ''); setCount(); renderSoft(); },
      onchange: () => { f.name = f.name.trim() || 'Carpeta'; nameIn.value = f.name; changed(); } });
    setCount();
    const prev = h('div', { class: 'inprev' });
    const drawPrev = () => { prev.style.background = inFillCss(f); prev.innerHTML = inGlyphSvg(f.glyph, f.dark ? '#000' : '#fff'); };
    drawPrev();
    const ca = h('input', { type: 'color', value: '#' + f.a.toLowerCase(), oninput: e => { f.a = e.target.value.slice(1).toUpperCase(); drawPrev(); renderSoft(); } });
    const cb = h('input', { type: 'color', value: '#' + f.b.toLowerCase(), oninput: e => { f.b = e.target.value.slice(1).toUpperCase(); drawPrev(); renderSoft(); } });
    const pals = h('div', { class: 'btns' }, IN_PALETTES.map(([a, b]) => h('div', { class: 'mpsw' + (f.a === a && f.b === b ? ' on' : ''), style: `background:linear-gradient(#${a},#${b})`,
      onclick: () => { f.a = a; f.b = b; changed(); } })));
    const gsearch = h('input', { type: 'search', placeholder: 'Buscar un glifo…' });
    const gbox = h('div', { class: 'inglyphs' });
    const fillGlyphs = () => {
      const q = gsearch.value.toLowerCase().trim();
      const gl = (window.AOS_GLIFOS || []).filter(g => !q || g.n.includes(q) || (IN_GROUPS[g.g] || '').toLowerCase().includes(q));
      put(gbox, gl.length ? gl.map(g => h('button', { class: 'mpg' + (g.n === f.glyph ? ' on' : ''), title: `${g.n} · ${IN_GROUPS[g.g] || ''}`, html: inGlyphSvg(g.n, 'currentColor'),
        onclick: () => { f.glyph = g.n; drawPrev(); fillGlyphs(); renderSoft(); } }))
        : h('div', { class: 'muted small', style: 'grid-column:1/-1' }, window.AOS_GLIFOS ? 'Ninguno se llama así.' : 'Cargando los glifos…'));
    };
    gsearch.addEventListener('input', fillGlyphs);
    fillGlyphs();
    const fapps = h('div', { class: 'infapps' }, f.apps.filter(id => apps.has(id)).map(id => appTile(id, { list: 'folder', f })));
    dropOn(fapps, () => drag.id ? { to: 'folder', f, before: null, cls: 'over' } : null);
    const off = f.apps.filter(id => !apps.has(id)).length;
    const listed = [...apps.values()].filter(a => { const w = whereApp(a.id); return !w || w.in !== 'hidden'; }).sort((x, y) => x.name.localeCompare(y.name));
    const checks = h('div', { class: 'inlist' }, listed.map(a => {
      const w = whereApp(a.id), inThis = w && w.in === 'folder' && w.f === f;
      const other = w && !inThis ? (w.in === 'dock' ? 'dock' : w.in === 'folder' ? w.f.name : `pág. ${w.p + 1}`) : '';
      return h('label', {}, h('input', { type: 'checkbox', checked: inThis, onchange: e => {
        const p = pageOf(f);
        if (e.target.checked) move({ id: a.id }, { to: 'folder', f, before: null });
        else move({ id: a.id }, { to: 'page', p, before: nextOf(m.pages[p], f) });
      } }), icon(a.id, 22), a.name, h('span', { class: 'muted' }, other));
    }));
    put(editor, h('div', { class: 'btns', style: 'flex-wrap:nowrap;justify-content:space-between' }, h('h2', { style: 'margin:0' }, 'Carpeta'), h('span', { class: 'muted small mono' }, f.id)),
      field('Nombre', nameIn), count,
      h('label', { class: 'f' }, `Apps (${f.apps.length - off})` + (off ? ` · ${off} sin instalar, se conservan` : '')), fapps,
      h('div', { class: 'note' }, 'Arrastrá para ordenarlas o para sacarlas a una página, o marcá cuáles van:'), checks,
      h('label', { class: 'f' }, 'Color y glifo'),
      h('div', { class: 'btns', style: 'flex-wrap:nowrap;align-items:flex-start' }, prev, h('div', {}, pals, h('div', { class: 'btns', style: 'margin-top:8px' }, ca, cb,
        h('select', { style: 'width:auto', onchange: e => { f.fill = e.target.value; changed(); } }, IN_FILLS.map(([v, n]) => h('option', { value: v, selected: v === f.fill }, n))),
        h('select', { style: 'width:auto', onchange: e => { f.dark = e.target.value === 'b'; changed(); } }, h('option', { value: 'w', selected: !f.dark }, 'Glifo blanco'), h('option', { value: 'b', selected: f.dark }, 'Glifo negro'))))),
      h('div', { class: 'note' }, 'En P4OS la carpeta se ve esmerilada, como en el iPhone, y el glifo aparece mientras está vacía. Los colores quedan guardados en menu.txt.'),
      gsearch, gbox,
      h('div', { class: 'btns', style: 'margin-top:14px' }, steps({ obj: f }), pageSel,
        h('button', { class: 'btn red', onclick: () => {
          if (!confirm(`¿Borrar la carpeta «${f.name}»? Sus apps vuelven a la página, donde estaba la carpeta.`)) return;
          const p = pageOf(f), pg = m.pages[p];
          pg.splice(pg.indexOf(f), 1, ...f.apps.map((id, i) => ({ t: 'app', id, pre: i ? [] : f.pre })));
          sel = null; changed();
        } }, 'Borrar carpeta')));
  }
  /* while typing: the grids and the status, not the editor under the cursor */
  function renderSoft() { renderDock(); put(strip, m.pages.map(pageBox)); renderStatus(); }
  function renderStatus() {
    const t = text(), chk = inCheck(m, apps, t), d = dirty();
    saveBtn.disabled = !d || chk.err.length > 0;
    undoBtn.disabled = !d;
    saveBtn.textContent = d ? 'Guardar cambios' : 'Guardar';
    const on = [...apps.keys()].length, hid = m.hidden.filter(id => apps.has(id)).length;
    put(status,
      h('div', { class: 'row' }, h('div', { class: 'grow' }, orig == null ? 'La tarjeta no tiene menu.txt: la placa muestra el orden de siempre.' : 'menu.txt en la tarjeta',
        h('div', { class: 'muted small' }, `${on} apps instaladas · ${allFolders().length} carpetas · ${m.pages.filter(pg => pg.length).length} páginas · ${hid} ocultas · ${inBytes(t)} bytes`)),
        h('div', { class: d ? 'warn' : 'muted small' }, d ? 'cambios sin guardar' : 'igual que en la placa')),
      bad ? h('div', { class: 'row inerr small' }, `La placa ignora el menu.txt de la tarjeta (${bad}) y muestra el orden de siempre. Descargá el respaldo si lo querés; Guardar lo reemplaza.`) : null,
      chk.err.map(e => h('div', { class: 'row inerr small' }, e)),
      chk.warn.map(e => h('div', { class: 'row inwarn small' }, e)));
  }
  function render() {
    renderSoft();
    renderHidden();
    renderEditor();
  }

  const addFolder = () => {
    if (allFolders().length >= IN.folders) { toast('La placa lee 32 carpetas como mucho', true); return; }
    const f = newFolder([]);
    const p = Math.min(curPage, m.pages.length - 1);
    m.pages[p].push(f);
    sel = { obj: f };
    changed();
  };
  const addWidget = () => {
    const w = { t: 'widget', type: 'clock', w: 2, h: 2, arg: '', pre: [] };
    const p = Math.min(curPage, m.pages.length - 1);
    m.pages[p].unshift(w);
    sel = { obj: w };
    changed();
  };
  const addPage = () => {
    if (m.pages.length && !m.pages[m.pages.length - 1].some(it => !inGhost(it, apps))) { toast('Ya hay una página vacía al final', true); return; }
    m.pages.push([]);
    curPage = m.pages.length - 1;
    changed();
    strip.scrollTo({ left: strip.scrollWidth, behavior: 'smooth' });
  };
  dropOn(hidBox, () => drag && drag.id ? { to: 'hidden', cls: 'over' } : null);
  dropOn(dockBox, () => drag && drag.id ? { to: 'dock', before: null, cls: 'over' } : null);
  put(main, h('h1', {}, 'Inicio'), status,
    h('div', { class: 'btns', style: 'margin:18px 0 12px;justify-content:space-between' },
      h('div', { class: 'btns' },
        h('button', { class: 'btn', onclick: addPage }, '+ Página'),
        h('button', { class: 'btn', onclick: addFolder }, '+ Carpeta'),
        h('button', { class: 'btn', onclick: addWidget }, '+ Widget')),
      h('div', { class: 'btns' }, undoBtn, saveBtn,
        h('button', { class: 'btn', onclick: () => { if (dirty() && !confirm('Hay cambios sin guardar. ¿Recargar desde la placa y perderlos?')) return; load(); } }, 'Recargar'),
        h('button', { class: 'btn', onclick: download }, 'Descargar menu.txt'),
        h('button', { class: 'btn', onclick: rawEditor }, 'Texto'))),
    h('div', { class: 'incols' },
      h('div', { style: 'min-width:0' },
        h('h2', { style: 'margin-top:0' }, 'Dock'), h('div', { class: 'indockwrap' }, dockBox),
        h('h2', {}, 'Páginas'), strip,
        h('p', { class: 'note' }, 'Parada, la placa muestra 4 × 6 íconos por página en este orden; acostada reparte el mismo orden en 7 × 3. Lo que no entra en una página pasa a la siguiente.')),
      h('div', { class: 'inside' }, editor, h('div', {}, h('h2', { style: 'margin-top:0' }, 'Ocultas'), hidBox))));
  window.onbeforeunload = () => dirty() && location.hash === '#inicio' ? 'Hay cambios sin guardar' : undefined;
  await load();
}

/* ---- the card, for the apps' pages: window.P4OS hands these out (below) ---- */
/* A file put whole (PUT streams it through a .part on the card), with its progress. */
function fsPut(path, body, onProg) {
  return new Promise((ok, bad) => {
    const x = new XMLHttpRequest();
    x.open('PUT', '/api/fs/put?path=' + encodeURIComponent(path));
    if (onProg) x.upload.onprogress = e => { if (e.lengthComputable) onProg(e.loaded / e.total); };
    x.onload = () => {
      if (x.status === 200) { ok(); return; }
      let m = x.statusText || String(x.status);
      try { m = JSON.parse(x.responseText).error || m; } catch {}
      bad(new Error(m));
    };
    x.onerror = () => bad(new Error('se cortó la conexión con la placa'));
    x.send(body);
  });
}
/* A folder's entries; a folder that is not there yet is empty. */
async function fsList(path) {
  try { return (await api('fs?path=' + encodeURIComponent(path) + '&_=' + Date.now())).entries || []; }
  catch (e) { if (/no existe/.test(e.message)) return []; throw e; }
}
const fsDelete = path => api('fs/delete?path=' + encodeURIComponent(path), { method: 'POST' });
const fsMkdir = path => api('fs/mkdir?path=' + encodeURIComponent(path), { method: 'POST' }).catch(() => {});
const fsUrl = (path, dl) => '/api/fs/get?' + (dl ? 'attachment=1&' : '') + 'path=' + encodeURIComponent(path);
/* A file as text, or null if it is not there. */
async function fsText(path) {
  const r = await fetch(fsUrl(path) + '&_=' + Date.now(), { cache: 'no-store' });
  if (r.status === 404) return null;
  if (!r.ok) throw new Error('no se pudo leer ' + path);
  return r.text();
}
async function fsBytes(path) {
  const r = await fetch(fsUrl(path) + '&_=' + Date.now(), { cache: 'no-store' });
  if (!r.ok) throw new Error(r.status === 404 ? 'no existe ' + path : 'no se pudo leer ' + path);
  return r.arrayBuffer();
}
const openApp = id => api('open?id=' + encodeURIComponent(id), { method: 'POST' });
const fmtDate = s => s ? new Date(s * 1000).toLocaleDateString('es-AR', { day: '2-digit', month: 'short', year: 'numeric' }) : '';
function saveBlob(blob, name) {
  const a = h('a', { href: URL.createObjectURL(blob), download: name });
  document.body.append(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}
/* A name for the card: lower case, a-z 0-9 _ -, no accents. */
const fsSlug = (s, max, def) => s.normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase()
  .replace(/[^a-z0-9_-]+/g, '_').replace(/^_+|_+$/g, '').slice(0, max) || def;

/* ---- router ---- */
/* ---- Red ----
 * The Red app's LAN sweeps, saved by the board to the card's /redes as one
 * NDJSON file each (aos_nettools.c; AmoledOS's aos_scan.c writes the same
 * lines), and a new sweep started from here over /api/net. The board only
 * writes the lines: the tables, the changes since the sweep before and the
 * channel graph are drawn here. */
const RED_DIR = '/redes';
const RED_PORTS = { 21: 'FTP', 22: 'SSH', 23: 'Telnet', 53: 'DNS', 80: 'HTTP', 139: 'NetBIOS', 443: 'HTTPS', 445: 'SMB',
  502: 'Modbus', 554: 'RTSP', 631: 'IPP', 1880: 'Node-RED', 1883: 'MQTT', 3000: 'Grafana', 3389: 'RDP', 5000: 'UPnP',
  5555: 'SCPI', 5900: 'VNC', 6053: 'ESPHome', 8000: 'HTTP', 8080: 'HTTP', 8081: 'HTTP', 8123: 'Home Assistant',
  8443: 'HTTPS', 8554: 'RTSP', 8883: 'MQTT/TLS', 8888: 'HTTP', 9000: 'HTTP', 9100: 'Impresora', 32400: 'Plex' };
const RED_WEB = new Set([80, 443, 3000, 5000, 8000, 8080, 8081, 8123, 8443, 8888, 9000]);
const RED_PHASE = ['Ping', 'Puertos', 'Nombres y redes Wi-Fi'];
const ipNum = ip => (ip || '').split('.').reduce((a, b) => a * 256 + (parseInt(b, 10) || 0), 0);

/* Who made a MAC, by its first three bytes. A short list lives here (from
 * AmoledOS); the whole register can go on the card as /redes/oui.txt, either
 * IEEE's oui.txt ("AA-BB-CC   (hex)   Name") or one "AA:BB:CC<tab>Name" per
 * line, and is read once, the first time a survey with MACs is shown. */
const RED_OUI = {
  'a4:2b:8c': 'Netgear', 'b8:27:eb': 'Raspberry Pi', 'dc:a6:32': 'Raspberry Pi', 'e4:5f:01': 'Raspberry Pi',
  'd8:3a:dd': 'Raspberry Pi', '00:11:32': 'Synology', '3c:2e:ff': 'HP', 'f0:18:98': 'Apple', '8c:85:90': 'Apple',
  'a4:83:e7': 'Apple', 'ac:bc:32': 'Apple', '00:1b:63': 'Apple', '00:1a:11': 'Google', 'f4:f5:d8': 'Google',
  '18:b4:30': 'Nest', '50:c7:bf': 'TP-Link', 'c4:e9:84': 'TP-Link', 'ec:08:6b': 'TP-Link', '34:60:f9': 'Tuya',
  '68:57:2d': 'Espressif', '24:0a:c4': 'Espressif', '7c:df:a1': 'Espressif', '84:cc:a8': 'Espressif',
  '2c:f4:32': 'Espressif', 'b4:e6:2d': 'Espressif', '5c:cf:7f': 'Espressif', 'cc:50:e3': 'Espressif',
  '94:b9:7e': 'Espressif', 'a4:cf:12': 'Espressif', 'dc:4f:22': 'Espressif', '00:17:88': 'Philips Hue',
  '00:50:56': 'VMware', '52:54:00': 'QEMU/KVM', '00:e0:4c': 'Realtek', '30:ed:a0': 'LG',
};
let redOuiFile = null;                 /* null: not looked for yet */
async function redLoadOui() {
  if (redOuiFile) return;
  redOuiFile = {};
  const t = await fsText(RED_DIR + '/oui.txt').catch(() => null);
  if (!t) return;
  for (const l of t.split('\n')) {
    const m = /^\s*([0-9A-Fa-f]{2})[-:]?([0-9A-Fa-f]{2})[-:]?([0-9A-Fa-f]{2})\s+(?:\(hex\)\s+)?(.+?)\s*$/.exec(l);
    if (m && !/^\(base 16\)/.test(m[4])) redOuiFile[(m[1] + ':' + m[2] + ':' + m[3]).toLowerCase()] = m[4];
  }
}
/* A MAC with the locally administered bit set (second digit 2, 6, A or E) was
 * made up by the device: a phone's private address, which no maker owns and
 * which can change from one sweep to the next. */
function redVendor(mac) {
  if (!mac) return '';
  const k = mac.slice(0, 8).toLowerCase();
  const v = (redOuiFile && redOuiFile[k]) || RED_OUI[k];
  if (v) return v;
  return parseInt(mac[1], 16) & 2 ? 'MAC aleatoria' : '';
}

/* One survey, whatever the order of its lines; a line cut by a power loss is
 * skipped, not fatal. */
function redParse(text) {
  const s = { inicio: {}, fin: null, wifi: [], hosts: new Map() };
  const host = ip => {
    if (!s.hosts.has(ip)) s.hosts.set(ip, { ip, ports: [], servicios: [], name: '' });
    return s.hosts.get(ip);
  };
  for (const l of text.split('\n')) {
    if (!l.trim()) continue;
    let x;
    try { x = JSON.parse(l); } catch { continue; }
    if (x.t === 'inicio') s.inicio = x;
    else if (x.t === 'fin') s.fin = x;
    else if (x.t === 'wifi') s.wifi.push(x);
    else if (x.t === 'host') Object.assign(host(x.ip), { ping: x.ping, rtt: x.rtt, yo: x.yo, que: x.que, mac: x.mac, arp: x.arp });
    else if (x.t === 'puertos') host(x.ip).ports = (x.abiertos || []).slice().sort((a, b) => a - b);
    else if (x.t === 'nombre') {
      const hh = host(x.ip);
      if (!hh.name) hh.name = x.nombre || x.host || '';
      if (x.servicio && !hh.servicios.includes(x.servicio)) hh.servicios.push(x.servicio);
    }
  }
  s.list = [...s.hosts.values()].sort((a, b) => ipNum(a.ip) - ipNum(b.ip));
  s.wifi.sort((a, b) => b.rssi - a.rssi);
  return s;
}

/* What changed from one sweep to the next. By address: a phone that got
 * another one from the router shows as one gone and one new. */
function redDiff(cur, prev) {
  /* the same MAC at another address is a move, not one new and one gone
   * (a random phone MAC is not a person: it is not followed) */
  const fixed = m => m && !(parseInt(m[1], 16) & 2);
  const prevByMac = new Map(prev.list.filter(x => fixed(x.mac)).map(x => [x.mac, x]));
  const curMacs = new Set(cur.list.filter(x => fixed(x.mac)).map(x => x.mac));
  const movidos = cur.list.filter(x => fixed(x.mac) && prevByMac.has(x.mac) && prevByMac.get(x.mac).ip !== x.ip)
    .map(x => ({ x, antes: prevByMac.get(x.mac).ip }));
  const nuevos = cur.list.filter(x => !prev.hosts.has(x.ip) && !(fixed(x.mac) && prevByMac.has(x.mac)));
  const idos = prev.list.filter(x => !cur.hosts.has(x.ip) && !(fixed(x.mac) && curMacs.has(x.mac)));
  const puertos = [];
  for (const x of cur.list) {
    const p = prev.hosts.get(x.ip);
    if (!p) continue;
    const abiertos = x.ports.filter(n => !p.ports.includes(n)), cerrados = p.ports.filter(n => !x.ports.includes(n));
    if (abiertos.length || cerrados.length) puertos.push({ x, abiertos, cerrados });
  }
  const pb = new Set(prev.wifi.map(w => w.bssid)), cb = new Set(cur.wifi.map(w => w.bssid));
  return { nuevos, idos, movidos, puertos, redesNuevas: cur.wifi.filter(w => !pb.has(w.bssid)), redesIdas: prev.wifi.filter(w => !cb.has(w.bssid)) };
}

/* Where a network's energy is, in channel numbers (5 MHz apart, so 20 MHz is
 * 4 of them): its centre and half its width. */
function redSpan(w) {
  const width = w.ancho || 20;
  let c = w.canal;
  if (width === 40) c += 2 * (w.segundo || 0);
  else if (width >= 80) {
    const blocks = width === 160 ? [50, 114, 163] : [42, 58, 106, 122, 138, 155];
    c = blocks.find(b => Math.abs(b - w.canal) <= width / 10) || c;
  }
  return { c, half: width / 10 };
}

/* The graph of the channels: every network as a bell, as tall as its signal. */
function redChart(aps, band, mine) {
  const W = 720, H = 220, top = 18, base = H - 26;
  const lo = band === 2 ? -1 : Math.min(...aps.map(w => redSpan(w).c - redSpan(w).half)) - 2;
  const hi = band === 2 ? 15 : Math.max(...aps.map(w => redSpan(w).c + redSpan(w).half)) + 2;
  const X = c => 30 + (c - lo) / (hi - lo) * (W - 40);
  const Y = rssi => base - Math.max(0, Math.min(1, (rssi + 100) / 70)) * (base - top);
  let svg = `<svg viewBox="0 0 ${W} ${H}" class="redchart" role="img" aria-label="Canales ${band === 2 ? '2,4' : '5'} GHz">`;
  for (const r of [-90, -70, -50]) svg += `<line x1="30" x2="${W - 10}" y1="${Y(r)}" y2="${Y(r)}" class="grid"/><text x="2" y="${Y(r) + 4}" class="ax">${r}</text>`;
  const ticks = band === 2 ? [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13] : [...new Set(aps.map(w => w.canal))].sort((a, b) => a - b);
  for (const t of ticks) svg += `<text x="${X(t)}" y="${H - 8}" class="ax" text-anchor="middle">${t}</text>`;
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
  const labels = [];
  aps.forEach((w, i) => {
    const { c, half } = redSpan(w);
    const x0 = X(c - half), x1 = X(c + half), y = Y(w.rssi), k = (x1 - x0) * 0.32;
    const own = mine && w.ssid && w.ssid.startsWith(mine);     /* "Casa" and "Casa 5G" */
    const col = own ? 'var(--accent)' : `hsl(${(i * 67) % 360} 70% 60%)`;
    svg += `<path d="M${x0} ${base} C${x0 + k} ${y} ${x1 - k} ${y} ${x1} ${base}" fill="${col}" fill-opacity="${own ? .28 : .1}" stroke="${col}" stroke-width="${own ? 2.5 : 1.3}"><title>${esc(w.ssid || 'oculta')} · canal ${w.canal} · ${w.rssi} dBm</title></path>`;
    /* a name over the strong ones, unless it would land on another */
    const ly = Math.max(top, y - 5), lx = X(c), lw = (w.ssid || '(oculta)').length * 6.5;
    if ((w.rssi > -82 || own) && !labels.some(l => Math.abs(l.x - lx) < (l.w + lw) / 2 && Math.abs(l.y - ly) < 13)) {
      labels.push({ x: lx, y: ly, w: lw });
      svg += `<text x="${lx}" y="${ly}" text-anchor="middle" class="lab" fill="${col}">${esc(w.ssid || '(oculta)')}</text>`;
    }
  });
  return h('div', { class: 'redchartbox', html: svg + '</svg>' });
}

/* Of 1, 6 and 11 (the three that do not overlap in 2,4 GHz), the one the
 * neighbours use least, weighting each by its signal. Our own access points
 * do not count against themselves. */
function redBestChannel(aps, mine) {
  const load = c => aps.filter(w => w.canal <= 14 && w.ssid !== mine)
    .reduce((a, w) => { const s = redSpan(w); const d = Math.abs(s.c - c); return d < s.half + 2 ? a + Math.max(0, w.rssi + 100) * (1 - d / (s.half + 2)) : a; }, 0);
  return [1, 6, 11].map(c => ({ c, load: load(c) })).sort((a, b) => a.load - b.load);
}

function redCsv(rows) {
  const q = v => { const s = v == null ? '' : String(v); return /[",\n;]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s; };
  return new Blob(['﻿' + rows.map(r => r.map(q).join(',')).join('\r\n') + '\r\n'], { type: 'text/csv' });
}

function pageRed() {
  let files = [], cur = null, curName = '', timer = null, lastFile = null, gone = false;
  const sel = h('select'), cmp = h('select');
  const scanCard = h('div', { class: 'card pad' });
  const listCard = h('div');
  const out = h('div');
  const full = h('input', { type: 'checkbox' });
  const bar = h('div', { class: 'prog', style: 'display:none' }, h('div'));
  const msg = h('div', { class: 'muted small', style: 'margin-top:8px' });
  const live = h('div', { class: 'small', style: 'margin-top:6px' });
  const go = h('button', { class: 'btn pri', onclick: () => start() }, 'Barrer la red');
  const stop = h('button', { class: 'btn red', style: 'display:none', onclick: () => post('net', { do: 'stop' }).catch(e => toast(e.message, true)) }, 'Parar');

  put(scanCard,
    h('div', { class: 'btns' }, go, stop,
      h('label', { class: 'small', style: 'display:flex;gap:6px;align-items:center' }, full, 'Completo: también los que no contestan el ping'),
      h('button', { class: 'btn', onclick: () => openApp('aos.net').then(() => toast('Red abierta en la placa')).catch(e => toast(e.message, true)) }, 'Abrir Red en la placa')),
    bar, msg, live);

  const label = f => {
    const m = /^(\d{4})(\d\d)(\d\d)-(\d\d)(\d\d)(\d\d)?/.exec(f.name);
    return (m ? `${m[3]}/${m[2]}/${m[1]} ${m[4]}:${m[5]}` : f.name) + ' · ' + fmtBytes(f.size);
  };

  async function loadList(pick) {
    files = (await fsList(RED_DIR)).filter(f => !f.dir && /\.ndjson$/.test(f.name))
      .sort((a, b) => (b.mtime || 0) - (a.mtime || 0) || b.name.localeCompare(a.name));
    if (gone) return;
    if (!files.length) {
      put(listCard, h('div', { class: 'card pad muted' }, 'Todavía no hay relevamientos guardados. Cada barrido de la red queda en la tarjeta: hacé uno con el botón de arriba o desde la pestaña Hosts de la app Red.'));
      put(out);
      return;
    }
    put(sel, files.map(f => h('option', { value: f.name }, label(f))));
    sel.value = pick && files.some(f => f.name === pick) ? pick : files[0].name;
    put(listCard, h('div', { class: 'card pad' },
      h('div', { class: 'btns' }, sel,
        h('button', { class: 'btn', onclick: () => location.href = fsUrl(RED_DIR + '/' + sel.value, true) }, 'Descargar'),
        h('button', { class: 'btn', onclick: () => exportCsv('equipos') }, 'CSV de equipos'),
        h('button', { class: 'btn', onclick: () => exportCsv('redes') }, 'CSV de redes'),
        h('button', { class: 'btn red', onclick: remove }, 'Borrar')),
      h('div', { class: 'btns', style: 'margin-top:10px' }, h('span', { class: 'muted small' }, 'Comparar con'), cmp)));
    await show();
  }

  async function show() {
    curName = sel.value;
    const i = files.findIndex(f => f.name === curName);
    put(cmp, h('option', { value: '' }, 'nada'), files.filter(f => f.name !== curName).map(f => h('option', { value: f.name }, label(f))));
    cmp.value = files[i + 1] ? files[i + 1].name : '';         /* the one before, by default */
    const text = await fsText(RED_DIR + '/' + curName);
    if (gone || curName !== sel.value) return;
    cur = redParse(text || '');
    if (cur.list.some(x => x.mac)) await redLoadOui();
    await render();
  }

  async function render() {
    const s = cur, ini = s.inicio, fin = s.fin;
    const nports = s.list.reduce((a, x) => a + x.ports.length, 0);
    const named = s.list.filter(x => x.name).length;
    const summary = h('div', { class: 'card pad' },
      h('div', { class: 'redstats' },
        [[s.wifi.length, 'redes Wi-Fi'], [s.list.length, 'equipos'], [nports, 'puertos abiertos'], [named, 'con nombre']]
          .map(([n, t]) => h('div', {}, h('b', {}, String(n)), h('span', { class: 'muted small' }, t)))),
      h('div', { class: 'muted small', style: 'margin-top:10px' },
        [ini.fecha || 'sin fecha', ini.ssid ? `desde ${ini.ssid} (${ini.ip || '?'}${ini.rango ? ', ' + ini.rango : ''})` : 'sin red',
          ini.completo ? 'barrido completo' : null,
          fin ? (fin.ms / 1000).toFixed(1).replace('.', ',') + ' s' : null].filter(Boolean).join(' · '),
        fin && fin.cortado ? h('span', { class: 'warn' }, ' · cortado a mitad') : null,
        !fin ? h('span', { class: 'warn' }, ' · incompleto') : null));

    let diffCard = null;
    if (cmp.value) {
      const prevText = await fsText(RED_DIR + '/' + cmp.value);
      if (gone) return;
      const d = redDiff(s, redParse(prevText || ''));
      const who = x => x.ip + (x.name ? ' · ' + x.name : x.que ? ' · ' + x.que : redVendor(x.mac) ? ' · ' + redVendor(x.mac) : '');
      const lines = [
        ...d.nuevos.map(x => h('div', { class: 'row' }, h('div', { class: 'grow' }, h('span', { class: 'ok' }, '＋ '), who(x)), h('div', { class: 'val' }, 'equipo nuevo'))),
        ...d.idos.map(x => h('div', { class: 'row' }, h('div', { class: 'grow' }, h('span', { class: 'bad' }, '－ '), who(x)), h('div', { class: 'val' }, 'ya no está'))),
        ...d.movidos.map(({ x, antes }) => h('div', { class: 'row' }, h('div', { class: 'grow' }, h('span', { class: 'warn' }, '⇄ '), who(x)), h('div', { class: 'val' }, 'antes en ' + antes))),
        ...d.puertos.map(({ x, abiertos, cerrados }) => h('div', { class: 'row' }, h('div', { class: 'grow' }, h('span', { class: 'warn' }, '◆ '), who(x)),
          h('div', { class: 'val' }, [abiertos.length ? 'abrió ' + abiertos.join(', ') : '', cerrados.length ? 'cerró ' + cerrados.join(', ') : ''].filter(Boolean).join(' · ')))),
        ...d.redesNuevas.map(w => h('div', { class: 'row' }, h('div', { class: 'grow' }, h('span', { class: 'ok' }, '＋ '), (w.ssid || '(oculta)') + ' · canal ' + w.canal), h('div', { class: 'val' }, 'red nueva'))),
        ...d.redesIdas.map(w => h('div', { class: 'row' }, h('div', { class: 'grow' }, h('span', { class: 'bad' }, '－ '), (w.ssid || '(oculta)') + ' · canal ' + w.canal), h('div', { class: 'val' }, 'red que no se ve'))),
      ];
      diffCard = [h('h2', {}, 'Cambios desde el ' + label(files.find(f => f.name === cmp.value) || { name: cmp.value, size: 0 }).split(' · ')[0]),
        h('div', { class: 'card' }, lines.length ? lines : h('div', { class: 'row muted' }, 'Nada cambió: los mismos equipos, puertos y redes.'))];
    }

    let hostsCard = null;
    if (s.list.length) {
      const anyMac = s.list.some(x => x.mac);
      const portChip = (ip, p) => {
        const name = RED_PORTS[p];
        const txt = p + (name ? ' ' + name : '');
        return RED_WEB.has(p) ? h('a', { class: 'pill redport', href: `${p === 443 || p === 8443 ? 'https' : 'http'}://${ip}:${p}/`, target: '_blank', rel: 'noopener' }, txt)
          : h('span', { class: 'pill redport' }, txt);
      };
      hostsCard = [h('h2', {}, 'Equipos'), h('div', { class: 'card redwrap' }, h('table', { class: 'redt' },
        h('tr', {}, ['Dirección', 'Nombre', 'Qué es', 'Ping', 'Puertos', anyMac ? 'MAC' : null].filter(Boolean).map(t => h('th', {}, t))),
        s.list.map(x => h('tr', {},
          h('td', { class: 'mono', style: 'white-space:nowrap;word-break:normal' }, h('b', {}, x.ip), x.yo ? h('div', { class: 'muted small' }, 'esta placa') : null),
          h('td', {}, x.name ? h('b', {}, x.name) : h('span', { class: 'muted' }, '—'), x.servicios.length ? h('div', { class: 'muted small' }, x.servicios.join(' · ')) : null),
          h('td', { class: 'muted' }, x.que || ''),
          h('td', { class: 'muted', style: 'white-space:nowrap' }, x.yo ? '—' : x.ping ? (x.rtt != null ? x.rtt + ' ms' : 'sí') : x.arp ? 'sólo ARP' : 'no'),
          h('td', {}, x.ports.length ? x.ports.map(p => portChip(x.ip, p)) : h('span', { class: 'muted' }, 'ninguno')),
          anyMac ? h('td', { style: 'white-space:nowrap' }, h('span', { class: 'mono muted', style: 'word-break:normal' }, x.mac || '—'),
            redVendor(x.mac) ? h('div', { class: 'small' }, redVendor(x.mac)) : null) : null)))),
        h('p', { class: 'note' }, '«Sólo ARP» es un equipo que no contestó el ping ni ningún puerto, pero sí quién tiene esa dirección: existe igual, con el firewall puesto (celulares, Windows). Una MAC aleatoria es la dirección privada de un teléfono: no tiene fabricante y puede cambiar. Los puertos web abren el equipo en otra pestaña.' +
          (anyMac ? ' Para ver el fabricante de todas las MAC, poné la lista de la IEEE (oui.txt) en /redes de la tarjeta.' : ''))];
    }

    let wifiCard = null;
    if (s.wifi.length) {
      const g24 = s.wifi.filter(w => w.canal <= 14), g5 = s.wifi.filter(w => w.canal > 14);
      const best = g24.length ? redBestChannel(g24, ini.ssid) : null;
      const ours = g24.find(w => w.ssid === ini.ssid);
      const advice = best && h('p', { class: 'note' },
        `De los canales que no se pisan (1, 6 y 11), el menos cargado por los vecinos es el ${best[0].c}` +
        (ours ? (ours.canal === best[0].c ? `, y es el que usa ${ini.ssid}.` : `; ${ini.ssid} está en el ${ours.canal}.`) : '.'));
      wifiCard = [h('h2', {}, 'Redes Wi-Fi'),
        g24.length ? h('div', { class: 'card pad' }, h('div', { class: 'muted small' }, '2,4 GHz'), redChart(g24, 2, ini.ssid), advice) : null,
        g5.length ? h('div', { class: 'card pad', style: 'margin-top:12px' }, h('div', { class: 'muted small' }, '5 GHz'), redChart(g5, 5, ini.ssid)) : null,
        h('div', { class: 'card redwrap', style: 'margin-top:12px' }, h('table', { class: 'redt' },
          h('tr', {}, ['Red', 'Señal', 'Canal', 'Ancho', 'Seguridad', 'BSSID'].map(t => h('th', {}, t))),
          s.wifi.map(w => h('tr', {},
            h('td', {}, w.oculta || !w.ssid ? h('span', { class: 'muted' }, '(oculta)') : h('b', { class: w.ssid === ini.ssid ? 'ok' : '' }, w.ssid)),
            h('td', { style: 'white-space:nowrap' }, h('span', { class: 'redsig', style: `width:${Math.max(4, Math.min(60, (w.rssi + 100) * 0.9))}px` }), ' ', h('span', { class: 'muted small' }, w.rssi + ' dBm')),
            h('td', {}, String(w.canal)), h('td', { class: 'muted' }, (w.ancho || 20) + ' MHz'),
            h('td', { class: w.cifrado === 'abierta' ? 'warn' : 'muted' }, w.cifrado || ''),
            h('td', { class: 'mono muted', style: 'white-space:nowrap;word-break:normal' }, w.bssid || '')))))];
    }

    put(out, summary, diffCard, hostsCard, wifiCard,
      !s.list.length && !s.wifi.length ? h('div', { class: 'card pad muted' }, 'Este relevamiento no encontró nada.') : null);
  }

  function exportCsv(kind) {
    if (!cur) return;
    const base = curName.replace(/\.ndjson$/, '');
    if (kind === 'equipos') saveBlob(redCsv([['ip', 'nombre', 'servicios', 'que_es', 'ping_ms', 'solo_arp', 'puertos', 'mac', 'fabricante'],
      ...cur.list.map(x => [x.ip, x.name, x.servicios.join(' / '), x.que, x.ping ? x.rtt : '', x.arp ? 'si' : '', x.ports.join(' '), x.mac, redVendor(x.mac)])]), base + '-equipos.csv');
    else saveBlob(redCsv([['ssid', 'bssid', 'rssi_dbm', 'canal', 'ancho_mhz', 'seguridad'],
      ...cur.wifi.map(w => [w.ssid, w.bssid, w.rssi, w.canal, w.ancho || 20, w.cifrado])]), base + '-redes.csv');
  }

  async function remove() {
    if (!confirm('¿Borrar el relevamiento ' + label(files.find(f => f.name === sel.value)).split(' · ')[0] + '?')) return;
    try { await fsDelete(RED_DIR + '/' + sel.value); toast('Borrado'); loadList(); }
    catch (e) { toast(e.message, true); }
  }

  /* the sweep: polled once a second while it runs (every poll keeps it
   * alive: closing the page stops it) */
  function paint(st) {
    const busy = st.state === 'busy';
    go.style.display = busy ? 'none' : '';
    stop.style.display = busy ? '' : 'none';
    bar.style.display = busy ? '' : 'none';
    full.disabled = busy;
    const pct = st.phase === 0 && st.total ? st.done / st.total * 33 : st.phase === 1 ? 45 : 80;
    bar.firstChild.style.width = pct + '%';
    if (busy) msg.textContent = `${RED_PHASE[st.phase] || ''}… ${st.range || ''} · ${(st.elapsed_ms / 1000).toFixed(0)} s`;
    else if (st.state === 'failed') msg.innerHTML = '', msg.append(h('span', { class: 'bad' }, st.error || 'El barrido falló'));
    else if (st.state === 'done') msg.textContent = `Último barrido: ${st.hosts.length} equipo${st.hosts.length === 1 ? '' : 's'} en ${(st.elapsed_ms / 1000).toFixed(0)} s` + (st.file ? '. Guardado.' : '.');
    else msg.textContent = 'Barre la red de la placa (su /24): quién contesta, qué puertos tiene abiertos y cómo se llama; al final mira las redes Wi-Fi de alrededor y lo guarda en la tarjeta.';
    put(live, busy && st.hosts.length ? h('div', { class: 'muted' }, st.hosts.length + ' encontrados: ' +
      st.hosts.slice(0, 40).map(x => x.name || x.ip).join(', ') + (st.hosts.length > 40 ? '…' : '')) : null);
  }
  async function poll() {
    let st;
    try { st = await api('net'); } catch (e) { msg.textContent = e.message; return; }
    if (gone) return;
    paint(st);
    if (lastFile === null) lastFile = st.file;
    if (st.state === 'busy') { if (!timer) timer = setInterval(poll, 1000); return; }
    clearInterval(timer); timer = null;
    if (st.state === 'done' && st.file && st.file !== lastFile) { lastFile = st.file; toast('Barrido guardado'); loadList(st.file); }
  }
  async function start() {
    try { await post('net', { do: 'scan', full: full.checked }); } catch (e) { toast(e.message, true); return; }
    lastFile = lastFile ?? '';
    poll();
  }

  sel.onchange = show;
  cmp.onchange = render;
  put(main, h('h1', {}, 'Red'), scanCard, h('h2', {}, 'Relevamientos guardados'), listCard, out);
  poll();
  loadList().catch(e => toast(e.message, true));
  return () => { gone = true; clearInterval(timer); };
}

const PAGES = { pantalla: pagePantalla, inicio: pageInicio, wifi: pageWifi, red: pageRed, ha: pageHa, terminal: pageTerminal, programador: pageProgramador, banco: pageBanco, claude: pageClaude, mqtt: pageMqtt, macropad: pageMacropad, expansion: pageExpansion, archivos: pageArchivos, ajustes: pageAjustes, firmware: pageFirmware, registro: pageRegistro };

/* ---- the apps' own pages (docs/PORTAL-PAGES.md) ----
 * An app brings its page in apps/<x>/web/<x>.js; tools/install_apps.sh puts
 * it in the card's /web, the firmware serves it from there, and this loads
 * every .js it finds at startup, with its date in the address so the
 * browser never keeps an old one. A page is an ES module that calls
 * P4OS.registerPage(); P4OS is the whole API it gets, versioned like the
 * apps' AOS_ABI_VERSION: what is in version 1 stays as it is. A page that
 * fails to load or to draw is marked in the menu and the rest goes on. */
let leave = null;                   /* what the page on screen asked to run when it is left */
const NAV_BEFORE = 'archivos';      /* the apps' pages go above the system's */

function navEntry(id, name, icon, bad) {
  const nav = $('#nav');
  let a = nav.querySelector(`a[data-p="${CSS.escape(id)}"]`);
  if (!a) {
    a = h('a', { href: '#' + id, 'data-p': id });
    nav.insertBefore(a, nav.querySelector(`a[data-p="${NAV_BEFORE}"]`));
  }
  put(a, h('span', { class: 'ic' }, icon || '◇'), h('span', {}, name + (bad ? ' ⚠' : '')));
  a.title = bad || '';
  return a;
}

window.P4OS = Object.freeze({
  version: 1,
  main, h, put, $, api, post, toast, row, fmtBytes, fmtDate,
  /* the card: a missing file is null and a missing folder empty; fsPut
   * streams any size, with an optional progress callback (0..1) */
  fsText, fsBytes, fsList, fsPut, fsDelete, fsMkdir, fsUrl, fsSlug, saveBlob,
  openApp,
  /* { id, name, icon, render(main) }: render may return a function, called
   * when the page is left (to stop its timers) */
  registerPage(pg) {
    if (!pg || !/^[a-z0-9_-]+$/.test(pg.id || '') || typeof pg.render !== 'function') throw new Error('registerPage: id y render');
    if (PAGES[pg.id] && !PAGES[pg.id].plugged) throw new Error(`registerPage: "${pg.id}" es del portal`);
    const show = () => {
      try { return pg.render(main); } catch (e) {
        put(main, h('h1', {}, pg.name || pg.id), h('div', { class: 'card' }, h('p', { class: 'note' }, 'La página falló: ' + e.message)));
        console.error(e);
      }
    };
    show.plugged = true;
    PAGES[pg.id] = show;
    navEntry(pg.id, pg.name || pg.id, pg.icon);
  },
});

async function loadCardPages() {
  let files;
  try { files = (await fsList('/web')).filter(e => !e.dir && /^[A-Za-z0-9_-]+\.m?js$/.test(e.name)); }
  catch { return; }                 /* no card, or no /web */
  await Promise.all(files.map(async f => {
    try { await import(`/web/${f.name}?v=${f.mtime || 0}`); }
    catch (e) {
      const id = f.name.replace(/\.m?js$/, '').toLowerCase();
      navEntry(id, id, '◇', 'No cargó: ' + e.message);
      PAGES[id] = Object.assign(() => put(main, h('h1', {}, id),
        h('div', { class: 'card' }, h('p', { class: 'note' }, `/web/${f.name} no cargó: ${e.message}`))), { plugged: true });
      console.error(f.name, e);
    }
  }));
  /* the order the modules finished in is chance: by name, always the same */
  const nav = $('#nav'), before = nav.querySelector(`a[data-p="${NAV_BEFORE}"]`);
  [...nav.querySelectorAll('a')].filter(a => PAGES[a.dataset.p] && PAGES[a.dataset.p].plugged)
    .sort((a, b) => a.lastChild.textContent.localeCompare(b.lastChild.textContent)).forEach(a => nav.insertBefore(a, before));
}

function route() {
  const p = (location.hash || '#pantalla').slice(1);
  if (leave) { try { leave(); } catch {} leave = null; }
  document.querySelectorAll('.nav a').forEach(a => a.classList.toggle('on', a.dataset.p === p));
  const r = (PAGES[p] || pagePantalla)();
  if (typeof r === 'function') leave = r;
}
/* ---- access (aos_access.h on the board) ----
 * The board says, per request, whether this network needs the password
 * (GET /api/auth). Without a session the portal shows only this: the
 * password, sent once to /api/login, which answers with a cookie the
 * page's scripts cannot read. The rules themselves are set on the board,
 * Settings, Portal web. */
let auth = null;
function loginScreen(msg) {
  if (document.getElementById('login')) return;
  const pw = h('input', { type: 'password', autocomplete: 'current-password', placeholder: 'contraseña', required: true });
  const err = h('p', { class: 'bad small', style: 'min-height:1.4em;margin:10px 0 0' }, msg || '');
  const go = async e => {
    e.preventDefault();
    try {
      const r = await fetch('/api/login', { method: 'POST', body: JSON.stringify({ password: pw.value }) });
      const j = await r.json().catch(() => ({}));
      if (r.ok && j.ok) { location.reload(); return; }
      err.textContent = j.error + (j.wait_s ? ` (${j.wait_s} s)` : '');
      pw.select();
    } catch (x) { err.textContent = x.message; }
  };
  const name = (auth && auth.name) || 'p4os';
  document.body.append(h('div', { id: 'login', class: 'login' },
    h('form', { class: 'card pad', onsubmit: go },
      h('h1', {}, name),
      h('p', { class: 'muted' }, 'Esta red pide la contraseña del portal.'),
      /* for the browser's password manager: which device this is */
      h('input', { type: 'text', autocomplete: 'username', value: name, style: 'display:none', readonly: true }),
      pw, err,
      h('div', { class: 'btns', style: 'margin-top:14px' }, h('button', { class: 'btn pri', type: 'submit' }, 'Entrar')),
      h('p', { class: 'note' }, 'Se pone, se cambia o se quita en la placa: Ajustes, Portal web. ',
        h('a', { href: '/api/tls/ca' }, 'La autoridad HTTPS de la placa'), ', para que el navegador confíe en ella.'))));
  pw.focus();
}

async function logout() {
  await fetch('/api/logout', { method: 'POST' }).catch(() => {});
  location.reload();
}

(async () => {
  try { auth = await (await fetch('/api/auth')).json(); } catch { auth = null; }
  if (auth && auth.need === 'login' && !auth.ok) { loginScreen(); return; }
  if (auth && auth.session)
    $('#nav').append(h('a', { href: '#', class: 'logout', onclick: e => { e.preventDefault(); logout(); } },
      h('span', { class: 'ic' }, '⏻'), h('span', {}, 'Cerrar sesión')));
  window.addEventListener('hashchange', route);
  refreshInfo();
  setInterval(refreshInfo, 10000);
  route();
  /* a deep link to an app's page (#hola) lands before the page is known */
  loadCardPages().then(() => { const p = (location.hash || '').slice(1); if (PAGES[p] && PAGES[p].plugged) route(); });
})();
