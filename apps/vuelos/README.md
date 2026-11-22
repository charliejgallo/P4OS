# Vuelos

A flight radar: a place on the map and the aircraft around it, live, on the
whole 720x1280 screen (1280x720 turned). The map is Mapas' (OpenFreeMap's
vector tiles drawn by the board), in a muted style so that the aircraft
stand out; the aircraft come from a free ADS-B service or from a receiver
of one's own on the network.

## Using it

| | |
| --- | --- |
| one finger | moves the map; a flick keeps it going |
| two fingers | zoom about the point between them |
| double tap | a level closer, there |
| tap an aircraft | its card: flight, registration, type, altitude, ground speed, vertical speed, track, squawk, distance and bearing, and origin and destination when known |
| the target on the card | the map follows that aircraft |
| long press | the place goes there |
| + / − | a level in or out |
| the house | back to the place, at a zoom that shows the radius |
| the bar, or the list button | the aircraft by distance or by altitude; a row goes to it |
| the gear | the settings |

With a gamepad: the cross (or the stick) moves the map, L and R zoom, A
picks the aircraft nearest the centre (and the next one at every press), B
lets it go, START opens the list. In the panels, B or START goes back.

What is drawn:

- **The place**, a dot in a ring, with **distance rings** at a round step
  (NM or km, by the units) and the radius asked for a little brighter.
- **The airports** of the pack: large ones from zoom 5.5, medium from 7.5,
  small from 9.5, with their IATA code (or ICAO).
- **The aircraft**: a shape by kind (airliner, wide-body, regional or
  business, light aircraft, helicopter, glider, balloon, drone, ground
  vehicle), from the emitter category the source sends or, without one, from
  the type designator; turned by the track; coloured by altitude (warm low,
  cold high, grey on the ground). The callsign under each one and, from
  zoom 9, its altitude, where the labels do not run into each other.
- **The trails** of the last minutes (1, 3, 5 or 10; the picked aircraft
  shows all it has), coloured by the altitude of each stretch.
- A red ring around an aircraft squawking **7500, 7600 or 7700**.

Between two readings each aircraft goes on by its ground speed and track
from the time of its fix. When the next reading comes, the difference
between where it was being drawn and where the new fix puts it fades out
over a second: it glides, it never jumps. A source that answers every few
minutes (adsb.lol without a key, OpenSky without an account) still moves
smoothly, but turns are only seen at the next reading.

**Alerts** (each one can be switched off): an aircraft passing overhead
(within 3 km and below 12 000 ft by default, both settable in the page), an
emergency squawk, a military aircraft (when the source flags it: airplanes.live,
adsb.lol and readsb with its database do). A toast and, if wanted, a beep;
once per aircraft. Only while the app is open: in the background it asks
nothing of the sources.

## The sources, their terms and how they are respected

| Source | What is asked | Its limit | What the app does |
| --- | --- | --- | --- |
| [airplanes.live](https://airplanes.live/api-guide/) (default) | `https://api.airplanes.live/v2/point/<lat>/<lon>/<NM>` | one request a second, 250 NM at most; **non-commercial use**, no SLA, no key for now | one request every 5 s |
| [adsb.lol](https://www.adsb.lol/docs/open-data/api/) | `https://api.adsb.lol/v2/lat/<lat>/lon/<lon>/dist/<NM>` | without a key, 500 requests a day per address; free keys give 1 000 (on request) or 7 200 (to feeders) a day, sent as `x-api-key`; over the quota, 429. Data under the **ODbL** | the day's requests spread evenly: 500 a day is one every 173 s. The key and its daily number are typed in the page. Requests are counted per UTC day (preferences `vl_dayno`, `vl_dayused`) and stop at the number |
| [OpenSky Network](https://openskynetwork.github.io/opensky-api/rest.html) | `https://opensky-network.org/api/states/all?lamin=…&lomin=…&lamax=…&lomax=…&extended=1` | anonymous: 400 credits a day, 10 s resolution; with an API client of an account (OAuth2 client credentials, a 30-minute token): 4 000. A request costs 1 to 4 credits by the box's area (≤25, ≤100, ≤400, more square degrees). For research and **non-commercial use** | the box's cost is worked out from the radius, and the day's credits spread evenly (anonymous, 100 km around Madrid: one every 216 s; with a client, every 22 s). The client's id and secret are typed in the page; the token is asked for with a POST and renewed before it expires |
| A receiver of one's own | the address typed in the page, e.g. `http://<raspberry>/tar1090/data/aircraft.json` (readsb, dump1090-fa, tar1090) | it is yours | every 2 s |

Every source is asked for the radius set (10 to 400 km, 250 NM at most for
the first two) around the place; the app keeps the 200 nearest aircraft.
After a failure it waits a reading more; after a 429, ten minutes; after a
401 or 403, two minutes.

**Origin and destination** come from [adsbdb](https://github.com/mrjackwills/adsbdb)
(`https://api.adsbdb.com/v0/callsign/<callsign>`, free, no key): one request
when an aircraft is picked, kept in RAM for the session and never written to
the card. Its route data is David J. Taylor's and may not be copied into
another database, which the app does not do; a route by callsign can be
stale, so the card says nothing when the service does not know it. Off in
the settings, and never asked for with one's own receiver.

On screen and in the store's text the sources are named, as their terms
ask: the attribution line at the bottom says the map's sources and the
aircraft's.

## On the card

| | |
| --- | --- |
| `/data/vuelos.txt` | the settings, written by the app and by the page (format in `main/vl_conf.c`); the keys of the services live here and nowhere else |
| `/apps/vuelos_p4.pak` | the airports (below); without it the map has none |
| `/maps/` | Mapas' tile cache and offline zones, shared: a tile seen in one app is not downloaded again by the other, and a zone downloaded for Mapas also works here (the files are written through a `.part` and renamed, so two apps never see half a tile) |

The first place is Clima's city (its preferences), or the centre of Buenos
Aires, as Mapas starts. A place by name, the receiver's address and the
keys are typed in the page; on the board, a long press or "Use the centre
of the map" chooses one, and "Weather's city" takes Clima's.

## The airports pack

`vuelos_p4.pak` is made from [OurAirports](https://ourairports.com/data/)'
`airports.csv`, which is in the public domain:

    python3 apps/vuelos/tools/gen_airports.py airports.csv -o vuelos_p4.pak

Every large and medium airport and the small ones with a four-letter code
(`--heliports` adds heliports with one). 48 bytes an airport, sorted by
latitude; the app reads it whole into PSRAM. Made from the CSV of
2026-11-21: 19 575 airports (1 174 large, 4 108 medium, 14 293 small),
918 KB; with `--heliports`, 22 500 and 1 055 KB. The map tests the
longitude before anything else, since a band of latitude goes round the
world. `tools/sample_airports.csv` is
a few rows around Madrid for the simulator's tests.

## The portal page

`web/vuelos.js` (`#vuelos`): the place searched by name with Open-Meteo's
geocoding (as Clima's page) or taken from Clima; the source, with what each
one allows, and its key, client or address; the radius, the units, the
trails, the labels, the routes; the alerts. It writes `/data/vuelos.txt`,
which the app takes again within two seconds. While the app is open the
page also shows **what the board sees**: a table of the aircraft of the last
reading, sorted by any column, from the live channel (`/api/live`, key
`ac`), made only while a page is looking.

## Testing

Only against the fake server, never against the real services:

    python3 apps/vuelos/tools/fake_adsb.py --port 8946 --emergency
    cd sim && VUELOS_FAKE=http://127.0.0.1:8946 P4_SIM_PORTAL_PORT=8146 ./build-vuelos/p4os_sim

`VUELOS_FAKE` sends airplanes.live, adsb.lol, OpenSky (and its token) and
adsbdb to that address, keeping each one's path; one's own receiver is
reached by its own address (`url=http://127.0.0.1:8946/data/aircraft.json`).
The fake server invents forty aircraft around a point (straight lines
across the area, arrivals and departures through the centre, one passing
overhead low, a helicopter going round, a glider in a thermal, a balloon,
a military transport, and with `--emergency` one squawking 7700), answers
in each service's shape, logs every request with the time since the last
one, and answers 429 to airplanes.live faster than one a second. Its
callsigns, registrations and routes are all made up.

Development switches (simulator; the board's `getenv()` is NULL):
`VUELOS_PICK=<callsign>` picks that flight when it shows up,
`VUELOS_SCREEN=list|settings` opens that panel.

## How it is built

- `vl_render.c`, `vl_mvt.c`, `vl_draw.c`, `vl_store.c`: Mapas' renderer,
  copied with its own prefix (apps share no code) and a muted style
  (`ST[]` and the label colours in `vl_render.c`): greys, a darker sea, the
  runways and airport areas a shade lighter.
- `vuelos.c`: the worker that renders the map into buffers bigger than the
  screen (core 0), the tiles' downloads, the view, the touch, the gamepad.
- `vl_frame.c`: every frame cut out of the best map buffer, with the
  rings, the airports, the aircraft, the card and the buttons drawn on top,
  and up to the panel (flipped upright, turned by the PPA lying down).
  Frames go up while aircraft move, as often as half a pixel of the fastest
  one takes (12 a second at most, one a second when nothing moves).
- `vl_feed.c`: the requests, a small JSON scanner for the three shapes
  (readsb's `ac` / `aircraft`, OpenSky's rows), the tracks, trails, the
  movement between readings, the routes, the alerts, the page's table.
- `vl_conf.c`, `vl_air.c`, `vl_ui.c`: the settings file, the airports, the
  list and settings panels.

## Memory on the board

The map's four buffers (two whole and two at half size, bigger than the
screen), the frame, the airports and the tiles kept decoded come to about
12 MB of PSRAM, and the board opens the app with about 14 free: under 2 MB
are left, in pieces. Up to firmware 0.31.0 a request's whole ceiling is set
aside in one piece before it goes out, and the first build named 1.5 MB
for a reading and 1.1 MB for a tile: neither was ever there, no request
started, and the app said "Buscando aviones..." for ever (the board,
2026-11-22: it had only been tried in the simulator, whose memory does not
run out).

- A reading's ceiling goes by the radius (256 KB up to 60 NM, 512 KB up to
  130, 768 KB beyond; an aircraft is about a kilobyte), and a tile's is
  1.1 MB; when the firmware says there is no such piece the app asks with
  half, down to 48 KB for a reading and 256 KB for a tile. Since firmware
  0.32.0 the buffer grows with what arrives and the ceiling costs nothing.
- A request that does not start, or that is not answered in 30 s, is a
  line in the log and a line on the bar and in the list ("Sin memoria",
  "Sin respuesta", "Acceso rechazado" with the source's name), not the
  same text as waiting.

Tried in the simulator with `P4_SIM_HTTP_ROOM` and `P4_SIM_HTTP_WHOLE=1`
(`docs/APPS-P4.md`): 24 aircraft with pieces of 100 KB, "Sin memoria" with
pieces of 30 KB.

## What the services answered from the board (2026-11-22)

- **adsb.lol refuses HTTP/1.0:** 426, Upgrade Required, and 200 to the
  same request in HTTP/1.1. The firmware's client spoke 1.0 up to 0.31.0,
  so adsb.lol needs firmware 0.32.0; on 0.31.0 the bar says "Error 426".
- **airplanes.live answers 403** to the author's network whatever is
  asked, with a page that says to write to them; it may answer others. It
  stays the default, since it is the one that allows a reading every 5 s,
  and when it refuses before giving a single reading the app asks adsb.lol
  in its place (`vl_src_now()`), at adsb.lol's pace, until the source is
  chosen again. The bar and the attribution name the source in use.

`fake_adsb.py` does both: 426 to adsb.lol in HTTP/1.0 always, and 403 from
airplanes.live with `--refuse-airplanes`; `--chunked` and `--keep` answer
in chunks and never close, for the client's sake.

## Later: a receiver on the board (RTL-SDR)

Decoding ADS-B on the P4 itself with an RTL-SDR on the USB host is a phase
of its own. `apps/rf` already drives the dongle and takes its 2 Msps. What
has to be measured before writing it:

1. Whether one core keeps up with 2 Msps of 8-bit I/Q: the magnitude
   (a table of 256x256 or `|I|+|Q|` approximations), the preamble search
   and the 112-bit decode, in µs per million samples, with the other core
   free for the interface. dump1090's demodulator is the reference.
2. The USB bulk transfers' steadiness at 2 Msps (dropped buffers per
   minute) while the map is being drawn.
3. The PSRAM bandwidth taken (4 MB/s in, plus the magnitude buffer) against
   the panel's refresh (`docs/MEMORY.md`, "PSRAM bandwidth").
4. Messages per second and the range with a 1090 MHz antenna, against a
   receiver of one's own at the same spot.

If it keeps up, it is a fifth source in `vl_feed.c` fed by a thread instead
of HTTP, with CPR position decoding (global and local) and the aircraft's
state built from the message types.

Map data © OpenStreetMap contributors (ODbL), OpenMapTiles and OpenFreeMap;
aircraft from the source chosen; routes from adsbdb; airports from
OurAirports (public domain).
