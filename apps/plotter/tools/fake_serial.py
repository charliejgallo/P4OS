#!/usr/bin/env python3
"""
fake_serial.py - a device that prints numbers, on a pty, to try the Monitor
serie app (apps/plotter) in the simulator without hardware.

    apps/plotter/tools/fake_serial.py [mode] [--rate N] [--seconds S]

It opens a pty, prints its path and the line to give the simulator:

    P4_SIM_UART_B=/dev/ttys012 ./sim/build-plotter/p4os_sim

(the Monitor's channel A reads "uart.b" unless told otherwise). Modes:

  sine      two plain columns, "0.951 0.309": a sine and a cosine, 10/s
  arduino   the Arduino IDE's plotter, "temp:21.53,hum:40.2,dew:9.6",
            with a board's boot log in front and now and then a line with
            no numbers, 5/s
  csv       a CSV with a header, "time;volts;amps" and decimal commas
            ("12:00:01;3,312;0,154"), the header again every 200 lines, 4/s
  meter     a multimeter's "+0265 4": four digits and a code that says
            where the point goes (0 none, 1 X.XXX, 2 XX.XX, 4 XXX.X),
            changing range now and then, 2/s (the user's meter at 2400 8N1
            does about that)
  fast      what a 115200 line carries at full tilt: "n:123,a:0.12,b:3.4,
            c:-1.0" as fast as --rate (800/s by default), with n counting up
            so a gap shows: the app must not lose any
  mix       sine, arduino, csv and meter in turn, 20 s each

A pty keeps what nobody read and hands it over at once when the simulator
opens it: --delay S stays quiet S seconds first, so a fast mode starts the
way a real line does, with nothing backed up.

Every line that comes in is answered with "OK <line>", to try Enviar.
"""
import argparse
import math
import os
import pty
import random
import select
import sys
import time
import tty


def log(*a):
    print("[fake-serial]", *a, file=sys.stderr, flush=True)


BOOT = [
    "ets Jun  8 2016 00:22:57",
    "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)",
    "configsip: 0, SPIWP:0xee",
    "load:0x3fff0030,len:1344",
    "entry 0x400805f0",
    "Sensor ready",
]


class Device:
    def __init__(self, mode, rate):
        self.mode = mode
        self.rate = rate
        self.n = 0
        self.t0 = time.time()
        self.boot = list(BOOT) if mode in ("arduino", "mix") else []
        self.range = 4
        self.reading = 265.0

    def line(self, mode, t):
        self.n += 1
        if mode == "sine":
            return f"{math.sin(t * 2 * math.pi / 8):.3f} {math.cos(t * 2 * math.pi / 8):.3f}"
        if mode == "arduino":
            if self.boot:
                return self.boot.pop(0)
            if self.n % 37 == 0:
                return "WiFi: still trying..."
            temp = 21.5 + 1.8 * math.sin(t / 9) + random.uniform(-0.08, 0.08)
            hum = 40 + 6 * math.sin(t / 13 + 1) + random.uniform(-0.3, 0.3)
            dew = temp - (100 - hum) / 5
            return f"temp:{temp:.2f},hum:{hum:.1f},dew:{dew:.1f}"
        if mode == "csv":
            if self.n % 200 == 1:
                return "time;volts;amps"
            lt = time.localtime()
            v = 3.3 - 0.4 * (1 - math.exp(-((t % 60) / 15))) + random.uniform(-0.004, 0.004)
            a = 0.15 + 0.05 * math.sin(t / 3) + random.uniform(-0.002, 0.002)
            return f"{lt.tm_hour:02d}:{lt.tm_min:02d}:{lt.tm_sec:02d};{v:.3f}".replace(".", ",") + f";{a:.3f}".replace(".", ",")
        if mode == "meter":
            # a voltage that wanders, shown as the meter would: 4 digits,
            # the point's code after them
            self.reading = 26.5 + 2.2 * math.sin(t / 7) + random.uniform(-0.05, 0.05)
            if int(t / 30) % 3 == 2:
                v, code, dec = self.reading / 10, 1, 3      # the 2 V range: X.XXX
            else:
                v, code, dec = self.reading, 4, 1           # XXX.X
            digits = int(round(abs(v) * 10 ** dec))
            return f"{'+' if v >= 0 else '-'}{digits:04d} {code}"
        if mode == "fast":
            return f"n:{self.n},a:{math.sin(t * 7):.3f},b:{3 + math.sin(t):.2f},c:{random.uniform(-1, 1):.2f}"
        return ""

    def mode_at(self, t):
        if self.mode != "mix":
            return self.mode
        return ("sine", "arduino", "csv", "meter")[int(t / 20) % 4]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", nargs="?", default="mix", choices=["sine", "arduino", "csv", "meter", "fast", "mix"])
    ap.add_argument("--rate", type=float, default=0, help="lines a second (0: the mode's own)")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0: never)")
    ap.add_argument("--delay", type=float, default=0, help="silent this long first (a pty keeps what nobody read)")
    a = ap.parse_args()
    rates = {"sine": 10, "arduino": 5, "csv": 4, "meter": 2, "fast": 800, "mix": 5}
    rate = a.rate or rates[a.mode]

    master, slave = pty.openpty()
    tty.setraw(slave)
    path = os.ttyname(slave)
    print(f"PTY {path}", flush=True)
    print(f"ENV P4_SIM_UART_B={path}", flush=True)
    log(f"{a.mode} at {rate}/s")
    dev = Device(a.mode, rate)
    time.sleep(a.delay)
    start = time.time()
    next_t = start
    inbuf = b""
    sent = 0
    while True:
        now = time.time()
        if a.seconds and now - start > a.seconds:
            break
        wait = max(0.0, next_t - now)
        r, _, _ = select.select([master], [], [], min(wait, 0.05))
        if r:
            try:
                data = os.read(master, 4096)
            except OSError:
                data = b""
            inbuf += data
            while b"\n" in inbuf or b"\r" in inbuf:
                cut = min(i for i in (inbuf.find(b"\n"), inbuf.find(b"\r")) if i >= 0)
                got, inbuf = inbuf[:cut], inbuf[cut + 1:]
                if got:
                    log("got", got)
                    os.write(master, b"OK " + got + b"\r\n")
        now = time.time()
        # catch up in a burst, as a fast device's driver would
        burst = 0
        while next_t <= now and burst < 200:
            t = next_t - start
            text = dev.line(dev.mode_at(t), t)
            if text:
                os.write(master, (text + "\r\n").encode())
                sent += 1
            next_t += 1.0 / rate
            burst += 1
    log(f"{sent} lines sent")


if __name__ == "__main__":
    main()
