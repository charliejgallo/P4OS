#!/usr/bin/env python3
"""Contact sheet of Chatarra's ASCII art, to look at it without the game.

    python3 tools/hoja.py main/ch_world.c out.png [zoom] [filter-regex]

Reads the palette from main/ch_pixel.c and every `static const char *const
NAME[N] = { ... };` of the given file, and lays them out with their names.
'.' and any character not in the palette are transparent (drawn as a
checkerboard so holes show).
"""
import re, sys, os
from PIL import Image, ImageDraw

AQUI = os.path.dirname(os.path.abspath(__file__))

def paleta():
    src = open(os.path.join(AQUI, '..', 'main', 'ch_pixel.c')).read()
    pal = {}
    for c, h in re.findall(r"\{\s*'(\\.|[^'])',\s*0x([0-9A-Fa-f]{6})\s*\}", src):
        if c.startswith('\\'): c = c[1:]
        pal[c] = tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
    return pal

def sprites(path, filtro=None):
    src = open(path).read()
    out = []
    for m in re.finditer(r'static const char \*const (\w+)\[(\w+)\] = \{(.*?)\n\};', src, re.S):
        if filtro and not re.search(filtro, m.group(1)): continue
        rows = [re.sub(r'\\(.)', r'\1', s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(3))]
        if rows: out.append((m.group(1), rows))
    return out

def pintar(rows, pal, z):
    w = max(len(r) for r in rows); h = len(rows)
    im = Image.new('RGB', (w * z, h * z))
    d = ImageDraw.Draw(im)
    for y, r in enumerate(rows):
        for x in range(w):
            c = r[x] if x < len(r) else '.'
            col = pal.get(c)
            if col is None:
                col = (40, 40, 48) if ((x + y) & 1) else (28, 28, 34)
            d.rectangle([x * z, y * z, x * z + z - 1, y * z + z - 1], fill=col)
    return im

def main():
    path, out = sys.argv[1], sys.argv[2]
    z = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    filtro = sys.argv[4] if len(sys.argv) > 4 else None
    pal = paleta()
    ims = [(n, pintar(r, pal, z)) for n, r in sprites(path, filtro)]
    ancho = 1400
    x = y = 0; alto_fila = 0; pos = []
    for n, im in ims:
        if x + im.width > ancho: x = 0; y += alto_fila + 16; alto_fila = 0
        pos.append((x, y, n, im)); x += im.width + 8; alto_fila = max(alto_fila, im.height)
    hoja = Image.new('RGB', (ancho, y + alto_fila + 16), (12, 12, 16))
    d = ImageDraw.Draw(hoja)
    for x, y, n, im in pos:
        hoja.paste(im, (x, y + 12)); d.text((x, y), n, fill=(200, 200, 200))
    hoja.save(out)

if __name__ == '__main__':
    main()
