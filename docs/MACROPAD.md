# The Macro pad

A Stream Deck for the bench. The app shows pages of big buttons. A button can:

- send a key combo to the computer over USB;
- type a text;
- play a short script;
- call a Home Assistant service;
- publish an MQTT message;
- open another P4OS app.

After the pages come three faces, in the top bar:

- **Trackpad:** a trackpad with a keyboard. One finger moves the pointer, a
  tap clicks, two fingers (or the strip) scroll, and a pinch zooms (cmd+= /
  cmd+-, a step every 30 % of spread). A two-finger gesture is a scroll or
  a zoom, whichever passes its threshold first, until the fingers lift.
  With the Mando and MIDI faces it covers everything the watch's Control
  PC did, but the IMU's air mouse (no IMU here).
- **Mando:** a gamepad. A stick that springs back, a D-pad, A/B/X/Y, L/R,
  L2/R2, Select/Home/Start. Two fingers at once (the stick and a button).
  Changes go out on the next 10 ms tick, and every 20 ms while a thumb is
  on the stick; nothing when idle; one neutral report on leaving.
- **MIDI:** a piano, one octave a row upright and two octaves lying down,
  octave -/+, three velocities (Suave 48, Medio 88, Fuerte 120), a bend
  strip (from where the finger lands, back to 0 on release) and a mod
  strip (CC 1). Two fingers, two notes; sliding plays a glissando. Leaving
  sends all notes off (CC 123). Two notes at most: the touch data carries
  two fingers.

- **App:** `aos.macropad` (`components/aos_apps/aos_app_macropad.c`).
- **Layout and runner:** `components/aos_apps/aos_macropad.c` (`aos_macropad.h`).
- **Portal page:** `#macropad` (`components/aos_portal/aos_portal_macropad.c`).
- **USB:** `components/aos_hal/aos_usb_p4.c`.

## Using it

- **Pages:** each page holds 15 slots, 3×5 upright and 5×3 lying down, in the
  same reading order.
- **Tap:** runs the button. It flashes green or red, and the bottom line says
  what went out, for example `Enviado: cmd+c`.
- **Long press on a button:** opens its editor sheet:
  - the action type and its settings;
  - one of ~120 glyphs;
  - a colour;
  - a *Probar* (try) button.
- **Tap on an empty slot:** creates a button there.
- **Long press on a page's chip:** rename, move, empty or delete the page.
  The `+` chip adds a page, up to 8.
- **Buttons with a state** show it:
  - an HA entity: a light on or off, or a sensor's value;
  - an MQTT button with a `state` topic, such as a Tasmota plug's
    `stat/<x>/POWER`.

  A toggle that is on is filled with its colour. One that is off is dark,
  with the colour only on the glyph.
- **Trackpad page:**
  - one finger moves the pointer;
  - a tap is a click and a long press is a right click;
  - two fingers, or the strip on the right, scroll;
  - *Clic*, *Arrastrar* (holds the left button for drag and drop) and
    *Clic derecho* are buttons;
  - Esc, Tab, the arrows, Backspace and Enter are keys;
  - the text field opens the on-screen keyboard and types live into the
    computer.

The portal page is the comfortable editor:

- drag buttons between slots;
- pick glyphs with a search box (the browser shows them with the Material
  Design Icons webfont from jsdelivr, or by name when offline);
- entity and app lists;
- the raw JSON;
- *Probar en la placa* (try it on the board);
- a reset to the default layout.

`?mp=<page>.<slot>#macropad` opens one button.

## The layout file

The layout lives in `<sd>/macropad.json`. It is written whole on every
change, and the app redraws when the portal saves.

```json
{ "version": 1,
  "pages": [
    { "name": "Mac", "buttons": [
      { "label": "Copiar", "glyph": "CONTENT_COPY", "color": "#0A84FF", "type": "key", "key": "cmd+c" },
      null,
      { "label": "Saludo", "glyph": "KEYBOARD", "color": "#30D158", "type": "text", "text": "Hola" } ] },
    { "name": "Casa", "auto": "ha", "buttons": [] } ] }
```

A slot is its index in `buttons`, and `null` is an empty slot. Glyphs are the
`AOS_SYM_*` names of `aos_sys_glyphs.h` without the prefix.

### The action types

| `type` | Fields |
|---|---|
| `key` | `key`: modifiers (`cmd`/`win`, `ctrl`, `alt`/`opt`, `shift`) joined by `+`, then one key. Examples: `cmd+shift+4`, `ctrl+alt+t`, `f5`, `ctrl+]`. Media keys: `play`, `next`, `prev`, `mute`, `volup`, `voldown`, `brightup`, `brightdown`, `eject`. The full list is in `components/aos_hal/aos_hid_keys.c`. |
| `text` | `text`: typed as a US keyboard would type it. Accents and ñ are skipped, and the status line counts them. |
| `seq` | `seq`: one command per line, Pato goma's language plus three verbs of its own (see below). |
| `ha` | `entity`. Optional: `service` (`light.turn_on`; without it, the usual tap: toggle, open/close, run) and `data` (JSON). |
| `mqtt` | `topic` and `payload`. Optional: `retain`, `qos` (0 or 1) and `state` (a topic to show as the button's state). |
| `app` | `app`: an app id, such as `aos.bench`. |

### The script language

The `seq` language has these commands:

- `STRING text`
- `KEY name`
- `DELAY ms`
- `MOUSE dx dy`
- `SCROLL n`
- `CLICK 1|2`
- `REPEAT n`
- `HA entity`, or `HA domain.service entity {json}`
- `MQTT topic payload`
- `OPEN app.id`

Lines that start with `#` are comments. A line the runner does not
understand is skipped.

The runner plays one step per 10 ms tick, and a key or a character is a step
of its own. A long text therefore keeps the screen alive, as on the watch.

### The default layout

When there is no file, the app starts with three pages:

- **Mac:** copy, paste, cut, undo, the two screenshots, Spotlight, app
  switcher, lock, and the media and volume keys.
- **Casa:** filled once from Home Assistant, the first time HA answers: up to
  15 scenes, lights, switches, fans, covers and scripts. After that the page
  is the user's.
- **Taller:** Banco, Terminal, Programador, Modbus, Bus and MQTT; three
  Tasmota plugs (`cmnd/<x>/POWER TOGGLE`, with their state on
  `stat/<x>/POWER`); an MQTT trigger for HA automations
  (`p4os/macropad/aviso`); an "idf build" script; and `ctrl+]` to leave
  `idf.py monitor`.

## USB on the P4

The board's "OTG" connector is the P4's own USB 2.0 High-Speed port. The
console lives on the CH343 UART, so the port is free. Its modes (off,
keyboard and mouse, disk) are in `docs/USB.md`.

Opening the Macro pad, or running any HID action, asks for `KEYS` mode. That
mode installs TinyUSB (`espressif/esp_tinyusb`, in
`components/aos_hal/idf_component.yml`) as one HID interface with three
reports:

- a keyboard (id 1);
- consumer control for the media keys (id 2);
- a mouse (id 3).

The descriptors cover both speeds, plus the device qualifier. The endpoint
polls every 1 ms.

The gamepad (an interface of its own), MIDI (board to computer) and the
network over the cable come with the keyboard; there is no host mode (no
5 V on the port). All of it, and disk mode, is in `docs/USB.md`.

Two HAL functions are new for the Macro pad:

- `aos_hal_usb_key_valid()`;
- `aos_hal_usb_mouse_hold()`, which holds a button for drag and drop.

**The simulator never touches the Mac's keyboard or mouse.** `hal_sim.c` and
`sim/usb_sim.c` only print what the board would send. The simulator parses
key names with the same parser as the board, so a name the board would
refuse fails in the simulator too.

## What the board confirmed

Plugged into a Mac on 2026-09-30, and followed from the portal's log:

- **Enumeration:** "P4OS <name>" (VID 0x303A), at High Speed, the first
  time. macOS asks to allow the accessory first.
- **The reports:** all three work. Media keys change the Mac's volume
  (63 → 69 → 63, read from the Mac), shortcuts reach macOS (⌘⇧3, ⌘Space,
  ⌘V), and the trackpad moves, clicks with a tap and scrolls with two fingers
  the right way.
- **Timing:** typed text arrives whole; no report failed.
- **The LEDs:** the Mac sends the keyboard's output report back.
- **Waking:** a button wakes a sleeping Mac. The Mac did not suspend the bus
  with the board on it, so the report itself woke it; the remote wakeup path
  is still untried.
- **Power:** the board runs from the OTG port alone, and moving from the
  console cable to it does not restart it.

Still in the simulator only: two-finger scrolling there is tested with the
strip, because the simulator's pinch is horizontal only.

### Following the first plug-in from the portal

The serial console stays on the CH343, but the tests go better without it:
opening that port resets the board. Everything USB logs goes to the log
ring instead. In the portal: **Registro**, filter `usb`.

| Line | Means |
|---|---|
| `switching the port to keyboard and mouse` | the Macro pad (or an action) asked for `KEYS` |
| `keyboard and mouse up on the OTG port (P4OS <name>)` | TinyUSB installed |
| `configured by the computer, at high (480 Mbit/s) speed` | enumerated; the speed is the one the computer took |
| `the computer's keyboard LEDs: num …, caps …` | the computer sends the LEDs back: it reads our keyboard report (press caps lock on the Mac's keyboard) |
| `suspended by the computer (remote wakeup allowed)` / `resumed` | the Mac went to sleep and came back; also what an unplug looks like, because VBUS is not monitored |
| `N report(s) not sent: …` | a key or click did not go, and why (not a keyboard, not configured, the computer took nothing for 40 ms); at most one line every 2 s |
| `tinyusb_driver_install: …` | TinyUSB did not start |

If the board restarts during a test, **Registro → Ver el arranque anterior**
has the lines before the restart, and **Firmware → Último cuelgue** has the
core dump if it was a panic (`docs/BUILDING.md`).
