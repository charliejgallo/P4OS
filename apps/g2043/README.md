# 2043 — The battle of Ceres

A vertical shoot-'em-up for AmoledOS, a tribute to Capcom's **1943**: an energy
bar that drains by itself, capsules dropped by a formation when it falls as a
whole, and a different boss at the end of each planet.

```bash
# in the simulator (from sim/)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
P4_SIM_SCRIPT="wait 1500; open demo.2043" ./build/p4os_sim

# the .so for the card
tools/build_apps.sh g2043                          # apps/g2043/build/g2043.so
```

## On P4OS

This is the watch game moved onto the P4OS retro canvas
([`docs/RETRO.md`](../../docs/RETRO.md)) and laid out again for the whole
screen. It keeps the planets, bosses, weapons, capsules, scores and
preference keys, and the id is still `demo.2043`.

- **The canvas is the screen.** Portrait: 240x426 shown x3, 720x1278, the
  whole panel. Landscape: 240x360 x2, 480x720, centred, with the controls in
  the columns at its sides. The width is the same in both, so the waves and
  bosses keep one geometry; the height is picked when the app opens (`gx_h`).
  The app has no orientation flag: it follows the screen, and turning the
  screen creates it again (back at the title).
- **The field.** 240 wide instead of 184 and nearly twice as tall. The wave
  tables were redone for it (longer rows and arrowheads, waves in pairs on
  the flanks, side waves crossing at different heights), enemies settle and
  bosses park on rows given as a percentage of the field (`G_ROW`), the
  bosses sweep wider, and the shots that cross the whole field are a little
  faster. The pools are bigger to match.
- **The HUD** is two rows at the top: score, record and spare ships; energy,
  weapon and planet. The bottom of the field is where the thumbs go.
- **Flying.** Drag a finger anywhere that is not a button. The ship moves
  *with* the finger, 3/2 of what the finger moves, from wherever it was when
  the finger came down: the finger never covers the ship, and a thumb low on
  the screen reaches the whole field.
- **DISPARAR** fires while held, and only then; **TONEL** is the barrel
  roll, invulnerable for a moment, and the button dims while it recharges.
  Bottom right, over the field in portrait; right of the field in landscape.
- **AUTO** (off by default, key `g2043_auto2`) fires by itself, for playing
  with one thumb. The fire button then turns amber and reads *DISPARO AUTO*,
  so continuous fire never looks like a stuck button.
- **Pause.** The button in the top right corner, or the back gesture.
- **Why the controls are the app's.** In portrait they float over the
  canvas, and there the retro service reports as the canvas finger whichever
  finger touched first: a thumb held on fire would steal the drag. The app
  reads both fingers from `aos_hal_touch_frames()` and tells them apart
  itself: a finger on a button is that button, any other is the drag.
- **The sky** is dithered 2x2 between RGB565 levels: on the tall field a
  plain gradient steps in bands dozens of rows tall.
- **Shake and flash.** The watch did these inside its hand upscaler. They are
  now `gx_shake_flash()`, applied to the canvas in place.
- **Detail.** The game switches its detail level from the service's fps.
- **Fixes.**
  - Resuming returns to the exact state the pause came from.
  - Winning shows the "SISTEMA LIBERADO" panel after 5 s.
- **Simulator switches:**
  - `G2043_AUTO=1`: the ship flies itself.
  - `G2043_FPS=1`: shows the fps.
  - `G2043_LEVEL`, `_BOSS`, `_TEST=pu|over`, `_TRACE`: as on the watch.

## How it is put together

| File | What it does |
| --- | --- |
| `gx_pixel.[ch]` | RGB565 buffer, primitives, ASCII sprites, 5x7 font, integer trigonometry |
| `gx_art.[ch]` | the ship and the seven enemies, as ASCII art |
| `gx_world.[ch]` | the planets: gradient, scenery and the wave table |
| `gx_foe.[ch]` | enemy behaviour and the three bosses |
| `g2043.[ch]` | the app: input, HUD, collisions, state machine |

**The grid is 240x426 in portrait, 240x360 in landscape**, drawn in full
every frame straight into the retro service's buffer (200 KB at most, in
PSRAM); the OS scales it x3 or x2 by hardware. On the desktop the simulator
holds 30 fps; **what the full-screen present costs on the board is still to be
measured**. The pause panel's `FPS` switch shows the real frames per second at
the bottom left, and below 27 fps the game drops its expensive decorations by
itself.

Positions go in **fixed point of 1/16 of a pixel** (`FX()` / `UNFX()`) and
angles in **brads** (256 per turn, `gx_sin`/`gx_cos`/`gx_atan2`). libm is not
used: every floating-point symbol would have to be exported from the firmware.

## How a planet is added

All the content lives in tables. A new planet is two things:

1. **A wave table** in `gx_world.c`. Each row is `{ frame, type, how many,
   formation, column%, gift }` (for the side formations the column is the
   height they cross at, as a percentage of the field). If the formation
   falls as a whole before escaping, it drops the capsule in the `gift` field (`1 + PU_*`); if a single
   ship escapes, there is no prize. Just like in 1943.

2. **A row in `gx_levels[]`**: name, sky gradient, scenery colours, background
   style (`BG_DUST` / `BG_OCEAN` / `BG_BELT`), duration in frames until the
   boss, scroll speed and which boss closes it.

Nothing else. `gx_level_count` comes from the `sizeof` and the state machine
chains the planets by itself.

A new **background style** is two `case`s: one in `scenery_spawn()` and another
in `gx_bg_draw()`.

A new **boss** is four functions and a row in `gx_bosses[]`:

| Function | What it does |
| --- | --- |
| `init` | shares the stamina out among the parts and puts it above the screen |
| `think` | moves it and decides when it fires |
| `draw` | draws it with primitives, not with a bitmap: the parts move |
| `hitbox` | where each part is. **Radius 0 = it cannot be hit right now** |

The health bar is the sum of the parts plus the core, and the core is always
part number `parts`. The rule that "you have to bring down the pods first" comes
out of `hitbox` returning radius 0 for the core while a part is still alive;
there is no special case in the engine.

## The three bosses

| Planet | Boss | The trick |
| --- | --- | --- |
| Red Titan (`TITAN ROJO`) | **Crimson Guardian** (`GUARDIAN CARMESI`) | a cruiser with two pods; the core is armoured until both fall, and then it opens and fires in a fan |
| Sea of Neptune (`MAR DE NEPTUNO`) | **Orbital Kraken** (`KRAKEN ORBITAL`) | four arms that undulate and hurt on contact; it throws orbs that correct their course and every so often it throws itself at you |
| Belt of Ceres (`CINTURON DE CERES`) | **Belt Core** (`NUCLEO DEL CINTURON`) | a rotating fortress with four blocks; when all of them fall, the core opens and fires a spiral |

## Weapons

| Capsule | Weapon | How it behaves |
| --- | --- | --- |
| `D` | double | four straight bullets, high rate of fire |
| `T` | triple | a fan of three, of five at level 3 |
| `L` | laser | pierces, damage 2 |
| `O` | wave | slow and wide, damage 4, the best against bosses |
| `E` | energy | +90 of the bar |
| `S` | shield | takes one hit |
| `1` | extra ship | |

Picking up the same capsule raises the weapon's level up to 3. Dying takes you
back to the factory weapon, as it should.

## Simulator shortcuts

They only exist inside the simulator (`AOS_SIM_BUILTIN`); on the board there are
no environment variables.

| Variable | What for |
| --- | --- |
| `G2043_LEVEL=2` | starts straight on that planet, with automatic fire |
| `G2043_BOSS=1` | on top of that, jumps straight to the boss |
| `G2043_TEST=pu` | drops one capsule of each type |
| `G2043_TEST=over` | a single ship and little energy, to reach the end sign |
| `G2043_TRACE=1` | prints wave, enemies alive and whether it fires, every second (`=f`: ten times a second) |

In the simulator the mouse is the finger; scripts fly the ship with `drag`
(`P4_SIM_SCRIPT`, docs/BUILDING.md).
