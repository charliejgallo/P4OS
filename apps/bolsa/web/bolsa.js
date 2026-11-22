/* The Bolsa app's page in the board's portal (docs/PORTAL-PAGES.md): the
 * lists, the providers' keys and the price alerts.
 *
 * Everything lives in /data/bolsa/bolsa.txt, the file the app writes too
 * (apps/bolsa/main/bs_cfg.c has the format), and the app reads it again
 * within three seconds if it is open. The last prices come from
 * /data/bolsa/quotes.txt, which the app writes: the page asks no service by
 * itself, so the keys never leave the board's card and the user's browser.
 *
 * The keys are typed here by the user; nothing in the app or this page
 * carries one. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const LOC = { es: 'es-AR', en: 'en-US', de: 'de-DE' }[P.lang ? P.lang() : 'es'] || 'es-AR';
const DIR = '/data/bolsa';
const FILE = DIR + '/bolsa.txt';
const QUOTES = DIR + '/quotes.txt';
const NAMES = DIR + '/names.txt';
const LEARNED = {};   /* names.txt: what the searches found, "SYM|Name" or "Name" */
const APP_ID = 'aos.bolsa';

/* The app's catalog (bs_sym.c): what is not a company */
const CATALOG = [
  ['USD.OFICIAL', N_('Oficial'), N_('Dólar oficial')], ['USD.BLUE', N_('Blue'), N_('Dólar blue')],
  ['USD.MEP', N_('MEP'), N_('Dólar MEP (bolsa)')], ['USD.CCL', N_('CCL'), N_('Dólar contado con liqui')],
  ['USD.CRIPTO', N_('Cripto'), N_('Dólar cripto')], ['USD.MAYOR', N_('Mayorista'), N_('Dólar mayorista')],
  ['EUR.ARS', 'EUR', N_('Euro en pesos')], ['BRL.ARS', 'BRL', N_('Real en pesos')],
  ['CLP.ARS', 'CLP', N_('Peso chileno en pesos')], ['UYU.ARS', 'UYU', N_('Peso uruguayo en pesos')],
  ['RIESGO.AR', N_('Riesgo'), N_('Riesgo país')],
  ['^MERV', 'MERV', 'Merval'], ['^GSPC', 'SPX', 'S&P 500'], ['^DJI', 'DJI', 'Dow Jones'],
  ['^IXIC', 'IXIC', 'Nasdaq'], ['^BVSP', 'BVSP', 'Bovespa'], ['^FTSE', 'FTSE', 'FTSE 100'],
  ['^GDAXI', 'DAX', 'DAX'], ['^N225', 'N225', 'Nikkei 225'],
  ['cg:bitcoin', 'BTC', 'Bitcoin'], ['cg:ethereum', 'ETH', 'Ethereum'], ['cg:solana', 'SOL', 'Solana'],
  ['cg:ripple', 'XRP', 'XRP'], ['cg:cardano', 'ADA', 'Cardano'], ['cg:dogecoin', 'DOGE', 'Dogecoin'],
  ['cg:litecoin', 'LTC', 'Litecoin'], ['cg:polkadot', 'DOT', 'Polkadot'], ['cg:stellar', 'XLM', 'Stellar'],
  ['cg:monero', 'XMR', 'Monero'],
];
const TRANSLATED = new Set(['USD.OFICIAL', 'USD.BLUE', 'USD.MEP', 'USD.CCL', 'USD.CRIPTO', 'USD.MAYOR',
  'EUR.ARS', 'BRL.ARS', 'CLP.ARS', 'UYU.ARS', 'RIESGO.AR']);
const LIST_NAMES = [N_('Mías'), N_('Argentina'), N_('EE. UU.'), N_('Cripto'), N_('Índices')];

/* The providers that want a key, with where the user gets a free one */
const KEYED = [
  { id: 'twelve', name: 'Twelve Data', url: 'https://twelvedata.com/pricing',
    note: N_('8 consultas por minuto y 800 por día. Cotizaciones con las 52 semanas, gráficos del día (cada 5 minutos) y diarios.') },
  { id: 'stooq', name: 'Stooq', url: 'https://stooq.com',
    note: N_('Históricos diarios con apertura, máximo, mínimo y volumen; índices. La clave se saca en su página, en la descarga de datos.') },
  { id: 'alpha', name: 'Alpha Vantage', url: 'https://www.alphavantage.co/support/#api-key',
    note: N_('25 consultas por día: cotizaciones, los últimos 100 días y semanales.') },
  { id: 'finnhub', name: 'Finnhub', url: 'https://finnhub.io/register',
    note: N_('Sólo cotizaciones (sus gráficos son de pago): sirve junto con otro para los históricos.') },
];

const catName = s => {
  const c = CATALOG.find(x => x[0] === s);
  if (c) return TRANSLATED.has(s) ? tr(c[2]) : c[2];
  const l = LEARNED[s];
  return l ? l.slice(l.indexOf('|') + 1) : '';
};
const showOf = s => {
  const c = CATALOG.find(x => x[0] === s);
  if (c) return TRANSLATED.has(s) ? tr(c[1]) : c[1];
  const l = LEARNED[s];
  if (l && l.includes('|')) return l.slice(0, l.indexOf('|'));
  if (s.startsWith('cg:')) return s.slice(3);
  return s.replace(/^\^/, '').replace(/\.BA$/, '');
};
const cleanSym = s => {
  s = (s || '').trim();
  if (s.startsWith('cg:')) return s.replace(/[^a-z0-9:.-]/g, '');
  return s.toUpperCase().replace(/[^A-Z0-9.^:-]/g, '');
};
const cleanKey = s => (s || '').replace(/[^A-Za-z0-9._-]/g, '');
const num = (v, d) => v == null || isNaN(v) ? '–' : Number(v).toLocaleString(LOC, { minimumFractionDigits: d, maximumFractionDigits: d });
const decOf = v => Math.abs(v) >= 100000 ? 0 : Math.abs(v) >= 1 ? 2 : 4;

/* bolsa.txt <-> an object, keeping what this page does not touch */
function parse(text) {
  const c = { refresh: 5, prov: 'auto', keys: {}, view: '', lists: [], alerts: [], other: [] };
  for (const raw of (text || '').split('\n')) {
    const line = raw.replace(/\r$/, '');
    if (!line.trim() || line.startsWith('#')) continue;
    const sp = line.indexOf(' ');
    const k = sp < 0 ? line : line.slice(0, sp), v = sp < 0 ? '' : line.slice(sp + 1).trim();
    if (k === 'refresh') c.refresh = Math.max(1, Math.min(60, parseInt(v, 10) || 5));
    else if (k === 'prov') c.prov = v || 'auto';
    else if (k === 'key') { const i = v.indexOf(' '); if (i > 0) c.keys[v.slice(0, i)] = v.slice(i + 1).trim(); }
    else if (k === 'view') c.view = v;
    else if (k === 'list') {
      const i = v.indexOf(':');
      if (i > 0) c.lists.push({ name: v.slice(0, i).trim(), syms: v.slice(i + 1).split(',').map(cleanSym).filter(Boolean) });
    } else if (k === 'alert') {
      const m = v.match(/^(\S+)\s+([<>])\s+([\d.]+)(\s+fired)?/);
      if (m) c.alerts.push({ sym: m[1], dir: m[2], value: parseFloat(m[3]), fired: !!m[4] });
    } else c.other.push(line);
  }
  return c;
}

function write(c) {
  const out = ['# Bolsa (aos.bolsa): written by the app and by its page in the portal.',
    'refresh ' + c.refresh, 'prov ' + c.prov];
  for (const k of KEYED) if (c.keys[k.id]) out.push('key ' + k.id + ' ' + cleanKey(c.keys[k.id]));
  if (c.view) out.push('view ' + c.view);
  for (const l of c.lists) out.push('list ' + l.name.replace(/[:\n]/g, ' ').slice(0, 23) + ':' + l.syms.join(','));
  for (const a of c.alerts) out.push('alert ' + a.sym + ' ' + a.dir + ' ' + a.value.toFixed(4) + (a.fired ? ' fired' : ''));
  for (const o of c.other) out.push(o);
  return out.join('\n') + '\n';
}

function css() {
  if (document.getElementById('css-bolsa')) return;
  const s = document.createElement('style');
  s.id = 'css-bolsa';
  s.textContent = `
.bsl { margin: 10px 0 18px; }
.bsl-head { display: flex; gap: 8px; align-items: center; margin-bottom: 6px; }
.bsl-head input { flex: 1; min-width: 0; font-size: 1.05em; font-weight: 600; }
.bs-sym { display: flex; align-items: center; gap: 10px; padding: 8px 12px; border-top: 1px solid var(--line); }
.bs-sym:first-child { border-top: 0; }
.bs-sym .bs-s { font-weight: 600; min-width: 5.5em; }
.bs-sym .bs-n { flex: 1; min-width: 0; color: var(--dim); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bs-sym .bs-p { text-align: right; min-width: 6.5em; font-variant-numeric: tabular-nums; }
.bs-up { color: var(--green); } .bs-dn { color: var(--red); }
.bs-ib { border: 0; background: var(--card2); color: inherit; border-radius: 8px; width: 2em; height: 2em; cursor: pointer; }
.bs-ib:disabled { opacity: .3; cursor: default; }
.bs-add { display: flex; gap: 8px; padding: 8px 12px; border-top: 1px solid var(--line); }
.bs-add input { flex: 1; min-width: 0; }
.bs-key { padding: 10px 12px; border-top: 1px solid var(--line); }
.bs-key:first-child { border-top: 0; }
.bs-key .bs-kr { display: flex; gap: 8px; align-items: center; margin-top: 6px; }
.bs-key input { flex: 1; min-width: 0; font-family: ui-monospace, monospace; }
.bs-key input[type=number] { flex: none; width: 5em; }
.bs-al { display: flex; gap: 8px; align-items: center; padding: 8px 12px; border-top: 1px solid var(--line); flex-wrap: wrap; }
.bs-al:first-child { border-top: 0; }
.bs-al select, .bs-al input { min-width: 0; }
@media (max-width: 560px) {
  .bs-sym .bs-n { display: none; }
  .bs-sym { gap: 6px; padding: 8px; }
  .bs-sym .bs-s { min-width: 0; flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .bs-sym .bs-p { min-width: 0; font-size: .92em; }
  .bs-ib { width: 1.8em; height: 1.8em; }
  .bsl-head { flex-wrap: wrap; }
  .bs-add, .bs-key, .bs-al { padding: 8px; }
}`;
  document.head.appendChild(s);
}

P.registerPage({
  id: 'bolsa',
  name: tr('Bolsa'),
  icon: '📈',
  render(main) {
    const { h, put } = P;
    css();
    let cfg = null, quotes = {}, gone = false, dirty = false;
    const listsBox = h('div', {}, h('div', { class: 'card pad muted' }, tr('Cargando…')));
    const keysBox = h('div', { class: 'card' });
    const alertsBox = h('div', { class: 'card' });
    const saveBtn = h('button', { class: 'btn pri', onclick: save }, tr('Guardar'));
    const mark = () => { dirty = true; saveBtn.textContent = tr('Guardar cambios'); };

    function price(sym) {
      const q = quotes[sym];
      if (!q) return h('span', { class: 'bs-p muted' }, '–');
      const d = sym === 'RIESGO.AR' ? 0 : decOf(q.price);
      let cls = '', pct = '';
      if (q.prev) {
        const p = (q.price / q.prev - 1) * 100;
        cls = p > 0 ? 'bs-up' : p < 0 ? 'bs-dn' : '';
        pct = ' ' + (p > 0 ? '+' : '') + num(p, 2) + ' %';
      }
      return h('span', { class: 'bs-p ' + cls, title: q.src }, num(q.price, d) + pct);
    }

    function drawLists() {
      const cards = cfg.lists.map((l, li) => {
        const name = h('input', { value: LIST_NAMES.includes(l.name) ? tr(l.name) : l.name, maxlength: 23,
          oninput: e => { l.name = e.target.value || l.name; mark(); } });
        const rows = l.syms.map((s, i) => h('div', { class: 'bs-sym' },
          h('span', { class: 'bs-s' }, showOf(s)),
          h('span', { class: 'bs-n' }, catName(s) || (s !== showOf(s) ? s : '')),
          price(s),
          h('button', { class: 'bs-ib', title: tr('Subir'), disabled: i === 0, onclick: () => { l.syms.splice(i - 1, 0, l.syms.splice(i, 1)[0]); mark(); drawLists(); } }, '↑'),
          h('button', { class: 'bs-ib', title: tr('Bajar'), disabled: i === l.syms.length - 1, onclick: () => { l.syms.splice(i + 1, 0, l.syms.splice(i, 1)[0]); mark(); drawLists(); } }, '↓'),
          h('button', { class: 'bs-ib', title: tr('Sacar'), onclick: () => { l.syms.splice(i, 1); mark(); drawLists(); } }, '×')));
        const dl = 'bs-cat-' + li;
        const inp = h('input', { placeholder: tr('Símbolo: NANDU, YERBA.BA, ^MERV, USD.BLUE, cg:bitcoin…'), list: dl });
        const add = () => {
          let s = inp.value.trim();
          const hit = CATALOG.find(c => c[1].toLowerCase() === s.toLowerCase() || catName(c[0]).toLowerCase() === s.toLowerCase());
          s = hit ? hit[0] : cleanSym(s);
          if (!s) return;
          if (l.syms.includes(s)) { P.toast(tr('Ya está en la lista'), true); return; }
          if (l.syms.length >= 30) { P.toast(tr('La lista está llena (30)'), true); return; }
          l.syms.push(s); mark(); drawLists();
        };
        inp.addEventListener('keydown', e => { if (e.key === 'Enter') add(); });
        return h('div', { class: 'bsl' },
          h('div', { class: 'bsl-head' }, name,
            h('button', { class: 'btn red', onclick: () => {
              if (!confirm(tr('¿Borrar la lista %s?', name.value))) return;
              cfg.lists.splice(li, 1); mark(); drawLists();
            } }, tr('Borrar lista'))),
          h('div', { class: 'card' }, ...rows,
            h('div', { class: 'bs-add' }, inp,
              h('datalist', { id: dl }, ...CATALOG.map(c => h('option', { value: c[0] }, c[1] + ' · ' + catName(c[0])))),
              h('button', { class: 'btn', onclick: add }, tr('Sumar')))));
      });
      put(listsBox, ...cards,
        cfg.lists.length < 8 ? h('button', { class: 'btn', onclick: () => { cfg.lists.push({ name: tr('Nueva'), syms: [] }); mark(); drawLists(); } }, tr('Agregar una lista')) : null);
    }

    function drawKeys() {
      const rows = KEYED.map(k => {
        const inp = h('input', { type: 'password', value: cfg.keys[k.id] || '', placeholder: tr('sin clave'), autocomplete: 'off',
          oninput: e => { cfg.keys[k.id] = cleanKey(e.target.value); mark(); } });
        return h('div', { class: 'bs-key' },
          h('div', {}, h('b', {}, k.name), ' ', h('a', { href: k.url, target: '_blank', rel: 'noopener' }, tr('sacar una clave gratis'))),
          h('div', { class: 'small muted' }, tr(k.note)),
          h('div', { class: 'bs-kr' }, inp,
            h('button', { class: 'btn', onclick: e => { inp.type = inp.type === 'password' ? 'text' : 'password'; e.target.textContent = inp.type === 'password' ? tr('Ver') : tr('Ocultar'); } }, tr('Ver'))));
      });
      const sel = h('select', { onchange: e => { cfg.prov = e.target.value; mark(); } },
        h('option', { value: 'auto' }, tr('Automático (el primero con clave)')),
        ...KEYED.map(k => h('option', { value: k.id }, k.name)));
      sel.value = cfg.prov;
      const ref = h('input', { type: 'number', min: 1, max: 60, value: cfg.refresh, style: 'width:5em',
        oninput: e => { cfg.refresh = Math.max(1, Math.min(60, parseInt(e.target.value, 10) || 5)); mark(); } });
      put(keysBox, ...rows,
        h('div', { class: 'bs-key' }, h('div', { class: 'bs-kr' }, h('span', { class: 'grow' }, tr('Proveedor preferido para acciones')), sel)),
        h('div', { class: 'bs-key' }, h('div', { class: 'bs-kr' }, h('span', { class: 'grow' }, tr('Actualizar las cotizaciones cada (minutos)')), ref)));
    }

    function drawAlerts() {
      const all = [...new Set(cfg.lists.flatMap(l => l.syms))];
      const rows = cfg.alerts.map((a, i) => h('div', { class: 'bs-al' },
        h('b', { style: 'min-width:5.5em' }, showOf(a.sym)),
        h('span', { class: 'grow' }, (a.dir === '>' ? tr('si sube a %s', num(a.value, decOf(a.value))) : tr('si baja a %s', num(a.value, decOf(a.value)))) +
          (a.fired ? ' · ' + tr('ya avisó') : '')),
        quotes[a.sym] ? h('span', { class: 'muted small' }, tr('ahora %s', num(quotes[a.sym].price, decOf(quotes[a.sym].price)))) : null,
        a.fired ? h('button', { class: 'btn', onclick: () => { a.fired = false; dirtyFired.add(a); mark(); drawAlerts(); } }, tr('Rearmar')) : null,
        h('button', { class: 'bs-ib', title: tr('Borrar'), onclick: () => { cfg.alerts.splice(i, 1); mark(); drawAlerts(); } }, '×')));
      const sym = h('select', {}, ...all.map(s => h('option', { value: s }, showOf(s) + (catName(s) ? ' · ' + catName(s) : ''))));
      const dir = h('select', {}, h('option', { value: '>' }, tr('si sube a')), h('option', { value: '<' }, tr('si baja a')));
      const val = h('input', { type: 'number', step: 'any', min: 0, placeholder: tr('precio'), style: 'width:9em' });
      sym.onchange = () => { const q = quotes[sym.value]; val.placeholder = q ? num(q.price, decOf(q.price)) : tr('precio'); };
      sym.onchange();
      put(alertsBox, ...rows,
        cfg.alerts.length >= 24 ? null : h('div', { class: 'bs-al' }, sym, dir, val,
          h('button', { class: 'btn', onclick: () => {
            const v = parseFloat(String(val.value).replace(',', '.'));
            if (!sym.value || !(v > 0)) { P.toast(tr('Elegí un símbolo y escribí un precio'), true); return; }
            cfg.alerts.push({ sym: sym.value, dir: dir.value, value: v, fired: false }); mark(); drawAlerts();
          } }, tr('Agregar aviso'))));
    }

    async function save() {
      try {
        /* what the app may have changed since (an alert that fired, a
         * reordered list on the screen) loses to what is on this page, but
         * the alerts that fired meanwhile are kept as fired */
        const now = parse(await P.fsText(FILE));
        for (const a of cfg.alerts) {
          const b = now.alerts.find(x => x.sym === a.sym && x.dir === a.dir && Math.abs(x.value - a.value) < 1e-6);
          if (b && b.fired && !dirtyFired.has(a)) a.fired = true;
        }
        cfg.view = now.view || cfg.view;
        await P.fsMkdir(DIR).catch(() => {});
        await P.fsPut(FILE, write(cfg));
        dirty = false;
        saveBtn.textContent = tr('Guardar');
        P.toast(tr('Guardado: la placa lo toma en unos segundos'));
      } catch (e) { P.toast(e.message, true); }
    }
    const dirtyFired = new Set();

    async function load() {
      let text = null;
      try { text = await P.fsText(FILE); } catch (e) { P.toast(e.message, true); }
      try {
        const q = await P.fsText(QUOTES);
        for (const line of (q || '').split('\n')) {
          if (!line || line.startsWith('#')) continue;
          const [s, p, pv, t, src] = line.split('\t');
          quotes[s] = { price: parseFloat(p), prev: pv ? parseFloat(pv) : null, t: parseInt(t, 10), src };
        }
      } catch (e) { /* no prices yet */ }
      try {
        for (const line of ((await P.fsText(NAMES)) || '').split('\n')) {
          const i = line.indexOf('\t');
          if (i > 0) LEARNED[line.slice(0, i)] = line.slice(i + 1);
        }
      } catch (e) { /* none learned */ }
      if (gone) return;
      if (text == null) {
        put(listsBox, h('div', { class: 'card pad' }, tr('Todavía no hay configuración: abrí Bolsa en la placa una vez y volvé.')));
        return;
      }
      cfg = parse(text);
      drawLists();
      drawKeys();
      drawAlerts();
    }

    put(main, h('h1', {}, tr('Bolsa')),
      h('p', { class: 'note' }, tr('Datos con demora. La app muestra cotizaciones y gráficos; no es un consejo de inversión.')),
      h('h2', {}, tr('Listas')), listsBox,
      h('p', { class: 'note' }, tr('Acciones de EE. UU. con su símbolo (NANDU), de Buenos Aires con .BA (YERBA.BA), índices con ^ (^MERV), monedas como cg:bitcoin. El dólar, las monedas, el riesgo país y la cripto andan sin clave.')),
      h('h2', {}, tr('Claves de los proveedores')), keysBox,
      h('p', { class: 'note' }, tr('Las acciones y los índices necesitan la clave gratis de al menos uno. Se guardan sólo en la tarjeta de la placa, en /data/bolsa/bolsa.txt.')),
      h('h2', {}, tr('Avisos de precio')), alertsBox,
      h('p', { class: 'note' }, tr('La placa avisa con un mensaje y un pitido cuando el precio cruza el valor, mientras la app esté abierta (aunque esté atrás).')),
      h('div', { class: 'btns', style: 'margin-top:10px' }, saveBtn,
        h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast(tr('Abierta en la placa'))).catch(e => P.toast(e.message, true)) }, tr('Abrir Bolsa en la placa'))));
    load();
    const warn = e => { if (dirty) { e.preventDefault(); e.returnValue = ''; } };
    window.addEventListener('beforeunload', warn);
    return () => { gone = true; window.removeEventListener('beforeunload', warn); };
  },
});
