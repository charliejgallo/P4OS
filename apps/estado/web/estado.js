/* Estado de la casa's page in the board's portal (apps/estado,
 * docs/PORTAL-PAGES.md).
 *
 * Three parts:
 *   - now: what the app on the board publishes every second through the
 *     live channel (/api/live, key "state"): each service with its traffic
 *     light, latency, the last six hours as a sparkline, the availability of
 *     the day and the week, and its certificate. "Ver más" asks the board
 *     for that service's whole day, week and outages (key "detail"). The
 *     buttons go back the same way: "check 3", "pause 3 1", "trust 3".
 *   - the services: /data/estado.txt (apps/estado/main/ec_cfg.c), edited by
 *     fields, put in order and written back whole; the app sees the file's
 *     date change within two seconds ("reload" makes it look at once).
 *   - the history: /data/estado_eventos.txt, a line per change, newest
 *     first. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const { h, put, toast, fsText, fsPut, fsMkdir, openApp } = P;

const APP = 'aos.estado';
const FILE = '/data/estado.txt';
const LOG = '/data/estado_eventos.txt';
const LOG_OLD = '/data/estado_eventos.old.txt';

const TMPL = [
  { id: 'ha', label: 'Home Assistant', kind: 'http', scheme: 'http', port: 8123, path: '/manifest.json', expect: '200', word: 'Home Assistant', group: N_('Casa'), every: 60, slow: 800 },
  { id: 'frigate', label: 'Frigate', kind: 'http', scheme: 'http', port: 5000, path: '/api/config', expect: '200', word: 'cameras', group: N_('Cámaras'), every: 60, slow: 800 },
  { id: 'nas', label: N_('NAS'), kind: 'tcp', port: 445, group: N_('Red'), every: 60, slow: 200 },
  { id: 'printer', label: N_('Impresora 3D'), kind: 'http', scheme: 'http', port: 80, path: '/', expect: '200-399', word: '', group: N_('Taller'), every: 60, slow: 1000 },
  { id: 'router', label: N_('Router'), kind: 'http', scheme: 'http', port: 80, path: '/', expect: '200-499', word: '', group: N_('Red'), every: 30, slow: 300 },
  { id: 'web', label: N_('Página web'), kind: 'http', scheme: 'https', port: 443, path: '/', expect: '200-399', word: '', group: N_('Internet'), every: 300, slow: 1500 },
  { id: 'tcp', label: N_('Puerto TCP'), kind: 'tcp', port: 22, group: N_('Red'), every: 60, slow: 200 },
];
const TMPL_BY = Object.fromEntries(TMPL.map(t => [t.id, t]));
const EVERY = [10, 30, 60, 120, 300, 600, 900, 1800, 3600];

const STATES = [N_('Probando'), N_('Anda'), N_('Lento'), N_('Caído'), N_('En pausa')];
const STATE_COLOR = ['var(--dim)', 'var(--green)', '#e0b000', 'var(--red)', 'var(--dim)'];
const ERRS = {
  dns: N_('No se encuentra el nombre'), refused: N_('Rechaza la conexión'), timeout: N_('No contesta'),
  tls: N_('Certificado no válido'), clock: N_('La placa todavía no tiene hora'), code: N_('Contestó otro código'),
  word: N_('Falta la palabra'), proto: N_('No contesta HTTP'), closed: N_('Cortó la conexión'),
  mem: N_('Sin memoria'), url: N_('La dirección no sirve'),
};

const CSS = `
.ecsum{display:flex;gap:16px;flex-wrap:wrap;align-items:center;margin:4px 0 12px}
.ecsum b{font-variant-numeric:tabular-nums}
.ecdot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px;vertical-align:middle}
.ecgroup{color:var(--dim);font-size:.9em;margin:16px 0 6px}
.ecgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(270px,1fr));gap:12px}
.eccard{background:var(--card);border-radius:14px;padding:12px 14px;display:flex;flex-direction:column;gap:6px;min-width:0;border:2px solid transparent}
.eccard.down{border-color:var(--red)}
.echead{display:flex;align-items:center;gap:6px;min-width:0}
.ecname{font-weight:600;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ecbig{font-size:1.6em;font-variant-numeric:tabular-nums;line-height:1.1}
.ecsub{color:var(--dim);font-size:.85em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ecnums{display:flex;gap:14px;font-size:.85em;color:var(--dim);flex-wrap:wrap}
.ecnums b{color:var(--fg,inherit);font-weight:600;font-variant-numeric:tabular-nums}
.ecspark{width:100%;height:38px;display:block}
.ecbtns{display:flex;flex-wrap:wrap;gap:6px}
.ecbtns .btn{padding:5px 10px}
.eccert{font-size:.85em}
.ecmore{grid-column:1/-1;background:var(--card);border-radius:14px;padding:14px;display:flex;flex-direction:column;gap:10px}
.ecchart{width:100%;height:180px;display:block}
.ecweek{display:grid;grid-template-columns:repeat(7,1fr);gap:6px}
.ecday{background:var(--card2);border-radius:8px;padding:6px;text-align:center;font-size:.8em}
.ecday b{display:block;font-size:1.1em;font-variant-numeric:tabular-nums}
.ecouts{display:flex;flex-direction:column;gap:4px;font-size:.9em}
.ecouts .open{color:var(--red)}
.ecopts{display:flex;gap:18px;flex-wrap:wrap;align-items:center;margin-bottom:10px}
.ecopts label{display:flex;gap:6px;align-items:center;white-space:nowrap}
.eced{background:var(--card);border-radius:14px;padding:12px 14px;margin-bottom:10px}
.eced .ecf{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:8px 12px;margin-top:8px}
.eced label{display:flex;flex-direction:column;font-size:.85em;color:var(--dim);gap:3px}
.eced input,.eced select{font-size:1rem;min-width:0}
.eced .wide{grid-column:1/-1}
.ecedh{display:flex;gap:6px;align-items:center}
.ecedh .btn{padding:4px 9px}
.eclog{width:100%;border-collapse:collapse;font-size:.9em}
.eclog td{padding:5px 8px;border-top:1px solid var(--line);vertical-align:top}
.eclog td:first-child{white-space:nowrap;color:var(--dim);font-variant-numeric:tabular-nums}
.ectabs{display:flex;gap:6px;margin:6px 0 14px;flex-wrap:wrap}
`;

function style() {
  if (document.getElementById('css-estado')) return;
  document.head.appendChild(h('style', { id: 'css-estado' }, CSS));
}

/* ---- estado.txt -------------------------------------------------------------------- */

function blank() {
  return { name: '', group: '', tmpl: 'tcp', type: 'http', url: '', host: '', port: '', expect: '200-399', word: '',
    every: 60, slow: 800, timeout: 5000, cert: 0, paused: false, trust: false };
}

function parse(text) {
  const opts = { fails: 2, sound: true, boot: true };
  const out = [];
  let cur = null;
  for (const raw of (text || '').split('\n')) {
    const s = raw.trim();
    if (!s || s[0] === '#' || s[0] === ';') continue;
    if (s[0] === '[') {
      const e = s.lastIndexOf(']');
      cur = blank();
      cur.name = s.slice(1, e > 0 ? e : undefined).trim();
      out.push(cur);
      continue;
    }
    const eq = s.indexOf('=');
    if (eq < 0) continue;
    const k = s.slice(0, eq).trim(), v = s.slice(eq + 1).trim();
    if (!cur) {
      if (k === 'fails') opts.fails = Math.max(1, Math.min(5, +v || 2));
      else if (k === 'sound' || k === 'boot') opts[k] = v !== '0';
      continue;
    }
    if (k === 'paused' || k === 'trust') cur[k] = v !== '0';
    else if (k in cur) cur[k] = v;
  }
  return { opts, list: out };
}

function serialize(opts, list) {
  let t = '# Home status for P4OS (apps/estado). The options, then one block per service:\n' +
    '# [Name] group, tmpl, type (http or tcp), url (http) or host and port (tcp),\n' +
    '# expect (codes: 200, 200-399, 200,401), word (must be in the answer), every (s),\n' +
    '# slow (ms), timeout (ms), cert (days of warning), paused, trust.\n';
  t += `fails = ${opts.fails}\nsound = ${opts.sound ? 1 : 0}\nboot = ${opts.boot ? 1 : 0}\n`;
  for (const m of list) {
    t += `\n[${m.name}]\n`;
    if (m.group) t += `group = ${m.group}\n`;
    t += `tmpl = ${m.tmpl}\n`;
    if (m.type === 'tcp') {
      t += `type = tcp\nhost = ${m.host}\nport = ${m.port}\n`;
    } else {
      t += `type = http\nurl = ${m.url}\n`;
      if (m.expect) t += `expect = ${m.expect}\n`;
      if (m.word) t += `word = ${m.word}\n`;
      t += `cert = ${+m.cert || 0}\n`;
      if (m.trust) t += 'trust = 1\n';
    }
    t += `every = ${+m.every || 60}\nslow = ${+m.slow || 800}\ntimeout = ${+m.timeout || 5000}\n`;
    if (m.paused) t += 'paused = 1\n';
  }
  return t;
}

function fromTmpl(id, host) {
  const t = TMPL_BY[id] || TMPL_BY.tcp;
  const m = blank();
  m.tmpl = t.id;
  m.type = t.kind;
  m.name = tr(t.label);
  m.group = tr(t.group);
  const hst = host || '';                 /* never a made-up address: it would be tried */
  if (t.kind === 'http') {
    const def = t.port === (t.scheme === 'https' ? 443 : 80);
    m.url = `${t.scheme}://${hst}${def ? '' : ':' + t.port}${t.path}`;
    m.expect = t.expect;
    m.word = t.word;
    m.cert = t.scheme === 'https' ? 14 : 0;
  } else {
    m.host = hst;
    m.port = String(t.port);
  }
  m.every = t.every;
  m.slow = t.slow;
  return m;
}

function check(list) {
  const seen = new Set();
  for (const [i, m] of list.entries()) {
    const n = m.name || tr('servicio %d', i + 1);
    if (!m.name || m.name.length > 39 || /[\[\]]/.test(m.name)) return tr('%s: el nombre, de 1 a 39 caracteres y sin corchetes', n);
    if (seen.has(m.name.toLowerCase())) return tr('%s: hay dos con ese nombre', m.name);
    seen.add(m.name.toLowerCase());
    if (m.group.length > 23) return tr('%s: el grupo, de 23 caracteres como mucho', m.name);
    if (m.type === 'tcp') {
      if (!m.host || /[\s/]/.test(m.host)) return tr('%s: falta la dirección (IP o nombre, sin espacios)', m.name);
      if (!(/^\d+$/.test(m.port) && +m.port > 0 && +m.port < 65536)) return tr('%s: el puerto va de 1 a 65535', m.name);
    } else {
      if (/^https?:\/\/(:|\/|$)/i.test(m.url)) return tr('%s: falta la IP o el nombre en la dirección', m.name);
      if (!/^https?:\/\/[^\s/]+/i.test(m.url)) return tr('%s: la dirección tiene que empezar con http:// o https://', m.name);
      if (!/^\s*\d{3}(\s*-\s*\d{3})?(\s*,\s*\d{3}(\s*-\s*\d{3})?)*\s*$/.test(m.expect)) return tr('%s: los códigos que valen, como 200, 200-399 o 200,401', m.name);
    }
  }
  return null;
}

/* ---- little helpers ---------------------------------------------------------------- */

const num = (v, d) => (P.num ? P.num(v, d) : v.toFixed(d));

function ms(v) {
  if (v == null || v < 0) return '--';
  if (v === 0) return '<1 ms';
  if (v < 10000) return v + ' ms';
  return num(v / 1000, 1) + ' s';
}

function pct(p) {
  if (p == null || p < 0) return '--';
  const t = Math.floor(p * 100);
  if (t >= 10000) return '100 %';
  return num(t / 100, t >= 9900 ? 2 : 1) + ' %';
}

function dur(s) {
  if (s == null || s < 0) return '--';
  const d = Math.floor(s / 86400), hh = Math.floor(s % 86400 / 3600), mm = Math.floor(s % 3600 / 60);
  if (d) return tr('%d d %d h', d, hh);
  if (hh) return tr('%d h %d min', hh, mm);
  if (mm) return tr('%d min', mm);
  return tr('%d s', s);
}

function ago(s) {
  if (s == null || s < 0) return '';
  if (s < 10) return tr('recién');
  return tr('hace %s', dur(s));
}

function when(epoch) {
  return new Date(epoch * 1000).toLocaleString(P.lang ? P.lang() : 'es', { weekday: 'short', day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
}

function errText(s) {
  if (s.err === 'code') return tr('Contestó %d', s.code);
  return ERRS[s.err] ? tr(ERRS[s.err]) : '';
}

async function send(line) {
  const r = await fetch('/api/live?app=' + APP, { method: 'POST', body: line });
  if (!r.ok) throw new Error(tr('La placa no tomó la orden (%d)', r.status));
}

const SVGNS = 'http://www.w3.org/2000/svg';
function svg(tag, attrs, ...kids) {
  const e = document.createElementNS(SVGNS, tag);
  for (const [k, v] of Object.entries(attrs || {})) e.setAttribute(k, v);
  for (const k of kids) if (k) e.appendChild(k);
  return e;
}

/* a line of latencies over n slots: null or <0 is a hole; down slots (-2, or ok == 0) a red tick */
function lineChart(vals, down, w, hgt, cls) {
  const top = Math.max(10, ...vals.filter(v => v != null && v >= 0)) * 1.15;
  const n = vals.length;
  const x = i => (n > 1 ? i * w / (n - 1) : 0);
  const y = v => hgt - 4 - (v / top) * (hgt - 8);
  let d = '', pen = false;
  vals.forEach((v, i) => {
    if (v == null || v < 0) { pen = false; return; }
    d += (pen ? 'L' : 'M') + x(i).toFixed(1) + ',' + y(v).toFixed(1);
    pen = true;
  });
  const g = svg('svg', { class: cls, viewBox: `0 0 ${w} ${hgt}`, preserveAspectRatio: 'none' });
  vals.forEach((v, i) => {
    if (down[i]) g.appendChild(svg('rect', { x: Math.max(0, x(i) - w / n / 2), y: 0, width: Math.max(2, w / n), height: hgt, fill: 'var(--red)', opacity: '0.35' }));
  });
  g.appendChild(svg('path', { d, fill: 'none', stroke: 'var(--accent)', 'stroke-width': '2', 'vector-effect': 'non-scaling-stroke' }));
  return { el: g, top };
}

/* ---- the live part ----------------------------------------------------------------- */

function shape(s) {
  return [s.state, !!s.cert, s.paused, s.cert && s.cert.can_trust, s.err === 'tls'].join('|');
}

function liveCard(s, onMore) {
  const go = (line, ok) => send(line).then(() => ok && toast(ok)).catch(e => toast(e.message, true));
  const r = {
    dot: h('span', { class: 'ecdot' }), big: h('div', { class: 'ecbig' }), sub: h('div', { class: 'ecsub' }),
    nums: h('div', { class: 'ecnums' }), spark: h('div', {}), cert: h('div', { class: 'eccert' }),
  };
  const btns = [
    h('button', { class: 'btn', onclick: () => go('check ' + s.i, tr('Probando %s', s.name)) }, tr('Probar')),
    h('button', { class: 'btn', onclick: () => go(`pause ${s.i} ${s.paused ? 0 : 1}`) }, s.paused ? tr('Seguir') : tr('Pausar')),
    h('button', { class: 'btn', onclick: onMore }, tr('Ver más')),
  ];
  if (s.err === 'tls' && s.cert && s.cert.can_trust) {
    btns.push(h('button', { class: 'btn pri', onclick: () => {
      if (confirm(tr('¿Confiar en el certificado que muestra %s? Desde ahora sólo vale ese: si el servidor lo cambia, va a salir caído hasta que confíes en el nuevo.', s.name))) go('trust ' + s.i, tr('Listo: se confía en este certificado'));
    } }, tr('Confiar en este certificado')));
  }
  const el = h('div', { class: 'eccard' + (s.state === 3 ? ' down' : '') },
    h('div', { class: 'echead' }, r.dot, h('div', { class: 'ecname', title: s.addr }, s.name)),
    r.big, r.sub, r.spark, r.nums, r.cert, h('div', { class: 'ecbtns' }, btns));
  el._r = r;
  el._shape = shape(s);
  fill(el, s);
  return el;
}

function fill(el, s) {
  const r = el._r;
  r.dot.style.background = STATE_COLOR[s.state];
  if (s.state === 3) r.big.textContent = tr('Caído');
  else if (s.state === 1 || s.state === 2) r.big.textContent = ms(s.ms);
  else r.big.textContent = tr(STATES[s.state] || STATES[0]);
  r.big.style.color = s.state === 1 ? '' : STATE_COLOR[s.state];
  let sub = '';
  if (s.state === 3) sub = errText(s) + ' · ' + ago(s.since);
  else if (s.state === 1 || s.state === 2) sub = s.fails ? (s.fails === 1 ? tr('falló una vez, probando de nuevo') : tr('falló %d veces, probando de nuevo', s.fails)) : tr('anda %s', ago(s.since));
  else sub = s.addr;
  r.sub.textContent = sub;
  r.sub.title = s.why || s.addr;
  put(r.nums, h('span', {}, tr('24 h'), ' ', h('b', {}, pct(s.a24))), h('span', {}, tr('7 días'), ' ', h('b', {}, pct(s.a7))),
    s.conn >= 0 ? h('span', {}, tr('conectar'), ' ', h('b', {}, ms(s.conn))) : null);
  const vals = s.spark.map(v => (v >= 0 ? v : null));
  put(r.spark, lineChart(vals, s.spark.map(v => v === -2), 300, 38, 'ecspark').el);
  if (s.cert && s.cert.state >= 1 && s.cert.state <= 3) {
    const c = s.cert;
    r.cert.textContent = c.state === 3 ? tr('Certificado de %s: venció', c.name) : tr('Certificado de %s: %d días', c.name, c.days);
    r.cert.style.color = c.state === 1 ? 'var(--dim)' : c.state === 2 ? 'var(--orange)' : 'var(--red)';
  } else if (s.cert) {
    r.cert.textContent = tr('No se pudo leer el certificado');
    r.cert.style.color = 'var(--dim)';
  } else {
    r.cert.textContent = '';
  }
}

function moreBox(s, d) {
  const box = h('div', { class: 'ecmore' });
  const title = h('div', { class: 'row' }, h('b', { class: 'grow' }, tr('%s: las últimas 24 horas', s.name)),
    h('span', { class: 'muted small' }, s.addr));
  if (!d) return put(box, title, h('p', { class: 'note' }, tr('Pidiendo el historial a la placa...'))), box;
  const vals = d.b5.map(b => (b && b[1] ? b[2] : null));
  const down = d.b5.map(b => !!(b && b[0] && !b[1]));
  const ch = lineChart(vals, down, 800, 180, 'ecchart');
  const days = [];
  for (let k = 0; k < 7; k++) {
    let n = 0, ok = 0;
    for (const b of d.h.slice(k * 24, k * 24 + 24)) if (b) { n += b[0]; ok += b[1]; }
    const start = (Math.floor(d.now / 3600) - 167 + k * 24) * 3600;
    const label = new Date(start * 1000 + 12 * 3600 * 1000).toLocaleDateString(P.lang ? P.lang() : 'es', { weekday: 'short' });
    const p = n ? ok * 100 / n : -1;
    days.push(h('div', { class: 'ecday' }, label, h('b', { style: p < 0 ? '' : `color:${p >= 99.5 ? 'var(--green)' : p >= 95 ? '#e0b000' : 'var(--red)'}` }, pct(p))));
  }
  const outs = d.out.length
    ? d.out.map(([a, b, w]) => h('div', { class: b ? '' : 'open' },
      b ? tr('%s, %s: %s', when(a), dur(b - a), tr(ERRS[w] || '')) : tr('%s: sigue caído (%s)', when(a), tr(ERRS[w] || ''))))
    : [h('div', { class: 'muted' }, tr('Sin cortes registrados'))];
  return put(box, title,
    h('div', { class: 'muted small' }, tr('Latencia de cada cinco minutos (arriba: %s); en rojo, los que no contestó', ms(Math.round(ch.top)))),
    ch.el,
    h('b', {}, tr('Disponibilidad por día')), h('div', { class: 'ecweek' }, days),
    h('b', {}, tr('Últimos cortes')), h('div', { class: 'ecouts' }, outs)), box;
}

/* ---- the editor -------------------------------------------------------------------- */

function editor(state, onChange) {
  const box = h('div', {});
  const draw = () => {
    const { list } = state;
    put(box, list.map((m, i) => {
      const field = (key, label, attrs = {}, cls) => h('label', { class: cls || null }, tr(label),
        h('input', { value: m[key] == null ? '' : m[key], ...attrs, oninput: e => { m[key] = e.target.value.trim(); onChange(); } }));
      const move = d => { const j = i + d; if (j < 0 || j >= list.length) return; [list[i], list[j]] = [list[j], list[i]]; onChange(); draw(); };
      const kind = h('select', { onchange: e => {
        m.type = e.target.value;
        if (m.type === 'tcp' && !m.host) { const u = /^https?:\/\/([^/:]+)/i.exec(m.url); m.host = u ? u[1] : ''; m.port = m.port || '22'; }
        if (m.type === 'http' && !m.url) m.url = `http://${m.host || ''}/`;
        onChange(); draw();
      } }, h('option', { value: 'http', selected: m.type !== 'tcp' ? '' : null }, tr('Pedido HTTP(S)')),
      h('option', { value: 'tcp', selected: m.type === 'tcp' ? '' : null }, tr('Puerto TCP abierto')));
      const every = h('select', { onchange: e => { m.every = +e.target.value; onChange(); } },
        EVERY.concat(EVERY.includes(+m.every) ? [] : [+m.every]).sort((a, b) => a - b)
          .map(v => h('option', { value: v, selected: +m.every === v ? '' : null }, dur(v))));
      const tsel = h('select', { onchange: e => { m.tmpl = e.target.value; onChange(); } },
        TMPL.map(t => h('option', { value: t.id, selected: t.id === m.tmpl ? '' : null }, tr(t.label))));
      return h('div', { class: 'eced' },
        h('div', { class: 'ecedh' }, h('b', { class: 'grow' }, m.name || tr('Sin nombre')),
          h('button', { class: 'btn', title: tr('Más arriba'), onclick: () => move(-1), disabled: i ? null : '' }, '↑'),
          h('button', { class: 'btn', title: tr('Más abajo'), onclick: () => move(1), disabled: i < list.length - 1 ? null : '' }, '↓'),
          h('button', { class: 'btn red', onclick: () => { if (confirm(tr('¿Sacar %s de la lista?', m.name || tr('este servicio')))) { list.splice(i, 1); onChange(); draw(); } } }, tr('Borrar'))),
        h('div', { class: 'ecf' },
          field('name', N_('Nombre'), { maxlength: 39 }),
          field('group', N_('Grupo'), { maxlength: 23, placeholder: tr('Casa, Red, Cámaras...') }),
          h('label', {}, tr('Ícono'), tsel),
          h('label', {}, tr('Cómo se prueba'), kind),
          m.type === 'tcp'
            ? [field('host', N_('Dirección (IP o nombre)'), { placeholder: '10.20.30.40' }),
              field('port', N_('Puerto'), { inputmode: 'numeric' })]
            : [field('url', N_('Dirección'), { placeholder: 'http://10.20.30.40:8123/' }, 'wide'),
              field('expect', N_('Códigos que valen'), { placeholder: '200-399' }),
              field('word', N_('Palabra que tiene que estar (opcional)')),
              field('cert', N_('Avisar si al certificado le quedan menos días (0: no)'), { inputmode: 'numeric' })],
          h('label', {}, tr('Probar cada'), every),
          field('slow', N_('Lento desde (ms)'), { inputmode: 'numeric' }),
          field('timeout', N_('Esperar hasta (ms)'), { inputmode: 'numeric' }),
          h('label', { style: 'flex-direction:row;align-items:center;gap:6px' },
            h('input', { type: 'checkbox', checked: m.paused ? '' : null, onchange: e => { m.paused = e.target.checked; onChange(); } }), tr('En pausa'))));
    }));
  };
  draw();
  return { box, draw };
}

/* ---- the history ------------------------------------------------------------------- */

function historyTable(text, only) {
  const rows = (text || '').split('\n').filter(Boolean).map(l => l.split('\t')).filter(c => c.length >= 4);
  const pick = only ? rows.filter(c => c[1] === only) : rows;
  const out = pick.reverse().slice(0, 400).map(([t, name, what, detail, msv]) => {
    let txt = '', col = '';
    if (what === 'down') {
      txt = /^\d+$/.test(detail) ? tr('se cayó: contestó %s', detail) : tr('se cayó: %s', tr(ERRS[detail] || detail));
      col = 'var(--red)';
    } else if (what === 'up') {
      txt = tr('volvió después de %s', dur(+detail));
      col = 'var(--green)';
      if (+msv >= 0) txt += ' (' + ms(+msv) + ')';
    } else if (what === 'cert') {
      txt = +detail < 0 ? tr('su certificado venció') : tr('a su certificado le quedan %s días', detail);
      col = 'var(--orange)';
    } else {
      txt = what + ' ' + detail;
    }
    return h('tr', {}, h('td', {}, t), h('td', {}, name), h('td', { style: `color:${col}` }, txt));
  });
  return out.length ? h('table', { class: 'eclog' }, out) : h('p', { class: 'note' }, tr('Nada todavía: acá aparece cada vez que algo se cae o vuelve.'));
}

/* ---- the page ---------------------------------------------------------------------- */

P.registerPage({
  id: 'estado',
  name: tr('Estado de la casa'),
  icon: '◉',
  render(main) {
    style();
    const st = { opts: { fails: 2, sound: true, boot: true }, list: [] };
    let dirty = false, timer = 0, gone = false, last = null, more = -1, moreData = null, tab = 'now';
    const status = h('p', { class: 'note' }, tr('Leyendo...'));
    const summary = h('div', { class: 'ecsum' });
    const live = h('div', {});
    const saveBtn = h('button', { class: 'btn pri', disabled: '' }, tr('Guardar en la tarjeta'));
    const changed = () => { dirty = true; saveBtn.removeAttribute('disabled'); };
    const ed = editor(st, changed);
    const cards = new Map();

    const drawLive = () => {
      if (!last) { put(live); cards.clear(); return; }
      const ss = last.services;
      const n = [1, 2, 3].map(k => ss.filter(s => s.state === k).length);
      put(summary,
        h('span', {}, h('span', { class: 'ecdot', style: 'background:var(--green)' }), h('b', {}, n[0]), ' ', n[0] === 1 ? tr('anda') : tr('andan')),
        n[1] ? h('span', {}, h('span', { class: 'ecdot', style: 'background:#e0b000' }), h('b', {}, n[1]), ' ', n[1] === 1 ? tr('lento') : tr('lentos')) : null,
        n[2] ? h('span', {}, h('span', { class: 'ecdot', style: 'background:var(--red)' }), h('b', {}, n[2]), ' ', n[2] === 1 ? tr('caído') : tr('caídos')) : null,
        !last.net ? h('span', { style: 'color:var(--orange)' }, tr('La placa no tiene red: los chequeos esperan.')) : null,
        h('span', { class: 'grow' }),
        h('button', { class: 'btn', onclick: () => send('check all').then(() => toast(tr('Probando todos'))).catch(e => toast(e.message, true)) }, tr('Probar todos')));
      const groups = [];
      for (const s of ss) {
        let g = groups.find(x => x.name === s.group);
        if (!g) groups.push(g = { name: s.group, items: [] });
        g.items.push(s);
      }
      const nodes = [];
      const seen = new Set();
      for (const g of groups) {
        nodes.push(h('div', { class: 'ecgroup' }, g.name || tr('Sin grupo')));
        const grid = h('div', { class: 'ecgrid' });
        for (const s of g.items) {
          const key = s.i + '|' + s.name;
          seen.add(key);
          let el = cards.get(key);
          if (el && el._shape === shape(s)) fill(el, s);
          else {
            el = liveCard(s, () => { more = more === s.i ? -1 : s.i; moreData = null; if (more >= 0) send('detail ' + more).catch(() => {}); drawLive(); });
            cards.set(key, el);
          }
          grid.appendChild(el);
          if (more === s.i) grid.appendChild(moreBox(s, moreData && moreData.i === s.i ? moreData : null));
        }
        nodes.push(grid);
      }
      for (const k of cards.keys()) if (!seen.has(k)) cards.delete(k);
      put(live, nodes);
    };

    const poll = async () => {
      if (gone) return;
      try {
        const r = await fetch(`/api/live?app=${APP}&key=state`, { cache: 'no-store' });
        if (r.status === 404) {
          last = null;
          put(status, tr('La app Estado de la casa no está abierta en la placa. '),
            h('button', { class: 'btn', onclick: () => openApp(APP).catch(e => toast(e.message, true)) }, tr('Abrirla')));
          put(summary);
        } else if (r.ok) {
          last = await r.json();
          put(status, last.services.length ? '' : tr('Ningún servicio todavía: agregalos en Servicios.'));
          if (more >= 0) {
            const d = await fetch(`/api/live?app=${APP}&key=detail`, { cache: 'no-store' });
            if (d.ok) moreData = await d.json();
          }
        }
      } catch (e) {
        put(status, tr('No se puede leer el estado: %s', e.message));
      }
      if (tab === 'now') drawLive();
      timer = setTimeout(poll, more >= 0 ? 2000 : 1500);
    };

    saveBtn.onclick = async () => {
      const bad = check(st.list);
      if (bad) return toast(bad, true);
      try {
        await fsMkdir('/data').catch(() => {});
        await fsPut(FILE, serialize(st.opts, st.list));
        send('reload').catch(() => {});
        dirty = false;
        saveBtn.setAttribute('disabled', '');
        toast(tr('Guardado: la placa empieza a probarlos'));
      } catch (e) {
        toast(tr('No se pudo guardar: %s', e.message), true);
      }
    };
    const tmplSel = h('select', {}, TMPL.map(t => h('option', { value: t.id }, tr(t.label))));
    const hostIn = h('input', { placeholder: tr('IP o nombre: 10.20.30.40'), style: 'min-width:0;flex:1' });
    const addBtn = h('button', { class: 'btn', onclick: () => {
      if (st.list.length >= 32) return toast(tr('No entran más de 32 servicios'), true);
      const m = fromTmpl(tmplSel.value, hostIn.value.trim());
      let n = 2;
      const base = m.name;
      while (st.list.some(x => x.name.toLowerCase() === m.name.toLowerCase())) m.name = base + ' ' + n++;
      st.list.push(m);
      hostIn.value = '';
      changed();
      ed.draw();
    } }, tr('+ Agregar'));
    const opt = (key, label) => h('label', {}, h('input', { type: 'checkbox', checked: st.opts[key] ? '' : null,
      onchange: e => { st.opts[key] = e.target.checked; changed(); } }), tr(label));
    const optsBox = h('div', {});
    const drawOpts = () => put(optsBox, h('div', { class: 'ecopts' },
      opt('sound', N_('Pitido con cada aviso')), opt('boot', N_('Vigilar desde que prende la placa')),
      h('label', {}, tr('Fallas seguidas para darlo por caído'),
        h('select', { onchange: e => { st.opts.fails = +e.target.value; changed(); } },
          [1, 2, 3, 4, 5].map(v => h('option', { value: v, selected: st.opts.fails === v ? '' : null }, String(v)))))));

    const histSel = h('select', {});
    const histBox = h('div', {});
    const loadHistory = () => {
      put(histBox, h('p', { class: 'note' }, tr('Leyendo...')));
      Promise.all([fsText(LOG_OLD).catch(() => null), fsText(LOG).catch(() => null)]).then(([a, b]) => {
        const text = (a || '') + (b || '');
        const names = [...new Set(text.split('\n').map(l => l.split('\t')[1]).filter(Boolean))].sort();
        const cur = histSel.value;
        put(histSel, h('option', { value: '' }, tr('Todos')), names.map(n => h('option', { value: n, selected: n === cur ? '' : null }, n)));
        put(histBox, historyTable(text, histSel.value));
      });
    };
    histSel.onchange = loadHistory;

    const panes = {
      now: h('div', {}, status, summary, live),
      edit: h('div', {},
        h('p', { class: 'note' }, tr('Cada servicio se prueba solo: un pedido HTTP(S) que tiene que contestar con uno de los códigos (y, si se pide, tener una palabra), o un puerto TCP que tiene que abrir. Probá sólo tus equipos.')),
        optsBox, ed.box,
        h('div', { class: 'eced' }, h('b', {}, tr('Agregar desde una plantilla')),
          h('div', { class: 'row', style: 'gap:8px;margin-top:8px;flex-wrap:wrap' }, tmplSel, hostIn, addBtn)),
        h('div', { class: 'btns' }, saveBtn)),
      hist: h('div', {}, h('div', { class: 'row', style: 'gap:8px;margin-bottom:10px' }, histSel,
        h('button', { class: 'btn', onclick: loadHistory }, tr('Actualizar')),
        h('a', { class: 'btn', href: P.fsUrl ? P.fsUrl(LOG) : '#', download: 'estado_eventos.txt' }, tr('Bajar'))), histBox),
    };
    const tabs = h('div', { class: 'ectabs' });
    const show = t => {
      tab = t;
      put(tabs, [['now', N_('Ahora')], ['edit', N_('Servicios')], ['hist', N_('Historial')]].map(([k, l]) =>
        h('button', { class: 'btn' + (k === t ? ' pri' : ''), onclick: () => show(k) }, tr(l))));
      put(body, panes[t]);
      if (t === 'hist') loadHistory();
      if (t === 'now') drawLive();
    };
    const body = h('div', {});
    put(main, h('h1', {}, tr('Estado de la casa')), tabs, body);
    show('now');

    fsText(FILE).then(t => {
      const p = parse(t);
      st.opts = p.opts;
      st.list.splice(0, st.list.length, ...p.list);
      drawOpts();
      ed.draw();
    }).catch(e => toast(e.message, true));
    drawOpts();
    poll();
    const leave = e => { if (dirty) { e.preventDefault(); e.returnValue = ''; } };
    window.addEventListener('beforeunload', leave);
    return () => {
      gone = true;
      clearTimeout(timer);
      window.removeEventListener('beforeunload', leave);
    };
  },
});
