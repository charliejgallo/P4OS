#!/usr/bin/env python3
"""
fake_avr_boot.py - an Arduino's bootloader on a pty, to test the Programador's
AVR family (components/aos_flasher/aos_flasher_avr.c) without a board.

    tools/fake_avr_boot.py serve [--boot optiboot|atmegaboot|mega|caterina]
                                 [--chip atmega328p] [--lines lines.txt]
                                 [--dump flash.bin] [--hold]
    tools/fake_avr_boot.py check --dump flash.bin FILE.hex|FILE.bin [--at 0x0]

serve opens a pty and prints its path; run the simulator with
P4_SIM_UART_B=<that path> and P4_SIM_UART_B_LINES=<the --lines file>, so the
board's EN line (wired to RESET, as on the bench) reaches it. The bootloaders:

    optiboot    STK500 v1 at 115200, 512 bytes at the top (an Uno, a Nano)
    atmegaboot  STK500 v1 at 57600, 2 KB (an old Nano): it says 1.16
    mega        STK500 v2 at 115200, 8 KB, signs on as "AVRISP_2" (a Mega 2560)
    caterina    AVR109 at 57600, 4 KB (a Leonardo's, over its USB)

The speed is the pty's own (the simulator sets it with tcsetattr; both ends
see one termios): at the wrong one the bootloader hears noise and says
nothing back, as a real one. After a reset - EN going low and back - the
bootloader listens for a second (eight for Caterina; every command it
understands starts the second again), then "runs the sketch", which prints a
line every half second with the CRC of the flash. --hold keeps it in the
bootloader for good (without --lines it always is).

The flash starts with a made-up old sketch and the bootloader's bytes at the
top. A page write erases and writes that page; one into the bootloader's
room is refused (logged as BOOT WRITE, counted in the summary), as the real
ones cannot rewrite themselves. --dump writes the flash after every leave and
at exit; check compares a .hex (or a .bin at --at) with it, byte by byte,
and exits 0 when every byte of the file is in the flash.
"""
import argparse
import os
import pty
import random
import select
import signal
import sys
import termios
import time
import tty
import zlib

CHIPS = {
    "atmega328p": (b"\x1e\x95\x0f", 32768, 128),
    "atmega168": (b"\x1e\x94\x06", 16384, 128),
    "atmega2560": (b"\x1e\x98\x01", 262144, 256),
    "atmega32u4": (b"\x1e\x95\x87", 32768, 128),
}
BOOTS = {
    # name: (protocol, baud, room, listen seconds, default chip)
    "optiboot": ("stk1", 115200, 512, 1.0, "atmega328p"),
    "atmegaboot": ("stk1", 57600, 2048, 1.0, "atmega328p"),
    "mega": ("stk2", 115200, 8192, 1.0, "atmega2560"),
    "caterina": ("avr109", 57600, 4096, 8.0, "atmega32u4"),
}


def log(*a):
    print("[fake-avr]", *a, file=sys.stderr, flush=True)


class Board:
    def __init__(self, a, master, slave):
        self.a, self.master, self.slave = a, master, slave
        self.proto, self.baud, self.room, self.listen, chip = BOOTS[a.boot]
        self.sig, self.size, self.page = CHIPS[a.chip or chip]
        rnd = random.Random(1234)
        self.flash = bytearray(rnd.randbytes(self.size // 3)) + b"\xff" * (self.size - self.size // 3)
        boot = bytes((i * 7 + 0x5A) & 0xFF for i in range(self.room))
        self.flash[self.size - self.room:] = boot
        self.boot_writes = 0
        self.counts = {}
        self.rx = bytearray()
        self.addr = 0               # bytes
        self.en = 1
        self.lines_pos = 0
        self.in_boot = not a.lines
        self.until = 0.0
        self.next_beat = 0.0
        self.seq = 0

    # -- the wire -------------------------------------------------------------

    def host_baud(self):
        try:
            return termios.tcgetattr(self.slave)[4]
        except termios.error:
            return 0

    def send(self, data):
        while data:
            try:
                n = os.write(self.master, data)
                data = data[n:]
            except BlockingIOError:
                time.sleep(0.001)

    def count(self, name):
        self.counts[name] = self.counts.get(name, 0) + 1

    def feed(self, data):
        if not self.in_boot:
            return                              # the sketch does not read
        if self.host_baud() != self.baud:
            self.count("noise")                 # wrong speed: garbage, no answer
            return
        self.rx += data
        while True:
            n = {"stk1": self.stk1, "stk2": self.stk2, "avr109": self.avr109}[self.proto]()
            if not n:
                break
            del self.rx[:n]
            self.until = time.monotonic() + self.listen

    # -- STK500 v1 ------------------------------------------------------------

    def stk1(self):
        b = self.rx
        if not b:
            return 0
        need = {0x30: 2, 0x41: 3, 0x42: 22, 0x45: 7, 0x50: 2, 0x51: 2, 0x55: 4, 0x56: 6, 0x75: 2}
        c = b[0]
        if c in (0x64, 0x74):
            if len(b) < 4:
                return 0
            n = b[1] << 8 | b[2]
            total = 4 + (n if c == 0x64 else 0) + 1
        elif c in need:
            total = need[c]
        else:
            self.count("junk")
            return 1                            # not a command: skipped
        if len(b) < total:
            return 0
        if b[total - 1] != 0x20:
            self.count("bad_eop")
            return 1
        ok, out = b"\x14", b"\x10"
        if c == 0x30:
            self.count("sync")
            self.send(ok + out)
        elif c == 0x41:
            p = b[1]
            v = {0x81: 8 if self.a.boot == "optiboot" else 1, 0x82: 0 if self.a.boot == "optiboot" else 16}.get(p, 3)
            self.send(ok + bytes([v]) + out)
        elif c in (0x42, 0x45, 0x50):
            self.count({0x42: "set_device", 0x45: "set_device_ext", 0x50: "enter"}[c])
            self.send(ok + out)
        elif c == 0x51:
            self.count("leave")
            self.send(ok + out)
            self.run_sketch()
        elif c == 0x55:
            self.addr = (b[1] | b[2] << 8) * 2
            self.send(ok + out)
        elif c == 0x56:
            self.send(ok + b"\x00" + out)
        elif c == 0x75:
            self.send(ok + self.sig + out)
        elif c == 0x64:
            n = b[1] << 8 | b[2]
            self.program(self.addr, bytes(b[4:4 + n]))
            time.sleep(0.004)
            self.send(ok + out)
        elif c == 0x74:
            n = b[1] << 8 | b[2]
            self.count("read")
            self.send(ok + bytes(self.flash[self.addr:self.addr + n]) + out)
        return total

    # -- STK500 v2 ------------------------------------------------------------

    def stk2(self):
        b = self.rx
        i = b.find(0x1B)
        if i < 0:
            return len(b)
        if i:
            return i
        if len(b) < 5:
            return 0
        n = b[2] << 8 | b[3]
        if b[4] != 0x0E or n > 300:
            return 1
        if len(b) < 6 + n:
            return 0
        x = 0
        for v in b[:5 + n]:
            x ^= v
        if x != b[5 + n]:
            self.count("bad_checksum")
            return 6 + n
        seq, body = b[1], bytes(b[5:5 + n])
        ans = self.v2(body)
        h = bytes([0x1B, seq, len(ans) >> 8, len(ans) & 255, 0x0E]) + ans
        x = 0
        for v in h:
            x ^= v
        self.send(h + bytes([x]))
        if body[0] == 0x11:
            self.run_sketch()
        return 6 + n

    def v2(self, m):
        c = m[0]
        if c == 0x01:
            self.count("sign_on")
            return b"\x01\x00\x08AVRISP_2"
        if c == 0x03:
            return bytes([0x03, 0x00, {0x91: 2, 0x92: 10}.get(m[1], 0)])
        if c in (0x02, 0x10, 0x12):
            self.count({0x02: "set_param", 0x10: "enter", 0x12: "chip_erase"}[c])
            if c == 0x12:
                self.flash[:self.size - self.room] = b"\xff" * (self.size - self.room)
            return bytes([c, 0x00])
        if c == 0x11:
            self.count("leave")
            return b"\x11\x00"
        if c == 0x06:
            w = (m[1] << 24 | m[2] << 16 | m[3] << 8 | m[4]) & 0x7FFFFFFF
            self.addr = w * 2
            return b"\x06\x00"
        if c == 0x13:
            n = m[1] << 8 | m[2]
            self.program(self.addr, m[10:10 + n])
            time.sleep(0.004)
            return b"\x13\x00"
        if c == 0x14:
            n = m[1] << 8 | m[2]
            self.count("read")
            d = bytes(self.flash[self.addr:self.addr + n])
            self.addr += n
            return b"\x14\x00" + d + b"\x00"
        if c == 0x1B:
            return bytes([0x1B, 0x00, self.sig[m[4] % 3], 0x00])
        self.count(f"unknown_{c:02x}")
        return bytes([c, 0xC0])

    # -- AVR109 ---------------------------------------------------------------

    def avr109(self):
        b = self.rx
        if not b:
            return 0
        c = chr(b[0])
        cr = b"\r"
        if c in "ABgT":
            need = {"A": 3, "T": 2, "g": 4, "B": 4}[c]
            if len(b) < need:
                return 0
            if c == "B":
                n = b[1] << 8 | b[2]
                if len(b) < 4 + n:
                    return 0
                self.program(self.addr, bytes(b[4:4 + n]))
                self.addr += n
                time.sleep(0.004)
                self.send(cr)
                return 4 + n
            if c == "A":
                self.addr = (b[1] << 8 | b[2]) * 2
                self.send(cr)
            elif c == "g":
                n = b[1] << 8 | b[2]
                self.count("read")
                self.send(bytes(self.flash[self.addr:self.addr + n]))
                self.addr += n
            else:
                self.send(cr)
            return need
        if c == "S":
            self.count("hello")
            self.send(b"CATERIN")
        elif c == "p":
            self.send(b"S")
        elif c == "V":
            self.send(b"10")
        elif c == "a":
            self.send(b"Y")
        elif c == "b":
            self.send(b"Y" + bytes([0, self.page]))
        elif c == "t":
            self.send(b"\x44\x00")
        elif c == "s":
            self.send(self.sig[::-1])
        elif c in "PL":
            self.count({"P": "enter", "L": "leave_prog"}[c])
            self.send(cr)
        elif c == "e":
            self.count("chip_erase")
            self.flash[:self.size - self.room] = b"\xff" * (self.size - self.room)
            self.send(cr)
        elif c == "E":
            self.count("leave")
            self.send(cr)
            self.run_sketch()
        else:
            self.count("junk")
        return 1

    # -- the flash and the sketch --------------------------------------------

    def program(self, addr, data):
        self.count("write")
        if addr % self.page or len(data) != self.page:
            log(f"odd page write: 0x{addr:x} {len(data)} bytes")
        if addr + len(data) > self.size - self.room:
            self.boot_writes += 1
            log(f"BOOT WRITE at 0x{addr:x}: refused")
            return
        self.flash[addr:addr + len(data)] = data

    def run_sketch(self):
        self.dump()
        if not self.a.hold and self.a.lines:
            self.in_boot = False
            self.next_beat = time.monotonic() + 0.3

    def reset(self):
        self.in_boot = True
        self.rx = bytearray()
        self.until = time.monotonic() + self.listen

    def poll_lines(self):
        if not self.a.lines:
            return
        try:
            with open(self.a.lines) as f:
                f.seek(self.lines_pos)
                chunk = f.read()
                self.lines_pos = f.tell()
        except FileNotFoundError:
            return
        for line in chunk.splitlines():
            kv = dict(x.split("=") for x in line.split()[1:] if "=" in x)
            en = int(kv.get("en", self.en))
            if self.en == 0 and en == 1:
                self.count("reset")
                self.reset()
            self.en = en

    def tick(self):
        now = time.monotonic()
        if self.in_boot and self.a.lines and not self.a.hold and now > self.until:
            self.run_sketch()
        if not self.in_boot and self.en and now > self.next_beat:
            self.next_beat = now + 0.5
            crc = zlib.crc32(bytes(self.flash[:self.size - self.room]))
            if self.host_baud() in (9600, 115200):
                self.send(f"sketch running, flash crc {crc:08x}\r\n".encode())

    def dump(self):
        if self.a.dump:
            with open(self.a.dump, "wb") as f:
                f.write(self.flash)


def serve(a):
    master, slave = pty.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    path = os.ttyname(slave)
    if a.lines:
        open(a.lines, "w").close()
    b = Board(a, master, slave)
    print(f"PTY {path}", flush=True)
    env = f"P4_SIM_UART_B={path}" + (f" P4_SIM_UART_B_LINES={a.lines}" if a.lines else "")
    print(f"ENV {env}", flush=True)
    log(f"{a.boot} ({b.proto}, {b.baud}) on {a.chip or BOOTS[a.boot][4]}, {b.size // 1024} KB, "
        + ("RESET from " + a.lines if a.lines else "always in the bootloader"))

    def bye(*_):
        b.dump()
        log(f"commands: {b.counts}")
        if b.boot_writes:
            log(f"BOOT WRITES: {b.boot_writes}")
        sys.exit(0)
    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    while True:
        b.poll_lines()
        r, _, _ = select.select([master], [], [], 0.003)
        if r:
            try:
                data = os.read(master, 65536)
            except (BlockingIOError, OSError):
                data = b""
            if data:
                b.feed(data)
        b.tick()


def read_hex(path):
    out, upper = {}, 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(":"):
            continue
        r = bytes.fromhex(line[1:])
        n, addr, t = r[0], r[1] << 8 | r[2], r[3]
        if t == 0:
            for i in range(n):
                out[upper + addr + i] = r[4 + i]
        elif t == 2:
            upper = (r[4] << 8 | r[5]) << 4
        elif t == 4:
            upper = (r[4] << 8 | r[5]) << 16
    return out


def check(a):
    flash = open(a.dump, "rb").read()
    if a.file.lower().endswith((".hex", ".ihex")):
        want = read_hex(a.file)
    else:
        at = int(a.at, 0)
        want = {at + i: v for i, v in enumerate(open(a.file, "rb").read())}
    bad = [x for x, v in want.items() if x >= len(flash) or flash[x] != v]
    if bad:
        print(f"{len(bad)} of {len(want)} bytes differ, first at 0x{min(bad):x}")
        return 1
    print(f"all {len(want)} bytes in flash")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--boot", default="optiboot", choices=sorted(BOOTS))
    s.add_argument("--chip", choices=sorted(CHIPS))
    s.add_argument("--lines")
    s.add_argument("--dump")
    s.add_argument("--hold", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("--dump", required=True)
    c.add_argument("--at", default="0")
    c.add_argument("file")
    a = p.parse_args()
    sys.exit(serve(a) if a.cmd == "serve" else check(a))


if __name__ == "__main__":
    main()
