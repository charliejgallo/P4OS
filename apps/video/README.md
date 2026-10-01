# Video

Plays MJPEG AVI files from the card, full screen, with sound, upright or
lying down. Ported from AmoledOS, where it played 368x448 at 12-15 fps; on
the P4 the frames are decoded by the chip's JPEG engine and scaled onto the
screen by the PPA, so the same format goes up to the screen's own size.

```bash
# in the simulator
cmake -S sim -B sim/build-video -DCMAKE_BUILD_TYPE=Release -DP4OS_SIM_APPS=video
cmake --build sim/build-video -j8
cd sim && P4_SIM_SCRIPT="wait 800; open aos.video" ./build-video/p4os_sim

# the .so for the card
tools/build_apps.sh video                     # apps/video/build/video.so -> /apps on the card
```

## Making the videos

An **AVI of baseline 4:2:0 JPEG frames**, and beside it a **WAV** (16-bit
PCM; an MP3 also works) with the same name, both in the card's `videos`
folder. `convert.sh` makes both from anything ffmpeg reads:

```bash
apps/video/convert.sh clip.mp4                 # full: fits 1280x720 (wide) or 720x1280 (tall), 24 fps
apps/video/convert.sh clip.mp4 half            # 640x360 / 360x640 at 30 fps, doubled by the PPA
apps/video/convert.sh clip.mp4 full 24 6 out/  # mode, fps, JPEG quality (2 best .. 31), folder
```

Which is what these two lines do by hand, for a wide clip (swap 1280 and
720 for a tall one):

```bash
ffmpeg -i clip.mp4 -vf "scale=1280:720:force_original_aspect_ratio=decrease:force_divisible_by=2,fps=24" \
       -pix_fmt yuvj420p -c:v mjpeg -q:v 6 -an clip.avi
ffmpeg -i clip.mp4 -vn -ac 1 -ar 32000 -c:a pcm_s16le clip.wav
```

The shape is kept: a video that does not fill the screen is letterboxed.
The app takes any size and any frame rate, and scales each frame to the
biggest size that fits (in the PPA's steps of 1/16, up to x4):

| File | Upright (720x1280) | Lying down (1280x720) |
| --- | --- | --- |
| 1280x720 | 720x405, the whole width | the whole screen, 1:1 |
| 640x360 (`half`) | 720x405, the whole width | the whole screen, x2 |
| 720x1280 | the whole screen, 1:1 | 405x720, the whole height |
| 360x640 (`half`) | the whole screen, x2 | 405x720, the whole height |
| 368x448 (the watch's) | 713x868 | 575x700 |

A wide video plays big lying down; upright it is letterboxed. When the
screen turns, the video goes on in the new orientation.

Files over 1 GB (OpenDML, `AVIX`) play too: the index is read from `indx`,
else from `idx1`, else the chunks are walked once at open. FAT32 stops a
file at 4 GB.

## Using it

- **The list**: one card per video with its first frame, its length, size
  and frame rate, and "Sin sonido" when there is no WAV beside it. One
  column upright, two lying down.
- **The player**: the picture, full screen. A **tap** on the picture pauses
  and brings up the controls; another tap plays on, and the controls go
  away after a moment (three seconds after the last touch).
- **The controls**: on top, back to the list, the name and the numbers; at
  the bottom the **seek bar** (drag it: the picture follows the finger, the
  sound joins where it is let go), the time, and **-10 s / play-pause /
  +10 s**. At the end the last frame stays with the controls up; play
  starts it again.
- **Back**: the button, or the system's swipe from the left edge.

Everything is touch: the board has no buttons and no motion sensor.

## How a frame gets to the screen

Three stages at once (`main/video.h` has the details):

1. **reader** (a thread, `aos_hal_thread_start`): reads frame *n*'s JPEG
   with one `fread` at the place the AVI's index says, into a ring of six
   packets (`aos_hal_io_alloc`, placed so FatFS's whole-sector reads land on
   the SDMMC DMA's 128-byte lines).
2. **worker** (`aos_hal_worker_start`, core 0): decodes each packet with
   `aos_hal_jpeg_decode()` - the **P4's JPEG engine**, with esp_new_jpeg in
   software for what the engine refuses - into one of three frame slots,
   each with its own decoder handle (the pixels are the handle's until its
   next decode). Nothing else is done to the pixels.
3. **LVGL's timer**: when the frame's time comes by the clock,
   `aos_hal_display_blit_fit(x, y, dst_w, dst_h, px, w, h, stride, false)` -
   little-endian, the biggest size that fits, centred, reading the engine's
   rows padded to 16 px as they are. The PPA scales (up or down), turns for
   the orientation and writes the framebuffer in one pass. A whole-number
   scale of packed rows goes by `aos_hal_display_blit_scaled()` instead
   (1:1 unrotated is DMA2D there). With the controls up only the rows
   between the two bars are blitted. With the bars down and nothing over
   the picture the frame goes into the panel's free buffer instead and is
   flipped to (no tearing): scaled or turned by the PPA
   (`aos_hal_display_blit_into_fit`), and 1:1 upright the screen's width
   as whole rows, which the HAL copies by the AXI DMA (19 ms for 720x1280;
   the rows off a 128-byte line, when the picture does not start on a row
   multiple of 8, by the CPU). 1:1 upright narrower than the screen stays
   the DMA2D blit into the buffer on screen.

**The sound is the clock**: the player reports its position to the
millisecond (`aos_hal_player_info`), and the video's clock jumps to it when
they drift more than 45 ms apart. Late frames are never read nor decoded:
the reader and the worker are told where to skip to.

In the simulator the blit says no, and the same frames go through an
`lv_image` at the same place and scale: the screens there are what the
board shows.

## Measured on the board (2026-09-29)

With the JPEG engine's probe fixed (every frame had been going to software)
and 1:1 unrotated blits by DMA2D, before `aos_hal_display_blit_fit`:

| File, shown | fps | read | decode | blit |
| --- | --- | --- | --- | --- |
| 640x360 30 fps, landscape x2 | 30 (the worker waited 460 ms of every 500) | 4 ms | 3 ms | 10.6 ms |
| 720x1280 24 fps, portrait 1:1 | 23.8 (its own 24) | 6 ms | 10 ms | 13.4 ms |
| 1280x720 24 fps, portrait, halved on the CPU | 18 | 32 ms | 52 ms (with the halving) | 18 ms |

No screen flashes in any of them. The halving is gone now: that last one
goes to the PPA whole, scaled by 9/16 to 720x405. The stages overlap, so a
frame costs the slowest of the three, not their sum.

The app logs a line every 2 s (`/api/log`):

```
video: 23.9 fps  r6 d10 b13.4 l3 ms  -0  1280x720>720x405 | starved 0, waited 310 ms in 502, errors r0 w0
```

`r` card read, `d` decode, `b` blit, `l` how late the frames went up,
`-n` frames skipped, `WxH>WxH` the file and the size it goes up at; `starved` is the worker waiting for the card, `waited` the
worker waiting for the screen (a full ring: that is the good case). The top
bar shows the same line with the controls up.

## Pending on the board

- 1280x720 upright through `blit_fit` (9/16), and `full` at 30 fps.
- Lip sync: the clock follows what the player has handed to the output;
  if the picture runs ahead by the I2S buffers, a fixed offset goes into
  the clock in `vd_play.c` (`clock_ms`).
- A 360-wide frame through the engine: the blit reads the stride the HAL
  reports (`aos_jpeg_p4.c`'s padding rule, unconfirmed at that width).

## Asked of the OS (not done here)

- A public **"something of the system is over the app"**: the app reads
  `aos_panel_current()`, `aos_switcher_is_open()` and `aos_ui_geo()` from
  `aos_internal.h` (they are in the symbol table) to stop blitting over the
  control centre and the switcher. Banners are not covered.
- `open` / `read` / `lseek` / `close` in the symbol table, so an app can
  read the card the way docs/MEMORY.md recommends; this app uses `fread`.
