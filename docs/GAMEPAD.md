# A gamepad in the games

Since 0.9.1 every game on P4OS plays with a USB gamepad or joystick on the
board's USB host ([USB.md](USB.md), "The USB host"), from its title screen
to its last panel, without touching the screen. The screen keeps working as
before: the pad adds to it.

## The same buttons everywhere

HID has no standard layout and cheap pads number their buttons differently,
so each role takes two of them:

| Role | Buttons | What it does |
|---|---|---|
| A | 1 or 2 | the main action: jump, fire, confirm, play, retry |
| B | 3 or 4 | the second action, or back/cancel inside the game's own panels (it never closes the app) |
| L, R | 5 or 7, 6 or 8 | secondary: change club, car, tab, weapon... |
| START | 9 or 10 | pause and resume; "play" on a title screen |
| Directions | the D-pad (hat) or the left stick past half way | move; held, they repeat |

`/api/usb` shows the pad's buttons live, to see which is which.

On a game's panels (title, pause, shop, settings, game over) the D-pad
moves a white outline to the nearest button in that direction and A presses
it. The outline appears with the first press of the pad, so whoever plays
by touch never sees it. Board games put a cursor on the board the same way.

## Game by game

| Game | Controls |
|---|---|
| 2043 | stick flies the ship (speed follows the lean), D-pad full speed · A fire (hold) · B or R barrel roll · START pause |
| ARKANOS | D-pad or stick moves the paddle · A serves, and fires the laser while held · START pause |
| Atasco | D-pad moves a frame · A takes the car, the D-pad slides it, A or B lets go · B (nothing held) or L undo · R restart · START levels |
| Blackjack | A deals / hits / next hand · B stands (and clears, refuses insurance) · L double · R split · START menu |
| Buscaminas | D-pad moves a cursor · A digs (or chords) · B flags · START new board · L/R difficulty |
| Burbujas | stick aims by speed, D-pad in fine steps (up = straight up) · A shoots · B swaps the bubble · START pause |
| Chatarra | D-pad walks a cell at a time · A talks, opens, fights or uses what is in front · START menu · L/R zoom · in every menu and fight, D-pad + A, B back; on the scrap belt a claw (A grabs, B ends) |
| Claude Jump | D-pad or stick walks · START pause · L/R costumes in the shop |
| Claudito | left/right go along the action bar, A presses · with the sponge or the hand, the D-pad moves it and A held is the finger · B strokes it (or wakes it) |
| Dados | D-pad over the dice and buttons · A or START rolls · L/R change the die |
| Doom | stick or D-pad moves and turns · A fire / Enter · B use / back · L/R strafe, both together next weapon · START menu (held 2 s leaves) |
| Flappy | A flaps, starts and retries · START pause |
| Gemas | D-pad moves a frame · A picks a jewel, a direction swaps it · START pause |
| Golf | stick turns the line by speed, D-pad by 0.25° · L/R club · A is every tap of the swing and the putt · B from the 3D view back to the map · up/down move the putt's marker · START pause |
| Lua (Atrapa) | D-pad or stick steers the basket · A or START plays · START pause; in the app, the D-pad goes down the scripts and A opens one |
| Mila | D-pad a step at a time · B undo · R restart · L held shows the whole level · START pause · at home B pets her, R a toy, L the present |
| Monster Hop | D-pad hops (held, it keeps hopping) · A action · START pause, Play on the title · L/R tabs and zones |
| Neon Snakes | D-pad or stick turns · START pause |
| Simon | the four directions are the four pads, clockwise from the top left (up green) · A or START starts |
| Topos | D-pad moves a frame over the holes · A or B whacks · START pause |
| Truco | D-pad over your cards and the calls · A lifts and plays a card, answers · B puts a lifted card back · START menu |
| Turbo | stick is an analog wheel (D-pad the arrows) · A gas · B brake · START pause · L/R car in the garage |

Two-board games (Neon Snakes, Monster Hop's race, Mila's visits, Truco,
Golf) take the pad on each board for its own player.

## In an app

Two headers, in `components/aos_hal/include`, header only: an app that uses
them needs no newer firmware than 0.9.0.

- `aos_pad.h`: `aos_pad_update(&pad, lv_tick_get())` once a frame reads
  every pad into the roles above, with `held`, `pressed`, `released`,
  `repeat` (directions repeat while held, for menus and grids) and the left
  stick in `pad.x`/`pad.y` for analog use. Many cheap pads report their
  D-pad as the X/Y axes, all or nothing, with no hat: `pad.x`/`pad.y` stay
  0 until an axis has been seen part way (only a real stick does that),
  so such a D-pad is read as directions, not as a stick pushed to the end.
- `aos_pad_menu.h`: `aos_pad_menu_set(&menu, buttons, n, first)` hands it a
  panel's LVGL buttons and `aos_pad_menu_step(&menu, &pad)` does the rest:
  the D-pad moves the outline, A sends `LV_EVENT_CLICKED`. Set it again (or
  with no buttons) whenever the buttons change or go.

Things the games ran into:

- The app's `tick` comes five times a second, too slow for a D-pad: read
  the pad from the game's own frame timer, or an `lv_timer` of ~30 ms.
- The outline is drawn 9 px outside the button, and a parent clips it to
  its own area: a row that holds its buttons tight needs some padding (or
  `LV_OBJ_FLAG_OVERFLOW_VISIBLE` and a bigger draw area).
- A screen that has just come up should not take the press that brought it
  (a START that paused must not resume at once): `aos_pad_reset()` when
  it changes.
- On the retro canvas ([RETRO.md](RETRO.md)) the pad already presses the
  canvas's buttons; a game reads them through `aos_retro_buttons()`.

The Lua scripts have `aos.pad()` ([apps/lua/README.md](../apps/lua/README.md)).

In the simulator `P4_SIM_PAD=1` makes the keyboard a pad (the arrows, z/c
= A, x/v = B, a/d = L/R, Return = START, Shift = button 9), and a script's
`pad <buttons> <x> <y> [ms] [hat]` holds one (sim/main.c); a stick at
the end (32767) acts as such a D-pad until one `pad` has held it part way
(say 16000).
