/* The Botonera's page in the board's portal (docs/PORTAL-PAGES.md): the
 * buttons, to play them from the phone; the grid, to arrange it; and the
 * sounds folder, to upload, listen and delete.
 *
 * The grid is the app's own file, /data/botonera.txt (tab-separated: a P
 * line per page, a B line per button; apps/botonera/main/bn_grid.c), and the
 * sounds are the WAV and MP3 files of /sonidos. The page writes the file and
 * tells the app "reload" through the live channel; the app also notices the
 * file's date by itself.
 *
 * Playing goes through the live channel too (POST /api/live?app=aos.botonera):
 * "down 5" when a finger lands on a button and "up 5" when it lifts, so the
 * "hold" buttons work from the phone as on the board; "stopall", "page 1".
 * What sounds comes back in the "state" the app puts while someone looks.
 * The app plays only while it is in front on the board. Texts in Spanish,
 * through tr() (web.botonera.lang). */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const APP = 'aos.botonera';
const LIVE = 'live?app=' + APP;
const GRID = '/data/botonera.txt';
const DIR = '/sonidos';
const PER = 12, PAGES = 8;

/* the icons: the key the file keeps, and what the phone shows for it */
const ICONS = [['note', '🎵'], ['bell', '🔔'], ['star', '⭐'], ['fire', '🔥'], ['bomb', '💣'], ['trophy', '🏆'],
  ['bolt', '⚡'], ['heart', '💓'], ['volume', '🔊'], ['mic', '🎤'], ['tap', '👆'], ['check', '✔️'],
  ['close', '❌'], ['alarm', '⏰'], ['clock', '🕐'], ['sand', '⏳'], ['wave', '🎚️'], ['sine', '〰️'],
  ['rain', '🌧️'], ['wind', '💨'], ['sun', '☀️'], ['snow', '❄️'], ['cat', '🐱'], ['robot', '🤖'],
  ['rocket', '🚀'], ['phone', '📞'], ['door', '🚪'], ['dice', '🎲'], ['gamepad', '🎮'], ['cards', '🃏'],
  ['flag', '🚩'], ['magnet', '🧲'], ['bulb', '💡'], ['tv', '📺'], ['person', '👤'], ['rec', '⏺️']];
const EMOJI = Object.fromEntries(ICONS);
const COLORS = ['FF453A', 'FF9F0A', 'FFD60A', '30D158', '63E6BE', '40C8E0',
  '0A84FF', '5E5CE6', 'BF5AF2', 'FF375F', 'AC8E68', '8E8E93'];
const MODES = [['cut', N_('Cortar')], ['over', N_('Superponer')], ['hold', N_('Mantener')]];
const MODE_HELP = {
  cut: N_('Un toque lo hace sonar y otro lo corta.'),
  over: N_('Cada toque lo hace sonar de nuevo, encima de lo que ya suena.'),
  hold: N_('Suena mientras el dedo está apoyado.'),
};

if (!document.getElementById('css-botonera')) {
  const st = document.createElement('style');
  st.id = 'css-botonera';
  st.textContent = `
.bntop { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.bntop .grow { flex: 1; min-width: 120px; }
.bnstop { background: var(--red); color: #fff; border: 0; font-weight: 600; }
.bntabs { display: flex; gap: 8px; flex-wrap: wrap; margin: 4px 0 12px; }
.bntab { padding: 8px 14px; border-radius: 20px; background: var(--card2); cursor: pointer; border: 0; color: inherit; font: inherit; }
.bntab.on { background: var(--accent); color: #fff; }
.bngrid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }
@media (min-width: 760px) { .bngrid { grid-template-columns: repeat(4, 1fr); } }
.bnpad { position: relative; aspect-ratio: 1 / 0.92; border-radius: 18px; display: flex; flex-direction: column;
  align-items: center; justify-content: center; gap: 4px; cursor: pointer; user-select: none; -webkit-user-select: none;
  touch-action: none; overflow: hidden; color: #fff; transition: transform .06s, filter .06s; border: 4px solid transparent; }
.bnpad.dark { color: #111; }
.bnpad:active, .bnpad.on { filter: brightness(1.25); }
.bnpad.on { border-color: #fff; }
.bnpad.empty { background: var(--card2); color: var(--dim); }
.bnpad.sel { border-color: var(--accent); }
.bnpad .em { font-size: 34px; line-height: 1; }
.bnpad .nm { font-size: 14px; font-weight: 600; text-align: center; padding: 0 6px; max-width: 100%;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bnpad .tg { position: absolute; top: 5px; right: 8px; font-size: 13px; opacity: .85; }
.bnpad .pr { position: absolute; left: 10%; right: 10%; bottom: 6px; height: 5px; border-radius: 3px; background: rgba(0,0,0,.3); }
.bnpad .pr i { display: block; height: 100%; border-radius: 3px; background: #fff; width: 0; }
.bned label.f { display: block; margin: 12px 0 6px; color: var(--dim); font-size: 13px; }
.bned input[type=text], .bned select { width: 100%; box-sizing: border-box; padding: 10px; border-radius: 10px;
  border: 1px solid var(--line); background: var(--card2); color: inherit; font: inherit; }
.bned input[type=range] { width: 100%; }
.bnsw { display: flex; flex-wrap: wrap; gap: 8px; }
.bnsw button { width: 34px; height: 34px; border-radius: 50%; border: 3px solid transparent; cursor: pointer; }
.bnsw button.on { border-color: var(--fg, #fff); outline: 2px solid var(--accent); }
.bnic { display: flex; flex-wrap: wrap; gap: 6px; }
.bnic button { width: 42px; height: 42px; border-radius: 10px; border: 0; background: var(--card2); font-size: 22px; cursor: pointer; }
.bnic button.on { background: var(--accent); }
.bnfile { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; padding: 10px 16px; border-top: 1px solid var(--line); }
.bnfile:first-child { border-top: 0; }
.bnfile .nm { flex: 1; min-width: 140px; font-weight: 600; word-break: break-all; }
.bnfile audio { height: 34px; max-width: 220px; }
.bnwarn { border-left: 4px solid var(--orange); }
`;
  document.head.append(st);
}

/* ---- the grid's file, as the app reads and writes it ---- */

function emptyPage(name) { return { name, pads: Array(PER).fill(null) }; }

function parseGrid(text) {
  const pages = [];
  for (const raw of text.split('\n')) {
    const line = raw.replace(/\r$/, '');
    if (!line || line[0] === '#') continue;
    const f = line.split('\t');
    if (f[0] === 'P' && f.length >= 2 && pages.length < PAGES) pages.push(emptyPage(f[1]));
    else if (f[0] === 'B' && pages.length && f.length >= 3) {
      const s = parseInt(f[1], 10);
      if (!(s >= 0 && s < PER) || !f[2] || f[2].includes('/')) continue;
      pages[pages.length - 1].pads[s] = {
        file: f[2], name: f[3] || f[2].replace(/\.[^.]+$/, ''), color: (f[4] || '8E8E93').toUpperCase(),
        icon: EMOJI[f[5]] ? f[5] : 'note', mode: ['cut', 'over', 'hold'].includes(f[6]) ? f[6] : 'cut',
        loop: f[7] === '1', vol: Math.max(0, Math.min(100, parseInt(f[8] ?? '100', 10) || 0)),
      };
    }
  }
  if (!pages.length) pages.push(emptyPage(tr('Efectos')));
  return pages;
}

const clean = s => String(s).replace(/[\t\r\n]/g, ' ');

function writeGrid(pages) {
  let t = '# Botonera 1: P <page>; B <slot> <file> <name> <color> <icon> <cut|over|hold> <loop> <vol>\n';
  for (const p of pages) {
    t += `P\t${clean(p.name)}\n`;
    p.pads.forEach((b, s) => {
      if (b) t += `B\t${s}\t${clean(b.file)}\t${clean(b.name)}\t${b.color}\t${b.icon}\t${b.mode}\t${b.loop ? 1 : 0}\t${b.vol}\n`;
    });
  }
  return t;
}

const isLight = hex => {
  const n = parseInt(hex, 16);
  return ((n >> 16) & 255) * 299 + ((n >> 8) & 255) * 587 + (n & 255) * 114 > 160000;
};
const isSound = n => !n.startsWith('.') && /\.(wav|mp3)$/i.test(n);
const pretty = f => { const s = f.replace(/\.[^.]+$/, '').replace(/[_-]/g, ' '); return s.charAt(0).toUpperCase() + s.slice(1); };

P.registerPage({
  id: 'botonera',
  name: tr('Botonera'),
  icon: '▦',
  render(main) {
    const { h, put } = P;
    let pages = null, files = [], cur = 0, edit = false, sel = -1;
    let live = null, lastRev = null, gone = false, saveT = null, savedAt = 0;
    const top = h('div', { class: 'card pad', style: 'margin-bottom:14px' });
    const board = h('div', { class: 'card pad' });
    const editor = h('div');
    const sounds = h('div', { class: 'card' });
    const upCard = h('div', { class: 'card pad' });

    const send = text => P.api(LIVE, { method: 'POST', body: text }).catch(e => P.toast(e.message, true));
    const openApp = () => P.openApp(APP).then(() => P.toast(tr('Abierta en la placa'))).catch(e => P.toast(e.message, true));

    /* ---- saving: shortly after the last change, then the app reads it ---- */
    const save = () => {
      clearTimeout(saveT);
      saveT = setTimeout(async () => {
        try {
          await P.fsPut(GRID, writeGrid(pages));
          savedAt = Date.now();
          send('reload');
        } catch (e) { P.toast(e.message, true); }
      }, 500);
    };

    /* ---- the top: is the app there, stop everything ---- */
    const drawTop = () => {
      let msg = null;
      if (!live) msg = tr('La Botonera no está abierta en la placa: los botones de acá suenan allá cuando está al frente.');
      else if (!live.front) msg = tr('La Botonera está abierta pero detrás de otra app: traela al frente para que suene.');
      else if (live.rec) msg = tr('La placa está grabando un sonido.');
      put(top,
        h('div', { class: 'bntop' },
          h('div', { class: 'grow' },
            h('b', {}, live && live.front ? tr('Lista para sonar') : tr('Sin la app al frente')),
            msg ? h('div', { class: 'muted small' }, msg) : null),
          (!live || !live.front) ? h('button', { class: 'btn pri', onclick: openApp }, tr('Abrir en la placa')) : null,
          h('button', { class: 'btn bnstop', onclick: () => send('stopall') }, '■ ' + tr('Parar todo')),
          h('button', { class: 'btn' + (edit ? ' pri' : ''), onclick: () => { edit = !edit; sel = -1; draw(); } },
            edit ? tr('Listo') : tr('Editar'))));
    };

    /* ---- the buttons ---- */
    const padEls = [];
    const drawBoard = () => {
      if (!pages) {
        put(board, h('p', { class: 'muted' }, tr('Todavía no hay botonera: abrí la app en la placa una vez y crea los sonidos de ejemplo y la grilla.')),
          h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: openApp }, tr('Abrir en la placa'))));
        return;
      }
      if (cur >= pages.length) cur = pages.length - 1;
      const tabs = h('div', { class: 'bntabs' }, pages.map((p, i) =>
        h('button', { class: 'bntab' + (i === cur ? ' on' : ''), onclick: () => { cur = i; sel = -1; send('page ' + i); draw(); } }, p.name || '·')));
      padEls.length = 0;
      const grid = h('div', { class: 'bngrid' });
      pages[cur].pads.forEach((b, s) => {
        const gi = cur * PER + s;
        const el = h('div', { class: 'bnpad' + (b ? (isLight(b.color) ? ' dark' : '') : ' empty') + (edit && sel === s ? ' sel' : '') });
        if (b) {
          el.style.background = '#' + b.color;
          const tags = (b.loop ? '↻' : '') + (b.mode === 'over' && !b.loop ? '⧉' : '') + (b.mode === 'hold' ? '✋' : '');
          el.append(h('span', { class: 'em' }, EMOJI[b.icon] || '🎵'), h('span', { class: 'nm' }, b.name),
            h('span', { class: 'tg' }, edit ? '✎' : tags), h('span', { class: 'pr', style: 'display:none' }, h('i')));
        } else {
          el.append(h('span', { class: 'em' }, '+'), h('span', { class: 'nm' }, tr('Vacío')));
        }
        let down = false;
        el.addEventListener('pointerdown', e => {
          e.preventDefault();
          if (edit || !b) { sel = s; edit = true; draw(); return; }
          down = true;
          try { el.setPointerCapture(e.pointerId); } catch (_) { /* old browsers */ }
          el.classList.add('on');
          send('down ' + gi);
        });
        const up = () => { if (!down) return; down = false; send('up ' + gi); };
        el.addEventListener('pointerup', up);
        el.addEventListener('pointercancel', up);
        el.addEventListener('contextmenu', e => e.preventDefault());
        padEls[s] = el;
        grid.append(el);
      });
      const pageTools = edit ? h('div', { class: 'btns', style: 'margin-top:12px' },
        h('input', { type: 'text', value: pages[cur].name, maxlength: 30, style: 'flex:1;min-width:140px',
          onchange: e => { pages[cur].name = e.target.value.trim() || pages[cur].name; save(); draw(); } }),
        pages.length < PAGES ? h('button', { class: 'btn', onclick: () => {
          pages.push(emptyPage(tr('Página %d', pages.length + 1))); cur = pages.length - 1; sel = -1; save(); draw();
        } }, '+ ' + tr('Página')) : null,
        pages.length > 1 ? h('button', { class: 'btn red', onclick: () => {
          if (!confirm(tr('¿Borrar la página %s? Los sonidos quedan en la tarjeta.', pages[cur].name))) return;
          pages.splice(cur, 1); cur = Math.max(0, cur - 1); sel = -1; save(); draw();
        } }, tr('Borrar página')) : null) : null;
      put(board, tabs, grid, pageTools,
        h('p', { class: 'note' }, edit ? tr('Tocá un botón para cambiarlo. Los cambios se guardan solos y la placa los toma enseguida.')
          : tr('Apoyá el dedo en un botón para que suene en la placa. Varios a la vez también.')));
      paintLive();
    };

    /* what sounds, from the app's state */
    const paintLive = () => {
      const on = new Map();
      if (live && live.play) for (const [gi, v, pr] of live.play) on.set(gi, { v, pr });
      padEls.forEach((el, s) => {
        if (!el) return;
        const st = on.get(cur * PER + s);
        el.classList.toggle('on', !!(st && st.v > 0));
        const bar = el.querySelector('.pr');
        if (bar) {
          bar.style.display = st && st.v > 0 ? '' : 'none';
          if (st) bar.firstChild.style.width = (st.pr / 10) + '%';
        }
      });
    };

    /* ---- a button's editor ---- */
    const drawEditor = () => {
      if (!edit || sel < 0 || !pages) { put(editor); return; }
      const b = pages[cur].pads[sel];
      const set = (k, v) => { pages[cur].pads[sel][k] = v; save(); drawBoard(); };
      const fileSel = h('select', {
        onchange: e => {
          const f = e.target.value;
          if (!f) { pages[cur].pads[sel] = null; save(); draw(); return; }
          const old = pages[cur].pads[sel];
          if (!old) {
            pages[cur].pads[sel] = { file: f, name: pretty(f), color: COLORS[(cur * PER + sel) * 5 % COLORS.length], icon: 'note', mode: 'cut', loop: false, vol: 100 };
          } else {
            if (!old.name || old.name === pretty(old.file)) old.name = pretty(f);
            old.file = f;
          }
          save(); draw();
        },
      }, h('option', { value: '' }, tr('(vacío)')),
      files.map(f => h('option', { value: f.name, selected: b && b.file === f.name }, f.name)));
      const head = h('h2', {}, tr('Botón %d', sel + 1));
      if (!b) {
        put(editor, h('div', { class: 'card pad bned' }, head, h('label', { class: 'f' }, tr('Sonido')), fileSel,
          h('p', { class: 'note' }, tr('Elegí un sonido de la tarjeta, o subí uno abajo.')),
          h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => { sel = -1; draw(); } }, tr('Cerrar')))));
        return;
      }
      const missing = !files.some(f => f.name === b.file);
      const swap = d => {
        const t = sel + d;
        if (t < 0 || t >= PER) return;
        const pp = pages[cur].pads;
        [pp[sel], pp[t]] = [pp[t], pp[sel]];
        sel = t; save(); draw();
      };
      const toPage = i => {
        const free = pages[i].pads.indexOf(null);
        if (free < 0) { P.toast(tr('Esa página está llena'), true); return; }
        pages[i].pads[free] = b; pages[cur].pads[sel] = null;
        cur = i; sel = free; save(); draw();
      };
      put(editor, h('div', { class: 'card pad bned' + (missing ? ' bnwarn' : '') }, head,
        h('label', { class: 'f' }, tr('Sonido')), fileSel,
        missing ? h('p', { class: 'note' }, tr('%s no está en la tarjeta.', b.file)) : h('audio', { controls: true, preload: 'none', src: P.fsUrl(`${DIR}/${b.file}`), style: 'width:100%;margin-top:8px' }),
        h('label', { class: 'f' }, tr('Nombre')),
        h('input', { type: 'text', value: b.name, maxlength: 30, oninput: e => { b.name = e.target.value; save(); drawBoard(); } }),
        h('label', { class: 'f' }, tr('Color')),
        h('div', { class: 'bnsw' }, COLORS.map(c => h('button', { class: b.color === c ? 'on' : '', style: 'background:#' + c, title: '#' + c, onclick: () => { set('color', c); drawEditor(); } }))),
        h('label', { class: 'f' }, tr('Ícono')),
        h('div', { class: 'bnic' }, ICONS.map(([k, e]) => h('button', { class: b.icon === k ? 'on' : '', onclick: () => { set('icon', k); drawEditor(); } }, e))),
        h('label', { class: 'f' }, tr('Al tocarlo')),
        h('select', { onchange: e => { set('mode', e.target.value); drawEditor(); } },
          MODES.map(([k, l]) => h('option', { value: k, selected: b.mode === k }, tr(l)))),
        h('p', { class: 'note' }, tr(MODE_HELP[b.mode])),
        h('label', { style: 'display:flex;gap:10px;align-items:center;margin-top:10px' },
          h('input', { type: 'checkbox', checked: b.loop, onchange: e => set('loop', e.target.checked) }), tr('Repetir en bucle')),
        h('label', { class: 'f' }, tr('Volumen') + ': ' + b.vol + ' %'),
        h('input', { type: 'range', min: 0, max: 100, value: b.vol, onchange: e => { set('vol', +e.target.value); drawEditor(); } }),
        h('div', { class: 'btns', style: 'margin-top:14px' },
          h('button', { class: 'btn', onclick: () => swap(-1), disabled: sel === 0 }, '← ' + tr('Mover')),
          h('button', { class: 'btn', onclick: () => swap(1), disabled: sel === PER - 1 }, tr('Mover') + ' →'),
          pages.length > 1 ? h('select', { style: 'width:auto', onchange: e => e.target.value !== '' && toPage(+e.target.value) },
            h('option', { value: '' }, tr('A otra página...')),
            pages.map((p, i) => i === cur ? null : h('option', { value: i }, p.name))) : null,
          h('button', { class: 'btn red', onclick: () => { pages[cur].pads[sel] = null; save(); draw(); } }, tr('Vaciar')),
          h('button', { class: 'btn', onclick: () => { sel = -1; draw(); } }, tr('Cerrar')))));
    };

    /* ---- the folder ---- */
    const usedBy = name => {
      const out = [];
      pages && pages.forEach(p => p.pads.forEach(b => { if (b && b.file === name) out.push(b.name); }));
      return out;
    };
    const assign = name => {
      if (!pages) return;
      let s = edit && sel >= 0 && !pages[cur].pads[sel] ? sel : pages[cur].pads.indexOf(null);
      if (s < 0) { P.toast(tr('No hay botones libres en esta página'), true); return; }
      pages[cur].pads[s] = { file: name, name: pretty(name), color: COLORS[(cur * PER + s) * 5 % COLORS.length], icon: 'note', mode: 'cut', loop: false, vol: 100 };
      edit = true; sel = s; save(); draw();
      P.toast(tr('Puesto en el botón %d', s + 1));
    };
    const drawSounds = () => {
      if (!files.length) {
        put(sounds, h('div', { class: 'row muted' }, tr('No hay sonidos en la carpeta sonidos de la tarjeta.')));
        return;
      }
      put(sounds, files.map(f => {
        const used = usedBy(f.name);
        return h('div', { class: 'bnfile' },
          h('div', { class: 'nm' }, f.name, h('div', { class: 'muted small' }, P.fmtBytes(f.size) + (used.length ? ' · ' + tr('en: %s', used.join(', ')) : ''))),
          h('audio', { controls: true, preload: 'none', src: P.fsUrl(`${DIR}/${f.name}`) }),
          h('button', { class: 'btn', onclick: () => assign(f.name) }, tr('A un botón')),
          h('a', { class: 'btn', href: P.fsUrl(`${DIR}/${f.name}`, true) }, tr('Bajar')),
          h('button', { class: 'btn red', onclick: async () => {
            const q = used.length ? tr('%s lo usan %d botones. ¿Borrarlo igual?', f.name, used.length) : tr('¿Borrar %s? No se puede deshacer.', f.name);
            if (!confirm(q)) return;
            try { await P.fsDelete(`${DIR}/${f.name}`); P.toast(tr('Borrado')); loadFiles(); } catch (e) { P.toast(e.message, true); }
          } }, tr('Borrar')));
      }));
    };

    const prog = h('div', { class: 'prog', style: 'display:none' }, h('div'));
    const upload = async list => {
      await P.fsMkdir(DIR).catch(() => {});
      for (const file of list) {
        const m = /\.(wav|mp3)$/i.exec(file.name);
        if (!m) { P.toast(tr('%s no es WAV ni MP3', file.name), true); continue; }
        const name = P.fsSlug(file.name.slice(0, -m[0].length), 60, 'sonido') + m[0].toLowerCase();
        prog.style.display = '';
        try {
          await P.fsPut(`${DIR}/${name}`, file, p => { prog.firstChild.style.width = (p * 100) + '%'; });
          P.toast(tr('Subido: %s', name));
        } catch (e) { P.toast(e.message, true); }
      }
      prog.style.display = 'none';
      loadFiles();
    };
    const picker = h('input', { type: 'file', accept: '.wav,.mp3,audio/wav,audio/mpeg', multiple: true, style: 'display:none',
      onchange: e => { upload([...e.target.files]); e.target.value = ''; } });
    put(upCard,
      h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: () => picker.click() }, tr('Subir sonidos')), picker),
      prog,
      h('p', { class: 'note' }, tr('WAV o MP3, cortos: la placa usa hasta un minuto de cada uno. También se graban desde la app, con el micrófono.')));

    async function loadFiles() {
      try {
        const fl = await P.fsList(DIR);
        if (gone) return;
        files = fl.filter(f => !f.dir && isSound(f.name)).sort((a, b) => a.name.localeCompare(b.name));
      } catch (e) { files = []; }
      drawSounds();
      drawEditor();
    }
    async function loadGrid() {
      try {
        const t = await P.fsText(GRID);
        if (gone) return;
        pages = t == null ? null : parseGrid(t);
      } catch (e) { P.toast(e.message, true); }
      draw();
    }

    function draw() { drawTop(); drawBoard(); drawEditor(); drawSounds(); }

    /* the app's state, while the page is open: a few times a second */
    let busy = false;
    const poll = async () => {
      if (busy || gone || document.hidden) return;
      busy = true;
      let s = null;
      try { s = await P.api(LIVE + '&key=state'); } catch (_) { s = null; }
      busy = false;
      if (gone) return;
      const was = live;
      live = s;
      if (!!was !== !!s || (was && s && (was.front !== s.front || was.rec !== s.rec))) drawTop();
      /* the grid changed on the board (an edit there): read it again,
       * unless it is our own write, or something is being edited here */
      if (s && lastRev !== null && s.rev !== lastRev && Date.now() - savedAt > 3000 && !(edit && sel >= 0)) loadGrid();
      if (s) lastRev = s.rev;
      if (s && !edit && pages && s.page !== cur && s.page < pages.length) { cur = s.page; drawBoard(); }
      paintLive();
    };

    put(main, h('h1', {}, tr('Botonera')), top, board, editor, h('h2', {}, tr('Sonidos')), upCard, sounds);
    draw();
    loadGrid();
    loadFiles();
    const t = setInterval(poll, 300);
    poll();
    return () => { gone = true; clearInterval(t); clearTimeout(saveT); };
  },
});
