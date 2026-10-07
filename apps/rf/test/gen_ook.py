#!/usr/bin/env python3
"""
gen_ook.py <rate> <out.cu8>

On-off keyed transmissions at 433 MHz as an RTL2832 gives them (cu8, noise,
DC), for the decoders' tests (apps/rf/test/README.md). In 2 seconds:

  0.10 s  an EV1527 remote, ID A3F21, button 4, T = 320 us, 6 repeats,
          60 kHz above the centre
  0.60 s  a PT2262 remote, code 0F1F0110 1000 (tri-state: address, then the data pins, driven), T = 400 us, 4 repeats,
          -40 kHz
  1.00 s  a Nexus sensor, ID 0x5B, channel 2, 21.7 C, 64 %, 3 repeats, +10 kHz,
          10 dB weaker
  1.50 s  a Prologue sensor, type 9, ID 0x2C, channel 3, -4.5 C, 3 repeats, -80 kHz
"""
import sys
import numpy as np

rate, out = int(sys.argv[1]), sys.argv[2]
secs = 2.0
n = int(rate * secs)
env = np.zeros(n)
freq = np.zeros(n)
amp = np.zeros(n)
rng = np.random.default_rng(3)


def us(x):
    return int(round(x * rate / 1e6))


def put(t0, pulses, f, a):
    """pulses: list of (mark_us, space_us)"""
    i = int(t0 * rate)
    for m, s in pulses:
        j = i + us(m)
        env[i:j] = 1
        freq[i:j] = f
        amp[i:j] = a
        i = j + us(s)
    return i / rate


def pwm(bits, T):
    return [(3 * T, T) if b == '1' else (T, 3 * T) for b in bits]


def ev1527(idv, btn, T):
    bits = format(idv, '020b') + format(btn, '04b')
    return pwm(bits, T) + [(T, 31 * T)]


def pt2262(tri, T):
    bits = ''.join({'0': '00', '1': '11', 'F': '01'}[c] for c in tri)
    return pwm(bits, T) + [(T, 31 * T)]


def ppm(bits, mark=500, zero=1000, one=2000, sync=4000):
    return [(mark, one if b == '1' else zero) for b in bits] + [(mark, sync)]


def nexus(idv, ch, temp, hum, batt=1):
    t = int(round(temp * 10)) & 0xFFF
    bits = format(idv, '08b') + str(batt) + '0' + format(ch - 1, '02b') + format(t, '012b') + '1111' + format(hum, '08b')
    return ppm(bits)


def prologue(typ, idv, ch, temp, hum=0xCC, batt=1, btn=0):
    t = int(round(temp * 10)) & 0xFFF
    bits = format(typ, '04b') + format(idv, '08b') + str(batt) + str(btn) + format(ch - 1, '02b') + format(t, '012b') + format(hum, '08b')
    return ppm(bits)


t = 0.10
for _ in range(6):
    t = put(t, ev1527(0xA3F21, 0x4, 320), 60e3, 0.5)
t = 0.60
for _ in range(4):
    t = put(t, pt2262('0F1F01101000', 400), -40e3, 0.5)
t = 1.00
for _ in range(3):
    t = put(t, nexus(0x5B, 2, 21.7, 64), 10e3, 0.16)
t = 1.50
for _ in range(3):
    t = put(t, prologue(9, 0x2C, 3, -4.5), -80e3, 0.5)

tt = np.arange(n) / rate
ph = 2 * np.pi * np.cumsum(freq) / rate
s = env * amp * np.exp(1j * ph)
s += rng.normal(0, 0.02, n) + 1j * rng.normal(0, 0.02, n)
s += 0.01 + 0.006j
iq = np.empty(2 * n)
iq[0::2] = s.real
iq[1::2] = s.imag
np.clip(np.round(iq * 128 + 127.4), 0, 255).astype(np.uint8).tofile(out)
print(f"{out}: 433 MHz OOK, {rate} sps, {secs} s")
