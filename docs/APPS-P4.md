# Apps on the card (.so) on the ESP32-P4

The apps in `apps/` are shared objects that the firmware loads from
`/sdcard/apps` (`components/aos_dynapp`, `components/elf_loader`). This file is
what is specific to the P4. The app API itself is `aos_app.h`; the porting
recipes are in [plan/APPS.md](plan/APPS.md).

## Building and installing

```
tools/build_fw.sh rev1_3                                   # the firmware first
python3 tools/gen_symbols.py --build build/rev1_3          # when the HAL or LVGL changed
tools/build_fw.sh rev1_3                                   # again, with the new table
tools/build_apps.sh                  # every app, or: tools/build_apps.sh gemas
```

`build_apps.sh` builds each app for the ESP32-P4 whatever the app's own
`sdkconfig.defaults` say, with the LVGL configuration taken from the firmware
of `build/$PROFILE` (default `rev1_3`), and refuses a `.so` that:

- does not export `aos_app_abi` / `aos_app_init`, or says another ABI;
- is not single-float ABI (the firmware is `ilp32f`);
- needs a symbol that is not in `components/aos_dynapp/aos_symbols.c`;
- carries a local path.

To install, copy `apps/*/build/*.so` into `/apps` on the card (the portal's
`PUT /api/fs/put?path=/apps/<name>.so`, or the card in the computer) and
restart: the card is scanned at boot.

## How the code runs

- **Where:** in PSRAM, where the loader puts it
  (`CONFIG_ELF_LOADER_LOAD_PSRAM`). On the P4 data and instructions see PSRAM
  at the same addresses, and there is no executable heap at all.
- **Memory protection:** `CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION` is off.
  With it on, the PMP marks the PSRAM heap read/write only (on a v1.3 chip,
  entry 10 of `cpu_region_protect.c`), and an app's first instruction faults.
  The price is that the firmware's own code in PSRAM is not write-protected
  either. The way back is an executable MMU alias of the apps' pages above
  the heap, where no PMP entry reaches (the heap ends near 32 MB; the window
  is 64 MB).
- **Caches:** after loading, the L1 data cache is written back and both
  instruction caches are invalidated (`esp_elf_arch_flush`, P4 branch).
- **Float:** the P4 has a single-precision FPU. `double` arithmetic is
  libgcc's soft-float (`__adddf3` and friends, exported by the table); the S3's
  `__addsf3` family does not exist here.
- **Float is slow for signal processing:** a multiply-add costs about 5
  cycles whatever the code - esp-dsp's own benchmarks show its hand-written
  float dot product no faster than C. For filters and FFTs over many samples
  use 16-bit fixed point on the core's SIMD (below).

## DSP: esp-dsp on the SIMD

Since 2026-10-07 the firmware carries esp-dsp (1.8.2) and lends a chosen part
of it to the apps (`EXTRA_SYMBOLS` in `tools/gen_symbols.py`): dot products,
decimating FIRs, FFTs, biquads, in their `_arp4` (SIMD) and `_ansi` (C)
forms. The apps' build has no esp-dsp headers: declare what you call (the RF
app copies `fir_s16_t` as esp-dsp 1.8.2 has it, `apps/rf/main/rf_demod.c`).

Measured with the RF app (`apps/rf/README.md`):

| | In C | 16-bit on the SIMD |
|---|---|---|
| FFT of 2048 points (complex) | float: 1.4 ms | 0.46 ms (`dsps_fft2r_sc16_arp4_` + `dsps_bit_rev_sc16_ansi`) |
| A 56-tap half-band, 240 k outputs/s, I and Q, per 10 s | float, 4 sums at a time: 2.2 s; 16-bit with 32-bit sums: 3.6 s | 1.5 s (`dsps_fird_s16_arp4`) |

What it wants, and what bit:

- **Arrays 16-byte aligned, lengths a multiple of 8.** The 128-bit loads
  ignore the low address bits: a misaligned array gives wrong sums, silently.
- **`dsps_fird_s16_arp4` with an odd decimation (or 1) parts from C** as soon
  as its delay line wraps (seen with 5: the first 11 outputs equal, then not).
  Use it for even decimations and C for the rest.
- **It runs its taps forwards over the delay line; `_ansi` backwards.** Use
  symmetric taps over the whole padded length, or the two give the same
  filter some samples apart.
- **Its return value is not the output count** (1.8.2 returns a register it
  never loads): the count is `len`.
- **One call per output is too dear for short filters:** `dsps_dotprod_s16`
  costs ~85 cycles for 32 taps, mostly the call. Call the block functions.
- **The sc16 FFT halves at each stage** (the result is the DFT over N), and
  its twiddle table is global and set once per boot by
  `dsps_fft2r_init_sc16`: the RF app makes its own table with the same recipe
  (`rf_dsp.c`) so it does not depend on who initialised it first.
- Compare the SIMD routine with the C one once at start and fall back if
  they differ (`rf_demod_simd()`); the simulator only has the C ones.

## Measured

- **Boot (2026-09-30):** 34 modules and 40 apps registered in 1.5 s, and the
  system is up at 4.2 s. Registering used to rebuild the home screen once per
  app: 8.3 s with 40 apps. The scan now holds the home screen and builds it
  once (`aos_ui_hold_home`).
- **Boot with the scan cache (2026-10-12):** 44 modules, 51 apps; the scan
  takes 2643 ms when every `.so` is opened (the first boot, or after a new
  firmware) and 503 ms when they come from `/data/apps.cache` (only
  `lua.so`, which declares its apps per script, is opened). An app is
  opened to be described again when its file changes in name, size or
  date.
- **`hello_app.so`:** 3 KB, 18 symbols. 16 apps opened one after the other with
  no reset; internal RAM steady at ~126 KB free.
- Every app in `apps/` builds and passes the checks.

### The apps on the board

Fps measured with the app's own log line in `/api/log`.

| App | Upright | Lying down | How it draws |
|---|---|---|---|
| Doom | – | 35 fps | 320×200 ×3 by the PPA into the free buffer, flipped |
| Pinball 3D | 67–70 fps (the timer's 14 ms; the panel shows 60), 3–6 ms a frame; Casino Real 63–70 (it dipped to 38–45 in its video mode before the DMD redrew only what changes) | – | only the rectangles that changed, from the table's picture and depth; page flipping, the rest copied by the DMA (apps/pinball/README.md) |
| Duke Nukem 3D | – | 30 fps (the game's rate) at 320×200 and 480×300 (uncapped 59 and ~34); 640×400 15–21 fps, 640×480 15–18 (all of core 0; clean since the panel fixes of 8b3dde5/fcde1e5) | Build engine to an 8-bit frame, palette to RGB565, PPA into the free buffer, flipped (apps/duke3d/README.md) |
| Video | 640×360: 30 fps; 720×1280: 24 fps | 1280×720: 20 fps | JPEG engine + `blit_fit` |
| Visor 3D | ~28 fps with ~16 000 triangles; 13–15 fps with ~100 000 | same | rasteriser on both cores, adaptive size |
| Monster Hop | 22 fps | ~20 fps (15.5 before the internal RAM audit) | bands on both cores, panel buffer + flip; lying down the PPA turns each band |
| Turbo | 18–19 fps | ~15.7 fps | bands on both cores, panel buffer + flip |
| Golf | menu 30 fps | 3D view 1.9 s a picture | halves on both cores |
| Mila | idle: 1 % of the screen redrawn | level frames 8–12 ms, map 46 fps; flips lying down since the audit | dirty 32×32 tiles |
| Claude Jump / 2043 | 30 / ~24 fps | | retro canvas; 2043 presents its changed tiles only |
| Claudito | idle ~3 % of a core | | retro canvas, changed rows only |
| Lua (cubes) | 34 fps with 24 cubes | | full-screen present |
| Neon Snakes | 23 fps in combat | | |

To deploy: `tools/install_apps.sh p4os.local [app…]`. It uploads
`apps/<name>/build/<name>.so` and the `<name>_p4.pak` with its parts, if the
app has one, to `/apps`, deletes the stale parts, and restarts the board
through the portal. The card in the computer works too. The paks are
gitignored; each app's README says how to build its pak.

## Drawing text yourself

`lv_draw_label()` keeps the **pointer** to the text and draws later, on
LVGL's draw threads (two of them on the board, in parallel). A text built
on the stack (a `char buf[]`, a `snprintf` in a draw event) is gone by
then: set `dsc.text_local = 1` so LVGL copies it. Literals and `_()`
strings live on, and a canvas layer finished before returning is drawn
at once. Infrarrojo hung the board this way (2026-10-05): a draw thread
went round and round past the end of a dead buffer, and the UI waits for
it to finish the frame.

## The descriptor's flags, while the app runs

`self->desc.flags` may change while the app runs: the shell reads them at
every touch. The VNC viewer sets `AOS_APP_FLAG_NO_SWIPE |
AOS_APP_FLAG_LONG_DRAG` only while it shows the remote screen, so a drag
from the left edge is the remote's there and "back" everywhere else.

## When the app is left: kept or closed

`AOS_APP_FLAG_KEEP` asks to be kept hidden when the user leaves; whether it
is depends on Settings, Developer, "Apps in the background" (`aos_ui.c`
explains it). In the default, Automatic, an app is closed on leaving if it
holds 1 MB of PSRAM or more and has nothing running, and below 8 MB of free
PSRAM the hidden ones go too. "Running" is seen by the system: header pins
claimed while the app was in front, the speaker or the microphone open, a
recording, the worker going, or `AOS_APP_FLAG_BACKGROUND`. What it cannot
see, the app says: `aos_ui_set_busy(self->desc.id, true)` while a remote
session or a download must go on (VNC does it while connected), `false`
when it ends. So:

- Save the user's work in `hide()`: `destroy()` may follow at once (Dibujo
  saves the open drawing there).
- Do not count on being kept to stay fast: an app that holds megabytes is
  reopened from scratch.
- The log says what was decided and what each app held (`ui: keeping ... /
  closing ... on leaving: it holds N KB of PSRAM`), and whether an app that
  was closed gave its PSRAM back.
- At most two apps stay hidden beside the one in front; one more closes the
  least recently used that has nothing running.
- A card app whose code does not fit in PSRAM when it is opened closes the
  hidden apps with nothing running, lets the wallpaper photos go (they
  split the PSRAM) and tries once more (`ui: closing X: another app needs
  its memory`, `ui: the wallpaper photos let go`).

Measured on the board (2026-10-08): Photos 2.1 MB, Pixel Art 2.0 MB and
Drawing 1.7 MB are closed; Radio 490 KB, Notes 343 KB, Music 156 KB, CAN
154 KB and the small tools stay. The games have no KEEP, and Gems, Mila,
Monster Hop and Turbo give all their PSRAM back when closed.

## Starting with the board

An app with `AOS_APP_FLAG_BACKGROUND` can ask to start with the board,
hidden, as if the user had opened it and left it (2026-10-18): a logger
that must go on after a power cut or a firmware update.

- `aos_ui_boot_start_set(self->desc.id, true)` puts it on the list, and
  `false` takes it off; `aos_ui_boot_start_get()` says whether it is on.
  The app decides: a logger asks while it logs and stops asking when it is
  paused, so a board that boots does not log what nobody wants.
- At boot, once the card's apps are in, each one on the list gets
  `create()` with its root hidden and then `hide()`, never `show()`, until
  the user opens it; from then on it is any app that was left. Its `tick()`
  runs about five times a second meanwhile, on the shell's thread: work
  that waits (a sensor, a file) goes in a thread of its own
  (`aos_hal_thread_start`).
- Not in safe mode, and an app without `AOS_APP_FLAG_BACKGROUND` is refused
  (with `KEEP` alone it would get no ticks). An id that is not on the card
  is skipped and said in the log.
- The user sees the list in Settings, Developer, "Arrancan con la placa",
  and a tap takes an app off. The portal has it too: `GET /api/apps/boot`,
  and `POST /api/apps/boot?id=<id>&on=0|1`.
- **A glyph in the status bar** while it works hidden:
  `aos_ui_status_icon_set(self->desc.id, AOS_SYM_RECORD, 0xFF453A)` puts
  one of `aos_sys_glyphs.h` left of the system's (Datalog's red dot while
  it logs), and `NULL` takes it off; closing the app does too. Up to four
  apps at once. Callable from any thread: the bar draws it within 200 ms.
- **The limit of apps alive** (two besides the one in front, since
  2026-11-08; it was three) never closes
  an app with something running: `AOS_APP_FLAG_BACKGROUND`, busy, pins,
  sound. Only the idle ones go, the least recently used first.
- The clock may not be set yet at that moment: there is no battery-backed
  RTC chip on the board, and the time comes from the network. Check
  `aos_hal_time_is_valid()` before stamping a reading.

## Drawing a frame of your own

- **Worker** (`aos_hal_worker_start`): runs on core 0, priority 3, with a
  PSRAM stack. Never touches LVGL. **There is one in the whole system**, and
  `aos_hal_worker_stop()` stops whoever's it is: an app that keeps running
  in the background (`AOS_APP_FLAG_BACKGROUND`) and holds it would stop
  Video, Doom or Mapas from starting theirs. Such an app uses a thread of
  its own (`aos_hal_thread_start`), as Infrarrojo does.
- **Two cores for one frame** (`aos_hal_worker_split`): half runs on the
  caller's core and half on a helper pinned to the other core, at the
  caller's priority.
  - Measured: the Visor 3D triangle pass got 1.75× faster; its vertex pass,
    bound by PSRAM, not at all.
  - Each half needs its own scratch.
- **Blits:** `aos_hal_display_blit_scaled` and `blit_fit` write into the
  buffer on screen.
  - A 1:1 upright blit is a DMA2D copy: ~20 ms for 720×1280.
  - Anything scaled, turned or byte-swapped goes through the PPA.
- **Page flipping:** `aos_hal_display_back()` gives the panel's free frame
  buffer, in the panel's own orientation. Draw a whole frame into it, then
  `aos_hal_display_flip()`.
  - No copy, no tearing, and no wait for the 30 Hz refresh: there are three
    buffers.
  - `aos_hal_display_blit_into()` turns a rectangle of the screen into one of
    those buffers with the PPA.
  - Monster Hop upright went from 20 to 22 fps with it.
- **Lying down, the ceiling is the PPA.** Turning a 1280×720 frame takes 42 ms
  alone and 62 ms while both cores draw beside it (PSRAM).
  - Turning it on the CPU was worse: ~42 ms a core. Turbo's turned strips
    ran at 8 fps against the PPA path's 15.
  - The PPA turning bands of 8 rows from internal RAM does a frame in 32 ms,
    but an app needs two free blocks of 20 KB of internal RAM for it.
  - Bands of 4 rows hung the PPA.
- **Filling the free buffer:**
  - `aos_hal_display_blit_into()` copies a rectangle of your own frame into
    it, 1:1. Whole rows upright go by the AXI DMA, 19 ms for 720×1280;
    anything else goes through the PPA, turned as the screen is.
  - `aos_hal_display_blit_into_fit()` does the same scaled: Doom's 320×200
    ×3, Video's frames.
  - `aos_hal_display_back_age()` tells an app that redraws only what
    changed how old the free buffer's picture is: the union of the damage
    of its last `flips_since + 1` frames goes in. Redraw everything when
    it returns `UINT32_MAX` or when LVGL drew into the buffer.
- **Something over the app:** `aos_ui_overlay()` (`aos_ui.h`) returns bits
  for a panel, the switcher, the gesture home, the icon zoom, a banner, a
  toast.
  - Read it in the LVGL timer, keep a copy for the worker, and under
    anything but a toast neither blit nor flip. Pause the game.
  - When it clears, present a whole frame.
  - A flip hides whatever LVGL drew in the buffer on screen, so an app with
    LVGL widgets over its drawing blits the part they don't cover instead
    of flipping (Visor 3D with its bars up, Video with its controls, Golf
    always).
  - Monster Hop, Turbo, Doom, Video, Visor 3D, Mapas and Golf follow this
    (2026-09-30).
- **The screen stays on** while an app with `AOS_APP_FLAG_KEEP_AWAKE` is in
  front: there is an auto-off in Settings now, off by default.
- **Internal buffers:** allocate with
  `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA`. Without the DMA
  flag the heap may return the LP SRAM, which is 8–15× slower (Monster Hop's
  second band, `docs/MEMORY.md`).
  - Lying down there is ~57 KB less internal RAM: LVGL's rotation buffer
    exists only then. Since the internal RAM audit (2026-09-30) there is
    room anyway: Monster Hop gets two bands of 9 rows lying down (they used
    to shrink to 4, too few for the PPA to turn), Mila hers. In PSRAM the
    same bands draw twice as slowly (pref `bands_psram`, `docs/MEMORY.md`).
- **Draw only what changes where you can.**
  - Mila redraws dirty tiles.
  - Monster Hop draws the action button once as colour + alpha instead of
    ~56 discs a frame, which saved 10 ms.
  - Clip in X as well as Y: narrow bands multiply every per-row cost.

## The system's look and sounds (since 0.21.5)

- **The accent is the person's.** `AOS_C_ACCENT` is a call now,
  `aos_theme_accent()`, and returns the colour chosen in Settings,
  Wallpaper. Use it for what is active or selected and the app follows the
  choice; an app built before keeps the blue it was compiled with.
  `aos_theme_accent_rgb()` gives it as 0xRRGGBB for code that mixes
  colours itself.
- **Sounds:** `aos_ui_sound(AOS_SND_NOTIF)` and the rest of `aos_sound_t`
  play the system's own (a few notes of the speaker's bell), and respect the
  person's switch: nothing sounds when the system sounds are off. A game's
  own effects are the game's (`aos_hal_beep`, `aos_hal_spk_*`).
- **The screen recording** (`aos_hal_screenrec_*`, the control centre's
  Grabar) takes the panel's framebuffer as it is, so an app that flips or
  blits its own frames is recorded like any other.
- **Home screen widgets** (since 0.22.0): a widget type registers with
  `aos_ui_register_widget()` and, to be offered on the board, describes
  itself with `aos_ui_describe_widget()`: a name, a line about it, a glyph,
  and optionally `choices()`, the values the person can pick (they reach
  the widget as its arg, joined by commas, up to 255 bytes; none picked is
  an empty arg and the widget chooses). The sensors and Home Assistant
  widgets are the examples (`aos_widget_sensors.c`, `aos_app_ha.c`).
- **TLS to a device that signs its own certificates** (after 0.22.0):
  `aos_tls_open_trust()` (`components/aos_hal/aos_tls.h`) verifies against
  the PEM and the name the app gives, not the bundle and the address; an
  app declares the `aos_tls_*` calls itself instead of including the header
  (it pulls in mbedtls's), as `apps/impresoras/main/p3d_net.c` does.
- **The home screen's photo stays in PSRAM** (1.8 MB) while an app is in
  front. An app that needs every megabyte has it let go with
  `aos_ui_wall_photo_spare(true)` and back with `false`; Cameras does it
  while it shows a 1920×1080 stream.
- **Copies through PSRAM cost the same whoever makes them.** Handing a plain
  copy to the PPA or a DMA did not pay on this board (the wallpaper and the
  camera frames were measured slower than the CPU, docs/CAMERAS.md). What
  pays is copying less: converting straight into the panel's free buffer
  (`aos_hal_display_back` / `flip`) instead of into a frame of the app's
  that LVGL then copies again. Cameras' viewer does that.

## A page of your own in the portal

An app can bring a page to the board's web portal with no firmware: a
JavaScript module in `apps/<app>/web/`, uploaded with the app by
`tools/install_apps.sh` and loaded by the portal from the card. See
[PORTAL-PAGES.md](PORTAL-PAGES.md); `apps/hello_app/web/hello.js` is the
template.

## Bluetooth, the keyboard and text from outside

Since 0.7 ([BLUETOOTH.md](BLUETOOTH.md), [EMOJI.md](EMOJI.md)):

- **Keys go over the cable or over Bluetooth by themselves.**
  `aos_hal_usb_key`, `_type`, `_mouse`, `_click` and `_mouse_hold` reach a
  computer that took the board's Bluetooth keyboard mode when none has the
  USB port in KEYS mode. `aos_hal_usb_keys_ready()` is true either way, so
  it no longer means "there is a cable". `aos_hal_bt_keyboard_ready()` and
  `aos_hal_bt_keyboard_host()` say when it is Bluetooth, and to which
  computer. The gamepad and MIDI are the cable's only.
- **The phone:** `aos_hal_bt_state`, `_peer` and `_phone_battery`, its
  notifications through `aos_hal_notif_count` and `_at`, its music through
  `aos_hal_media_info` and `_command` (AMS, off by default). The simulator
  has a fake iPhone.
- **Text from outside** (a notification, a JSON from the network, a file)
  goes through `aos_text_safe(out, len, in)`: it drops what the font lacks
  and turns each emoji (skin tones, flags and joined sequences included)
  into the code point that draws it in colour. Emoji show only with the
  theme's fonts (`aos_font_*`), not with a font named by hand, and only with
  `/fonts/emoji.pak` on the card (the simulator's is
  `sim/sim_fs/fonts/emoji.pak`).

## Devices on the USB host

Since 0.9 ([USB.md](USB.md), "The USB host"), what the board's USB host
takes is there for the apps, all in `aos_hal.h`:

- **Gamepads and joysticks:** `aos_hal_hid_gamepad_get(i, &pad)` for
  `i` in `0..AOS_GAMEPAD_MAX-1` (false when there is none there): buttons
  as the pad numbers them (bit 0 = button 1), eight axes scaled to
  +-32767 (0 X and 1 Y are the left stick or the D-pad; down is positive),
  the hat, and `reports`, which moves while the pad is alive. Read it every
  frame. `aos_hal_hid_gamepad_dpad(&pad)` gives up/down/left/right from the
  hat or the left stick. HID has no standard button layout: a generic
  SNES-style pad is X=1, A=2, B=3, Y=4, L=5, R=6, Select=9, Start=10, but
  another pad differs, so offer to map them. `/api/usb` shows the pads
  live, to see which is which.
- **The same pad in every game:** `aos_pad.h` (header only, no new
  firmware) reads all the pads into roles, A (buttons 1 or 2), B (3 or 4),
  L (5 or 7), R (6 or 8), START (9 or 10) and the four directions, with
  `pressed`/`released` edges and directions that repeat while held, for
  menus and grids. The retro canvas maps its buttons the same way.
  `aos_pad_menu.h` takes a screen's LVGL buttons: the d-pad moves an
  outline to the nearest one in that direction and A clicks it; the
  outline appears only once the pad is used. In the simulator,
  `P4_SIM_PAD=1` makes the keyboard a pad (the arrows, z/c = A, x/v = B,
  a/d = L/R, Return = START) and a script's `pad <buttons> <x> <y> [ms]`
  holds one (sim/main.c). [GAMEPAD.md](GAMEPAD.md) has the controls of
  every game and what they ran into (the app's `tick` is too slow for a
  D-pad: read the pad from the game's own timer).
- **A keyboard** types into the open LVGL keyboard's text area by itself.
  An app with a keyboard of its own takes the keys while it is up with
  `aos_ui_hwkbd_handler(cb)` (`aos_ui.h`) and gives them back with NULL:
  `cb(key, mods)` gets a Unicode code point (accents already composed) or
  an `AOS_KEY_*`, and returns true for a key it used. Notes does this.
- **A mouse** points with an arrow: the left button is a finger, the right
  one "back", the middle one "home", the wheel scrolls. An app that wants
  more (VNC, Dibujo, PWM's knob) takes the raw reports while it is in
  front with `aos_ui_hwmouse_handler(cb)` (`aos_ui.h`, since 0.10): `cb`
  gets the arrow's position, the motion, the wheel and the buttons with
  their edges, and returns a mask of what it keeps for itself
  (`AOS_HWMOUSE_LEFT`, `_RIGHT`, `_MIDDLE`, `_WHEEL`); the rest goes on
  as before.
- **MIDI:** `aos_hal_midi_read(&msg)` (status, data1, data2) and
  `aos_hal_midi_send(&msg)`; `aos_hal_midi_devices()` lists them.
- **Webcams:** the Cameras app shows them (`usb://N`). For your own,
  `aos_hal_uvc_count`, `_info` (name and MJPEG sizes), `_start`, `_frame`
  (the newest JPEG, for `aos_hal_jpeg_decode`) and `_stop`.
- **Serial ports:** `aos_io_uart_open("usb0", baud, false, owner)` opens a
  USB serial device like a UART; `aos_hal_usb_serial_count()` says how many
  are plugged in.
- **Sound cards:** nothing to do. The player, `aos_hal_spk_*`, the tones
  and the microphone go to and come from the card by themselves when the
  user has one plugged in.

## Five fingers

Since 2026-10-17 the GT911's five fingers reach the apps:
`aos_touch_points_n(out, AOS_TOUCH_MAX)` (`aos_gesture.h`) hands out each
finger in a slot of its own with an id that stays while it is down, and
`aos_hal_touch_frames_n()` (`aos_hal.h`) the raw samples. The two-finger
`aos_touch_points()` and `aos_hal_touch_frames()` stay as they were, for the
apps built against them. A finger that lands beside others counts after a
second sample or 30 ms of rest, whichever comes first: the chip sends a new
sample only when something moves, and the old rule (a second sample) never
accepted a finger resting on a button while another rested on the d-pad.
The simulator's scripts have `fingers <ms> <x1> <y1> [<x2> <y2> ...]`, up to
five fingers resting still.

## Emulators: code written at run time, modules, the keyboard's state, the refresh rate

Asked by the emulator (`apps/emulador`, 2026-10-16), for anything like it:

- **Code an app writes and runs** (a recompiler, a JIT): write it into a
  block of the PSRAM heap (`heap_caps_aligned_alloc(256, n,
  MALLOC_CAP_SPIRAM)`, executable on the P4, as the apps' own code is), then
  `aos_hal_code_sync(code, size)` before jumping into it, from the core that
  runs it: the lines written back, the instruction cache's copies dropped,
  `fence.i`. Never the ROM's `Cache_*_All` (docs/MEMORY.md). The simulator
  answers true and runs nothing recompiled.
- **Modules of its own**: `aos_hal_module_open("/apps/<app>/<name>.so", err,
  sizeof err)`, `aos_hal_module_sym(m, "fn")` and `aos_hal_module_close(m)`.
  The apps' loader and symbol table; `dlsym` finds functions only, so a
  module exports one that returns its table. The loader keys modules by the
  file's name without `.so`: a module named as a card app is refused (and
  `tools/build_apps.sh` fails on it), and one is open once at a time. The
  app builds them into `build/modules/*.so`; `tools/build_apps.sh` checks
  them like apps (float ABI, the firmware's table, no local paths),
  `tools/install_apps.sh` puts them in the card's `apps/<app>/`, and the CI
  in `apps.zip` the same way. The app scan never looks into those folders.
  In the simulator they are linked into the app.
- **The USB keyboard's state**: `aos_hal_usb_kbd_held(usages, max, &mods)`,
  the keys held now on every keyboard (HID usages of the keyboard page) and
  the modifiers' byte, for a player on the keyboard. The events
  (`aos_hal_usb_kbd_read`, with their repeat) go on as before. The
  simulator reads the Mac's keyboard.
- **A refresh rate while in front**: `aos_hal_display_refresh_request(60)`
  returns the rate set: the closest to the one asked that keeps the panel's
  blanking wide enough for its restart (1.9 ms; docs/MEMORY.md, the blue
  flashes). With the default pixel clock (80 MHz) 60 Hz fits (measured with
  Duke Nukem 3D at 640x400 filling core 0: no frame lost); a board set back
  to 60 MHz (`POST /api/panel/qos?mhz=60`) gives about 51 Hz at most. 0 asks for the system's, which comes back by itself
  when the app leaves the front.

## Measuring on the board

- **The log:** `GET /api/log?from=N` returns 16 KB at a time from the oldest
  line of a 32 KB ring. Follow the `X-Log-Next` header, or the newest lines
  look missing. `tools/plog.sh` does that.
- **Screenshots:** `GET /api/screen.bmp` is LVGL's picture;
  `GET /api/screen.bmp?fb=1` is the panel's frame buffer, blits included, in
  the panel's orientation.
- **Touch:** `POST /api/touch` injects through LVGL. Apps that read the
  panel's own samples (`aos_hal_touch_frames`: Mila, Golf, Monster Hop's
  swipes) do not see it.
- **The serial console:** opening the CH340 port resets the board, even with
  DTR/RTS off. Start a capture first, then reproduce.

## Porting an app from AmoledOS

See [plan/APPS.md](plan/APPS.md) for which recipe each app takes. The rules
that bite:

- **The screen:** 720×1280 portrait, 1280×720 landscape. Nothing is enlarged
  (the pixel density is almost the watch's); more fits. No literal positions
  from 368×448.
- **Turning:** the runtime calls the app's `resize()` with the new root;
  without one, it destroys the app and creates it again, without
  reloading the `.so` (its statics keep their values). An app that keeps
  state in statics, or starts something only once per load, should have a
  `resize()`. Before 2026-10-12 the loader dropped `resize()`, so on the
  board every card app was created again.
- **Retro games** draw into a small canvas that the OS scales (`aos_retro_*`,
  [RETRO.md](RETRO.md)).
- **GCC is stricter than the simulator's clang:** `-Werror=format-truncation`
  fails the board build. Build the `.so` before calling a port done.
- **Memory:** follow [MEMORY.md](MEMORY.md). Threads through
  `aos_hal_thread_start` (PSRAM stack), big buffers by `malloc`, card buffers
  by `aos_hal_io_alloc`, never an RPC to the C6 under LVGL's lock.
- **A new libc or libgcc function** an app needs goes into `EXTRA_SYMBOLS` in
  `tools/gen_symbols.py`, then the table is regenerated and the firmware
  rebuilt.
