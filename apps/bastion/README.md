# Bastión

A tower defense, upright only: a fort in space, attacked by waves of aliens
that walk a road, and towers built on pads beside it. Ten worlds of three
sectors each (the third ends with a boss), fourteen towers with three tiers,
twelve aliens and three bosses, three powers, and a laboratory where what a
battle pays buys what stays for good.

Everything on screen is 3D, modelled by code in Blender and rendered ahead:
each world is one picture with the depth of its pixels, and every tower,
alien and the fort is a sheet of sprites from the same camera. The board
only composes them, which is how a hundred aliens fit in a frame.

```bash
# the art (Blender 3.3.1 on an M2: seconds a sheet, a minute or two a world)
B=/Applications/Blender.app/Contents/MacOS/Blender
cd apps/bastion/tools/blender
$B -b -P towers.py && $B -b -P aliens.py && $B -b -P fort.py && $B -b -P gallery.py
$B -b -P worlds_a.py -- --samples 96 && $B -b -P worlds_b.py -- --samples 96
cd ../../../..

python3 apps/bastion/tools/pack_p4.py            # -> apps/bastion/build/bastion_p4.pak (+ .pak.1)
                                                 #    and bastion_<world>_p4.pak, one a world
python3 apps/bastion/tools/pack_p4.py --flat     # without Blender: flat stand-ins, to try the game

# the simulator (the packs in sim/sim_fs/apps)
cp apps/bastion/build/bastion_*p4.pak* sim/sim_fs/apps/
cmake -S sim -B sim/build-bastion -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS="hello_app;bastion"
cmake --build sim/build-bastion -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.bastion" ./build-bastion/p4os_sim

# the .so
tools/build_apps.sh bastion                      # apps/bastion/build/bastion.so
```

On the card everything goes in `/apps`: `bastion.so`, `bastion_p4.pak` with
its part (15.9 MB: towers, aliens, the fort, the gallery's portraits,
fonts, icons, sounds) and one `bastion_<world>_p4.pak` a world (1.6-2.7 MB
each, 39 MB in all);
`tools/install_apps.sh` uploads them all. Neither the renders (`assets/`)
nor the packs are in git.

## Playing

- **A battle** is a sector's waves, eight to fifteen of them. Between waves
  nothing moves: build, upgrade and sell at leisure, then call the next one
  with the big button, which shows who comes in it.
- **Building:** tap a pad, then a tower in the panel (a first tap shows its
  reach and what it does, a second builds it). Tap a tower for its panel:
  upgrade (three tiers), whom it aims at (the first, the strongest, the
  weakest, the nearest), sell.
- **Powers** (once unlocked in the laboratory), bottom left: the orbital
  strike (tap it, then where it lands; tapped twice it aims itself), the
  cryo bomb, the repair.
- **On top:** the fort's life, the credits, the wave, the speed (x1, x2,
  x3) and the pause. The system's back and the BOOT button pause too.
- **Three modes**, the tabs over the map of worlds:
  - **Campaña:** the thirty sectors in order, with stars.
  - **Supervivencia:** twice the fort's life and waves without end in any
    world the campaign has reached, each 7 % tougher than the last, more
    of them and closer together, a boss every tenth. Every twenty waves
    is a goal: cores (20, 40, 60...), credits, a quarter of the fort's
    life back, and one of the mode's three stars. It ends when the fort
    falls or when the player retires from the pause's panel, which counts
    the same; each world keeps its record, and the waves held pay cores.
  - **Libre:** any world and sector with every tower and power unlocked
    and three times the credits, to try things. It pays nothing and keeps
    nothing.
- **The difficulty** (the title's button: easy, normal, hard) is the
  purse: a battle starts with 35 % more credits and each alien pays 25 %
  more, or 15 % and 13 % less. The harder purse pays 30 % more cores, the
  easier one 20 % fewer.
- **Fliers:** when the next wave brings any, the line over the panel says
  which towers cannot reach them.
- **Bosses pay as they are worn down:** 15 credits for every twentieth of
  their health lost and 100 when they fall, so the minutes a boss walks
  alone still buy upgrades (the user's Titan paid nothing until it died,
  and he could not upgrade against it).
- **The gallery** (the title's button) has a card for every alien, tower
  and world, locked until met: an alien's opens the first time it comes
  on screen in any battle, a tower's when the laboratory unlocks it, a
  world's when the campaign reaches it. A card has the thing's portrait
  (an alien's walks), its numbers and a line about it; what is locked is
  a dark shape with no name.
- **Stars:** a sector won with four fifths of the fort's life is three
  stars, with two fifths two, otherwise one. A sector opens when the one
  before it is won; a world when the last sector of the one before is.
- **The laboratory** spends cores (won in battle: more for new stars, a
  few even for a defeat): new towers, each tower's mastery (+8 % damage and
  -3 % cost a level), eight technologies of five levels (the fort's life,
  starting credits, bounties, damage, range, salvage, interest, critical
  hits) and the three powers. Experience makes the rank: each one is 1 %
  more damage.
- **USB gamepad** (`aos_pad.h`): the d-pad moves a frame from pad to pad,
  A chooses one and goes into its panel (d-pad and A there, B back), R
  calls the wave (or changes the speed during one), L goes to the powers,
  START pauses. In the menus the d-pad and A; L/R change world or the
  laboratory's tab, B goes back.

### The towers

| Tower | What it does |
|---|---|
| Ametralladora (`gatling`) | cheap and quick; armour shrugs it off |
| Cañón (`cannon`) | shells that explode; not the fliers |
| Láser (`laser`) | a beam that grows while it stays on one target |
| Misiles (`missile`) | homing, long reach, fliers first |
| Tesla (`tesla`) | lightning that jumps to four, five, six of them |
| Criogénica (`cryo`) | slows everything near it |
| Lanzallamas (`flame`) | a cone of fire that leaves them burning |
| Cañón de riel (`rail`) | goes through the whole line |
| Mortero de plasma (`mortar`) | huge blasts far away, blind within 3 m |
| Hangar de drones (`drone`) | three to five drones that chase |
| Pulso EMP (`emp`) | stuns, takes shields down, uncloaks |
| Pozo de gravedad (`gravity`) | slows, and throws them back down the road |
| Amplificador (`radar`) | towers in reach hit harder and see the cloaked |
| Refinería (`bank`) | credits at the end of every wave |

### The aliens

Larva, Corredor (fast), Ácaro (by the dozen), Coraza (armour: every hit
loses 7), Avispa (flies, cutting the road's corners), Divisor (three small
ones when it dies), Escudero (a shield that comes back), Sanador (heals the
ones around), Bruto, Espectro (cloaks: only an amplifier or an EMP shows
it), Saboteador (silences towers), Portador (flies and drops mites); and
the bosses: Reina (lays larvae), Titán (stomps the towers silent), Mente
(a great shield, blinks forward, heals).

## How it works

- **The packs** (`main/bs_pak.c`, `tools/pack_p4.py`): LZ4 entries. A
  world's definition is text (`tools/bsworlds.py` writes it, `bs_world.c`
  reads it): roads as dense points, pads, the fort, the waves of its three
  sectors. The game knows no world by name: a new one is a new pack.
- **Sprites as runs** (`bs_gfx.h`): a sheet's frames are cropped and kept
  per row as skips, runs of opaque pixels (copied) and runs of edge pixels
  with their alpha (blended). The transparent half of every cell is neither
  stored nor visited, and the sheets take half the memory. The aliens are
  turned to each world's hue as they are loaded.
- **The picture** (`main/bs_gfx.c`): a frame is a list of things in order
  (sprites, soft lights added, beams, rings, shadows, rectangles, text).
  The screen is 45 x 80 tiles of 16 pixels and each tile's things are
  hashed; a tile whose hash is the one the target already shows is left
  alone, the rest get the world's pixels back and their things drawn again.
  Nobody says what changed, so a still screen costs nothing and a tower's
  translucent reach is blended once.
- **Behind the rocks:** a sprite is a board standing on its foot, and each
  of its pixels is compared with the depth of the world's pixel there, only
  in the tiles where the world has something nearer than the sprite.
- **To the panel** (`main/bastion.c`): straight into the panel's free
  buffer, then a flip; nothing is copied. Each of the three buffers keeps
  its own tiles' hashes, so one two frames old just differs in more tiles.
  The tiles of a frame go half to each core (`aos_hal_worker_split`).
- **Threads:** a battle runs on the app's worker (core 0): the simulation
  in steps of 1/60 s (`bs_game.c`), the list (`bs_scene.c`, `bs_hud.c`),
  the tiles, the flip. LVGL's thread hands it taps and the gamepad through
  a ring, plays the sounds it asks for through another, and owns the menus
  and the panels over a battle. While LVGL or the system has something over
  the battle the worker holds, and LVGL shows the battle's last frame under
  it from a canvas. Without the worker (another app has it) the frames run
  on LVGL's thread, on one core.
- **Sound** (`main/bs_audio.c`, `tools/sounds.py`): 37 effects and five
  pieces of music synthesised with numpy (nothing recorded), mu-law at
  16 kHz; twelve voices over the music on the streaming speaker.
- **The save** (`main/bs_save.c`): `bastion.sav` in the data folder.
  Version 2 since the modes (the records, the difficulty, the aliens
  seen); a version 1 save is read and kept, and its gallery starts with
  the aliens of the sectors already won.
- **The gallery** (`main/bs_gallery.c`, `tools/blender/gallery.py`): the
  same models rendered through the same camera with more pixels a metre,
  packed as LVGL pictures and loaded a card at a time.

Memory (PSRAM only: an allocation never falls back to internal RAM): a
battle holds 9 MB with the four towers of the start and 13.3 MB with all
fourteen (the simulator's count): the world's picture and depth (3.7 MB), the frame
LVGL shows under its panels (1.8 MB), the sheets of the towers owned and of
the aliens its waves bring, and the sounds with one piece of music. The
menus' pictures are freed before a battle loads, and the wallpaper's photo
is let go while one runs. Internal RAM: none of its own.

Measured on the board (2026-11-20, firmware v0.27.0-10, Cráter Selene sector 1, played
through the portal's taps, page flipping): **62.4-62.6 frames a second**
throughout, the worker's 16 ms; a frame's work is 3.0-3.9 ms between waves
with the free pads breathing (140-270 tiles), 0.8 ms with a pad chosen
(they stop: 0-6 tiles change), and 5.2-6.1 ms in a wave with 30-37
aliens and eight towers firing (150-190 tiles, 110-140 things), of which
the simulation takes 0.55 ms and the list 0.45. 9.1 MB of PSRAM held with
the four towers of the start: the board had 17.4 MB free when the app
opened and 8.2 MB during the battle, with 208 KB of internal RAM free.
Then the user played the first world's three sectors (its boss too) and
the second's first, some forty minutes: never under 60.9 fps, the
heaviest frame 11.8 ms (38 aliens, 385 tiles, 247 things), 9.8 MB held,
nothing wrong in the log. The log says every 5 s:
`bastion: 62.5 fps, a frame 6.12 ms (sim 0.57, list 0.44, draw 5.11), 193 tiles, 142 things, 30 aliens, flip 1, 9100 KB held`.

## The numbers, and the bot that sets them

What a tower does, what an alien is and what the laboratory sells are in
`main/bs_data.c`. The waves are made by `tools/bsworlds.py`: how many come
grows with the wave, they come closer together towards the end of a
sector, and how tough they are is a table of thirty numbers, `HP`, one a
sector, measured:

```bash
apps/bastion/tools/harness.sh 0.85          # a bot plays the campaign as it is
apps/bastion/tools/harness.sh -c 0.85       # ... and measures the HP table to paste
apps/bastion/tools/harness.sh -c 0.85 -v    # with what leaked and when, sector by sector
apps/bastion/tools/harness.sh 0.85 -e -D 2  # the hard purse, and the mode without end too
```

`tools/harness.c` links the battle itself and plays every sector in order
as a player would (towers where they see most of the road, upgrades, the
powers, the laboratory between battles). With `-c` it looks, sector by
sector, for the health at which it keeps 72 % of the fort (`-t`), and the
first twelve sectors are then eased, since a person learns the game there
and the bot does not have to. After changing a tower, an alien, a layout
or the waves' shape: measure again and paste the table.

With the table as it is the bot wins all thirty sectors at the first try
with the normal purse (84 stars of 90) and with the easy one (90), and
with the hard one too but not always at the first (67 stars, one sector
at the fifth try); without end it holds 30 to 49 waves, with the
laboratory the campaign leaves. The second world's first two sectors were
then eased 10 % by hand: the user won the whole first world with the fort
untouched and kept 4 lives of 20 in the first sector of the second, where
a wave of nothing but fliers comes.

Things it found: a bot (or a player) that waits for a dear tower with one
pad built loses; fliers flying a straight line to the fort crossed the map
where no pad reached them (they now cut the road's corners); a ramp of
health through a sector that is too flat loses all its lives in the first
waves and one too steep in the last.

## Adding a world

1. A `World(...)` in `tools/bsworlds.py`: its roads as waypoints, the fort,
   how many pads (they are placed where they see most road; `python3
   bsworlds.py preview out.png` draws every layout, `check` the rules).
2. Its look: a `build_<id>(w)` in `tools/blender/worlds_a.py` or
   `worlds_b.py` (`worlds_kit.py` builds the road, the pads and the fort's
   foundation where the game expects them; `SPEC.md` has the rules).
3. Render it, measure the table again, pack. No code in the app.

The same for a tower or an alien: its model in `towers.py` or `aliens.py`
(`SPEC.md` has the sheets' formats), its numbers and behaviour in
`bs_data.c` and `bs_game.c`.

## Simulator switches

`BS_WORLD=<n>` and `BS_SECTOR=<k>` open that battle straight away
(`BS_MODE=1` without end, `BS_MODE=2` free; `BS_DIFF=1` easy, `2` hard),
`BS_ALL=1` owns every tower and power, `BS_CREDITS=<n>` starts a battle with
that purse, `BS_SHORT=<n>` ends a sector after n waves, `BS_MILE=<n>` puts a goal every
n waves of the mode without end, `BS_CORES=<n>` sets
the laboratory's, `BS_OPEN=1` opens every sector.

## Pending

- The board: the user played the first world and into the second (above):
  the sound and the difficulty are right. Still to see are the late
  worlds' crowds and bosses, a battle with all fourteen towers owned
  (the free mode: 13.3 MB in the simulator), the memory beside other apps
  and the gamepad.
- The modes without end and free, the difficulty, the bosses' pay and the
  gallery are on the board (2026-11-20). The user held 49 waves without
  end in the first world: the save of version 1 was brought up to date
  there, and the record kept. The goals every twenty waves and retiring
  are installed and not yet played on the board.
- **Crowds:** in that run's last waves, with 121 aliens and 810 things in
  the list, a frame took 35.9 ms (the list 1.3, the simulation 3.9 at
  three times the speed, drawing 30.8 for 798 tiles): 27 fps. Up to some
  forty aliens it holds its 60. The drawing of a crowd is what to look
  at next.
- The texts: 192 strings, all in English and German, checked against the
  HUD's buffers and the widths of what holds them (2026-11-20).
