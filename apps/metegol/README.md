# Metegol

Table football in 3D, lying down: one against the board or two on the same
screen. Sixteen national teams with their flags, twelve clubs of Argentine
football in their colours (their names and shirts, no crests), ten
stadiums.

Everything on screen is 3D, modelled by code in Blender and rendered ahead:
each stadium is one picture with the table in it, and the figures, the
rods and the ball are sprites from the same orthographic camera, so a
figure is the same sprite anywhere on the table. The board only composes
them.

```bash
# the art (Blender 3.3.1 on an M2: seconds for the figures and the table,
# about a minute a stadium)
B=/Applications/Blender.app/Contents/MacOS/Blender
cd apps/metegol/tools/blender
$B -b -P figures.py && $B -b -P table.py && $B -b -P stadiums.py -- --samples 64
cd ../../../..
python3 apps/metegol/tools/sounds.py             # assets/sounds, synthesised
python3 apps/metegol/tools/pack_p4.py            # build/metegol_p4.pak (+ two stadiums)
                                                 # and metegol_<stadium>_p4.pak, one a stadium
python3 apps/metegol/tools/kit_preview.py out.png boca river   # the kits on the figure

# the simulator (the packs in sim/sim_fs/apps)
cp apps/metegol/build/metegol_*p4.pak sim/sim_fs/apps/
cmake -S sim -B sim/build-metegol -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS="hello_app;metegol"
cmake --build sim/build-metegol -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.metegol" ./build-metegol/p4os_sim

# the .so
tools/build_apps.sh metegol                      # apps/metegol/build/metegol.so
```

On the card everything goes in `/apps`: `metegol.so`, `metegol_p4.pak`
(4.1 MB: the figures' masks, the rods, the ball, fonts, sounds, and the two Buenos
Aires stadiums) and one `metegol_<stadium>_p4.pak` for each of the other
eight, optional (1.1 to 1.5 MB each). Neither the renders (`assets/`) nor the packs are in git.

## Playing

- **Each player owns the half of the screen with their goal** (in one
  against the board, only their own). Sliding a finger up and down moves
  the rods as far as the finger goes, each one to its stops; tapping kicks
  with the rod nearest the ball (a whole quick turn of the rod, a quicker
  tap a harder kick); holding still raises the figures so the ball goes
  under them; a short flick towards the other goal is a shot whose
  strength is the flick's speed and whose effect is its slant.
- **Settings:** difficulty (easy, normal, hard, ace), the rods all at once
  or only the one nearest the ball, which goal player 1 defends, the
  ball's speed, a help that puts the nearest figure on the ball while the
  hand is still, sound, music, crowd, the shake on a goal, changing ends at
  half time, and the scoreboard's language.
- **A match** goes by time (two halves of 1 to 10 minutes) or to 3, 5, 7
  or 10 goals. Every goal is shown again, slower (a tap skips it).
- **USB gamepads**, one a player (`aos_hal_hid_gamepad_get(0)` and `(1)`,
  the roles of `aos_pad.h`): the d-pad or the stick moves the rods, A
  kicks, B raises, L/R choose the rod when they go one at a time, START
  pauses. In the menus the d-pad and A, B back, START plays.
- **The teams** are in `/data/metegol/equipos.txt`, which the app writes
  the first time; the portal's page (`web/metegol.js`) edits their names,
  the scoreboard's three letters, the shirt's design and colours and the
  flags. The app reads it again when it changes. A visitor whose shirt is
  too close to the home side's plays in its other kit.

## How it works

- **The match** (`main/mg_game.c`): 2D seen from above, in millimetres, in
  steps of 1/240 s with more substeps when the ball or a foot is fast. A
  rod slides between its stops and turns; a figure's foot reaches the
  ball's height only within ~55 degrees of hanging down, and there it is a
  capsule whose place is `D tan(angle)` from the rod and whose speed comes
  from the rod's turn and slide (that is how a pull shot bends). A kick is
  a whole turn of the rod: the foot goes over and comes round from behind,
  so a ball just behind a rod can be played too and no foot sweeps a ball
  back towards its own goal. Raised figures go up feet forward, away from
  a team-mate's shot. The table's corners and the foot of its bands are
  ramps, so a ball does not die where no foot reaches; one that still sits
  out of reach is served again from the side, as on a real table.
- **The board as the rival** (`main/mg_ai.c`): it sees the ball as it was
  a moment ago, guesses where it will cross each rod, covers the line to
  its goal, and kicks when the ball is in front of a foot, aiming at a spot
  of the mouth with a pull shot. The four levels are its lag, its error,
  how fast its hand is, how hard and how well it shoots.
- **The kits** (`main/mg_kit.c`, `tools/blender/figures.py`): the figure is
  rendered once in 32 turns about its rod, three times: its light, which
  part each pixel is (skin, hair, shirt, sleeve, collar, shorts, socks,
  boots), and where on the body it lies (forward, left, height). A shirt's
  design is a function of that place (stripes go round the body, the band
  at the chest's height, the sash on a slanted plane, the V from the
  shoulders, halves, checks, trims), so it wraps the body and turns with
  it. The side that attacks the other way is the same figure mirrored.
- **The picture** (`main/mg_gfx.c`): Bastion's compositor (16 x 16 tiles
  whose things are hashed; only tiles that changed are drawn again, half on
  each core), lying down: each row of tiles is drawn in the screen's
  orientation into a strip and written into the upright panel's buffer
  turned, so no frame goes through the PPA. The rods' shafts are bars the
  compositor need not redraw when they slide (only their ends and stops).
  Draw order: the stadium, the crowd's frame, shadows, the rods, the far
  band again, the figures and the ball from far to near, the near band
  again, the near handles, a ring where the band hides the ball, the
  scoreboard.
- **Sound** (`tools/sounds.py`, `main/mg_audio.c`): the knock of a figure,
  the bands, the post, the net, the whistle, the crowd's murmur (louder as
  the ball nears a goal), its roar and its "uuuh", and a cumbia for the
  menus; all synthesised with numpy, mu-law at 16 kHz.

## Measuring

`tools/harness.sh` plays the board against itself with no screen:

```bash
apps/metegol/tools/harness.sh                  # every level against every other
apps/metegol/tools/harness.sh -a 0 -b 3 -n 50  # easy against ace
apps/metegol/tools/harness.sh -g 5 -v          # to five goals, a line a match
apps/metegol/tools/harness.sh -p               # a person on a phone against each level
```

It prints, per pairing, the wins, the goals a match, the kicks, the balls
that sat still for three seconds and the ones served again, and the
fastest ball. The levels come out in order (ace beats everyone, easy
loses to everyone). With `-p` the other side is a person on a phone, as
near as a few lines get: one finger moves every rod, so only the rod nearest
the ball is aimed, a quarter of a second late and a little off, and a tap
kicks it with no aim. The levels are tuned against it, two halves of three
minutes: the person beats easy (5.8-3.8, 9 of 12), is even with normal
(3.7-4.1), loses to hard (3.2-8.3) and to ace (1.2-13.2). The first levels
scored a goal every ten seconds on the user at normal: what tamed them is a
delay before a ball at a foot is kicked, a rest after each kick, and less
power and aim.

Measured on the board (2026-11-21, Independiente v Racing in Avellaneda,
one against the board on normal, page flipping): **60-61 frames a second
while playing**, a frame 6-11 ms (220-425 tiles, 110 things); 38-47 during
a goal and its replay, when everything moves. 12.8 MB of PSRAM held with a
stadium loaded; when the app closes the log says what its own allocator
still holds (0 KB). The first build ran at 35-52 and 19 in the replay:
the crowd's whole band was redrawn up to 16 times a second, and the
board's rods twitched after every millimetre of their error; the crowd
now moves in eight strips one at a time and the board's hand waits for
6 mm. The log says every 5 s:
`metegol: 61.2 fps, a frame 6.19 ms (sim 0.20, list 0.23, draw 5.76), 219 tiles, 109 things, flip 1, 12803 KB held, ball 49,-16 state 1`.

Simulator switches: `MG_PLAY=1` starts a match at once, `MG_DEMO=1` lets the board play both sides, `MG_TEAMS=boca,river`,
`MG_STADIUM=<id>`, `MG_MODE=1` two players, `MG_DIFF=<0..3>`.

## Pending

- The system says 3.9 MB of PSRAM do not come back when the app closes,
  though the app's own allocator holds nothing then: outside the app
  (LVGL or the landscape screen), asked of the firmware.
- The goals and the replay (38-47 fps): everything moves at once there.
- After this: a cup (8 or 16) and a league of the twelve clubs against
  the board, with the table and the fixture saved. Table hockey (#10) is
  almost done with this engine.
