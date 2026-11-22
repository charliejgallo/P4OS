/* Estudio's page in the board's portal (docs/PORTAL-PAGES.md): what the
 * app keeps on the card, to listen to, carry and bring.
 *
 *   /estudio/*.json          the projects (the app opens them from Canción)
 *   /estudio/muestras/*.wav  the user's samples (a drum track can play them)
 *   /estudio/export/         the songs exported as WAV and MIDI
 *   /estudio/presets/*.json  the user's own synth sounds
 *
 * The app reads each folder when it lists it, so whatever is uploaded here
 * shows on the board the next time its list opens. The WAVs play straight
 * from the card in an <audio> (the portal answers Range requests); their
 * length comes from the header, read with a Range request of its first
 * bytes, as the Recorder's page does. Texts in Spanish through tr()
 * (web.estudio.lang). */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const ROOT = '/estudio';
const APP_ID = 'aos.estudio';

if (!document.getElementById('css-estudio')) {
  const st = document.createElement('style');
  st.id = 'css-estudio';
  st.textContent = `
.esrow { padding: 12px 18px; border-top: 1px solid var(--line); }
.esrow:first-child { border-top: 0; }
.eshead { display: flex; gap: 12px; align-items: baseline; flex-wrap: wrap; }
.eshead .nm { font-weight: 600; flex: 1; min-width: 140px; overflow-wrap: anywhere; }
.esrow audio { width: 100%; margin-top: 8px; height: 40px; }
.esrow .btns { margin-top: 8px; }
.esup { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.esup .prog { flex: 1; min-width: 120px; }
.essum { display: flex; gap: 24px; flex-wrap: wrap; align-items: baseline; }
.essum b { font-size: 26px; font-variant-numeric: tabular-nums; }
`;
  document.head.append(st);
}

const mmss = s => s == null ? '…'
  : s < 10 ? `${s.toFixed(1)} s`
  : `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;
const pretty = n => n.replace(/\.(json|wav|mid)$/i, '').replace(/_/g, ' ');

/* a WAV's duration from its first bytes: 'fmt ' gives the byte rate,
 * 'data' where the sound starts */
async function duration(path, size) {
  const r = await fetch(P.fsUrl(path), { headers: { Range: 'bytes=0-1023' } });
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
  id: 'estudio',
  name: tr('Estudio'),
  icon: '♫',
  render(main) {
    const { h, put } = P;
    let gone = false;
    const durs = new Map();

    /* one folder as a card: its files, each with its buttons; 'play' puts
     * a player on the WAVs */
    const section = (dir, re, opts) => {
      const box = h('div', { class: 'card' });
      let sig = '';
      const remove = async f => {
        if (!confirm(tr('¿Borrar %s? No se puede deshacer.', f.name))) return;
        try { await P.fsDelete(`${dir}/${f.name}`); P.toast(tr('Borrado')); load(true); }
        catch (e) { P.toast(e.message, true); }
      };
      const draw = files => {
        if (!files.length) { put(box, h('div', { class: 'row muted' }, opts.empty)); return; }
        put(box, files.map(f => {
          const path = `${dir}/${f.name}`;
          const wav = /\.wav$/i.test(f.name);
          const dur = h('span', { class: 'val' }, wav ? mmss(durs.get(path + f.size)) : '');
          if (wav && !durs.has(path + f.size))
            duration(path, f.size).then(s => { durs.set(path + f.size, s); dur.textContent = mmss(s); }).catch(() => { dur.textContent = '?'; });
          return h('div', { class: 'esrow' },
            h('div', { class: 'eshead' },
              h('span', { class: 'nm' }, opts.pretty ? pretty(f.name) : f.name),
              h('span', { class: 'muted small' }, `${P.fmtDate(f.mtime)} · ${P.fmtBytes(f.size)}`), dur),
            wav ? h('audio', { controls: true, preload: 'none', src: P.fsUrl(path) }) : null,
            h('div', { class: 'btns' },
              h('a', { class: 'btn', href: P.fsUrl(path, true), download: f.name }, tr('Bajar')),
              h('button', { class: 'btn red', onclick: () => remove(f) }, tr('Borrar'))));
        }));
      };
      async function load(force) {
        let fl;
        try { fl = await P.fsList(dir); } catch (e) { put(box, h('div', { class: 'row bad' }, e.message)); return; }
        if (gone) return;
        const now = fl.filter(f => !f.dir && !f.name.startsWith('.') && !f.name.endsWith('.part') && re.test(f.name))
          .sort((a, b) => (b.mtime || 0) - (a.mtime || 0) || a.name.localeCompare(b.name));
        const s = now.map(f => f.name + f.size + f.mtime).join('|');
        if (!force && s === sig) return;
        if (!force && [...box.querySelectorAll('audio')].some(a => !a.paused)) return;
        sig = s;
        draw(now);
      }
      /* the upload: several files at once, one after the other */
      let up = null;
      if (opts.accept) {
        const bar = h('progress', { class: 'prog', max: 1, value: 0, style: 'display:none' });
        const input = h('input', { type: 'file', accept: opts.accept, multiple: true, style: 'display:none',
          onchange: async () => {
            const files = [...input.files];
            input.value = '';
            bar.style.display = '';
            try {
              await P.fsMkdir(ROOT).catch(() => {});
              await P.fsMkdir(dir).catch(() => {});
              for (const f of files) {
                const ext = (f.name.match(/\.[a-z0-9]+$/i) || [opts.ext])[0].toLowerCase();
                const name = P.fsSlug(f.name.replace(/\.[a-z0-9]+$/i, ''), 40, 'archivo') + ext;
                if (opts.check && !(await opts.check(f))) { P.toast(tr('%s no es un archivo que el Estudio lea', f.name), true); continue; }
                await P.fsPut(`${dir}/${name}`, f, p => { bar.value = p; });
              }
              P.toast(files.length === 1 ? tr('Subido') : tr('Subidos'));
            } catch (e) { P.toast(e.message, true); }
            bar.style.display = 'none';
            load(true);
          } });
        up = h('div', { class: 'card pad' }, h('div', { class: 'esup' },
          h('button', { class: 'btn pri', onclick: () => input.click() }, opts.upload), bar, input),
          opts.note ? h('p', { class: 'note' }, opts.note) : null);
      }
      return { box, up, load };
    };

    /* a project the app can read: JSON with its "estudio" key */
    const isProject = async f => {
      try { return !!JSON.parse(await f.text()).estudio; } catch (e) { return false; }
    };
    const isWav = async f => {
      const b = new Uint8Array(await f.slice(0, 12).arrayBuffer());
      return String.fromCharCode(...b.slice(0, 4)) === 'RIFF' && String.fromCharCode(...b.slice(8, 12)) === 'WAVE';
    };
    const isPreset = async f => {
      try { return !!JSON.parse(await f.text()).estudio_preset; } catch (e) { return false; }
    };

    const projects = section(ROOT, /\.json$/i, {
      pretty: true, accept: '.json,application/json', ext: '.json', check: isProject,
      upload: tr('Subir proyectos'), empty: tr('Todavía no hay proyectos: el Estudio guarda el suyo solo.'),
      note: tr('En la placa se abren desde Canción → Abrir. Un proyecto lleva sus patrones, la canción, los sonidos y la mezcla; las muestras que use van aparte, en Muestras.') });
    const samples = section(`${ROOT}/muestras`, /\.wav$/i, {
      accept: '.wav,audio/wav,audio/x-wav', ext: '.wav', check: isWav,
      upload: tr('Subir muestras WAV'), empty: tr('No hay muestras. Subí WAV tuyos para tocarlos en la batería.'),
      note: tr('WAV PCM de 8, 16, 24 o 32 bits o en coma flotante, mono o estéreo, de cualquier frecuencia; se usan hasta 12 segundos de cada uno. En la placa: Ritmo → el sonido de la pista.') });
    const exports = section(`${ROOT}/export`, /\.(wav|mid)$/i, {
      empty: tr('Nada exportado todavía. Se exporta desde Canción → Exportar, en WAV o en MIDI.') });
    const presets = section(`${ROOT}/presets`, /\.json$/i, {
      pretty: true, accept: '.json,application/json', ext: '.json', check: isPreset,
      upload: tr('Subir sonidos'), empty: tr('Ninguno todavía. En la placa: Sonido → Guardar este sonido como…') });

    put(main,
      h('h1', {}, tr('Estudio')),
      h('div', { class: 'card pad' },
        h('p', {}, tr('Lo que el Estudio guarda en la tarjeta: los proyectos, tus muestras, lo exportado y tus sonidos del sintetizador.')),
        h('div', { class: 'btns' },
          h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast(tr('Abierto en la placa'))).catch(e => P.toast(e.message, true)) }, tr('Abrir el Estudio en la placa')),
          h('button', { class: 'btn', onclick: () => all(true) }, tr('Actualizar')))),
      h('h2', {}, tr('Proyectos')), projects.up, projects.box,
      h('h2', {}, tr('Muestras')), samples.up, samples.box,
      h('h2', {}, tr('Exportado')), exports.box,
      h('h2', {}, tr('Mis sonidos')), presets.up, presets.box);

    const all = force => { projects.load(force); samples.load(force); exports.load(force); presets.load(force); };
    all(true);
    const t = setInterval(() => all(false), 5000);
    return () => { gone = true; clearInterval(t); };
  },
});
