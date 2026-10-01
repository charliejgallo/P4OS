# ARKANOS

Brick breaking for P4OS: twelve walls, falling capsules and a final boss. It
is the watch game from AmoledOS, moved onto the OS's retro canvas (see
[`docs/RETRO.md`](../../docs/RETRO.md)).

It builds two ways from the same source. The simulator compiles it in (it is
in `P4OS_SIM_APPS`). For the board it is a dynamic app (`project_so`, as
`apps/burbujas`): `tools/build_apps.sh arkanos`.

## How it plays

It is played by touch alone, on a canvas that fills the screen:

| | canvas | on screen | the score |
| --- | --- | --- | --- |
| portrait (the better one) | 240x426 | x3, 720x1278 | a strip on top |
| landscape | 426x240 | x3, 1278x720 | a panel on each side of the arena |

The arena is 240 px wide either way (11 columns of 20 px bricks and 10 px
walls); only its height changes. The screen can turn mid-game: the game
carries on where it was, under the pause.

- **The paddle follows the finger.** It goes to the column the finger is on,
  anywhere on the field. In portrait there is a **deck** under the field, a
  rail with a carriage right under the paddle: a thumb there moves the paddle
  without covering the ball.
- **Lifting the finger launches the ball**, at the start of each ball and
  when the magnet (`I`) has caught it. The thumb comes down, aims, and serves
  on letting go.
- **With the laser (`D`), a finger on the glass fires**, at the cannons' own
  pace. A quick tap fires once.
- **Pause:** a finger on the score strip (portrait) or on the **II** button
  at the top of the right panel (landscape). The back gesture and leaving the
  app pause too. The pause has the sound and FPS switches, and the way out.
- The upgrades running show as a capsule with a bar that empties: on the
  deck in portrait, under the lives in landscape.

The OS draws no controls for it (`aos_retro_begin(..., AOS_RETRO_TOUCH)`):
the strip, the deck and the II button are the game's own pixel art.

### Bricks

| | |
| --- | --- |
| colours | one hit |
| silver | two hits, cracking as it goes |
| gold | three hits |
| steel | unbreakable, and you do not need to break it to clear the level |
| bomb | blows up its eight neighbours, and bombs chain |
| `?` | always drops a capsule |

### Capsules

`A` wide, `L` slow ball, `T` three balls, `D` laser, `I` magnet (the ball stays
stuck until you let it go), `V` one life, `P` 500 points.

Upgrades are lost when you lose a ball, as in the original: otherwise a wide
paddle with a laser turns any mistake into a free one.

### Scoring

Each brick has its own. Breaking several in a row without the ball touching the
paddle builds a **combo** and multiplies; the `X6`, `X12`, `X20` sign tells you.
Finishing a level gives 1000 plus 250 per life. After level 12 you go back to
the first one, faster, and the lap shows in the scoreboard (`N1-2`).

## What is different about it inside

The other watch canvas games (2043, claudito) redrew the whole screen every
frame. An arkanoid does not need that. The wall stands still; the only things
moving are the ball, the paddle, the odd capsule and the shards of the brick
that just broke. So this one carries a **dirty-rectangle list**.

There are two buffers of the canvas's size:

- `bg`: sky, stars, walls and bricks. It is rebuilt only when a brick
  changes.
- `fb`: the frame you see. On P4OS this is the OS's canvas itself
  (`r->px`).

Per frame:

1. Restore from `bg` into `fb` the rectangles that got dirtied last frame.
2. Draw what moves, noting down every rectangle.
3. Present **only** the union of the two sets
   (`aos_retro_present_rect()`). The OS scales exactly that, x3, and LVGL
   refreshes exactly that.

**Measured with `tools/ak_harness.c`: about 2 % of the canvas per frame on
average**, in both layouts (2.8 % on the watch's smaller canvas).

Design consequences, worth bearing in mind before adding anything:

- **There is no screen shake.** Moving the canvas would present all of it
  every frame. Hits are felt through local waves and flashes.
- **The background is not animated.** A moving sky forces a full repaint.
- **Everything that moves has to note down its rectangle.** The background
  also has to be repaintable rectangle by rectangle, which is why the stars
  live in a table and are not drawn at random on the fly.

That last point is THE possible mistake in this scheme. On screen it looks
like a dirty trail stuck there, and it is what the test bench is for.

## Test bench without a screen

`tools/ak_harness.c` compiles the game without LVGL or SDL, makes it play
itself for thousands of frames and **draws twice on each one**: once through
the dirty-rectangle path, once by rebuilding the whole canvas. If the two
buffers do not come out identical, something moved without noting itself
down.

```bash
cc -O2 -DAOS_SIM -I apps/arkanos/main -I components/aos_hal/include \
   -I components/aos_ui/include \
   apps/arkanos/tools/ak_harness.c apps/arkanos/main/ak_*.c -o /tmp/akh

/tmp/akh 60000            # 60 thousand frames, the twelve screens and lap 2
AK_TORPE=1 /tmp/akh 30000 # it also lets itself lose, to reach the game over
AK_LAND=1 /tmp/akh 60000  # the landscape layout
AK_TURN=1 /tmp/akh 60000  # the screen turns every 700 frames, mid-game
/tmp/akh 900 11 /tmp/jefe # dumps .ppm captures (1:1) of level 12
```

It prints how much gets pushed per frame. That is the number that justifies
the whole scheme.

## Simulator switches

```bash
cd sim && cmake -B build && cmake --build build -j8
P4_SIM_SCRIPT="wait 800; open demo.arkanos" ARK_AUTO=1 ARK_FPS=1 ./build/p4os_sim
```

| Variable | What for |
| --- | --- |
| `ARK_AUTO=1` | the paddle plays itself |
| `ARK_LEVEL=8` | starts on that level |
| `ARK_LIVES=1` | to reach the end-of-game sign quickly |
| `ARK_FPS=1` | shows frames per second and % of screen pushed |

The FPS counter can also be turned on from the pause, and **it is the first
thing to look at on the board**. The retro canvas has its own switches
(`AOS_RETRO_STATS`, `AOS_RETRO_VERIFY`...), listed in `docs/RETRO.md`.

## Files

| | |
| --- | --- |
| `ak_pixel.c` | primitives, the 5x7 font and the dirty-rectangle list |
| `ak_level.c` | brick types, capsules and the twelve maps |
| `ak_play.c` | ball, paddle, collisions, capsules, state machine |
| `ak_draw.c` | the two layouts (`ak_geo_set`), background, deck, scoreboard and everything that moves |
| `arkanos.c` | the app: the retro canvas, LVGL panels, input, the loop and turning (`resize`) |
| `tools/ak_harness.c` | the test bench above |

## Adding a level

A map 11 characters wide and up to 13 rows, plus a row in `ak_levels[]` with a
name, ball speed, capsule probability and sky colours. There is no per-level
code anywhere.

```c
static const char *const lv_mio[] = {
    "1.1.1.1.1.1",
    ".HHHHHHHHH.",
    "....BMB....",
};
...
{ "MI MURO", LV(lv_mio), 60, 28, 0x0B1026, 0x1B2350, 0x4A9DF5, 24 },
```
