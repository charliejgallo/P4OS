/*
 * P4OS - the Luces app's page in the portal: a lighting console for a
 * phone on the same network.
 *
 * Everything goes through the live channel (docs/PORTAL-PAGES.md, "Live
 * data"): the app puts "estado" (the fixtures, what they show, the scenes,
 * the types, the 512 channels) four times a second while this page looks,
 * and takes lines of key=value (apps/luces/main/lz_remote.c lists them).
 * New fixture types are CSV files this page writes in /luces/aparatos; the
 * app reads them again when told ("tipos=1").
 *
 * The lists are rebuilt only when the show changes ("cambios"), and never
 * while a field in them has the focus: buttons rebuilt under the finger
 * lose the tap.
 */
const P = window.P4OS;
const { h, put } = P;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });

const APP = 'aos.luces';
const LIVE = 'live?app=' + APP;
const TYPES_DIR = '/luces/aparatos';

const SWATCH = [['Rojo', 255, 0, 0, 0], ['Naranja', 255, 96, 0, 0], ['Ámbar', 255, 160, 0, 0], ['Amarillo', 255, 230, 0, 0],
  ['Verde', 0, 255, 0, 0], ['Cian', 0, 255, 255, 0], ['Azul', 0, 0, 255, 0], ['Violeta', 128, 0, 255, 0],
  ['Magenta', 255, 0, 200, 0], ['Cálido', 255, 180, 90, 0], ['Blanco', 255, 255, 255, 255]];
const STROBE = [['Sin strobe', 0], ['Lento', 60], ['Medio', 140], ['Rápido', 230]];
const FX = ['Ninguno', 'Arcoíris', 'Ola', 'Destellos', 'Recorrido', 'Colores', 'Respirar'];
const FADES = [[0, 'Corte'], [500, '0,5 s'], [1000, '1 s'], [2000, '2 s'], [3000, '3 s'], [5000, '5 s']];
const CUTS = [[0, 'Corte'], [50, 'Mitad'], [100, 'Todo fundido']];
const WORDS = ['dimmer', 'rojo', 'verde', 'azul', 'blanco', 'ambar', 'uv', 'strobe', 'pan', 'pan_fino', 'tilt', 'tilt_fino'];

function css() {
  if (document.getElementById('css-luces')) return;
  document.head.appendChild(h('style', { id: 'css-luces' }, `
    .lz { --lz: #ec4899; max-width: 900px; }
    .lz .card { margin-bottom: 14px; }
    .lz .pad { padding: 14px 16px; }
    .lz .top { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; }
    .lz .state { font-size: 13px; color: var(--dim); flex: 1 1 200px; }
    .lz .state.ok { color: var(--green); }
    .lz .state.bad { color: var(--orange); }
    .lz .btn.lzp { background: var(--lz); color: #fff; border-color: var(--lz); }
    .lz .btn.on { background: var(--text); color: var(--bg); }
    .lz .btn.red { background: var(--red); color: #fff; border-color: var(--red); }
    .lz .tabs { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; margin: 12px 0; }
    .lz .tabs .btn { padding: 10px 4px; }
    .lz .tiles { display: grid; grid-template-columns: repeat(auto-fill, minmax(78px, 1fr)); gap: 8px; }
    .lz .tile { border: 0; border-radius: 12px; background: var(--card2); color: inherit; padding: 10px 4px 8px;
      font: inherit; font-size: 12px; cursor: pointer; text-align: center; touch-action: manipulation; }
    .lz .tile.sel { outline: 3px solid var(--lz); }
    .lz .tile .d { width: 38px; height: 38px; border-radius: 50%; margin: 0 auto 6px; border: 2px solid var(--line); }
    .lz .tile .a { color: var(--dim); font-size: 11px; }
    .lz .tile .n { overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
    .lz .row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; padding: 0; border: 0; min-height: 0; }
    .lz label.sl { display: flex; align-items: center; gap: 12px; margin: 8px 0; }
    .lz label.sl span { min-width: 92px; color: var(--dim); font-size: 13px; }
    .lz label.sl b { min-width: 46px; text-align: right; font-size: 13px; }
    .lz input[type=range] { flex: 1; accent-color: var(--lz); height: 34px; }
    .lz .sw { display: grid; grid-template-columns: repeat(auto-fill, minmax(50px, 1fr)); gap: 10px; }
    .lz .sw button { border: 0; background: none; color: var(--dim); font: inherit; font-size: 11px; cursor: pointer; padding: 0; }
    .lz .sw i { display: block; width: 44px; height: 44px; border-radius: 50%; margin: 0 auto 4px; }
    .lz .sw input[type=color] { width: 44px; height: 44px; border: 0; padding: 0; background: none; border-radius: 50%; }
    .lz .chips { display: flex; flex-wrap: wrap; gap: 6px; margin: 8px 0; }
    .lz .chips .btn { padding: 7px 12px; }
    .lz .xy { position: relative; width: 100%; max-width: 420px; aspect-ratio: 4 / 3; background: #111114; border-radius: 14px;
      border: 2px solid var(--line); touch-action: none; cursor: crosshair; }
    .lz .xy::before, .lz .xy::after { content: ''; position: absolute; background: #2c2c2e; }
    .lz .xy::before { left: 0; right: 0; top: 50%; height: 2px; }
    .lz .xy::after { top: 0; bottom: 0; left: 50%; width: 2px; }
    .lz .xy i { position: absolute; width: 30px; height: 30px; margin: -15px 0 0 -15px; border-radius: 50%; background: var(--lz); z-index: 1; }
    .lz .scenes { display: grid; grid-template-columns: repeat(auto-fill, minmax(120px, 1fr)); gap: 8px; }
    .lz .scene { position: relative; border: 0; border-radius: 12px; background: var(--card2); color: inherit; font: inherit;
      text-align: left; padding: 12px 34px 12px 12px; min-height: 64px; cursor: pointer; touch-action: manipulation; }
    .lz .scene.cur { background: var(--lz); color: #fff; }
    .lz .scene small { display: block; opacity: .7; font-size: 11px; }
    .lz .scene .m { position: absolute; top: 4px; right: 4px; border: 0; background: none; color: inherit; font-size: 18px;
      cursor: pointer; padding: 4px 8px; }
    .lz .tap { font-size: 20px; padding: 14px 26px; }
    .lz table { width: 100%; border-collapse: collapse; font-size: 14px; }
    .lz th { text-align: left; color: var(--dim); font-weight: 500; padding: 4px; font-size: 12px; }
    .lz td { padding: 4px; border-top: 1px solid var(--line); }
    .lz td input, .lz td select, .lz .f input, .lz .f select { width: 100%; box-sizing: border-box; padding: 7px 8px; font: inherit;
      border: 1px solid var(--line); border-radius: 8px; background: var(--card); color: inherit; }
    .lz td input.n { max-width: 70px; }
    .lz td.ch { white-space: nowrap; color: var(--lz); font-weight: 600; }
    .lz td.ch.bad { color: var(--orange); }
    .lz .scroll { overflow-x: auto; }
    .lz .f { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 8px; align-items: end; margin: 8px 0; }
    .lz .f label { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--dim); }
    .lz .types div { padding: 6px 0; border-top: 1px solid var(--line); font-size: 13px; }
    .lz .types small { color: var(--dim); }
    .lz canvas.map { width: 100%; image-rendering: pixelated; border-radius: 8px; background: #111; cursor: pointer; }
    .lz .note { color: var(--dim); font-size: 13px; }
  `));
}

/* A sender that keeps only the latest value of a key while one is on its
 * way: a slider dragged on a phone makes dozens of events a second. */
function sender(onError) {
  const pending = new Map();
  let busy = false;
  async function flush() {
    if (busy || !pending.size) return;
    busy = true;
    const body = [...pending].map(([k, v]) => `${k}=${v}`).join('\n');
    pending.clear();
    try { await P.api(LIVE, { method: 'POST', body }); } catch (e) { onError(e); }
    busy = false;
    if (pending.size) setTimeout(flush, 40);
  }
  return (k, v) => { pending.delete(k); pending.set(k, v); flush(); };
}

P.registerPage({
  id: 'luces',
  name: tr('Luces'),
  icon: '💡',
  render(main) {
    css();
    let st = null, gone = false, seenShow = -1, taps = [], view = 'escenario';
    let localSel = null, localSelT = 0, dragging = false;
    try { view = localStorage.getItem('lz_view') || 'escenario'; } catch { /* private window */ }
    const send = sender(e => P.toast(tr('La app Luces no está abierta en la placa (%s)', e.message), true));

    /* ---- the bar: output, blackout, master ---- */
    const state = h('div', { class: 'state' }, tr('Buscando la app…'));
    const outBtn = h('button', { class: 'btn lzp', onclick: () => send('salida', st && st.salida.quiere ? 0 : 1) }, tr('Abrir salida'));
    const boBtn = h('button', { class: 'btn', onclick: () => send('blackout', st && st.blackout ? 0 : 1) }, 'Blackout');
    const master = h('input', { type: 'range', min: 0, max: 255, oninput: e => { send('master', e.target.value); masterV.textContent = pct(e.target.value); } });
    const masterV = h('b', {}, '');
    const bar = h('div', { class: 'card pad' },
      h('div', { class: 'top' }, state, outBtn, boBtn),
      h('label', { class: 'sl' }, h('span', {}, 'Master'), master, masterV));

    /* ---- Escenario ---- */
    const tiles = h('div', { class: 'tiles' });
    const selNote = h('span', { class: 'note' });
    const dim = h('input', { type: 'range', min: 0, max: 255, oninput: e => { send('dim', e.target.value); dimV.textContent = pct(e.target.value); } });
    const dimV = h('b', {}, '');
    const colorIn = h('input', { type: 'color', value: '#ff00c8', oninput: e => {
      const c = e.target.value;
      send('color', [1, 3, 5].map(i => parseInt(c.substr(i, 2), 16)).join(','));
    } });
    const strobeBtns = STROBE.map(([t, v]) => h('button', { class: 'btn', onclick: () => send('strobe', v) }, tr(t)));
    const strobeCard = h('div', { class: 'card pad' }, h('h3', {}, 'Strobe'), h('div', { class: 'chips' }, ...strobeBtns));
    const dot = h('i');
    const xy = h('div', { class: 'xy' }, dot);
    const posCard = h('div', { class: 'card pad' }, h('h3', {}, tr('Posición (pan y tilt)')), xy,
      h('div', { class: 'chips' }, h('button', { class: 'btn', onclick: () => send('pos', '32768,32768') }, tr('Al centro'))));
    const fxBtns = FX.map((t, i) => h('button', { class: 'btn', onclick: () => send('efecto', i) }, tr(t)));
    const speed = h('input', { type: 'range', min: 1, max: 100, oninput: e => send('velocidad', e.target.value) });
    const stage = h('div', {},
      h('div', { class: 'card pad' },
        h('div', { class: 'chips' },
          h('button', { class: 'btn', onclick: () => pick(st.aparatos.map((_, i) => i)) }, tr('Todos')),
          h('button', { class: 'btn', onclick: () => pick([]) }, tr('Ninguno')),
          h('button', { class: 'btn', onclick: () => pick(st.aparatos.map((_, i) => i).filter(i => i % 2 === 0)) }, tr('Pares')),
          h('button', { class: 'btn', onclick: () => pick(st.aparatos.map((_, i) => i).filter(i => i % 2 === 1)) }, tr('Impares')),
          selNote),
        tiles),
      h('div', { class: 'card pad' },
        h('label', { class: 'sl' }, h('span', {}, tr('Intensidad')), dim, dimV),
        h('div', { class: 'chips' }, ...[0, 25, 50, 75, 100].map(p => h('button', { class: 'btn', onclick: () => send('dim', Math.round(p * 2.55)) }, p + ' %')))),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Color')),
        h('div', { class: 'sw' },
          ...SWATCH.map(([t, r, g, b, w]) => h('button', { onclick: () => colour(r, g, b, w) },
            h('i', { style: `background: rgb(${r},${g},${b})` }), tr(t))),
          h('div', { style: 'text-align:center;font-size:11px;color:var(--dim)' }, colorIn, h('br'), tr('Otro')))),
      strobeCard, posCard,
      h('div', { class: 'card pad' }, h('h3', {}, tr('Efectos y pruebas')), h('div', { class: 'chips' }, ...fxBtns),
        h('label', { class: 'sl' }, h('span', {}, tr('Velocidad')), speed),
        h('p', { class: 'note' }, tr('El efecto corre sobre los aparatos elegidos al tocarlo (todos, si no hay ninguno elegido), encima de la escena.')),
        h('p', { class: 'note' }, tr('Pruebas: Recorrido prende un aparato por vez en blanco (cuál es cuál; uno fuera de turno tiene la dirección mal). Colores pasa todos por rojo, verde, azul y blanco (uno de otro color tiene los canales en otro orden que su tipo). Respirar sube y baja la intensidad despacio (escalones o parpadeo abajo son la curva del aparato).'))));

    function colour(r, g, b, w) {
      send('color', [r, g, b, w].join(','));
      /* a colour on a fixture that is dark is a colour nobody sees */
      const sel = currentSel();
      const any = st && st.aparatos.some((a, i) => (sel.length ? sel.includes(i) : true) && a.dim === 0);
      if (any) send('dim', 255);
    }

    function currentSel() {
      if (localSel && Date.now() - localSelT < 1500) return localSel;
      return st ? st.aparatos.map((a, i) => (a.sel ? i : -1)).filter(i => i >= 0) : [];
    }

    function pick(list) {
      localSel = list;
      localSelT = Date.now();
      send('elegir', list.join(','));
      drawStage();
    }

    function xyAt(e) {
      const r = xy.getBoundingClientRect();
      const x = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)), y = Math.min(1, Math.max(0, (e.clientY - r.top) / r.height));
      dot.style.left = (x * 100) + '%';
      dot.style.top = (y * 100) + '%';
      send('pos', Math.round(x * 65535) + ',' + Math.round(y * 65535));
    }
    xy.addEventListener('pointerdown', e => { dragging = true; xy.setPointerCapture(e.pointerId); xyAt(e); });
    xy.addEventListener('pointermove', e => { if (dragging) xyAt(e); });
    xy.addEventListener('pointerup', () => { dragging = false; });
    xy.addEventListener('pointercancel', () => { dragging = false; });

    /* ---- Escenas ---- */
    const scenes = h('div', { class: 'scenes' });
    const fadeBtns = FADES.map(([ms, t]) => h('button', { class: 'btn', onclick: () => send('fundido', ms) }, tr(t)));
    const chaseBtn = h('button', { class: 'btn lzp', onclick: () => send('secuencia', st && st.secuencia ? 0 : 1) }, tr('Andar'));
    const bpm = h('input', { type: 'number', min: 20, max: 300, style: 'width: 90px', onchange: e => send('bpm', e.target.value) });
    const chaseWarn = h('p', { class: 'note', style: 'color: var(--orange)' },
      tr('Hacen falta al menos dos escenas distintas: con una sola, la secuencia llama siempre a la misma. "Escenas de muestra" agrega cinco.'));
    const cutBtns = CUTS.map(([v, t]) => h('button', { class: 'btn', onclick: () => send('corte', v) }, tr(t)));
    const scenesView = h('div', {},
      h('div', { class: 'card pad' },
        h('div', { class: 'chips' }, h('button', { class: 'btn lzp', onclick: store }, tr('Guardar lo que hay')),
          h('button', { class: 'btn', onclick: () => send('muestra', 1) }, tr('Escenas de muestra'))),
        h('h3', {}, tr('Fundido al llamar una escena')), h('div', { class: 'chips' }, ...fadeBtns)),
      h('div', { class: 'card pad' }, scenes),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Secuencia: una escena tras otra, al tiempo')),
        h('div', { class: 'row' }, chaseBtn, bpm, h('span', { class: 'note' }, 'BPM'),
          h('button', { class: 'btn tap', onclick: tap }, 'Tap')),
        h('div', { class: 'chips' }, ...cutBtns),
        chaseWarn,
        h('p', { class: 'note' }, tr('Pasa por todas las escenas en orden, una por golpe. Tap: tocalo al ritmo de la música, tres o cuatro veces.'))));

    /* tap tempo here, not on the board: the network's jitter would be in the count */
    function tap() {
      const t = performance.now();
      if (taps.length && t - taps[taps.length - 1] > 2500) taps = [];
      taps.push(t);
      if (taps.length > 5) taps.shift();
      if (taps.length >= 2) {
        const b = Math.round(60000 * (taps.length - 1) / (taps[taps.length - 1] - taps[0]));
        if (b >= 20 && b <= 300) { send('bpm', b); bpm.value = b; }
      }
    }

    function store() {
      const n = prompt(tr('Nombre de la escena'), tr('Escena %d', (st ? st.escenas.length : 0) + 1));
      if (n) send('guardar', n.replace(/[\n|;]/g, ' ').slice(0, 23));
    }

    function sceneMenu(i) {
      const name = st.escenas[i];
      const what = prompt(tr('%s: 1 volver a grabar con lo que hay ahora, 2 cambiar el nombre, 3 borrar', name), '');
      if (what === '1') send('regrabar', i);
      else if (what === '2') {
        const n = prompt(tr('Nombre de la escena'), name);
        if (n) send('renombrar', i + ',' + n.replace(/[\n|;]/g, ' ').slice(0, 23));
      } else if (what === '3' && confirm(tr('¿Borrar la escena %s?', name))) send('borrar', i);
    }

    /* ---- Patch ---- */
    const patchBody = h('div');
    const addType = h('select');
    const addN = h('input', { type: 'number', min: 1, max: 64, value: 1 });
    const typesBox = h('div', { class: 'types' });
    const ntName = h('input', { placeholder: tr('PAR de 8 canales') });
    const ntCh = h('input', { placeholder: 'dimmer,rojo,verde,azul,blanco,strobe,0,0' });
    const patchView = h('div', {},
      h('div', { class: 'card pad' }, h('h3', {}, tr('Agregar aparatos')),
        h('div', { class: 'f' },
          h('label', {}, tr('Tipo'), addType),
          h('label', {}, tr('Cuántos'), addN),
          h('button', { class: 'btn lzp', onclick: () => send('agregar', addType.value + ',' + (addN.value || 1)) }, tr('Agregar'))),
        h('p', { class: 'note' }, tr('Van desde la primera dirección libre, uno tras otro.'))),
      h('div', { class: 'card pad' }, patchBody),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Tipos de aparato')), typesBox,
        h('h3', {}, tr('Un tipo nuevo')),
        h('div', { class: 'f' }, h('label', {}, tr('Nombre'), ntName), h('label', {}, tr('Canales, en orden'), ntCh),
          h('button', { class: 'btn lzp', onclick: newType }, tr('Guardar en la tarjeta'))),
        h('p', { class: 'note' }, tr('Una palabra por canal, como dice el manual del aparato: %s; o un número, para un canal fijo en ese valor (un modo, una macro en 0).', WORDS.join(', ')))));

    async function newType() {
      const name = ntName.value.trim(), ch = ntCh.value.trim().toLowerCase().replace(/\s+/g, '');
      if (!name || !ch) { P.toast(tr('Falta el nombre o los canales'), true); return; }
      const bad = ch.split(',').filter(w => w && !WORDS.includes(w) && !/^\d+$/.test(w) && !/^[a-z_]+=\d+$/.test(w));
      if (bad.length) { P.toast(tr('No entiendo: %s', bad.join(', ')), true); return; }
      const slug = (P.fsSlug ? P.fsSlug(name, 40, 'tipo') : name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')) || 'tipo';
      try {
        await P.fsMkdir('/luces').catch(() => {});
        await P.fsMkdir(TYPES_DIR).catch(() => {});
        await P.fsPut(`${TYPES_DIR}/${slug}.csv`, `# P4OS Luces: un tipo de aparato (apps/luces/README.md)\nnombre,${name.replace(/,/g, ' ')}\ncanales,${ch}\n`);
        send('tipos', 1);
        ntName.value = ntCh.value = '';
        P.toast(tr('Guardado en %s', `${TYPES_DIR}/${slug}.csv`));
      } catch (e) { P.toast(e.message, true); }
    }

    function drawPatch() {
      if (patchView.contains(document.activeElement) && document.activeElement.tagName !== 'BUTTON') return;
      const opts = (sel) => st.tipos.map((t, i) => h('option', { value: i, selected: i === sel ? '' : null }, `${t.n} · ${t.canales.length}`));
      const keep = addType.value;
      put(addType, ...opts(keep === '' ? 1 : +keep));
      let used = 0;
      st.aparatos.forEach(a => { used += st.tipos[a.t].canales.length; });
      const rows = st.aparatos.map((a, i) => {
        const n = st.tipos[a.t].canales.length;
        const name = h('input', { value: a.n });
        const addr = h('input', { class: 'n', type: 'number', min: 1, max: 512, value: a.a });
        const type = h('select', {}, ...opts(a.t));
        const save = () => send('aparato', `${i},${addr.value},${type.value},${name.value.replace(/[\n,]/g, ' ').slice(0, 23)}`);
        name.onchange = addr.onchange = type.onchange = save;
        return h('tr', {},
          h('td', {}, name), h('td', {}, type), h('td', {}, addr),
          h('td', { class: 'ch' + (a.pisa ? ' bad' : ''), title: a.pisa ? tr('Se pisa con otro aparato o se pasa del canal 512') : '' },
            n > 1 ? `${a.a}-${a.a + n - 1}` : String(a.a)),
          h('td', {}, h('button', { class: 'btn', title: tr('Sacar del patch'),
            onclick: () => { if (confirm(tr('¿Sacar %s del patch?', a.n))) send('quitar', i); } }, '✕')));
      });
      put(patchBody,
        h('h3', {}, tr('%d aparatos, %d canales de 512', st.aparatos.length, used)),
        h('div', { class: 'scroll' }, h('table', {},
          h('tr', {}, h('th', {}, tr('Nombre')), h('th', {}, tr('Tipo')), h('th', {}, tr('Dirección')), h('th', {}, tr('Canales')), h('th')),
          ...rows)));
      put(typesBox, ...st.tipos.map(t => h('div', {}, h('b', {}, t.n), ' ', h('small', {}, t.archivo || tr('incluido')), h('br'),
        h('small', {}, t.canales.join(', ')))));
    }

    /* ---- Salida ---- */
    const portSel = h('select', { onchange: e => send('puerto', e.target.value) });
    const outStats = h('p', { class: 'note' });
    const map = h('canvas', { class: 'map', width: 32 * 12, height: 16 * 12 });
    const pickInfo = h('p', {});
    let pickCh = 1;
    map.onclick = e => {
      const r = map.getBoundingClientRect();
      const c = Math.floor((e.clientX - r.left) / r.width * 32), row = Math.floor((e.clientY - r.top) / r.height * 16);
      if (c >= 0 && c < 32 && row >= 0 && row < 16) pickCh = row * 32 + c + 1;
      drawOut();
    };
    const outView = h('div', {},
      h('div', { class: 'card pad' }, h('div', { class: 'f' }, h('label', {}, tr('Puerto'), portSel)), outStats),
      h('div', { class: 'card pad' }, h('h3', {}, tr('Los 512 canales como salen')), map, pickInfo,
        h('p', { class: 'note' }, tr('Oscuros: sin aparato. Tocá un canal para ver qué es.'))));

    function fixtureOf(ch) {
      for (const [i, a] of st.aparatos.entries()) {
        const t = st.tipos[a.t];
        if (ch >= a.a && ch < a.a + t.canales.length) return [a, t.canales[ch - a.a]];
      }
      return null;
    }

    function drawOut() {
      if (!st) return;
      const ctx = map.getContext('2d');
      const patched = new Uint8Array(513);
      st.aparatos.forEach(a => { for (let c = a.a; c < a.a + st.tipos[a.t].canales.length && c <= 512; c++) patched[c] = 1; });
      ctx.fillStyle = '#000';
      ctx.fillRect(0, 0, map.width, map.height);
      for (let i = 0; i < 512; i++) {
        const v = parseInt(st.universo.substr(i * 2, 2), 16) || 0;
        ctx.fillStyle = patched[i + 1] ? `rgb(${40 + v * 196 / 255 | 0},${30 + v * 42 / 255 | 0},${50 + v * 103 / 255 | 0})` : '#161618';
        ctx.fillRect((i % 32) * 12 + 1, (i / 32 | 0) * 12 + 1, 10, 10);
        if (i + 1 === pickCh) { ctx.strokeStyle = '#fff'; ctx.lineWidth = 2; ctx.strokeRect((i % 32) * 12 + 1, (i / 32 | 0) * 12 + 1, 10, 10); }
      }
      const f = fixtureOf(pickCh);
      const v = parseInt(st.universo.substr((pickCh - 1) * 2, 2), 16) || 0;
      pickInfo.textContent = tr('Canal %d: %d', pickCh, v) + ' · ' + (f ? `${f[0].n}, ${/^\d+$/.test(f[1]) ? tr('fijo') : f[1]}` : tr('sin aparato'));
      const s = st.salida;
      outStats.textContent = s.abierta ? tr('Saliendo por %s: %s paquetes/s, %d mandados.', s.puerto, s.hz.toFixed(1), s.paquetes)
        : tr('Salida cerrada (%s).', s.puerto);
      if (document.activeElement !== portSel && portSel.options.length !== s.puertos.length)
        put(portSel, ...s.puertos.map(p => h('option', { value: p }, p)));
      if (document.activeElement !== portSel) portSel.value = s.puerto;
    }

    /* ---- the state ---- */
    function pct(v) { return Math.round(v * 100 / 255) + ' %'; }
    const on = (b, yes) => b.classList.toggle('on', !!yes);

    function drawStage() {
      if (!st) return;
      const sel = currentSel();
      if (tiles.childElementCount !== st.aparatos.length) {
        put(tiles, ...st.aparatos.map((a, i) => h('button', { class: 'tile', onclick: () => {
          const s = currentSel();
          pick(s.includes(i) ? s.filter(x => x !== i) : [...s, i].sort((x, y) => x - y));
        } }, h('div', { class: 'd' }), h('div', { class: 'n' }), h('div', { class: 'a' }))));
      }
      st.aparatos.forEach((a, i) => {
        const t = tiles.children[i];
        t.classList.toggle('sel', sel.includes(i));
        t.children[0].style.background = a.ve;
        t.children[1].textContent = a.n;
        t.children[2].textContent = a.a;
      });
      selNote.textContent = sel.length ? tr('%d elegidos', sel.length) : tr('ninguno elegido: todos');
      const f = st.aparatos[sel.length ? sel[0] : 0];
      if (f) {
        if (document.activeElement !== dim) { dim.value = f.dim; dimV.textContent = pct(f.dim); }
        STROBE.forEach(([, v], k) => on(strobeBtns[k], f.strobe === v));
        if (!dragging) { dot.style.left = (f.pan / 655.35) + '%'; dot.style.top = (f.tilt / 655.35) + '%'; }
      }
      strobeCard.style.display = st.tipos.some((t, i) => t.strobe && st.aparatos.some(a => a.t === i)) ? '' : 'none';
      posCard.style.display = st.tipos.some((t, i) => t.pos && st.aparatos.some(a => a.t === i)) ? '' : 'none';
      fxBtns.forEach((b, k) => on(b, st.efecto === k));
      if (document.activeElement !== speed) speed.value = st.velocidad;
    }

    function drawScenes() {
      if (scenes.dataset.k !== String(st.cambios) + '/' + st.escenas.length) {
        scenes.dataset.k = String(st.cambios) + '/' + st.escenas.length;
        put(scenes, ...(st.escenas.length ? st.escenas.map((n, i) => h('div', { class: 'scene', onclick: () => send('escena', i) },
          h('small', {}, String(i + 1)), n,
          h('button', { class: 'm', title: tr('Grabar, renombrar o borrar'), onclick: e => { e.stopPropagation(); sceneMenu(i); } }, '⋯')))
          : [h('p', { class: 'note' }, tr('Todavía no hay escenas. Armá un look en Escenario y guardalo acá.'))]));
      }
      [...scenes.children].forEach((c, i) => c.classList && c.classList.toggle('cur', i === st.escena));
      FADES.forEach(([ms], k) => on(fadeBtns[k], st.fundido === ms));
      CUTS.forEach(([v], k) => on(cutBtns[k], st.corte === v));
      chaseWarn.style.display = st.escenas.length < 2 ? '' : 'none';
      chaseBtn.textContent = st.secuencia ? tr('Parar') : tr('Andar');
      chaseBtn.className = 'btn ' + (st.secuencia ? 'red' : 'lzp');
      if (document.activeElement !== bpm) bpm.value = st.bpm;
    }

    function draw() {
      const s = st.salida;
      state.className = 'state ' + (s.error ? 'bad' : s.abierta ? 'ok' : '');
      state.textContent = s.error ? s.error
        : s.abierta ? tr('Saliendo por %s · %s paquetes/s', s.puerto, s.hz.toFixed(0)) + (st.blackout ? ' · BLACKOUT' : '')
          : tr('Salida cerrada (%s)', s.puerto);
      outBtn.textContent = s.quiere ? tr('Cerrar salida') : tr('Abrir salida');
      outBtn.className = 'btn ' + (s.quiere ? 'red' : 'lzp');
      boBtn.className = 'btn' + (st.blackout ? ' red' : '');
      if (document.activeElement !== master) { master.value = st.master; masterV.textContent = pct(st.master); }
      if (view === 'escenario') drawStage();
      else if (view === 'escenas') drawScenes();
      else if (view === 'patch') {
        const k = st.cambios + '/' + st.tipos.length;
        if (k !== seenShow) { seenShow = k; drawPatch(); }
      } else drawOut();
    }

    async function poll() {
      if (gone) return;
      if (!document.hidden) {
        try {
          st = await P.api(LIVE + '&key=estado');
          draw();
        } catch {
          state.className = 'state bad';
          state.textContent = tr('La app Luces no está abierta en la placa: abrila ahí (o desde Apps) y esta página la maneja.');
        }
      }
      setTimeout(poll, document.hidden ? 1500 : 250);
    }

    /* ---- the tabs ---- */
    const views = { escenario: stage, escenas: scenesView, patch: patchView, salida: outView };
    const names = { escenario: 'Escenario', escenas: 'Escenas', patch: 'Patch', salida: 'Salida' };
    const tabs = h('div', { class: 'tabs' });
    function show(v) {
      view = v;
      try { localStorage.setItem('lz_view', v); } catch { /* fine */ }
      Object.entries(views).forEach(([k, el]) => { el.style.display = k === v ? '' : 'none'; });
      put(tabs, ...Object.keys(views).map(k => h('button', { class: 'btn' + (k === v ? ' lzp' : ''), onclick: () => show(k) }, tr(names[k]))));
      seenShow = -1;
      if (st) draw();
    }
    put(main, h('div', { class: 'lz' }, h('h1', {}, tr('Luces')), bar, tabs, stage, scenesView, patchView, outView));
    show(views[view] ? view : 'escenario');
    poll();
    return () => { gone = true; };
  },
});
