# CAN

A CAN bus analyser for P4OS, on the P4's TWAI controllers and any two
GPIOs of the header. Id `aos.can`. Classic CAN 2.0 (11- and 29-bit ids, up
to 8 bytes), 25 kbit/s to 1 Mbit/s, through `aos_io_can_*`
(`components/aos_io/include/aos_io.h`, `docs/MODULES.md`, "PWM, an analog
level, infrared and CAN").

- **Connection:** TX and RX pins (the header's usable GPIOs, with the owner
  of the taken ones), speed (125k, 250k, 500k, 1M or any other in kbit/s,
  83.3 say), mode and the controller's filter (id and mask, 11 or 29 bits).
  The state of the controller (active, warning, passive, bus off), its
  error counters, frames a second and the bus load are always in view; bus
  off shows a "Recuperar" button.
- **Modes:** *Solo escucha* (never acknowledges nor sends: a car's bus),
  *Normal*, and *Autoprueba* (sends and hears itself, no transceiver; with
  TX and RX on the same pin nothing needs wiring). **The app opens in Solo
  escucha every time**, whatever was used last.
- **Tramas:** every frame with its time since the connection, id, length,
  data and ASCII (lying down). Drag it to pause and read back; "Seguir"
  catches up. The BOOT button pauses and resumes.
- **Por id:** one row per id, sorted: last data, frames a second, count;
  each byte lit amber for a third of a second after it changes and in amber
  letters for three seconds. Tap an id for its 64 bits (those that ever
  moved in orange, those that just did in yellow), to show only it or hide
  it, take it to Enviar, or chart a byte or two (Motorola or Intel).
- **Enviar:** a frame on a hex pad (id, 29 bits, remote, length, eight
  bytes), sent at once or saved; saved ones go out with a tap or every
  10 ms to 1 s with ▶ (eight at once). Five sends in a row that nobody
  acknowledges stop the periodic ones and the replay.
- **Grabar:** to `<card>/can/can-AAAAMMDD-HHMMSS.csv` (or `can-NNN.csv`
  before the clock is set) in SavvyCAN's GVRET CSV, so SavvyCAN opens it.
  The recordings sheet replays one with its timing, once or in a loop
  (Normal or Autoprueba), and deletes them.
- **Señales:** the signals of `<card>/can/senales.dbc`, a DBC subset (`BO_`
  and `SG_`: start bit, length up to 32 bits, Intel or Motorola, sign,
  scale, offset, unit; multiplexed signals are skipped), each with its
  value, and one charted over 30 s. The first run writes an example with
  the simulator car's signals.
- **Cableado:** the SN65HVD230 drawn between the header and the bus with
  the pins chosen, the 120 Ω ends, the OBD-II pins, and what not to do on a
  car.

## Portal page

`web/can.js`, the page `#can`: the state, the table by id with the bytes
that change, the last frames, send (by hand or a saved one), start and stop
a recording, download and delete recordings, and edit `enviar.txt` and
`senales.dbc`. It talks to the app through two files of `/data`, so the app
has to be open on the board (in front or not):

- `/data/can_cmd.txt`, written by the page: `watch`, `send 123#1122`,
  `rec on`, `rec off`, `clear`, `reload`. The app's file thread reads it
  every second and deletes it; the page writes the next batch once it is
  gone.
- `/data/can_live.json`, written by the app once a second for 15 s after
  each `watch` (the page repeats it every 5 s), through a `.part` and a
  rename.

## Files

| File | What |
|---|---|
| `main/can.c` | the app, the bar, the state, the tabs |
| `main/can_bus.c` | the bus thread (the only one on the handle) and the file thread (recording, replay, the portal) |
| `main/can_frame.c` | candump text forms, the DBC subset, the saved frames |
| `main/can_spy.c` | "Tramas" and "Por id", the id's sheet |
| `main/can_send.c` | "Enviar" |
| `main/can_sheets.c` | connection, wiring, recordings |
| `main/can_sig.c` | "Señales" and the chart |

The ring of the last 4096 frames, the table of 192 ids and the pause's copy
are in PSRAM; the two threads' stacks too (`aos_hal_thread_start`). The
app's `CMakeLists.txt` adds `components/aos_io/include`, which
`apps/common.cmake` does not give.

## Trying it

In the simulator every CAN node shares a bus with a car (0x0C0 engine
speed, 0x1A0 vehicle speed, 0x3E8 temperatures, 0x18FEF100 extended); a
node in the self test hears only itself. On the board, with no transceiver,
Autoprueba on one pin; a real bus waits for SN65HVD230 transceivers.

What only the board can confirm: the error counters and the bus-off
recovery on a real bus, the load estimate against a scope, and that the
bus thread keeps up with a busy 500 kbit/s bus (the driver's queue is 64
frames).
