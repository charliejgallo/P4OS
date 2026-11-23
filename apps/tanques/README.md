# Tanques (the sample)

A turn-based tank battle with chubby toy tanks, in Monster Hop's style. **This
is the sample**, made to settle the look and the feel before the real art and
the rest of the game (the hangar, six tanks, twenty weapons, the story): one
small map, one tank model in three paints, two weapons and a whole turn in
both views, against the board or between two people passing it round.

```bash
# the art (Blender 3.3, headless: ~3 minutes on the M2), then the pack
B=/Applications/Blender.app/Contents/MacOS/Blender
(cd apps/tanques/tools/blender && $B -b -P tanques_art.py)   # -> build/art
python3 apps/tanques/tools/pack.py                          # -> build/tanques_p4.pak (4.1 MB)

# in the simulator
mkdir -p sim/sim_fs/apps && cp apps/tanques/build/tanques_p4.pak sim/sim_fs/apps/
cmake -S sim -B sim/build-tanques -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS="hello_app;tanques"
cmake --build sim/build-tanques -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.tanques" ./build-tanques/p4os_sim

# the .so for the card
tools/build_apps.sh tanques                 # apps/tanques/build/tanques.so (68 KB)
```

On the card both go in `/apps`. Neither is in git.

## The turn

1. **Drive** (the battle view, from above): the tiles show where the action
   points reach, blue where both weapons are still affordable after the
   drive, yellow where only the cannon is, orange where no shot is left. Touch
   a tile and the tank drives there; drag to look around. Uphill costs more,
   the steep, the stones, the trees and the other tank are in the way.
   Driving can be done in stretches while points last.
2. **Aim** ("Apuntar"): the gunner's view from the turret. Drag sideways to
   turn the turret, up and down to raise the barrel. The cannon (3 AP) shows
   the first stretch of its path; the mortar (5 AP) lobs and shows all of it
   and the ring where it lands. "¡Fuego!" ends the turn.
3. The flight, the blast, the crater, the damage and the trees and stones it
   breaks are watched from above; then the other tank plays.

Back (the system's gesture) leaves the gunner's view, or goes to the title.

## The 3D: what was measured and what was chosen

| | how |
| --- | --- |
| the ground | a height map (36 x 36 m, a vertex every 0.5 m) drawn by the app, in both views, with its colours lit per vertex (the sun, the hills' shadows, the hollows) and a grain texture. A crater lowers heights and lights again only what it changed |
| the battle view | the fixed orthographic camera of the sprites (yaw 20, looking down 55 degrees, 32 px a metre): the whole map is drawn **once** into a picture with its depth, and a frame is a window of it with the sprites over it, each pixel tested against that depth |
| tanks, trees, stones | **Blender sprites** (`tools/blender/tanques_art.py`, built by code): hull and turret apart, 32 yaws each, from above and from 8 degrees for the first person, with four planes (shade, coverage, region, depth). The app paints the regions itself, so one model comes in any colour |
| the first person | drawn whole every frame at **half the screen each way** (the ground's triangles, the things as sprites scaled by distance, the turret and barrel rendered for the gunner's eye) and doubled |

Measured with `tools/bench.c`, which runs the same code on the Mac next to
Visor 3D's rasteriser, whose times on the board are known (mila_hd: 105 ms
on one core; mila, 16 000 triangles at 288 x 512: ~35 ms on two). On this Mac
those take 1.4-1.8 ms and 1.2 ms: one board core is ~60-75 Mac cores' worth
of this work, two cores ~30. The board column is that ratio, **not a
measurement**; the parts bound by PSRAM (copies, the doubling) are estimated
from its bandwidth, which the ratio does not see.

| | Mac (one core) | board, expected |
| --- | --- | --- |
| battle view, a frame (window + ~45 sprites) | 0.34-0.48 ms | ~25-35 ms (most of it the 1.8 MB window copy) |
| ... with the reach tiles and the path | 0.67-0.89 ms | +15-25 ms (only redrawn when something changes) |
| ... during a blast (~55 puffs and clods) | 0.8-1.5 ms | ~35-55 ms on two cores |
| the whole map into the world picture (at the start) | 7-9 ms | ~0.5 s |
| a mortar's crater drawn again | 3-4 ms | ~0.2-0.3 s, a hitch at the landing |
| first person, a frame (~6 000 triangles a half, ~90 sprites) | 1.8-2.6 ms | ~60-80 ms on two cores: **12-16 fps** while dragging |
| ... of which the doubling | 0.8-1.0 ms | ~15 ms (the PPA could do it) |
| **the tank as real-time triangles** instead of sprites, 3 100 / 9 900 / 39 800 triangles, at 360 x 640 | 0.2-2 ms | **13-120 ms one core per tank** |

So: sprites for everything with a shape, triangles only for the ground. A
low-poly tank in real time would cost each frame what the whole first-person
view does, and look flat beside the sprites. On top of the render, LVGL
pushes the canvas (a full frame through its own flush), not measured here.

What the log says on the board, every 3 s while something moves:

```
tanques: battle view 24.8 fps 720x1280, render 31 ms (world again 0), 44 things
tanques: first person 13.0 fps 720x1280, render 70 ms (ground 52, doubling 15), 11812 triangles
```

### Memory (PSRAM, counted by `tq_malloc`)

| | |
| --- | --- |
| the pack (rows packed: only each row's covered run) | 4.0 MB |
| the first person's smaller copies of the far sprites | 0.6 MB |
| the world picture and its depth, 1572 x 1478 | 9.3 MB |
| the first person's half-size frame and depth | 0.9 MB |
| two screen frames for the canvas | 3.6 MB |
| **all** | **18.5 MB** (Monster Hop's worst level: ~20) |

A bigger map needs the world as a cache around the view (Monster Hop's ring)
or an 8-bit depth: the picture grows with the map's area.

## Files

| File | What |
| --- | --- |
| `main/tanques.c` | life cycle, the worker, the HUD, the turn's states, touch |
| `main/tq_game.c` | the map's things, the reach (Dijkstra over 1 m cells), paths, shells, blasts, the board's tank |
| `main/tq_terrain.c` | the height map, its light, craters |
| `main/tq_scene.c` | the two views |
| `main/tq_gfx.c` | triangles, sprites, puffs, the doubling |
| `main/tq_art.c` | the pack, the paints |
| `main/tq_view.h` | the cameras (the Blender script has the same numbers) |
| `tools/blender/tanques_art.py` | every model and render |
| `tools/pack.py` | the pack, and contact sheets of the renders |
| `tools/bench.c` | the renderers on the Mac, with the calibration |

## Simulator switches

`TQ_START=1|2` straight into a battle (against the board, two players),
`TQ_AIM=1` then into the gunner's view, `TQ_AUTO=1` the board plays both.

## Not in the sample

The joystick (`aos_pad.h`), the language catalogs, the Store's card, sound
beyond a beep, the hangar, more tanks and weapons, story and free modes,
other maps. Measured on the board: nothing yet.
