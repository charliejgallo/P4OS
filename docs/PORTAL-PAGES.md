# An app's own page in the portal

An app on the card can bring a page of its own to the board's web portal,
the way it brings its icon: no firmware to build, no OTA. The page is a
JavaScript module in the app's `web/` folder; `tools/install_apps.sh` puts
it in the card's `/web`, the firmware serves it from there, and the portal
loads it at startup and adds it to its menu.

    apps/<app>/web/<app>.js   ->   /sdcard/web/<app>.js   ->   http://p4os.local/web/<app>.js

`apps/hello_app/web/hello.js` is the template: a page that shows the board's
uptime, reads and saves a text file on the card, and opens the app on the
board.

## Writing one

A page is an ES module that calls `P4OS.registerPage()` once:

```js
const P = window.P4OS;
P.registerPage({
  id: 'hola',            // the address: #hola. [a-z0-9_-]
  name: 'Hola',          // the menu's text
  icon: '☺',             // one character, as the portal's own entries
  render(main) {         // draws the page into main
    P.put(main, P.h('h1', {}, 'Hola'));
    const t = setInterval(/* ... */, 5000);
    return () => clearInterval(t);   // optional: run when the page is left
  },
});
```

The texts are in Spanish, like the rest of the portal. The portal's styles
(`card`, `row`, `btn`, `note`, `val`, `muted`...) are there to use.

## `window.P4OS`, version 1

What a page gets, and all it should use. `P4OS.version` is 1; what is in
version 1 stays as it is, like the apps' `AOS_ABI_VERSION`.

| | |
|---|---|
| `main` | the page's container |
| `h(tag, attrs, ...kids)` | makes an element: `on*` attributes are listeners, `class`, `html` |
| `put(el, ...kids)` | replaces an element's children (null and false are skipped) |
| `$(selector, el)` | `querySelector` |
| `api(path, opts)` | `fetch('/api/' + path)`, JSON back, throws with the portal's error |
| `post(path, body)` | `api` with POST and a JSON body |
| `toast(text, bad)` | the portal's message at the bottom |
| `fmtBytes(n)` | "1,2 MB" |
| `fsText(path)` | a card file as text (throws "no existe" if it is not there) |
| `fsPut(path, data)` | writes a card file (a string or a Blob), any size |
| `fsList(path)` | a folder: `[{ name, dir, size, mtime }]` |
| `fsDelete(path)`, `fsMkdir(path)` | |
| `openApp(id)` | opens an app on the board (`demo.hello`) |
| `registerPage(page)` | see above |

Anything else of the board is in its JSON API (`api('info')`,
`api('bt')`...), the same the portal's own pages use.

## How the portal loads them

- At startup `app.js` lists `/web` and imports every `.js` there, with its
  date in the address (`/web/hello.js?v=<mtime>`) so the browser never keeps
  an old one. A deep link (`#hola`) works: the page is shown once it is
  loaded.
- The apps' pages go in the menu above the system's (Archivos, Ajustes...),
  by name.
- **A broken page does not break the portal.** One that does not load shows
  in the menu as "name ⚠", with the reason on hover; one that throws while
  drawing shows the error in its own space.
- The firmware serves `/web/<name>` from the card for names of
  `[A-Za-z0-9._-]`, one level deep: nothing outside `/web` is reachable
  through it. It is no new door: the portal could already write any file on
  the card.

## Installing and updating

    tools/install_apps.sh p4os.local <app>

uploads the `.so`, the pack and the page. A page needs no restart, only a
reload of the portal; when nothing but pages goes up, the script does not
restart the board. In the simulator the card is `sim/sim_fs`: copy the page
to `sim/sim_fs/web/`.

## Why the card and not the `.so`

The boot scan opens and closes every `.so`; a page inside it would have to
be copied out of each one at every boot. A file on the card is served as it
is, and is replaced with one upload.

The pages that live in the firmware's `app.js` today (Pixel Art, Lua, Maps,
the 3D viewer, Notes) can move to their apps this way, and the firmware gets
smaller.
