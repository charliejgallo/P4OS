#!/usr/bin/env python3
"""
gen_iq.py <mode> <rate> <out.cu8> [seconds]

Synthetic I/Q for the demodulator's tests (apps/rf/test/README.md), as an
RTL2832 gives it: unsigned 8-bit I and Q, 127.4 the zero, with noise, a DC
offset and the tuner a little off frequency.

  wfm  a station at the centre: a 1 kHz tone at 30 % of 75 kHz deviation and
       the 19 kHz stereo pilot at 10 %, pre-emphasised (75 us); another
       station 200 kHz above with a 3 kHz tone, 6 dB stronger
  am   a carrier 1.2 kHz off the centre (a tuner's error), 1 kHz at 50 %;
       another carrier 25 kHz away with 400 Hz, as strong
  nfm  a carrier 600 Hz off, 1 kHz tone at 2.5 kHz deviation, keyed on for
       the middle half of the time only (the squelch has to close outside);
       another 12.5 kHz away with 2 kHz
"""
import sys
import numpy as np

mode, rate, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
n = int(rate * secs)
t = np.arange(n) / rate
rng = np.random.default_rng(1)


def fm(f0, audio, dev):
    return np.exp(1j * (2 * np.pi * f0 * t + 2 * np.pi * dev * np.cumsum(audio) / rate))


def preemph(x, us):
    # the inverse of a one-pole de-emphasis, to first order: x + tau dx/dt
    tau = us * 1e-6
    return x + tau * np.gradient(x) * rate


if mode == "wfm":
    mpx = 0.3 * preemph(np.sin(2 * np.pi * 1000 * t), 75) / 1.0 + 0.1 * np.sin(2 * np.pi * 19000 * t)
    other = 0.3 * np.sin(2 * np.pi * 3000 * t)
    s = 0.25 * fm(0, mpx, 75000) + 0.5 * fm(200000, other, 75000)
elif mode == "am":
    s = 0.25 * (1 + 0.5 * np.sin(2 * np.pi * 1000 * t)) * np.exp(2j * np.pi * 1200 * t)
    s += 0.25 * (1 + 0.5 * np.sin(2 * np.pi * 400 * t)) * np.exp(2j * np.pi * 26200 * t)
elif mode == "nfm":
    key = ((t > secs / 4) & (t < 3 * secs / 4)).astype(float)
    s = 0.25 * key * fm(600, np.sin(2 * np.pi * 1000 * t), 2500)
    s += 0.25 * fm(13100, np.sin(2 * np.pi * 2000 * t), 2500)
else:
    sys.exit("mode: wfm, am, nfm")

s += (rng.normal(0, 0.02, n) + 1j * rng.normal(0, 0.02, n))   # noise
s += 0.01 + 0.006j                                              # the ADC's DC
iq = np.empty(2 * n)
iq[0::2] = s.real
iq[1::2] = s.imag
np.clip(np.round(iq * 128 + 127.4), 0, 255).astype(np.uint8).tofile(out)
print(f"{out}: {mode}, {rate} sps, {secs} s")
