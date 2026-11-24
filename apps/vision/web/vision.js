/* Vision's page in the board's portal (docs/PORTAL-PAGES.md): the camera's
 * live picture with what the board found drawn over it, while the app is
 * open on the board.
 *
 * Live through /api/live: the app puts "frame" (the camera's own JPEG) and
 * "state" (JSON: the mode, the speed, the boxes in the frame's pixels, the
 * codes read) about ten times a second while someone looks; the page asks
 * for both in a loop, one request at a time, and draws them on a canvas.
 * It sends the app lines: mode=N, photo, thr=NN (percent), test=0|1. The
 * texts are in Spanish and go through tr(), with this page's catalog
 * (web.vision.lang). */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const num = (v, d) => v == null ? '--' : v.toFixed(d).replace('.', ',');
const APP = 'aos.vision';
const LIVE = 'live?app=' + APP;

const MODES = [N_('Caras'), N_('Personas'), N_('Objetos'), N_('Manos'), N_('Códigos')];
const NOUN = [[N_('cara'), N_('caras')], [N_('persona'), N_('personas')], [N_('objeto'), N_('objetos')],
  [N_('mano'), N_('manos')], [N_('código'), N_('códigos')]];
const COLOR = ['#30D158', '#0A84FF', null, '#FF9F0A', '#FFD60A'];
const PAL = ['#30D158', '#0A84FF', '#FF9F0A', '#BF5AF2', '#FF375F', '#40C8E0', '#FFD60A', '#64D2FF'];
const COCO = [N_('persona'), N_('bicicleta'), N_('auto'), N_('moto'), N_('avión'), N_('colectivo'), N_('tren'),
  N_('camión'), N_('barco'), N_('semáforo'), N_('hidrante'), N_('cartel de pare'), N_('parquímetro'), N_('banco'),
  N_('pájaro'), N_('gato'), N_('perro'), N_('caballo'), N_('oveja'), N_('vaca'), N_('elefante'), N_('oso'),
  N_('cebra'), N_('jirafa'), N_('mochila'), N_('paraguas'), N_('cartera'), N_('corbata'), N_('valija'),
  N_('frisbee'), N_('esquís'), N_('tabla de snow'), N_('pelota'), N_('barrilete'), N_('bate'),
  N_('guante de béisbol'), N_('patineta'), N_('tabla de surf'), N_('raqueta'), N_('botella'), N_('copa de vino'),
  N_('taza'), N_('tenedor'), N_('cuchillo'), N_('cuchara'), N_('bol'), N_('banana'), N_('manzana'),
  N_('sándwich'), N_('naranja'), N_('brócoli'), N_('zanahoria'), N_('pancho'), N_('pizza'), N_('dona'),
  N_('torta'), N_('silla'), N_('sillón'), N_('planta'), N_('cama'), N_('mesa'), N_('inodoro'), N_('televisor'),
  N_('notebook'), N_('mouse'), N_('control remoto'), N_('teclado'), N_('celular'), N_('microondas'), N_('horno'),
  N_('tostadora'), N_('pileta de cocina'), N_('heladera'), N_('libro'), N_('reloj'), N_('florero'), N_('tijera'),
  N_('osito de peluche'), N_('secador de pelo'), N_('cepillo de dientes')];
const GESTURE = [N_('uno'), N_('dos'), N_('tres'), N_('cuatro'), N_('cinco'), N_('pulgar arriba'), N_('ok'),
  N_('mano'), N_('llamame'), N_('pulgar abajo'), N_('mano')];
const CODE_KIND = ['QR', 'EAN-13', 'EAN-8', 'UPC-A'];
/* the app's states (vision.h, vi_state_t) that say why there is no picture */
const STATE_MSG = { 1: N_('No hay cámara ni fotos de prueba en la placa.'), 2: N_('Esperando la imagen de la cámara…'),
  3: N_('Cargando el modelo…'), 4: N_('Falta el paquete de modelos de este modo: bajalo desde la Tienda.'),
  5: N_('Falta el motor de visión: instalá la app de nuevo.'), 6: N_('El modelo no cargó.') };

const css = `
.vi-wrap{position:relative;width:100%;max-width:960px;margin:0 auto}
.vi-canvas{width:100%;display:block;border-radius:12px;background:#0a0a0c;aspect-ratio:4/3}
.vi-msg{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;text-align:center;
  padding:24px;color:#fff;font-size:1.1em}
.vi-seg{display:flex;flex-wrap:wrap;gap:6px}
.vi-seg .btn.on{background:var(--accent);color:#fff}
.vi-big{font-size:1.6em;font-weight:600;white-space:nowrap}
.vi-list{width:100%;border-collapse:collapse;font-size:15px}
.vi-list td{padding:7px 4px;border-top:1px solid var(--line);vertical-align:middle;word-break:break-all}
.vi-dot{display:inline-block;width:10px;height:10px;border-radius:5px;margin-right:8px}
`;

function label(st, b) {
  if (st.mode === 2) return tr(COCO[b[4]] || '?') + ' ' + Math.round(b[5] * 100) + '%';
  if (st.mode === 3) return tr(GESTURE[b[4]] || N_('mano'));
  return Math.round(b[5] * 100) + '%';
}
const color = (mode, cls) => COLOR[mode] || PAL[(cls || 0) % PAL.length];

P.registerPage({
  id: 'vision',
  name: 'Visión',
  icon: '◉',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-vision')) document.head.append(h('style', { id: 'css-vision' }, css));
    let alive = true, st = null, img = null, lastSeq = -1, missing = false;
    const codes = [];   /* read lately, newest first */
    const send = text => P.api(LIVE, { method: 'POST', body: text }).catch(e => P.toast(e.message, true));

    const canvas = h('canvas', { class: 'vi-canvas', width: 640, height: 480 });
    const msg = h('div', { class: 'vi-msg' }, tr('Conectando…'));
    const big = h('div', { class: 'vi-big' }, '—');
    const speed = h('div', { class: 'muted' }, '');
    const modeBtns = MODES.map((m, i) => h('button', { class: 'btn', onclick: () => send('mode=' + i) }, tr(m)));
    const testBtn = h('button', { class: 'btn', onclick: () => send('test=' + (st && st.src === 2 ? 0 : 1)) },
      tr('Fotos de prueba'));
    const thr = h('input', { type: 'range', min: 5, max: 95, value: 50, style: 'flex:1' });
    const thrVal = h('span', { class: 'val' }, '');
    thr.oninput = () => { thrVal.textContent = thr.value + '%'; };
    thr.onchange = () => send('thr=' + thr.value);
    const thrRow = h('div', { class: 'row' }, h('span', { class: 'muted' }, tr('Umbral')), thr, thrVal);
    const list = h('table', { class: 'vi-list' });
    const listTitle = h('div', { class: 'muted small' }, tr('En la imagen ahora'));
    const openBtn = h('button', { class: 'btn pri', onclick: () => P.openApp(APP) }, tr('Abrir Visión en la placa'));

    put(main,
      h('div', { class: 'card pad' },
        h('div', { class: 'vi-wrap' }, canvas, msg),
        h('div', { class: 'row', style: 'margin-top:12px;flex-wrap:wrap;gap:8px' }, h('div', { class: 'grow' }, big, speed),
          h('button', { class: 'btn', onclick: () => send('photo') }, tr('Sacar una foto')))),
      h('div', { class: 'card pad' },
        h('div', { class: 'vi-seg' }, ...modeBtns),
        h('div', { style: 'margin-top:12px' }, thrRow),
        h('div', { class: 'btns', style: 'margin-top:8px' }, testBtn, openBtn)),
      h('div', { class: 'card pad' }, listTitle, list),
      h('p', { class: 'note' }, tr('La placa manda la imagen y lo que encuentra mientras esta página está abierta. ' +
        'Las fotos quedan en la carpeta de Fotos de la tarjeta, con los recuadros.')));

    const ctx = canvas.getContext('2d');
    function draw() {
      if (!img || !st) return;
      const W = st.w || img.width, H = st.h || img.height;
      if (canvas.width !== W || canvas.height !== H) { canvas.width = W; canvas.height = H; }
      ctx.imageSmoothingEnabled = true;
      ctx.drawImage(img, 0, 0, W, H);
      const sx = 1, sy = 1;
      ctx.lineWidth = Math.max(2, W / 200);
      ctx.font = `600 ${Math.round(W / 36)}px system-ui,sans-serif`;
      const box = (x0, y0, x1, y1, c, text) => {
        ctx.strokeStyle = c; ctx.fillStyle = c;
        ctx.strokeRect(x0 * sx, y0 * sy, (x1 - x0) * sx, (y1 - y0) * sy);
        const tw = ctx.measureText(text).width + 10, th = W / 28;
        ctx.fillRect(x0 * sx - 1, y0 * sy - 1, tw, th);
        ctx.fillStyle = '#000'; ctx.fillText(text, x0 * sx + 4, y0 * sy + th * 0.8);
      };
      for (const b of st.boxes) box(b[0], b[1], b[2], b[3], color(st.mode, b[4]), label(st, b));
      for (const c of st.codes) box(c.b[0], c.b[1], c.b[2], c.b[3], COLOR[4], CODE_KIND[c.k] || '');
    }

    function showState(s) {
      missing = false;
      st = s;
      modeBtns.forEach((b, i) => b.classList.toggle('on', i === s.mode));
      testBtn.classList.toggle('on', s.src === 2);
      const n = s.mode === 4 ? s.codes.length : s.boxes.length;
      const noun = NOUN[s.mode] || ['', ''];
      big.textContent = n + ' ' + tr(noun[n === 1 ? 0 : 1]);
      speed.textContent = `${num(s.fps, 1)} fps · ${num(s.mode === 4 ? s.ms : s.model_ms, 1)} ms`;
      thrRow.style.display = s.mode === 4 ? 'none' : '';
      listTitle.textContent = s.mode === 4 ? tr('Leídos hace poco') : tr('En la imagen ahora');
      if (document.activeElement !== thr && s.thr > 0) {
        thr.value = Math.round(s.thr * 100);
        thrVal.textContent = thr.value + '%';
      }
      const why = s.state !== 7 ? STATE_MSG[s.state] : null;
      msg.textContent = why ? tr(why) : '';
      msg.style.display = why ? '' : 'none';
      openBtn.style.display = 'none';
      /* the list: what is in the picture, or the codes read lately */
      const rows = [];
      if (s.mode === 4) {
        for (const c of s.codes) {
          const i = codes.findIndex(x => x.t === c.t && x.k === c.k);
          if (i >= 0) codes.splice(i, 1);
          codes.unshift({ t: c.t, k: c.k });
        }
        codes.length = Math.min(codes.length, 8);
        for (const c of codes) {
          const copy = h('button', { class: 'btn small', onclick: () => navigator.clipboard.writeText(c.t)
            .then(() => P.toast(tr('Copiado'))).catch(() => P.toast(tr('No se pudo copiar'), true)) }, tr('Copiar'));
          const text = /^https?:\/\//.test(c.t) ? h('a', { href: c.t, target: '_blank', rel: 'noopener' }, c.t) : c.t;
          rows.push(h('tr', {}, h('td', {}, h('span', { class: 'vi-dot', style: 'background:' + COLOR[4] }), text),
            h('td', { class: 'muted small' }, CODE_KIND[c.k] || ''), h('td', { style: 'text-align:right' }, copy)));
        }
      } else {
        s.boxes.forEach((b, i) => {
          const name = s.mode === 2 ? tr(COCO[b[4]] || '?') : s.mode === 3 ? tr(GESTURE[b[4]] || N_('mano'))
            : tr(noun[0]) + ' ' + (i + 1);
          rows.push(h('tr', {}, h('td', {}, h('span', { class: 'vi-dot', style: 'background:' + color(s.mode, b[4]) }),
            name), h('td', { class: 'muted', style: 'text-align:right' }, Math.round(b[5] * 100) + '%')));
        });
      }
      if (!rows.length) rows.push(h('tr', {}, h('td', { class: 'muted' }, tr('Nada por ahora.'))));
      put(list, ...rows);
    }

    function showMissing() {
      if (missing) return;
      missing = true;
      msg.textContent = tr('Visión no está abierta en la placa.');
      msg.style.display = '';
      openBtn.style.display = '';
      big.textContent = '—';
      speed.textContent = '';
    }

    /* a picture too big for the live channel comes small: "V565", its width
     * and height, then RGB565 pixels */
    async function smallOrJpeg(blob) {
      const b = await blob.arrayBuffer(), dv = new DataView(b);
      if (b.byteLength < 8 || dv.getUint32(0, false) !== 0x56353635) return blob;
      const w = dv.getUint16(4, true), hh = dv.getUint16(6, true), id = new ImageData(w, hh);
      for (let i = 0; i < w * hh; i++) {
        const p = dv.getUint16(8 + i * 2, true);
        id.data[i * 4] = (p >> 11) * 255 / 31; id.data[i * 4 + 1] = ((p >> 5) & 63) * 255 / 63;
        id.data[i * 4 + 2] = (p & 31) * 255 / 31; id.data[i * 4 + 3] = 255;
      }
      return id;
    }

    async function loop() {
      while (alive) {
        const t0 = performance.now();
        try {
          const [rs, rf] = await Promise.all([
            fetch('/api/' + LIVE + '&key=state', { cache: 'no-store' }),
            fetch('/api/' + LIVE + '&key=frame', { cache: 'no-store' })]);
          if (rs.ok) {
            const s = await rs.json();
            if (rf.ok && s.seq !== lastSeq) {
              lastSeq = s.seq;
              const blob = await rf.blob();
              const b = await createImageBitmap(await smallOrJpeg(blob));
              if (img && img.close) img.close();
              img = b;
            }
            showState(s);
            draw();
          } else {
            showMissing();
          }
        } catch { showMissing(); }
        const wait = (missing ? 1500 : 90) - (performance.now() - t0);
        await new Promise(res => setTimeout(res, wait > 0 ? wait : 0));
      }
    }
    loop();
    return () => { alive = false; if (img && img.close) img.close(); };
  },
});
