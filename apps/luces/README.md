# Luces

A lighting console for DMX512 fixtures, on an RS485 port of the header.
Id `aos.luces`. The DMX app (`apps/dmx`) is the bench tool, to test a
console or a fixture channel by channel; Luces is for running a show:
fixtures by name instead of channels, colours, scenes, a chase on the beat.
The sending (BREAK, timing, 40 packets a second) is the firmware's
(`aos_io_dmx_*`); the app has no thread of its own.

## The model

- **Type:** what a fixture is, its channels in order with a meaning each
  (dimmer, red, green, blue, white, amber, UV, strobe, pan, pan fine,
  tilt, tilt fine, or a channel held at a fixed value). Nine come built
  in (a dimmer, RGB, RGBW, a 7-channel PAR, an RGBWA+UV PAR, two moving
  heads, a strobe, a fog machine); more are CSV files on the card.
- **Fixture:** a type at an address, with a name ("PAR 1").
- **Look:** what a fixture shows: intensity, colour, strobe, pan and tilt.
  The programmer changes the looks of the selected fixtures; a scene keeps
  every fixture's look.
- **Output:** every 40 ms the looks (after a fade, a chase step or an
  effect) become the 512 channels: each channel from its meaning, the
  intensity through the master, the colours dimmed by the intensity when
  the type has no dimmer channel (RGB pixels), Blackout over everything.

## The tabs

- **Escenario:** the fixtures as tiles in the colour they show; a tap
  selects one, a long press selects only that one, and Todos / Ninguno /
  Pares / Impares. With none selected, what follows acts on all of them.
  Intensity (a slider and 0-100 %), eleven colours (a colour on a dark
  fixture turns it on), strobe, a pan/tilt pad (when some fixture moves),
  effects (rainbow, wave, sparkle) with their speed, and the master.
  Three **test modes** next to the effects, for a rig that does not do
  what it should:
  - **Recorrido** lights one fixture at a time in white: which is which,
    and a fixture lit out of turn (or two at once) has a wrong address.
  - **Colores** takes them all through red, green, blue and white: a
    fixture in another colour has its channels in another order than its
    type says.
  - **Respirar** raises and lowers the intensity slowly in the look's
    colour: steps or flicker at the bottom are the fixture's dimming curve.
- **Escenas:** "Guardar lo que hay" stores every fixture's look as a
  scene; a tap calls it back with the chosen fade (cut to 5 s), a long
  press records it again, renames it or deletes it. The chase goes through
  every scene in order at a tempo (BPM by hand or by tapping Tap to the
  music) with a cut, half or all of each step as a fade. It needs two
  different scenes at least (with one it calls the same over and over, and
  nothing changes; the tab says so). **Escenas de muestra** adds five for
  whatever is patched (red, green, blue, a rainbow across the fixtures,
  odd and even in two colours); a first show starts with them.
- **Patch:** the fixtures with their addresses (orange when they overlap
  or go past 512); a tap edits the name, the address and the type, or
  removes it. "Agregar aparatos" adds 1 to 16 of a type at the next free
  addresses. Below, the types with their channels.
- **Salida:** the port, what goes out, and the 512 channels as a map (dark:
  no fixture); a tap on a channel says its value and whose it is.

**The output opens closed** every time the app opens (▶ in the bar): sending
on somebody's rig is the user's call. Once open it keeps going with the app
in the background.

## On the card

`<card>/luces/`:

- `patch.csv`: `nombre,tipo,direccion` a line. The first time, eight RGB
  pixels at 1-24 (the bench's strip) and a 7-channel PAR at 25.
- `escenas.txt`: a scene a line (the five sample scenes when it is missing), `name|fade ms|i:dim,r,g,b,w,amber,uv,strobe,pan,tilt;...`
  with `i` the fixture's place in the patch.
- `aparatos/*.csv`: the types, two lines each:

  ```
  nombre,PAR de 8 canales
  canales,dimmer,rojo,verde,azul,blanco,strobe,macro=0,velocidad=0
  ```

  One word a channel as the fixture's manual lists them: `dimmer`, `rojo`,
  `verde`, `azul`, `blanco`, `ambar`, `uv`, `strobe`, `pan`, `pan_fino`,
  `tilt`, `tilt_fino`; a number (the channel held at that value: a mode, a
  macro at 0) or `name=value` (the same, named). The app writes an example
  when the folder is empty; "Volver a leer los tipos de la tarjeta" reads
  them again, and the patch keeps its fixtures by the type's name.

## From the portal

The **Luces** page is a second console, made for a phone on the same
network: the output, Blackout and the master on top; the four tabs as on
the board (tiles to select, colours and a colour picker, the pan/tilt pad
by finger, the scenes, the chase with a Tap that counts in the browser, the
patch as a table, new types written to the card, the map of the 512
channels). It goes through the live channel: the app puts a state four
times a second while the page looks and takes orders as lines of
`key=value` (`main/lz_remote.c` lists them). The app has to be open on the
board.

## Tried

In the simulator, 2026-11-09: the programmer (selection, intensity,
colours, the pad), two scenes stored and called back with a fade, the chase
at 120 BPM, the output at 40 packets a second, the four tabs in portrait and
landscape; from the portal page in a phone-sized window, the same plus
adding fixtures, changing an address and writing a new type to the card.

On the bench, the DMX app's console already lights the ESP32's strip
(`tools/rs485_test/esp32`, DMX light role, channels 1-24) through the two
automatic RS485 modules (`docs/img/bench-rs485.svg`). Luces's first show is
that strip, so it is the next thing to try on the board.

## Files

- `main/lz.h` the model; `main/lz_core.c` the library, the show on the
  card, the programmer, the fades, the chase, the effects and the universe
- `main/lz_ui.h`, `main/luces.c` the frame (bar, tabs, keypad, keyboard,
  the output); `main/lz_stage.c` Escenario; `main/lz_tabs.c` Escenas,
  Patch and Salida
- `main/lz_remote.c` the portal's orders and state
- `web/luces.js` the portal page
