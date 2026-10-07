# RF

Software radios and transceivers for P4OS. An **RTL-SDR** (an RTL2832U USB
stick) on the board's USB host gives a spectrum and waterfall - drag to tune,
tap a frequency to centre it, tap the number on top to type one, a row of
bands to jump to - and **listens**: broadcast FM, AM (the airband) and narrow
FM (amateurs, PMR, marine), through the board's speaker or a USB sound card,
with a squelch. It also decodes the 433 and 868 MHz remotes and sensors
around, records all of it, and has a page in the portal. What comes next (a
CC1101 on the header, FSK) is in `docs/plan/RF.md`.

## Listening

The row above the rates chooses: **No audio** (the spectrum only: the stream
rests while the board is locked or the app is behind another), **FM**
(broadcast, mono, 75 us de-emphasis, the stereo pilot shown as "stereo"),
**AM** and **Narrow FM**. With a mode on, the radio goes on with the screen
locked and with another app in front. The bands choose their mode too.

The frequency listened to is the centre, the red line; the channel's width is
shaded around it. The squelch (AM and narrow FM; default 10 dB) mutes the
audio while the channel is less than that over the noise floor of the
spectrum; its sheet shows the signal live. While listening the rate is one of
240 k (the default for AM and narrow FM), 960 k (broadcast FM's default, more
spectrum on screen) or 1.92 Msps.

Measured on the board on 2026-10-07 (the spectrum running, core 0):

| | Radio thread | Core 0 |
|---|---|---|
| FM at 240 k | 25 % | 40 % |
| FM at 960 k | 59 % | 76 % |
| AM at 240 k | 19 % | 37 % |
| Narrow FM at 240 k | 18.5 % | 36 % |

A real FM station (98.3 MHz) gave the stereo pilot 36 to 43 dB over its
neighbours at both rates: the demodulator is right on air, not only on the
test signals. FM was listened to on the board the next morning: it sounds
right. AM and narrow FM wait for real traffic.

## Data: 433 and 868 MHz

**Data** (and the 433 and 868 bands, which choose it) listens to the band
around the centre at 240 ksps for on-off keyed transmissions - the remotes
of gates, alarms and doorbells, cheap weather sensors - and lists what it
gets where the waterfall was: the time, what it is, how many repeats, and
its values; a tap shows the rest (frequency to the kHz, signal over the
noise, modulation, pulse widths, the bits). It goes on with the screen
locked, like listening.

`rf_ook.c` finds the pulses (the power over the noise floor, sliced in the
middle in dB between noise and marks, with hysteresis and a 40 us debounce)
and reads them as rtl_433's analyser does: PWM, PPM or Manchester, from the
widths. Decoded by name: **EV1527** and **PT2262** remotes, **Nexus**-type
(the many brands of 36-bit PPM thermo-hygrometers) and **Prologue**
sensors. Anything else still shows its modulation, widths and bits, so a
new protocol can be recognised (and written) from what the app shows.
Repeats of the same sender and button within 2 s are one line with a count,
and a piece of the last repeat (cut short when the button was let go) is
not listed. A code that is valid as both EV1527 and PT2262 (one EV1527 in
30 is) is named by its last four symbols - a PT2262 remote's data pins are
driven, never floating - and its details show the other reading.

Tried on the board on 2026-10-07 with a copier remote (EV1527, two buttons,
21 dB over the noise): every press listed with its button, 24 and 25
repeats, as an I/Q recording of it confirms on the Mac (`test/README.md`).

Each new one goes to the card, `rf/datos-<day>.csv` (on by default), and,
when asked and the board's MQTT is connected, to
`<board name>/rf/<protocol>-<id>` as JSON (temperature, humidity, battery,
button, the bits).

## Keeping and playing back

The save button (the disk, second row) opens:

- **Screenshot**: the screen as the panel shows it, spectrum included, to
  `photos/Capturas` (`aos_hal_display_save`).
- **Record the audio** (while listening): a WAV in the Recorder's folder.
- **Record the signal (I/Q)**: the raw samples at the present rate to
  `rf/iq/<when>_<Hz>_<sps>.cu8`, with a `.txt` beside it, through a 4 MB ring
  and a writer thread of its own (the card writes ~3 MB/s: 2.4 Msps is
  4.8 MB/s, 240 k is 0.5). Changing the rate ends it.
- **Play a recording**: the app runs on the file as if it were the radio
  (data mode decodes it, listening modes demodulate it); tuning is off
  until it stops.
- The CSV and MQTT switches above.

## The page in the portal

`web/rf.js`, at `http://p4os.local/#rf` while the app is open on the board
(the page offers to open it): the frequency and its steps, the modes, rate,
gain, step and squelch, the bands; the spectrum with its own waterfall drawn
by the browser (a click tunes there, the wheel over the spectrum moves a
step); Data mode's list, a row opening its pulses and bits; the save
buttons (the board's screenshot, a PNG of the spectrum from the browser, the
recordings), and the recordings and CSVs to play, download or delete. It
talks to the app through the portal's live channel (`docs/PORTAL-PAGES.md`,
"Live data"); while a page looks the radio goes on even with nothing to
listen to and the board locked.

## Wiring

The RTL-SDR goes on the USB host, as a pendrive does (`docs/USB.md`, "The
USB host"): turn on **Settings → USB → USB host**, and wire a USB-A socket
to the back header.

| | Pins 25/27 (High Speed) | Pins 21/23 (Full Speed) |
|---|---|---|
| Sample rates | 0.96, 1.44, 1.92, 2.4 Msps (listening: 0.24, 0.96, 1.92) | 0.24 Msps |
| What fits on screen | 1 to 2.4 MHz at once | 240 kHz at once |
| Wires | **shorter than 15 cm** | any |

5 V from pin 1, ground on pin 5, on either port. An RTL-SDR draws about
300 mA: the stick runs warm, and a powered hub is kinder to the board's 5 V.

## How it is built

- **The firmware knows nothing of radios.** It lends the app raw access to a
  USB device (`aos_hal_usb_raw_*`, `docs/USB.md` "Raw devices"): control
  transfers and a stream of bulk transfers into a ring in PSRAM. The driver
  is in the app.
- **librtlsdr** (`main/rtlsdr/`) is that driver: Steve Markgraf's and
  osmocom's library, the one nearly every RTL-SDR program uses, with all its tuners (R820T/R820T2,
  R828D, E4000, FC0012, FC0013, FC2580), the RTL-SDR Blog V4 included.
- **`main/port/`** is libusb for it, on the board's raw USB: the device
  list is the host's, control transfers are the HAL's, and the samples do
  not go through libusb's async API but through the HAL's stream. Every
  libusb name is mapped to an `rfusb_` one, so in the simulator, which
  links the Mac's real libusb, the two never meet.
- **Sources** (`rf.h`): the app works on cu8 I/Q from an `rf_src_ops_t`.
  The RTL-SDR is the first one (`rf_src_rtl.c`); an rtl_tcp server, another
  SDR on USB or a recording on the card are more of them, not another app.
- **Two halves:** the engine, a thread of its own on core 0 at priority 1
  (the system's tick thread's: at 3 a busy engine starved it and the hang
  watchdog restarted the board), owns the source - every retune is a few
  dozen USB transfers - and reads every sample off the ring: all of them go
  through the demodulator, some into spectra (25 a second, each the average
  of 4 FFTs of 2048 points, `rf_dsp.c`). LVGL's timer draws the trace and a
  row of the waterfall, straight to the panel. The waterfall is a ring of
  twice its height, each row written twice, so it scrolls by moving a
  pointer.
- **The demodulator** (`rf_demod.c`, its chain in the comment at the top):
  16-bit filters on the P4's SIMD through esp-dsp (`docs/APPS-P4.md`, "DSP"),
  float only for the discriminators. The audio goes to `aos_hal_spk_*` at
  48 kHz (FM) or 24 kHz (AM, narrow FM), kept between 50 and 200 ms queued:
  the stick's and the audio's crystals drift apart, so a sample is dropped
  or doubled now and then, and a block is dropped if the engine catches up
  after falling behind.
- The app remembers the frequency, rate, gain, step, mode and squelch.
- **`rf/control.txt`** on the card (lines of `key=value`: `freq` in Hz or in
  MHz with a point, `mode` off/wfm/am/nfm, `rate`, `gain` in tenths of a dB
  or `auto`, `sq`, `step`) tunes the app when it changes: what the portal's
  page will use. `simd=0` and `fft=0` switch the SIMD off, to measure.

## Measured on the board

2026-10-06, the same stick on pins 25/27: 2.048 Msps at 4.1 MB/s and
2.4 Msps at 4.8 MB/s, no sample lost; 25 frames a second with 25-35 % of
core 0 and 8-15 % of core 1; an FFT of 2048 points in 1.35 ms. Unplugged
with the app open, the stream stopped clean and the app took the stick back
by itself within a second of plugging it in again.

Getting there: through LVGL the spectrum and the waterfall (620 000 pixels
out of PSRAM every frame) and an FFT working in PSRAM cost 93/97 % of the
two cores for 12 frames a second, 4.3 ms an FFT. Now the trace is redrawn
only where it moved, both pictures are blitted straight to the panel
(`aos_hal_display_blit_scaled`; through LVGL only while something is over
the app, and in the simulator), and the FFT works in 28 KB of internal RAM.

## Tests

`test/README.md`: the demodulator on the Mac against synthetic signals with
neighbours, noise and a mistuned carrier, and in the simulator with a file
as the source.

## Trying it in the simulator

The simulator drives the real stick plugged into the Mac, through libusb
(`brew install libusb`), only when asked:

```bash
P4_SIM_USB=1 sim/build/p4os_sim
```

With `P4_SIM_USB=1` the simulator lists the Mac's USB devices and the host
reads as on; it opens only what an app asks for, and the RF app only asks
for the RTL-SDRs librtlsdr knows. Measured on 2026-10-06 with a generic
RTL2832U + R820T (`0bda:2838`) on a Mac: 2.048 Msps, 4.0 MB/s, no sample
lost, the FM band's stations on screen.

## Licence

librtlsdr is **GPL-2.0-or-later** (`main/rtlsdr/COPYING`, authors in
`main/rtlsdr/AUTHORS`), so **`rf.so` as a whole is GPL-2.0-or-later**, as
`doom.so` is; the repository at a release's tag is its source. The firmware
that loads it, and its raw USB, stay MIT.

The library is version 2.0.3, from
`https://github.com/steve-m/librtlsdr/archive/refs/tags/v2.0.3.tar.gz`
(SHA-256 `851b87a62e548470c287c26669b83abb665d83bccb8d8492d07a697c7b9c4e37`,
the tarball Homebrew builds), copied as it is but for these changes:

- `librtlsdr.c` and each `tuner_*.c`: one line, `#include "rf_port.h"`,
  after their own includes. It sends their `fprintf(stderr, ...)` messages
  to the board's log (tag `rf`, the portal's Registro) and their `usleep`
  through the HAL.
- `librtlsdr.c`: at its end, `rtlsdr_p4os_usb_handle()`, which hands the
  app the open device's handle, so it can stream from it through the HAL.

Only the library's sources and headers are here (`src/librtlsdr.c`,
`src/tuner_*.c` and `include/*.h`); its programs (rtl_fm, rtl_tcp...) and
build files are not.
