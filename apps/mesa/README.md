# Juegos de mesa

Six board games in 3D, against the board or two (or four) at the same
screen: chess, draughts, reversi, four in a row, ludo and backgammon. More
games can come as modules on the card, each with its art.

```bash
# the art (Blender 3.3.1 on an M2: a few minutes a set)
B=/Applications/Blender.app/Contents/MacOS/Blender
cd apps/mesa/tools/blender
python3 texturas.py                                   # ludo's and backgammon's printed faces
$B -b -P ajedrez.py -- --set madera --samples 96
$B -b -P tableros.py -- --set madera --samples 96     # every other game, the dice
for s in marmol vidrio metal; do                      # the optional sets
  $B -b -P ajedrez.py -- --set $s --only board,flat,pieces
  $B -b -P tableros.py -- --set $s --only board,flat,pieces
done
cd ../../../..
python3 apps/mesa/tools/pack_p4.py                    # build/mesa_p4.pak and mesa_<set>_p4.pak

# the rules and the engines, on the Mac
cc -O2 -I apps/mesa/main -o mh apps/mesa/tools/harness.c apps/mesa/main/g_*.c
./mh perft          # chess's move generator against the published counts
./mh self 50 1      # every game played to its end, random and the board's moves

# the simulator (the packs in sim/sim_fs/apps)
cp apps/mesa/build/mesa_*p4.pak sim/sim_fs/apps/
cmake -S sim -B sim/build-mesa -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS="hello_app;mesa"
cmake --build sim/build-mesa -j8

tools/build_apps.sh mesa                              # apps/mesa/build/mesa.so
```

On the card: `mesa.so` and `mesa_p4.pak` (3.4 MB: every game in wood, the
dice, the menu's pictures, the sounds) in `/apps`; the marble, glass and
metal sets are `mesa_marmol_p4.pak`, `mesa_vidrio_p4.pak` and
`mesa_metal_p4.pak` (optional: 2.5, 1.1 and 1.1 MB). Neither the renders (`assets/`) nor the
packs are in git.

## Playing

- **The menu:** a card for each game, the game left open ("Seguir la
  partida"), the chess clock on its own and the settings.
- **A new game:** the game's options (draughts: ours or the international
  rules, men taking backwards, the capture of the most; ludo: two to four
  players), against the board (which side, its level) or two people (face
  to face, or passing the board), and for chess, draughts, reversi and
  four in a row a clock (minutes and increment, Fischer or Bronstein, or
  seconds a move).
- **Moving:** tap a piece to lift it (where it may go is marked), then
  where it goes; a draughts capture is its landings in turn, or straight
  the last one when there is only one way. Reversi and four in a row are a
  single tap; in ludo a tap on the token; dice roll with the button or a
  tap on the board.
- **The buttons:** the menu (new game, resign, save or open a PGN, the
  settings, leave), undo and redo (back to the last time a person was to
  move; a roll stays rolled), a hint (what the board would play), the dice,
  and in chess the score sheet (always open lying down).
- **Two people:** face to face the far player's card and buttons are
  turned half round; passing the board turns it to whoever moves.
- **Settings:** the set (wood, and marble, glass and metal when their
  packs are there), 3D or flat view, marking the possible moves, the
  coordinates, the sound, how fast the pieces fly.
- **The chess clock on its own:** a half of the screen each, the far one
  turned; tapping one's own half ends one's turn.
- **USB gamepad** (`aos_pad.h`): the d-pad moves a frame from cell to
  cell, A takes and sets down (and rolls), B puts back, L and R undo and
  redo, START the menu; in the menus and sheets the d-pad and A, B back.

## The games

| Game | Rules | The board plays |
|---|---|---|
| Ajedrez | all of them: castling, en passant, promotion with a choice, mate, stalemate, the third repetition, fifty moves and too little to mate, by themselves | alpha-beta with a transposition table, quiescence, null move, killers and history; eight levels: the low ones search shallow and add noise to the root's moves, so they play a plausible move a bit worse, not a random one |
| Damas | ours: 8 x 8, the flying king, capture compulsory, men taking backwards and the most captured as options; international: 10 x 10, both always | alpha-beta with captures searched out, six levels |
| Reversi | the usual; passing when there is no move | alpha-beta on a table of the squares and mobility, exact in the last empties, six levels |
| Cuatro en línea | 7 x 6 | bitboards, alpha-beta with a table, seven levels; perfect once its search reaches the end of the game |
| Ludo | 2 to 4; a six to leave, rolls again on a six, a capture or a token home, three sixes lose the turn, safe starts and stars, exact roll home | rules of thumb (capture, leave, flee, safety), three levels |
| Backgammon | one game without the cube: the opening roll, the bar, bearing off, the most dice must be played, gammon and backgammon | every play of the roll, the best position at its end (pips, blots and the shots at them, points and primes), three levels |

## How it works

- **The contract** (`main/mesa_game.h`): every game is a `mesa_game_t`: its
  board's size and picture, where each cell is in board units, its legal
  moves as the cells a finger touches, applying one, the dice, its pieces
  with ids that follow them, and the board's play. The app knows nothing
  else about a game.
- **Modules:** a game that does not come with the app is
  `apps/mesa/<name>.so` on the card (`aos_hal_module_open`), exporting
  `mesa_module_abi()` (compared with `MESA_ABI`), `mesa_module_game(i)` and
  `mesa_module_host()`, with its art in `apps/mesa_<word>_p4.pak`. The app
  scans them at start and adds them to the menu (`main/jm_games.c`). None
  exists yet.
- **The picture** (`main/jm_gfx.c`): Bastion's compositor, its size set
  at run time, drawing into an LVGL canvas that fills the screen: LVGL
  turns it with the screen and draws the cards and the buttons over it.
  Each 16 x 16 tile of the screen is hashed with what is drawn on it, and
  only the tiles that changed are drawn again and invalidated: a still
  board costs nothing.
- **The view** (`main/jm_view.c`): the camera is orthographic and fixed
  (55 degrees over the table, straight from above for the flat view, from
  the front for four in a row), so one sprite of a piece serves every
  cell, anchored at its foot. Pieces fly in arcs with a shadow under them
  on the table; a taken piece leaves for its tray as the one that took it
  lands; a disc turned or a pawn promoted hops in place; a new disc falls.
  Lying down the board is the same picture; backgammon, wider than tall,
  has a picture turned a quarter for upright.
- **The board thinks** on a thread of its own (`main/jm_play.c`), on a copy
  of the position; undo, a new game or leaving stop it and wait for it.
- **The packs** (`tools/pack_p4.py`): the boards as JPEG (the P4's engine
  decodes them), with their geometry as text; the pieces as sheets of runs
  (Bastion's format); the menu's pictures RGB565; the sounds synthesised.
- **The art** (`tools/blender`): everything modelled by code: the chess
  pieces turned from profiles (the knight's head an outline given
  thickness, rendered looking either way so it always faces the board's
  middle), checkers, tokens, dice; the woods, marbles, glass and metals are
  procedural materials. `mesa_common.py` has the camera and the formula
  the app uses.
- **Saved:** the game left open in the data folder (`mesa_partida.bin`, its
  options and its list of moves and rolls), the preferences, and PGN files
  in `/sdcard/mesa/ajedrez/`.

## Simulator switches

`MESA_GAME=<word>` opens a new game of that game (`ajedrez`, `damas`,
`reversi`, `cuatro`, `ludo`, `backgammon`), `MESA_PEOPLE=<n>` with n
people (0: the board plays every side), `MESA_LEVEL`, `MESA_OPT=<n>` its
first option, `MESA_TWO=1` passing the board, `MESA_CLOCK=<s>` a clock,
`MESA_FEN=<fen>` a chess position, `MESA_MOVES=<n>` n moves played first,
`MESA_FLAT=1` the flat view.

## Pending

- The board: speed of the engines on the P4 (the levels' times were set on
  the Mac), the frame rate of a piece's flight, the JPEG decoder with the
  boards, the gamepad.
- Mate-in-N problems (no free collection chosen yet); a doubling cube for
  backgammon; the modules' first example (tic-tac-toe or nine men's morris).
- Four in a row's highest level is perfect only once its search reaches
  the end; an opening book would make it perfect from the first move.
