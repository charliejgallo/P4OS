/* The Radio app's page in the board's portal (docs/PORTAL-PAGES.md), from
 * AmoledOS' radio.html: what plays on the board and its controls, the nine
 * keys (drag a station onto one, or a key onto another to swap them), "my
 * list" of stations, a search in radio-browser.info's open directory, and
 * adding one by hand. It speaks /api/radio (the keys, what plays: firmware,
 * components/aos_portal/aos_portal_radio.c) and the card's files: the list
 * is /radio/library.json and each key's logo /radio/logoN.jpg, a 160 px JPEG
 * drawn here in the browser (through wsrv.nl, a public image proxy, when a
 * station's site does not let its logo be read). The search and "listen
 * here" are the browser's own, straight to radio-browser and the stream. */
const P = window.P4OS;
const { main, h, put, api, post, toast, fsText, fsPut, fsDelete, fsMkdir, fsUrl, openApp } = P;

if (!document.getElementById('css-radio')) {
  const st = document.createElement('style');
  st.id = 'css-radio';
  st.textContent = `
.rdnow { display: grid; grid-template-columns: 72px 1fr; gap: 16px; align-items: center; padding: 18px 20px; }
.rdnow .lg { width: 72px; height: 72px; border-radius: 12px; object-fit: cover; background: var(--card2); }
.rdnow .st { font-weight: 650; font-size: 18px; }
.rdnow .ti { font-size: 15px; }
.rdnow .de { color: var(--dim); font-size: 13px; }
.rdctl { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; padding: 0 20px 18px; }
.rdctl .btn { min-width: 46px; }
.rdctl input[type=range] { flex: 1; min-width: 140px; }
.rdkeys { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; }
.rdkey { background: var(--card); border-radius: 14px; padding: 12px; display: grid; grid-template-columns: 52px 1fr; gap: 6px 12px;
  align-items: center; min-height: 76px; border: 2px solid transparent; }
.rdkey.over { border-color: var(--accent); }
.rdkey.on { border-color: var(--orange); }
.rdkey .n { width: 52px; height: 52px; border-radius: 10px; background: var(--card2); display: grid; place-items: center; font-weight: 700;
  color: var(--dim); overflow: hidden; }
.rdkey .n img { width: 52px; height: 52px; object-fit: cover; }
.rdkey .nm { font-weight: 600; font-size: 14.5px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.rdkey.free .nm { color: var(--dim); font-weight: 400; }
.rdkey .ac { grid-column: 1 / -1; display: flex; gap: 6px; }
.rdkey .ac .btn { padding: 4px 11px; font-size: 13px; }
@media (max-width: 700px) { .rdkeys { grid-template-columns: repeat(2, 1fr); } }
.rdlist .row { flex-wrap: wrap; gap: 4px 12px; }
.rdlist .lg { width: 36px; height: 36px; border-radius: 8px; object-fit: cover; background: var(--card2); flex: none; display: inline-block; }
.rdlist .nm { font-weight: 600; }
.rdlist .in { color: var(--dim); font-size: 12.5px; flex: 1 1 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.rdlist .ac { margin-left: auto; display: flex; gap: 6px; align-items: center; }
.rdlist .ac .btn, .rdlist .ac select { padding: 4px 11px; font-size: 13px; width: auto; }
.rdlist .row[draggable=true] { cursor: grab; }
.rdtag { font-size: 11px; padding: 1px 8px; border-radius: 9px; background: var(--card2); color: var(--dim); }
.rdtag.no { background: #5a1f1f; color: #ffb4a8; }
.rdsearch { display: grid; grid-template-columns: 2fr 1fr 1fr auto; gap: 10px; align-items: end; padding: 18px 20px; }
.rdhand { display: grid; grid-template-columns: 1fr 2fr auto; gap: 10px; align-items: end; padding: 18px 20px; }
.rdsearch label, .rdhand label { font-size: 13px; color: var(--dim); display: grid; gap: 5px; }
@media (max-width: 700px) { .rdsearch { grid-template-columns: 1fr 1fr; } .rdhand { grid-template-columns: 1fr; } }
`;
  document.head.append(st);
}

/* The list the page starts with, before anything is saved: stations checked
 * against the stream code on the watch on 2026-09-26 (the same on the P4). */
const DEFAULTS = [
  { name: "Metro 95.1", url: "https://playerservices.streamtheworld.com/api/livestream-redirect/METRO.mp3", favicon: "https://cdn-profiles.tunein.com/s25987/images/logog.jpg", cc: "AR", tags: "95.1, 95.1 fm, argentina, buenos aires", codec: "MP3", kbps: 96 },
  { name: "Aspen 102.3", url: "https://playerservices.streamtheworld.com/api/livestream-redirect/ASPEN.mp3", favicon: "https://fmaspen.com/wp-content/themes/aspen/images/logoaspen.png", cc: "AR", tags: "", codec: "MP3", kbps: 96 },
  { name: "La 100", url: "https://playerservices.streamtheworld.com/api/livestream-redirect/FM999_56.mp3", favicon: "http://cdn-profiles.tunein.com/s6984/images/logod.jpg", cc: "AR", tags: "99.9 fm, argentina, buenos aires", codec: "MP3", kbps: 96 },
  { name: "Rock & Pop 95.9", url: "https://playerservices.streamtheworld.com/api/livestream-redirect/ROCKANDPOP.mp3", favicon: "https://fmrockandpop.com/images/ryp/logo_2022.webp", cc: "AR", tags: "classic hits, hits, music, pop", codec: "MP3", kbps: 96 },
  { name: "Radio Rivadavia", url: "https://playerservices.streamtheworld.com/api/livestream-redirect/RIVADAVIA.mp3", favicon: "https://rivadavia.com.ar/apple-touch-icon.png", cc: "AR", tags: "", codec: "MP3", kbps: 96 },
  { name: "Radio Disney Argentina", url: "https://playerservices.streamtheworld.com/api/livestream-redirect/DISNEY_ARG_BA.mp3", favicon: "https://ar.radiodisney.com/favicon.ico", cc: "AR", tags: "", codec: "MP3", kbps: 128 },
  { name: "FM Blackie 89.1", url: "https://playerservices.streamtheworld.com/api/livestream-redirect/BLACKIE_89_1.mp3", favicon: "https://fmblackie.com.ar/wp-content/themes/blackie/images/logo.png", cc: "AR", tags: "jazz, music", codec: "MP3", kbps: 96 },
  { name: "Nacional Rock 93.7", url: "https://sa.mp3.icecast.magma.edge-access.net/sc_rad39", favicon: "", cc: "AR", tags: "", codec: "MP3", kbps: 192 },
  { name: "Nacional Folklórica 98.7", url: "https://sa.mp3.icecast.magma.edge-access.net/sc_rad38", favicon: "https://www.radionacional.com.ar/reproductor/images/Logo_Folklorica.png", cc: "AR", tags: "folklore", codec: "MP3", kbps: 160 },
  { name: "Nacional Clásica 96.7", url: "https://sa.mp3.icecast.magma.edge-access.net/sc_rad37", favicon: "https://radioarg.net/storage/radios/7853/73050/conversions/SBpKi6a8TTFvVnHfz7P89UvhnfltzR-metabmFjaW9uYWwtY2xhc2ljYS5qcGc=--lg.webp", cc: "AR", tags: "", codec: "MP3", kbps: 192 },
  { name: "Radio Nacional AM 870", url: "https://sa.mp3.icecast.magma.edge-access.net/sc_rad1", favicon: "https://upload.wikimedia.org/wikipedia/commons/0/00/Nacional_AM_870_2016.png", cc: "AR", tags: "argentina, buenos aires", codec: "MP3", kbps: 56 },
  { name: "La Popu", url: "https://liveradio.mediainbox.net/popular.mp3", favicon: "https://www.lapopu.com.ar/img/favicon.png", cc: "AR", tags: "cuarteto, local news, music", codec: "MP3", kbps: 96 },
  { name: "La Nación +Música", url: "https://stream.radio.co/s2ed3bec0a/listen", favicon: "https://masmusica.lanacion.com.ar/assets/img/favicon.png", cc: "AR", tags: "adult contemporary, old hits", codec: "MP3", kbps: 128 },
  { name: "Radio Mitre", url: "http://playerservices.streamtheworld.com/api/livestream-redirect/AM790_56AAC_SC", favicon: "https://cloudfront-arc.cienradios.com/radiomitre/favicons/apple-icon-120x120.png", cc: "AR", tags: "news", codec: "AAC+", kbps: 64 },
  { name: "Radio 10", url: "https://radio10.stweb.tv/radio10/live/playlist.m3u8", favicon: "https://www.radio10.com.ar/css-custom/220/favicons/apple-touch-icon-120x120.png", cc: "AR", tags: "news, noticias", codec: "AAC", kbps: 65, hls: true },
  { name: "Radio Con Vos 89.9", url: "https://server1.stweb.tv/rcvos/live/playlist.m3u8", favicon: "", cc: "AR", tags: "news, pop, rock", codec: "AAC", kbps: 49, hls: true },
  { name: "La Red AM 910", url: "https://playerservices.streamtheworld.com/api/livestream-redirect/LA_RED_AM910AAC.aac", favicon: "", cc: "AR", tags: "", codec: "AAC+", kbps: 64 },
  { name: "El Destape", url: "https://ipanel.instream.audio/8004/stream", favicon: "https://www.eldestapeweb.com/img/favicons/apple-icon-120x120.png", cc: "AR", tags: "noticias locales", codec: "AAC", kbps: 48 },
  { name: "SomaFM Groove Salad", url: "https://ice1.somafm.com/groovesalad-128-mp3", favicon: "https://somafm.com/img3/groovesalad-400.jpg", cc: "US", tags: "ambient, chillout, downtempo, groove", codec: "MP3", kbps: 128 },
  { name: "SomaFM Drone Zone", url: "https://ice1.somafm.com/dronezone-128-mp3", favicon: "https://somafm.com/img3/dronezone-400.jpg", cc: "US", tags: "ambient, atmospheric, chillout, drone", codec: "MP3", kbps: 128 },
  { name: "SomaFM Secret Agent", url: "https://ice1.somafm.com/secretagent-128-mp3", favicon: "https://somafm.com/img3/secretagent-400.jpg", cc: "US", tags: "ambient, downtempo, jazz, lounge", codec: "MP3", kbps: 128 },
  { name: "Radio Paradise", url: "http://stream.radioparadise.com/mp3-128", favicon: "https://radioparadise.com/apple-touch-icon.png", cc: "US", tags: "california, eclectic, free, internet", codec: "MP3", kbps: 128 },
  { name: "FIP", url: "https://icecast.radiofrance.fr/fip-midfi.mp3", favicon: "https://www.fip.fr/dist/favicons/logo-120.png", cc: "FR", tags: "fip", codec: "MP3", kbps: 128 },
  { name: "FIP Jazz", url: "https://icecast.radiofrance.fr/fipjazz-midfi.mp3", favicon: "https://www.fip.fr/dist/favicons/logo-120.png", cc: "FR", tags: "jazz, music, music only", codec: "MP3", kbps: 128 },
  { name: "KEXP Seattle", url: "https://kexp-mp3-128.streamguys1.com/kexp128.mp3", favicon: "https://www.kexp.org/static/assets/img/logo-black.svg", cc: "US", tags: "alternative, eclectic, electronic, folk", codec: "MP3", kbps: 128 },
  { name: "Radio Swiss Jazz", url: "http://stream.srg-ssr.ch/m/rsj/mp3_128", favicon: "http://www.radioswissjazz.ch/favicon.ico", cc: "CH", tags: "jazz, public radio, srg ssr", codec: "MP3", kbps: 128 },
  { name: "Radio Swiss Classic", url: "http://stream.srg-ssr.ch/m/rsc_de/mp3_128", favicon: "https://www.radioswissclassic.ch/favicon.ico", cc: "CH", tags: "classical, public radio, srg ssr", codec: "MP3", kbps: 128 },
  { name: "Radio Swiss Pop", url: "http://stream.srg-ssr.ch/m/rsp/mp3_128", favicon: "http://www.radioswisspop.ch/favicon.ico", cc: "CH", tags: "pop, public radio, srg ssr", codec: "MP3", kbps: 128 },
  { name: "WFMU", url: "https://stream0.wfmu.org/freeform-128k", favicon: "https://wfmu.org/images/wfmu_logo_94.gif", cc: "US", tags: "east orange, freeform, jersey city", codec: "MP3", kbps: 128 },
  { name: "Deutschlandfunk", url: "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3", favicon: "https://www.deutschlandfunk.de/static/img/deutschlandfunk/icons/apple-touch-icon-128x128.png", cc: "DE", tags: "culture, news, public service, information", codec: "MP3", kbps: 128 },
  { name: "Radio Deejay", url: "https://4c4b867c89244861ac216426883d1ad0.msvdn.net/radiodeejay/radiodeejay/master_ma.m3u8", favicon: "https://www.deejay.it/favicon.ico", cc: "IT", tags: "italian, news, pop, talk", codec: "AAC+", kbps: 122, hls: true },
  { name: "France Inter", url: "https://stream.radiofrance.fr/franceinter/franceinter_hifi.m3u8?id=radiofrance", favicon: "https://upload.wikimedia.org/wikipedia/commons/thumb/a/a0/France_Inter_logo_2021.svg/1200px-France_Inter_logo_2021.svg.png", cc: "FR", tags: "", codec: "AAC", kbps: 0, hls: true },
  { name: "Onda Cero", url: "https://atres-live.ondacero.es/live/ondacero/bitrate_1.m3u8", favicon: "https://statics.atresmedia.com/ondacero/webapp/static/logotipo.svg", cc: "ES", tags: "noticias y música", codec: "AAC", kbps: 128, hls: true }
];

const KEYS = 9;
const SERVERS = ['de1', 'de2', 'fi1', 'at1'];

/* What the board decodes: MP3 and AAC (HE-AAC is "AAC+" in radio-browser),
 * streamed or in HLS. An HLS station often says UNKNOWN: usually AAC in TS,
 * so it gets the benefit of the doubt. "MP4" there is fMP4 HLS, which not. */
const plays = st => {
  const c = (st.codec || 'MP3').toUpperCase();
  return c === 'MP3' || c === 'AAC' || c === 'AAC+' || (c === 'UNKNOWN' && !!st.hls);
};

function render() {
  let state = null, list = [], logoGen = Date.now(), listening = null;
  const say = (t, bad) => toast(t, bad);
  const rpost = body => post('radio', body);

  /* ---- what plays ---- */
  const nowLogo = h('div', { class: 'lg' }), nowSt = h('div', { class: 'st' }, '–'), nowTi = h('div', { class: 'ti' }),
    nowDe = h('div', { class: 'de' });
  const bPlay = h('button', { class: 'btn pri', title: 'Reproducir o pausar' }, '▶');
  const vol = h('input', { type: 'range', min: 0, max: 100 });
  const art = h('input', { type: 'checkbox' });
  const keysBox = h('div', { class: 'rdkeys' });
  const listBox = h('div', { class: 'card rdlist' });
  const resBox = h('div', { class: 'card rdlist' });
  const audio = h('audio', { preload: 'none' });

  const logoUrl = k => fsUrl(`/radio/logo${k}.jpg`) + '&t=' + logoGen;
  const playingKey = () => {
    const rd = state.radio || {};
    if (rd.index >= 0 && rd.index < KEYS && state.keys[rd.index].url === rd.url) return rd.index;
    return state.keys.findIndex(k => k.url && k.url === rd.url);
  };
  const live = () => state.player && state.player.live && state.player.state !== 'stopped';

  function paintNow() {
    const rd = state.radio || {}, pl = state.player || {};
    const on = live();
    nowSt.textContent = on ? (rd.station || '–') : (pl.state === 'playing' ? 'suena la música de la tarjeta' : 'no suena nada');
    nowTi.textContent = on ? (rd.title || rd.icy_name || '') : '';
    let de = '';
    if (on) {
      de = pl.state === 'paused' ? 'en pausa' : ({ connecting: 'conectando', buffering: 'cargando', playing: 'en vivo',
        retrying: 'reconectando', failed: 'falló', off: 'detenida' }[rd.state] || rd.state);
      if (rd.kbps) de += ' · ' + (rd.codec || 'MP3') + (rd.hls ? ' HLS ' : ' ') + rd.kbps + ' kbps';
      if (rd.host) de += ' · ' + rd.host;
      if (rd.state === 'playing') de += ' · búfer ' + (rd.buffer_ms / 1000).toFixed(1).replace('.', ',') + ' s';
      if (rd.error && (rd.state === 'failed' || rd.state === 'retrying')) de += ' · ' + rd.error;
    } else if (rd.state === 'failed' && rd.error) {
      de = 'falló: ' + rd.error;
    }
    nowDe.textContent = de;
    const k = on ? playingKey() : -1;
    const want = k >= 0 && state.keys[k].logo ? logoUrl(k) : '';
    if ((nowLogo.dataset.src || '') !== want) {
      nowLogo.dataset.src = want;
      put(nowLogo, want ? h('img', { class: 'lg', src: want, alt: '' }) : null);
    }
    bPlay.textContent = on && pl.state === 'playing' ? '⏸' : '▶';
    if (document.activeElement !== vol) vol.value = state.volume;
    art.checked = !!state.art;
  }

  /* ---- the keys ---- */
  function paintKeys() {
    const on = live() ? playingKey() : -1;
    const sig = JSON.stringify(state.keys) + on + logoGen;
    if (keysBox.dataset.sig === sig) return;
    keysBox.dataset.sig = sig;
    put(keysBox, state.keys.map((k, i) => {
      const n = h('div', { class: 'n' }, k.url && k.logo ? null : String(i + 1));
      if (k.url && k.logo) {
        const img = h('img', { src: logoUrl(i), alt: '' });
        img.onerror = () => put(n, String(i + 1));
        n.append(img);
      }
      const d = h('div', { class: 'rdkey' + (k.url ? '' : ' free') + (i === on ? ' on' : '') },
        n, h('div', { class: 'nm' }, (i + 1) + '. ' + (k.url ? k.name : 'libre')),
        k.url ? h('div', { class: 'ac' },
          h('button', { class: 'btn pri', onclick: () => rpost({ do: 'play', k: i }).then(refresh).catch(e => say(e.message, true)) }, '▶ Poner'),
          h('button', { class: 'btn', onclick: () => rpost({ do: 'clear', k: i }).then(() => { logoGen = Date.now(); refresh(); })
            .catch(e => say(e.message, true)) }, 'Vaciar')) : null);
      if (k.url) {
        d.draggable = true;
        d.ondragstart = ev => ev.dataTransfer.setData('text/p4-key', String(i));
      }
      d.ondragover = ev => { ev.preventDefault(); d.classList.add('over'); };
      d.ondragleave = () => d.classList.remove('over');
      d.ondrop = ev => {
        ev.preventDefault();
        d.classList.remove('over');
        const from = ev.dataTransfer.getData('text/p4-key'), lib = ev.dataTransfer.getData('text/p4-lib');
        if (from !== '' && +from !== i) {
          rpost({ do: 'swap', a: +from, b: i }).then(() => { logoGen = Date.now(); refresh(); }).catch(e => say(e.message, true));
        } else if (lib !== '') {
          assign(list[+lib], i);
        }
      };
      return d;
    }));
  }

  /* A 160 px JPEG of the logo, on black, whole: what the board shows. */
  const loadImg = src => new Promise((ok, bad) => {
    const img = new Image();
    img.crossOrigin = 'anonymous';
    img.onload = () => ok(img);
    img.onerror = bad;
    img.src = src;
    setTimeout(bad, 8000);
  });
  async function makeLogo(url) {
    for (const u of [url, 'https://wsrv.nl/?url=' + encodeURIComponent(url) + '&w=320&h=320&fit=inside&output=png']) {
      try {
        const img = await loadImg(u);
        const c = document.createElement('canvas');
        c.width = c.height = 160;
        const g = c.getContext('2d');
        g.fillStyle = '#000';
        g.fillRect(0, 0, 160, 160);
        const s = Math.min(144 / img.naturalWidth, 144 / img.naturalHeight);
        const w = img.naturalWidth * s, hh = img.naturalHeight * s;
        g.imageSmoothingQuality = 'high';
        g.drawImage(img, (160 - w) / 2, (160 - hh) / 2, w, hh);
        const blob = await new Promise(r => c.toBlob(r, 'image/jpeg', 0.88));     /* throws if tainted */
        if (blob) return blob;
      } catch { /* the next one */ }
    }
    return null;
  }

  /* A station on key k: the key first, then its logo, drawn here. */
  async function assign(st, k) {
    if (!st) return;
    try {
      await rpost({ do: 'set', k, name: st.name.slice(0, 47), url: st.url });
      const blob = st.favicon ? await makeLogo(st.favicon) : null;
      await fsMkdir('/radio');
      if (blob) await fsPut(`/radio/logo${k}.jpg`, blob);
      else await fsDelete(`/radio/logo${k}.jpg`).catch(() => {});
      logoGen = Date.now();
      await rpost({ do: 'set', k, name: st.name.slice(0, 47), url: st.url });   /* the app reads the logo again */
      say(`${st.name} quedó en la tecla ${k + 1}`);
    } catch (e) {
      say(e.message || 'no se pudo hablar con la placa', true);
    }
    refresh();
  }

  /* ---- the list ---- */
  async function loadList() {
    try {
      const t = await fsText('/radio/library.json');
      const j = t ? JSON.parse(t) : null;
      list = j && Array.isArray(j.stations) ? j.stations : DEFAULTS.slice();
    } catch { list = DEFAULTS.slice(); }
    paintList();
  }
  async function saveList() {
    try {
      await fsMkdir('/radio');
      await fsPut('/radio/library.json', JSON.stringify({ v: 1, stations: list }));
    } catch (e) { say('no se pudo guardar la lista: ' + e.message, true); }
  }

  function listen(url, btn) {
    if (listening === btn) {
      audio.pause();
      audio.removeAttribute('src');
      btn.textContent = '🎧';
      listening = null;
      return;
    }
    if (listening) listening.textContent = '🎧';
    audio.src = url;
    audio.play().catch(() => {});
    btn.textContent = '⏹';
    listening = btn;
  }

  function stationRow(st, extra) {
    const logo = st.favicon ? h('img', { class: 'lg', src: st.favicon, alt: '' }) : h('span', { class: 'lg' });
    if (st.favicon) logo.onerror = () => logo.replaceWith(h('span', { class: 'lg' }));
    const ok = plays(st);
    const tag = h('span', { class: 'rdtag' + (ok ? '' : ' no'), title: ok ? '' : 'no suena en la placa' },
      (st.codec && st.codec !== 'UNKNOWN' ? st.codec : '?') + (st.hls ? ' HLS' : '') + (st.kbps ? ' ' + st.kbps : ''));
    const ear = h('button', { class: 'btn', title: 'Escuchar acá' }, '🎧');
    ear.onclick = () => listen(st.url, ear);
    const ac = h('span', { class: 'ac' }, ear,
      ok ? h('button', { class: 'btn', title: 'Probar en la placa',
        onclick: () => rpost({ do: 'test', name: st.name.slice(0, 47), url: st.url }).then(refresh).catch(e => say(e.message, true)) }, '▶') : null);
    const row = h('div', { class: 'row' }, logo, h('span', { class: 'nm' }, st.name), tag, ac,
      h('span', { class: 'in' }, [(st.cc || '').toUpperCase(), st.tags || '', st.url].filter(Boolean).join(' · ')));
    extra(ac, row);
    return row;
  }

  function paintList() {
    if (!list.length) {
      put(listBox, h('div', { class: 'row muted' }, 'La lista está vacía: buscá radios abajo.'));
      return;
    }
    put(listBox, list.map((st, i) => stationRow(st, (ac, row) => {
      const sel = h('select', {}, h('option', { value: '' }, 'Tecla…'),
        ...Array.from({ length: KEYS }, (_, n) => h('option', { value: n }, String(n + 1))));
      sel.onchange = () => { if (sel.value !== '') assign(st, +sel.value); sel.value = ''; };
      ac.append(sel, h('button', { class: 'btn', onclick: () => {
        if (confirm('¿Quitar esta radio de tu lista?')) { list.splice(i, 1); paintList(); saveList(); }
      } }, 'Quitar'));
      row.draggable = true;
      row.title = 'Arrastrala a una tecla';
      row.ondragstart = ev => ev.dataTransfer.setData('text/p4-lib', String(i));
    })));
  }

  /* ---- the search ---- */
  const qName = h('input', { type: 'text', placeholder: 'rock, jazz, 95.1…' });
  const qCountry = h('select', {}, h('option', { value: '' }, 'Todos'),
    ...[['AR', 'Argentina'], ['UY', 'Uruguay'], ['CL', 'Chile'], ['BR', 'Brasil'], ['MX', 'México'], ['ES', 'España'],
      ['US', 'USA'], ['GB', 'UK'], ['FR', 'France'], ['DE', 'Deutschland'], ['IT', 'Italia'], ['CH', 'Schweiz']]
      .map(([v, n]) => h('option', { value: v, selected: v === 'AR' }, n)));
  const qTag = h('input', { type: 'text', placeholder: 'jazz' });
  const qOnly = h('input', { type: 'checkbox', checked: true });

  async function search(ev) {
    ev.preventDefault();
    put(resBox, h('div', { class: 'row muted' }, 'buscando…'));
    const only = qOnly.checked;
    const p = new URLSearchParams({ order: 'clickcount', reverse: 'true', limit: only ? '120' : '40', hidebroken: 'true' });
    if (qName.value.trim()) p.set('name', qName.value.trim());
    if (qCountry.value) p.set('countrycode', qCountry.value);
    if (qTag.value.trim()) p.set('tag', qTag.value.trim().toLowerCase());
    let res = null;
    for (const s of SERVERS) {
      try {
        const r = await fetch(`https://${s}.api.radio-browser.info/json/stations/search?${p}`);
        if (r.ok) { res = await r.json(); break; }
      } catch { /* the next mirror */ }
    }
    if (!res) { put(resBox, h('div', { class: 'row bad' }, 'radio-browser.info no contestó')); return; }
    const seen = new Set(), rows = [];
    for (const s of res) {
      const url = s.url_resolved || s.url;
      if (!url || seen.has(url) || rows.length >= 40) continue;
      seen.add(url);
      const st = { name: s.name.trim().slice(0, 47), url, favicon: s.favicon || '', cc: s.countrycode || '',
        tags: (s.tags || '').split(',').slice(0, 4).join(', '), codec: s.codec || 'MP3', kbps: s.bitrate || 0, hls: s.hls === 1 };
      if (only && !plays(st)) continue;
      rows.push(stationRow(st, ac => {
        const have = () => list.some(x => x.url === st.url);
        const b = h('button', { class: 'btn pri', disabled: have() }, have() ? '✓ en tu lista' : '+ Agregar');
        b.onclick = () => { list.push(st); paintList(); saveList(); b.textContent = '✓ en tu lista'; b.disabled = true; };
        ac.append(b);
      }));
    }
    put(resBox, rows.length ? rows : h('div', { class: 'row muted' }, 'No se encontró nada.'));
  }

  /* ---- by hand ---- */
  const mName = h('input', { type: 'text', maxlength: 47, required: true });
  const mUrl = h('input', { type: 'text', maxlength: 255, required: true, placeholder: 'https://…/stream.mp3' });
  const hand = h('form', { class: 'rdhand', autocomplete: 'off' },
    h('label', {}, 'Nombre', mName), h('label', {}, 'Dirección del stream', mUrl), h('button', { class: 'btn pri', type: 'submit' }, 'Agregar'));
  hand.onsubmit = ev => {
    ev.preventDefault();
    const url = mUrl.value.trim(), name = mName.value.trim();
    if (!/^https?:\/\//.test(url)) { say('la dirección tiene que empezar con http:// o https://', true); return; }
    list.push({ name: name.slice(0, 47), url, favicon: '', cc: '', tags: '', codec: 'MP3' });
    paintList();
    saveList();
    hand.reset();
  };

  /* ---- the board ---- */
  async function refresh() {
    try { state = await api('radio'); } catch { return; }
    paintNow();
    paintKeys();
  }
  bPlay.onclick = () => {
    const pl = state && state.player;
    const what = pl && pl.live && pl.state === 'playing' ? { do: 'pause' }
      : pl && pl.live && pl.state === 'paused' ? { do: 'resume' } : { do: 'play', k: state ? state.last : 0 };
    rpost(what).then(refresh).catch(e => say(e.message, true));
  };
  vol.onchange = () => rpost({ do: 'vol', v: +vol.value }).catch(() => {});
  art.onchange = () => rpost({ do: 'art', on: art.checked }).catch(() => {});

  const searchForm = h('form', { class: 'rdsearch' },
    h('label', {}, 'Nombre', qName), h('label', {}, 'País', qCountry), h('label', {}, 'Género', qTag),
    h('button', { class: 'btn pri', type: 'submit' }, 'Buscar'),
    h('label', { style: 'grid-column:1/-1;display:flex;gap:8px;align-items:center' }, qOnly,
      ' Sólo las que suenan en la placa (MP3, AAC, HLS)'));
  searchForm.onsubmit = search;

  put(main,
    h('h1', {}, 'Radio'),
    h('div', { class: 'btns', style: 'margin-bottom:6px' },
      h('button', { class: 'btn pri', onclick: () => openApp('aos.radio').catch(e => say(e.message, true)) }, 'Abrir la Radio en la placa')),
    h('h2', {}, 'En la placa ahora'),
    h('div', { class: 'card' },
      h('div', { class: 'rdnow' }, nowLogo, h('div', {}, nowSt, nowTi, nowDe)),
      h('div', { class: 'rdctl' },
        h('button', { class: 'btn', title: 'Anterior', onclick: () => rpost({ do: 'prev' }).then(refresh).catch(() => {}) }, '⏮'),
        bPlay,
        h('button', { class: 'btn', title: 'Siguiente', onclick: () => rpost({ do: 'next' }).then(refresh).catch(() => {}) }, '⏭'),
        h('button', { class: 'btn', title: 'Detener', onclick: () => rpost({ do: 'stop' }).then(refresh).catch(() => {}) }, '⏹'),
        h('span', { class: 'muted small' }, 'Volumen'), vol)),
    h('h2', {}, 'Las teclas'),
    h('p', { class: 'note' }, 'Arrastrá una radio de tu lista a una tecla, o una tecla sobre otra para cambiarlas de lugar.'),
    keysBox,
    h('h2', {}, 'Mi lista'),
    listBox,
    h('h2', {}, 'Buscar radios'),
    h('p', { class: 'note' }, 'En el directorio abierto de radio-browser.info, con más de 50.000 estaciones de todo el mundo.'),
    h('div', { class: 'card' }, searchForm),
    resBox,
    h('h2', {}, 'Agregar a mano'),
    h('div', { class: 'card' }, hand),
    h('h2', {}, 'Ajustes'),
    h('div', { class: 'card' }, h('div', { class: 'row' },
      h('div', { class: 'grow' }, 'Tapas de los discos', h('div', { class: 'muted small' },
        'Busca en iTunes la tapa del tema que suena. Lo único que sale de la placa es "artista tema".')), art)),
    h('p', { class: 'note' }, 'Suenan MP3 y AAC (también HE-AAC) por http:// o https://, con redirecciones y listas .pls o .m3u, y HLS (.m3u8) con segmentos TS o AAC. Ogg, Opus, FLAC, HLS en fMP4 y HLS cifrado no: el buscador los marca. La radio sigue sonando con la app cerrada.'),
    audio);

  refresh();
  loadList();
  const t = setInterval(refresh, 3000);
  return () => { clearInterval(t); audio.pause(); };
}

P.registerPage({ id: 'radio', name: 'Radio', icon: '♫', render });
