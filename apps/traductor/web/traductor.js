/* The translator's page in the board's portal (docs/PORTAL-PAGES.md): which
 * engine the board uses, the address and key of the user's own
 * LibreTranslate, the e-mail for MyMemory's bigger limit, the language packs
 * on the card, and the history with its favourites.
 *
 * The settings are /data/traductor/traductor.txt and the history
 * /data/traductor/hist.txt (apps/traductor/main/td_cfg.c and td_hist.c say
 * how); the app reads them again within three seconds when they change.
 * The key is typed here and lives only on the card. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const DIR = '/data/traductor';
const CFG = DIR + '/traductor.txt';
const HIST = DIR + '/hist.txt';
const APP_ID = 'aos.traductor';

const ENGINES = [
  ['auto', N_('Automático'), N_('La placa cuando hay paquete de idioma; si no, tu servidor, y si no hay, MyMemory.')],
  ['local', N_('En la placa'), N_('Sin red: sólo los pares de los paquetes de idioma de la tarjeta.')],
  ['server', N_('Servidor propio'), N_('Un LibreTranslate en tu red (una computadora o una Raspberry).')],
  ['online', N_('En línea'), N_('MyMemory: gratis y sin cuenta, 5000 caracteres por día; con un correo, 50 000.')],
];

const LANG = { es: N_('Español'), en: N_('Inglés'), pt: N_('Portugués'), it: N_('Italiano'), fr: N_('Francés'),
  de: N_('Alemán'), ca: N_('Catalán'), nl: N_('Neerlandés'), ja: N_('Japonés'), zh: N_('Chino'), ru: N_('Ruso') };
const langName = c => LANG[c] ? tr(LANG[c]) : c;

if (!document.getElementById('css-traductor')) {
  const st = document.createElement('style');
  st.id = 'css-traductor';
  st.textContent = `
.td-eng { display: grid; gap: 8px; }
.td-eng label { display: flex; gap: 12px; align-items: flex-start; padding: 10px 12px; border-radius: 12px;
  background: var(--card2); cursor: pointer; }
.td-eng input { margin-top: 4px; }
.td-eng b { display: block; }
.td-f { display: grid; gap: 6px; margin: 12px 0; }
.td-f input { width: 100%; box-sizing: border-box; }
.td-h { display: grid; grid-template-columns: 1fr auto; gap: 4px 12px; padding: 10px 0; border-top: 1px solid var(--line); }
.td-h .a { color: var(--dim); white-space: pre-wrap; overflow-wrap: anywhere; }
.td-h .b { white-space: pre-wrap; overflow-wrap: anywhere; }
.td-h .side { grid-row: span 2; display: flex; gap: 6px; align-items: start; }
.td-h .tag { font-size: 12px; color: var(--dim); }
.td-ib { background: none; border: 0; font-size: 20px; cursor: pointer; padding: 2px 6px; color: inherit; }
.td-star { color: var(--orange); }
`;
  document.head.append(st);
}

function parseCfg(text) {
  const c = { engine: 'auto', server: '', server_key: '', email: '', mymemory: '', history: 'on', other: [] };
  for (const line of (text || '').split('\n')) {
    const m = line.match(/^(\w+) (.*)$/);
    if (m && m[1] in c && m[1] !== 'other') c[m[1]] = m[2].trim();
    else if (line.trim()) c.other.push(line);
  }
  return c;
}

function writeCfg(c) {
  const out = ['engine ' + c.engine];
  if (c.server) out.push('server ' + c.server.replace(/\/+$/, ''));
  if (c.server_key) out.push('server_key ' + c.server_key);
  if (c.email) out.push('email ' + c.email);
  if (c.mymemory) out.push('mymemory ' + c.mymemory);
  if (c.history === 'off') out.push('history off');
  return out.concat(c.other).join('\n') + '\n';
}

const unesc = s => s.replace(/\\(.)/g, (m, c) => c === 'n' ? '\n' : c === 't' ? '\t' : c);
const esc = s => s.replace(/\\/g, '\\\\').replace(/\n/g, '\\n').replace(/\t/g, '\\t').replace(/\r/g, '');

function parseHist(text) {
  return (text || '').split('\n').filter(l => l.split('\t').length >= 6).map(l => {
    const f = l.split('\t');
    return { fav: f[0] === '1', when: +f[1], src: f[2], dst: f[3], a: unesc(f[4]), b: unesc(f.slice(5).join('\t')) };
  });
}

const writeHist = list => list.map(e => [e.fav ? 1 : 0, e.when, e.src, e.dst, esc(e.a), esc(e.b)].join('\t')).join('\n') + (list.length ? '\n' : '');

P.registerPage({
  id: 'traductor',
  name: tr('Traductor'),
  icon: '文',
  render(main) {
    const { h, put } = P;
    let cfg = parseCfg(''), hist = [], onlyFav = false, gone = false;
    const engBox = h('div', { class: 'card pad' }, h('span', { class: 'muted' }, tr('Cargando…')));
    const packBox = h('div', { class: 'card pad' });
    const histBox = h('div', { class: 'card pad' });

    async function load() {
      try { await P.fsMkdir(DIR); } catch (e) { /* already there */ }
      cfg = parseCfg(await P.fsText(CFG));
      hist = parseHist(await P.fsText(HIST));
      if (gone) return;
      drawEngine();
      drawHist();
    }

    async function saveCfg() {
      try {
        await P.fsPut(CFG, writeCfg(cfg));
        P.toast(tr('Guardado: la placa lo toma en unos segundos'));
      } catch (e) { P.toast(e.message, true); }
    }

    async function saveHist() {
      try { await P.fsPut(HIST, writeHist(hist)); } catch (e) { P.toast(e.message, true); }
    }

    function drawEngine() {
      const radios = ENGINES.map(([k, name, sub]) => h('label', {},
        h('input', { type: 'radio', name: 'td-eng', checked: cfg.engine === k, onchange: () => { cfg.engine = k; } }),
        h('span', {}, h('b', {}, tr(name)), h('span', { class: 'muted small' }, tr(sub)))));
      const server = h('input', { type: 'url', value: cfg.server, placeholder: 'http://192.168.0.20:5000',
        autocomplete: 'off', oninput: e => { cfg.server = e.target.value.trim(); } });
      const key = h('input', { type: 'password', value: cfg.server_key, placeholder: tr('Sólo si tu servidor la pide'),
        autocomplete: 'off', oninput: e => { cfg.server_key = e.target.value.trim(); } });
      const email = h('input', { type: 'email', value: cfg.email, placeholder: tr('nombre@ejemplo.com'),
        autocomplete: 'off', oninput: e => { cfg.email = e.target.value.trim(); } });
      const hoff = h('input', { type: 'checkbox', checked: cfg.history === 'off',
        onchange: e => { cfg.history = e.target.checked ? 'off' : 'on'; } });
      const probe = h('span', { class: 'muted small' });
      async function test() {
        if (!cfg.server) { P.toast(tr('Escribí la dirección del servidor'), true); return; }
        put(probe, tr('Probando…'));
        try {
          const r = await fetch(cfg.server.replace(/\/+$/, '') + '/languages');
          const l = await r.json();
          put(probe, tr('Anda: %d idiomas.', l.length || 0));
        } catch (e) {
          put(probe, tr('El navegador no llegó (puede ser el CORS del servidor); la placa quizás sí.'));
        }
      }
      put(engBox,
        h('h3', {}, tr('Motor')),
        h('div', { class: 'td-eng' }, ...radios),
        h('h3', {}, tr('Servidor propio (LibreTranslate)')),
        h('div', { class: 'td-f' }, h('span', {}, tr('Dirección')), server),
        h('div', { class: 'td-f' }, h('span', {}, tr('Clave')), key),
        h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: test }, tr('Probar desde el navegador')), probe),
        h('h3', {}, tr('MyMemory')),
        h('div', { class: 'td-f' }, h('span', {}, tr('Correo (opcional): sube el límite diario de 5000 a 50 000 caracteres')), email),
        h('p', { class: 'note' }, tr('Lo que se traduce en línea viaja al servicio, que puede guardarlo. Sin red, en la placa, no sale de la tarjeta.')),
        h('label', {}, hoff, ' ', tr('No guardar el historial')),
        h('div', { class: 'btns' },
          h('button', { class: 'btn pri', onclick: saveCfg }, tr('Guardar')),
          h('button', { class: 'btn', onclick: () => P.openApp(APP_ID) }, tr('Abrir en la placa'))));
    }

    async function drawPacks() {
      let files = [];
      try { files = await P.fsList('/apps'); } catch (e) { /* no card */ }
      if (gone) return;
      const packs = {};
      for (const f of files) {
        const m = f.name.match(/^traductor_([a-z]{2})_([a-z]{2})_p4\.pak(\.\d+)?$/);
        if (!m) continue;
        const k = m[1] + '-' + m[2];
        packs[k] = (packs[k] || 0) + f.size;
      }
      const rows = Object.keys(packs).sort().map(k => P.row(langName(k.slice(0, 2)) + ' ↔ ' + langName(k.slice(3)), P.fmtBytes(packs[k])));
      put(packBox,
        h('h3', {}, tr('Paquetes de idioma en la tarjeta')),
        rows.length ? h('div', {}, ...rows) : h('p', { class: 'muted' }, tr('Ninguno todavía.')),
        h('p', { class: 'note' }, tr('Se bajan desde la Tienda de la placa, en los extras del Traductor. Cada par trae los modelos de las dos direcciones (sin red, unos segundos por oración) y los diccionarios. Entre dos idiomas que no son el inglés, la placa traduce pasando por el inglés.')));
    }

    function drawHist() {
      const list = hist.map((e, i) => ({ e, i })).filter(x => !onlyFav || x.e.fav);
      const items = list.map(({ e, i }) => h('div', { class: 'td-h' },
        h('div', {},
          h('div', { class: 'tag' }, langName(e.src) + ' → ' + langName(e.dst) + (e.when ? ' · ' + P.fmtDate(e.when) : '')),
          h('div', { class: 'a' }, e.a)),
        h('div', { class: 'side' },
          h('button', { class: 'td-ib' + (e.fav ? ' td-star' : ''), title: tr('Favorito'),
            onclick: async () => { e.fav = !e.fav; await saveHist(); drawHist(); } }, e.fav ? '★' : '☆'),
          h('button', { class: 'td-ib', title: tr('Copiar'),
            onclick: () => navigator.clipboard && navigator.clipboard.writeText(e.b).then(() => P.toast(tr('Copiado'))) }, '⧉'),
          h('button', { class: 'td-ib', title: tr('Borrar'),
            onclick: async () => { hist.splice(i, 1); await saveHist(); drawHist(); } }, '×')),
        h('div', { class: 'b' }, e.b)));
      const exportTxt = () => {
        const txt = hist.map(e => `${langName(e.src)} → ${langName(e.dst)}${e.fav ? ' ★' : ''}\n${e.a}\n${e.b}\n`).join('\n');
        P.saveBlob(new Blob([txt], { type: 'text/plain' }), 'traducciones.txt');
      };
      put(histBox,
        h('h3', {}, tr('Historial')),
        h('div', { class: 'btns' },
          h('button', { class: 'btn' + (onlyFav ? '' : ' pri'), onclick: () => { onlyFav = false; drawHist(); } }, tr('Todo (%d)', hist.length)),
          h('button', { class: 'btn' + (onlyFav ? ' pri' : ''), onclick: () => { onlyFav = true; drawHist(); } }, tr('Favoritos (%d)', hist.filter(e => e.fav).length)),
          h('button', { class: 'btn', onclick: exportTxt, disabled: !hist.length }, tr('Bajar como texto'))),
        items.length ? h('div', {}, ...items) : h('p', { class: 'muted' }, onlyFav ? tr('No hay favoritos.') : tr('Todavía no hay traducciones.')));
    }

    put(main, h('h2', {}, tr('Traductor')), engBox, packBox, histBox);
    load().catch(e => P.toast(e.message, true));
    drawPacks();
    /* the board adds to the history while the page is open */
    const timer = setInterval(async () => {
      try {
        const t = await P.fsText(HIST);
        const n = parseHist(t);
        if (JSON.stringify(n) !== JSON.stringify(hist)) { hist = n; drawHist(); }
      } catch (e) { /* the board is away for a moment */ }
    }, 5000);
    return () => { gone = true; clearInterval(timer); };
  },
});
