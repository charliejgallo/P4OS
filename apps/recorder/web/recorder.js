/* The Recorder's page in the board's portal (docs/PORTAL-PAGES.md): the
 * recordings on the card, to listen to, download or delete.
 *
 * They are the WAVs in /recordings, as the app writes them. Each one's
 * duration comes from its header, read with a Range request of its first
 * bytes (the firmware serves ranges since 0.8.0); the player streams the
 * file the same way, so it seeks without downloading it all. The app looks
 * at its folder every two seconds, so a recording deleted here leaves its
 * list too. The list here follows the card every five seconds, while
 * nothing plays. */
const P = window.P4OS;
const DIR = '/recordings';
const APP_ID = 'app.recorder';

if (!document.getElementById('css-recorder')) {
  const st = document.createElement('style');
  st.id = 'css-recorder';
  st.textContent = `
.rcsum { display: flex; gap: 28px; flex-wrap: wrap; align-items: baseline; }
.rcsum b { font-size: 28px; font-variant-numeric: tabular-nums; }
.rcrow { padding: 14px 18px; border-top: 1px solid var(--line); }
.rcrow:first-child { border-top: 0; }
.rchead { display: flex; gap: 12px; align-items: baseline; flex-wrap: wrap; }
.rchead .nm { font-weight: 600; flex: 1; min-width: 140px; }
.rcrow audio { width: 100%; margin-top: 10px; height: 40px; }
.rcrow .btns { margin-top: 8px; }
`;
  document.head.append(st);
}

/* "grabacion_0007.wav" -> "Grabación 0007", as the app shows it */
const pretty = n => {
  let s = n.replace(/\.wav$/i, '').replace(/_/g, ' ');
  s = s.charAt(0).toUpperCase() + s.slice(1);
  return s.replace(/^Grabacion /, 'Grabación ');
};
const when = t => t ? new Date(t * 1000).toLocaleString('es-AR', { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }) : '';
const mmss = s => s == null ? '…' : `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;

/* The WAV's duration from its first bytes: 'fmt ' gives the byte rate,
 * 'data' where the audio starts. A recording cut short may say 0 bytes of
 * data: then whatever is after the header counts. */
async function duration(path, size) {
  const r = await fetch(P.fsUrl(path), { headers: { Range: 'bytes=0-511' } });
  const b = new DataView(await r.arrayBuffer());
  const tag = o => String.fromCharCode(b.getUint8(o), b.getUint8(o + 1), b.getUint8(o + 2), b.getUint8(o + 3));
  if (b.byteLength < 12 || tag(0) !== 'RIFF' || tag(8) !== 'WAVE') return null;
  let o = 12, rate = 0;
  while (o + 8 <= b.byteLength) {
    const id = tag(o), len = b.getUint32(o + 4, true);
    if (id === 'fmt ' && o + 20 <= b.byteLength) rate = b.getUint32(o + 16, true);
    if (id === 'data') {
      const data = len && len <= size - o - 8 ? len : size - o - 8;
      return rate ? data / rate : null;
    }
    o += 8 + len + (len & 1);
  }
  return null;
}

P.registerPage({
  id: 'grabadora',
  name: 'Grabadora',
  icon: '●',
  render(main) {
    const { h, put } = P;
    const sum = h('div', { class: 'card pad' });
    const list = h('div', { class: 'card' });
    const durs = new Map();          /* name + size -> seconds, so a refresh does not ask again */
    let files = [], sig = '', gone = false;

    const summary = () => {
      const total = files.reduce((a, f) => a + (durs.get(f.name + f.size) || 0), 0);
      const bytes = files.reduce((a, f) => a + f.size, 0);
      put(sum,
        h('div', { class: 'rcsum' },
          h('div', {}, h('b', {}, String(files.length)), ' ', h('span', { class: 'muted small' }, files.length === 1 ? 'grabación' : 'grabaciones')),
          h('div', {}, h('b', {}, mmss(total)), ' ', h('span', { class: 'muted small' }, 'en total')),
          h('div', {}, h('b', {}, P.fmtBytes(bytes)), ' ', h('span', { class: 'muted small' }, 'en la tarjeta'))),
        h('div', { class: 'btns', style: 'margin-top:12px' },
          h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast('Abierta en la placa')).catch(e => P.toast(e.message, true)) }, 'Abrir la Grabadora en la placa'),
          h('button', { class: 'btn', onclick: () => load(true) }, 'Actualizar')),
        h('p', { class: 'note' }, 'Se graba desde la placa. Acá se escuchan, se bajan como WAV y se borran; la app ve los cambios sola.'));
    };

    const remove = async f => {
      if (!confirm(`¿Borrar ${pretty(f.name)}? No se puede deshacer.`)) return;
      try { await P.fsDelete(`${DIR}/${f.name}`); P.toast('Borrada'); load(true); }
      catch (e) { P.toast(e.message, true); }
    };

    const draw = () => {
      if (!files.length) {
        put(list, h('div', { class: 'row muted' }, 'Todavía no hay grabaciones. Se graban desde la app en la placa.'));
        summary();
        return;
      }
      put(list, files.map(f => {
        const path = `${DIR}/${f.name}`;
        const dur = h('span', { class: 'val' }, mmss(durs.get(f.name + f.size)));
        if (!durs.has(f.name + f.size))
          duration(path, f.size).then(s => { durs.set(f.name + f.size, s); dur.textContent = mmss(s); summary(); }).catch(() => { dur.textContent = '?'; });
        return h('div', { class: 'rcrow' },
          h('div', { class: 'rchead' },
            h('span', { class: 'nm' }, pretty(f.name)),
            h('span', { class: 'muted small' }, `${when(f.mtime)} · ${P.fmtBytes(f.size)}`), dur),
          h('audio', { controls: true, preload: 'none', src: P.fsUrl(path) }),
          h('div', { class: 'btns' },
            h('a', { class: 'btn', href: P.fsUrl(path, true) }, 'Bajar'),
            h('button', { class: 'btn red', onclick: () => remove(f) }, 'Borrar')));
      }));
      summary();
    };

    /* newest first, like the app; redrawn only when the folder changed, and
     * never under a player that is playing */
    async function load(force) {
      let fl;
      try { fl = await P.fsList(DIR); } catch (e) { put(list, h('div', { class: 'row bad' }, e.message)); return; }
      if (gone) return;
      const now = fl.filter(f => !f.dir && !f.name.startsWith('.') && /\.wav$/i.test(f.name))
        .sort((a, b) => (b.mtime || 0) - (a.mtime || 0) || b.name.localeCompare(a.name));
      const s = now.map(f => f.name + f.size + f.mtime).join('|');
      if (!force && s === sig) return;
      if (!force && [...list.querySelectorAll('audio')].some(a => !a.paused)) return;
      sig = s;
      files = now;
      draw();
    }

    put(main, h('h1', {}, 'Grabadora'), sum, h('h2', {}, 'Grabaciones'), list);
    summary();
    load(true);
    const t = setInterval(load, 5000);
    return () => { gone = true; clearInterval(t); };
  },
});
