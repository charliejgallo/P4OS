#!/usr/bin/env python3
"""Draws docs/img/pinout.svg: the 40-pin header with what P4OS puts on each pin.

    python3 tools/pinout_diagram.py

The header is drawn as it faces you on the board, like tools/bench_diagrams.py:
pin 1 at the top right, odd pins down the right column, even pins down the
left one. Next to each pin, nearest first: its name, the port of the default
modules.txt profile (components/aos_io/aos_io.c, DEFAULT_MODULES) that uses
it, what else the system does there, and the pin an app picks by default when
it has one. Keep PINS in step with those when a default changes.
"""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "img", "pinout.svg")

# kind -> (fill, text, stroke). "app", "rec" and "alt" are outlined: not fixed. "app" is the pin an
# app starts with, "rec" the pin suggested for something that has no default (docs/MODULES.md,
# "Suggested pins"), "alt" a second use or a note.
KIND = {
    "pwr":  ("#b3423a", "#fff", None),
    "gnd":  ("#4b5263", "#fff", None),
    "gpio": ("#4a8a5c", "#fff", None),
    "i2c":  ("#2f5fa8", "#fff", None),
    "uart": ("#0f7b7b", "#fff", None),
    "spi":  ("#7446b8", "#fff", None),
    "usb":  ("#a8691a", "#fff", None),
    "dbg":  ("#b03a78", "#fff", None),
    "sys":  ("#6b7280", "#fff", None),
    "app":  ("#fff7ed", "#9a3412", "#ea580c"),
    "rec":  ("#f0fdf4", "#166534", "#16a34a"),
    "alt":  ("#ffffff", "#4b5563", "#9ca3af"),
}

LEGEND = [
    ("pwr", "Power"), ("gnd", "Ground"), ("gpio", "GPIO"), ("i2c", "I2C port"), ("uart", "UART port"),
    ("spi", "SPI port"), ("usb", "USB host"), ("dbg", "Target / SWD"), ("sys", "Taken by the board"),
    ("app", "An app's default pin"), ("rec", "Suggested pin"), ("alt", "Also / note"),
]

# pin -> chips, nearest the header first: (text, kind)
PINS = {
    1:  [("5V", "pwr"), ("live with the board off", "alt")],
    2:  [("3V3", "pwr"), ("3 A, off with POWER", "alt")],
    3:  [("5V", "pwr"), ("USB host VBUS", "usb")],
    4:  [("GPIO7", "gpio"), ("i2c.board SDA", "i2c"), ("touch, audio: shared", "alt")],
    5:  [("GND", "gnd")],
    6:  [("GPIO8", "gpio"), ("i2c.board SCL", "i2c"), ("400 kHz, 2.2k pull-ups", "alt")],
    7:  [("GPIO37", "gpio"), ("UART0 TX: console", "sys")],
    8:  [("GPIO2", "gpio"), ("target BOOT", "dbg"), ("touch INT if R108", "alt")],
    9:  [("GPIO38", "gpio"), ("UART0 RX: console", "sys")],
    10: [("GND", "gnd")],
    11: [("GPIO5", "gpio"), ("target EN / NRST", "dbg")],
    12: [("GPIO3", "gpio"), ("uart.b TX", "uart"), ("SWCLK", "dbg")],
    13: [("GND", "gnd")],
    14: [("GPIO4", "gpio"), ("uart.b RX", "uart"), ("SWDIO", "dbg")],
    15: [("GPIO21", "gpio"), ("i2c.ext SDA", "i2c"), ("ADC1 ch5", "alt")],
    16: [("GPIO28", "gpio"), ("IR receiver", "app")],
    17: [("GPIO22", "gpio"), ("i2c.ext SCL", "i2c"), ("ADC1 ch6", "alt")],
    18: [("3V3", "pwr")],
    19: [("GND", "gnd")],
    20: [("GPIO29", "gpio"), ("uart.a DE", "uart"), ("RS485", "alt")],
    21: [("GPIO24", "gpio"), ("USB FS D−", "usb"), ("host, 12 Mbit/s", "alt")],
    22: [("GPIO30", "gpio"), ("uart.a TX", "uart"), ("RS485", "alt")],
    23: [("GPIO25", "gpio"), ("USB FS D+", "usb")],
    24: [("GPIO31", "gpio"), ("uart.a RX", "uart"), ("RS485", "alt")],
    25: [("USBD_N", "usb"), ("USB HS D−", "usb"), ("host or OTG, 480 Mbit/s", "alt")],
    26: [("GND", "gnd")],
    27: [("USBD_P", "usb"), ("USB HS D+", "usb")],
    28: [("GPIO34", "gpio"), ("1-Wire", "rec"), ("PWM", "rec"), ("strapping", "alt")],
    29: [("GND", "gnd")],
    30: [("GPIO35", "gpio"), ("BOOT button", "sys")],
    31: [("GPIO32", "gpio"), ("IR LED", "app")],
    32: [("GPIO49", "gpio"), ("spi.a CS", "spi"), ("ADC2 ch0", "alt")],
    33: [("GND", "gnd")],
    34: [("GPIO50", "gpio"), ("spi.a MOSI", "spi"), ("ADC2 ch1", "alt")],
    35: [("GPIO46", "gpio"), ("CAN TX", "app")],
    36: [("GPIO51", "gpio"), ("spi.a MISO", "spi"), ("ADC2 ch2", "alt")],
    37: [("GPIO47", "gpio"), ("CAN RX", "app")],
    38: [("GPIO52", "gpio"), ("spi.a SCK", "spi"), ("ADC2 ch3", "alt")],
    39: [("GPIO48", "gpio"), ("LED strip", "rec"), ("PWM / servo", "rec")],
    40: [("GND", "gnd")],
}

NOTES = [
    "Orange is the pin an app starts with, green the one suggested for what has no default; any free GPIO does for either. More PWM outputs: a PCA9685 on I2C.",
    "SWD rides on a UART port's pins (TX = SWCLK, RX = SWDIO, EN = NRST): one cable for the Terminal, the Flasher and SWD.",
    "Signals are 3.3 V, GPIO46 to 48 too (the VO4 LDO's domain, measured). Nothing is fixed but the grey ones: modules.txt names each port's pins.",
]

W, ROW0, STEP, CH = 1500, 120, 44, 30
CX = W / 2
FONT = 'font-family="ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"'


def chip_w(text):
    return round(len(text) * 8.3 + 22)


def chip(x, y, text, kind, anchor):
    """A rounded label whose near edge is at x; anchor 'l' grows right, 'r' grows left. Returns the far edge."""
    fill, fg, stroke = KIND[kind]
    w = chip_w(text)
    x0 = x if anchor == "l" else x - w
    st = f' stroke="{stroke}" stroke-width="1.5"' if stroke else ""
    s = (f'<rect x="{x0}" y="{y - CH / 2}" width="{w}" height="{CH}" rx="7" fill="{fill}"{st}/>'
         f'<text x="{x0 + w / 2}" y="{y + 5}" text-anchor="middle" font-size="14" font-weight="600" fill="{fg}">{text}</text>')
    return s, (x0 + w if anchor == "l" else x0)


def main():
    h = ROW0 + 20 * STEP + 170
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {h}" {FONT}>',
         f'<rect width="{W}" height="{h}" rx="18" fill="#ffffff" stroke="#e5e7eb"/>',
         f'<text x="{CX}" y="44" text-anchor="middle" font-size="24" font-weight="700" fill="#111827">P4OS on the 40-pin header</text>',
         f'<text x="{CX}" y="70" text-anchor="middle" font-size="14" fill="#6b7280">'
         'as it faces you on the board (pin 1 top right): the default ports of modules.txt and the pins the apps start with</text>',
         f'<rect x="{CX - 62}" y="{ROW0 - 30}" width="124" height="{20 * STEP + 16}" rx="6" fill="#2f343b"/>']
    for n in range(1, 41):
        y = ROW0 + ((n - 1) // 2) * STEP
        odd = n % 2 == 1
        side = 1 if odd else -1
        o.append(f'<rect x="{CX + side * 21 - 8}" y="{y - 8}" width="16" height="16" fill="#0b0d10"/>')
        nx = CX + side * 96
        o.append(f'<rect x="{nx - 26}" y="{y - CH / 2}" width="52" height="{CH}" rx="7" fill="#fff" stroke="#d1d5db"/>'
                 f'<text x="{nx}" y="{y + 5}" text-anchor="middle" font-size="14" font-weight="600" fill="#374151">#{n}</text>')
        x = nx + side * 34
        for text, kind in PINS[n]:
            s, far = chip(x, y, text, kind, "l" if odd else "r")
            o.append(s)
            x = far + side * 8
    # legend and notes
    ly = ROW0 + 20 * STEP + 6
    o.append(f'<line x1="60" y1="{ly}" x2="{W - 60}" y2="{ly}" stroke="#e5e7eb"/>')
    total = sum(chip_w(t) + 14 for _, t in LEGEND) - 14
    x = (W - total) / 2
    for kind, text in LEGEND:
        s, far = chip(x, ly + 34, text, kind, "l")
        o.append(s)
        x = far + 14
    for i, line in enumerate(NOTES):
        o.append(f'<text x="{CX}" y="{ly + 84 + i * 22}" text-anchor="middle" font-size="13" fill="#4b5563">{line}</text>')
    o.append("</svg>\n")
    with open(OUT, "w") as f:
        f.write("\n".join(o))
    print("wrote", os.path.relpath(OUT))


if __name__ == "__main__":
    main()
