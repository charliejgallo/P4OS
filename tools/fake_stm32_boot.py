#!/usr/bin/env python3
"""
fake_stm32_boot.py - an STM32's system bootloader (ST's AN3155) on a pty, to
test the Programador's STM32 family (components/aos_flasher/aos_flasher_stm32.c)
without a board.

    tools/fake_stm32_boot.py serve [--chip f103|f411|g071] [--lines lines.txt]
                                   [--dump flash.bin] [--legacy-erase] [--protected]
    tools/fake_stm32_boot.py check --dump flash.bin FILE.hex|FILE.bin

serve opens a pty and prints its path; run the simulator with
P4_SIM_UART_B=<that path> and P4_SIM_UART_B_LINES=<the --lines file>: the
port's EN is NRST and its BOOT is BOOT0, as wired on the bench. A reset (EN
low and back) with BOOT0 high starts the bootloader; with BOOT0 low, the
"program", which says a line now and then at 115200 8N1. Without --lines it
is always the bootloader.

The bootloader wants even parity (it reads the pty's termios: 8N1 is noise
to it, as on the wire) and takes its speed from the first 0x7F, answered
ACK; a second 0x7F gets NACK, as a real one. Then: GET, GET VERSION, GET ID,
READ MEMORY (flash, the flash-size register, nothing else), WRITE MEMORY
(NOR: only clears bits; an unerased spot fails the check later), ERASE
(0x43, with --legacy-erase) or EXTENDED ERASE (0x44): pages by number - an
F411's sectors of 16/64/128 KB - or all; GO; READOUT UNPROTECT. With
--protected, READ and WRITE are NACKed until READOUT UNPROTECT erases all.

    chip   PID    flash   erase unit
    f103   0x410  64 KB   1 KB pages
    f411   0x431  512 KB  sectors 4 x 16, 64, 3 x 128 KB
    g071   0x460  128 KB  2 KB pages

check compares a .hex (or a .bin, from 0x08000000) with the dump.
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

ACK, NACK = 0x79, 0x1F
BASE = 0x08000000
CHIPS = {
    # pid, size, size register, unit table (start, size) by number
    "f103": (0x410, 64 << 10, 0x1FFFF7E0, [(i * 1024, 1024) for i in range(64)]),
    "f411": (0x431, 512 << 10, 0x1FFF7A22,
             [(i * 0x4000, 0x4000) for i in range(4)] + [(0x10000, 0x10000)] + [(0x20000 + i * 0x20000, 0x20000) for i in range(3)]),
    "g071": (0x460, 128 << 10, 0x1FFF75E0, [(i * 2048, 2048) for i in range(64)]),
}


def log(*a):
    print("[fake-stm32]", *a, file=sys.stderr, flush=True)


class Chip:
    def __init__(self, a, master, slave):
        self.a, self.master, self.slave = a, master, slave
        self.pid, self.size, self.size_reg, self.units = CHIPS[a.chip]
        rnd = random.Random(99)
        self.flash = bytearray(rnd.randbytes(self.size // 4)) + b"\xff" * (self.size - self.size // 4)
        self.protected = a.protected
        self.rx = bytearray()
        self.in_boot = not a.lines
        self.synced = False
        self.en, self.boot = 1, 0
        self.lines_pos = 0
        self.counts = {}
        self.next_line = 0.0
        self.state = None           # what the next bytes are for
        self.need = 0

    def count(self, k):
        self.counts[k] = self.counts.get(k, 0) + 1

    def send(self, data):
        while data:
            try:
                data = data[os.write(self.master, data):]
            except BlockingIOError:
                time.sleep(0.001)

    def even_parity(self):
        try:
            cf = termios.tcgetattr(self.slave)[2]
        except termios.error:
            return False
        return bool(cf & termios.PARENB) and not (cf & termios.PARODD)

    # -- the protocol: a small state machine over the bytes ------------------

    def feed(self, data):
        if not self.in_boot:
            return
        if not self.even_parity():
            self.count("noise")
            return
        self.rx += data
        while self.step():
            pass

    def take(self, n):
        if len(self.rx) < n:
            return None
        b = bytes(self.rx[:n])
        del self.rx[:n]
        return b

    def step(self):
        if self.state is None:
            if not self.synced:
                b = self.take(1)
                if b is None:
                    return False
                if b[0] == 0x7F:
                    self.synced = True
                    self.count("sync")
                    self.send(bytes([ACK]))
                return True
            m = self.take(2)
            if m is None:
                return False
            c, nc = m
            if c == 0x7F and nc != 0x7F:
                self.rx[:0] = bytes([nc])
                self.send(bytes([NACK]))    # already synced
                return True
            if c ^ nc != 0xFF:
                self.send(bytes([NACK]))
                return True
            return self.command(c)
        return self.state()

    def command(self, c):
        self.count(f"cmd_{c:02x}")
        if c == 0x00:
            cmds = [0x00, 0x01, 0x02, 0x11, 0x21, 0x31, 0x43 if self.a.legacy_erase else 0x44, 0x63, 0x73, 0x82, 0x92]
            self.send(bytes([ACK, len(cmds), 0x31] + cmds + [ACK]))
        elif c == 0x01:
            self.send(bytes([ACK, 0x31, 0, 0, ACK]))
        elif c == 0x02:
            self.send(bytes([ACK, 1, self.pid >> 8, self.pid & 255, ACK]))
        elif c in (0x11, 0x21, 0x31):
            if self.protected and c != 0x21:
                self.send(bytes([NACK]))
                return True
            self.send(bytes([ACK]))
            self.cur = c
            self.state = self.st_addr
        elif c in (0x43, 0x44):
            if (c == 0x44) == self.a.legacy_erase or self.protected:
                self.send(bytes([NACK]))
                return True
            self.send(bytes([ACK]))
            self.state = self.st_erase43 if c == 0x43 else self.st_erase44
        elif c == 0x92:
            self.send(bytes([ACK]))
            self.flash[:] = b"\xff" * self.size
            self.protected = False
            self.send(bytes([ACK]))
            self.synced = False          # it restarts
        else:
            self.send(bytes([NACK]))
        return True

    def st_addr(self):
        m = self.take(5)
        if m is None:
            return False
        if m[0] ^ m[1] ^ m[2] ^ m[3] != m[4]:
            self.send(bytes([NACK]))
            self.state = None
            return True
        self.addr = int.from_bytes(m[:4], "big")
        ok = (BASE <= self.addr < BASE + self.size) or (self.cur == 0x11 and self.addr in (self.size_reg,))
        if not ok:
            self.send(bytes([NACK]))
            self.state = None
            return True
        self.send(bytes([ACK]))
        if self.cur == 0x21:
            self.count("go")
            self.state = None
            self.run_program()
        else:
            self.state = self.st_read_n if self.cur == 0x11 else self.st_write
        return True

    def st_read_n(self):
        m = self.take(2)
        if m is None:
            return False
        n = m[0] + 1
        if m[0] ^ m[1] != 0xFF:
            self.send(bytes([NACK]))
        elif self.addr == self.size_reg:
            self.send(bytes([ACK]) + ((self.size >> 10).to_bytes(2, "little") + b"\xff\xff")[:n])
        else:
            o = self.addr - BASE
            self.send(bytes([ACK]) + bytes(self.flash[o:o + n]))
        self.state = None
        return True

    def st_write(self):
        if not self.rx:
            return False
        n = self.rx[0] + 1
        m = self.take(n + 2)
        if m is None:
            return False
        x = 0
        for v in m[:-1]:
            x ^= v
        if x != m[-1] or n % 4:
            self.send(bytes([NACK]))
        else:
            o = self.addr - BASE
            for i, v in enumerate(m[1:-1]):
                self.flash[o + i] &= v
            self.count("write")
            time.sleep(0.002)
            self.send(bytes([ACK]))
        self.state = None
        return True

    def erase_units(self, nums):
        for k in nums:
            if k >= len(self.units):
                return False
        for k in nums:
            st, sz = self.units[k]
            self.flash[st:st + sz] = b"\xff" * sz
        self.count("erased_units")
        return True

    def st_erase43(self):
        if not self.rx:
            return False
        if self.rx[0] == 0xFF:
            m = self.take(2)
            if m is None:
                return False
            self.flash[:] = b"\xff" * self.size
            self.count("mass_erase")
            self.send(bytes([ACK if m[1] == 0x00 else NACK]))
        else:
            n = self.rx[0] + 1
            m = self.take(n + 2)
            if m is None:
                return False
            x = 0
            for v in m[:-1]:
                x ^= v
            self.send(bytes([ACK if x == m[-1] and self.erase_units(list(m[1:-1])) else NACK]))
        self.state = None
        return True

    def st_erase44(self):
        if len(self.rx) < 2:
            return False
        if self.rx[0] == 0xFF and self.rx[1] == 0xFF:
            m = self.take(3)
            if m is None:
                return False
            self.flash[:] = b"\xff" * self.size
            self.count("mass_erase")
            self.send(bytes([ACK if m[2] == 0x00 else NACK]))
        else:
            n = (self.rx[0] << 8 | self.rx[1]) + 1
            m = self.take(2 + 2 * n + 1)
            if m is None:
                return False
            x = 0
            for v in m[:-1]:
                x ^= v
            nums = [m[2 + 2 * i] << 8 | m[3 + 2 * i] for i in range(n)]
            self.send(bytes([ACK if x == m[-1] and self.erase_units(nums) else NACK]))
        self.state = None
        return True

    # -- resets and the program ----------------------------------------------

    def run_program(self):
        self.dump()
        if self.a.lines:
            self.in_boot = False
            self.next_line = time.monotonic() + 0.3

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
            en, boot = int(kv.get("en", self.en)), int(kv.get("boot", self.boot))
            if self.en == 0 and en == 1:
                self.count("reset_boot" if boot else "reset_run")
                self.rx, self.state, self.synced = bytearray(), None, False
                if boot:
                    self.in_boot = True
                else:
                    self.run_program()
            self.en, self.boot = en, boot

    def tick(self):
        if not self.in_boot and time.monotonic() > self.next_line:
            self.next_line = time.monotonic() + 1.0
            self.send(b"programa corriendo\r\n")

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
    c = Chip(a, master, slave)
    print(f"PTY {path}", flush=True)
    print(f"ENV P4_SIM_UART_B={path}" + (f" P4_SIM_UART_B_LINES={a.lines}" if a.lines else ""), flush=True)
    log(f"{a.chip} (PID 0x{c.pid:03x}, {c.size >> 10} KB)" + (", protected" if a.protected else ""))

    def bye(*_):
        c.dump()
        log(f"commands: {c.counts}")
        sys.exit(0)
    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    while True:
        c.poll_lines()
        r, _, _ = select.select([master], [], [], 0.003)
        if r:
            try:
                data = os.read(master, 65536)
            except (BlockingIOError, OSError):
                data = b""
            if data:
                c.feed(data)
        c.tick()


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
        elif t == 4:
            upper = (r[4] << 8 | r[5]) << 16
    return out


def check(a):
    flash = open(a.dump, "rb").read()
    if a.file.lower().endswith(".hex"):
        want = read_hex(a.file)
    else:
        want = {BASE + i: v for i, v in enumerate(open(a.file, "rb").read())}
    bad = [x for x, v in want.items() if not (0 <= x - BASE < len(flash)) or flash[x - BASE] != v]
    if bad:
        print(f"{len(bad)} of {len(want)} bytes differ, first at 0x{min(bad):x}")
        return 1
    print(f"all {len(want)} bytes in flash")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--chip", default="f103", choices=sorted(CHIPS))
    s.add_argument("--lines")
    s.add_argument("--dump")
    s.add_argument("--legacy-erase", action="store_true")
    s.add_argument("--protected", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("--dump", required=True)
    c.add_argument("file")
    a = p.parse_args()
    sys.exit(serve(a) if a.cmd == "serve" else check(a))


if __name__ == "__main__":
    main()
