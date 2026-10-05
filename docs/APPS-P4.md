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

## Measured

- **Boot (2026-09-30):** 34 modules and 40 apps registered in 1.5 s, and the
  system is up at 4.2 s. Registering used to rebuild the home screen once per
  app: 8.3 s with 40 apps. The scan now holds the home screen and builds it
  once (`aos_ui_hold_home`).
- **`hello_app.so`:** 3 KB, 18 symbols. 16 apps opened one after the other with
  no reset; internal RAM steady at ~126 KB free.
- Every app in `apps/` builds and passes the checks.

### The apps on the board

Fps measured with the app's own log line in `/api/log`.

| App | Upright | Lying down | How it draws |
|---|---|---|---|
| Doom | – | 35 fps | 320×200 ×3 by the PPA into the free buffer, flipped |
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

## The descriptor's flags, while the app runs

`self->desc.flags` may change while the app runs: the shell reads them at
every touch. The VNC viewer sets `AOS_APP_FLAG_NO_SWIPE |
AOS_APP_FLAG_LONG_DRAG` only while it shows the remote screen, so a drag
from the left edge is the remote's there and "back" everywhere else.

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
