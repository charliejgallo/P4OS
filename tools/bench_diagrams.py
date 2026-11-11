#!/usr/bin/env python3
"""Draws the bench wiring diagrams of docs/img/bench-*.svg.

    python3 tools/bench_diagrams.py

The 40-pin header is drawn as it faces you on the board: pin 1 at the top
RIGHT, odd pins down the right column, even pins down the left one (the
board's header is the mirror of a Raspberry Pi's drawing). Each diagram is
what was wired and tried on the board (docs/plan/PRUEBAS-PLACA.md).
"""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "img")

# pin -> (label, kind): kind p5 = 5 V, p3 = 3V3, gnd, gpio, no (not usable)
HEADER = {
    1: ("5V", "p5"), 2: ("3V3", "p3"), 3: ("5V", "p5"), 4: ("GPIO7 board SDA", "no"),
    5: ("GND", "gnd"), 6: ("GPIO8 board SCL", "no"), 7: ("GPIO37 console", "no"), 8: ("GPIO2", "gpio"),
    9: ("GPIO38 console", "no"), 10: ("GND", "gnd"), 11: ("GPIO5", "gpio"), 12: ("GPIO3", "gpio"),
    13: ("GND", "gnd"), 14: ("GPIO4", "gpio"), 15: ("GPIO21 SDA", "gpio"), 16: ("GPIO28", "gpio"),
    17: ("GPIO22 SCL", "gpio"), 18: ("3V3", "p3"), 19: ("GND", "gnd"), 20: ("GPIO29", "gpio"),
    21: ("GPIO24", "gpio"), 22: ("GPIO30", "gpio"), 23: ("GPIO25", "gpio"), 24: ("GPIO31", "gpio"),
    25: ("USB D-", "no"), 26: ("GND", "gnd"), 27: ("USB D+", "no"), 28: ("GPIO34", "gpio"),
    29: ("GND", "gnd"), 30: ("GPIO35 BOOT", "no"), 31: ("GPIO32", "gpio"), 32: ("GPIO49", "gpio"),
    33: ("GND", "gnd"), 34: ("GPIO50", "gpio"), 35: ("GPIO46", "gpio"), 36: ("GPIO51", "gpio"),
    37: ("GPIO47", "gpio"), 38: ("GPIO52", "gpio"), 39: ("GPIO48", "gpio"), 40: ("GND", "gnd"),
}

C = {"wire": "#b45309", "gnd": "#111111", "p5": "#dc2626", "p3": "#dc2626", "dim": "#6b7280",
     "text": "#374151", "card": "#f9fafb", "line": "#d1d5db"}

HX = 500            # the header's left edge
ROW0, STEP = 175, 26
XL, XR = HX + 22, HX + 58      # even (left) and odd (right) pin centres


def pin_xy(n):
    r = (n - 1) // 2
    return (XR if n % 2 else XL), ROW0 + r * STEP


class Svg:
    def __init__(self, w, h, title, sub):
        self.w, self.h = w, h
        self.body, self.top = [], []
        self.add(f'<rect width="{w}" height="{h}" fill="#ffffff"/>')
        self.add(f'<text x="{w/2}" y="40" text-anchor="middle" font-size="22" font-weight="bold" fill="#111">{title}</text>')
        self.add(f'<text x="{w/2}" y="64" text-anchor="middle" font-size="13" fill="#555">{sub}</text>')

    def add(self, s):
        self.body.append(s)

    def over(self, s):          # drawn last, on top of the wires
        self.top.append(s)

    def wire(self, pts, color=C["wire"], w=2.6, dash=None):
        d = "M" + " L".join(f"{x} {y}" for x, y in pts)
        da = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<path d="{d}" stroke="{color}" stroke-width="{w}" fill="none" stroke-linejoin="round"{da}/>')

    def dot(self, x, y, color=C["gnd"]):
        self.add(f'<circle cx="{x}" cy="{y}" r="4" fill="{color}"/>')

    def text(self, x, y, s, size=12, color=C["text"], anchor="start", bold=False, top=False):
        b = ' font-weight="bold"' if bold else ""
        t = (f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" text-anchor="{anchor}"{b} '
             f'stroke="#fff" stroke-width="4" stroke-linejoin="round" paint-order="stroke">{s}</text>')
        (self.over if top else self.add)(t)

    def resistor_h(self, x1, x2, y, label, fill="#fde68a", edge="#92400e", above=True):
        m = (x1 + x2) / 2
        self.wire([(x1, y), (m - 32, y)])
        self.wire([(m + 32, y), (x2, y)])
        self.add(f'<rect x="{m-32}" y="{y-10}" width="64" height="20" rx="3" fill="{fill}" stroke="{edge}" stroke-width="2"/>')
        self.text(m, y - 16 if above else y + 26, label, anchor="middle")

    def resistor_v(self, x, y1, y2, label, color=C["wire"]):
        m = (y1 + y2) / 2
        self.wire([(x, y1), (x, m - 26)], color)
        self.wire([(x, m + 26), (x, y2)], color)
        self.add(f'<rect x="{x-10}" y="{m-26}" width="20" height="52" rx="3" fill="#fde68a" stroke="#92400e" stroke-width="2"/>')
        self.text(x + 16, m + 4, label)

    def led_h(self, x, y, color, toward="left"):
        """A LED from x to x+40 (or x-40), anode first: current flows toward 'toward'."""
        if toward == "left":
            self.add(f'<path d="M{x} {y-14} L{x} {y+14} L{x-34} {y} Z" fill="{color}" stroke="#333" stroke-width="2"/>')
            self.add(f'<path d="M{x-36} {y-14} V{y+14}" stroke="#333" stroke-width="3"/>')
        else:
            self.add(f'<path d="M{x} {y-14} L{x} {y+14} L{x+34} {y} Z" fill="{color}" stroke="#333" stroke-width="2"/>')
            self.add(f'<path d="M{x+36} {y-14} V{y+14}" stroke="#333" stroke-width="3"/>')

    def gnd_sym(self, x, y):
        self.add(f'<path d="M{x} {y} V{y+10} M{x-12} {y+10} H{x+12} M{x-8} {y+15} H{x+8} M{x-4} {y+20} H{x+4}" '
                 f'stroke="{C["gnd"]}" stroke-width="2.4" fill="none"/>')

    def flag(self, x, y, label, color=C["p3"]):
        self.add(f'<path d="M{x} {y} V{y-12}" stroke="{color}" stroke-width="2.4"/>')
        self.add(f'<path d="M{x-14} {y-12} H{x+14}" stroke="{color}" stroke-width="3"/>')
        self.text(x, y - 18, label, color=color, anchor="middle", bold=True)

    def net(self, x, y, label, color=C["p3"], side="left"):
        """A net label: a tag at the end of a wire; tags with the same name are one wire."""
        w = 12 + 8 * len(label)
        bx = x - w if side == "left" else x
        self.add(f'<rect x="{bx}" y="{y-11}" width="{w}" height="22" rx="11" fill="#fff" stroke="{color}" stroke-width="2"/>')
        self.add(f'<text x="{bx+w/2}" y="{y+4}" text-anchor="middle" font-size="11" font-weight="bold" fill="{color}">{label}</text>')

    def plain(self, x, y, txt, size=11, color="#e5e7eb", anchor="start", bold=False):
        b = ' font-weight="bold"' if bold else ""
        self.over(f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" text-anchor="{anchor}"{b}>{txt}</text>')

    def box(self, x, y, w, h, fill, edge, title=None, sub=None, tc="#111"):
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{fill}" stroke="{edge}" stroke-width="2"/>')
        if title:
            self.add(f'<text x="{x+w/2}" y="{y+h/2-(6 if sub else -5)}" text-anchor="middle" font-size="14" font-weight="bold" fill="{tc}">{title}</text>')
        if sub:
            self.add(f'<text x="{x+w/2}" y="{y+h/2+14}" text-anchor="middle" font-size="11" fill="{tc}">{sub}</text>')

    def notes(self, x, y, w, title, lines):
        h = 36 + 20 * len(lines)
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{C["card"]}" stroke="{C["line"]}"/>')
        self.add(f'<text x="{x+18}" y="{y+24}" font-size="14" font-weight="bold" fill="#111">{title}</text>')
        for i, l in enumerate(lines):
            ind = 9 if l.startswith("  ") else 0         # a bullet's second line, under its text
            self.add(f'<text x="{x+18+ind}" y="{y+46+20*i}" font-size="12" fill="{C["text"]}">{l.strip()}</text>')

    def header(self, used):
        """used: {pin: colour} for the pins wired in this diagram."""
        h = 20 * STEP + 8
        self.add(f'<rect x="{HX}" y="{ROW0-17}" width="80" height="{h}" rx="6" fill="#1f2937"/>')
        self.add(f'<text x="{HX+40}" y="{ROW0-26}" text-anchor="middle" font-size="11" fill="{C["dim"]}">the header, as it faces you</text>')
        for n, (label, kind) in HEADER.items():
            x, y = pin_xy(n)
            if n in used:
                col = used[n]
                self.over(f'<circle cx="{x}" cy="{y}" r="8" fill="{col}" stroke="#fff" stroke-width="2"/>')
            else:
                col = {"p5": "#7f1d1d", "p3": "#7f1d1d", "gnd": "#030712", "no": "#4b5563"}.get(kind, "#9ca3af")
                self.over(f'<circle cx="{x}" cy="{y}" r="7" fill="{col}"/>')
            weight = n in used
            colour = used.get(n, C["dim"] if kind == "no" else C["text"])
            if colour in ("#111111", "#030712"):
                colour = "#111"
            if n % 2:
                self.text(x + 20, y + 4, f"{n}  {label}", size=11.5, color=colour, bold=weight, top=True)
            else:
                self.text(x - 20, y + 4, f"{label}  {n}", size=11.5, color=colour, anchor="end", bold=weight, top=True)
        self.over(f'<text x="{XR+2}" y="{ROW0-8}" font-size="10" fill="#f59e0b" text-anchor="middle">1</text>')

    def save(self, name):
        s = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.w} {self.h}" '
             f'font-family="Helvetica, Arial, sans-serif" font-size="14">\n' +
             "\n".join(self.body + self.top) + "\n</svg>\n")
        with open(os.path.join(OUT, name), "w") as f:
            f.write(s)
        print("wrote", name)


NOTES_Y = ROW0 + 20 * STEP + 20


def pwm():
    s = Svg(1200, 880, "PWM on the 40-pin header",
            "two LEDs at two frequencies, a servo at 50 Hz and an analog level: three LEDC timers and the sigma-delta")
    used = {5: "#111111", 26: "#111111", 28: "#f59e0b", 35: "#f59e0b", 37: "#8b5cf6", 39: "#f59e0b"}
    gx, rail = 60, 1150
    # LED 1, left: GPIO34 (pin 28) through its resistor to the GND of pin 26, one row up
    x26, y26 = pin_xy(26)
    x28, y28 = pin_xy(28)
    s.resistor_h(x28 - 8, 250, y28, "330 Ω", above=False)
    s.wire([(250, y28), (230, y28)])
    s.led_h(230, y28, "#ef4444")
    s.wire([(194, y28), (gx, y28)], C["gnd"])
    s.wire([(x26 - 8, y26), (gx, y26), (gx, y28)], C["gnd"])
    s.dot(gx, y28)
    s.text(212, y28 + 36, "LED 1, 1 kHz", anchor="middle")
    s.text(gx, y26 - 10, "GND (pin 26)", bold=True, color="#111")
    # the right side: pins 35, 37 and 39 turn up into three rows of their own, GND from pin 5 down a rail
    x5, y5 = pin_xy(5)
    x35, y35 = pin_xy(35)
    x37, y37 = pin_xy(37)
    x39, y39 = pin_xy(39)
    ya, yb, yc = 300, 400, 500
    s.wire([(x5 + 8, y5), (rail, y5), (rail, yc + 60)], C["gnd"])
    s.text(rail - 10, y5 - 8, "GND (pin 5)", anchor="end", bold=True, color="#111")
    # LED 2 on GPIO46
    s.wire([(x35 + 8, y35), (720, y35), (720, ya)])
    s.resistor_h(720, 880, ya, "330 Ω")
    s.wire([(880, ya), (920, ya)])
    s.led_h(920, ya, "#22c55e", toward="right")
    s.wire([(956, ya), (rail, ya)], C["gnd"])
    s.dot(rail, ya)
    s.text(938, ya - 24, "LED 2, 5 kHz", anchor="middle")
    # the analog level on GPIO47: 1 kΩ, a node, 1 µF down to GND
    s.wire([(x37 + 8, y37), (745, y37), (745, yb)], "#7c3aed")
    s.resistor_h(745, 905, yb, "1 kΩ", fill="#ddd6fe", edge="#5b21b6")
    s.wire([(905, yb), (980, yb)], "#7c3aed")
    s.dot(980, yb, "#7c3aed")
    s.wire([(980, yb), (980, yb + 8)], "#7c3aed")
    s.add(f'<path d="M966 {yb+9} H994 M966 {yb+17} H994" stroke="#5b21b6" stroke-width="3"/>')
    s.wire([(980, yb + 18), (980, yb + 40), (rail, yb + 40)], C["gnd"])
    s.dot(rail, yb + 40)
    s.text(1000, yb + 18, "1 µF")
    s.wire([(980, yb), (1000, yb - 30)], "#7c3aed", 1.5, "4 3")
    s.box(890, yb - 60, 230, 30, "#f5f3ff", "#7c3aed")
    s.text(1005, yb - 40, "meter here to GND: 0-3.3 V", color="#5b21b6", anchor="middle")
    s.text(965, yb + 32, "analog level (sigma-delta, no timer)", color="#7c3aed", anchor="end")
    # the servo on GPIO48, with its own 5 V supply beside it
    s.wire([(x39 + 8, y39), (770, y39), (770, yc), (880, yc)], "#f97316", 3)
    s.box(880, yc - 32, 120, 64, "#2563eb", "#1e3a8a", "Servo", "SG90, MG90...", "#fff")
    s.box(1030, yc - 32, 100, 64, "#fee2e2", "#dc2626", "its own 5 V", "supply", "#991b1b")
    s.wire([(1000, yc - 10), (1030, yc - 10)], "#dc2626", 3)
    s.text(1015, yc - 18, "+", color="#dc2626", anchor="middle", bold=True)
    s.wire([(940, yc + 32), (940, yc + 60), (rail, yc + 60)], "#78350f", 3)
    s.wire([(1080, yc + 32), (1080, yc + 60)], "#78350f", 3)
    s.dot(1080, yc + 60, "#78350f")
    s.dot(rail, yc + 60)
    s.text(782, yc - 10, "signal (orange)")
    s.text(782, yc + 20, "PWM 50 Hz")
    s.text(rail - 10, yc + 82, "GND (brown), shared by the servo and its supply", anchor="end")
    s.notes(40, NOTES_Y, 1120, "What it tries (on the pins the pin map leaves free; it worked on the board on 2026-10-05, then on GPIO28, 29, 30 and 32)", [
        "• LED 1 (GPIO34, pin 28) at 1 kHz and LED 2 (GPIO46, pin 35) at 5 kHz: two LEDC timers. LEDs: the long leg (anode) toward the resistor",
        "• The servo (GPIO48, pin 39) at 50 Hz, 500-2500 µs: the third timer, the last one free (LEDC timer 1 is the screen's backlight)",
        "• The analog level (GPIO47, pin 37): sigma-delta through 1 kΩ and 1 µF, no timer; 1.34 V asked, 1.313 V measured",
        "• GPIO46 and GPIO47 are where the CAN app starts: one or the other. More outputs than pins: a PCA9685 on the I2C port",
        "• Without its own supply, a small servo can take 5 V from pin 1 or 3, with 470 µF across it near the servo",
    ])
    s.header(used)
    s.save("bench-pwm.svg")


def ir():
    s = Svg(1100, 860, "Infrared on the 40-pin header",
            "a demodulating receiver (VS1838B, TSOP38238) and an IR LED driven by an NPN transistor")
    used = {1: "#dc2626", 10: "#111111", 16: "#f59e0b", 18: "#dc2626", 31: "#f59e0b", 33: "#111111"}
    x1, y1 = pin_xy(1)
    x10, y10 = pin_xy(10)
    x16, y16 = pin_xy(16)
    x18, y18 = pin_xy(18)
    x31, y31 = pin_xy(31)
    x33, y33 = pin_xy(33)
    # the receiver, left: OUT straight from pin 16; its GND and VCC by tags (pin 18 is the row its GND is on)
    rx, ry = 130, y16 - 26
    re = rx + 150                           # the box's right edge, where its three legs come out
    gy, vy = y16 + STEP, y16 + 2 * STEP
    s.box(rx, ry, 150, 110, "#111827", "#111827")
    s.add(f'<circle cx="{rx+60}" cy="{ry+50}" r="24" fill="#374151"/>')
    s.plain(rx + 60, ry + 98, "receiver", anchor="middle")
    s.wire([(x16 - 8, y16), (re, y16)])
    s.wire([(re, gy), (re + 40, gy)], C["gnd"])
    s.net(re + 40, gy, "GND", C["gnd"], side="right")
    s.wire([(re, vy), (re + 40, vy)], C["p3"])
    s.net(re + 40, vy, "3V3", side="right")
    s.wire([(x18 - 8, y18), (440, y18)], C["p3"])
    s.net(440, y18, "3V3")
    s.wire([(x10 - 8, y10), (440, y10)], C["gnd"])
    s.net(440, y10, "GND", C["gnd"])
    for (y, t) in ((y16, "OUT"), (gy, "GND"), (vy, "VCC")):
        s.plain(re - 8, y + 4, t, anchor="end", bold=True)
    s.text(rx, ry + 136, "VS1838B from the front: OUT, GND, VCC")
    s.text(rx, ry + 154, "(other models differ: check yours)")
    s.text(rx, ry + 172, "VCC to 3.3 V (pin 18), never 5 V", color="#b91c1c")
    # the sender, right: pin 31 -> 1 kΩ -> base; the emitter to the GND of pin 33, one row down
    s.resistor_h(x31 + 8, 790, y31, "1 kΩ")
    bx = 840                                # the base bar
    s.wire([(790, y31), (bx, y31)])
    s.add(f'<circle cx="{bx+18}" cy="{y31}" r="32" fill="#fff" stroke="#111" stroke-width="2"/>')
    s.add(f'<path d="M{bx} {y31-18} V{y31+18}" stroke="#111" stroke-width="3"/>')
    cx_ = bx + 34                           # collector and emitter leave on this x
    s.add(f'<path d="M{bx} {y31-8} L{cx_} {y31-26} V{y31-70}" stroke="#111" stroke-width="2.4" fill="none"/>')
    s.add(f'<path d="M{bx} {y31+8} L{cx_} {y31+26} V{y33}" stroke="#111" stroke-width="2.4" fill="none"/>')
    s.add(f'<path d="M{cx_-4} {y31+18} L{cx_+2} {y31+28} L{cx_-10} {y31+28} Z" fill="#111"/>')
    s.text(bx - 6, y31 - 12, "B", size=11, anchor="end")
    s.text(cx_ + 8, y31 - 40, "C", size=11)
    s.text(cx_ + 8, y31 + 46, "E", size=11)
    s.text(cx_ + 34, y31 + 6, "NPN 2N2222", size=11)
    s.text(cx_ + 34, y31 + 22, "or BC547", size=11)
    s.wire([(x33 + 8, y33), (cx_, y33)], C["gnd"])
    s.dot(cx_, y33)
    s.text(cx_ - 10, y33 + 20, "emitter to GND (pin 33)", anchor="end")
    # the IR LED and its 47 Ω up to 5 V, straight across from pin 1
    ly = y31 - 150
    s.wire([(cx_, y31 - 70), (cx_, ly + 32)], C["gnd"])
    s.add(f'<path d="M{cx_-14} {ly} L{cx_+14} {ly} L{cx_} {ly+30} Z" fill="#7c3aed" stroke="#4c1d95" stroke-width="2"/>')
    s.add(f'<path d="M{cx_-14} {ly+32} H{cx_+14}" stroke="#4c1d95" stroke-width="3"/>')
    s.add(f'<path d="M{cx_+20} {ly+8} l14 -10 M{cx_+22} {ly+20} l14 -10" stroke="#7c3aed" stroke-width="2"/>')
    s.text(cx_ + 42, ly + 14, "IR LED (940 nm)")
    s.text(cx_ + 42, ly + 30, "long leg up, toward 47 Ω")
    s.resistor_v(cx_, ly - 90, ly, "47 Ω", C["p5"])
    s.wire([(x1 + 8, y1), (cx_, y1), (cx_, ly - 90)], C["p5"])
    s.text(cx_ + 10, y1 - 8, "5 V from pin 1 (or 3)", color="#b91c1c")
    s.notes(40, NOTES_Y + 20, 1020, "What it tries (the Infrarrojo app's own pins; it worked on the board on 2026-10-05, then on GPIO5 and GPIO31)", [
        "• Learn: the receiver on GPIO28 (pin 16). On the board it read a monitor's remote: Samsung, address 0x0707, command 7 (volume up)",
        "• Send: the LED on GPIO32 (pin 31) at 38 kHz. On the board it turned the monitor's volume up, from the learned code and from SmartIR's",
        "• A header pin gives about 20 mA; through the transistor the LED takes ~70 mA from 5 V and reaches across a room",
        "• Not GPIO21/22 for the receiver: they are the I2C bus the system scans by itself. Tags with the same name (3V3, GND) are one wire",
    ])
    s.header(used)
    s.save("bench-ir.svg")


def strip():
    s = Svg(1100, 870, "A WS2812B strip on the 40-pin header",
            "12 LEDs fed from the header's 5 V, the data straight from a 3.3 V pin through a resistor")
    used = {1: "#dc2626", 40: "#111111", 39: "#f59e0b"}
    x1, y1 = pin_xy(1)
    x39, y39 = pin_xy(39)
    x40, y40 = pin_xy(40)
    v5, vd, vg = 290, 318, 385            # the three wires, left of the pin labels
    under = y40 + 33                      # the data runs under the header to the left
    # the strip lies on the left, its input at the right end: 5V, GND, DIN pads
    sy = 501
    end = 270
    s.add(f'<rect x="30" y="{sy-34}" width="{end-30}" height="110" rx="8" fill="#111827"/>')
    for i in range(4):
        lx = end - 60 - i * 52
        col = ["#ef4444", "#f59e0b", "#22c55e", "#3b82f6"][i]
        s.add(f'<rect x="{lx-17}" y="{sy+2}" width="34" height="34" rx="4" fill="#f9fafb"/>')
        s.add(f'<circle cx="{lx}" cy="{sy+19}" r="10" fill="{col}"/>')
    s.add(f'<path d="M{end-24} {sy+60} l-14 -8 l0 16 Z" fill="#f59e0b"/>')
    s.plain(44, sy + 64, "...12 LEDs; arrows away from DIN", size=11)
    pads = {"5V": sy - 22, "GND": sy - 2, "DIN": sy + 18}
    for name, y in pads.items():
        s.add(f'<rect x="{end-6}" y="{y-7}" width="18" height="14" rx="2" fill="#d1a33a"/>')
        s.plain(end - 12, y + 4, name, size=11, anchor="end", bold=True)
    s.wire([(x39 + 8, y39), (716, y39), (716, under), (vd, under)])
    s.resistor_v(vd, pads["DIN"], under, "330 Ω")
    s.wire([(vd, pads["DIN"]), (end + 12, pads["DIN"])])
    s.wire([(x40 - 8, y40), (vg, y40), (vg, pads["GND"]), (end + 12, pads["GND"])], C["gnd"])
    s.wire([(x1 + 8, y1), (620, y1), (620, ROW0 - 60), (v5, ROW0 - 60), (v5, pads["5V"]), (end + 12, pads["5V"])], C["p5"])
    s.text(v5 + 6, ROW0 - 66, "5 V from pin 1 (or 3)", color="#b91c1c")
    s.text(vg + 8, y40 + 19, "GND (pin 40)")
    s.text(560, under + 18, "DIN: GPIO48 (pin 39), round under the header", color="#b45309", bold=True)
    # the capacitor across the input
    cxp = 200
    s.add(f'<path d="M{cxp} {sy-92} V{sy-78} M{cxp-14} {sy-78} H{cxp+14} M{cxp-14} {sy-70} H{cxp+14} M{cxp} {sy-70} V{sy-56}" stroke="#5b21b6" stroke-width="2.6" fill="none"/>')
    s.text(cxp - 22, sy - 76, "470-1000 µF", anchor="end")
    s.text(cxp - 22, sy - 60, "5V to GND", anchor="end")
    s.text(cxp - 22, sy - 44, "at the strip", anchor="end")
    s.notes(40, NOTES_Y + 30, 1020, "What it tries (the pin the pin map suggests for a strip; it worked on the board on 2026-10-05, then on GPIO4)", [
        "• The data on GPIO48 (pin 39) through 330 Ω near the strip; 5 V from pin 1 (or 3), GND from pin 40, the pin beside it",
        "• In LED Strips: GPIO48, WS2812B, GRB, 12 LEDs, a 500 mA budget (12 LEDs at full white would take ~720 mA)",
        "• On the board: colours in the right order, effects at 50 fps, and back by itself after a restart",
        "• 3.3 V data was enough with short wires; if LEDs flicker at random, a level shifter (74AHCT125)",
    ])
    s.header(used)
    s.save("bench-ws2812.svg")


def eeprom24():
    s = Svg(1100, 860, "A 24LC EEPROM on the 40-pin header (I2C)",
            "the header's external I2C bus, i2c.ext: SDA on GPIO21, SCL on GPIO22, the chip at address 0x50")
    used = {15: "#f59e0b", 17: "#f59e0b", 18: "#dc2626", 19: "#111111"}
    x15, y15 = pin_xy(15)
    x17, y17 = pin_xy(17)
    x18, y18 = pin_xy(18)
    x19, y19 = pin_xy(19)
    # power from the header as net tags: every tag with the same name is one wire
    s.wire([(x18 - 8, y18), (420, y18)], C["p3"])
    s.net(420, y18, "3V3")
    s.wire([(x19 + 8, y19), (680, y19)], C["gnd"])
    s.net(680, y19, "GND", C["gnd"], side="right")
    # the chip, turned so pins 5-8 face the header (notch at the bottom)
    cw, ch = 190, 176
    cx, cy = 840, y15 - 70
    s.add(f'<rect x="{cx}" y="{cy}" width="{cw}" height="{ch}" rx="6" fill="#1f2937"/>')
    s.add(f'<path d="M{cx+cw/2-16} {cy+ch} a16 16 0 0 1 32 0" fill="#fff"/>')
    s.add(f'<circle cx="{cx+cw-16}" cy="{cy+ch-18}" r="5" fill="#9ca3af"/>')
    s.plain(cx + cw / 2, cy + ch / 2 - 4, "24LC256", size=15, color="#fff", anchor="middle", bold=True)
    s.plain(cx + cw / 2, cy + ch / 2 + 14, "top view, turned", size=10, color="#9ca3af", anchor="middle")
    py = [cy + 28 + i * 40 for i in range(4)]
    for (n, nm), y in zip([(5, "SDA"), (6, "SCL"), (7, "WP"), (8, "VCC")], py):
        s.add(f'<path d="M{cx-16} {y} H{cx}" stroke="#9ca3af" stroke-width="6"/>')
        s.plain(cx + 8, y + 4, f"{n} {nm}", bold=True)
    for (n, nm), y in zip([(4, "VSS"), (3, "A2"), (2, "A1"), (1, "A0")], py):
        s.add(f'<path d="M{cx+cw} {y} H{cx+cw+16}" stroke="#9ca3af" stroke-width="6"/>')
        s.plain(cx + cw - 8, y + 4, f"{nm} {n}", anchor="end", bold=True)
    s.text(cx + cw / 2, cy + ch + 30, "notch and pin-1 dot at the bottom here", anchor="middle", color=C["dim"])
    sda_y, scl_y, wp_y, vcc_y = py
    # SDA and SCL, with a 4.7 kΩ pull-up each to 3V3
    s.wire([(x15 + 8, y15), (700, y15), (700, sda_y), (cx - 16, sda_y)])
    s.wire([(x17 + 8, y17), (760, y17), (760, scl_y), (cx - 16, scl_y)])
    s.dot(700, sda_y, C["wire"])
    s.dot(760, scl_y, C["wire"])
    s.resistor_v(700, sda_y - 150, sda_y, "", C["wire"])
    s.resistor_v(760, sda_y - 90, scl_y, "", C["wire"])
    s.text(716, sda_y - 71, "4.7 kΩ")
    s.text(776, sda_y - 21, "4.7 kΩ")
    s.net(712, sda_y - 150, "3V3")
    s.net(748, sda_y - 90, "3V3", side="right")
    # WP to GND (writable), VCC to 3V3
    s.wire([(cx - 16, wp_y), (800, wp_y)], C["gnd"])
    s.net(800, wp_y, "GND", C["gnd"])
    s.wire([(cx - 16, vcc_y), (800, vcc_y)], C["p3"])
    s.net(800, vcc_y, "3V3")
    # A0-A2 and VSS to GND
    gx = cx + cw + 40
    for y in py:
        s.wire([(cx + cw + 16, y), (gx, y)], C["gnd"])
        s.dot(gx, y)
    s.wire([(gx, py[0]), (gx, py[-1] + 30)], C["gnd"])
    s.net(gx - 22, py[-1] + 42, "GND", C["gnd"], side="right")
    s.notes(40, NOTES_Y, 1020, "Wiring a 24LC (24LC256 and its family; the EEPROM app draws each chip)", [
        "• SDA (chip pin 5) to GPIO21, header pin 15; SCL (chip pin 6) to GPIO22, header pin 17; a 4.7 kΩ pull-up from each to 3V3",
        "• VCC (8) to 3V3 (header pin 18 or 2), VSS (4) to GND (pin 19); A0-A2 (1-3) to GND: address 0x50",
        "• WP (7) to GND to write; to 3V3 it is read only. Never 5 V on the header",
        "• Tags with the same name (3V3, GND) are the same wire. In the EEPROM app: I2C, port i2c.ext, address 0x50, Detect",
    ])
    s.header(used)
    s.save("bench-24lc.svg")


DEVKIT_LEFT = ["3V3", "EN", "VP", "VN", "IO34", "IO35", "IO32", "IO33", "IO25", "IO26", "IO27", "IO14",
               "IO12", "GND", "IO13", "SD2", "SD3", "CMD", "5V"]
DEVKIT_RIGHT = ["GND", "IO23", "IO22", "TX0", "RX0", "IO21", "GND", "IO19", "IO18", "IO5", "IO17", "IO16",
                "IO4", "IO0", "IO2", "IO15", "SD1", "SD0", "CLK"]


def devkit(s, DX, DY0, hot, DW=200):
    """An ESP32 DevKit of 38 pins from the top: the module (antenna up), 19 pins down each side, USB
    at the bottom. hot: {("L" or "R", index): colour} for the pins wired. Returns the left and right
    pin columns' x, the pins' y as a function of the index, and the board's top and bottom."""
    left, right = DEVKIT_LEFT, DEVKIT_RIGHT
    top, bot = DY0 - 66, DY0 + 18 * STEP + 70
    lx, rx_ = DX + 14, DX + DW - 14
    py = lambda i: DY0 + i * STEP                       # noqa: E731
    s.add(f'<rect x="{DX}" y="{top}" width="{DW}" height="{bot-top}" rx="8" fill="#1e293b"/>')
    s.add(f'<rect x="{DX+40}" y="{top+8}" width="{DW-80}" height="44" rx="3" fill="#cbd5e1" stroke="#94a3b8"/>')
    s.add(f'<path d="M{DX+52} {top+20} h20 v12 h16 v-12 h16 v12 h16 v-12 h20" stroke="#64748b" stroke-width="2" fill="none"/>')
    s.plain(DX + DW / 2, top + 47, "ESP32-WROOM-32", size=10, color="#334155", anchor="middle", bold=True)
    s.add(f'<rect x="{DX+DW/2-30}" y="{py(5)}" width="60" height="150" rx="4" fill="#0f172a"/>')
    s.plain(DX + DW / 2, py(8) + 4, "ESP32", size=13, color="#e2e8f0", anchor="middle", bold=True)
    s.plain(DX + DW / 2, py(9) + 4, "DevKit", size=11, color="#94a3b8", anchor="middle")
    s.plain(DX + DW / 2, py(10) + 4, "top view", size=10, color="#94a3b8", anchor="middle")
    s.plain(DX + DW / 2, py(16) + 4, "flash pins", size=10, color="#fca5a5", anchor="middle", bold=True)
    s.plain(DX + DW / 2, py(17) + 4, "leave free", size=10, color="#fca5a5", anchor="middle")
    # the micro-USB and the two buttons at the bottom
    s.add(f'<rect x="{DX+DW/2-22}" y="{bot-22}" width="44" height="30" rx="3" fill="#9ca3af" stroke="#6b7280"/>')
    s.plain(DX + DW / 2, bot + 24, "micro-USB", size=11, color="#6b7280", anchor="middle")
    for bx_, name in ((DX + 34, "EN"), (DX + DW - 34, "BOOT")):
        s.add(f'<rect x="{bx_-12}" y="{bot-30}" width="24" height="18" rx="3" fill="#e5e7eb" stroke="#6b7280"/>')
        s.plain(bx_, bot - 36, name, size=9, color="#e2e8f0", anchor="middle", bold=True)
    flash = {"CMD", "SD0", "SD1", "SD2", "SD3", "CLK"}      # the SPI flash's own pins: nothing goes there
    for side, names, x, anchor, dx in (("L", left, lx, "start", 14), ("R", right, rx_, "end", -14)):
        for i, nm in enumerate(names):
            h = hot.get((side, i))
            fill = "#e5e7eb" if h == "#111111" else h       # a black (GND) pin keeps a light name
            s.over(f'<circle cx="{x}" cy="{py(i)}" r="{7 if h else 6}" fill="{h or "#d1a33a"}" stroke="#fff" stroke-width="{2 if h else 0}"/>')
            s.plain(x + dx, py(i) + 4, nm, size=11, color=fill or ("#fca5a5" if nm in flash else "#e5e7eb"),
                    anchor=anchor, bold=bool(h))
    return lx, rx_, py, top, bot


def esp32_devkit():
    """An ESP32 DevKit of 38 pins (DevKitC V4 and its many copies) on uart.b with EN and BOOT."""
    s = Svg(1100, 965, "An ESP32 DevKit (38 pins) on the 40-pin header",
            "uart.b with its EN and BOOT lines: the Flasher puts it in download mode by itself, "
            "and esptool does the same through the bridge")
    c_tx, c_rx, c_boot, c_en = "#ea580c", "#16a34a", "#7c3aed", "#0891b2"
    used = {1: "#dc2626", 8: c_boot, 11: c_en, 12: c_tx, 13: "#111111", 14: c_rx}
    right = DEVKIT_RIGHT
    lx, rx_, py, top, bot = devkit(s, 130, ROW0 + STEP, {
        ("L", 1): c_en, ("L", 18): "#dc2626", ("R", 0): "#111111",
        ("R", 3): c_rx, ("R", 4): c_tx, ("R", 13): c_boot})
    # TX of uart.b (pin 12) to RX0, straight across; TX0 to RX of uart.b (pin 14): crossed
    x12, y12 = pin_xy(12)
    x14, y14 = pin_xy(14)
    x8, y8 = pin_xy(8)
    i_rx0, i_tx0, i_io0 = right.index("RX0"), right.index("TX0"), right.index("IO0")
    lane_rx, lane_boot = 375, 405
    s.wire([(x12 - 8, y12), (lane_rx - 10, y12), (lane_rx - 10, py(i_rx0)), (rx_ + 8, py(i_rx0))], c_tx, 3)
    s.wire([(x14 - 8, y14), (lane_rx + 10, y14), (lane_rx + 10, py(i_tx0)), (rx_ + 8, py(i_tx0))], c_rx, 3)
    s.wire([(x8 - 8, y8), (lane_boot, y8), (lane_boot, py(i_io0)), (rx_ + 8, py(i_io0))], c_boot, 3)
    # EN and GND by tags: pin 11 and pin 13 are on the header's right column
    x11, y11 = pin_xy(11)
    x13, y13 = pin_xy(13)
    s.wire([(x11 + 8, y11), (690, y11)], c_en, 3)
    s.net(690, y11, "EN", c_en, side="right")
    s.wire([(x13 + 8, y13), (690, y13)], C["gnd"], 3)
    s.net(690, y13, "GND", C["gnd"], side="right")
    s.wire([(lx - 8, py(1)), (100, py(1))], c_en, 3)
    s.net(100, py(1), "EN", c_en)
    s.wire([(rx_ + 8, py(0)), (352, py(0))], C["gnd"], 3)
    s.net(352, py(0), "GND", C["gnd"], side="right")
    # the legend: the six wires, header pin first
    gx, gy = 730, pin_xy(23)[1] - 14
    rows = [(c_tx, "pin 12, GPIO3 (uart.b TX)", "RX0"), (c_rx, "pin 14, GPIO4 (uart.b RX)", "TX0"),
            (c_boot, "pin 8, GPIO2 (the BOOT line)", "IO0"), (c_en, "pin 11, GPIO5 (the EN line)", "EN"),
            (C["gnd"], "pin 13, GND", "GND"), (C["p5"], "pin 1, 5 V", "5V")]
    s.add(f'<rect x="{gx}" y="{gy}" width="330" height="{44 + 26 * len(rows)}" rx="8" fill="{C["card"]}" stroke="{C["line"]}"/>')
    s.text(gx + 16, gy + 24, "The six wires: header → DevKit", size=13, color="#111", bold=True)
    for k, (col, frm, to) in enumerate(rows):
        y = gy + 50 + 26 * k
        s.wire([(gx + 16, y - 4), (gx + 46, y - 4)], col, 4)
        s.text(gx + 56, y, frm, size=12)
        s.text(gx + 316, y, to, size=12, color=col if col != C["gnd"] else "#111", anchor="end", bold=True)
    # 5 V from pin 1 over the top and down the left to the DevKit's 5V pin
    x1, y1 = pin_xy(1)
    v5 = 40
    s.wire([(x1 + 8, y1), (640, y1), (640, top - 24), (v5, top - 24), (v5, py(18)), (lx - 8, py(18))], C["p5"], 3)
    s.text(v5 + 10, top - 30, "5 V from pin 1 to its 5V pin (or its own micro-USB, not both)", color="#b91c1c")
    s.notes(40, bot + 46, 1020, "What it tries (all of it worked on the board, 2026-11-05)", [
        "• uart.b crossed: TX (GPIO3, pin 12) to the DevKit's RX0, its TX0 to RX (GPIO4, pin 14); GND (pin 13) to one of its GND pins",
        "• The EN line (GPIO5, pin 11) to EN, the BOOT line (GPIO2, pin 8) to IO0: Detect resets it into the ROM by itself. Open-drain, so its buttons still work",
        "• Written by the Flasher at 115200, 460800 and 921600 (89 KB/s), MD5 fine; the Terminal read it and talked back to it",
        "• From the Mac through the bridge: esptool --port rfc2217://p4os.local:4000 chip_id, and write_flash at 460800, hashes verified",
        "• Power: 5 V from pin 1 to its 5V pin, or its own micro-USB; never both. CMD, SD0-SD3 and CLK are the flash's: a wire there (GND on CMD)",
        "  leaves the flash silent, ID ff/ffff and \"invalid header: 0xffffffff\" in a loop. Printed names vary: TX0/TXD/TX, RX0/RXD/RX, IO0/G0",
    ])
    s.header(used)
    s.save("bench-esp32.svg")


def nano_header():
    """An Arduino Nano (5 V) on uart.b: its TX through a divider, the EN line to its reset."""
    s = Svg(1100, 895, "An Arduino Nano on the 40-pin header",
            "uart.b with the EN line on its reset: the Flasher writes it through its bootloader, "
            "and avrdude does the same through the bridge")
    c_tx, c_rx, c_en = "#ea580c", "#16a34a", "#0891b2"
    used = {1: "#dc2626", 11: c_en, 12: c_tx, 13: "#111111", 14: c_rx}
    # the Nano from the top, USB up: D12..D2, GND, RST, RX0, TX1 down the side facing the header
    DX, DW, DY0 = 60, 170, ROW0 + 2 * STEP
    left = ["D13", "3V3", "REF", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "5V", "RST", "GND", "VIN"]
    right = ["D12", "D11", "D10", "D9", "D8", "D7", "D6", "D5", "D4", "D3", "D2", "GND", "RST", "RX0", "TX1"]
    top, bot = DY0 - 64, DY0 + 14 * STEP + 34
    lx, rx_ = DX + 14, DX + DW - 14
    py = lambda i: DY0 + i * STEP                       # noqa: E731
    s.add(f'<rect x="{DX}" y="{top}" width="{DW}" height="{bot-top}" rx="6" fill="#1e3a8a"/>')
    s.add(f'<rect x="{DX+DW/2-26}" y="{top-14}" width="52" height="40" rx="3" fill="#9ca3af" stroke="#6b7280"/>')
    s.plain(DX + DW / 2, top - 22, "USB", size=11, color="#6b7280", anchor="middle")
    s.add(f'<rect x="{DX+DW/2-34}" y="{py(5)}" width="68" height="68" rx="3" fill="#0f172a" transform="rotate(45 {DX+DW/2} {py(5)+34})"/>')
    s.plain(DX + DW / 2, py(6) + 10, "ATmega", size=11, color="#e2e8f0", anchor="middle", bold=True)
    s.plain(DX + DW / 2, py(7) + 4, "328P", size=11, color="#e2e8f0", anchor="middle", bold=True)
    s.plain(DX + DW / 2, py(10) + 4, "Nano", size=13, color="#e2e8f0", anchor="middle", bold=True)
    s.plain(DX + DW / 2, py(11) + 4, "top view", size=10, color="#93c5fd", anchor="middle")
    s.add(f'<rect x="{DX+DW/2-12}" y="{py(12)-6}" width="24" height="20" rx="3" fill="#e5e7eb" stroke="#6b7280"/>')
    s.plain(DX + DW / 2, py(13) + 14, "RESET", size=9, color="#e2e8f0", anchor="middle", bold=True)
    hot_r = {"RX0": c_tx, "TX1": c_rx, "RST": c_en, "GND": "#111111"}
    for i, nm in enumerate(left):
        hot = "#dc2626" if nm == "5V" else None
        s.over(f'<circle cx="{lx}" cy="{py(i)}" r="{7 if hot else 6}" fill="{hot or "#d1a33a"}" stroke="#fff" stroke-width="{2 if hot else 0}"/>')
        s.plain(lx + 14, py(i) + 4, nm, size=11, color=hot or "#e5e7eb", bold=bool(hot))
    for i, nm in enumerate(right):
        hot = hot_r.get(nm)
        s.over(f'<circle cx="{rx_}" cy="{py(i)}" r="{7 if hot else 6}" fill="{hot or "#d1a33a"}" stroke="#fff" stroke-width="{2 if hot else 0}"/>')
        txt = "#e5e7eb" if hot in (None, "#111111") else hot
        s.plain(rx_ - 14, py(i) + 4, nm, size=11, color=txt, anchor="end", bold=bool(hot))
    i_gnd, i_rst, i_rx0, i_tx1 = 11, 12, 13, 14
    x12, y12 = pin_xy(12)
    x14, y14 = pin_xy(14)
    lane_tx, node = 305, 390
    # TX of uart.b (pin 12) straight to RX0: 3.3 V is a high for a 5 V ATmega
    s.wire([(x12 - 8, y12), (lane_tx, y12), (lane_tx, py(i_rx0)), (rx_ + 8, py(i_rx0))], c_tx, 3)
    # TX1 is 5 V: 1 kOhm to the node, 2 kOhm from the node down to GND, the node (3.3 V) up to RX (pin 14)
    ty = py(i_tx1)
    s.resistor_h(rx_ + 8, node, ty, "1 kΩ", above=False)
    s.wire([(node, ty), (node, y14), (x14 - 8, y14)], c_rx, 3)
    s.dot(node, ty, c_rx)
    s.resistor_v(node, ty, ty + 96, "", C["gnd"])
    s.text(node - 16, ty + 46, "2 kΩ", anchor="end")
    s.text(node - 16, ty + 62, "(or 2 × 1 kΩ)", size=11, color=C["dim"], anchor="end")
    s.gnd_sym(node, ty + 96)
    s.text(node - 8, ty - 10, "3.3 V", size=11, color=c_rx, anchor="end", bold=True)
    # RST and GND by tags, as on the header's right column
    s.wire([(rx_ + 8, py(i_rst)), (250, py(i_rst))], c_en, 3)
    s.net(250, py(i_rst), "EN", c_en, side="right")
    s.wire([(rx_ + 8, py(i_gnd)), (250, py(i_gnd))], C["gnd"], 3)
    s.net(250, py(i_gnd), "GND", C["gnd"], side="right")
    x11, y11 = pin_xy(11)
    x13, y13 = pin_xy(13)
    s.wire([(x11 + 8, y11), (690, y11)], c_en, 3)
    s.net(690, y11, "EN", c_en, side="right")
    s.wire([(x13 + 8, y13), (690, y13)], C["gnd"], 3)
    s.net(690, y13, "GND", C["gnd"], side="right")
    # 5 V from pin 1 over the top and down the left to its 5V pin
    x1, y1 = pin_xy(1)
    v5 = 30
    s.wire([(x1 + 8, y1), (640, y1), (640, top - 50), (v5, top - 50), (v5, py(11)), (lx - 8, py(11))], C["p5"], 3)
    s.text(v5 + 10, top - 56, "5 V from pin 1 to its 5V pin (or its own USB, not both)", color="#b91c1c")
    # the legend
    gx, gy = 730, pin_xy(23)[1] - 14
    rows = [(c_tx, "pin 12, GPIO3 (uart.b TX)", "RX0 (D0)"), (c_rx, "pin 14, GPIO4 (uart.b RX)", "TX1 (D1) via 1k/2k"),
            (c_en, "pin 11, GPIO5 (the EN line)", "RST"), (C["gnd"], "pin 13, GND", "GND"), (C["p5"], "pin 1, 5 V", "5V")]
    s.add(f'<rect x="{gx}" y="{gy}" width="330" height="{44 + 26 * len(rows)}" rx="8" fill="{C["card"]}" stroke="{C["line"]}"/>')
    s.text(gx + 16, gy + 24, "The five wires: header → Nano", size=13, color="#111", bold=True)
    for k, (col, frm, to) in enumerate(rows):
        y = gy + 50 + 26 * k
        s.wire([(gx + 16, y - 4), (gx + 46, y - 4)], col, 4)
        s.text(gx + 56, y, frm, size=12)
        s.text(gx + 316, y, to, size=12, color=col if col != C["gnd"] else "#111", anchor="end", bold=True)
    s.notes(40, NOTES_Y, 1020, "What it tries (all of it worked on the board, 2026-11-05)", [
        "• TX (GPIO3, pin 12) straight to RX0: 3.3 V is a high for a 5 V ATmega. Its TX1 is 5 V: to RX (GPIO4, pin 14) only through",
        "  1 kΩ in series with 2 kΩ to GND (or two 1 kΩ in series: 3.33 V at the node); never straight. A 3.3 V Pro Mini needs none",
        "• The EN line (GPIO5, pin 11) to RST: Detect found its old bootloader (STK500 v1 1.16, 57600) and the Flasher wrote, read and verified it",
        "  If Detect gets no answer while its program keeps running (a counter that never restarts), the EN wire is not on RST",
        "• Bridge: avrdude -c arduino -p m328p -P net:p4os.local:4001 -b 57600 wrote and verified it; each connection resets it through EN",
        "• Power: 5 V from pin 1 to its 5V pin, or its own USB; never both. Tags with the same name (EN, GND) are one wire",
    ])
    s.header(used)
    s.save("bench-nano.svg")


def sn65(s, x, w, ys, side, title, chip_y, smd_y):
    """An SN65HVD230 board (the blue GY-230 and its copies) with its six pins down one edge, 'left'
    or 'right', at the y of ys (3V3, GND, CTX, CRX, CANH, CANL); returns where a wire meets each
    pin. The chip is marked VP230; the SMD marked 121 is its 120 ohm across CANH and CANL."""
    top, bot = min(ys[0], chip_y) - 28, max(ys[-1], smd_y) + 32
    s.add(f'<rect x="{x}" y="{top}" width="{w}" height="{bot-top}" rx="6" fill="#1e3a8a"/>')
    s.text(x + w / 2, top - 12, title, size=13, color="#111", anchor="middle", bold=True)
    px = x + w - 14 if side == "right" else x + 14
    inner = x + 14 if side == "right" else x + w - 68        # the chip and the SMD, away from the pins
    # the chip, legs up and down as on the board, pin 1 dot at the bottom left
    s.add(f'<rect x="{inner}" y="{chip_y}" width="54" height="40" rx="3" fill="#0f172a"/>')
    for k in range(4):
        lx = inner + 9 + k * 12
        s.add(f'<rect x="{lx-2}" y="{chip_y-6}" width="4" height="6" fill="#9ca3af"/>')
        s.add(f'<rect x="{lx-2}" y="{chip_y+40}" width="4" height="6" fill="#9ca3af"/>')
    s.add(f'<circle cx="{inner+8}" cy="{chip_y+32}" r="2.5" fill="#475569"/>')
    s.plain(inner + 27, chip_y + 24, "VP230", size=10, color="#e2e8f0", anchor="middle", bold=True)
    s.add(f'<rect x="{inner}" y="{smd_y-8}" width="30" height="16" rx="2" fill="#111827" stroke="#9ca3af"/>')
    s.plain(inner + 15, smd_y + 4, "121", size=9, color="#e5e7eb", anchor="middle", bold=True)
    s.plain(inner + 36, smd_y + 4, "120 Ω", size=10, color="#fde68a", bold=True)
    names = ["3V3", "GND", "CTX", "CRX", "CANH", "CANL"]
    for nm, y in zip(names, ys):
        s.over(f'<circle cx="{px}" cy="{y}" r="7" fill="#d1a33a" stroke="#fff" stroke-width="2"/>')
        if side == "right":
            s.plain(px - 14, y + 4, nm, size=11, anchor="end", bold=True)
        else:
            s.plain(px + 14, y + 4, nm, size=11, bold=True)
    return px + 8 if side == "right" else px - 8


def can():
    """Two SN65HVD230 on one bus: the P4's TWAI on the header, an ESP32 DevKit's on its IO21/IO22."""
    s = Svg(1200, 1180, "CAN on the 40-pin header: the P4 and an ESP32 on one bus",
            "two 3.3 V SN65HVD230 transceivers and a twisted pair with 120 Ω at each end; "
            "the P4's TWAI on GPIO46/47, the ESP32's on IO21/IO22")
    c_tx, c_rx, c_h, c_l, c_p = "#ea580c", "#16a34a", "#ca8a04", "#2563eb", C["p3"]
    used = {35: c_tx, 18: c_p, 19: "#111111", 37: c_rx}
    y_h, y_l = 910, 932                                 # the bus, under everything
    # the P4's transceiver, low on the left: pins 35 and 37 are in the right column, so their wires
    # go round under the header and meet CTX and CRX straight on
    x18, y18 = pin_xy(18)
    x19, y19 = pin_xy(19)
    x35, y35 = pin_xy(35)
    x37, y37 = pin_xy(37)
    y_tx = pin_xy(40)[1] + 35
    ys1 = [y_tx + (k - 2) * 2 * STEP for k in range(6)]
    m1 = sn65(s, 190, 150, ys1, "right", "SN65HVD230 #1", ys1[0] + 34, (ys1[4] + ys1[5]) / 2)
    s.wire([(x35 + 8, y35), (712, y35), (712, ys1[2]), (m1, ys1[2])], c_tx, 3)
    s.wire([(x37 + 8, y37), (728, y37), (728, ys1[3]), (m1, ys1[3])], c_rx, 3)
    # power by tags: the board's 3V3 and GND are the header's pins 18 and 19
    s.wire([(m1, ys1[0]), (380, ys1[0])], c_p, 3)
    s.net(380, ys1[0], "3V3", side="right")
    s.wire([(m1, ys1[1]), (380, ys1[1])], C["gnd"], 3)
    s.net(380, ys1[1], "GND", C["gnd"], side="right")
    s.wire([(x18 - 8, y18), (440, y18)], c_p, 3)
    s.net(440, y18, "3V3")
    s.wire([(x19 + 8, y19), (690, y19)], C["gnd"], 3)
    s.net(690, y19, "GND", C["gnd"], side="right")
    # the ESP32 DevKit, right of the header: IO21 is its TX, IO22 its RX
    right = DEVKIT_RIGHT
    i_gnd, i_rx, i_tx = 0, right.index("IO22"), right.index("IO21")
    lx, rx_, py, top, bot = devkit(s, 760, ROW0 + STEP, {
        ("L", 0): c_p, ("R", i_gnd): "#111111", ("R", i_rx): c_rx, ("R", i_tx): c_tx})
    ys2 = [py(-1), py(0), py(1), py(2), py(3), py(4)]
    m2 = sn65(s, 1030, 150, ys2, "left", "SN65HVD230 #2", py(0) - 6, py(3) + 4)
    s.wire([(rx_ + 8, py(i_gnd)), (m2, ys2[1])], C["gnd"], 3)
    s.wire([(rx_ + 8, py(i_rx)), (m2, ys2[3])], c_rx, 3)
    s.wire([(rx_ + 8, py(i_tx)), (975, py(i_tx)), (975, ys2[2]), (m2, ys2[2])], c_tx, 3)
    s.wire([(lx - 8, py(0)), (744, py(0)), (744, top - 24), (990, top - 24), (990, ys2[0]), (m2, ys2[0])], c_p, 3)
    s.text(760, top - 30, "the DevKit's 3V3 feeds transceiver #2")
    s.text(985, bot + 46, "its micro-USB on a USB host socket of the P4: 5 V, a GND shared with the P4, its console",
           color=C["dim"], anchor="end")
    # the bus: down from each board, twisted along the bottom
    h1, l1, h2, l2 = 390, 372, 1000, 1016
    s.wire([(m1, ys1[4]), (h1, ys1[4]), (h1, y_h), (450, y_h)], c_h, 3)
    s.wire([(m1, ys1[5]), (l1, ys1[5]), (l1, y_l), (450, y_l)], c_l, 3)
    s.wire([(930, y_h), (h2, y_h), (h2, ys2[4]), (m2, ys2[4])], c_h, 3)
    s.wire([(930, y_l), (l2, y_l), (l2, ys2[5]), (m2, ys2[5])], c_l, 3)
    for col, start in ((c_l, 1), (c_h, 0)):
        d, y0 = "", (y_h, y_l)
        for k in range(12):
            a, b = y0[(k + start) % 2], y0[(k + start + 1) % 2]
            x0 = 450 + 40 * k
            d += f"{'M' if k == 0 else ' L'}{x0} {a} C{x0+20} {a} {x0+20} {b} {x0+40} {b}"
        s.add(f'<path d="{d}" stroke="{col}" stroke-width="3" fill="none"/>')
    s.text(h1 + 8, y_h - 30, "CANH", color=c_h, bold=True)
    s.text(l1 - 8, y_l - 14, "CANL", color=c_l, anchor="end", bold=True)
    s.text(h2 - 8, 760, "CANH", color=c_h, anchor="end", bold=True)
    s.text(l2 + 8, 760, "CANL", color=c_l, bold=True)
    s.text(690, y_h - 18, "a twisted pair (two wires twisted by hand will do on a bench)", anchor="middle")
    s.text(690, y_l + 28, "60 Ω between CANH and CANL with everything off: the two 120 Ω in parallel",
           anchor="middle", bold=True)
    s.notes(40, y_l + 50, 1120, "Wiring it (the CAN app's own pins; on GPIO28/29 it worked on the board, 2026-11-07: 162 frames a second at 500 kbit/s, no errors)", [
        "• P4: TX (GPIO46, pin 35) to CTX and RX (GPIO47, pin 37) to CRX, not crossed as on a UART; 3V3 from pin 18, GND from pin 19",
        "• ESP32: IO21 to CTX and IO22 to CRX, its own 3V3 and GND. Not IO5 nor the other pins that toggle at boot: garbage on the bus",
        "• The ESP32's micro-USB on a USB host socket of the P4 (docs/USB.md): its 5 V, a GND shared with the P4, its console in the Terminal",
        "• 120 Ω at each end of the bus (these boards carry one, marked 121): 60 Ω between CANH and CANL with everything off (60.3 Ω measured)",
        "  A third node in the middle of the bus goes without its own. Tags with the same name (3V3, GND) are one wire",
        "• Only 3.3 V transceivers (SN65HVD230, 231, 232): a 5 V one (MCP2551, TJA1050) puts 5 V on the header. The same speed on every node",
    ])
    s.header(used)
    s.save("bench-can.svg")


def auto485(s, x, top, title):
    """The bench's RS485 module as it sits on the table, parts up: an automatic-direction "RS485 to
    TTL" board (an 8-pin transceiver, a 74HC-type chip that switches the direction on the data,
    TXD and RXD LEDs, fuses and TVS diodes, 120 ohm marked 121 across A and B). The green terminal
    block on top (A+, B-, earth), the four pins at the bottom (VCC, TXD, RXD, GND). Returns the x
    of each pin and terminal, and where the pins end and the screws start."""
    w, h = 150, 330
    s.add(f'<rect x="{x}" y="{top}" width="{w}" height="{h}" rx="5" fill="#1e3a8a"/>')
    s.text(x + w / 2, top - 92, title, size=13, color="#111", anchor="middle", bold=True)
    # the terminal block
    s.add(f'<rect x="{x+6}" y="{top-34}" width="{w-12}" height="44" rx="3" fill="#4ade80" stroke="#15803d"/>')
    tx = {"A+": x + 33, "B-": x + 75, "E": x + 117}
    for nm, cx in tx.items():
        s.add(f'<circle cx="{cx}" cy="{top-12}" r="13" fill="#d1d5db" stroke="#6b7280" stroke-width="2"/>')
        s.add(f'<path d="M{cx-7} {top-12} h14 M{cx} {top-19} v14" stroke="#6b7280" stroke-width="2"/>')
        s.plain(cx, top + 26, "⏚" if nm == "E" else nm.replace("-", "−"), size=12, color="#fde68a", anchor="middle", bold=True)
    # the parts: two fuses, the TVS, the transceiver, the direction chip, the LEDs, the 120 ohm
    s.add(f'<rect x="{x+16}" y="{top+44}" width="30" height="40" rx="2" fill="#ca8a04"/>')
    s.add(f'<rect x="{x+54}" y="{top+44}" width="30" height="40" rx="2" fill="#ca8a04"/>')
    s.add(f'<rect x="{x+92}" y="{top+46}" width="20" height="34" rx="2" fill="#111827"/>')
    s.add(f'<rect x="{x+118}" y="{top+46}" width="20" height="34" rx="2" fill="#111827"/>')
    s.add(f'<rect x="{x+60}" y="{top+120}" width="56" height="44" rx="3" fill="#0f172a"/>')
    s.plain(x + 88, top + 146, "485", size=10, color="#e2e8f0", anchor="middle", bold=True)
    s.add(f'<rect x="{x+40}" y="{top+210}" width="90" height="40" rx="3" fill="#0f172a"/>')
    s.plain(x + 85, top + 234, "74HC", size=10, color="#e2e8f0", anchor="middle", bold=True)
    s.plain(x + 85, top + 268, "auto direction", size=9, color="#bfdbfe", anchor="middle")
    for k, nm in enumerate(("TXD", "RXD")):
        lx = x + 18 + k * 26
        s.add(f'<rect x="{lx}" y="{top+150}" width="10" height="16" rx="2" fill="#f87171"/>')
        s.plain(lx + 5, top + 182, nm, size=8, color="#e5e7eb", anchor="middle")
    s.add(f'<rect x="{x+16}" y="{top+96}" width="22" height="10" rx="2" fill="#111827" stroke="#9ca3af"/>')
    s.plain(x + 27, top + 104, "121", size=7, color="#e5e7eb", anchor="middle", bold=True)
    s.plain(x + 44, top + 105, "120 Ω on board", size=9, color="#fde68a", bold=True)
    # the pins
    px = {nm: x + 30 + 30 * k for k, nm in enumerate(("VCC", "TXD", "RXD", "GND"))}
    bot = top + h
    for nm, cx in px.items():
        s.add(f'<rect x="{cx-5}" y="{bot}" width="10" height="20" fill="#9ca3af"/>')
        s.over(f'<circle cx="{cx}" cy="{bot+22}" r="7" fill="#d1a33a" stroke="#fff" stroke-width="2"/>')
        s.plain(cx, bot - 10, nm, size=10, color="#e5e7eb", anchor="middle", bold=True)
    return px, tx, bot + 22, top - 12


def rs485():
    """Two automatic RS485 modules on one bus: the P4's uart.a on the header, an ESP32 DevKit's UART2."""
    s = Svg(1240, 1280, "RS485 on the 40-pin header: the P4 and an ESP32 on one bus",
            "two automatic \"RS485 to TTL\" modules (no DE pin, 120 Ω each) on 3.3 V and a twisted pair; the P4's uart.a "
            "on GPIO30/31, the ESP32's UART2 on IO17/IO4, and a WS2812B strip on its IO18 for DMX")
    c_tx, c_rx, c_a, c_b, c_p = "#ea580c", "#16a34a", "#ca8a04", "#2563eb", C["p3"]
    used = {18: c_p, 22: c_tx, 24: c_rx, 26: "#111111"}
    c_din = "#7c3aed"
    y_a, y_b = 1000, 1022
    x18, y18 = pin_xy(18)
    x22, y22 = pin_xy(22)
    x24, y24 = pin_xy(24)
    x26, y26 = pin_xy(26)
    # the P4's module, left of the header
    p1, t1, yp1, yt1 = auto485(s, 150, 260, "RS485 module #1 (the P4's)")
    s.wire([(x22 - 8, y22), (400, y22), (400, yp1 + 70), (p1["TXD"], yp1 + 70), (p1["TXD"], yp1 + 8)], c_tx, 3)
    s.wire([(x24 - 8, y24), (380, y24), (380, yp1 + 50), (p1["RXD"], yp1 + 50), (p1["RXD"], yp1 + 8)], c_rx, 3)
    s.text(406, y22 - 8, "TX → TXD", color=c_tx, bold=True)
    s.text(374, y24 + 18, "RX ← RXD", color=c_rx, anchor="end", bold=True)
    s.wire([(p1["VCC"], yp1 + 8), (p1["VCC"], yp1 + 22)], c_p, 3)
    s.net(p1["VCC"] + 6, yp1 + 33, "3V3", side="left")
    s.wire([(p1["GND"], yp1 + 8), (p1["GND"], yp1 + 22)], C["gnd"], 3)
    s.net(p1["GND"] - 6, yp1 + 33, "GND", C["gnd"], side="right")
    s.wire([(x18 - 8, y18), (450, y18)], c_p, 3)
    s.net(450, y18, "3V3")
    s.wire([(x26 - 8, y26), (450, y26)], C["gnd"], 3)
    s.net(450, y26, "GND", C["gnd"])
    s.text(440, pin_xy(20)[1] + 4, "pin 20 (DE): free", color=C["dim"], anchor="end")
    # the ESP32 DevKit and its module
    right = DEVKIT_RIGHT
    i_gnd, i_tx, i_rx, i_din = 0, right.index("IO17"), right.index("IO4"), right.index("IO18")
    i_5v, i_g2 = DEVKIT_LEFT.index("5V"), DEVKIT_LEFT.index("GND")
    lx, rx_, py, top, bot = devkit(s, 760, ROW0 + STEP, {
        ("L", 0): c_p, ("R", i_gnd): "#111111", ("R", i_tx): c_tx, ("R", i_rx): c_rx,
        ("R", i_din): c_din, ("L", i_5v): C["p5"], ("L", i_g2): "#111111"})
    p2, t2, yp2, yt2 = auto485(s, 1010, 260, "RS485 module #2 (the ESP32's)")
    s.wire([(rx_ + 8, py(i_tx)), (985, py(i_tx)), (985, yp2 + 70), (p2["TXD"], yp2 + 70), (p2["TXD"], yp2 + 8)], c_tx, 3)
    s.wire([(rx_ + 8, py(i_rx)), (1000, py(i_rx)), (1000, yp2 + 50), (p2["RXD"], yp2 + 50), (p2["RXD"], yp2 + 8)], c_rx, 3)
    s.wire([(p2["VCC"], yp2 + 8), (p2["VCC"], yp2 + 22)], c_p, 3)
    s.net(p2["VCC"] + 6, yp2 + 33, "ESP 3V3", side="left")
    s.wire([(p2["GND"], yp2 + 8), (p2["GND"], yp2 + 22)], C["gnd"], 3)
    s.net(p2["GND"] - 6, yp2 + 33, "GND", C["gnd"], side="right")
    s.wire([(lx - 8, py(0)), (730, py(0))], c_p, 3)
    s.net(730, py(0), "ESP 3V3")
    s.wire([(rx_ + 8, py(i_gnd)), (990, py(i_gnd))], C["gnd"], 3)
    s.net(990, py(i_gnd), "GND", C["gnd"], side="right")
    s.text(985, bot + 46, "its micro-USB on a USB host socket of the P4 (or the Mac): 5 V, GND, its console",
           color=C["dim"], anchor="end")
    # the DMX light: 8 WS2812B pixels, by tags (IO18 to DIN, the DevKit's 5V and a GND)
    s.wire([(rx_ + 8, py(i_din)), (962, py(i_din))], c_din, 3)
    s.net(962, py(i_din), "DIN", c_din, side="right")
    s.wire([(lx - 8, py(i_5v)), (732, py(i_5v))], C["p5"], 3)
    s.net(732, py(i_5v), "ESP 5V", C["p5"])
    s.wire([(lx - 8, py(i_g2)), (742, py(i_g2))], C["gnd"], 3)
    s.net(742, py(i_g2), "GND", C["gnd"])
    sx0, sx1, sy = 470, 870, 836
    s.add(f'<rect x="{sx0}" y="{sy}" width="{sx1-sx0}" height="68" rx="8" fill="#111827"/>')
    for i in range(8):
        cx_ = sx1 - 40 - i * 46
        col = ["#ef4444", "#f97316", "#facc15", "#22c55e", "#06b6d4", "#3b82f6", "#8b5cf6", "#ec4899"][i]
        s.add(f'<rect x="{cx_-15}" y="{sy+19}" width="30" height="30" rx="4" fill="#f9fafb"/>')
        s.add(f'<circle cx="{cx_}" cy="{sy+34}" r="9" fill="{col}"/>')
    s.plain(sx0 + 12, sy + 88, "8 WS2812B pixels (the DMX light role: channels 1-24, RGB each); data runs right to left",
            size=11, color=C["dim"])
    pads = {"5V": sy + 10, "GND": sy + 34, "DIN": sy + 58}
    lab = {"5V": ("ESP 5V", C["p5"]), "GND": ("GND", C["gnd"]), "DIN": ("DIN", c_din)}
    for name, y in pads.items():
        s.add(f'<rect x="{sx1}" y="{y-6}" width="14" height="12" rx="2" fill="#d1a33a"/>')
        s.wire([(sx1 + 14, y), (sx1 + 40, y)], lab[name][1], 3)
        s.net(sx1 + 40, y, lab[name][0], lab[name][1], side="right")
    s.text(sx0 + 4, sy - 12, "WS2812B strip on the ESP32 (only for DMX)", size=13, color="#111", bold=True)
    # the bus: from each terminal block, round the board and along the bottom
    s.wire([(t1["A+"], yt1 - 14), (t1["A+"], 214), (120, 214), (120, y_a), (450, y_a)], c_a, 3)
    s.wire([(t1["B-"], yt1 - 14), (t1["B-"], 200), (104, 200), (104, y_b), (450, y_b)], c_b, 3)
    s.wire([(t2["B-"], yt2 - 14), (t2["B-"], 214), (1190, 214), (1190, y_a), (890, y_a)], c_b, 3)
    s.wire([(t2["A+"], yt2 - 14), (t2["A+"], 200), (1206, 200), (1206, y_b), (890, y_b)], c_a, 3)
    for col, start in ((c_b, 1), (c_a, 0)):
        d, y0 = "", (y_a, y_b)
        for k in range(11):                     # an odd count: A and B swap lanes, as a twist does
            a, b = y0[(k + start) % 2], y0[(k + start + 1) % 2]
            x0 = 450 + 40 * k
            d += f"{'M' if k == 0 else ' L'}{x0} {a} C{x0+20} {a} {x0+20} {b} {x0+40} {b}"
        s.add(f'<path d="{d}" stroke="{col}" stroke-width="3" fill="none"/>')
    s.text(126, 760, "A+", color=c_a, bold=True)
    s.text(98, 780, "B−", color=c_b, anchor="end", bold=True)
    s.text(1184, 860, "B−", color=c_b, anchor="end", bold=True)
    s.text(1212, 880, "A+", color=c_a, bold=True)
    s.text(670, y_a - 18, "a twisted pair: A+ to A+ and B− to B−", anchor="middle")
    s.text(670, y_b + 28, "60 Ω between A and B with everything off: the two modules' 120 Ω in parallel",
           anchor="middle", bold=True)
    s.notes(40, y_b + 50, 1160, "Wiring it (tools/rs485_test/README.md; the Modbus app's Cableado tab says the same)", [
        "• P4: TX (GPIO30, pin 22) to TXD, RX (GPIO31, pin 24) to RXD; VCC to 3V3 (pin 18), GND to pin 26. GPIO29 (DE) stays free",
        "• ESP32: IO17 to TXD, IO4 to RXD, VCC to its 3V3, GND to its GND. IO16 (DE) stays free",
        "• DMX light: the strip's DIN to IO18, its 5V to the DevKit's 5V pin (from its USB), GND to a GND. 8 pixels at a third of the light",
        "• These modules name TXD and RXD the MCU's way (TX to TXD). If the TXD LED does not blink when the P4 sends, or nobody",
        "  answers, swap TXD and RXD on that module: at 3.3 V on both sides nothing gets hurt",
        "• Feed them from 3.3 V, not 5 V: at 5 V the RXD pin would put 5 V on the header, which is not 5 V tolerant",
        "• Each module carries its 120 Ω (marked 121): right for a bus of two. A third module in the middle needs its 121 lifted",
        "• The third screw (earth) is for a cable's shield; on the bench the two boards share GND through the USB already",
    ])
    s.header(used)
    s.save("bench-rs485.svg")


def w5500():
    """A W5500 Lite Ethernet module (HW-884) on spi.a, seen from above with its RJ45 at the top."""
    s = Svg(1100, 905, "A W5500 Ethernet module on the 40-pin header",
            "the W5500 Lite (HW-884) on spi.a with jumper wires: the bench before the auxiliary board")
    c_sck, c_mo, c_mi, c_cs = "#2563eb", "#16a34a", "#ca8a04", "#ea580c"
    used = {18: C["p3"], 26: "#111111", 32: c_cs, 34: c_mo, 36: c_mi, 38: c_sck}
    x18, y18 = pin_xy(18)
    x26, y26 = pin_xy(26)
    x32, y32 = pin_xy(32)
    x34, y34 = pin_xy(34)
    x36, y36 = pin_xy(36)
    x38, y38 = pin_xy(38)
    # the module: two rows of six pins 20.32 mm apart (eight pitches), MI level with the header's pin 36
    lx, rx = 76, 76 + 8 * STEP
    ys = [y36 - (5 - k) * STEP for k in range(6)]
    s.add(f'<rect x="{lx-18}" y="{ys[0]-32}" width="{rx-lx+36}" height="{ys[5]-ys[0]+70}" rx="5" fill="#15803d"/>')
    s.add(f'<rect x="{lx+42}" y="{ys[0]-107}" width="{rx-lx-84}" height="205" rx="4" fill="#9ca3af" stroke="#6b7280" stroke-width="2"/>')
    s.add(f'<rect x="{lx+60}" y="{ys[0]-107}" width="{rx-lx-120}" height="34" fill="#374151"/>')
    s.plain((lx + rx) / 2, ys[0] - 40, "RJ45", size=13, color="#374151", anchor="middle", bold=True)
    s.plain((lx + rx) / 2, ys[0] - 22, "top view", size=10, color="#4b5563", anchor="middle")
    s.plain((lx + rx) / 2, ys[5] + 26, "W5500 Lite", size=12, color="#dcfce7", anchor="middle", bold=True)
    for nm, y in zip(["G", "G", "MO", "SCK", "CS", "INT"], ys):
        s.over(f'<circle cx="{lx}" cy="{y}" r="7" fill="#d1a33a" stroke="#fff" stroke-width="2"/>')
        s.plain(lx + 13, y + 4, nm, size=11, bold=True)
    for nm, y in zip(["GND", "V", "V", "NC", "RST", "MI"], ys):
        s.over(f'<circle cx="{rx}" cy="{y}" r="7" fill="#d1a33a" stroke="#fff" stroke-width="2"/>')
        s.plain(rx - 13, y + 4, nm, size=11, anchor="end", bold=True)
    # MISO straight across; SCK round the bottom; CS and MOSI over the top, to the far row
    s.wire([(x36 - 8, y36), (rx + 8, ys[5])], c_mi, 3)
    s.wire([(x38 - 8, y38), (400, y38), (400, 700), (20, 700), (20, ys[3]), (lx - 8, ys[3])], c_sck, 3)
    s.wire([(36, ys[3] - 9), (36, ys[3] + 9)], "#ffffff", 9)        # CS hops over SCK: the one crossing
    s.wire([(x32 - 8, y32), (380, y32), (380, 340), (36, 340), (36, ys[4]), (lx - 8, ys[4])], c_cs, 3)
    s.wire([(x34 - 8, y34), (360, y34), (360, 355), (52, 355), (52, ys[2]), (lx - 8, ys[2])], c_mo, 3)
    s.text(400 + 8, 690, "SCK", color=c_sck, bold=True)
    s.text(380 + 8, 334, "CS", color=c_cs, bold=True)
    s.text(322, 372, "MOSI", color=c_mo, bold=True)
    s.text(350, y36 - 8, "MISO", color=c_mi, bold=True)
    # power by tags: every tag with the same name is one wire
    s.wire([(x18 - 8, y18), (440, y18)], C["p3"])
    s.net(440, y18, "3V3")
    s.wire([(x26 - 8, y26), (440, y26)], C["gnd"])
    s.net(440, y26, "GND", C["gnd"])
    s.wire([(rx + 8, ys[0]), (rx + 24, ys[0])], C["gnd"])
    s.net(rx + 24, ys[0], "GND", C["gnd"], side="right")
    s.wire([(rx + 8, ys[1]), (rx + 24, ys[1])], C["p3"])
    s.net(rx + 24, ys[1], "3V3", side="right")
    s.notes(720, 330, 340, "The first test, with no new firmware", [
        "• Bus, SPI: port spi.a, CS 49, mode 0, 1 MHz",
        "• send the bytes 00 39 00 00",
        "• the fourth byte back must be 04:",
        "  the W5500's version register",
    ])
    s.notes(40, NOTES_Y + 20, 1020, "Wiring it (it worked on the board, 2026-11-11: the chip answered 00 00 00 04, its version, at 1 MHz)", [
        "• SCK to GPIO52 (pin 38), MO to GPIO50 (pin 34), MI to GPIO51 (pin 36), CS to GPIO49 (pin 32): the header's spi.a",
        "• V to 3V3 (pin 18), GND to pin 26. The module is 3.3 V only, about 200 mA, with no regulator of its own: never 5 V",
        "• INT, NC, RST and the second V and G stay free: the driver polls. If the chip does not answer, try RST to 3V3",
        "• The pins as printed on the board: G, G, MO, SCK, CS, INT down one side, GND, V, V, NC, RST, MI down the other. Read yours before powering it",
        "• Short wires: the SPI runs at several MHz. Tags with the same name (3V3, GND) are one wire. The two rows are 20.32 mm apart",
    ])
    s.header(used)
    s.save("bench-w5500.svg")


if __name__ == "__main__":
    pwm()
    ir()
    strip()
    eeprom24()
    esp32_devkit()
    nano_header()
    can()
    rs485()
    w5500()
