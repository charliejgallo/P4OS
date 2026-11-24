#!/usr/bin/env python3
"""The HD icon pack: the apps' pictures into the card's /icons/hd.pak.

    tools/icons/mkpack.py <dir> [out.pak]        (out: build/icons/hd.pak)
    tools/icons/mkpack.py --list <pack>          what a pack holds

<dir> holds one picture per app, named by the app's id (aos.clock,
tanques, ...):

    <id>.png         the master, square and at least 120 px: it is scaled
                     to every size below that has no picture of its own
    <id>@<N>.png     a picture made for that size, used as it is (N x N);
                     the folders' miniature, <id>@26.png, should be one,
                     drawn simpler, rather than the master scaled down

SIZES are what the system shows (docs/plan/ICONOS.md): 120 the home screen
upright (AOS_UI_ICON), 108 lying down (AOS_UI_ICON_LAND) and 26 a folder's
miniature. A picture at another size is kept as well: the board takes the
nearest at or above the size it needs and scales it down.

The pictures are RGBA, transparent outside the icon's shape: the board
draws them as they are, with no squircle of its own behind them.

The layout, little-endian (components/aos_ui/include/aos_icon_pack.h reads
it):

    "AICP"  u16 version (1)  u16 count  u32 0  u32 0         16 bytes
    count entries, sorted by id and then size:               48 bytes each
        char id[36] (NUL-padded)  u16 size  u16 0  u32 off  u32 len
    the pictures: QOI (tools/gen_emoji.py's encoder), at off from the start

The pack is not kept in git: it is built from the sources and goes out
with a release as a part of the system, like the emoji.
"""
import os
import re
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from gen_emoji import qoi_encode  # noqa: E402

MAGIC = b"AICP"
VERSION = 1
HEAD = 16
ENTRY = 48
ID_MAX = 36
SIZES = (120, 108, 26)
MAX_SIDE = 256                      # the board refuses bigger pictures
NAME = re.compile(r"^([A-Za-z0-9_.\-]+?)(?:@(\d+))?\.png$")


def load(path, size=None):
    img = Image.open(path).convert("RGBA")
    if img.width != img.height:
        sys.exit(f"{path}: {img.width}x{img.height}, not square")
    if size and img.size != (size, size):
        if size > img.width:
            sys.exit(f"{path}: {img.width} px, smaller than {size}")
        img = img.resize((size, size), Image.LANCZOS)
    return img


def collect(src):
    masters, own = {}, {}
    for name in sorted(os.listdir(src)):
        m = NAME.match(name)
        if not m:
            if name.lower().endswith(".png"):
                sys.exit(f"{name}: not <id>.png nor <id>@<size>.png")
            continue
        app, size = m.group(1), m.group(2)
        if len(app.encode()) > ID_MAX:
            sys.exit(f"{name}: the id is longer than {ID_MAX} bytes")
        path = os.path.join(src, name)
        if size:
            n = int(size)
            if not 8 <= n <= MAX_SIDE:
                sys.exit(f"{name}: {n} px, not between 8 and {MAX_SIDE}")
            img = load(path)
            if img.width != n:
                sys.exit(f"{name}: {img.width} px, the name says {n}")
            own[(app, n)] = img
        else:
            masters[app] = path
    pics = dict(own)
    for app, path in masters.items():
        for n in SIZES:
            if (app, n) not in pics:
                pics[(app, n)] = load(path, n)
    return pics


def build(src, out):
    pics = collect(src)
    if not pics:
        sys.exit(f"{src}: no pictures")
    keys = sorted(pics, key=lambda k: (k[0].encode(), k[1]))
    blobs = [qoi_encode(pics[k]) for k in keys]
    off = HEAD + ENTRY * len(keys)
    index = bytearray()
    for (app, n), q in zip(keys, blobs):
        index += struct.pack("<36sHHII", app.encode(), n, 0, off, len(q))
        off += len(q)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<HHII", VERSION, len(keys), 0, 0))
        f.write(index)
        for q in blobs:
            f.write(q)
    apps = len({k[0] for k in keys})
    print(f"{out}: {apps} apps, {len(keys)} pictures, {off / 1024:.0f} KB")
    missing = [a for a in sorted({k[0] for k in keys})
               if not all((a, n) in pics for n in SIZES)]
    for a in missing:
        have = sorted(n for (b, n) in pics if b == a)
        print(f"  {a}: only {have}; the board scales the nearest bigger one")


def show(path):
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != MAGIC:
        sys.exit(f"{path}: not an icon pack")
    ver, count = struct.unpack_from("<HH", b, 4)
    print(f"{path}: version {ver}, {count} pictures, {len(b) / 1024:.0f} KB")
    for i in range(count):
        app, n, _, off, ln = struct.unpack_from("<36sHHII", b, HEAD + i * ENTRY)
        name = app.rstrip(b"\0").decode()
        print(f"  {name:36} {n:4} px {ln:7} B")


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "--list":
        show(a[1])
    elif 1 <= len(a) <= 2 and not a[0].startswith("-"):
        build(a[0], a[1] if len(a) == 2 else os.path.join("build", "icons", "hd.pak"))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
