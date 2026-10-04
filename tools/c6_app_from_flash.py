#!/usr/bin/env python3
"""The app image inside a dump of the C6's flash (docs/C6.md, "Backup").

    tools/c6_app_from_flash.py c6/backup/c6-factory.bin [out.bin]

Reads the partition table at 0x8000, takes the app slot otadata says is
running (ota_0 when otadata is blank), and cuts the image to its real
length: header, segments, checksum and the SHA-256 when it has one. The
result is what tools/install_c6.sh can send to the C6 through the P4, the
way back to the factory firmware with no cable.
"""
import struct
import sys


def partitions(d):
    out = {}
    for i in range(0x8000, 0x8000 + 0xC00, 32):
        magic, ptype, sub, off, size = struct.unpack_from("<HBBII", d, i)
        if magic != 0x50AA:
            break
        name = d[i + 12:i + 28].split(b"\0")[0].decode()
        out[name] = (ptype, sub, off, size)
    return out


def running_slot(d, parts):
    if "otadata" not in parts:
        return "ota_0"
    off = parts["otadata"][2]
    best = None
    for sector in (off, off + 0x1000):
        seq, = struct.unpack_from("<I", d, sector)
        if seq not in (0, 0xFFFFFFFF) and (best is None or seq > best):
            best = seq
    if best is None:
        return "ota_0"
    slots = sorted(n for n in parts if n.startswith("ota_") and n[4:].isdigit())
    return slots[(best - 1) % len(slots)]


def image_length(d, off):
    if d[off] != 0xE9:
        raise SystemExit("no app image at 0x%x" % off)
    nseg, hashed = d[off + 1], d[off + 23]
    p = off + 24
    for _ in range(nseg):
        _, ln = struct.unpack_from("<II", d, p)
        p += 8 + ln
    p += 1                              # checksum byte, then pad to 16
    while (p - off) % 16:
        p += 1
    if hashed:
        p += 32
    return p - off


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    parts = partitions(d)
    slot = running_slot(d, parts)
    off = parts[slot][2]
    n = image_length(d, off)
    out = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1].replace(".bin", "-app.bin")
    open(out, "wb").write(d[off:off + n])
    print("%s: %s at 0x%x, %d bytes -> %s" % (sys.argv[1], slot, off, n, out))


if __name__ == "__main__":
    main()
