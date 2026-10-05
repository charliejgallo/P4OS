/* The CAN app's page in the board's portal (docs/PORTAL-PAGES.md): the spy
 * live, sending, the recordings, and the files the app reads.
 *
 * The page and the app meet in two files of /data, with no firmware of
 * their own:
 *   /data/can_cmd.txt    the page's orders, a line each: "watch",
 *                        "send 123#1122", "rec on", "rec off", "clear",
 *                        "reload". The app reads it within a second and
 *                        deletes it; the page writes the next batch only
 *                        once it is gone.
 *   /data/can_live.json  the app's snapshot: the state, every id with its
 *                        last data and the bytes that just changed, the
 *                        last frames. Written once a second for 15 s after
 *                        each "watch", which the page repeats while open.
 * So the app must be open on the board (in front or not); the bus is
 * opened there. The recordings are the CSVs in /can (SavvyCAN's format),
 * the saved frames /can/enviar.txt and the signals /can/senales.dbc. */
const P = window.P4OS;
const APP_ID = 'aos.can';
const CMD = '/data/can_cmd.txt';
const LIVE = '/data/can_live.json';
const DIR = '/can';
const SAVED = DIR + '/enviar.txt';
const DBC = DIR + '/senales.dbc';

if (!document.getElementById('css-can')) {
  const st = document.createElement('style');
  st.id = 'css-can';
  st.textContent = `
.cnsum { display: flex; gap: 22px; flex-wrap: wrap; align-items: baseline; }
.cnsum b { font-size: 24px; font-variant-numeric: tabular-nums; }
.cndot { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; background: var(--dim); }
.cntbl { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
.cntbl th { text-align: left; font-weight: 500; color: var(--dim); font-size: 13px; padding: 8px 12px; }
.cntbl td { padding: 6px 12px; border-top: 1px solid var(--line); white-space: nowrap; }
.cntbl td.num { text-align: right; color: var(--dim); }
.cntbl .id { color: var(--accent); }
.cnb { display: inline-block; min-width: 2.2ch; padding: 1px 3px; margin-right: 3px; border-radius: 4px; }
.cnb.hot { background: var(--orange); color: #000; }
.cnlast { max-height: 320px; overflow: auto; padding: 8px 14px; line-height: 1.55; white-space: pre; word-break: normal; }
.cnlast .tx { color: var(--accent); }
.cnin { flex: 1; min-width: 180px; font: inherit; padding: 9px 12px; border-radius: 10px; border: 1px solid var(--line); background: var(--card2); color: inherit; }
.cnedit { width: 100%; min-height: 180px; box-sizing: border-box; border: 0; border-top: 1px solid var(--line); padding: 12px 14px; background: transparent; color: inherit; resize: vertical; }
.cnscroll { overflow-x: auto; }
.cnwarn { color: var(--orange); }
`;
  document.head.append(st);
}

const STATE = { active: ['activo', 'var(--green)'], warning: ['con avisos', '#ffd60a'], passive: ['pasivo', 'var(--orange)'], bus_off: ['fuera del bus', 'var(--red)'] };
const MODE = { listen: 'Solo escucha', normal: 'Normal', selftest: 'Autoprueba' };
const rate = r => r % 1000000 === 0 ? `${r / 1000000} Mbit/s` : r % 1000 === 0 ? `${r / 1000} kbit/s` : `${r} bit/s`;
const when = t => t ? new Date(t * 1000).toLocaleString('es-AR', { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }) : '';
/* candump's form: 123#11223344, 18FEF100#FF00, 7DF#R */
const FRAME_RE = /^([0-9a-f]{1,8})#(r[0-8]?|([0-9a-f]{2}[.:]?){0,8})$/i;

P.registerPage({
  id: 'can',
  name: 'CAN',
  icon: '⇆',
  render(main) {
    const { h, put } = P;
    const sum = h('div', { class: 'card pad' });
    const ids = h('div', { class: 'card cnscroll' });
    const last = h('div', { class: 'card mono cnlast' });
    const send = h('div', { class: 'card pad' });
    const recs = h('div', { class: 'card' });
    const files = h('div', { class: 'card' });
    let live = null, lastN = -1, staleFor = 0, gone = false;
    let queue = [], pending = false;

    /* ---- orders to the app ---- */
    const order = line => { if (!queue.includes(line) || line.startsWith('send')) queue.push(line); pump(); };
    async function pump() {
      if (pending || !queue.length || gone) return;
      pending = true;
      try {
        await P.fsMkdir('/data').catch(() => {});
        while (!gone && await P.fsText(CMD) !== null) await new Promise(r => setTimeout(r, 400));
        const batch = queue.splice(0, 16);
        await P.fsPut(CMD, batch.join('\n') + '\n');
      } catch (e) { P.toast(e.message, true); }
      pending = false;
      if (queue.length) setTimeout(pump, 300);
    }

    /* ---- the state ---- */
    const openBtn = () => h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast('Abierta en la placa')).catch(e => P.toast(e.message, true)) }, 'Abrir CAN en la placa');
    /* the numbers change every second, the buttons only with the state: they
     * are redrawn apart, so a click is not lost under a redraw */
    const sumInfo = h('div', {}), sumBtns = h('div', { class: 'btns', style: 'margin-top:12px' });
    put(sum, sumInfo, sumBtns);
    let btnKey = '';
    function drawSummary() {
      const stale = !live || staleFor >= 5;
      const key = stale ? 'stale' : `${live.open}|${!!live.rec}`;
      if (key !== btnKey) {
        btnKey = key;
        put(sumBtns,
          !stale && live.open && (live.rec
            ? h('button', { class: 'btn red', onclick: () => order('rec off') }, 'Parar la grabación')
            : h('button', { class: 'btn', onclick: () => order('rec on') }, 'Grabar')),
          !stale && live.open && h('button', { class: 'btn', onclick: () => order('clear') }, 'Limpiar'),
          openBtn());
      }
      if (stale) {
        put(sumInfo,
          h('div', { class: 'row', style: 'padding:0' }, h('span', { class: 'cndot' }), h('div', { class: 'grow' }, live ? 'La app dejó de contestar.' : 'Esperando a la app…')),
          h('p', { class: 'note' }, 'La página ve el bus a través de la app CAN: tiene que estar abierta en la placa (al frente o atrás). El bus se conecta desde ahí.'));
        return;
      }
      const L = live;
      const [sname, scolor] = L.open ? (STATE[L.state] || [L.state, 'var(--red)']) : ['desconectado', 'var(--dim)'];
      const head = L.open
        ? `${rate(L.rate)} · ${MODE[L.mode] || L.mode} · ${L.tx === L.rx ? 'GPIO' + L.tx : `TX ${L.tx} · RX ${L.rx}`}`
        : 'Sin bus: se conecta desde la app, tocando la conexión.';
      put(sumInfo,
        h('div', { class: 'row', style: 'padding:0' }, h('span', { class: 'cndot', style: `background:${scolor}` }), h('div', { class: 'grow' }, h('b', {}, sname), ' · ', head)),
        L.open && h('div', { class: 'cnsum', style: 'margin-top:10px' },
          h('div', {}, h('b', {}, String(Math.round(L.fps))), ' ', h('span', { class: 'muted small' }, 'tramas/s')),
          h('div', {}, h('b', {}, Math.round(L.load * 100) + ' %'), ' ', h('span', { class: 'muted small' }, 'de carga')),
          h('div', {}, h('b', {}, String(L.rxn)), ' ', h('span', { class: 'muted small' }, 'recibidas')),
          h('div', {}, h('b', {}, String(L.txn)), ' ', h('span', { class: 'muted small' }, 'enviadas')),
          h('div', {}, h('b', {}, String(L.drop)), ' ', h('span', { class: 'muted small' }, 'perdidas')),
          h('div', {}, h('b', {}, `${L.txe} / ${L.rxe}`), ' ', h('span', { class: 'muted small' }, 'errores TX / RX'))),
        L.rec && h('p', { class: 'note' }, `Grabando ${L.rec}: ${L.recn} tramas.`),
        L.replay && h('p', { class: 'note' }, `Repitiendo ${L.replay}.`));
    }

    function drawSpy() {
      if (!live || !live.ids?.length) {
        put(ids, h('div', { class: 'row muted' }, live?.open ? 'Esperando tramas…' : 'Sin tramas.'));
        put(last);
        return;
      }
      put(ids, h('table', { class: 'cntbl' },
        h('tr', {}, h('th', {}, 'id'), h('th', {}, 'n'), h('th', {}, 'datos'), h('th', { style: 'text-align:right' }, 'por s'), h('th', { style: 'text-align:right' }, 'vistas')),
        live.ids.slice().sort((a, b) => a[1] - b[1] || parseInt(a[0], 16) - parseInt(b[0], 16)).map(([id, ext, len, data, recent, hz, n]) => h('tr', {},
          h('td', { class: 'mono id' }, id + (ext ? ' ·29' : '')),
          h('td', { class: 'num' }, String(len)),
          h('td', { class: 'mono' }, data.split(' ').filter(Boolean).map((b, i) => h('span', { class: 'cnb' + (recent >> i & 1 ? ' hot' : '') }, b))),
          h('td', { class: 'num' }, hz.toFixed(1)),
          h('td', { class: 'num' }, String(n))))));
      put(last, live.last.slice().reverse().map(([t, id, fl, data]) =>
        h('div', { class: fl & 4 ? 'tx' : '' }, `${(t / 1000).toFixed(3).padStart(9)}  ${fl & 4 ? '>' : ' '} ${id.padEnd(8)}  ${data}`)));
    }

    async function poll() {
      let t = null;
      try { t = await P.fsText(LIVE); } catch { /* not there yet */ }
      if (gone) return;
      let j = null;
      try { j = t ? JSON.parse(t) : null; } catch { /* caught half written: next time */ }
      if (j && j.n !== lastN) { live = j; lastN = j.n; staleFor = 0; }
      else staleFor++;
      drawSummary();
      drawSpy();
      drawSendHint();
    }

    /* ---- sending ---- */
    const input = h('input', { class: 'cnin mono', placeholder: '7DF#02010C', spellcheck: false });
    const hint = h('p', { class: 'note' });
    const savedList = h('div', {});
    function drawSendHint() {
      const m = live && staleFor < 5 ? (live.open ? live.mode : 'off') : 'none';
      hint.className = 'note' + (m === 'normal' ? '' : ' cnwarn');
      hint.textContent = {
        none: 'La app no está contestando: lo que mandes se manda cuando vuelva.',
        off: 'El bus está desconectado en la app.',
        listen: 'La app está en Solo escucha: no sale nada. El modo se cambia en la placa.',
        selftest: 'Autoprueba: vuelve sólo a la placa, no sale al bus.',
        normal: 'Normal: sale al bus. Nunca al de un auto en marcha.',
      }[m];
    }
    const sendOne = fr => {
      fr = fr.trim();
      if (!FRAME_RE.test(fr)) { P.toast('Como en candump: 123#11223344, 18FEF100#FF00 o 7DF#R', true); return; }
      order('send ' + fr);
      P.toast('Mandada a la app: ' + fr);
    };
    function drawSaved(text) {
      const rows = (text || '').split('\n').map(l => l.trim()).filter(l => l && !l.startsWith('#'))
        .map(l => { const [fr, per, ...name] = l.split(/\s+/); return { fr, per: +per || 0, name: name.join(' ') }; })
        .filter(r => FRAME_RE.test(r.fr));
      put(savedList, rows.length ? rows.map(r => h('div', { class: 'row' },
        h('div', { class: 'grow' }, h('span', { class: 'mono' }, r.fr), ' ', h('span', { class: 'muted small' }, [r.name, r.per ? `cada ${r.per} ms (las periódicas se arrancan en la placa)` : ''].filter(Boolean).join(' · '))),
        h('button', { class: 'btn', onclick: () => sendOne(r.fr) }, 'Mandar'))) : h('div', { class: 'row muted' }, 'Sin tramas guardadas.'));
    }
    put(send,
      h('div', { class: 'row', style: 'gap:10px;flex-wrap:wrap' }, input,
        h('button', { class: 'btn pri', onclick: () => sendOne(input.value) }, 'Mandar')),
      hint,
      h('h3', {}, 'Guardadas en la placa'),
      savedList);
    input.addEventListener('keydown', e => { if (e.key === 'Enter') sendOne(input.value); });

    /* ---- the recordings ---- */
    async function loadRecs() {
      let fl;
      try { fl = await P.fsList(DIR); } catch (e) { put(recs, h('div', { class: 'row bad' }, e.message)); return; }
      if (gone) return;
      const csv = fl.filter(f => !f.dir && !f.name.startsWith('.') && /\.csv$/i.test(f.name))
        .sort((a, b) => b.name.localeCompare(a.name));
      put(recs, csv.length ? csv.map(f => h('div', { class: 'row' },
        h('div', { class: 'grow' }, h('span', { class: 'mono' }, f.name), ' ', h('span', { class: 'muted small' }, `${when(f.mtime)} · ${P.fmtBytes(f.size)}`)),
        h('div', { class: 'btns' },
          h('a', { class: 'btn', href: P.fsUrl(`${DIR}/${f.name}`, true) }, 'Bajar'),
          h('button', { class: 'btn red', onclick: async () => {
            if (live?.rec === f.name) { P.toast('Se está grabando: pararla primero', true); return; }
            if (!confirm(`¿Borrar ${f.name}? No se puede deshacer.`)) return;
            try { await P.fsDelete(`${DIR}/${f.name}`); P.toast('Borrada'); loadRecs(); } catch (e) { P.toast(e.message, true); }
          } }, 'Borrar'))))
        : h('div', { class: 'row muted' }, 'Todavía no hay grabaciones: se graba desde la placa o con el botón de arriba.'));
    }

    /* ---- the files the app reads ---- */
    function editor(path, title, about, after) {
      const ta = h('textarea', { class: 'cnedit mono', spellcheck: false });
      const box = h('div', {},
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('b', {}, title), h('div', { class: 'muted small' }, about)),
          h('button', { class: 'btn', onclick: async () => {
            try {
              await P.fsMkdir(DIR).catch(() => {});
              await P.fsPut(path, ta.value.endsWith('\n') ? ta.value : ta.value + '\n');
              order('reload');
              P.toast('Guardado: la app lo vuelve a leer');
              after?.(ta.value);
            } catch (e) { P.toast(e.message, true); }
          } }, 'Guardar')),
        ta);
      P.fsText(path).then(t => { ta.value = t ?? ''; after?.(t); }).catch(e => P.toast(e.message, true));
      return box;
    }
    put(files,
      editor(SAVED, 'Tramas guardadas', 'can/enviar.txt: una por línea, la trama, el período en ms (0 a mano) y un nombre.', drawSaved),
      editor(DBC, 'Señales', 'can/senales.dbc: un .dbc (BO_ y SG_; lo demás se saltea). La app las decodifica y grafica.'));

    put(main,
      h('h1', {}, 'CAN'), sum,
      h('h2', {}, 'Por id'), ids,
      h('h2', {}, 'Últimas tramas'), last,
      h('h2', {}, 'Mandar'), send,
      h('h2', {}, 'Grabaciones'), recs,
      h('h2', {}, 'Archivos de la app'), files);

    drawSummary();
    drawSendHint();
    order('watch');
    poll();
    loadRecs();
    const tPoll = setInterval(poll, 1000);
    const tWatch = setInterval(() => order('watch'), 5000);
    const tRecs = setInterval(loadRecs, 10000);
    return () => { gone = true; clearInterval(tPoll); clearInterval(tWatch); clearInterval(tRecs); };
  },
});
