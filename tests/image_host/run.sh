#!/bin/bash
# The board's picture decoder (components/aos_hal/aos_image_p4.c) on the Mac.
#
# ESP-IDF's parts are stand-ins (inc/, fakes.c): the JPEG engine and
# esp_new_jpeg over libjpeg, heap_caps over malloc; libpng is the real one.
# It checks every path - engine, software with scaling, PNG, BMP, a range of
# a file, progressive refused, the colour probe in its three outcomes - and
# compares each result with PIL's own resize.
#
#   tests/image_host/run.sh        (brew: libpng, jpeg-turbo; python: Pillow)
set -e
cd "$(dirname "$0")"
P4=../..
out=$(mktemp -d)
cc -O1 -DAOS_SIM -Iinc -I$P4/components/aos_hal/include -I$P4/components/aos_hal \
   $(pkg-config --cflags libpng) -I/opt/homebrew/include -o "$out/t" main.c fakes.c \
   $P4/components/aos_hal/aos_image_p4.c $(pkg-config --libs libpng) -L/opt/homebrew/lib -ljpeg 2>/dev/null
cd "$out"
python3 - <<'PY'
from PIL import Image, ImageDraw
import random
random.seed(1)
def photo(w, h):
    im = Image.new("RGB", (w, h)); d = ImageDraw.Draw(im)
    for y in range(0, h, 4): d.rectangle([0, y, w, y + 4], fill=(int(255 * y / h), 90, 255 - int(255 * y / h)))
    for _ in range(40):
        x, y, r = random.randrange(w), random.randrange(h), random.randrange(w // 30, w // 8)
        d.ellipse([x - r, y - r, x + r, y + r], fill=(random.randrange(256), random.randrange(256), random.randrange(256)))
    return im
photo(4032, 3024).save("big.jpg", quality=88)
photo(1600, 1200).save("mid.jpg", quality=90)
photo(1600, 1200).save("prog.jpg", quality=90, progressive=True)
photo(100, 80).save("small.jpg", quality=95)
photo(500, 400).save("flat.png")
photo(300, 200).save("pic.bmp")
photo(1000, 1000).save("cover.jpg", quality=90)
open("song.mp3", "wb").write(b"ID3" + b"\0" * 997 + open("cover.jpg", "rb").read() + b"\xff\xfb" * 500)
PY
cover=$(stat -f %z cover.jpg)
run() { "./t" "$@" | grep RESULT; }
run big.jpg 0 0 176 176 0 big_fit.png
run big.jpg 0 0 720 1280 1 big_fill.png
run mid.jpg 0 0 176 176 1 mid_fill.png
run mid.jpg 0 0 720 1280 0 mid_fit.png
run small.jpg 0 0 300 300 1 small_fill.png
run flat.png 0 0 200 200 0 png_fit.png
run pic.bmp 0 0 150 150 0 bmp_fit.png
run song.mp3 1000 "$cover" 300 300 1 range.png
! run prog.jpg 0 0 176 176 0 prog.png
FAKE_ENGINE=rgb_ok run mid.jpg 0 0 176 176 1 rgb.png
FAKE_ENGINE=swapped run mid.jpg 0 0 176 176 1 swap.png
FAKE_ENGINE=dead run mid.jpg 0 0 176 176 1 dead.png
python3 - <<'PY'
from PIL import Image, ImageChops, ImageStat
import math, sys
def ref(src, mw, mh, fill):
    im = Image.open(src).convert("RGB"); w, h = im.size
    if not fill:
        s = min(mw / w, mh / h, 1); return im.resize((round(w * s), round(h * s)), Image.BOX)
    if w * mh > h * mw: cw = h * mw // mh; im = im.crop(((w - cw) // 2, 0, (w - cw) // 2 + cw, h))
    else: ch = w * mh // mw; im = im.crop((0, (h - ch) // 2, w, (h - ch) // 2 + ch))
    return im.resize((mw, mh), Image.BILINEAR if im.size[0] < mw else Image.BOX)
def psnr(a, b):
    m = sum(x * x for x in ImageStat.Stat(ImageChops.difference(a, b)).rms) / 3
    return 99 if m == 0 else 20 * math.log10(255 / math.sqrt(m))
bad = 0
# the software path's stand-in shrinks by nearest neighbour, hence its lower floor
for src, mw, mh, fill, out, floor in [("big.jpg",176,176,0,"big_fit",24), ("big.jpg",720,1280,1,"big_fill",24),
        ("mid.jpg",176,176,1,"mid_fill",32), ("mid.jpg",720,1280,0,"mid_fit",32), ("small.jpg",300,300,1,"small_fill",32),
        ("flat.png",200,200,0,"png_fit",30), ("pic.bmp",150,150,0,"bmp_fit",32), ("cover.jpg",300,300,1,"range",32),
        ("mid.jpg",176,176,1,"rgb",32), ("mid.jpg",176,176,1,"swap",32), ("mid.jpg",176,176,1,"dead",22)]:
    o, r = Image.open(out + ".png").convert("RGB"), ref(src, mw, mh, fill)
    p = psnr(o, r) if o.size == r.size else 0
    ok = o.size == r.size and p >= floor
    bad += not ok
    print("%-10s %s %-11s PSNR %5.1f dB" % (out, "ok  " if ok else "FAIL", o.size, p))
sys.exit(bad)
PY
echo "all good"
