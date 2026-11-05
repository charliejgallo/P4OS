#!/usr/bin/env python3
"""
link.py - lays blink.o out for an STM32F103 and writes <name>.bin (from
0x08000000), <name>.hex (Intel HEX, with its address) and <name>.uf2 (the
STM32F1 family), without an ARM linker.

    link.py build/rapido.o build/ rapido

The flash, from 0x08000000:
    0x00   the vector table: the stack's top (the end of 20 KB of RAM, which
           every F103C8 and its clones have), reset_handler, and fault for
           the other 14 Cortex-M exceptions. No interrupts are used, so no
           more entries.
    0x40   blink.o's .text, then its read-only data

The relocating is blink_pico/link.py's (R_ARM_ABS32 and the Thumb calls,
nothing else), reused as it is.
"""
import importlib.util
import os
import struct
import sys

# blink_pico's link.py by its path and under another name: both files are "link"
_spec = importlib.util.spec_from_file_location(
    "pico_link", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "blink_pico", "link.py"))
_pico = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_pico)
link_blob = _pico.link_blob

FLASH = 0x08000000
STACK = 0x20005000
UF2_STM32F1 = 0x5EE21072


def intel_hex(image, base):
    out = []

    def rec(kind, addr, data):
        b = bytes([len(data), addr >> 8 & 255, addr & 255, kind]) + data
        out.append(":" + (b + bytes([-sum(b) & 255])).hex().upper())

    upper = None
    for o in range(0, len(image), 16):
        a = base + o
        if a >> 16 != upper:
            upper = a >> 16
            rec(4, 0, struct.pack(">H", upper))
        rec(0, a & 0xFFFF, image[o:o + 16])
    rec(5, 0, struct.pack(">I", base))          # start address: the vector table's
    rec(1, 0, b"")
    return "\n".join(out) + "\n"


def uf2(image, base):
    out = bytearray()
    n = (len(image) + 255) // 256
    for i in range(n):
        chunk = image[256 * i:256 * (i + 1)].ljust(256, b"\0")
        out += struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, base + 256 * i, 256, i, n, UF2_STM32F1)
        out += chunk + b"\0" * (476 - 256) + struct.pack("<I", 0x0AB16F30)
    return bytes(out)


def main():
    prog_o, outdir, name = sys.argv[1:4]
    code, sym = link_blob(prog_o, FLASH + 0x40)
    for need in ("reset_handler", "fault"):
        if need not in sym:
            sys.exit(f"{prog_o}: no {need}")
    vec = struct.pack("<II", STACK, sym["reset_handler"] | 1) + struct.pack("<I", sym["fault"] | 1) * 14
    image = bytes(vec + code)
    image += b"\xff" * (-len(image) % 4)
    open(f"{outdir}/{name}.bin", "wb").write(image)
    open(f"{outdir}/{name}.hex", "w").write(intel_hex(image, FLASH))
    open(f"{outdir}/{name}.uf2", "wb").write(uf2(image, FLASH))
    print(f"{name}: {len(image)} bytes, reset_handler at 0x{sym['reset_handler']:08x}")


if __name__ == "__main__":
    main()
