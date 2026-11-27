/* The Mensajes app's page in the board's portal (docs/PORTAL-PAGES.md): read
 * and write your Telegram chats from the phone's browser, through the board.
 *
 * Live through /api/live (docs/PORTAL-PAGES.md "Live data"): while the page
 * looks, the app puts "state" (signed in or not, the network, the unread
 * count), "chats" (the list) and "chat" (the open chat's last messages),
 * all JSON; the page sends it lines: "open u:123" (opens that chat, also on
 * the board's screen), "send u:123 text" and "app <api_id> <api_hash>",
 * which is how the app's keys can be typed here instead of on the board.
 * The photos are the cache's files on the card (fsUrl); the messages never
 * touch the card on their way here. The texts are in Spanish and go
 * through tr(), with this page's catalog (web.mensajes.lang). */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const LOC = { es: 'es-AR', en: 'en-US', de: 'de-DE' }[P.lang ? P.lang() : 'es'] || 'es-AR';
const APP = 'aos.mensajes';
const LIVE = 'live?app=' + APP;

const css = `
.mj-wrap { display: grid; grid-template-columns: minmax(240px, 340px) 1fr; gap: 14px; align-items: start; }
.mj-back { display: none; }
@media (max-width: 760px) { .mj-wrap { grid-template-columns: 1fr; } .mj-wrap.mj-in .mj-list { display: none; }
  .mj-wrap:not(.mj-in) .mj-chat { display: none; } .mj-back { display: inline-block; }
  .mj-chat { height: 66vh; } }
.mj-list { padding: 6px 0; max-height: 78vh; overflow-y: auto; }
.mj-row { display: flex; gap: 12px; align-items: center; padding: 10px 14px; cursor: pointer; border-radius: 12px; }
.mj-row:hover { background: var(--card2); }
.mj-row.mj-on { background: var(--card2); }
.mj-av { width: 46px; height: 46px; border-radius: 50%; flex: none; object-fit: cover; display: flex;
  align-items: center; justify-content: center; color: #fff; font-weight: 600; }
.mj-txt { flex: 1; min-width: 0; }
.mj-name { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.mj-prev { color: var(--dim); font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.mj-side { text-align: right; font-size: 12px; color: var(--dim); flex: none; }
.mj-badge { display: inline-block; min-width: 20px; padding: 1px 7px; border-radius: 10px; background: var(--accent);
  color: #fff; font-size: 12px; text-align: center; margin-top: 4px; }
.mj-chat { display: flex; flex-direction: column; height: 78vh; padding: 0; overflow: hidden; }
.mj-head { display: flex; align-items: center; gap: 10px; padding: 12px 16px; border-bottom: 1px solid var(--line); }
.mj-msgs { flex: 1; overflow-y: auto; padding: 14px; display: flex; flex-direction: column; gap: 6px; }
.mj-b { max-width: 78%; padding: 8px 12px; border-radius: 16px; background: var(--card2); align-self: flex-start;
  white-space: pre-wrap; overflow-wrap: anywhere; }
.mj-b.mj-out { align-self: flex-end; background: var(--accent); color: #fff; }
.mj-b img { display: block; max-width: 100%; max-height: 320px; border-radius: 10px; margin: 4px 0; }
.mj-who { font-size: 12px; font-weight: 600; color: var(--accent); }
.mj-b.mj-out .mj-who { color: #fff; }
.mj-meta { font-size: 11px; opacity: .7; text-align: right; margin-top: 2px; }
.mj-media { font-size: 13px; opacity: .85; }
.mj-svc { align-self: center; font-size: 12px; color: var(--dim); background: var(--card2); padding: 4px 12px;
  border-radius: 12px; }
.mj-send { display: flex; gap: 8px; padding: 10px; border-top: 1px solid var(--line); }
.mj-send input { flex: 1; }
`;

const COLORS = ['#E17076', '#FAA774', '#A695E7', '#7BC862', '#6EC9CB', '#65AADD', '#EE7AAE', '#D4A15C'];
const color = k => { let n = 0; for (const c of k) n = (n * 31 + c.charCodeAt(0)) >>> 0; return COLORS[n % 8]; };
const initials = name => (name || '?').split(/\s+/).filter(Boolean).slice(0, 2).map(w => [...w][0]).join('').toUpperCase();

function when(t) {
  if (!t) return '';
  const d = new Date(t * 1000), now = new Date();
  if (d.toDateString() === now.toDateString()) return d.toLocaleTimeString(LOC, { hour: '2-digit', minute: '2-digit' });
  return d.toLocaleDateString(LOC, { day: 'numeric', month: 'short' });
}

P.registerPage({
  id: 'mensajes',
  name: tr('Mensajes'),
  icon: '✉',
  render(main) {
    const { h, put } = P;
    if (!document.getElementById('css-mensajes')) document.head.append(h('style', { id: 'css-mensajes' }, css));
    let alive = true, st = null, chats = [], chat = null, openK = '', seqChats = '', seqChat = '';
    const send = text => P.api(LIVE, { method: 'POST', body: text }).catch(e => P.toast(e.message, true));

    const status = h('div', { class: 'note' }, tr('Buscando la app en la placa…'));
    const top = h('div', { class: 'card pad' }, status);
    const list = h('div', { class: 'card mj-list' });
    const title = h('div', { class: 'mj-name' }, '');
    const sub = h('div', { class: 'mj-prev' }, '');
    const backBtn = h('button', { class: 'btn mj-back', onclick: () => { wrap.classList.remove('mj-in'); } }, '‹');
    const msgs = h('div', { class: 'mj-msgs' });
    const input = h('input', { type: 'text', placeholder: tr('Escribí un mensaje'), maxlength: 900 });
    const go = () => {
      const t = input.value.trim();
      if (!t || !openK) return;
      send(`send ${openK} ${t}`).then(() => { input.value = ''; seqChat = ''; });
    };
    input.onkeydown = e => { if (e.key === 'Enter') go(); };
    const sendBox = h('div', { class: 'mj-send' }, input, h('button', { class: 'btn pri', onclick: go }, tr('Enviar')));
    const chatCard = h('div', { class: 'card mj-chat' },
      h('div', { class: 'mj-head' }, backBtn, h('div', { class: 'mj-txt' }, title, sub)), msgs, sendBox);
    const wrap = h('div', { class: 'mj-wrap' }, list, chatCard);

    /* the app's keys, when the board has none yet */
    const idIn = h('input', { type: 'text', placeholder: 'api_id', inputmode: 'numeric', style: 'width:140px' });
    const hashIn = h('input', { type: 'text', placeholder: 'api_hash', style: 'width:320px', autocomplete: 'off' });
    const keys = h('div', { class: 'card pad' },
      h('h3', {}, tr('Las claves de la app')),
      h('p', { class: 'note' }, tr('Para entrar con tu cuenta, Mensajes necesita un api_id y un api_hash tuyos. Se sacan gratis en my.telegram.org, en «API development tools». Se guardan en la placa, no en la tarjeta.')),
      h('div', { class: 'row', style: 'gap:10px;flex-wrap:wrap' }, idIn, hashIn,
        h('button', { class: 'btn pri', onclick: () => {
          const id = idIn.value.trim(), hs = hashIn.value.trim().toLowerCase();
          if (!/^\d+$/.test(id) || !/^[0-9a-f]{32}$/.test(hs)) { P.toast(tr('El api_id es un número y el api_hash tiene 32 letras y números (0-9, a-f).'), true); return; }
          send(`app ${id} ${hs}`).then(() => { hashIn.value = ''; P.toast(tr('Guardadas: seguí en la placa con tu número')); });
        } }, tr('Guardar'))));
    keys.style.display = 'none';
    put(main, h('h1', {}, tr('Mensajes')), top, keys, wrap);

    const avatar = (c, size) => c.photo
      ? h('img', { class: 'mj-av', src: P.fsUrl(c.photo), style: size ? `width:${size}px;height:${size}px` : '', alt: '' })
      : h('div', { class: 'mj-av', style: `background:${color(c.k || c.name)}` + (size ? `;width:${size}px;height:${size}px` : '') }, initials(c.name));

    function drawList() {
      put(list, ...chats.map(c => h('div', { class: 'mj-row' + (c.k === openK ? ' mj-on' : ''), onclick: () => {
        openK = c.k; seqChat = ''; chat = null; drawChat(); send('open ' + c.k); wrap.classList.add('mj-in'); drawList();
      } },
        avatar(c),
        h('div', { class: 'mj-txt' }, h('div', { class: 'mj-name' }, (c.pinned ? '📌 ' : '') + c.name),
          h('div', { class: 'mj-prev' }, (c.out ? (c.read ? '✓✓ ' : '✓ ') : '') + c.text)),
        h('div', { class: 'mj-side' }, when(c.time), c.unread ? h('div', {}, h('span', { class: 'mj-badge' }, String(c.unread))) : null))));
      if (!chats.length) put(list, h('div', { class: 'pad muted' }, tr('Todavía no hay chats')));
    }

    function drawChat() {
      const c = chats.find(x => x.k === openK);
      title.textContent = chat && chat.k === openK ? chat.name : (c ? c.name : tr('Elegí un chat'));
      sub.textContent = chat && chat.k === openK ? chat.status : '';
      if (!chat || chat.k !== openK) { put(msgs, h('div', { class: 'muted' }, openK ? tr('Cargando…') : '')); return; }
      /* at the bottom it follows; a chat drawn for the first time starts there */
      const near = msgs.scrollHeight - msgs.scrollTop - msgs.clientHeight < 80 || msgs.dataset.drawn !== chat.k;
      const group = openK[0] !== 'u';
      put(msgs, ...chat.msgs.map(m => {
        if (m.service) return h('div', { class: 'mj-svc' }, m.media || m.text);
        const parts = [];
        if (group && !m.out) parts.push(h('div', { class: 'mj-who' }, m.from));
        if (m.fwd) parts.push(h('div', { class: 'mj-media' }, tr('Reenviado de %s', m.fwd)));
        if (m.photo) parts.push(h('a', { href: P.fsUrl(m.photo), target: '_blank' }, h('img', { src: P.fsUrl(m.photo), alt: '' })));
        else if (m.media) parts.push(h('div', { class: 'mj-media' }, m.media));
        if (m.text) parts.push(h('div', {}, m.text));
        if (m.reactions) parts.push(h('div', {}, m.reactions));
        parts.push(h('div', { class: 'mj-meta' }, (m.edited ? tr('editado') + ' ' : '') + when(m.date) +
          (m.out ? (m.pending ? ' ⏳' : m.read ? ' ✓✓' : ' ✓') : '')));
        return h('div', { class: 'mj-b' + (m.out ? ' mj-out' : '') }, ...parts);
      }));
      if (near) msgs.scrollTop = msgs.scrollHeight;
      msgs.dataset.drawn = chat.k;
    }

    function showState() {
      if (!st) {
        status.textContent = tr('Mensajes no está abierta en la placa.');
        put(top, status, h('button', { class: 'btn pri', style: 'margin-top:8px', onclick: () => P.openApp(APP).then(() => P.toast(tr('Abierta en la placa'))) }, tr('Abrir Mensajes en la placa')));
        wrap.style.display = 'none';
        keys.style.display = 'none';
        return;
      }
      wrap.style.display = st.auth === 7 ? '' : 'none';
      keys.style.display = !st.have_app ? '' : 'none';
      const net = st.net === 3 ? '' : ' · ' + tr('conectando…');
      if (st.auth === 7) status.textContent = tr('Conectada como %s', st.me || '?') + (st.unread ? ' · ' + tr('%s sin leer', st.unread) : '') + net;
      else if (!st.have_app) status.textContent = tr('Faltan las claves de la app.');
      else status.textContent = tr('No hay sesión: entrá desde la placa con tu número y el código.');
      put(top, status);
      if (st.open && !openK) { openK = st.open; wrap.classList.add('mj-in'); }
    }

    /* each value asked again only when its version moved (X-Live-Seq) */
    async function getIf(key, seen) {
      const r = await fetch('/api/' + LIVE + '&key=' + key, { cache: 'no-store' });
      if (r.status === 404) return [null, ''];
      if (!r.ok) throw new Error(r.statusText);
      const s = r.headers.get('X-Live-Seq') || '';
      if (s && s === seen) return [undefined, s];
      return [await r.json(), s];
    }

    async function tick() {
      if (!alive) return;
      try {
        const [s] = await getIf('state', '');
        st = s;
        showState();
        if (st && st.auth === 7) {
          const [cl, cs] = await getIf('chats', seqChats);
          if (cl !== undefined && cl !== null) { chats = cl; seqChats = cs; drawList(); }
          const [ch, hs] = await getIf('chat', seqChat);
          if (ch !== undefined && ch !== null) { chat = ch; seqChat = hs; drawChat(); }
        }
      } catch (e) {
        st = null;
        showState();
      }
      if (alive) timer = setTimeout(tick, 1500);
    }
    let timer = setTimeout(tick, 50);
    return () => { alive = false; clearTimeout(timer); };
  },
});
