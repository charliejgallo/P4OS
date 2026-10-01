# Lua

Scripts on the card for P4OS (ported from AmoledOS): Lua 5.4.8 in one `.so`,
and every `.lua` in `/sdcard/lua` is an app of its own in the launcher, with
its name, its colour and its icon. A mistake in a script is a message with a
line number, never a reboot.

```bash
# in the simulator (sim_fs is the fake card, git-ignored)
mkdir -p sim/sim_fs/lua && cp apps/lua/scripts/*.lua apps/lua/scripts/*.aic sim/sim_fs/lua/
cmake -S sim -B sim/build-lua -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS=lua && cmake --build sim/build-lua -j8
cd sim && P4_SIM_SCRIPT="wait 800; open lua.cubo" ./build-lua/p4os_sim
LUA_RUN=pelota.lua P4_SIM_SCRIPT="wait 800; open aos.lua" ./build-lua/p4os_sim   # the list app, straight into a script

# the .so for the card
tools/build_apps.sh lua                       # apps/lua/build/lua.so
# card:  /apps/lua.so, and the scripts in /lua/ (apps/lua/scripts/*.lua and *.aic)
```

The scripts are data: no rebuild to add one. `PUT /api/fs/put?path=/lua/mine.lua`
on the portal, or the card in the computer. A new script is in the list inside
the app at once, and in the launcher after a restart (the card is scanned at
boot). While a script runs, the app watches its file and reloads it when it
changes, so saving is the whole step.

## The samples (`scripts/`, to `/sdcard/lua/`)

| File | Shows |
|---|---|
| `hola.lua` | The smallest script: `draw()` and a beep on touch. |
| `cubo.lua` | The bench: wireframe cubes with the maths per vertex in Lua. Tap to add one (up to 24). `resize()` keeps them spinning through a turn. |
| `pelota.lua` | Drawing without erasing: a drawn grid frozen with `aos.background()`, eight balls, and the rows and pixels each frame cost. |
| `gestos.lua` | `gesture()` (pinch to zoom, drag, double tap) and `aos.fingers()` (a ring under each finger). |
| `pintar.lua` | New. Finger painting with every finger at once, a palette strip, and its own canvas: `@canvas 360x640`, x2. The paper starts with a black kitten on it. |
| `atrapa.lua` | New. A small game by touch alone: the basket follows the finger, anywhere on the screen. `@orientation portrait`. |

`cubo.aic` and `hola.aic` are their launcher icons, assembled from the
`.aic.txt` beside them with `python3 tools/aic.py asm scripts/cubo.aic.txt`.
`/sdcard/icons/lua.<name>.aic` also works and wins.

## The canvas

A script draws into a buffer of its own, and the OS shows it through the
retro canvas ([docs/RETRO.md](../../docs/RETRO.md)), scaled by a whole number.
There are no on-screen controls: the whole screen is the script's.

- **By default it fills the screen at x3**: 240x426 upright (720x1278),
  426x240 lying down (1278x720). A script reads the size from `aos.W` and
  `aos.H` and never learns the scale; the finger comes in canvas pixels too.
- **A script may choose.** `-- @canvas WxH` in its first lines (both sides
  16..1280, at most 720x1280 pixels). The OS takes the biggest factor that
  fits, centred: `184x224` is the watch's canvas at x3, `360x640` is x2,
  `720x1280` is the screen itself at x1.
- **Turning the screen.** A canvas of a chosen size stays the same, pixels
  and all, and is only placed again (in landscape a portrait canvas gets
  smaller). The default canvas takes the new shape: if the script defines
  `resize(w, h)` it is called, with `aos.W`/`aos.H` already new, a black
  canvas and no frozen background; otherwise the script starts again.
- **`-- @orientation portrait`** (or `landscape`) turns the screen for the
  script's launcher entry. A script opened from the list inside the app runs
  in whatever orientation the list was in.

The frame's ceiling is 50 per second. Only what changed is copied and scaled:
every primitive marks the box it touched.

## The `aos` table

| | |
|---|---|
| `aos.W`, `aos.H` | the canvas |
| `aos.clear(c)` | fills everything |
| `aos.pixel(x, y, c)` | |
| `aos.rect(x, y, w, h, c)` / `aos.frame(...)` | filled / outline |
| `aos.line(x0, y0, x1, y1, c)` | |
| `aos.disc(cx, cy, r, c)` / `aos.ring(...)` | filled / outline |
| `aos.text(x, y, s, c [, scale])` | 5x7, upper case, digits and signs; words people read are not for this |
| `aos.shade(x, y, w, h, f)` | darkens (f<0) or lightens (f>0), in sixteenths |
| `aos.touch()` | `x, y, down`: where the finger is |
| `aos.fingers()` | `n, id1, x1, y1, id2, x2, y2`: every finger down, each with an id that stays while it does |
| `aos.ms()` | milliseconds since the script started |
| `aos.beep(hz, ms)` | |
| `aos.background()` | freezes what is drawn as the world; from then on the app undoes each frame |
| `aos.stats()` | `script_ms, screen_ms, frame_ms, rows, pixels` of the last frame |

Colours are `0xRRGGBB`. And the callbacks, all optional: `init()`,
`tick(dt)`, `draw()`, `touch(x, y, ev)` (`"down"`, `"move"`, `"up"`),
`gesture(ev, x, y, a, b, c)` (`"tap"`, `"double"`, `"long"`, `"drag"`,
`"release"`, `"pinchstart"`, `"pinch"`, `"pinchend"`) and, new, `resize(w, h)`.

**Input is the finger alone.** The board has no buttons and no motion
sensor, and the API never had anything to read either. The way out is the
system's: the left edge goes back (to the list, or out of a launcher entry)
and the bottom edge goes home. A drag that starts anywhere else is the
script's, pinches included.

No `io`, `os`, `package` or `debug`: their sources are not in the binary.

## What changed from the watch

- **The script runs on a thread of its own** (96 KB of stack in PSRAM, core
  0, priority 3) and not in the LVGL task. That task has 12 KB on the P4, and
  measured with the board's GCC (`-fstack-usage`, `-Os`) a nested
  parenthesis costs ~144 bytes of C stack in the parser and a nested pcall
  ~700: on the watch 68 parentheses already rebooted the board, and here no
  value of `LUAI_MAXCCALLS` would fit. On the thread it is 100 (see
  `CMakeLists.txt`): 1000 nested parentheses end in `C stack overflow` as
  an ordinary Lua error, and a pcall that nests itself stops at the guard
  with the same error in its hands (both tried in the simulator). The interface also stops waiting for a
  script: a slow one only gets fewer frames.
  - One word hands everything over (`phase`, acquire/release): while it says
    busy the thread owns the script's buffer and state, otherwise the LVGL
    task does. The LVGL task never waits, except to leave or to turn.
  - The finger is queued in the LVGL task and delivered at the start of the
    next frame, before `tick()`; `aos.fingers()` is a snapshot taken there.
    `aos.beep()` is queued and played when the frame comes back.
  - The script draws into its own buffer; what changed is copied onto the
    OS's canvas in the LVGL task, so LVGL never reads pixels being drawn.
- **The budget can no longer be swallowed.** Once the 400 ms hook cuts a
  call, it fires on every instruction, so `while true do pcall(f) end` cannot
  catch the cut and go round again. The chunk and `init()` get 2 s.
- **Lua's heap has a ceiling**, 4 MB of PSRAM, past which the script gets
  "not enough memory" instead of taking the system's.
- **The canvas is the OS's** (above), instead of an x2 upscale and a blit.
  The watch's frame-cost label is gone: screenshots see the canvas now, and
  `aos.stats()` gives the numbers.
- **The error message reloads the script** when tapped; there is no side
  button to go on with.
- The list is laid out for the 5" screen and both orientations, sorted.

## Measured

In the simulator (Release, M-series Mac), which says the plumbing works, not
what the board does: `cubo.lua` holds 50 fps with 24 cubes (192 vertices a
frame), and `pelota.lua` presents 5,000 of the canvas's 102,000 pixels a
frame. The watch's numbers (1.9 M loop turns a second from PSRAM, a 77 KB
state costing 52 bytes of internal RAM) are in AmoledOS's `docs/LUA.md`.

## Still to do

- **On the board**: run `cubo.lua` at 24 cubes and read `aos.stats()`; check
  that 1000 nested parentheses and a deep pcall end in a Lua error (the
  guard is sized from `-fstack-usage`, not yet from a crash); watch internal
  RAM while a script runs.
- The portal's `#lua` page (editor, list, console reading `_estado.txt`,
  run through `POST /api/open?id=lua.x`) is in `components/aos_portal/web/app.js`.
