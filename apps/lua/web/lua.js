/* Lua's page in the board's portal (docs/PORTAL-PAGES.md), moved out
 * of the firmware's app.js on 2026-10-04 as it was: tools/install_apps.sh
 * puts this file in the card's /web and the portal loads it from there.
 * window.P4OS (version 1) is all it uses, and its styles come with it. */
const P = window.P4OS;
const { main, h, put, $, api, post, toast, row, fmtBytes, fmtDate, fsPut, fsList, fsDelete, fsMkdir, fsUrl,
  fsText, fsBytes, openApp, saveBlob, fsSlug } = P;

if (!document.getElementById('css-lua')) {
  const st = document.createElement('style');
  st.id = 'css-lua';
  st.textContent = `
.luacols { display: grid; grid-template-columns: minmax(0, 1fr) minmax(300px, 400px); gap: 18px; align-items: start; }
.luata { width: 100%; min-height: 460px; background: var(--card); border: 1px solid var(--line); border-radius: 12px; padding: 12px 14px; outline: none;
  font: 14px/1.5 ui-monospace, "JetBrains Mono", Menlo, monospace; resize: vertical; tab-size: 4; }
.luata:focus { border-color: var(--accent); }
.luacon { margin-top: 12px; background: #14100f; border: 1px solid var(--line); border-radius: 12px; padding: 10px 14px; min-height: 44px; white-space: pre-wrap;
  word-break: break-word; color: var(--dim); font: 13px/1.5 ui-monospace, Menlo, monospace; }
.luacon.bad { color: #ff6b6b; border-color: #4a2626; background: #1a0f0f; }
.luacon.ok { color: var(--green); }
.luawho { display: block; color: var(--dim); font-size: 12px; margin-bottom: 3px; }
.luarun { box-shadow: inset 3px 0 0 var(--accent); }
.luahelp { font-size: 13px; color: var(--dim); position: sticky; top: 16px; }
.luahelp h2 { margin: 14px 0 6px; } .luahelp h2:first-child { margin-top: 0; }
.luahelp dl { margin: 0; display: grid; grid-template-columns: auto 1fr; gap: 3px 10px; }
.luahelp dl { grid-template-columns: minmax(0, 1.1fr) minmax(0, 1fr); }
.luahelp dt { color: var(--accent); font-family: ui-monospace, Menlo, monospace; font-size: 12.5px; overflow-wrap: anywhere; }
.luahelp dd { margin: 0; }
@media (max-width: 1000px) { .luacols { grid-template-columns: 1fr; } .luahelp { position: static; } }
`;
  document.head.append(st);
}

/* ---- Lua ---- */
/* Scripts for the Lua app (apps/lua), in /lua on the card. The firmware
 * knows nothing of Lua: this is the file API, as AmoledOS's /lua page was
 * (lua.html). The app watches the file of the script it is running and
 * reloads it when it changes, so Guardar is the whole step; it writes
 * /lua/_estado.txt, its first line the script it runs and the rest its error,
 * which the console here reads every three seconds. Each script is an app of
 * its own ("lua.cubo" is cubo.lua) from the boot that saw it; a new one runs
 * from the list inside the app until then. */
const LUA_DIR = '/lua', LUA_STATE = '/lua/_estado.txt';
const LUA_TEMPLATE = `-- nombre del guión
--
-- Las funciones son opcionales. Lo que sobreviva entre cuadros va acá
-- afuera, como local.

local n = 0

function draw()
    n = n + 1
    aos.clear(0x001018)
    aos.disc(aos.W // 2, aos.H // 2, 40 + n % 30, 0x00E5FF)
    aos.text(12, 12, "HOLA", 0xFFFFFF, 3)
end

function touch(x, y, ev)
    if ev == "down" then aos.beep(1200, 25) end
end
`;
const LUA_HELP = [['Funciones', [['init()', 'una vez, antes del primer cuadro'], ['tick(dt)', 'cada cuadro; dt en milisegundos'], ['draw()', 'cada cuadro, después de tick'],
    ['touch(x, y, ev)', 'ev es "down", "move" o "up"'], ['gesture(ev, x, y, a, b, c)', '"tap", "double", "long", "drag", "release", "pinchstart", "pinch", "pinchend"'],
    ['resize(w, h)', 'la pantalla giró (lienzo por defecto)']]],
  ['La tabla aos', [['aos.W, aos.H', 'el lienzo: 240 × 426 parado, a ×3'], ['clear(c)', 'llena todo'], ['pixel(x, y, c)', ''], ['rect(x, y, w, h, c)', 'relleno'],
    ['frame(x, y, w, h, c)', 'contorno'], ['line(x0, y0, x1, y1, c)', ''], ['disc(cx, cy, r, c)', 'relleno'], ['ring(cx, cy, r, c)', 'contorno'],
    ['text(x, y, s, c [, esc])', '5×7, mayúsculas, dígitos y signos'], ['shade(x, y, w, h, f)', 'oscurece (f<0) o aclara, en dieciseisavos'],
    ['touch()', 'x, y, apoyado'], ['fingers()', 'n, id1, x1, y1, id2, x2, y2…'], ['ms()', 'desde que arrancó el guión'], ['beep(hz, ms)', ''],
    ['background()', 'congela lo dibujado como fondo'], ['stats()', 'guión, pantalla y cuadro en ms, filas, píxeles']]],
  ['En las primeras líneas', [['-- @canvas WxH', 'otro lienzo (16..1280 por lado), a la escala entera que entre'], ['-- @orientation portrait', 'o landscape: gira la pantalla al abrirlo desde el inicio']]]];

let luaT, luaRunning = '';
async function pageLua() {
  let files = [], orig = '', saved = '', view = 'list', ids = new Set();
  const listBox = h('div', { class: 'card' });
  const conList = h('div', { class: 'luacon' }), conEd = h('div', { class: 'luacon' });
  const nameIn = h('input', { type: 'text', placeholder: 'nombre.lua', maxlength: 40, class: 'mono' });
  const status = h('span', { class: 'muted small' });
  const ta = h('textarea', { class: 'luata', spellcheck: 'false', autocapitalize: 'off', autocorrect: 'off' });
  const listSec = h('div', {}), edSec = h('div', { style: 'display:none' });
  const say = (t, cls) => { status.textContent = t; status.className = 'small ' + (cls || 'muted'); };
  const fileOf = () => { let n = nameIn.value.trim(); if (n && !/\.lua$/i.test(n)) n += '.lua'; return n; };
  const idOf = f => 'lua.' + f.replace(/\.lua$/i, '');

  async function load() {
    try {
      files = (await fsList(LUA_DIR)).filter(f => !f.dir && /\.lua$/i.test(f.name) && f.name[0] !== '.').sort((a, b) => a.name.localeCompare(b.name));
      ids = new Set((await api('apps')).map(a => a.id));
    } catch (e) { put(listBox, h('div', { class: 'row bad' }, e.message)); return; }
    await consoleRead();
    put(listBox, files.length ? files.map(f => h('div', { class: 'row' + (f.name === luaRunning ? ' luarun' : '') },
      h('div', { class: 'grow' }, h('div', { class: 'mono ell' }, f.name),
        h('div', { class: 'muted small' }, fmtBytes(f.size) + (f.mtime ? ' · ' + fmtDate(f.mtime) : '') + (f.name === luaRunning ? ' · el último que corrió la placa' : ''))),
      h('div', { class: 'btns' },
        h('button', { class: 'btn', onclick: () => run(f.name) }, 'Correr'),
        h('a', { class: 'btn', href: fsUrl(LUA_DIR + '/' + f.name, true) }, 'Bajar'),
        h('button', { class: 'btn pri', onclick: () => edit(f.name) }, 'Editar')))) :
      h('div', { class: 'row muted' }, 'Todavía no hay guiones. Creá el primero con «Nuevo guión».'));
  }

  /* _estado.txt: the script, then its error */
  async function consoleRead() {
    let first = '', err = '';
    try {
      const t = await fsText(LUA_STATE);
      if (t != null) { const l = t.split('\n'); first = (l.shift() || '').trim(); err = l.join('\n').trim(); }
    } catch { return; }
    luaRunning = first;
    for (const box of [conList, conEd]) {
      box.className = 'luacon' + (err ? ' bad' : first ? ' ok' : '');
      put(box, first ? [h('span', { class: 'luawho' }, 'En la placa: ' + first), err || 'corriendo, sin errores'] : 'La placa todavía no corrió ningún guión.');
    }
  }

  async function edit(file) {
    let text;
    try { text = await fsText(LUA_DIR + '/' + file); } catch (e) { toast(e.message, true); return; }
    if (text == null) { toast('No existe ' + file, true); return; }
    orig = file; nameIn.value = file; ta.value = saved = text; say('');
    show('editor');
  }
  function fresh() {
    orig = ''; nameIn.value = ''; ta.value = LUA_TEMPLATE; saved = null; say('');
    show('editor');
    nameIn.focus();
  }
  function show(v) {
    view = v;
    listSec.style.display = v === 'list' ? '' : 'none';
    edSec.style.display = v === 'editor' ? '' : 'none';
    if (v === 'list') load(); else consoleRead();
  }
  const dirty = () => view === 'editor' && ta.value !== saved;
  function back() {
    if (dirty() && !confirm('Hay cambios sin guardar. ¿Volver igual?')) return;
    show('list');
  }

  async function save() {
    const file = fileOf();
    if (!file) { say('Ponele un nombre al guión', 'bad'); return false; }
    if (!/^[^/\\]+\.lua$/i.test(file) || file[0] === '.' || file[0] === '_') { say('El nombre no puede tener barras ni empezar con punto o guion bajo', 'bad'); return false; }
    if (file !== orig && files.some(f => f.name === file) && !confirm(`Ya hay un ${file}. ¿Reemplazarlo?`)) return false;
    say('Guardando…');
    try {
      await fsMkdir(LUA_DIR);
      const body = ta.value;
      await fsPut(LUA_DIR + '/' + file, body);
      /* renaming is saving with the new name and deleting the old one */
      if (orig && orig !== file) { await fsDelete(LUA_DIR + '/' + orig).catch(() => {}); if (luaRunning === orig) luaRunning = file; }
      orig = file; nameIn.value = file; saved = body;
      files = (await fsList(LUA_DIR)).filter(f => !f.dir && /\.lua$/i.test(f.name));
      /* the one running is reloaded by the board within a second: worth saying */
      say(file === luaRunning ? 'Guardado: la placa lo está recargando' : 'Guardado en la placa', 'ok');
      setTimeout(consoleRead, 1500);
      return true;
    } catch (e) { say('No se pudo guardar: ' + e.message, 'bad'); return false; }
  }

  /* its own app if the board has seen it since booting; the list app otherwise */
  async function run(file) {
    try {
      if (!ids.size) ids = new Set((await api('apps')).map(a => a.id));
      if (ids.has(idOf(file))) { await openApp(idOf(file)); toast('Corriendo ' + file + ' en la placa'); }
      else { await openApp('aos.lua'); toast(file + ' es nuevo: aparece en el inicio después de reiniciar. Abrí la lista de Lua y tocalo ahí.'); }
      setTimeout(consoleRead, 1500);
    } catch (e) { toast(e.message, true); }
  }

  async function del() {
    if (!orig) { show('list'); return; }
    if (!confirm(`¿Borrar ${orig} de la placa?`)) return;
    try { await fsDelete(LUA_DIR + '/' + orig); toast('Borrado ' + orig); saved = ta.value; show('list'); } catch (e) { toast(e.message, true); }
  }

  function uploadLua(fl) {
    const list = [...fl].filter(f => /\.(lua|aic)$/i.test(f.name));
    if (!list.length) { toast('Sólo archivos .lua (y sus íconos .aic)', true); return; }
    (async () => {
      await fsMkdir(LUA_DIR);
      for (const f of list) try { await fsPut(LUA_DIR + '/' + f.name, f); } catch (e) { toast(f.name + ': ' + e.message, true); return; }
      toast(list.length === 1 ? 'Subido ' + list[0].name : `Subidos ${list.length} archivos`);
      load();
    })();
  }

  ta.addEventListener('keydown', e => {
    if ((e.metaKey || e.ctrlKey) && e.key === 's') { e.preventDefault(); save(); }
    else if (e.key === 'Tab' && !e.shiftKey && !e.metaKey && !e.ctrlKey) {           /* four spaces, as the samples */
      e.preventDefault();
      const s = ta.selectionStart;
      ta.setRangeText('    ', s, ta.selectionEnd, 'end');
    }
  });
  ta.addEventListener('input', () => { if (status.textContent.startsWith('Guardado')) say(''); });
  const input = h('input', { type: 'file', multiple: true, accept: '.lua,.aic', style: 'display:none', onchange: e => uploadLua(e.target.files) });
  const drop = h('div', { class: 'drop' }, 'Soltá acá archivos .lua para subirlos a ', h('b', {}, LUA_DIR));
  drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); uploadLua(e.dataTransfer.files); };

  put(listSec, h('div', { class: 'btns', style: 'margin-bottom:14px' },
      h('button', { class: 'btn pri', onclick: fresh }, 'Nuevo guión'),
      h('button', { class: 'btn', onclick: () => input.click() }, 'Subir…'), input,
      h('button', { class: 'btn', onclick: () => openApp('aos.lua').then(() => toast('Abriendo Lua en la placa'), e => toast(e.message, true)) }, 'Abrir la app en la placa')),
    listBox, conList, drop);
  put(edSec, h('div', { class: 'btns', style: 'margin-bottom:14px;flex-wrap:wrap' },
      h('button', { class: 'btn', onclick: back }, '← Guiones'),
      h('div', { style: 'flex:1 1 220px' }, nameIn), status),
    h('div', { class: 'luacols' },
      h('div', { style: 'min-width:0' }, ta, conEd,
        h('div', { class: 'btns', style: 'margin-top:14px' },
          h('button', { class: 'btn pri', onclick: save }, 'Guardar en la placa'),
          h('button', { class: 'btn', onclick: async () => { if (dirty() || !orig) { if (!await save()) return; } run(orig); } }, 'Guardar y correr'),
          h('button', { class: 'btn red', onclick: del }, 'Borrar de la placa'),
          h('span', { class: 'muted small' }, '⌘S / Ctrl+S guarda'))),
      h('div', { class: 'card pad luahelp' }, LUA_HELP.flatMap(([t, rows]) => [h('h2', {}, t), h('dl', {}, rows.flatMap(([k, v]) => [h('dt', {}, k), h('dd', {}, v)]))]),
        h('p', {}, 'Los colores son 0xRRGGBB. Están string, table, math, utf8 y coroutine; no están io, os, package ni debug. Una llamada que tarda más de 400 ms se corta con un error.'))));
  put(main, h('h1', {}, 'Lua'), listSec, edSec,
    h('p', { class: 'note' }, 'Los .lua viven en /lua de la tarjeta. La placa vigila el archivo del guión que está corriendo y lo recarga cuando cambia, así que guardar acá alcanza. Un error no reinicia nada: sale en la pantalla y acá, con su número de línea. Un guión nuevo es una app del inicio desde el próximo arranque.'));
  window.onbeforeunload = () => dirty() && location.hash === '#lua' ? 'Hay cambios sin guardar' : undefined;
  show('list');
  clearTimeout(luaT);
  /* every three seconds, with the tab in view: the board's portal goes deaf if hammered */
  const tick = () => { if (location.hash !== '#lua' || !ta.isConnected) return; if (!document.hidden) consoleRead(); luaT = setTimeout(tick, 3000); };
  luaT = setTimeout(tick, 3000);
}

P.registerPage({ id: 'lua', name: 'Lua', icon: '☾', render: () => { pageLua(); } });
