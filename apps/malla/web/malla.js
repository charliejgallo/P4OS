/* Malla's page in the board's portal (docs/PORTAL-PAGES.md): chatting with
 * the mesh from the phone's browser, through the node the board talks to.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Live through /api/live (docs/PORTAL-PAGES.md "Live data"), only while the
 * app is open on the board (also in the background): the app puts "state"
 * (the link, our node, the channels), "nodes" and "msgs" as JSON, and takes
 * lines from here: "send <peer> <ch> <text>" (peer 0 is the channel),
 * "read <peer> <ch>", "url <link>" (a channel set to take, asked first).
 * The texts are in Spanish and go through tr(), with this page's catalog
 * (web.malla.lang). */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const APP = 'aos.malla';
const LIVE = 'live?app=' + APP;
const BROADCAST = 4294967295;

const CSS = `
.mlwrap{display:grid;grid-template-columns:minmax(220px,300px) 1fr;gap:14px}
@media (max-width:760px){.mlwrap{grid-template-columns:1fr}}
.mlconv{display:flex;align-items:center;gap:10px;padding:10px 12px;border-radius:12px;cursor:pointer}
.mlconv:hover{background:var(--card2)}
.mlconv.on{background:var(--card2);outline:2px solid var(--accent)}
.mlav{width:38px;height:38px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-weight:600;font-size:13px;color:#000;flex:none}
.mlname{font-weight:600}
.mlprev{color:var(--dim);font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.mlbadge{background:var(--accent);color:#fff;border-radius:10px;padding:1px 8px;font-size:12px}
.mlmsgs{height:52vh;min-height:280px;overflow-y:auto;display:flex;flex-direction:column;gap:8px;padding:6px 2px}
.mlb{max-width:78%;padding:8px 12px;border-radius:16px;background:var(--card2);white-space:pre-wrap;word-wrap:break-word}
.mlb.me{align-self:flex-end;background:var(--accent);color:#fff}
.mlwho{font-size:12px;font-weight:600;margin:0 4px 2px}
.mlfoot{font-size:11px;color:var(--dim);margin:2px 6px 0}
.mlfoot.me{align-self:flex-end}
.mlfoot .ok{color:var(--green)}.mlfoot .bad{color:var(--red)}
.mlrow{display:flex;flex-direction:column}
.mlin{display:flex;gap:8px;margin-top:10px}
.mlin input{flex:1}
.mlreact{font-size:13px;margin:2px 8px}
.mlnodes td,.mlnodes th{padding:4px 8px;text-align:left;font-size:13px;white-space:nowrap}
.mlnodes tr{cursor:pointer}
`;

const ROUTING = { 1: tr('no hay ruta'), 2: tr('lo rechazaron'), 3: tr('no hubo respuesta'), 4: tr('sin radio'),
  5: tr('nadie lo confirmó'), 6: tr('canal desconocido'), 7: tr('demasiado largo'), 8: tr('sin respuesta'),
  9: tr('límite de uso del canal'), 34: tr('falta su clave pública'), 35: tr('falta su clave pública'),
  38: tr('demasiados seguidos') };

const hex = n => '!' + (n >>> 0).toString(16).padStart(8, '0');
const rgb = n => {
  let r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
  const m = Math.max(r, g, b);
  if (m < 120) { r += 120 - m; g += 120 - m; b += 120 - m; }
  return `rgb(${Math.min(r, 255)},${Math.min(g, 255)},${Math.min(b, 255)})`;
};
/* black on a light colour, white on a dark one, as on the board */
const ink = n => {
  const m = rgb(n).match(/\d+/g).map(Number);
  return m[0] * 30 + m[1] * 59 + m[2] * 11 > 14000 ? '#000' : '#fff';
};
const age = s => s == null || s < 0 ? tr('nunca') : s < 60 ? tr('recién') : s < 3600 ? tr('hace %d min', Math.floor(s / 60))
  : s < 86400 ? tr('hace %d h', Math.floor(s / 3600)) : tr('hace %d d', Math.floor(s / 86400));
const when = m => {
  if (m.w) {
    const d = new Date(m.w * 1000), now = new Date();
    const hm = d.toTimeString().slice(0, 5);
    return d.toDateString() === now.toDateString() ? hm : `${d.getDate()}/${d.getMonth() + 1} ${hm}`;
  }
  return m.age >= 0 ? age(m.age) : '';
};

async function liveGet(key) {
  const r = await fetch('/api/' + LIVE + '&key=' + key, { cache: 'no-store' });
  if (r.status === 404) return null;
  if (!r.ok) throw new Error(r.statusText);
  return { seq: r.headers.get('X-Live-Seq'), data: await r.json() };
}

P.registerPage({
  id: 'malla',
  name: tr('Malla'),
  icon: '✉',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-malla')) document.head.appendChild(h('style', { id: 'css-malla' }, CSS));
    let alive = true, st = null, nodes = [], msgs = [], seqs = {}, sel = null, closed = false;
    const head = h('div', { class: 'card pad' });
    const convs = h('div', { class: 'card pad' });
    const title = h('h2', { style: 'margin:0 0 6px' });
    const box = h('div', { class: 'mlmsgs' });
    const input = h('input', { type: 'text', maxlength: 200, placeholder: tr('Mensaje'),
      onkeydown: e => { if (e.key === 'Enter') send(); } });
    const btn = h('button', { class: 'btn pri', onclick: () => send() }, tr('Enviar'));
    const chat = h('div', { class: 'card pad' }, title, box, h('div', { class: 'mlin' }, input, btn));
    const nodesCard = h('div', { class: 'card pad' });
    const url = h('input', { type: 'text', placeholder: 'https://meshtastic.org/e/#...', style: 'width:100%' });
    const urlCard = h('div', { class: 'card pad' },
      h('h3', { style: 'margin-top:0' }, tr('Tomar canales de una URL')),
      h('p', { class: 'note' }, tr('Reemplaza los canales del nodo (y su radio, si la URL la trae). El nodo se reinicia.')),
      url, h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => takeUrl() }, tr('Aplicar'))));

    put(main, h('h1', {}, tr('Malla')), head,
      h('div', { class: 'mlwrap' }, convs, chat), nodesCard, urlCard);

    const nodeOf = n => nodes.find(x => x.n === n);
    const nameOf = n => { const x = nodeOf(n); return x && x.l ? x.l : hex(n); };
    const shortOf = n => { const x = nodeOf(n); return x && x.s ? x.s : (n & 0xffff).toString(16); };
    const post = text => P.api(LIVE, { method: 'POST', body: text });

    function send() {
      const t = input.value.trim();
      if (!t || !sel) return;
      if (new TextEncoder().encode(t).length > 228) { P.toast(tr('Es demasiado largo para la malla'), true); return; }
      post(`send ${sel.peer} ${sel.ch} ${t}`).then(() => { input.value = ''; })
        .catch(e => P.toast(e.message, true));
    }

    function takeUrl() {
      const u = url.value.trim();
      if (!u.includes('#')) { P.toast(tr('No es una URL de canales de Meshtastic'), true); return; }
      if (!confirm(tr('¿Reemplazar los canales del nodo? Se reinicia.'))) return;
      post('url ' + u).then(() => { P.toast(tr('Mandado al nodo')); url.value = ''; })
        .catch(e => P.toast(e.message, true));
    }

    function conversations() {
      const list = (st.chans || []).map(c => ({ peer: 0, ch: c.i, name: c.name, unread: c.unread, last: null }));
      const dms = new Map();
      for (const m of msgs) {
        if (m.r) continue;
        if (m.p) {
          if (!dms.has(m.p)) dms.set(m.p, { peer: m.p, ch: 0, name: nameOf(m.p), unread: 0, last: null });
          const d = dms.get(m.p);
          d.last = m;
          if (m.f !== st.my && m.st === 0 && m.u) d.unread++;
        } else {
          const c = list.find(x => x.ch === m.c);
          if (c) c.last = m;
        }
      }
      for (const n of nodes) { const d = dms.get(n.n); if (d) d.unread = n.u || 0; }
      return list.concat([...dms.values()].sort((a, b) => msgs.indexOf(b.last) - msgs.indexOf(a.last)));
    }

    function drawConvs() {
      const list = conversations();
      if (!sel && list.length) sel = { peer: list[0].peer, ch: list[0].ch };
      put(convs, ...list.map(c => {
        const on = sel && sel.peer === c.peer && (c.peer || sel.ch === c.ch);
        const av = c.peer ? h('div', { class: 'mlav', style: `background:${rgb(c.peer)};color:${ink(c.peer)}` }, shortOf(c.peer))
          : h('div', { class: 'mlav', style: 'background:var(--accent);color:#fff' }, String(c.ch));
        const prev = c.last ? `${c.last.f === st.my ? tr('Vos') : shortOf(c.last.f)}: ${c.last.txt}` : tr('Sin mensajes todavía');
        return h('div', { class: 'mlconv' + (on ? ' on' : ''), onclick: () => { sel = { peer: c.peer, ch: c.ch }; draw(true); } },
          av, h('div', { class: 'grow', style: 'min-width:0' }, h('div', { class: 'mlname' }, c.name), h('div', { class: 'mlprev' }, prev)),
          c.unread ? h('span', { class: 'mlbadge' }, String(c.unread)) : null);
      }));
    }

    function stateText(m) {
      if (m.f !== st.my) return [m.w || m.age >= 0 ? when(m) : ''];
      const s = { 1: [tr('en cola')], 2: [tr('enviado')],
        3: [h('span', { class: 'ok' }, '✓ ' + (m.t === BROADCAST ? tr('repetido') : tr('recibido')))],
        4: [h('span', { class: 'bad' }, '⚠ ' + (ROUTING[m.e] || tr('error')))] }[m.st] || [];
      return [when(m), s.length ? ' · ' : '', ...s];
    }

    function drawChat(scroll) {
      if (!sel) { title.textContent = ''; put(box); return; }
      const ch = (st.chans || []).find(c => c.i === sel.ch);
      title.textContent = sel.peer ? nameOf(sel.peer) : (ch ? ch.name : tr('Canal %d', sel.ch));
      const mine = msgs.filter(m => !m.r && m.p === sel.peer && (sel.peer || m.c === sel.ch));
      const reacts = msgs.filter(m => m.r);
      const atEnd = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
      put(box, ...(mine.length ? mine.map(m => {
        const me = m.f === st.my;
        const rs = reacts.filter(r => r.rid === m.id).map(r => `${r.txt} ${r.f === st.my ? tr('Vos') : shortOf(r.f)}`).join('   ');
        return h('div', { class: 'mlrow' },
          !me && !sel.peer ? h('div', { class: 'mlwho', style: `color:${rgb(m.f)}` }, nameOf(m.f)) : null,
          h('div', { class: 'mlb' + (me ? ' me' : '') }, m.txt),
          rs ? h('div', { class: 'mlreact', style: me ? 'align-self:flex-end' : '' }, rs) : null,
          h('div', { class: 'mlfoot' + (me ? ' me' : '') }, ...stateText(m)));
      }) : [h('p', { class: 'muted' }, sel.peer ? tr('Escribí el primer mensaje directo.') : tr('Todavía no hay mensajes en este canal.'))]));
      if (scroll || atEnd) box.scrollTop = box.scrollHeight;
      const unread = sel.peer ? (nodeOf(sel.peer) || {}).u : ((st.chans || []).find(c => c.i === sel.ch) || {}).unread;
      if (unread) post(`read ${sel.peer} ${sel.ch}`).catch(() => {});
    }

    function drawNodes() {
      const sorted = nodes.slice().sort((a, b) => (a.n === st.my ? -1 : b.n === st.my ? 1 : (a.age < 0 ? 1e9 : a.age) - (b.age < 0 ? 1e9 : b.age)));
      put(nodesCard, h('h3', { style: 'margin-top:0' }, tr('%d nodos', nodes.length)),
        h('div', { style: 'overflow-x:auto' }, h('table', { class: 'mlnodes', style: 'width:100%;border-collapse:collapse' },
          h('tr', {}, h('th', {}, tr('Nodo')), h('th', {}, tr('Visto')), h('th', {}, tr('Saltos')), h('th', {}, tr('Batería')),
            h('th', {}, 'SNR'), h('th', {}, tr('Distancia'))),
          ...sorted.map(n => h('tr', { title: hex(n.n), onclick: () => { if (n.n !== st.my) { sel = { peer: n.n, ch: 0 }; draw(true); window.scrollTo(0, 0); } } },
            h('td', {}, h('span', { class: 'mlav', style: `display:inline-flex;width:26px;height:26px;font-size:10px;margin-right:8px;vertical-align:middle;background:${rgb(n.n)};color:${ink(n.n)}` }, n.s || ''), n.l || hex(n.n)),
            h('td', {}, n.n === st.my ? tr('este') : age(n.age)),
            h('td', {}, n.n === st.my ? '' : n.hops < 0 ? '?' : n.hops === 0 ? tr('directo') : String(n.hops)),
            h('td', {}, n.bat < 0 ? '' : n.bat > 100 ? tr('enchufado') : n.bat + ' %'),
            h('td', {}, n.hops === 0 && n.n !== st.my ? (n.snr / 4).toFixed(1).replace('.', ',') + ' dB' : ''),
            h('td', {}, n.dist != null ? (n.dist < 1000 ? n.dist + ' m' : (n.dist / 1000).toFixed(1).replace('.', ',') + ' km') : ''))))));
    }

    function drawHead() {
      if (!st) {
        put(head, h('p', {}, tr('Malla no está abierta en la placa.')),
          h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: () => P.openApp(APP) }, tr('Abrir Malla en la placa'))));
        return;
      }
      const label = { ready: tr('Conectado'), config: tr('Cargando la base de nodos'), opening: tr('Conectando'),
        retry: tr('Reintentando'), idle: tr('Sin conexión') }[st.state] || st.state;
      const color = { ready: 'var(--green)', config: 'var(--orange)', opening: 'var(--orange)', retry: 'var(--red)' }[st.state] || 'var(--dim)';
      put(head, h('div', { class: 'row' },
        h('div', { class: 'grow' }, h('b', {}, st.my_name || tr('Sin nodo')), ' ', h('span', { class: 'muted' }, st.my_id || ''),
          h('div', { class: 'muted small' }, [st.link, st.fw, st.err].filter(Boolean).join(' · '))),
        h('span', { class: 'pill', style: `color:${color}` }, '● ' + label)));
    }

    function draw(scroll) {
      drawHead();
      if (!st) { put(convs); put(box); put(nodesCard); return; }
      drawConvs();
      drawChat(scroll);
      drawNodes();
    }

    (async () => {
      let first = true;
      while (alive) {
        try {
          const s = await liveGet('state');
          if (!s) { if (st || first) { st = null; draw(); } }
          else {
            let changed = s.seq !== seqs.state;
            st = s.data; seqs.state = s.seq;
            for (const k of ['nodes', 'msgs']) {
              const r = await liveGet(k);
              if (r && r.seq !== seqs[k]) { seqs[k] = r.seq; changed = true; if (k === 'nodes') nodes = r.data; else msgs = r.data; }
            }
            if (changed || first) draw(first);
          }
          first = false;
        } catch (e) {
          if (!closed) console.warn('malla:', e);
        }
        await new Promise(r => setTimeout(r, 1500));
      }
    })();
    return () => { alive = false; closed = true; };
  },
});
