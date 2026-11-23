# Fusión (`apps/sumar`, `demo.sumar`)

A game of numbered tiles that slide and add up: a swipe moves every tile
that way, two equal tiles that meet fuse into one of double the value, and
a new tile appears after each move. The game ends when no move is left.
Its own name, look and rules on top of that mechanic, not a copy of any
particular game.

- **Boards** from 3×3 to 6×6.
- **Modes:** Clásico (a goal per level; each one reached brings a higher one
  and one more undo, up to five), Contra reloj (two minutes, each goal adds
  20 s, undoing costs 10 s), Bloques (n − 2 stones that never move and cut
  the lines, never two side by side) and De a dos (one move each on the same
  board; whoever makes the goal tile wins, or the higher score when no move
  is left; no undo).
- **Undo:** up to four moves back, limited by the count (Clásico, Bloques)
  or by the time (Contra reloj).
- **Twelve achievements**, a record (score and highest tile) per size and
  mode, the game in course kept in `fusion.sav` in the app's data folder.
- **Four themes** drawn in code (`fu_art.c`): Neón, Gemas, Papel, Bosque.

## How it is put together

| File | What |
|---|---|
| `fusion.c` | entry, life cycle, the icon (AIC) |
| `fu_core.c` | the rules: a move as a list of slides and merges, spawning, stones |
| `fu_art.c` | the themes; each tile rendered on first use into an RGB565A8 sprite at the cell's size (shape per pixel into an ARGB8888 canvas, the number by LVGL, then converted) |
| `fu_ui.c` | the menu, the game, the panels, the animation and the input |
| `fu_snd.c` | the sounds, synthesised as they play into the streaming speaker |
| `fu_save.c` | preferences, records, achievements, the saved game |

The board is the same size in both orientations (the short side minus a
margin), so the sprites survive turning the screen; `resize()` lays the
screen out again without touching the game. Each tile is an LVGL image: a
move gives every image where it goes and the 16 ms timer slides them there
(105 ms, ease-out), then bounces the merged ones and grows the new one
(170 ms). The animation runs on the clock. A swipe during an animation
finishes it at once and plays.

The app logs `fusion: N fps (M frames animating)` every five seconds while
a game is on screen: that is how to measure it on the board. In the
simulator it holds 60.

## Controls

- **Touch:** a swipe anywhere on the screen (34 px decides it).
- **Gamepad** (`aos_pad.h`): the D-pad moves the tiles, B or L undoes,
  START pauses; the menu and the panels go through `aos_pad_menu.h`, with
  START as "Jugar" on the menu and B closing the pause and the achievements.
- **Keyboard:** arrows or WASD move, U, Z or Backspace undo, Esc or P pause;
  on the panels the arrows and Enter.

## Simulator shortcuts

    FUSION_SLOW=8      the animation eight times slower
    FUSION_THEME=2     the theme (0 Neón, 1 Gemas, 2 Papel, 3 Bosque)
    FUSION_TEST="4 0 7412 1 3 2 1 ..."
                       a game of size 4, mode 0, score 7412, then the cells
                       row by row as exponents (0 empty, 255 a stone); a
                       board with no move shows the end panel

The store screenshots came from those (with `lang=en` and `fu_ach=4095` in
the prefs file, so no achievement toast covers the board).
