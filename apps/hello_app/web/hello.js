/* hello_app's page in the board's portal: the template for an app's own page
 * (docs/PORTAL-PAGES.md).
 *
 * tools/install_apps.sh puts apps/<x>/web/*.js in the card's /web, and the
 * portal loads each one at startup: no firmware involved. A page is an ES
 * module that calls P4OS.registerPage() once; window.P4OS is all it gets,
 * and P4OS.version says which API that is (1 here). What it shows:
 *   - the board's info (P4OS.api),
 *   - a text file on the card, read and saved (P4OS.fsText, P4OS.fsPut),
 *   - opening the app on the board (P4OS.openApp),
 *   - a timer that stops when the page is left (render returns a function).
 * The texts are in Spanish, like the rest of the portal. */
const P = window.P4OS;
const FILE = '/data/hello.txt';

P.registerPage({
  id: 'hola',
  name: 'Hola',
  icon: '☺',
  render(main) {
    const { h, put } = P;
    const up = h('span', { class: 'val' }, '…');
    const text = h('textarea', { rows: 4, style: 'width:100%' });
    const save = async () => {
      try {
        await P.fsMkdir('/data');
        await P.fsPut(FILE, text.value);
        P.toast('Guardado en ' + FILE);
      } catch (e) { P.toast('No se pudo guardar: ' + e.message, true); }
    };
    put(main,
      h('h1', {}, 'Hola'),
      h('div', { class: 'card' },
        h('p', { class: 'note' }, 'Esta página la trae la app hello_app en su carpeta web/, y el portal la carga de la tarjeta: se actualiza sin tocar el firmware.'),
        h('div', { class: 'row' }, h('div', { class: 'grow' }, 'Encendida hace'), up)),
      h('div', { class: 'card' },
        h('div', { class: 'row' }, h('div', { class: 'grow' }, 'Un archivo de la tarjeta: ' + FILE)),
        text,
        h('div', { class: 'row' },
          h('button', { class: 'btn', onclick: save }, 'Guardar'),
          h('button', { class: 'btn', onclick: () => P.openApp('demo.hello').then(() => P.toast('Abierta en la placa')) },
            'Abrir la app en la placa'))));
    P.fsText(FILE).then(t => { text.value = t ?? '¡Hola desde la tarjeta!'; }).catch(e => P.toast(e.message, true));
    const tick = () => P.api('info').then(i => { up.textContent = Math.floor(i.uptime_s / 60) + ' min'; }).catch(() => {});
    tick();
    const t = setInterval(tick, 5000);
    return () => clearInterval(t);          /* the page is left: stop asking */
  },
});
