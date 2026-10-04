/* The weather app's page in the board's portal (docs/PORTAL-PAGES.md): where
 * the board looks the weather up, chosen with a real keyboard, and the
 * forecast for it.
 *
 * The place is one line in /data/clima_lugar.txt, "name<TAB>lat10k<TAB>
 * lon10k" (degrees x 10000), which the app writes when a place is chosen on
 * the board and reads when it changes, within three seconds if it is open.
 * The search and the forecast are Open-Meteo's (open-meteo.com, free, no
 * key), asked from the browser, as the app asks from the board. */
const P = window.P4OS;
const FILE = '/data/clima_lugar.txt';
const APP_ID = 'aos.clima';

if (!document.getElementById('css-clima')) {
  const st = document.createElement('style');
  st.id = 'css-clima';
  st.textContent = `
.wxnow { display: flex; gap: 20px; align-items: center; flex-wrap: wrap; }
.wxnow .ic { font-size: 64px; line-height: 1; }
.wxnow .t { font-size: 48px; font-weight: 650; font-variant-numeric: tabular-nums; }
.wxdays { display: grid; grid-template-columns: repeat(auto-fit, minmax(76px, 1fr)); gap: 10px; margin-top: 16px; }
.wxday { background: var(--card2); border-radius: 12px; padding: 10px; text-align: center; }
.wxday .ic { font-size: 28px; }
.wxday .mx { font-weight: 600; }
.wxsearch { display: flex; gap: 10px; }
.wxsearch input { flex: 1; }
`;
  document.head.append(st);
}

/* WMO weather codes, as the app groups them (wx_api.c) */
function wmo(c) {
  if (c === 0) return ['☀️', 'Despejado'];
  if (c <= 2) return ['🌤️', 'Parcialmente nublado'];
  if (c === 3) return ['☁️', 'Nublado'];
  if (c <= 48) return ['🌫️', 'Niebla'];
  if (c <= 57) return ['🌦️', 'Llovizna'];
  if (c <= 67 || (c >= 80 && c <= 82)) return ['🌧️', 'Lluvia'];
  if (c <= 77 || c === 85 || c === 86) return ['🌨️', 'Nieve'];
  return ['⛈️', 'Tormenta'];
}

const parse = t => {
  const [name, lat, lon] = (t || '').split('\n')[0].split('\t');
  return name && lat && lon ? { name, lat10k: +lat, lon10k: +lon } : null;
};
const deg = v => (v / 10000).toFixed(4);
const DAY = ['dom', 'lun', 'mar', 'mié', 'jue', 'vie', 'sáb'];

P.registerPage({
  id: 'clima',
  name: 'Clima',
  icon: '☀',
  render(main) {
    const { h, put } = P;
    const now = h('div', { class: 'card pad' }, h('span', { class: 'muted' }, 'Cargando…'));
    const q = h('input', { type: 'search', placeholder: 'Ciudad, pueblo o barrio', autocomplete: 'off' });
    const results = h('div', { class: 'card', style: 'display:none' });
    let place = null, gone = false;

    async function forecast() {
      if (!place) {
        put(now, h('p', { class: 'muted' }, 'La placa todavía no tiene un lugar elegido: buscalo abajo.'));
        return;
      }
      put(now, h('div', { class: 'muted' }, place.name + ' · cargando el pronóstico…'));
      try {
        const u = `https://api.open-meteo.com/v1/forecast?latitude=${deg(place.lat10k)}&longitude=${deg(place.lon10k)}` +
          '&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code' +
          '&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=auto&forecast_days=7';
        const d = await (await fetch(u)).json();
        if (gone) return;
        const c = d.current, [ic, txt] = wmo(c.weather_code);
        put(now,
          h('div', { class: 'wxnow' },
            h('span', { class: 'ic' }, ic),
            h('div', {}, h('div', { class: 't' }, Math.round(c.temperature_2m) + '°'), h('div', { class: 'muted' }, txt)),
            h('div', { class: 'grow' },
              h('div', {}, h('b', {}, place.name)),
              h('div', { class: 'muted small' }, `Sensación ${Math.round(c.apparent_temperature)}° · humedad ${c.relative_humidity_2m} % · viento ${Math.round(c.wind_speed_10m)} km/h`),
              h('div', { class: 'muted small' }, `${deg(place.lat10k)}, ${deg(place.lon10k)}`))),
          h('div', { class: 'wxdays' }, d.daily.time.map((t, i) => {
            const [di] = wmo(d.daily.weather_code[i]);
            const day = i ? DAY[new Date(t + 'T12:00').getDay()] : 'hoy';
            return h('div', { class: 'wxday' },
              h('div', { class: 'muted small' }, day), h('div', { class: 'ic' }, di),
              h('div', {}, h('span', { class: 'mx' }, Math.round(d.daily.temperature_2m_max[i]) + '°'), ' ',
                h('span', { class: 'muted' }, Math.round(d.daily.temperature_2m_min[i]) + '°')),
              h('div', { class: 'muted small' }, (d.daily.precipitation_probability_max[i] ?? 0) + ' %'));
          })));
      } catch (e) {
        put(now, h('div', {}, h('b', {}, place.name)), h('p', { class: 'note' }, 'No se pudo traer el pronóstico: ' + e.message));
      }
    }

    async function load() {
      try { place = parse(await P.fsText(FILE)); } catch (e) { P.toast(e.message, true); }
      if (!gone) forecast();
    }

    async function choose(r) {
      const lat10k = Math.round(r.latitude * 10000), lon10k = Math.round(r.longitude * 10000);
      /* the app's field holds 39 bytes; a tab would break the line */
      let name = r.name.replace(/\t/g, ' ');
      while (new TextEncoder().encode(name).length > 39) name = name.slice(0, -1);
      try {
        await P.fsMkdir('/data');
        await P.fsPut(FILE, `${name}\t${lat10k}\t${lon10k}\n`);
        P.toast(`${name}: la placa lo toma en unos segundos`);
        results.style.display = 'none';
        q.value = '';
        place = { name, lat10k, lon10k };
        forecast();
      } catch (e) { P.toast(e.message, true); }
    }

    async function search() {
      const name = q.value.trim();
      if (name.length < 2) return;
      results.style.display = '';
      put(results, h('div', { class: 'row muted' }, 'Buscando…'));
      try {
        const lang = (navigator.language || 'es').slice(0, 2);
        const d = await (await fetch(`https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(name)}&count=10&language=${lang}&format=json`)).json();
        const list = d.results || [];
        put(results, list.length ? list.map(r => h('div', { class: 'row click', onclick: () => choose(r) },
          h('div', { class: 'grow' }, h('b', {}, r.name), h('div', { class: 'muted small' }, [r.admin1, r.country].filter(Boolean).join(', '))),
          h('div', { class: 'val' }, 'Elegir'))) : h('div', { class: 'row muted' }, 'No se encontró ningún lugar con ese nombre.'));
      } catch (e) { put(results, h('div', { class: 'row bad' }, 'No se pudo buscar: ' + e.message)); }
    }
    q.onkeydown = e => { if (e.key === 'Enter') search(); };

    put(main, h('h1', {}, 'Clima'), now,
      h('h2', {}, 'Cambiar el lugar'),
      h('div', { class: 'card pad' },
        h('div', { class: 'wxsearch' }, q, h('button', { class: 'btn pri', onclick: search }, 'Buscar')),
        h('p', { class: 'note' }, 'El pronóstico es de Open-Meteo. La app lo toma en unos segundos si está abierta, o la próxima vez que se abra.')),
      results,
      h('div', { class: 'btns', style: 'margin-top:14px' },
        h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast('Abierta en la placa')).catch(e => P.toast(e.message, true)) }, 'Abrir Clima en la placa')));
    load();
    return () => { gone = true; };
  },
});
