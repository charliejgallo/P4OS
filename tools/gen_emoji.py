#!/usr/bin/env python3
"""The colour emoji pack: Noto's images into the card's /fonts/emoji.pak.

    tools/gen_emoji.py <noto-emoji checkout> [out.pak]

<noto-emoji checkout> is github.com/googlefonts/noto-emoji; only 2D/png/72
and third_party/region-flags/png are read, so a sparse checkout does:

    git clone --depth 1 --filter=blob:none --sparse \\
        https://github.com/googlefonts/noto-emoji.git
    cd noto-emoji && git sparse-checkout set 2D/png/72 third_party/region-flags/png

Noto's images are under the Apache licence 2.0 and the flags are in the
public domain (THIRD-PARTY.md). The pack is not kept in git: it is built
here and goes out with the release, like the games' art.

Every emoji becomes a SIZE x SIZE RGBA image encoded as QOI (qoiformat.org):
lossless, about as small as PNG for these, and a decoder of a page that
runs the same on the board and in the simulator. components/aos_ui/
aos_emoji.c reads it and scales each image to the size of the text.

Layout, little-endian:

    header   "AEMJ" u16 version (1) u16 size u32 count u32 index_offset
             u32 data_offset
    index    count entries of ENTRY bytes, sorted by sequence:
             u8 length, u8 0 x3, u32 code points x MAXCP (unused ones 0),
             u32 offset of the image from data_offset, u32 its bytes
    data     the QOI images

The sequences are Noto's: emoji, skin tones, ZWJ joiners and keycaps, with
no variation selectors (U+FE0F), which the reader skips in the text too.
Flags are regional-indicator pairs (US -> U+1F1FA U+1F1F8) and the three
UK subdivision tag sequences.
"""
import os
import struct
import sys

from PIL import Image

SIZE = 48
MAXCP = 10
ENTRY = 4 + 4 * MAXCP + 8

SUBDIVISIONS = {"GB-ENG": "gbeng", "GB-SCT": "gbsct", "GB-WLS": "gbwls"}


def qoi_encode(img):
    """RGBA image -> QOI bytes (the reference encoder, as specified)."""
    w, h = img.size
    px = img.tobytes()
    out = bytearray(b"qoif" + struct.pack(">IIBB", w, h, 4, 0))
    index = [(0, 0, 0, 0)] * 64
    prev = (0, 0, 0, 255)
    run = 0
    n = w * h
    for i in range(n):
        p = tuple(px[i * 4:i * 4 + 4])
        if p == prev:
            run += 1
            if run == 62 or i == n - 1:
                out.append(0xC0 | (run - 1))
                run = 0
            continue
        if run:
            out.append(0xC0 | (run - 1))
            run = 0
        r, g, b, a = p
        h6 = (r * 3 + g * 5 + b * 7 + a * 11) % 64
        if index[h6] == p:
            out.append(h6)
        else:
            index[h6] = p
            if a == prev[3]:
                dr = (r - prev[0] + 128) % 256 - 128
                dg = (g - prev[1] + 128) % 256 - 128
                db = (b - prev[2] + 128) % 256 - 128
                if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                    out.append(0x40 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2))
                elif -32 <= dg <= 31 and -8 <= dr - dg <= 7 and -8 <= db - dg <= 7:
                    out.append(0x80 | (dg + 32))
                    out.append((dr - dg + 8) << 4 | (db - dg + 8))
                else:
                    out += bytes((0xFE, r, g, b))
            else:
                out += bytes((0xFF, r, g, b, a))
        prev = p
    out += b"\x00" * 7 + b"\x01"
    return bytes(out)


def square(path):
    img = Image.open(path).convert("RGBA")
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def flag(path):
    """A flag keeps its proportions, centred in the square."""
    img = Image.open(path).convert("RGBA")
    w, h = img.size
    s = SIZE / max(w, h)
    img = img.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.paste(img, ((SIZE - img.width) // 2, (SIZE - img.height) // 2))
    return out


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    root = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "emoji.pak"
    items = {}

    png = os.path.join(root, "2D", "png", "72")
    for name in os.listdir(png):
        if name.startswith("emoji_u") and name.endswith(".png"):
            seq = tuple(int(x, 16) for x in name[7:-4].split("_") if x != "fe0f")
            items[seq] = (square, os.path.join(png, name))

    flags = os.path.join(root, "third_party", "region-flags", "png")
    for name in os.listdir(flags):
        code = name[:-4]
        if len(code) == 2 and code.isalpha():
            seq = tuple(0x1F1E6 + ord(c) - ord("A") for c in code.upper())
        elif code in SUBDIVISIONS:
            seq = (0x1F3F4,) + tuple(0xE0000 + ord(c) for c in SUBDIVISIONS[code]) + (0xE007F,)
        else:
            continue
        items.setdefault(seq, (flag, os.path.join(flags, name)))

    seqs = sorted(s for s in items if len(s) <= MAXCP)
    index = bytearray()
    data = bytearray()
    for seq in seqs:
        make, path = items[seq]
        q = qoi_encode(make(path))
        cps = list(seq) + [0] * (MAXCP - len(seq))
        index += struct.pack("<B3x" + "I" * MAXCP + "II", len(seq), *cps, len(data), len(q))
        data += q
    header = struct.pack("<4sHHIII", b"AEMJ", 1, SIZE, len(seqs), 20, 20 + len(index))
    with open(out_path, "wb") as f:
        f.write(header + index + data)
    print("%s: %d emoji at %d px, %.1f MB (index %d KB)" % (
        out_path, len(seqs), SIZE, (len(header) + len(index) + len(data)) / 1048576, len(index) // 1024))


if __name__ == "__main__":
    main()
