/*
 * P4OS - the Modbus app's page in the portal: the device profiles.
 *
 * A profile is a CSV file in /modbus/perfiles on the card (the format is in
 * apps/modbus/main/mb_prof.h and the README): this page lists them, edits
 * one as a table, saves it, makes a new one, uploads and downloads them, and
 * tells the board which to use. The app reads a file again when it changes,
 * and while this page looks it sends the values it reads (or plays) through
 * the live channel (docs/PORTAL-PAGES.md, "Live data"); this page sends
 * "perfil=<file>" the same way to choose one.
 */
const P = window.P4OS;
const { h, put } = P;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });

const APP = 'aos.modbus';
const DIR = '/modbus/perfiles';
const LIVE = 'live?app=' + APP;
const TABLES = ['coil', 'di', 'hr', 'ir'];
const TYPES = ['bit', 'u16', 's16', 'hex', 'u32', 's32', 'f32', 'u32cdab', 's32cdab', 'f32cdab'];
const SIMS = ['', 'fijo', 'seno', 'rampa', 'deriva', 'contador', 'alterna', 'azar'];
const COLS = ['tabla', 'direccion', 'nombre', 'tipo', 'escala', 'unidad', 'acceso', 'min', 'max', 'simulacion', 'valor'];

/* ---- the file ---- */

function splitCsv(line) {
  const out = [];
  let cur = '', q = false;
  for (const c of line) {
    if (c === '"') q = !q;
    else if (c === ',' && !q) { out.push(cur.trim()); cur = ''; }
    else cur += c;
  }
  out.push(cur.trim());
  return out;
}

function parse(text) {
  const p = { nombre: '', nota: '', esclavo: '1', velocidad: '', formato: '', periodo: '1000', pts: [], bad: 0 };
  for (let raw of text.replace(/^﻿/, '').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const c = splitCsv(line);
    const k = c[0].toLowerCase();
    const map = { name: 'nombre', note: 'nota', slave: 'esclavo', baud: 'velocidad', format: 'formato', poll: 'periodo' };
    const key = map[k] || k;
    if (['nombre', 'nota', 'esclavo', 'velocidad', 'formato', 'periodo'].includes(key)) p[key] = c[1] || '';
    else if (TABLES.includes(k) || ['holding', 'input', 'coils', 'discrete', 'entrada', 'entradas'].includes(k)) {
      const t = { holding: 'hr', input: 'ir', coils: 'coil', discrete: 'di', entrada: 'di', entradas: 'di' }[k] || k;
      const pt = {};
      COLS.forEach((name, i) => { pt[name] = c[i] ?? ''; });
      pt.tabla = t;
      p.pts.push(pt);
    } else p.bad++;
  }
  return p;
}

function q(s) { s = String(s ?? ''); return /[,"]/.test(s) ? '"' + s.replace(/"/g, '') + '"' : s; }

function serialize(p) {
  const L = ['# P4OS Modbus profile (apps/modbus/README.md, "Perfiles")', 'nombre,' + q(p.nombre)];
  if (p.nota) L.push('nota,' + q(p.nota));
  L.push('esclavo,' + (p.esclavo || 1));
  if (p.velocidad) L.push('velocidad,' + p.velocidad);
  if (p.formato) L.push('formato,' + p.formato);
  L.push('periodo,' + (p.periodo || 1000));
  L.push('# ' + COLS.join(','));
  for (const pt of p.pts) L.push(COLS.map(c => q(pt[c])).join(','));
  return L.join('\n') + '\n';
}

function slug(name) {
  const s = (P.fsSlug ? P.fsSlug(name, 40, 'perfil') : name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')) || 'perfil';
  return s + '.csv';
}

/* ---- the page ---- */

function css() {
  if (document.getElementById('css-modbus')) return;
  document.head.appendChild(h('style', { id: 'css-modbus' }, `
    .mbw { display: grid; grid-template-columns: minmax(220px, 300px) 1fr; gap: 16px; align-items: start; }
    @media (max-width: 860px) { .mbw { grid-template-columns: 1fr; } }
    .mbl button.mbi { display: block; width: 100%; text-align: left; padding: 10px 12px; border: 0; border-radius: 10px;
      background: transparent; color: inherit; cursor: pointer; font: inherit; }
    .mbl button.mbi:hover { background: var(--card2); }
    .mbl button.mbi.on { background: var(--card2); outline: 2px solid var(--accent); }
    .mbl .mbn { font-weight: 600; }
    .mbt { width: 100%; border-collapse: collapse; font-size: 13px; }
    .mbt th { text-align: left; color: var(--dim); font-weight: 500; padding: 4px; white-space: nowrap; }
    .mbt td { padding: 2px 3px; border-top: 1px solid var(--line); }
    .mbt input, .mbt select { width: 100%; box-sizing: border-box; padding: 5px 6px; font: inherit;
      border: 1px solid var(--line); border-radius: 6px; background: var(--card); color: inherit; }
    .mbt select { min-width: 72px; }
    .mbt input { min-width: 60px; }
    .mbt input.w { min-width: 150px; }
    .mbt input.n { max-width: 84px; }
    .mbt { min-width: 1080px; }
    .mbt td.v { font-weight: 600; color: var(--orange); white-space: nowrap; min-width: 80px; }
    .mbt td.v.e { color: var(--dim); font-weight: 400; }
    .mbh { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; margin-bottom: 12px; }
    .mbh label { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--dim); }
    .mbh input { padding: 7px 8px; font: inherit; font-size: 14px; border: 1px solid var(--line); border-radius: 8px;
      background: var(--card); color: var(--fg, inherit); }
    .mbscroll { overflow-x: auto; }
  `));
}

P.registerPage({
  id: 'modbus',
  name: tr('Modbus'),
  icon: '⇄',
  render(main) {
    css();
    let files = [], sel = null, prof = null, dirty = false, live = null, gone = false;
    const listBox = h('div', { class: 'mbl' });
    const edBox = h('div');
    const fileIn = h('input', { type: 'file', accept: '.csv,text/csv', style: 'display:none', onchange: upload });

    async function loadList() {
      try {
        await P.fsMkdir('/modbus').catch(() => {});
        await P.fsMkdir(DIR).catch(() => {});
        files = (await P.fsList(DIR)).filter(f => !f.dir && f.name.endsWith('.csv') && !f.name.startsWith('.'))
          .map(f => f.name).sort();
      } catch (e) { P.toast(e.message, true); }
      drawList();
    }

    function drawList() {
      put(listBox,
        h('div', { class: 'card pad' },
          h('h2', { style: 'margin-top:0' }, tr('Perfiles')),
          files.length ? null : h('p', { class: 'note' }, tr('No hay perfiles todavía. La app escribe cuatro de ejemplo la primera vez que se abre.')),
          ...files.map(f => h('button', { class: 'mbi' + (f === sel ? ' on' : ''), onclick: () => choose(f) }, h('div', { class: 'mbn' }, f))),
          h('div', { class: 'btns', style: 'margin-top:12px' },
            h('button', { class: 'btn pri', onclick: create }, tr('Nuevo')),
            h('button', { class: 'btn', onclick: () => fileIn.click() }, tr('Subir .csv'))),
          fileIn,
          h('p', { class: 'note small' }, tr('Un perfil dice qué tiene un equipo Modbus y dónde. En Maestro la app lo lee entero; en Esclavo hace de ese equipo.'))));
    }

    async function choose(f) {
      if (dirty && !confirm(tr('Hay cambios sin guardar. ¿Descartarlos?'))) return;
      sel = f;
      dirty = false;
      try {
        const t = await P.fsText(`${DIR}/${f}`);
        prof = parse(t || '');
        if (prof.bad) P.toast(tr('%s líneas sin entender', prof.bad), true);
      } catch (e) { P.toast(e.message, true); prof = null; }
      drawList();
      drawEditor();
    }

    function mark() { dirty = true; }

    function field(label, key, attrs = {}) {
      return h('label', {}, label, h('input', { value: prof[key] ?? '', ...attrs, oninput: e => { prof[key] = e.target.value.trim(); mark(); } }));
    }

    function cell(pt, key, kind) {
      if (kind === 'sel') {
        const opts = key === 'tabla' ? TABLES : key === 'tipo' ? TYPES : key === 'acceso' ? ['r', 'rw'] : SIMS;
        return h('td', {}, h('select', { onchange: e => { pt[key] = e.target.value; mark(); } },
          ...opts.map(o => h('option', { value: o, selected: (pt[key] || (key === 'acceso' ? 'r' : '')) === o ? '' : null }, o || '—'))));
      }
      return h('td', {}, h('input', { class: kind || '', value: pt[key] ?? '', oninput: e => { pt[key] = e.target.value.trim(); mark(); } }));
    }

    function drawEditor() {
      if (!prof) { put(edBox, h('div', { class: 'card pad' }, h('p', { class: 'note' }, tr('Elegí un perfil de la lista, o hacé uno nuevo.')))); return; }
      const rows = prof.pts.map((pt, i) => h('tr', {},
        cell(pt, 'tabla', 'sel'), cell(pt, 'direccion', 'n'), cell(pt, 'nombre', 'w'), cell(pt, 'tipo', 'sel'), cell(pt, 'escala', 'n'),
        cell(pt, 'unidad', 'n'), cell(pt, 'acceso', 'sel'), cell(pt, 'min', 'n'), cell(pt, 'max', 'n'), cell(pt, 'simulacion', 'sel'),
        cell(pt, 'valor', 'n'),
        h('td', { class: 'v e', id: 'mbv' + i }, '—'),
        h('td', {}, h('button', { class: 'btn small', title: tr('Quitar'), onclick: () => { prof.pts.splice(i, 1); mark(); drawEditor(); } }, '✕'))));
      put(edBox, h('div', { class: 'card pad' },
        h('h2', { style: 'margin-top:0' }, prof.nombre || sel),
        h('div', { class: 'muted small', style: 'margin-bottom:10px' }, `${DIR}/${sel}`),
        h('div', { class: 'mbh' },
          field(tr('Nombre'), 'nombre'), field(tr('Nota'), 'nota'),
          field(tr('Esclavo (unidad)'), 'esclavo', { inputmode: 'numeric' }),
          field(tr('Velocidad'), 'velocidad', { placeholder: '9600', inputmode: 'numeric' }),
          field(tr('Formato'), 'formato', { placeholder: '8N1' }),
          field(tr('Leerlo cada (ms)'), 'periodo', { inputmode: 'numeric' })),
        h('div', { class: 'mbscroll' }, h('table', { class: 'mbt' },
          h('thead', {}, h('tr', {}, ...[tr('Tabla'), tr('Dirección'), tr('Nombre'), tr('Tipo'), tr('Escala'), tr('Unidad'), tr('Acceso'),
            tr('Mín'), tr('Máx'), tr('Simulación'), tr('Inicial'), tr('En la placa'), ''].map(t => h('th', {}, t)))),
          h('tbody', {}, ...rows))),
        h('div', { class: 'btns', style: 'margin-top:12px' },
          h('button', { class: 'btn', onclick: addRow }, tr('Agregar punto')),
          h('button', { class: 'btn pri', onclick: save }, tr('Guardar')),
          h('button', { class: 'btn', onclick: useIt }, tr('Usar en la placa')),
          h('button', { class: 'btn', onclick: download }, tr('Descargar')),
          h('button', { class: 'btn', onclick: duplicate }, tr('Duplicar')),
          h('button', { class: 'btn red', onclick: remove }, tr('Borrar'))),
        h('p', { class: 'note small' }, tr('Tablas: coil, di (entradas), hr (holding), ir (input). Tipos de 32 bits: el registro alto primero; los "cdab", el bajo primero. Valor = crudo × escala. La simulación es para cuando la app hace de esclavo: se mueve entre mín y máx (contador: sube máx por hora). La columna "En la placa" muestra lo que la app lee o hace ahora, si está abierta con este perfil.'))));
    }

    function addRow() {
      const last = prof.pts[prof.pts.length - 1];
      const words = last && /32/.test(last.tipo) ? 2 : 1;
      prof.pts.push({ tabla: last ? last.tabla : 'hr', direccion: last ? String((parseInt(last.direccion) || 0) + words) : '0',
        nombre: tr('Valor %s', prof.pts.length + 1), tipo: last && last.tabla !== 'coil' && last.tabla !== 'di' ? 'u16' : (last ? 'bit' : 'u16'),
        escala: '1', unidad: '', acceso: 'r', min: '', max: '', simulacion: '', valor: '' });
      mark();
      drawEditor();
    }

    async function save() {
      if (!prof) return;
      try {
        await P.fsPut(`${DIR}/${sel}`, serialize(prof));
        dirty = false;
        P.toast(tr('Guardado: la app lo vuelve a leer sola'));
      } catch (e) { P.toast(e.message, true); }
    }

    async function useIt() {
      if (dirty) await save();
      try {
        await P.openApp(APP);
        await new Promise(r => setTimeout(r, 1200));
        await P.api(LIVE, { method: 'POST', body: 'perfil=' + sel });
        P.toast(tr('La placa usa %s', prof.nombre || sel));
      } catch (e) { P.toast(e.message, true); }
    }

    function download() {
      P.saveBlob(new Blob([serialize(prof)], { type: 'text/csv' }), sel);
    }

    async function newFile(base, p) {
      let name = slug(base), k = 2;
      while (files.includes(name)) name = slug(base).replace(/\.csv$/, `-${k++}.csv`);
      await P.fsPut(`${DIR}/${name}`, serialize(p));
      await loadList();
      dirty = false;
      await choose(name);
    }

    async function create() {
      const n = prompt(tr('Nombre del perfil nuevo'));
      if (!n) return;
      try { await newFile(n, { nombre: n, nota: '', esclavo: '1', velocidad: '9600', formato: '8N1', periodo: '1000', pts: [] }); }
      catch (e) { P.toast(e.message, true); }
    }

    async function duplicate() {
      const n = prompt(tr('Nombre de la copia'), (prof.nombre || '') + ' 2');
      if (!n) return;
      try { await newFile(n, { ...prof, nombre: n, pts: prof.pts.map(p => ({ ...p })) }); }
      catch (e) { P.toast(e.message, true); }
    }

    async function remove() {
      if (!confirm(tr('¿Borrar %s de la tarjeta? No se puede deshacer.', sel))) return;
      try {
        await P.fsDelete(`${DIR}/${sel}`);
        sel = null; prof = null; dirty = false;
        await loadList();
        drawEditor();
      } catch (e) { P.toast(e.message, true); }
    }

    async function upload(e) {
      const f = e.target.files[0];
      e.target.value = '';
      if (!f) return;
      const text = await f.text();
      const p = parse(text);
      if (!p.pts.length) { P.toast(tr('%s no parece un perfil: no tiene puntos', f.name), true); return; }
      try { await newFile(p.nombre || f.name.replace(/\.csv$/i, ''), p); P.toast(tr('Subido')); }
      catch (err) { P.toast(err.message, true); }
    }

    /* what the board reads or plays, while this page looks */
    const tick = async () => {
      if (gone) return;
      try {
        const j = await P.api(LIVE + '&key=valores');
        live = j;
        if (prof && j.perfil === sel && !dirty) {
          for (const p of j.pts) {
            const td = document.getElementById('mbv' + p.i);
            if (!td) continue;
            td.textContent = p.e ? '—' : p.v;
            td.className = 'v' + (p.e ? ' e' : '');
          }
        }
      } catch { /* the app is closed or has no profile */ }
      if (!gone) setTimeout(tick, 1000);
    };

    put(main, h('h1', {}, tr('Modbus')), h('div', { class: 'mbw' }, listBox, edBox));
    drawList();
    drawEditor();
    loadList();
    tick();
    return () => { gone = true; };
  },
});
