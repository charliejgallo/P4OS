# Mi casa

A board of the services at home: whether they are up, how long they take
and since when they are down. Id `aos.estado`; in English "Home Status".

## What it checks

Each service is tried on its own every 10 s to 1 h (`every`), by one of
two ways:

- **An HTTP(S) request:** one GET that must answer with one of the
  expected codes (`200`, `200-399`, `200,401`...) and, if a word is given,
  contain it (any case; the first 64 KB of the body). The latency is from
  the request leaving to the first byte of the answer; the time to connect
  (DNS, TCP and the TLS handshake, which on the board is mostly the board's
  own 1.6-1.8 s of work) is shown apart.
- **An open TCP port:** the connect alone, and its time is the latency.

Ping is not there: the firmware lends apps no ICMP. The Red app's
`nt_ping` is in the symbol table, but it is Red's own single ping session,
stopped three seconds after the screen stops asking for it; using it would
take Red's away. A TCP port (SSH 22, SMB 445, MQTT 1883, a web server)
says the same thing for almost everything at home; a one-shot
`aos_hal_ping()` is a request to the firmware (`HANDOFF-NUEVAS-APPS.md`).

A service is **down** after `fails` failures in a row (two by default);
after the first one it is tried again within 15 s. It is **slow** (yellow)
when the latency passes `slow`. While the board has no network nothing is
tried and nothing counts.

Templates: Home Assistant (`/manifest.json` with "Home Assistant" in it),
Frigate (`/api/config` with "cameras"), a NAS (TCP 445), a 3D printer
(`/`, 200-399), the router (`/`, 200-499: a login page counts) and any web
page (https, 200-399, a certificate warning at 14 days), and any TCP port.
They leave the address empty: an address nobody changed would be tried for
real.

### Certificates

The HAL's TLS verifies a certificate but does not hand it to apps, and
mbedtls is not in the symbol table. So `ec_cert.c` reads it the way a
scanner does: a TLS 1.2 ClientHello of its own over a bare socket and the
server's Certificate message, which TLS 1.2 sends in the clear; then the
connection is dropped. It gives notAfter (the days left), the CN, the
first DNS name of the SAN and whether it is self-signed. Every six hours,
and at once after a refusal. A server that speaks TLS 1.3 only refuses the
hello and its days stay unknown (a request to the firmware: the peer's
certificate after `aos_tls_open`).

A home device with a certificate of its own (a NAS, a router, a self-signed
Home Assistant) is refused by the HAL's check. "Confiar en este
certificado" (in the detail, or on the portal) saves the certificate it
showed as `estado_<key>.pem` and from then on that service connects with
`aos_tls_open_trust()` against that certificate alone, checked against its
DNS name (or the address, when its SAN only has IPs). If the server changes
it, the service shows down until it is trusted again.

## The screens

- **The board:** the services in their groups, in the file's order, as
  tiles: traffic light, name, latency (or "Caído" and why), and how long
  ago that changed. The tile carries a lock and the days when the
  certificate is running out. Groups sit side by side when they fit: two
  columns upright, four lying down. Over them, how many are up, slow and
  down; the buttons for desk mode, the options and a new service.
- **A service:** the latency now, the availability of the last 24 hours
  and 7 days, and the certificate's days (or, for TCP, the time to
  connect); the latency of the last 6 or 24 hours from five-minute buckets,
  with a strip under it (green all good, yellow some failures, red nothing
  answered, grey no data); the last check in words; the latest outages;
  Probar ahora, Pausar, Confiar. Lying down the chart and the outages go
  left and the rest right.
- **Desk mode:** the whole screen, the time and a summary, and a tile per
  service as big as they fit, tinted by state. The screen stays on. A tap
  closes it.
- **The options:** a beep with each notice, start with the board, failures
  before it is down, check them all now.
- **The editor:** a template first, then name, group, how it is checked,
  address, codes, word, how often, slow from, certificate warning. The
  portal is more comfortable, and the editor says so.

## Notices

The app has `AOS_APP_FLAG_BACKGROUND` and, while it has services and the
option is on, asks to start with the board (`aos_ui_boot_start_set`). When
a service goes down or comes back, and when a certificate runs short (at
the warning, then at 7, 3 and 1 days and the end), a card comes up over
whatever app is in front (the display's top layer) with two notes; it goes
by itself, and a tap opens that service. While anything is down a red
`AOS_SYM_LAN_DISCONNECT` stays in the status bar and the icon carries how
many. The app reads its own catalog (`ec_i18n.c`, as Datalog does): it
speaks while hidden.

## Files

In the data folder (`/data` on the portal):

- `estado.txt`: the options and the services, one block each (format in
  `main/ec_cfg.c`). The app and the page both write it; the app looks at
  its date every two seconds.
- `estado_hist.bin`: per service, by the key of its name, 24 h in
  five-minute buckets, 7 days in hours, the last 12 outages; 3.8 KB a
  service, saved every five minutes when something changed and on closing.
  A renamed service starts a new history.
- `estado_eventos.txt`: a line per change for people and the page,
  `date<TAB>name<TAB>down|up|cert<TAB>detail<TAB>ms`. Past 256 KB it moves
  to `estado_eventos.old.txt`.
- `estado_<key>.pem`: a trusted certificate.

## The portal's page

`web/estado.js`, three tabs: **Ahora** (the live state through
`aos_hal_live_*`: every service with a sparkline of six hours, the day and
the week, the certificate, Probar / Pausar / Confiar, and "Ver más" with
its whole day, the week by day and its outages), **Servicios** (the list
by fields, in order, from templates; saved whole to `estado.txt`) and
**Historial** (`estado_eventos.txt`, newest first, by service).

Messages to the app: `reload`, `check <i>|all`, `pause <i> 0|1`,
`trust <i>`, `detail <i>` (which service goes in the `detail` key).

## Trying it

Only against local fake servers, never the real devices at home:

```
apps/estado/tools/fake_estado.py --write-config sim/sim_fs/data/estado.txt --history sim/sim_fs/data
```

serves seven made-up services on 127.0.0.1 (HTTP, HTTPS with a
self-signed certificate that ends in `--cert-days` days, TCP) and drives
them from a control port: `curl 127.0.0.1:18999/nas/down`, `.../up`,
`.../hang`, `.../slow/1500`, `.../code/503`, `.../noword`, `.../tls13`,
`.../flap/20`. `--history` writes a made-up week for the screenshots
(`--english` for English names); `--with-tools` adds the repo's
`tools/fake_ha.py` (8123, TCP) and `tools/fake_frigate.py` (5000) to the
list. The simulator: `EC_OPEN=<n>` opens a service, `EC_DESK=1` desk mode,
`EC_EDIT=<n>|new` the editor, `EC_CONFIG=<path>` another estado.txt.

## Limits

- Up to 32 services, two checks at a time. A service that does not answer
  holds one checker for its whole timeout (1 to 8 s).
- `destroy()` waits for both checkers to come back (up to 20 s: a TLS
  connect and handshake can each take the whole timeout) before the loader
  unloads the `.so`.
- Like any app with BACKGROUND, it is closed if four more apps are opened
  (`enforce_alive_max`): the checks stop until it is opened again.
