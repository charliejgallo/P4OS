#!/usr/bin/env python3
"""
BASTION - every sound of the game, synthesised here with numpy: nothing
recorded, nothing borrowed.

    import sounds
    kit = sounds.build()                  # {'shot_gatling': float32 array, ..., 'music_menu': ...}
    kit = sounds.build(['laser', 'music0'])

    python3 sounds.py --wav ../build/preview/wav [--only laser,music0] [--mix]

The game plays about twelve voices over the music on a small speaker, at
16 kHz and 8-bit mu-law, so everything here is made for that: the effects
are short and dry, each with a shape of its own (a tick, a thump, a zing, a
whoosh ...), most of their energy between 300 Hz and 5 kHz, no DC, a few
milliseconds of fade at both ends and a peak of 0.9. Deep sounds carry
harmonics on purpose, because the speaker gives no fundamental under
200 Hz.

The music is five written pieces (melody, chords, bass, drums), one a
world's mood and one for the menu, each in its own key and tempo. A piece
is a whole number of bars and everything in it is laid on a ring: a note or
a reverberation that runs past the end comes back in at the start, and the
filters of the whole mix are circular, so the last sample flows into the
first with no click and no gap.

Every random number comes from a generator seeded by the sound's name, so
two builds give the same arrays, whatever was asked for.
"""
import os
import sys
import time
import wave as wavefile
import zlib

import numpy as np

RATE = 16000
R = RATE
TAU = 2 * np.pi
TOP = 7200.0        # no partial is written above this: the rate's half is 8 kHz

EFFECTS = ['shot_gatling', 'shot_cannon', 'laser', 'shot_missile', 'zap', 'cryo', 'flame', 'shot_rail', 'shot_mortar',
           'drone', 'emp', 'gravity', 'boom_s', 'boom_m', 'boom_l', 'hit', 'die1', 'die2', 'die3', 'boss_roar',
           'boss_die', 'fort_hit', 'alarm', 'build', 'upgrade', 'sell', 'coin', 'wave_start', 'wave_clear', 'victory',
           'defeat', 'tap', 'deny', 'strike', 'freeze', 'repair', 'star']
MUSIC = ['music0', 'music1', 'music2', 'music3', 'music_menu']
NAMES = EFFECTS + MUSIC


# ---------------------------------------------------------------------------
# The basics
# ---------------------------------------------------------------------------

def rng_for(name):
    return np.random.default_rng(zlib.crc32(name.encode()))


def N(d):
    return int(round(d * R))


def tt(n):
    return np.arange(n) / R


def glide(f0, f1, n, shape='exp'):
    u = np.linspace(0.0, 1.0, n)
    return f0 * (f1 / f0) ** u if shape == 'exp' else f0 + (f1 - f0) * u


def phase(f, n=None):
    if n is not None:
        f = np.broadcast_to(np.asarray(f, float), (n,))
    return TAU * np.cumsum(f) / R


def wave(f, n, kind='saw', duty=0.5, top=TOP):
    """a saw, square, triangle or pulse built from its harmonics, each one
    left out as it nears the top, so a sweep never folds back"""
    f = np.broadcast_to(np.asarray(f, float), (n,))
    ph = phase(f)
    steady = float(f.max()) == float(f.min())
    out = np.zeros(n)
    for k in range(1, int((top + 500) / max(float(f.min()), 20.0)) + 1):
        if kind == 'saw':
            a = 1.0 / k
        elif kind == 'square':
            a = 1.0 / k if k % 2 else 0.0
        elif kind == 'tri':
            a = (-1.0) ** ((k - 1) // 2) / (k * k) if k % 2 else 0.0
        else:
            a = np.sin(np.pi * k * duty) / k
        if abs(a) < 1e-4:
            continue
        w = np.clip((top + 500 - k * f) / 500.0, 0.0, 1.0)
        if steady and w[0] <= 0:
            break
        out += a * w * np.sin(k * ph)
    return out


def _gain(f, lo, hi, order):
    g = np.ones_like(f)
    if lo:
        g = g / np.sqrt(1.0 + (lo / np.maximum(f, 1e-3)) ** (2 * order))
    if hi:
        g = g / np.sqrt(1.0 + (f / hi) ** (2 * order))
    return g


def filt(x, lo=None, hi=None, order=2, circular=False):
    """a high-pass at lo and a low-pass at hi with Butterworth's slopes and
    no phase, done on the spectrum; circular for a loop"""
    n = len(x)
    if circular:
        return np.fft.irfft(np.fft.rfft(x) * _gain(np.fft.rfftfreq(n, 1.0 / R), lo, hi, order), n)
    pad = 1024
    m = 1 << int(np.ceil(np.log2(n + 2 * pad)))
    y = np.zeros(m)
    y[pad:pad + n] = x
    y = np.fft.irfft(np.fft.rfft(y) * _gain(np.fft.rfftfreq(m, 1.0 / R), lo, hi, order), m)
    return y[pad:pad + n]


def svf(x, fc, q=1.0, mode='lp'):
    """a state-variable filter whose cutoff may move with the sound (an
    array): the swept whooshes and the bodies of the explosions"""
    n = len(x)
    g = np.tan(np.pi * np.clip(np.broadcast_to(np.asarray(fc, float), (n,)), 20.0, R * 0.45) / R)
    k = 1.0 / q
    a1 = 1.0 / (1.0 + g * (g + k))
    a2 = g * a1
    a3 = g * a2
    A1, A2, A3, X = a1.tolist(), a2.tolist(), a3.tolist(), np.asarray(x, float).tolist()
    lp, bp = [0.0] * n, [0.0] * n
    c1 = c2 = 0.0
    for i in range(n):
        v3 = X[i] - c2
        v1 = A1[i] * c1 + A2[i] * v3
        v2 = c2 + A2[i] * c1 + A3[i] * v3
        c1 = 2.0 * v1 - c1
        c2 = 2.0 * v2 - c2
        lp[i] = v2
        bp[i] = v1
    if mode == 'lp':
        return np.array(lp)
    if mode == 'bp':
        return np.array(bp) * k
    return np.asarray(x, float) - k * np.array(bp) - np.array(lp)


def unit(x):
    return x / max(1e-9, float(np.max(np.abs(x))))


def edges(x, a=0.002, r=0.006):
    """a raised-cosine fade in and out"""
    x = np.array(x, float)
    na, nr = min(len(x) // 2, max(1, N(a))), min(len(x) // 2, max(1, N(r)))
    x[:na] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(na) / na)
    x[-nr:] *= 0.5 + 0.5 * np.cos(np.pi * (np.arange(nr) + 1) / nr)
    return x


def shape(n, pts):
    """an envelope through (place 0..1, level) points"""
    u = np.linspace(0.0, 1.0, n)
    return np.interp(u, [p[0] for p in pts], [p[1] for p in pts])


def put(buf, x, at, gain=1.0):
    """x added into buf from 'at' seconds; what runs past the end is lost"""
    i = N(at)
    m = min(len(x), len(buf) - i)
    if m > 0:
        buf[i:i + m] += gain * x[:m]


def ring(n, parts, g=None):
    """struck metal or glass: partials (Hz, level, seconds to fall to 1/e)"""
    t = tt(n)
    out = np.zeros(n)
    for (f, a, tau) in parts:
        if f < TOP + 300:
            out += a * np.sin(TAU * f * t + (g.uniform(0, TAU) if g is not None else 0.0)) * np.exp(-t / tau)
    return out


def reverb(x, amount=0.25, tau=0.25, seed='room', circular=False, length=1.0):
    """a room: the sound convolved with a second of decaying noise. On a
    loop the tail comes round to the start"""
    g = rng_for(seed)
    m = N(length)
    ir = filt(g.standard_normal(m), 300, 4500) * np.exp(-tt(m) / tau)
    ir[:N(0.012)] = 0.0
    ir /= np.sqrt(np.sum(ir * ir))
    n = len(x)
    if circular:
        h = np.zeros(n)
        h[:m] = ir
        return x + amount * np.fft.irfft(np.fft.rfft(x) * np.fft.rfft(h), n)
    k = 1 << int(np.ceil(np.log2(n + m)))
    y = np.fft.irfft(np.fft.rfft(x, k) * np.fft.rfft(ir, k), k)[:n]
    return x + amount * y


# ---------------------------------------------------------------------------
# Notes and instruments. An instrument is a function (Hz, samples held) ->
# the note with its release after it. They are sums of harmonics whose
# levels move in time, so nothing here aliases and no filter is needed.
# ---------------------------------------------------------------------------

_PC = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def midi(name):
    """'A4', 'C#5', 'Bb3' -> its MIDI number"""
    if isinstance(name, (int, np.integer)):
        return int(name)
    n = _PC[name[0]]
    i = 1
    while name[i] in '#b':
        n += 1 if name[i] == '#' else -1
        i += 1
    return n + 12 * (int(name[i:]) + 1)


def mhz(m):
    return 440.0 * 2.0 ** ((midi(m) - 69) / 12.0)


def harm(f, n, amp, vib=0.0, vib_rate=5.6, vib_delay=0.12, detune=(0.0,), kcap=44):
    """sum over harmonics k of amp(k, t, f) * sin(k * phase): amp returns
    an array that broadcasts over (harmonics, samples)"""
    t = tt(n)
    out = np.zeros(n)
    for cents in detune:
        ff = f * 2.0 ** (cents / 1200.0)
        ph = TAU * ff * t
        if vib:
            ph = ph + (vib * ff / vib_rate) * np.sin(TAU * vib_rate * t) * np.clip((t - vib_delay) / 0.15, 0.0, 1.0)
        K = max(1, min(kcap, int(TOP / ff)))
        k = np.arange(1, K + 1, dtype=float)[:, None]
        out += (amp(k, t[None, :], ff) * np.sin(k * ph[None, :])).sum(axis=0)
    return out


def adsr(n_on, rel, a, d, s):
    """attack a, then a fall towards s with time constant d, and after
    n_on samples a release of rel seconds"""
    n = n_on + N(rel)
    t = tt(n)
    e = np.where(t < a, t / a, s + (1.0 - s) * np.exp(-(t - a) / d))
    e *= np.clip((n - np.arange(n)) / float(n - n_on), 0.0, 1.0) ** 1.5
    return e


def i_lead(f, n_on):
    """the tune's voice: a narrow pulse with a vibrato that comes in late"""
    e = adsr(n_on, 0.05, 0.008, 0.12, 0.72)
    return harm(f, len(e), lambda k, t, ff: np.sin(np.pi * k * 0.28) / k / (1.0 + (k * ff / 5200.0) ** 2), vib=0.006) * e


def i_brass(f, n_on):
    """a horn: a saw that brightens as it swells"""
    e = adsr(n_on, 0.07, 0.035, 0.30, 0.80)

    def amp(k, t, ff):
        bright = np.minimum(1.0, t / 0.06) * (0.55 + 0.45 * np.exp(-t / 0.3))
        return np.exp(-k * ff / (450.0 + 3000.0 * bright)) / k
    return harm(f, len(e), amp, vib=0.004, vib_delay=0.22, detune=(-5.0, 5.0)) * e


def i_pad(f, n_on):
    e = adsr(n_on, 0.28, 0.12, 0.5, 0.9)
    return harm(f, len(e), lambda k, t, ff: np.exp(-k * ff / 2000.0) / k, detune=(-9.0, 0.0, 8.0), kcap=24) * e


def i_choir(f, n_on):
    """voices on 'ah': saws weighed by three formants"""
    e = adsr(n_on, 0.32, 0.16, 0.5, 0.95)

    def amp(k, t, ff):
        x = k * ff
        w = (np.exp(-((x - 650.0) / 190.0) ** 2) + 0.7 * np.exp(-((x - 1100.0) / 220.0) ** 2)
             + 0.35 * np.exp(-((x - 2700.0) / 380.0) ** 2) + 0.05)
        return w / np.sqrt(k)
    return harm(f, len(e), amp, vib=0.004, vib_delay=0.3, detune=(-8.0, 0.0, 9.0)) * e


def i_bass(f, n_on):
    """a plucked synth bass, bright at the start and with its even
    harmonics raised (an octave doubled into it), so a small speaker
    shows it by its harmonics"""
    e = adsr(n_on, 0.03, 0.004, 0.25, 0.55)
    return harm(f, len(e), lambda k, t, ff: (1.0 + 0.8 * (k % 2 == 0)) * np.exp(
        -k * ff / (750.0 + 2600.0 * np.exp(-t / 0.09))) / k) * e


def i_pluck(f, n_on):
    """a harp or kalimba: the high harmonics die first"""
    n = max(n_on, N(0.25)) + N(0.12)
    x = harm(f, n, lambda k, t, ff: np.exp(-t * (3.0 + 1.4 * k)) / k)
    return edges(x, 0.003, 0.12)


def i_stab(f, n_on):
    """a short bright chord stab"""
    e = adsr(n_on, 0.04, 0.004, 0.09, 0.25)
    return harm(f, len(e), lambda k, t, ff: np.exp(-k * ff / (900.0 + 3200.0 * np.exp(-t / 0.07))) / k,
                detune=(-6.0, 6.0)) * e


def i_str(f, n_on):
    """low strings played short: the ostinato of the last world"""
    e = adsr(n_on, 0.05, 0.012, 0.10, 0.55)
    return harm(f, len(e), lambda k, t, ff: np.exp(-k * ff / 2600.0) / k, detune=(-7.0, 7.0)) * e


def i_bell(f, n_on):
    n = max(n_on, N(0.8)) + N(0.1)
    parts = [(f, 1.0, 0.45), (f * 2.0, 0.5, 0.30), (f * 3.01, 0.28, 0.18), (f * 4.2, 0.18, 0.10), (f * 5.43, 0.1, 0.07)]
    return edges(ring(n, parts), 0.002, 0.1)


def i_flute(f, n_on):
    """an ocarina with breath in it"""
    e = adsr(n_on, 0.09, 0.045, 0.3, 0.85)
    n = len(e)
    w = [1.0, 0.32, 0.12, 0.05]
    x = harm(f, n, lambda k, t, ff: np.array(w[:len(k)])[:, None] * np.ones_like(t), vib=0.007, kcap=4)
    g = rng_for('breath%.2f/%d' % (f, n))
    breath = filt(g.standard_normal(n), min(f * 1.5, 5000.0), min(f * 5.0, 7000.0)) * (0.5 + 0.5 * np.exp(-tt(n) / 0.06))
    return (x + 0.10 * unit(breath)) * e


def i_blip(f, n_on):
    """a bubble: a sine that rises into its pitch and is gone"""
    n = N(0.16)
    t = tt(n)
    ph = phase(f * (1.0 - 0.38 * np.exp(-t / 0.016)))
    return edges((np.sin(ph) + 0.22 * np.sin(2 * ph)) * np.exp(-t / 0.05), 0.003, 0.02)


def i_timp(f, n_on):
    n = N(0.7)
    t = tt(n)
    ph = phase(f * (1.0 + 0.22 * np.exp(-t / 0.03)))
    g = rng_for('timp')
    x = (np.sin(ph) + 0.6 * np.sin(2 * ph) * np.exp(-t / 0.18) + 0.4 * np.sin(3.02 * ph) * np.exp(-t / 0.10)
         + 0.25 * np.sin(4.1 * ph) * np.exp(-t / 0.06)) * np.exp(-t / 0.24)
    x += 0.8 * unit(filt(g.standard_normal(n), 200, 1500)) * np.exp(-t / 0.018)
    return edges(x, 0.001, 0.1)


# ---------------------------------------------------------------------------
# Drums
# ---------------------------------------------------------------------------

def d_kick(g, f0=170.0, f1=50.0, d=0.22):
    n = N(d)
    t = tt(n)
    x = np.tanh(2.6 * np.sin(phase(f1 + (f0 - f1) * np.exp(-t / 0.035))) * np.exp(-t / 0.085))
    x += 0.5 * np.sin(phase(2.6 * f1 + 3.0 * (f0 - f1) * np.exp(-t / 0.02))) * np.exp(-t / 0.03)
    x += 0.5 * unit(filt(g.standard_normal(n), 1500, 6000)) * np.exp(-t / 0.004)
    return edges(x, 0.0005, 0.03)


def d_snare(g, d=0.20, tone=190.0, tau=0.07):
    n = N(d)
    t = tt(n)
    x = (np.sin(phase(tone * (1 + 0.3 * np.exp(-t / 0.01)), n)) + 0.5 * np.sin(TAU * tone * 1.7 * t)) * np.exp(-t / 0.04)
    x = 0.7 * x + 1.1 * unit(filt(g.standard_normal(n), 1200, 6800)) * np.exp(-t / tau)
    return edges(x, 0.0005, 0.03)


def d_hat(g, tau=0.02):
    n = N(tau * 5 + 0.02)
    return edges(unit(filt(g.standard_normal(n), 5200, None, 3)) * np.exp(-tt(n) / tau), 0.0005, 0.01)


def d_shaker(g):
    n = N(0.09)
    t = tt(n)
    return unit(filt(g.standard_normal(n), 3800, 7000, 3)) * np.minimum(1.0, t / 0.012) * np.exp(-t / 0.022)


def d_rim(g):
    n = N(0.06)
    t = tt(n)
    return edges((np.sin(TAU * 1750 * t) + 0.6 * np.sin(TAU * 2480 * t)) * np.exp(-t / 0.009)
                 + 0.4 * unit(filt(g.standard_normal(n), 2500, None)) * np.exp(-t / 0.003), 0.0003, 0.01)


def d_wood(g, f=820.0):
    n = N(0.09)
    t = tt(n)
    return edges((np.sin(TAU * f * t) + 0.4 * np.sin(TAU * f * 2.6 * t)) * np.exp(-t / 0.018), 0.0005, 0.01)


def d_bongo(g, f=320.0):
    n = N(0.18)
    t = tt(n)
    ph = phase(f * (1 + 0.35 * np.exp(-t / 0.012)))
    x = (np.sin(ph) + 0.35 * np.sin(2.3 * ph)) * np.exp(-t / 0.055)
    return edges(x + 0.3 * unit(filt(g.standard_normal(n), 900, 4000)) * np.exp(-t / 0.006), 0.0005, 0.02)


def d_tom(g, f=140.0):
    n = N(0.3)
    t = tt(n)
    ph = phase(f * (1 + 0.5 * np.exp(-t / 0.03)))
    x = np.tanh(1.8 * (np.sin(ph) + 0.4 * np.sin(2 * ph)) * np.exp(-t / 0.09))
    return edges(x + 0.3 * unit(filt(g.standard_normal(n), 600, 3500)) * np.exp(-t / 0.008), 0.0005, 0.04)


def d_crash(g, d=1.2, tau=0.35):
    n = N(d)
    t = tt(n)
    x = unit(filt(g.standard_normal(n), 2800, None, 2)) * np.exp(-t / tau)
    x += 0.25 * ring(n, [(f, 1.0, tau * 0.8) for f in (3140, 4270, 5330, 6210)], g)
    return edges(x, 0.001, 0.15)


# ---------------------------------------------------------------------------
# The effects. Each returns the raw sound; finish() gives it its fades and
# its level.
# ---------------------------------------------------------------------------

FX = {}


def fx(name, a=0.002, r=0.006, peak=0.9):
    def deco(fn):
        FX[name] = (fn, a, r, peak)
        return fn
    return deco


def finish(x, peak, a, r):
    x = filt(np.asarray(x, float), lo=70.0)
    x = edges(x - x.mean(), a, r)
    return (x * (peak / np.max(np.abs(x)))).astype(np.float32)


@fx('shot_gatling', a=0.001, r=0.004)
def _gatling(g):
    """a dry tick: a crack of noise over a square that drops"""
    n = N(0.07)
    t = tt(n)
    crack = unit(filt(g.standard_normal(n), 900, 5500)) * np.exp(-t / 0.011)
    body = unit(wave(glide(560, 170, n), n, 'square')) * np.exp(-t / 0.014)
    return crack + 0.7 * body


@fx('shot_cannon', a=0.001, r=0.02)
def _cannon(g):
    """a heavy thump: a sine falling from 470 Hz, driven hard, under a
    blast of low noise"""
    n = N(0.35)
    t = tt(n)
    thump = np.tanh(3.2 * np.sin(phase(88 + 380 * np.exp(-t / 0.03))) * np.exp(-t / 0.09))
    blast = unit(filt(g.standard_normal(n), 320, 2800)) * np.exp(-t / 0.06)
    body = unit(filt(g.standard_normal(n), 250, 1100, 3)) * np.exp(-t / 0.11)
    click = unit(filt(g.standard_normal(n), 2500, None)) * np.exp(-t / 0.004)
    return np.tanh(1.5 * (0.28 * thump + 1.2 * blast + 0.8 * body + 0.4 * click))


@fx('laser', a=0.003, r=0.06)
def _laser(g):
    """a bright zing held for its whole length (it is played again every
    quarter of a second while the beam is on): two tones that slide down
    a little, shaken at 110 Hz"""
    n = N(0.3)
    t = tt(n)
    ph = phase(glide(2700, 1650, n))
    wob = np.sin(TAU * 110 * t)
    x = np.sin(ph + 1.8 * wob) + 0.5 * np.sin(1.503 * ph + 1.2 * wob) + 0.3 * np.sin(0.5 * ph)
    return x * (0.78 + 0.22 * np.exp(-t / 0.05))


@fx('shot_missile', a=0.01, r=0.03)
def _missile(g):
    """a whoosh that rises: noise through a band that climbs, and a faint
    engine tone under it"""
    n = N(0.5)
    x = unit(svf(g.standard_normal(n), glide(380, 3900, n), 2.2, 'bp'))
    x += 0.3 * unit(wave(glide(190, 640, n), n, 'saw'))
    return x * shape(n, [(0, 0.0), (0.08, 0.4), (0.72, 1.0), (1, 0.0)])


@fx('zap', a=0.001, r=0.02)
def _zap(g):
    """an electric crackle: a square that jumps pitch every few
    milliseconds, cut by random gaps, with sparks over it"""
    n = N(0.3)
    t = tt(n)
    seg = N(0.007)
    k = n // seg + 1
    f = np.repeat(g.uniform(600, 3400, k), seg)[:n]
    gate = np.repeat((g.random(k) < 0.72).astype(float), seg)[:n]
    gate = np.convolve(gate, np.ones(24) / 24.0, 'same')
    sparks = np.where(g.random(n) < 0.03, g.uniform(-1, 1, n), 0.0)
    sparks = unit(filt(sparks, 1500, None))
    hum = unit(wave(100.0, n, 'saw')) * (0.5 + 0.5 * np.sign(np.sin(TAU * 50 * t)))
    x = unit(wave(f, n, 'square')) * gate * 0.7 + 1.1 * sparks + 0.3 * filt(hum, 300, None)
    return x * (0.35 + 0.65 * np.exp(-t / 0.11))


@fx('cryo', a=0.003, r=0.03)
def _cryo(g):
    """an icy hiss with a chime in it"""
    n = N(0.4)
    t = tt(n)
    hiss = unit(filt(g.standard_normal(n), 2600, 6000, 3)) * (0.8 * np.exp(-t / 0.09) + 0.2 * np.exp(-t / 0.3))
    chime = np.zeros(n)
    for i, f in enumerate((1865, 2489, 3136, 3729, 4699)):
        chime += np.sin(TAU * f * t) * np.exp(-t / (0.16 - 0.02 * i)) * (0.7 + 0.3 * np.sin(TAU * (17 + 5 * i) * t)) / (1 + i * 0.4)
    return 0.8 * hiss + 0.55 * unit(chime)


@fx('flame', a=0.02, r=0.08)
def _flame(g):
    """a roar: low noise that flutters, pushed into saturation; played
    again every quarter of a second while the thrower is on"""
    n = N(0.3)
    roar = unit(filt(g.standard_normal(n), 200, 1500))
    hiss = unit(filt(g.standard_normal(n), 1500, 4500))
    flut = unit(filt(g.standard_normal(n), None, 28.0))
    return np.tanh(2.4 * (roar * (0.65 + 0.35 * flut) + 0.3 * hiss))


@fx('shot_rail', a=0.004, r=0.04)
def _rail(g):
    """a charge that whines up, a crack, and rails that ring after it"""
    n = N(0.6)
    out = np.zeros(n)
    nc = N(0.13)
    ph = phase(glide(420, 4300, nc))
    put(out, (np.sin(ph) + 0.5 * np.sin(1.5 * ph)) * np.linspace(0, 1, nc) ** 2 * edges(np.ones(nc), 0.002, 0.004), 0.0, 0.45)
    nk = n - nc
    t = tt(nk)
    crack = unit(g.standard_normal(nk)) * np.exp(-t / 0.012)
    crack += unit(wave(3300 * np.exp(-t / 0.02) + 160, nk, 'saw')) * np.exp(-t / 0.035)
    tail = ring(nk, [(1180, 1.0, 0.16), (1770, 0.7, 0.13), (2950, 0.5, 0.10), (4130, 0.35, 0.08)], g)
    tail += 0.35 * unit(filt(g.standard_normal(nk), 1500, 5000)) * np.exp(-t / 0.14)
    put(out, crack + 0.5 * tail, 0.13, 1.0)
    return out


@fx('shot_mortar', a=0.001, r=0.03)
def _mortar(g):
    """a hollow thoomp: a falling sine inside a tube's resonance"""
    n = N(0.4)
    t = tt(n)
    thoomp = np.sin(phase(125 + 420 * np.exp(-t / 0.03))) * np.exp(-t / 0.08)
    tube = unit(filt(g.standard_normal(n), 380, 600, 3)) * np.exp(-t / 0.09)
    tube += 0.5 * unit(filt(g.standard_normal(n), 820, 1120, 3)) * np.exp(-t / 0.06)
    pop = unit(filt(g.standard_normal(n), 800, 2500)) * np.exp(-t / 0.008)
    return np.tanh(2.0 * (0.55 * thoomp + 1.3 * tube + 0.5 * pop))


@fx('drone', a=0.02, r=0.05)
def _drone(g):
    """a short buzz of small rotors"""
    n = N(0.25)
    t = tt(n)
    f = glide(205, 255, n) * (1 + 0.03 * np.sin(TAU * 31 * t))
    x = wave(f, n, 'pulse', duty=0.3) + 0.6 * wave(f * 1.5, n, 'saw')
    return filt(x * (0.7 + 0.3 * np.sin(TAU * 47 * t)), 300, 4500)


@fx('emp', a=0.002, r=0.05)
def _emp(g):
    """a wave of pulses that falls from high to low and slows as it goes"""
    n = N(0.6)
    t = tt(n)
    sq = unit(wave(glide(2000, 120, n), n, 'square'))
    pulse = 0.5 + 0.5 * np.tanh(4.0 * np.sin(phase(glide(36, 11, n))))
    wash = unit(filt(g.standard_normal(n), 500, 4000))
    return (0.8 * sq + 0.3 * wash) * (0.15 + 0.85 * pulse) * np.exp(-t / 0.36)


@fx('gravity', a=0.06, r=0.18)
def _gravity(g):
    """a deep warble: a low saw whose pitch rocks, with two high partials
    that rock against it so it is heard on a small speaker too"""
    n = N(0.7)
    t = tt(n)
    f = 104.0 * glide(1.15, 0.85, n) * (1 + 0.22 * np.sin(TAU * 6.5 * t) + 0.08 * np.sin(TAU * 11 * t))
    x = 0.6 * unit(filt(wave(f, n, 'saw'), 160, 1500))
    ph = phase(f)
    x += 0.7 * np.sin(4.02 * ph) * (0.5 + 0.5 * np.sin(TAU * 6.5 * t + 2.0)) + 0.5 * np.sin(7.9 * ph) * (
        0.5 + 0.5 * np.sin(TAU * 6.5 * t + 4.0))
    return np.tanh(1.2 * x)


def boom(g, d, thump=(150.0, 48.0), cut=(3500.0, 350.0), debris=0.0):
    """an explosion: noise under a low-pass that closes, a sine that falls,
    and for the big ones a rattle of debris after it"""
    n = N(d)
    t = tt(n)
    body = unit(filt(svf(g.standard_normal(n), glide(cut[0], cut[1], n), 0.8, 'lp'), 220, None, 1)) * np.exp(-t / (d * 0.26))
    th = np.sin(phase(thump[1] + (thump[0] - thump[1]) * np.exp(-t / (d * 0.12)))) * np.exp(-t / (d * 0.30))
    crack = unit(filt(g.standard_normal(n), 1200, 6000)) * np.exp(-t / 0.006)
    x = 1.8 * body + 0.5 * th + 0.6 * crack
    if debris:
        grains = np.where(g.random(n) < 0.012, g.uniform(-1, 1, n), 0.0)
        x += debris * unit(filt(grains, 900, 5000)) * shape(n, [(0, 0), (0.15, 1), (1, 0)])
    return np.tanh(1.8 * x)


@fx('boom_s', a=0.001, r=0.03)
def _boom_s(g):
    return boom(g, 0.3, (260.0, 110.0), (5600.0, 1300.0))


@fx('boom_m', a=0.001, r=0.05)
def _boom_m(g):
    return boom(g, 0.5, (190.0, 72.0), (4400.0, 750.0), 0.25)


@fx('boom_l', a=0.001, r=0.10)
def _boom_l(g):
    return boom(g, 0.9, (140.0, 50.0), (3600.0, 480.0), 0.5)


@fx('hit', a=0.001, r=0.01)
def _hit(g):
    """a metallic tink"""
    n = N(0.08)
    t = tt(n)
    x = ring(n, [(2350, 1.0, 0.020), (3620, 0.7, 0.013), (5210, 0.45, 0.009)])
    return x + 0.5 * unit(filt(g.standard_normal(n), 3000, None)) * np.exp(-t / 0.002)


@fx('die1', a=0.003, r=0.04)
def _die1(g):
    """a squelch: a wet, wobbling band over a falling saw, and bubbles"""
    n = N(0.25)
    t = tt(n)
    src = unit(wave(glide(430, 95, n) * (1 + 0.25 * np.sin(TAU * 23 * t)), n, 'saw')) + 0.5 * g.standard_normal(n)
    x = unit(svf(src, 600 + 1000 * np.abs(np.sin(TAU * 9 * t)), 4.0, 'bp')) * np.exp(-t / 0.14)
    for at in (0.03, 0.10, 0.165):
        nb = N(0.035)
        put(x, np.sin(phase(glide(480, 1500, nb))) * np.hanning(nb), at, 0.6)
    return x


@fx('die2', a=0.004, r=0.08)
def _die2(g):
    """a shriek: a high saw falling, shaking fast"""
    n = N(0.3)
    t = tt(n)
    f = glide(2400, 950, n) * (1 + 0.06 * np.sin(TAU * 38 * t))
    x = unit(wave(f, n, 'saw')) + 0.4 * unit(wave(f * 1.49, n, 'square'))
    x = unit(filt(x, 1200, 5500)) + 0.25 * unit(filt(g.standard_normal(n), 2500, 6000))
    return x * (0.6 + 0.4 * np.exp(-t / 0.1))


@fx('die3', a=0.001, r=0.03)
def _die3(g):
    """a pop: a bubble that bursts upwards, and a splat"""
    n = N(0.2)
    t = tt(n)
    x = np.zeros(n)
    nb = N(0.032)
    put(x, np.sin(phase(glide(270, 1750, nb))) * np.hanning(nb) ** 0.5, 0.0, 1.0)
    ns = n - N(0.028)
    ts = tt(ns)
    splat = unit(filt(g.standard_normal(ns), 700, 3600)) * np.exp(-ts / 0.035)
    splat += 0.5 * np.sin(phase(glide(620, 160, ns))) * np.exp(-ts / 0.05)
    put(x, splat, 0.028, 0.8)
    return x


def growl(g, n, f, f1, f2, rough=29.0):
    """a throat: a rough saw and breath through two formants that move"""
    t = tt(n)
    jitter = unit(filt(g.standard_normal(n), None, 40.0))
    src = unit(wave(f * (1 + 0.04 * np.sin(TAU * 27 * t) + 0.03 * jitter), n, 'saw'))
    src = src * (1.0 - 0.5 * (0.5 + 0.5 * np.sin(TAU * rough * t))) + 0.3 * g.standard_normal(n)
    return (unit(svf(src, f1, 5.0, 'bp')) + 0.7 * unit(svf(src, f2, 6.0, 'bp')) + 0.35 * unit(filt(src, 2100, 2900))
            + 0.35 * unit(filt(src, None, 320)))


@fx('boss_roar', a=0.02, r=0.10)
def _boss_roar(g):
    n = N(1.0)
    f = 90.0 * shape(n, [(0, 0.8), (0.15, 1.18), (0.6, 1.0), (1, 0.68)])
    x = growl(g, n, f, shape(n, [(0, 480), (0.3, 820), (1, 440)]), shape(n, [(0, 1000), (0.3, 1350), (1, 900)]))
    return np.tanh(2.0 * unit(x) * shape(n, [(0, 0), (0.08, 1), (0.5, 0.9), (0.85, 0.5), (1, 0)]))


@fx('boss_die', a=0.01, r=0.15)
def _boss_die(g):
    """the roar falls apart: its pitch sinks and shakes while three
    explosions go off inside it"""
    n = N(1.5)
    t = tt(n)
    nr = N(1.2)
    f = glide(170, 38, nr)
    roar = growl(g, nr, f, glide(900, 300, nr), glide(1500, 700, nr), rough=22.0)
    roar *= shape(nr, [(0, 0), (0.05, 1), (0.6, 0.7), (1, 0)]) * (0.7 + 0.3 * np.sin(phase(glide(9, 22, nr))))
    x = np.zeros(n)
    put(x, roar, 0.0, 1.0)
    put(x, boom(g, 0.3, (260.0, 110.0), (5600.0, 1300.0)), 0.18, 0.7)
    put(x, boom(g, 0.45, (190.0, 72.0), (4400.0, 750.0), 0.25), 0.50, 0.85)
    put(x, boom(g, 0.66, (140.0, 50.0), (3600.0, 480.0), 0.5), 0.84, 1.1)
    return np.tanh(1.8 * unit(x))


@fx('fort_hit', a=0.001, r=0.04)
def _fort_hit(g):
    """an alarmed clang: struck plate with two partials a semitone apart,
    which beat"""
    n = N(0.4)
    t = tt(n)
    x = ring(n, [(410, 0.8, 0.20), (965, 1.0, 0.17), (1030, 0.9, 0.17), (1480, 0.6, 0.12), (2210, 0.5, 0.09),
                 (2950, 0.4, 0.07), (3870, 0.3, 0.05)], g)
    x = unit(x) + 0.5 * np.sin(phase(110 + 160 * np.exp(-t / 0.02))) * np.exp(-t / 0.05)
    return x + 0.8 * unit(filt(g.standard_normal(n), 600, 4000)) * np.exp(-t / 0.015)


@fx('alarm', a=0.006, r=0.02)
def _alarm(g):
    """a klaxon on two notes, twice"""
    n = N(0.8)
    x = np.zeros(n)
    for i in range(4):
        m = N(0.2)
        f = (640.0, 480.0)[i % 2] * shape(m, [(0, 0.94), (0.2, 1.0), (1, 1.0)])
        tone = wave(f, m, 'pulse', duty=0.35) + 0.5 * wave(f * 1.006, m, 'saw')
        put(x, edges(tone, 0.008, 0.012), 0.2 * i, 1.0)
    return filt(x, 450, 3800)


@fx('build', a=0.001, r=0.02)
def _build(g):
    """a clunk, a latch, and a chirp that rises"""
    n = N(0.35)
    x = np.zeros(n)

    def clunk(d, f):
        m = N(d)
        t = tt(m)
        c = np.sin(phase(f + 2 * f * np.exp(-t / 0.012))) * np.exp(-t / 0.045)
        c += unit(filt(g.standard_normal(m), 400, 2600)) * np.exp(-t / 0.016)
        return c + 0.5 * ring(m, [(830, 1.0, 0.03), (1490, 0.7, 0.02)])
    put(x, clunk(0.12, 75.0), 0.0, 1.0)
    put(x, clunk(0.08, 120.0), 0.075, 0.55)
    m = N(0.21)
    ph = phase(glide(700, 2200, m))
    put(x, edges(np.sin(ph) + 0.3 * np.sin(2 * ph), 0.008, 0.05), 0.135, 0.75)
    return x


def _ping(f, d, tau, bright=0.4):
    n = N(d)
    t = tt(n)
    return edges((np.sin(TAU * f * t) + bright * np.sin(TAU * 2 * f * t) * np.exp(-t / (tau * 0.5))
                  + 0.2 * np.sin(TAU * 3 * f * t) * np.exp(-t / (tau * 0.3))) * np.exp(-t / tau), 0.002, 0.01)


@fx('upgrade', a=0.002, r=0.05)
def _upgrade(g):
    """a bright arpeggio going up two octaves"""
    n = N(0.5)
    x = np.zeros(n)
    for i, nm in enumerate(('C5', 'E5', 'G5', 'C6', 'E6', 'G6')):
        last = i == 5
        d = 0.19 if last else 0.10
        m = N(d)
        f = mhz(nm)
        tone = unit(wave(f, m, 'pulse', duty=0.3)) * np.exp(-tt(m) / (0.09 if last else 0.05))
        put(x, edges(tone, 0.002, 0.02) + 0.6 * _ping(f * 2, d, 0.08 if last else 0.04), 0.062 * i, 0.8 + 0.04 * i)
    return x


@fx('sell', a=0.001, r=0.02)
def _sell(g):
    """coins dropping: five small rings of metal, the last ones settling"""
    n = N(0.35)
    x = np.zeros(n)
    for at, f, a in ((0.0, 3150, 1.0), (0.055, 3920, 0.8), (0.115, 2790, 0.9), (0.165, 4350, 0.6), (0.225, 3400, 0.7)):
        m = N(0.11)
        c = ring(m, [(f, 1.0, 0.035), (f * 1.48, 0.6, 0.025), (f * 0.52, 0.35, 0.02)])
        c += 0.4 * unit(filt(g.standard_normal(m), 3000, None)) * np.exp(-tt(m) / 0.002)
        put(x, edges(c, 0.0005, 0.01), at, a)
    return x


@fx('coin', a=0.001, r=0.015)
def _coin(g):
    """a small ping on two notes"""
    n = N(0.12)
    x = np.zeros(n)
    put(x, _ping(1976.0, 0.04, 0.03), 0.0, 0.8)
    put(x, _ping(2637.0, 0.085, 0.035), 0.035, 1.0)
    return x


def jingle(d, events, room=0.2, seed='jingle'):
    """a phrase for the jingles: (seconds, instrument, 'C5+E5', seconds
    held, level) laid on a line, with a little room"""
    n = N(d)
    x = np.zeros(n)
    for at, instr, names, hold, gain in events:
        for nm in names.split('+'):
            put(x, unit(instr(mhz(nm), N(hold))), at, gain)
    return reverb(x, room, 0.22, seed)


@fx('wave_start', a=0.01, r=0.08)
def _wave_start(g):
    """a horn call: a fifth up, the second note held; the pitch slides
    into each note like a siren"""
    n = N(0.9)
    f = np.concatenate([glide(350, 392, N(0.05)), np.full(N(0.17), 392.0), glide(392, 587.3, N(0.06)),
                        np.full(n - N(0.28), 587.3)])
    x = wave(f, n, 'saw') + wave(f * 1.006, n, 'saw') + 0.6 * wave(f * 0.5, n, 'square')
    bright = shape(n, [(0, 700), (0.12, 2600), (0.27, 1500), (0.45, 3400), (1, 2200)])
    x = unit(svf(x, bright, 1.2, 'lp'))
    return x * shape(n, [(0, 0), (0.04, 0.9), (0.24, 0.7), (0.30, 0.6), (0.42, 1.0), (0.8, 0.9), (1, 0)])


@fx('wave_clear', a=0.004, r=0.08)
def _wave_clear(g):
    """a short fanfare: three quick notes up to a chord"""
    return jingle(0.9, [(0.00, i_brass, 'C5', 0.09, 1.0), (0.11, i_brass, 'E5', 0.09, 1.0),
                        (0.22, i_brass, 'G5', 0.09, 1.0), (0.33, i_brass, 'C6', 0.42, 1.0),
                        (0.33, i_brass, 'E5+G5', 0.42, 0.55), (0.33, i_bell, 'C7', 0.3, 0.25),
                        (0.33, i_bass, 'C3', 0.4, 0.5)], seed='wave_clear')


@fx('victory', a=0.004, r=0.12)
def _victory(g):
    """the fanfare of a won world: a rising call, then three chords home
    (flat six, flat seven, one) with drums under them"""
    s = 0.115
    ev = [(0 * s, i_brass, 'G4', 0.09, 0.9), (1 * s, i_brass, 'C5', 0.09, 0.9), (2 * s, i_brass, 'E5', 0.09, 0.9),
          (3 * s, i_brass, 'G5', 0.20, 1.0), (5 * s, i_brass, 'E5', 0.09, 0.9), (6 * s, i_brass, 'G5', 0.09, 0.9),
          (7 * s, i_brass, 'C6', 0.30, 1.0), (7 * s, i_brass, 'E5+G5', 0.30, 0.6), (7 * s, i_bass, 'C3', 0.30, 0.6),
          (10 * s, i_brass, 'Eb6', 0.19, 1.0), (10 * s, i_brass, 'Ab5+C6', 0.19, 0.6), (10 * s, i_bass, 'Ab2', 0.2, 0.6),
          (12 * s, i_brass, 'F6', 0.19, 1.0), (12 * s, i_brass, 'Bb5+D6', 0.19, 0.6), (12 * s, i_bass, 'Bb2', 0.2, 0.6),
          (14 * s, i_brass, 'G6', 0.70, 1.0), (14 * s, i_brass, 'C6+E6', 0.70, 0.65), (14 * s, i_brass, 'G4+C5', 0.70, 0.45),
          (14 * s, i_bass, 'C3', 0.7, 0.7), (14 * s, i_bell, 'C7', 0.5, 0.3), (15.5 * s, i_bell, 'G7', 0.5, 0.2),
          (17 * s, i_bell, 'E7', 0.5, 0.2)]
    x = jingle(2.5, ev, 0.25, 'victory')
    for at, f, a in ((7 * s, 131.0, 0.8), (10 * s, 104.0, 0.7), (12 * s, 117.0, 0.7), (14 * s, 131.0, 1.0)):
        put(x, unit(i_timp(f, 0)), at, 0.9 * a)
    put(x, d_crash(g, 0.9, 0.3), 14 * s, 0.5)
    return np.tanh(1.2 * unit(x))


@fx('defeat', a=0.01, r=0.25)
def _defeat(g):
    """a falling phrase in a minor key, over chords that sink; the last
    note droops"""
    s = 0.15
    ev = [(0 * s, i_flute, 'E5', 3 * s * 0.95, 1.0), (3 * s, i_flute, 'D5', s * 0.9, 0.9),
          (4 * s, i_flute, 'C5', 3 * s * 0.95, 1.0), (7 * s, i_flute, 'B4', s * 0.9, 0.9),
          (8 * s, i_flute, 'C5', 2 * s * 0.95, 0.95),
          (0 * s, i_pad, 'A3+C4+E4', 4 * s, 0.33), (4 * s, i_pad, 'F3+A3+C4', 4 * s, 0.33),
          (8 * s, i_pad, 'D3+F3+A3', 2 * s, 0.33), (10 * s, i_pad, 'A2+E3+A3+C4', 5 * s, 0.33),
          (0 * s, i_bass, 'A2', 3.5 * s, 0.5), (4 * s, i_bass, 'F2', 3.5 * s, 0.5), (8 * s, i_bass, 'D2', 1.8 * s, 0.5),
          (10 * s, i_bass, 'A1', 4 * s, 0.6)]
    x = jingle(2.5, ev, 0.3, 'defeat')
    # the last note of the tune, A4, sinking a semitone as it fades
    m = N(0.95)
    ph = phase(440.0 * shape(m, [(0, 1.0), (0.35, 1.0), (1, 0.94)]) * (1 + 0.006 * np.sin(TAU * 5 * tt(m))))
    last = (np.sin(ph) + 0.32 * np.sin(2 * ph) + 0.12 * np.sin(3 * ph)) * shape(m, [(0, 0), (0.05, 1), (0.4, 0.8), (1, 0)])
    put(x, reverb(last, 0.3, 0.22, 'defeat'), 10 * s, 1.0)
    return x


@fx('tap', a=0.001, r=0.006)
def _tap(g):
    n = N(0.04)
    t = tt(n)
    return np.sin(TAU * 1500 * t) * np.exp(-t / 0.006) + 0.5 * unit(filt(g.standard_normal(n), 3000, None)) * np.exp(-t / 0.002)


@fx('deny', a=0.004, r=0.012)
def _deny(g):
    """a low buzz in two short pushes, on a sour interval"""
    n = N(0.15)
    x = np.zeros(n)
    for at, d in ((0.0, 0.065), (0.08, 0.07)):
        m = N(d)
        put(x, edges(wave(150.0, m, 'square') + 0.7 * wave(112.0, m, 'saw'), 0.004, 0.01), at, 1.0)
    return filt(x, 340, 3500, 3)


@fx('strike', a=0.02, r=0.15)
def _strike(g):
    """the orbital strike: a whistle that falls for a second, then the
    biggest blast of the game"""
    n = N(1.6)
    x = np.zeros(n)
    m = N(0.96)
    t = tt(m)
    ph = phase(glide(3900, 620, m) * (1 + 0.012 * np.sin(TAU * 14 * t)))
    u = np.linspace(0, 1, m)
    put(x, edges((np.sin(ph) + 0.25 * np.sin(2 * ph) * np.clip((u - 0.4) / 0.25, 0, 1)) * (0.12 + 0.6 * u ** 1.6), 0.02, 0.01),
        0.0, 1.0)
    put(x, boom(g, 0.68, (150.0, 45.0), (4600.0, 420.0), 0.6), 0.92, 1.25)
    return x


@fx('freeze', a=0.001, r=0.12)
def _freeze(g):
    """a big crystalline crash: a crack, a shower of glass partials struck
    within a tenth of a second, and a few late tinkles"""
    n = N(0.9)
    t = tt(n)
    x = np.zeros(n)
    for i in range(26):
        f = float(np.exp(g.uniform(np.log(1500), np.log(6000))))
        m = n - N(0.1)
        put(x, ring(m, [(f, 1.0, g.uniform(0.07, 0.32))], g) * np.minimum(1, tt(m) / 0.001), g.uniform(0, 0.09), g.uniform(0.4, 1))
    x = unit(x)
    x += 0.9 * unit(filt(g.standard_normal(n), 2600, 6500)) * np.exp(-t / 0.05)
    x += 0.9 * unit(filt(g.standard_normal(n), 300, 1400)) * np.exp(-t / 0.018)
    for at in (0.28, 0.37, 0.49, 0.58, 0.70):
        put(x, _ping(float(g.uniform(3000, 6000)), 0.12, 0.03, 0.2), at, 0.35)
    return x


@fx('repair', a=0.03, r=0.15)
def _repair(g):
    """a warm shimmer that rises an octave, with sparks of a major chord
    on top"""
    n = N(0.8)
    t = tt(n)
    f = glide(262, 523, n)
    x = np.zeros(n)
    for i, (ratio, a) in enumerate(((1.0, 1.0), (1.5, 0.7), (2.0, 0.6), (2.52, 0.4), (3.0, 0.3))):
        ph = phase(f * ratio * (1 + 0.003 * i))
        x += a * (np.sin(ph) + 0.25 * np.sin(2 * ph)) * (0.75 + 0.25 * np.sin(TAU * (4.5 + 1.3 * i) * t + i))
    x = unit(x) * shape(n, [(0, 0), (0.15, 0.6), (0.7, 1.0), (1, 0)])
    for at, nm in ((0.30, 'C6'), (0.41, 'E6'), (0.52, 'G6'), (0.63, 'C7')):
        put(x, _ping(mhz(nm), 0.16, 0.06, 0.3), at, 0.4)
    return x


@fx('star', a=0.001, r=0.06)
def _star(g):
    """a sparkling ding: a bell, and four tiny ones scattered after it"""
    n = N(0.4)
    x = ring(n, [(2093, 1.0, 0.13), (3136, 0.6, 0.10), (4186, 0.4, 0.07), (5274, 0.25, 0.05)])
    for at, f in ((0.06, 4699), (0.11, 6272), (0.17, 5274), (0.24, 6645)):
        put(x, _ping(float(f), 0.1, 0.03, 0.1), at, 0.35)
    return x


# ---------------------------------------------------------------------------
# Music. A Song is a ring of bars; its tracks are added into it and
# whatever runs past the end comes back in at the start.
# ---------------------------------------------------------------------------

_QUAL = {'': (0, 4, 7), 'm': (0, 3, 7), '7': (0, 4, 7, 10), 'maj7': (0, 4, 7, 11), 'm7': (0, 3, 7, 10),
         'm9': (0, 3, 7, 10, 14), 'sus4': (0, 5, 7), 'dim': (0, 3, 6)}


def _root(name):
    i = 2 if len(name) > 1 and name[1] in '#b' else 1
    return midi(name[:i] + '0') % 12, name[i:]


def chord(name, lo=57):
    """the chord's notes, each put in the octave that starts at lo: close
    voicings that move little from chord to chord"""
    pc, q = _root(name)
    notes = sorted(lo + ((pc + iv - lo) % 12) for iv in _QUAL[q])
    # no two notes a semitone apart: the lower of such a pair goes up an
    # octave (the seventh of a major seventh, the ninth of a minor ninth)
    for _ in range(3):
        for i in range(len(notes) - 1):
            if notes[i + 1] - notes[i] == 1:
                notes[i] += 12
        notes.sort()
    return notes


def root(name, lo=36):
    pc, _ = _root(name)
    return lo + ((pc - lo) % 12)


def parse(s):
    """'D5:3 -:1 F#5+A5:4' -> [([74], 3.0), ([], 1.0), ([78, 81], 4.0)]:
    notes and how many steps each lasts"""
    out = []
    for tok in s.split():
        names, ln = tok.split(':')
        out.append(([] if names == '-' else [midi(x) for x in names.split('+')], float(ln)))
    return out


class Song:
    def __init__(self, name, step, spb, bars):
        self.name, self.step, self.spb, self.bars = name, step, spb, bars
        self.n = step * spb * bars
        self.tracks = {}
        self.cache = {}
        self.g = rng_for(name)

    def add(self, track, x, at, gain=1.0, wet=False):
        if track not in self.tracks:
            self.tracks[track] = [np.zeros(self.n), wet]
        b = self.tracks[track][0]
        at = int(at) % self.n
        while len(x):
            m = min(len(x), self.n - at)
            b[at:at + m] += gain * x[:m]
            x = x[m:]
            at = 0

    def note(self, instr, m, n_on):
        key = (instr.__name__, m, n_on)
        if key not in self.cache:
            self.cache[key] = unit(instr(mhz(m), n_on))
        return self.cache[key]

    def at(self, track, instr, bar, step, notes, ln, gain=1.0, gate=0.9, wet=False):
        """notes (one MIDI number or several) at a step of a bar, lasting
        ln steps"""
        if isinstance(notes, (int, np.integer)):
            notes = [notes]
        n_on = max(64, int(ln * self.step * gate))
        for m in notes:
            self.add(track, self.note(instr, int(m), n_on), int(round((bar * self.spb + step) * self.step)), gain, wet)

    def line(self, track, instr, bars, at_bar=0, gain=1.0, gate=0.92, tr=0, wet=False):
        """a part written a bar a string (see parse); every bar must add
        up to the bar's steps"""
        for i, s in enumerate(bars):
            pos = 0.0
            for notes, ln in parse(s):
                if notes:
                    self.at(track, instr, at_bar + i, pos, [m + tr for m in notes], ln, gain, gate, wet)
                pos += ln
            assert abs(pos - self.spb) < 1e-6, (self.name, track, at_bar + i, s, pos)

    def hits(self, track, sample, pattern, bars, gain=1.0):
        """a drum on a pattern of one character a step (or a half step, if
        the pattern is twice as long): X loud, x, o soft, . nothing"""
        div = len(pattern) / float(self.spb)
        for b in bars:
            for i, c in enumerate(pattern):
                if c != '.':
                    self.add(track, sample, int(round((b * self.spb + i / div) * self.step)),
                             gain * {'X': 1.0, 'x': 0.72, 'o': 0.42}[c])

    def render(self, peak=0.5, room=0.3, tau=0.3, drive=1.5):
        dry = sum(b for b, wet in self.tracks.values() if not wet)
        wet = sum(b for b, wet in self.tracks.values() if wet)
        x = dry + reverb(wet, room, tau, self.name, circular=True, length=1.2)
        x = filt(x, 85.0, 7400.0, 3, circular=True)
        x = np.tanh(drive * unit(x))
        return (x * (peak / np.max(np.abs(x)))).astype(np.float32)

    def levels(self):
        return {k: float(np.sqrt(np.mean(b * b))) for k, (b, w) in self.tracks.items()}


SONGS = {}


def song(name):
    def deco(fn):
        SONGS[name] = fn
        return fn
    return deco


@song('music0')
def _music0():
    """The first worlds: a hopeful march for the outpost. D major, 117 to
    the minute, sixteen bars: eight of the tune, eight that climb higher
    and come back down to the dominant."""
    s = Song('music0', 2048, 16, 16)
    ch = ['D', 'D', 'G', 'A', 'Bm', 'G', 'A', 'A', 'G', 'A', 'F#m', 'Bm', 'G', 'Em', 'A', 'A']
    tune = ['D5:3 D5:1 F#5:4 A5:4 F#5:4',
            'D5:3 E5:1 F#5:4 D5:8',
            'B4:3 B4:1 D5:4 G5:4 D5:4',
            'E5:3 F#5:1 E5:4 C#5:6 -:2',
            'F#5:3 F#5:1 B5:4 A5:4 F#5:4',
            'G5:3 F#5:1 E5:4 D5:8',
            'E5:4 A5:4 G5:2 F#5:2 E5:4',
            'E5:8 -:4 A4:2 C#5:2',
            'B5:6 A5:2 G5:4 D5:4',
            'A5:6 G5:2 E5:4 C#5:4',
            'F#5:4 A5:4 C#6:6 A5:2',
            'B5:8 F#5:4 D5:4',
            'G5:3 G5:1 B5:4 D6:4 B5:4',
            'E5:3 E5:1 G5:4 B5:4 G5:4',
            'A5:4 C#6:4 E6:6 C#6:2',
            'A5:8 -:2 A4:2 B4:2 C#5:2']
    s.line('lead', i_lead, tune, gain=1.0, wet=True)
    s.line('horn', i_brass, tune[8:], at_bar=8, tr=-12, gain=0.5, wet=True)
    kick, snare, hat, crash = d_kick(s.g), d_snare(s.g, 0.16, 200.0, 0.055), d_hat(s.g, 0.018), d_crash(s.g, 1.0, 0.3)
    for b, c in enumerate(ch):
        r = root(c, 38)
        fifth = r + 7 if r + 7 <= 50 else r - 5
        # the march's bass: root on one and three, the fifth on two and four
        for st, m, ln in ((0, r, 3), (4, fifth, 3), (8, r, 3), (12, fifth, 2), (14, r + 12 if b % 2 else fifth, 2)):
            s.at('bass', i_bass, b, st, m, ln, 0.68, 0.7)
        notes = chord(c, 57)
        for st in (2, 6, 10, 14):
            s.at('stab', i_stab, b, st, notes, 2, 0.20, 0.8)
        s.at('pad', i_pad, b, 0, notes, 16, 0.10, 0.98, wet=True)
        if b >= 8:
            up = notes + [notes[0] + 12]
            for st in range(16):
                s.at('arp', i_pluck, b, st, up[(0, 1, 2, 3, 2, 1, 2, 1)[st % 8]] + 12, 1, 0.16 if st % 4 else 0.24)
    s.hits('kick', kick, 'X.......X.....o.', range(16), 0.80)
    s.hits('snare', snare, '....X..o....X.oo', range(0, 16, 2), 0.55)
    s.hits('snare', snare, '....X..o....Xo.o', (1, 5, 9, 13), 0.55)
    s.hits('snare', snare, '....X..o..o.xoxX', (3, 7, 11, 15), 0.55)
    s.hits('hat', hat, 'x.o.x.o.x.o.x.o.', range(16), 0.36)
    s.hits('crash', crash, 'X...............', (0, 8), 0.35)
    return s


@song('music1')
def _music1():
    """The jungle: mysterious and bubbling. E minor with the flat second
    and a tritone chord in the middle, 100 to the minute, sixteen bars. An
    ocarina winds over bubbles, hand drums and a bass that leaves gaps."""
    s = Song('music1', 2400, 16, 16)
    ch = ['Em9', 'Em9', 'Fmaj7', 'Fmaj7', 'Em9', 'Em9', 'Dm7', 'B7', 'Cmaj7', 'Cmaj7', 'Bbmaj7', 'Bbmaj7', 'Am9', 'Am9',
          'B7', 'B7']
    tune = ['E5:6 G5:2 B5:8',
            'A5:2 B5:2 A5:2 G5:2 E5:8',
            'F5:6 A5:2 C6:8',
            'B5:2 C6:2 B5:2 A5:2 F5:4 E5:4',
            'E5:6 G5:2 B5:4 D6:4',
            'E6:8 D6:2 B5:2 G5:4',
            'A5:6 F5:2 D5:8',
            'D#5:4 F#5:4 A5:4 C6:4',
            'B5:8 G5:4 E5:4',
            'G5:2 A5:2 B5:4 E5:8',
            'A5:8 F5:4 D5:4',
            'E5:2 F5:2 A5:4 D5:8',
            'C6:6 B5:2 A5:4 E5:4',
            'G5:2 A5:2 C6:2 A5:2 E5:8',
            'F#5:4 A5:4 B5:4 D#6:4',
            'B5:8 -:8']
    s.line('lead', i_flute, tune, gain=1.0, gate=0.96, wet=True)
    s.line('lead8', i_bell, [tune[i] if i in (7, 14) else '-:16' for i in range(16)], tr=12, gain=0.16, wet=True)
    order = (0, 3, 5, 2, 6, 4, 7, 3, 1, 5, 2, 6, 4, 0, 7, 5)
    kick, shaker, rim = d_kick(s.g, 150.0, 55.0, 0.18), d_shaker(s.g), d_rim(s.g)
    b_hi, b_lo, wood = d_bongo(s.g, 390.0), d_bongo(s.g, 255.0), d_wood(s.g, 900.0)
    for b, c in enumerate(ch):
        notes = chord(c, 64)
        two = notes + [m + 12 for m in notes]
        for st in range(16):
            if 'x.xxx.xxx.xx.xxx'[(st + 5 * (b % 2)) % 16] == 'x':
                s.at('bubbles', i_blip, b, st, two[order[st] % len(two)] + 12, 1, 0.46 if st % 2 == 0 else 0.30, wet=True)
        for st in (0, 3, 6, 10, 13):
            s.at('kalimba', i_pluck, b, st, notes[:4], 2, 0.20)
        r = root(c, 40)
        for st, m, ln in ((0, r, 3), (6, r, 2), (8, r + 7, 2), (11, r + 12, 1), (14, r + (11 if 'maj7' in c else 10), 2)):
            s.at('bass', i_bass, b, st, m, ln, 0.78, 0.75)
        s.at('pad', i_choir, b, 0, chord(c, 55)[:4], 16, 0.085, 0.98, wet=True)
    s.hits('kick', kick, 'X.....x...o.....', range(16), 0.85)
    s.hits('rim', rim, '....x.......x...', range(16), 0.45)
    s.hits('bongo_hi', b_hi, '..x..x....x..x.o', range(16), 0.5)
    s.hits('bongo_lo', b_lo, '....o...x.....x.', range(16), 0.55)
    s.hits('shaker', shaker, 'oxoxoxoxoxoxoxox', range(16), 0.42)
    s.hits('wood', wood, '.......o.....o..', range(1, 16, 2), 0.4)
    return s


@song('music2')
def _music2():
    """The middle worlds: driving and tense. A minor, 150 to the minute,
    twenty-four bars: the tune, a lower stretch that worries at a figure
    over the flat second, and the tune again an octave fuller."""
    s = Song('music2', 1600, 16, 24)
    ch = ['Am', 'Am', 'F', 'G', 'Am', 'Am', 'F', 'E',
          'Dm', 'Dm', 'Am', 'Am', 'Bb', 'Bb', 'E', 'E',
          'Am', 'Am', 'F', 'G', 'Am', 'Am', 'E', 'E']
    a = ['A4:3 C5:3 E5:2 A5:6 G5:2',
         'E5:3 G5:3 E5:2 D5:2 C5:2 D5:2 E5:2',
         'F5:3 A5:3 C6:2 A5:6 F5:2',
         'G5:3 B5:3 D6:2 B5:4 G5:2 B5:2',
         'A5:3 C6:3 E6:2 C6:6 A5:2',
         'E5:3 A5:3 C6:2 B5:2 A5:2 G5:2 E5:2']
    tune = a + ['F5:4 A5:4 C6:4 F6:4',
                'E6:6 D6:2 B5:4 G#5:4',
                'D5:2 -:1 D5:1 F5:2 -:2 A5:4 F5:2 D5:2',
                'D5:2 -:1 D5:1 F5:2 -:2 A5:2 G5:2 F5:2 E5:2',
                'C5:2 -:1 C5:1 E5:2 -:2 A5:4 E5:2 C5:2',
                'C5:2 -:1 C5:1 E5:2 -:2 A5:2 B5:2 C6:2 A5:2',
                'Bb4:2 -:1 Bb4:1 D5:2 -:2 F5:4 D5:2 Bb4:2',
                'Bb5:4 A5:4 F5:4 D5:4',
                'E5:2 -:1 E5:1 G#5:2 -:2 B5:4 G#5:2 E5:2',
                'E5:2 G#5:2 B5:2 D6:2 E6:4 -:4'] + a + [
                'D6:4 C6:4 B5:4 G#5:4',
                'E5:2 G#5:2 B5:2 E6:2 D6:2 B5:2 G#5:2 E5:2']
    s.line('lead', i_lead, tune, gain=1.0, gate=0.88, wet=True)
    s.line('lead8', i_brass, tune[16:], at_bar=16, tr=-12, gain=0.5, gate=0.88, wet=True)
    kick, snare, hat, hat_o = d_kick(s.g, 180.0, 52.0, 0.18), d_snare(s.g, 0.18, 210.0, 0.06), d_hat(s.g, 0.014), d_hat(s.g, 0.05)
    crash, tom = d_crash(s.g, 1.0, 0.3), d_tom(s.g, 150.0)
    octs = (0, 0, 12, 0, 0, 12, 0, 0, 12, 0, 0, 12, 0, 0, 12, 12)
    for b, c in enumerate(ch):
        r = root(c, 40)
        for st in range(16):
            s.at('bass', i_bass, b, st, r + octs[st], 1, 0.70 if st % 4 == 0 else 0.54, 0.62)
        notes = chord(c, 57)
        tense = 8 <= b < 16
        for st in ((0, 3, 6, 8, 11, 14) if not tense else (0, 6, 12)):
            s.at('stab', i_stab, b, st, notes, 2, 0.20, 0.85)
        s.at('pad', i_pad, b, 0, notes, 16, 0.09, 0.98, wet=True)
        if b >= 8:
            up = notes + [notes[0] + 12, notes[1] + 12]
            for st in range(16):
                s.at('arp', i_pluck, b, st, up[(0, 1, 2, 3, 4, 3, 2, 1)[st % 8]] + 12, 1, 0.15 if st % 2 else 0.22)
    full = [b for b in range(24) if not 8 <= b < 16]
    s.hits('kick', kick, 'X...X...X...X...', full, 0.72)
    s.hits('kick', kick, 'X.....x.X.....x.', range(8, 16), 0.72)
    s.hits('snare', snare, '....X.......X...', [b for b in range(24) if b % 8 != 7], 0.6)
    s.hits('snare', snare, '....X...x.xxXxXX', (7, 15, 23), 0.6)
    s.hits('tom', tom, '..........x..x..', (9, 11, 13), 0.5)
    s.hits('hat', hat, 'xoxoxoxoxoxoxoxo', range(24), 0.34)
    s.hits('hat_o', hat_o, '..x...x...x...x.', full, 0.30)
    s.hits('crash', crash, 'X...............', (0, 8, 16), 0.38)
    return s


@song('music3')
def _music3():
    """The last worlds: dark and epic. C minor in 12/8 (104 beats a
    minute, three quavers each), sixteen bars: low strings gallop, horns
    carry the tune under a choir, kettledrums mark the bars. It is written
    here in D minor and played a tone lower."""
    s = Song('music3', 3072, 12, 16)
    tr = -2
    ch = ['Dm', 'Bb', 'Gm', 'A', 'Dm', 'Bb', 'C', 'A', 'F', 'C', 'Gm', 'A', 'Bb', 'C', 'Dm', 'A']
    tune = ['D5:6 F5:3 A5:3',
            'Bb5:6 A5:3 F5:3',
            'G5:6 Bb5:3 D6:3',
            'C#6:6 A5:3 E5:3',
            'D5:3 F5:3 A5:3 D6:3',
            'F6:6 D6:3 Bb5:3',
            'E6:6 C6:3 G5:3',
            'A5:9 -:1 A5:1 A5:1',
            'A5:6 C6:3 F6:3',
            'E6:6 D6:2 C6:2 G5:2',
            'Bb5:6 D6:3 G5:3',
            'A5:6 C#6:3 E6:3',
            'F6:6 D6:3 Bb5:3',
            'G6:6 E6:3 C6:3',
            'F6:3 E6:3 D6:3 A5:3',
            'C#6:9 A5:3']
    s.line('horn', i_brass, tune, tr=tr - 12, gain=1.0, gate=0.95, wet=True)
    s.line('lead', i_lead, tune, tr=tr, gain=0.42, gate=0.95, wet=True)
    kick, snare, shaker = d_kick(s.g, 150.0, 45.0, 0.26), d_snare(s.g, 0.2, 175.0, 0.075), d_shaker(s.g)
    crash = d_crash(s.g, 1.4, 0.45)
    for b, c in enumerate(ch):
        r = root(c, 38) + tr
        fifth = r + 7 if r + 7 <= 48 else r - 5
        # the gallop: long, short short, long on every beat
        for beat in range(4):
            m = fifth if beat == 2 else r
            for off, ln, a in ((0, 1, 1.0), (1, 0.5, 0.6), (1.5, 0.5, 0.6), (2, 1, 0.8)):
                up = 12 if (beat == 3 and off == 2) else 0
                s.at('strings', i_str, b, beat * 3 + off, [m + 12 + up, m + 24 + up], ln, 0.30 * a, 0.7)
            s.at('bass', i_bass, b, beat * 3, m, 3, 0.66, 0.8)
        notes = [m + tr for m in chord(c, 57)]
        s.at('choir', i_choir, b, 0, notes, 12, 0.13, 0.98, wet=True)
        if b >= 8:
            s.at('choir', i_choir, b, 0, [notes[-1] + 12 if notes[-1] < 64 else notes[-1]], 12, 0.08, 0.98, wet=True)
        for st, a in ((0, 1.0), (6, 0.7), (10, 0.45), (11, 0.6)):
            s.at('timp', i_timp, b, st, r + 12, 1, 0.75 * a, wet=True)
        if b % 4 == 0:
            s.at('bell', i_bell, b, 0, [r + 36, r + 43], 6, 0.22, wet=True)
    s.hits('kick', kick, 'X....oX.....', range(16), 0.80)
    s.hits('snare', snare, '...X.....X..', [b for b in range(16) if b % 4 != 3], 0.6)
    s.hits('snare', snare, '......X...........xxxxXX', (3, 7, 11, 15), 0.6)
    s.hits('shaker', shaker, 'xoxxoxxoxxox', range(16), 0.38)
    s.hits('crash', crash, 'X...........', (0, 8), 0.42)
    return s


@song('music_menu')
def _music_menu():
    """The title screen: calm and spacious, a hero's theme. F major, 91 to
    the minute, twelve bars: four of bells over a harp, eight of the theme
    on a horn with an ocarina an octave above it."""
    s = Song('music_menu', 2625, 16, 12)
    ch = ['F', 'Bb', 'Dm', 'C', 'F', 'C', 'Dm', 'Bb', 'F', 'Am', 'Bb', 'C']
    bells = ['-:8 C6:4 F6:4', 'D6:8 Bb5:8', 'A5:4 D6:4 F6:8', 'E6:8 C6:4 G5:4']
    theme = ['C5:4 F5:6 G5:2 A5:4',
             'G5:8 E5:4 C5:4',
             'D5:4 F5:6 G5:2 A5:4',
             'Bb5:8 A5:4 F5:4',
             'C6:8 A5:4 F5:4',
             'E5:4 A5:4 C6:6 A5:2',
             'D6:8 Bb5:4 G5:4',
             'C6:6 G5:2 E5:8']
    s.line('bells', i_bell, bells, gain=0.60, wet=True)
    s.line('horn', i_brass, theme, at_bar=4, tr=-12, gain=1.0, gate=0.97, wet=True)
    s.line('flute', i_flute, theme, at_bar=4, gain=0.5, gate=0.97, wet=True)
    kick, rim, shaker, tom = d_kick(s.g, 140.0, 55.0, 0.2), d_rim(s.g), d_shaker(s.g), d_tom(s.g, 120.0)
    for b, c in enumerate(ch):
        notes = chord(c, 53)
        s.at('pad', i_pad, b, 0, notes, 16, 0.20, 0.98, wet=True)
        s.at('choir', i_choir, b, 0, [m + 12 for m in notes], 16, 0.07, 0.98, wet=True)
        up = notes + [m + 12 for m in notes]
        for i, k in enumerate((0, 2, 3, 4, 5, 4, 3, 2)):
            s.at('harp', i_pluck, b, 2 * i, up[k] + 12, 2, 0.40 if i % 2 == 0 else 0.30, wet=True)
        r = root(c, 41) if b != 5 else 40      # bar 6 is C over E: the bass walks down from F to D
        s.at('bass', i_bass, b, 0, r, 10, 0.62, 0.9)
        s.at('bass', i_bass, b, 12, r + 7 if r + 7 <= 52 else r - 5, 4, 0.46, 0.85)
        if b >= 4:
            s.at('bells', i_bell, b, 0, notes[-1] + 24, 4, 0.14, wet=True)
    s.hits('kick', kick, 'X.........x.....', range(12), 0.7)
    s.hits('rim', rim, '........x.......', range(12), 0.4)
    s.hits('shaker', shaker, 'o.x.o.x.o.x.o.x.', range(12), 0.36)
    s.hits('tom', tom, '..............o.', (3, 7, 11), 0.5)
    return s


# ---------------------------------------------------------------------------
# Building, measuring, writing
# ---------------------------------------------------------------------------

def build(only=None):
    """every sound, or the ones named: {name: float32 mono at RATE}"""
    if isinstance(only, str):
        only = [only]
    out = {}
    for name in NAMES:
        if only and name not in only:
            continue
        if name in FX:
            fn, a, r, peak = FX[name]
            out[name] = finish(fn(rng_for(name)), peak, a, r)
        else:
            out[name] = SONGS[name]().render()
    return out


def stats(x):
    x = np.asarray(x, float)
    p = np.abs(np.fft.rfft(x)) ** 2
    f = np.fft.rfftfreq(len(x), 1.0 / R)
    band = float(p[(f >= 300) & (f <= 5000)].sum() / p.sum())
    return dict(len=len(x) / R, peak=float(np.max(np.abs(x))), rms=float(np.sqrt(np.mean(x * x))), dc=float(np.mean(x)),
                centroid=float((f * p).sum() / p.sum()), band=band)


def seam(x):
    """how a loop joins: the step from its last sample to its first, and
    the typical and the largest steps inside it"""
    x = np.asarray(x, float)
    d = np.abs(np.diff(x))
    return abs(x[0] - x[-1]), float(np.median(d)), float(np.percentile(d, 99)), float(d.max())


def check(kit):
    """the promises of this file, asserted; prints the table"""
    print('%-13s %6s %6s %6s %8s %8s %6s' % ('name', 'sec', 'peak', 'rms', 'dc', 'centroid', '.3-5k'))
    for name, x in kit.items():
        st = stats(x)
        print('%-13s %6.3f %6.3f %6.3f %8.5f %8.0f %5.0f%%' % (name, st['len'], st['peak'], st['rms'], st['dc'],
                                                              st['centroid'], 100 * st['band']))
        assert x.dtype == np.float32 and x.ndim == 1, name
        assert np.all(np.isfinite(x)), name
        assert st['peak'] <= 1.0, name
        assert abs(st['dc']) < 0.004, (name, st['dc'])
        if name in MUSIC:
            assert abs(st['peak'] - 0.5) < 0.01, name
        else:
            assert abs(st['peak'] - 0.9) < 0.01, name
            assert abs(x[0]) < 0.02 and abs(x[-1]) < 0.02, name
    for name in MUSIC:
        if name in kit:
            x = kit[name]
            j, med, p99, top = seam(x)
            # the join must be an ordinary step of the piece, and the 20 ms
            # either side of it no rougher than the rest
            k = N(0.02)
            near = np.abs(np.diff(np.concatenate([x[-k:], x[:k]]).astype(float)))
            print('%-13s wrap step %.5f; steps inside: median %.5f, 99%% %.5f, max %.5f; max within 20 ms of the join %.5f'
                  % (name, j, med, p99, top, near.max()))
            assert j <= p99, (name, j, p99)
            assert near.max() <= top + 1e-9, name


def write_wav(path, x):
    with wavefile.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(R)
        w.writeframes((np.clip(x, -1, 1) * 32767.0).astype('<i2').tobytes())


def main():
    a = sys.argv[1:]
    only = None
    out = None
    if '--only' in a:
        only = a[a.index('--only') + 1].split(',')
    if '--wav' in a:
        out = a[a.index('--wav') + 1]
    if '--mix' in a:
        # how loud each track of each piece is, to balance them by numbers
        for name in MUSIC:
            if only and name not in only:
                continue
            sg = SONGS[name]()
            lv = sg.levels()
            ref = max(lv.values())
            print(name, ' '.join('%s %.2f' % (k, v / ref) for k, v in sorted(lv.items(), key=lambda kv: -kv[1])))
            x = sg.render().astype(float).reshape(sg.bars, -1)
            print('   rms by bar:', ' '.join('%.2f' % v for v in np.sqrt((x * x).mean(axis=1))))
        return
    t0 = time.time()
    kit = build(only)
    t1 = time.time()
    check(kit)
    again = build(only)
    for name in kit:
        assert np.array_equal(kit[name], again[name]), 'not the same twice: ' + name
    print('built %d sounds in %.1f s; a second build gave the same arrays' % (len(kit), t1 - t0))
    if out:
        os.makedirs(out, exist_ok=True)
        for name, x in kit.items():
            write_wav(os.path.join(out, name + '.wav'), x)
        print('written to', out)


if __name__ == '__main__':
    main()
