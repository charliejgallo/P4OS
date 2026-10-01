# PIXEL ART — a drawing app with frames

Ported from AmoledOS. Twelve canvases ("lienzos") of 8x8, 16x16, 32x32 or
64x64 cells, painted from a 32-colour palette. A canvas holds up to 16 frames:
duplicate one, move a few cells, and the stack plays back as an animation and
goes out to the card as a looping GIF. A single frame goes out as a PNG.

```bash
# in the simulator (from the repo root)
cmake -S sim -B sim/build-pixel -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS=pixel && cmake --build sim/build-pixel -j8
cd sim && P4_SIM_SCRIPT="wait 800; open aos.pixel" ./build-pixel/p4os_sim

# the .so for the microSD
tools/build_apps.sh pixel                          # apps/pixel/build/pixel.so
```

## On the 5" screen

The canvas is 704 px across upright and 640 px lying down: multiples of 64,
so a cell is a whole number of pixels at every size (88, 44, 22 or 11 px
upright). Upright, top to bottom: the bar (back, previous frame, the frame
counter -tap it to play-, next frame, menu), the canvas, the tools, and the
32 colours as an 8x4 grid where the thumb reaches. Lying down the canvas is
on the left and the rest is a column on the right. Everything is touch; the
edge's back gesture is off (a stroke is a long drag), back is the arrow.

- **Tools:** pencil, fill, colour picker (takes the colour of the cell you
  tap and goes back to the pencil), undo, zoom. The chosen tool wears the
  current colour.
- **Strokes** are joined cell to cell, so a fast finger over a 64x64 canvas
  draws a line and not dots.
- **Undo** keeps the last 16 strokes, fills and clears, on whichever frame
  they were. Adding or deleting a frame clears it (the frames move under it).
- **Zoom:** two fingers pinch about the point between them and drag the
  view; one finger still paints. It stops when a cell reaches ~176 px. The
  first finger of a pinch has already painted by the time the second lands:
  a stroke younger than 350 ms is taken back. The magnifier button zooms 2x
  about the centre, or back to fit.
- **The menu** (a card over the canvas): play, duplicate frame, blank frame,
  speed (80 to 1000 ms per frame), export GIF, export PNG, grid on/off,
  delete frame, clear frame, delete canvas (asks twice).
- **The gallery:** 3x4 upright, 6x2 lying down. An empty slot asks for the
  size.

Everything is saved on its own: three seconds after the last change, when
leaving the editor, when the app goes to the background (it is kept alive,
`AOS_APP_FLAG_KEEP`, so the drawing, its frame and its zoom are still there),
and on exit. When the screen turns, `resize()` lays both screens out again
on the same document; the zoom goes back to fit.

## The samples

The first time the app opens with an empty folder it writes five canvases:
a **sunset over the sea with a black cat on a rock** (64x64, three frames of
shimmering water), the watch's black kitten walking (16x16, four frames), a
beating heart (8x8), a winking face and a checkerboard. A preference
(`px_seed`) remembers it, so deleting them is respected.

## Files

| Where | What |
|---|---|
| `/sdcard/pixel/lienzoN.pix` | the canvas, N = 1..12 |
| `/sdcard/pixel/lienzoN.gif` | every frame, looping, 512x512 (an 8x8: 256x256) |
| `/sdcard/pixel/lienzoN-F.png` | frame F, indexed PNG, the same size |

The `.pix` format is AmoledOS's (written up in `px_file.h`): twelve bytes of
header, the palette in RGB888, one byte per cell per frame. The palette is
the watch's byte for byte, so an 8x8 or 16x16 canvas moves between the two
as it is. A watch cannot open a 32x32 or 64x64 one (its reader knows 8 and
16) and shows it as "no se lee".

## Not here yet

- **Sending to another board.** The watch's ESP-NOW link code is kept, with
  its protocol unchanged (so it could still trade with a watch, 8x8 and 16x16
  only), but "Enviar" only appears when there is a paired partner and
  `aos_hal_link_start()` succeeds. On the P4 the radio is the C6 behind
  esp_hosted and ESP-NOW is not there yet, so the option does not show.
- **The portal page** is `#pixel` in `components/aos_portal/web/app.js`
  (ported from AmoledOS's `pixel.html`, with 12 slots and 32x32/64x64):
  gallery, editor, PNG/GIF downloads, `.pix` or picture uploads. The app
  notices a file changing on the card (every 3 s) and reloads it.

## How it draws

- **One canvas of 704x704 RGB565** (990 KB, PSRAM through `malloc()`) shown
  1:1. Painting a cell writes that square into the buffer and invalidates it
  alone; a frame change or a zoom step rewrites the buffer and invalidates
  once (the pinch redraws at most once per LVGL frame).
- The palette is 32 LVGL objects and not the watch's one canvas: that trick
  spared internal RAM, and on the P4 LVGL's objects live in PSRAM.
- The thumbnails are one canvas per slot (192 px, the frame badge painted in).
- **The encoders are the app's own** (the firmware has no PNG encoder and no
  GIF at all): stored-deflate PNG, real LZW GIF. The flood fill's stack
  (8 KB, for 4096 cells) and the GIF's row buffer are off the LVGL task's
  stack.
- **Size:** the `.so` is 36 KB, 112 symbols, all in the firmware's table.

## Tests on the Mac

```bash
cc -O1 -Wall -Wextra -Iapps/pixel/main apps/pixel/tools/px_harness.c apps/pixel/main/px_file.c apps/pixel/main/px_export.c -o /tmp/px_harness && /tmp/px_harness /tmp/px
cc -O1 -Iapps/pixel/main apps/pixel/tools/px_convert.c apps/pixel/main/px_file.c apps/pixel/main/px_export.c -o /tmp/px_convert && /tmp/px_convert lienzo1.pix sunset.gif 8
```

The harness round-trips a document, exercises the frame operations and the
fill (a 64x64 fill over the whole frame, the stack's worst case, included),
writes PNGs and GIFs (512x512 from a 64x64 too), decodes them with its own
readers and compares every pixel.

## Development switches (simulator only)

```
PX_DEMO=1    writes the samples if the folder is empty
PX_SLOT=n    opens canvas n straight away
PX_FRAME=f   ...on frame f
PX_MENU=1    ...with the menu open
PX_NEW=1     opens the size chooser
PX_LINK=1    tries to start the link without a paired partner
```
