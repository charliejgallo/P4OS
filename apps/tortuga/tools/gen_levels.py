#!/usr/bin/env python3
"""Tortuga - the game's levels into the app: levels/mundos.tl becomes
main/tg_levels_data.c, the text as it is (the app parses it at start,
with the same parser that reads a world from the card).

    python3 apps/tortuga/tools/gen_levels.py

Run it after changing levels/mundos.tl, and tools/tg_ref.py check before:
the C core and the model have to agree on every level."""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'levels', 'mundos.tl')
OUT = os.path.join(HERE, '..', 'main', 'tg_levels_data.c')


def main():
    data = open(SRC, encoding='utf-8').read().encode('utf-8')
    lines = []
    for raw in data.split(b'\n'):
        s = ''
        for b in raw + b'\n':
            if b == 0x5c:
                s += '\\\\'
            elif b == 0x22:
                s += '\\"'
            elif b == 0x0a:
                s += '\\n'
            elif 32 <= b < 127:
                s += chr(b)
            else:
                s += '\\%03o' % b
        lines.append('    "%s"' % s)
    with open(OUT, 'w') as f:
        f.write('/* Made by tools/gen_levels.py from levels/mundos.tl: do not edit. */\n')
        f.write('#include "tg_app.h"\n\n')
        f.write('const char tg_levels_text[] =\n')
        f.write('\n'.join(lines))
        f.write(';\n\nconst unsigned tg_levels_len = sizeof(tg_levels_text) - 1;\n')
    print(OUT, len(data), 'bytes')


main()
