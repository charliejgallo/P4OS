# Mila

A Sokoban with a black kitten. Mila, sweet and small with big amber eyes,
pushes things back to their place around the house, one cell at a time:
yarn balls into their baskets, cookie tins onto their placemats, flower pots
onto their soil, boxes onto tape crosses, crates onto moon marks. Between
levels she lives in her casita, where you pet her and play with the toys you
bought in the shop, which also sells hats, collars and scarves.

This is the AmoledOS watch game (`ESP32S3_AmoledOS/apps/mila`, its
`DESIGN.md` has the rules and the worlds) on the P4's 5" screen, with every
sprite rendered again in Blender at 1.5 x (the casita at 1.92 x, so its
room is the screen's width). The rules, the 40 levels, the
shop, the casita and the saved progress are the watch's; what changed is
below.

```bash
# the art (once, and after a model changes; see "The art" for the times)
cd apps/mila/tools/blender
B=/Applications/Blender.app/Contents/MacOS/Blender
for i in 0 1 2; do $B -b -P mila.py -- --out ../../art/mila --part $i/3 & done; wait
python3 mila_merge.py ../../art/mila
$B -b -P worlds_a.py -- --out ../../art --world living,kitchen,garden --phase 2
$B -b -P worlds_b.py -- --out ../../art --world attic,roofs --phase 2
$B -b -P casita.py -- --out ../../art/casita          # only the shop's toy icons are used
# the casita at its own scale, and Mila's casita frames at it (17 min in three parts)
ML_RES=1.92 $B -b -P casita.py -- --out ../../art/casita_hd
C=c_walk,c_run,c_sit,c_sleep,c_belly,c_pounce,c_bat,c_jump,c_scratch,c_eat,c_groom,c_meow,c_purr,c_peek,c_lie
for i in 0 1 2; do ML_RES=1.92 $B -b -P mila.py -- --out ../../art/mila_hd --anims $C --part $i/3 & done; wait
python3 mila_merge.py ../../art/mila_hd
$B -b -P ui.py -- --out ../../art/ui
ML_RES=1.95652 $B -b -P map.py -- --out ../../art/map
cd ../..

# the pack: the levels (solved again) and the art
python3 tools/pack_p4.py --split 7.5          # -> apps/mila/build/mila_p4.pak, .pak.1, .pak.2

# in the simulator
cp apps/mila/build/mila_p4.pak* sim/sim_fs/apps/
cmake -S sim -B sim/build-mila -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS=mila
cmake --build sim/build-mila -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.mila" ./build-mila/p4os_sim

# the .so for the card
tools/build_apps.sh mila                      # apps/mila/build/mila.so
```

On the card they go in `/apps`: `mila.so` and `mila_p4.pak` with its parts
(`mila_p4.pak.1`, `.2`: the portal takes 8 MB per upload, and the game reads
the parts as one file). A part left from an older pack is read as the end
of the new one: replace them all together. Neither is in
git, nor are the renders (`art/`, ignored by `apps/mila/.gitignore`).
Before the renders exist, `python3 tools/pack_p4.py --from-watch
<AmoledOS>/apps/mila/assets` makes a stand-in pack from the watch's own
sprites smoothed up, and `--stand-in mila=<dir>` does it for one folder:
only to try the engine.

## The art: the watch's scenes at 1.5 x

`tools/blender` is the watch's pipeline, copied, with one change in
`ml_common.py`: `ML_RES` (1.5 unless told otherwise) makes every pixel of the
camera that much smaller. Metres do not change, so the models, a floor's
height and the depth pass (1/32 m a step) are the watch's, and the engine's
depth numbers too. Pixel sizes written in the scripts (a lamp's light pool,
the shop's toy thumbnails, the UI's icons) are multiplied by it; the map is
rendered with `ML_RES=720/368` so its panels are the 720-px column.

| | Watch | P4 |
| --- | --- | --- |
| One cell | 72 x 54 px | 108 x 81 px |
| Mila, in a level / in the casita / on the shop's turntable | 58 / 87 / 175 px | 87 / 166 / 260 px |
| The casita's room | 368 px wide | 719 x 801 (ML_RES 1.92) |
| A level's view | 5 x 8 cells | 6.7 x 15.8 upright, 11.9 x 8.9 lying down |
| Map panels | 368 px wide | 720 px wide |
| UI icons, logo | 40 px, 300 x 120 | 60 px, 450 x 180 |
| Pack | 6.6 MB | 16.9 MB on the card in three parts, 29.5 MB unpacked (1088 sheets) |

Why 1.5: it is Monster Hop's scale on this board, so the two games look
alike; the grid stays on whole pixels (108 x 81, a floor 36 px); upright
every level fits the screen's height (the tallest is 11 rows: 891 px) and
the narrow ones its width, lying down every level fits the width. At 2 x
(the plan's first guess, a ~26 MB pack) the casita's frames alone would take
10 MB of PSRAM. Everything in the scenes has more detail than on the watch
because it is rendered, not scaled: Mila's fur and whiskers, the hats' knit,
the wood grain.

The casita is rendered apart at 1.92 x (`ML_RES=1.92` into `art/casita_hd`
and `art/mila_hd`; `tools/pack_p4.py` then leaves out the casita and her
casita frames at 1.5). The room is 3.4 m across, the watch's 368 px, and at
1.92 it is 719 px: the screen's width upright. Lying down the same picture
is 801 px on a 720 px screen, and what is left out is the top of the wall
(wallpaper and trim) and the front of the floor, which the watch's HUD
covered too: one set of renders serves both. At 1.5 the room was 563 px,
three quarters of the width, with black around.

Render times on the M2 (2026-09-29, with other renders running beside):
Mila's 230 frames with her 14 layers 61 min in three parts at once, the five kits,
the casita, the map and the UI within that hour beside them.

## How frames reach the screen: only what changed

A puzzle stands still most of the time, so a frame only draws, and the timer
only pushes, the 32 x 32 tiles that changed (`ml_dmg_t` in `ml_render.h`):

- a level: the draw list against the last one (a thing that moved, Mila's
  pose), the cache's blocks drawn again (a plate pressed, a hole filled),
  the HUD's boxes when a number or a flash changes. A camera that moved, the
  zoom onto Mila, a mode that changed or a turned screen is the whole frame.
- the casita: the draw list (Mila, the toys, the guest), the hearts, the
  coins when they change.
- the map: all of it while it scrolls, the current stone's ring and Mila's
  marker while it stands.

The camera helps: a level that fits the screen one way is centred that way
and never moves along it, and the other way it keeps still until Mila walks
out of the middle 40 %, then glides to centre her. So walking and pushing in
most levels redraws 3-5 % of the screen.

The worker (core 0, priority 3) steps the scene and draws what changed in
bands of 24 KB of internal DMA RAM, even bands on its core and odd ones on
the other (`aos_hal_worker_split`), each copied into a frame buffer of our
own in PSRAM (on a 128-byte line), which is also the picture of the LVGL
canvas under everything (what LVGL composes under a panel, a toast, the
switcher or the gesture home). When nothing changed the worker draws
nothing and sleeps 10 ms; it never draws more than ~60 frames a second.

**Page flipping** (the board; `mila.c`, "Frames to the panel"): each band
is copied as well into the panel's free buffer (`aos_hal_display_back`),
upright as it is, lying down turned by the PPA on the way from internal RAM
(`aos_hal_display_blit_into_fit`, bands of 9 rows), and the LVGL timer (8
ms) flips to it (`aos_hal_display_flip`): no tearing, no push. A panel
buffer holds the frame of the flip that last showed it, one or two flips
old, so the worker draws into it the union of what the flips since changed
(a ring of the last four flips' tiles) and what this frame changes; all of
it when that is not known (never shown by us, or LVGL drew into it:
`aos_hal_display_back_age`). One frame of our own is enough: the timer
flips within a tick and gives it back, where two were needed for the push.
No flip while anything of LVGL's would be hidden by one:

- our own panels (pause, results, shop, settings, lobby) and the loader
  are states in which the worker draws no scene frames; the scene's first
  frame, drawn under the loader, is pushed as before, then LVGL redraws the
  whole screen from the canvas as the loader goes;
- the casita's logo, coins and buttons, the level's HUD and the map's
  labels are drawn into the frame by the worker, not by LVGL: nothing of
  LVGL's is on screen in a scene;
- under the system's panel, the switcher, the gesture home, the zoom from
  the icon or a banner (`aos_ui_overlay()`, read by the timer) the scene
  waits and nothing goes up; under a toast the frames go through LVGL's
  canvas, which draws the toast over them;
- when LVGL has drawn into the buffer on screen since our flip (the loader
  or a panel going, the canvas coming back after a turn) the next frame is
  whole, since what it drew there may be the canvas caught halfway.

Without a free buffer (the simulator, pref `fbs` = 1) the timer pushes the
newest frame's changes as before: upright, runs of whole rows with
`aos_hal_display_blit_scaled` (the DMA2D copy); lying down, each run's
rectangle with `aos_hal_display_blit_fit` (the PPA turns it on the way: a
whole frame is 62 ms there, a kitten walking a cell a few); two frames of
our own then (one in the casita), each remembering what it lacks since it
was last drawn, and the panel what it lacks since the last push.

The log, every 3 s in a scene (portal `/api/log` on the board):

```
mila: level: 41.3 fps 720x1280, render 2 ms, 4% redrawn, push 900 us (1 rects), 150 idle steps | 120 flipped, 0 pushed, 0 via LVGL, 1 whole after LVGL
```

`fps` is frames drawn (only frames that changed something), `render` the
worker's time per frame (step, what changed, bands, the copies into the
panel's buffer), `redrawn` the share of the screen, `push` the timer's time
per push or flip and the rectangles in it; then how the frames went up, and
how many were whole because LVGL had drawn into the buffer on screen. Also
logged: `mila: frames: ...` (flipped, turned or pushed, when that changes)
and `mila: 0x.. over the app` / `nothing over the app` as the system's UI
comes and goes.

## Memory

Every scene loads only its own art (`ml_art.c`), and logs what it holds:

```
mila: memory in the casita: art 5934 KB + unpacked 0 + cache 2496 + frames 1800 (1) + overview 0 = 10230 KB of PSRAM; bands 46 KB internal
mila: casita: 21.6 fps 720x1280, render 2 ms, 5% redrawn, push 900 us (1 rects), 72 idle steps, 120 unpacks, 194 KB unpacked
```

Measured in the simulator with the renders' pack, every toy (`ML_UNLOCK=1`)
and the biggest outfit, the beanie and the pearls (the numbers the app works
out from its own buffers; the simulator's heap numbers are made up),
2026-09-30:

| Scene | Art | Cache | Frames | Whole-level picture | PSRAM |
| --- | --- | --- | --- | --- | --- |
| a level (all 40 loaded one by one) | 1.7-2.5 MB (roofs 6 the most) | 4.8 MB | 3.5 MB (two) | 1.8 MB, only while it shows | 12.0-12.9 MB; 10.2-11.1 MB while played |
| the casita, at 1.92 | 5.8 MB (her 138 casita frames packed: 4.2 MB, body and shadow 3.25, beanie 0.8, pearls 0.2; unpacked they would be 7.4), + 0.2-0.35 MB unpacked | 2.4 MB | 1.8 MB (one) | - | 10.2-10.6 MB |
| the map | 7.6 MB (seven 720-px panels) | - | 3.5 MB (two) | - | 11.1 MB |

With page flipping (the board, since 2026-09-30) every scene keeps one
frame (the memory line says `frames 1800 (1)`): the table's level and map
less 1.8 MB each, so a level 10.2-11.1 MB (8.4-9.3 while played) and the
map 9.3 MB; the casita stays as it was.

The casita at 1.5 was 13.5 MB (art 5.9, cache 3.75, two frames 3.5); at
1.92, done the same way, it would be 17.4 MB. What makes it 10.2:

- **Mila's casita frames stay packed** (`ML_ZIP`, `tools/pack_p4.py` and
  `ml_art.c`): every frame its own LZ4 block, the pixels' bytes in planes
  (57 % of the frame; interleaved LZ4 only gets to 62 %: the dithered
  RGB565 of her fur hardly packs). Her sprites know their size and anchor
  without their pixels; when her pose changes, `ml_zstream_get()` unpacks
  the frame into one of two slots per layer (body, shadow, hat, neck; the
  guest's four too), so the frame the last draw list points at stays whole
  for what changed. A frame is unpacked once per change: 20-50 a second
  while she walks, each 20-60 KB. The slots and the scratch hold 0.2-0.35 MB.
- **The cache is the room's box**, not the screen: 768 x 832 upright, 768 x
  768 lying down (it does not wrap; around it is black).
- **One frame in the casita** (without page flipping the other scenes keep
  two): the worker waits for the push, a few ms, since little changes
  there. The frame goes back to the worker as soon as the blit returns (or
  the flip), or, when the canvas shows it (the simulator), after LVGL's
  next refresh (`release_shown`).

Internal RAM: the two bands (46 KB, DMA-capable so they are not the slow LP
SRAM) and the worker's stack in PSRAM (the HAL's). What was trimmed to get
there: the watch's copy of the frame for the canvas is gone (the canvas shows
the frame buffer itself); the whole-level picture is freed while the level is
played and painted again for a peek; the casita's room picture is freed once
it is in the cache; Mila's frames are freed on the map.

## Playing: touch, or a USB gamepad

The watch's BOOT button (undo, pause) is gone; everything is on the glass:

- **Swipe** to step one cell (40 px of finger, for 5" of glass), a thing in
  the way is pushed; keep the finger down and Mila keeps walking.
- **Tap a cell** and Mila walks there by the shortest way, never pushing.
- **Undo** bottom left, **restart** bottom right, **pause** top left (and
  the system's back).
- A level opens on the whole room with its goal; a tap flies the camera down
  onto Mila. **Pinch** (fingers together) for the whole room again, spread
  them to go back; or **touch and hold Mila**.

**A USB gamepad** (`aos_pad.h`) plays it all without touching the screen:

| | |
| --- | --- |
| D-pad / left stick | a level: one step a press (a thing in the way is pushed); held, she keeps walking. The casita: the cursor along its buttons. The map: Mila to the next open level (up/right) or the one before (down/left) |
| A | the casita's button, the map's level, the panels' outlined button; ends a level's overview |
| B | undo in a level; back on the map and the panels; a pet in the casita |
| R | start over in a level; the next world on the map; a toy for her in the casita; the next item in the shop |
| L | held, the whole level (as a finger held on Mila); the world before on the map; the day's present in the casita; the item before in the shop |
| START | pause and resume; ends the overview; Jugar in the casita, the level on the map |

The casita's cursor is a lighter tile under a button, drawn in the frame
(`mlc_gamepad`); the map's is Mila's own marker (`mlm_gamepad`), and the
strip glides to keep her in view; the panels (pause, results, shop,
settings, lobby) move `aos_pad_menu.h`'s outline to the nearest button. The
casita's cursor and the outline only show once the pad is used.

Upright the level sits between the HUD's rows; lying down the HUD goes to the
corners beside it. The screen can turn at any moment: a level being played
pauses, the panels are built again for the new shape, and the worker makes
the frames, the cache and the overview again (`JOB_FIT`), then draws the
scene once under the pause.

The casita: upright the logo above the room (the whole width) and the
buttons in a bar under it; lying down the room on the left (the whole
height) and the buttons in a column, the margins in thirds. The map:
upright the 720-px strip; lying down the strip in the middle, the coins and
the home button on the left, and the worlds on the right, each with its
emblem, name and stars (a tap glides the strip there). The shop: upright
Mila on her turntable in the middle; lying down Mila on the left and the
item, the colours and the button on the right.

## Saved progress

The watch's preferences, the same keys (`ml_prog.c`): `ml_s_<world>` (the
stars of each level), `ml_coins`, `ml_own`, `ml_hat`, `ml_neck`, `ml_hatc`,
`ml_neckc`, `ml_gift`, `ml_last`, `ml_snd`, `ml_st_*`; and the loader's
measures `ml_ldc`, `ml_ldm`, `ml_ldl`.

## Two boards

Visits and races ride on the ESP-NOW link (`aos_hal_link_*`), which the P4
does not have (esp_hosted brings no ESP-NOW). The app asks
`aos_hal_link_start()` once when it opens; when it fails the casita has no
friend's button and nothing else waits on the link. The protocol is the
watch's, untouched (`ml_link.c`). In the simulator the link works (UDP), so
the button shows there when another simulator is paired.

## Files

| File | What |
| --- | --- |
| `main/mila.c` | life cycle, the worker and its jobs (`JOB_FIT` when the screen turns), what changed, the bands on both cores, the push, touch, the states |
| `main/ml_render.c` | the frame in bands over the clip's columns; the tiles that changed (`ml_dmg_t`), and a draw list against the last one |
| `main/ml_world.c` | the level's cache: a ring the screen's shape plus a block each way; the overview |
| `main/ml_play.c` | a level being played; the camera that keeps still |
| `main/ml_hud.c` | the HUD in the frame, for either orientation |
| `main/ml_casita.c`, `main/ml_map.c` | the casita and the map, laid out for the screen as it is |
| `main/ml_ui.c` | the LVGL panels, built again when the screen turns (`ml_ui_layout`) |
| the rest | the watch's, unchanged |
| `tools/blender/` | the watch's pipeline at `ML_RES` |
| `tools/pack_p4.py` | the pack (the watch's `pack_assets.py` with the P4's paths) |
| `tools/levels.py`, `tools/levels/`, `tools/solve.c`, `tools/gen.py` | the levels, their solver and generator, the watch's |

## Simulator switches

The watch's: `ML_LEVEL=<world>,<n>`, `ML_SCREEN=map|shop|settings`,
`ML_UNLOCK=1`, `ML_COINS=n`, `ML_LOADER=1`, `ML_GUEST=<hat>,<neck>` (a friend's
Mila walks into the casita, no second simulator), and `ML_LINK=visit|race` with
two linked simulators. On the board, `apps/mila_dev.txt` with `unlock` or
`level=roofs,8`.

## To measure on the board

- The log line above upright and lying down: a level while walking (a few %
  redrawn), a level whose camera moves (a whole frame: the render is the
  question, ~25 ms expected on two cores), the zoom onto Mila (whole frames,
  bilinear), the map scrolling (whole frames), and lying down the push of
  small rectangles through the PPA.
- PSRAM free in the casita with a hat and a scarf on: about 10.5 MB should
  be the app's (the memory line says what it holds).
- The casita's log line while Mila walks and runs: `render` (unpacking her
  frames from PSRAM is in it) and `unpacks`.
- Page flipping (2026-09-30): the log says `frames: bands copied into the
  panel's free buffer, flipped to` upright and `... turned ... by the PPA`
  lying down; the scene lines say `N flipped, 0 pushed`, and `whole after
  LVGL` stays at a few (one after the loader, a panel or a turn), not one
  a frame. By eye: no tearing when the camera glides or the map scrolls,
  and nothing of the system's (pull-downs, the switcher, a banner, a
  toast) hidden or flickering over the casita, a level or the map. Lying
  down, the PPA's bands of 9 rows (8 work, 4 hung it) with the render.
