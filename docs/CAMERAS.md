# Cameras

The Cámaras app (`apps/camaras`, id `aos.cameras`) shows every camera of the
house at once and any of them full screen. It is AmoledOS's viewer
(`ESP32S3_AmoledOS/docs/CAMERAS.md` is the measured paper behind it) moved to
a 5" screen: the RTSP client, the RTP depacketisers and the lag policy are
the watch's; the mosaic, the threads, the frame hand-over, the snapshots and
the Frigate tab are new.

Everything below was built and measured in the simulator against local fake
cameras. **The board has not run it yet**: the last section says what only
the board can answer.

## cameras.txt

At the root of the card. The portal edits it (Archivos, **Editar:
cameras.txt**) and the app reloads it on save. The first time the app opens
without one it writes a commented template. One block per camera, up to 8:

```ini
[Entrada]
url  = rtsp://192.168.0.10:554/Streaming/Channels/102
full = rtsp://192.168.0.10:554/Streaming/Channels/101
user = admin
pass = secret

[Patio]
url = http://192.168.0.21:1984/api/stream.mjpeg?src=patio

[Portón]
url     = http://192.168.0.30/snapshot.jpg
refresh = 2

[Frigate]
url = http://192.168.0.20:5000
```

| Key | Meaning |
|---|---|
| `url` | What the mosaic keeps open. `rtsp://` (H.264 Baseline, or MJPEG over RTP), `http://` MJPEG (go2rtc, Frigate, a camera's own), or an `http://` URL that answers one JPEG and closes (a snapshot). |
| `full` | Optional: what the full screen opens instead (a camera's main stream while the mosaic uses its sub stream). Without it the full screen keeps the tile's connection. |
| `user`, `pass` | RTSP Digest or Basic, HTTP Basic. They may also come inside the URL (`rtsp://user:pass@host/…`); they are taken out before anything shows the URL. |
| `refresh` | Seconds between requests to a snapshot URL (default 1). |
| `[Frigate]` | A block with that name is not a camera: it turns on the Frigate tab. |

The same four fields per camera as AmoledOS's `/camaras` page kept in NVS;
the file is plain text, passwords included, like `ha.txt`. For development,
`CAM_CONFIG=/path/cameras.txt` in the simulator's environment reads another
file and `CAM_OPEN=<n>` opens the n-th camera straight away.

## What it looks like

- **Mosaic.** A 16:9 tile per camera, cropped to fill. The layout picks the
  column count that gives the tiles the most area: upright, one column up to
  about four cameras and two beyond; lying down, 2×2 for four (526×296 each
  under the header), 3×2 for six. Each tile says its codec and rate, or why
  it has no picture (red dot), and retries by itself.
- **Full screen.** Tap a tile. Fit (the whole picture) or fill (the whole
  screen, cropped), remembered. The overlay hides after 5 s and a tap brings
  it back: name, codec, size, fps, kb/s (and "audio sin reproducir" when the
  SDP offers a track), and buttons for previous/next camera, snapshot,
  fit/fill, turn the screen, back to the mosaic. The back gesture works too.
- **Turning.** The app declares no orientation, so it follows the system's
  (Control Centre). The turn button toggles it from the viewer
  (`aos_ui_request_landscape`); the app puts the person's orientation back
  when it closes. Lying down, a 16:9 camera fills the 1280×720 glass.
- **Snapshot.** A JPEG in `photos/camaras/<Name>-YYYYMMDD-HHMMSS.jpg`, which
  Fotos shows as the "camaras" album. MJPEG: the camera's own bytes, full
  size, no second compression. H.264: the next decoded picture at its own
  size, encoded at quality 90 (`aos_hal_jpeg_save`, esp_new_jpeg's encoder
  on the board).
- **Frigate tab.** The cameras from `/api/config` (disabled ones left out),
  each tile the `latest.jpg?h=360` every 2 s, full screen Frigate's MJPEG
  (`/api/<camera>?fps=10&h=720`). The events from `/api/events` with their
  thumbnails, label (in Spanish), score, camera and time, refreshed every
  20 s or by the button; tapping one shows its `snapshot.jpg`. Clips (MP4)
  are not played; the row only marks that there is one. Lying down, tiles
  on the left and events on the right; upright, events under the tiles.

## How it works

```
 camera thread (one per tile, aos_hal_thread_start, prio 3)        LVGL timer (15 ms)
 ┌──────────────────────────────────────────────────────────┐      ┌───────────────────────┐
 │ RTSP/HTTP session -> depay -> decode -> scale to the box ─┼─ 3 ─▶│ take newest frame,    │
 │   H.264: aos_hal_h264_* (tinyh264 / libavcodec)          │frames│ point the tile's or   │
 │   JPEG:  aos_hal_jpeg_* (P4 JPEG engine / libavcodec)    │      │ viewer's lv_canvas at │
 │   scale: cam_conv.c, nearest neighbour, RGB565 native    │      │ it                    │
 └──────────────────────────────────────────────────────────┘      └───────────────────────┘
```

- **One thread per camera.** The HAL's app worker is one task per app, and
  on the board it is still a stub; the mosaic needs several. Each thread
  runs the whole pipeline and never touches LVGL.
- **Three frames per camera** (back, ready, front) that change hands under a
  mutex: the newest frame always wins and neither side waits. The thread
  scales straight to the size the UI asked for (the tile, or the screen with
  fit/fill), so LVGL draws an image at 1:1 (`lv_canvas`, no LVGL scaling but
  for the instant after a size change). Going through LVGL instead of a
  direct blit also puts the video in the portal's capture and in the
  simulator's screenshots.
- **The mosaic's rate.** H.264 tiles decode **keyframes only** (P frames are
  dropped before the decoder): one refresh per GOP, a second with the camera
  at GOP = fps, and almost no CPU. MJPEG tiles decode at most ~5 frames a
  second (newest wins). The full screen decodes everything with AmoledOS's
  policy: past 700 ms late, skip to the next keyframe.
- **Opening a camera keeps its connection**: the view is re-targeted to the
  screen size and to "everything". An H.264 view then waits for the next
  keyframe (its P frames need references it skipped) with the tile's
  picture on screen, enlarged. The other tiles let go of their connections
  while the viewer is open, and take them again on the way back. A `full`
  URL opens a connection of its own.
- **Stopping never blocks the UI.** A released view is freed by whichever of
  the thread and the UI lets go last. Only `destroy()` waits for all threads
  (the .so is unloaded right after); connects go in one-second tries that
  check the stop flag, so a dead camera holds the exit about a second, not
  the 5 s connect timeout.

### HAL added for it

| Call | Board | Simulator |
|---|---|---|
| `aos_hal_jpeg_open/decode/close` | `aos_jpeg_p4.c`: the P4 JPEG engine with a per-stream DMA in/out buffer, channel order learnt from the same probe picture as `aos_image_p4.c`; esp_new_jpeg in software for progressive or refused frames | `sim/jpeg_sim.c`, libavcodec |
| `aos_hal_jpeg_save` | esp_new_jpeg encoder, RGB565 LE in | libavcodec MJPEG encoder |
| `aos_hal_h264_*` (existing) | `aos_h264.c` over `espressif/esp_h264` 1.4.1 (tinyh264), now compiled in | `sim/sim_codec.c`, libavcodec |

`aos_tcp.c` had a race that only multiple threads show: a slot was chosen
before the connect and filled after it, so two concurrent connects could get
the same handle (seen as tiles dropping each other's connection). The slot is
now reserved atomically, and there are 6 slots instead of 4 (MQTT, Modbus TCP
and the scope hold theirs too; lwIP's `CONFIG_LWIP_MAX_SOCKETS` is 10).

## Measured in the simulator (2026-09-28)

MacBook, SDL simulator, fakes on 127.0.0.1. Viewer numbers are the app's own
log line (`[camaras] <name>: N shown/s …`, every 10 s).

| Stream | Viewer, upright | Viewer, lying down | Mosaic tile |
|---|---|---|---|
| H.264 704×576 @ 12 (the doorbell's mode) | 12 fps → 720×588 | 12 fps → 880×720 | 1 fps (GOP 12) |
| H.264 1280×720 @ 15 | 15 fps → 720×404 | 15–16 fps → 1280×720 | 1 fps |
| H.264 1920×1080 @ 25 | 25 fps → 720×404 | 25 fps → 1280×720 | 1 fps |
| MJPEG 640×360 @ 12 | 12 fps | 12 fps → 1280×720 | 4 fps |
| MJPEG 640×480 @ 10 | 10 fps | 10 fps → 960×720 | 5 fps |
| MJPEG 1280×720 @ 25 (3.4 Mb/s) | 25 fps | 25 fps | — |

Lag stayed under 15 ms, nothing dropped. Decode and scale are below the
simulator's millisecond clock except the scale to 1280×720 (≈5 ms). None of
this says anything about the board's speed: libavcodec decodes Main/High
profile, CABAC and B-frames too, on a desktop CPU.

Memory the app takes, from its buffers (all heap, PSRAM on the board):

| | Per camera |
|---|---|
| frames, mosaic tile (3 × w×h×2) | 0.8 MB upright (480×270), 0.9 MB lying down (526×296) |
| frames, full screen | 5.5 MB lying down (3 × 1280×720×2) |
| MJPEG session | 1 MB (receive + newest-JPEG buffers) + the decoder's output |
| RTSP session | 0.6 MB (receive + NAL buffer) + tinyh264's pictures (1.4 MB at 704×576, 2.9 MB at 720p on the watch) |
| thread stack | 12 KB internal RAM |

Four cameras in the mosaic come to roughly 10 MB of PSRAM; a full-screen
720p H.264 camera to about 9 MB.

## The fakes

All three bind 127.0.0.1 only and draw the same picture: a light room with
Mila, the black kitten, walking across it, the camera's name, the time and a
frame counter (a frozen or dropped frame shows). ffmpeg makes the frames at
the camera's rate.

| Tool | What |
|---|---|
| `tools/fake_rtsp.py --cam timbre:704x576:12 [--profile main] [--user u --pass p] [--audio]` | RTSP over TCP interleaved: OPTIONS, DESCRIBE (sprop-parameter-sets), SETUP, PLAY, GET_PARAMETER, TEARDOWN; Digest without qop; single NAL and FU-A; libx264 baseline, one slice per picture, SPS/PPS before each keyframe, GOP = fps. `--audio` offers a PCMU track that is never sent. |
| `tools/fake_mjpeg.py --cam patio:640x360:12 [--chunked]` | `/<name>.mjpeg` multipart, `/<name>.jpg` one picture. `--chunked` answers like ffmpeg's own server. |
| `tools/fake_frigate.py --cam entrada:1280x720:10` | `/api/config` (one camera disabled), `/api/events`, event `thumbnail.jpg` / `snapshot.jpg`, `/api/<camera>/latest.jpg`, `/api/<camera>` MJPEG; a new event every `--every` seconds. |

```bash
tools/fake_rtsp.py --port 18554 --cam timbre:704x576:12 --cam exterior:1280x720:15 --audio &
tools/fake_mjpeg.py --port 18081 --cam patio:640x360:12 --cam cocina:640x480:10 &
tools/fake_frigate.py --port 15000 &
cd sim && CAM_CONFIG=/path/cameras.txt ./build/p4os_sim
```

Multi-slice pictures (x264's `sliced-threads`, some cameras) make the
simulator's libavcodec hand out a picture per slice ("concealing … errors"
in the log); the board's tinyh264 assembles them. The fake sends one slice
per picture.

## What only the board can confirm

- **tinyh264 on the P4** (360 MHz RISC-V, no SIMD in the library): the
  watch did 704×576 at 17 fps decoder-alone, 1280×720 at 8.6. The P4 should
  do better; by how much decides whether 720p cameras need their sub stream
  in the viewer, and whether the mosaic could afford every frame of one
  H.264 camera instead of keyframes only.
- **The JPEG engine on camera frames**: its padding rule (the stride comes
  from the byte count it reports), its speed, and the fallback to software.
- **The scale in C**: 1280×720 output is ~0.9 Mpx per frame through the
  column map; if it shows in the frame time, the PPA scales RGB565 (and
  converts I420) in hardware and is the obvious next step.
- **LVGL drawing a 1280×720 canvas per frame** and the flush through the
  PPA rotation lying down: the watch pushed pixels past LVGL because its
  render cost 95 ms a frame; the P4's DPI framebuffer should not need that,
  but it has to be measured.
- **The network**: lwIP's 5.7 KB TCP window (`CONFIG_LWIP_TCP_WND_DEFAULT`)
  capped MJPEG on the watch; with the C6 in between it may again. Four
  MJPEG tiles at 3-4 Mb/s each is 12-16 Mb/s over the SDIO link.
- **Internal RAM** for four camera threads (12 KB of stack each) and
  tinyh264's internal-RAM peak (60-105 KB on the watch).
- **The .so itself**: board builds of dynamic apps are not set up yet
  (`build_apps.sh` still calls Xtensa's `nm`, the apps' `sdkconfig.defaults`
  still say esp32s3, `aos_symbols.c` is the watch's). The app's sources
  compile with the firmware's RISC-V GCC and flags; the new HAL calls are in
  the firmware, but only reachable once the symbol table is regenerated.

Not done: audio (the SDP's track is noticed, not set up or played), Frigate
clips, PTZ, recording.
