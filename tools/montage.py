#!/usr/bin/env python3
"""montage.py out.png a.png b.png ... [--cols N] [--scale 0.5]: screenshots side by side, labelled."""
import sys
from PIL import Image, ImageDraw
args = sys.argv[1:]
cols, scale = 4, 0.5
if "--cols" in args:
    i = args.index("--cols"); cols = int(args[i + 1]); del args[i:i + 2]
if "--scale" in args:
    i = args.index("--scale"); scale = float(args[i + 1]); del args[i:i + 2]
out, files = args[0], args[1:]
ims = [Image.open(f).convert("RGB") for f in files]
ims = [im.resize((int(im.width * scale), int(im.height * scale))) for im in ims]
cw = max(im.width for im in ims); ch = max(im.height for im in ims) + 24
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (cw + 8), rows * (ch + 8)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for k, (im, f) in enumerate(zip(ims, files)):
    x, y = (k % cols) * (cw + 8), (k // cols) * (ch + 8)
    sheet.paste(im, (x, y + 24))
    d.text((x + 4, y + 4), f.split("/")[-1], fill=(255, 255, 255))
sheet.save(out)
