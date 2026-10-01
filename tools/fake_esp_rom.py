#!/usr/bin/env python3
"""
fake_esp_rom.py - an ESP32 ROM serial bootloader on a pty, to test the
Programador app (components/aos_flasher) without a target board.

    tools/fake_esp_rom.py serve [--chip esp32s3] [--flash-size 4MB]
                                [--dump flash.bin] [--lines lines.txt]
                                [--fast] [--no-stub-support]
    tools/fake_esp_rom.py check --dump flash.bin SOURCE

serve opens a pty and prints its path; run the simulator with
P4_SIM_UART_B=<that path> (and P4_SIM_UART_B_LINES=<the --lines file> so the
target's EN/BOOT reach it: sim/io_sim.c appends "<ms> en=X boot=Y" there on
every aos_io_uart_lines()). It answers what esp-serial-flasher 2.x sends:

    SYNC (eight answers, as the ROM), READ_REG / WRITE_REG with the chip's
    detection register (0x40001000), its eFuse MAC and revision words and
    the SPI peripheral registers the library drives for the flash ID (0x9F),
    GET_SECURITY_INFO (not on the ESP32, 12 bytes on the S2), SPI_ATTACH,
    SPI_SET_PARAMS, CHANGE_BAUDRATE, FLASH_BEGIN/DATA/END (erase, sequence
    numbers, the XOR checksum, NOR semantics: a write can only clear bits,
    so an unerased sector shows), SPI_FLASH_MD5 (32 hex chars from the ROM,
    16 raw bytes from the stub), MEM_BEGIN/DATA/END (the flasher stub: after
    MEM_END with an entry point it says "OHAI" and behaves as the stub:
    two status bytes instead of four, ERASE_FLASH/ERASE_REGION), READ_FLASH.
    Anything else is logged as UNKNOWN and answered with INVALID_COMMAND.

The flash starts dirty (a fixed pseudo-random "old firmware"), so a region
that was written without being erased first fails the check. --dump writes
the image after every MD5, FLASH_END and at exit, plus <dump>.json with the
regions written. The timing is roughly real (bytes at the current baud rate,
erase at ~2 ms per KB) unless --fast.

With --lines the EN/BOOT edges matter, as on a real board: EN rising with
BOOT low enters the download mode (the ROM banner, then SLIP); with BOOT high
it "runs" what is in flash: the boot log of the app found through the
partition table (its project name and version from the app descriptor), and
a heartbeat line every second and a half - what the Terminal shows after
"Monitor". Without --lines it is always in download mode.

check compares the dump with a firmware source the way the app resolves it:
a folder with flasher_args.json (or with build/flasher_args.json), a
name@0x10000.bin, or a plain .bin (0x10000 if it is an app image, else 0x0).
Exit status 0 when every file is in flash, byte for byte, at its offset.
"""
import argparse
import hashlib
import json
import os
import pty
import random
import select
import signal
import struct
import sys
import time
import tty

# ---------------------------------------------------------------------------
# Chips (esp-serial-flasher's src/esp_targets.c)
# ---------------------------------------------------------------------------

SPI_XX = dict(cmd=0x00, usr=0x18, usr2=0x20, w0=0x58)
CHIPS = {
    "esp8266": dict(magic=0xFFF0C101, chip_id=None, efuse=None, mac_off=None, spi=0x60000200,
                    regs=dict(cmd=0x00, usr=0x1C, usr2=0x24, w0=0x40), sec=None, rom="ets Jan  8 2013,rst cause:2, boot mode:(1,7)"),
    "esp32":   dict(magic=0x00F01D83, chip_id=0, efuse=0x3FF5A000, mac_off=0x04, spi=0x3FF42000,
                    regs=dict(cmd=0x00, usr=0x1C, usr2=0x24, w0=0x80), sec=None, rom="ets Jun  8 2016 00:22:57"),
    "esp32s2": dict(magic=0x000007C6, chip_id=2, efuse=0x3F41A000, mac_off=0x44, spi=0x3F402000,
                    regs=SPI_XX, sec="s2", rom="ESP-ROM:esp32s2-rc4-20191025"),
    "esp32c3": dict(magic=0x1B31506F, chip_id=5, efuse=0x60008800, mac_off=0x44, spi=0x60002000,
                    regs=SPI_XX, sec="full", rom="ESP-ROM:esp32c3-api1-20210207"),
    "esp32s3": dict(magic=0x00000009, chip_id=9, efuse=0x60007000, mac_off=0x44, spi=0x60002000,
                    regs=SPI_XX, sec="full", rom="ESP-ROM:esp32s3-20210327"),
    "esp32c6": dict(magic=0x2CE0806F, chip_id=13, efuse=0x600B0800, mac_off=0x44, spi=0x60003000,
                    regs=SPI_XX, sec="full", rom="ESP-ROM:esp32c6-20220919"),
    "esp32p4": dict(magic=0, chip_id=18, efuse=0x5012D000, mac_off=0x44, spi=0x5008D000,
                    regs=SPI_XX, sec="full", rom="ESP-ROM:esp32p4-eco2-20240710"),
    "esp32h2": dict(magic=0xD7B73E80, chip_id=16, efuse=0x600B0800, mac_off=0x44, spi=0x60003000,
                    regs=SPI_XX, sec="full", rom="ESP-ROM:esp32h2-20221101"),
}
DETECT_REG = 0x40001000
ESP32_APB_CTL_DATE = 0x3FF6607C

CMD_NAMES = {
    0x02: "FLASH_BEGIN", 0x03: "FLASH_DATA", 0x04: "FLASH_END", 0x05: "MEM_BEGIN", 0x06: "MEM_END",
    0x07: "MEM_DATA", 0x08: "SYNC", 0x09: "WRITE_REG", 0x0A: "READ_REG", 0x0B: "SPI_SET_PARAMS",
    0x0D: "SPI_ATTACH", 0x0E: "READ_FLASH", 0x0F: "CHANGE_BAUDRATE", 0x10: "FLASH_DEFL_BEGIN",
    0x11: "FLASH_DEFL_DATA", 0x12: "FLASH_DEFL_END", 0x13: "SPI_FLASH_MD5", 0x14: "GET_SECURITY_INFO",
    0xD0: "ERASE_FLASH", 0xD1: "ERASE_REGION", 0xD2: "READ_FLASH_STUB",
}
INVALID_COMMAND, COMMAND_FAILED, INVALID_CRC = 0x05, 0x06, 0x07


def parse_size(s):
    s = s.upper().rstrip("B")
    mult = 1
    if s.endswith("M"):
        mult, s = 1 << 20, s[:-1]
    elif s.endswith("K"):
        mult, s = 1 << 10, s[:-1]
    return int(s, 0) * mult


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---------------------------------------------------------------------------
# The target
# ---------------------------------------------------------------------------

class Target:
    def __init__(self, args, master):
        self.a = args
        self.fd = master
        self.chip = CHIPS[args.chip]
        self.flash_size = parse_size(args.flash_size)
        rnd = random.Random(1234)
        # the "old firmware": anything but 0xFF, so an unerased write shows
        noff = bytes(range(255)) + b"\x7f"
        self.flash = bytearray(rnd.randbytes(self.flash_size).translate(noff))
        self.mac = bytes(int(x, 16) for x in args.mac.split(":"))
        self.regs = {}
        self.stub = False
        self.baud = 115200
        self.mode = "download"          # download | reset | app
        self.rx = bytearray()
        self.in_frame = False
        self.writing = None             # (offset, packet_size, next_seq)
        self.mem = None
        self.regions = []
        self.counts = {}
        self.unknown = []
        self.bauds = [115200]
        self.app_next = 0
        self.app_name = ""
        self.lines_pos = 0
        self.en, self.boot = 1, 1
        self._init_regs()

    # -- registers --------------------------------------------------------

    def _init_regs(self):
        c = self.chip
        self.regs[DETECT_REG] = c["magic"]
        if c["efuse"] is not None:
            m = self.mac
            base = c["efuse"] + c["mac_off"]
            self.regs[base] = (m[2] << 24) | (m[3] << 16) | (m[4] << 8) | m[5]
            self.regs[base + 4] = (m[0] << 8) | m[1]
            e = c["efuse"]
            blk1 = e + 0x44
            name = self.a.chip
            if name == "esp32":              # v3.1
                self.regs[e + 12] = 1 << 15
                self.regs[e + 20] = (1 << 20) | (1 << 24)
                self.regs[ESP32_APB_CTL_DATE] = 1 << 31
            elif name == "esp32s3":          # v0.2
                self.regs[blk1 + 12] = 2 << 18
            elif name == "esp32c3":          # v0.4
                self.regs[blk1 + 12] = 4 << 18
            elif name in ("esp32c6", "esp32h2"):   # v0.1
                self.regs[blk1 + 12] = 1 << 18
            elif name == "esp32p4":          # v1.0, detected by its SPI date register
                self.regs[blk1 + 8] = 1 << 4
                self.regs[0x500D0000] = 0x2207202

    def reg_read(self, addr):
        return self.regs.get(addr, 0)

    def reg_write(self, addr, value, mask):
        old = self.regs.get(addr, 0)
        value = (old & ~mask) | (value & mask)
        spi, r = self.chip["spi"], self.chip["regs"]
        if addr == spi + r["cmd"] and value & (1 << 18):
            op = self.regs.get(spi + r["usr2"], 0) & 0xFF
            if op == 0x9F:
                cap = {1 << 20: 0x14, 2 << 20: 0x15, 4 << 20: 0x16, 8 << 20: 0x17, 16 << 20: 0x18,
                       32 << 20: 0x19}.get(self.flash_size, 0x16)
                self.regs[spi + r["w0"]] = 0xEF | (0x40 << 8) | (cap << 16)
            else:
                log(f"  SPI user command 0x{op:02x} (not emulated)")
            value &= ~(1 << 18)             # done at once
        self.regs[addr] = value & 0xFFFFFFFF

    # -- the wire ---------------------------------------------------------

    def send_raw(self, data):
        view = memoryview(data)
        while view:
            try:
                n = os.write(self.fd, view)
                view = view[n:]
            except BlockingIOError:
                time.sleep(0.001)

    def slip(self, payload):
        out = bytearray(b"\xc0")
        for b in payload:
            if b == 0xC0:
                out += b"\xdb\xdc"
            elif b == 0xDB:
                out += b"\xdb\xdd"
            else:
                out.append(b)
        out.append(0xC0)
        self.send_raw(bytes(out))

    def status_len(self):
        # the ESP32-family ROMs end a response with four status bytes, the
        # ESP8266's ROM and every stub with two
        return 2 if (self.stub or self.a.chip == "esp8266") else 4

    def reply(self, cmd, value=0, data=b"", ok=True, err=0):
        status = bytes([0 if ok else 1, 0 if ok else err]) + b"\0" * (self.status_len() - 2)
        body = bytes(data) + status
        self.slip(struct.pack("<BBHI", 1, cmd, len(body), value) + body)

    def feed(self, data):
        if not self.a.fast:
            time.sleep(len(data) * 10 / self.baud)
        if self.mode != "download":
            return
        for b in data:
            if b == 0xC0:
                if self.in_frame and self.rx:
                    self.packet(bytes(self.unslip(self.rx)))
                    self.rx = bytearray()
                    self.in_frame = True        # a delimiter may also open the next one
                else:
                    self.in_frame = True
                    self.rx = bytearray()
            elif self.in_frame:
                self.rx.append(b)

    @staticmethod
    def unslip(b):
        out, i = bytearray(), 0
        while i < len(b):
            if b[i] == 0xDB and i + 1 < len(b):
                out.append(0xC0 if b[i + 1] == 0xDC else 0xDB)
                i += 2
            else:
                out.append(b[i])
                i += 1
        return out

    # -- commands ---------------------------------------------------------

    def packet(self, p):
        if len(p) < 8 or p[0] != 0:
            log(f"  junk frame of {len(p)} bytes")
            return
        cmd, size, chk = struct.unpack_from("<BHI", p, 1)
        d = p[8:]
        name = CMD_NAMES.get(cmd, f"0x{cmd:02x}")
        self.counts[name] = self.counts.get(name, 0) + 1
        if len(d) != size:
            log(f"  {name}: size field {size} but {len(d)} bytes")
            return self.reply(cmd, ok=False, err=INVALID_COMMAND)
        h = getattr(self, "c_" + name.lower(), None) if cmd in CMD_NAMES else None
        if not h:
            log(f"  UNKNOWN command {name} ({len(d)} bytes): {d[:32].hex()}")
            self.unknown.append(name)
            return self.reply(cmd, ok=False, err=INVALID_COMMAND)
        h(cmd, d, chk)

    def c_sync(self, cmd, d, chk):
        if d[:4] != b"\x07\x07\x12\x20":
            return
        log(f"SYNC ({'stub' if self.stub else 'ROM'})")
        for _ in range(8):
            self.reply(cmd, value=0x20120707)

    def c_read_reg(self, cmd, d, chk):
        (addr,) = struct.unpack_from("<I", d)
        self.reply(cmd, value=self.reg_read(addr))

    def c_write_reg(self, cmd, d, chk):
        addr, value, mask, delay = struct.unpack_from("<IIII", d)
        self.reg_write(addr, value, mask)
        self.reply(cmd)

    def c_get_security_info(self, cmd, d, chk):
        sec = self.chip["sec"]
        if sec is None:
            log("GET_SECURITY_INFO: not on this chip")
            return self.reply(cmd, ok=False, err=INVALID_COMMAND)
        data = struct.pack("<IB7s", 0, 0, b"\0" * 7)
        if sec == "full":
            data += struct.pack("<II", self.chip["chip_id"], 0)
        log(f"GET_SECURITY_INFO ({len(data)} bytes)")
        self.reply(cmd, data=data)

    def c_spi_attach(self, cmd, d, chk):
        log(f"SPI_ATTACH ({len(d)} bytes)")
        self.reply(cmd)

    def c_spi_set_params(self, cmd, d, chk):
        fid, total, block, sector, page, mask = struct.unpack_from("<IIIIII", d)
        log(f"SPI_SET_PARAMS flash {total >> 20} MB")
        self.reply(cmd)

    def c_change_baudrate(self, cmd, d, chk):
        new, old = struct.unpack_from("<II", d)
        log(f"CHANGE_BAUDRATE {self.baud} -> {new}")
        self.reply(cmd)
        self.baud = new
        self.bauds.append(new)

    def erase(self, off, size):
        start = off & ~0xFFF
        end = min(self.flash_size, (off + size + 0xFFF) & ~0xFFF)
        self.flash[start:end] = b"\xff" * (end - start)
        if not self.a.fast:
            time.sleep((end - start) / 1024 * 0.002)

    def c_flash_begin(self, cmd, d, chk):
        erase, packets, psize, off = struct.unpack_from("<IIII", d)
        enc = struct.unpack_from("<I", d, 16)[0] if len(d) >= 20 else None
        log(f"FLASH_BEGIN 0x{off:06x} erase {erase} B, {packets} x {psize} B" + (f", encrypted={enc}" if enc is not None else ""))
        if off + erase > self.flash_size:
            return self.reply(cmd, ok=False, err=COMMAND_FAILED)
        self.erase(off, erase)
        self.writing = [off, psize, 0, 0]
        self.reply(cmd)

    def c_flash_data(self, cmd, d, chk):
        size, seq = struct.unpack_from("<II", d)
        data = d[16:]
        x = 0xEF
        for b in data:
            x ^= b
        if len(data) != size:
            return self.reply(cmd, ok=False, err=INVALID_COMMAND)
        if x != (chk & 0xFF):
            log(f"  FLASH_DATA seq {seq}: bad checksum")
            return self.reply(cmd, ok=False, err=INVALID_CRC)
        if not self.writing or seq != self.writing[2]:
            log(f"  FLASH_DATA seq {seq}, expected {self.writing[2] if self.writing else None}")
            return self.reply(cmd, ok=False, err=COMMAND_FAILED)
        off = self.writing[0] + seq * self.writing[1]
        for i, b in enumerate(data):            # NOR flash: bits only go to 0
            self.flash[off + i] &= b
        self.writing[2] += 1
        self.writing[3] += len(data)
        self.note_region(off, len(data))
        self.reply(cmd)

    def note_region(self, off, n):
        if self.regions and self.regions[-1][0] + self.regions[-1][1] == off:
            self.regions[-1][1] += n
        else:
            self.regions.append([off, n])

    def c_flash_end(self, cmd, d, chk):
        (stay,) = struct.unpack_from("<I", d) if d else (1,)
        if self.writing:
            log(f"FLASH_END after {self.writing[3]} B, stay_in_loader={stay}")
        self.writing = None
        self.reply(cmd)
        self.dump()

    def c_spi_flash_md5(self, cmd, d, chk):
        addr, size = struct.unpack_from("<II", d)
        md5 = hashlib.md5(self.flash[addr:addr + size])
        log(f"SPI_FLASH_MD5 0x{addr:06x} +{size}: {md5.hexdigest()}")
        if not self.a.fast:
            time.sleep(size / (8 << 20))
        self.reply(cmd, data=md5.digest() if self.stub else md5.hexdigest().encode())
        self.dump()

    def c_mem_begin(self, cmd, d, chk):
        size, blocks, bsize, off = struct.unpack_from("<IIII", d)
        log(f"MEM_BEGIN 0x{off:08x} {size} B")
        self.mem = [off, bsize, 0]
        self.reply(cmd)

    def c_mem_data(self, cmd, d, chk):
        size, seq = struct.unpack_from("<II", d)
        data = d[16:]
        x = 0xEF
        for b in data:
            x ^= b
        if x != (chk & 0xFF) or len(data) != size:
            return self.reply(cmd, ok=False, err=INVALID_CRC)
        self.reply(cmd)

    def c_mem_end(self, cmd, d, chk):
        stay, entry = struct.unpack_from("<II", d)
        log(f"MEM_END entry 0x{entry:08x}")
        self.reply(cmd)
        if entry and not stay and not self.a.no_stub_support:
            time.sleep(0.01)
            self.stub = True
            self.slip(b"OHAI")
            log("stub running")

    def c_read_flash(self, cmd, d, chk):
        addr, size = struct.unpack_from("<II", d)
        self.reply(cmd, data=self.flash[addr:addr + size])

    def c_erase_flash(self, cmd, d, chk):
        if not self.stub:
            return self.reply(cmd, ok=False, err=INVALID_COMMAND)
        self.erase(0, self.flash_size)
        self.reply(cmd)

    def c_erase_region(self, cmd, d, chk):
        if not self.stub:
            return self.reply(cmd, ok=False, err=INVALID_COMMAND)
        off, size = struct.unpack_from("<II", d)
        self.erase(off, size)
        self.reply(cmd)

    # -- resets through EN/BOOT --------------------------------------------

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
            rising = self.en == 0 and en == 1
            self.en, self.boot = en, boot
            if en == 0 and self.mode != "reset":
                self.mode = "reset"
                self.rx, self.in_frame = bytearray(), False
            if rising:
                self.boot_now()

    def boot_now(self):
        self.stub = False
        self.baud = 115200
        self.bauds = [115200]
        self.writing = None
        rom = self.chip["rom"]
        if self.boot == 0:
            self.mode = "download"
            log("reset, BOOT low: download mode")
            self.send_raw(f"{rom}\r\nrst:0x1 (POWERON),boot:0x3 (DOWNLOAD(USB/UART0))\r\nwaiting for download\r\n".encode())
        else:
            self.mode = "app"
            self.app_name, ver = self.find_app()
            log(f"reset, BOOT high: runs {self.app_name or 'nothing'}")
            lines = [rom, "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)"]
            if self.app_name:
                lines += ["I (27) boot: ESP-IDF 2nd stage bootloader",
                          "I (31) boot: Loaded app from partition at offset 0x10000",
                          "I (245) cpu_start: Pro cpu start user code",
                          f"I (262) app_init: Project name:     {self.app_name}",
                          f"I (267) app_init: App version:      {ver}",
                          "I (301) main_task: Calling app_main()"]
            else:
                lines += ["invalid header: 0xffffffff"] * 3
            self.send_raw(("\r\n".join(lines) + "\r\n").encode())
            self.app_next = time.time() + 1.5

    def find_app(self):
        off = 0x10000
        pt = self.flash[0x8000:0x8000 + 0xC00]
        for i in range(0, len(pt), 32):
            magic, typ, sub, poff = struct.unpack_from("<HBBI", pt, i)
            if magic != 0x50AA:
                break
            if typ == 0 and sub in (0x00, 0x10):
                off = poff
                break
        h = self.flash[off:off + 112]
        if h[0] != 0xE9 or h[32:36] != b"\x32\x54\xcd\xab":
            return "", ""
        name = h[80:112].split(b"\0")[0].decode(errors="replace")
        ver = h[48:80].split(b"\0")[0].decode(errors="replace")
        return name, ver

    def tick(self):
        if self.mode == "app" and self.app_name and time.time() >= self.app_next:
            self.app_next = time.time() + 1.5
            ms = int(time.time() * 1000) % 1000000
            self.send_raw(f"I ({ms}) main: {self.app_name} funcionando, heap libre {280000 - ms % 997} B\r\n".encode())

    # -- output -------------------------------------------------------------

    def dump(self):
        if not self.a.dump:
            return
        with open(self.a.dump, "wb") as f:
            f.write(self.flash)
        with open(self.a.dump + ".json", "w") as f:
            json.dump({"chip": self.a.chip, "stub": self.stub, "bauds": self.bauds,
                       "regions": [{"offset": hex(o), "size": n} for o, n in self.regions],
                       "commands": self.counts, "unknown": self.unknown}, f, indent=1)


def serve(a):
    master, slave = pty.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    path = os.ttyname(slave)
    if a.lines:
        open(a.lines, "w").close()
    t = Target(a, master)
    print(f"PTY {path}", flush=True)
    env = f"P4_SIM_UART_B={path}" + (f" P4_SIM_UART_B_LINES={a.lines}" if a.lines else "")
    print(f"ENV {env}", flush=True)
    log(f"fake {a.chip} ROM, {a.flash_size} flash, MAC {a.mac}" + (", EN/BOOT from " + a.lines if a.lines else ", always in download mode"))

    def bye(*_):
        t.dump()
        log(f"commands: {t.counts}")
        if t.unknown:
            log(f"UNKNOWN commands seen: {t.unknown}")
        sys.exit(0)
    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    while True:
        t.poll_lines()
        r, _, _ = select.select([master], [], [], 0.005)
        if r:
            try:
                data = os.read(master, 65536)
            except (BlockingIOError, OSError):
                data = b""
            if data:
                t.feed(data)
        t.tick()


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

def resolve(source):
    """[(offset, path)] the way components/aos_flasher/aos_flasher_src.c does."""
    if os.path.isdir(source):
        d = source
        if not os.path.exists(os.path.join(d, "flasher_args.json")) and os.path.exists(os.path.join(d, "build", "flasher_args.json")):
            d = os.path.join(d, "build")
        with open(os.path.join(d, "flasher_args.json")) as f:
            args = json.load(f)
        return sorted((int(k, 0), os.path.join(d, v)) for k, v in args["flash_files"].items())
    name = os.path.basename(source)
    if "@0x" in name.lower() and name.lower().endswith(".bin"):
        return [(int(name[name.lower().rindex("@0x") + 1:-4], 16), source)]
    with open(source, "rb") as f:
        h = f.read(40)
    app = len(h) >= 36 and h[0] == 0xE9 and h[32:36] == b"\x32\x54\xcd\xab"
    return [(0x10000 if app else 0, source)]


def check(a):
    with open(a.dump, "rb") as f:
        flash = f.read()
    ok = True
    for off, path in resolve(a.source):
        data = open(path, "rb").read()
        got = flash[off:off + len(data)]
        pad = (-len(data)) % 4
        same = got == data
        pad_ok = flash[off + len(data):off + len(data) + pad] == b"\xff" * pad
        print(f"0x{off:06x} {os.path.relpath(path, a.source) if os.path.isdir(a.source) else os.path.basename(path)}: "
              f"{len(data)} B, md5 {hashlib.md5(data).hexdigest()} -> {'OK' if same and pad_ok else 'DIFFERENT'}")
        if not same:
            bad = next(i for i in range(len(data)) if got[i:i + 1] != data[i:i + 1])
            print(f"    first difference at 0x{off + bad:06x}")
        ok &= same and pad_ok
    side = a.dump + ".json"
    if os.path.exists(side):
        info = json.load(open(side))
        print(f"target: {info['chip']}, stub {info['stub']}, bauds {info['bauds']}, unknown commands {info['unknown']}")
    print("ALL OK" if ok else "MISMATCH")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("serve")
    s.add_argument("--chip", default="esp32s3", choices=sorted(CHIPS))
    s.add_argument("--flash-size", default="4MB")
    s.add_argument("--mac", default="7c:df:a1:e5:3a:10")
    s.add_argument("--dump")
    s.add_argument("--lines")
    s.add_argument("--fast", action="store_true", help="no simulated wire or erase time")
    s.add_argument("--no-stub-support", action="store_true", help="MEM_END does not start a stub")
    c = sub.add_parser("check")
    c.add_argument("--dump", required=True)
    c.add_argument("source")
    a = ap.parse_args()
    if a.cmd == "check":
        sys.exit(check(a))
    if a.cmd is None:
        a = ap.parse_args(["serve"] + sys.argv[1:])
    serve(a)


if __name__ == "__main__":
    main()
