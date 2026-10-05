# DIBUJO — drawing and design

A drawing program for the 5" screen, used with a finger, a USB mouse, a
joystick, and a keyboard for shortcuts. A drawing has up to eight layers and
is the size of the screen, an A4 at the screen's resolution, or a square.
Brushes, shapes, text, selections, a grid with guides and a magnet, rulers,
mirror symmetry, and deep undo. Drawings go out as PNG, and pictures come in
as a background to trace over.

```bash
# in the simulator (from the repo root)
cmake -S sim -B sim/build-dibujo -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS="hello_app;dibujo" && cmake --build sim/build-dibujo -j8
cd sim && P4_SIM_SCRIPT="wait 800; open aos.dibujo" ./build-dibujo/p4os_sim

# the .so for the microSD
tools/build_apps.sh dibujo                         # apps/dibujo/build/dibujo.so
```

## On the screen

Upright, top to bottom: the bar (back, undo, redo, the zoom, the two
colours, layers, menu), the canvas, the options of the tool, and the eight
tools. Lying down, the tools are a column on the left and the panels take
the whole height beside them. The edge's back gesture is off (a stroke is a
long drag), so back is the arrow.

| Tool | What it does | Options |
|---|---|---|
| Brush | pencil, soft brush, airbrush, translucent marker (tap it again for the four) | size 1-300 px, opacity; smoothing and symmetry in its panel |
| Eraser | hard, to transparent | size, opacity |
| Shapes | line, rectangle, ellipse, polygon, arrow (tap it again for the five) | fill on/off, outline width; rounded corners and outline on/off in its panel |
| Text | tap where it goes; the system's fonts, 16 to 64 px, emoji too | — |
| Fill | flood fill, growing one pixel over a line's soft edge | tolerance; looks at the layer or at everything |
| Eyedropper | takes a colour, then back to the tool before it | the layer or everything |
| Selection | a rectangle: drag inside it to move it, or duplicate, copy, paste, delete | select all, paste |
| Hand | pan with one finger | fit, 100 %, zoom out and in |

- **Shapes, texts, moved selections and imported pictures float** until they
  are fixed (✓, Enter, or a tap outside): the corners scale, the circle above
  turns, the body moves, a line has its two ends. With the magnet on, they
  snap to the grid, the guides, the edges and the middle, and a turn goes in
  15° steps. ✕ (or Esc) drops it; a moved selection goes back where it was.
- **Colour**: a stroke colour and a fill colour (the two squares in the
  bar). The panel has a saturation/brightness square and a hue bar, the
  colours used last, and four palettes (`paletas.txt`, so the portal or a
  person can edit them): "+" adds the current colour, a long press removes one.
- **Layers**: add, duplicate, move up and down, merge down, delete (asks
  twice), hide, opacity; the background (white, black, transparent, or the
  current colour) under all of them.
- **Menu**: save, export PNG, import a picture, grid (8 to 128 px), rulers
  and guides (drag one out of a ruler, back onto it to delete it), magnet,
  symmetry (side to side, top and bottom, both), fit, back to the gallery.
- **Two fingers** zoom and pan. The first finger of a pinch has already
  painted when the second lands: a stroke younger than 350 ms is taken back.
- **The gallery**: the drawings, newest first. "Nuevo" asks for the size; a
  long press offers export, duplicate and delete.

Everything is saved on its own: when leaving a drawing, when the app goes to
the background (it is kept alive, `AOS_APP_FLAG_KEEP`), on exit, and twenty
seconds after the last change. When the screen turns, `resize()` lays the
editor out again on the same drawing; an open panel opens again.

## Mouse, keyboard, joystick

- **Mouse**: the system's arrow is a finger (`aos_hwmouse.c`). The wheel
  zooms about the arrow. The system turns the wheel into a scroll of what is
  under the arrow; the canvas's touch layer is made scrollable with nothing
  to show, and the scroll is caught as it begins (`LV_EVENT_SCROLL_BEGIN`
  hands over the animation: its values give the direction and are made
  equal, so nothing moves). See `wheel_cb` in `dib_editor.c`.
- **Keyboard** (`aos_ui_hwkbd_handler`, while the editor is up and no text
  is being typed): B brush, E eraser, U shapes (again: the next shape), T
  text, G fill, I eyedropper, M selection, H or space hand; Ctrl+Z / Ctrl+Y
  (or Ctrl+Shift+Z) undo and redo, Ctrl+S save, Ctrl+E export, Ctrl+C / X / V
  copy, cut, paste, Ctrl+A / D select all and none; [ ] size, + - 0 1 zoom,
  fit and 100 %; X swaps the colours; Enter fixes, Esc drops, Delete deletes;
  the arrows nudge a floating object (Shift: 10 px) or pan.
- **Joystick** (`aos_pad.h`, `docs/GAMEPAD.md`): the stick or the D-pad moves
  a cursor the size of the brush (a single press of the D-pad is one pixel of
  the drawing); A is the finger; B undoes; L and R change the brush's size
  (zoom with the other tools); B held with L or R zooms; START walks the bars'
  buttons with an outline (`aos_pad_menu.h`), and a panel's buttons while
  one is open; B or START go back to the canvas.

## How it works

The engine (`dib_doc`, `dib_paint`, `dib_raster`, `dib_png`) has no LVGL and
no HAL: it is checked on the Mac (below) before the board sees it.

- **Layers are tiles** of 64x64 ARGB8888, allocated when first painted: an
  empty layer costs nothing, a full 1280x1024 one 5 MB.
- **History by tiles**: the first time a step touches a tile, the tile is
  copied into the step. Undo and redo swap those copies with the layer's, so
  the same function goes both ways and nothing is copied twice. Layer
  operations (add, delete, move, merge, opacity, background) are steps too.
  Up to 96 steps, fewer if memory runs short: layers, history and the
  floating layer share one budget taken from the free PSRAM at start (8 MB
  are left to the rest, 6 to 20 MB for the app), and the oldest steps go
  first.
- **Brushes are dabs** along the smoothed path. A dab raises the stroke's
  own mask, and the pixel is recomputed from the tile as it was before the
  stroke (the history's copy) plus the colour at mask × opacity: opacity
  caps the stroke, the marker keeps the mask's maximum so it never darkens
  itself, and the airbrush's low flow builds up while the finger rests.
- **The screen**: the drawing's RGB565 copy at its own resolution is
  recomposed only where something changed; the canvas window samples it at
  the zoom (averaging 2x2 when shrinking) into one `lv_canvas`. The grid,
  guides and symmetry axis are painted into the window as it is sampled;
  handles, the selection and the pad's cursor are LVGL objects on top.
- **Shapes** are filled by an antialiased scanline rasterizer (non-zero
  winding, four sub-rows, exact coverage across). Outlines are geometry: a
  ring for rectangles and ellipses, a quad per segment plus round joins for
  lines and polygons. A floating shape is drawn again from its box each time
  it moves, so nothing blurs until it is fixed; texts and selections are
  bitmaps drawn through the box's inverse transform, bilinear (pixel for
  pixel while unturned and unscaled).
- **Text** is drawn by LVGL into an off-screen ARGB canvas with the system's
  fonts, after `aos_text_safe()` (emoji in colour).
- **PNG** is the app's own writer: RGBA8888, the filter chosen per row,
  deflate with LZ77 and the fixed Huffman tables, rows asked for one at a
  time. Exporting and decoding an imported picture (`aos_hal_image_decode`)
  run on the worker while a card says so.
- **Icons**: every tool's icon is drawn at start with the same rasterizer.

Measured in the simulator: the example (720x1280, three layers) loads in
5 ms and its .dib is 267 KB; its PNG 126 KB. On the board, measure with the
log lines (`saved ... in N ms`, `loaded in N ms`).

## Files

| Where | What |
|---|---|
| `/sdcard/dibujo/dibujoN.dib` | a drawing: layers, guides, a 160 px preview (format in `dib_doc.h`) |
| `/sdcard/dibujo/dibujoN.png` | its export, RGBA |
| `/sdcard/dibujo/paletas.txt` | the palettes and the recent colours, one line each |
| `/sdcard/dibujo/importar/` | pictures to import (JPEG, PNG, BMP); the photos folder is offered too |

The first visit writes an example (a sunset with a house, drawn with the
app's own tools); a preference (`dib_seed`) remembers it.

## The portal page

`web/dibujo.js` (`#dibujo`): the gallery with the previews read from the
files' first bytes (a `Range` request), a drawing put together in the
browser from its layers, with its layers listed and hideable for the PNG
it downloads; the .dib itself; the PNGs the app exported; and pictures to
import, uploaded as baseline JPEG (or PNG with transparency) no bigger than
1280 px, which is what the board's decoder takes.

## Tests on the Mac

```bash
cc -O1 -Wall -Wextra -DDIB_HOST -Iapps/dibujo/main apps/dibujo/tools/dib_harness.c \
   apps/dibujo/main/dib_doc.c apps/dibujo/main/dib_paint.c apps/dibujo/main/dib_raster.c \
   apps/dibujo/main/dib_png.c -lm -o /tmp/dib_harness && /tmp/dib_harness /tmp/dib
python3 apps/dibujo/tools/dib_pngcheck.py /tmp/dib
```

Every brush (undo and redo give back exactly the pixels before and after),
the marker over itself, a cancelled stroke, every shape (a ring's hole is
empty), a turned bitmap and one pixel for pixel, the fill (it does not leak
out of a rectangle), six layer operations undone and redone, a moved
selection, a .dib saved and loaded pixel for pixel, the PNG decoded by
Python's zlib to the very rows the engine composes (with an opaque and a
transparent background), and the memory budget under a heavy session.

## Development switches (simulator only)

```
DIB_DEMO=1      writes the example if the folder has no drawings
DIB_OPEN=n      opens drawing n of the gallery (1 = the newest)
DIB_NEW=WxH     a new drawing of that size
DIB_SIZES=1     the size chooser;   DIB_SHEET=n   drawing n's actions
DIB_TOOL=t      that tool (0..7);   DIB_PANEL=p   that panel (1 brushes,
                2 colour, 3 layers, 4 menu, 5 shapes)
DIB_KEYS=...    keys typed into the editor, ',' between them (^z = Ctrl+Z)
DIB_WHEEL=n     the mouse wheel, n notches, through the system's own path
```
