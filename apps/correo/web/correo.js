/*
 * P4OS - Correo's page in the portal: reading and writing mail from the
 * phone or the computer, through the board.
 *
 * The browser cannot speak IMAP, so everything goes through the app on the
 * live channel (docs/PORTAL-PAGES.md, "Live data"; apps/correo/main/cr_portal.c):
 * the page posts short lines ("state", "list a f text", "open a f uid",
 * "att a f uid i", "flag ...", "archive ...", "delete ...", "send path")
 * and reads the JSON the app leaves under "state", "list", "msg", "att" and
 * "sent". A message to send is written to the card as JSON with fsPut
 * (/data/correo/salida/), with its attachments beside it, and the app sends
 * it with the account's SMTP; the passwords never come to the page.
 *
 * The text of a message arrives as the board's formatted blocks and is
 * drawn with DOM nodes (textContent), never as the message's own HTML.
 */
const P = window.P4OS;
const { h, put, api, toast } = P;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });

const APP = 'aos.correo';
const LIVE = 'live?app=' + APP;
const OUT = '/data/correo/salida';
const B = { PARA: 0, H1: 1, H2: 2, QUOTE: 3, LI: 4, PRE: 5, HR: 6, IMG: 7, NOTE: 8, FWD: 9 };
const S = { BOLD: 1, ITAL: 2, UNDER: 4, LINK: 8, CODE: 16 };
const F = { SEEN: 1, ANSWERED: 2, FLAGGED: 4 };

if (!document.getElementById('css-correo')) {
  const st = document.createElement('style');
  st.id = 'css-correo';
  st.textContent = `
.crwrap { display: grid; grid-template-columns: 220px minmax(260px, 360px) minmax(0, 1fr); gap: 14px; align-items: start; }
.crcol { min-width: 0; }
.crfold { display: flex; align-items: center; gap: 8px; padding: 8px 10px; border-radius: 10px; cursor: pointer; }
.crfold:hover { background: var(--card2); } .crfold.on { background: var(--card2); font-weight: 600; }
.crfold .crn { margin-left: auto; color: var(--dim); font-size: 13px; }
.cracct { margin: 14px 0 4px; font-weight: 600; display: flex; align-items: center; gap: 8px; }
.cracct .crdot { width: 10px; height: 10px; border-radius: 5px; display: inline-block; }
.cracct small { font-weight: 400; color: var(--dim); display: block; }
.crrow { padding: 10px 12px; border-bottom: 1px solid var(--line); cursor: pointer; }
.crrow:hover { background: var(--card2); } .crrow.on { background: var(--card2); }
.crrow .crtop { display: flex; gap: 8px; } .crrow .crwho { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.crrow.unread .crwho { font-weight: 700; } .crrow.unread .crwho::before { content: '●'; color: var(--accent); margin-right: 6px; font-size: 11px; }
.crrow .crd { color: var(--dim); font-size: 12px; white-space: nowrap; }
.crrow .crs { color: var(--dim); font-size: 14px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.crlist { max-height: 72vh; overflow: auto; }
.crsubj { margin: 10px 0 8px; font-size: 22px; font-weight: 600; line-height: 1.25; }
.crbody b { color: inherit; } .crmsg .crhd { color: var(--dim); font-size: 13px; line-height: 1.5; }
.crbody { margin-top: 12px; line-height: 1.5; overflow-wrap: anywhere; }
.crbody p { margin: 0 0 10px; white-space: pre-wrap; }
.crbody h3 { margin: 14px 0 6px; } .crbody h4 { margin: 12px 0 6px; }
.crbody .crq { border-left: 3px solid var(--accent); padding-left: 10px; color: var(--dim); }
.crbody .crq2 { border-left-color: var(--green); } .crbody .crq3 { border-left-color: var(--orange); }
.crbody pre { background: var(--card2); padding: 10px; border-radius: 8px; overflow: auto; }
.crbody .crli { display: flex; gap: 8px; margin: 0 0 6px; } .crbody .crli b { min-width: 1.4em; color: var(--dim); font-weight: 400; }
.crbody .crnote { color: var(--dim); font-size: 13px; white-space: pre-wrap; }
.crbody .crfwd { background: var(--card2); padding: 8px 10px; border-radius: 8px; color: var(--dim); white-space: pre-wrap; font-size: 13px; margin-bottom: 10px; }
.crbody code { font-family: ui-monospace, Menlo, monospace; }
.crbody img { max-width: 100%; border-radius: 8px; }
.cratt { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 12px; }
.crcomp textarea { width: 100%; min-height: 260px; box-sizing: border-box; font: inherit; padding: 10px; border-radius: 10px; border: 1px solid var(--line); background: var(--card2); color: inherit; }
.crcomp input, .crcomp select { width: 100%; box-sizing: border-box; padding: 8px 10px; border-radius: 10px; border: 1px solid var(--line); background: var(--card2); color: inherit; font: inherit; }
.crcomp label { display: block; margin: 8px 0 4px; color: var(--dim); font-size: 13px; }
.crback { display: none; }
.crout { container-type: inline-size; }
@container (max-width: 1150px) {
  .crwrap { grid-template-columns: minmax(240px, 1fr) 1.4fr; }
  .crc1 { grid-column: 1 / -1; display: flex; flex-wrap: wrap; gap: 4px 6px; align-items: center; }
  .crc1 .cracct { margin: 0 4px 0 10px; } .crc1 .cracct small { display: none; }
  .crc1 .crfold { padding: 6px 10px; } .crc1 .btns { margin: 0 0 0 auto !important; }
}
@container (max-width: 700px) {
  .crwrap { grid-template-columns: 1fr; }
  .crc1 { display: block; } .crc1 .cracct small { display: block; } .crc1 .btns { margin-top: 14px !important; }
  .crwrap[data-v="list"] .crc1, .crwrap[data-v="list"] .crc3 { display: none; }
  .crwrap[data-v="msg"] .crc1, .crwrap[data-v="msg"] .crc2 { display: none; }
  .crwrap[data-v="folders"] .crc2, .crwrap[data-v="folders"] .crc3 { display: none; }
  .crback { display: inline-block; }
  .crlist { max-height: none; }
}
`;
  document.head.append(st);
}

/* The board draws emoji from code points of its own (plane 15): here they
 * are only a mark. */
const clean = s => String(s || '').replace(/[\u{F0000}-\u{FFFFD}]/gu, '◌');

const MON = ['ene', 'feb', 'mar', 'abr', 'may', 'jun', 'jul', 'ago', 'sep', 'oct', 'nov', 'dic'];
function fdate(t, long) {
  if (!t) return '';
  const d = new Date(t * 1000), now = new Date();
  const hm = d.toTimeString().slice(0, 5);
  if (long) return `${d.getDate()} ${tr(MON[d.getMonth()])} ${d.getFullYear()}, ${hm}`;
  if (d.toDateString() === now.toDateString()) return hm;
  if (d.getFullYear() === now.getFullYear()) return `${d.getDate()} ${tr(MON[d.getMonth()])}`;
  return d.toLocaleDateString();
}

const say = line => api(LIVE, { method: 'POST', body: line });
const get = key => api(LIVE + '&key=' + key);

/* Waits for a key whose JSON passes ok(): the app answers from its timer. */
async function wait(key, ok, ms = 20000) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    try { const v = await get(key); if (ok(v)) return v; } catch { /* not there yet */ }
    await new Promise(r => setTimeout(r, 300));
  }
  throw new Error(tr('La placa no contestó'));
}

/* Byte offsets of the board's UTF-8 text, in a JS string. */
const enc = new TextEncoder(), dec = new TextDecoder();

function block(b, links) {
  const bytes = enc.encode(b.x);
  const frag = document.createDocumentFragment();
  let pos = 0;
  const text = (a, e) => clean(dec.decode(bytes.slice(a, e)));
  for (const [st, len, style, link] of b.s.concat([[bytes.length, 0, 0, -1]])) {
    if (st > pos) frag.append(text(pos, st));
    if (len) {
      let el;
      if (style & S.LINK && links[link]) el = h('a', { href: links[link], target: '_blank', rel: 'noopener noreferrer' });
      else if (style & S.CODE) el = h('code');
      else if (style & S.BOLD) el = h('b');
      else if (style & S.ITAL) el = h('i');
      else el = h('u');
      el.textContent = text(st, st + len);
      frag.append(el);
    }
    pos = Math.max(pos, st + len);
  }
  switch (b.t) {
    case B.H1: return h('h3', {}, frag);
    case B.H2: return h('h4', {}, frag);
    case B.QUOTE: return h('p', { class: 'crq' + (b.l > 1 ? ' crq' + Math.min(b.l, 3) : ''), style: `margin-left:${(b.l - 1) * 10}px` }, frag);
    case B.LI: return h('div', { class: 'crli', style: `margin-left:${b.l * 18}px` }, h('b', {}, b.b || '•'), h('span', {}, frag));
    case B.PRE: return h('pre', {}, frag);
    case B.HR: return h('hr');
    case B.NOTE: return h('div', { class: 'crnote' }, frag);
    case B.FWD: return h('div', { class: 'crfwd' }, frag);
    default: return h('p', {}, frag);
  }
}

function addrs(list) {
  /* "Name <a@b>, c@d" -> [{name, addr}] (good enough for what the board sends) */
  const out = [];
  for (const m of String(list || '').matchAll(/(?:"?([^",<]*?)"?\s*<([^>]+)>|([^\s,<>]+@[^\s,<>]+))/g)) {
    out.push({ name: (m[1] || '').trim(), addr: (m[2] || m[3]).trim() });
  }
  return out;
}
const show = a => a.name ? `${a.name} <${a.addr}>` : a.addr;

P.registerPage({
  id: 'correo', name: tr('Correo'), icon: '✉', render(main) {
    let state = null, sel = { a: -1, f: -1 }, rows = [], open = null, q = '', alive = true, view = 'list', misses = 0;
    const wrap = h('div', { class: 'crwrap', 'data-v': 'list' });
    const c1 = h('div', { class: 'crcol crc1 card pad' });
    const c2 = h('div', { class: 'crcol crc2 card' });
    const c3 = h('div', { class: 'crcol crc3 card pad' });
    const status = h('div', { class: 'note' });
    const go = v => { view = v; wrap.dataset.v = v; };
    put(wrap, c1, c2, c3);
    put(main, h('div', { class: 'crout' }, h('h1', {}, tr('Correo')), status, wrap));

    async function loadList() {
      try {
        await say(`list ${sel.a} ${sel.f} ${q}`.trim());
        const v = await wait('list', v => v.a === sel.a && v.f === sel.f);
        rows = v.rows;
        drawList();
      } catch (e) { toast(e.message, true); }
    }

    function drawFolders() {
      const kids = [];
      const pick = (a, f) => { sel = { a, f }; drawFolders(); loadList(); go('list'); };
      const total = state.accounts.reduce((n, a) => n + a.unseen, 0);
      kids.push(h('div', { class: 'crfold' + (sel.a < 0 ? ' on' : ''), onclick: () => pick(-1, -1) },
        '⬇ ', tr('Todas las entradas'), total ? h('span', { class: 'crn' }, String(total)) : null));
      for (const a of state.accounts) {
        kids.push(h('div', { class: 'cracct' }, h('span', { class: 'crdot', style: `background:${a.color}` }),
          h('div', {}, clean(a.label), h('small', {}, clean(a.state)))));
        for (const f of a.folders) {
          const n = f.sp === 3 ? f.n : (f.sp === 2 || f.sp === 5 ? 0 : f.unseen);
          kids.push(h('div', { class: 'crfold' + (sel.a === a.i && sel.f === f.i ? ' on' : ''), style: `padding-left:${10 + f.depth * 16}px`, onclick: () => pick(a.i, f.i) },
            clean(f.name), n ? h('span', { class: 'crn' }, String(n)) : null));
        }
      }
      kids.push(h('div', { class: 'btns', style: 'margin-top:14px' }, h('button', { class: 'btn pri', onclick: () => compose() }, tr('Escribir'))));
      put(c1, ...kids);
    }

    function drawList() {
      const search = h('input', { type: 'search', placeholder: tr('Buscar'), value: q, style: 'width:100%;box-sizing:border-box' });
      search.addEventListener('change', () => { q = search.value.trim(); loadList(); });
      const list = h('div', { class: 'crlist' });
      put(list, ...rows.map(r => h('div', {
        class: 'crrow' + (r.fl & F.SEEN ? '' : ' unread') + (open && open.u === r.u && open.a === r.a ? ' on' : ''),
        onclick: () => openMsg(r),
      }, h('div', { class: 'crtop' }, h('span', { class: 'crwho' }, clean(r.from)), h('span', { class: 'crd' }, (r.fl & F.FLAGGED ? '★ ' : '') + fdate(r.d))),
      h('div', { class: 'crs' }, clean(r.s) || tr('(sin asunto)')))));
      if (!rows.length) put(list, h('div', { class: 'pad muted' }, tr('No hay mensajes')));
      put(c2, h('div', { class: 'pad' }, h('button', { class: 'btn crback', onclick: () => go('folders') }, '‹ ' + tr('Buzones')), search), list);
    }

    async function openMsg(r) {
      put(c3, h('div', { class: 'muted' }, tr('Abriendo...')));
      go('msg');
      try {
        await say(`open ${r.a} ${r.f} ${r.u}`);
        const m = await wait('msg', v => v.error || (v.u === r.u && v.a === r.a && v.f === r.f));
        if (m.error) throw new Error(m.error);
        open = m;
        r.fl |= F.SEEN;
        drawMsg();
        drawList();
      } catch (e) { put(c3, h('div', { class: 'muted' }, tr('No se pudo abrir el mensaje'), ': ', e.message)); }
    }

    async function getAtt(i, inlineImg) {
      try {
        let before = 0;
        try { before = (await get('att')).t || 0; } catch { /* the first one */ }
        await say(`att ${open.a} ${open.f} ${open.u} ${i}`);
        const v = await wait('att', v => v.t !== undefined && v.t !== before, 60000);
        if (!v.ok) throw new Error(tr('No se pudo'));
        if (inlineImg) return P.fsUrl(v.path);
        const a = h('a', { href: P.fsUrl(v.path, true), download: '' });
        document.body.append(a); a.click(); a.remove();
      } catch (e) { toast(e.message, true); }
      return null;
    }

    async function act(line, back) {
      try { await say(line); toast(tr('Listo')); } catch (e) { toast(e.message, true); }
      if (back) { open = null; put(c3); go('list'); }
      setTimeout(loadList, 700);
    }

    function drawMsg() {
      const m = open;
      const from = addrs(m.from)[0] || { name: m.from, addr: '' };
      const body = h('div', { class: 'crbody' });
      for (const b of m.blocks) {
        if (b.t === B.IMG) {
          const att = m.att[b.img];
          const ph = h('div', { class: 'crnote' }, h('button', { class: 'btn', onclick: async () => {
            const url = await getAtt(b.img, true);
            if (url) put(ph, h('img', { src: url, alt: att ? att.name : '' }));
          } }, '\u{1F5BC} ' + (att ? clean(att.name) : tr('Imagen'))));
          body.append(ph);
          continue;
        }
        body.append(block(b, m.links));
      }
      if (m.remote) body.append(h('div', { class: 'crnote' }, tr('Las imágenes de internet no se bajan.')));
      const atts = m.att.filter(a => !(a.inl && a.cid));
      const star = m.fl & F.FLAGGED;
      put(c3, h('div', { class: 'crmsg' },
        h('div', { class: 'btns' },
          h('button', { class: 'btn crback', onclick: () => go('list') }, '‹ ' + tr('Lista')),
          h('button', { class: 'btn pri', onclick: () => compose('reply') }, tr('Responder')),
          addrs(m.to).length + addrs(m.cc).length > 1 ? h('button', { class: 'btn', onclick: () => compose('all') }, tr('A todos')) : null,
          h('button', { class: 'btn', onclick: () => compose('fwd') }, tr('Reenviar')),
          h('button', { class: 'btn', onclick: () => act(`archive ${m.a} ${m.f} ${m.u}`, true) }, tr('Archivar')),
          h('button', { class: 'btn', onclick: () => act(`flag ${m.a} ${m.f} ${m.u} ${star ? 0 : F.FLAGGED} ${star ? F.FLAGGED : 0}`) }, star ? '★' : '☆'),
          h('button', { class: 'btn', onclick: () => act(`flag ${m.a} ${m.f} ${m.u} 0 ${F.SEEN}`, true) }, tr('No leído')),
          h('button', { class: 'btn red', onclick: () => act(`delete ${m.a} ${m.f} ${m.u}`, true) }, tr('Borrar'))),
        h('div', { class: 'crsubj' }, clean(m.subject) || tr('(sin asunto)')),
        h('div', { class: 'crhd' }, h('b', {}, clean(show(from))), h('br'),
          tr('Para:'), ' ', clean(m.to), m.cc ? [h('br'), tr('Cc:'), ' ', clean(m.cc)] : null, h('br'), fdate(m.d, true)),
        body,
        atts.length ? h('div', { class: 'cratt' }, ...atts.map(a => h('button', { class: 'btn', onclick: () => getAtt(a.i) },
          '\u{1F4CE} ' + clean(a.name) + ' · ' + P.fmtBytes(a.size)))) : null));
    }

    function compose(mode) {
      const m = open;
      const accts = state.accounts;
      let acct = mode && m ? m.a : (sel.a >= 0 ? sel.a : 0);
      let to = '', cc = '', subject = '', text = '', irt = '', refs = '', reply_uid = 0, reply_folder = -1;
      if (mode && m) {
        const pre = (p, s) => (s.toLowerCase().startsWith(p.toLowerCase()) ? s : `${p} ${s}`);
        if (mode === 'fwd') {
          subject = pre('Fwd:', m.subject);
          text = `\n\n---------- ${tr('Mensaje reenviado')} ----------\n${tr('De')}: ${m.from}\n${tr('Fecha')}: ${fdate(m.d, true)}\n${tr('Asunto')}: ${m.subject}\n\n` + m.quote.replace(/^> ?/gm, '');
        } else {
          to = (m.reply_to || m.from);
          if (mode === 'all') {
            const me = accts.map(a => a.email.toLowerCase());
            cc = addrs(m.to).concat(addrs(m.cc)).filter(a => !me.includes(a.addr.toLowerCase())).map(show).join(', ');
          }
          subject = pre('Re:', m.subject);
          text = `\n\n${fdate(m.d, true)}, ${(addrs(m.from)[0] || {}).name || m.from} ${tr('escribió')}:\n${m.quote}`;
          irt = m.msgid;
          refs = (m.refs ? m.refs + ' ' : '') + m.msgid;
        }
        reply_uid = m.u; reply_folder = m.f;
      }
      const f = {
        acct: h('select', {}, ...accts.map(a => h('option', { value: a.i, selected: a.i === acct }, `${a.name} <${a.email}>`))),
        to: h('input', { value: clean(to) }), cc: h('input', { value: clean(cc) }), bcc: h('input'),
        subject: h('input', { value: clean(subject) }), body: h('textarea'), files: h('input', { type: 'file', multiple: true }),
      };
      f.body.value = clean(text);
      const sending = h('span', { class: 'muted' });
      async function send() {
        if (!f.to.value.trim() && !f.cc.value.trim() && !f.bcc.value.trim()) { toast(tr('Falta a quién mandarlo'), true); return; }
        const stamp = Date.now();
        const attachments = [];
        try {
          sending.textContent = tr('Enviando...');
          /* the folders, if this is the first time (an error is "it exists") */
          await P.fsMkdir('/data/correo').catch(() => {});
          await P.fsMkdir(OUT).catch(() => {});
          if (f.files.files.length) await P.fsMkdir(`${OUT}/adj-${stamp}`).catch(() => {});
          for (const file of f.files.files) {
            const path = `${OUT}/adj-${stamp}/${P.fsSlug(file.name.replace(/\.[^.]*$/, ''), 60, 'adjunto')}${(file.name.match(/\.[^.]*$/) || [''])[0].toLowerCase()}`;
            await P.fsPut(path, file);
            attachments.push(path);
          }
          const job = { acct: Number(f.acct.value), to: f.to.value, cc: f.cc.value, bcc: f.bcc.value, subject: f.subject.value,
            body: f.body.value, irt, refs, reply_uid, reply_folder, forward: mode === 'fwd', attachments };
          const path = `${OUT}/${stamp}.json`;
          await P.fsPut(path, JSON.stringify(job));
          let before = 0;
          try { before = (await get('sent')).t || 0; } catch { /* first one */ }
          await say(`send ${path}`);
          const v = await wait('sent', v => v.t !== before, 90000);
          if (!v.ok) throw new Error(v.why || tr('No se pudo enviar'));
          toast(tr('Enviado'));
          if (open) drawMsg(); else put(c3);
          go(open ? 'msg' : 'list');
        } catch (e) { sending.textContent = ''; toast(tr('No se pudo enviar') + ': ' + e.message, true); }
        for (const p of attachments) await P.fsDelete(p).catch(() => {});
        if (attachments.length) P.fsDelete(`${OUT}/adj-${stamp}`).catch(() => {});
      }
      put(c3, h('div', { class: 'crcomp' },
        h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => { if (open) drawMsg(); else put(c3); go(open ? 'msg' : 'list'); } }, tr('Cancelar')),
          h('button', { class: 'btn pri', onclick: send }, tr('Enviar')), sending),
        h('label', {}, tr('De')), f.acct, h('label', {}, tr('Para')), f.to, h('label', {}, tr('Cc')), f.cc,
        h('label', {}, tr('Cco')), f.bcc, h('label', {}, tr('Asunto')), f.subject, h('label', {}, tr('Mensaje')), f.body,
        h('label', {}, tr('Adjuntos')), f.files));
      go('msg');
    }

    async function poll() {
      if (!alive) return;
      if (!document.hidden) {
        try {
          await say('state');
          const s = await get('state');
          misses = 0;
          const first = !state;
          state = s;
          if (!state.accounts.length) {
            put(status, tr('No hay cuentas: agregalas en la placa, en Correo, Ajustes.'));
          } else {
            put(status);
          }
          drawFolders();
          if (first) loadList();
        } catch {
          if (++misses >= 2) {
            put(status, h('div', { class: 'card pad' }, tr('Correo no está abierto en la placa.'), ' ',
              h('button', { class: 'btn pri', onclick: () => P.openApp(APP).then(() => { misses = 0; }) }, tr('Abrir en la placa'))));
          }
        }
      }
      setTimeout(poll, state ? 4000 : 1500);
    }
    poll();
    return () => { alive = false; };
  },
});
