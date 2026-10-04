/* The Cameras app's page in the board's portal (docs/PORTAL-PAGES.md): the
 * cameras in /cameras.txt as cards to edit, instead of the file by hand.
 *
 * The file is the app's (apps/camaras/main/cam_cfg.c): one [Name] block per
 * camera with url (the mosaic's stream), full (the full screen's, optional),
 * user, pass and refresh (seconds, for a URL that answers one JPEG), up to
 * eight, and an optional [Frigate] block with its url. This page reads it,
 * shows each camera, and writes it back whole, with the app's explanation on
 * top. Lines it does not know are not kept: the page is the editor now.
 *
 * A preview is the browser's own: an MJPEG or JPEG camera over http:// is
 * shown here straight from the camera, without credentials (browsers do not
 * send them inside an image's address). RTSP has no preview in a browser.
 * The passwords are in the file in the clear, as the app keeps them; the
 * page shows them masked. */
const P = window.P4OS;
const { main, h, put, toast, fsText, fsPut, openApp } = P;

if (!document.getElementById('css-camaras')) {
  const st = document.createElement('style');
  st.id = 'css-camaras';
  st.textContent = `
.cmlist { display: grid; gap: 14px; }
.cmcard { background: var(--card); border-radius: var(--radius); padding: 16px 20px; }
.cmhead { display: flex; align-items: center; gap: 10px; }
.cmhead .nm { flex: 1; font-weight: 650; font-size: 17px; }
.cmhead .kind { font-size: 11px; padding: 1px 8px; border-radius: 9px; background: var(--card2); color: var(--dim); }
.cmgrid { display: grid; grid-template-columns: 1fr 1fr; gap: 4px 14px; }
.cmgrid .wide { grid-column: 1 / -1; }
.cmgrid label { font-size: 13px; color: var(--dim); display: grid; gap: 4px; margin-top: 8px; }
.cmprev { margin-top: 12px; border-radius: 12px; overflow: hidden; background: #000; max-width: 640px; }
.cmprev img { display: block; width: 100%; }
.cmprev .msg { color: var(--dim); font-size: 13px; padding: 12px; }
@media (max-width: 700px) { .cmgrid { grid-template-columns: 1fr; } }
`;
  document.head.append(st);
}

const FILE = '/cameras.txt';
const MAX = 8;
const HEADER = `# Cameras for P4OS, written by the portal's Cámaras page. One block per
# camera, up to 8:
#
# [Entrada]
# url  = rtsp://192.168.0.10:554/Streaming/Channels/102
# full = rtsp://192.168.0.10:554/Streaming/Channels/101
# user = admin
# pass = secret
#
# url   rtsp:// for H.264 Baseline or MJPEG over RTP (decoded on the board),
#       http:// for MJPEG (go2rtc: http://host:1984/api/stream.mjpeg?src=cam)
#       or for a single JPEG, asked again every 'refresh' seconds.
# full  optional: what the full screen opens (the main stream).
#
# [Frigate]
# url = http://192.168.0.20:5000
#
# See docs/CAMERAS.md.
`;

/* cameras.txt -> { cams: [{ name, url, full, user, pass, refresh }], frigate } */
function parse(text) {
  const cams = [];
  let frigate = '', cur = null, inFrigate = false;
  for (const raw of (text || '').split('\n')) {
    const s = raw.trim();
    if (!s || s[0] === '#' || s[0] === ';') continue;
    const sec = s.match(/^\[(.*)\]$/);
    if (sec) {
      const name = sec[1].trim();
      inFrigate = name.toLowerCase() === 'frigate';
      cur = inFrigate ? null : { name, url: '', full: '', user: '', pass: '', refresh: '' };
      if (cur) cams.push(cur);
      continue;
    }
    const kv = s.match(/^([a-z]+)\s*=\s*(.*)$/i);
    if (!kv) continue;
    const k = kv[1].toLowerCase(), v = kv[2].trim();
    if (inFrigate) { if (k === 'url') frigate = v; }
    else if (cur && k in cur && k !== 'name') cur[k] = v;
  }
  return { cams, frigate };
}

function serialize(cams, frigate) {
  let out = HEADER;
  for (const c of cams) {
    out += `\n[${c.name}]\nurl  = ${c.url}\n`;
    if (c.full) out += `full = ${c.full}\n`;
    if (c.user) out += `user = ${c.user}\n`;
    if (c.pass) out += `pass = ${c.pass}\n`;
    if (c.refresh) out += `refresh = ${c.refresh}\n`;
  }
  if (frigate) out += `\n[Frigate]\nurl = ${frigate}\n`;
  return out;
}

/* rtsp://user:pass@host/... -> the URL without them, and them apart */
function splitCreds(url) {
  const m = url.match(/^(rtsp|https?):\/\/([^/@]*)@(.*)$/i);
  if (!m) return null;
  const [user, ...rest] = m[2].split(':');
  return { url: `${m[1]}://${m[3]}`, user: decodeURIComponent(user || ''), pass: decodeURIComponent(rest.join(':')) };
}

const kindOf = url => /^rtsp:/i.test(url) ? 'RTSP' : /^https?:/i.test(url) ? 'HTTP' : '?';

function check(cams, frigate) {
  const seen = new Set();
  for (const [i, c] of cams.entries()) {
    const n = `cámara ${i + 1}`;
    if (!c.name || c.name.length > 31 || /[\[\]\n]/.test(c.name)) return `${n}: el nombre, de 1 a 31 caracteres y sin corchetes`;
    if (seen.has(c.name.toLowerCase())) return `${c.name}: hay dos con ese nombre`;
    if (c.name.toLowerCase() === 'frigate') return 'Frigate va en su propio lugar, abajo';
    seen.add(c.name.toLowerCase());
    if (!/^(rtsp|https?):\/\/\S+$/i.test(c.url)) return `${c.name}: la dirección empieza con rtsp://, http:// o https://`;
    if (c.full && !/^(rtsp|https?):\/\/\S+$/i.test(c.full)) return `${c.name}: la dirección de pantalla completa no es válida`;
    if (c.refresh && !/^\d{1,4}$/.test(c.refresh)) return `${c.name}: el refresco es un número de segundos`;
  }
  if (frigate && !/^https?:\/\/\S+$/i.test(frigate)) return 'Frigate: la dirección empieza con http:// o https://';
  return null;
}

function render() {
  let cams = [], frigate = '', dirty = false;
  const listBox = h('div', { class: 'cmlist' });
  const frigateIn = h('input', { type: 'text', placeholder: 'http://192.168.0.20:5000' });
  frigateIn.oninput = () => { frigate = frigateIn.value.trim(); dirty = true; };
  const addBtn = h('button', { class: 'btn' }, '+ Agregar una cámara');
  const saveBtn = h('button', { class: 'btn pri' }, 'Guardar en la tarjeta');

  function field(c, key, label, attrs = {}) {
    const inp = h('input', Object.assign({ type: 'text', value: c[key] || '' }, attrs));
    inp.oninput = () => {
      c[key] = inp.value.trim();
      dirty = true;
      if (key === 'name') inp.closest('.cmcard').querySelector('.nm').textContent = c.name || 'Sin nombre';
      if (key === 'url') inp.closest('.cmcard').querySelector('.kind').textContent = kindOf(c.url);
    };
    if (key === 'url' || key === 'full') inp.onchange = () => {
      const sp = splitCreds(inp.value.trim());
      if (sp) {
        c[key] = sp.url;
        if (sp.user) c.user = sp.user;
        if (sp.pass) c.pass = sp.pass;
        toast('Separé el usuario y la contraseña de la dirección');
        paint();
      }
    };
    return h('label', { class: attrs.wide ? 'wide' : null }, label, inp);
  }

  function preview(c, box) {
    if (box.firstChild) { put(box); return; }
    if (!/^https?:/i.test(c.url)) {
      put(box, h('div', { class: 'msg' }, 'RTSP no se puede ver en el navegador: miralo en la placa.'));
      return;
    }
    const img = h('img', { alt: '', src: c.url + (c.url.includes('?') ? '&' : '?') + '_=' + Date.now() });
    img.onerror = () => put(box, h('div', { class: 'msg' },
      'No se pudo ver desde este navegador' + (c.user ? ' (la cámara pide usuario y contraseña, que el navegador no manda en una imagen)' : '') + '.'));
    put(box, img);
  }

  function paint() {
    if (!cams.length) {
      put(listBox, h('div', { class: 'cmcard muted' }, 'Todavía no hay cámaras: agregá una.'));
    } else {
      put(listBox, cams.map((c, i) => {
        const prev = h('div', { class: 'cmprev' });
        const move = d => { const j = i + d; [cams[i], cams[j]] = [cams[j], cams[i]]; dirty = true; paint(); };
        return h('div', { class: 'cmcard' },
          h('div', { class: 'cmhead' },
            h('span', { class: 'nm' }, c.name || 'Sin nombre'), h('span', { class: 'kind' }, kindOf(c.url)),
            h('button', { class: 'btn', title: 'Ver acá', onclick: () => preview(c, prev) }, '👁'),
            h('button', { class: 'btn', title: 'Subir', disabled: i === 0, onclick: () => move(-1) }, '↑'),
            h('button', { class: 'btn', title: 'Bajar', disabled: i === cams.length - 1, onclick: () => move(1) }, '↓'),
            h('button', { class: 'btn red', onclick: () => {
              if (confirm(`¿Quitar ${c.name || 'esta cámara'}?`)) { cams.splice(i, 1); dirty = true; paint(); }
            } }, 'Quitar')),
          h('div', { class: 'cmgrid' },
            field(c, 'name', 'Nombre', { maxlength: 31, placeholder: 'Timbre' }),
            field(c, 'refresh', 'Refresco (s, sólo para un JPEG suelto)', { placeholder: '–', inputmode: 'numeric' }),
            field(c, 'url', 'Dirección (la grilla: el stream secundario)', { wide: true, placeholder: 'rtsp://192.168.0.10:554/Streaming/Channels/102' }),
            field(c, 'full', 'Pantalla completa (opcional: el stream principal)', { wide: true, placeholder: 'rtsp://192.168.0.10:554/Streaming/Channels/101' }),
            field(c, 'user', 'Usuario', { autocomplete: 'off' }),
            field(c, 'pass', 'Contraseña', { type: 'password', autocomplete: 'new-password' })),
          prev);
      }));
    }
    addBtn.disabled = cams.length >= MAX;
    frigateIn.value = frigate;
  }

  addBtn.onclick = () => {
    if (cams.length >= MAX) return;
    cams.push({ name: '', url: '', full: '', user: '', pass: '', refresh: '' });
    dirty = true;
    paint();
    listBox.lastChild.querySelector('input').focus();
  };

  saveBtn.onclick = async () => {
    const err = check(cams, frigate);
    if (err) { toast(err, true); return; }
    try {
      await fsPut(FILE, serialize(cams, frigate));
      dirty = false;
      toast('Guardado. La app lo lee al abrirse.');
    } catch (e) { toast('No se pudo guardar: ' + e.message, true); }
  };

  async function load() {
    try {
      ({ cams, frigate } = parse(await fsText(FILE)));
    } catch (e) { toast('No se pudo leer ' + FILE + ': ' + e.message, true); }
    paint();
  }

  put(main,
    h('h1', {}, 'Cámaras'),
    h('div', { class: 'btns', style: 'margin-bottom:6px' },
      h('button', { class: 'btn pri', onclick: () => openApp('aos.cameras').catch(e => toast(e.message, true)) }, 'Abrir Cámaras en la placa')),
    h('h2', {}, 'Las cámaras'),
    listBox,
    h('div', { class: 'btns', style: 'margin-top:14px' }, addBtn, saveBtn),
    h('h2', {}, 'Frigate'),
    h('div', { class: 'card' }, h('div', { class: 'row' }, h('div', { class: 'grow' }, frigateIn))),
    h('p', { class: 'note' }, 'Opcional: la pestaña Frigate de la app, con los eventos y los clips.'),
    h('h2', {}, 'Qué puede mostrar la placa'),
    h('p', { class: 'note' }, 'Directo de la cámara, con rtsp://: H.264 en perfil Baseline o MJPEG (en una Hikvision, el segundo stream: rtsp://IP/Streaming/Channels/102). Para todo lo demás, un transcodificador que lo entregue como MJPEG por http://, como go2rtc (el que trae Home Assistant): http://IP:1984/api/stream.mjpeg?src=NOMBRE. Si pegás una dirección con usuario y contraseña adentro, se separan solos. Las contraseñas quedan en /cameras.txt de la tarjeta, como las guarda la app.'));

  window.onbeforeunload = () => dirty && location.hash === '#camaras' ? 'Hay cambios sin guardar' : undefined;
  load();
  return () => { window.onbeforeunload = null; };
}

P.registerPage({ id: 'camaras', name: 'Cámaras', icon: '◉', render });
