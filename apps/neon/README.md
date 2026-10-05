# Neon Snakes

Neon snakes eat neon fruit on a screen that is black everywhere else. No
score and no HUD: the snakes, the fruit and the frame of the arena.

It came from AmoledOS, where the arena was fixed to the watch's 368x448
(21x24 and 34x40 cells of 16 and 10 px). On P4OS the arena is the **whole
screen**, edge to edge, less only the home strip at the bottom, in both
orientations, and the cells are bigger: the watch's were a speck on a 5"
panel.

| | Normal | Combate |
| --- | --- | --- |
| Cell | 24 px | 18 px |
| Arena, standing up | 28 x 49 | 38 x 67 |
| Arena, lying down | 51 x 26 | 69 x 36 |
| Snakes | yours | six: you and five bots (four on a field under 1800 cells) |
| Fruit | one per ~600 cells (2) | one per ~130 cells (19), plus the sparks a dead snake leaves |
| Dying | against a wall or yourself: OTRA VEZ / MENÚ | the snake flashes, leaves every other segment as sparks of its colour, and comes back after ~3 s |
| Speed | 150 ms a step, down to 85 as it grows | 125 ms a step |

## Controls

The finger on the whole screen, and nothing else on it:

- **Swipe** the way you want to go. A short flick is enough (18 px), and one
  drag can turn several times: each leg is measured again from where the
  last turn was taken.
- **Tap** on the side of the head you want to turn to.

Two turns can be queued ahead of the step, so a quick U-turn is not lost.
A small pause pill floats in the top-right corner, below the strip where a
drag down opens the OS's panels; the system's back pauses too, and a swipe
to the right on a menu goes back. Touches are read from the touch panel's
own samples (`aos_hal_touch_frames()`), not as LVGL events, so a flick
shorter than a frame still turns and a second finger is a swipe of its own.

**A USB gamepad** (`aos_pad.h`) plays too: the d-pad or the stick turns
(of two directions pressed at once, the one across the snake's way wins),
START pauses and resumes. On the menus, the pause and OTRA VEZ / MENÚ the
d-pad goes through the buttons with an outline and A presses
(`aos_pad_menu.h`); START presses the one picked, B goes back (from the
pause it resumes) and never leaves the app.

An earlier build had four arrows floating over the bottom corners and a
CONTROL entry on the menu to choose; on the board swiping won, and both are
gone (a `neon_ctrl` value saved by that build is ignored).

**Nothing hides under the pause pill.** The engine has a cover mask: the
cells under the pill never get a fruit or a new snake. The two-device match
has no mask, because the other engine could not know it.

**Turning the screen** keeps the game: its arena is drawn at whatever cell
fits the new screen, the cover mask follows the pill, and it comes back
paused. The next game gets an arena of its own.

The menus use the whole screen too: a big title, the fruit row, NORMAL and
COMBATE and how to play down one column standing up; title on the left and
buttons on the right lying down.

## Two devices

Combate -> MULTIJUGADOR is the watch's lockstep match, kept whole: the lower
MAC is the host, both run the same engine from the same seed, the host sends
every STEP (both humans' turns and the hash of the board) on the reliable
channel before applying it, and the guest applies exactly those. The arena
stays the watch's 34 x 40 with four snakes and nine fruits, drawn at up to
28 px a cell, and the protocol is the watch's, so a P4 could play a watch
the day the P4 has the link: `tools/ns_harness.c hashes` prints the hash
after every step of such a match, and built against AmoledOS's `ns_game.c`
it prints the same 20,000 lines.

The P4 has no link yet. When `aos_hal_link_start()` fails, MULTIJUGADOR
greys out and says why, and stays that way while the app is open; Normal
and Combate against the bots are the game.

## How it is drawn

`ns_art.c` draws every sprite by code when a mode opens: each fruit and each
part of a snake (6 bends and 4 tails in two stripe colours, 4 heads with eyes
and tongue, the white of the flash) is a list of discs, capsules, cones,
ellipses and arcs in cell units, shaded from their distance field - a white
core, a one-pixel rim, a glow over half a cell. Six snake colours now: the
watch's four and lavender and coral.

The screen is a 720x1280 RGB565 canvas shown 1:1 (an LVGL canvas, not the
retro service: nothing is scaled). Because the background is black
everywhere, glows are baked into the sprites and combined by taking the
brighter channel. A sprite is 2x2 cells centred on its own, so a cell can be
repainted from its 3x3 neighbourhood alone; `ns_draw.c` repaints only the
blocks around what the engine marked (a head, a tail, a fruit) plus the
effects and the fruits' pulse.

On this field one frame can mark dozens of scattered blocks - forty fruits
breathing at once - and LVGL keeps 32 invalid areas before it gives up and
redraws the whole screen. So the rectangles are merged, the pair that wastes
the fewest pixels first, down to 18. Measured in the bench: 0.8 % of the
screen per frame in Normal, 4.8-5.2 % in Combate, up to 18 areas.

## Files

| | |
| --- | --- |
| `main/ns_game.c` | the rules: deterministic, integers only, no LVGL; the bots |
| `main/ns_art.c` | the sprites and the title's tube letters |
| `main/ns_draw.c` | the compositor |
| `main/neon.c` | layout, menus, input, the link, the timer, the icon |
| `tools/ns_harness.c` | the bench |

```bash
cc -O1 -Imain tools/ns_harness.c main/ns_game.c main/ns_art.c main/ns_draw.c -lm -o /tmp/nsh
/tmp/nsh                              # rules, compositor vs full repaint, lockstep
/tmp/nsh shot title|normal|combat out.ppm [steps]
/tmp/nsh hashes [steps]               # the two-device match, for the watch diff
```

The bench checks the grid against the snakes after every step, draws every
frame both ways (the marked blocks, and from scratch) and compares them to
the pixel, on the P4's arenas, and runs two engines on the same random
directions for 20,000 steps comparing hashes.

## Development switches (simulator)

```bash
NS_MODE=normal|solo|combat|multi   straight to a game or a screen
NS_AUTO=1                          the bot steers your snake as well
NS_LINK=0                          aos_hal_link_start() fails, as on the P4
```

The simulator's link cannot carry a match yet: `sim/main.c` never calls
`aos_hal_sim_link_tick()`, so two simulators start the link and hear nothing.
