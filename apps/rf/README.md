# RF

Software radios and transceivers for P4OS. The first part is a spectrum and
waterfall from an **RTL-SDR** (an RTL2832U USB stick) on the board's USB
host: drag to tune, tap a frequency to centre it, tap the number on top to
type one, and a row of bands to jump to. The plan for the rest (listening
to FM and AM, decoders at 433 MHz, a CC1101 on the header) is in
`docs/plan/RF.md`.

## Wiring

The RTL-SDR goes on the USB host, as a pendrive does (`docs/USB.md`, "The
USB host"): turn on **Settings → USB → USB host**, and wire a USB-A socket
to the back header.

| | Pins 25/27 (High Speed) | Pins 21/23 (Full Speed) |
|---|---|---|
| Sample rates | 1.024, 1.8, 2.048, 2.4 Msps | 0.25 Msps |
| What fits on screen | 1 to 2.4 MHz at once | 250 kHz at once |
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
- **Two halves:** the worker (core 0) owns the source - every retune is a
  few dozen USB transfers - and reads every sample off the ring, turning
  some into spectra (25 a second, each the average of 4 FFTs of 2048
  points, `rf_dsp.c`); LVGL's timer draws the trace and a row of the
  waterfall. The waterfall is a ring of twice its height, each row written
  twice, so it scrolls by moving a pointer.
- The app stops the stream while it is in the background (the stream's
  transfer buffers, 64 KB of internal RAM, are freed then) and remembers
  the frequency, rate, gain and step.

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
