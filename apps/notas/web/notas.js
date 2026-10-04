/* The Notes app's page in the board's portal (docs/PORTAL-PAGES.md): the
 * notes on the card as cards, a visual editor with the app's formatting, a
 * task list editor, the raw Markdown, the bin and uploads.
 *
 * tools/install_apps.sh puts this file in the card's /web and the portal
 * loads it; window.P4OS (version 1) is all it uses. Its styles come with it
 * (the .nt* rules, added to the page once).
 *
 * Until the firmware drops its own copy of this page (pageNotas in
 * components/aos_portal/web/app.js), "notas" is the portal's and
 * registerPage() would refuse it: so it registers only when the menu has no
 * Notas of its own, and the portal's goes on working meanwhile. */
const P = window.P4OS;
const { h, put, $, api, toast } = P;
/* the portal's own helpers this page was written with, over P4OS */
const fsText = path => P.fsText(path).catch(e => { if (/no existe/.test(e.message)) return null; throw e; });
const fsList = path => P.fsList(path).catch(e => { if (/no existe/.test(e.message)) return []; throw e; });
const fsPut = (path, data) => P.fsPut(path, data);
const fsDelete = path => P.fsDelete(path);
const fsMkdir = path => P.fsMkdir(path);
const openApp = id => P.openApp(id);
function saveBlob(blob, name) {
  const a = h('a', { href: URL.createObjectURL(blob), download: name });
  document.body.append(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}
const NT_CSS = `
.ntcards { display: grid; grid-template-columns: repeat(auto-fill, minmax(230px, 1fr)); gap: 14px; margin-top: 14px; }
.ntcard { --tag: transparent; background: color-mix(in srgb, var(--tag) 30%, var(--card)); border-radius: var(--radius); padding: 14px 16px; cursor: pointer; min-height: 150px; display: flex; flex-direction: column; gap: 6px; border: 1px solid var(--line); }
.ntcard:hover { filter: brightness(1.08); }
.ntct { font-weight: 650; font-size: 17px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ntpin { float: right; font-size: 13px; }
.ntprev { color: var(--dim); font-size: 13.5px; white-space: pre-line; overflow: hidden; flex: 1; max-height: 7.5em; }
.ntprev .done { text-decoration: line-through; opacity: .7; }
.ntmeta { display: flex; justify-content: space-between; color: var(--dim); font-size: 12px; }
.ntsheet { max-width: 860px; overflow: visible; }
.nttitle { width: 100%; border: 0; background: transparent; font-size: 30px; font-weight: 700; outline: none; padding: 4px 0; }
.nttags { display: flex; gap: 6px; }
.nttag { width: 26px; height: 26px; border-radius: 50%; border: 2px solid transparent; background: var(--card2); cursor: pointer; padding: 0; font-size: 12px; color: var(--dim); }
.nttag.on { border-color: var(--text); }
.ntbar { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; padding: 8px 0 12px; border-bottom: 1px solid var(--line); margin-bottom: 12px; position: sticky; top: 0; background: var(--card); z-index: 2; }
.ntbar select { width: auto; padding: 7px 10px; border-radius: 999px; border: 0; background: var(--card2); }
.ntbar .ntb { padding: 7px 12px; min-width: 38px; }
.ntpop { position: relative; }
.ntpop > summary { list-style: none; padding: 7px 12px; }
.ntpop > summary::-webkit-details-marker { display: none; }
.ntsw { position: absolute; top: 40px; left: 0; z-index: 5; display: flex; gap: 6px; padding: 10px; background: var(--card2); border-radius: 14px; box-shadow: 0 8px 24px rgba(0,0,0,.35); }
.ntswb { width: 28px; height: 28px; border-radius: 50%; border: 1px solid var(--line); cursor: pointer; padding: 0; background: var(--card); color: var(--text); font-weight: 700; }
.ntdoc { outline: none; min-height: 320px; font-size: 17px; line-height: 1.5; }
.ntdoc .nb { --ind: 0; position: relative; padding-left: calc(var(--ind) * 28px); min-height: 1.5em; }
.ntdoc .nb[data-t=h1] { font-size: 29px; font-weight: 700; margin-top: 14px; }
.ntdoc .nb[data-t=h2] { font-size: 22px; font-weight: 700; margin-top: 10px; }
.ntdoc .nb[data-t=h3] { font-weight: 700; margin-top: 6px; }
.ntdoc .nb[data-t=quote] { font-style: italic; color: color-mix(in srgb, var(--text) 75%, var(--dim)); border-left: 4px solid var(--yellow); padding-left: calc(var(--ind) * 28px + 14px); margin: 4px 0; }
.ntdoc .nb[data-t=bullet], .ntdoc .nb[data-t=num], .ntdoc .nb[data-t=check] { padding-left: calc(var(--ind) * 28px + 30px); }
.ntdoc .nb[data-t=bullet]::before, .ntdoc .nb[data-t=num]::before { position: absolute; left: calc(var(--ind) * 28px); width: 24px; text-align: center; color: var(--dim); }
.ntdoc .nb[data-t=bullet]::before { content: attr(data-g); }
.ntdoc .nb[data-t=num]::before { content: attr(data-n); text-align: right; width: 26px; }
.ntdoc .nb[data-t=check]::before { content: ''; position: absolute; left: calc(var(--ind) * 28px + 2px); top: .3em; width: 16px; height: 16px; border: 2px solid var(--dim); border-radius: 5px; cursor: pointer; }
.ntdoc .nb[data-t=check][data-c="1"]::before { content: '✓'; background: var(--yellow); border-color: var(--yellow); color: #000; font-size: 13px; line-height: 16px; text-align: center; font-weight: 800; }
.ntdoc .nb[data-t=check][data-c="1"] { text-decoration: line-through; color: var(--dim); }
.ntdoc .nb[data-t=rule] { padding: 8px 0; }
.ntdoc .nb[data-t=rule] hr { border: 0; border-top: 2px solid var(--line); margin: 0; }
.ntraw { width: 100%; min-height: 420px; font: 14px/1.5 ui-monospace, Menlo, monospace; background: var(--card2); border: 0; border-radius: 12px; padding: 14px; }
.nttasks .nttask { display: flex; align-items: center; gap: 8px; padding: 4px 0 4px calc(var(--ind, 0) * 28px); }
.nttasks input[type=checkbox] { width: 20px; height: 20px; accent-color: var(--yellow); flex: none; }
.nttxt { flex: 1; min-width: 0; border: 0; border-bottom: 1px solid var(--line); background: transparent; padding: 6px 2px; outline: none; }
.nttxt.done { text-decoration: line-through; color: var(--dim); }
.ntprio { width: auto; border: 0; background: var(--card2); border-radius: 8px; padding: 4px 6px; font-weight: 700; }
.ntmini { padding: 4px 9px; font-size: 13px; }
.ntprog { display: flex; align-items: center; gap: 12px; margin-bottom: 10px; color: var(--dim); font-size: 13px; }
.ntprog .ntbarfill { height: 8px; border-radius: 4px; background: var(--yellow); min-width: 8px; }
.ntprog::before { content: ''; }
`;

const NT_DIR = '/notas', NT_TRASH = '/notas/papelera';
const NT_A = { B: 1, I: 2, U: 4, S: 8 };
const NT_FG = ['', 'red', 'orange', 'gold', 'green', 'teal', 'royalblue', 'purple', 'deeppink', 'gray'];
const NT_HL = ['', 'yellow', 'lightgreen', 'lightblue', 'pink', 'orange', 'plum'];
const NT_SZ = ['', 'small', 'large', 'x-large'];
const NT_TAGS = ['', 'red', 'orange', 'yellow', 'green', 'teal', 'blue', 'purple', 'pink'];
/* the app's palettes (nt_theme.c): the editor shows the one of the portal's theme */
const NT_PAL = {
  dark: { fg: ['#ffffff', '#ff453a', '#ff9f0a', '#ffd60a', '#30d158', '#40c8e0', '#409cff', '#bf5af2', '#ff6482', '#98989d'],
          hl: ['', '#7a6400', '#1e6b34', '#1d4e89', '#8a2347', '#8a4a00', '#5e3480'] },
  light: { fg: ['#1c1c1e', '#d70015', '#c93400', '#a05a00', '#248a3d', '#0071a4', '#0040dd', '#8944ab', '#d30f45', '#6c6c70'],
           hl: ['', '#ffe866', '#b8f0c2', '#bfddff', '#ffc2d4', '#ffd3a1', '#e2c9f5'] } };
const NT_TAGC = ['#8e8e93', '#ff453a', '#ff9f0a', '#ffd60a', '#30d158', '#40c8e0', '#0a84ff', '#bf5af2', '#ff375f'];
const NT_SZR = [1, 24 / 28, 36 / 28, 48 / 28];          /* size relative to the body's */
const NT_BUL = ['-', '◦', '▪', '–', '→', '★', '✓', '◆'];
const NT_BUL_SHOW = ['•', '◦', '▪', '–', '→', '★', '✓', '◆'];
const NT_NUMS = ['1. 2. 3.', 'a) b) c)', 'A) B) C)', 'i. ii. iii.', 'I. II. III.'];
const ntDark = () => !matchMedia('(prefers-color-scheme: light)').matches;
const ntPal = () => NT_PAL[ntDark() ? 'dark' : 'light'];

/* ---- the Markdown ---- */
function ntInline(s) {
  /* -> [[text, attr], ...], like parse_inline() */
  const runs = [];
  const add = (t, a) => { if (!t) return; const l = runs[runs.length - 1]; if (l && l[1] === a) l[0] += t; else runs.push([t, a]); };
  const closes = (i, mk) => {
    for (let j = i; j + mk.length <= s.length; j++) {
      if (s[j] === '\\') { j++; continue; }
      if (s.startsWith(mk, j) && j > 0 && s[j - 1] !== ' ') {
        if (mk === '*' && s[j + 1] === '*') { j++; continue; }
        return true;
      }
    }
    return false;
  };
  const span = [];
  let base = 0, flags = 0, under = 0, bold = 0, ital = 0, strike = 0, mark = 0, i = 0;
  const setf = (bit, n) => { flags = n > 0 ? flags | bit : flags & ~bit; };
  while (i < s.length) {
    const c = s[i], a = base | flags;
    if (c === '\\' && i + 1 < s.length && /[!-\/:-@\[-`{-~]/.test(s[i + 1])) { add(s[i + 1], a); i += 2; continue; }
    if (c === '*' || c === '~') {
      const dbl = s[i + 1] === c;
      if (c === '~' && !dbl) { add(c, a); i++; continue; }
      if (c === '*' && dbl && s[i + 2] === '*') {
        if (!(flags & 3) && s[i + 3] && s[i + 3] !== ' ' && closes(i + 3, '***')) { flags |= 3; i += 3; continue; }
        if ((flags & 3) === 3) { flags &= ~3; i += 3; continue; }
      }
      const ml = dbl ? 2 : 1, mk = c === '~' ? '~~' : dbl ? '**' : '*', bit = c === '~' ? NT_A.S : dbl ? NT_A.B : NT_A.I;
      if (flags & bit) { if (i > 0 && s[i - 1] !== ' ') { flags &= ~bit; i += ml; continue; } }
      else if (s[i + ml] && s[i + ml] !== ' ' && closes(i + ml, mk)) { flags |= bit; i += ml; continue; }
      add(c, a); i++; continue;
    }
    if (c === '<') {
      const gt = s.indexOf('>', i);
      const m = gt > 0 && /^<(\/?)([a-zA-Z]+)([^>]*)>$/.exec(s.slice(i, gt + 1));
      if (m) {
        const close = !!m[1], tag = m[2].toLowerCase(), d = close ? -1 : 1;
        let known = true;
        if (tag === 'u' || tag === 'ins') setf(NT_A.U, under = Math.max(0, under + d));
        else if (tag === 'b' || tag === 'strong') setf(NT_A.B, bold = Math.max(0, bold + d));
        else if (tag === 'i' || tag === 'em') setf(NT_A.I, ital = Math.max(0, ital + d));
        else if (tag === 's' || tag === 'del' || tag === 'strike') setf(NT_A.S, strike = Math.max(0, strike + d));
        else if (tag === 'mark') { mark = Math.max(0, mark + d); base = (base & ~(7 << 10)) | ((mark ? 1 : 0) << 10); }
        else if (tag === 'span' || tag === 'font') {
          if (close) { if (span.length) base = span.pop(); }
          else if (span.length < 8) {
            span.push(base);
            const st = /style\s*=\s*["']([^"']*)/.exec(m[3]);
            for (const decl of (st ? st[1] : '').split(';')) {
              const k = decl.split(':')[0].trim(), v = (decl.split(':')[1] || '').trim();
              if (k === 'color') { let x = NT_FG.indexOf(v); if (v === 'yellow') x = 3; base = (base & ~(15 << 6)) | (Math.max(0, x) << 6); }
              else if (k === 'background' || k === 'background-color') base = (base & ~(7 << 10)) | (Math.max(0, NT_HL.indexOf(v)) << 10);
              else if (k === 'font-size') { let x = NT_SZ.indexOf(v); if (v === 'xx-large') x = 3; base = (base & ~(3 << 4)) | (Math.max(0, x) << 4); }
            }
          }
        } else if (tag !== 'br') known = false;
        if (known) { i = gt + 1; continue; }
      }
    }
    add(c, a); i++;
  }
  return runs;
}

function ntMarker(s, prevT, prevS) {
  /* -> [block fields, length of the marker], like marker() */
  let m;
  if ((m = /^(#{3,6}) /.exec(s))) return [{ t: 'h3' }, m[0].length];
  if (s.startsWith('## ')) return [{ t: 'h2' }, 3];
  if (s.startsWith('# ')) return [{ t: 'h1' }, 2];
  if (s.startsWith('> ')) return [{ t: 'quote' }, 2];
  if (s.startsWith('>')) return [{ t: 'quote' }, 1];
  if ((m = /^(?:[-*] )?\[([ xX])\] /.exec(s))) {
    let l = m[0].length, p = /^(!{1,3}) /.exec(s.slice(l));
    const b = { t: 'check', c: m[1] !== ' ', s: 0 };
    if (p) { b.s = p[1].length; l += p[0].length; }
    return [b, l];
  }
  for (let k = 0; k < NT_BUL.length; k++) if (s.startsWith(NT_BUL[k] + ' ')) return [{ t: 'bullet', s: k }, NT_BUL[k].length + 1];
  for (const [g, k] of [['* ', 0], ['+ ', 0], ['• ', 0], ['■ ', 2], ['➤ ', 4], ['✔ ', 6], ['♦ ', 7]]) if (s.startsWith(g)) return [{ t: 'bullet', s: k }, g.length];
  if ((m = /^\d{1,6}[.)] /.exec(s))) return [{ t: 'num', s: 0 }, m[0].length];
  if ((m = /^[ivxlcdmIVXLCDM]{1,8}\. /.exec(s))) {
    const up = /[A-Z]/.test(s[0]), letters = m[0].length === 3 && prevT === 'num' && (prevS === 1 || prevS === 2);
    return [{ t: 'num', s: letters ? (up ? 2 : 1) : (up ? 4 : 3) }, m[0].length];
  }
  if ((m = /^([a-zA-Z]{1,3}\)|[a-zA-Z]\.) /.exec(s))) return [{ t: 'num', s: /[A-Z]/.test(s[0]) ? 2 : 1 }, m[0].length];
  return [{ t: 'p' }, 0];
}

function ntParse(src, fallback) {
  const meta = { title: fallback || '', kind: 'note', tag: 0, pinned: false, hide_done: false, created: '', modified: '' };
  const blocks = [];
  let lines = src.replace(/\r/g, '').split('\n'), hasTitle = false;
  if (/^---\s*$/.test(lines[0] || '')) {
    const end = lines.indexOf('---', 1) >= 0 ? lines.indexOf('---', 1) : lines.findIndex((l, i) => i > 0 && /^---/.test(l));
    if (end > 0) {
      for (const l of lines.slice(1, end)) {
        const c = l.indexOf(':');
        if (c < 0) continue;
        const k = l.slice(0, c).trim();
        let v = l.slice(c + 1).trim();
        if (v.length >= 2 && (v[0] === '"' || v[0] === "'") && v.endsWith(v[0])) v = v[0] === '"' ? v.slice(1, -1).replace(/\\(.)/g, '$1') : v.slice(1, -1);
        if (k === 'title') { meta.title = v; hasTitle = true; }
        else if (k === 'type') meta.kind = v === 'tasks' ? 'tasks' : 'note';
        else if (k === 'color') meta.tag = Math.max(0, NT_TAGS.indexOf(v.toLowerCase()));
        else if (k === 'pinned') meta.pinned = v === 'true' || v === 'yes';
        else if (k === 'hide_done') meta.hide_done = v === 'true';
        else if (k === 'created') meta.created = v;
        else if (k === 'modified') meta.modified = v;
      }
      lines = lines.slice(end + 1);
    }
  }
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  let pt = 'p', ps = 0;
  for (const raw of lines) {
    let k = 0, sp = 0;
    while (k < raw.length && (raw[k] === ' ' || raw[k] === '\t')) { sp += raw[k] === '\t' ? 2 : 1; k++; }
    let s = raw.slice(k), b = { t: 'p', s: 0, i: Math.min(5, sp >> 1), c: false, runs: [] };
    if (s[0] === '\\') s = s.slice(1);
    else if (/^(---|\*\*\*|___)\s*$/.test(s)) { b.t = 'rule'; b.i = 0; s = ''; }
    else {
      const [f, ml] = ntMarker(s, pt, ps);
      Object.assign(b, f);
      s = s.slice(ml);
      if ((b.t === 'bullet' && s.startsWith('\\[')) || (b.t === 'check' && s.startsWith('\\!'))) s = s.slice(1);
    }
    b.runs = ntInline(s);
    blocks.push(b);
    pt = b.t; ps = b.s;
  }
  if (!hasTitle && blocks.length && blocks[0].t === 'h1') meta.title = blocks.shift().runs.map(r => r[0]).join('');
  if (!blocks.length) blocks.push({ t: meta.kind === 'tasks' ? 'check' : 'p', s: 0, i: 0, c: false, runs: [] });
  ntNumber(blocks);
  return { meta, blocks };
}

function ntNumber(blocks) {
  const cnt = [0, 0, 0, 0, 0, 0], sty = [0, 0, 0, 0, 0, 0];
  for (const b of blocks) {
    const k = Math.min(5, b.i || 0);
    for (let j = k + 1; j < 6; j++) cnt[j] = 0;
    if (!['bullet', 'num', 'check'].includes(b.t)) { for (let j = k; j < 6; j++) cnt[j] = 0; b.n = 0; continue; }
    if (b.t === 'num') { if (cnt[k] && sty[k] === b.s) cnt[k]++; else { cnt[k] = 1; sty[k] = b.s; } b.n = cnt[k]; }
    else { cnt[k] = 0; b.n = 0; }
  }
}

function ntRoman(n) {
  const v = [1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1], s = ['m', 'cm', 'd', 'cd', 'c', 'xc', 'l', 'xl', 'x', 'ix', 'v', 'iv', 'i'];
  let o = '';
  for (let i = 0; i < 13; i++) while (n >= v[i]) { o += s[i]; n -= v[i]; }
  return o;
}
function ntAlpha(n) { let o = ''; while (n > 0) { n--; o = String.fromCharCode(97 + n % 26) + o; n = Math.floor(n / 26); } return o; }
function ntLabel(style, n) {
  n = Math.max(1, n || 1);
  return [n + '.', ntAlpha(n) + ')', ntAlpha(n).toUpperCase() + ')', ntRoman(n) + '.', ntRoman(n).toUpperCase() + '.'][style] || n + '.';
}

function ntEsc(t) { return t.replace(/[\\*~<]/g, '\\$&'); }
function ntRunMd(t, a) {
  if (!a) return ntEsc(t);
  const fg = (a >> 6) & 15, hl = (a >> 10) & 7, sz = (a >> 4) & 3;
  let open = '', close = '';
  if (fg || hl || sz) {
    const st = [fg && NT_FG[fg] ? 'color:' + NT_FG[fg] : '', hl && NT_HL[hl] ? 'background:' + NT_HL[hl] : '', sz ? 'font-size:' + NT_SZ[sz] : ''].filter(Boolean).join(';');
    open = '<span style="' + st + '">'; close = '</span>';
  }
  if (a & NT_A.U) { open += '<u>'; close = '</u>' + close; }
  const lead = /^ */.exec(t)[0], trail = t.length > lead.length ? / *$/.exec(t)[0] : '';
  const core = t.slice(lead.length, t.length - trail.length);
  let mo = '', mc = '';
  if (core && (a & 11)) {
    if (a & NT_A.S) { mo += '~~'; mc = '~~' + mc; }
    if (a & NT_A.B) { mo += '**'; mc = '**' + mc; }
    if (a & NT_A.I) { mo += '*'; mc = '*' + mc; }
  }
  return open + ntEsc(lead) + mo + ntEsc(core) + mc + ntEsc(trail) + close;
}
function ntNeedsEsc(t) {
  if (!t) return false;
  const c = t[0];
  if (c === ' ' || c === '\t' || c.charCodeAt(0) >= 0x80 || /[!-\/:-@\[-`{-~0-9]/.test(c)) return true;
  if (/^[a-zA-Z][).]/.test(t)) return true;
  return /^[ivxlcdmIVXLCDM]+\./.test(t);
}
function ntNow() {
  const d = new Date(), p = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}
function ntSerialize(meta, blocks) {
  ntNumber(blocks);
  const t = meta.title || '';
  const q = /[:#"'\\]/.test(t) || /^[ -]/.test(t) || / $/.test(t);
  let o = '---\ntitle: ' + (q ? '"' + t.replace(/["\\]/g, '\\$&') + '"' : t) + '\n';
  o += 'type: ' + (meta.kind === 'tasks' ? 'tasks' : 'note') + '\n';
  if (meta.tag) o += 'color: ' + NT_TAGS[meta.tag] + '\n';
  if (meta.pinned) o += 'pinned: true\n';
  if (meta.hide_done) o += 'hide_done: true\n';
  if (meta.created) o += 'created: ' + meta.created + '\n';
  if (meta.modified) o += 'modified: ' + meta.modified + '\n';
  o += '---\n';
  if (blocks.length === 1 && blocks[0].t === 'p' && !blocks[0].runs.length) return o;
  for (const b of blocks) {
    if (b.t === 'rule') { o += '---\n'; continue; }
    const text = b.runs.map(r => r[0]).join('');
    o += '  '.repeat(b.i || 0);
    if (b.t === 'h1') o += '# ';
    else if (b.t === 'h2') o += '## ';
    else if (b.t === 'h3') o += '### ';
    else if (b.t === 'quote') o += '> ';
    else if (b.t === 'bullet') o += NT_BUL[b.s || 0] + ' ' + (text[0] === '[' ? '\\' : '');
    else if (b.t === 'num') o += ntLabel(b.s || 0, b.n) + ' ';
    else if (b.t === 'check') o += (b.c ? '- [x] ' : '- [ ] ') + (b.s ? '!'.repeat(b.s) + ' ' : text[0] === '!' ? '\\' : '');
    else if (ntNeedsEsc(text)) o += '\\';
    o += b.runs.map(r => ntRunMd(r[0], r[1])).join('') + '\n';
  }
  return o;
}
const ntPlain = blocks => blocks.map(b => b.t === 'rule' ? '———' :
  '  '.repeat(b.i || 0) + (b.t === 'bullet' ? NT_BUL_SHOW[b.s || 0] + ' ' : b.t === 'num' ? ntLabel(b.s, b.n) + ' ' :
  b.t === 'check' ? (b.c ? '☑ ' : '☐ ') : '') + b.runs.map(r => r[0]).join('')).join('\n');

/* A title as a file name, like safe_name() in nt_store.c */
function ntSafeName(t) {
  let s = [...(t || '')].map(c => c < ' ' || '/\\:*?"<>|'.includes(c) ? '-' : c).join('');
  while (new TextEncoder().encode(s).length > 64) s = [...s].slice(0, -1).join('');
  s = s.replace(/^[ .]+|[ .]+$/g, '');
  return s || 'Sin título';
}

/* ---- the visual editor's DOM ---- */
const ntHex = c => { const m = /rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*([\d.]+))?\)/.exec(c); return m && (m[4] === undefined || +m[4] > 0) ? [+m[1], +m[2], +m[3]] : null; };
const ntRgb = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16));
function ntNearest(rgb, list) {
  /* the palette entry nearest to a computed colour (both themes count, and the CSS names a
   * Markdown viewer would have used), or 0 if nothing is close */
  if (!rgb) return 0;
  let best = 0, bd = 2400;
  list.forEach((h, i) => {
    if (!h || !i) return;
    const c = ntRgb(h), d = (c[0] - rgb[0]) ** 2 + (c[1] - rgb[1]) ** 2 + (c[2] - rgb[2]) ** 2;
    if (d < bd) { bd = d; best = i; }
  });
  return best;
}
function ntIndexOf(rgb, kind) {
  if (!rgb) return 0;
  const a = ntNearest(rgb, NT_PAL.dark[kind]), b = ntNearest(rgb, NT_PAL.light[kind]);
  return a || b;
}

function ntRunEl(t, a) {
  const pal = ntPal(), el = h('span', {}, t);
  const st = [];
  if (a & NT_A.B) st.push('font-weight:700');
  if (a & NT_A.I) st.push('font-style:italic');
  const dec = [(a & NT_A.U) && 'underline', (a & NT_A.S) && 'line-through'].filter(Boolean).join(' ');
  if (dec) st.push('text-decoration-line:' + dec);
  const fg = (a >> 6) & 15, hl = (a >> 10) & 7, sz = (a >> 4) & 3;
  if (fg && pal.fg[fg]) st.push('color:' + pal.fg[fg]);
  if (hl && pal.hl[hl]) st.push('background-color:' + pal.hl[hl]);
  if (sz) st.push('font-size:' + NT_SZR[sz].toFixed(3) + 'em');
  if (st.length) el.setAttribute('style', st.join(';'));
  else return document.createTextNode(t);
  return el;
}
function ntBlockEl(b) {
  const el = h('div', { class: 'nb', 'data-t': b.t, 'data-s': b.s || 0, 'data-c': b.c ? 1 : 0, style: '--ind:' + (b.i || 0) });
  if (b.t === 'rule') { el.contentEditable = 'false'; el.append(h('hr')); return el; }
  for (const [t, a] of b.runs) el.append(ntRunEl(t, a));
  if (!b.runs.length) el.append(h('br'));
  return el;
}

/* Back from the DOM to blocks: one per .nb, a <br> in the middle starts another (pastes) */
function ntReadDom(ed) {
  const blocks = [];
  for (const el of [...ed.children]) {
    const t = el.dataset ? el.dataset.t || 'p' : 'p';
    const base = { t, s: +(el.dataset && el.dataset.s) || 0, i: +(el.style && el.style.getPropertyValue('--ind')) || 0, c: el.dataset && el.dataset.c === '1' };
    if (t === 'rule') { blocks.push({ ...base, runs: [] }); continue; }
    const bs = getComputedStyle(el), bsize = parseFloat(bs.fontSize), bweight = +bs.fontWeight, bital = bs.fontStyle === 'italic';
    const blockRgb = ntHex(bs.color);
    let cur = { ...base, runs: [] };
    const push = (txt, a) => { if (!txt) return; const l = cur.runs[cur.runs.length - 1]; if (l && l[1] === a) l[0] += txt; else cur.runs.push([txt, a]); };
    const walk = node => {
      for (const n of node.childNodes) {
        if (n.nodeType === 3) {
          const p = n.parentElement, cs = getComputedStyle(p);
          let a = 0;
          if (+cs.fontWeight >= 600 && bweight < 600) a |= NT_A.B;
          if (cs.fontStyle === 'italic' && !bital) a |= NT_A.I;
          for (let e = p; e && e !== el; e = e.parentElement) {
            const d = getComputedStyle(e).textDecorationLine;
            if (/underline/.test(d) || e.tagName === 'U') a |= NT_A.U;
            if (/line-through/.test(d) || /^(S|STRIKE|DEL)$/.test(e.tagName)) a |= NT_A.S;
            if (!((a >> 10) & 7)) { const bg = ntHex(getComputedStyle(e).backgroundColor); if (bg) a |= ntIndexOf(bg, 'hl') << 10; }
          }
          /* a colour of its own is one that differs from its block's (a ticked box or a
           * quote dims the whole block, and that is not the text's colour) */
          const fc = ntHex(cs.color);
          if (fc && blockRgb && Math.abs(fc[0] - blockRgb[0]) + Math.abs(fc[1] - blockRgb[1]) + Math.abs(fc[2] - blockRgb[2]) > 30) a |= ntIndexOf(fc, 'fg') << 6;
          const r = parseFloat(cs.fontSize) / bsize;
          a |= (r < 0.93 ? 1 : r < 1.14 ? 0 : r < 1.5 ? 2 : 3) << 4;
          push(n.nodeValue.replace(/\u00a0/g, ' ').replace(/\n/g, ' '), a);
        } else if (n.nodeName === 'BR') {
          if (n.nextSibling || n.parentElement !== el) { blocks.push(cur); cur = { ...base, t: base.t === 'check' ? 'check' : base.t, c: false, runs: [] }; }
        } else if (n.nodeType === 1) {
          if (/^(DIV|P|LI|H\d)$/.test(n.nodeName) && cur.runs.length) { blocks.push(cur); cur = { ...base, runs: [] }; }
          walk(n);
        }
      }
    };
    walk(el);
    blocks.push(cur);
  }
  if (!blocks.length) blocks.push({ t: 'p', s: 0, i: 0, c: false, runs: [] });
  ntNumber(blocks);
  return blocks;
}

/* Every top-level child a block; numbers and empty lines kept up to date */
function ntTidy(ed) {
  for (const n of [...ed.childNodes]) {
    if (n.nodeType === 1 && n.classList.contains('nb')) continue;
    if (n.nodeType === 3 && !n.nodeValue.trim()) { n.remove(); continue; }
    const d = h('div', { class: 'nb', 'data-t': 'p', 'data-s': 0, 'data-c': 0, style: '--ind:0' });
    n.replaceWith(d);
    d.append(n);
  }
  if (!ed.children.length) ed.append(ntBlockEl({ t: 'p', runs: [] }));
  const cnt = [0, 0, 0, 0, 0, 0], sty = [0, 0, 0, 0, 0, 0];
  for (const el of ed.children) {
    if (el.dataset.t !== 'rule' && !el.textContent && !el.querySelector('br')) el.append(h('br'));
    const k = Math.min(5, +el.style.getPropertyValue('--ind') || 0), t = el.dataset.t, s = +el.dataset.s || 0;
    for (let j = k + 1; j < 6; j++) cnt[j] = 0;
    if (!['bullet', 'num', 'check'].includes(t)) { for (let j = k; j < 6; j++) cnt[j] = 0; continue; }
    if (t === 'num') { if (cnt[k] && sty[k] === s) cnt[k]++; else { cnt[k] = 1; sty[k] = s; } el.dataset.n = ntLabel(s, cnt[k]); }
    else cnt[k] = 0;
    if (t === 'bullet') el.dataset.g = NT_BUL_SHOW[s] || '•';
  }
}

/* The blocks the selection touches (or the caret's) */
function ntSelBlocks(ed) {
  const sel = getSelection();
  if (!sel.rangeCount) return [];
  const r = sel.getRangeAt(0);
  const up = n => { while (n && n.parentNode !== ed) n = n.parentNode; return n; };
  const a = up(r.startContainer), b = up(r.endContainer);
  if (!a || !b) return [];
  const kids = [...ed.children], i = kids.indexOf(a), j = kids.indexOf(b);
  return i < 0 || j < 0 ? [] : kids.slice(Math.min(i, j), Math.max(i, j) + 1);
}

/* ---- the page ---- */
function renderNotas(main) {
  let entries = [], trashMode = false, filter = 'all', query = '';
  let cur = null;          /* { file, meta, blocks, loadedText, mode: 'visual'|'text' } */
  let dirty = false, saving = false;
  const listSec = h('section'), edSec = h('section', { style: 'display:none' });
  const status = h('span', { class: 'muted small' });
  const say = (t, cls) => { status.textContent = t || ''; status.className = 'small ' + (cls || 'muted'); };

  async function readEntry(dir, f) {
    let text = '';
    try { text = (await fsText(dir + '/' + f.name)) || ''; } catch { text = ''; }
    const doc = ntParse(text, f.name.replace(/\.[^.]+$/, ''));
    return { name: f.name, mtime: f.mtime, size: f.size, doc };
  }

  async function load() {
    say('');
    const dir = trashMode ? NT_TRASH : NT_DIR;
    let files = [];
    try { files = (await fsList(dir)).filter(f => !f.dir && /\.(md|txt)$/i.test(f.name) && f.name[0] !== '.'); }
    catch (e) { put(listBox, h('p', { class: 'bad' }, e.message)); return; }
    put(listBox, h('p', { class: 'muted' }, 'Leyendo ' + files.length + ' notas…'));
    entries = [];
    for (const f of files) entries.push(await readEntry(dir, f));
    entries.sort((a, b) => (b.doc.meta.pinned - a.doc.meta.pinned) || (b.doc.meta.modified || '').localeCompare(a.doc.meta.modified || '') || b.mtime - a.mtime);
    if (!trashMode) try { trashCount = (await fsList(NT_TRASH)).filter(f => !f.dir && /\.(md|txt)$/i.test(f.name)).length; } catch { trashCount = 0; }
    draw();
  }

  const fold = s => s.normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase();
  let trashCount = 0;
  const listBox = h('div', { class: 'ntcards' });
  const search = h('input', { type: 'search', placeholder: 'Buscar', oninput: e => { query = e.target.value; draw(); } });
  const chips = h('div', { class: 'btns' });
  const trashBtn = h('button', { class: 'btn', onclick: () => { trashMode = !trashMode; load(); } });

  function draw() {
    put(chips, [['all', 'Todas'], ['note', 'Notas'], ['tasks', 'Listas']].map(([k, l]) =>
      h('button', { class: 'btn' + (filter === k ? ' pri' : ''), onclick: () => { filter = k; draw(); } }, l)));
    trashBtn.textContent = trashMode ? '← Notas' : 'Papelera' + (trashCount ? ' (' + trashCount + ')' : '');
    const q = fold(query);
    const shown = entries.filter(e => (filter === 'all' || e.doc.meta.kind === filter) &&
      (!q || fold(e.doc.meta.title + '\n' + ntPlain(e.doc.blocks)).includes(q)));
    if (!shown.length) {
      put(listBox, h('p', { class: 'muted' }, trashMode ? 'La papelera está vacía.' : q ? 'Nada coincide con la búsqueda.' : 'Todavía no hay notas.'));
      return;
    }
    put(listBox, shown.map(e => {
      const m = e.doc.meta, tasks = m.kind === 'tasks';
      const checks = e.doc.blocks.filter(b => b.t === 'check' && b.runs.length);
      const body = tasks ? h('div', { class: 'ntprev' }, checks.slice(0, 6).map(b => h('div', { class: b.c ? 'done' : '' }, (b.c ? '☑ ' : '☐ ') + b.runs.map(r => r[0]).join(''))))
        : h('div', { class: 'ntprev' }, ntPlain(e.doc.blocks).slice(0, 400));
      const card = h('div', { class: 'ntcard', style: m.tag ? `--tag:${NT_TAGC[m.tag]}` : '' },
        h('div', { class: 'ntct' }, m.pinned ? h('span', { class: 'ntpin', title: 'Fijada' }, '📌') : null, m.title || 'Sin título'),
        body,
        h('div', { class: 'ntmeta' }, (m.modified || '').replace(/^\d{4}-/, '') +
          (tasks && checks.length ? ' · ' + checks.filter(b => b.c).length + '/' + checks.length : ''), h('span', {}, tasks ? 'lista' : 'nota')));
      if (trashMode) card.append(h('div', { class: 'btns', style: 'margin-top:10px' },
        h('button', { class: 'btn', onclick: ev => { ev.stopPropagation(); restore(e); } }, 'Restaurar'),
        h('button', { class: 'btn red', onclick: ev => { ev.stopPropagation(); purge(e); } }, 'Borrar para siempre')));
      else card.onclick = () => open(e);
      return card;
    }));
  }

  async function restore(e) {
    try {
      const to = await freeName(NT_DIR, e.name.replace(/\.[^.]+$/, ''), null);
      await api('fs/rename?path=' + encodeURIComponent(NT_TRASH + '/' + e.name) + '&to=' + encodeURIComponent(NT_DIR + '/' + to), { method: 'POST' });
      toast('Restaurada');
      load();
    } catch (err) { toast(err.message, true); }
  }
  async function purge(e) {
    if (!confirm('¿Borrar "' + (e.doc.meta.title || e.name) + '" para siempre?')) return;
    try { await fsDelete(NT_TRASH + '/' + e.name); toast('Borrada'); load(); } catch (err) { toast(err.message, true); }
  }

  async function freeName(dir, base, self) {
    const names = new Set((await fsList(dir)).map(f => f.name.toLowerCase()));
    let n = base + '.md';
    for (let k = 2; k < 1000; k++) {
      if (self && n.toLowerCase() === self.toLowerCase()) return n;
      if (!names.has(n.toLowerCase())) return n;
      n = `${base} (${k}).md`;
    }
    return n;
  }

  /* ---- editing ---- */
  const titleIn = h('input', { class: 'nttitle', placeholder: 'Título', oninput: () => touch() });
  const tagBox = h('div', { class: 'nttags' });
  const pinBox = h('label', { class: 'small' });
  const ed = h('div', { class: 'ntdoc', contenteditable: 'true', spellcheck: 'true' });
  const raw = h('textarea', { class: 'ntraw', spellcheck: 'false', oninput: () => touch() });
  const tasksBox = h('div', { class: 'nttasks' });
  const bar = h('div', { class: 'ntbar' });
  const modeBtn = h('button', { class: 'btn' });
  const touch = () => { dirty = true; if (status.textContent.startsWith('Guardad')) say(''); };

  function open(e) {
    cur = { file: e ? e.name : '', meta: e ? { ...e.doc.meta } : null, blocks: e ? e.doc.blocks : null, mtime: e ? e.mtime : 0, size: e ? e.size : 0, mode: 'visual' };
    if (!e) return;
    showEditor();
  }
  function fresh(kind) {
    cur = { file: '', meta: { title: '', kind, tag: 0, pinned: false, hide_done: false, created: ntNow(), modified: '' },
            blocks: [{ t: kind === 'tasks' ? 'check' : 'p', s: 0, i: 0, c: false, runs: [] }], mtime: 0, size: 0, mode: 'visual' };
    showEditor();
    titleIn.focus();
  }

  function drawMeta() {
    put(tagBox, NT_TAGC.map((c, i) => h('button', { class: 'nttag' + (cur.meta.tag === i ? ' on' : ''), title: i ? NT_TAGS[i] : 'sin color',
      style: i ? `background:${c}` : '', onclick: () => { cur.meta.tag = i; touch(); drawMeta(); } }, i ? '' : '∅')));
    put(pinBox, h('input', { type: 'checkbox', checked: cur.meta.pinned, onchange: e => { cur.meta.pinned = e.target.checked; touch(); } }), ' Fijada arriba');
  }

  function showEditor() {
    dirty = false;
    titleIn.value = cur.meta.title;
    drawMeta();
    say(cur.file ? 'notas/' + cur.file : 'Nueva: se guarda al tocar Guardar');
    renderBody();
    convBtn.textContent = cur.meta.kind === 'tasks' ? 'Convertir en nota' : 'Convertir en lista';
    listSec.style.display = 'none';
    edSec.style.display = '';
    window.scrollTo(0, 0);
  }

  function renderBody() {
    const tasks = cur.meta.kind === 'tasks';
    bar.style.display = !tasks && cur.mode === 'visual' ? '' : 'none';
    ed.style.display = !tasks && cur.mode === 'visual' ? '' : 'none';
    raw.style.display = cur.mode === 'text' ? '' : 'none';
    tasksBox.style.display = tasks && cur.mode === 'visual' ? '' : 'none';
    modeBtn.textContent = cur.mode === 'visual' ? 'Ver el Markdown' : (tasks ? 'Ver la lista' : 'Ver con formato');
    if (cur.mode === 'text') raw.value = ntSerialize({ ...cur.meta, title: titleIn.value }, cur.blocks).replace(/^---\n[\s\S]*?\n---\n/, '');
    else if (tasks) drawTasks();
    else { put(ed, cur.blocks.map(ntBlockEl)); ntTidy(ed); }
  }

  /* what is on screen, back into cur.blocks */
  function collect() {
    if (cur.mode === 'text') cur.blocks = ntParse('---\ntitle: x\n---\n' + raw.value, '').blocks;
    else if (cur.meta.kind !== 'tasks') cur.blocks = ntReadDom(ed);
    else {
      /* an empty task is not kept, as in the app */
      cur.blocks = cur.blocks.filter(b => b.runs.length && b.runs.some(r => r[0].trim()));
      if (!cur.blocks.length) cur.blocks.push({ t: 'check', s: 0, i: 0, c: false, runs: [] });
    }
    cur.meta.title = titleIn.value.trim();
  }

  function toggleMode() {
    collect();
    cur.mode = cur.mode === 'visual' ? 'text' : 'visual';
    renderBody();
  }

  /* the visual editor's commands */
  const exec = (c, v) => { ed.focus(); document.execCommand('styleWithCSS', false, false); document.execCommand(c, false, v); touch(); };
  function setBlocks(fn) {
    const bl = ntSelBlocks(ed);
    if (!bl.length) return;
    bl.forEach(fn);
    ntTidy(ed);
    touch();
    syncBar();
  }
  const setType = (t, s) => setBlocks(el => {
    if (el.dataset.t === 'rule') return;
    const same = el.dataset.t === t && (+el.dataset.s || 0) === (s || 0);
    el.dataset.t = same && t !== 'p' ? 'p' : t;
    el.dataset.s = same ? 0 : (s || 0);
    el.dataset.c = 0;
    if (/^h/.test(el.dataset.t)) el.style.setProperty('--ind', 0);
  });
  const indent = d => setBlocks(el => { if (el.dataset.t !== 'rule' && !/^h/.test(el.dataset.t)) el.style.setProperty('--ind', Math.max(0, Math.min(5, (+el.style.getPropertyValue('--ind') || 0) + d))); });
  function fontSize(k) {
    /* the classic trick: size 7 marks the selection, then the marks become our sizes */
    exec('fontSize', '7');
    ed.querySelectorAll('font[size="7"]').forEach(f => {
      const s = h('span', { style: k ? `font-size:${NT_SZR[k].toFixed(3)}em` : '' });
      s.append(...f.childNodes);
      f.replaceWith(s);
    });
    touch();
  }
  function insertRule() {
    const bl = ntSelBlocks(ed);
    const at = bl.length ? bl[bl.length - 1] : ed.lastElementChild;
    const r = ntBlockEl({ t: 'rule', runs: [] }), p = ntBlockEl({ t: 'p', runs: [] });
    at.after(r, p);
    const sel = getSelection(), range = document.createRange();
    range.setStart(p, 0); range.collapse(true); sel.removeAllRanges(); sel.addRange(range);
    ntTidy(ed); touch();
  }

  const typeSel = h('select', { title: 'Estilo del párrafo', onchange: e => { setType(e.target.value, 0); ed.focus(); } },
    [['p', 'Cuerpo'], ['h1', 'Título'], ['h2', 'Encabezado'], ['h3', 'Subtítulo'], ['quote', 'Cita']].map(([v, l]) => h('option', { value: v }, l)));
  const sizeSel = h('select', { title: 'Tamaño', onchange: e => { fontSize(+e.target.value); e.target.value = ''; } },
    h('option', { value: '' }, 'Tamaño'), [[1, 'Chica'], [0, 'Normal'], [2, 'Grande'], [3, 'Enorme']].map(([v, l]) => h('option', { value: v }, l)));
  const bulSel = h('select', { title: 'Viñetas', onchange: e => { if (e.target.value !== '') setType('bullet', +e.target.value); e.target.value = ''; ed.focus(); } },
    h('option', { value: '' }, '• Viñetas'), NT_BUL_SHOW.map((g, i) => h('option', { value: i }, g + '  ' + ['Punto', 'Círculo', 'Cuadrado', 'Guion', 'Flecha', 'Estrella', 'Tilde', 'Rombo'][i])));
  const numSel = h('select', { title: 'Numeradas', onchange: e => { if (e.target.value !== '') setType('num', +e.target.value); e.target.value = ''; ed.focus(); } },
    h('option', { value: '' }, '1. Numeradas'), NT_NUMS.map((g, i) => h('option', { value: i }, g)));
  const swatches = (kind, cb) => {
    const pal = ntPal()[kind];
    return h('div', { class: 'ntsw' }, pal.map((c, i) => h('button', { class: 'ntswb', title: kind === 'fg' ? NT_FG[i] || 'normal' : NT_HL[i] || 'sin resaltado',
      style: c && i ? `background:${c}` : '', onmousedown: e => e.preventDefault(), onclick: () => cb(i) }, i ? '' : (kind === 'fg' ? 'A' : '∅'))));
  };
  const fgBox = h('details', { class: 'ntpop' }, h('summary', { class: 'btn', title: 'Color del texto' }, h('b', { style: 'color:' + ntPal().fg[1] }, 'A')),
    swatches('fg', i => { exec('foreColor', i ? ntPal().fg[i] : ntPal().fg[0]); fgBox.open = false; }));
  const hlBox = h('details', { class: 'ntpop' }, h('summary', { class: 'btn', title: 'Resaltado' }, h('span', { style: 'background:' + ntPal().hl[1] + ';padding:0 4px;border-radius:3px' }, 'ab')),
    swatches('hl', i => { exec('hiliteColor', i ? ntPal().hl[i] : 'transparent'); hlBox.open = false; }));
  const tb = (label, title, fn, style) => h('button', { class: 'btn ntb', title, style, onmousedown: e => e.preventDefault(), onclick: fn }, label);
  put(bar, typeSel,
    tb('B', 'Negrita (⌘B)', () => exec('bold'), 'font-weight:800'),
    tb('I', 'Cursiva (⌘I)', () => exec('italic'), 'font-style:italic'),
    tb('U', 'Subrayado (⌘U)', () => exec('underline'), 'text-decoration:underline'),
    tb('S', 'Tachado', () => exec('strikeThrough'), 'text-decoration:line-through'),
    sizeSel, fgBox, hlBox, bulSel, numSel,
    tb('☑', 'Casillas', () => setType('check', 0)),
    tb('⇤', 'Menos sangría (⇧Tab)', () => indent(-1)),
    tb('⇥', 'Más sangría (Tab)', () => indent(1)),
    tb('—', 'Línea', insertRule),
    tb('Tx', 'Borrar el formato', () => exec('removeFormat')),
    tb('↶', 'Deshacer (⌘Z)', () => exec('undo')),
    tb('↷', 'Rehacer', () => exec('redo')));

  function syncBar() {
    const bl = ntSelBlocks(ed);
    if (bl.length) typeSel.value = ['p', 'h1', 'h2', 'h3', 'quote'].includes(bl[0].dataset.t) ? bl[0].dataset.t : 'p';
  }
  const onSel = () => { if (ed.isConnected && ed.contains(getSelection().anchorNode)) syncBar(); };
  document.addEventListener('selectionchange', onSel);

  ed.addEventListener('keydown', e => {
    if ((e.metaKey || e.ctrlKey) && e.key === 's') return;
    const bl = ntSelBlocks(ed), b = bl[0];
    if (e.key === 'Tab') { e.preventDefault(); indent(e.shiftKey ? -1 : 1); return; }
    if (!b || bl.length > 1) return;
    const t = b.dataset.t, empty = !b.textContent;
    if (e.key === 'Enter' && !e.shiftKey) {
      if (empty && ['bullet', 'num', 'check', 'quote'].includes(t)) {
        e.preventDefault();
        if (+b.style.getPropertyValue('--ind') > 0) b.style.setProperty('--ind', +b.style.getPropertyValue('--ind') - 1);
        else { b.dataset.t = 'p'; b.dataset.s = 0; b.dataset.c = 0; }
        ntTidy(ed); touch(); return;
      }
      if (t === 'p' && b.textContent === '---') {
        e.preventDefault();
        b.replaceWith(ntBlockEl({ t: 'rule', runs: [] }));
        insertRule(); return;
      }
      /* after the browser splits the block: a heading is followed by body text, a box unticked */
      setTimeout(() => {
        const nb = ntSelBlocks(ed)[0];
        if (nb && nb !== b) {
          if (/^h/.test(nb.dataset.t)) { nb.dataset.t = 'p'; nb.dataset.s = 0; }
          nb.dataset.c = 0;
          if (nb.dataset.t === 'check') nb.dataset.s = 0;
        }
        ntTidy(ed);
      });
    }
    if (e.key === 'Backspace' && t !== 'p' && t !== 'rule') {
      const sel = getSelection(), r = sel.getRangeAt(0), pre = document.createRange();
      pre.setStart(b, 0); pre.setEnd(r.startContainer, r.startOffset);
      if (sel.isCollapsed && !pre.toString().length) {
        e.preventDefault();
        b.dataset.t = 'p'; b.dataset.s = 0; b.dataset.c = 0;
        ntTidy(ed); touch();
      }
    }
  });
  ed.addEventListener('input', () => {
    touch();
    /* "- ", "1. ", "[] ", "# ", "> " at the start of a paragraph */
    const b = ntSelBlocks(ed)[0];
    if (b && b.dataset.t === 'p') {
      const txt = b.textContent.replace(/\u00a0/g, ' ');
      const m = [['- ', 'bullet', 0], ['* ', 'bullet', 0], ['1. ', 'num', 0], ['a) ', 'num', 1], ['i. ', 'num', 3], ['[] ', 'check', 0], ['[ ] ', 'check', 0],
        ['# ', 'h1', 0], ['## ', 'h2', 0], ['### ', 'h3', 0], ['> ', 'quote', 0]].find(([k]) => txt === k || (txt.startsWith(k) && b.textContent.length === k.length));
      if (m) {
        b.dataset.t = m[1]; b.dataset.s = m[2];
        b.replaceChildren(h('br'));
        const sel = getSelection(), range = document.createRange();
        range.setStart(b, 0); range.collapse(true); sel.removeAllRanges(); sel.addRange(range);
      }
    }
    ntTidy(ed);
  });
  /* a tap on a box ticks it */
  ed.addEventListener('mousedown', e => {
    const b = e.target.closest && e.target.closest('.nb');
    if (!b || b.dataset.t !== 'check' || b.parentNode !== ed) return;
    const left = b.getBoundingClientRect().left + (+b.style.getPropertyValue('--ind') || 0) * 28;
    if (e.clientX >= left - 4 && e.clientX < left + 30) { e.preventDefault(); b.dataset.c = b.dataset.c === '1' ? 0 : 1; touch(); }
  });
  /* pasted text comes in as text: the styles of another page mean nothing here */
  ed.addEventListener('paste', e => {
    e.preventDefault();
    const t = e.clipboardData.getData('text/plain');
    if (!t.includes('\n')) { document.execCommand('insertText', false, t); return; }
    const parsed = ntParse('---\ntitle: x\n---\n' + t, '').blocks;
    const at = ntSelBlocks(ed).pop() || ed.lastElementChild;
    const els = parsed.map(ntBlockEl);
    at.after(...els);
    if (!at.textContent && at.dataset.t !== 'rule') at.remove();
    ntTidy(ed); touch();
  });

  /* ---- a task list ---- */
  function drawTasks() {
    const bl = cur.blocks;
    const total = bl.filter(b => b.t === 'check' && b.runs.length).length, done = bl.filter(b => b.t === 'check' && b.c && b.runs.length).length;
    const rowOf = (b, k) => {
      const txt = h('input', { class: 'nttxt' + (b.c ? ' done' : ''), value: b.runs.map(r => r[0]).join(''),
        oninput: e => { b.runs = e.target.value ? [[e.target.value, 0]] : []; touch(); },
        onkeydown: e => { if (e.key === 'Enter') { e.preventDefault(); bl.splice(k + 1, 0, { t: 'check', s: 0, i: b.i, c: false, runs: [] }); touch(); drawTasks(); focusTask(k + 1); } } });
      return h('div', { class: 'nttask', style: '--ind:' + (b.i || 0) },
        h('input', { type: 'checkbox', checked: b.c, onchange: e => {
          b.c = e.target.checked;
          for (let j = k + 1; j < bl.length && (bl[j].i || 0) > (b.i || 0); j++) bl[j].c = b.c;
          touch(); drawTasks(); } }),
        txt,
        h('select', { class: 'ntprio', title: 'Prioridad', style: b.s ? 'color:' + ['', '#0a84ff', '#ff9f0a', '#ff453a'][b.s] : '',
          onchange: e => { b.s = +e.target.value; touch(); drawTasks(); } },
          ['–', '!', '!!', '!!!'].map((l, i) => h('option', { value: i, selected: b.s === i }, l))),
        h('button', { class: 'btn ntmini', title: 'Más afuera', onclick: () => { b.i = Math.max(0, (b.i || 0) - 1); touch(); drawTasks(); } }, '⇤'),
        h('button', { class: 'btn ntmini', title: 'Más adentro', onclick: () => { b.i = Math.min(5, (b.i || 0) + 1); touch(); drawTasks(); } }, '⇥'),
        h('button', { class: 'btn ntmini', title: 'Subir', disabled: k === 0, onclick: () => { bl.splice(k - 1, 0, bl.splice(k, 1)[0]); touch(); drawTasks(); } }, '↑'),
        h('button', { class: 'btn ntmini', title: 'Bajar', disabled: k === bl.length - 1, onclick: () => { bl.splice(k + 1, 0, bl.splice(k, 1)[0]); touch(); drawTasks(); } }, '↓'),
        h('button', { class: 'btn ntmini', title: 'Borrar', onclick: () => { bl.splice(k, 1); if (!bl.length) bl.push({ t: 'check', s: 0, i: 0, c: false, runs: [] }); touch(); drawTasks(); } }, '✕'));
    };
    for (const b of bl) if (b.t !== 'check') { b.t = 'check'; b.s = 0; b.c = false; }
    const pending = [], doneRows = [];
    bl.forEach((b, k) => (b.c ? doneRows : pending).push(rowOf(b, k)));
    const add = h('input', { class: 'nttxt', placeholder: '+ Agregar tarea (Enter)', onkeydown: e => {
      if (e.key !== 'Enter' || !e.target.value.trim()) return;
      const empty = bl.findIndex(b => !b.runs.length);
      if (empty >= 0) bl.splice(empty, 1);
      let at = bl.length;
      for (let j = bl.length - 1; j >= 0; j--) if (!bl[j].c) { at = j + 1; break; }
      if (!bl.some(b => !b.c)) at = 0;
      bl.splice(at, 0, { t: 'check', s: 0, i: 0, c: false, runs: [[e.target.value.trim(), 0]] });
      touch(); drawTasks(); $('.nttask-add', tasksBox).focus(); } });
    add.classList.add('nttask-add');
    put(tasksBox,
      total ? h('div', { class: 'ntprog' }, h('div', { class: 'ntbarfill', style: `width:${Math.round(done * 100 / total)}%` }), h('span', {}, `${done} de ${total}`)) : null,
      pending, h('div', { class: 'nttask' }, h('span', { style: 'width:20px' }), add),
      doneRows.length ? [h('h2', {}, 'Completadas (' + doneRows.length + ')'), doneRows] : null);
  }
  const focusTask = k => { const el = tasksBox.querySelectorAll('.nttask .nttxt')[k]; if (el) el.focus(); };

  /* ---- saving ---- */
  async function save() {
    if (saving) return false;
    collect();
    const empty = !cur.meta.title && cur.blocks.every(b => !b.runs.length && b.t !== 'rule');
    if (empty) { say('La nota está vacía', 'bad'); return false; }
    saving = true;
    say('Guardando…');
    try {
      await fsMkdir(NT_DIR);
      /* someone (the board) may have written it since we read it */
      if (cur.file) {
        const now = (await fsList(NT_DIR)).find(f => f.name === cur.file);
        if (now && (now.mtime !== cur.mtime || now.size !== cur.size) &&
            !confirm('La nota cambió en la placa mientras la editabas. ¿Reemplazarla con esta versión?')) { say(''); return false; }
      }
      cur.meta.modified = ntNow();
      if (!cur.meta.created) cur.meta.created = cur.meta.modified;
      const name = await freeName(NT_DIR, ntSafeName(cur.meta.title), cur.file || null);
      const body = ntSerialize(cur.meta, cur.blocks);
      await fsPut(NT_DIR + '/' + name, body);
      if (cur.file && cur.file.toLowerCase() !== name.toLowerCase()) await fsDelete(NT_DIR + '/' + cur.file).catch(() => {});
      cur.file = name;
      const st = (await fsList(NT_DIR)).find(f => f.name === name);
      cur.mtime = st ? st.mtime : 0; cur.size = st ? st.size : 0;
      dirty = false;
      if (cur.meta.kind === 'tasks' && cur.mode === 'visual') drawTasks();
      say('Guardada en notas/' + name, 'ok');
      return true;
    } catch (e) { say('No se pudo guardar: ' + e.message, 'bad'); return false; }
    finally { saving = false; }
  }

  async function del() {
    if (!cur.file) { back(true); return; }
    if (!confirm('¿Mover "' + (titleIn.value || cur.file) + '" a la papelera?')) return;
    try {
      await fsMkdir(NT_TRASH);
      const to = await freeName(NT_TRASH, cur.file.replace(/\.[^.]+$/, ''), null);
      await api('fs/rename?path=' + encodeURIComponent(NT_DIR + '/' + cur.file) + '&to=' + encodeURIComponent(NT_TRASH + '/' + to), { method: 'POST' });
      toast('Movida a la papelera');
      back(true);
    } catch (e) { toast(e.message, true); }
  }

  function download() {
    collect();
    saveBlob(new Blob([ntSerialize(cur.meta, cur.blocks)], { type: 'text/markdown' }), ntSafeName(cur.meta.title) + '.md');
  }

  function convert() {
    collect();
    if (cur.meta.kind === 'note') {
      cur.blocks = cur.blocks.filter(b => b.t !== 'rule' && b.runs.length).map(b => ({ ...b, t: 'check', s: b.t === 'check' ? b.s : 0, c: b.t === 'check' && b.c }));
      if (!cur.blocks.length) cur.blocks.push({ t: 'check', s: 0, i: 0, c: false, runs: [] });
      cur.meta.kind = 'tasks';
    } else cur.meta.kind = 'note';
    touch();
    renderBody();
    convBtn.textContent = cur.meta.kind === 'tasks' ? 'Convertir en nota' : 'Convertir en lista';
  }

  function back(force) {
    if (!force && dirty && !confirm('Hay cambios sin guardar. ¿Volver igual?')) return;
    dirty = false;
    edSec.style.display = 'none';
    listSec.style.display = '';
    load();
  }

  async function uploadNotes(fl) {
    const list = [...fl].filter(f => /\.(md|txt)$/i.test(f.name));
    if (!list.length) { toast('Sólo archivos .md o .txt', true); return; }
    await fsMkdir(NT_DIR);
    for (const f of list) {
      try { await fsPut(NT_DIR + '/' + await freeName(NT_DIR, f.name.replace(/\.[^.]+$/, ''), null), f); }
      catch (e) { toast(f.name + ': ' + e.message, true); return; }
    }
    toast(list.length === 1 ? 'Subida ' + list[0].name : `Subidas ${list.length} notas`);
    load();
  }

  modeBtn.onclick = toggleMode;
  const convBtn = h('button', { class: 'btn', onclick: convert }, 'Convertir');
  /* ⌘S / Ctrl+S while the editor is on screen, wherever the focus is */
  const onKey = e => {
    if ((e.metaKey || e.ctrlKey) && e.key === 's' && edSec.isConnected && edSec.style.display !== 'none') { e.preventDefault(); save(); }
  };
  document.addEventListener('keydown', onKey);
  titleIn.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); if (cur.meta.kind === 'tasks') { const t = $('.nttask-add', tasksBox) || $('.nttxt', tasksBox); if (t) t.focus(); } else ed.focus(); }
  });

  const input = h('input', { type: 'file', multiple: true, accept: '.md,.txt', style: 'display:none', onchange: e => uploadNotes(e.target.files) });
  const drop = h('div', { class: 'drop' }, 'Soltá acá archivos .md o .txt para sumarlos a ', h('b', {}, NT_DIR));
  drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); uploadNotes(e.dataTransfer.files); };

  put(listSec,
    h('div', { class: 'btns', style: 'margin-bottom:14px' },
      h('button', { class: 'btn pri', onclick: () => fresh('note') }, 'Nueva nota'),
      h('button', { class: 'btn pri', onclick: () => fresh('tasks') }, 'Nueva lista'),
      search, trashBtn,
      h('button', { class: 'btn', onclick: () => input.click() }, 'Subir…'), input,
      h('button', { class: 'btn', onclick: () => openApp('aos.notas').then(() => toast('Abriendo Notas en la placa'), e => toast(e.message, true)) }, 'Abrir la app en la placa')),
    chips, listBox, drop);
  put(edSec,
    h('div', { class: 'btns', style: 'margin-bottom:14px' },
      h('button', { class: 'btn', onclick: () => back() }, '← Notas'), status),
    h('div', { class: 'card pad ntsheet' },
      titleIn,
      h('div', { class: 'btns', style: 'margin:6px 0 14px' }, tagBox, pinBox),
      bar, ed, tasksBox, raw),
    h('div', { class: 'btns', style: 'margin-top:14px' },
      h('button', { class: 'btn pri', onclick: save }, 'Guardar en la placa'),
      modeBtn, convBtn,
      h('button', { class: 'btn', onclick: download }, 'Descargar .md'),
      h('button', { class: 'btn red', onclick: del }, 'Mover a la papelera'),
      h('span', { class: 'muted small' }, '⌘S / Ctrl+S guarda')));
  put(main, h('h1', {}, 'Notas'), listSec, edSec,
    h('p', { class: 'note' }, 'Las notas son archivos Markdown en /notas de la tarjeta, los mismos que abre la app. Si la app tiene abierta la nota que guardás acá, la vuelve a leer sola. Lo borrado va a /notas/papelera.'));
  window.onbeforeunload = () => dirty && location.hash === '#notas' ? 'Hay cambios sin guardar' : undefined;
  load();
  /* the page is left */
  return () => {
    document.removeEventListener('keydown', onKey);
    document.removeEventListener('selectionchange', onSel);
    window.onbeforeunload = null;
  };
}

if (P && P.version >= 1 && !document.querySelector('#nav a[data-p="notas"]')) {
  if (!document.getElementById('nt-style')) document.head.append(h('style', { id: 'nt-style' }, NT_CSS));
  P.registerPage({ id: 'notas', name: 'Notas', icon: '✎', render: renderNotas });
} else {
  console.info('notas.js: the portal brings its own Notas page; this one waits for the firmware to drop it');
}
