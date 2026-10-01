# Chatarra's 2x art

On P4OS the world is drawn at **twice the resolution of the world**: a 12-unit
cell is a **24x24 tile**, a 15x24-unit character is a **30x48 drawing**. The game
logic does not change at all: cells, rooms, positions, doors, solid rows and
collisions are the same numbers. Only the art has twice the pixels in each
direction, drawn anew (with code, see section 2), never upscaled.

This file is the contract for everybody drawing assets: the format, the
palette, the style, and the list of what is done.

## 1. How the engine draws it

- **Zoom levels are 2, 4 and 6** canvas pixels per world unit (`ZOOM_MIN`,
  `ZOOM_MAX`, `ZOOM_PASO` in `chatarra.h`). An art pixel is `zoom / 2` canvas
  pixels, so **1, 2 or 3 canvas pixels** (2, 4 or 6 on the glass): always a
  whole number, which is why the zoom is even. Standing up and lying down the
  natural zoom is 2: the whole room, every art pixel shown.
- **2x assets are blitted 1:1** (`ch_blit2()`, `ch_blit2m()`, `ch_blit2_px()` in
  `main/ch_pixel.h`): no upscaler, no automatic bevel, no grain. All the shading
  is in the drawing.
- **The anchor does not move.** A 2x sprite is drawn at the same logical x, y as
  its 1x one and covers the same logical box, twice as wide and twice as tall,
  exactly (`tools/arte2x.py` refuses anything else).
- **Fallback.** Anything without a 2x drawing is still drawn from its 1x art
  through the EPX upscaler (`ch_blit()`), so the game is always complete while
  assets are being redrawn. In `ch_world.c`, `hd()` finds the 2x array of a 1x
  one; `blit_hd()` and `blit_tile_hd()` use it or fall back.
- **Tiles wrap**: a tile continues into its own copy on every side. Water and
  lava are animated by rolling their rows downwards (2 rows a step at 2x), so
  their patterns are horizontal and wrap vertically.
- **Fringes** (`BR_*`) are drawn for the NORTH edge only; the engine turns them
  for the other three sides. Solid top rows, tongues hanging down, transparent
  below.
- **In the UI** (menus, combat) a UI unit is 2 canvas pixels, so a 2x icon
  drawn with `ch_blit2m(b, x, y, rows, n, esc)` has `esc` canvas pixels per art
  pixel, and covers exactly what the 1x icon covered at the same `esc`.

## 2. The format

An ASCII map, one palette character per pixel, `.` transparent:

```c
static const char *const HD_PX_PASTO[24] = {
    "eeee2eeeeEeeeee1eeeeeeee",   /* 24 characters */
    /* ... 24 rows */
};
```

- **Width = 2 x the 1x width, height = 2 x the 1x height.** All rows the same
  length.
- **Names**: `HD_<1x name>` for a replacement (`HD_SP_CASA` replaces `SP_CASA`);
  a 2x-only asset gets its own name (`HD_SP_BARRERA`).
- **`?` is a colour** (snow's shadow) and `??` followed by some characters is a
  C trigraph: in C strings write it `\?` (the generator always does). `\` and
  `"` are not colours.
- `#` and `+` are NOT palette colours: in `ch_ui.c`'s 1x icons they mean "body"
  and "detail". Do not use them in 2x art.

**Where they live**

| Assets | 1x source | 2x source |
| --- | --- | --- |
| tiles, fringes, decorations, furniture, animals, people, chest, sign, booth, stalls | `main/ch_world.c` | `main/ch_world2x.inc` (arrays) and `main/ch_world2x_tabla.inc` (the lookup), **generated** by `tools/arte2x.py`; the drawings are in `tools/arte2x_suelos.py` and `tools/arte2x_cosas.py`, with the pencil in `tools/lienzo.py` |
| robots and parts | `main/ch_parts.c`, drawn by code (kept for scale 1) | `main/ch_parts2x.inc`, **generated** by `tools/arte2x_robots.py`: the 64 parts as colour-slot sprites (section 6); the map figure is drawn at 2x in code (`ch_mini_draw`) |
| the font | `main/ch_pixel.c`, 5x7 | `main/ch_fuente2x.inc`, 10x14, **generated** by `tools/fuente2x.py` (the glyphs are typed there by hand) |
| arenas, projectiles, impacts, particles | `main/ch_battle.c`, code | the same code, at the canvas's resolution; the arena floor is the zone's 2x tile in perspective |
| glints, weather, the alert mark, arrows, hatch | `main/ch_map.c`, `main/ch_world.c` | small 2x ASCII sprites next to the code that draws them (`HD_BRILLO`, `HD_COPO`, `HD_ALERTA`...), the arrow built in art pixels, `HD_SP_ESCOTILLA` |
| menu and item icons | `main/ch_ui.c`, `ICONOS[]` 12x12 | `main/ch_ui2x.inc` (`HD_IC_*` and the `ICONOS_HD[]` lookup), **generated** by `tools/arte2x_iconos.py` with the same pencil; `ch_ui_icono()` blits them with `ch_blit2m(..., esc)` and falls back to the 1x through the upscaler for any icon without one |
| rooms (which tile goes where) | `main/ch_zonas.c` | no art: letters of the `TILES` table |

To redraw something from `ch_world.c`, change its function in the script and
run `python3 tools/arte2x.py` (from `apps/chatarra/`).
`python3 tools/arte2x.py --hoja sheet.png [regex]` draws a contact sheet with
the 1x beside the 2x (tiles repeated 2x2, to check the seams), and
`python3 tools/hoja.py <file.c> sheet.png [zoom] [regex]` shows any file's
ASCII art. If you prefer to draw an asset character by character, take it out
of the script's tables and put the array in a file of your own: the next run
must not draw over your work.

## 3. The palette

The watch's 49 colours plus ramps for the 2x art (`PALETA` in
`main/ch_pixel.c`). Each ramp is listed light to dark:

| Material | Ramp | Notes |
| --- | --- | --- |
| grass, foliage | `1 e 2 E 3 f F 4` | `z` a yellow-green tip |
| dirt | `6 h H 5` | `h` is also skin's mid tone |
| stone | `7 i 8 I 9` | |
| wood | `0 j J !` | `!` is also the warm outline |
| water | `$ l % L &` | `$` foam, `=` pale blue |
| sand | `( q Q )` | |
| rust | `* u U ,` | |
| brick, roof | `- a A /` | `/` mortar |
| skin | `: h ;` | cheeks `_` |
| metal | `w G < g > D d x K k` | `k` is near black, the only "black" |
| snow | `w G ?` | the ground never pure white: `w` is for highlights |
| ice | `[ c C ]` | |
| lava, fire | `W y o O X Z` | `X Z` crust |
| neon | `^ n N \|` | |
| purple | `} p P {` | |
| pink | `~ m M` | |
| blue | `= b B` and `` ` `` | |
| red | `_ r R` | |
| vivid green | `v V` | |
| orange, terracotta | `o O`, `t T` | |
| shadow | `s S` | |

No colour is 0x000000: zero is transparent inside the engine.

## 4. Style guide

- **Light comes from the top left.** Every block, plate, stone and plank has
  its top edge (and, softer, its left edge) one tone lighter, and its bottom (and
  right) edge one tone darker. Shadows fall to the bottom right.
- **Outlines are the darkest tone of the material** (sel-out): `!` around wood
  and people, `4` around foliage, `9` around stone, `,` around rust. Near-black
  `k` only for machines, openings and where something meets the ground.
- **Three or four tones per surface**, from its ramp and in order: never jump
  two steps at a hard edge except for an outline.
- **Dither sparingly**: a checkerboard of two neighbouring tones for soft
  transitions (mottled grass, a dirt patch). Never a lone pixel in a flat area
  unless it means something: a glint, a pebble, a flower.
- **One motif per tile, clustered.** Fine scattered noise repeats into snow
  from two cells away; a few clumps (a tuft, a pebble, a crack) read as ground.
- **Level of detail**: at 2x there is room for what the watch could not draw -
  rivets, a door knob, window reflections, siding, bark, eyes with a highlight -
  but the silhouette stays the watch's, so a player of the watch recognises
  everything, and the solid cells still match what is drawn.
- **Things stand at the bottom of their box**, on the same rows as the 1x art:
  the anchors and the solid rows come from the 1x numbers.
- **Characters**: 30x48, a big head (16x16), eyes 4x4 with a 2x3 pupil and a
  highlight, a warm `!` outline, hair and clothes in two tones each.
- **Icons** (24x24): the 1x icon's silhouette and colours, outlined in `k` or
  the material's darkest tone, lit top-left, one detail the 1x could not hold.

## 5. The list

### ch_world.c (phase 1, done, generated)

| Asset | 1x | 2x | Status |
| --- | --- | --- | --- |
| `PX_PASTO` | 12x12 | 24x24 | done |
| `PX_PASTO2` | 12x12 | 24x24 | done |
| `PX_TIERRA` | 12x12 | 24x24 | done |
| `PX_TIERRA2` | 12x12 | 24x24 | done |
| `PX_PASTO_C` | 12x12 | 24x24 | done |
| `PX_PASTO_C2` | 12x12 | 24x24 | done |
| `PX_FLORES` | 12x12 | 24x24 | done |
| `PX_FLORES2` | 12x12 | 24x24 | done |
| `PX_ADOQUIN` | 12x12 | 24x24 | done |
| `PX_LOSA` | 12x12 | 24x24 | done |
| `PX_PARQUET` | 12x12 | 24x24 | done |
| `PX_ALTO` | 12x12 | 24x24 | done |
| `PX_AGUA` | 12x12 | 24x24 | done |
| `PX_CERCA` | 12x12 | 24x24 | done |
| `PX_PIEDRA` | 12x12 | 24x24 | done |
| `PX_LADRILLO` | 12x12 | 24x24 | done |
| `PX_MADERA` | 12x12 | 24x24 | done |
| `PX_PARED` | 12x12 | 24x24 | done |
| `PX_ALFOMBRA` | 12x12 | 24x24 | done |
| `PX_MOSTRADOR` | 12x12 | 24x24 | done |
| `PX_METAL` | 12x12 | 24x24 | done |
| `PX_METAL2` | 12x12 | 24x24 | done |
| `PX_MURO` | 12x12 | 24x24 | done |
| `PX_CHATARRA` | 12x12 | 24x24 | done |
| `PX_ACEITE` | 12x12 | 24x24 | done |
| `PX_ROCA` | 12x12 | 24x24 | done |
| `PX_REJILLA` | 12x12 | 24x24 | done |
| `PX_BALDOSA` | 12x12 | 24x24 | done |
| `PX_NEGRO` | 12x12 | 24x24 | done |
| `PX_PUENTE` | 12x12 | 24x24 | done |
| `PX_ARBUSTO` | 12x12 | 24x24 | done |
| `PX_NIEVE` | 12x12 | 24x24 | done |
| `PX_NEVADO` | 12x12 | 24x24 | done |
| `PX_HIELO` | 12x12 | 24x24 | done |
| `PX_LAVA` | 12x12 | 24x24 | done |
| `PX_VOLCAN` | 12x12 | 24x24 | done |
| `PX_ARENA` | 12x12 | 24x24 | done |
| `PX_MUELLE` | 12x12 | 24x24 | done |
| `PX_VADO` | 12x12 | 24x24 | done |
| `PX_CIRCUITO` | 12x12 | 24x24 | done |
| `PX_CIRCUITO2` | 12x12 | 24x24 | done |
| `PX_MURO_CIRC` | 12x12 | 24x24 | done |
| `PX_SECO` | 12x12 | 24x24 | done |
| `PX_PARAMO` | 12x12 | 24x24 | done |
| `PX_GRAVA` | 12x12 | 24x24 | done |
| `PX_CIUDAD` | 12x12 | 24x24 | done |
| `PX_MURO_CIU` | 12x12 | 24x24 | done |
| `PX_CRISTAL` | 12x12 | 24x24 | done |
| `BR_PASTO` | 12x12 | 24x24 | done |
| `BR_TIERRA` | 12x12 | 24x24 | done |
| `BR_ARENA` | 12x12 | 24x24 | done |
| `BR_NIEVE` | 12x12 | 24x24 | done |
| `BR_GRAVA` | 12x12 | 24x24 | done |
| `BR_CIUDAD` | 12x12 | 24x24 | done |
| `BR_VOLCAN` | 12x12 | 24x24 | done |
| `SP_ARBOL` | 24x36 | 48x72 | done |
| `SP_CASA` | 48x36 | 96x72 | done |
| `SP_TALLER` | 48x36 | 96x72 | done |
| `SP_FUENTE` | 36x22 | 72x44 | done |
| `SP_CARTEL` | 24x14 | 48x28 | done |
| `SP_FAROLA` | 12x24 | 24x48 | done |
| `SP_MAQUINA` | 24x24 | 48x48 | done |
| `SP_PILA` | 24x24 | 48x48 | done |
| `SP_PINO` | 24x36 | 48x72 | done |
| `SP_TORRE` | 24x36 | 48x72 | done |
| `SP_ESTATUA` | 24x36 | 48x72 | done |
| `SP_SERVIDOR` | 24x36 | 48x72 | done |
| `SP_HORNO` | 48x36 | 96x72 | done |
| `SP_BARCO` | 48x36 | 96x72 | done |
| `MU_MESA_PX` | 24x18 | 48x36 | done |
| `MU_SILLA_PX` | 12x20 | 24x40 | done |
| `MU_ESTANTE_PX` | 24x32 | 48x64 | done |
| `MU_COMPU_PX` | 12x22 | 24x44 | done |
| `MU_PLANTA_PX` | 12x24 | 24x48 | done |
| `MU_VASIJA_PX` | 12x18 | 24x36 | done |
| `MU_CUADRO_PX` | 12x14 | 24x28 | done |
| `MU_CAMA_PX` | 24x30 | 48x60 | done |
| `MU_BANCO_PX` | 24x18 | 48x36 | done |
| `MU_CESTO_PX` | 12x16 | 24x32 | done |
| `MU_MACETA_PX` | 12x18 | 24x36 | done |
| `AN_GATO_D` | 16x14 | 32x28 | done |
| `AN_GATO_I` | 16x14 | 32x28 | done |
| `AN_PAJARO_D` | 14x10 | 28x20 | done |
| `AN_PAJARO_I` | 14x10 | 28x20 | done |
| `SP_ABUELA` | 15x24 | 30x48 | done |
| `SP_CHICO` | 15x24 | 30x48 | done |
| `SP_VECINO` | 15x24 | 30x48 | done |
| `SP_SENORA` | 15x24 | 30x48 | done |
| `SP_COFRE` | 18x15 | 36x30 | done |
| `SP_COFRE_ABIERTO` | 18x15 | 36x30 | done |
| `SP_SIGNO` | 15x20 | 30x40 | done |
| `SP_CABINA` | 24x42 | 48x84 | done |
| `SP_PUESTO_CHATARRA` (was rectangles) | - | 72x40 | done |
| `SP_PUESTO_FERIA` (was rectangles) | - | 72x56 | done |
| `SP_BARRERA` (was rectangles) | - | 24x24 | done |

| `SP_ESCOTILLA` (was rectangles) | - | 24x24 | done (phase 2) |

The rest of the world's code-drawn bits were redrawn in phase 2: the door
arrows (a bevelled arrowhead in art pixels), the glints on water and lava,
snow, embers, dust and drips, the creatures' alert mark, the chest's star and
the level numbers (the 2x font).

### ch_parts.c, ch_battle.c, the font (phases 1 and 2, done)

| Asset | Status |
| --- | --- |
| the 16 heads, 16 torsos, 16 arms, 16 legs (`HD_CAB`, `HD_TOR`, `HD_BRA`, `HD_PIE`) | done (phase 2): 2x sprites in colour slots, see section 6 |
| the map figure (`ch_mini_draw`): 4 directions, 2 steps, legs or wheels, 3 head finishes | done (phase 1): 36x48, drawn at 2x |
| the 8 arenas | done (phase 2): the scenery at the canvas's resolution, and the floor is the zone's 2x ground in perspective |
| projectiles (fire, cryo, acid, plasma, volt), the impact's star, the particles | done (phase 2) |
| the 5x7 font | done (phase 2): 10x14, `main/ch_fuente2x.inc` |

### ch_ui.c (phase 2)

The icons are drawn in `tools/arte2x_iconos.py` (one function each) and
written to `main/ch_ui2x.inc` by `python3 tools/arte2x_iconos.py`;
`--hoja sheet.png` draws the 1x beside the 2x and the 2x at the HUD's and
the lists' real sizes, on the menus' dark background.

| Asset | 1x | 2x | Status |
| --- | --- | --- | --- |
| `IC_TALLER` | 12x12 | 24x24 | done |
| `IC_OBJETOS` | 12x12 | 24x24 | done |
| `IC_EQUIPO` | 12x12 | 24x24 | done |
| `IC_REGISTRO` | 12x12 | 24x24 | done |
| `IC_MAPA` | 12x12 | 24x24 | done |
| `IC_AYUDA` | 12x12 | 24x24 | done |
| `IC_SONIDO` | 12x12 | 24x24 | done |
| `IC_GUARDAR` | 12x12 | 24x24 | done |
| `IC_CERRAR` | 12x12 | 24x24 | done |
| `IC_MOCHILA` | 12x12 | 24x24 | done |
| `IC_ACEITE` | 12x12 | 24x24 | done |
| `IC_BATERIA` | 12x12 | 24x24 | done |
| `IC_SOLDADOR` | 12x12 | 24x24 | done |
| `IC_CHIP` | 12x12 | 24x24 | done |
| `IC_IMAN` | 12x12 | 24x24 | done |
| `IC_LLAVE` | 12x12 | 24x24 | done |
| `IC_PASE` | 12x12 | 24x24 | done |
| `IC_TORNILLOS` | 12x12 | 24x24 | done |
| `IC_ANCLA` | 12x12 | 24x24 | done |
| `IC_HERRAMIENTA` | 12x12 | 24x24 | done |
| `IC_BARRIL` | 12x12 | 24x24 | done |
| `IC_COMBATE` | 12x12 | 24x24 | done |
| `IC_TRUEQUE` | 12x12 | 24x24 | done |
| `IC_PIEZA` | 12x12 | 24x24 | done |
| `IC_COLGAR` | 12x12 | 24x24 | done |
| `IC_DIARIO` | 12x12 | 24x24 | done |
| `IC_AJUSTES` | 12x12 | 24x24 | done |

## 6. The robots

A robot is four parts over one 26x40-unit box, and a part has no colour of
its own: it wears the robot's skin (`ch_skins[]`, four colours) and its own
elemental type (`ch_tipo_color[]`). So the part sprites are written in colour
**slots**, turned into colours for each robot by `tinta()` in `ch_parts.c`:

| Slot | Colour |
| --- | --- |
| `1` | the skin's light, lit (its top-left edges) |
| `2` | the skin's light: the plate |
| `3` | the skin's mid: the shaded side |
| `4` | the skin's dark: seams, grooves, the lower edge |
| `5` | the skin's dark, darker: recesses, eye sockets |
| `6` `7` | the skin's highlight (visors, eyes) and its glint |
| `8` | the outline (near black) |
| `9` `0` | the part's TYPE colour and its light: cores, lamps, emitters |

Any other character is the ordinary palette (section 3): chrome and steel
greys, glass, rubber `k K x`, gold `y Y W`, rust `u U *`. Digits are slots in
robot sprites only; everywhere else they are the palette's ramps.

**Canvases** are in HALF units of the box, at the box's own columns (x 0..51),
so the four combine exactly as the watch's rectangles did:

| Part | Size | Row 0 is | Notes |
| --- | --- | --- | --- |
| head | 52 x 34 | box y -5 | its bottom at box y 11, the aerial above the box |
| torso | 52 x 36 | box y 9 | the neck at box y 10, the shoulders' spikes up to y 7 |
| legs | 52 x 30 | box y 25 | the hips at box y 26, the sole on row 29 |
| arm | 24 x 40 | box y 12 | the RIGHT arm, column 0 at box x 18, its shoulder's inner edge at column 6; the left arm is its mirror (column c lands on half x 15 - c) |

The engine draws the legs, the back arm (a shade darker), the torso, the head
and the front arm, raised 3 units when attacking; a robot facing left is the
whole composition mirrored. Parts not yet seen, in the register, are the same
sprite as a two-tone silhouette. `python3 tools/arte2x_robots.py sheet.png`
writes the C and draws the sixteen robots made of each variant.

**Style for parts**: plates are rounded, outlined in `8`, lit top-left in `1`,
their face `2` with a dithered step into `3` on the lower right (curved
sheets, not boards); panel lines are a `4` groove with a `1` lip; bolts are
2x2 (`1 3 / 3 4`); joints are grey rings (`g D x`); eyes sit in a `5` socket
with a `7` glint. The first zones' parts are worn - a rust stain, a scratch -
and the last ones polished, with `w` glints on their lit edges
(`desgaste()`, by the part's zone).
