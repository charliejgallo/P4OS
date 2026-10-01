# GEMAS

A match-three game for P4OS (ported from AmoledOS), of the **Bejeweled** family: two neighbouring
jewels swap places and anything left in a line of three or more breaks, what is
above falls, and sometimes that makes another line by itself.

```bash
# in the simulator
cmake -S sim -B sim/build -DP4OS_SIM_APPS="hello_app;gemas" && cmake --build sim/build -j8
P4_SIM_SCRIPT="wait 800; open demo.gemas" ./sim/build/p4os_sim

# the .so for the microSD
cd apps/gemas && idf.py so                         # build/gemas.so
cp build/gemas.so /Volumes/<sd>/apps/
```

## On the 5" screen

The layout comes from the root, not from constants, and fills the screen.
Upright the board is 8 x 88 px, edge to edge across the width and low on the
screen where the thumb reaches; the HUD takes everything above it (level and
mode beside the pause button, the score in 144 px digits, the best, the
level's progress bar and, in time attack, the clock bar), with the leftover
space spread between its three groups. Lying down the board is 8 x 84 px on
the left and the HUD is a column on the right. The jewels are **rendered** at
the cell size, never scaled, so they are as sharp as on the watch. The board
and the HUD keep clear of the system's edge strips (the top one pulls the
notifications, the bottom one goes home).

The menu fills the height too: the title, the seven jewels, a card showing
how each special is made, the settings and the play button; lying down, in
two columns.

Both orientations are allowed. Turning the screen destroys and re-creates
the app; the game lives in a static, so it comes back paused exactly where it
was (an animation in flight is brought to its end first).

## How it plays

The two usual ways, and both work:

- **tap and tap**: one jewel is selected, the neighbour swaps with it;
- **drag**: press a jewel and pull sideways.

The drag resolves at a quarter of a cell (22 pixels), well before the 50 LVGL needs to call it a
gesture, so the two never clash. The app carries `AOS_APP_FLAG_NO_SWIPE`
precisely for that: the back gesture only counts in the menu.

If the move forms nothing, the jewels return by themselves. If the board runs
out of possible moves, it shuffles itself.

It is played by touch alone (the board has no buttons and no motion sensor):
you pause with the round button in the HUD. After six seconds
without a touch, the game pulses two jewels that would form a line.

## The special jewels

| Formed by | Leaves | What it does |
| --- | --- | --- |
| a line of 4 | **flame** | on breaking, it blows up the 8 surrounding cells |
| an L or T shape | **star** | clears its whole row and its whole column |
| a line of 5 | **hypercube** | swapped with a jewel, it takes every jewel of that colour |

The hypercube forms no lines: it is a wildcard, not a colour. If an explosion
catches it, it detonates anyway. And two hypercubes side by side clear the
board.

Each special breaks with its own animation: the flame with an orange wave and a
shake, the star with two beams, and the hypercube sweeping the board outwards
from where it was, in order of distance.

## Modes and difficulty

**Sin apuro** has no clock: you play until you get bored. **Contrarreloj** has a
bar that drains on its own and refills as jewels break; when it empties, the
game is over.

| | Colours | Drain (level 1) | Per jewel broken |
| --- | --- | --- | --- |
| EASY | 6 | 120 thousandths/s | 300 |
| NORMAL | 7 | 180 thousandths/s | 240 |
| HARD | 7 | 270 thousandths/s | 180 |

The bar is 10,000 thousandths and the drain rises with each level. One colour
fewer on EASY is not a detail: with six colours there are far more possible
moves.

## How it is built

Four modules and the one that joins them:

| | |
| --- | --- |
| `gm_art.c` | draws the jewels in code, when the app opens |
| `gm_board.c` | the rules: lines, specials, falling, shuffling. No LVGL, no HAL |
| `gm_fx.c` | sparks, waves, beams, labels and the flash |
| `gm_snd.c` | the melodies, played through the single tone there is |
| `gemas.c` | one view per cell and the state machine |

**The jewels are drawn, not stored.** Each one is a convex polygon with facets:
for every pixel, which edge the ray from the centre falls on is looked up, and
from that come the relative distance to the edge and the facet. Those two things
build the central table, the crown, the dark outline fillet and the specular
highlight. It is some 250 KB of RGB565A8 sprites (in PSRAM) that weigh nothing
in the `.so`, and the size is whatever the layout asks for.

**There is no canvas.** Unlike Claudito and 2043, which draw the whole frame
into a buffer and stretch it, here every jewel is an LVGL object. In a game
where half the screen is still most of the time, having LVGL repaint only what
moved comes out far cheaper than redrawing 165 thousand pixels thirty times a
second.

**The flash and the star's beams stay inside the board**: a flash across
720x1280 is a full-screen blend per frame in software. For the same reason
the shock waves stop growing at the board's size.

**The board's background is a single image** of two by two cells that LVGL
repeats with `LV_IMAGE_ALIGN_TILE`: one object instead of sixty-four.

## Development switches (simulator only)

| Variable | What for |
| --- | --- |
| `GEMAS_AUTO=1` | the game plays itself; for watching a thousand cascades in a row |
| `GEMAS_TEST=4` | serves up a line of four and plays it after a second |
| `GEMAS_TEST=5` | one of five (hypercube) |
| `GEMAS_TEST=L` | an L (star) |
| `GEMAS_TEST=F` | a flame already made, to watch it detonate |
| `GEMAS_TEST=S` | a star already made |
| `GEMAS_TEST=H` | a hypercube, to watch it sweep the colour |
| `GEMAS_TIME=1` | starts in time-attack mode |
| `GEMAS_SCORE=1234567` | starts a game with that score, to check the HUD with a long number |
| `GEMAS_SLOW=200` | milliseconds per frame; for watching an animation calmly |
| `GEMAS_TRACE=1` | prints state and phase on every frame |
