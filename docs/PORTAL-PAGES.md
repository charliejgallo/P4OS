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

The texts are in Spanish, like the rest of the portal: it has no
translations.

- **`render` is synchronous.** What it returns is the cleanup; an `async`
  render returns a promise and the cleanup is lost. Load the data inside,
  as a promise (`P.fsText(path).then(...)`), and draw when it comes.
- **Styles.** The portal's classes are there to use (`card`, `pad`, `row`,
  `grow`, `val`, `btn`, `pri`, `red`, `btns`, `note`, `muted`, `small`,
  `pill`, `prog`, `mono`), and so are its colour variables (`--card`,
  `--card2`, `--line`, `--dim`, `--accent`, `--green`, `--red`,
  `--orange`), which follow the browser's light or dark mode. A page's own
  rules go in one `<style id="css-<app>">` it adds once, with class names
  of its own prefix (Radio's start with `rd`, Cameras' with `cm`).
- **Errors.** `api` and `post` throw an `Error` with the portal's message;
  show it with `toast(message, true)`.

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
| `row(label, value, cls)` | a settings-style row |
| `fmtBytes(n)`, `fmtDate(s)` | "1,2 MB"; a Unix time as "04 oct 2026" |
| `fsText(path)` | a card file as text; `null` if it is not there |
| `fsBytes(path)` | a card file as an `ArrayBuffer` |
| `fsList(path)` | a folder: `[{ name, dir, size, mtime }]`; empty if it is not there |
| `fsPut(path, data, onProgress)` | writes a card file (a string, Blob or buffer), any size; `onProgress(0..1)` is optional |
| `fsDelete(path)`, `fsMkdir(path)` | |
| `fsUrl(path, download)` | the address of a card file, to link or to show in an `<img>`, `<audio>` or `<video>`; it answers `Range` requests, so a player seeks and `fetch(url, { headers: { Range: 'bytes=0-511' } })` reads a header |
| `fsSlug(text, max, default)` | a name the card takes: lower case, a-z 0-9 _ -, no accents |
| `saveBlob(blob, name)` | hands a file to the browser to save |
| `openApp(id)` | opens an app on the board (`demo.hello`) |
| `registerPage(page)` | see above |

Anything else of the board is in its JSON API (`api('info')`,
`api('bt')`...), the same the portal's own pages use.

## Talking to the app

A page runs in the browser and its app on the board; they meet on the card.

- **Files** are the usual way. The page writes with `fsPut` and the app
  reads. An app that is open and has to notice looks at the file's date
  now and then: Notes checks its folder every two seconds and reads the
  open note again when it changed there and has nothing unsaved.
- **`openApp(id)`** brings the app to the front.
- **A JSON API of its own** is firmware, as Radio's `/api/radio` is
  (`components/aos_portal/aos_portal_radio.c`). Most pages do not need
  one.
- **The internet** is the browser's, not the board's: Radio searches
  radio-browser.info from the page. An image from another site drawn into
  a `canvas` taints it (CORS) and it can no longer be exported; Radio asks
  for such logos through `wsrv.nl` when it has to turn them into a JPEG for
  the card.

The pages there are, to copy from:

| Page | What it shows | What to look at |
|---|---|---|
| `apps/hello_app/web/hello.js` | the template | everything basic |
| `apps/camaras/web/camaras.js` | `/cameras.txt` by fields, with a preview | parsing and rewriting a configuration file, keeping its comments |
| `apps/radio/web/radio.js` | the nine keys by drag and drop, the list, a search | a JSON API, a service on the internet, images made in a `canvas` |
| `apps/notas/web/notas.js` | the notes, with an editor | a folder of files the app follows |
| `apps/recorder/web/recorder.js` | the recordings, to listen to, download, delete | `<audio>` straight from the card, a file's header read with a `Range` request |
| `apps/clima/web/clima.js` | the place, searched with a keyboard, and the forecast | a setting the app keeps in a preference, mirrored to a file both sides write |
| `apps/cotiz/web/cotiz.js` | which rates the board shows | a one-line file the app reads |
| `apps/pixel`, `lua`, `mapas`, `visor3d` | drawings, scripts, offline zones, models | big uploads with progress, downloads made in the browser |

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
to `sim/sim_fs/web/` and open `http://127.0.0.1:<port>/#<id>`.

After changing a page, **reload the whole portal** (F5). Going to the same
address with another `#` does not reload it, and the old page stays. A
reload is enough: each page is asked for with its date (`?v=<mtime>`), so
there is no cache to empty.

A release carries every app's page in `web.zip`, for the card's `/web`: the
CI collects `apps/*/web/*.js` by itself.

## Why the card and not the `.so`

The boot scan opens and closes every `.so`; a page inside it would have to
be copied out of each one at every boot. A file on the card is served as it
is, and is replaced with one upload.

Pixel Art, Lua, Maps, the 3D viewer and Notes lived in the firmware's
`app.js` until 2026-10-04 and moved to their apps this way: `app.js` went
from 5300 lines to 2565, and its CSS from 364 to 255. Each brings its styles
in a `<style>` it adds once.

Radio and Cameras were born this way, each with its page in its app; Radio
has its own JSON API for the keys (`/api/radio`, aos_portal_radio.c). Red is
an internal app, so its page (the saved sweeps, `/api/net`) lives in the
firmware's `app.js` with the system's.
