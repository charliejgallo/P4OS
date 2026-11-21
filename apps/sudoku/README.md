# SUDOKU

Sudoku for P4OS: classic 9x9 in five levels, 6x6 to start with, Killer,
Diagonal and a daily challenge. Solving pays coins, and the coins buy themes
in the game's own shop.

```bash
# the art and the sounds (needed: the app shows flat colours without it)
python3 apps/sudoku/tools/pack_p4.py              # apps/sudoku/build/sudoku_p4.pak, ~3.5 MB, ~12 s
python3 apps/sudoku/tools/pack_p4.py --preview /tmp/pv --only neon,mar

# in the simulator
cmake -S sim -B sim/build-sudoku -DP4OS_SIM_APPS="hello_app;sudoku" && cmake --build sim/build-sudoku -j8
cp apps/sudoku/build/sudoku_p4.pak sim/sim_fs/apps/
(cd sim && P4_SIM_SCRIPT="wait 800; open demo.sudoku" ./build-sudoku/p4os_sim)

# the .so and the pack on the board
tools/build_apps.sh sudoku
tools/install_apps.sh p4os.local sudoku

# the engine alone, on the Mac: speed and spread of the generator
cc -O2 -I apps/sudoku/main -o /tmp/sdh apps/sudoku/tools/sd_harness.c apps/sudoku/main/sd_core.c
/tmp/sdh 20            # 20 boards of every variant and level
/tmp/sdh tech 100      # which techniques 100 Master boards needed
```

## Levels by technique

The generator (`sd_core.c`) never grades a board by its number of clues. It
solves each candidate the way a person would, one step at a time and always
the simplest technique that works, and the level is the hardest technique
the board needed:

| Level | Hardest technique |
| --- | --- |
| Easy | full house, naked and hidden singles (and at least 36 clues) |
| Medium | locked candidates, naked and hidden pairs |
| Hard | triples, X-Wing, XY-Wing, XYZ-Wing, Swordfish |
| Expert | quads, Jellyfish, short chains (up to 8 links) |
| Master | long alternating inference chains |

Up to Hard a board is dug with the logic itself: clues come out while the
logic capped at that level still solves it, which leaves a unique board as
hard as the level allows. Expert and Master dig to a minimal board by brute
force (uniqueness) and keep it only if its grade is the level; a board too
hard gets clues back where the logic got stuck. On the Mac a Master board
takes ~70 ms (about a hundred thrown away); the board is maybe twenty times
slower, so a thread (`sd_gen.c`) keeps one board of every mode and level
ready in `/data/sudoku/reserva.bin` and makes the next while you play.

Killer cages are grown over a solution (no digit twice in a cage), then the
logic, with what a cage's sum allows and the rule of 45 inside each unit,
solves it from no clues; wherever it gets stuck a clue is added. A board the
logic solves has one solution, so none of this needs brute force.

The daily challenge is generated from the date: the same board on every
P4OS that day. It waits for the network clock (`aos_hal_time_is_valid()`).

## Hints

A hint looks at the board as it is: a wrong digit first; otherwise the
logical solver's next step from the values on the board, minus what earlier
hints already ruled out. It names the technique, marks the cells (a chain
is drawn with its links: solid where one of two must be true, dashed where
both cannot) and explains it in a sentence. "Apply" does that step.

## Coins and the shop

A base by mode and level, plus half without hints, a quarter without
mistakes, a quarter under par time, half without help (errors not shown and
no automatic notes) and two a day of streak. The shop has fifteen themes
besides the free one, from 150 to 1500 coins, each previewed on a demo board
before buying. Coins, themes, settings and statistics live in
`/data/sudoku/datos.txt`; the game in progress in `partida.bin`.

## The art

`tools/pack_p4.py` paints everything with numpy and PIL: per theme a
1280 x 1280 picture (upright the screen shows its middle 720 columns, lying
down its middle 720 rows), a 720 x 720 material under the board, a
thumbnail, the given digits in the theme's style (Rubik, OFL, in
`tools/fonts`; seven segments for Aliens, LED dots for City) at the two cell
sizes, and up to three sprites that move over the picture. Seven sound sets
are synthesised too. The JPEGs are stored as they are and the HAL decodes
them straight from the pack file, on a thread of the app's (`sd_theme.c`).

## Controls

Touch: a cell, then a number. Gamepad: the D-pad walks the cells, A places
the chosen number, B notes it, L and R change it, START pauses (the pause
holds hint, undo and restart). Keyboard: 1-9 place (shifted, notes),
arrows, Backspace or 0 erase, N notes, A automatic notes, H hint, Z/U undo,
Y/R redo, P or Esc pause.
