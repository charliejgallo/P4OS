#!/usr/bin/env python3
"""
fake_uboot.py - a HiSilicon IP camera's U-Boot on a pty, to test the
Programador's "U-Boot por consola" family (components/aos_flasher/
aos_flasher_uboot.c) without a camera.

    tools/fake_uboot.py serve [--flash-mb 16] [--bootdelay 1] [--no-crc]
                              [--locked] [--noise 0.0005] [--max-baud 921600]
                              [--flash-out flash.bin]

serve opens a pty and prints its path; run the simulator with
P4_SIM_UART_B=<that path>. It plays U-Boot 2010.06 as HiSilicon ships it on a
Hi3518EV200: nothing until the first byte comes in (the user powering the
camera on), then its banner, the SPI NOR it found, and "Hit any key to stop
autoboot" with a countdown of --bootdelay seconds. One key stops it at the
"hisilicon # " prompt, --stop-key ("q" by default), and nothing else: the
user's SC10W says "Hit any key" and lets Ctrl+C and Enter by, stopping on
one of space, q, x or Esc (2026-11-05); --any-key takes any byte;
otherwise it boots "Linux" and ends at a login prompt, which answers each
Enter with another. --locked: bootdelay 0 and no key stops it (a closed
console). Like that camera, it has no bdinfo (--bdinfo gives it one) and
its environment in flash fails its CRC ("using default environment").

At the prompt, every byte is echoed, as on the real one, and these work:
version, printenv, bdinfo, sf probe 0, sf read <ram> <off> <len>, md / md.b
/ md.w / md.l <addr> [count] (U-Boot takes the numbers in hex, with or
without 0x), crc32 <addr> <len> (gone with --no-crc: "Unknown command"),
setenv (setenv baudrate N says "Switch baudrate ... and press ENTER", moves
to N and waits for a CR at N), reset, and Ctrl+C ("<INTERRUPT>"). The speed
is the pty's own (the simulator sets it with tcsetattr; both ends see one
termios): at a speed other than the console's, what it sends comes out as
garbage and what it hears is ignored. Above --max-baud the switch is refused
("not supported"), as a U-Boot whose table lacks that speed.

--writable lets it be written, to try the camera's install scripts
(camara-sc10w/tutorial: instalar.py) without the camera: --sd DIR is its
microSD (mmcinfo, fatls mmc 0, fatload mmc 0 <ram> <file>), and mw.b, sf
erase and sf write do what they say. After a reset, a flash that no longer
starts with the factory U-Boot boots as OpenIPC's ("OpenIPC # ", any key
stops it): Linux panics for want of its rootfs and the camera restarts,
until "run setnor16m" saves the 16 MB layout; then it ends at OpenIPC's
login. Without --writable, anything that writes is refused, as below.
kill -USR1 <its pid> unplugs it: the next byte that comes in powers it on.

--noise P garbles each byte of md's output with probability P, to exercise
the Programador's per-chunk CRC and its retries.

The flash is made up, laid out as the SC10W's (mtdparts in bootargs):
512K boot (U-Boot), 2560K kernel (a uImage at 0x80000), 12800K rootfs (a
jffs2 at 0x300000, mostly erased) and 512K config - most of it 0xFF. --flash-out
writes it at start, to compare with a dump. Anything that would write
(saveenv, sf erase, sf write, mw, tftp) is refused, logged and counted: the
summary on exit says how many there were, which must be none.
"""
import argparse
import os
import pty
import random
import select
import signal
import struct
import sys
import termios
import time
import tty
import zlib

PROMPT = b"hisilicon # "
VERSION = "U-Boot 2010.06-dirty (Feb 27 2016 - 14:46:03)"
RAM_BASE, RAM_SIZE = 0x80000000, 64 << 20


def log(*a):
    print("[fake-uboot]", *a, file=sys.stderr, flush=True)


def make_flash(mb):
    size = mb << 20
    rnd = random.Random(3518)
    f = bytearray(b"\xff" * size)
    boot = bytearray(rnd.randbytes(0x30000))
    boot[0x100:0x100 + len(VERSION)] = VERSION.encode()
    f[0:len(boot)] = boot
    kernel = bytearray(rnd.randbytes(0x21B428))     # 2208808 bytes, as the camera's
    kernel[0:4] = b"\x27\x05\x19\x56"               # a uImage's magic
    kernel[0x20:0x20 + 16] = b"Linux-3.4.35\0\0\0\0"
    f[0x80000:0x80000 + len(kernel)] = kernel
    root = 0x300000
    for i in range(0, 0x600000, 0x2000):            # jffs2 nodes in the first 6 MB, the rest erased
        f[root + i:root + i + 2] = b"\x85\x19"
        f[root + i + 12:root + i + 0x1800] = rnd.randbytes(0x1800 - 12)
    cfg = size - 0x80000
    f[cfg:cfg + 0x4000] = rnd.randbytes(0x4000)
    return f


def ENV(mb):
    root = (mb << 10) - 512 - 2560 - 512
    return [
        "bootargs=mem=40M console=ttyAMA0,115200 root=/dev/mtdblock2 rootfstype=jffs2 "
        f"mtdparts=hi_sfc:512K(boot),2560K(kernel),{root}K(rootfs),512K(config) init=/linuxrc",
        "bootcmd=sf probe 0;sf read 0x82000000 0x80000 0x280000;bootm 0x82000000",
        "bootdelay=1", "baudrate=115200", "ethaddr=00:00:23:34:45:66", "ipaddr=192.168.1.10",
        "serverip=192.168.1.2", "netmask=255.255.255.0", "bootfile=uImage",
        "stdin=serial", "stdout=serial", "stderr=serial", "verify=n", "ver=" + VERSION,
    ]


class Camera:
    def __init__(self, a, master, slave):
        self.a, self.master, self.slave = a, master, slave
        self.flash = make_flash(a.flash_mb)
        self.env = dict(e.split("=", 1) for e in ENV(a.flash_mb))
        self.ram = bytearray(RAM_SIZE)
        self.baud = 115200
        self.state = "off"                          # off, countdown, prompt, linux, baudwait
        self.until = 0.0
        self.line = bytearray()
        self.probed = False
        self.counts = {}
        self.forbidden = 0
        self.rnd = random.Random(7)
        self.out = bytearray()
        self.openipc = False                        # the flash boots OpenIPC's U-Boot
        self.nor16 = False                          # ... with its 16 MB layout saved
        self.writes = 0
        self.factory_boot = bytes(self.flash[0x100:0x100 + len(VERSION)])
        self.prompt_text = PROMPT

    # -- the wire ---------------------------------------------------------------

    def host_baud(self):
        try:
            return termios.tcgetattr(self.slave)[4]
        except termios.error:
            return 0

    def send(self, data, noisy=False):
        if isinstance(data, str):
            data = data.encode()
        data = data.replace(b"\n", b"\r\n")
        wrong = self.host_baud() != self.baud
        if wrong or (noisy and self.a.noise):
            b = bytearray(data)
            for i in range(len(b)):
                if wrong or self.rnd.random() < self.a.noise:
                    b[i] = self.rnd.randrange(256)
            data = bytes(b)
        while data:
            try:
                n = os.write(self.master, data)
                data = data[n:]
            except BlockingIOError:
                time.sleep(0.0005)

    def count(self, name):
        self.counts[name] = self.counts.get(name, 0) + 1

    # -- booting ----------------------------------------------------------------

    def power_on(self):
        mb = self.a.flash_mb
        self.baud = 115200
        self.probed = False
        self.openipc = bytes(self.flash[0x100:0x100 + len(VERSION)]) != self.factory_boot
        if self.openipc:
            self.prompt_text = b"OpenIPC # "
            self.send(f"\n\nSystem startup\n\nU-Boot 2010.06 (Oct 04 2026 - 11:27:52)\n\n"
                      f"SPI Nor total size: {mb}MB\n" + ("" if self.nor16 else
                      "*** Warning - bad CRC, using default environment\n\n*** Write default environment to flash\n")
                      + "In:    serial\nOut:   serial\nErr:   serial\n")
            self.send(f"Hit any key to stop autoboot: {self.a.bootdelay:2d} ")
            self.state = "countdown"
            self.until = time.monotonic() + self.a.bootdelay
            log("OpenIPC's U-Boot, counting down")
            return
        self.prompt_text = PROMPT
        self.send(f"\n\n{VERSION}\n\nCheck Flash Memory Controller v100 ... Found\n"
                  f"SPI Nor(cs 0) ID: 0xc2 0x20 0x{0x18 if mb == 16 else 0x17:02x}\n"
                  f"Block:64KB Chip:{mb}MB Name:\"MX25L{mb * 8}XX\"\n"
                  f"SPI Nor total size: {mb}MB\nMMC:   \nNo EMMC/MMC/SD device found !\n"
                  "*** Warning - bad CRC, using default environment\n\n"
                  "In:    serial\nOut:   serial\nErr:   serial\n")
        if self.a.locked:
            self.boot_linux()
            return
        self.send(f"Hit any key to stop autoboot: {self.a.bootdelay:2d} ")
        self.state = "countdown"
        self.until = time.monotonic() + self.a.bootdelay
        log("booted, counting down")

    def boot_linux(self):
        if self.openipc:
            if not self.nor16:
                self.send("## Booting kernel from Legacy Image at 82000000 ...\n   Image Name:   Linux-4.9.37-hi3518ev200\n"
                          "No filesystem could mount root, tried:  squashfs\n"
                          "Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(31,3)\n"
                          "Rebooting in 20 seconds..\n")
                self.state = "panic"
                self.until = time.monotonic() + 2
                log("OpenIPC with the 8 MB layout: panic")
                return
            self.send("## Booting kernel from Legacy Image at 82000000 ...\nStarting kernel ...\n"
                      "Linux version 4.9.37\nStarting majestic: OK\n\nWelcome to OpenIPC\nopenipc-hi3518ev200 login: ")
            self.state = "linux"
            log("OpenIPC up")
            return
        self.send(f"{self.a.flash_mb * 1024} KiB hi_fmc at 0:0 is now current device\n\n"
                  "## Booting kernel from Legacy Image at 82000000 ...\n   Image Name:   Linux-3.4.35\n"
                  "Starting kernel ...\n\nUncompressing Linux... done, booting the kernel.\n"
                  "Booting Linux on physical CPU 0\nLinux version 3.4.35 (hisilicon) #1 Mon Mar 5 2018\n"
                  "CPU: ARM926EJ-S [41069265] revision 5 (ARMv5TEJ), cr=00053177\n"
                  "Machine: hi3518ev200\nWelcome to HiLinux.\n(none) login: ")
        self.state = "linux"
        log("autoboot not stopped: Linux")

    def prompt(self):
        self.send(self.prompt_text)
        self.state = "prompt"

    # -- input ------------------------------------------------------------------

    def feed(self, data):
        if self.host_baud() != self.baud:
            self.count("noise")
            return
        if self.state == "off":
            self.power_on()
            return
        for c in data:
            if self.state == "countdown":
                if not (self.a.any_key or self.openipc) and bytes([c]) != self.a.stop_key.encode().decode("unicode_escape").encode("latin-1"):
                    continue                        # "any key" is one key here
                self.send("\b\b\b 0 \n")
                log("autoboot stopped")
                self.prompt()
                continue
            if self.state == "linux":
                if c == 0x0D:
                    self.send("\nopenipc-hi3518ev200 login: " if self.openipc else "\n(none) login: ")
                continue
            if self.state == "baudwait":
                if c == 0x0D:
                    self.prompt()
                continue
            if self.state != "prompt":
                continue
            if c == 0x03:
                self.send("<INTERRUPT>\n")
                self.line.clear()
                self.prompt()
            elif c in (0x0D, 0x0A):
                self.send("\n")
                cmd = self.line.decode(errors="replace").strip()
                self.line.clear()
                self.run(cmd)
            elif c in (0x08, 0x7F):
                if self.line:
                    self.line.pop()
                    self.send("\b \b")
            else:
                self.line.append(c)
                self.send(bytes([c]))

    def tick(self):
        if self.state == "countdown" and time.monotonic() > self.until:
            self.send("\b\b\b 0 \n")
            self.boot_linux()
        if self.state == "panic" and time.monotonic() > self.until:
            self.state = "off"
            self.power_on()

    # -- commands -----------------------------------------------------------------

    def num(self, s):
        return int(s, 16)                          # U-Boot: hex, with or without 0x

    def run(self, cmd):
        if not cmd:
            self.prompt()
            return
        w = cmd.split()
        name = w[0]
        self.count(name if name != "sf" or len(w) < 2 else "sf " + w[1])
        try:
            if name == "version":
                self.send("\nU-Boot 2010.06 (Oct 04 2026 - 11:27:52)\n" if self.openipc else f"\n{VERSION}\n")
            elif name == "printenv":
                for k, v in self.env.items():
                    self.send(f"{k}={v}\n")
                self.send(f"\nEnvironment size: {sum(len(k) + len(v) + 2 for k, v in self.env.items())}/65532 bytes\n")
            elif name == "bdinfo" and self.a.bdinfo:
                self.send("arch_number = 0x00001F40\nenv_t       = 0x00000000\nboot_params = 0x80000100\n"
                          "DRAM bank   = 0x00000000\n-> start    = 0x80000000\n-> size     = 0x04000000\n"
                          f"ethaddr     = {self.env['ethaddr']}\nip_addr     = {self.env['ipaddr']}\n"
                          f"baudrate    = {self.baud} bps\n")
            elif name == "sf" and len(w) >= 2 and w[1] == "probe":
                self.probed = True
                self.send(f"{self.a.flash_mb * 1024} KiB hi_fmc at 0:0 is now current device\n")
            elif name == "sf" and len(w) == 5 and w[1] == "read":
                if not self.probed:
                    self.send("No SPI flash selected. Please run `sf probe'\n")
                else:
                    ram, off, n = self.num(w[2]), self.num(w[3]), self.num(w[4])
                    if off + n > len(self.flash) or not (RAM_BASE <= ram and ram + n <= RAM_BASE + RAM_SIZE):
                        self.send("ERROR: attempting read past flash size\n")
                    else:
                        self.ram[ram - RAM_BASE:ram - RAM_BASE + n] = self.flash[off:off + n]
            elif name == "sf" and len(w) >= 2 and w[1] in ("erase", "write", "update"):
                if not self.a.writable:
                    self.refuse(cmd)
                elif w[1] == "erase" and len(w) == 4:
                    off, n = self.num(w[2]), self.num(w[3])
                    if off % 0x10000 or n % 0x10000 or off + n > len(self.flash):
                        self.send("ERROR: erase area not aligned or past the flash\n")
                    else:
                        for o in range(off, off + n, 0x10000):
                            self.flash[o:o + 0x10000] = b"\xff" * 0x10000
                        self.send("".join(f"Erasing at 0x{o + 0x10000:x} -- {(o - off + 0x10000) * 100 // n:3d}% complete.\n"
                                          for o in range(off, off + n, n // 8 or 0x10000)))
                        self.writes += 1
                elif w[1] == "write" and len(w) == 5:
                    ram, off, n = self.num(w[2]), self.num(w[3]), self.num(w[4])
                    src = self.ram[ram - RAM_BASE:ram - RAM_BASE + n]
                    for i in range(n):
                        self.flash[off + i] &= src[i]       # NOR: a write only clears bits
                    self.send(f"Writing at 0x{off + n:x} -- 100% complete.\n")
                    self.writes += 1
                else:
                    raise ValueError
            elif name.startswith("md"):
                self.md(name, w)
            elif name == "crc32":
                if self.a.no_crc:
                    self.send(f"Unknown command '{name}' - try 'help'\n")
                else:
                    a, n = self.num(w[1]), self.num(w[2])
                    crc = zlib.crc32(self.ram[a - RAM_BASE:a - RAM_BASE + n]) & 0xFFFFFFFF
                    self.send(f"CRC32 for {a:08x} ... {a + n - 1:08x} ==> {crc:08x}\n")
            elif name == "setenv":
                if len(w) >= 3 and w[1] == "baudrate":
                    if self.baudrate(int(w[2], 10)):
                        return
                elif len(w) >= 2:
                    self.env[w[1]] = " ".join(w[2:])
            elif self.a.writable and name in ("mw", "mw.b"):
                a, v, n = self.num(w[1]), self.num(w[2]), self.num(w[3])
                self.ram[a - RAM_BASE:a - RAM_BASE + n] = bytes([v & 0xFF]) * n
            elif self.a.writable and name == "mmcinfo" and self.a.sd:
                self.send("Device: HIMCI_V200\nName: SD16G \nCapacity: 15523119104\nBus Width: 4-bit\n")
            elif self.a.writable and name == "fatls" and self.a.sd:
                files = sorted(f for f in os.listdir(self.a.sd) if os.path.isfile(os.path.join(self.a.sd, f)))
                for f in files:
                    self.send(f" {os.path.getsize(os.path.join(self.a.sd, f)):8d}   {f} \n")
                self.send(f"\n{len(files)} file(s), 0 dir(s)\n\n")
            elif self.a.writable and name == "fatload" and self.a.sd and len(w) == 5:
                path = os.path.join(self.a.sd, w[4])
                if not os.path.isfile(path):
                    self.send(f"** Unable to read \"{w[4]}\" from mmc 0:1 **\n")
                else:
                    data = open(path, "rb").read()
                    a = self.num(w[3])
                    self.ram[a - RAM_BASE:a - RAM_BASE + len(data)] = data
                    self.send(f"reading {w[4]}\n\n{len(data)} bytes read\n")
            elif self.openipc and name == "run" and w[1:] == ["setnor16m"]:
                self.send("Saving Environment to SPI Flash...\nWriting to SPI flash, offset 0x00040000 size 64K ...done\n"
                          "resetting ...\n")
                self.nor16 = True
                self.writes += 1
                self.state = "off"
                self.power_on()
                return
            elif name in ("saveenv", "mw", "tftp", "tftpboot", "erase", "protect", "cp"):
                self.refuse(cmd)
            elif name == "reset":
                self.send("resetting ...\n")
                self.state = "off"
                self.power_on()
                return
            else:
                self.send(f"Unknown command '{name}' - try 'help'\n")
        except (IndexError, ValueError):
            self.send(f"Usage:\n{name} ...\n")
        self.prompt()

    def refuse(self, cmd):
        self.forbidden += 1
        log(f"FORBIDDEN: {cmd}")
        self.send("refused by the fake: this would write\n")

    def baudrate(self, b):
        if b > self.a.max_baud:
            self.send(f"## Baudrate {b} bps not supported\n")
            return False
        self.send(f"## Switch baudrate to {b} bps and press ENTER ...\n")
        time.sleep(0.05)
        self.baud = b
        self.env["baudrate"] = str(b)
        self.state = "baudwait"
        log(f"console at {b}")
        return True

    def md(self, name, w):
        size = {"md": 4, "md.l": 4, "md.w": 2, "md.b": 1}.get(name)
        if size is None or len(w) < 2:
            raise ValueError
        a = self.num(w[1])
        count = self.num(w[2]) if len(w) > 2 else 64
        n = count * size
        if not (RAM_BASE <= a and a + n <= RAM_BASE + RAM_SIZE):
            self.send("Bad address\n")
            return
        buf = self.ram[a - RAM_BASE:a - RAM_BASE + n]
        out = []
        for o in range(0, n, 16):
            row = buf[o:o + 16]
            if size == 4:
                cells = " ".join(f"{v:08x}" for v in struct.unpack(f"<{len(row) // 4}I", row))
            elif size == 2:
                cells = " ".join(f"{v:04x}" for v in struct.unpack(f"<{len(row) // 2}H", row))
            else:
                cells = " ".join(f"{v:02x}" for v in row)
            asc = "".join(chr(c) if 32 <= c < 127 else "." for c in row)
            out.append(f"{a + o:08x}: {cells}    {asc}\n")
            if len(out) == 256:
                self.send("".join(out), noisy=True)
                out = []
        if out:
            self.send("".join(out), noisy=True)

    def summary(self):
        log(f"commands: {self.counts}")
        log(f"FORBIDDEN (writing) commands: {self.forbidden}")
        if self.a.writable:
            log(f"writes (allowed, --writable): {self.writes}; OpenIPC {self.openipc}, 16 MB layout {self.nor16}")
            if self.a.flash_out:
                open(self.a.flash_out + ".after", "wb").write(self.flash)


def serve(a):
    master, slave = pty.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    path = os.ttyname(slave)
    cam = Camera(a, master, slave)
    if a.flash_out:
        open(a.flash_out, "wb").write(cam.flash)
    print(f"PTY {path}", flush=True)
    print(f"ENV P4_SIM_UART_B={path}", flush=True)
    log(f"{a.flash_mb} MB flash, bootdelay {a.bootdelay}" + (", locked" if a.locked else "")
        + (", no crc32" if a.no_crc else "") + (f", noise {a.noise}" if a.noise else ""))

    def bye(*_):
        cam.summary()
        sys.exit(0)
    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)

    def unplug(*_):                                 # kill -USR1: the camera unplugged; the next byte powers it on
        cam.state = "off"
        log("unplugged")
    signal.signal(signal.SIGUSR1, unplug)
    while True:
        r, _, _ = select.select([master], [], [], 0.003)
        if r:
            try:
                data = os.read(master, 65536)
            except (BlockingIOError, OSError):
                data = b""
            if data:
                cam.feed(data)
        cam.tick()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--flash-mb", type=int, default=16, choices=(8, 16))
    s.add_argument("--bootdelay", type=int, default=1)
    s.add_argument("--no-crc", action="store_true")
    s.add_argument("--locked", action="store_true")
    s.add_argument("--any-key", action="store_true")
    s.add_argument("--writable", action="store_true")
    s.add_argument("--sd")
    s.add_argument("--stop-key", default="q", help='the one key that stops it ("q", " ", "\\x1b"...)')
    s.add_argument("--bdinfo", action="store_true")
    s.add_argument("--noise", type=float, default=0.0)
    s.add_argument("--max-baud", type=int, default=921600)
    s.add_argument("--flash-out")
    a = p.parse_args()
    sys.exit(serve(a))


if __name__ == "__main__":
    main()
