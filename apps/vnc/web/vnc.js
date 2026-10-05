/* The VNC viewer's page in the board's portal (docs/PORTAL-PAGES.md): the
 * computers the viewer knows, as cards to edit instead of the file by hand.
 *
 * The file is the app's (apps/vnc/main/vnc_cfg.c), /data/vnc.txt: one
 * [Name] block per computer with host, port, pass, enc (auto, tight, zrle,
 * hextile, raw), depth (16 or 24), quality (Tight's JPEG, 0-9) and view
 * (1: look only), up to sixteen. The page reads it, shows each computer and
 * writes it back whole; the app, open on the list, reloads it within two
 * seconds. Lines it does not know are not kept.
 *
 * The passwords are in the file in the clear, as the app keeps them, and
 * both say so where they are typed. VNC uses only their first 8
 * characters. */
const P = window.P4OS;
const { main, h, put, toast, fsText, fsPut, openApp } = P;

if (!document.getElementById('css-vnc')) {
  const st = document.createElement('style');
  st.id = 'css-vnc';
  st.textContent = `
.vnlist { display: grid; gap: 14px; }
.vncard { background: var(--card); border-radius: var(--radius); padding: 16px 20px; }
.vnhead { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.vnhead .nm { flex: 1; min-width: 120px; font-weight: 650; font-size: 17px; overflow-wrap: anywhere; }
.vnhead .tag { font-size: 11px; padding: 1px 8px; border-radius: 9px; background: var(--card2); color: var(--dim); }
.vngrid { display: grid; grid-template-columns: 2fr 1fr; gap: 4px 14px; }
.vngrid .wide { grid-column: 1 / -1; }
.vngrid label { font-size: 13px; color: var(--dim); display: grid; gap: 4px; margin-top: 8px; }
.vngrid select, .vngrid input { width: 100%; box-sizing: border-box; }
.vncheck { display: flex !important; align-items: center; gap: 8px; grid-column: 1 / -1; }
.vncheck input { width: auto; }
.vnwarn { color: var(--orange); font-size: 12px; margin-top: 4px; }
@media (max-width: 600px) { .vngrid { grid-template-columns: 1fr; } }
`;
  document.head.append(st);
}

const FILE = '/data/vnc.txt';
const MAX = 16;
const ENCS = [['auto', 'Automática (Tight, ZRLE, Hextile)'], ['tight', 'Tight (JPEG en las fotos)'],
  ['zrle', 'ZRLE (la de la Mac)'], ['hextile', 'Hextile'], ['raw', 'Raw (sin comprimir)']];
const HEADER = `# Computers for the VNC viewer of P4OS (apps/vnc). One block each:
# [Name]  host, port (5900), pass, enc (auto tight zrle hextile raw),
# depth (16 or 24), quality (0-9), view (1: look only).
# The passwords are kept here in the clear.
`;

const blank = () => ({ name: '', host: '', port: '5900', pass: '', enc: 'auto', depth: '16', quality: '6', view: '0' });

/* vnc.txt -> [{ name, host, port, pass, enc, depth, quality, view }] */
function parse(text) {
  const list = [];
  let cur = null;
  for (const raw of (text || '').split('\n')) {
    const s = raw.trim();
    if (!s || s[0] === '#' || s[0] === ';') continue;
    const sec = s.match(/^\[(.*)\]$/);
    if (sec) {
      cur = Object.assign(blank(), { name: sec[1].trim() });
      list.push(cur);
      continue;
    }
    const kv = s.match(/^([a-z]+)\s*=\s*(.*)$/i);
    if (cur && kv && kv[1].toLowerCase() in cur && kv[1] !== 'name') cur[kv[1].toLowerCase()] = kv[2].trim();
  }
  return list;
}

function serialize(list) {
  let out = HEADER;
  for (const c of list) {
    out += `\n[${c.name}]\nhost = ${c.host}\nport = ${c.port || 5900}\n`;
    if (c.pass) out += `pass = ${c.pass}\n`;
    if (c.enc && c.enc !== 'auto') out += `enc = ${c.enc}\n`;
    if (c.depth === '24') out += 'depth = 24\n';
    if (c.quality !== '' && c.quality !== '6') out += `quality = ${c.quality}\n`;
    if (c.view === '1') out += 'view = 1\n';
  }
  return out;
}

function check(list) {
  const seen = new Set();
  for (const [i, c] of list.entries()) {
    const n = c.name || `computadora ${i + 1}`;
    if (!c.name || c.name.length > 47 || /[\[\]\n]/.test(c.name)) return `${n}: el nombre, de 1 a 47 caracteres y sin corchetes`;
    if (seen.has(c.name.toLowerCase())) return `${c.name}: hay dos con ese nombre`;
    seen.add(c.name.toLowerCase());
    if (!c.host || /\s/.test(c.host) || c.host.length > 95) return `${c.name}: falta la dirección (IP o nombre, sin espacios)`;
    const p = +c.port;
    if (!/^\d+$/.test(c.port) || p < 1 || p > 65535) return `${c.name}: el puerto va de 1 a 65535 (VNC suele ser 5900)`;
    if (!/^\d$/.test(c.quality)) return `${c.name}: la calidad va de 0 a 9`;
    if (/\n/.test(c.pass) || c.pass.length > 63) return `${c.name}: la contraseña es muy larga`;
  }
  return null;
}

function render() {
  let list = [], dirty = false;
  const listBox = h('div', { class: 'vnlist' });
  const addBtn = h('button', { class: 'btn' }, '+ Agregar una computadora');
  const saveBtn = h('button', { class: 'btn pri' }, 'Guardar en la tarjeta');

  const changed = () => { dirty = true; };

  function input(c, key, label, attrs = {}) {
    const inp = h('input', Object.assign({ type: 'text', value: c[key] || '' }, attrs));
    inp.oninput = () => {
      c[key] = key === 'pass' ? inp.value : inp.value.trim();
      changed();
      if (key === 'name') inp.closest('.vncard').querySelector('.nm').textContent = c.name || 'Sin nombre';
    };
    return h('label', { class: attrs.wide ? 'wide' : null }, label, inp);
  }

  function select(c, key, label, options) {
    const sel = h('select', {}, options.map(([v, t]) => h('option', { value: v }, t)));
    sel.value = c[key];
    sel.onchange = () => { c[key] = sel.value; changed(); };
    return h('label', {}, label, sel);
  }

  function paint() {
    if (!list.length) {
      put(listBox, h('div', { class: 'vncard muted' }, 'Todavía no hay computadoras: agregá una.'));
    } else {
      put(listBox, list.map((c, i) => {
        const move = d => { const j = i + d; [list[i], list[j]] = [list[j], list[i]]; changed(); paint(); };
        const view = h('input', { type: 'checkbox' });
        view.checked = c.view === '1';
        view.onchange = () => { c.view = view.checked ? '1' : '0'; changed(); };
        const show = h('input', { type: 'checkbox' });
        const pass = input(c, 'pass', 'Contraseña VNC', { type: 'password', autocomplete: 'new-password', maxlength: 63 });
        show.onchange = () => { pass.querySelector('input').type = show.checked ? 'text' : 'password'; };
        return h('div', { class: 'vncard' },
          h('div', { class: 'vnhead' },
            h('span', { class: 'nm' }, c.name || 'Sin nombre'),
            c.pass ? h('span', { class: 'tag' }, 'con contraseña') : null,
            h('button', { class: 'btn', title: 'Subir', disabled: i === 0, onclick: () => move(-1) }, '↑'),
            h('button', { class: 'btn', title: 'Bajar', disabled: i === list.length - 1, onclick: () => move(1) }, '↓'),
            h('button', { class: 'btn red', onclick: () => {
              if (confirm(`¿Quitar ${c.name || 'esta computadora'}?`)) { list.splice(i, 1); changed(); paint(); }
            } }, 'Quitar')),
          h('div', { class: 'vngrid' },
            input(c, 'name', 'Nombre', { maxlength: 47, placeholder: 'Mac del estudio', wide: true }),
            input(c, 'host', 'Dirección (IP o nombre)', { placeholder: '192.168.0.20', autocomplete: 'off' }),
            input(c, 'port', 'Puerto', { inputmode: 'numeric', placeholder: '5900' }),
            pass,
            h('label', { class: 'vncheck' }, show, 'Mostrar la contraseña'),
            select(c, 'enc', 'Codificación', ENCS),
            select(c, 'depth', 'Colores', [['16', '16 bits (rápido)'], ['24', '24 bits (exactos)']]),
            select(c, 'quality', 'Calidad JPEG (Tight)', [...Array(10).keys()].map(q => [String(q), String(q)])),
            h('label', { class: 'vncheck' }, view, 'Sólo mirar: no manda el mouse ni el teclado')),
          h('div', { class: 'vnwarn' }, 'La contraseña queda en /data/vnc.txt de la tarjeta, legible. VNC usa sólo los primeros 8 caracteres.'));
      }));
    }
    addBtn.disabled = list.length >= MAX;
  }

  addBtn.onclick = () => {
    if (list.length >= MAX) return;
    list.push(blank());
    changed();
    paint();
    listBox.lastChild.querySelector('input').focus();
  };

  saveBtn.onclick = async () => {
    const err = check(list);
    if (err) { toast(err, true); return; }
    try {
      await fsPut(FILE, serialize(list));
      dirty = false;
      toast('Guardado. La app lo toma sola.');
    } catch (e) { toast('No se pudo guardar: ' + e.message, true); }
  };

  async function load() {
    try {
      list = parse(await fsText(FILE));
    } catch (e) { toast('No se pudo leer ' + FILE + ': ' + e.message, true); }
    paint();
  }

  put(main,
    h('h1', {}, 'VNC'),
    h('div', { class: 'btns', style: 'margin-bottom:6px' },
      h('button', { class: 'btn pri', onclick: () => openApp('aos.vnc').catch(e => toast(e.message, true)) }, 'Abrir VNC en la placa')),
    h('h2', {}, 'Las computadoras'),
    listBox,
    h('div', { class: 'btns', style: 'margin-top:14px' }, addBtn, saveBtn),
    h('h2', {}, 'Cómo prepararlas'),
    h('p', { class: 'note' }, 'En una Mac: Ajustes del Sistema → General → Compartir → Compartir pantalla → (i) → «Los visores VNC pueden controlar la pantalla con contraseña», y una contraseña de hasta 8 caracteres. La Mac manda su resolución real (en una Retina, el doble): la placa la achica sola. En Windows o Linux, un servidor VNC como TightVNC, TigerVNC o x11vnc; Tight es la codificación más rápida con fotos y video.'),
    h('p', { class: 'note' }, 'En la placa: un toque es un clic, arrastrar arrastra, dos dedos tocando son clic derecho, pellizcar hace zoom. El mouse y el teclado USB pasan directo.'));

  window.onbeforeunload = () => dirty && location.hash === '#vnc' ? 'Hay cambios sin guardar' : undefined;
  load();
  return () => { window.onbeforeunload = null; };
}

P.registerPage({ id: 'vnc', name: 'VNC', icon: '▣', render });
