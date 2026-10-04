/* The exchange rates app's page in the board's portal (docs/PORTAL-PAGES.md):
 * which rates the board shows, with today's values beside them.
 *
 * The choice is one line in /data/cotiz.txt, the comma-separated keys the
 * app knows (cz_api.c), which it reads within three seconds if it is open.
 * The app draws them in its own fixed order, not the line's, so ticking one
 * more does not reorder the screen. The values come from dolarapi.com, asked
 * from the browser, as the app asks from the board. */
const P = window.P4OS;
const FILE = '/data/cotiz.txt';
const APP_ID = 'aos.cotiz';
/* the app's CZ_ESPECIES, in its order; 'match' is dolarapi's casa or moneda */
const KINDS = [
  { key: 'oficial', name: 'Oficial', match: 'oficial', dollar: true },
  { key: 'blue', name: 'Blue', match: 'blue', dollar: true },
  { key: 'bolsa', name: 'Bolsa (MEP)', match: 'bolsa', dollar: true },
  { key: 'contadoconliqui', name: 'Contado c/liqui', match: 'contadoconliqui', dollar: true },
  { key: 'mayorista', name: 'Mayorista', match: 'mayorista', dollar: true },
  { key: 'cripto', name: 'Cripto', match: 'cripto', dollar: true },
  { key: 'tarjeta', name: 'Tarjeta', match: 'tarjeta', dollar: true },
  { key: 'eur', name: 'Euro', match: 'EUR', dollar: false },
  { key: 'brl', name: 'Real', match: 'BRL', dollar: false },
  { key: 'clp', name: 'Peso chileno', match: 'CLP', dollar: false },
  { key: 'uyu', name: 'Peso uruguayo', match: 'UYU', dollar: false },
];
const DEFAULT = 'oficial,blue,bolsa,contadoconliqui,cripto,tarjeta,eur';   /* the app's LISTA_DEF */

const money = v => v == null ? '–' : '$ ' + Number(v).toLocaleString('es-AR', { minimumFractionDigits: 2, maximumFractionDigits: 2 });

P.registerPage({
  id: 'cotiz',
  name: 'Cotizaciones',
  icon: '$',
  render(main) {
    const { h, put } = P;
    const list = h('div', { class: 'card' }, h('div', { class: 'row muted' }, 'Cargando…'));
    const when = h('p', { class: 'note' });
    const boxes = {};
    let values = {}, gone = false;

    const draw = chosen => {
      put(list, KINDS.map(k => {
        const box = boxes[k.key] = h('input', { type: 'checkbox' });
        box.checked = chosen.includes(k.key);
        const v = values[k.key];
        return h('label', { class: 'row click', style: 'gap:14px' }, box,
          h('div', { class: 'grow' }, k.dollar ? 'Dólar ' + k.name : k.name),
          h('div', { class: 'val' }, v ? `${money(v.compra)} / ${money(v.venta)}` : ''));
      }));
    };

    async function save() {
      const keys = KINDS.filter(k => boxes[k.key].checked).map(k => k.key);
      if (!keys.length) { P.toast('Elegí al menos una', true); return; }
      try {
        await P.fsMkdir('/data');
        await P.fsPut(FILE, keys.join(',') + '\n');
        P.toast('Guardado: la placa lo toma en unos segundos');
      } catch (e) { P.toast(e.message, true); }
    }

    async function load() {
      let line = null;
      try { line = await P.fsText(FILE); } catch (e) { P.toast(e.message, true); }
      const chosen = (line && line.trim() ? line : DEFAULT).split(/[,\s]+/).filter(Boolean);
      if (gone) return;
      draw(chosen);
      try {
        const [d, c] = await Promise.all([fetch('https://dolarapi.com/v1/dolares').then(r => r.json()),
          fetch('https://dolarapi.com/v1/cotizaciones').then(r => r.json())]);
        let latest = '';
        for (const k of KINDS) {
          const v = k.dollar ? d.find(x => x.casa === k.match) : c.find(x => x.moneda === k.match);
          if (v) { values[k.key] = v; if ((v.fechaActualizacion || '') > latest) latest = v.fechaActualizacion; }
        }
        if (gone) return;
        draw(KINDS.filter(k => boxes[k.key] && boxes[k.key].checked).map(k => k.key));
        when.textContent = 'Compra / venta, de dolarapi.com' +
          (latest ? ', actualizado ' + new Date(latest).toLocaleString('es-AR', { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }) : '') + '.';
      } catch (e) { when.textContent = 'No se pudieron traer los valores: ' + e.message; }
    }

    put(main, h('h1', {}, 'Cotizaciones'),
      h('h2', {}, 'Qué muestra la placa'), list, when,
      h('div', { class: 'btns', style: 'margin-top:10px' },
        h('button', { class: 'btn pri', onclick: save }, 'Guardar'),
        h('button', { class: 'btn', onclick: () => P.openApp(APP_ID).then(() => P.toast('Abierta en la placa')).catch(e => P.toast(e.message, true)) }, 'Abrir Cotizaciones en la placa')),
      h('p', { class: 'note' }, 'La placa las muestra siempre en este orden; si la app está abierta, cambia sola en unos segundos.'));
    load();
    return () => { gone = true; };
  },
});
