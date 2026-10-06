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
            self.add(f'<text x="{x+18}" y="{y+46+20*i}" font-size="12" fill="{C["text"]}">{l}</text>')

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
    used = {16: "#f59e0b", 20: "#f59e0b", 22: "#8b5cf6", 26: "#111111", 31: "#f59e0b", 33: "#111111"}
    gx = 60
    x16, y16 = pin_xy(16)
    x20, y20 = pin_xy(20)
    x22, y22 = pin_xy(22)
    x26, y26 = pin_xy(26)
    # LED 1 and LED 2: resistor, LED, to the GND rail on the left
    for (x, y, col, lbl) in ((x16, y16, "#ef4444", "LED 1, 1 kHz"), (x20, y20, "#22c55e", "LED 2, 5 kHz")):
        s.resistor_h(x - 8, 300, y, "330 Ω")
        s.wire([(300, y), (230, y)])
        s.led_h(230, y, col)
        s.wire([(194, y), (gx, y)], C["gnd"])
        s.dot(gx, y)
        s.text(160, y - 18, lbl, anchor="middle")
    # the analog level: 1 kΩ, a node, 1 µF down to the GND wire of pin 26
    s.resistor_h(x22 - 8, 330, y22, "1 kΩ", fill="#ddd6fe", edge="#5b21b6", above=False)
    s.wire([(330, y22), (220, y22)], "#7c3aed")
    s.dot(220, y22, "#7c3aed")
    s.wire([(220, y22), (220, y22 + 8)], "#7c3aed")
    s.add(f'<path d="M206 {y22+9} H234 M206 {y22+17} H234" stroke="#5b21b6" stroke-width="3"/>')
    s.wire([(220, y22 + 18), (220, y26)], C["gnd"])
    s.dot(220, y26)
    s.text(240, y22 + 18, "1 µF")
    s.wire([(x26 - 8, y26), (gx, y26), (gx, y16)], C["gnd"])
    s.dot(gx, y26)
    s.text(gx, y26 + 22, "GND (pin 26)", bold=True, color="#111")
    s.wire([(220, y22), (150, y26 + 60)], "#7c3aed", 1.5, "4 3")
    s.box(40, y26 + 60, 230, 30, "#f5f3ff", "#7c3aed")
    s.text(155, y26 + 80, "meter here to GND: 0-3.3 V", color="#5b21b6", anchor="middle")
    s.text(155, y26 + 110, "analog level (sigma-delta, no timer)", color="#7c3aed", anchor="middle")
    # the servo: signal pin 31, GND pin 33, and its own 5 V supply beside it
    x31, y31 = pin_xy(31)
    x33, y33 = pin_xy(33)
    top = y31 - 160
    s.box(870, top, 130, 64, "#2563eb", "#1e3a8a", "Servo", "SG90, MG90...", "#fff")
    s.box(1040, top, 140, 64, "#fee2e2", "#dc2626", "its own 5 V", "supply", "#991b1b")
    s.wire([(1000, top + 22), (1040, top + 22)], "#dc2626", 3)
    s.text(1020, top + 12, "+", color="#dc2626", anchor="middle", bold=True)
    s.wire([(x31 + 8, y31), (900, y31), (900, top + 64)], "#f97316", 3)
    s.wire([(x33 + 8, y33), (1110, y33), (1110, top + 64)], "#78350f", 3)
    s.wire([(960, y33), (960, top + 64)], "#78350f", 3)
    s.dot(960, y33, "#78350f")
    s.text(760, y31 - 8, "signal (orange): PWM 50 Hz")
    s.text(760, y33 + 20, "GND (brown), shared by the servo and its supply")
    s.notes(40, NOTES_Y, 1120, "What it tries (all of it worked on the board, 2026-10-05)", [
        "• LED 1 (GPIO28, pin 16) at 1 kHz and LED 2 (GPIO29, pin 20) at 5 kHz: two LEDC timers. LEDs: the long leg (anode) toward the resistor",
        "• The servo (GPIO32, pin 31) at 50 Hz, 500-2500 µs: the third timer, the last one free (LEDC timer 1 is the screen's backlight)",
        "• The analog level (GPIO30, pin 22): sigma-delta through 1 kΩ and 1 µF, no timer; 1.34 V asked, 1.313 V measured",
        "• Without its own supply, a small servo can take 5 V from pin 1 or 3, with 470 µF across it near the servo",
    ])
    s.header(used)
    s.save("bench-pwm.svg")


def ir():
    s = Svg(1100, 860, "Infrared on the 40-pin header",
            "a demodulating receiver (VS1838B, TSOP38238) and an IR LED driven by an NPN transistor")
    used = {1: "#dc2626", 11: "#f59e0b", 13: "#111111", 18: "#dc2626", 24: "#f59e0b", 26: "#111111"}
    x1, y1 = pin_xy(1)
    x11, y11 = pin_xy(11)
    x13, y13 = pin_xy(13)
    x18, y18 = pin_xy(18)
    x24, y24 = pin_xy(24)
    x26, y26 = pin_xy(26)
    # the receiver, right: OUT pin 11, GND pin 13; VCC from 3V3 pin 18 around the bottom
    rx, ry = 830, y11 - 26
    s.box(rx, ry, 150, 110, "#111827", "#111827")
    s.add(f'<circle cx="{rx+90}" cy="{ry+50}" r="24" fill="#374151"/>')
    s.plain(rx + 90, ry + 98, "receiver", anchor="middle")
    s.wire([(x11 + 8, y11), (rx, y11)])
    s.wire([(x13 + 8, y13), (rx, y13)], C["gnd"])
    vy = y13 + 26
    # 3V3 by tags: pin 18's and the receiver's are the same wire
    s.wire([(x18 - 8, y18), (420, y18)], C["p3"])
    s.net(420, y18, "3V3")
    s.wire([(rx - 60, vy), (rx, vy)], C["p3"])
    s.net(rx - 60, vy, "3V3")
    for (y, t) in ((y11, "OUT"), (y13, "GND"), (vy, "VCC")):
        s.plain(rx + 8, y + 4, t, bold=True)
    s.text(rx, ry + 136, "VS1838B from the front: OUT, GND, VCC")
    s.text(rx, ry + 154, "(other models differ: check yours)")
    s.text(rx, ry + 172, "VCC to 3.3 V (pin 18), never 5 V", color="#b91c1c")
    # the sender, left: pin 24 -> 1 kΩ -> base
    s.resistor_h(x24 - 8, 330, y24, "1 kΩ")
    bx = 260                                # the base bar
    s.wire([(330, y24), (bx, y24)])
    s.add(f'<circle cx="{bx-18}" cy="{y24}" r="32" fill="#fff" stroke="#111" stroke-width="2"/>')
    s.add(f'<path d="M{bx} {y24-18} V{y24+18}" stroke="#111" stroke-width="3"/>')
    cx_ = bx - 34                           # collector and emitter leave on this x
    s.add(f'<path d="M{bx} {y24-8} L{cx_} {y24-26} V{y24-70}" stroke="#111" stroke-width="2.4" fill="none"/>')
    s.add(f'<path d="M{bx} {y24+8} L{cx_} {y24+26} V{y26}" stroke="#111" stroke-width="2.4" fill="none"/>')
    s.add(f'<path d="M{cx_+4} {y24+18} L{cx_-2} {y24+28} L{cx_+10} {y24+28} Z" fill="#111"/>')
    s.text(bx + 6, y24 - 12, "B", size=11)
    s.text(cx_ - 16, y24 - 40, "C", size=11)
    s.text(cx_ - 16, y24 + 46, "E", size=11)
    s.text(cx_ - 120, y24 + 6, "NPN 2N2222", size=11)
    s.text(cx_ - 120, y24 + 22, "or BC547", size=11)
    s.wire([(x26 - 8, y26), (cx_, y26)], C["gnd"])
    s.dot(cx_, y26)
    s.text(cx_ + 10, y26 + 20, "emitter to GND (pin 26)")
    # the IR LED and its 47 Ω up to 5 V from pin 1 (over the top)
    ly = y24 - 150
    s.wire([(cx_, y24 - 70), (cx_, ly + 32)], C["gnd"])
    s.add(f'<path d="M{cx_-14} {ly} L{cx_+14} {ly} L{cx_} {ly+30} Z" fill="#7c3aed" stroke="#4c1d95" stroke-width="2"/>')
    s.add(f'<path d="M{cx_-14} {ly+32} H{cx_+14}" stroke="#4c1d95" stroke-width="3"/>')
    s.add(f'<path d="M{cx_+20} {ly+8} l14 -10 M{cx_+22} {ly+20} l14 -10" stroke="#7c3aed" stroke-width="2"/>')
    s.text(cx_ + 42, ly + 14, "IR LED (940 nm)")
    s.text(cx_ + 42, ly + 30, "long leg up, toward 47 Ω")
    s.resistor_v(cx_, ly - 90, ly, "47 Ω", C["p5"])
    s.wire([(x1 + 8, y1), (620, y1), (620, ROW0 - 60), (cx_, ROW0 - 60), (cx_, ly - 90)], C["p5"])
    s.text(cx_ + 10, ROW0 - 66, "5 V from pin 1 (or 3)", color="#b91c1c")
    s.notes(40, NOTES_Y + 20, 1020, "What it tries (all of it worked on the board, 2026-10-05)", [
        "• Learn: the receiver on GPIO5 (pin 11) read a monitor's remote: Samsung, address 0x0707, command 7 (volume up)",
        "• Send: the LED on GPIO31 (pin 24) at 38 kHz turned the monitor's volume up, from the learned code and from SmartIR's library",
        "• A header pin gives about 20 mA; through the transistor the LED takes ~70 mA from 5 V and reaches across a room",
        "• Not GPIO21/22 for the receiver: they are the I2C bus the system scans by itself. The two 3V3 tags are one wire",
    ])
    s.header(used)
    s.save("bench-ir.svg")


def strip():
    s = Svg(1100, 840, "A WS2812B strip on the 40-pin header",
            "12 LEDs fed from the header's 5 V, the data straight from a 3.3 V pin through a resistor")
    used = {1: "#dc2626", 10: "#111111", 14: "#f59e0b"}
    x1, y1 = pin_xy(1)
    x10, y10 = pin_xy(10)
    x14, y14 = pin_xy(14)
    vd, vg, v5 = 350, 325, 300            # the three wires come down left of the pin labels
    # the strip lies below, its input at the right end: 5V, GND, DIN pads
    sy = y14 + 170
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
    s.wire([(x14 - 8, y14), (vd, y14)])
    s.resistor_v(vd, y14, pads["DIN"], "330 Ω")
    s.wire([(vd, pads["DIN"]), (end + 12, pads["DIN"])])
    s.wire([(x10 - 8, y10), (vg, y10), (vg, pads["GND"]), (end + 12, pads["GND"])], C["gnd"])
    s.wire([(x1 + 8, y1), (620, y1), (620, ROW0 - 60), (v5, ROW0 - 60), (v5, pads["5V"]), (end + 12, pads["5V"])], C["p5"])
    s.text(v5 + 6, ROW0 - 66, "5 V from pin 1 (or 3)", color="#b91c1c")
    s.text(vg + 8, y10 - 8, "GND (pin 10)")
    s.text(vd + 18, y14 - 8, "DIN", color="#b45309", bold=True)
    # the capacitor across the input
    cxp = 200
    s.add(f'<path d="M{cxp} {sy-92} V{sy-78} M{cxp-14} {sy-78} H{cxp+14} M{cxp-14} {sy-70} H{cxp+14} M{cxp} {sy-70} V{sy-56}" stroke="#5b21b6" stroke-width="2.6" fill="none"/>')
    s.text(cxp - 22, sy - 76, "470-1000 µF", anchor="end")
    s.text(cxp - 22, sy - 60, "5V to GND", anchor="end")
    s.text(cxp - 22, sy - 44, "at the strip", anchor="end")
    s.notes(40, NOTES_Y, 1020, "What it tries (it worked on the board, 2026-10-05)", [
        "• The data on GPIO4 (pin 14) through 330 Ω near the strip; 5 V from pin 1 (or 3), GND from pin 10",
        "• In LED Strips: GPIO4, WS2812B, GRB, 12 LEDs, a 500 mA budget (12 LEDs at full white would take ~720 mA)",
        "• Colours in the right order, effects at 50 fps, and back by itself after a restart",
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


if __name__ == "__main__":
    pwm()
    ir()
    strip()
    eeprom24()
