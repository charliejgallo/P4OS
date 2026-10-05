#!/usr/bin/env python3
"""DIBUJO - decodes the harness's PNGs with Python's zlib and compares them
with the raw rows the engine composed (dib_harness.c writes both).

    python3 apps/dibujo/tools/dib_pngcheck.py /tmp/dib
"""
import struct
import sys
import zlib


def decode(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'signature'
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        crc = struct.unpack('>I', data[pos + 8 + n:pos + 12 + n])[0]
        assert zlib.crc32(typ + body) & 0xFFFFFFFF == crc, 'crc of ' + typ.decode()
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype == 6, 'not RGBA8'
        elif typ == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
            else:
                assert f == 0, 'filter %d' % f
        out += line
        prev = line
    return w, h, bytes(out)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '/tmp/dib'
    bad = 0
    for name in ('prueba', 'transparente'):
        w, h, px = decode('%s/%s.png' % (d, name))
        want = open('%s/%s.rgba' % (d, name), 'rb').read()
        if px != want:
            bad += 1
            diff = next(i for i in range(len(px)) if px[i] != want[i])
            print('%s: differs at byte %d (pixel %d, %d)' % (name, diff, (diff // 4) % w, diff // 4 // w))
        else:
            print('%s: %dx%d identical' % (name, w, h))
    sys.exit(1 if bad else 0)


main()
