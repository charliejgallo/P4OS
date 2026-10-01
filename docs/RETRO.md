# The retro canvas

The AmoledOS games draw a small pixel-art canvas, 184x224 RGB565, and upscaled
it x2 by hand to the watch's 368x448. On P4OS that last step belongs to the OS
(recipe **C** of `docs/plan/APPS.md`): the game draws its canvas, says which
part changed, and the OS shows it scaled by a whole number. On the board the
PPA does the scaling. In the simulator, software does it.

Around the game, the OS also draws the on-screen controls the game asks for,
in the system theme, and reads them with both fingers.

- API: `components/aos_ui/include/aos_retro.h`
- The service: `components/aos_ui/aos_retro.c`
- The scaler: `components/aos_hal/aos_retro_p4.c` (board, PPA) and
  `sim/retro_sim.c` (simulator)
- Games on it: `apps/arkanos`, `apps/topos`, `apps/cjump`, `apps/g2043`,
  `apps/claudito`, `apps/chatarra`, and Lua's canvas (`apps/lua`). What each
  asks for is in [The games on it](#the-games-on-it).

## Using it

```c
#include "aos_retro.h"

static void step(void *user) { /* read input, advance one tick, draw, present */ }
static void draw(void *user) { /* once per drawn frame (optional) */ }

static void *create(aos_app_t *self, lv_obj_t *root)
{
    const aos_retro_t *r = aos_retro_begin(root, 184, 224, 0,
                                           AOS_RETRO_LR | AOS_RETRO_A | AOS_RETRO_PAUSE);
    if (!r) return NULL;                   /* no memory */
    /* r->px: 184*224 pixels, RGB565 as LVGL stores it, stride 184 */
    aos_retro_set_label(AOS_RETRO_BTN_A, _("Saltar"));
    aos_retro_run(30, step, draw, ctx);    /* 30 ticks per second of real time */
    ...
}

static void destroy(aos_app_t *self, void *inst)
{
    aos_retro_end();                       /* frees r->px */
}
```

In a step:

```c
uint32_t held = aos_retro_buttons();       /* AOS_RETRO_BTN_* held now */
uint32_t down = aos_retro_pressed();       /* went down since the last call */
uint32_t up   = aos_retro_released();      /* let go since the last call */
int x, y;
if (aos_retro_touch(&x, &y)) { ... }       /* finger on the canvas / slider, canvas px */
if (aos_retro_tap(&x, &y))   { ... }       /* a finger came down on the canvas */

/* draw into r->px, then either */
aos_retro_present();                       /* all of it changed */
aos_retro_present_rect(x, y, w, h);        /* only this, canvas px; call once per dirty rect */
```

Rules:

- **One canvas at a time.** It belongs to the app in front. A second
  `begin()` ends the first.
- **Everything runs with the LVGL lock held.** That means `create()`, events
  and `lv_timer`s, which is where the tick runs. Deleting the root also ends
  the canvas.
- **The app's LVGL panels go on top.** Menus, banners and so on are created on
  the root after `begin()`. They compose over the canvas as usual. The ported
  games put them on a "stage" object placed exactly over the canvas
  (`r->x`, `r->y`, `r->w * r->scale` by `r->h * r->scale`). The watch's
  coordinates go x1.5 there, because the watch showed the canvas x2 and P4OS
  shows it x3.
- **Only numbers and signs go on the canvas.** Words go in LVGL labels, as
  before, so translations and accents keep working.

### Declaring controls

The flags of `aos_retro_begin()`:

| Flag | What the OS draws and reports |
|---|---|
| `AOS_RETRO_DPAD` | A d-pad for the left thumb. It is one disc-shaped target, read like a real pad: the direction from the centre, diagonals included, with a small dead zone. |
| `AOS_RETRO_LR` | Left and right only. With no A/B these are the two halves of the control area, up to 320 px tall. With A/B they are two buttons on the left. |
| `AOS_RETRO_LR_SPLIT` | Left and right, one per thumb (implies `LR`). Upright, LEFT in the bottom-left corner and RIGHT in the bottom-right one, 208x240 at most, either side of the pause pill. Lying down, one low in each side column, 300 tall. A/B, if asked for too, go above RIGHT. Chunkier than the other buttons (radius 18, a 3 px border) and 24 px more forgiving: they are the buttons Claude Jump was tuned with. |
| `AOS_RETRO_A`, `AOS_RETRO_B` | Round action buttons for the right thumb, at the right whatever is on the left (a pair diagonal against the right edge, smaller beside a d-pad). They show a letter, with a caption from `aos_retro_set_label()` under them. |
| `AOS_RETRO_PAUSE` | A pause pill at the bottom centre, like START. It reports `AOS_RETRO_BTN_PAUSE`; the game decides what pausing means. |
| `AOS_RETRO_TOUCH` | The game reads the finger on its canvas through `aos_retro_touch()`/`aos_retro_tap()`, in canvas pixels. |
| `AOS_RETRO_SLIDER` | A horizontal band in the control area, exactly as wide as the canvas. A finger on it reports as a touch on the canvas column right above it, with y set to the last row. A paddle follows the thumb without the thumb covering the ball. A marker follows the finger. No game uses it today: ARKANOS draws its own deck on the canvas. |
| `AOS_RETRO_OVERLAY` | The controls float, translucent, over the bottom of the game, and the canvas takes the biggest factor that fits the whole screen. This is for the wide 240x426 x3 = 720x1278 variant. |
| `AOS_RETRO_CENTER` | The canvas is centred instead of at the top. This suits games that only have the pause button. |

- **Both fingers count.** The buttons are not LVGL clicks: LVGL's pointer is
  one finger. They are hit-tested against the touch panel's own samples
  (`aos_hal_touch_frames()`, both fingers), so a thumb on the arrows and
  another on A both register.
- **A finger on a control is only that control.** A finger on a button's
  touch target (or anywhere on the d-pad's disc, dead zone included) is
  never the canvas finger, even where the controls float over the canvas
  (`OVERLAY`). `aos_retro_touch()` reports the first finger that is on the
  canvas and on no control, so a thumb held on A never takes the place of
  the finger dragging on the canvas.
- **Taps.** A tap is a finger that comes down on the canvas (not on a
  control, and not one sliding in from outside, like an edge swipe) while
  no other finger is on the canvas. A thumb held on a button does not stop
  it. The panel gives no finger ids: a new finger is told by the count going
  up, and with two it is the one further from where the single one was.
- **Taps are not missed.** Every sample between two frames is looked at, so a
  tap shorter than a frame still shows in `aos_retro_pressed()`.
- **The targets are forgiving.** Each is 20 px bigger than the drawing.
- **The OS shows presses.** A held button lights in the accent colour.
- **The controls can be hidden.** `aos_retro_show_controls(false)` hides them
  under a menu and clears their state; the games call it with every panel.
  While they are hidden, the finger on the canvas is not reported either:
  `touch()` is false and no taps arrive, because that finger belongs to the
  menu's buttons.
- **A hidden canvas reads nothing.** While the view is not visible (the app
  is in the background), touches are read and dropped, so taps meant for
  another app never pile up.
- **The screen edges are left alone.** The control container swallows
  LVGL's pointer and gestures, so a slider run is not a swipe. The bottom
  36 px, where the home swipe starts, stay free.

### Where things go

The scale factor is automatic unless the game passes one. It is the biggest
whole number that fits the root and leaves room for the controls below:

- 420 px with a pad or action buttons, 150 px with only pause.
- Plus 96 to 120 px for the slider.

For the watch's 184x224 on the 720x1280 portrait screen that is **x3,
552x672**, at x = 84. It sits at the top, with up to 40 px of margin, and the
controls take the rest. With `CENTER` or no controls, the canvas is centred.
No game uses that shape any more: they all ask for a canvas that fills the
screen (see below), which leaves no room below, so their controls float
(`OVERLAY`) or are their own.

In landscape (1280x720), the controls go in a column on each side of the
game: the d-pad or LR on the left, A/B and pause on the right. There they
sit on the border beside the canvas and are solid; only upright `OVERLAY`
controls are translucent.

`aos_retro_to_canvas()` maps a screen point back to the canvas, for games
that keep their own LVGL hit areas.

### Frame pacing

`aos_retro_run(fps, step, draw, user)` is a fixed tick on an `lv_timer`:

- **Steps follow real time.** `step()` runs `fps` times per second, so a
  60-second round lasts 60 seconds and a ball keeps its speed even when a
  frame comes late.
- **Catching up has a limit.** At most 4 steps run in one timer call. Time
  that cannot be caught up is dropped, so the game does not spiral.
- **Drawing is once per call.** `draw()` runs once after the steps.
- **The loop can be stopped safely.** A step may end the app, for example
  with `aos_ui_back()`: the loop notices and returns.
- **Pausing does not trigger a catch-up.** `aos_retro_pause(true)` stops the
  tick; on resume it does not try to catch up the paused time.

This replaces the self-tuning timers of the watch games. They lengthened the
period when a frame was expensive, which slowed the game itself down.

The timer period is whole milliseconds (33 for 30 fps), so the measured
draw rate settles near 29 fps at `run(30)`. The steps still keep real time.
A game that switches something on an fps threshold should allow for that.

`aos_retro_stats()` gives:

- fps
- the cost of scaling the last refresh, averaged over the last 16 frames, and
  the maximum
- the pixels it scaled
- LVGL's whole refresh time
- whether the HAL scaler did the work

## How present() works

The canvas is shown by an ordinary LVGL object, the **view**. Its
`DRAW_MAIN` handler adds a single image draw task. The image is the canvas
as an `lv_image_dsc_t` at scale x k, with the pivot at the corner and no
antialiasing.

**The draw unit.** A draw unit of the service's own, `AOS_RETRO`, claims
exactly that task, recognised by its source pointer. It writes the scaled
pixels straight into the draw buffer LVGL is rendering, and only over the
area being refreshed:

- **The core goes to the HAL.** This is the part of the area that is aligned
  to whole canvas pixels. It goes to `aos_hal_retro_scale()` in one piece:
  the PPA on the board, a software stand-in with the same contract in the
  simulator.
- **The ragged edges go to the CPU.** These are up to k-1 rows or columns on
  each side, where a refresh chunk (80 rows on the board, 22 in the
  simulator) cuts a canvas pixel in two. They are scaled nearest-neighbour by
  the CPU, as is anything the HAL declines.

**The view answers `COVER_CHECK`.** LVGL therefore does not paint the root's
background under the canvas first.

**Nothing on the way is a scaled copy.** The game draws into the small canvas
only, which is the service's buffer: arkanos, topos and cjump draw straight
into `r->px`. The scaled pixels exist only in LVGL's draw buffer, for the
part being refreshed. LVGL's usual flush then takes them to the frame buffer,
rotating if needed. That path is the shell's own, the same one every app
goes through.

**The watch's buffer is gone.** Each game had a 330 KB `big` buffer, a copy
into it, and an LVGL blit out of it. None of that exists here.

**A present only marks.** `present_rect()` marks the scaled rectangle
invalid. The scaling happens at the next refresh, only there, so a game that
presents 3 % of its canvas pays for 3 %.

**If the unit declines, LVGL draws the same task.** That happens for a layer
that is not RGB565, a transformed layer such as the app switcher's thumbnail,
or a snapshot into ARGB8888. LVGL's software unit then uses its own
transform code: slower, same pixels.

**Screenshots include the canvas.** Snapshots (the portal's `/api/captura`,
the simulator's `shot`) render through the same draw pipeline.

### Measured in the simulator

Test conditions:

- Release build, M-series Mac.
- The canvas is 552x672 = 370,944 screen pixels.
- `AOS_RETRO_FULL=1` presents the whole canvas every frame; `ARK_AUTO=1`
  makes arkanos play itself.

| Path | Scaling a full canvas | LVGL's whole refresh |
|---|---|---|
| HAL scaler (the simulator's stand-in for the PPA) | ~30 us | ~1.25 ms |
| CPU path of the service (`P4_SIM_RETRO_HW=0`, the board's fallback) | ~50 us | ~1.30 ms |
| LVGL's own transform (`AOS_RETRO_LVGL=1`, our unit off) | n/a | ~3.1-3.4 ms |
| Normal play, arkanos with its dirty rectangles | ~1 us for ~5,000 px | ~1.2 ms |

All modes hold 29-30 fps, the target.

The arkanos test bench measures 2.8 % of the canvas pushed per frame on
average. For topos it is 9-16 %, for cjump about 18 %.

The simulator's numbers are not the board's. What they establish:

- The draw-unit route costs nothing next to LVGL's own refresh.
- It beats leaving the transform to LVGL. On the watch that stretch measured
  129 ms per frame.

**Correctness.** `AOS_RETRO_VERIFY=1` checks every pixel the unit writes
against the canvas pixel it must come from. The three games ran with their
bots, with ragged chunk edges on nearly every area. The result: about 22
million pixels in 3,500 areas, **0 wrong**.

## The PPA path and what it assumes

`aos_retro_p4.c` is written against `esp_driver_ppa` of the IDF in
`~/esp/esp-idf` and compiles in the firmware. **It has not run on a board.**
Its assumptions:

- **One blocking SRM operation per refreshed chunk.** Input is the aligned
  block of the canvas. Output is the block x k at its place in LVGL's draw
  buffer. The render waits for it, as it already waits for the flush's PPA
  rotation in `aos_hal_p4.c`. The service has its own SRM client, and the
  driver queues both on the one engine.
- **Alignment.** The PPA takes an output buffer only if its address and size
  are multiples of the 128-byte cache line.
  - The display's two draw buffers are allocated that way in `aos_hal_p4.c`
    (1280*80*2 bytes = 1,600 lines).
  - LVGL's own layers and snapshot buffers are aligned to
    `CONFIG_LV_DRAW_BUF_ALIGN` = 4 only. For those, the scaler returns false
    and the CPU path scales, which is why that path always exists.
  - The canvas is allocated by `aos_hal_retro_alloc()`: PSRAM, aligned and
    padded to 128 bytes, and written back once after zeroing.
- **Cache.**
  - For the source window, the driver writes it back (C2M) itself.
  - For the destination, it *invalidates* whole rows of the output picture
    (M2C, widened to the cache line) before the DMA writes them. Whatever the
    CPU drew in those rows and has not written back would be lost: the black
    beside the canvas, the controls.
  - So the scaler writes those rows back (C2M) first.
  - The draw unit only dispatches a task when **no other draw task is in
    progress on the layer**. The board runs two software draw threads, and
    this keeps them from dirtying the same rows between the write-back and
    the DMA. LVGL asks the unit again as soon as any task finishes.
- **Flash encryption must stay off.** With it on, the SRM refuses PSRAM
  buffers, and the scaler would return false every time.
- **Scaling quality (the big one).** The IDF documents the SRM as
  **bilinear**: "may cause chromatic aberration and loss of contrast at the
  edges". Pixel art x3 through a bilinear filter comes out soft.
  - `P4_SIM_RETRO_BILINEAR=1` makes the simulator's scaler bilinear, to see
    what that looks like before the board arrives.
  - If it is not acceptable on the glass, set the preference `retro_hw` to
    `0`. That sends everything to the CPU path, which is nearest-neighbour
    and crisp. No rebuild is needed; the preference is read when the first
    game opens after boot.
  - The CPU path is expected to be cheap on the P4 as well. It writes one
    canvas row per three screen rows pixel by pixel, and `memcpy`s the other
    two.

### What the board still has to confirm

1. **What the SRM's scaling looks like at x3 on pixel art.** If it is soft,
   choose between the PPA and `retro_hw=0` by eye.
2. **What a full-canvas present costs:**
   - through the PPA, including the cache write-back;
   - through the CPU path;
   - LVGL's whole refresh.

   `AOS_RETRO_FULL` and `AOS_RETRO_STATS` are simulator switches (`getenv`),
   so read the numbers with `aos_retro_stats()` or temporary logging.
3. **Whether the SRM accepts the draw buffers as they are.** They are in
   internal RAM, or in PSRAM if internal was short. Check the offsets and
   the `buffer_size` check. A refusal shows as the "SRM refused" warning,
   logged at most four times, and the CPU path takes over.
4. **No lost pixels beside the canvas.** That confirms the "alone on the
   layer" rule and the write-back are enough.
5. **No seams at chunk boundaries (bilinear only).** Each chunk's block is
   scaled separately, so a bilinear filter may not blend across the boundary
   the way it does inside a block.
6. **Touch.** On the GT911, check two-finger play: the d-pad plus A, and
   the pads' dead zones.

## Orientation

No game forces an orientation any more: none declares
`AOS_APP_FLAG_PORTRAIT` or `LANDSCAPE`. Each one lays itself out from the
root it gets, and the screen turning gives it a new root (the runtime
creates the app again, or calls its `resize()`, which Claude Jump and
ARKANOS use to keep the game going). The service lays out from the root's
size too: the canvas, and the controls below or beside it.

## The games on it

As of 2026-09-30, from the code:

| Game | Canvas (upright / lying down) | Flags | Controls |
|---|---|---|---|
| ARKANOS (`apps/arkanos`) | 240x426 x3 / 426x240 x3 | `TOUCH` | Touch only. The paddle goes to the finger's column, anywhere on the field or on the deck the game draws under the paddle (upright), so the finger does not cover the ball. Lifting the finger launches the ball; with the laser, a finger on the glass fires. The score strip (upright) or the II button (lying down) pauses. No OS slider or button. |
| Topos (`apps/topos`) | 180x320 x4 / 320x180 x4 | `TOUCH`, `CENTER` | Taps (`aos_retro_tap()`, the moment the finger lands). The pause is the score strip's left corner: a pause pill would sit on the bottom row of holes. |
| Claude Jump (`apps/cjump`) | 240x426 x3 / 240x240 x3, centred | `LR_SPLIT`, `PAUSE`, `OVERLAY`, `TOUCH` | LEFT and RIGHT in the bottom corners floating over the field (lying down, low in each black column), the pause pill between them (lying down, atop the right column). A finger on the canvas that is on neither button takes the critter to where it is; a tap on the score strip pauses. Until 2026-09-30 the side buttons were the game's own. |
| 2043 (`apps/g2043`) | 240x426 x3 / 240x360 x2, centred | none | Its own (see below). A finger dragged anywhere that is not a button flies the ship, relative to where it landed; DISPARAR and TONEL for the other thumb, and a pause button in the top right corner. |
| Claudito (`apps/claudito`) | 90x160 x8 / 160x90 x8 | `CENTER` | Its own LVGL hit areas over the canvas: the six buttons are drawn in the game's bar. |
| CHATARRA (`apps/chatarra`) | the root in halves, x2 (360x640 / 640x360) | `CENTER` | Its own, drawn in its UI layer. |
| Lua (`apps/lua`) | the script's size, or 240x426 / 426x240 x3 | none | The script's; the finger through an LVGL object over the canvas. |

**Why 2043 keeps its own controls.** With `A`/`B`/`PAUSE`/`OVERLAY`/`TOUCH`
the service would now tell the thumb on a button from the flying finger,
but it would still not match what 2043 was tuned to:

- the drag works anywhere that is not a button, lying down in the side
  columns too, where the service reports no canvas finger;
- the drag follows the same finger even when the panel swaps the two slots
  (the nearest one to where it was), where the service takes the first;
- the buttons are sized and placed for that game (176 and 124 px, the
  pause over the HUD's clear corner), say DISPARAR / DISPARO AUTO in the
  button and dim TONEL while it recharges.

## Development switches (simulator)

| Variable | What it does |
|---|---|
| `AOS_RETRO_STATS=1` | Every 60 frames: fps, scaling cost, pixels scaled, refresh time. Also logs every button change. |
| `AOS_RETRO_FULL=1` | Every present is the whole canvas, to measure the worst case. |
| `AOS_RETRO_VERIFY=1` | Checks every scaled pixel (see above). |
| `AOS_RETRO_LVGL=1` | Our draw unit claims nothing: LVGL's transform scales. For comparison. |
| `P4_SIM_RETRO_HW=0` | The simulator's HAL scaler declines, so the CPU path does everything. |
| `P4_SIM_RETRO_BILINEAR=1` | The simulator's HAL scaler filters bilinearly, like the SRM is documented to. |
| `AOS_RETRO_ADD_FLAGS=0x20c` | ORs these flags into every `aos_retro_begin()`, to look at a control layout no game asks for yet. |

On the board, `getenv()` returns NULL, and the only switch is the `retro_hw`
preference.

## Porting a watch game onto it

What arkanos, topos and cjump changed:

1. **The canvas.**
   - Delete the hand upscaler (`*_expand`, `*_SCALE`) and the big buffer.
   - Make the game's frame buffer `r->px`.
   - Keep the background buffer and the dirty-rectangle logic as they are.
   - Where each rectangle was upscaled and invalidated, call
     `aos_retro_present_rect()`.
2. **The loop.** Replace the self-tuning `lv_timer` with `aos_retro_run(30, step,
   draw, ctx)`. Games that ran on real `dt` (topos) give each step its exact
   share: 33, 33, 34 ms.
3. **Input.**
   - Remove the IMU (tilt). P4OS has none.
   - Replace the side button with an on-screen button, and keep
     `app->button` (BOOT) as a spare doing the same thing.
   - Read the finger through the service instead of a touch layer over the
     canvas.
4. **Panels.**
   - Put them on a stage over the canvas, coordinates x1.5.
   - Take fonts one step up: 14 to 20, 16 to 24, 20 to 28, 28 to 36, 36 to
     48, 48 to 64.
   - Hide the controls under every panel.
5. **Flags.** `LONG_DRAG` if the finger drags, `NO_SWIPE` if a drag from
   the edge is play. No orientation flag: lay out from the root, both ways.
6. **Keep everything else.**
   - The preference keys, so records carry over.
   - The strings in `_()`, identical, so the AmoledOS catalogs still match.
   - The simulator switches.
   - The test benches in `tools/`. Arkanos's no longer fakes the IMU, and it
     compiles with `-DAOS_SIM`.
7. **Add the app** to `P4OS_SIM_APPS` in `sim/CMakeLists.txt`.

## Exporting to dynamic apps

- **Where the symbols live.** `aos_retro_*` are in `libaos_ui.a` and
  `aos_hal_retro_*` in `libaos_hal.a`. Both are in `gen_symbols.py`'s
  `DEFAULT_LIBS`, and the 21 of them are in
  `components/aos_dynapp/aos_symbols.c`.
- **A new function** needs the table regenerated
  (`python3 tools/gen_symbols.py --build build/rev1_3` after a firmware
  build) and the firmware built again. A new flag does not.
- **The struct `aos_retro_t` and `aos_retro_stats_t` are ABI:** an app
  compiled against one layout reads the fields at those offsets. Add fields
  only at the end, and rebuild every app that uses them.

## Presenting less

- **`present_rect()` does not walk the object tree.** `lv_obj_invalidate_area()`
  in LVGL 9.5 walks every object on the screen overlapping the area, looking
  for blurred or drop-shadowed widgets to invalidate with it, and asks each
  one its extra draw size (an event). A game presenting two dozen rectangles
  a frame under a tree of hidden panels paid that two dozen times. The
  service clips the area to the view's visible part and hands it to the
  display (`lv_inv_area`). Nothing in P4OS uses blur or drop shadows; an app
  that put one over its canvas would have to invalidate it itself.
- **Present what changed, not what was drawn.** A game that redraws its whole
  canvas every frame can still present only the pixels that differ from the
  frame before, by keeping a copy of what the screen shows:
  - Claudito keeps the canvas as last presented and compares the rectangles
    it drew over the background with it, row by row, presenting the rows
    that changed cut to their columns. An idle critter went from ~20 % of
    the stage a frame to ~1 %.
  - 2043 compares the whole canvas in tiles of 8x8 and gathers the changed
    tiles by strips of four rows of tiles, at most two rectangles a strip (at
    most 28 a frame, within LVGL's 32). Its changes are scattered all over
    the field (20-45 % of the tiles), so that presents about 60 % of the
    canvas on the first planet and ~65 % on the third; on the ocean, where
    the swell moves everywhere, it gives up and presents it all.
  - Keep LVGL's 32 invalid areas a refresh in mind: past them it redraws the
    whole screen.
