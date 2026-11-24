#!/usr/bin/env python3
"""Style B, second half: Blender's 4x renders -> the icon sizes.

    python3 tools/icons/style_b_post.py

muestra/b_chubby/raw/<id>_big.png (480 px) -> <id>_120.png, <id>_108.png
muestra/b_chubby/raw/<id>_mini.png (104 px) -> <id>_26.png

Shrinking is done with premultiplied alpha (Pillow's "RGBa"), or the
transparent corners bleed a dark fringe into the tile's edge. The 26 gets a
light unsharp mask so the toy's outline survives the 4x shrink.
"""

import os

from PIL import Image, ImageFilter

import common as C

STYLE = "b_chubby"
RAW = os.path.join(C.OUT, STYLE, "raw")


def shrink(src, size, sharpen=False):
    im = Image.open(src)
    if im.mode == "I;16" or im.mode.startswith("I"):
        im = im.convert("RGBA")
    im = im.convert("RGBA").convert("RGBa")
    im = im.resize((size, size), Image.LANCZOS, reducing_gap=None)
    im = im.convert("RGBA")
    if sharpen:
        im = im.filter(ImageFilter.UnsharpMask(radius=0.8, percent=60, threshold=1))
    return im


def main():
    for app_id, _n, _i in C.ALL:
        big = os.path.join(RAW, f"{app_id}_big.png")
        mini = os.path.join(RAW, f"{app_id}_mini.png")
        if not os.path.exists(big):
            print("missing", big)
            continue
        for size in (120, 108):
            shrink(big, size).save(C.out_path(STYLE, app_id, size))
        shrink(mini, 26, sharpen=True).save(C.out_path(STYLE, app_id, 26))
    print("ok:", os.path.join(C.OUT, STYLE))


if __name__ == "__main__":
    main()
