#!/usr/bin/env python3
"""A quick look while drawing: one style's six icons at 120, 108 and 26 (and
the 26 blown up 4x, nearest neighbour, to see its pixels) on a dark wallpaper.

    python3 tools/icons/peek.py a_plano /tmp/peek.png
"""

import os
import sys

from PIL import Image

import common as C


def main(style, dst):
    bg = (40, 32, 74)
    W = 40 + len(C.ALL) * 150
    im = Image.new("RGB", (W, 380), bg)
    for i, (app_id, _n, _i) in enumerate(C.ALL):
        x = 20 + i * 150
        for size, y in ((120, 16), (108, 150)):
            p = Image.open(C.out_path(style, app_id, size)).convert("RGBA")
            im.paste(p, (x + (120 - size) // 2, y), p)
        m = Image.open(C.out_path(style, app_id, 26)).convert("RGBA")
        im.paste(m, (x, 290), m)
        big = m.resize((78, 78), Image.NEAREST)        # 3x, to see the pixels
        im.paste(big, (x + 40, 274), big)
    im.save(dst)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
