/* Oficina's page in the board's portal (docs/PORTAL-PAGES.md).
 *
 * The documents live on the card, in /documentos by default: the page lists
 * a folder (and its subfolders), uploads files to it by picking or dropping
 * them (with progress), downloads, deletes and makes folders, and shows a
 * quick look at a CSV as a table or at a text file. The documents the app
 * opened last come from its list, /data/oficina/recientes.txt, which holds
 * the board's paths (they start with the card's mount point).
 *
 * The app sees a new file when its list is shown again. Only window.P4OS
 * (version 1). */
const P = window.P4OS;
const { h, put, toast, fmtBytes, fmtDate, fsList, fsText, fsPut, fsDelete, fsMkdir, fsUrl, openApp } = P;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });

const HOME = '/documentos', RECENT = '/data/oficina/recientes.txt';
const KINDS = {
  xlsx: 'of-sheet', xlsm: 'of-sheet', ods: 'of-sheet', csv: 'of-sheet', tsv: 'of-sheet', xls: 'of-sheet', numbers: 'of-sheet',
  docx: 'of-doc', doc: 'of-doc', odt: 'of-doc', rtf: 'of-doc', pages: 'of-doc',
  pdf: 'of-pdf', pptx: 'of-slide', odp: 'of-slide', ppt: 'of-slide', key: 'of-slide',
};

if (!document.getElementById('css-oficina')) {
  const st = document.createElement('style');
  st.id = 'css-oficina';
  st.textContent = `
.ofbar { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
.ofpath { display: flex; gap: 4px; align-items: center; flex-wrap: wrap; min-width: 0; }
.ofpath a { cursor: pointer; color: var(--accent); }
.ofrow { display: flex; align-items: center; gap: 12px; padding: 8px 4px; border-bottom: 1px solid var(--line); min-width: 0; }
.ofrow:last-child { border-bottom: 0; }
.ofbadge { flex: none; width: 46px; height: 46px; border-radius: 10px; display: flex; align-items: center; justify-content: center;
  font-size: 11px; font-weight: 700; color: #fff; background: #8e8e93; letter-spacing: .02em; }
.ofbadge.of-sheet { background: #1e8e3e; } .ofbadge.of-doc { background: #2b6cd8; } .ofbadge.of-pdf { background: #d93025; }
.ofbadge.of-slide { background: #e8710a; } .ofbadge.of-dir { background: transparent; color: var(--accent); font-size: 28px; }
.ofname { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; cursor: pointer; }
.ofname small { display: block; color: var(--dim); }
.ofdrop { border: 2px dashed var(--line); border-radius: 14px; padding: 18px; text-align: center; color: var(--dim); }
.ofdrop.on { border-color: var(--accent); color: var(--accent); }
.ofpeek { max-height: 60vh; overflow: auto; }
.ofpeek table { border-collapse: collapse; font-size: 13px; }
.ofpeek td { border: 1px solid var(--line); padding: 3px 8px; white-space: nowrap; }
.ofpeek tr:first-child td { font-weight: 600; background: var(--card2); }
.ofpeek pre { margin: 0; white-space: pre-wrap; word-break: break-word; font-size: 13px; }
@media (max-width: 600px) { .ofrow .btn { padding: 6px 10px; } }
`;
  document.head.appendChild(st);
}

function ext(name) {
  const i = name.lastIndexOf('.');
  return i > 0 ? name.slice(i + 1).toLowerCase() : '';
}

/* a CSV's first lines as rows: its separator guessed as the app does */
function csvRows(text, max) {
  const first = text.split('\n').slice(0, 5).join('\n');
  let sep = ',', best = 0;
  for (const s of [',', ';', '\t', '|']) {
    const n = first.split('\n')[0].split(s).length;
    if (n > best) { best = n; sep = s; }
  }
  const rows = [];
  let row = [], f = '', q = false;
  for (let i = 0; i < text.length && rows.length < max; i++) {
    const c = text[i];
    if (q) {
      if (c === '"') { if (text[i + 1] === '"') { f += '"'; i++; } else q = false; }
      else f += c;
    } else if (c === '"' && f === '') q = true;
    else if (c === sep) { row.push(f); f = ''; }
    else if (c === '\n') { row.push(f.replace(/\r$/, '')); rows.push(row); row = []; f = ''; }
    else f += c;
  }
  if (f || row.length) { row.push(f); rows.push(row); }
  return rows;
}

P.registerPage({
  id: 'oficina',
  name: tr('Oficina'),
  icon: '▦',
  render(main) {
    let dir = HOME;
    const list = h('div', {});
    const path = h('div', { class: 'ofpath' });
    const peek = h('div', { class: 'card', style: 'display:none' });
    const recentBox = h('div', {});
    const progress = h('div', { class: 'small muted' });
    const file = h('input', { type: 'file', multiple: true, style: 'display:none', onchange: () => upload([...file.files]) });
    const drop = h('div', { class: 'ofdrop' }, tr('Soltá acá los documentos para subirlos a esta carpeta, o tocá para elegirlos'));
    drop.onclick = () => file.click();
    drop.ondragover = e => { e.preventDefault(); drop.classList.add('on'); };
    drop.ondragleave = () => drop.classList.remove('on');
    drop.ondrop = e => { e.preventDefault(); drop.classList.remove('on'); upload([...e.dataTransfer.files]); };

    async function upload(files) {
      for (const f of files) {
        try {
          await fsPut(dir + '/' + f.name, f, p => { progress.textContent = tr('Subiendo %s: %s', f.name, Math.round(p * 100) + '%'); });
        } catch (e) { toast(tr('No se pudo subir %s: %s', f.name, e.message), true); }
      }
      progress.textContent = '';
      if (files.length) toast(files.length === 1 ? tr('Subido') : tr('%s archivos subidos', files.length));
      file.value = '';
      load();
    }

    async function show(name) {
      const p = dir + '/' + name, e = ext(name);
      peek.style.display = '';
      put(peek, h('div', { class: 'row' }, h('div', { class: 'grow' }, h('b', {}, name)),
        h('button', { class: 'btn', onclick: () => { peek.style.display = 'none'; } }, tr('Cerrar'))), h('p', { class: 'muted' }, tr('Cargando…')));
      try {
        const r = await fetch(fsUrl(p), { headers: { Range: 'bytes=0-65535' } });
        const buf = await r.arrayBuffer();
        let text;
        try { text = new TextDecoder('utf-8', { fatal: true }).decode(buf); }
        catch { text = new TextDecoder('windows-1252').decode(buf); }
        let body;
        if (e === 'csv' || e === 'tsv') {
          const rows = csvRows(text, 200);
          body = h('table', {}, ...rows.map(row => h('tr', {}, ...row.map(c => h('td', {}, c)))));
        } else {
          body = h('pre', { class: 'mono' }, text);
        }
        put(peek, h('div', { class: 'row' }, h('div', { class: 'grow' }, h('b', {}, name),
          buf.byteLength >= 65536 ? h('span', { class: 'small muted' }, ' ' + tr('(el principio)')) : null),
          h('a', { class: 'btn', href: fsUrl(p, true), download: name }, tr('Bajar')),
          h('button', { class: 'btn', onclick: () => { peek.style.display = 'none'; } }, tr('Cerrar'))),
          h('div', { class: 'ofpeek' }, body));
      } catch (err) { toast(err.message, true); }
    }

    function row(item) {
      const e = ext(item.name), p = dir + '/' + item.name;
      const textlike = ['csv', 'tsv', 'txt', 'md', 'log', 'json', 'xml', 'ini', 'yaml', 'yml', 'toml', 'py', 'c', 'h', 'js', 'html', 'css', 'sh', 'lua'].includes(e);
      const badge = item.dir ? h('div', { class: 'ofbadge of-dir' }, '▸')
        : h('div', { class: 'ofbadge ' + (KINDS[e] || '') }, (e || '?').slice(0, 4).toUpperCase());
      const name = h('div', { class: 'ofname', onclick: () => {
        if (item.dir) { dir = p; load(); } else if (textlike) show(item.name); else window.open(fsUrl(p, true));
      } }, item.name, h('small', {}, item.dir ? tr('Carpeta') : fmtBytes(item.size) + ' · ' + fmtDate(item.mtime)));
      const del = h('button', { class: 'btn red', onclick: async () => {
        if (!confirm(tr('¿Borrar «%s»? No se puede deshacer.', item.name))) return;
        try { await fsDelete(p); toast(tr('Borrado')); load(); } catch (err) { toast(err.message, true); }
      } }, tr('Borrar'));
      return h('div', { class: 'ofrow' }, badge, name,
        item.dir ? null : h('a', { class: 'btn', href: fsUrl(p, true), download: item.name }, tr('Bajar')),
        item.dir ? null : del);
    }

    function crumbs() {
      const parts = dir.split('/').filter(Boolean);
      const kids = [];
      let acc = '';
      parts.forEach((part, i) => {
        acc += '/' + part;
        const to = acc;
        if (i) kids.push(h('span', { class: 'muted' }, '/'));
        kids.push(i === parts.length - 1 ? h('b', {}, part) : h('a', { onclick: () => { dir = to; load(); } }, part));
      });
      put(path, ...kids);
    }

    async function load() {
      crumbs();
      try {
        await fsMkdir(HOME).catch(() => {});
        const items = (await fsList(dir)).filter(x => !x.name.startsWith('.'));
        items.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
        put(list, items.length ? items.map(row) : h('p', { class: 'muted' }, tr('No hay archivos en esta carpeta.')));
      } catch (e) { toast(e.message, true); }
      loadRecent();
    }

    async function loadRecent() {
      const t = await fsText(RECENT).catch(() => null);
      const paths = (t || '').split('\n').map(s => s.trim()).filter(Boolean)
        /* the board's path: the card's part of it */
        .map(s => { const i = s.indexOf('/', s.startsWith('/') ? 1 : 0); return s.startsWith('/sdcard') || s.startsWith('sim_fs') ? s.slice(i) : s; })
        .slice(0, 8);
      if (!paths.length) { put(recentBox); return; }
      put(recentBox, h('div', { class: 'card' }, h('h3', {}, tr('Abiertos hace poco en la placa')),
        ...paths.map(p => {
          const name = p.slice(p.lastIndexOf('/') + 1), e = ext(name);
          return h('div', { class: 'ofrow' }, h('div', { class: 'ofbadge ' + (KINDS[e] || '') }, (e || '?').slice(0, 4).toUpperCase()),
            h('div', { class: 'ofname' }, name, h('small', {}, p.slice(0, p.lastIndexOf('/')) || '/')),
            h('a', { class: 'btn', href: fsUrl(p, true), download: name }, tr('Bajar')));
        })));
    }

    const mkdir = async () => {
      const n = prompt(tr('Nombre de la carpeta nueva'));
      if (!n || n.includes('/')) return;
      try { await fsMkdir(dir + '/' + n); load(); } catch (e) { toast(e.message, true); }
    };

    put(main,
      h('h1', {}, tr('Oficina')),
      h('div', { class: 'card' },
        h('p', { class: 'note' }, tr('Los documentos de la tarjeta. La app Oficina abre y edita planillas (xlsx, ods, csv) y textos; los demás formatos llegan en las próximas versiones.')),
        h('div', { class: 'ofbar' }, h('div', { class: 'grow ofpath' }, path),
          h('button', { class: 'btn', onclick: () => { dir = HOME; load(); } }, tr('Documentos')),
          h('button', { class: 'btn', onclick: mkdir }, tr('Carpeta nueva')),
          h('button', { class: 'btn pri', onclick: () => openApp('aos.oficina').then(() => toast(tr('Abierta en la placa'))).catch(e => toast(e.message, true)) },
            tr('Abrir la app en la placa'))),
        drop, file, progress),
      peek,
      h('div', { class: 'card' }, list),
      recentBox);
    load();
  },
});
