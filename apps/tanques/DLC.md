# Tanques: ideas for DLC

Thought out with the user on 2026-11-23, after playing it on the board.
**Made the same day: the two first DLCs, "Misión a la Luna" and "Día de
playa"** (README, The DLCs). The user's own: the Tortuga for the Moon with a
Tesla coil whose lightning hits several things round it, the Moon map, and
the Anfibio for the beach, islands with a single bridge that only the
Anfibio need not cross, a double cannon whose second shot the recoil throws
off; new mission goals and two modes each. What follows is what was thought
before, cheapest first, with what each
costs against the board's limits (PSRAM ~16.5 MB free when the app opens, a
four-tank battle ~14-15 MB; the pack 34 MB).

## 1. Missions with other goals (data, almost free)

Today every mission is "knock them all out". The rules go into the game once
(win conditions, rounds counted, a tank that is not a fighter); each pack of
missions after that is only data in `TQ_MISSION`:

- escort a truck across the map;
- hold a base for N rounds;
- a boss with a lot of health;
- limited ammunition (the uses per battle already exist);
- knock out a target in few turns.

The most game for the least work.

## 2. Biomes (a terrain generator and ~0.3 MB of scenery each)

- **The Moon**: low gravity, no wind, craters everywhere. It changes how a
  shot is aimed, not only how it looks (the gravity is one number in the
  shells' step and the arc).
- A beach or islands, with water that stops driving (and an amphibious
  tank, below).
- An autumn forest.
- A swamp that makes driving dearer.
- A city at night.

## 3. Weapons (code only, no art)

A magnet that pulls the enemy, a tornado that pushes it, a beam that bounces,
a portal pair, a rain of mines. Better as a prize inside a pack than as a
DLC of their own.

## 4. Tanks (the dearest: ~2 MB of pack each, a model and its renders)

A battle only loads the tanks playing, so memory is not the limit.

- **Anfibio**: crosses water (goes with the beach).
- **Tortuga**: a shield it unfolds.
- **Escorpión**: a short-range flamethrower.
- A giant boss tank for the missions: measure first, a big sprite is heavy.

## 5. Modes

King of the hill, capture the flag, waves. A **daily challenge** made from
the date's seed needs no art at all: better as a free update than a DLC.

## A first DLC

"Misión a la Luna": the Moon biome, six missions with the new goals, two
weapons and perhaps a tank.

## How it would ship

A second pack (`tanques_dlc1.pak`) in `/apps` that the game finds and unlocks
what it brings; the loader already reads packs in parts and by groups. How
the store sells or carries it (`store/`, `CARRIED_PACKS`) is not decided here.
