# DJ

Two decks and a mixer that decode their own tracks (`aos.dj`; the request is
G22 of the November 2026 batch, "mostrar lo que puede el P4").

```bash
# test tracks of our own (synthesized, known tempo) into the simulator's card
python3 apps/dj/tools/gen_tracks.py sim/sim_fs/music --mp3 --flac

# in the simulator (it sounds through aos_hal_spk_*, P4OS 0.32 and later)
cd sim && cmake -B build-dj -DP4OS_SIM_APPS="hello_app;dj" && cmake --build build-dj -j8
P4_SIM_SCRIPT="wait 800; open aos.dj" ./build-dj/p4os_sim

# the .so for the card
tools/build_apps.sh dj
```

## What is where

| File | What |
|---|---|
| `dj_engine.c` | the audio thread: the decks (playhead, motor, scratch, cue, hot cues, loops, sync), the strip (EQ through `dsps_biquad_f32_arp4` on the board, filter, effects), the mixer, the recording ring, the timing |
| `dj_track.c` | opening MP3 (the frame index), WAV and FLAC; the cache of decoded chunks around the playhead; the I/O thread that fills it and writes the recording |
| `dj_ana.c` | the analysis thread: the waveform (peak and three bands every 256 samples), the loudness, the tempo and the beat grid |
| `dj_mp3.c` | minimp3 (CC0, `main/minimp3/`) under names of its own |
| `dj_flac.c` | a small FLAC decoder of our own, seeking by bisection |
| `dj_meta.c` | what is remembered per track: the grid set by hand, the cue, the hot cues |
| `dj_ui.c` | the surface: every control drawn and touched by hand, five fingers |
| `dj_wave.c` | the scrolling waveforms and the overviews, RGB565 canvases |
| `dj_lib.c` | the sheets: the library with search, the settings, a deck's tempo and grid |
| `dj_midi.c` | a USB MIDI controller, learned control by control |
| `tools/gen_tracks.py` | synthesized test tracks with a known tempo and first beat |

On the card: `<data>/dj/tracks.txt` (tempo, grid, cue and hot cues per
track), `<data>/dj/midi.txt` (the learned controls); the mixes go to the
recordings folder as `mezcla-<date>.wav` (stereo, 16-bit, 48 kHz).

## How it works

**Memory.** A five-minute track decoded whole is 50 MB of stereo samples:
the decks keep only chunks of 32768 samples, 24 per deck (3 MB, about 18 s
of a 44.1 kHz track), the ones around the playhead and the ones under the
cue, the hot cues and the loop, so a jump there plays at once. The I/O
thread decodes the most urgent missing chunk first; a chunk that is not
there plays as silence and counts as a miss in the log. The engine takes
about 6 MB of PSRAM, plus 1 MB per deck once its echo is used and 512 KB
once a mix is recorded, and the waveform of each track (4 bytes every 256
samples: 330 KB for eight minutes).

**MP3 seeking.** Opening an MP3 reads every frame's offset once (the whole
file, headers only). A chunk is decoded from its first frame minus three
(the bit reservoir and the overlap), or straight on when the decoder is
already there.

**Tempo.** An onset envelope from the low and mid bands, folded over one
beat for each candidate from 86 to 176 BPM; the candidate whose fold piles
up the most is the tempo, the tallest bin the grid's phase, and the plain
energy of kick and snare decides between the beat and the off-beat. On the
five test tracks it finds the tempo exactly and the first beat within 4 ms
(the MP3s carry the encoder's delay, ~2260 samples, in the decoded audio).
A grid fixed by hand in the tempo sheet is kept; a detected one is detected
again on every load.

**Sound.** One speaker, mono: the board plays the sum of both sides; the
recording is stereo. The audio thread renders 64-sample blocks and keeps
the speaker's queue at 8 ms on the live output (`aos_hal_spk_open_low`), 20
on the usual one. A log line every 5 s says what a block costs (average,
worst, and the share of its 1333 us), the speaker's queue, the blocks the
board's mixer found short (`dry`), the samples that were not in the cache
(`misses`), and the UI's frame rate and drawing time.

## Not yet measured on the board

The audio thread's cost with both decks, effects and scratching; the
analysis time of a five-minute track; the UI's frame rate with both
waveforms scrolling (two RGB565 canvases redrawn 30 times a second, ~13 MB/s
of PSRAM writes: if the panel flickers, the waveforms go to 20 fps).
