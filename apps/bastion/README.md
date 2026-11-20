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
$B -b -P towers.py && $B -b -P aliens.py && $B -b -P fort.py
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
its part (12.7 MB: towers, aliens, the fort, fonts, icons, sounds) and one
`bastion_<world>_p4.pak` a world (1.6-2.7 MB each, 35 MB in all);
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

Memory (PSRAM only: an allocation never falls back to internal RAM): a
battle holds 9 MB with the four towers of the start and 13.3 MB with all
fourteen (the simulator's count): the world's picture and depth (3.7 MB), the frame
LVGL shows under its panels (1.8 MB), the sheets of the towers owned and of
the aliens its waves bring, and the sounds with one piece of music. The
menus' pictures are freed before a battle loads, and the wallpaper's photo
is let go while one runs. Internal RAM: none of its own.

The log says every 5 s, here from the simulator on a Mac (so the times
say nothing of the board, and `flip 0` is the canvas):
`bastion: 46.3 fps, a frame 0.34 ms (sim 0.04, list 0.03, draw 0.27), 146 tiles, 78 things, 10 aliens, flip 0, 13257 KB held`.

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
```

`tools/harness.c` links the battle itself and plays every sector in order
as a player would (towers where they see most of the road, upgrades, the
powers, the laboratory between battles). With `-c` it looks, sector by
sector, for the health at which it keeps 72 % of the fort (`-t`), and the
first twelve sectors are then eased, since a person learns the game there
and the bot does not have to. After changing a tower, an alien, a layout
or the waves' shape: measure again and paste the table.

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

`BS_WORLD=<n>` and `BS_SECTOR=<k>` open that battle straight away,
`BS_ALL=1` owns every tower and power, `BS_CREDITS=<n>` starts a battle with
that purse, `BS_SHORT=<n>` ends a sector after n waves, `BS_CORES=<n>` sets
the laboratory's, `BS_OPEN=1` opens every sector.

## Pending

- The board: the frame rate from the log (the tiles and the flip follow
  Pinball's and Turbo's ways, but only the simulator has run this), the
  memory beside other apps, the touch.
- The sound has been checked by numbers and spectrograms only: nobody has
  listened to it yet.
- The difficulty is the bot's: real players will say where it is off.
