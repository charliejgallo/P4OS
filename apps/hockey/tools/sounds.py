#!/usr/bin/env python3
"""
HOCKEY - every sound of the game, synthesised here with numpy (nothing
recorded): the mallet's clack on the puck, the rails, the slot's edge, the
puck dropping down the goal's chute, the buzzer, and the air bed's blower
(a loop). 16 kHz mono WAV in assets/sounds, which pack_p4.py turns into
mu-law.

    python3 tools/sounds.py
"""
import os
import wave

import numpy as np

RATE = 16000
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'sounds')
rng = np.random.default_rng(20261122)


def t_of(sec):
    return np.arange(int(sec * RATE)) / RATE


def env(n, a, d, curve=4.0):
    """attack a seconds, then an exponential fall over d"""
    t = np.arange(n) / RATE
    e = np.minimum(t / max(a, 1e-4), 1.0)
    return e * np.exp(-np.maximum(t - a, 0) * curve / max(d, 1e-4))


def lowpass(x, fc):
    a = np.exp(-2 * np.pi * fc / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1 - a) * v + a * acc
        y[i] = acc
    return y


def bandpass(x, lo, hi):
    return lowpass(x, hi) - lowpass(x, lo)


def pink(n):
    w = rng.standard_normal(n)
    f = np.fft.rfft(w)
    k = np.arange(len(f))
    k[0] = 1
    f /= np.sqrt(k)
    p = np.fft.irfft(f, n)
    return p / np.max(np.abs(p))


def norm(x, peak=0.9):
    m = np.max(np.abs(x))
    return x * (peak / m) if m > 0 else x


def save(name, x):
    os.makedirs(OUT, exist_ok=True)
    x = np.clip(x, -1, 1)
    with wave.open(os.path.join(OUT, name + '.wav'), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((x * 32000).astype('<i2').tobytes())
    print('%-12s %.2f s' % (name, len(x) / RATE))


def knock(f0, dur, noise, bright):
    """a hard plastic figure against a hard ball: a click, a short body"""
    t = t_of(dur)
    n = len(t)
    body = np.sin(2 * np.pi * f0 * t) + 0.5 * np.sin(2 * np.pi * f0 * 2.7 * t) + 0.25 * np.sin(2 * np.pi * f0 * 5.1 * t)
    click = rng.standard_normal(n) * env(n, 0.0005, 0.012, 6)
    click = lowpass(click, bright)
    return norm(body * env(n, 0.001, dur, 7) + noise * click)


def clack(f0, dur, bright, noise=1.0):
    """hard plastic on hard plastic: a bright click and a short ring"""
    t = t_of(dur)
    n = len(t)
    body = (np.sin(2 * np.pi * f0 * t) + 0.6 * np.sin(2 * np.pi * f0 * 2.31 * t) +
            0.35 * np.sin(2 * np.pi * f0 * 3.92 * t) + 0.2 * np.sin(2 * np.pi * f0 * 5.6 * t))
    click = lowpass(rng.standard_normal(n), bright) * env(n, 0.0003, 0.008, 6)
    return norm(body * env(n, 0.0005, dur, 9) + noise * 1.4 * click)


def thock(f0, dur):
    """the puck against a rail: a dull knock with the cabinet's body under it"""
    t = t_of(dur)
    n = len(t)
    body = np.sin(2 * np.pi * f0 * t) + 0.5 * np.sin(2 * np.pi * 2.7 * f0 * t)
    low = np.sin(2 * np.pi * 140 * t) * env(n, 0.001, dur, 4)
    click = lowpass(rng.standard_normal(n), 2600) * env(n, 0.0005, 0.012, 6)
    return norm(body * env(n, 0.0008, dur * 0.6, 7) + 0.7 * low + 0.9 * click)


def goal():
    """the puck down the slot: a clunk, its rattle down the chute, a thud"""
    out = np.zeros(int(1.1 * RATE))
    first = thock(330, 0.18)
    out[:len(first)] += first
    at = 0.12
    gap = 0.085
    k = 0
    while at < 0.8 and k < 11:
        c = clack(rng.uniform(700, 1000), 0.05, 3500, 0.6) * (0.55 * np.exp(-k * 0.18))
        i = int(at * RATE)
        out[i:i + len(c)] += c[:len(out) - i]
        at += gap
        gap *= 0.84
        k += 1
    th = thock(160, 0.25) * 0.9
    i = int(0.82 * RATE)
    out[i:i + len(th)] += th[:len(out) - i]
    return norm(out, 0.9)


def buzzer(dur, f):
    """the arcade's horn"""
    t = t_of(dur)
    ph = 2 * np.pi * f * t
    x = np.sign(np.sin(ph)) * 0.6 + np.sign(np.sin(ph * 1.5 + 0.4)) * 0.35 + 0.3 * np.sin(2 * ph)
    x = lowpass(x, 2600)
    e = np.minimum(t / 0.015, 1) * np.minimum((dur - t) / 0.06, 1).clip(0, 1)
    return norm(x * e, 0.75)


def air(dur):
    """the blower under the table: a broad hiss and a motor's hum"""
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    hiss = bandpass(pink(n), 250, 3800)
    hum = (np.sin(2 * np.pi * 98 * t) + 0.5 * np.sin(2 * np.pi * 196 * t) + 0.25 * np.sin(2 * np.pi * 294 * t))
    flutter = 0.85 + 0.15 * np.sin(2 * np.pi * 0.31 * t) * np.sin(2 * np.pi * 0.07 * t)
    return norm(hiss * flutter + 0.18 * hum, 0.6)


def loopable(x, fade=0.6):
    """the end crossfaded into the start, so it loops without a seam"""
    k = int(fade * RATE)
    a = x[:k].copy()
    b = x[-k:]
    w = np.linspace(0, 1, k)
    out = x[:-k].copy()
    out[:k] = b * (1 - w) + a * w
    return out


def main():
    save('hit', clack(1650, 0.06, 7000))
    save('hit_soft', norm(clack(1350, 0.045, 4000, 0.6), 0.7))
    save('wall', thock(520, 0.09))
    t = t_of(0.35)
    ping = sum(np.sin(2 * np.pi * f * t) * a for f, a in ((2380, 1), (3730, 0.5), (5110, 0.3), (1290, 0.4)))
    save('post', norm(ping * env(len(t), 0.0008, 0.3, 6) + 0.6 * thock(600, 0.35)))
    save('goal', goal())
    n = int(0.5 * RATE)
    place = np.zeros(n)
    c = clack(900, 0.05, 2500, 0.5) * 0.7
    place[:len(c)] += c
    place += bandpass(rng.standard_normal(n), 800, 5000) * 0.25 * np.minimum(np.arange(n) / (0.2 * RATE), 1) * \
        np.exp(-np.arange(n) / RATE * 4)
    save('place', norm(place, 0.6))
    save('buzz', buzzer(0.55, 233))
    save('buzz_end', np.concatenate([buzzer(0.3, 196), np.zeros(int(0.08 * RATE)), buzzer(1.0, 175)]))
    save('tap', norm(clack(2200, 0.03, 8000, 0.4), 0.45))
    save('air', loopable(air(6.0)))


if __name__ == '__main__':
    main()
