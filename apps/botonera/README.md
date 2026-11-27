# Botonera

A soundboard (`aos.botonera`; candidate 9 of group A in the apps' list): a
grid of big buttons that play the WAV and MP3 files of `<card>/sonidos`,
several at once.

```bash
# in the simulator (it sounds through aos_hal_spk_*; BN_SIM_TAP=<file> also
# writes what the mixer makes, raw 16-bit mono at 32 kHz)
cd sim && cmake -B build-botonera -DP4OS_SIM_APPS="hello_app;botonera" && cmake --build build-botonera -j8
P4_SIM_SCRIPT="wait 800; open aos.botonera" ./build-botonera/p4os_sim

# its page in the simulator's portal
mkdir -p sim/sim_fs/web && cp apps/botonera/web/botonera.js sim/sim_fs/web/

# the .so for the card
tools/build_apps.sh botonera
```

## What it does

- **Pages of twelve buttons** (3 x 4 upright, 4 x 3 lying down), up to
  eight. Each button has a sound, a name, a colour (twelve) and an icon
  (36, from the system's glyphs).
- **Several at once.** The grid does not go through LVGL's pointer, which
  follows one finger: a 10 ms timer reads every finger
  (`aos_touch_points_n`), as DJ does, and a finger landing on a button plays
  it on the touch, not on the release.
- **How a button plays:** *Cortar* (a touch plays it, another stops it),
  *Superponer* (every touch starts it again over what is playing),
  *Mantener* (it plays while the finger is on it). Any of them can loop (a
  looping button always stops with the next touch), and each has its own
  volume. *Parar todo* stops everything.
- **Editing** (the pencil): tap a button to change its sound, name, colour,
  icon, mode, loop and volume, to move it (then tap where it goes, on any
  page: the two swap) or to clear it. The page's name is tapped to rename
  it; + adds a page and the bin deletes one (two taps; the sounds stay on
  the card). An empty button opens the editor from the grid too.
- **Recording:** the editor's microphone (or *Grabar uno nuevo* in the list)
  records a WAV straight into the folder (`grabacion_NNN.wav`, 32 kHz, up
  to 60 s) with the firmware's recorder, and offers it for the button.
- **The examples:** the first time, the app synthesises sixteen sounds of
  its own into the folder (applause, horn, drum roll, ta-da, bell, correct,
  error, laser, boing, sad trombone, siren, tick tock, a beat, rain, wind, a
  heartbeat; 1.7 MB of 22 kHz WAV, under a second on the board) and a grid
  of two pages with them, named in the board's language. A missing example
  that the grid still names is made again. No recordings of anybody, no pack
  to download.
- It plays only while it is in front: leaving it stops everything and gives
  the speaker back to the music player.

## The portal's page

`web/botonera.js`, `#botonera`: the buttons, to play from the phone (a
pointer down sends `down N`, up sends `up N`, so *Mantener* works there too;
several fingers at once as well), what sounds on the board, *Parar todo*; in
*Editar* the same editor as on the board, page names and pages; and the
folder: upload (WAV and MP3, names made card-safe), listen, download, delete,
put on a button.

## How the pieces talk

| File | What |
|---|---|
| `bn_eng.c` | the mixer: its own thread (priority 6), up to 16 voices summed in blocks of 64 samples at 32 kHz into `aos_hal_spk_open_low`; a limiter so many at once do not clip; no locks (a ring of commands from the UI, state arrays only it writes) |
| `bn_dec.c` | WAV (8/16/24/32-bit, float, extensible, any channels and rate) and MP3 (minimp3, CC0, `main/minimp3/`, under names of its own) to 32 kHz mono in PSRAM, at most 60 s each |
| `bn_synth.c` | the example sounds |
| `bn_grid.c` | the grid's file, the icons and colours, the folder |
| `bn_ui.c` | the grid, the fingers, the loader thread and the cache of decoded sounds, the live channel |
| `bn_edit.c` | the sheets: a button's editor, the list of sounds, the recorder, the keyboard |

**Why it mixes by itself.** The firmware's speaker is one mono stream
(`aos_hal_spk_*`); `aos_hal_play_file` plays one file and cuts the last,
and the player is for music. Like DJ and Estudio, the app decodes and mixes
on its own and hands the sum to the speaker. Every sound is decoded once,
when its page is shown, and kept in PSRAM (12 MB at most, the least used go
first; a file that changed on the card is read again), so a touch only
points a voice at samples already there: it sounds at once and as many times
as wanted. A touch on a sound still decoding is kept and played as soon as
it is ready.

**The grid's file** is `<data>/botonera.txt`, written by the app and by the
page: a `P` line per page and a `B` line per button, tab-separated (slot,
file, name, colour, icon, `cut|over|hold`, loop, volume). The app reads it
again when its date changes (or when the page says `reload` through the live
channel), unless one of its sheets is open.

**The live channel** (`aos_hal_live_*`): the page posts `down N`, `up N`,
`stop N`, `stopall`, `page N`, `reload` (N is page x 12 + slot); the app puts
`state`, `{"front", "ready", "edit", "rec", "page", "rev", "play":
[[N, voices, permille]...]}`, while a page has asked in the last 4 s.

## Memory

All of it in PSRAM: the decoded sounds (64 KB a second), the grid, the UI's
state, the decoder's buffers and the threads' stacks (mixer 6 KB, loader
12 KB). Nothing asks for internal RAM.

## Not tried on the board yet

The latency from a touch to the speaker (10 ms of output plus the 8 ms
queue, on paper), the mixer's cost with 16 voices (a line `audio: ...` in the
log every 5 s while something plays), decoding a long MP3, and recording
while the speaker is closed.
