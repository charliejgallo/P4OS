/* The flight radar's page in the board's portal (docs/PORTAL-PAGES.md):
 * where the board looks, from which source, how far, and the alerts; what
 * needs a real keyboard (a place by its name, the receiver's address, a
 * service's key) is typed here. And, while the app is open on the board, a
 * table of the aircraft it sees.
 *
 * The settings are /data/vuelos.txt, key=value lines that the app writes
 * too (apps/vuelos/main/vl_conf.c); the app looks at its date every two
 * seconds and takes it again when it changes. The place is searched with
 * Open-Meteo's geocoding (free, no key), from the browser, as Clima's page
 * does. The table comes from the app over the live channel (/api/live). */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const FILE = '/data/vuelos.txt';
const APP_ID = 'aos.vuelos';

if (!document.getElementById('css-vuelos')) {
  const st = document.createElement('style');
  st.id = 'css-vuelos';
  st.textContent = `
.vlgrid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 12px; }
.vlgrid label { display: flex; flex-direction: column; gap: 4px; }
.vlgrid input, .vlgrid select { width: 100%; box-sizing: border-box; }
.vlsrc { display: grid; gap: 8px; }
.vlsrc label { display: flex; gap: 10px; align-items: flex-start; padding: 10px; border-radius: 10px; background: var(--card2); cursor: pointer; }
.vlsrc label.on { outline: 2px solid var(--accent); }
.vlsrc input { margin-top: 3px; }
.vlchk { display: flex; flex-wrap: wrap; gap: 8px 20px; }
.vlchk label { display: flex; gap: 8px; align-items: center; }
.vlsearch { display: flex; gap: 10px; }
.vlsearch input { flex: 1; min-width: 0; }
.vltable { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
.vltable th { text-align: left; font-weight: 600; color: var(--dim); cursor: pointer; padding: 6px 8px; white-space: nowrap; }
.vltable td { padding: 6px 8px; border-top: 1px solid var(--line); white-space: nowrap; }
.vltable td.n, .vltable th.n { text-align: right; }
.vltable tr.em td { color: var(--red); font-weight: 600; }
.vldot { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 8px; vertical-align: middle; }
.vlwrap { overflow-x: auto; }
`;
  document.head.append(st);
}

/* the defaults, the same as the app's */
const DEF = {
  place: '', lat: '', lon: '', src: 'airplanes', radius: '93', url: '', lol_key: '', lol_day: '500',
  os_id: '', os_secret: '', alert_over: '1', over_km: '3.0', over_ft: '12000', alert_emerg: '1',
  alert_mil: '0', sound: '1', trail: '3', units: 'aviation', routes: '1', labels: '1',
};

const SOURCES = [
  { k: 'airplanes', name: 'airplanes.live', note: N_('Sin cuenta. Un pedido por segundo como mucho y sólo para uso no comercial: la placa pregunta cada 5 s.') },
  { k: 'adsblol', name: 'adsb.lol', note: N_('Datos abiertos (ODbL). Sin clave, 500 pedidos por día desde tu dirección: la placa los reparte en el día (uno cada 3 minutos). Con una clave, poné cuántos te da.') },
  { k: 'opensky', name: 'OpenSky Network', note: N_('Para investigación y uso no comercial. Sin cuenta, 400 créditos por día (un pedido cada 4 minutos más o menos); con un cliente de la API de tu cuenta, 4000.') },
  { k: 'local', name: N_('Receptor propio'), note: N_('Un receptor ADS-B en tu red (readsb, dump1090, tar1090): la dirección de su aircraft.json. Cada 2 s.') },
];

function parse(text) {
  const c = { ...DEF };
  for (const line of (text || '').split('\n')) {
    if (line.startsWith('#')) continue;
    const i = line.indexOf('=');
    if (i < 0) continue;
    const k = line.slice(0, i).trim(), v = line.slice(i + 1).replace(/\r$/, '');
    if (k === 'place') {
      const [n, la, lo] = v.split('\t');
      c.place = n || ''; c.lat = la || ''; c.lon = lo || '';
    } else if (k in c) c[k] = v;
  }
  return c;
}

/* tabs and line breaks would break the file */
const clean = s => String(s ?? '').replace(/[\t\r\n]/g, ' ').trim();

function serialize(c) {
  const lines = ['# Vuelos: lo escriben la app y su pagina del portal'];
  if (c.lat !== '' && c.lon !== '') lines.push(`place=${clean(c.place)}\t${(+c.lat).toFixed(5)}\t${(+c.lon).toFixed(5)}`);
  for (const k of Object.keys(DEF)) if (!['place', 'lat', 'lon'].includes(k)) lines.push(`${k}=${clean(c[k])}`);
  return lines.join('\n') + '\n';
}

P.registerPage({
  id: 'vuelos',
  name: tr('Vuelos'),
  icon: '✈',
  render(main) {
    const { h, put } = P;
    let conf = { ...DEF }, gone = false, timer = 0, sortKey = 'd', sortUp = true, last = null;

    const placeBox = h('div', { class: 'muted' }, tr('Cargando…'));
    const q = h('input', { type: 'search', placeholder: tr('Ciudad, pueblo o aeropuerto'), autocomplete: 'off' });
    const results = h('div', { class: 'card', style: 'display:none' });
    const srcBox = h('div', { class: 'vlsrc' });
    const srcExtra = h('div', {});
    const table = h('div', { class: 'vlwrap' }, h('p', { class: 'muted' }, tr('Abrí Vuelos en la placa para ver acá lo que ve.')));
    const inputs = {};

    const field = (k, label, attrs = {}) => {
      const el = h('input', { ...attrs, value: conf[k] ?? '' });
      el.oninput = () => { conf[k] = el.type === 'checkbox' ? (el.checked ? '1' : '0') : el.value; };
      inputs[k] = el;
      return h('label', {}, h('span', { class: 'small muted' }, label), el);
    };
    const check = (k, label) => {
      const el = h('input', { type: 'checkbox' });
      el.checked = conf[k] === '1';
      el.onchange = () => { conf[k] = el.checked ? '1' : '0'; };
      inputs[k] = el;
      return h('label', {}, el, label);
    };
    const select = (k, label, opts) => {
      const el = h('select', {}, opts.map(([v, t]) => h('option', { value: v }, t)));
      el.value = conf[k];
      el.onchange = () => { conf[k] = el.value; };
      inputs[k] = el;
      return h('label', {}, h('span', { class: 'small muted' }, label), el);
    };

    function drawPlace() {
      put(placeBox, conf.lat !== ''
        ? h('div', {}, h('b', {}, conf.place || tr('Sin nombre')), h('div', { class: 'muted small' }, `${(+conf.lat).toFixed(4)}, ${(+conf.lon).toFixed(4)}`))
        : h('span', { class: 'muted' }, tr('Todavía no hay un lugar elegido: la placa usa la ciudad de Clima.')));
    }

    function drawSources() {
      put(srcBox, SOURCES.map(s => {
        const r = h('input', { type: 'radio', name: 'vlsrc', value: s.k });
        r.checked = conf.src === s.k;
        r.onchange = () => { conf.src = s.k; drawSources(); };
        return h('label', { class: conf.src === s.k ? 'on' : '' }, r,
          h('div', {}, h('b', {}, tr(s.name)), h('div', { class: 'small muted' }, tr(s.note))));
      }));
      const extra = {
        adsblol: () => h('div', { class: 'vlgrid' },
          field('lol_key', tr('Clave (opcional)'), { type: 'password', autocomplete: 'off' }),
          field('lol_day', tr('Pedidos por día que te da'), { type: 'number', min: 50, step: 50 })),
        opensky: () => h('div', {},
          h('div', { class: 'vlgrid' },
            field('os_id', tr('Client ID (opcional)'), { autocomplete: 'off' }),
            field('os_secret', tr('Client secret'), { type: 'password', autocomplete: 'off' })),
          h('p', { class: 'note' }, tr('Se crean en la página de tu cuenta de OpenSky. Quedan en la tarjeta de la placa, en /data/vuelos.txt.'))),
        local: () => h('div', { class: 'vlgrid' },
          field('url', tr('Dirección del aircraft.json'), { placeholder: 'http://192.168.0.50/tar1090/data/aircraft.json', autocomplete: 'off' })),
      }[conf.src];
      put(srcExtra, extra ? extra() : null);
    }

    async function load() {
      try { conf = parse(await P.fsText(FILE)); } catch (e) { P.toast(e.message, true); }
      if (gone) return;
      drawAll();
    }

    async function save() {
      try {
        await P.fsMkdir('/data');
        await P.fsPut(FILE, serialize(conf));
        P.toast(tr('Guardado: la placa lo toma en unos segundos'));
      } catch (e) { P.toast(tr('No se pudo guardar: %s', e.message), true); }
    }

    async function choose(r) {
      let name = clean(r.name);
      while (new TextEncoder().encode(name).length > 39) name = name.slice(0, -1);
      conf.place = name; conf.lat = String(r.latitude); conf.lon = String(r.longitude);
      results.style.display = 'none';
      q.value = '';
      drawPlace();
      await save();
    }

    async function fromClima() {
      try {
        const t = await P.fsText('/data/clima_lugar.txt');
        const [n, la, lo] = (t || '').split('\n')[0].split('\t');
        if (!n || !la || !lo) return P.toast(tr('Clima no tiene un lugar elegido'), true);
        await choose({ name: n, latitude: +la / 10000, longitude: +lo / 10000 });
      } catch (e) { P.toast(e.message, true); }
    }

    async function search() {
      const name = q.value.trim();
      if (name.length < 2) return;
      results.style.display = '';
      put(results, h('div', { class: 'row muted' }, tr('Buscando…')));
      try {
        const lang = (navigator.language || 'es').slice(0, 2);
        const d = await (await fetch(`https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(name)}&count=10&language=${lang}&format=json`)).json();
        const list = d.results || [];
        put(results, list.length ? list.map(r => h('div', { class: 'row click', onclick: () => choose(r) },
          h('div', { class: 'grow' }, h('b', {}, r.name), h('div', { class: 'muted small' }, [r.admin1, r.country].filter(Boolean).join(', '))),
          h('div', { class: 'val' }, tr('Elegir')))) : h('div', { class: 'row muted' }, tr('No se encontró ningún lugar con ese nombre.')));
      } catch (e) { put(results, h('div', { class: 'row bad' }, tr('No se pudo buscar: %s', e.message))); }
    }
    q.onkeydown = e => { if (e.key === 'Enter') search(); };

    /* the table of what the board sees */
    const fmtAlt = (a, u) => a.g ? tr('en tierra') : a.alt <= -999999 ? '-' : u ? `${P.num ? P.num(Math.round(a.alt * 0.3048), 0) : Math.round(a.alt * 0.3048)} m` : `${P.num ? P.num(a.alt, 0) : a.alt} ft`;
    const fmtDist = (d, u) => u ? `${P.num ? P.num(d, 1) : d.toFixed(1)} km` : `${P.num ? P.num(d / 1.852, 1) : (d / 1.852).toFixed(1)} NM`;
    const color = a => {
      if (a.g) return '#8A8F99';
      if (a.alt <= -999999) return '#C8CCD4';
      const T = [[0, [255, 122, 69]], [2000, [255, 164, 58]], [6000, [242, 212, 61]], [10000, [165, 224, 90]], [20000, [69, 214, 196]], [30000, [90, 168, 255]], [40000, [185, 139, 255]]];
      if (a.alt <= 0) return 'rgb(255,122,69)';
      for (let i = 1; i < T.length; i++) if (a.alt < T[i][0]) {
        const t = (a.alt - T[i - 1][0]) / (T[i][0] - T[i - 1][0]);
        return `rgb(${T[i - 1][1].map((v, j) => Math.round(v + (T[i][1][j] - v) * t)).join(',')})`;
      }
      return 'rgb(185,139,255)';
    };
    function drawTable() {
      if (!last) return;
      const u = last.units === 1;
      const COLS = [['f', tr('Vuelo')], ['r', tr('Matrícula')], ['t', tr('Tipo')], ['alt', tr('Altitud'), 1], ['gs', u ? 'km/h' : 'kt', 1], ['vr', tr('Vel. vertical'), 1], ['trk', tr('Rumbo'), 1], ['sq', 'Squawk'], ['d', tr('Distancia'), 1]];
      const rows = [...last.ac].sort((a, b) => {
        const va = a[sortKey], vb = b[sortKey];
        const c = typeof va === 'number' ? va - vb : String(va).localeCompare(String(vb));
        return sortUp ? c : -c;
      });
      put(table,
        h('p', { class: 'small muted' }, tr('%s aviones · %s · %s', last.n, last.src, last.place)),
        h('table', { class: 'vltable' },
          h('thead', {}, h('tr', {}, COLS.map(([k, t, n]) => h('th', { class: n ? 'n' : '', onclick: () => { sortUp = sortKey === k ? !sortUp : true; sortKey = k; drawTable(); } }, t + (sortKey === k ? (sortUp ? ' ▲' : ' ▼') : ''))))),
          h('tbody', {}, rows.map(a => h('tr', { class: ['7500', '7600', '7700'].includes(a.sq) ? 'em' : '' },
            h('td', {}, h('span', { class: 'vldot', style: `background:${color(a)}` }), a.f || a.hex),
            h('td', {}, a.r || ''), h('td', {}, a.t || ''),
            h('td', { class: 'n' }, fmtAlt(a, u)),
            h('td', { class: 'n' }, a.gs < 0 ? '-' : String(u ? Math.round(a.gs * 1.852) : a.gs)),
            h('td', { class: 'n' }, a.g ? '-' : (u ? (a.vr * 0.00508).toFixed(1) + ' m/s' : `${a.vr > 0 ? '+' : ''}${a.vr}`)),
            h('td', { class: 'n' }, a.trk < 0 ? '-' : `${String(a.trk % 360).padStart(3, '0')}°`),
            h('td', {}, a.sq.startsWith('-') ? '-' : a.sq + (a.mil ? ' ★' : '')),
            h('td', { class: 'n' }, fmtDist(a.d, u)))))));
    }
    async function poll() {
      if (gone) return;
      try {
        const r = await fetch(`/api/live?app=${APP_ID}&key=ac`);
        if (r.ok) { last = await r.json(); drawTable(); }
        else if (r.status === 404) put(table, h('p', { class: 'muted' }, tr('Abrí Vuelos en la placa para ver acá lo que ve.')));
      } catch (e) { /* the next one */ }
      if (!gone) timer = setTimeout(poll, 3000);
    }

    function drawAll() {
      drawPlace();
      drawSources();
      put(settings,
        h('div', { class: 'vlgrid' },
          field('radius', tr('Radio (km)'), { type: 'number', min: 10, max: 400, step: 5 }),
          select('units', tr('Unidades'), [['aviation', tr('De aviación (ft, kt, NM)')], ['metric', tr('Métricas (m, km/h, km)')]]),
          select('trail', tr('Estelas'), [['0', tr('No')], ['1', '1 min'], ['3', '3 min'], ['5', '5 min'], ['10', '10 min']])),
        h('div', { class: 'vlchk', style: 'margin-top:12px' },
          check('labels', tr('Nombres en el mapa')), check('routes', tr('Origen y destino (adsbdb)'))));
      put(alerts,
        h('div', { class: 'vlchk' },
          check('alert_over', tr('Un avión pasa encima')), check('alert_emerg', tr('Emergencias (7500, 7600, 7700)')),
          check('alert_mil', tr('Militares')), check('sound', tr('Con sonido'))),
        h('div', { class: 'vlgrid', style: 'margin-top:12px' },
          field('over_km', tr('"Encima" es a menos de (km)'), { type: 'number', min: 0.5, max: 20, step: 0.5 }),
          field('over_ft', tr('y por debajo de (ft)'), { type: 'number', min: 1000, max: 60000, step: 500 })));
    }

    const settings = h('div', {});
    const alerts = h('div', {});
    put(main,
      h('h1', {}, tr('Vuelos')),
      h('h2', {}, tr('Lugar')),
      h('div', { class: 'card pad' }, placeBox,
        h('div', { class: 'vlsearch', style: 'margin-top:12px' }, q, h('button', { class: 'btn pri', onclick: search }, tr('Buscar'))),
        h('div', { class: 'btns', style: 'margin-top:10px' }, h('button', { class: 'btn', onclick: fromClima }, tr('Usar el lugar de Clima'))),
        h('p', { class: 'note' }, tr('La búsqueda es de Open-Meteo. En la placa, una pulsación larga sobre el mapa también elige el lugar.'))),
      results,
      h('h2', {}, tr('Fuente de los datos')),
      h('div', { class: 'card pad' }, srcBox, h('div', { style: 'margin-top:12px' }, srcExtra)),
      h('h2', {}, tr('Mapa y datos')),
      h('div', { class: 'card pad' }, settings),
      h('h2', {}, tr('Avisos')),
      h('div', { class: 'card pad' }, alerts,
        h('p', { class: 'note' }, tr('Los avisos salen mientras Vuelos está abierta en la placa.'))),
      h('div', { class: 'btns', style: 'margin:14px 0' },
        h('button', { class: 'btn pri', onclick: save }, tr('Guardar')),
        h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast(tr('Abierta en la placa'))).catch(e => P.toast(e.message, true)) }, tr('Abrir Vuelos en la placa'))),
      h('h2', {}, tr('Lo que ve la placa')),
      h('div', { class: 'card pad' }, table));
    load();
    poll();
    return () => { gone = true; clearTimeout(timer); };
  },
});
