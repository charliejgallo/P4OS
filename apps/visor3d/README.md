# Visor 3D

Turn a 3D model with one finger, zoom, move and twist it with two, on the
whole screen, upright or lying down. Ported from AmoledOS (v0.6.0), where it
drew into 368x352 of the watch; here the model fills 720x1280 or 1280x720.

```bash
# in the simulator
cmake -S sim -B sim/build-visor3d -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS=visor3d
cmake --build sim/build-visor3d -j8
cd sim && P4_SIM_SCRIPT="wait 800; open demo.visor3d" ./build-visor3d/p4os_sim

# the .so for the card
tools/build_apps.sh visor3d                   # apps/visor3d/build/visor3d.so -> /apps on the card
```

## The models

They go in the card's **`3d/`** folder (`sim/sim_fs/3d` in the simulator):
**STL** (binary or ASCII) straight from any CAD program or slicer, and
**M3D**, the format with colours that AmoledOS's portal page `/3d` makes out
of STL, OBJ and GLB. Copy them with the card in the computer, or upload them
from the portal (Archivos, or `curl -T mila.m3d 'http://<board>/api/fs/put?path=/3d/mila.m3d'`).

`models/` has four, made from the AmoledOS games' own Blender builders:

| file | what | triangles |
|---|---|---|
| `mila.m3d` | Mila, the black kitten, sitting with her bow and bell | 15 690 |
| `tommy.m3d` | Monster Hop's hero, with his red cap | 15 904 |
| `zombie.m3d` | Monster Hop's zombie | 15 966 |
| `muscle.m3d` | Turbo's muscle car, navy with white stripes | 15 940 |
| `mila_hd.m3d` | Mila as the game builds her, not reduced | 102 884 |
| `tommy_hd.m3d` | Tommy with every mesh subdivided once (Catmull-Clark) | 106 184 |

The Blender scripts that make them (`mila_obj.py`, `models_obj.py`) stay in
AmoledOS's `apps/visor3d/tools/`: they load the games' builders, which live
in that repository. `tools/samples.py` here writes four test models with no
dependencies: a torus knot, an ASCII gear, a bumpy sphere of 80 000
triangles (over the budget: the viewer reduces it while loading) and a
low-poly planet.

The HD pair comes straight from those scripts' OBJ, through two tools here:
`tools/tommy_hd.py` (Blender: `models_obj.py`'s Tommy with a subdivision on
each mesh, the face and the emblem only cut, since smoothing sinks those
thin shells into the head and the shirt) and `tools/obj2m3d.py` (OBJ + MTL to
M3D, not reduced). The converter turns every loose piece to face outwards:
the viewer skips the back faces of a closed model, and Tommy's eyes and
mouth come wound inwards.

Measured on the board (2026-09-29), turning, with the render split across
the two cores (`aos_hal_worker_split`): the ~16 000-triangle samples at
~28 fps at 288x512; `mila_hd` 67 ms a frame at 180x320 (~15 fps: vertices
16 ms, triangles 49 ms; on one core it was 105 ms), `tommy_hd` ~13 fps;
loading either takes ~850 ms. Still, the full 720x1280 frame of `mila_hd`
takes ~120 ms.

## Using it

**The grid**: one card per model, with a picture of it drawn by the app
itself, its triangles and its size. The pictures come one by one while the
grid is up (the small files first: a big STL takes a while to weld). Tap a
card to open it.

**The model**, on the whole screen:

| | |
|---|---|
| one finger | turns it; let go with a flick and it keeps turning, slowing down |
| two fingers | zoom about the point between them, move it, and twist it (after a few degrees, so a plain pinch does not turn it askew) |
| tap | the bars on or off; a spinning model stops |
| double tap | back to the first view, turntable off |
| long press | solid or wireframe |

The bars are up while the model loads (the progress is on top) and go away
as soon as a finger turns or zooms it. On top: back, the name with the
triangles (and, while it moves, the frames per second and the size it is
drawn at), and **↻**, the first view again. Below: the **background** (dark,
grey or light, remembered; grey is the default, where both the black cat and
the white stripes read), **solid / wireframe**, and **Girar**, the turntable.

## How it is built

- **The worker draws, LVGL shows.** The model is drawn by the app's worker
  (`aos_hal_worker_start`: core 0, priority 3, stack in PSRAM), never in
  LVGL's task (core 1). LVGL's 10 ms timer moves the view and puts the newest
  of three finished frames on the screen.
- **To the screen by the HAL's blit**, little-endian (the rasteriser writes
  LVGL's byte order): `aos_hal_display_blit_scaled()` for whole scales,
  `aos_hal_display_blit_fit()` for 1.25 and 2.5. The PPA scales, turns for
  the orientation and writes the framebuffer in one pass; 1:1 upright goes by
  the DMA2D copy. The blit returns with the picture in the framebuffer.
- **The resolution follows the cost.** While anything moves the frame is drawn
  at one of five sizes, the screen divided by 1, 1.25, 2, 2.5 or 4 (each
  divides 720 and 1280 whole). It starts at **360x640 doubled** and, every six
  moving frames, goes a step coarser if a frame costs over 40 ms (render or
  blit, whichever is slower: they run on two cores at once), or a step finer
  if the finer size would cost under 26 ms, guessed from the pixels and from
  that size's own blit when it has been measured. Once the view has been
  still for 160 ms, one frame at full size, 720x1280 or 1280x720: that is the
  picture you look at.
- **Nothing of LVGL's over the picture.** The bars are opaque, and while they
  are up only the rows between them are blitted (their heights are multiples
  of 20 so those rows are whole at every size). When LVGL paints where the
  picture is (the bars going, the viewer appearing, a panel closing) the frame
  on screen goes up again right after LVGL's refresh (`LV_EVENT_REFR_READY`).
  While the control centre, the notifications, the switcher or the home
  gesture is over the app, nothing is blitted.
- **Clearing only what was drawn.** A 720x1280 frame and its z-buffer are
  3.7 MB of PSRAM, which the panel's refresh reads too (docs/MEMORY.md). Each
  slot and the z-buffer remember the box the last frame drew in them, and
  only that is cleared.
- **The twist** comes from `aos_touch_points()`: the angle of the line between
  the two fingers against the last sample with the same two. `aos_gesture`
  gives no angle (the watch's chip swapped the fingers' X on a diagonal; the
  GT911 does not). The twist turns the picture about the point between the
  fingers, and one-finger turns are read in the twisted frame, so a finger
  along the model's own horizontal still spins it about its own up.
- **Loading** (`v3_mesh.c`, unchanged from the watch) welds an STL's loose
  triangles and reduces anything over 24 000 triangles, a few passes if
  needed, with its progress on top; leaving mid-load cancels it at once.
- **Memory:** three full-screen slots and a z-buffer, 7.4 MB of PSRAM, plus
  the model and 140 KB per grid picture. In the simulator the blit says no
  and the frames go through an LVGL canvas at the same place.

## Measured in the simulator (2026-09-29, a Mac)

Mila, 15 690 triangles: about 3 ms a frame at 360x640 and 4-6 ms at
720x1280, so there the size climbs to the full screen while moving. The
numbers say nothing about the board, whose worker core (360 MHz) draws what
the S3 did (~50 ms for a zombie at 184x176) a few times faster, on seven to
twenty-eight times the pixels.

## What the board must measure

- The log (`/api/log`) says every 5 s while the model moves:
  `visor3d: mila.m3d, 15690 tri: N ms a frame moving at WxH (N frames, blit N us),
  N ms at full (N) | per frame: clear N + vertices N + triangles N us`, and a
  line whenever the moving size changes. Which size each model settles at,
  and whether it stays around 25-30 fps.
- The blit at each size: 1:1 upright (DMA2D, ~4.4 ms measured by Video),
  1:1 lying down (the PPA at x1, 48 ms: only the still frame uses it),
  x2 (10.6 ms in Video), and the untried 1.25 and 2.5 by `blit_fit`.
- The still frame at 720x1280: how long after the finger stops the sharp
  picture arrives.
- Screen flashes and `can't fetch data from external memory fast enough`
  while turning a model at full size (PSRAM bandwidth, docs/MEMORY.md).
- The twist on the GT911: the threshold (7 degrees) and that a pinch
  without twisting does not turn the model askew.
- V3_BUDGET went from the watch's 24 000 to 150 000 (the HD samples).
- PSRAM free after closing the app against before opening it.

## The portal page

`#3d` in the portal (`components/aos_portal/web/app.js`, ported from
AmoledOS's `modelos.html`) lists `/3d`, previews a model with the board's
light, and turns STL, OBJ (+MTL) and GLB into M3D in the browser: colours
kept, reduced only past `V3_BUDGET` and 131 072 vertices, and every loose
piece turned outwards as `tools/obj2m3d.py` does (checked against it: same
winding on every triangle of a test OBJ). A button opens the viewer.

## Not here

- The simulator's scripted `pinch` moves the fingers along a horizontal line,
  so the twist can only be tried by hand there (Option + mouse mirrors a
  second finger around the screen's centre).

## Formats

- **STL**: binary or ASCII, Z up as CAD writes it (turned Y up on loading).
- **M3D**: `"M3D1"`, vertex and triangle counts, flags, float vertices,
  uint32 triangles and, with flag 1, a RGB565 colour per face; Y up,
  little-endian (`v3_mesh.h`).
