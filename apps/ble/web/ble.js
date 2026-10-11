/* The BLE app's page in the board's portal (docs/PORTAL-PAGES.md): the scan
 * seen from a computer, while the app is open on the board.
 *
 * Live through /api/live (docs/PORTAL-PAGES.md "Live data"): the app puts
 * "state" (the scan and the air's two minutes), "devices" (the table) and
 * "dev" (the device this page has open, its packets explained and raw), once
 * a second while someone looks. The page sends lines of key=value: sel,
 * pause, active, duty, env, minrssi, csv, mqtt, forget, fav, alias. The CSVs
 * of the sensors are plain card files under /ble. Texts in Spanish, like the
 * rest of the portal. */
const P = window.P4OS;
const APP = 'aos.ble';
const LIVE = 'live?app=' + APP;

const css = `
.bl-top{display:flex;flex-wrap:wrap;gap:10px 16px;align-items:center}
.bl-canvas{width:100%;display:block;border-radius:10px;background:var(--card2)}
.bl-tbl{width:100%;border-collapse:collapse;font-size:14px}
.bl-tbl th{text-align:left;font-weight:600;color:var(--dim);padding:6px 4px;cursor:pointer;white-space:nowrap;user-select:none}
.bl-tbl td{padding:6px 4px;border-top:1px solid var(--line);vertical-align:top}
.bl-tbl tr.k{cursor:pointer}
.bl-tbl tr.k:hover td{background:var(--card2)}
.bl-tbl tr.sel td{background:color-mix(in srgb,var(--accent) 18%,transparent)}
.bl-tbl tr.gone td{opacity:.5}
.bl-tbl .num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.bl-tbl .sub{color:var(--dim);font-size:12px}
.bl-bar{display:inline-block;height:8px;border-radius:4px;vertical-align:middle;margin-left:6px}
.bl-dot{display:inline-block;width:10px;height:10px;border-radius:5px;margin-right:6px;vertical-align:middle}
.bl-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px}
.bl-sen{background:var(--card2);border-radius:12px;padding:12px;cursor:pointer}
.bl-sen .big{font-size:2em;font-weight:600;font-variant-numeric:tabular-nums}
.bl-kv{display:grid;grid-template-columns:minmax(120px,30%) 1fr;gap:4px 12px;font-size:14px}
.bl-kv .k{color:var(--dim)}
.bl-hex{font-family:ui-monospace,Menlo,monospace;font-size:13px;word-break:break-all;color:var(--dim)}
.bl-wrap{overflow-x:auto}
@media (max-width:700px){.bl-hide-s{display:none}}
`;

const CLS_COLOR = {
  'Teléfono': '#0A84FF', 'Computadora': '#5E5CE6', 'Tableta': '#5E5CE6', 'Reloj': '#FF9F0A', 'Audio': '#BF5AF2',
  'Rastreador': '#FF375F', 'Sensor': '#30D158', 'Baliza': '#40C8E0', 'Teclado o mouse': '#64D2FF', 'Joystick': '#FF6482',
  'Tele': '#AC8E68', 'Salud': '#FF453A', 'Luz': '#FFD60A', 'Deporte': '#66D4CF', 'Placa de desarrollo': '#E5484D',
};
const num = (v, d = 1) => v === undefined || v === null ? '' : (+v).toFixed(d).replace('.', ',');
const age = ms => ms < 2000 ? 'ahora' : ms < 60000 ? Math.round(ms / 1000) + ' s' : ms < 3600000 ? Math.round(ms / 60000) + ' min' : Math.round(ms / 3600000) + ' h';
const rssiColor = r => r >= -60 ? 'var(--green)' : r >= -75 ? 'var(--accent)' : r >= -88 ? '#e6b800' : 'var(--orange)';
const devName = d => d.al || d.n || d.lbl || d.co || 'Sin nombre';
const ivText = v => v <= 0 ? '' : v < 1000 ? v + ' ms' : num(v / 1000) + ' s';

function senLine(s) {
  if (!s) return '';
  if (s.encrypted) return 'cifrado';
  const p = [];
  if (s.temp !== undefined) p.push(num(s.temp) + ' °C');
  if (s.hum !== undefined) p.push(num(s.hum, 0) + ' %');
  if (s.press !== undefined) p.push(num(s.press, 0) + ' hPa');
  if (s.co2 !== undefined) p.push(s.co2 + ' ppm');
  if (s.lux !== undefined) p.push(s.lux + ' lx');
  if (s.open !== undefined) p.push(s.open ? 'abierto' : 'cerrado');
  if (s.motion !== undefined) p.push(s.motion ? 'movimiento' : 'quieto');
  if (s.hr !== undefined) p.push(s.hr + ' lpm');
  if (s.batt !== undefined) p.push('bat. ' + s.batt + ' %');
  else if (s.volt !== undefined) p.push(num(s.volt, 2) + ' V');
  return p.join(' · ');
}

P.registerPage({
  id: 'ble',
  name: 'BLE',
  icon: 'ᛒ',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-ble')) document.head.append(h('style', { id: 'css-ble' }, css));
    let st = null, devs = [], sel = null, dev = null, alive = true;
    let sortKey = 'r', sortDir = -1, query = '', onlyAlive = true, clsFilter = '';
    const drawn = {};
    const changed = (part, sig) => { if (drawn[part] === sig) return false; drawn[part] = sig; return true; };
    const send = text => P.api(LIVE, { method: 'POST', body: text }).catch(e => P.toast(e.message, true));

    /* ---- the top: the state and the controls ---- */
    const status = h('div', { class: 'note' }, '');
    const controls = h('div', { class: 'bl-top', style: 'margin-top:10px' });
    /* told when a link went down while the board was scanning */
    const lostBox = h('div', { style: 'display:none;margin-top:10px;padding:10px 12px;border-radius:10px;background:color-mix(in srgb,var(--orange) 18%,transparent)' });
    const chart = h('canvas', { class: 'bl-canvas', height: 140, style: 'margin-top:12px' });
    const top = h('div', { class: 'card' }, h('h2', {}, 'Bluetooth LE'), status, controls, lostBox, chart);
    const missing = h('div', { class: 'card' });

    /* ---- the devices ---- */
    const search = h('input', { type: 'search', placeholder: 'Buscar por nombre, dirección o fabricante', style: 'flex:1;min-width:200px',
      oninput: () => { query = search.value.toLowerCase(); drawTable(true); } });
    const clsSel = h('select', { onchange: () => { clsFilter = clsSel.value; drawTable(true); } }, h('option', { value: '' }, 'Todo'));
    const aliveBox = h('input', { type: 'checkbox', checked: true, onchange: () => { onlyAlive = aliveBox.checked; drawTable(true); } });
    /* the table is drawn again every second: the clicks are taken on its
     * box, which stays, at pointerdown, so a redraw between pressing and
     * releasing does not swallow them */
    const tableBox = h('div', { class: 'bl-wrap' });
    tableBox.addEventListener('pointerdown', ev => {
      const th = ev.target.closest('th[data-k]'), tr = ev.target.closest('tr[data-a]');
      if (th) {
        const k = th.dataset.k;
        if (sortKey === k) sortDir = -sortDir; else { sortKey = k; sortDir = ['r', 'fav', 'adv'].includes(k) ? -1 : 1; }
        drawTable(true);
      } else if (tr) select(tr.dataset.a);
    });
    const exportBtns = h('div', { class: 'btns' },
      h('button', { class: 'btn', onclick: () => exportCsv() }, 'Bajar la tabla (CSV)'),
      h('button', { class: 'btn', onclick: () => P.saveBlob(new Blob([JSON.stringify(devs, null, 1)], { type: 'application/json' }), `ble-${Date.now()}.json`) }, 'Bajar todo (JSON)'));
    const list = h('div', { class: 'card' }, h('h2', {}, 'Equipos'),
      h('div', { class: 'row', style: 'gap:10px;flex-wrap:wrap' }, search, clsSel, h('label', { class: 'row', style: 'gap:6px' }, aliveBox, 'Sólo los que se oyen')),
      tableBox, exportBtns);

    /* ---- one device ---- */
    const detail = h('div', { class: 'card', style: 'display:none' });
    /* ---- the sensors, and their files ---- */
    const sensors = h('div', { class: 'card' });
    const files = h('div', { class: 'card' });

    put(main, missing, top, sensors, list, detail, files);

    function showMissing(m) {
      missing.style.display = m ? '' : 'none';
      top.style.display = list.style.display = sensors.style.display = m ? 'none' : '';
      if (m) put(missing, h('h2', {}, 'BLE'), h('p', { class: 'note' }, 'La app BLE no está abierta en la placa: los datos en vivo salen de ella.'),
        h('button', { class: 'btn pri', onclick: () => P.openApp(APP).then(() => P.toast('Abriendo BLE en la placa')).catch(e => P.toast(e.message, true)) }, 'Abrir BLE en la placa'));
    }

    function drawControls(s) {
      if (!changed('ctl', [s.paused, s.active, s.duty, s.env, s.min_rssi, s.csv, s.mqtt, s.mqtt_ready, s.bt_off].join())) return;
      const select = (cmd, cur, opts) => h('select', { onchange: ev => send(cmd + '=' + ev.target.value) },
        ...opts.map(([v, t]) => h('option', { value: v, selected: v === cur }, t)));
      const sw = (label, on, cmd) => h('label', { class: 'row', style: 'gap:6px' },
        h('input', { type: 'checkbox', checked: on, onchange: ev => send(cmd + '=' + (ev.target.checked ? 1 : 0)) }), label);
      put(controls,
        h('button', { class: 'btn' + (s.paused ? ' pri' : ''), onclick: () => send('pause=' + (s.paused ? 0 : 1)) }, s.paused ? 'Seguir' : 'Pausa'),
        h('span', {}, 'Escaneo ', select('active', s.active ? 1 : 0, [[1, 'activo'], [0, 'pasivo']])),
        h('span', {}, 'Escuchando ', select('duty', s.duty, [[10, '10 %'], [30, '30 %'], [60, '60 %'], [100, '100 %']])),
        h('span', {}, 'Entorno ', select('env', s.env, [[0, 'al aire libre'], [1, 'casa'], [2, 'oficina']])),
        h('span', {}, 'Señal mínima ', select('minrssi', s.min_rssi, [[-100, 'todos'], [-90, '-90 dBm'], [-80, '-80 dBm'], [-70, '-70 dBm']])),
        sw('CSV de los sensores', s.csv, 'csv'),
        sw(s.mqtt_ready ? 'MQTT' : 'MQTT (no conectado)', s.mqtt, 'mqtt'),
        h('button', { class: 'btn', onclick: () => { if (confirm('¿Vaciar la lista? Los favoritos quedan.')) send('forget=1'); } }, 'Vaciar la lista'));
    }

    function drawLost(s) {
      if (!changed('lost', s.lost + '|' + Math.floor((s.lost_age || 0) / 60000))) return;
      if (!s.lost) { lostBox.style.display = 'none'; return; }
      const w = [];
      if (s.lost & 1) w.push('el teléfono');
      if (s.lost & 2) w.push('la computadora');
      if (s.lost & 4) w.push('el Wi-Fi');
      const what = w.length > 1 ? w.slice(0, -1).join(', ') + ' y ' + w[w.length - 1] : w[0];
      lostBox.style.display = '';
      put(lostBox, h('div', {}, `⚠ Mientras escaneaba se desconectó ${what} (${s.lost_age < 60000 ? 'recién' : 'hace ' + age(s.lost_age)}). El Bluetooth y el Wi-Fi de la placa comparten una sola radio: escuchar menos tiempo o pausar el escaneo les deja más aire.`),
        h('div', { class: 'btns', style: 'margin-top:8px' }, h('button', { class: 'btn', onclick: () => send('lost_ok=1') }, 'Entendido')));
    }

    function drawChart(s) {
      const W = chart.clientWidth || 600, H = 140, dpr = window.devicePixelRatio || 1;
      if (chart.width !== W * dpr) { chart.width = W * dpr; chart.height = H * dpr; }
      const g = chart.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, W, H);
      const pk = s.pkts || [], dv = s.devs || [];
      const mp = Math.max(10, ...pk), md = Math.max(5, ...dv), n = pk.length || 1, bw = W / n;
      const accent = getComputedStyle(document.body).getPropertyValue('--accent') || '#0a84ff';
      g.fillStyle = accent;
      g.globalAlpha = .55;
      pk.forEach((v, i) => { const bh = v / mp * (H - 24); g.fillRect(i * bw, H - bh, Math.max(1, bw - 1), bh); });
      g.globalAlpha = 1;
      g.strokeStyle = '#FF9F0A';
      g.lineWidth = 2;
      g.beginPath();
      dv.forEach((v, i) => { const y = H - v / md * (H - 24); i ? g.lineTo(i * bw, y) : g.moveTo(i * bw, y); });
      g.stroke();
      g.font = '12px sans-serif';
      g.fillStyle = accent;
      g.fillText(`paquetes/s (máx. ${mp})`, 6, 14);
      g.fillStyle = '#FF9F0A';
      g.textAlign = 'right';
      g.fillText(`equipos/s (máx. ${md})`, W - 6, 14);
      g.textAlign = 'left';
    }

    function showState(s) {
      st = s;
      showMissing(false);
      status.textContent = s.bt_off ? 'Bluetooth apagado en la placa (se prende en Ajustes o desde la app).'
        : `${s.alive} cerca · ${s.seen ?? s.known} vistos · ${num(s.pps)} paquetes/s · ${s.total} en total · ${s.lost} perdidos · `
          + (s.paused ? 'en pausa' : `${s.active ? 'activo' : 'pasivo'} ${s.duty} %`)
          /* the table's top, and what fits in one answer of the live channel */
          + (s.forgotten ? ` · la placa guarda ${s.cap} equipos y olvidó ${s.forgotten} para hacer lugar` : '')
          + (s.listed !== undefined && s.listed < s.known ? ` · la tabla de abajo muestra los ${s.listed} más fuertes de ${s.known}` : '');
      drawControls(s);
      drawLost(s);
      drawChart(s);
    }

    /* ---- the table ---- */
    const COLS = [
      ['fav', '★', d => d.fav ? 1 : 0], ['name', 'Nombre', d => devName(d).toLowerCase()], ['cls', 'Qué es', d => d.cls],
      ['co', 'Fabricante', d => d.co || ''], ['a', 'Dirección', d => d.a], ['r', 'Señal', d => d.age < 30000 ? d.ra : -200],
      ['dist', 'Distancia', d => d.dist], ['iv', 'Intervalo', d => d.iv || 1e9], ['adv', 'Paquetes', d => d.adv + d.rsp],
      ['age', 'Oído', d => d.age], ['sub', 'Datos', d => d.sub],
    ];
    const HIDE_S = new Set(['co', 'dist', 'iv', 'adv']);

    function visible() {
      let v = devs;
      if (onlyAlive) v = v.filter(d => d.age < 30000 || d.fav);
      if (clsFilter) v = v.filter(d => d.cls === clsFilter);
      if (query) v = v.filter(d => [devName(d), d.n, d.a, d.co, d.lbl, d.sub].join(' ').toLowerCase().includes(query));
      const col = COLS.find(c => c[0] === sortKey) || COLS[5];
      return v.slice().sort((x, y) => { const a = col[2](x), b = col[2](y); return (a < b ? -1 : a > b ? 1 : 0) * sortDir; });
    }

    function drawTable(force) {
      const v = visible();
      const sig = v.map(d => [d.a, d.r, d.age > 30000, d.fav, d.al, d.n, d.sub, d.adv].join('|')).join(';') + sel + sortKey + sortDir;
      if (!force && !changed('table', sig)) return;
      const clses = [...new Set(devs.map(d => d.cls))].sort();
      if (changed('cls', clses.join())) {
        put(clsSel, h('option', { value: '' }, 'Todo'), ...clses.map(c => h('option', { value: c, selected: c === clsFilter }, c)));
      }
      const head = h('tr', {}, ...COLS.map(([k, t]) => h('th', {
        class: (HIDE_S.has(k) ? 'bl-hide-s' : '') + (['r', 'dist', 'iv', 'adv', 'age'].includes(k) ? ' num' : ''), 'data-k': k,
      }, t + (sortKey === k ? (sortDir < 0 ? ' ▾' : ' ▴') : ''))));
      const rows = v.map(d => {
        const gone = d.age >= 30000;
        const w = Math.max(4, Math.min(60, (d.r + 100) * 0.85));
        return h('tr', { class: 'k' + (d.a === sel ? ' sel' : '') + (gone ? ' gone' : ''), 'data-a': d.a },
          h('td', {}, d.fav ? '★' : ''),
          h('td', {}, h('span', { class: 'bl-dot', style: `background:${CLS_COLOR[d.cls] || '#8e8e93'}` }), devName(d),
            d.al && d.n ? h('div', { class: 'sub' }, d.n) : null),
          h('td', {}, d.cls, d.lbl && d.lbl !== devName(d) ? h('div', { class: 'sub' }, d.lbl) : null),
          h('td', { class: 'bl-hide-s' }, d.co || '', d.cid !== undefined ? h('div', { class: 'sub' }, '0x' + d.cid.toString(16).toUpperCase().padStart(4, '0')) : null),
          h('td', {}, h('span', { class: 'mono small' }, d.a), h('div', { class: 'sub' }, d.k)),
          h('td', { class: 'num' }, gone ? '—' : d.r + ' dBm', gone ? null : h('span', { class: 'bl-bar', style: `width:${w}px;background:${rssiColor(d.r)}` })),
          h('td', { class: 'num bl-hide-s' }, '≈ ' + num(d.dist, d.dist < 10 ? 1 : 0) + ' m'),
          h('td', { class: 'num bl-hide-s' }, ivText(d.iv)),
          h('td', { class: 'num bl-hide-s' }, d.adv + (d.rsp ? ' + ' + d.rsp : '')),
          h('td', { class: 'num' }, age(d.age)),
          h('td', { class: 'sub' }, d.sen ? senLine(d.sen) : d.sub));
      });
      put(tableBox, h('table', { class: 'bl-tbl' }, h('thead', {}, head), h('tbody', {}, ...rows)),
        v.length ? null : h('p', { class: 'note' }, devs.length ? 'Nadie con este filtro.' : 'Escuchando...'));
    }

    function exportCsv() {
      const esc = s => '"' + String(s ?? '').replace(/"/g, '""') + '"';
      const lines = ['direccion,tipo_dir,nombre,alias,clase,fabricante,rssi,rssi_prom,rssi_min,rssi_max,distancia_m,intervalo_ms,anuncios,respuestas,oido_hace_ms,datos'];
      for (const d of devs) lines.push([d.a, d.k, d.n, d.al, d.cls, d.co, d.r, d.ra, d.rmin, d.rmax, d.dist, d.iv, d.adv, d.rsp, d.age, d.sen ? senLine(d.sen) : d.sub].map(esc).join(','));
      P.saveBlob(new Blob([lines.join('\n') + '\n'], { type: 'text/csv' }), `ble-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}.csv`);
    }

    /* ---- the sensors ---- */
    function drawSensors() {
      const ss = devs.filter(d => d.sen);
      const sig = ss.map(d => [d.a, senLine(d.sen), d.al, d.age > 120000].join('|')).join(';');
      if (!changed('sen', sig)) return;
      put(sensors, h('h2', {}, 'Sensores'),
        ss.length ? h('div', { class: 'bl-grid' }, ...ss.map(d => h('div', { class: 'bl-sen', onclick: () => select(d.a), style: d.age > 120000 ? 'opacity:.5' : '' },
          h('div', {}, devName(d)), h('div', { class: 'small muted' }, `${d.sen.format} · ${d.a}`),
          h('div', { class: 'big' }, d.sen.temp !== undefined ? num(d.sen.temp) + '°' : d.sen.encrypted ? 'cifrado' : ''),
          h('div', {}, d.sen.hum !== undefined ? num(d.sen.hum, 0) + ' % de humedad' : ''),
          h('div', { class: 'small muted' }, senLine({ ...d.sen, temp: undefined, hum: undefined }) + ' · ' + age(d.age)))))
          : h('p', { class: 'note' }, 'Ninguno a la vista. Aparecen solos los que anuncian en BTHome, pvvx/ATC, MiBeacon sin cifrar, Govee, Ruuvi, SwitchBot, Qingping, Inkbird o Eddystone TLM.'));
    }

    /* ---- one device ---- */
    function select(a) {
      sel = sel === a ? null : a;
      dev = null;
      drawn.detail = drawn.detailHead = null;
      send('sel=' + (sel || ''));
      drawTable(true);
      drawDetail();
      if (sel) setTimeout(() => detail.scrollIntoView({ behavior: 'smooth', block: 'start' }), 50);
    }

    const sigCanvas = h('canvas', { class: 'bl-canvas', height: 120 });
    const tempCanvas = h('canvas', { class: 'bl-canvas', height: 120 });
    const detailHead = h('div', {});
    const detailBody = h('div', {});

    function drawLine(cv, vals, lo, hi, color, bars) {
      const W = cv.clientWidth || 600, H = 120, dpr = window.devicePixelRatio || 1;
      if (cv.width !== W * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
      const g = cv.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, W, H);
      const n = vals.length, bw = W / n;
      g.fillStyle = g.strokeStyle = color;
      g.lineWidth = 2;
      g.beginPath();
      let pen = false;
      vals.forEach((v, i) => {
        if (v === null || v === 0) { pen = false; return; }
        const y = H - 6 - (v - lo) / (hi - lo) * (H - 12);
        if (bars) g.fillRect(i * bw, y, Math.max(1, bw - 1), H - y);
        else { pen ? g.lineTo(i * bw, y) : g.moveTo(i * bw, y); pen = true; }
      });
      if (!bars) g.stroke();
      g.font = '11px sans-serif';
      g.fillStyle = '#8e8e93';
      g.fillText(num(hi, bars ? 0 : 1), 4, 12);
      g.fillText(num(lo, bars ? 0 : 1), 4, H - 4);
    }

    /* the key of a device that encrypts: typed here, kept by the app in
     * ble/claves.txt; the input is made once per device so typing is not
     * undone by the redraws */
    const keyBox = h('div', {});

    /* ---- the GATT explorer, on the board's connection: the page asks
     * (gatt=<address>, gatt_readall, gatt_read, gatt_sub, gatt_write) and
     * draws what the app puts as "gatt" ---- */
    let gatt = null;
    const gattBox = h('div', {});
    const GATT_STATE = ['Desconectado', 'Conectando...', 'Conectado, listando servicios...', 'Conectado', 'Sin conexión'];
    const propsText = p => [[2, 'lee'], [8, 'escribe'], [4, 'escribe sin respuesta'], [16, 'notifica'], [32, 'indica']]
      .filter(([b]) => p & b).map(([, t]) => t).join(' · ');
    gattBox.addEventListener('pointerdown', ev => {
      const b = ev.target.closest('[data-g]');
      if (!b) return;
      const act = b.dataset.g, hd = b.dataset.h;
      if (act === 'connect') send('gatt=' + sel);
      else if (act === 'off') send('gatt=off');
      else if (act === 'all') send('gatt_readall=1');
      else if (act === 'read') send('gatt_read=' + hd);
      else if (act === 'sub') send(`gatt_sub=${hd},${b.dataset.m}`);
      else if (act === 'hilink') send('gatt_hilink=' + hd);
      else if (act === 'write') {
        const v = prompt('Escribir: texto, o bytes con 0x (por ejemplo 0x01 A0)');
        if (v !== null && v !== '') send(`gatt_write=${hd},${v}`);
      }
    });
    function drawGatt() {
      const g = gatt && gatt.a === sel ? gatt : null;
      if (!changed('gatt', JSON.stringify(g) + sel)) return;
      const btn = (t, act, extra = {}, cls = 'btn') => h('button', { class: cls, 'data-g': act, ...extra }, t);
      if (!g || (!g.open && g.state === 0)) {
        put(gattBox, h('h3', {}, 'GATT'), h('p', { class: 'note' }, 'Conectarse para ver sus servicios y características, leerlas, escribirlas y escuchar sus notificaciones. Mientras tanto la placa deja de escanear un momento; muchos equipos aceptan una sola conexión a la vez.'),
          h('div', { class: 'btns' }, btn('Conectar por GATT', 'connect', {}, 'btn pri')));
        return;
      }
      let st = GATT_STATE[g.state] || '';
      if (g.state === 3) st += ` · MTU ${g.mtu}` + (g.rssi !== null ? ` · señal ${g.rssi} dBm` : '') + (g.reading[0] < g.reading[1] ? ` · leyendo ${g.reading[0] + 1} de ${g.reading[1]}` : '');
      if (g.state === 4) st += `: ${g.why} (${g.reason})`;
      const live = g.state >= 1 && g.state <= 3;
      const rows = [];
      for (const a of g.attrs) {
        if (a.k === 0) {
          rows.push(h('tr', {}, h('td', { colspan: 3, style: 'padding-top:14px' }, h('b', { style: 'color:var(--accent)' }, a.n), h('div', { class: 'sub' }, `Servicio ${a.u} · handles ${a.h} a ${a.end}`))));
          continue;
        }
        const desc = a.k === 2;
        const acts = [];
        if (!desc && (a.p & 2)) acts.push(btn('Leer', 'read', { 'data-h': a.h }));
        if (!desc && (a.p & 12)) acts.push(btn('Escribir', 'write', { 'data-h': a.h }));
        if (a.hl) acts.push(btn('Pedir los datos (Hi-Link)', 'hilink', { 'data-h': a.h }, 'btn pri'));
        if (!desc && (a.p & 48)) acts.push(btn(a.sub ? 'Dejar de escuchar' : (a.p & 16) ? 'Notificaciones' : 'Indicaciones', 'sub',
          { 'data-h': a.h, 'data-m': a.sub ? 0 : (a.p & 16) ? 1 : 2 }, a.sub ? 'btn pri' : 'btn'));
        rows.push(h('tr', {},
          h('td', { style: desc ? 'padding-left:22px' : '' }, desc ? h('span', { class: 'sub' }, a.n) : a.n,
            h('div', { class: 'sub' }, `${a.u} · ${a.h}` + (desc ? '' : ' · ' + propsText(a.p)))),
          h('td', { class: 'bl-hex', style: `white-space:pre-wrap;color:${a.s ? 'var(--orange)' : 'var(--fg, inherit)'}` }, a.v + (a.c > 1 ? '' : '')),
          h('td', { style: 'white-space:nowrap' }, ...acts)));
      }
      put(gattBox, h('h3', {}, 'GATT'), h('p', { class: 'note', style: g.state === 4 ? 'color:var(--orange)' : '' }, st),
        h('div', { class: 'btns' }, g.state === 3 ? btn('Leer todo', 'all', {}, 'btn pri') : null,
          live ? btn('Desconectar', 'off') : btn('Conectar de nuevo', 'connect')),
        rows.length ? h('div', { class: 'bl-wrap' }, h('table', { class: 'bl-tbl' }, h('tbody', {}, ...rows))) : null,
        g.log.length ? h('details', {}, h('summary', { class: 'small muted' }, 'Lo último'), h('div', { class: 'small muted', style: 'white-space:pre-line' }, g.log.join('\n'))) : null);
    }
    async function gattTick() {
      if (!alive || !sel) return;
      const d = devs.find(x => x.a === sel);
      if (!d || !(d.conn || (gatt && gatt.a === sel))) return;
      try { gatt = await P.api(LIVE + '&key=gatt'); } catch { /* none yet */ }
      drawGatt();
    }
    function drawKey(d) {
      if (!changed('key', [d.a, d.key].join('|'))) return;
      const inp = h('input', { placeholder: '32 cifras hexadecimales (la bindkey)', style: 'width:340px;font-family:ui-monospace,Menlo,monospace', maxlength: 47 });
      const st = d.key === -2 ? 'Sin clave: sus lecturas van cifradas.' : d.key === 0 ? 'Clave correcta: las lecturas se descifran.'
        : d.key === 2 ? 'La clave no coincide con lo que anuncia.' : d.key === 3 ? 'Usa un cifrado viejo (MiBeacon v2/v3) que la app no descifra.'
        : 'Clave cargada: esperando un paquete cifrado para probarla.';
      put(keyBox, h('h3', {}, 'Clave'), h('p', { class: 'note', style: d.key === 0 ? 'color:var(--green)' : d.key >= 2 ? 'color:var(--orange)' : '' }, st),
        h('div', { class: 'btns' }, inp,
          h('button', { class: 'btn pri', onclick: () => {
            const k = inp.value.replace(/[\s:-]/g, '');
            if (!/^[0-9a-fA-F]{32}$/.test(k)) { P.toast('Tienen que ser 32 cifras hexadecimales', true); return; }
            /* the app confirms by the device's state in the next table:
             * until then, only that it was sent */
            send(`key=${d.a},${k}`).then(() => P.toast('Clave enviada: la app la guarda y la prueba con el próximo paquete'));
          } }, 'Guardar la clave'),
          d.key > -2 ? h('button', { class: 'btn', onclick: () => send(`key=${d.a},`) }, 'Borrar la clave') : null),
        h('p', { class: 'note small' }, 'La bindkey de un sensor Xiaomi sale de su cuenta de Mi Home (por ejemplo con "Xiaomi Cloud Tokens Extractor"), o de Home Assistant si ya lo lee. BTHome cifrado usa la clave que le pusiste al configurarlo.'));
    }

    function lines(ls) {
      return h('div', { class: 'bl-kv' }, ...ls.flatMap(([k, v]) => [h('div', { class: 'k' }, k), h('div', {}, v)]));
    }

    function drawDetail() {
      const d = devs.find(x => x.a === sel);
      if (!sel || !d) { detail.style.display = 'none'; return; }
      detail.style.display = '';
      if (changed('detailHead', [sel, d.fav, d.al, d.n].join('|'))) {
        const alias = h('input', { value: d.al || '', placeholder: 'Un nombre propio', maxlength: 27, style: 'width:220px' });
        put(detailHead, h('h2', {}, devName(d)),
          h('div', { class: 'btns' },
            h('button', { class: 'btn' + (d.fav ? ' pri' : ''), onclick: () => send(`fav=${d.a},${d.fav ? 0 : 1}`) }, d.fav ? '★ Favorito' : '☆ Marcar'),
            alias, h('button', { class: 'btn', onclick: () => send(`alias=${d.a},${alias.value.replace(/[\n|]/g, ' ')}`) }, 'Guardar el nombre'),
            h('button', { class: 'btn', onclick: () => select(d.a) }, 'Cerrar')));
        put(detail, detailHead, detailBody);
      }
      drawKey(d);
      const sig = JSON.stringify([d.r, d.age, d.adv, d.key, d.sen && d.sen.encrypted, dev && dev.adv, dev && dev.rsp, dev && dev.hist && dev.hist.slice(-3)]);
      if (!changed('detail', sig)) return;
      const info = [['Qué es', d.cls + (d.lbl ? ' · ' + d.lbl : '')], ['Dirección', `${d.a} (${d.k})`]];
      if (d.co) info.push(['Fabricante', `${d.co}` + (d.cid !== undefined ? ` (0x${d.cid.toString(16).toUpperCase().padStart(4, '0')})` : '')]);
      info.push(['Señal', d.age < 10000 ? `${d.r} dBm · prom. ${num(d.ra, 0)} · mín. ${d.rmin} · máx. ${d.rmax}` : '—'],
        ['Distancia', `≈ ${num(d.dist)} m (estimada por la señal)`], ['Intervalo', ivText(d.iv) || '?'],
        ['Paquetes', `${d.adv} anuncios, ${d.rsp} respuestas, cambió ${d.chg} veces`], ['Oído', `hace ${age(d.age)} · desde hace ${age(d.seen)}`],
        ['Conectable', d.conn ? 'sí (abajo, GATT)' : 'no']);
      const parts = [lines(info), h('h3', {}, 'Señal, dos minutos'), sigCanvas];
      if ((d.sen && d.sen.encrypted) || d.key > -2) parts.push(keyBox);
      if (d.conn || (gatt && gatt.a === sel)) parts.push(gattBox);
      if (d.sen) parts.push(h('h3', {}, 'Sensor · ' + d.sen.format), lines(Object.entries(d.sen).filter(([k]) => k !== 'format').map(([k, v]) => [k, String(v)])));
      if (dev && dev.temps && dev.temps.some(v => v !== null)) parts.push(h('h3', {}, 'Temperatura, dos horas'), tempCanvas);
      if (dev && dev.beacon) {
        const b = dev.beacon;
        const kinds = ['', 'iBeacon', 'AltBeacon', 'Eddystone UID', 'Eddystone URL', 'Eddystone TLM', 'Eddystone EID'];
        parts.push(h('h3', {}, 'Baliza · ' + (kinds[b.kind] || '')), lines([['UUID', b.uuid], ['Mayor / menor', `${b.major} / ${b.minor}`], ['Potencia de referencia', b.tx1m + ' dBm']].concat(b.url ? [['URL', b.url]] : [])));
      }
      if (dev) {
        if (dev.adv) parts.push(h('h3', {}, `Anuncio · ${dev.adv.length / 2} bytes`), lines(dev.adv_lines), h('div', { class: 'bl-hex' }, dev.adv.replace(/(..)/g, '$1 ')));
        if (dev.rsp) parts.push(h('h3', {}, `Respuesta al escaneo · ${dev.rsp.length / 2} bytes`), lines(dev.rsp_lines), h('div', { class: 'bl-hex' }, dev.rsp.replace(/(..)/g, '$1 ')));
      } else parts.push(h('p', { class: 'note' }, 'Pidiendo los paquetes a la placa...'));
      put(detailBody, ...parts);
      drawn.gatt = null;
      drawGatt();
      if (dev && dev.hist) drawLine(sigCanvas, dev.hist.map(v => v || null), -100, -30, 'var(--accent)', true);
      if (dev && dev.temps) {
        const t = dev.temps.filter(v => v !== null);
        if (t.length) { let lo = Math.min(...t), hi = Math.max(...t); if (hi - lo < 1) { lo -= .5; hi += .5; } drawLine(tempCanvas, dev.temps, lo, hi, '#FF9F0A', false); }
      }
    }

    async function loadFiles() {
      const fl = await P.fsList('/ble').catch(() => []);
      const csv = fl.filter(f => f.name.endsWith('.csv') && !f.name.startsWith('.')).sort((a, b) => b.name.localeCompare(a.name));
      if (!changed('files', csv.map(f => f.name + f.size).join())) return;
      put(files, h('h2', {}, 'Lecturas guardadas (CSV)'),
        csv.length ? null : h('p', { class: 'note' }, 'Todavía no hay. Se prende con "CSV de los sensores", arriba o en los ajustes de la app.'),
        ...csv.map(f => h('div', { class: 'row' }, h('div', { class: 'grow' }, f.name, h('span', { class: 'small muted' }, ' ' + P.fmtBytes(f.size))),
          h('a', { class: 'btn', href: P.fsUrl('/ble/' + f.name, true) }, 'Bajar'),
          h('button', { class: 'btn red', onclick: async () => { if (!confirm('¿Borrar ' + f.name + '?')) return; await P.fsDelete('/ble/' + f.name).catch(e => P.toast(e.message, true)); drawn.files = null; loadFiles(); } }, '✕'))));
    }

    /* ---- polling ---- */
    async function tick() {
      try {
        showState(await P.api(LIVE + '&key=state'));
      } catch { showMissing(true); return; }
      try { devs = await P.api(LIVE + '&key=devices'); } catch { /* not put yet */ }
      if (sel) { try { dev = await P.api(LIVE + '&key=dev'); if (dev.a !== sel) dev = null; } catch { dev = null; } }
      drawTable(false);
      drawSensors();
      drawDetail();
    }
    showMissing(true);
    let busy = false;
    const loop = async () => { if (busy || !alive) return; busy = true; await tick(); busy = false; };
    loop();
    loadFiles();
    const t1 = setInterval(loop, 1000), t2 = setInterval(loadFiles, 20000), t3 = setInterval(gattTick, 400);
    return () => { alive = false; clearInterval(t1); clearInterval(t2); clearInterval(t3); };
  },
});
