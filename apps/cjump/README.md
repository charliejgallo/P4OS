# Claude Jump

> **Not an Anthropic product.** Claude and the Claude Code character belong to
> [Anthropic](https://www.anthropic.com). This app is an unofficial homage,
> made out of affection for the character and with no claim to it; it is
> not affiliated with or endorsed by Anthropic. The pixel art was drawn for
> this project.

A vertical platform jumper starring the Claude Code critter. You climb by
bouncing, collect coins and spend them on costumes in the menu's shop.

It came from AmoledOS and lives on the P4OS retro canvas
([`docs/RETRO.md`](../../docs/RETRO.md)), extended to fill the screen.

- **The canvas.** 240 px wide, x3, as tall as the screen allows:
  240x426 = 720x1278 standing up (the whole screen) and 240x240 = 720x720
  lying down, centred. The width is the same both ways, so the world and its
  rules are too; only `CJ_H` (a variable, `cj_canvas_h`) changes.
- **The two buttons.** One at each side: standing up they float,
  translucent, over the bottom corners, with the OS's pause pill between
  them; lying down each has the black column beside the canvas. Holding one
  walks the critter that way; letting go, it glides to a stop. They are read
  from the touch panel's samples, both fingers, not as LVGL clicks. With both
  held, the one pressed last wins.
- **The finger on the canvas.** It takes the critter straight to where the
  finger is.
- **Pause.** The OS's pause pill, or a tap on the score strip.
- **The world.** Platforms off to the side (`extra_pc` per zone) fill the
  wider field; a few platforms under the start fill the first screen; each
  zone has a landmark low on the sky (hills, a setting sun, the moon, a
  nebula, snowy peaks).
- **Turning the screen.** `cjump_resize()` rebuilds the canvas and the
  screens and keeps the game: the camera keeps the critter at the same
  fraction of the screen, and a game in play comes back paused.
- **Screens.** Laid out on a 368x448 design grid: sizes x1.5, positions
  through `X()`/`Y()`, which spread the grid down the tall screen standing
  up. The shop's preview is its own small canvas, x8 (x6 lying down).

The game scrolls **without repainting the screen**: the background is fixed and
only the dirty rectangles are pushed (on P4OS, presented to the retro canvas,
which scales only them). That is the *why*; what follows is the
*how*, for working on it.

---

## Adding a costume

It is the most likely change and it is two places, both in `main/cj_skins.c`.

**1. One row in `cj_skins[]`:**

```c
{ N_("Buzo"), 320, 0x3FA9C9, 0x1E5F76 },
/*    name   price   body     shadow  */
```

- The name carries `N_()` and is translated in the shop with `_()`. **It goes
  into an LVGL label, not onto the canvas, so it may carry accents** — that is
  the difference from 2043 or arkanos, which draw their text with a bitmap font
  of their own and have their catalogues transliterated.
- Price 0 = it comes unlocked. If you add a free one, raise `CJ_SKINS_FREE` in
  `cjump.h` as well.
- The body and the shadow are the whole critter's. If the body is dark
  (luminance < 120) the eyes are drawn with white behind them automatically;
  nothing to do, but it is worth knowing why it exists: without it the Ninja was
  a grey rectangle with no face.

**2. A `case` in `accessory()`**, with the same index as the row.

And then raise `CJ_SKINS`. Since it is a 32-bit map in the `cj_own` preference,
the real ceiling is 32 costumes.

### The rule that cannot be broken

Everything the costume draws has to fit within 5 px on each side of the body
and 14 px above. The dirty box (`cj_hero_box()`) is 7 px at the sides and
4 px below the body, because the bounce's squash widens the body, shifts it
2 px left and drops it 4 px, costume and all. That is the rectangle
that gets dirtied, and **whatever runs outside it leaves a trail stuck on the
screen** — nothing crashes, it just looks dirty, and it only shows up after a
while of playing.

Do not trust your eye for that. The test bench verifies it:

```bash
cd apps/cjump/tools
cc -I../main -I../../../components/aos_ui/include \
   cj_skins_harness.c ../main/cj_pixel.c ../main/cj_skins.c -o /tmp/cjsk
/tmp/cjsk /tmp/skins.ppm && sips -s format png /tmp/skins.ppm --out /tmp/skins.png
```

It draws each costume, in every pose, squash, direction and with the rocket,
onto a canvas painted a sentinel colour and **exits with an error if a pixel
is left outside the box**, saying which and at what coordinate. As a bonus
it writes a grid with all sixteen together, which is the only way to see
whether two came out too similar.

It needs neither LVGL, nor the HAL, nor the simulator.

---

## Simulator switches

A `getenv()` in `create()`, inside `#ifdef AOS_SIM_BUILTIN`. They do not exist
on the board.

| | |
| --- | --- |
| `CJ_AUTO=1` | the critter plays itself and starts over on dying. This is what you leave running for a good while to hunt for trails from badly recorded dirty rectangles |
| `CJ_FPS=1` | frames per second and % of screen pushed, in the score |
| `CJ_COINS=500` | coins, for testing the shop without playing |
| `CJ_SKIN=11` | costume worn (and unlocked) |
| `CJ_OWN=1` | everything unlocked |
| `CJ_SHOP=1` | opens straight into the shop |
| `CJ_ZONE=3` | starts the game in that zone |
| `CJ_SCREEN=pause`&nbsp;\|&nbsp;`over` | opens straight into that panel |

```bash
cd sim
P4_SIM_SCRIPT="wait 800; open demo.cjump" CJ_AUTO=1 ./build/p4os_sim
```

## Preferences

All with the `cj_` prefix. To wipe the progress it is enough to remove them from
the simulator's `prefs.txt`, or from the portal on the board.

| Key | What it stores |
| --- | --- |
| `cj_hi` | record in metres |
| `cj_coins` | coins in your pocket |
| `cj_skin` | costume worn |
| `cj_own` | bitmap of the ones bought (the first three are forced on load) |
| `cj_sfx` | sound |
| `cj_fps` | frames-per-second counter |

---

## Working on the game

| What | Where |
| --- | --- |
| Zones: sky, platform colours and difficulty | `cj_zones[]` in `cj_game.c`. Adding one is a row and raising `CJ_ZONES` |
| Jump, gravity, spring, rocket | the `#define`s in `cjump.h`, all in 1/16 of a pixel per frame |
| What a coin is worth / the bonus | `check_coins()` in `cj_game.c` and `BONUS_PER_M` in `cjump.c`. They are calibrated against the costume prices |
| How a platform is drawn | `draw_plat()` in `cj_draw.c` |

### Screen limits

The bottom 36 px of the screen start the system's home swipe, so the buttons
end 48 px above the edge. The score strip is the top of the screen itself;
the panel is flat with square corners, so its margins are small.
