# Monster Hop

A hop-by-hop action game on a grid, in the manner of the late-90s 3D
Frogger: Tommy, an eleven-year-old in a cap, crosses twenty-four levels full
of monsters to collect five keys in each and reach the exit. Every block,
prop, monster and outfit was modelled in Blender and rendered to sprites that
the game lights, colours and sorts by depth.

This is the AmoledOS watch game (`ESP32S3_AmoledOS/apps/monsterhop`) on the
P4's 5" screen, with the art of the desktop version
([charliejgallo/MonsterHop](https://github.com/charliejgallo/MonsterHop)).
The rules, the levels, the monsters, the shop and the saved progress are the
watch's; what changed is below.

```bash
# the art pack (once, and after the art or a level changes)
python3 apps/monsterhop/tools/pack_p4.py      # -> apps/monsterhop/build/monsterhop_p4.pak

# in the simulator
cp apps/monsterhop/build/monsterhop_p4.pak sim/sim_fs/apps/
cmake -S sim -B sim/build-monsterhop -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS=monsterhop
cmake --build sim/build-monsterhop -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.monsterhop" ./build-monsterhop/p4os_sim

# the .so for the card
tools/build_apps.sh monsterhop                # apps/monsterhop/build/monsterhop.so
```

On the card, both go in `/apps`: `monsterhop.so` and `monsterhop_p4.pak`.
The portal's upload (`PUT /api/fs/put?path=/apps/monsterhop_p4.pak`) streams
to the card with no size limit, so the 35 MB pack goes in one piece (about
30 s at the measured 1.1 MB/s); the game still reads a pack split in parts
(`monsterhop_p4.pak`, `.pak.1`, `.pak.2`...) if one is ever needed. Neither
file is in git.

## The art: the desktop's HD renders at 3/4

| | Watch | Desktop HD | P4 |
| --- | --- | --- | --- |
| Screen pixels per pixel of the projection (`MH_PX`) | 1 | 2 | **1.5** |
| +1 m along X / Y | (60, 14) / (20, -42) | (120, 28) / (40, -84) | (90, 21) / (30, -63) |
| The view, in watch pixels | 368 x 448 | 800 x 450 | 480 x 853 upright, 853 x 480 lying down |
| Pack | 17.7 MB | 74 MB | 35 MB (66 MB unpacked) |

`tools/pack_p4.py` reads the HD sprites from the MonsterHop repository's
`art/hd/` (the Blender scenes rendered with `MH_RES=2`) and brings each frame
down to 3/4: colour area-filtered with premultiplied alpha, the region ids
and the depth taken from each footprint's strongest source pixel (never
averaged), shadows and glows area-filtered. Each frame is first padded so
its anchor lands on a whole pixel, so every sprite keeps its place exactly.
The interface's pictures (map, logo, house, emblems) only exist at the
watch's size in AmoledOS's `assets/ui`; they are smoothed up to the P4's
720-pixel column. The levels are built by `tools/levels.py` (a copy of the
watch's, with its maps) into `build/levels`, and checked: a level with a
key out of reach stops the pack. No Blender run is needed.

Why 1.5 and not the HD's 2, measured in the simulator with the art of every
level loaded:

| Art in PSRAM | At 2 (HD) | At 1.5 (P4) |
| --- | --- | --- |
| an ordinary level | 11.5-14.7 MB | 6.7-8.8 MB |
| a boss's lair | 16.0-19.5 MB (the Brute alone 7.3) | 9.3-11.2 MB |

Next to that go the background cache (5 MB) and two or three frames (1.8 MB
each). At 2 a lair does not fit the 32 MB board next to the firmware; at
1.5 the worst is ~20 MB. 1.5 also keeps the projection's steps along the
ground whole pixels, which is what keeps neighbouring tiles seamless (only
a floor's height, 34.5 px, is not, and every block of a floor rounds the
same way). And the view is the desktop's: lying down, 853 x 480 watch
pixels against its 800 x 450.

The desktop's far scenery behind each zone is not in the pack (13.7 MB
each at its size); nor are its bloom and vignette, which are a pass over
every pixel of the frame. Its airborne bits (embers, fireflies, plankton,
sand...) are: ~90 small discs a frame (`mh_post.c`).

## How frames reach the screen

The worker (core 0, priority 3, below LVGL's 4) steps the game and renders
the whole frame at the screen's resolution, 1:1, in bands of 24 KB of
internal RAM (16 rows upright, 9 lying down), each copied once into a PSRAM
frame buffer in LVGL's byte order. The LVGL timer (8 ms, core 1) pushes the
newest finished frame with `aos_hal_display_blit_scaled(0, 0, w, h, fb, 1,
false)`: upright that is the DMA2D copy LVGL's own flush uses (4.4 ms a
screen on the board); lying down the HAL turns it through the PPA. While a
frame is pushed the worker draws the next into another buffer (two always,
a third when PSRAM has 2 MB to spare). Under the pause and the results the
same buffer is an LVGL canvas, so the panels compose over the last frame.

Why not the retro canvas, or half resolution scaled up: the art is not
pixel art, and at 1:1 it is what the desktop shows; scaling the frame x2
through the PPA would cost a quarter of the render but blur the renders the
pack took care to keep sharp.

Expected on the board (not measured yet; the simulator's Mac renders a
frame in 1-2 ms and says nothing): a frame is ~3.7 MB of PSRAM traffic for
the CPU (the cache read into the band, the band written out), which at the
bench's 146-165 MB/s is ~24 ms, plus the sprites: **~25 fps upright**, where
the watch played at 25.5. Lying down the PPA's 1:1 pass is the question:
the PPA measured 48 ms for a whole unrotated frame at x1, which would hold
**landscape near 20 fps** with the render hidden behind it. The app logs
what it gets every 2 s while playing (portal `/api/log`):

```
mhop: 24.8 fps 720x1280, render 31 ms, push 4400 us
```

## Playing: touch, or a USB gamepad

- **Swipe** to hop one cell that way (the most aligned of the grid's four
  ways); a **tap** hops up the screen. The fingers are read from the panel's
  own samples (`aos_hal_touch_frames`), both of them, so a flick shorter
  than a frame still hops.
- **The gold button** (bottom right) is the action: it pulls the lever,
  opens the chest or pushes the crate in front of Tommy, and with nothing
  to use it is a **super hop** over two cells or up two floors.
- **Pause**: the two bars in the top-left corner, or the system's back.
  The pause shows the level's plan: keys, checkpoints, monsters, the exit.
- **Arrows on screen** (Ajustes > Controles): four arrows along the grid's
  axes under the left thumb; held down, they keep hopping.
- The fly-over at a level's start ends with any touch.

The panels have a back button in their corner; a swipe to the right does
the same.

**A USB gamepad** (`aos_pad.h`) plays it all without touching the screen:

| | |
| --- | --- |
| D-pad / left stick | hop along the grid (up the screen is +Y, as the drawn arrows); held, it keeps hopping every 260 ms |
| A | the action (the gold button); on the panels, the outlined button |
| START | pause and resume; on the title, Jugar |
| B | back on every panel (closes the map's level card); the title has none |
| L / R | the shop's tabs, the map's zones, the race's level |

On the panels the d-pad moves a white outline to the nearest button that
way (`aos_pad_menu.h`; it only shows once the pad is used), A clicks it;
the trophies' list scrolls instead. Lying down, the level under the
outline on the map is the card's, and A plays it. Any of A, B or START
skips the fly-over.

## Both orientations

Standing up everything is a 720-pixel column (the map scrolls under the
coins bar; a level's card pops over it). Lying down it is the desktop's
arrangement: the title's map on the left, the map in the middle with the
level's card on the left and the six zones on the right, the house's picture
beside its buttons, Tommy big beside the shop's list, the pause's plan beside
its counts. The screen can turn at any moment: a level being played pauses,
the panels are laid out again, and the worker makes the frames and the
background cache again in the new shape before the game carries on (a race
cannot pause: it keeps running).

## Saved progress

Stars, best times, coins, the wardrobe, the album, trophies, stats and the
settings are the watch's preferences, under the same keys (`mh_prog.c`), so
they survive closing the app and a new pack. The on-screen arrows are one
more, `mh_pad`.

## The key race

It rides on the ESP-NOW link (`aos_hal_link_*`), which the P4 does not have
yet. The house asks `aos_hal_link_start()` once; when it fails, *Jugar con un
amigo* is not shown at all, and nothing else waits on the link. The protocol
is the watch's, untouched, so a P4 could race a watch once it has the link.
In the simulator the link works (UDP), so the button shows there.

## Files

| File | What |
| --- | --- |
| `main/mh_p4.h` | the build's switches (`MH_P4`, `MH_VIEW_RUNTIME`): in a header because the simulator compiles every app with the same flags |
| `main/monsterhop.c` | life cycle, the worker and its jobs (`JOB_FIT` when the screen turns), the push, touch, the states |
| `main/mh_ui.c` | the LVGL panels, laid out for either orientation (`mh_ui_layout`) |
| `main/mh_hud.c` | the HUD in the frame, the on-screen controls included |
| `main/mh_world.c` | the background cache: a ring the view's shape plus a block each way |
| `main/mh_render.c` | the frame in bands: the cache, then the sprites depth-tested per pixel against it |
| `main/mh_post.c` | the zones' airborne bits |
| the rest | the watch's, unchanged but for the art's scale being a float (`mh_px`) |
| `tools/pack_p4.py` | the pack, from the HD art |
| `tools/levels.py`, `tools/levels/` | the levels as text maps, and their check |

## Simulator switches

The watch's still work: `MH_LEVEL=<0..23>|test`, `MH_DIFF`, `MH_UNLOCK=1`,
`MH_COINS`, `MH_TRAIL=1..4`, `MH_START=x,y`, `MH_TRACE=1`,
`MH_SCREEN=map|house|wardrobe|shop|album|trophies|stats|settings|result`,
and `MH_RACE=<level>` with two simulators linked. New: `MH_ARTLOG=1` prints
every sheet loaded with its size. On the board, `apps/monsterhop_dev.txt`
with `unlock` or `level=N` does what it did on the watch.

## Pending

- Measure on the board: the fps upright and lying down, and PSRAM free in a
  boss's lair lying down (the worst: ~11 MB of art, 5 MB of cache, two
  frames). The log line above has what is needed.
- The far scenery behind each zone, at a size the P4 can keep (JPEG through
  the HAL's decoder would be the way).
- The interface's pictures re-rendered at the P4's size in Blender
  (`tools/blender/ui.py` in AmoledOS) instead of smoothed up from the watch's.
- New strings (the settings' controls, "No hay memoria para girar la
  pantalla") are not in the language catalogs yet.
