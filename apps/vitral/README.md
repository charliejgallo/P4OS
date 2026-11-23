# Vitral

A falling-pieces game of stained glass for P4OS (`demo.vitral`).

Pieces of coloured glass fall into a window of lead and a full row breaks.
What makes it its own:

- **Pieces of three and of five squares**, never four: the two trominoes
  and the eighteen one-sided pentominoes, in a well **twelve** wide. The
  marathon brings the awkward pentominoes in by level (P, L, I, T first;
  U, Y, N, V at level 3; Z, F, W at 5; X at 7), with the small ones as
  breathers in every bag.
- **Rows of one colour.** A piece's colour is not tied to its shape. A full
  row whose glass is all one colour is a *vitral*: its share of the points
  counts three times and it takes the row under it with it. Clear glass
  (one piece in sixteen) goes with any colour.
- **The look:** every pixel of glass, lead and stone is drawn by code. The
  lead runs only round each piece, so the stack reads as a real window.
  Breaking rows flash and shatter into shards; a vitral sends beams of
  coloured light across the window. The title is a rose window.

## Modes

| Mode | |
|---|---|
| Maratón | a level every ten rows, faster each time |
| Contrarreloj | three minutes, the best score |
| Rompecabezas | thirty windows (fifteen pictures, then the same turned over with every piece) to fill exactly with the pieces given; undo, a hint, stars by time |
| De a dos | two wells on one board: side by side lying down, face to face upright (the far one turned round); the rows you break send rubble to the other |

The puzzles are made by `vt_pz_build()` in `vt_game.c`: a picture is a
mask whose columns are each one run with stone under it, and a search tiles
it bottom-up so that every piece can fall straight into place. The order it
found is the pieces' order and the hint. `tools/vt_harness.c` checks every
level is won by its own solution.

## Controls

- **Gestures** (the default): drag sideways to move the piece, down to
  lower it; a flick down drops it, a flick up holds it; a tap turns it
  (clockwise on the right half, the other way on the left); a tap on the
  held box holds. Fingers are read raw (`aos_touch_points_n`), so two thumbs
  work at once, and in a match each player plays in their half.
- **On-screen keys** (Options): left, soft drop, right, drop, turn both ways.
- **Gamepad** (`aos_pad.h`): the D-pad moves and lowers, up drops, A and B
  turn, L or R hold, START pauses. In a match the first gamepad is player 1
  and the second player 2. Panels: D-pad and A, B back.
- **Keyboard:** arrows, Z/X (or up) turn, space drops, C holds, P or Esc
  pauses; in a match player 2 has A/D, S, W, Q/E and F.

## Files

| File | |
|---|---|
| `main/vt_game.c` | the rules and the puzzle generator (no LVGL) |
| `main/vt_gfx.c` | glass, lead, stone, the rose window; the dirty tiles |
| `main/vt_view.c` | layouts, the wells, effects and particles, the labels |
| `main/vt_input.c` | fingers, on-screen keys, gamepads, keyboard |
| `main/vt_snd.c` | the sounds, synthesised at start-up, mixed on the streaming speaker |
| `main/vt_menu.c` | the panels |
| `main/vitral.c` | life cycle and the frame |
| `tools/vt_harness.c` | the bench for the rules |

## Drawing and speed

The retro canvas (`aos_retro.h`) at 360x640 or 640x360, shown x2, at 60
frames a second. A background buffer holds the wall, the frames and the
settled glass, drawn cell by cell only where a cell's glass, lead or
neighbours changed; what moves is drawn over a copy each frame and only the
8x8 tiles it touched are presented, gathered into at most two rectangles per
48-row band. In the simulator: 59.8 fps, about 30 us of drawing a frame on
average. The log prints fps and drawing time every five seconds.

## Simulator switches

`VT_MODE=0..3` starts a mode, `VT_LEVEL` its level (the puzzle's from 0),
`VT_KEYS=1` the on-screen keys, `VT_DEMO=n` lets a bot place n pieces,
`VT_SCENE=1` a stack with a vitral one drop away (`2` a busy board),
`VT_PZ_PLACE=n` places the first n pieces of a puzzle, `VT_SND_DUMP=<dir>`
writes the sounds as WAVs (the simulator's speaker is silent).

## Bench

```
cc -O2 -I apps/vitral/main apps/vitral/tools/vt_harness.c apps/vitral/main/vt_game.c -o /tmp/vt && /tmp/vt
```
