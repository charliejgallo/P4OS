# Golf

The AmoledOS watch's golf game (`ESP32S3_AmoledOS/apps/golf`) on the P4's 5"
screen, full screen, upright or lying down: a golfer modelled and animated in
Blender, a 3D view of each hole from behind the ball with hills, trees and a
sky, and a detailed map from above that shows where the ball flew. Three
courses of eight holes, the shop, the tournament against three computer
golfers, practice, and 2 to 4 players passing the board around. The rules,
the courses, the physics and the saved progress are the watch's; what changed
is below.

| Course | Theme | What it plays like |
| --- | --- | --- |
| Sierra Verde | woods | parkland: pines, oaks and poplars, a creek, doglegs, par 32 |
| Dunas del Faro | coast | links by the sea: beaches, marram dunes, pot bunkers, palms, 1.6x the wind |
| Parque de los Lagos | lakes | water on every hole: ponds, a creek, an island green, lilies; calm air |

```bash
# the art, at twice the watch's pixels (Blender 3.3, headless; ~1 h on the M2)
B=/Applications/Blender.app/Contents/MacOS/Blender
cd apps/golf/tools/blender
$B -b -P props.py  -- --out ../../build/art/props  --scale 2 --ss 3
$B -b -P golfer.py -- --out ../../build/art/render --scale 2
cd -
# the pack (a minute)
python3 apps/golf/tools/pack_p4.py            # -> apps/golf/build/golf_p4.pak

# in the simulator
cp apps/golf/build/golf_p4.pak sim/sim_fs/apps/
cmake -S sim -B sim/build-golf -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS=golf
cmake --build sim/build-golf -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.golf" ./build-golf/p4os_sim

# the .so for the card
tools/build_apps.sh golf                      # apps/golf/build/golf.so
```

On the card both go in `/apps`: `golf.so` and `golf_p4.pak` (~4 MB). Neither
is in git (`apps/*/build/`). Without the pack the game still runs, with trees
drawn by code and no golfer; a watch pack (version 1) is refused for the
golfer, whose frames are for the other screen.

## Playing: touch only

The watch's game was already touch: nothing needed the IMU or the button.

1. **The map.** Turned so the hole goes up the screen (lying down, so it runs
   to the right: the long side of the screen is its length), zoomed to what
   matters for the shot. The finger moves the line, the arrows change the
   club; two fingers zoom and move the map (a stretched preview while they
   move, the sharp map when they lift). On easy, the dotted yellow line is
   where the ball really goes.
2. **Swing.** The 3D view. Three taps anywhere: start the backswing, stop
   the power (over the top of the bar is an overswing), and stop the marker
   in the green band on its way down.
3. **The flight**, back on the map, with the ball's shadow and its line.
4. **On the green**, the slope as chevrons; two taps, power and go.

Pause: the list button in the corner or the system's back. A swipe to the
right leaves the setup screens, the settings and the shop.

## Playing with a USB gamepad

The same game, with no finger. On the map the stick turns the line by speed
(a little deflection is a slow, fine turn, all of it a fast one) and the
d-pad a quarter of a degree a press, repeating while held and in whole
degrees after a second; the line turns the way the stick points on the
screen, however the map is turned. With the putter, up and down move the
marker nearer or further. L and R change the club. A is every tap: Hit opens
the 3D view, then the swing's three taps (backswing, power, accuracy), the
putt's two, and skipping the flight or the banner. B in the 3D view, before
the swing starts, goes back to the map. START pauses.

The panels (menu, setup, settings, shop, pause, the hole and round cards)
are walked with the d-pad, with A or START to press and B as the system's
back (on the menu it does nothing: the app is left from the system). L and
R change the course in the setup and the category in the shop. Nobody lists
the buttons for the pad: `gfa_button` marks the ones it makes
(`GF_PAD_BTN`) and `pad_tick` (golf.c) gathers those under the panel showing
every frame, so a setup built again for another course, the shop's list
after a purchase and every panel after the screen turns are followed by
themselves.

## Both orientations

The screen can turn at any moment (`app_resize`). The panels are built again
for the new shape and show what they showed; the canvas gets a sky and grass
at once, and the worker makes the state's picture again (`gfp_refit`): the
map for the new view, the 3D view from the same camera for the new frame, the
menu's scene for the other side. Until it is there the state waits
(`fit_pending`), and a picture the worker was making for the old shape is
thrown away when it arrives (`s_lgen`). A round goes on where it was, the
tracer of a flight in the air included.

| | Upright 720x1280 | Lying down 1280x720 |
| --- | --- | --- |
| menu | the title over the sky, the buttons beside the golfer | the watch's: the buttons in a column on the right |
| map | the hole up the screen | the hole to the right |
| swing | principal point at (360, 700): sky above, the golfer's feet at 1062 | (640, 318): the feet at 680 |
| shop | the turntable on top, the items in two columns under it | the turntable on the left, the list on the right |
| hole card | the card on top, the golfer reacting under it | the card on the right, the golfer on the left |

## The art: the watch's cameras at twice the pixels

The P4's pixels are almost the watch's size (294 against 322 ppi), so the
golfer is not drawn bigger: the 3D view shows more around him. The Blender
scripts render the same cameras at `--scale 2`: the swing camera's 368x448
frame becomes 736x896 with the same 50 degrees of field, so its focal length
is 960.7 px on either orientation, and a frame is placed by its offset from
the principal point (`GF_ART_PPX / PPY`, `gf_cam_frame`). The golfer comes out
~140x345 px at the top of the backswing, rendered with one more level of
subdivision than the watch's (`SUBSURF_EXTRA`) so his silhouette stays
smooth at that size. Trees are 256 px tall on their side (the watch's 128),
192x192 seen from above, the flag 128 px; the shop's turntable is 368x560.

Why 2 and not the screen's 1.96 (720 / 368): whole pixels keep the pack's
crops and anchors exact, and lying down the golfer's feet still fit 40 px
above the bottom with the horizon 130 px under the top. Any more and lying
down has no sky.

### Colouring when drawn

The watch coloured each frame once per outfit and kept it: at twice the
pixels the swing and the wait alone would be 5.5 MB of RGB565 and alpha (8 MB
with the hole card's cheer and sadness). Here what stays in PSRAM is each
frame's LZ4 block as it is in the pack, ~1 MB for the swing and the wait,
and drawing a frame decodes it into a scratch buffer and lights each pixel
from a table of the outfit's colours (`s_lut`, 16 regions x 256 greys): a new
outfit is a new table, and the shop's turntable changes clothes at once. A
frame is decoded again only when it changes (22 times a second in the swing).

## How frames reach the screen

The watch's scheme, unchanged: one RGB565 canvas the size of the screen,
backgrounds (the map, the 3D view) in buffers of their own, and every frame
the rectangles dirtied the frame before are restored, the moving things drawn,
and only those rectangles invalidated. LVGL pushes them through its own flush
(the DMA2D upright, turned on the CPU in internal RAM lying down), so the
HUD's labels and buttons compose over the canvas for free. A frame of the
swing is ~240 kpx (the golfer, his shadow, the meter), the flight ~10 kpx.

Every picture that takes longer than a frame is made by the worker (core 0,
priority 3), each in two halves on the two cores (`aos_hal_worker_split`
through `gf_split`):

- **The 3D view** (`gf_view3d.c`): the ground and the sky at half the pixels
  each way (360 rays upright, the watch's 368: the ground costs what it did
  there), then scaled up bilinearly, then the far trees and the flag at the
  screen's resolution, tested against the ground's depth. Trees whose art is
  magnified 2x or more are drawn with the ground at its resolution: the art
  has no more detail to give, and they write their depth so the far trees go
  behind them. First pass: the left and the right half of the columns;
  second: the top and the bottom half of the rows.
- **The map** (`gf_map.c`): at the screen's resolution, the strips of 16 rows
  alternating between the cores, then the trees, the markers and the green's
  chevrons in two halves of the rows.
- **The ground texture** of a hole (half a metre a texel), the same way.

The sprite sampler (`gf_mip_draw`) went from a float per pixel to 16.16
fixed point, and `gf_blend` to an inline: the trees of the 3D view took half
the time they did.

## Memory (the simulator, every allocation counted by `gf_malloc`)

| | PSRAM |
| --- | --- |
| three screen buffers (canvas, map, 3D view), each 720x1280 | 5.3 MB |
| the 3D view's ground and depth at half the pixels | 0.9 MB |
| height grid, normals and ground texture, for the biggest hole | 1.8 MB |
| trees and flag, with their mip levels | 2.2 MB |
| the golfer's blocks: swing + wait (+ cheer and sad at the hole card) | 1.0 MB (+0.5) |
| **playing** | **~11.8 MB** (12.1 at the hole card) |

The pinch's stretched map borrows the 3D view's buffer (that view is made
again after). Internal RAM: nothing of the game's; the worker's stack and the
split helper's are in PSRAM, LVGL's objects too.

## Measured and expected

In the simulator (the Mac, both halves one after the other), best of several
runs, `tools/bench.c`:

| | upright | lying down |
| --- | --- | --- |
| 3D view, hole 1 / 3 | 22 / 33 ms | 47 / 52 ms |
| map, hole 1 / 3 | 59 / 78 ms | 81 / 74 ms |

The watch's own renderers on the same Mac: 14 ms the 3D view, 12 ms the map,
which were 1.6 s and 0.9 s on the S3. By that ratio and two cores, expected
on the board (to be measured): **the 3D view ~0.7-1.2 s upright, ~1.5-2 s lying
down** (drawn ahead while aiming, so the swing usually opens at once), **the
map ~1.5-2 s** (behind the previous shot's banner, 1.8 s). The swing should
run at the 30 fps of its timer and the flight too; the watch did 23 and 29.

What to read on the board, in the portal's `/api/log`:

```
golf: swing 29.6 fps 720x1280, draw 3.1 ms, push 240 kpx/frame (panel 29.5 fps, 247 kpx) | game 11821 KB, psram free ...
golf: 3D view 720x1280 812 ms (passes 402 + 395): ground 350 (...), sky 40, up 80, trees 270
golf: map 1540 ms (327 px/m x100): strips 1300 = base ... + layers ... + light ..., trees 240
```

The first line comes every 3 s while something moves (the game's own drawing,
what it asks LVGL to push, what the panel did push), the others after each
render; `state N: frames in ms` when a state ends, as on the watch.

## The game against another board

It rides on the ESP-NOW link (`aos_hal_link_*`), which the P4 may not have
(the C6 through esp_hosted). The multiplayer screen tries
`aos_hal_link_start()` once per opening of the app; when it fails, nothing
about it is shown. The protocol is the watch's, untouched.

## Saved progress

The watch's preferences, under the same keys: `gf_coins`, `gf_own0..5`,
`gf_eq`, `gf_units`, `gf_sfx`, `gf_diff`, `gf_course`, `gf_best0..2`. The
menu's picture is cached on the card as `golf_menu<course><p|l>.bin` (1.8 MB
each; bump `MENU_CACHE_VER` in `gf_play.c` when the renderers or hole 1
change).

## Files

| File | What |
| --- | --- |
| `golf.c` | life cycle, the layout (`gfa_layout`), the panels for either orientation, the turn (`app_resize`), the frame-rate log |
| `gf_play.c` | a hole being played; the worker and its jobs; `gfp_refit` |
| `gf_view3d.c` | the 3D view in two passes of two halves |
| `gf_map.c` | the map, its strips on the two cores |
| `gf_art.c` | `golf_p4.pak`: trees with mip levels, the golfer coloured when drawn |
| `gf_gfx.c` | pixels; `gf_malloc` counts; `gf_split` |
| `gf_shop.c` | the shop, both orientations |
| the rest | the watch's, unchanged (courses, world, physics, game, audio, link) |
| `tools/pack_p4.py` · `tools/lz4blk.c` | the pack, from the 2x renders |
| `tools/blender/golfer.py` · `props.py` | the watch's scripts, with `--scale` |
| `tools/bench.c` | the renderers on the Mac |

## Simulator switches

`GF_MODE=0..4`, `GF_HOLE=1..8`, `GF_DIFF=0..2`, `GF_AUTO=1` (the bot plays),
`GF_COINS=n`, `GF_COURSE=0..2`, `GF_SCREEN=shop|settings|practice|boot|card`,
`GF_LINK=1`, as on the watch.

## Pending

- Measure on the board: the lines above, upright and lying down, and PSRAM
  free while playing and after closing.
- The strings new to the port ("En esta placa, pasándola", "Contra la otra
  placa", "Aparea otra placa en Enlace para jugar a distancia") are not in
  the language catalogs yet.
- The map is the slowest render (~5.6x the watch's pixels); if the board says
  it is too slow behind the banner, its rough base could be computed only
  where no layer covers it.
