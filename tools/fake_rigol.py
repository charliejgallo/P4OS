#!/usr/bin/env python3
"""
A pretend Rigol DS1104Z on the raw SCPI socket (TCP 5555), for the Banco's
oscilloscope in the simulator. Standard library only.

    python3 tools/fake_rigol.py [--port 5555] [--fast] [-v]

What it emulates, well enough for aos_scope.c and the tab:

  *IDN?  *RST  *CLS  *OPC?
  :RUN  :STOP  :SINGle  :AUToscale  :CLEar  :TFORce
  :TRIGger:STATus?                              TD / WAIT / AUTO / STOP
  :CHANnel<n>:DISPlay | :SCALe | :OFFSet | :PROBe | :COUPling   (? and set)
  :TIMebase[:MAIN]:SCALe                        (? and set)
  :TRIGger:EDGe:LEVel | :SOURce | :SLOPe         (? and set)
  :TRIGger:SWEep  :TRIGger:MODE                  (? and set, cosmetic)
  :WAVeform:SOURce | :MODE | :FORMat             (? and set)
  :WAVeform:PREamble?  :WAVeform:DATA?           1200 BYTE points, TMC block
  :MEASure:ITEM? <item>,CHANnel<n>               VPP VMAX VMIN VAVG VRMS FREQ PER...
  :DISPlay:DATA? ON,0,PNG                        an 800x480 PNG of the screen

Long and short forms, any case, several commands per line with ';'. A query
it does not know gets NO answer, as on the real one (the client must time
out, not hang).

The signals (at the probe tip):
  CH1  1 kHz sine, 0..3.3 V (3.3 Vpp around 1.65 V)
  CH2  5 kHz PWM 0..5 V, its duty cycle drifting 20..60 % every 5 s
  CH3  10 kHz triangle +-0.5 V (off at start)
  CH4  a 0.5 Hz ramp 0..2 V (a slow sawtooth)
plus noise. The screen is what the real one shows: 12 divisions of the
timebase around the trigger point, 8 of V/div around the offset, 25 counts a
division around byte 127 (V = (byte - Yorigin - Yreference) * Yincrement), so
turning the knobs in the app changes the trace; STOP freezes it; SINGle waits
for a trigger and stops.
"""
import argparse
import math
import random
import re
import socket
import socketserver
import struct
import sys
import threading
import time
import zlib

IDN = "RIGOL TECHNOLOGIES,DS1104Z,DS1ZA000000001,00.04.04.SP4"
COLORS = {1: (0xF5, 0xD9, 0x0A), 2: (0x39, 0xC0, 0xED), 3: (0xE8, 0x3E, 0xC8), 4: (0x3C, 0x78, 0xF0)}
INVALID = "9.9E37"

# ---------------------------------------------------------------------------
# SCPI words: long form, the short one is its capitals
# ---------------------------------------------------------------------------
WORDS = """RUN STOP SINGle AUToscale CLEar TFORce
CHANnel DISPlay SCALe OFFSet PROBe COUPling BWLimit INVert UNITs VERNier
TIMebase MAIN DELay MODE
TRIGger STATus EDGe LEVel SOURce SLOPe SWEep HOLDoff NREJect POSition
WAVeform FORMat PREamble DATA POINts STARt
MEASure ITEM
IDN RST CLS OPC ERRor SYSTem
POSitive NEGative RFALl NORMal MAXimum RAW BYTE WORD ASCii
VPP VMAX VMIN VAVG VRMS VAMP VTOP VBASe FREQuency PERiod PWIDth NWIDth PDUTy NDUTy
ON OFF DC AC GND AUTO SINGLE""".split()
SHORT = {}
for w in WORDS:
    s = "".join(c for c in w if c.isupper() or c.isdigit())
    SHORT[w.upper()] = s
    SHORT[s] = s


def canon(tok):
    """'CHANnel1' / 'chan1' -> ('CHAN', '1'); unknown words come back upper-cased."""
    t = tok.strip().upper()
    m = re.match(r"^([A-Z*]+?)(\d*)$", t)
    if not m:
        return t, ""
    word, num = m.group(1), m.group(2)
    word = word.lstrip("*")
    return SHORT.get(word, word), num


def canon_arg(a):
    w, n = canon(a)
    return w + n


def eng(v):
    return "%.6e" % v


# ---------------------------------------------------------------------------
# The instrument
# ---------------------------------------------------------------------------
class Scope:
    def __init__(self, fast):
        self.lock = threading.RLock()
        self.fast = fast
        self.reset()

    def reset(self):
        self.ch = {n: {"disp": n != 3, "scale": s, "offset": o, "probe": 10.0, "coup": "DC"}
                   for n, s, o in ((1, 1.0, -1.65), (2, 2.0, -5.0), (3, 0.5, 0.0), (4, 1.0, 1.0))}
        self.tb = 500e-6
        self.trig = {"level": 1.65, "source": 1, "slope": "POS", "sweep": "AUTO"}
        self.run = True
        self.single = False
        self.wav = {"source": 1, "mode": "NORM", "format": "BYTE"}
        self.acq = None
        self.t0_wall = time.time()

    # ---- the signals ----
    def duty(self, tw):
        return 0.40 + 0.20 * math.sin(2 * math.pi * tw / 5.0)

    def period(self, n):
        return {1: 1e-3, 2: 2e-4, 3: 1e-4, 4: 2.0}[n]

    def raw(self, n, t, tw):
        """The noiseless voltage of channel n at time t (tw: wall time, slow drifts)."""
        if n == 1:
            return 1.65 + 1.65 * math.sin(2 * math.pi * 1000.0 * t)
        if n == 2:
            p = 2e-4
            ph = (t / p) % 1.0
            d = self.duty(tw)
            edge = 1.5e-6 / p       # rise time, as a share of the period
            if ph < edge:
                return 5.0 * ph / edge
            if ph < d:
                return 5.0
            if ph < d + edge:
                return 5.0 * (1 - (ph - d) / edge)
            return 0.0
        if n == 3:
            ph = (t * 1e4) % 1.0
            return (4 * ph - 1) * 0.5 if ph < 0.5 else (3 - 4 * ph) * 0.5
        ph = (t / 2.0) % 1.0
        return 2.0 * ph

    def mean(self, n, tw):
        return {1: 1.65, 2: 5.0 * self.duty(tw), 3: 0.0, 4: 1.0}[n]

    def volt(self, n, t, tw):
        c = self.ch[n]["coup"]
        if c == "GND":
            return 0.0
        v = self.raw(n, t, tw)
        if c == "AC":
            v -= self.mean(n, tw)
        return v

    # ---- acquisitions ----
    def find_trigger(self, t_now, tw):
        n = self.trig["source"]
        if not 1 <= n <= 4 or not self.ch[n]["disp"]:
            return None
        p = self.period(n)
        steps = 2000
        lvl = self.trig["level"]
        slope = self.trig["slope"]
        prev = self.volt(n, t_now, tw)
        for i in range(1, steps + 1):
            t = t_now + p * i / steps
            v = self.volt(n, t, tw)
            up = prev < lvl <= v
            dn = prev > lvl >= v
            if (slope == "POS" and up) or (slope == "NEG" and dn) or (slope == "RFAL" and (up or dn)):
                # refine linearly between the two samples
                f = (lvl - prev) / (v - prev) if v != prev else 0.0
                return t - p / steps * (1 - f)
            prev = v
        return None

    def acquire(self, force=False):
        """The current acquisition: a trigger time and a noise seed."""
        with self.lock:
            now = time.time()
            if self.acq and not force:
                if not self.run:
                    return self.acq
                if now - self.acq["wall"] < max(12 * self.tb, 0.03):
                    return self.acq
            t_now = now - self.t0_wall
            if not self.run:
                if not self.acq:
                    self.acq = {"wall": now, "t0": t_now, "seed": 1, "tw": now, "status": "STOP"}
                return self.acq
            t0 = self.find_trigger(t_now, now)
            waiting = self.single or self.trig["sweep"] != "AUTO"
            if t0 is None and waiting and self.acq:
                self.acq["status"] = "WAIT"         # the old picture stays until a trigger
                return self.acq
            status = "TD" if t0 is not None else "WAIT" if waiting else "AUTO"
            self.acq = {"wall": now, "t0": t0 if t0 is not None else t_now, "seed": random.getrandbits(32),
                        "tw": now, "status": status}
            if self.single and status == "TD":
                self.run = False
                self.single = False
            return self.acq

    def samples(self, n, acq=None):
        """1200 volts of channel n across the 12 divisions, noise included."""
        acq = acq or self.acquire()
        c = self.ch[n]
        rnd = random.Random(acq["seed"] * 7 + n)
        sigma = 0.004 * c["probe"] / 10 + 0.02 * c["scale"] * 0.3
        xinc = self.tb * 12 / 1200
        t0 = acq["t0"] - 6 * self.tb
        tw = acq["tw"]
        return [self.volt(n, t0 + i * xinc, tw) + rnd.gauss(0, sigma) for i in range(1200)]

    def to_bytes(self, n, volts):
        c = self.ch[n]
        yinc = c["scale"] / 25.0
        out = bytearray(len(volts))
        for i, v in enumerate(volts):
            b = int(round(127 + (v + c["offset"]) / yinc))
            out[i] = 0 if b < 0 else 255 if b > 255 else b
        return bytes(out)

    def preamble(self):
        n = self.wav["source"]
        c = self.ch.get(n, self.ch[1])
        yinc = c["scale"] / 25.0
        xinc = self.tb * 12 / 1200
        return "0,0,1200,1,%s,%s,0,%s,%g,127" % (eng(xinc), eng(-6 * self.tb), eng(yinc), c["offset"] / yinc)

    def measure(self, item, n):
        if n not in self.ch or not self.ch[n]["disp"]:
            return INVALID
        v = self.samples(n)
        c = self.ch[n]
        # what the screen holds: the real one measures clipped data
        top = 4.1 * c["scale"] - c["offset"]
        bot = -4.1 * c["scale"] - c["offset"]
        v = [min(max(x, bot), top) for x in v]
        vmax, vmin = max(v), min(v)
        if item == "VMAX":
            return eng(vmax)
        if item == "VMIN":
            return eng(vmin)
        if item in ("VPP", "VAMP"):
            return eng(vmax - vmin)
        if item == "VAVG":
            return eng(sum(v) / len(v))
        if item == "VRMS":
            return eng(math.sqrt(sum(x * x for x in v) / len(v)))
        if item in ("VTOP", "VBAS"):
            return eng(vmax if item == "VTOP" else vmin)
        if item in ("FREQ", "PER", "PDUT", "NDUT", "PWID", "NWID"):
            p = self.period(n)
            if c["coup"] == "GND" or 12 * self.tb < p or 12 * self.tb > 2000 * p or vmax - vmin < c["scale"] * 0.3:
                return INVALID
            if item == "FREQ":
                return eng(1.0 / p * (1 + random.gauss(0, 2e-5)))
            if item == "PER":
                return eng(p * (1 + random.gauss(0, 2e-5)))
            d = self.duty(time.time()) if n == 2 else 0.5
            if item == "PDUT":
                return eng(d * 100)
            if item == "NDUT":
                return eng((1 - d) * 100)
            return eng(p * (d if item == "PWID" else 1 - d))
        return INVALID

    def autoscale(self):
        with self.lock:
            for n in (1, 2, 4):
                self.ch[n]["disp"] = True
            self.ch[3]["disp"] = False
            on = [n for n in (1, 2, 3, 4) if self.ch[n]["disp"]]
            for k, n in enumerate(on):
                tw = time.time()
                vs = [self.volt(n, i * self.period(n) / 400, tw) for i in range(400)]
                vpp = max(vs) - min(vs)
                mid = (max(vs) + min(vs)) / 2
                s = next125(max(vpp / 3.0, 0.002), up=True)
                self.ch[n]["scale"] = s
                # stacked like the real one: each channel in its own band
                band = (1.5 * len(on) - 1.5) / 2 - 1.5 * k
                self.ch[n]["offset"] = round((band * s - mid) / (s / 50)) * (s / 50)
            self.trig["source"] = 1
            self.trig["slope"] = "POS"
            self.trig["level"] = 1.65
            self.tb = 500e-6
            self.run = True
            self.single = False
            self.acq = None

    # ---- the screenshot ----
    def png(self):
        W, H = 800, 480
        img = bytearray(b"\x00\x00\x00" * (W * H))

        def px(x, y, rgb):
            if 0 <= x < W and 0 <= y < H:
                i = (y * W + x) * 3
                img[i:i + 3] = bytes(rgb)

        def rect(x0, y0, x1, y1, rgb):
            row = bytes(rgb) * (x1 - x0)
            for y in range(max(y0, 0), min(y1, H)):
                i = (y * W + x0) * 3
                img[i:i + len(row)] = row

        gx, gy, gw, gh = 100, 40, 600, 400          # the grid, 50 px a division
        rect(0, 0, W, 30, (0x20, 0x20, 0x28))
        rect(0, 450, W, H, (0x20, 0x20, 0x28))
        for i in range(13):
            for y in range(gy, gy + gh + 1, 5 if i in (0, 12) else 10):
                px(gx + i * 50, y, (0x80, 0x80, 0x80) if i in (0, 6, 12) else (0x50, 0x50, 0x50))
        for j in range(9):
            for x in range(gx, gx + gw + 1, 5 if j in (0, 8) else 10):
                px(x, gy + j * 50, (0x80, 0x80, 0x80) if j in (0, 4, 8) else (0x50, 0x50, 0x50))
        acq = self.acquire()
        for n in (4, 3, 2, 1):
            c = self.ch[n]
            if not c["disp"]:
                continue
            b = self.to_bytes(n, self.samples(n, acq))
            prev = None
            for x in range(gw):
                i0, i1 = x * 2, x * 2 + 2
                ys = [gy + gh // 2 - int((v - 127) * 2) for v in b[i0:i1]]
                lo, hi = min(ys), max(ys)
                if prev is not None:
                    lo, hi = min(lo, prev), max(hi, prev)
                prev = ys[-1]
                for y in range(max(lo, gy), min(hi, gy + gh) + 1):
                    px(gx + x, y, COLORS[n])
            y0 = gy + gh // 2 - int(c["offset"] / c["scale"] * 50)
            rect(gx - 14, y0 - 6, gx - 2, y0 + 7, COLORS[n])
            text(img, W, gx - 12, y0 - 3, str(n), (0, 0, 0))
            text(img, W, 20 + (n - 1) * 150, 460, "CH%d %s" % (n, fmt_eng(c["scale"], "V")), COLORS[n], 2)
        text(img, W, 10, 8, "RIGOL", (0xE0, 0xE0, 0xE0), 2)
        st = self.acquire()["status"] if self.run else "STOP"
        text(img, W, 100, 8, st, (0x30, 0xD1, 0x58) if st == "TD" else (0xFF, 0x45, 0x3A), 2)
        text(img, W, 200, 8, "H " + fmt_eng(self.tb, "S"), (0xE0, 0xE0, 0xE0), 2)
        text(img, W, 520, 8, "T CH%d %s" % (self.trig["source"], fmt_eng(self.trig["level"], "V")),
             (0xFF, 0x9F, 0x0A), 2)
        raw = b"".join(b"\x00" + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def next125(v, up=True):
    """The 1-2-5 value at or above v (up) or at or below it."""
    e = math.floor(math.log10(v))
    for k in range(-1, 3):
        for m in (1, 2, 5):
            x = m * 10 ** (e + k)
            if up and x >= v * 0.999:
                return x
    return v


def fmt_eng(v, unit):
    for p, f in (("", 1), ("M", 1e-3), ("U", 1e-6), ("N", 1e-9)):
        if abs(v) >= f * 0.999 or p == "N":
            s = "%g" % (v / f)
            return s + p + unit
    return "%g%s" % (v, unit)


# a 5x7 font for the screenshot's few words: each glyph 7 rows of 5 bits
FONT = {
    "0": "0E 11 13 15 19 11 0E", "1": "04 0C 04 04 04 04 0E", "2": "0E 11 01 02 04 08 1F",
    "3": "1F 02 04 02 01 11 0E", "4": "02 06 0A 12 1F 02 02", "5": "1F 10 1E 01 01 11 0E",
    "6": "06 08 10 1E 11 11 0E", "7": "1F 01 02 04 08 08 08", "8": "0E 11 11 0E 11 11 0E",
    "9": "0E 11 11 0F 01 02 0C", ".": "00 00 00 00 00 0C 0C", "-": "00 00 00 1F 00 00 00",
    " ": "00 00 00 00 00 00 00", "A": "0E 11 11 1F 11 11 11", "C": "0E 11 10 10 10 11 0E",
    "D": "1E 11 11 11 11 11 1E", "G": "0E 11 10 17 11 11 0F", "H": "11 11 11 1F 11 11 11",
    "I": "0E 04 04 04 04 04 0E", "L": "10 10 10 10 10 10 1F", "M": "11 1B 15 15 11 11 11",
    "N": "11 11 19 15 13 11 11", "O": "0E 11 11 11 11 11 0E", "P": "1E 11 11 1E 10 10 10",
    "R": "1E 11 11 1E 14 12 11", "S": "0F 10 10 0E 01 01 1E", "T": "1F 04 04 04 04 04 04",
    "U": "11 11 11 11 11 11 0E", "V": "11 11 11 11 11 0A 04", "W": "11 11 11 15 15 15 0A",
}


def text(img, W, x, y, s, rgb, scale=1):
    col = bytes(rgb)
    for ch in s.upper():
        rows = FONT.get(ch, FONT[" "]).split()
        for r, hx in enumerate(rows):
            bits = int(hx, 16)
            for cx in range(5):
                if bits & (0x10 >> cx):
                    for dy in range(scale):
                        for dx in range(scale):
                            X, Y = x + cx * scale + dx, y + r * scale + dy
                            if 0 <= X < W and 0 <= Y < 480:
                                i = (Y * W + X) * 3
                                img[i:i + 3] = col
        x += 6 * scale


# ---------------------------------------------------------------------------
# The command interpreter
# ---------------------------------------------------------------------------
POLLS = re.compile(r"^(WAV|MEAS|TRIG:STAT|CHAN\d|TIM|TRIG:EDG)")


class Session:
    def __init__(self, scope, verbose, out):
        self.s = scope
        self.verbose = verbose
        self.out = out          # function(bytes)
        self.polls = {}
        self.t_sum = time.time()

    def log(self, line):
        short = ":".join(canon(t)[0] + canon(t)[1] for t in line.split(" ")[0].strip(":").split(":"))
        poll = bool(POLLS.match(short)) and ("?" in line or short.startswith("WAV"))
        if self.verbose or not poll:
            print("<- %s" % line, flush=True)
        else:
            key = short.split(",")[0]
            self.polls[key] = self.polls.get(key, 0) + 1
        now = time.time()
        if now - self.t_sum >= 3 and self.polls:
            tot = sum(self.polls.values())
            top = ", ".join("%s %d" % kv for kv in sorted(self.polls.items(), key=lambda kv: -kv[1])[:6])
            print("   (%d queries in %.1f s: %s)" % (tot, now - self.t_sum, top), flush=True)
            self.polls = {}
            self.t_sum = now

    def delay(self, ms):
        if not self.s.fast and ms:
            time.sleep(ms / 1000.0)

    def handle_line(self, line):
        line = line.strip()
        if not line:
            return
        self.log(line)
        prefix = []
        for part in line.split(";"):
            part = part.strip()
            if not part:
                continue
            # a command without a leading ':' continues the previous path
            if not part.startswith(":") and not part.startswith("*") and prefix:
                part = ":" + ":".join(prefix) + ":" + part
            head, _, args = part.partition(" ")
            query = head.endswith("?")
            head = head.rstrip("?")
            toks = [t for t in head.split(":") if t]
            path = [canon(t) for t in toks]
            prefix = [w + n for w, n in path[:-1]]
            self.dispatch(path, query, [a.strip() for a in args.split(",")] if args.strip() else [])

    def reply(self, text):
        self.out((text + "\n").encode("ascii"))

    def block(self, data):
        self.out(b"#9%09d" % len(data) + data + b"\n")

    def dispatch(self, path, q, args):
        s = self.s
        words = [w for w, _ in path]
        # optional node: :TIMebase[:MAIN]:SCALe
        if words[:2] == ["TIM", "MAIN"]:
            path = path[:1] + path[2:]
            words = [w for w, _ in path]
        key = ":".join(words)
        num = path[0][1] if path else ""
        a0 = args[0] if args else ""
        with s.lock:
            if key == "IDN" and q:
                return self.reply(IDN)
            if key == "RST":
                return s.reset()
            if key in ("CLS", "CLE", "TFOR"):
                return
            if key == "OPC":
                return self.reply("1") if q else None
            if key == "RUN":
                s.run, s.single = True, False
                s.acq = None
                return
            if key == "STOP":
                s.run, s.single = False, False
                if s.acq:
                    s.acq["status"] = "STOP"
                return
            if key == "SING":
                s.run, s.single = True, True
                s.acq = None
                return
            if key == "AUT":
                s.autoscale()
                self.delay(1500)
                return
            if key == "TRIG:STAT" and q:
                if not s.run:
                    return self.reply("STOP")
                return self.reply(s.acquire()["status"])
            if words[0] == "CHAN" and num and int(num) in s.ch and len(words) == 2:
                c = s.ch[int(num)]
                item = words[1]
                if q:
                    self.delay(1)
                    if item == "DISP":
                        return self.reply("1" if c["disp"] else "0")
                    if item == "SCAL":
                        return self.reply(eng(c["scale"]))
                    if item == "OFFS":
                        return self.reply(eng(c["offset"]))
                    if item == "PROB":
                        return self.reply(eng(c["probe"]))
                    if item == "COUP":
                        return self.reply(c["coup"])
                    if item in ("BWL",):
                        return self.reply("OFF")
                    return print("   ?? unknown query, no answer", flush=True)
                if item == "DISP":
                    c["disp"] = canon_arg(a0) in ("1", "ON")
                    return
                if item == "SCAL":
                    v = float(a0)
                    lo, hi = 1e-3 * c["probe"], 10.0 * c["probe"]
                    c["scale"] = min(max(v, lo), hi)
                    lim = offs_limit(c["scale"], c["probe"])
                    c["offset"] = min(max(c["offset"], -lim), lim)
                    return
                if item == "OFFS":
                    lim = offs_limit(c["scale"], c["probe"])
                    c["offset"] = min(max(float(a0), -lim), lim)
                    return
                if item == "PROB":
                    p = float(a0)
                    if p in (0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000):
                        c["scale"] *= p / c["probe"]
                        c["offset"] *= p / c["probe"]
                        c["probe"] = p
                    return
                if item == "COUP":
                    v = canon_arg(a0)
                    if v in ("DC", "AC", "GND"):
                        c["coup"] = v
                    return
            if key == "TIM:SCAL":
                if q:
                    return self.reply(eng(s.tb))
                s.tb = min(max(float(a0), 5e-9), 50.0)
                s.acq = None if s.run else s.acq
                return
            if key == "TIM:MODE":
                return self.reply("MAIN") if q else None
            if words[:2] == ["TRIG", "EDG"] and len(words) == 3:
                it = words[2]
                if it == "LEV":
                    if q:
                        return self.reply(eng(s.trig["level"]))
                    s.trig["level"] = float(a0)
                    return
                if it == "SOUR":
                    if q:
                        return self.reply("CHAN%d" % s.trig["source"])
                    v = canon_arg(a0)
                    if v.startswith("CHAN") and v[4:].isdigit():
                        s.trig["source"] = int(v[4:])
                    return
                if it == "SLOP":
                    if q:
                        return self.reply(s.trig["slope"])
                    v = canon_arg(a0)
                    if v in ("POS", "NEG", "RFAL"):
                        s.trig["slope"] = v
                    return
            if key == "TRIG:SWE":
                if q:
                    return self.reply(s.trig["sweep"])
                v = canon_arg(a0)
                s.trig["sweep"] = {"SINGLE": "SING", "NORM": "NORM"}.get(v, "AUTO") if v != "SING" else "SING"
                return
            if key == "TRIG:MODE":
                return self.reply("EDGE") if q else None
            if words[0] == "WAV" and len(words) == 2:
                it = words[1]
                if it == "SOUR":
                    if q:
                        return self.reply("CHAN%d" % s.wav["source"])
                    v = canon_arg(a0)
                    if v.startswith("CHAN") and v[4:].isdigit():
                        s.wav["source"] = int(v[4:])
                    return
                if it == "MODE":
                    if q:
                        return self.reply(s.wav["mode"])
                    s.wav["mode"] = canon_arg(a0)
                    return
                if it == "FORM":
                    if q:
                        return self.reply(s.wav["format"])
                    s.wav["format"] = canon_arg(a0)
                    return
                if it == "PRE" and q:
                    self.delay(1)
                    return self.reply(s.preamble())
                if it == "DATA" and q:
                    n = s.wav["source"]
                    if n not in s.ch or not s.ch[n]["disp"]:
                        self.delay(2)
                        return self.block(b"")
                    data = s.to_bytes(n, s.samples(n))
                    self.delay(12)
                    if s.wav["format"] == "ASC":
                        c = s.ch[n]
                        return self.block(",".join(eng((b - 127) * c["scale"] / 25 - c["offset"]) for b in data).encode())
                    return self.block(data)
            if key == "MEAS:ITEM" and q:
                if len(args) < 1:
                    return
                item = canon_arg(args[0])
                src = canon_arg(args[1]) if len(args) > 1 else "CHAN1"
                n = int(src[4:]) if src.startswith("CHAN") and src[4:].isdigit() else 0
                self.delay(3)
                return self.reply(s.measure(item, n))
            if key == "DISP:DATA" and q:
                t = time.time()
                png = s.png()
                self.delay(max(0, 900 - (time.time() - t) * 1000))       # the real one takes seconds
                print("-> PNG %d bytes" % len(png), flush=True)
                return self.block(png)
            if key == "SYST:ERR" and q:
                return self.reply('0,"No error"')
        print("   ?? not understood%s: %s %s" % (" (no answer)" if q else "", key, ",".join(args)), flush=True)


def offs_limit(scale, probe):
    # the DS1000Z: +-100 V above 500 mV/div (x1), +-2 V below, both times the probe
    return (100.0 if scale / probe >= 0.5 else 2.0) * probe


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        peer = "%s:%d" % self.client_address
        print("== connected %s" % peer, flush=True)
        sock = self.request
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sess = Session(self.server.scope, self.server.verbose, sock.sendall)
        buf = b""
        try:
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        sess.handle_line(line.decode("ascii", "replace"))
                    except (ValueError, IndexError) as e:
                        print("   !! %s" % e, flush=True)
        except (ConnectionResetError, BrokenPipeError):
            pass
        print("== disconnected %s" % peer, flush=True)


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--fast", action="store_true", help="answer at once (no emulated latency)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every query too")
    a = ap.parse_args()
    srv = Server((a.host, a.port), Handler)
    srv.scope = Scope(a.fast)
    srv.verbose = a.verbose
    print("fake DS1104Z on %s:%d" % (a.host, a.port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
