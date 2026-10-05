/* The PWM generator's page in the board's portal (apps/pwm, aos.pwm;
 * docs/PORTAL-PAGES.md).
 *
 * The page and the app meet on the card, in /pwm:
 *   _estado.txt  what the app has now: written a moment after each change,
 *                with what the hardware gave (real frequency, bits, the
 *                error of a channel that would not open) and run=1 while
 *                the app is open.
 *   _pedido.txt  what the page wants: the whole set of channels, in the same
 *                format. The app takes it within 300 ms, applies it and
 *                deletes it; ack= in its state says which one it took.
 *   <name>.pwm   the presets, the same lines again.
 * One line per channel of key=value pairs; err= goes last and takes the rest.
 *
 * The knobs turn like the board's: by how much the pointer turns, never to
 * where it is; Shift (or the wheel) goes ten times finer. A pattern is
 * animated here from its parameters, so its phase is not the board's. */
const P = window.P4OS;
const DIR = '/pwm';
const STATE = DIR + '/_estado.txt';
const REQ = DIR + '/_pedido.txt';
const APP = 'aos.pwm';

const MODES = [['pwm', 'PWM'], ['servo', 'Servo'], ['led', 'LED'], ['nivel', 'Nivel']];
const PATS = [['fijo', 'Fijo'], ['respirar', 'Respirar'], ['barrido', 'Barrido'], ['estrobo', 'Estrobo'],
  ['rampa', 'Rampa'], ['seno', 'Seno'], ['pasos', 'Pasos']];
const COLORS = { pwm: '#0a84ff', servo: '#ff9f0a', led: '#ffd60a', nivel: '#40c8e0' };
/* the header's GPIOs an app can have, with their pin, in the connector's order */
const PINS = [[2, 8], [5, 11], [3, 12], [4, 14], [21, 15], [28, 16], [22, 17], [29, 20], [24, 21], [30, 22],
  [25, 23], [31, 24], [34, 28], [32, 31], [49, 32], [50, 34], [46, 35], [51, 36], [47, 37], [52, 38], [48, 39]];
const PIN_OF = Object.fromEntries(PINS);
const FREQ_MAX = 20000000, PER_MIN = 0.05, PER_MAX = 60, MAX_CH = 7;

/* ---------------------------------------------------------------- the format */

const DEF = () => ({ gpio: -1, mode: 'pwm', on: false, inv: false, freq: 1000, duty: 0.5, pulse: 1500, smin: 500,
  smax: 2500, sdeg: 180, bright: 0.5, gamma: 2.2, level: 0.5, pat: 'fijo', period: 2, lo: 0, hi: 1,
  steps: [0, 0.33, 0.66, 1], rfreq: 0, bits: 0, live: 0, err: '' });

function parse(text) {
  const st = { run: false, ack: 0, ch: [] };
  for (const raw of (text || '').split('\n')) {
    const l = raw.trim();
    if (l.startsWith('pwm')) {
      const run = / run=(\d)/.exec(l), ack = / ack=(\d+)/.exec(l);
      if (run) st.run = run[1] === '1';
      if (ack) st.ack = +ack[1];
      continue;
    }
    if (!l.startsWith('ch ')) continue;
    const c = DEF();
    let rest = l.slice(3);
    const e = rest.indexOf(' err=');
    if (e >= 0) { c.err = rest.slice(e + 5); rest = rest.slice(0, e); }
    for (const kv of rest.split(' ')) {
      const i = kv.indexOf('=');
      if (i < 0) continue;
      const k = kv.slice(0, i), v = kv.slice(i + 1);
      if (k === 'mode' || k === 'pat') c[k] = v;
      else if (k === 'on' || k === 'inv') c[k] = v === '1';
      else if (k === 'steps') c.steps = v.split(',').filter(x => x !== '').map(Number);
      else if (k in c) c[k] = Number(v);
    }
    st.ch.push(c);
  }
  return st;
}

const f4 = v => (+v).toFixed(4);
function line(c) {
  return `ch gpio=${c.gpio} mode=${c.mode} on=${c.on ? 1 : 0} inv=${c.inv ? 1 : 0} freq=${Math.round(c.freq)} ` +
    `duty=${f4(c.duty)} pulse=${Math.round(c.pulse)} smin=${c.smin} smax=${c.smax} sdeg=${c.sdeg} ` +
    `bright=${f4(c.bright)} gamma=${(+c.gamma).toFixed(2)} level=${f4(c.level)} pat=${c.pat} ` +
    `period=${(+c.period).toFixed(3)} lo=${f4(c.lo)} hi=${f4(c.hi)} steps=${c.steps.map(s => (+s).toFixed(3)).join(',')}`;
}

/* ---------------------------------------------------------------- numbers */

const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const num = (v, d) => (+v).toFixed(d).replace('.', ',');
function sig(v, unit) { const a = Math.abs(v); return num(v, a >= 100 ? 0 : a >= 10 ? 1 : 2) + ' ' + unit; }
function fmtFreq(hz) { return hz >= 1e6 ? sig(hz / 1e6, 'MHz') : hz >= 1e3 ? sig(hz / 1e3, 'kHz') : Math.round(hz) + ' Hz'; }
function fmtTime(s) { return s >= 1 ? sig(s, 's') : s >= 1e-3 ? sig(s * 1e3, 'ms') : s >= 1e-6 ? sig(s * 1e6, 'µs') : sig(s * 1e9, 'ns'); }
function niceFreq(hz) {
  if (!(hz >= 1)) return 1;
  if (hz >= FREQ_MAX) return FREQ_MAX;
  if (hz < 1000) return Math.round(hz);
  const q = Math.pow(10, Math.floor(Math.log10(hz)) - 2);
  return Math.round(Math.round(hz / q) * q);
}
const hasFreq = m => m === 'pwm' || m === 'led';

function val(c) {
  switch (c.mode) {
    case 'pwm': return c.duty;
    case 'servo': return c.smax > c.smin ? clamp((c.pulse - c.smin) / (c.smax - c.smin), 0, 1) : 0;
    case 'led': return c.bright;
    default: return c.level;
  }
}
function setVal(c, v) {
  v = clamp(v, 0, 1);
  if (c.mode === 'pwm') c.duty = v;
  else if (c.mode === 'servo') c.pulse = Math.round(c.smin + v * (c.smax - c.smin));
  else if (c.mode === 'led') c.bright = v;
  else c.level = v;
}
function duty(c, v) {
  if (c.mode === 'servo') return clamp((c.smin + v * (c.smax - c.smin)) * 50 / 1e6, 0, 1);
  if (c.mode === 'led') return Math.pow(v, c.gamma);
  return v;
}
function fmtVal(c, v) {
  switch (c.mode) {
    case 'pwm': return [num(v * 100, 1) + ' %', fmtFreq(c.rfreq || c.freq) + (c.bits ? ` · ${c.bits} bits` : '')];
    case 'servo': return [Math.round(v * c.sdeg) + '°', Math.round(c.smin + v * (c.smax - c.smin)) + ' µs'];
    case 'led': return [num(v * 100, v < 0.1 ? 1 : 0) + ' %', 'ciclo ' + num(Math.pow(v, c.gamma) * 100, 1) + ' %'];
    default: return [num(v * 3.3, 2) + ' V', Math.round(v * 255) + ' de 255'];
  }
}
function patAt(c, ph) {
  let s;
  switch (c.pat) {
    case 'respirar': { const e = Math.exp(Math.sin(2 * Math.PI * ph - Math.PI / 2)); s = (e - 1 / Math.E) / (Math.E - 1 / Math.E); break; }
    case 'barrido': s = ph < 0.5 ? 2 * ph : 2 - 2 * ph; break;
    case 'estrobo': s = ph < 0.1 ? 1 : 0; break;
    case 'rampa': s = ph; break;
    case 'seno': s = 0.5 - 0.5 * Math.cos(2 * Math.PI * ph); break;
    case 'pasos': { const n = Math.max(1, c.steps.length); return clamp(c.steps[Math.min(n - 1, Math.floor(ph * n))] ?? 0, 0, 1); }
    default: return val(c);
  }
  return clamp(c.lo + (c.hi - c.lo) * s, 0, 1);
}
const live = c => c.pat === 'fijo' ? val(c) : patAt(c, (Date.now() / 1000 / c.period) % 1);

/* ---------------------------------------------------------------- styles */

function css() {
  if (document.getElementById('css-pwm')) return;
  document.head.append(P.h('style', { id: 'css-pwm' }, `
.pwg { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 16px; }
.pwc { padding: 16px 18px; display: flex; flex-direction: column; gap: 10px; border: 2px solid transparent; }
.pwc.on { border-color: var(--pwc); }
.pwhd { display: flex; align-items: center; gap: 10px; }
.pwhd b { font-size: 20px; }
.pwhd .dot { background: var(--card2); } .pwhd .dot.on { background: var(--green); } .pwhd .dot.bad { background: var(--red); }
.pwhd .grow { flex: 1; min-width: 0; }
.pwrow select { flex: 1; min-width: 0; padding: 6px 8px; }
.pwper .small { min-width: 64px; text-align: right; white-space: nowrap; }
.pwsw { position: relative; width: 52px; height: 30px; flex: none; }
.pwsw input { opacity: 0; width: 0; height: 0; }
.pwsw span { position: absolute; inset: 0; border-radius: 15px; background: var(--card2); cursor: pointer; transition: .15s; }
.pwsw span::after { content: ''; position: absolute; width: 24px; height: 24px; left: 3px; top: 3px; border-radius: 50%; background: #fff; transition: .15s; }
.pwsw input:checked + span { background: var(--green); }
.pwsw input:checked + span::after { left: 25px; }
.pwseg { display: flex; background: var(--card2); border-radius: 10px; padding: 3px; gap: 3px; }
.pwseg button { flex: 1; border: 0; background: none; padding: 6px 4px; border-radius: 8px; cursor: pointer; font-size: 14px; }
.pwseg button.on { background: var(--pwc); color: #000; font-weight: 600; }
.pwmid { display: flex; align-items: center; gap: 14px; }
.pwknob { width: 150px; height: 150px; flex: none; touch-action: none; cursor: grab; user-select: none; }
.pwknob.off { opacity: .55; }
.pwknob text { fill: var(--text); text-anchor: middle; font-family: inherit; }
.pwbig { font-size: 26px; font-weight: 650; } .pwsub { font-size: 13px; fill: var(--dim) !important; }
.pwside { flex: 1; display: flex; flex-direction: column; gap: 8px; min-width: 0; }
.pwrow { display: flex; align-items: center; gap: 8px; font-size: 14px; }
.pwrow > span:first-child { color: var(--dim); min-width: 74px; }
.pwrow input[type=number] { width: 100%; background: var(--card2); border: 1px solid transparent; border-radius: 8px; padding: 6px 8px; }
.pwrow .btn { padding: 6px 10px; font-size: 13px; }
.pwwave { width: 100%; height: 64px; display: block; }
.pwerr { color: var(--red); font-size: 13px; }
.pwpre .row { padding: 10px 16px; flex-wrap: wrap; }
`));
}

/* ---------------------------------------------------------------- the knob */

function arcPath(f) {
  const cx = 75, cy = 75, r = 62, a0 = 135 * Math.PI / 180, a1 = (135 + 270 * clamp(f, 0, 1)) * Math.PI / 180;
  const p = a => `${(cx + r * Math.cos(a)).toFixed(1)} ${(cy + r * Math.sin(a)).toFixed(1)}`;
  return `M ${p(a0)} A ${r} ${r} 0 ${a1 - a0 > Math.PI ? 1 : 0} 1 ${p(a1)}`;
}

/* An SVG knob that reports turns (in whole ranges: 270 degrees = 1). */
function knob(onTurn) {
  const box = P.h('div', {
    class: 'pwknob', html: `<svg viewBox="0 0 150 150" width="150" height="150">
      <path d="${arcPath(1)}" stroke="var(--card2)" stroke-width="12" fill="none" stroke-linecap="round"/>
      <path class="ind" d="${arcPath(0.5)}" stroke="var(--pwc)" stroke-width="12" fill="none" stroke-linecap="round"/>
      <text class="pwbig" x="75" y="80">…</text><text class="pwsub" x="75" y="104"></text></svg>`,
  });
  let last = null;
  const ang = e => {
    const r = box.getBoundingClientRect();
    return Math.atan2(e.clientY - (r.top + r.height / 2), e.clientX - (r.left + r.width / 2)) * 180 / Math.PI;
  };
  box.addEventListener('pointerdown', e => { last = ang(e); box.setPointerCapture(e.pointerId); });
  box.addEventListener('pointermove', e => {
    if (last == null) return;
    const a = ang(e);
    let d = a - last;
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    last = a;
    onTurn(d / 270 * (e.shiftKey ? 0.1 : 1));
  });
  const up = () => { last = null; };
  box.addEventListener('pointerup', up);
  box.addEventListener('pointercancel', up);
  box.addEventListener('wheel', e => { e.preventDefault(); onTurn((e.deltaY < 0 ? 1 : -1) * 0.004); }, { passive: false });
  box.show = (f, big, sub, on) => {
    box.querySelector('.ind').setAttribute('d', arcPath(f));
    box.querySelector('.pwbig').textContent = big;
    box.querySelector('.pwsub').textContent = sub;
    box.classList.toggle('off', !on);
  };
  return box;
}

/* ---------------------------------------------------------------- the page */

P.registerPage({
  id: 'pwm',
  name: 'Generador PWM',
  icon: '⎍',
  render(main) {
    const { h, put } = P;
    css();
    let st = { run: false, ack: 0, ch: [] };
    let seq = Date.now() % 100000, lastEdit = 0, sendT = 0, shape = '', loaded = false, warned = false;
    const cards = [];

    const status = h('div', { class: 'row' });
    const grid = h('div', { class: 'pwg' });
    const presets = h('div', { class: 'card pwpre' });
    const pname = h('input', { type: 'text', placeholder: 'nombre del preset' });

    /* ---- talking to the app ---- */
    let made = null;
    const mkdir = () => made || (made = P.fsMkdir(DIR));      /* once: it answers 409 when it is there */
    const send = () => {
      lastEdit = Date.now();
      clearTimeout(sendT);
      sendT = setTimeout(async () => {
        seq++;
        const body = `pwm 1 seq=${seq}\n` + st.ch.map(line).join('\n') + '\n';
        try { await mkdir(); await P.fsPut(REQ, body); }
        catch (e) { P.toast('No se pudo mandar: ' + e.message, true); }
        lastEdit = Date.now();
      }, 150);
      if (!st.run && !warned) { warned = true; P.toast('La app no está abierta en la placa: lo toma al abrirla'); }
      paint();
    };
    const poll = async () => {
      let t;
      try { t = await P.fsText(STATE); } catch { return; }
      const s = parse(t);
      if (!loaded || Date.now() - lastEdit > 2000) {
        st = s;
        if (!st.ch.length) st.ch.push(DEF());
      } else {
        /* keep what is being edited; take what only the board knows */
        st.run = s.run;
        s.ch.forEach((c, i) => { if (st.ch[i]) Object.assign(st.ch[i], { rfreq: c.rfreq, bits: c.bits, err: c.err }); });
      }
      loaded = true;
      paint();
    };

    /* ---- one channel ---- */
    function channel(i) {
      const c = () => st.ch[i];
      const el = h('div', { class: 'card pwc' });
      const dot = h('span', { class: 'dot' });
      const name = h('span', { class: 'grow muted small' });
      const sw = h('input', { type: 'checkbox', onchange: e => { c().on = e.target.checked; send(); } });
      const pin = h('select', { onchange: e => { c().gpio = +e.target.value; send(); } });
      const del = h('button', { class: 'btn', title: 'Borrar el canal', onclick: () => {
        if (st.ch.length < 2) return;
        st.ch.splice(i, 1); shape = ''; send();
      } }, '✕');
      const seg = h('div', { class: 'pwseg' }, MODES.map(([k, n]) =>
        h('button', { 'data-k': k, onclick: () => { c().mode = k; shape = ''; send(); } }, n)));
      const kn = knob(d => {
        const ch = c();
        if (ch.pat !== 'fijo') return;
        setVal(ch, val(ch) + d);
        send();
      });
      const wave = h('canvas', { class: 'pwwave', width: 600, height: 128 });
      const freq = h('input', { type: 'number', min: 1, max: FREQ_MAX, onchange: e => { c().freq = niceFreq(+e.target.value); send(); } });
      const dec = k => () => { c().freq = niceFreq(c().freq * k); send(); };
      const freqRow = h('div', { class: 'pwrow' }, h('span', {}, 'Frecuencia'), freq,
        h('button', { class: 'btn', onclick: dec(0.1) }, '÷10'), h('button', { class: 'btn', onclick: dec(10) }, '×10'));
      const real = h('div', { class: 'small muted' });
      const pat = h('select', { onchange: e => { c().pat = e.target.value; shape = ''; send(); } },
        PATS.map(([k, n]) => h('option', { value: k }, n)));
      const per = h('input', { type: 'range', min: 0, max: 1000, oninput: e => {
        const x = +e.target.value / 1000;
        c().period = +(PER_MIN * Math.pow(PER_MAX / PER_MIN, x)).toFixed(3);
        send();
      } });
      const perTxt = h('span', { class: 'small' });
      const lo = h('input', { type: 'range', min: 0, max: 1000, oninput: e => { c().lo = Math.min(+e.target.value / 1000, c().hi); send(); } });
      const hi = h('input', { type: 'range', min: 0, max: 1000, oninput: e => { c().hi = Math.max(+e.target.value / 1000, c().lo); send(); } });
      const span = h('span', { class: 'small' });
      const steps = h('input', { type: 'text', placeholder: '0, 50, 100, 25', onchange: e => {
        const v = e.target.value.split(/[ ,;]+/).filter(Boolean).map(x => clamp(+x.replace(',', '.') / 100, 0, 1)).filter(x => !isNaN(x));
        if (v.length) { c().steps = v.slice(0, 8); send(); }
      } });
      const extra = h('div', { class: 'pwside' });
      const err = h('div', { class: 'pwerr' });
      put(el,
        h('div', { class: 'pwhd' }, h('b', {}, String(i + 1)), dot, name, h('label', { class: 'pwsw' }, sw, h('span')), del),
        h('div', { class: 'pwrow' }, h('span', {}, 'Pin'), pin),
        seg,
        h('div', { class: 'pwmid' }, kn, h('div', { class: 'pwside' },
          h('div', { class: 'btns' },
            h('button', { class: 'btn', onclick: () => { if (c().pat === 'fijo') { setVal(c(), val(c()) - 0.01); send(); } } }, '−'),
            h('button', { class: 'btn', onclick: () => { if (c().pat === 'fijo') { setVal(c(), val(c()) + 0.01); send(); } } }, '+')),
          wave, real)),
        freqRow,
        h('div', { class: 'pwrow' }, h('span', {}, 'Patrón'), pat),
        h('div', { class: 'pwrow pwper' }, h('span', {}, 'Período'), per, perTxt),
        h('div', { class: 'pwrow pwspan' }, h('span', {}, 'Desde'), lo),
        h('div', { class: 'pwrow pwspan' }, h('span', {}, 'Hasta'), hi),
        h('div', { class: 'pwrow pwspan small muted' }, span),
        h('div', { class: 'pwrow pwsteps' }, h('span', {}, 'Pasos (%)'), steps),
        extra, err);

      /* the settings of each mode */
      const numIn = (k, step, min, max, label, unit) => h('div', { class: 'pwrow' }, h('span', {}, label),
        h('input', { type: 'number', step, min, max, value: c()[k], onchange: e => {
          const ch = c(), v0 = val(ch);
          ch[k] = clamp(+e.target.value, min, max);
          if (ch.smax < ch.smin + 100) ch.smax = ch.smin + 100;
          if (ch.mode === 'servo') setVal(ch, v0);
          send();
        } }), h('span', { class: 'small muted' }, unit));
      const m = c().mode;
      if (m !== 'nivel') extra.append(h('label', { class: 'pwrow' },
        h('input', { type: 'checkbox', checked: c().inv, onchange: e => { c().inv = e.target.checked; send(); } }),
        h('span', {}, 'Invertir la salida')));
      if (m === 'servo') extra.append(numIn('smin', 10, 200, 2900, 'Mínimo', 'µs'), numIn('smax', 10, 300, 3000, 'Máximo', 'µs'),
        numIn('sdeg', 10, 10, 360, 'Recorrido', '°'));
      if (m === 'led') extra.append(numIn('gamma', 0.1, 1, 3, 'Gamma', ''));
      if (m === 'nivel') extra.append(h('div', { class: 'small muted' }, 'Con 1 kΩ en serie y 1 µF a GND: de 0 a 3,3 V.'));

      function update() {
        const ch = c();
        if (!ch) return;
        const col = COLORS[ch.mode] || COLORS.pwm;
        el.style.setProperty('--pwc', col);
        const on = ch.on && !ch.err;
        el.classList.toggle('on', on);
        dot.className = 'dot' + (on ? ' on' : ch.err ? ' bad' : '');
        sw.checked = ch.on;
        name.textContent = (MODES.find(m => m[0] === ch.mode) || MODES[0])[1] +
          (ch.pat !== 'fijo' ? ' · ' + (PATS.find(p => p[0] === ch.pat) || PATS[0])[1] : '');
        seg.querySelectorAll('button').forEach(b => b.classList.toggle('on', b.dataset.k === ch.mode));
        if (document.activeElement !== pin) {
          const used = new Set(st.ch.filter((_, k) => k !== i).map(x => x.gpio));
          put(pin, h('option', { value: -1 }, 'sin pin'), PINS.map(([g, p]) =>
            h('option', { value: g, disabled: used.has(g) }, `GPIO${g} · pata ${p}${used.has(g) ? ' (otro canal)' : ''}`)));
          pin.value = String(ch.gpio);
        }
        freqRow.style.display = hasFreq(ch.mode) ? '' : 'none';
        if (document.activeElement !== freq) freq.value = ch.freq;
        real.textContent = ch.rfreq ? `real ${fmtFreq(ch.rfreq)} · ${ch.bits} bits` : ch.on ? '' : 'apagado';
        if (document.activeElement !== pat) pat.value = ch.pat;
        const pt = ch.pat !== 'fijo';
        el.querySelectorAll('.pwper').forEach(r => { r.style.display = pt ? '' : 'none'; });
        el.querySelectorAll('.pwspan').forEach(r => { r.style.display = pt && ch.pat !== 'pasos' ? '' : 'none'; });
        el.querySelectorAll('.pwsteps').forEach(r => { r.style.display = ch.pat === 'pasos' ? '' : 'none'; });
        if (document.activeElement !== per) per.value = Math.round(Math.log(ch.period / PER_MIN) / Math.log(PER_MAX / PER_MIN) * 1000);
        perTxt.textContent = fmtTime(ch.period);
        if (document.activeElement !== lo) lo.value = Math.round(ch.lo * 1000);
        if (document.activeElement !== hi) hi.value = Math.round(ch.hi * 1000);
        span.textContent = `entre ${fmtVal(ch, ch.lo)[0]} y ${fmtVal(ch, ch.hi)[0]}`;
        if (document.activeElement !== steps) steps.value = ch.steps.map(s => Math.round(s * 100)).join(', ');
        err.textContent = ch.err || '';
        animate();
      }
      function animate() {
        const ch = c();
        if (!ch) return;
        const v = live(ch);
        const [big, sub] = fmtVal(ch, v);
        kn.show(v, big, sub, ch.on && !ch.err);
        const g = wave.getContext('2d'), W = wave.width, H = wave.height;
        g.clearRect(0, 0, W, H);
        g.strokeStyle = COLORS[ch.mode] || COLORS.pwm;
        g.lineWidth = 4;
        g.beginPath();
        const yh = 14, yl = H - 14;
        if (ch.mode === 'nivel') {
          const y = yl - (yl - yh) * v;
          g.moveTo(0, y); g.lineTo(W, y);
        } else {
          const d = duty(ch, v), n = ch.mode === 'servo' ? 1 : 2, pw = W / n;
          const hiY = ch.inv ? yl : yh, loY = ch.inv ? yh : yl;
          g.moveTo(0, loY);
          for (let k = 0; k < n; k++) {
            const x = k * pw;
            if (d > 0.0005) { g.lineTo(x, loY); g.lineTo(x, hiY); g.lineTo(x + d * pw, hiY); }
            if (d < 0.9995) { g.lineTo(x + d * pw, loY); g.lineTo(x + pw, loY); }
          }
        }
        g.stroke();
      }
      el.update = update;
      el.animate = animate;
      return el;
    }

    /* ---- painting ---- */
    function paint() {
      put(status,
        h('span', { class: 'dot ' + (st.run ? 'on' : 'off') }),
        h('div', { class: 'grow' }, st.run ? 'La app está abierta en la placa' : 'La app no está abierta en la placa: las salidas están apagadas',
          h('div', { class: 'small muted' }, freqs())),
        !st.run && h('button', { class: 'btn pri', onclick: () => P.openApp(APP).then(() => P.toast('Abierta en la placa')).catch(e => P.toast(e.message, true)) }, 'Abrir en la placa'),
        h('button', { class: 'btn red', onclick: () => { st.ch.forEach(c => { c.on = false; }); send(); } }, 'Apagar todo'));
      const sh = st.ch.map(c => [c.mode, c.pat, c.smin, c.smax, c.sdeg, c.gamma, c.inv].join(',')).join('|');
      if (sh !== shape) {
        shape = sh;
        cards.length = 0;
        st.ch.forEach((_, i) => cards.push(channel(i)));
        put(grid, cards, st.ch.length < MAX_CH && h('button', { class: 'btn', style: 'align-self:start', onclick: () => {
          st.ch.push(DEF()); shape = ''; send();
        } }, '+ Canal'));
      }
      cards.forEach(c => c.update());
    }
    function freqs() {
      const f = [...new Set(st.ch.filter(c => c.on && c.mode !== 'nivel').map(c => c.mode === 'servo' ? 50 : c.freq))];
      if (!f.length) return 'Ningún PWM encendido';
      return f.map(fmtFreq).join(' · ') + ` (${f.length} de 3 frecuencias)` + (f.length > 3 ? ': el LEDC no da más de tres' : '');
    }

    /* ---- presets ---- */
    async function listPresets() {
      let files = [];
      try { files = (await P.fsList(DIR)).filter(f => !f.dir && /\.pwm$/.test(f.name) && !/^[._]/.test(f.name)); }
      catch (e) { P.toast(e.message, true); }
      files.sort((a, b) => a.name.localeCompare(b.name));
      put(presets,
        h('div', { class: 'row' }, h('div', { class: 'grow' }, pname),
          h('button', { class: 'btn pri', onclick: savePreset }, 'Guardar'),
          h('label', { class: 'btn' }, 'Subir', h('input', { type: 'file', accept: '.pwm', style: 'display:none', onchange: upload }))),
        files.length ? files.map(f => h('div', { class: 'row' },
          h('div', { class: 'grow' }, f.name.replace(/\.pwm$/, ''), h('div', { class: 'small muted' }, P.fmtDate(f.mtime))),
          h('button', { class: 'btn', onclick: () => loadPreset(f.name) }, 'Cargar'),
          h('a', { class: 'btn', href: P.fsUrl(DIR + '/' + f.name, true) }, 'Bajar'),
          h('button', { class: 'btn red', onclick: () => delPreset(f.name) }, 'Borrar')))
          : h('div', { class: 'row muted' }, 'Todavía no hay presets.'));
    }
    async function savePreset() {
      const name = P.fsSlug(pname.value, 32, 'preset');
      try {
        await mkdir();
        await P.fsPut(`${DIR}/${name}.pwm`, '# P4OS, PWM generator (aos.pwm)\npwm 1\n' + st.ch.map(line).join('\n') + '\n');
        P.toast('Guardado: ' + name);
        pname.value = '';
        listPresets();
      } catch (e) { P.toast(e.message, true); }
    }
    async function loadPreset(file) {
      try {
        const s = parse(await P.fsText(`${DIR}/${file}`));
        if (!s.ch.length) throw new Error('el preset no tiene canales');
        st.ch = s.ch; shape = ''; send();
        P.toast('Cargado: ' + file.replace(/\.pwm$/, ''));
      } catch (e) { P.toast(e.message, true); }
    }
    async function delPreset(file) {
      if (!confirm(`¿Borrar el preset ${file.replace(/\.pwm$/, '')}?`)) return;
      try { await P.fsDelete(`${DIR}/${file}`); listPresets(); } catch (e) { P.toast(e.message, true); }
    }
    async function upload(e) {
      const f = e.target.files[0];
      if (!f) return;
      try {
        await mkdir();
        await P.fsPut(`${DIR}/${P.fsSlug(f.name.replace(/\.pwm$/i, ''), 32, 'preset')}.pwm`, await f.text());
        listPresets();
      } catch (err) { P.toast(err.message, true); }
    }

    put(main,
      h('h1', {}, 'Generador PWM'),
      h('div', { class: 'card' }, status),
      h('p', { class: 'note' }, 'Los canales de la app PWM de la placa, en vivo. Las perillas giran como en la placa (Mayúsculas o la rueda: más fino). Los patrones se ven animados acá con su forma, no con la fase de la placa.'),
      grid,
      h('h2', {}, 'Presets'),
      presets,
      h('p', { class: 'note' }, 'En /pwm de la tarjeta, una línea por canal. Al cargar uno, los canales encendidos se encienden en la placa.'));
    poll();
    listPresets();
    const tp = setInterval(poll, 1000);
    const ta = setInterval(() => cards.forEach((c, i) => { if (st.ch[i] && st.ch[i].pat !== 'fijo') c.animate(); }), 80);
    return () => { clearInterval(tp); clearInterval(ta); clearTimeout(sendT); };
  },
});
