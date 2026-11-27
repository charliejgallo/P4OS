/* Cielo's page in the board's portal (docs/PORTAL-PAGES.md): the sky of
 * now over the board's place, the coming passes of the satellites it
 * follows, and the place, searched by name.
 *
 * What it reads, all written by the app (apps/cielo/main/cl_sat.c):
 *   /data/cielo_ahora.json     the moment, the place, the sidereal time, the
 *                              bodies and the satellites (every 30 s while
 *                              the app is open)
 *   /data/cielo_pasadas.json   the passes, worked out on the board
 *   /apps/cielo_p4.pak         the app's sky pack: the stars and the figures
 *                              are drawn here from the same catalogue
 * and what it writes: the "place=" line of /data/cielo.txt, which the app
 * takes again within two seconds. The search is Open-Meteo's geocoding (free,
 * no key), asked from the browser, as the Clima page does. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const CONF = '/data/cielo.txt';
const NOW = '/data/cielo_ahora.json';
const PASSES = '/data/cielo_pasadas.json';
const PACK = '/apps/cielo_p4.pak';
const APP_ID = 'aos.cielo';

if (!document.getElementById('css-cielo')) {
  const st = document.createElement('style');
  st.id = 'css-cielo';
  st.textContent = `
.clsky { display: flex; flex-wrap: wrap; gap: 18px; align-items: flex-start; }
.clsky canvas { width: min(100%, 560px); aspect-ratio: 1; border-radius: 50%; background: #02050c; }
.clsky .side { flex: 1; min-width: 220px; }
.cltab { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
.cltab th, .cltab td { text-align: left; padding: 8px 6px; border-bottom: 1px solid var(--line); }
.cltab th { color: var(--dim); font-weight: 500; font-size: 13px; }
.cltab tr.dim td { color: var(--dim); }
.cltab td.vis { color: var(--green); font-weight: 600; }
.clsearch { display: flex; gap: 10px; flex-wrap: wrap; }
.clsearch input { flex: 1; min-width: 140px; }
.clbody { display: flex; justify-content: space-between; padding: 4px 0; border-bottom: 1px solid var(--line); }
@media (max-width: 560px) { .cltab .hide { display: none; } }
`;
  document.head.append(st);
}

const COMPASS = [N_('N'), N_('NE'), N_('E'), N_('SE'), N_('S'), N_('SO'), N_('O'), N_('NO')];
const compass = az => tr(COMPASS[Math.round(az / 45) & 7]);
const DAY = [N_('dom'), N_('lun'), N_('mar'), N_('mié'), N_('jue'), N_('vie'), N_('sáb')];
const hm = t => { const d = new Date(t * 1000); return d.toTimeString().slice(0, 5); };
const dayhm = t => { const d = new Date(t * 1000); return `${tr(DAY[d.getDay()])} ${d.getDate()}/${d.getMonth() + 1} ${hm(t)}`; };

/* the pack: the stars (J2000 unit vectors, magnitudes, B-V) and the figures */
function parsePack(buf) {
  const dv = new DataView(buf);
  const magic = new TextDecoder().decode(new Uint8Array(buf, 0, 8));
  if (magic !== 'CIELOPK1') throw new Error(tr('el pack no es de esta versión'));
  const n = dv.getUint32(8, true), sec = {};
  for (let i = 0; i < n; i++) {
    const o = 12 + 12 * i;
    const tag = new TextDecoder().decode(new Uint8Array(buf, o, 4));
    sec[tag] = [dv.getUint32(o + 4, true), dv.getUint32(o + 8, true)];
  }
  const [so] = sec.STAR, ns = dv.getUint32(so, true), stars = [];
  for (let i = 0; i < ns; i++) {
    const r = so + 4 + 24 * i;
    const x = dv.getInt16(r, true) / 32767, y = dv.getInt16(r + 2, true) / 32767, z = dv.getInt16(r + 4, true) / 32767;
    const mag = dv.getInt16(r + 6, true) / 100, bv = dv.getInt16(r + 8, true);
    stars.push({ ra: Math.atan2(y, x), dec: Math.asin(Math.max(-1, Math.min(1, z))), mag, bv: bv === 32767 ? 0.6 : bv / 1000 });
  }
  const [go] = sec.SEGS, ng = dv.getUint32(go, true), segs = [];
  for (let i = 0; i < ng; i++) segs.push([dv.getUint16(go + 4 + 4 * i, true), dv.getUint16(go + 6 + 4 * i, true)]);
  return { stars, segs };
}

/* a star's colour by B-V, roughly as the app's (cl_cat.c) */
function bvColour(bv) {
  if (bv < 0) return '#aac4ff';
  if (bv < 0.3) return '#dfe8ff';
  if (bv < 0.6) return '#fff8ee';
  if (bv < 1.0) return '#ffe6c4';
  if (bv < 1.4) return '#ffcf9c';
  return '#ffb880';
}

/* alt and az (radians) of a direction of RA/Dec for the sidereal time and
 * latitude; the pack is J2000 and the time of date: a third of a degree,
 * nothing at this size */
function altaz(ra, dec, lst, lat) {
  const H = lst - ra;
  const sa = Math.sin(lat) * Math.sin(dec) + Math.cos(lat) * Math.cos(dec) * Math.cos(H);
  const alt = Math.asin(Math.max(-1, Math.min(1, sa)));
  const az = Math.atan2(-Math.cos(dec) * Math.sin(H), Math.sin(dec) * Math.cos(lat) - Math.cos(dec) * Math.cos(H) * Math.sin(lat));
  return [alt, (az + 2 * Math.PI) % (2 * Math.PI)];
}

function drawSky(cv, pack, now) {
  const dpr = window.devicePixelRatio || 1, size = Math.round(cv.clientWidth * dpr) || 560;
  cv.width = cv.height = size;
  const g = cv.getContext('2d'), R = size / 2 - 4 * dpr, c = size / 2;
  const red = !!now.night;
  const col = s => red ? '#ff4030' : s;
  const lat = now.lat * Math.PI / 180, lst = now.lst * Math.PI / 180;
  /* the pole that is up on top: south in the south, as the app's dome */
  const top = now.lat < 0 ? Math.PI : 0;
  const proj = (alt, az) => {
    const r = R * Math.tan((Math.PI / 2 - alt) / 2);
    const f = top - az;
    return [c + r * Math.sin(f), c - r * Math.cos(f)];
  };
  g.fillStyle = '#02050c';
  g.fillRect(0, 0, size, size);
  g.strokeStyle = col('#2f4a32');
  g.lineWidth = 2 * dpr;
  g.beginPath(); g.arc(c, c, R, 0, 2 * Math.PI); g.stroke();
  if (pack) {
    const pos = pack.stars.map(s => altaz(s.ra, s.dec, lst, lat));
    g.strokeStyle = col('rgba(58,110,165,0.75)');
    g.lineWidth = 1 * dpr;
    g.beginPath();
    for (const [a, b] of pack.segs) {
      if (pos[a][0] < 0 || pos[b][0] < 0) continue;
      const [x1, y1] = proj(...pos[a]), [x2, y2] = proj(...pos[b]);
      g.moveTo(x1, y1); g.lineTo(x2, y2);
    }
    g.stroke();
    for (let i = 0; i < pack.stars.length; i++) {
      const s = pack.stars[i];
      if (s.mag > 5.2) break;
      if (pos[i][0] < 0) continue;
      const [x, y] = proj(...pos[i]);
      g.fillStyle = col(bvColour(s.bv));
      g.beginPath(); g.arc(x, y, Math.max(0.5, (5.4 - s.mag) * 0.42) * dpr, 0, 2 * Math.PI); g.fill();
    }
  }
  g.font = `${13 * dpr}px system-ui, sans-serif`;
  g.textAlign = 'center';
  for (let k = 0; k < 8; k += 2) {
    const [x, y] = proj(-0.06, k * Math.PI / 4);
    g.fillStyle = col(k === 0 ? '#ff8a7a' : '#d9c27a');
    g.fillText(tr(COMPASS[k]), x, y + 4 * dpr);
  }
  g.textAlign = 'left';
  for (const b of now.bodies || []) {
    if (b.alt < 0) continue;
    const [x, y] = proj(b.alt * Math.PI / 180, b.az * Math.PI / 180);
    const r = (b.id <= 1 ? 7 : 4) * dpr;
    g.fillStyle = col(b.id === 0 ? '#ffe9a0' : b.id === 1 ? '#f0f0ea' : '#ffc98a');
    g.beginPath(); g.arc(x, y, r, 0, 2 * Math.PI); g.fill();
    g.fillText(b.name, x + r + 4 * dpr, y + 4 * dpr);
  }
  for (const s of now.sats || []) {
    if (s.alt < 0) continue;
    const [x, y] = proj(s.alt * Math.PI / 180, s.az * Math.PI / 180);
    g.fillStyle = col(s.lit ? '#5fd0ff' : '#6c8c9a');
    g.fillRect(x - 4 * dpr, y - 4 * dpr, 8 * dpr, 8 * dpr);
    g.fillText(s.name, x + 8 * dpr, y + 4 * dpr);
  }
}

P.registerPage({
  id: 'cielo',
  name: tr('Cielo'),
  icon: '✦',
  render(main) {
    const { h, put } = P;
    let gone = false, pack = null, timer = 0, onlyVisible = true;
    const cv = h('canvas', {});
    const side = h('div', { class: 'side' }, h('span', { class: 'muted' }, tr('Cargando…')));
    const passes = h('div', { class: 'card pad' }, h('span', { class: 'muted' }, tr('Cargando…')));
    const placeBox = h('div', { class: 'card pad' });
    const q = h('input', { type: 'search', placeholder: tr('Ciudad, pueblo o barrio'), autocomplete: 'off' });
    const lat = h('input', { type: 'number', step: '0.0001', min: '-90', max: '90', placeholder: tr('Latitud') });
    const lon = h('input', { type: 'number', step: '0.0001', min: '-180', max: '180', placeholder: tr('Longitud') });
    const results = h('div', { class: 'card', style: 'display:none' });

    async function sky() {
      let now = null;
      try { now = JSON.parse(await P.fsText(NOW) || 'null'); } catch (e) { /* not yet */ }
      if (gone) return;
      if (!now) {
        put(side, h('p', { class: 'note' }, tr('La app todavía no escribió el cielo: abrila en la placa.')));
        return;
      }
      drawSky(cv, pack, now);
      put(side,
        h('div', {}, h('b', {}, now.place), ' ', h('span', { class: 'muted small' }, `${now.lat.toFixed(2)}, ${now.lon.toFixed(2)}`)),
        h('div', { class: 'muted small', style: 'margin-bottom:10px' }, tr('El cielo a las %s', dayhm(now.time))),
        (now.bodies || []).map(b => h('div', { class: 'clbody' },
          h('span', {}, b.name),
          h('span', { class: b.alt > 0 ? '' : 'muted' }, b.alt > 0 ? `${b.alt.toFixed(0)}° ${compass(b.az)} · ${b.mag.toFixed(1)}` : tr('bajo el horizonte')))),
        pack ? null : h('p', { class: 'note' }, tr('Sin el pack del cielo en la tarjeta, sólo los planetas.')));
    }

    async function loadPasses() {
      let d = null;
      try { d = JSON.parse(await P.fsText(PASSES) || 'null'); } catch (e) { /* not yet */ }
      if (gone) return;
      if (!d) { put(passes, h('p', { class: 'note' }, tr('Todavía no hay pasadas: la app las calcula al abrirse.'))); return; }
      const now = Date.now() / 1000;
      const list = (d.passes || []).filter(p => p.set > now && (!onlyVisible || p.vis_start));
      const tle = d.tle_time ? tr('Órbitas de hace %s h', Math.max(0, Math.round((now - d.tle_time) / 3600))) : tr('Sin órbitas todavía');
      put(passes,
        h('div', { class: 'row' },
          h('label', { class: 'grow' }, h('input', { type: 'checkbox', checked: onlyVisible, onchange: e => { onlyVisible = e.target.checked; loadPasses(); } }), ' ', tr('Sólo las visibles')),
          h('span', { class: 'muted small' }, tle)),
        list.length ? h('table', { class: 'cltab' },
          h('tr', {}, h('th', {}, tr('Satélite')), h('th', {}, tr('Empieza')), h('th', {}, tr('Máximo')), h('th', { class: 'hide' }, tr('Termina')), h('th', {}, tr('Brillo'))),
          list.map(p => {
            const vs = p.vis_start || p.rise, ve = p.vis_end || p.set;
            return h('tr', { class: p.vis_start ? '' : 'dim' },
              h('td', {}, p.sat),
              h('td', {}, dayhm(vs), h('div', { class: 'muted small' }, compass(p.rise_az))),
              h('td', {}, `${hm(p.top)} · ${p.top_el}°`, h('div', { class: 'muted small' }, compass(p.top_az))),
              h('td', { class: 'hide' }, hm(ve), h('div', { class: 'muted small' }, compass(p.set_az))),
              p.vis_start ? h('td', { class: 'vis' }, p.mag < 90 ? p.mag.toFixed(1) : '?') : h('td', {}, tr('no visible')));
          })) : h('p', { class: 'note' }, onlyVisible ? tr('Ninguna pasada visible en los próximos días.') : tr('Ninguna pasada en los próximos días.')),
        h('p', { class: 'note' }, tr('Visible: el satélite está al sol y el cielo, oscuro. El brillo es una estimación: cuanto más bajo el número, más brilla.')));
    }

    async function readConf() {
      const t = await P.fsText(CONF) || '';
      const ln = t.split('\n').find(l => l.startsWith('place='));
      const [name, la, lo] = ln ? ln.slice(6).split('\t') : [];
      put(placeBox,
        h('div', {}, h('b', {}, name || tr('Sin lugar elegido')), ' ', la ? h('span', { class: 'muted small' }, `${(+la).toFixed(4)}, ${(+lo).toFixed(4)}`) : null),
        h('div', { class: 'clsearch', style: 'margin-top:12px' }, q, h('button', { class: 'btn pri', onclick: search }, tr('Buscar'))),
        h('div', { class: 'clsearch', style: 'margin-top:10px' }, lat, lon, h('button', { class: 'btn', onclick: () => save(tr('Mi lugar'), +lat.value, +lon.value) }, tr('Usar estas coordenadas'))),
        h('p', { class: 'note' }, tr('La latitud es negativa al sur del ecuador y la longitud, al oeste de Greenwich.')));
      if (la) { lat.value = (+la).toFixed(4); lon.value = (+lo).toFixed(4); }
      return t;
    }

    async function save(name, la, lo) {
      if (!(la >= -90 && la <= 90 && lo >= -180 && lo <= 180) || isNaN(la) || isNaN(lo)) { P.toast(tr('Coordenadas fuera de rango'), true); return; }
      name = name.replace(/[\t\n]/g, ' ');
      while (new TextEncoder().encode(name).length > 47) name = name.slice(0, -1);
      try {
        const t = await P.fsText(CONF) || '';
        const lines = t.split('\n').filter(l => l && !l.startsWith('place='));
        lines.splice(lines[0] && lines[0].startsWith('#') ? 1 : 0, 0, `place=${name}\t${la.toFixed(4)}\t${lo.toFixed(4)}`);
        await P.fsMkdir('/data');
        await P.fsPut(CONF, lines.join('\n') + '\n');
        P.toast(tr('%s: la placa lo toma en unos segundos', name));
        results.style.display = 'none';
        q.value = '';
        readConf();
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
        put(results, list.length ? list.map(r => h('div', { class: 'row click', onclick: () => save(r.name, r.latitude, r.longitude) },
          h('div', { class: 'grow' }, h('b', {}, r.name), h('div', { class: 'muted small' }, [r.admin1, r.country].filter(Boolean).join(', '))),
          h('div', { class: 'val' }, tr('Elegir')))) : h('div', { class: 'row muted' }, tr('No se encontró ningún lugar con ese nombre.')));
      } catch (e) { put(results, h('div', { class: 'row bad' }, tr('No se pudo buscar: %s', e.message))); }
    }
    q.onkeydown = e => { if (e.key === 'Enter') search(); };

    put(main, h('h1', {}, tr('Cielo')),
      h('div', { class: 'card pad clsky' }, cv, side),
      h('h2', {}, tr('Próximas pasadas')), passes,
      h('h2', {}, tr('Lugar')), placeBox, results,
      h('div', { class: 'btns', style: 'margin-top:14px' },
        h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast(tr('Abierta en la placa'))).catch(e => P.toast(e.message, true)) }, tr('Abrir Cielo en la placa'))),
      h('p', { class: 'note' }, tr('Estrellas: Yale Bright Star Catalogue (dominio público). Órbitas: CelesTrak. Búsqueda de lugares: Open-Meteo.')));
    P.fsBytes(PACK).then(b => { pack = parsePack(b); if (!gone) sky(); }).catch(() => {});
    sky();
    loadPasses();
    readConf().catch(e => P.toast(e.message, true));
    timer = setInterval(() => { sky(); loadPasses(); }, 30000);
    return () => { gone = true; clearInterval(timer); };
  },
});
