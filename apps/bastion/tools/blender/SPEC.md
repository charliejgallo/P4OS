# Bastión: the 3D assets

Bastión is a tower defense for P4OS (an ESP32-P4 board with a 5" 720x1280
screen, upright). A fort in space is attacked by waves of aliens that walk
a road; the player builds towers on pads beside it. Ten worlds, fourteen
towers with three tiers each, twelve aliens and three bosses.

Everything on screen is **modelled by code in Blender and rendered ahead**:
each world is one picture (with the depth of its pixels, so sprites pass
behind rocks), and every tower, alien and the fort is a sheet of sprites
rendered by the same camera. The board only composes them, which is how it
reaches its frame rate. So the look of the game is decided here.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P towers.py -- --only gatling
    python3 preview.py ../../assets/towers/gatling3_head.png --out ../../build/preview/gatling.png

Blender **3.3.1**, Cycles on Metal. A script is the source of truth: no
hand-made .blend, every run headless and repeatable. `bs_common.py` (import
it as `B`) has the camera, the lights, the materials, a kit of primitives
and the two ways of rendering. **Read it first**; `towers.py` has one tower
(`gatling`) as the worked example.

## The look

- **Bright, saturated, toy-like science fiction.** Think of painted
  miniatures under a strong light: clean shapes, glossy paint, glowing
  parts. The player asked for "colores variados y brillantes".
- **It must read at its real size.** A tower is ~100 px wide and an alien
  ~45 px on a 5" screen. Bold silhouettes, big features, strong contrast
  between parts; nothing thinner than 5 cm (2 px). Look at the preview at
  100 %, not zoomed.
- **Glow sells it.** Emissive parts (`B.glow`) for eyes, cores, rings,
  muzzles, vents. There is no bloom in the render (Standard view
  transform); the game adds halos of its own, so give the light a visible
  surface, not just strength.
- Each thing has **one accent colour** that tells it apart at a glance; the
  rest is neutral (the towers' dark armour, an alien's body colour).
- Original designs only: nothing that copies a known game's or film's
  units.

## The camera and the units

Metres, Z up. X is across the screen, +Y is away from the camera (up the
screen). The camera is **orthographic**, looking down 53.13 degrees
(sin 0.8, cos 0.6), 40 px a metre:

    screen x = 360 + 40 X          screen y = 1280 - 32 Y - 24 Z

so a metre of ground away from the camera is 32 rows and a metre of height
24 rows. Being orthographic, a sprite is drawn 1:1 anywhere on the screen.
`B.setup()` makes this camera; **do not change it**.

A model stands on its own origin: Z = 0 is the ground under it, and it
looks along **+X** (yaw 0). Yaw grows counter clockwise seen from above:
90 looks away from the camera.

## Sheets (towers, aliens, the fort, the extras)

`B.render_sheet(name, build, out_dir, cols, rows, cell, anchor, yaws)`
writes `<name>.png` (RGBA, straight alpha) and `<name>.json`. `build(parent,
col, row)` builds the model under `parent`; with `yaws` the parent is
already turned. `anchor` is the cell's pixel where the origin lands. Keep
every pixel of the model inside its cell at every yaw and frame (the
preview draws the cell's border: nothing may touch it).

| Sheet | Cell | Anchor | Columns | Rows |
|---|---|---|---|---|
| `towers/<id><tier>_base` | 128 x 176 | (64, 136) | 1 | 1 |
| `towers/<id><tier>_head` | 128 x 176 | (64, 136) | 16 yaws (0, 22.5 ...) or 16 frames of a loop | 1 |
| `aliens/<id>` | 96 x 96 | (48, 70) | 8 yaws (0, 45 ...) | 4 frames of its walk (a loop) |
| `aliens/<id>` (brute, carrier) | 144 x 144 | (72, 108) | 8 yaws | 4 frames |
| `aliens/<boss>` | 224 x 224 | (112, 170) | 8 yaws | 4 frames |
| `extras/missile` | 48 x 48 | (24, 24) | 16 yaws | 1 |
| `extras/drone` | 64 x 64 | (32, 40) | 8 yaws | 2 frames |
| `fort/fort` | 320 x 352 | (160, 256) | 3 states: whole, damaged, nearly lost | 1 |

No ground and no shadow in a sheet (the film is transparent): the game
draws a soft shadow under each unit, and lifts a flier above its shadow.
A flier is modelled with its belly at Z = 0.2.

### Towers (`towers.py`, into `assets/towers`)

A tower stands on a hexagonal pad of radius 0.95 m; keep its base within
0.85 m of the centre and its head within the cell at every yaw (a barrel
may reach ~1.3 m from the centre). Up to ~3.2 m tall. **Tier 1** is the one
bought, **tier 3** fully upgraded: visibly bigger and richer (more barrels,
more armour, more light), still the same tower. `_base` is what does not
turn; `_head` what aims, turning about the pad's centre. A tower that does
not aim (marked *loop*) uses the head's 16 cells as the frames of a
seamless loop (something turning or pulsing) instead.

| id | Name | Accent | What it is |
|---|---|---|---|
| `gatling` | Ametralladora | `#ffb020` | rotary cannon, the cheap fast gun (done: the example) |
| `cannon` | Cañón | `#ff4038` | one heavy barrel that fires explosive shells |
| `laser` | Láser | `#ff3bd4` | a lens emitter with cooling fins; a continuous beam |
| `missile` | Misiles | `#9be830` | a rack of launch tubes, tilted up; hits fliers |
| `tesla` | Tesla | `#40b8ff` | a coil with a shining ball on top; chain lightning. *loop* (the ball and arcs flicker) |
| `cryo` | Criogénica | `#a8f4ff` | a frost emitter among ice crystals; slows. *loop* (a ring of vents turning) |
| `flame` | Lanzallamas | `#ff7a1a` | a wide nozzle fed by two tanks; short range |
| `rail` | Cañón de riel | `#a070ff` | a very long barrel between two rails with coils; pierces a line |
| `mortar` | Mortero de plasma | `#2ee6c0` | a fat short barrel pointing up at 60 degrees; huge blasts far away |
| `drone` | Hangar de drones | `#ff6fa8` | a landing deck with a beacon mast. *loop* (the beacon turns) |
| `emp` | Pulso EMP | `#5a6cff` | a ring emitter that throbs. *loop* |
| `gravity` | Pozo de gravedad | `#c040ff` | a black sphere held between turning rings. *loop* |
| `radar` | Amplificador | `#38ff9c` | a dish that turns; strengthens the towers around it. *loop* |
| `bank` | Refinería | `#ffd83a` | a small pump that makes credits. *loop* |

### Aliens (`aliens.py`, into `assets/aliens`) and extras (`assets/extras`)

Four frames of a walk loop (legs, wings, a body that squashes), eight yaws.
The body colour below is the one rendered; the game turns the hue per
world, so keep each alien to **one hue family plus neutral darks and its
glow**, and they will stay good-looking in any hue.

| id | Name | Size | Colour | What it is |
|---|---|---|---|---|
| `grub` | Larva | 1.0 m long | green | the basic crawler: a segmented grub with stubby legs |
| `runner` | Corredor | 0.9 m tall | yellow | fast two-legged sprinter, leaning forward |
| `swarm` | Ácaro | 0.5 m | red | a tiny mite; they come by the dozen |
| `shell` | Coraza | 1.2 m | steel blue | a beetle under thick plates |
| `wasp` | Avispa | 1.0 m | orange | flier: wings that beat (the 4 frames), a sting |
| `split` | Divisor | 1.1 m | purple | a wobbling blob with two nuclei: it splits when it dies |
| `shield` | Escudero | 1.0 m | cyan | a walker carrying a shield generator on its back (the game draws the bubble) |
| `healer` | Sanador | 1.0 m | mint | a floating jellyfish with a glowing bell |
| `brute` | Bruto | 2.2 m | dark red | a four-legged hulk with a horned head (144 px cell) |
| `phase` | Espectro | 1.1 m | pale violet | a thin stalker; the game fades it when it cloaks |
| `jammer` | Saboteador | 1.0 m | magenta | a floating orb with antennae that silences towers |
| `carrier` | Portador | 2.4 m wide | teal | flier: a manta that drops mites (144 px cell) |
| `queen` | Reina | boss, ~4 m | green/gold | the hive mother: a huge abdomen, a crown of spikes |
| `titan` | Titán | boss, ~4 m tall | steel/red | an armoured walker on thick legs |
| `overmind` | Mente | boss, ~3.5 m | violet | a floating brain in a cage of rings and tendrils |

Extras: `missile` (0.6 m long, white with a lime band and a nozzle,
looking along +X, 16 yaws) and `drone` (a 0.6 m quadcopter with a pink
light, 8 yaws, 2 frames of its rotors).

### The fort (`fort.py`, into `assets/fort`)

The thing to protect: a compact space fort of radius ~2.6 m and up to 5 m
tall - a hexagonal keep with a shield projector, antennae, lit windows,
the same dark armour as the towers with cyan light. Three states in one
sheet: whole; damaged (plates missing, scorch marks, the light turned
amber); nearly lost (torn open, red light, bent antennae). The game adds
smoke and fire itself.

## Worlds (`worlds.py`, into `assets/worlds`)

A world is one picture of the whole screen: `B.render_world(id, out)` writes
`<id>_bg.png` (720 x 1280) and `<id>_depth.npy`. Its layout comes from
`../bsworlds.py` (`import bsworlds; w = bsworlds.BY_ID['luna']`), which the
game reads too, so **the road and the pads must be exactly where it says**:

- `w.paths`: one or more roads as dense points (x, y), 0.4 m apart. Build
  each as a ribbon `bsworlds.PATH_W` (2.2 m) wide at **Z = 0, flat**: the
  aliens walk on Z = 0. Where roads share points they are one road. A road
  that starts beyond the screen's edge just runs off it.
- `w.pads`: where towers stand. A hexagonal pad of radius 0.95 m whose top
  is at **Z = 0** (set it into the ground, with a lit rim in `w.accent`).
  Nothing may stand on a pad or over it.
- `w.fort`: the fort is a sprite the game draws; leave a round foundation
  of radius 2.7 m at Z = 0 there, clear of everything.
- Everything else is yours, and should be rich: the ground (with relief,
  but the road, pads and fort plate stay at Z = 0 and nothing higher than
  0.5 m stands within 1.6 m of the road's centre so it never hides an
  alien wrongly), rocks, crystals, plants, pools, machinery, and the sky or
  void beyond the ground if the world floats. Tall things (up to 6 m)
  belong at the sides and between the road's bends. The top 96 rows and
  the bottom 190 rows are under the game's HUD: keep them calm, nothing
  important there.
- `w.theme` names the look; `w.accent`, `w.ground`, `w.road`, `w.sky` are
  the palette to start from (the road must contrast with the ground, and
  aliens of any hue must read on it: avoid a road as saturated as they are).

| id | theme | What it is |
|---|---|---|
| `luna` | moon | grey regolith, craters, cyan crystals, a blue planet's light |
| `selva` | jungle | dark teal ground, bioluminescent plants in magenta and cyan, giant mushrooms |
| `magma` | lava | black basalt plates over glowing lava, vents, embers |
| `hielo` | ice | pale blue ice, frozen spires, cracks with light inside |
| `orbita` | station | metal deck plates, pipes, lit strips, the void and stars beyond the edges |
| `cristal` | crystal | pink sand dunes, amethyst spires, glassy shards |
| `cienaga` | swamp | dark mud, pools of glowing green acid, twisted roots |
| `nebula` | nebula | stone platforms floating over a violet nebula, joined by the road |
| `colmena` | hive | amber organic walls, hexagonal cells, pods, resin |
| `nucleo` | core | obsidian, red energy lines, a black hole's glow |

Light each world to suit it (the sprites are lit by `B.studio()`: a warm
key from the front left; keep the world's key light from the same side so
shadows agree, and keep the road bright enough to play on).

## Rules for whoever works here

- Touch only your own script(s) in this folder and your own folder under
  `apps/bastion/assets/`. Previews and scratch files go in
  `apps/bastion/build/preview/` (ignored by git). No git commands.
- Do not change `bs_common.py`, `SPEC.md` or `../bsworlds.py`; if one of
  them is wrong or lacks something, work around it in your own script and
  say so in your report.
- Look at your renders (`preview.py`, then open the PNG) and iterate until
  they are good at 100 %: this is art, not a checklist.
- `--samples 48` is enough for sheets (they render in seconds); worlds at
  96-128.
- Comments in English, in prose.
