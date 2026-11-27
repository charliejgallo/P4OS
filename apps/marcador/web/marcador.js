/* Marcador's page in the board's portal (docs/PORTAL-PAGES.md).
 *
 * The games the app keeps on the card, one text file each in
 * /data/marcador/g<id>.txt (written by apps/marcador/main/mr_model.c): the
 * list with the players and their scores, and for the one chosen its whole
 * history, point by point, with the time of play and the score after each.
 * The history goes out as a CSV for a spreadsheet, a game can be deleted,
 * and the app can be opened on the board. Nothing is written but deletes:
 * the scores are the board's.
 *
 * The file, one thing per line:
 *   type <n> | name <text> | target <n> | opt <8 numbers> | played <s> | winner <i> <a>
 *   p <RRGGBB> <score> <games> <sets> <wins> <name>
 *   h <who> <kind> <delta> <t> <group> then per player: <score> <games> <sets> before it
 */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const N_ = s => s;
const DIR = '/data/marcador';
const KINDS = [N_('Paño y fósforos'), N_('Porotos'), N_('Cuentaganado'), N_('Tablero de estadio'),
               N_('Cartel de mesa'), N_('Ábaco'), N_('Pizarrón')];

function parse(text) {
  const g = { type: 0, name: '', target: 0, opt: [], played: 0, winner: -1, p: [], h: [] };
  for (const raw of text.split('\n')) {
    const line = raw.replace(/\r$/, '');
    const sp = line.indexOf(' ');
    const k = sp < 0 ? line : line.slice(0, sp), rest = sp < 0 ? '' : line.slice(sp + 1);
    const n = rest.split(' ').map(Number);
    if (k === 'type') g.type = n[0];
    else if (k === 'name') g.name = rest;
    else if (k === 'target') g.target = n[0];
    else if (k === 'opt') g.opt = n;
    else if (k === 'played') g.played = n[0];
    else if (k === 'winner') g.winner = n[1] ? n[0] : -1;
    else if (k === 'p') {
      const f = rest.split(' ');
      g.p.push({ color: '#' + f[0], score: +f[1], games: +f[2], sets: +f[3], wins: +f[4], name: f.slice(5).join(' ') });
    } else if (k === 'h') {
      const before = [];
      for (let i = 5; i + 2 < n.length; i += 3) before.push(n[i]);
      g.h.push({ who: n[0], kind: n[1], delta: n[2], t: n[3], before });
    }
  }
  return g;
}

/* the score of each player after entry i: the next one's picture, or the score now */
function after(g, i, who) {
  return i + 1 < g.h.length ? g.h[i + 1].before[who] : g.p[who]?.score;
}

const clock = s => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

function css() {
  if (document.getElementById('css-marcador')) return;
  const st = document.createElement('style');
  st.id = 'css-marcador';
  st.textContent = `
.mrc-list { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 10px; }
.mrc-game { cursor: pointer; border: 2px solid transparent; }
.mrc-game.on { border-color: var(--accent); }
.mrc-players { display: flex; flex-wrap: wrap; gap: 6px 14px; margin-top: 6px; }
.mrc-dot { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; vertical-align: middle; }
.mrc-big { display: flex; flex-wrap: wrap; gap: 10px; margin: 8px 0 4px; }
.mrc-big > div { flex: 1 1 120px; background: var(--card2); border-radius: 12px; padding: 10px 12px; border-top: 4px solid; }
.mrc-big b { display: block; font-size: 30px; line-height: 1.1; }
.mrc-hist { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
.mrc-hist td, .mrc-hist th { padding: 5px 8px; border-bottom: 1px solid var(--line); text-align: left; }
.mrc-hist th { color: var(--dim); font-weight: 500; }
.mrc-hist td.n, .mrc-hist th.n { text-align: right; }
.mrc-title { font-size: 22px; font-weight: 600; }
.mrc-scroll { max-height: 60vh; overflow: auto; }
`;
  document.head.appendChild(st);
}

P.registerPage({
  id: 'marcador',
  name: tr('Marcador'),
  icon: '#',
  render(main) {
    const { h, put } = P;
    css();
    const list = h('div', { class: 'mrc-list' });
    const detail = h('div', {});
    let files = [], chosen = null, alive = true, seen = '';

    const showDetail = (f, g) => {
      const kind = tr(KINDS[g.type] || '');
      const pts = g.p.map((p, i) => {
        let main = String(p.score), sub = '';
        if (g.type === 4 && g.opt[0] === 2) { main = `${p.sets} · ${p.games}`; sub = tr('sets · games'); }
        else if (g.type === 4 && g.opt[0] === 1) { main = `${p.sets}`; sub = tr('games, y %s en el actual', p.score); }
        return h('div', { style: `border-color:${p.color}` },
          h('span', {}, p.name, i === g.winner ? ' ★' : ''), h('b', {}, main),
          sub ? h('span', { class: 'small muted' }, sub) : null,
          p.wins ? h('div', { class: 'small muted' }, tr('Partidas ganadas: %d', p.wins)) : null);
      });
      const rows = [];
      for (let i = g.h.length - 1; i >= 0; i--) {
        const e = g.h[i];
        if (e.kind === 1) { rows.push(h('tr', {}, h('td', {}, clock(e.t)), h('td', { colspan: 3 }, tr('Se reinició la cuenta')))); continue; }
        const p = g.p[e.who];
        if (!p) continue;
        rows.push(h('tr', {},
          h('td', {}, clock(e.t)),
          h('td', {}, h('span', { class: 'mrc-dot', style: `background:${p.color}` }), p.name),
          h('td', { class: 'n' }, (e.delta > 0 ? '+' : '') + e.delta),
          h('td', { class: 'n' }, String(after(g, i, e.who)))));
      }
      const csv = () => {
        const head = ['tiempo', 'jugador', 'cambio', ...g.p.map(p => p.name)];
        const lines = [head];
        g.h.forEach((e, i) => {
          lines.push([clock(e.t), e.kind === 1 ? tr('reinicio') : (g.p[e.who]?.name ?? ''), e.delta,
                      ...g.p.map((_, w) => after(g, i, w))]);
        });
        const q = v => /[",;\n]/.test(String(v)) ? `"${String(v).replace(/"/g, '""')}"` : String(v);
        P.saveBlob(new Blob([lines.map(l => l.map(q).join(',')).join('\n') + '\n'], { type: 'text/csv' }),
                   (g.name || 'marcador').replace(/[^\w\- ]+/g, '_') + '.csv');
      };
      const del = async () => {
        if (!confirm(tr('¿Borrar la partida "%s"? No se puede deshacer.', g.name))) return;
        try { await P.fsDelete(`${DIR}/${f.name}`); P.toast(tr('Partida borrada')); chosen = null; load(); }
        catch (e) { P.toast(e.message, true); }
      };
      put(detail,
        h('div', { class: 'card pad' },
          h('div', { class: 'row' }, h('div', { class: 'grow mrc-title' }, g.name),
            h('span', { class: 'pill' }, kind)),
          h('div', { class: 'small muted' },
            tr('Tiempo de juego %s', clock(g.played)), g.target ? ' · ' + tr('a %d', g.target) : ''),
          h('div', { class: 'mrc-big' }, ...pts),
          h('div', { class: 'btns' },
            h('button', { class: 'btn', onclick: csv, disabled: !g.h.length }, tr('Bajar el historial (CSV)')),
            h('button', { class: 'btn', onclick: () => P.openApp('aos.marcador').then(() => P.toast(tr('Abierta en la placa'))).catch(e => P.toast(e.message, true)) },
              tr('Abrir Marcador en la placa')),
            h('button', { class: 'btn red', onclick: del }, tr('Borrar')))),
        h('div', { class: 'card pad' },
          h('div', { class: 'mrc-title' }, tr('Historial')),
          rows.length
            ? h('div', { class: 'mrc-scroll' }, h('table', { class: 'mrc-hist' },
                h('thead', {}, h('tr', {}, h('th', {}, tr('Tiempo')), h('th', {}, tr('Jugador')),
                  h('th', { class: 'n' }, tr('Cambio')), h('th', { class: 'n' }, tr('Queda')))),
                h('tbody', {}, ...rows)))
            : h('p', { class: 'muted' }, tr('Todavía no se anotó nada'))));
    };

    const choose = async f => {
      chosen = f.name;
      for (const el of list.children) el.classList.toggle('on', el.dataset.f === f.name);
      try { showDetail(f, parse(await P.fsText(`${DIR}/${f.name}`))); }
      catch (e) { P.toast(e.message, true); }
    };

    const load = async (again) => {
      let entries;
      try { entries = await P.fsList(DIR); } catch (e) { P.toast(e.message, true); return; }
      files = entries.filter(e => !e.dir && /^g\d+\.txt$/.test(e.name)).sort((a, b) => b.mtime - a.mtime);
      if (!alive) return;
      /* looking again: nothing to do unless a file changed */
      const sig = files.map(f => f.name + ':' + f.mtime).join(',');
      if (again && sig === seen) return;
      seen = sig;
      if (!files.length) {
        put(list, h('div', { class: 'card' }, h('p', { class: 'muted' },
          tr('Todavía no hay partidas guardadas. Se empiezan en la placa, con Marcador.'))));
        put(detail);
        return;
      }
      const cards = [];
      for (const f of files) {
        let g;
        try { g = parse(await P.fsText(`${DIR}/${f.name}`)); } catch { continue; }
        cards.push(h('div', { class: 'card pad mrc-game' + (f.name === chosen ? ' on' : ''), 'data-f': f.name, onclick: () => choose(f) },
          h('div', { class: 'row' }, h('div', { class: 'grow' }, h('b', {}, g.name)),
            h('span', { class: 'small muted' }, P.fmtDate ? P.fmtDate(f.mtime) : '')),
          h('div', { class: 'small muted' }, tr(KINDS[g.type] || '')),
          h('div', { class: 'mrc-players' }, ...g.p.map(p =>
            h('span', {}, h('span', { class: 'mrc-dot', style: `background:${p.color}` }), `${p.name} `, h('b', {}, String(p.score)))))));
      }
      put(list, ...cards);
      const pick = files.find(f => f.name === chosen) || files[0];
      if (pick) choose(pick);
    };

    put(main, h('h1', {}, tr('Marcador')),
      h('p', { class: 'note' }, tr('Las partidas que lleva la placa: los puntajes y el historial de cada una.')),
      list, detail);
    load();
    /* the board saves as people score: look again now and then */
    const t = setInterval(() => { if (document.visibilityState === 'visible') load(true); }, 15000);
    return () => { alive = false; clearInterval(t); };
  },
});
