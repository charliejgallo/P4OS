#!/usr/bin/env python3
"""
link.py - puts boot2.o and blink.o together for an RP2040 and writes
blink.bin (from 0x10000000) and blink.uf2, without an ARM linker: the Mac
has clang, which compiles for a Cortex-M0+, but no ld for ARM ELF.

    link.py build/boot2.o build/blink.o build/

The flash, from 0x10000000:
    boot2       252 bytes of code and its CRC-32 (the boot ROM's, MPEG-2:
                poly 0x04C11DB7, no reflection, init 0xFFFFFFFF, no xorout)
    0x100       the vector table: the stack's top, reset_handler, and
                fault for the other 14 exceptions
    0x140       blink.o's .text, then its read-only data

Only the relocations such an object has are done: R_ARM_ABS32 (a literal
pool's address of a string or a table) and the Thumb calls (R_ARM_THM_CALL,
R_ARM_THM_JUMP24). Anything else, or a symbol that is not there (a call to
memcpy), stops it with a message: nothing is linked by guess.
"""
import struct
import sys

FLASH = 0x10000000
STACK = 0x20042000
UF2_RP2040 = 0xE48BFF56


def elf(path):
    b = open(path, "rb").read()
    assert b[:4] == b"\x7fELF" and b[4] == 1 and b[5] == 1, f"{path}: not a 32-bit little-endian ELF"
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from("<10I", b, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, flags=flags, off=off, size=size, link=link, info=info, align=align, entsize=entsize))
    strtab = secs[shstrndx]
    for s in secs:
        n = b[strtab["off"] + s["name"]:]
        s["name"] = n[:n.index(0)].decode()
        s["data"] = bytearray(b[s["off"]:s["off"] + s["size"]]) if s["type"] != 8 else bytearray(s["size"])
    syms = []
    symtab = next(s for s in secs if s["type"] == 2)
    names = secs[symtab["link"]]
    for i in range(symtab["size"] // 16):
        nm, val, size, info, other, shndx = struct.unpack_from("<IIIBBH", b, symtab["off"] + 16 * i)
        n = b[names["off"] + nm:]
        syms.append(dict(name=n[:n.index(0)].decode(), value=val, shndx=shndx, type=info & 15))
    return secs, syms


def link_blob(path, base):
    """The allocated sections of one object, laid out from base: (bytes, symbols by name)"""
    secs, syms = elf(path)
    at = {}
    blob = bytearray()
    for i, s in enumerate(secs):
        if not (s["flags"] & 2) or s["size"] == 0 or s["name"].startswith(".ARM.ex"):
            continue        # not loaded, or the unwind tables nothing here uses
        if s["flags"] & 1 and not s["name"].startswith(".text"):
            sys.exit(f"{path}: {s['name']} is writable data; this program must have none")
        al = max(s["align"], 4)
        blob += b"\0" * (-len(blob) % al)
        at[i] = base + len(blob)
        blob += s["data"]
    addr = {}
    for y in syms:
        if y["shndx"] in at:
            addr[y["name"]] = at[y["shndx"]] + y["value"]
    for s in secs:
        if s["type"] != 9 or s["info"] not in at:       # SHT_REL against a section we placed
            continue
        tgt = secs[s["info"]]
        tbase = at[s["info"]]
        toff = tbase - base
        for k in range(s["size"] // 8):
            off, info = struct.unpack_from("<II", s["data"], 8 * k)
            y = syms[info >> 8]
            rtype = info & 255
            if y["shndx"] in at:
                S = at[y["shndx"]] + y["value"]
            else:
                sys.exit(f"{path}: {y['name'] or '?'} is not defined here (relocation type {rtype})")
            P = tbase + off
            if rtype == 2:          # R_ARM_ABS32
                A, = struct.unpack_from("<I", blob, toff + off)
                struct.pack_into("<I", blob, toff + off, (S + A) & 0xFFFFFFFF)
            elif rtype in (10, 30):  # R_ARM_THM_CALL, R_ARM_THM_JUMP24: BL/B.W, addend -4
                d = (S & ~1) - P - 4
                if not -(1 << 24) <= d < (1 << 24):
                    sys.exit("branch out of range")
                sgn = (d >> 24) & 1
                i1, i2 = (d >> 23) & 1, (d >> 22) & 1
                j1, j2 = (1 - i1) ^ sgn, (1 - i2) ^ sgn
                hi = 0xF000 | (sgn << 10) | ((d >> 12) & 0x3FF)
                lo_kind = 0xD000 if rtype == 10 else 0x9000
                lo = lo_kind | (j1 << 13) | (j2 << 11) | ((d >> 1) & 0x7FF)
                struct.pack_into("<HH", blob, toff + off, hi, lo)
            else:
                sys.exit(f"{path}: relocation type {rtype} at {tgt['name']}+0x{off:x} is not handled")
    return blob, addr


def crc32_mpeg2(data):
    c = 0xFFFFFFFF
    for x in data:
        c ^= x << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c


def uf2(image, base):
    out = bytearray()
    n = (len(image) + 255) // 256
    for i in range(n):
        chunk = image[256 * i:256 * (i + 1)].ljust(256, b"\0")
        out += struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, base + 256 * i, 256, i, n, UF2_RP2040)
        out += chunk + b"\0" * (476 - 256) + struct.pack("<I", 0x0AB16F30)
    return bytes(out)


def main():
    boot_o, prog_o, outdir = sys.argv[1:4]
    boot, _ = link_blob(boot_o, 0x20041F00)          # where the ROM runs it; it uses no address of its own
    if len(boot) > 252:
        sys.exit(f"boot2 is {len(boot)} bytes: 252 at most")
    boot = bytes(boot).ljust(252, b"\0")
    boot += struct.pack("<I", crc32_mpeg2(boot))
    code, sym = link_blob(prog_o, FLASH + 0x140)
    for need in ("reset_handler", "fault"):
        if need not in sym:
            sys.exit(f"{prog_o}: no {need}")
    vec = struct.pack("<II", STACK, sym["reset_handler"] | 1) + struct.pack("<I", sym["fault"] | 1) * 14
    image = boot + vec + bytes(code)
    open(f"{outdir}/blink.bin", "wb").write(image)
    open(f"{outdir}/blink.uf2", "wb").write(uf2(image, FLASH))
    print(f"blink.bin {len(image)} bytes, reset_handler at 0x{sym['reset_handler']:08x}; blink.uf2 {len(image) // 256 + 1} blocks")


if __name__ == "__main__":
    main()
