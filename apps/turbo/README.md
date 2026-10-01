# Turbo

An arcade racer in the manner of the mid-90s arcade drivers: a red wedge
supercar seen from behind, three lanes of traffic, bridges over the road, and
a clock that only the checkpoints refill. The road is drawn in pseudo-3D, one
screen row at a time; the cars and everything standing beside the road are
modelled in Blender and rendered to sprites.

This is the AmoledOS watch game (`ESP32S3_AmoledOS/apps/turbo`) on the P4's
5" screen, upright and lying down, with its art rendered again from the same
Blender pipeline at the P4's size. The stages, the cars, the traffic, the
garage, the records and the saved progress are the watch's; what changed is
below.

```bash
# the art (once, ~65 min on an M2, both can run at once)
cd apps/turbo/tools/blender
/Applications/Blender.app/Contents/MacOS/Blender -b -P cars.py  -- --out ../../assets/cars
/Applications/Blender.app/Contents/MacOS/Blender -b -P props.py -- --out ../../assets/props
cd ../../../..
# the pack
python3 apps/turbo/tools/pack_p4.py            # -> apps/turbo/build/turbo_p4.pak (9.3 MB)

# in the simulator (run from a folder whose sim_fs/apps holds the pack)
cmake -S sim -B sim/build-turbo -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS=turbo
cmake --build sim/build-turbo -j8
cd sim && P4_SIM_SCRIPT="wait 1500; open demo.turbo" ./build-turbo/p4os_sim

# the .so for the card
tools/build_apps.sh turbo                      # apps/turbo/build/turbo.so
```

On the card both go in `/apps`: `turbo.so` and `turbo_p4.pak`. Without the
pack the game still runs, with boxes for cars and props. Neither file is in
git, nor are the renders (`assets/`).

## Playing: touch only

The watch steered by tilting it; the P4 has no IMU.

- **The wheel**, bottom left: put the left thumb anywhere on the left half
  and drag it sideways; a drag of 150 px is full lock (210 or 105 with the
  other two sensitivities). Let go and the wheel comes back to the centre.
  The wheel drawn on the screen turns with it.
- **Or the arrows** (Ajustes > Dirección > Flechas): two keys under the left
  thumb; held, the wheel turns at a steady rate, and comes back when both are
  up.
- **The pedals**, bottom right: brake and gas. A finger can slide from one
  to the other. *Acelerar solo* (Ajustes) keeps the gas down unless the brake
  is.
- **Pause**: the button in the top-left corner, or the system's back.

Both fingers are read from the panel's own samples
(`aos_hal_touch_frames`), so steering and the gas work at once.

## The screen

The camera is the watch's with twice the focal length (600 px): across the
720-pixel column the road looks as it did across the watch's 368, and the art
is rendered for that. Upright the horizon is at row 563, the car's rear
bumper at 938, and the wheel and the pedals fill the bottom under the car;
the clock, the time and the speed sit in the sky. Lying down the horizon is at
row 252 and the wheel and the pedals take the two lower corners. The screen
can turn at any moment: a race pauses, the panels are laid out again, and the
worker takes the new shape before the next frame (a race against another
board cannot pause and carries on).

## The art at the P4's size

| | Watch | P4 |
| --- | --- | --- |
| Focal length | 300 px | 600 px |
| The player's car (7 yaw frames) | 368 x 448 renders, 1:1 | 736 x 896 renders (`cars.py --near-scale 2`), 1:1 |
| The traffic (3 views each) | 100 px/m renders packed at 70 % | the same renders at 100 % |
| The props | 64-384 px tall | 1.5 times that (`props.py --hscale 1.5`) |
| The backdrops | 1024 x 160 | 2048 x 320 |
| Pack | 3.9 MB | 9.3 MB (35 MB unpacked) |

Why these: the player's car is drawn exactly as rendered, so it has to be the
camera's size. Traffic in the next lane, 5 m ahead, is drawn at 120 px per
metre, so the renders' own 100 are nearly 1:1 and more would only cost
memory. The props are drawn at every size as they come closer, most of them
at a fraction of their full size; at 1.5 times the watch's the nearest ones
are magnified a little and the memory is 2.25 times the watch's instead of
4.

### Memory, measured in the simulator (the art each stage keeps in PSRAM)

| Stage | Art (props + traffic) |
| --- | --- |
| Metro Freeway | 6.6 MB |
| Costa Azul | 5.4 MB |
| Red Canyon | 4.9 MB |
| Snow Pass | 4.7 MB |
| Orbit 9 | 4.2 MB |
| Hollow Road | 5.2 MB |
| Tunnel Ridge | 6.6 MB |

Next to it: the renderer 3.4 MB (1.5 MB of tables and the sky's panorama,
1.9 MB for the player's car coloured once per paint), the HUD's baked words,
digits, wheel, arrows and pedals 1.8 MB, and two frames of 1.8 MB (3.6 MB). The
worst stage is ~15.6 MB racing. A third frame is taken only when 2 MB stay
free after it. Internal RAM: two bands of 24 KB (one per core; 12 KB each,
or one, if the heap has no room), the worker's stack is in PSRAM.

## How frames reach the screen

The worker (core 0, priority 3, below LVGL's 4) steps the race, works the
frame's geometry once (`tb_render_prepare`: every segment, every road row in
whole pixels, the draw list with the columns each sprite touches), then draws
it in bands of internal RAM on both cores (`aos_hal_worker_split`), each band
copied out once. The LVGL timer (8 ms, core 1) pushes the newest frame.

- **Upright** a band is 17 rows of the screen; the frame goes out with
  `aos_hal_display_blit_scaled(0, 0, 720, 1280, fb, 1, false)`, the DMA2D
  copy.
- **Lying down** a band is a strip of 17 columns, all 720 rows. The strip is
  turned as it leaves the band: the screen's column x is one of the portrait
  panel's rows, so each strip lands as 17 whole rows of the frame, one
  contiguous block of PSRAM, and the frame goes out as it is with
  `aos_hal_display_blit_native()`. The PPA turning a whole frame took 62 ms
  (a cap of 15 fps); turning 17 columns in internal RAM is a strided read
  of 24 KB per strip. With `noturn` in `apps/turbo_dev.txt` the frames go out
  unturned and the PPA turns them, to compare the two.

Under the pause and the results the panels need the frame on an LVGL canvas,
which cannot show a turned one: the worker then draws one more, unturned.

### What to measure on the board

The app logs every 2 s while racing (portal `/api/log`):

```
turbo: 24.8 fps 720x1280 rows, prep 1.2 ms, bands 36.4 ms, frame 38 ms, push 20100 us, wait 3 ms/s, stage 0
```

`rows` / `turned` / `ppa` is the path; `prep` the step and the geometry,
`bands` the drawing on both cores, `push` the blit on the LVGL side, `wait`
how long the worker waited for a free buffer. Each race also appends its
totals to `/apps/turbo_stats.txt`. `apps/turbo_dev.txt` with `auto go 0`
has the bot race Metro Freeway as soon as the app opens (`unlock`, `reset`,
`noturn` as above).

Expected, not measured: the watch drew 165 000 pixels in 30-39 ms on one
240 MHz core; the P4 draws 921 600 on two 360 MHz ones, so the bands should
take 35-50 ms and the frame rate land near 20-25 fps upright, a little less
lying down (the strips cross every row). The simulator says nothing about
it (the Mac draws a frame in 3-5 ms).

## Racing another board

It rides on the ESP-NOW link (`aos_hal_link_*`), which the P4 does not have
yet. The menu asks `aos_hal_link_start()`; when it fails, the *Contra...*
button is not shown and nothing waits on the link. The protocol is the
watch's, untouched, so a P4 could race a watch once it has the link.

## Files

| File | What |
| --- | --- |
| `main/turbo.c` | life cycle, the worker and its jobs (`JOB_FIT` when the screen turns), the bands and the turning (`band_part`, `turn_out`), the push, touch, the panels and their layout for either orientation |
| `main/tb_render.c` | the pseudo-3D renderer: prepare once, then any rectangle of the screen |
| `main/tb_hud.c` | the HUD in the frame: clock, speed, wheel, arrows, pedals, and where they go (`tb_hud_layout`) |
| `main/tb_gfx.c` | pixels, native order; images with a stride (a strip of columns) |
| `main/tb_art.c` | the pack |
| `main/tb_track.c`, `tb_game.c`, `tb_audio.c`, `tb_link.c` | the watch's, unchanged but for the link's check |
| `tools/blender/` | the watch's Blender scripts with the P4's sizes (`--near-scale`, `--hscale`, `--bgscale`) |
| `tools/pack_p4.py`, `tools/lz4blk.c` | the pack |

## Simulator switches

`TB_STAGE=0..6` (a time trial of that stage), `TB_TOUR=1`, `TB_AUTO=1` (the
bot drives), `TB_COINS=n`, `TB_SCREEN=garage|settings|select`, and new:
`TB_TURNED=1`, lying down, draws the frames turned as the board does and
turns them back for the canvas (the simulator has no panel to blit to), and
`TB_DBG=1` adds the car's speed, place and the touch inputs to the 2 s log line.

## Preferences

The watch's (`tb_coins`, `tb_cars`, `tb_paints`, `tb_car`, `tb_pc<car>`,
`tb_diff`, `tb_sens`, `tb_sfx`, `tb_unl`, `tb_best<stage>`, `tb_tour`,
`tb_rb<stage>`, `tb_rname`), and new: `tb_ctl` (0 wheel, 1 arrows) and
`tb_auto` (the gas held).

## Pending

- Measure on the board: fps and PSRAM upright and lying down, `turned`
  against `noturn`.
- New strings (the controls' settings) are not in the language catalogs yet.
- More stages: [STAGE-IDEAS.md](STAGE-IDEAS.md).
