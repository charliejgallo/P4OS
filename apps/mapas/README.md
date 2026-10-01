# Mapas

OpenStreetMap on the whole 720x1280 screen (1280x720 turned): OpenFreeMap's
vector tiles drawn by the board in a dark style, zones saved on the card,
whole zones downloaded to the card for use without a connection, and a
search that answers as you type. Ported from AmoledOS's watch app.

## Using it

| | |
| --- | --- |
| one finger | moves the map; a flick keeps it going |
| two fingers | zoom about the point between them |
| double tap | a level closer, there |
| + / − | a level in or out |
| long press | the coordinates of that point, and a pin there |
| the search bar | search: the downloaded zones first, Photon online after OK |
| ☰ | save this view, the zones, the offline zones, Clima's city, settings |
| the ✕ in the bar | takes the pin off |

Swiping up from the bottom edge still goes home; the left edge is the map's
(it goes back only from the menu and the search).

The first time it opens where Clima is set, or in the centre of Buenos
Aires; after that, where it was left.

## On the card

Everything is in `/maps` on the card (`sim/sim_fs/maps` in the simulator):

| | |
| --- | --- |
| `*.amp` | offline zones: every tile of a zone in one file (format in `main/mp_store.h`) |
| `*.idx` | their names for the search (AIX2, `main/mp_search.h`) |
| `zones.txt` | saved places: name, lat and lon x 1e6, zoom x 10, tab-separated |
| `goto.txt` | "show it on the board": lat, lon, zoom; read once and deleted |
| `cache/z/x/y.mvt` | the tiles seen online |
| `bench.txt` | a timed, scripted pan and zoom (below); deleted when done |

The `.amp`/`.idx` files are the same as AmoledOS's: the zones downloaded
from its portal page, or made with its `tools/map_pack.py`, can be copied
over as they are.

## How it draws

- The worker (core 0) renders into buffers bigger than the screen, of two
  qualities: **full** (the screen's resolution, 96 px of margin, 912x1472)
  while the map is still, and **half** (616x896, enlarged x2, 256 screen px
  of margin) while it moves faster than a full render keeps up with. Which
  one comes next is decided from what a full render has been costing and
  how fast the map moves; a full render the view has already left is
  abandoned.
- Every frame, LVGL's side cuts the screen out of the best buffer (copied
  rows at 1:1, the nearest pixel while scaled), draws the bar and the
  buttons on top, and blits the 720x1280 frame (by DMA2D upright, by the
  PPA turned). The frame is also an LVGL canvas under everything, so
  toasts and the system's panels are drawn over the map by LVGL; while one
  of them is up, the frames go through LVGL instead of the blit.
- The simulator has no blit: the same frames go through the canvas.

## Preferences

| | |
| --- | --- |
| `map_online` | 0: only what is on the card (the menu's *Download*) |
| `map_blit` | 0: frames through LVGL, never blitted (for comparing) |
| `map_inflight` | tiles downloaded at once, 1 (default) to 3 |
| `map_cx`, `map_cy`, `map_z` | where it was left |
| `map_tpl` | the tile URL template, from OpenFreeMap's TileJSON |

## Measuring on the board

`/maps/bench.txt` (empty, `q=text` for the search, or `r`) makes the app go
to the Obelisco at z13 and log, per phase (pan, zoom in, pan, zoom out,
pinch, search), the frames per second, what the cut and the push cost, how
many frames were blitted and how many went through LVGL, and the renders of
each quality with their time. `r` renders the same view at z15, 15.5 and
16, six times each. Every full render logs its parts.

Development switches for the simulator: `MAPAS_OFFLINE=1` keeps it off the
network whatever the preference says; `MAPAS_Q=text` opens that search,
`MAPAS_PICK=1` then picks the first result; `MAPAS_SCREEN=list|search`;
`MAPAS_RBENCH=1` renders the view into the watch's buffer, the half and the
full one and logs the cost of each; `MAPAS_TFULL=ms` pretends a full render
costs that, to see the half buffers the Mac is too fast to need.

## The portal page

The portal's `#mapas` page (`components/aos_portal/web/app.js`, ported from
AmoledOS's `mapas.html`) searches with Photon, saves zones and edits
`zones.txt`, estimates and downloads a zone as one `.amp` with its AIX2
`.idx` (byte for byte what `map_pack.py` writes), and sends `goto.txt`.
Its map is its own canvas renderer of the same tiles in this style (no
MapLibre: the portal loads nothing from a CDN).

Data © OpenStreetMap contributors (ODbL), OpenMapTiles and OpenFreeMap;
search by Photon (komoot).
