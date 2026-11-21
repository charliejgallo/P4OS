/* Metegol's page in the board's portal (docs/PORTAL-PAGES.md): the teams.
 *
 * The teams live in /data/metegol/equipos.txt, which the app writes the
 * first time and reads again when it changes (main/mg_kit.h has the
 * format). Here each team is a card: its name, the three letters of the
 * scoreboard, the shirt's design and colours, and the flag of a national
 * team, with a drawing of the shirt as the board paints it. Saving
 * rewrites the file whole; "the factory's" deletes it, and the app writes
 * the original again the next time it opens. */
const P = window.P4OS;
const tr = P.tr || ((s, ...a) => { let i = 0; return s.replace(/%[sd]/g, () => a[i++]); });
const FILE = '/data/metegol/equipos.txt';
const APP_ID = 'demo.metegol';
const DESIGNS = ['lisa', 'rayas', 'bastones', 'franja', 'banda', 'v', 'mitades', 'cuadros', 'vivos', 'aros'];
const COLORS = [['color1', 'Camiseta'], ['color2', 'Diseño'], ['mangas', 'Mangas'], ['cuello', 'Cuello'],
  ['pantalon', 'Pantalón'], ['medias', 'Medias']];
const HEAD = `# Metegol: los equipos. Lo lee la app al abrir un partido; se edita acá
# o desde la página del portal. Un bloque por equipo:
#   nombre, corto (tres letras del marcador), liga (selecciones o clubes),
#   diseño: lisa, rayas, bastones, franja, banda, v, mitades, cuadros, vivos o aros,
#   color1 (el de la camiseta), color2 (el del diseño), mangas, cuello, pantalon, medias,
#   estadio (el de local, si tiene) y bandera (las selecciones).
# Una bandera son trazos separados por ';': h:c1,c2,... (franjas horizontales;
# c*2 la hace el doble de ancha), v:... (verticales), disc:color:x,y,r,
# cross:color:ancho, canton:color:ancho,alto, rombo:color, cuadros:color:x,y,lado,
# star:color:x,y,r. x, y y los tamaños van de 0 a 1.
`;

function parse(txt) {
  const out = [];
  let t = null;
  for (let line of txt.split('\n')) {
    line = line.trim();
    if (!line || line.startsWith('#')) continue;
    if (line.startsWith('[')) {
      t = { id: line.slice(1, line.indexOf(']')) };
      out.push(t);
      continue;
    }
    const i = line.indexOf('=');
    if (t && i > 0) t[line.slice(0, i).trim()] = line.slice(i + 1).trim();
  }
  return out;
}

function write(teams) {
  const keys = ['nombre', 'corto', 'liga', 'diseño', 'color1', 'color2', 'mangas', 'cuello', 'pantalon', 'medias',
    'estadio', 'bandera'];
  let s = HEAD;
  for (const t of teams) {
    s += `\n[${t.id}]\n`;
    for (const k of keys) if (t[k]) s += `${k} = ${t[k]}\n`;
  }
  return s;
}

const hex = c => '#' + (c || 'ffffff').replace('#', '').padStart(6, '0').slice(0, 6);

/* the shirt's design flattened, as mg_kit_shirt() paints it seen from the front */
function shirt(t, f, l, z) {
  const c1 = hex(t.color1), c2 = hex(t.color2);
  const d = t['diseño'] || 'lisa';
  const a = Math.atan2(l, f) / (2 * Math.PI) + 0.5;
  switch (d) {
    case 'rayas': case 'bastones': {
      const n = d === 'bastones' ? 10 : 16;
      return (Math.floor((a - 0.5) * n + 0.5 + 1000) & 1) ? c2 : c1;
    }
    case 'franja': return z > 0.40 && z < 0.68 ? c2 : c1;
    case 'aros': return (Math.floor(z * 7) & 1) ? c2 : c1;
    case 'banda': return Math.abs(z - (0.52 - 0.30 * l + 0.16 * f)) < 0.15 ? c2 : c1;
    case 'v': return f >= -0.15 && Math.abs(z - (0.55 + 0.42 * Math.abs(l)) - 0.12 * (1 - f)) < 0.11 ? c2 : c1;
    case 'mitades': return l > 0 ? c2 : c1;
    case 'cuadros': return ((Math.floor(a * 14 + 100) + Math.floor(z * 6)) & 1) ? c2 : c1;
    case 'vivos': return Math.abs(f) < 0.16 && Math.abs(l) > 0.7 && z < 0.82 ? c2 : c1;
    default: return c1;
  }
}

function drawShirt(cv, t) {
  const g = cv.getContext('2d');
  const W = cv.width, H = cv.height;
  g.clearRect(0, 0, W, H);
  /* a shirt's outline: body, sleeves, collar */
  const body = (x, y) => x > W * 0.22 && x < W * 0.78 && y > H * 0.12;
  const sleeve = (x, y) => y > H * 0.12 && y < H * 0.45 && (x < W * 0.22 || x > W * 0.78) && x > W * 0.04 && x < W * 0.96 &&
    y - H * 0.12 < (x < W / 2 ? x - W * 0.04 : W * 0.96 - x) * 1.1;
  const img = g.createImageData(W, H);
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      let c = null;
      const u = (x + 0.5) / W * 2 - 1, z = 1 - (y + 0.5) / H;
      if (body(x, y)) {
        const l = -u * 0.95, f = Math.sqrt(1 - l * l * 0.9);
        c = shirt(t, f, l, z * 0.9 + 0.05);
        if (Math.hypot(x - W / 2, y - H * 0.12) < W * 0.1) c = hex(t.cuello);
      } else if (sleeve(x, y)) {
        c = hex(t['diseño'] === 'mitades' ? (u > 0 ? t.color1 : t.color2) : t.mangas);
      }
      if (!c) continue;
      const i = (y * W + x) * 4;
      img.data[i] = parseInt(c.slice(1, 3), 16);
      img.data[i + 1] = parseInt(c.slice(3, 5), 16);
      img.data[i + 2] = parseInt(c.slice(5, 7), 16);
      img.data[i + 3] = 255;
    }
  }
  g.putImageData(img, 0, 0);
  g.fillStyle = hex(t.pantalon);
  g.fillRect(W * 0.26, H * 0.92, W * 0.48, H * 0.08);
}

function css() {
  if (document.getElementById('css-metegol')) return;
  const s = document.createElement('style');
  s.id = 'css-metegol';
  s.textContent = `
.mgGrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:12px}
.mgTeam{display:flex;gap:12px;align-items:flex-start}
.mgTeam canvas{width:84px;height:84px;flex:none;border-radius:10px;background:var(--card2)}
.mgTeam .mgF{display:flex;flex-direction:column;gap:6px;flex:1;min-width:0}
.mgTeam input[type=text]{width:100%;box-sizing:border-box}
.mgCols{display:flex;flex-wrap:wrap;gap:6px}
.mgCols label{display:flex;flex-direction:column;align-items:center;font-size:11px;color:var(--dim)}
.mgCols input[type=color]{width:38px;height:28px;padding:0;border:1px solid var(--line);border-radius:6px;background:none}
.mgTabs{display:flex;gap:8px;margin-bottom:12px}
`;
  document.head.appendChild(s);
}

P.registerPage({
  id: 'metegol',
  name: tr('Metegol'),
  icon: '⚽',
  render(main) {
    const { h, put } = P;
    css();
    let teams = [], league = 'clubes', dirty = false;
    const grid = h('div', { class: 'mgGrid' }, h('div', { class: 'muted' }, tr('Cargando…')));
    const tabs = h('div', { class: 'mgTabs' });
    const saveBtn = h('button', { class: 'btn pri', onclick: () => save() }, tr('Guardar'));

    const field = (t, k, ph, cb) => {
      const i = h('input', { type: 'text', value: t[k] || '', placeholder: ph });
      i.oninput = () => { t[k] = i.value.trim(); dirty = true; if (cb) cb(); };
      return i;
    };

    const card = t => {
      const cv = h('canvas', { width: 84, height: 84 });
      const redraw = () => drawShirt(cv, t);
      const design = h('select', {}, ...DESIGNS.map(d => h('option', { value: d }, d)));
      design.value = t['diseño'] || 'lisa';
      design.onchange = () => { t['diseño'] = design.value; dirty = true; redraw(); };
      const cols = h('div', { class: 'mgCols' }, ...COLORS.map(([k, name]) => {
        const c = h('input', { type: 'color', value: hex(t[k]) });
        c.oninput = () => { t[k] = c.value.slice(1).toUpperCase(); dirty = true; redraw(); };
        return h('label', {}, c, tr(name));
      }));
      const parts = [field(t, 'nombre', tr('Nombre'), null),
        h('div', { class: 'row', style: 'gap:8px;padding:0' }, field(t, 'corto', 'ABC', null), design), cols];
      if (t.liga === 'selecciones') parts.push(field(t, 'bandera', tr('Bandera (trazos)'), null));
      setTimeout(redraw, 0);
      return h('div', { class: 'card pad mgTeam' }, cv, h('div', { class: 'mgF' }, ...parts));
    };

    const draw = () => {
      put(tabs, ['selecciones', 'clubes'].map(lg => h('button', {
        class: 'btn' + (lg === league ? ' pri' : ''), onclick: () => { league = lg; draw(); }
      }, lg === 'clubes' ? tr('Clubes') : tr('Selecciones'))));
      put(grid, teams.filter(t => (t.liga || 'selecciones') === league).map(card));
    };

    async function save() {
      try {
        await P.fsMkdir('/data/metegol').catch(() => {});
        await P.fsPut(FILE, write(teams));
        dirty = false;
        P.toast(tr('Guardado: la placa los toma al volver al menú'));
      } catch (e) { P.toast(e.message, true); }
    }

    async function factory() {
      if (!confirm(tr('¿Volver a los equipos de fábrica? Se pierden los cambios.'))) return;
      try {
        await P.fsDelete(FILE);
        P.toast(tr('Listo: la app los escribe de nuevo al abrirse'));
        teams = [];
        draw();
      } catch (e) { P.toast(e.message, true); }
    }

    put(main,
      h('h2', {}, tr('Metegol: los equipos')),
      h('p', { class: 'note' }, tr('Los nombres, las tres letras del marcador y los colores de cada camiseta. Sin escudos: el diseño y los colores alcanzan.')),
      tabs, grid,
      h('div', { class: 'btns', style: 'margin-top:14px' }, saveBtn,
        h('button', { class: 'btn', onclick: () => P.openApp(APP_ID) }, tr('Abrir en la placa')),
        h('button', { class: 'btn red', onclick: factory }, tr('Volver a los de fábrica'))));

    P.fsText(FILE).then(txt => { teams = parse(txt); draw(); })
      .catch(() => put(grid, h('div', { class: 'muted' }, tr('Todavía no hay equipos: abrí Metegol una vez en la placa.'))));
    const warn = e => { if (dirty) { e.preventDefault(); e.returnValue = ''; } };
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  },
});
