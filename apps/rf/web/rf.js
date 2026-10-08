/* The RF app's page in the board's portal (docs/PORTAL-PAGES.md): the radio
 * from the browser, while the app is open on the board.
 *
 * Live through /api/live (docs/PORTAL-PAGES.md "Live data"): the app puts
 * "spec" (its spectrum, 1024 bins, binary), "state" (JSON, every second)
 * and "events" (Data mode's last 30, JSON); the page draws the spectrum and
 * its own waterfall, and sends the app lines of key=value (freq, mode,
 * rate, gain, sq, step, step_by, capture, rec_iq, rec_wav, play, play_stop,
 * log, mqtt), the same as rf/control.txt. The recordings and the CSVs are
 * plain card files (fsList, fsUrl). The texts are in Spanish, like the rest
 * of the portal. */
const P = window.P4OS;
const APP = 'aos.rf';
const LIVE = 'live?app=' + APP;

const MODES = [['off', 'Sin audio'], ['wfm', 'FM'], ['am', 'AM'], ['nfm', 'FM angosta'], ['data', 'Datos'], ['lora', 'LoRa']];
const HALF = { wfm: 100000, am: 4000, nfm: 6500 };
const BANDS = [['FM', 98000000, 'wfm', 100000], ['Aire', 125000000, 'am', 25000], ['Marina', 156800000, 'nfm', 25000],
  ['2 m', 145000000, 'nfm', 12500], ['433', 433920000, 'data', 25000], ['70 cm', 435000000, 'nfm', 12500],
  ['PMR', 446006250, 'nfm', 12500], ['868', 868300000, 'data', 25000], ['LoRa 868', 868100000, 'lora', 25000],
  ['LoRa 915', 915200000, 'lora', 25000], ['ADS-B', 1090000000, 'off', 1000000]];
const STEPS = [1000, 5000, 6250, 8330, 10000, 12500, 25000, 100000, 125000, 250000, 1000000];
/* Meshtastic's regions and presets, as the app has them (rf.c): a default
 * channel's slot is the djb2 hash of the preset's name modulo the slots */
const MESH_REGIONS = [['ANZ', 915, 928], ['US', 902, 928], ['EU_868', 869.4, 869.65], ['EU_433', 433, 434], ['CN', 470, 510],
  ['IN', 865, 867], ['KR', 920, 923], ['TW', 920, 925], ['RU', 868.7, 869.2], ['NZ_865', 864, 868]];
const MESH_PRESETS = [['LongFast', 11, 250], ['MediumFast', 9, 250], ['MediumSlow', 10, 250], ['ShortFast', 7, 250],
  ['ShortSlow', 8, 250], ['ShortTurbo', 7, 500], ['LongModerate', 11, 125], ['LongSlow', 12, 125]];
function meshFreq(r, p) {
  let h = 5381;
  for (const c of MESH_PRESETS[p][0]) h = (Math.imul(h, 33) + c.charCodeAt(0)) >>> 0;
  const bw = MESH_PRESETS[p][2] / 1000, [, lo, hi] = MESH_REGIONS[r];
  const slots = Math.max(1, Math.floor((hi - lo) / bw + 1e-6));
  return Math.round((lo + bw / 2 + (h % slots) * bw) * 1e6);
}
const RATES = { hs: [960000, 1440000, 1920000, 2400000], listen: [240000, 960000, 1920000], fs: [240000] };

const css = `
.rf-freq{font-size:2.4em;font-weight:600;cursor:pointer;font-variant-numeric:tabular-nums}
.rf-top{display:flex;align-items:center;gap:12px;justify-content:center}
.rf-canvas{width:100%;display:block;border-radius:10px;background:#080a10;touch-action:none;cursor:crosshair}
.rf-axis{position:relative;height:18px;font-size:12px;color:var(--dim)}
.rf-axis span{position:absolute;transform:translateX(-50%)}
.rf-seg{display:flex;flex-wrap:wrap;gap:6px}
.rf-seg .btn.on{background:var(--accent);color:#fff}
.rf-ev{width:100%;border-collapse:collapse;font-size:14px}
.rf-ev td{padding:6px 4px;border-top:1px solid var(--line);vertical-align:top}
.rf-ev tr.k{cursor:pointer}
.rf-ev .det{color:var(--dim);font-size:12px}
.rf-hover{font-size:13px;color:var(--dim);min-height:1.2em;text-align:center}
`;

/* dark blue to cyan to yellow to red, the app's own waterfall colours */
const LUT = (() => {
  const S = [[0, 0, 0, 12], [50, 0, 24, 110], [100, 0, 120, 210], [150, 30, 210, 200], [200, 250, 220, 40], [235, 255, 90, 20], [255, 255, 240, 230]];
  const out = new Uint8ClampedArray(256 * 3);
  for (let i = 0; i < 256; i++) {
    let k = 0;
    while (k < 5 && i > S[k + 1][0]) k++;
    const t = (i - S[k][0]) / (S[k + 1][0] - S[k][0]);
    for (let c = 0; c < 3; c++) out[i * 3 + c] = S[k][c + 1] + t * (S[k + 1][c + 1] - S[k][c + 1]);
  }
  return out;
})();

const mhz = (hz, d = 4) => (hz / 1e6).toFixed(d).replace('.', ',');
const khz = hz => (hz / 1000).toLocaleString('es-AR', { maximumFractionDigits: 3 }) + ' kHz';

function evLine(e) {
  let s;
  if (e.temp !== undefined) {
    s = `ID ${e.id} · canal ${e.channel} · ${e.temp.toFixed(1).replace('.', ',')} °C`;
    if (e.hum !== undefined) s += ` · ${e.hum} %`;
    if (e.batt_low) s += ' · batería baja';
  } else if (e.proto === 'PT2262') s = `código ${e.code.slice(0, 8)} · datos ${e.code.slice(8)}`;
  else if (e.proto) s = `ID ${e.id.toString(16).toUpperCase().padStart(5, '0')} · botón ${(e.button ?? 0).toString(16).toUpperCase()}`;
  else s = `${e.mod || '?'} ${e.short}/${e.long} µs · ${e.bits} bits · ${e.hex}`;
  return s + ` · ${e.off >= 0 ? '+' : ''}${Math.round(e.off / 1000)} kHz`;
}

P.registerPage({
  id: 'rf',
  name: 'RF',
  icon: '≈',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-rf')) document.head.append(h('style', { id: 'css-rf' }, css));
    let st = null, spec = null, alive = true, lastSpec = 0, evSeen = -1, opened = {};
    /* what each part was last drawn from: redrawn only when it changes, so a
     * click or an open list is never pulled from under the pointer */
    const drawn = {};
    const changed = (part, sig) => { if (drawn[part] === sig) return false; drawn[part] = sig; return true; };
    const send = text => P.api(LIVE, { method: 'POST', body: text }).catch(e => P.toast(e.message, true));

    /* ---- the top: the frequency, the steps, the status ---- */
    const freq = h('div', { class: 'rf-freq', title: 'Tocar para escribir una frecuencia' }, '—');
    freq.onclick = () => {
      const v = prompt('Frecuencia en MHz', st ? mhz(st.freq) : '');
      if (v) send('freq=' + (parseFloat(v.replace(',', '.')) * 1e6).toFixed(0));
    };
    const status = h('div', { class: 'note', style: 'text-align:center' }, '');
    const top = h('div', { class: 'card' },
      h('div', { class: 'rf-top' },
        h('button', { class: 'btn', onclick: () => send('step_by=-1') }, '◀'), freq,
        h('button', { class: 'btn', onclick: () => send('step_by=1') }, '▶')),
      status);

    /* ---- the controls ---- */
    const modeRow = h('div', { class: 'rf-seg' });
    const rateSel = h('select', { onchange: () => send('rate=' + rateSel.value) });
    const gainSel = h('select', { onchange: () => send('gain=' + gainSel.value) });
    const stepSel = h('select', { onchange: () => send('step=' + stepSel.value) },
      ...STEPS.map(s => h('option', { value: s }, khz(s))));
    const sq = h('input', { type: 'range', min: 0, max: 40, step: 1, style: 'width:140px', onchange: () => send('sq=' + sq.value) });
    const sqLbl = h('span', { class: 'small' }, '');
    const sqBox = h('span', {}, 'Silenciador ', sq, ' ', sqLbl);
    /* the board's volume (the system's) and the radio's own mute */
    const vol = h('input', { type: 'range', min: 0, max: 100, step: 1, style: 'width:120px', onchange: () => send('vol=' + vol.value) });
    const volLbl = h('span', { class: 'small' }, '');
    let muted = false;
    const muteBtn = h('button', { class: 'btn', onclick: () => send('mute=' + (muted ? 0 : 1)) }, 'Silenciar');
    const volBox = h('span', {}, 'Volumen ', vol, ' ', volLbl, ' ', muteBtn);
    const bands = h('div', { class: 'rf-seg' }, ...BANDS.map(([n, hz, m, step]) =>
      h('button', { class: 'btn', onclick: () => send(`mode=${m}\nfreq=${hz}\nstep=${step}`) }, n)));
    const meshReg = h('select', {}, ...MESH_REGIONS.map(([n], i) => h('option', { value: i }, n)));
    const meshPre = h('select', {}, ...MESH_PRESETS.map(([n], i) => h('option', { value: i, selected: i === 1 }, n)));
    const meshLbl = h('span', { class: 'small' }, '');
    const meshShow = () => {
      const p = MESH_PRESETS[meshPre.value];
      meshLbl.textContent = `${mhz(meshFreq(meshReg.value, meshPre.value), 4)} MHz · SF${p[1]} · ${p[2]} kHz`;
    };
    meshReg.onchange = meshPre.onchange = meshShow;
    meshShow();
    const mesh = h('div', { class: 'row', style: 'flex-wrap:wrap;gap:10px;margin-top:10px' }, 'Meshtastic, canal por defecto: ',
      meshReg, meshPre, meshLbl,
      h('button', { class: 'btn', onclick: () => send(`mode=lora\nfreq=${meshFreq(meshReg.value, meshPre.value)}\nstep=${MESH_PRESETS[meshPre.value][2] * 1000}`) }, 'Escuchar ahí'));
    const controls = h('div', { class: 'card' },
      modeRow,
      h('div', { class: 'row', style: 'flex-wrap:wrap;gap:14px;margin-top:10px' },
        h('span', {}, 'Muestreo ', rateSel), h('span', {}, 'Ganancia ', gainSel), h('span', {}, 'Paso ', stepSel), sqBox, volBox),
      h('div', { style: 'margin-top:10px' }, bands), mesh);

    /* ---- the spectrum and the waterfall (or Data's list) ---- */
    const sc = h('canvas', { class: 'rf-canvas', height: 220 });
    const wf = h('canvas', { class: 'rf-canvas', height: 320, style: 'margin-top:4px' });
    const axis = h('div', { class: 'rf-axis' });
    const hover = h('div', { class: 'rf-hover' }, ' ');
    const evBox = h('div', {});
    const loraBox = h('div', { style: 'margin-top:10px' });
    const view = h('div', { class: 'card' }, sc, axis, wf, evBox, loraBox, hover);

    const xToHz = (cv, ev) => {
      const r = cv.getBoundingClientRect();
      const span = spec ? spec.rate : st ? st.rate : 0;
      const centre = spec ? spec.freq : st ? st.freq : 0;
      return centre + ((ev.clientX - r.left) / r.width - 0.5) * span;
    };
    for (const cv of [sc, wf]) {
      cv.onmousemove = ev => { if (spec) hover.textContent = mhz(xToHz(cv, ev)) + ' MHz'; };
      cv.onmouseleave = () => { hover.textContent = ' '; };
      cv.onclick = ev => {
        if (!st) return;
        const s = st.step || 1;
        send('freq=' + Math.round(xToHz(cv, ev) / s) * s);
      };
    }
    /* the wheel tunes over the spectrum only: over the tall waterfall it
     * would keep the page from scrolling */
    sc.onwheel = ev => { ev.preventDefault(); send('step_by=' + (ev.deltaY > 0 ? -1 : 1)); };
    sc.title = 'Tocar para ir a esa frecuencia; la rueda mueve de a un paso';

    /* ---- keeping ---- */
    const keep = h('div', { class: 'card' });
    const files = h('div', {});

    const missing = h('div', { class: 'card' },
      h('p', {}, 'La app RF no está abierta en la placa (o la placa no tiene la RTL-SDR).'),
      h('button', { class: 'btn pri', onclick: () => P.openApp(APP).then(() => P.toast('Abierta en la placa')) }, 'Abrir RF en la placa'));

    put(main, h('h1', {}, 'RF'), missing, top, controls, view, keep, files);
    const showMissing = on => {
      missing.style.display = on ? '' : 'none';
      for (const c of [top, controls, view, keep]) c.style.display = on ? 'none' : '';
    };

    function drawSpec(s) {
      const W = sc.clientWidth || 600, dpr = window.devicePixelRatio || 1;
      for (const cv of [sc, wf]) if (cv.width !== Math.round(W * dpr)) cv.width = Math.round(W * dpr);
      const g = sc.getContext('2d'), w = sc.width, H = sc.height;
      const floor = s.floor / 10, lo = floor - 8, hi = floor + 62;
      g.fillStyle = '#080a10';
      g.fillRect(0, 0, w, H);
      /* grid every 10 dB */
      g.strokeStyle = '#242834';
      g.lineWidth = 1;
      for (let db = Math.ceil(lo / 10) * 10; db < hi; db += 10) {
        const y = (hi - db) / (hi - lo) * H;
        g.beginPath(); g.moveTo(0, y); g.lineTo(w, y); g.stroke();
      }
      /* the channel listened to */
      const half = HALF[st ? st.mode : ''];
      if (half) {
        const bw = Math.max(2, half / s.rate * w);
        g.fillStyle = 'rgba(80,110,200,.18)';
        g.fillRect(w / 2 - bw, 0, 2 * bw, H);
      }
      /* the trace */
      g.beginPath();
      const n = s.bins.length;
      for (let x = 0; x < w; x++) {
        const b0 = Math.floor(x * n / w), b1 = Math.max(b0 + 1, Math.floor((x + 1) * n / w));
        let m = 0;
        for (let b = b0; b < b1; b++) if (s.bins[b] > m) m = s.bins[b];
        const db = m / 2 - 140, y = Math.min(H - 1, Math.max(0, (hi - db) / (hi - lo) * H));
        x ? g.lineTo(x, y) : g.moveTo(x, y);
      }
      g.lineTo(w, H); g.lineTo(0, H); g.closePath();
      g.fillStyle = 'rgba(30,120,170,.45)';
      g.fill();
      g.strokeStyle = '#78e6ff';
      g.stroke();
      g.strokeStyle = 'rgba(220,60,60,.8)';
      g.beginPath(); g.moveTo(w / 2, 0); g.lineTo(w / 2, H); g.stroke();
      /* the axis */
      const span = s.rate, steps = [10e3, 20e3, 25e3, 50e3, 100e3, 200e3, 250e3, 500e3, 1e6];
      const tick = steps.find(t => span / t <= W / 90) || 1e6, lo_hz = s.freq - span / 2;
      const kids = [];
      for (let t = Math.ceil(lo_hz / tick) * tick; t < lo_hz + span; t += tick)
        kids.push(h('span', { style: `left:${(t - lo_hz) / span * 100}%` }, (t / 1e6).toFixed(tick >= 1e6 ? 0 : tick >= 100e3 ? 1 : tick >= 10e3 ? 2 : 3).replace('.', ',')));
      put(axis, ...kids);
      /* the waterfall: down a row, the new one on top */
      if (st && st.mode === 'data') return;
      const gw = wf.getContext('2d');
      gw.drawImage(wf, 0, 1);
      const row = gw.createImageData(wf.width, 1);
      const wlo = floor - 2, whi = floor + 42;
      for (let x = 0; x < wf.width; x++) {
        const b0 = Math.floor(x * n / wf.width), b1 = Math.max(b0 + 1, Math.floor((x + 1) * n / wf.width));
        let m = 0;
        for (let b = b0; b < b1; b++) if (s.bins[b] > m) m = s.bins[b];
        const v = Math.max(0, Math.min(255, Math.round((m / 2 - 140 - wlo) * 255 / (whi - wlo))));
        row.data.set([LUT[v * 3], LUT[v * 3 + 1], LUT[v * 3 + 2], 255], x * 4);
      }
      gw.putImageData(row, 0, 0);
    }

    function showState(s) {
      st = s;
      showMissing(false);
      freq.textContent = mhz(s.freq) + ' MHz';
      let t = s.state === 'run' ? `${s.kind} ${s.tuner} · ${(s.rate / 1e6).toFixed(3).replace('.', ',')} Msps · ${String(s.mbs).replace('.', ',')} MB/s` :
        s.state === 'search' ? 'Buscando la radio…' : (s.why || 'Sin radio');
      if (s.listening) t += ` · señal ${Math.round(s.level)} dB${s.open ? '' : ' · silenciado'}${s.mode === 'wfm' && s.pilot >= 10 ? ' · estéreo' : ''}`;
      if (s.mode === 'data') t += ` · ${s.received} recibidos`;
      if (s.mode === 'lora') t += ` · LoRa en el canal, el último minuto: ${s.lora_n} paquetes, ocupado ${String(s.lora_busy).replace('.', ',')} %`;
      if (s.playing) t += ' · reproduciendo ' + s.play;
      if (s.rec_iq) t += ` · grabando la señal (${s.iq_mb} MB${s.iq_dropped ? ', perdiendo' : ''})`;
      if (s.rec_wav) t += ` · grabando el audio (${Math.floor(s.wav_s / 60)}:${String(s.wav_s % 60).padStart(2, '0')})`;
      status.textContent = t;
      if (changed('modes', s.mode))
        put(modeRow, ...MODES.map(([m, n]) => h('button', { class: 'btn' + (s.mode === m ? ' on' : ''), onclick: () => send('mode=' + m) }, n)));
      const rates = !s.high_speed ? RATES.fs : s.mode !== 'off' && s.mode !== 'lora' ? RATES.listen : RATES.hs;
      if (!rates.includes(s.rate)) rates.unshift(s.rate);   /* a recording's own */
      if (changed('rates', rates.join() + '/' + s.rate))
        put(rateSel, ...rates.map(r => h('option', { value: r, selected: r === s.rate }, (r / 1e6).toFixed(3).replace('.', ',') + ' Msps')));
      rateSel.disabled = s.mode === 'data' || s.playing;
      if (changed('gains', s.gains.join() + '/' + s.gain))
        put(gainSel, h('option', { value: 'auto', selected: s.gain < 0 }, 'Automática'),
          ...s.gains.map(g => h('option', { value: g, selected: g === s.gain }, (g / 10).toFixed(1).replace('.', ',') + ' dB')));
      if (document.activeElement !== stepSel) stepSel.value = s.step;
      if (document.activeElement !== sq) sq.value = s.sq;
      sqLbl.textContent = s.sq ? s.sq + ' dB' : 'no';
      if (s.vol !== undefined) {
        if (document.activeElement !== vol) vol.value = s.vol;
        volLbl.textContent = s.vol + ' %';
        muted = !!s.muted;
        muteBtn.textContent = muted ? 'Activar el sonido' : 'Silenciar';
        muteBtn.classList.toggle('red', muted);
      }
      sqBox.style.display = s.mode === 'am' || s.mode === 'nfm' ? '' : 'none';
      wf.style.display = s.mode === 'data' ? 'none' : '';
      evBox.style.display = s.mode === 'data' ? '' : 'none';
      loraBox.style.display = s.mode === 'lora' ? '' : 'none';
      if (s.mode === 'lora') loadLora();
      drawKeep(s);
      if (s.mode === 'data' && s.received !== evSeen) {
        evSeen = s.received;
        loadEvents();
      }
    }

    function loadEvents() {
      P.api(LIVE + '&key=events').then(list => {
        if (!list.length) {
          put(evBox, h('p', { class: 'note' }, 'Esperando transmisiones en la banda: controles remotos, sensores de temperatura, timbres…'));
          return;
        }
        const rows = [];
        for (const e of list) {
          rows.push(h('tr', { class: 'k', onclick: () => { opened[e.n] = !opened[e.n]; loadEvents(); } },
            h('td', {}, e.t), h('td', {}, h('b', {}, e.proto || 'Desconocido'), e.count > 1 ? ` ×${e.count}` : ''),
            h('td', {}, evLine(e)), h('td', { class: 'small' }, Math.round(e.snr) + ' dB')));
          if (opened[e.n])
            rows.push(h('tr', {}, h('td', {}), h('td', { colspan: 3, class: 'det' },
              `${mhz(e.freq, 3)} MHz · ${e.mod || '?'} · corto ${e.short} µs · largo ${e.long} µs · pausa ${e.gap} µs · ${e.bits} bits: ${e.hex}`)));
        }
        put(evBox, h('table', { class: 'rf-ev' }, h('tbody', {}, ...rows)),
          h('p', { class: 'note small' }, 'Tocá una fila para ver sus pulsos y sus bits. Todo queda también en rf/datos-<día>.csv de la tarjeta.'));
      }).catch(() => {});
    }

    /* LoRa's packets: the newest first, what they are, where and how long */
    let loraSeen = '';
    function loadLora() {
      P.api(LIVE + '&key=lora').then(list => {
        const key = list.length ? list[0].n + '/' + list.length : '';
        if (key === loraSeen) return;
        loraSeen = key;
        if (!list.length) {
          put(loraBox, h('p', { class: 'note' }, 'Esperando paquetes LoRa en la banda.'));
          return;
        }
        put(loraBox, h('table', { class: 'rf-ev' }, h('tbody', {}, ...list.map(e => h('tr', {},
          h('td', {}, e.t),
          h('td', {}, h('b', {}, e.preset || (e.sf ? `SF${e.sf} · ${e.bw / 1000} kHz` : 'Otra señal'))),
          h('td', {}, `${mhz(e.freq, 4)} MHz` + (e.sf ? ` · SF${e.sf} · ${e.bw / 1000} kHz` : ` · ${Math.round(e.width / 1000)} kHz de ancho`)),
          h('td', { class: 'small' }, `${e.ms} ms · ${Math.round(e.snr)} dB`))))),
          h('p', { class: 'note small' }, 'El SF y el ancho salen del preámbulo de cada paquete. Todo queda también en rf/lora-<día>.csv de la tarjeta.'));
      }).catch(() => {});
    }

    function drawKeep(s) {
      if (!changed('keep', [s.listening, s.rec_wav, s.rec_iq, s.playing, s.log, s.mqtt, s.mqtt_ready, s.rate].join())) return;
      const tog = (on, a, b, cmd) => h('button', { class: 'btn' + (on ? ' red' : ''), onclick: () => send(cmd + '=' + (on ? 0 : 1)) }, on ? b : a);
      const sw = (label, on, cmd) => h('label', { class: 'row', style: 'gap:8px' },
        h('input', { type: 'checkbox', checked: on, onchange: ev => send(cmd + '=' + (ev.target.checked ? 1 : 0)) }), label);
      put(keep, h('h2', {}, 'Guardar'),
        h('div', { class: 'btns' },
          h('button', { class: 'btn', onclick: () => send('capture=1').then(() => P.toast('La placa guarda la captura en Fotos → Capturas')) }, 'Captura de la placa'),
          h('button', { class: 'btn', onclick: savePng }, 'Imagen del espectro (PNG)'),
          s.listening || s.rec_wav ? tog(s.rec_wav, 'Grabar el audio', 'Terminar el audio', 'rec_wav') : null,
          tog(s.rec_iq, 'Grabar la señal (I/Q)', 'Terminar la señal', 'rec_iq'),
          s.playing ? h('button', { class: 'btn red', onclick: () => send('play_stop=1') }, 'Dejar de reproducir') : null),
        sw('Guardar lo recibido en Datos y LoRa (rf/datos- y rf/lora-<día>.csv)', s.log, 'log'),
        sw(s.mqtt_ready ? 'Publicarlo por MQTT' : 'Publicarlo por MQTT (no conectado)', s.mqtt, 'mqtt'),
        h('p', { class: 'note small' }, `La señal ocupa ${Math.round(s.rate * 2 * 60 / 1e6)} MB por minuto a esta tasa; la tarjeta escribe unos 3 MB/s.`));
    }

    function savePng() {
      const c = document.createElement('canvas');
      c.width = sc.width;
      c.height = sc.height + (wf.style.display === 'none' ? 0 : wf.height);
      const g = c.getContext('2d');
      g.drawImage(sc, 0, 0);
      if (wf.style.display !== 'none') g.drawImage(wf, 0, sc.height);
      g.fillStyle = '#fff';
      g.font = '14px sans-serif';
      g.fillText(`${freq.textContent} · ${new Date().toLocaleString('es-AR')}`, 8, 18);
      c.toBlob(b => P.saveBlob(b, `rf-${st ? st.freq : 0}-${Date.now()}.png`));
    }

    async function loadFiles() {
      const [iq, rf, wav] = await Promise.all([P.fsList('/rf/iq'), P.fsList('/rf'), P.fsList('/recordings')]);
      const rec = iq.filter(f => f.name.endsWith('.cu8')).sort((a, b) => b.name.localeCompare(a.name));
      const csv = rf.filter(f => f.name.startsWith('datos-') && f.name.endsWith('.csv')).sort((a, b) => b.name.localeCompare(a.name));
      const aud = wav.filter(f => f.name.startsWith('RF_') && f.name.endsWith('.wav')).sort((a, b) => b.name.localeCompare(a.name));
      const del = async (path, also) => {
        if (!confirm('¿Borrar ' + path.split('/').pop() + '?')) return;
        try { await P.fsDelete(path); if (also) await P.fsDelete(also).catch(() => {}); loadFiles(); } catch (e) { P.toast(e.message, true); }
      };
      put(files,
        h('div', { class: 'card' }, h('h2', {}, 'Señales grabadas (I/Q)'),
          rec.length ? null : h('p', { class: 'note' }, 'Todavía no hay.'),
          ...rec.map(f => {
            const [when, hz, sps] = f.name.replace('.cu8', '').split('_');
            return h('div', { class: 'row' }, h('div', { class: 'grow' }, `${when} · ${mhz(+hz, 3)} MHz · ${(+sps / 1e6).toFixed(3).replace('.', ',')} Msps`, h('span', { class: 'small muted' }, ' ' + P.fmtBytes(f.size))),
              h('button', { class: 'btn', onclick: () => send('play=' + f.name) }, 'Reproducir'),
              h('a', { class: 'btn', href: P.fsUrl('/rf/iq/' + f.name, true) }, 'Bajar'),
              h('button', { class: 'btn red', onclick: () => del('/rf/iq/' + f.name, '/rf/iq/' + f.name.replace('.cu8', '.txt')) }, '✕'));
          })),
        h('div', { class: 'card' }, h('h2', {}, 'Audio grabado'),
          aud.length ? null : h('p', { class: 'note' }, 'Todavía no hay.'),
          ...aud.map(f => h('div', { class: 'row' }, h('div', { class: 'grow' }, f.name),
            h('audio', { controls: true, preload: 'none', src: P.fsUrl('/recordings/' + f.name) }),
            h('a', { class: 'btn', href: P.fsUrl('/recordings/' + f.name, true) }, 'Bajar')))),
        h('div', { class: 'card' }, h('h2', {}, 'Datos recibidos (CSV)'),
          csv.length ? null : h('p', { class: 'note' }, 'Todavía no hay.'),
          ...csv.map(f => h('div', { class: 'row' }, h('div', { class: 'grow' }, f.name, h('span', { class: 'small muted' }, ' ' + P.fmtBytes(f.size))),
            h('a', { class: 'btn', href: P.fsUrl('/rf/' + f.name, true) }, 'Bajar')))));
    }

    /* ---- polling: the spectrum as fast as it comes (one ask at a time), the
     * state every second, the files now and then ---- */
    async function specLoop() {
      while (alive) {
        const t0 = performance.now();
        try {
          const r = await fetch('/api/' + LIVE + '&key=spec', { cache: 'no-store' });
          if (r.ok) {
            const b = await r.arrayBuffer(), dv = new DataView(b);
            if (b.byteLength >= 24 && dv.getUint32(0, false) === 0x52465331) {
              const s = { seq: dv.getUint32(4, true), freq: dv.getUint32(8, true), rate: dv.getUint32(12, true),
                floor: dv.getInt16(16, true), bins: new Uint8Array(b, 24, dv.getUint16(18, true)) };
              if (s.seq !== lastSpec) { lastSpec = s.seq; spec = s; drawSpec(s); }
            }
          }
        } catch { /* the board is away: the state's loop says so */ }
        const wait = 80 - (performance.now() - t0);
        await new Promise(res => setTimeout(res, wait > 0 ? wait : 0));
      }
    }
    const stateTick = () => P.api(LIVE + '&key=state').then(showState).catch(() => showMissing(true));
    showMissing(true);
    stateTick();
    specLoop();
    loadFiles().catch(() => {});
    const t1 = setInterval(stateTick, 1000), t2 = setInterval(() => loadFiles().catch(() => {}), 15000);
    return () => { alive = false; clearInterval(t1); clearInterval(t2); };
  },
});
