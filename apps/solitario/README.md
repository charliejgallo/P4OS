# Solitario

The classic solitaires on a felt: **Klondike** drawing one or three,
**Spider** with one, two or four suits, **FreeCell**, **Pyramid**,
**TriPeaks**, **Golf**, **Yukon**, and **Española**, Klondike with the
Spanish deck of forty (Truco's drawing). Eleven games, each with its own
statistics.

---

## Playing

- Drag a card or a run; it leans a little with the finger's speed and
  casts a shadow, and the piles it may land on are outlined. A drop that
  is not legal springs back.
- Tap a card and it goes to its best place (home only when that is safe,
  otherwise a column, then a free cell); tap it again within 0.4 s and it
  goes home. A card with nowhere to go shivers.
- Tap the stock to draw or deal; when it is empty and passes remain, to
  turn the waste over.
- Undo, a hint, and a **Finish** button when every card can walk home
  (with *Auto finish* on it plays by itself).
- Upright: the bar on top and the buttons under the thumb. Lying down:
  everything in a panel on the left and the table with the whole height.
  Turning the screen keeps the hand.

**Gamepad:** the D-pad walks the piles (up and down inside a column choose
how many cards), A picks up and drops (on the same pile, A sends it to its
best place), B cancels, L undoes, R hints, START opens the menu. In the
panels the D-pad and A walk the buttons, B goes back.

## Scoring and handicaps

Options: scoring **standard**, **Vegas** (a hand costs one coin per card,
each card home pays five), **cumulative Vegas** (the balance carries over),
**timed** (standard, two points off every ten seconds and a bonus for speed
at the end) or **none**; the stock with **three passes** or no limit;
**undo** and **hints** on or off; **solvable hands**; **auto finish**; the
ending; the sound.

## Solvable hands

`so_solve.c` is a depth-first search over the same engine the player uses:
the legal moves ordered by a value per game, the useless ones left out, a
safe card home played without branching, and a table of seen states (the
hash treats columns in another order as the same state). It is not
complete, and need not be: what it solves is solvable.

With *Solvable hands* on, a new hand is searched for while the old cards
gather on the stock, a slice of the search each frame; if 1.6 s pass with
no win, the hand comes from `so_seeds.c`, 120 deals per game the bench
solved (with the stock limited to three passes, so they hold with either
setting). The **daily hand** walks that table by date: the same deal for
everyone that day, the game turning with the week. The **hint** runs the
same search from the position on the table, and falls back to a heuristic.

## The endings

To choose, or at random:

- **Bounce:** the cards jump off the foundations and bounce down the
  screen. The trail is real: each card is stamped onto the felt's own
  RGB565 buffer as it goes and the felt is not cleared, which is how the
  old desktop ending got its look. Only the stamped rectangles are
  repainted; the clean felt is copied back at the end.
- **Fireworks:** cards rise like rockets and burst into small spinning cards
  and sparks that fall and fade (objects from a pool, nothing stamped).
- **Spiral:** the cards leave the centre each along its own arm, turning and
  growing, stamped turned (`so_stamp565_rot`) so the arms stay drawn.

A tap ends it and shows what was won.

## Achievements and looks

Seventeen achievements; most unlock something: eight felts (green, blue,
red, wood, leather, night, ocean, damask), eight backs (classic red and
blue, art deco, moon, waves, tartan, harlequin, gold) and the four-colour
deck. The first of each is free, and so is the **large** deck, which is
about reading well, not a prize.

## How it is drawn

The felt is one opaque RGB565 canvas drawn once per size (`so_felt_draw565`,
per pixel with value noise for the wood and leather). Every card is an
`lv_image` whose source is one of the face images (or the back) drawn at
the size this layout wants - a column of seven fits 92 px upright, ten
Spider columns 66 px - so a hundred cards on the table are a hundred small
objects sharing a few images, and a card that moves repaints only where it
was and where it goes.

- **The French cards** (`so_art.c`) follow Blackjack's method at any size:
  the suits are implicit curves sampled 4 x 4 into a mask per size, the
  indices a stroke font antialiased by distance. A fanned column shows only
  a strip of each card, so the rank and the suit are both large and side by
  side at the top.
- **The courts** (`so_court.c`) are our own pictures: short lists of
  ellipses, polygons, strokes and a suit, in a box of 100 x 70 units that is
  half the picture, drawn again turned 180 degrees. The king has a sceptre,
  crown and ermine; the queen a rose, a tiara and long hair; the jack a
  lance with a pennant and a feathered cap; the robe takes the suit's colour.
- **The Spanish deck** (`so_es.c`) is Truco's drawing (a copy, apps do not
  share code), with a large number and the suit in the strip. LVGL draws it
  onto a canvas lent our image for the moment. Its numbers are red for
  coins and cups and dark for swords and clubs, the two "colours" the
  columns alternate.
- **Motion** is the app's own, as in Blackjack: the frame timer moves every
  card along its tween (a hop on longer flights, a spring when a drop is
  refused), turns it by squeezing its width while it grows a little in
  height, pops it when it lands home and rings the foundation. After each
  move the game's log says which cards moved and in which order, so a deal
  is dealt and a redeal turned over card by card.

## The rules engine

`so_game.c` knows nothing of LVGL. Every game is a set of piles (stock,
waste, foundations, columns, cells, and for Pyramid and TriPeaks the single
positions of the layout, each covered by up to two others). A move writes
primitive operations (move n cards, turn one) to a log, which undo plays
backwards, the view animates and the solver walks.

## The bench

```bash
cc -O2 -I apps/solitario/main apps/solitario/tools/so_harness.c \
   apps/solitario/main/so_game.c apps/solitario/main/so_solve.c \
   apps/solitario/main/so_art.c apps/solitario/main/so_court.c -lm -o /tmp/soh

/tmp/soh fuzz 300            # random and greedy play on every game: every card once,
                             # undo restores the hash and score, autocomplete wins
/tmp/soh solve 100 -1 200000 # how many deals the solver wins, and how fast
/tmp/soh seeds 120 so_seeds.c 1000000   # the table of solved deals
/tmp/soh sheet sheet.ppm 96  # the deck, the backs, the empty marks, the felts
/tmp/soh time 96             # what drawing each thing costs here
```

Measured on the Mac with a budget of 200 000 states: Klondike 70 % solved,
Klondike drawing three 69 %, Spider 97 / 70 / 4 % (one, two, four suits),
FreeCell 83 %, Pyramid 62 %, TriPeaks 73 %, Golf 9 %, Yukon 78 %, Española
73 %. Spider with four suits and Golf rarely solve in the board's time: they
deal from the table. Every solution the bench finds is replayed move by
move on a fresh deal.

## Development switches

In the simulator (on the board `getenv()` returns NULL):

```bash
SO_MODE=3            # a new hand of that game (0..10, so_mode_t)
SO_SEED=42           # with this deal
SO_PLAY=40           # play that many moves of the solution
SO_WIN=1             # play the whole solution and win (the ending)
SO_WINANIM=2         # 0 bounce, 1 fireworks, 2 spiral
SO_PANEL=looks       # menu games options looks stats ach help
SO_FELT=3 SO_BACK=2 SO_DECK=1
SO_ALL=1             # every achievement
```

## Files

| File | What |
| --- | --- |
| `main/solitario.c` | life, the frame, touch, the gamepad, the flow of a hand |
| `main/so_game.c` | the rules of every game, undo, hints, values |
| `main/so_solve.c` | the solver; `so_seeds.c` the solved deals (generated) |
| `main/so_view.c` | layouts, cards as objects, motion, drag, highlights |
| `main/so_win.c` | the three endings |
| `main/so_ui.c` | bars and panels |
| `main/so_data.c` | settings, statistics, achievements, the hand in progress |
| `main/so_art.c`, `so_court.c` | French cards, courts, backs, felts, stamps |
| `main/so_es.c` | the Spanish deck |
| `tools/so_harness.c` | the bench |

Data: `solitario.dat` (statistics and achievements) and
`solitario_mano.dat` (the hand in progress, as its seed and moves) in the
app's data folder; the settings in preferences (`so_*`).
