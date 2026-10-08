#!/usr/bin/env python3
"""LoRa packets as cu8 I/Q, for the RF app's LoRa meter (apps/rf/main/rf_lora.c).

    python3 gen_lora.py <rate> <out.cu8>

Each packet is what a LoRa radio sends: a preamble of up-chirps, the two
sync-word symbols, 2.25 down-chirps and the payload's symbols (random), at
an offset from the centre and a signal-to-noise ratio measured in its own
bandwidth. The noise is a real stick's (its floor on the board, 2026-10-07:
about 8.5 steps of the 8-bit samples, each of I and Q). Also in the file: a
narrow on-off burst (a 433 MHz remote), which is not LoRa, and a carrier
that stays on the whole time, which is nothing.

  t (s)  what                                   expected
  0.20   SF9  BW 250 k,   0 kHz, 20 dB, 16 + 30 MediumFast
  0.60   SF11 BW 250 k, -200 kHz, 15 dB, 16 + 20 LongFast
  1.60   SF7  BW 500 k, +150 kHz, 20 dB, 16 + 40 ShortTurbo
  1.80   SF12 BW 125 k, +300 kHz, 12 dB, 16 + 12 LongSlow
  3.20   SF7  BW 125 k, -350 kHz, 18 dB,  8 + 30 SF7 125 k (LoRaWAN-like)
  3.50   SF9  BW 250 k,   0 kHz,  0 dB, 16 + 30 MediumFast, weak
  4.00   on-off keying, +100 kHz, 50 ms             not LoRa
  all    a carrier at +400 kHz                      nothing
"""
import sys
import numpy as np

rate, out = int(sys.argv[1]), sys.argv[2]
dur = 4.4
n = int(dur * rate)
rng = np.random.default_rng(7)
SIGMA = 8.5 / 127.5
s = (rng.normal(0, SIGMA, n) + 1j * rng.normal(0, SIGMA, n)).astype(np.complex64)


def symbol_freqs(sf, bw, value, down=False):
    """The instantaneous frequency of one symbol, sample by sample."""
    ns = int(round((2 ** sf) / bw * rate))
    t = np.arange(ns) / rate
    T = (2 ** sf) / bw
    if down:
        return bw / 2 - bw * t / T
    return -bw / 2 + bw * np.mod(t / T + value / 2 ** sf, 1.0)


def packet(sf, bw, n_pre, n_pay, value_rng):
    f = [symbol_freqs(sf, bw, 0) for _ in range(n_pre)]
    f += [symbol_freqs(sf, bw, v) for v in (8 * 2, 8 * 11)]         # sync word 0x2B
    d = symbol_freqs(sf, bw, 0, down=True)
    f += [d, d, d[: len(d) // 4]]
    f += [symbol_freqs(sf, bw, int(v)) for v in value_rng.integers(0, 2 ** sf, n_pay)]
    return np.concatenate(f)


def put(t0, sf, bw, off, snr_db, n_pre, n_pay):
    f = packet(sf, bw, n_pre, n_pay, rng) + off
    ph = 2 * np.pi * np.cumsum(f) / rate
    # SNR in the packet's own bandwidth
    amp = np.sqrt(10 ** (snr_db / 10) * 2 * SIGMA ** 2 * bw / rate)
    i0 = int(t0 * rate)
    m = min(len(f), n - i0)
    s[i0:i0 + m] += (amp * np.exp(1j * ph[:m])).astype(np.complex64)
    print(f"{t0:5.2f} s  SF{sf:<2} BW {bw // 1000:>3} k  {off / 1000:+5.0f} kHz  {snr_db:2.0f} dB  "
          f"{m / rate * 1000:6.1f} ms")


put(0.20, 9, 250000, 0, 20, 16, 30)
put(0.60, 11, 250000, -200e3, 15, 16, 20)
put(1.60, 7, 500000, 150e3, 20, 16, 40)
put(1.80, 12, 125000, 300e3, 12, 16, 12)
put(3.20, 7, 125000, -350e3, 18, 8, 30)
put(3.50, 9, 250000, 0, 0, 16, 30)

# a remote: 50 ms of on-off keying, narrow
i0, i1 = int(4.0 * rate), int(4.05 * rate)
t = np.arange(i1 - i0) / rate
key = (np.mod(t, 1.2e-3) < 0.4e-3).astype(np.float32)
s[i0:i1] += (0.3 * key * np.exp(2j * np.pi * 100e3 * t)).astype(np.complex64)
# a carrier all along
t = np.arange(n) / rate
s += (0.05 * np.exp(2j * np.pi * 400e3 * t)).astype(np.complex64)

iq = np.empty(2 * n, np.float32)
iq[0::2] = s.real
iq[1::2] = s.imag
np.clip(np.round(iq * 127.5 + 127.5), 0, 255).astype(np.uint8).tofile(out)
