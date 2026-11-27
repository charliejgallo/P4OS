/* Grabador's page in the board's portal (apps/grabador, docs/PORTAL-PAGES.md).
 *
 * Through the live channel (/api/live): the app puts "state" once a second
 * while the page looks (how it is, the settings, the clips) and the newest
 * picture as a file (its path in "jpg"); the page sends short texts back
 * (arm, rec, del, set). The clips download straight from the card. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const { h, put, toast, openApp } = P;

const APP = 'aos.grabador';
const STATES = [N_('Apagado'), N_('Sin cámara'), N_('Arrancando la cámara…'), N_('Vigilando'), N_('Grabando'), N_('Con problemas')];

const CSS = `
.grlive{position:relative;max-width:720px;background:#111;border-radius:14px;overflow:hidden}
.grlive img{width:100%;display:block}
.grrec{position:absolute;top:10px;left:12px;color:#ff453a;font-weight:700;text-shadow:0 1px 3px #000}
.grbar{position:absolute;left:12px;right:12px;bottom:10px;height:8px;border-radius:4px;background:rgba(0,0,0,.5)}
.grbar i{display:block;height:100%;border-radius:4px;background:#ffd60a}
.grclips{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px}
.grclip{background:var(--card);border-radius:12px;overflow:hidden}
.grclip img{width:100%;aspect-ratio:4/3;object-fit:cover;background:#111;display:block}
.grclip div{padding:8px 10px;display:flex;justify-content:space-between;align-items:center;gap:6px}
.grset{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:10px 16px}
`;
function style() {
  if (document.getElementById('grcss')) return;
  document.head.append(h('style', { id: 'grcss' }, CSS));
}

const send = line => fetch('/api/live?app=' + APP, { method: 'POST', body: line }).then(r => { if (!r.ok) throw new Error(tr('La app no está abierta')); });
const fsUrl = (p, dl) => '/api/fs/get?path=' + encodeURIComponent(p) + (dl ? '&attachment=1' : '');

P.registerPage({
  id: 'grabador',
  name: tr('Grabador'),
  icon: '◉',
  render(main) {
    style();
    let timer = 0, gone = false, last = null, clipsKey = '';
    const img = h('img', { alt: '' });
    const rec = h('div', { class: 'grrec' });
    const fill = h('i', { style: 'width:0' });
    const live = h('div', { class: 'grlive' }, img, rec, h('div', { class: 'grbar' }, fill));
    const line = h('p', { class: 'mid' }), note = h('p', { class: 'note' });
    const btns = h('div', { class: 'btns' });
    const clips = h('div', { class: 'grclips' });
    const set = h('div', { class: 'grset' });
    const sel = (label, key, opts, val) => {
      const s = h('select', { onchange: () => send(`set ${key} ${s.value}`).catch(e => toast(e.message, true)) },
        opts.map(([v, t]) => h('option', { value: v }, tr(t))));
      s.value = String(val);
      return h('label', {}, tr(label), s);
    };
    const chk = (label, key, val) => h('label', {}, h('input', { type: 'checkbox', checked: !!val,
      onchange: e => send(`set ${key} ${e.target.checked ? 1 : 0}`).catch(x => toast(x.message, true)) }), ' ' + tr(label));
    let setKey = '';
    function drawSet(c) {
      const k = JSON.stringify(c);
      if (k === setKey) return;
      setKey = k;
      const hours = [...Array(24).keys()].map(x => [x, String(x).padStart(2, '0')]);
      put(set,
        sel(N_('Tamaño'), 'res', [[0, '640 x 480'], [1, '1280 x 720']], c.res),
        sel(N_('Cuadros por segundo'), 'fps', [[5, '5'], [10, '10'], [15, '15']], c.fps),
        sel(N_('Calidad'), 'kbps', [[800, N_('Baja (0,8 Mbit/s)')], [1500, N_('Media (1,5 Mbit/s)')], [3000, N_('Alta (3 Mbit/s)')], [5000, N_('Muy alta (5 Mbit/s)')]], c.kbps),
        sel(N_('Sensibilidad al movimiento'), 'sens', [[0, N_('Baja')], [1, N_('Media')], [2, N_('Alta')]], c.sens),
        sel(N_('Grabar desde antes del movimiento'), 'pre_s', [[0, N_('Nada')], [2, N_('2 segundos')], [3, N_('3 segundos')], [5, N_('5 segundos')]], c.pre_s),
        sel(N_('Seguir grabando después'), 'post_s', [[5, N_('5 segundos')], [10, N_('10 segundos')], [20, N_('20 segundos')], [30, N_('30 segundos')], [60, N_('1 minuto')]], c.post_s),
        sel(N_('Cortar las grabaciones cada'), 'clip_max_s', [[60, N_('1 minuto')], [300, N_('5 minutos')], [600, N_('10 minutos')]], c.clip_max_s),
        sel(N_('Espacio para grabaciones'), 'space_mb', [[512, '512 MB'], [2048, '2 GB'], [8192, '8 GB'], [32768, '32 GB']], c.space_mb),
        h('div', {}, chk(N_('Sólo a ciertas horas'), 'sched', c.sched),
          c.sched ? h('div', { class: 'btns' }, sel(N_('de'), 'from_h', hours, c.from_h), sel(N_('a'), 'to_h', hours, c.to_h)) : null),
        h('div', {}, chk(N_('Avisar por el bot de Telegram'), 'notify', c.notify)),
        h('div', {}, chk(N_('Cámara IP por RTSP (Frigate, VLC, Cámaras)'), 'rtsp', c.rtsp),
          c.rtsp ? h('div', { class: 'note' }, `rtsp://${last?.device || 'p4os'}.local:${c.rtsp_port}/`) : null));
    }
    function draw(s) {
      const name = tr(STATES[s.state] || '');
      put(line, h('b', {}, s.state === 4 ? (s.manual ? tr('Grabando a mano') : tr('Grabando: hay movimiento'))
        : s.state === 3 ? (s.cfg.armed ? tr('Vigilando') : tr('Mirando (desarmado)')) : name),
        s.why ? ' · ' + s.why : '');
      put(note, s.camera ? `${s.camera} · ${s.w}x${s.h} · ${s.fps.toFixed(1)} ` + tr('cuadros/s') + ` · ${s.kbps} kbit/s`
        + (s.cfg.rtsp ? ' · ' + tr('%s mirando por RTSP', s.viewers) : '') : '');
      rec.textContent = s.state === 4 ? `● ${Math.floor(s.rec_ms / 60000)}:${String(Math.floor(s.rec_ms / 1000) % 60).padStart(2, '0')}` : '';
      fill.style.width = Math.min(100, s.level * 800) + '%';
      fill.style.background = s.moving ? '#ff453a' : '#ffd60a';
      put(btns,
        h('button', { class: 'btn' + (s.cfg.armed ? '' : ' pri'), onclick: () => send('arm ' + (s.cfg.armed ? 0 : 1)).catch(e => toast(e.message, true)) },
          s.cfg.armed ? tr('Desarmar') : tr('Armar')),
        h('button', { class: 'btn', style: 'background:#ff453a;color:#fff', onclick: () => send('rec ' + (s.manual ? 0 : 1)).catch(e => toast(e.message, true)) },
          s.manual ? tr('Detener') : tr('Grabar ahora')));
      drawSet(s.cfg);
      const key = JSON.stringify(s.clips);
      if (key !== clipsKey) {
        clipsKey = key;
        put(clips, s.clips.length ? s.clips.map(c => {
          const base = s.dir + '/' + c.name;
          const n = c.name;
          return h('div', { class: 'grclip' },
            h('a', { href: fsUrl(base), target: '_blank' }, h('img', { src: fsUrl(base.replace(/\.mp4$/, '.jpg')), alt: '', loading: 'lazy', onerror: e => { e.target.style.visibility = 'hidden'; } })),
            h('div', {}, h('span', {}, `${n.slice(6, 8)}/${n.slice(4, 6)} ${n.slice(9, 11)}:${n.slice(11, 13)}:${n.slice(13, 15)} · ${(c.bytes / 1048576).toFixed(1)} MB`),
              h('span', { class: 'btns' },
                h('a', { class: 'btn', href: fsUrl(base, true) }, tr('Bajar')),
                h('button', { class: 'btn', onclick: () => { if (confirm(tr('¿Borrar esta grabación?'))) send('del ' + n).catch(e => toast(e.message, true)); } }, tr('Borrar')))));
        }) : h('p', { class: 'note' }, tr('Todavía no hay grabaciones. Armá la cámara: graba cuando algo se mueve.')));
      }
    }
    async function poll() {
      if (gone) return;
      try {
        const r = await fetch(`/api/live?app=${APP}&key=state`, { cache: 'no-store' });
        if (r.ok) {
          last = await r.json();
          draw(last);
          img.src = fsUrl(last.jpg) + '&t=' + Date.now();
        } else {
          put(line, tr('La app no está abierta en la placa.'), ' ', h('button', { class: 'btn', onclick: () => openApp(APP) }, tr('Abrirla')));
        }
      } catch {}
      timer = setTimeout(poll, 1000);
    }
    put(main, h('h1', {}, tr('Grabador')), live, line, note, btns,
      h('h2', {}, tr('Grabaciones')), clips, h('h2', {}, tr('Ajustes')), set);
    poll();
    return () => { gone = true; clearTimeout(timer); };
  },
});
