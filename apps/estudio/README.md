# Estudio

A synthesizer, a drum machine and a sequencer for the 720×1280 screen and
five fingers (`aos.estudio`; the request is G9 of the November 2026 batch).
It replaces the old candidates Drum machine, Synthesizer and Metronome.

```bash
# in the simulator (it sounds: the app opens SDL itself there, see "Sound")
cd sim && cmake -B build-estudio -DP4OS_SIM_APPS="hello_app;estudio" && cmake --build build-estudio -j8
P4_SIM_SCRIPT="wait 800; open aos.estudio" ./build-estudio/p4os_sim

# the .so for the card
tools/build_apps.sh estudio
```

## What is where

| File | What |
|---|---|
| `es_dsp.c` | the sound: oscillators (sine table, polyBLEP saw and square, triangle, noise), two-operator FM, the TPT state-variable filter, the ADSRs, the LFO, the synthesized drums, the samples, delay, reverb, drive, the master filter and the limiter |
| `es_engine.c` | the audio thread, the command queues, the transport and the sequencer (swing, probability, recording, the count-in), the arpeggiator, the voice allocation, MIDI out, the offline WAV export, the speaker |
| `es_patch.c` | the synth's parameters as one table (JSON key, name, range, curve) and the 14 factory presets |
| `es_proj.c` | the project model, its JSON and a small JSON parser (the firmware lends none) |
| `es_file.c` | WAV in (8/16/24/32-bit PCM and float, any rate, mono or stereo), WAV out, the MIDI file |
| `es_midi.c` | USB MIDI in and out, in a thread of its own |
| `es_ui*.c` | the six views: Tocar, Ritmo, Notas, Sonido, Mezcla, Canción |
| `web/estudio.js` | the portal page: projects, samples, exports and presets |

On the card: `estudio/*.json` (projects), `estudio/muestras/*.wav`,
`estudio/presets/*.json`, `estudio/export/`.

## Sound

- **48 kHz, mono, blocks of 32 samples (0.67 ms).** 48 kHz is the board's
  bus rate, so the HAL does not resample. The sequencer and the
  arpeggiator fire at block boundaries: 0.67 ms of resolution (swing measured
  exact to one block on the Mac, `136.0 / 83.3 ms` for `135.8 / 83.2`).
- **The thread** (`aos_hal_thread_start`, priority 6, core 0, its stack in
  PSRAM) keeps the speaker's ring at a target and sleeps 1 ms when it is
  full. The mixer takes 480 samples (10 ms) at a time, so the targets are
  480 + 96, 480 + 480 and 480 + 960 samples: Baja 12 ms, Normal 20 ms,
  Segura 30 ms. On the board the I2S DMA adds 6 × 240 frames = 30 ms and the
  mixer up to 10 ms more: **50-70 ms from a key to the speaker**, measured
  from the code, not with a microphone. That is the HAL, not the engine
  (the request is in `HANDOFF-NUEVAS-APPS.md`).
- **Memory:** everything in PSRAM, nothing internal. The delay line (2 s,
  375 KB) and the reverb (~35 KB) are walked in order, which the cache
  keeps up with; the voices and the project are small. The user's samples
  are converted to 16-bit mono, at most 12 s each and 12 MB in all.
- **Cost:** on the Mac a block with every voice and the effects takes
  ~6 µs. On the board it is not measured yet: the app logs every 5 s

  ```
  estudio: audio: block 32 smp (666 us), avg N us, max N us, load N %, voices N, underruns N, queue N smp, peak N %
  ```

  and Canción → Audio → **Prueba de carga** plays all eight voices, every
  drum and the effects for five seconds (`tools/plog.sh p4os.local`). If the
  load is too high, Voces 6 is the first step down. `es_dsp.c` and
  `es_engine.c` build with `-O2` (the rest with the apps' `-Os`).
- **In the simulator** `aos_hal_spk_*` swallows the samples, so under
  `AOS_SIM` the app opens SDL in queue mode, which behaves like the ring.
  `ES_SIM_TAP=<file.wav>` writes everything that goes out to a WAV (the
  header stays at 0 if the simulator quits without closing the app; the
  samples are there). `ES_SIM_MIDI="ms:status:d1:d2,..."` is a MIDI
  keyboard that plays by itself.

## MIDI

In (a keyboard on the USB host): channel 1 plays the part chosen on the
screen, 2 to 4 the parts 1 to 3, 10 the drums (GM notes to the track with
that sound); bend ±2, CC 1 modulation, 64 sustain, 74 cutoff, 71 resonance,
Start and Stop. Out: to a device on the host, the parts on channels 1-3,
the drums on 10 and the clock (24 a quarter, Start, Stop); to the computer,
one chosen track on channel 1 and no clock (`aos_hal_usb_midi_note` is
all there is).

## Not done

- Following an external MIDI clock (Start and Stop are followed, the tempo
  is not).
- The latency on the board and the audio thread's cost there: to measure.
