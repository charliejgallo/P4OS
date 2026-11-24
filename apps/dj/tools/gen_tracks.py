#!/usr/bin/env python3
"""Synthesized test tracks for the DJ app, with a tempo that is known.

Every sound is made here (sines, noise, saws), so the tracks carry no one
else's music and can go into the simulator's card, the screenshots and the
documents. Each track has an intro, a drop, a breakdown and an outro, so the
waveform shows its parts, and the beat grid is exact: the first kick is at
sample 0 plus the track's lead-in.

    python3 apps/dj/tools/gen_tracks.py OUT_DIR [--mp3] [--flac] [--short]

writes WAV files (16-bit stereo 44.1 kHz) and, with --mp3, the same as MP3
through `lame` (and deletes the WAV); --flac through `flac`. A file
tracks.txt next to them lists the name, the tempo and the lead-in in
samples: what the app's beat detection should find.
"""
import argparse
import os
import subprocess
import sys
import wave

import numpy as np

SR = 44100

TRACKS = [
    # name, bpm, key root (MIDI), minor, style, bars
    ("P4OS - Calle Norte", 124.0, 45, True, "house", 64),
    ("P4OS - Luz de Neon", 128.0, 41, False, "techno", 64),
    ("P4OS - Marea", 100.0, 43, True, "broken", 48),
    ("P4OS - Rayo", 174.0, 50, True, "dnb", 96),
    ("P4OS - Faro", 118.0, 48, False, "house", 56),
]


def midi_hz(n):
    return 440.0 * 2.0 ** ((n - 69) / 12.0)


def env_exp(n, tau):
    return np.exp(-np.arange(n) / (tau * SR))


def kick(n):
    t = np.arange(n) / SR
    f = 48 + 110 * np.exp(-t * 28)
    ph = 2 * np.pi * np.cumsum(f) / SR
    return np.sin(ph) * env_exp(n, 0.22) * 0.95


def clap(n, rng):
    e = np.zeros(n)
    for d in (0, 0.011, 0.022):
        i = int(d * SR)
        e[i:] += env_exp(n - i, 0.012 if d < 0.02 else 0.09)
    x = rng.standard_normal(n) * e
    # a band around 1.5 kHz
    return bandish(x, 900, 3500) * 0.5


def hat(n, rng, open_=False):
    x = rng.standard_normal(n)
    x = x - np.concatenate(([0], x[:-1]))          # brighter
    return x * env_exp(n, 0.09 if open_ else 0.025) * 0.18


def bandish(x, lo, hi):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, len(x))


def saw(freq, n, detune=0.0):
    t = np.arange(n) / SR
    out = np.zeros(n)
    for d in (-detune, 0.0, detune):
        ph = (t * freq * (1 + d)) % 1.0
        out += 2 * ph - 1
    return out / 3


def lowpass(x, fc):
    a = np.exp(-2 * np.pi * fc / SR)
    y = np.empty_like(x)
    s = 0.0
    for i in range(len(x)):
        s = (1 - a) * x[i] + a * s
        y[i] = s
    return y


def lowpass_fast(x, fc):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X *= 1 / np.sqrt(1 + (f / fc) ** 4)
    return np.fft.irfft(X, len(x))


def chord_tones(root, minor, degree):
    scale = [0, 2, 3, 5, 7, 8, 10] if minor else [0, 2, 4, 5, 7, 9, 11]
    return [root + 12 + scale[(degree + k) % 7] + 12 * ((degree + k) // 7) for k in (0, 2, 4)]


def add(buf, x, at, pan=0.0):
    if at >= buf.shape[0]:
        return
    n = min(len(x), buf.shape[0] - at)
    gl = np.sqrt(0.5 * (1 - pan))
    gr = np.sqrt(0.5 * (1 + pan))
    buf[at:at + n, 0] += x[:n] * gl * 1.414
    buf[at:at + n, 1] += x[:n] * gr * 1.414


def make(name, bpm, root, minor, style, bars, seed):
    rng = np.random.default_rng(seed)
    beat = 60.0 / bpm
    lead = int(0.25 * SR)                       # a quarter second of silence first
    n = lead + int(bars * 4 * beat * SR) + int(2 * SR)
    out = np.zeros((n, 2))
    K = kick(int(0.5 * SR))
    C = clap(int(0.35 * SR), rng)
    progression = [0, 5, 3, 4] if minor else [0, 4, 5, 3]

    def at_beat(b):
        return lead + int(round(b * beat * SR))

    # parts: intro 16 bars, drop A, breakdown 8, drop B, outro 8
    q = bars // 8
    parts = []
    for bar in range(bars):
        if bar < 2 * q:
            parts.append("intro")
        elif bar < bars - 3 * q:
            parts.append("drop" if bar < bars // 2 or bar >= bars // 2 + q else "break")
        elif bar < bars - q:
            parts.append("drop")
        else:
            parts.append("outro")

    for bar in range(bars):
        part = parts[bar]
        b0 = bar * 4
        deg = progression[(bar // 2) % 4]
        drums = part != "break"
        if style == "dnb":
            kicks = [0, 2.5]
            claps = [1, 3]
        elif style == "broken":
            kicks = [0, 1.75, 2.5]
            claps = [1, 3]
        else:
            kicks = [0, 1, 2, 3]
            claps = [1, 3]
        if drums:
            for k in kicks:
                add(out, K, at_beat(b0 + k))
            if part != "intro" or bar >= q:
                for c in claps:
                    # a hip-hop snare is loud: it is what the head nods to
                    add(out, C * (1.8 if style == "broken" else 1.0), at_beat(b0 + c), pan=0.1)
            for s in range(8):
                h = hat(int(0.2 * SR), rng, open_=(s % 2 == 1 and style == "house"))
                vel = 1.0 if s % 2 == 1 else 0.45
                add(out, h * vel, at_beat(b0 + s * 0.5), pan=-0.35 if s % 2 else 0.35)
        # bass: eighths on the root of the chord, off the kick in house
        if part in ("drop", "outro") or (part == "intro" and bar >= q):
            bn = root - 12 + ([0, 2, 3, 5, 7, 8, 10] if minor else [0, 2, 4, 5, 7, 9, 11])[deg % 7]
            for s in range(8):
                if style in ("house", "techno") and s % 2 == 0:
                    continue
                if style == "broken" and s % 2 == 1:
                    continue
                ln = int(beat * 0.45 * SR)
                x = lowpass_fast(saw(midi_hz(bn), ln, 0.004), 380) * env_exp(ln, 0.25) * 0.55
                add(out, x, at_beat(b0 + s * 0.5))
        # chords: stabs in the drops, a long pad in the breakdown
        tones = chord_tones(root, minor, deg)
        if part == "break":
            if bar % 2 == 0:
                ln = int(8 * beat * SR)
                pad = sum(saw(midi_hz(t), ln, 0.006) for t in tones) / 3
                pad = lowpass_fast(pad, 1400) * np.minimum(1, np.arange(ln) / (0.8 * SR)) * 0.35
                add(out, pad, at_beat(b0), pan=0.0)
        elif part == "drop":
            for s in (0.5, 1.5, 2.75, 3.5) if style != "dnb" else (0.5, 2.0, 3.5):
                ln = int(beat * 0.3 * SR)
                st = sum(saw(midi_hz(t + 12), ln, 0.008) for t in tones) / 3
                st = lowpass_fast(st, 2600) * env_exp(ln, 0.09) * 0.32
                add(out, st, at_beat(b0 + s), pan=-0.25 if s < 2 else 0.25)
    # a soft master: normalize to -1 dBFS
    peak = np.max(np.abs(out))
    out *= 0.89 / peak
    return out, lead


def write_wav(path, x):
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--mp3", action="store_true")
    ap.add_argument("--flac", action="store_true")
    ap.add_argument("--short", action="store_true", help="16 bars each, for quick tests")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    lines = []
    for i, (name, bpm, root, minor, style, bars) in enumerate(TRACKS):
        if a.short:
            bars = 16
        x, lead = make(name, bpm, root, minor, style, bars, 1234 + i)
        base = os.path.join(a.out, name)
        write_wav(base + ".wav", x)
        kind = "wav"
        if a.flac and i == len(TRACKS) - 1:
            subprocess.run(["flac", "-s", "-f", "-o", base + ".flac", base + ".wav"], check=True)
            os.remove(base + ".wav")
            kind = "flac"
        elif a.mp3 and i != 2:          # one stays WAV, to try both
            subprocess.run(["lame", "--quiet", "-V2", base + ".wav", base + ".mp3"], check=True)
            os.remove(base + ".wav")
            kind = "mp3"
        lines.append(f"{name}.{kind}\t{bpm:.2f}\t{lead}")
        print(f"{name}.{kind}: {bpm} BPM, lead-in {lead} samples, {len(x) / SR:.1f} s")
    with open(os.path.join(a.out, "tracks.txt"), "w") as f:
        f.write("# name\tbpm\tfirst beat (samples at 44.1 kHz)\n" + "\n".join(lines) + "\n")


if __name__ == "__main__":
    sys.exit(main())
