# Hockey de mesa

Table air hockey in 3D, upright: one against the board or two on the same
screen, each player with a finger in their own half. Three tables: a
classic bar table, a neon one and an ice rink.

Everything on screen is 3D, modelled by code in Blender and rendered
ahead: each table is one picture of the room and the table, and the
mallets and the puck are sprites from the same orthographic camera (16
degrees from the vertical, a pixel a millimetre), so a mallet is the same
sprite anywhere on the table. The board only composes them.

```bash
# the art (Blender 3.3.1 on an M2: about a minute a table, its pieces included)
B=/Applications/Blender.app/Contents/MacOS/Blender
cd apps/hockey/tools
for t in bar neon hielo; do $B -b -P blender/table.py -- --theme $t --samples 160; done
cd ../../..
python3 apps/hockey/tools/sounds.py             # assets/sounds, synthesised
python3 apps/hockey/tools/pack_p4.py            # build/hockey_p4.pak (with the bar's table)
                                                # and hockey_neon_p4.pak, hockey_hielo_p4.pak

# the simulator (the packs in sim/sim_fs/apps)
cp apps/hockey/build/hockey*_p4.pak sim/sim_fs/apps/
cmake -S sim -B sim/build-hockey -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS="hello_app;hockey"
cmake --build sim/build-hockey -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.hockey" ./build-hockey/p4os_sim

# the .so
tools/build_apps.sh hockey                      # apps/hockey/build/hockey.so
```

On the card everything goes in `/apps`: `hockey.so`, `hockey_p4.pak`
(0.53 MB: fonts, sounds and the bar's table) and, optional,
`hockey_neon_p4.pak` (0.24 MB) and `hockey_hielo_p4.pak` (0.26 MB).
Neither the renders (`assets/`) nor the packs are in git.

## Playing

- **The mallet follows the finger** that holds it by its knob, as fast as
  a mallet may move (it does not jump) and never past the centre line.
  Against the board any finger plays player 1 (the last one down); with
  two players a finger belongs to the half it lands in, so both play at
  once (`aos_touch_points_n`). The pause is the round button on the right
  rail; the BOOT button pauses too.
- **A match** goes to 3, 5, 7 or 10 goals, or by time (1 to 10 minutes,
  a golden goal if it ends level). The side that took a goal gets the
  puck, lying still in its half. Every goal is shown again, slower (a tap
  skips it). A puck that sits still for four seconds goes back on the
  table for the side whose half it is in.
- **Settings:** the board's level (easy, medium, hard, ace), the mallet's
  size (33, 40 or 49 mm), the puck's speed (its top speed 2.1, 2.6 or
  3.25 m/s), sound, and the blower's hum. The table is chosen before the
  match, and the menu shows it behind.
- **USB gamepads**, one a player (`aos_hal_hid_gamepad_get(0)` and `(1)`,
  the roles of `aos_pad.h`): the d-pad or the stick pushes the mallet, A
  held pushes it fast (a shot), B takes it back to the goal, START pauses.
  Player 2 sits at the other end, so their directions are turned. Against
  the board both gamepads play player 1. In the menus the d-pad and A, B
  back, START plays.
- With two players the far player's texts (the goal, the end, the hint,
  their score) are turned half a turn, for them.

## How it works

- **The match** (`main/hk_game.c`): 2D seen from above, in millimetres
  (560 x 1040 of surface, round corners, a 184 mm mouth), in steps of
  1/240 s with up to 16 substeps when something is fast. The puck rides on
  air (22 mm/s² and a little drag), bounces off the rails and the posts,
  and a mallet hands it its speed (restitution 0.78 against the mallet,
  which does not give way). Mallet and puck are swept against each other
  in every substep: the time of contact of the two moving circles is
  solved from the quadratic, so a mallet at 4 m/s never passes through
  the puck (the harness counts it: 0). A puck squeezed against a rail
  pushes the mallet back.
- **The board as the rival** (`main/hk_ai.c`): it sees the puck as it was
  a moment ago and guesses where it is now as well as it reads motion;
  it defends on the line from its goal to the puck, or where a puck
  coming at it will cross its line (rails bounced if it reads them); when
  the puck is in its half, within its reach and not shooting at its goal,
  it gets behind it (round it, if needed) and shoots through it at a spot
  of the far mouth, or off a rail. The four levels are its lag, error,
  speed, the shot's speed, aim, how well it reads bounces, how far it
  comes out, its rest after a shot and how long it waits for a puck.
- **The picture** (`main/hk_gfx.c`): Metegol's compositor (which is
  Bastion's) upright: 16 x 16 tiles whose things are hashed, only the
  tiles that changed drawn again, half the rows on each core, straight
  into the panel's free buffer (no turning: the panel is upright, as the
  game). Draw order: the table, shadows, the neon glows and the puck's
  trail, a goal's light, a hit's ring, a puck down the far slot and the
  far end over it, the mallets and the puck from far to near, the near
  end over them (it hides the foot of a mallet against it), the scores
  and the clock in the rails' windows, the banners. Text can be turned
  half a turn (`HK_F_ROT`).
- **Sound** (`tools/sounds.py`, `main/hk_audio.c`): the mallet's clack, a
  softer one, the rail's knock, the slot's edge, the puck rattling down
  the goal's chute, the puck put down, the buzzer, and the blower's hum (a
  loop, while a match is on); all synthesised with numpy, mu-law at
  16 kHz.

## Measuring

`tools/harness.sh` plays the board against itself with no screen:

```bash
apps/hockey/tools/harness.sh                  # every level against every other
apps/hockey/tools/harness.sh -a 0 -b 3 -n 50  # easy against ace
apps/hockey/tools/harness.sh -p               # a person with a finger against each level
apps/hockey/tools/harness.sh -p -m 33 -s 1.25 # the small mallet, the fast puck
apps/hockey/tools/harness.sh -t 3 -v          # three minutes, a line a match
```

With `-p` the other side is a person with a finger, as near as a few
lines get: they see the puck a fifth of a second late and carry it on a
little, read a bounce, keep the mallet in front of their goal under the
puck, and swipe a puck in their half towards the middle of the far goal,
a little off; their finger eases into where it goes, at up to 3 m/s. The
levels are tuned against it, to 7 goals, 40 matches each: the person
beats easy (40-0, a goal of the board every 127 s), is about even with
medium (18-22, every 50 s), loses to hard (3-37) and to ace (0-40). The
first levels scored on the person every 12-19 s even at easy: what tamed
them was a lower top speed of the puck (2.6 m/s, not 3.4), slower shots,
a wait before the board goes for a puck and a rest after each shot. The
first person of the harness also scored many own goals sweeping a puck
by its own mouth; a person pulls back to cover the goal instead, and so
does it now. The levels stay in order with the small and the large mallet
and the slow and the fast puck.

The log says every 5 s:
`hockey: 52.0 fps, a frame 0.06 ms (sim 0.00, list 0.00, draw 0.06), 82 tiles, 16 things, flip 0, 4861 KB held, puck 63,177 state 1`
(the simulator). **On the board** (2026-11-22, rev 1.3,
`v0.31.0-35-g5e95c18`): **62 fps** through a whole match against the
board at medium, 2.7 to 7.1 ms a frame with the puck in play (drawing is
all of it; the simulation 0.07 ms) and 1.0 to 1.3 ms waiting for a
serve. The match was played from the portal, one finger dragged at
random for 78 s: the board won 7 to 1, with 39 hard shots. The memory was
all back at the home screen afterwards (15 009 KB free before, 14 945
after). Two fingers at once, the other tables and the levels against a
person are still to be tried by hand. About 4.9 MB of PSRAM
held with a table loaded; when the app closes the log says what its own
allocator still holds (it must be 0 KB).

Simulator switches: `HK_PLAY=1` starts a match at once, `HK_DEMO=1` lets
the board play both sides, `HK_MODE=1` two players, `HK_DIFF=<0..3>`,
`HK_TABLE=<bar|neon|hielo>`, `HK_PADDLE=<0..2>`.

## Pending

- The board: the frames a second with the page flipping, and the two
  fingers at once by hand (the portal's touches do not reach
  `aos_touch_points_n`).
