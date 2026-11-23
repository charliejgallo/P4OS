/* Monitor serie's page in the board's portal (docs/PORTAL-PAGES.md): the
 * chart live while the app is open on the board, its series, a line to
 * send, the last lines, the CSVs it recorded, and its profiles and alarms.
 *
 * Live through /api/live (docs/PORTAL-PAGES.md "Live data"): the app puts
 * "data" four times a second (binary float32: ns, n, the window in seconds,
 * how far back a paused view ends, then n rows of the time in seconds back
 * from now and a value per series, NaN where none) and "state" every second
 * (JSON). The page sends lines of key=value: pause, win, clear, rec, send.
 * The profiles and alarms are /data/plotter.txt, which the app reads again
 * when its date changes. The texts are in Spanish and go through tr(), with
 * this page's catalog (web.plotter.lang). */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const num = P.num || ((v, d) => v == null ? '-' : v.toFixed(d).replace('.', ','));
const APP = 'aos.plotter';
const LIVE = 'live?app=' + APP;
const CFG = '/data/plotter.txt';
const WINS = [[10, '10 s'], [30, '30 s'], [60, '1 min'], [300, '5 min'], [900, '15 min'], [3600, '1 h'], [14400, '4 h'], [28800, '8 h']];

const css = `
.pl-cv{width:100%;height:340px;display:block;border-radius:10px;background:#0e1116;touch-action:none;cursor:crosshair}
.pl-leg{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0}
.pl-leg span{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:14px;background:var(--card2);font-size:14px;cursor:pointer}
.pl-leg span.off{opacity:.45}
.pl-leg i{width:10px;height:10px;border-radius:5px;display:inline-block}
.pl-big{font-size:2.6em;font-weight:600;font-variant-numeric:tabular-nums}
.pl-seg{display:flex;flex-wrap:wrap;gap:6px}
.pl-seg .btn.on{background:var(--accent);color:#fff}
.pl-tab{width:100%;border-collapse:collapse;font-size:14px;font-variant-numeric:tabular-nums}
.pl-tab td,.pl-tab th{padding:6px 4px;border-top:1px solid var(--line);text-align:right}
.pl-tab td:first-child,.pl-tab th:first-child{text-align:left}
.pl-tab tr.al td{color:var(--red)}
.pl-raw{font-family:ui-monospace,Menlo,monospace;font-size:13px;line-height:1.35;white-space:pre-wrap;word-break:break-all}
.pl-raw .g{color:var(--accent)}
.pl-in{width:100%;box-sizing:border-box;padding:8px;border-radius:8px;border:1px solid var(--line);background:var(--card2);color:inherit;font:inherit}
.pl-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px;margin:8px 0}
.pl-grid label{display:flex;flex-direction:column;font-size:12px;color:var(--dim);gap:3px}
.pl-prof{border:1px solid var(--line);border-radius:12px;padding:10px 12px;margin:10px 0}
.pl-hover{font-size:13px;color:var(--dim);min-height:1.3em}
@media (max-width:600px){.pl-cv{height:260px}.pl-big{font-size:2em}}
`;

/* ---- the profiles file: the same parser and writer as pl_cfg.c ---- */

function tokens(line) {
  const out = [];
  let cur = '', q = false, any = false;
  for (const c of line) {
    if (c === '"') { q = !q; any = true; continue; }
    if (!q && (c === ' ' || c === '\t')) { if (cur || any) out.push(cur); cur = ''; any = false; continue; }
    cur += c;
  }
  if (cur || any) out.push(cur);
  return out;
}

function parseCfg(text) {
  const cfg = { profiles: [], alarms: [] };
  let cur = null;
  for (const raw of (text || '').split('\n')) {
    const t = tokens(raw.trim());
    if (!t.length || t[0].startsWith('#')) continue;
    if (t[0] === 'profile') { cur = { name: t[1] || '', match: '', comma: false, fields: [] }; cfg.profiles.push(cur); }
    else if (t[0] === 'end') cur = null;
    else if (t[0] === 'match' && cur) cur.match = t[1] || '';
    else if (t[0] === 'comma' && cur) cur.comma = t[1] === '1';
    else if (t[0] === 'field' && cur) {
      const f = { field: +t[1] || 1, name: t[2] || '', unit: t[3] || '', scale: 1, offset: 0, dp: 0, map: '' };
      for (const kv of t.slice(4)) {
        const [k, v] = [kv.slice(0, kv.indexOf('=')), kv.slice(kv.indexOf('=') + 1)];
        if (k === 'scale') f.scale = +v || 1;
        else if (k === 'offset') f.offset = +v || 0;
        else if (k === 'dp') f.dp = +v || 0;
        else if (k === 'map') f.map = v;
      }
      cur.fields.push(f);
    } else if (t[0] === 'alarm') {
      const a = { name: t[1] || '', lo: '', hi: '', beep: false };
      for (const kv of t.slice(2)) {
        const [k, v] = [kv.slice(0, kv.indexOf('=')), kv.slice(kv.indexOf('=') + 1)];
        if (k === 'lo') a.lo = v === '-' ? '' : v;
        else if (k === 'hi') a.hi = v === '-' ? '' : v;
        else if (k === 'beep') a.beep = v === '1';
      }
      if (a.name) cfg.alarms.push(a);
    }
  }
  return cfg;
}

const clean = s => String(s ?? '').replace(/["\n\r]/g, '');
const numOr = (s, d) => { const v = parseFloat(String(s).replace(',', '.')); return isFinite(v) ? v : d; };

function writeCfg(cfg) {
  let s = '# Monitor serie: perfiles y avisos (los edita la app y la página del portal)\n';
  for (const p of cfg.profiles) {
    s += `\nprofile "${clean(p.name)}"\nmatch "${clean(p.match)}"\ncomma ${p.comma ? 1 : 0}\n`;
    for (const f of p.fields)
      s += `field ${Math.max(1, f.field | 0)} "${clean(f.name)}" "${clean(f.unit)}" scale=${numOr(f.scale, 1) || 1} offset=${numOr(f.offset, 0)} dp=${Math.max(0, f.dp | 0)} map="${clean(f.map)}"\n`;
    s += 'end\n';
  }
  const al = cfg.alarms.filter(a => a.name && (a.lo !== '' || a.hi !== ''));
  if (al.length) s += '\n';
  for (const a of al) {
    const lo = a.lo === '' ? '-' : numOr(a.lo, 0), hi = a.hi === '' ? '-' : numOr(a.hi, 0);
    s += `alarm "${clean(a.name)}" lo=${lo} hi=${hi} beep=${a.beep ? 1 : 0}\n`;
  }
  return s;
}

/* ---- numbers ---- */

function niceStep(range, ticks) {
  if (!(range > 0)) return 1;
  const raw = range / ticks, mag = Math.pow(10, Math.floor(Math.log10(raw))), n = raw / mag;
  return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
}
const decOf = step => step >= 1 ? 0 : Math.min(6, Math.ceil(-Math.log10(step) - 1e-3));
function ago(s) {
  if (!s) return '0';
  if (s % 3600 === 0) return `-${s / 3600} h`;
  if (s % 60 === 0) return `-${s / 60} min`;
  if (s >= 60) return `-${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  return `-${s} s`;
}
const fmtv = (v, d) => v == null || !isFinite(v) ? '-' : num(v, d);

P.registerPage({
  id: 'plotter',
  name: tr('Monitor serie'),
  icon: '↝',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-plotter')) document.head.appendChild(h('style', { id: 'css-plotter' }, css));
    let alive = true, state = null, data = null, hidden = new Set(), hoverX = null, cfg = null;

    const status = h('div', { class: 'small muted' });
    const missing = h('div', { class: 'card pad' },
      h('p', {}, tr('Monitor serie no está abierto en la placa: el gráfico en vivo aparece cuando la app está abierta (al frente o atrás).')),
      h('button', { class: 'btn pri', onclick: () => P.openApp(APP) }, tr('Abrir en la placa')));
    const big = h('div', {});
    const cv = h('canvas', { class: 'pl-cv' });
    const leg = h('div', { class: 'pl-leg' });
    const hover = h('div', { class: 'pl-hover' });
    const ctrls = h('div', { class: 'pl-seg' });
    const table = h('div', {});
    const raw = h('div', { class: 'pl-raw' });
    const files = h('div', {});
    const prof = h('div', {});
    const input = h('input', { class: 'pl-in', placeholder: tr('Línea para enviar…'),
      onkeydown: e => { if (e.key === 'Enter') sendLine(); } });

    const send = text => P.api(LIVE, { method: 'POST', body: text }).catch(e => P.toast(e.message, true));
    function sendLine() {
      if (!input.value) return;
      send('send=' + input.value).then(() => P.toast(tr('Enviado')));
      input.value = '';
    }

    put(main,
      h('h1', {}, tr('Monitor serie')),
      missing,
      h('div', { class: 'card pad' }, status, big, ctrls, leg, cv, hover,
        h('div', { class: 'btns' },
          h('button', { class: 'btn', onclick: savePng }, tr('Bajar la imagen')),
          h('button', { class: 'btn', onclick: saveCsv }, tr('Bajar lo que se ve (CSV)')))),
      h('div', { class: 'card pad' }, h('h2', {}, tr('Series')), table),
      h('div', { class: 'card pad' }, h('h2', {}, tr('Enviar al puerto')),
        h('div', { class: 'row' }, h('div', { class: 'grow' }, input), h('button', { class: 'btn pri', onclick: sendLine }, tr('Enviar'))),
        h('p', { class: 'note' }, tr('Con el fin de línea que se eligió en la app.'))),
      h('div', { class: 'card pad' }, h('h2', {}, tr('Lo que llega')), raw),
      h('div', { class: 'card pad' }, h('h2', {}, tr('Grabaciones (CSV)')), files),
      h('div', { class: 'card pad' }, h('h2', {}, tr('Perfiles y avisos')), prof));

    /* ---- the chart ---- */
    function draw() {
      const dpr = window.devicePixelRatio || 1, W = cv.clientWidth, H = cv.clientHeight;
      if (cv.width !== W * dpr || cv.height !== H * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
      const g = cv.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.fillStyle = '#0e1116';
      g.fillRect(0, 0, W, H);
      const axisW = 64, px = 8, py = 10, pw = W - axisW - px, ph = H - py - 26;
      if (!data || !state || !data.n) {
        g.fillStyle = '#8e8e93';
        g.font = '15px system-ui';
        g.textAlign = 'center';
        g.fillText(state ? tr('Esperando datos…') : '', W / 2, H / 2);
        return;
      }
      const ser = state.series || [], ns = data.ns;
      let lo = Infinity, hi = -Infinity;
      for (let s = 0; s < ns; s++) {
        if (hidden.has(s) || (ser[s] && ser[s].hidden)) continue;
        for (let i = 0; i < data.n; i++) { const v = data.v[i * ns + s]; if (isFinite(v)) { if (v < lo) lo = v; if (v > hi) hi = v; } }
      }
      if (!isFinite(lo)) { lo = 0; hi = 1; }
      if (hi - lo < 1e-9) { const m = Math.abs(lo) * 0.05 || 1; lo -= m; hi += m; }
      const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
      const win = data.win, end = -data.back, t0 = end - win;
      const X = t => px + (t - t0) / win * pw, Y = v => py + (hi - v) / (hi - lo) * ph;
      g.font = '12px system-ui';
      g.strokeStyle = '#2a3038';
      g.fillStyle = '#8e8e93';
      g.lineWidth = 1;
      const step = niceStep(hi - lo, ph > 250 ? 5 : 4), d = decOf(step);
      g.textAlign = 'left';
      for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) {
        const y = Math.round(Y(v)) + 0.5;
        g.beginPath(); g.moveTo(px, y); g.lineTo(px + pw, y); g.stroke();
        g.fillText(num(Math.abs(v) < step / 1000 ? 0 : v, d), px + pw + 6, y + 4);
      }
      const steps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200];
      const ts = steps.find(s => s * pw / win >= 110) || 7200;
      g.textAlign = 'center';
      for (let k = Math.ceil(-end / ts); k * ts <= -t0; k++) {
        const x = Math.round(X(-k * ts)) + 0.5;
        g.beginPath(); g.moveTo(x, py); g.lineTo(x, py + ph); g.stroke();
        g.fillText(ago(k * ts), Math.max(px + 14, x), py + ph + 18);
      }
      g.save();
      g.beginPath(); g.rect(px, py, pw, ph); g.clip();
      for (let s = 0; s < ns; s++) {
        if (hidden.has(s) || (ser[s] && ser[s].hidden)) continue;
        g.strokeStyle = ser[s] ? ser[s].color : '#40c8e0';
        g.lineWidth = s === state.big ? 2.5 : 1.8;
        g.beginPath();
        let on = false, prevT = null;
        const gap = Math.max(win / 600 * 3, 1);
        for (let i = 0; i < data.n; i++) {
          const v = data.v[i * ns + s], t = data.t[i];
          if (!isFinite(v)) continue;
          if (on && prevT !== null && t - prevT > gap * 5) on = false;
          if (on) g.lineTo(X(t), Y(v)); else { g.moveTo(X(t), Y(v)); on = true; }
          prevT = t;
        }
        g.stroke();
        for (const k of ['lo', 'hi']) {
          const a = ser[s] && ser[s][k];
          if (a == null) continue;
          g.setLineDash([6, 6]); g.globalAlpha = 0.6;
          g.beginPath(); g.moveTo(px, Y(a)); g.lineTo(px + pw, Y(a)); g.stroke();
          g.setLineDash([]); g.globalAlpha = 1;
        }
      }
      g.restore();
      if (hoverX !== null && hoverX >= px && hoverX <= px + pw) {
        const t = t0 + (hoverX - px) / pw * win;
        let best = -1, bd = Infinity;
        for (let i = 0; i < data.n; i++) { const dd = Math.abs(data.t[i] - t); if (dd < bd) { bd = dd; best = i; } }
        g.strokeStyle = 'rgba(255,255,255,.6)';
        g.beginPath(); g.moveTo(hoverX + 0.5, py); g.lineTo(hoverX + 0.5, py + ph); g.stroke();
        if (best >= 0) {
          const parts = [num(data.t[best], 1) + ' s'];
          for (let s = 0; s < ns; s++) {
            const v = data.v[best * ns + s];
            if (!isFinite(v) || !ser[s]) continue;
            parts.push(`${ser[s].name} ${fmtv(v, ser[s].dec)} ${ser[s].unit}`);
            g.fillStyle = ser[s].color;
            g.beginPath(); g.arc(X(data.t[best]), Y(v), 4, 0, 7); g.fill();
          }
          hover.textContent = parts.join('   ');
        }
      } else hover.textContent = '';
    }
    cv.addEventListener('pointermove', e => { hoverX = e.offsetX; draw(); });
    cv.addEventListener('pointerleave', () => { hoverX = null; draw(); });

    function savePng() { cv.toBlob(b => b && P.saveBlob(b, 'monitor-serie.png'), 'image/png'); }
    function saveCsv() {
      if (!data || !state) return;
      const ser = state.series || [];
      let s = 'seconds' + ser.map(x => ',' + clean(x.name).replace(/,/g, ' ') + (x.unit ? ` (${x.unit})` : '')).join('') + '\n';
      for (let i = 0; i < data.n; i++) {
        s += data.t[i].toFixed(3);
        for (let k = 0; k < data.ns; k++) { const v = data.v[i * data.ns + k]; s += ',' + (isFinite(v) ? v : ''); }
        s += '\n';
      }
      P.saveBlob(new Blob([s], { type: 'text/csv' }), 'monitor-serie.csv');
    }

    /* ---- the state ---- */
    function showState(st) {
      state = st;
      missing.style.display = 'none';
      const ser = st.series || [];
      put(status, (st.running ? tr('Canal %s · %s a %s %s', st.ch, st.port, st.baud, st.frame) : tr('Canal %s · sin conexión', st.ch)) +
        ' · ' + tr('%s líneas', st.lines) + (st.lost ? ' · ' + tr('%s perdidas', st.lost) : '') + ' · ' + st.fmt +
        (st.rec ? ' · ' + tr('grabando') : ''));
      const b = ser[st.big] || ser[0];
      put(big, b ? h('div', {}, h('span', { class: 'small', style: `color:${b.color}` }, b.name), ' ',
        h('div', { class: 'pl-big', style: b.state ? 'color:var(--red)' : '' }, fmtv(b.last, b.dec), ' ', h('span', { class: 'small muted' }, b.unit))) : null);
      put(ctrls,
        h('button', { class: 'btn' + (st.paused ? ' on' : ''), onclick: () => send('pause=' + (st.paused ? 0 : 1)) }, st.paused ? tr('En vivo') : tr('Pausa')),
        ...WINS.map(([s, l]) => h('button', { class: 'btn' + (st.win === s ? ' on' : ''), onclick: () => send('win=' + s) }, l)),
        h('button', { class: 'btn' + (st.rec ? ' on' : ''), onclick: () => send('rec=' + (st.rec ? 0 : 1)) }, st.rec ? tr('Detener la grabación') : tr('Grabar CSV')),
        h('button', { class: 'btn red', onclick: () => confirm(tr('¿Borrar el gráfico?')) && send('clear') }, tr('Borrar')));
      put(leg, ...ser.map((s, i) => h('span', { class: hidden.has(i) || s.hidden ? 'off' : '', onclick: () => { hidden.has(i) ? hidden.delete(i) : hidden.add(i); showState(state); draw(); } },
        h('i', { style: `background:${s.color}` }), s.name)));
      put(table, ser.length ? h('table', { class: 'pl-tab' },
        h('tr', {}, h('th', {}, tr('Serie')), h('th', {}, tr('Último')), h('th', {}, tr('Mín.')), h('th', {}, tr('Máx.')), h('th', {}, tr('Prom.')), h('th', {}, tr('Avisos'))),
        ...ser.map(s => h('tr', { class: s.state ? 'al' : '' },
          h('td', {}, h('span', { style: `color:${s.color}` }, '● '), s.name, s.unit ? ` (${s.unit})` : ''),
          h('td', {}, fmtv(s.last, s.dec)), h('td', {}, fmtv(s.min, s.dec)), h('td', {}, fmtv(s.max, s.dec)), h('td', {}, fmtv(s.avg, s.dec + 1)),
          h('td', {}, (s.lo != null ? '< ' + fmtv(s.lo, s.dec) : '') + (s.lo != null && s.hi != null ? ' · ' : '') + (s.hi != null ? '> ' + fmtv(s.hi, s.dec) : ''))))) :
        h('p', { class: 'note' }, tr('Todavía no llegaron números.')));
      put(raw, ...(st.raw || []).flatMap(([l, got]) => [h('div', {}, l), h('div', { class: 'g' }, '→ ' + got)]));
    }
    function showMissing() { state = null; data = null; missing.style.display = ''; draw(); }

    async function dataLoop() {
      while (alive) {
        const t0 = performance.now();
        try {
          const r = await fetch('/api/' + LIVE + '&key=data', { cache: 'no-store' });
          if (r.ok) {
            const f = new Float32Array(await r.arrayBuffer());
            const ns = f[0] | 0, n = f[1] | 0;
            const d = { ns, n, win: f[2], back: f[3], t: new Float32Array(n), v: new Float32Array(n * ns) };
            for (let i = 0; i < n; i++) {
              d.t[i] = f[4 + i * (1 + ns)];
              for (let s = 0; s < ns; s++) d.v[i * ns + s] = f[4 + i * (1 + ns) + 1 + s];
            }
            data = d;
            draw();
          }
        } catch { /* the state's loop says so */ }
        const wait = 250 - (performance.now() - t0);
        await new Promise(res => setTimeout(res, wait > 0 ? wait : 0));
      }
    }
    const stateTick = () => P.api(LIVE + '&key=state').then(showState).catch(showMissing);

    /* ---- the recordings ---- */
    function loadFiles() {
      return P.fsList('/plotter').then(list => {
        const csv = list.filter(f => !f.dir && f.name.endsWith('.csv') && !f.name.startsWith('.')).sort((a, b) => b.mtime - a.mtime);
        put(files, csv.length ? null : h('p', { class: 'note' }, tr('Todavía no hay: el botón de grabar de la app (o Grabar CSV acá) las deja en /plotter de la tarjeta.')),
          ...csv.slice(0, 30).map(f => h('div', { class: 'row' },
            h('div', { class: 'grow' }, f.name, h('span', { class: 'small muted' }, ' ' + P.fmtBytes(f.size))),
            h('a', { class: 'btn', href: P.fsUrl('/plotter/' + f.name, true) }, tr('Bajar')),
            h('button', { class: 'btn red', onclick: () => confirm(tr('¿Borrar %s?', f.name)) && P.fsDelete('/plotter/' + f.name).then(loadFiles) }, '✕'))));
      });
    }

    /* ---- profiles and alarms ---- */
    function inp(obj, key, label, type) {
      return h('label', {}, label, h('input', { class: 'pl-in', value: obj[key] ?? '', type: type || 'text',
        oninput: e => { obj[key] = type === 'number' ? e.target.value : e.target.value; } }));
    }
    function showCfg() {
      if (!cfg) return;
      const save = () => P.fsPut(CFG, writeCfg(cfg)).then(() => { P.toast(tr('Guardado: la app lo toma en un segundo')); }).catch(e => P.toast(e.message, true));
      put(prof,
        h('p', { class: 'note' }, tr('Un perfil es para las líneas que el modo automático no entiende: cada serie es un campo de la línea (cortada en espacios, tabs, comas, punto y coma, «:» y «=»), por una escala más un desplazamiento; el punto decimal puede venir de otro campo, con códigos como 0:0,1:3,2:2,4:1 (código:decimales).')),
        ...cfg.profiles.map((p, pi) => h('div', { class: 'pl-prof' },
          h('div', { class: 'pl-grid' }, inp(p, 'name', tr('Nombre')), inp(p, 'match', tr('Sólo líneas con')),
            h('label', {}, tr('Coma decimal'), h('input', { type: 'checkbox', checked: p.comma, onchange: e => { p.comma = e.target.checked; } }))),
          ...p.fields.map((f, fi) => h('div', { class: 'pl-grid' },
            inp(f, 'field', tr('Campo'), 'number'), inp(f, 'name', tr('Serie')), inp(f, 'unit', tr('Unidad')),
            inp(f, 'scale', tr('Multiplicar por'), 'number'), inp(f, 'offset', tr('Sumar'), 'number'),
            inp(f, 'dp', tr('Punto según el campo (0: no)'), 'number'), inp(f, 'map', tr('Códigos')),
            h('label', {}, ' ', h('button', { class: 'btn red', onclick: () => { p.fields.splice(fi, 1); showCfg(); } }, tr('Quitar la serie'))))),
          h('div', { class: 'btns' },
            h('button', { class: 'btn', onclick: () => { p.fields.push({ field: (p.fields.at(-1)?.field || 0) + 1, name: '', unit: '', scale: 1, offset: 0, dp: 0, map: '' }); showCfg(); } }, tr('Agregar una serie')),
            h('button', { class: 'btn red', onclick: () => { cfg.profiles.splice(pi, 1); showCfg(); } }, tr('Borrar el perfil'))))),
        h('div', { class: 'btns' },
          h('button', { class: 'btn', onclick: () => { cfg.profiles.push({ name: tr('Perfil %d', cfg.profiles.length + 1), match: '', comma: false, fields: [{ field: 1, name: '', unit: '', scale: 1, offset: 0, dp: 0, map: '' }] }); showCfg(); } }, tr('Perfil nuevo'))),
        h('h3', {}, tr('Avisos')),
        h('p', { class: 'note' }, tr('Por el nombre de la serie: un aviso en la placa (y un pitido, si se pide) cuando el valor sale de los límites. Vacío: sin límite.')),
        ...cfg.alarms.map((a, ai) => h('div', { class: 'pl-grid' }, inp(a, 'name', tr('Serie')), inp(a, 'lo', tr('Mínimo'), 'number'), inp(a, 'hi', tr('Máximo'), 'number'),
          h('label', {}, tr('Pitido'), h('input', { type: 'checkbox', checked: a.beep, onchange: e => { a.beep = e.target.checked; } })),
          h('label', {}, ' ', h('button', { class: 'btn red', onclick: () => { cfg.alarms.splice(ai, 1); showCfg(); } }, '✕')))),
        h('div', { class: 'btns' },
          h('button', { class: 'btn', onclick: () => { cfg.alarms.push({ name: (state?.series?.[0]?.name) || '', lo: '', hi: '', beep: true }); showCfg(); } }, tr('Agregar un aviso')),
          h('button', { class: 'btn pri', onclick: save }, tr('Guardar'))));
    }
    P.fsText(CFG).then(t => { cfg = parseCfg(t); showCfg(); }).catch(e => put(prof, h('p', { class: 'note' }, e.message)));

    showMissing();
    stateTick();
    dataLoop();
    loadFiles().catch(() => {});
    const t1 = setInterval(stateTick, 1000), t2 = setInterval(() => loadFiles().catch(() => {}), 15000);
    const onResize = () => draw();
    window.addEventListener('resize', onResize);
    return () => { alive = false; clearInterval(t1); clearInterval(t2); window.removeEventListener('resize', onResize); };
  },
});
