#!/usr/bin/env python3
"""The makers of the 802.15.4 radios the Zigbee app hears, by the first three
bytes of their long address (the IEEE's OUI registry), for the card's
/zigbee/oui.bin.

    tools/zigbee/gen_oui.py ~/Downloads/oui.csv [out]   (out: build/zigbee/oui.bin)

oui.csv is the IEEE's public MA-L registry, https://standards-oui.ieee.org/
oui/oui.csv. The site refuses downloads that are not a browser's, so it is
fetched by hand (open the address in a browser) and not by this script.

The layout, little-endian; components/aos_154/aos_154.c reads it:

    "AOUI"  u32 count                                      8 bytes
    count records sorted by prefix, 32 bytes each:
        u8 prefix[3]  u8 len  char name[28] (len bytes, the rest zero)

A binary search with fseek finds one in ~16 reads, so nothing is kept in
memory. The names are shortened (no "Co., Ltd.", "Inc." and the like) to fit
a line of the screen.
"""
import csv
import os
import re
import struct
import sys

REC = 32
NAME = 28
TAIL = re.compile(r"[ ,.]*\b(co\.?,? ?ltd|company limited|corporation|corp|incorporated|inc|ltd|limited|llc|gmbh|"
                  r"s\.?a\.?|ag|b\.?v\.?|oy|ab|s\.?r\.?l|pte|pty|plc|k\.?k)\b\.?$", re.I)


def short(name):
    n = " ".join(name.split())
    n = re.sub(r"\s*\([^)]*\)\s*", " ", n).strip()      # "Telink Semiconductor (Taipei) Co., Ltd."
    for _ in range(3):
        m = TAIL.search(n)
        if not m:
            break
        n = n[:m.start()].rstrip(" ,.")
    b = n.encode("utf-8")[:NAME]
    return b.decode("utf-8", "ignore").encode("utf-8")


def main():
    if not 2 <= len(sys.argv) <= 3:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) == 3 else os.path.join("build", "zigbee", "oui.bin")
    recs = {}
    with open(src, newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            a = (row.get("Assignment") or "").strip().upper()
            if re.fullmatch(r"[0-9A-F]{6}", a):
                recs[bytes.fromhex(a)] = short(row.get("Organization Name") or "?")
    if not recs:
        sys.exit("%s: no OUI found (is it the IEEE's oui.csv?)" % src)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(b"AOUI" + struct.pack("<I", len(recs)))
        for p in sorted(recs):
            n = recs[p]
            f.write(p + bytes([len(n)]) + n.ljust(NAME, b"\0"))
    print("%s: %d makers, %d KB" % (out, len(recs), (8 + REC * len(recs)) // 1024))


if __name__ == "__main__":
    main()
