#!/usr/bin/env python3
"""
METEGOL - every sound of the game, synthesised here with numpy (nothing
recorded): the figures' knock on the ball, the bands, the post, the net,
the referee's whistle, the crowd (its murmur, its roar, its "uuuh") and
the menus' music. 16 kHz mono WAV in assets/sounds, which pack_p4.py
turns into mu-law.

    python3 tools/sounds.py
"""
import os
import wave

import numpy as np

RATE = 16000
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'assets', 'sounds')
rng = np.random.default_rng(20261121)


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


def whistle(dur, trill=28.0):
    t = t_of(dur)
    n = len(t)
    f = 2950 + 40 * np.sin(2 * np.pi * 5 * t)
    ph = 2 * np.pi * np.cumsum(f) / RATE
    tone = np.sin(ph) + 0.3 * np.sin(2 * ph)
    am = 0.55 + 0.45 * np.sign(np.sin(2 * np.pi * trill * t)) * 0.8
    breath = bandpass(rng.standard_normal(n), 2000, 4500) * 0.25
    e = np.minimum(t / 0.02, 1) * np.minimum((dur - t) / 0.04, 1)
    return norm((tone * am + breath) * e, 0.8)


def crowd_bed(dur, seed_shift=0):
    """many voices far away: pink noise shaped like a room of people"""
    n = int(dur * RATE)
    base = bandpass(pink(n), 180, 2400)
    # a slow swell, and murmurs of chant
    t = np.arange(n) / RATE
    swell = 0.75 + 0.25 * np.sin(2 * np.pi * 0.11 * t + seed_shift) * np.sin(2 * np.pi * 0.037 * t)
    chant = np.zeros(n)
    for k in range(6):
        f = rng.uniform(170, 330)
        vib = 1 + 0.01 * np.sin(2 * np.pi * rng.uniform(4, 6) * t)
        ph = 2 * np.pi * np.cumsum(f * vib) / RATE
        voice = np.sign(np.sin(ph)) * 0.3
        voice = bandpass(voice, 300, 1200)
        beat = 0.5 + 0.5 * np.sin(2 * np.pi * rng.uniform(0.6, 1.1) * t + rng.uniform(0, 6))
        chant += voice * beat ** 3
    return norm(base * swell + 0.35 * chant, 0.7)


def loopable(x, fade=0.6):
    """the end crossfaded into the start, so it loops without a seam"""
    k = int(fade * RATE)
    a = x[:k].copy()
    b = x[-k:]
    w = np.linspace(0, 1, k)
    out = x[:-k].copy()
    out[:k] = b * (1 - w) + a * w
    return out


def roar():
    dur = 2.8
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    x = bandpass(pink(n), 200, 3500)
    voices = np.zeros(n)
    for k in range(14):
        f = rng.uniform(180, 420) * np.linspace(1.0, rng.uniform(1.05, 1.3), n)
        ph = 2 * np.pi * np.cumsum(f) / RATE
        voices += bandpass(np.sign(np.sin(ph)), 400, 1800) * rng.uniform(0.5, 1)
    e = np.minimum(t / 0.25, 1) * np.exp(-np.maximum(t - 1.2, 0) * 1.6)
    return norm((x + 0.4 * voices / 14) * e, 0.9)


def ooh():
    dur = 1.3
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    x = np.zeros(n)
    for k in range(16):
        f = rng.uniform(150, 260) * (1 + 0.15 * np.exp(-t * 3))
        ph = 2 * np.pi * np.cumsum(f) / RATE
        x += np.sign(np.sin(ph)) * rng.uniform(0.5, 1)
    # the vowel "u": low formants
    x = bandpass(x, 220, 750) + 0.4 * bandpass(pink(n), 200, 900)
    e = np.minimum(t / 0.18, 1) * np.exp(-np.maximum(t - 0.4, 0) * 2.8)
    return norm(x * e, 0.8)


def music():
    """sixteen seconds that loop: a cumbia's bass and güiro under a melody
    of a toy accordion"""
    bpm = 104
    beat = 60 / bpm
    bars = 8
    dur = bars * 4 * beat
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    out = np.zeros(n)
    chords = [(57, 60, 64), (62, 65, 69), (55, 59, 62), (57, 60, 64), (57, 60, 64), (62, 65, 69), (52, 56, 59), (57, 60, 64)]

    def hz(m):
        return 440.0 * 2 ** ((m - 69) / 12)

    def note(at, length, f, vol, kind):
        i0 = int(at * RATE)
        m = int(length * RATE)
        if i0 >= n:
            return
        m = min(m, n - i0)
        tt = np.arange(m) / RATE
        if kind == 'bass':
            w = np.sin(2 * np.pi * f * tt) + 0.3 * np.sin(4 * np.pi * f * tt)
            e = env(m, 0.005, length, 3)
        elif kind == 'reed':
            ph = 2 * np.pi * f * tt
            w = (np.sign(np.sin(ph)) * 0.5 + np.sin(ph) + 0.4 * np.sin(2 * ph + 0.3 * np.sin(2 * np.pi * 5.5 * tt)))
            w = w + 0.5 * np.sign(np.sin(ph * 1.004))
            e = np.minimum(tt / 0.02, 1) * np.minimum((length - tt) / 0.05, 1).clip(0, 1)
        else:
            w = np.sin(2 * np.pi * f * tt)
            e = env(m, 0.002, length, 5)
        out[i0:i0 + m] += vol * w * e
    for b in range(bars):
        c = chords[b]
        t0 = b * 4 * beat
        # bass: root, fifth
        for k, (o, m) in enumerate(((0, c[0] - 12), (1, c[0] - 12 + 7), (2, c[0] - 12), (3, c[0] - 12 + 7))):
            note(t0 + o * beat, beat * 0.9, hz(m), 0.55, 'bass')
        # the chord off the beat
        for k in range(4):
            for m in c:
                note(t0 + k * beat + beat / 2, beat * 0.35, hz(m), 0.10, 'reed')
    # the güiro: scraped noise, a long and two short
    g = bandpass(rng.standard_normal(n), 2500, 6000)
    gate = np.zeros(n)
    for k in range(int(dur / beat)):
        for (o, l) in ((0, 0.45), (0.5, 0.18), (0.75, 0.18)):
            i0 = int((k + o) * beat * RATE)
            m = int(l * beat * RATE)
            if i0 + m < n:
                gate[i0:i0 + m] = np.linspace(1, 0.4, m)
    out += 0.18 * g * gate
    # a melody on top, from the chord's notes
    mel_rng = np.random.default_rng(7)
    for b in range(bars):
        c = chords[b]
        t0 = b * 4 * beat
        steps = [0, 1, 1.5, 2, 3, 3.5] if b % 2 == 0 else [0, 0.5, 1, 2, 2.5, 3]
        for i, s in enumerate(steps):
            m = c[mel_rng.integers(0, 3)] + 12
            length = (steps[i + 1] - s if i + 1 < len(steps) else 4 - s) * beat
            note(t0 + s * beat, length * 0.95, hz(m), 0.16, 'reed')
    return norm(lowpass(out, 5000), 0.7)


def main():
    save('kick', knock(620, 0.11, 0.9, 5000))
    save('kick_soft', knock(430, 0.08, 0.5, 2500) * 0.8)
    t = t_of(0.16)
    wall = np.sin(2 * np.pi * 170 * t) * env(len(t), 0.001, 0.12, 5) + 0.6 * lowpass(rng.standard_normal(len(t)), 900) * env(len(t), 0.001, 0.05, 5)
    save('wall', norm(wall))
    t = t_of(0.5)
    post = sum(np.sin(2 * np.pi * f * t) * a for f, a in ((1180, 1), (2950, 0.5), (4310, 0.3), (640, 0.4)))
    save('post', norm(post * env(len(t), 0.001, 0.45, 5) + 0.3 * knock(800, 0.5, 1, 6000)))
    n = int(0.45 * RATE)
    net = bandpass(rng.standard_normal(n), 300, 2600) * env(n, 0.02, 0.4, 4)
    save('net', norm(net + 0.5 * knock(300, 0.45, 0.2, 1500), 0.8))
    save('whistle', whistle(0.45))
    w = np.concatenate([whistle(0.22), np.zeros(int(0.12 * RATE)), whistle(0.22), np.zeros(int(0.12 * RATE)), whistle(0.9)])
    save('whistle_end', w)
    save('roar', roar())
    save('ooh', ooh())
    n = int(0.6 * RATE)
    tt = np.arange(n) / RATE
    roll = lowpass(rng.standard_normal(n), 300) * (0.6 + 0.4 * np.sin(2 * np.pi * 23 * tt)) * env(n, 0.05, 0.6, 3)
    save('serve', norm(roll, 0.6))
    save('stop', norm(knock(260, 0.07, 1, 1800), 0.7))
    save('tap', norm(knock(1400, 0.04, 0.3, 7000), 0.5))
    save('crowd', loopable(crowd_bed(9.0)))
    save('music_menu', music())


if __name__ == '__main__':
    main()
