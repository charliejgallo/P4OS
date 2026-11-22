#!/usr/bin/env python3
"""Invented PDF files for Office's page drawing (phase 7), each one made to
exercise a part of what a page can paint:

    python3 apps/oficina/tools/samples_draw.py DIR [font.ttf]

  dibujo.pdf      lines (caps, joins, dashes, widths), curves, even-odd and
                  nonzero fills, a clip by a circle, alpha, colour in Gray,
                  RGB, CMYK, Lab, Separation (type 2 function) and DeviceN
                  (type 4 function), an axial shading with "sh", a radial
                  one as a pattern, coloured and uncoloured tiling patterns,
                  a form XObject, an annotation with its appearance, and a
                  Type 3 font
  imagenes.pdf    pictures: Indexed 4 bits, grey 1 bit, RGB with a soft mask,
                  a JPEG (with Pillow), a stencil mask in a colour, an
                  inline picture, colour key masking, a picture turned
  fuentes.pdf     the same words in fonts carried by the file: TrueType
                  (simple, symbolic with a (3,0) cmap, and CIDFontType2 with
                  a CIDToGIDMap), CFF (bare, with /Differences; inside
                  OpenType; as CIDFontType0), Type 1 (eexec, Subrs, seac
                  accents); text modes (stroke, fill and stroke, invisible),
                  rise, horizontal scaling, turned text

The fonts' outlines are borrowed, a few letters, from a TrueType font on
the computer (Arial on the Mac unless one is named): the files stay here,
for the tests. Needs fontTools; Pillow for the JPEG.
"""
import io
import math
import os
import random
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from samples_pdf import Pdf  # noqa: E402

from fontTools.ttLib import TTFont  # noqa: E402
from fontTools.fontBuilder import FontBuilder  # noqa: E402
from fontTools.pens.t2CharStringPen import T2CharStringPen  # noqa: E402
from fontTools.pens.ttGlyphPen import TTGlyphPen  # noqa: E402
from fontTools.misc import eexec  # noqa: E402

WORD = 'Cañón güiro, 123 años'
LETTERS = sorted(set('Canonguiro,123s ') | {'n', 'a', 'o', 'u'})
NAMES = {' ': 'space', ',': 'comma', '1': 'one', '2': 'two', '3': 'three'}


def gname(c):
    return NAMES.get(c, c)


def page_doc(p, contents, res, size=(595, 842), annots=None):
    cs = p.add('<<>>', zlib.compress(contents.encode('latin-1')))
    p.objs[cs] = (b'<</Filter /FlateDecode>>', p.objs[cs][1])
    pages = p.reserve()
    extra = ' /Annots [%s]' % ' '.join('%d 0 R' % a for a in annots) if annots else ''
    pg = p.add('<</Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] /Contents %d 0 R /Resources %s%s>>' % (pages, size[0], size[1], cs, res, extra))
    p.add('<</Type /Pages /Kids [%d 0 R] /Count 1>>' % pg, num=pages)
    return p.add('<</Type /Catalog /Pages %d 0 R>>' % pages)


def fstream(p, d, data, flate=True):
    if flate and not os.environ.get('OF_PLAIN'):
        n = p.add(d[:-2] + ' /Filter /FlateDecode>>', zlib.compress(data))
    else:
        n = p.add(d, data)
    return n


# --------------------------------------------------------------------------
# dibujo.pdf

def dibujo(path):
    p = Pdf()
    c = []
    c.append('BT /F1 18 Tf 50 800 Td (Lineas, colores, sombreados) Tj ET')
    # lines: caps and joins, dashes
    y = 760
    for cap in range(3):
        c.append('%d J 0 j 12 w 0 0 0.6 RG 60 %d m 200 %d l S' % (cap, y - cap * 22, y - cap * 22))
    for join in range(3):
        c.append('2 J %d j 10 w 0.6 0 0 RG %d 700 m %d 740 l %d 700 l S' % (join, 240 + join * 70, 270 + join * 70, 300 + join * 70))
    c.append('0 J 2 w 0 g [8 4 2 4] 0 d 60 690 m 520 690 l S [] 0 d')
    c.append('0.2 w 60 680 m 520 680 l S 0 w 60 675 m 520 675 l S')
    # curves, even-odd and nonzero
    star = '%d %d m %d %d l %d %d l %d %d l %d %d l h'
    def st(cx, cy, r):
        pts = [(cx + r * math.sin(i * 4 * math.pi / 5), cy + r * math.cos(i * 4 * math.pi / 5)) for i in range(5)]
        return star % tuple(v for pt in pts for v in (round(pt[0]), round(pt[1])))
    c.append('0.9 0.6 0 rg ' + st(110, 600, 50) + ' f')
    c.append('0.9 0.6 0 rg ' + st(230, 600, 50) + ' f*')
    c.append('0 0.5 0 RG 3 w 0.7 0.9 0.7 rg 300 560 m 300 660 400 660 400 600 c 400 540 460 560 470 600 c B')
    # a clip by a circle
    k = 0.5523
    circ = '%f %f m ' % (560, 600)
    r, cx, cy = 40, 520, 600
    circ = '%g %g m %g %g %g %g %g %g c %g %g %g %g %g %g c %g %g %g %g %g %g c %g %g %g %g %g %g c h' % (
        cx + r, cy, cx + r, cy + k * r, cx + k * r, cy + r, cx, cy + r, cx - k * r, cy + r, cx - r, cy + k * r, cx - r, cy,
        cx - r, cy - k * r, cx - k * r, cy - r, cx, cy - r, cx + k * r, cy - r, cx + r, cy - k * r, cx + r, cy)
    c.append('q ' + circ + ' W n')
    for i in range(10):
        c.append('%g %g %g rg %d 550 10 100 re f' % (i / 10, 0.3, 1 - i / 10, 475 + i * 10))
    c.append('Q')
    # alpha
    c.append('q /GSa gs 1 0 0 rg 60 430 100 80 re f 0 0 1 rg 110 460 100 80 re f Q')
    # colour spaces
    c.append('0 0 0 1 k 240 470 40 40 re f 1 0 0 0 k 285 470 40 40 re f 0 1 0 0 k 330 470 40 40 re f 0 0 1 0 k 375 470 40 40 re f')
    c.append('/CSlab cs 60 80 -40 sc 420 470 40 40 re f')
    c.append('/CSsep cs 0.3 sc 465 470 40 40 re f /CSsep cs 1 scn 510 470 40 40 re f')
    c.append('/CSdn cs 0.8 0.2 sc 240 425 40 40 re f /CSdn cs 0.1 0.9 sc 285 425 40 40 re f')
    # axial shading with sh, inside a clip
    c.append('q 60 300 200 100 re W n /ShA sh Q')
    # radial as a pattern
    c.append('/Pattern cs /PR scn 290 300 m 490 300 l 490 400 l 290 400 l h f')
    # tiling patterns
    c.append('/Pattern cs /PT scn 60 180 200 100 re f')
    c.append('/CSpu cs 0 0.4 0.8 /PU scn 290 180 200 100 re f')
    c.append('0 0 0 RG 1 w 290 180 200 100 re S')
    # a form, twice
    c.append('q 1 0 0 1 60 60 cm /Fm Do Q q 0.7 0.3 -0.3 0.7 250 60 cm /Fm Do Q')
    # Type 3 text
    c.append('0 0.4 0 rg BT /T3 30 Tf 400 100 Td (abab) Tj ET')
    res_extra = []
    # resources
    font = p.add('<</Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding>>')
    gsa = p.add('<</Type /ExtGState /ca 0.5 /CA 0.5>>')
    sep_fn = p.add('<</FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0.8 0.9 0] /N 1>>')
    dn_fn = p.add('<</FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1]>>', b'{ 2 copy add 2 div 3 1 roll exch }')
    ax_fn = p.add('<</FunctionType 3 /Domain [0 1] /Functions [%d 0 R %d 0 R] /Bounds [0.5] /Encode [0 1 0 1]>>' % (
        p.add('<</FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [1 1 0] /N 1>>'), p.add('<</FunctionType 2 /Domain [0 1] /C0 [1 1 0] /C1 [0 0.6 0] /N 1>>')))
    sha = p.add('<</ShadingType 2 /ColorSpace /DeviceRGB /Coords [60 0 260 0] /Function %d 0 R /Extend [true true]>>' % ax_fn)
    rad_fn = p.add('<</FunctionType 0 /Domain [0 1] /Range [0 1 0 1 0 1] /Size [3] /BitsPerSample 8>>', bytes([255, 255, 255, 0, 120, 255, 0, 0, 80]))
    shr = p.add('<</ShadingType 3 /ColorSpace /DeviceRGB /Coords [390 350 5 390 350 90] /Function %d 0 R /Extend [false true]>>' % rad_fn)
    pr = p.add('<</Type /Pattern /PatternType 2 /Shading %d 0 R>>' % shr)
    tile = b'1 0 0 rg 0 0 10 10 re f 0 0 1 rg 10 10 10 10 re f 0 0.6 0 RG 1 w 0 20 m 20 0 l S'
    pt = p.add('<</Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 20 20] /XStep 20 /YStep 20 /Resources <<>>>>', tile)
    pu = p.add('<</Type /Pattern /PatternType 1 /PaintType 2 /TilingType 1 /BBox [0 0 12 12] /XStep 12 /YStep 12 /Resources <<>>>>',
               b'3 3 6 6 re f')
    form = b'0.95 0.95 0.8 rg 0 0 160 90 re f 0 0 0 RG 2 w 0 0 160 90 re S BT /F1 14 Tf 10 40 Td (Un formulario) Tj ET'
    fm = p.add('<</Type /XObject /Subtype /Form /BBox [0 0 160 90] /Resources <</Font <</F1 %d 0 R>>>>>>' % font, form)
    # Type 3: "a" a triangle, "b" a square
    pa = p.add('<<>>', b'1000 0 0 0 750 750 d1 0 0 m 750 0 l 375 750 l f')
    pb = p.add('<<>>', b'1000 0 d0 0.8 0 0 rg 0 0 750 750 re f')
    t3 = p.add('<</Type /Font /Subtype /Type3 /FontBBox [0 0 750 750] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs <</a %d 0 R /b %d 0 R>> '
               '/Encoding <</Type /Encoding /Differences [97 /a /b]>> /FirstChar 97 /LastChar 98 /Widths [1000 1000] /Resources <<>>>>' % (pa, pb))
    res = ('<</Font <</F1 %d 0 R /T3 %d 0 R>> /ExtGState <</GSa %d 0 R>> /ColorSpace <</CSlab [/Lab <</WhitePoint [0.9505 1 1.089] /Range [-128 127 -128 127]>>] '
           '/CSsep [/Separation /Naranja /DeviceCMYK %d 0 R] /CSdn [/DeviceN [/A /B] /DeviceRGB %d 0 R] /CSpu [/Pattern /DeviceRGB]>> '
           '/Shading <</ShA %d 0 R>> /Pattern <</PR %d 0 R /PT %d 0 R /PU %d 0 R>> /XObject <</Fm %d 0 R>>>>' % (font, t3, gsa, sep_fn, dn_fn, sha, pr, pt, pu, fm))
    # an annotation with its appearance
    ap = p.add('<</Type /XObject /Subtype /Form /BBox [0 0 100 40]>>', b'1 1 0 rg 0 0 100 40 re f 0.5 0 0 RG 3 w 2 2 96 36 re S')
    an = p.add('<</Type /Annot /Subtype /Square /Rect [450 20 550 60] /AP <</N %d 0 R>>>>' % ap)
    root = page_doc(p, '\n'.join(c), res, annots=[an])
    p.write(path, root)


# --------------------------------------------------------------------------
# imagenes.pdf

def imagenes(path):
    p = Pdf()
    c = ['BT /F1 18 Tf 50 800 Td (Imagenes) Tj ET']
    xo = {}
    # Indexed, 4 bits
    w, h = 16, 8
    pal = bytes(v for i in range(16) for v in (i * 16, 255 - i * 16, (i * 37) % 256))
    data = b''.join(bytes(((x // 1) << 4 | (x + 1) % 16) for x in range(0, 16, 2)) for _ in range(h))
    xo['Ix'] = fstream(p, '<</Type /XObject /Subtype /Image /Width %d /Height %d /BitsPerComponent 4 /ColorSpace [/Indexed /DeviceRGB 15 <%s>]>>'
                       % (w, h, pal.hex()), data)
    c.append('q 160 0 0 80 50 680 cm /Ix Do Q')
    # grey, 1 bit: a checkerboard
    w, h = 32, 32
    rows = []
    for y in range(h):
        bits = 0
        for x in range(w):
            bits = bits << 1 | ((x // 4 + y // 4) & 1)
        rows.append(bits.to_bytes(4, 'big'))
    xo['G1'] = fstream(p, '<</Type /XObject /Subtype /Image /Width 32 /Height 32 /BitsPerComponent 1 /ColorSpace /DeviceGray>>', b''.join(rows))
    c.append('q 100 0 0 100 240 660 cm /G1 Do Q')
    # RGB with a soft mask over a stripe
    c.append('0 0 0.8 rg 360 700 220 20 re f')
    w, h = 64, 64
    rgb = bytes(v for y in range(h) for x in range(w) for v in (x * 4, y * 4, 128))
    sm = bytes(int(255 * max(0, 1 - math.hypot(x - 32, y - 32) / 32)) for y in range(h) for x in range(w))
    smn = fstream(p, '<</Type /XObject /Subtype /Image /Width 64 /Height 64 /BitsPerComponent 8 /ColorSpace /DeviceGray>>', sm)
    xo['Rs'] = fstream(p, '<</Type /XObject /Subtype /Image /Width 64 /Height 64 /BitsPerComponent 8 /ColorSpace /DeviceRGB /SMask %d 0 R>>' % smn, rgb)
    c.append('q 120 0 0 120 400 650 cm /Rs Do Q')
    # a JPEG
    try:
        from PIL import Image
        im = Image.new('RGB', (200, 120))
        px = im.load()
        for y in range(120):
            for x in range(200):
                px[x, y] = (x, 255 - y * 2, (x + y) % 256)
        b = io.BytesIO()
        im.save(b, 'JPEG', quality=85)
        xo['Jp'] = p.add('<</Type /XObject /Subtype /Image /Width 200 /Height 120 /BitsPerComponent 8 /ColorSpace /DeviceRGB /Filter /DCTDecode>>', b.getvalue())
        c.append('q 200 0 0 120 50 480 cm /Jp Do Q')
        c.append('q 0 100 -160 0 460 480 cm /Jp Do Q')
    except ImportError:
        pass
    # a stencil mask, in a colour
    w, h = 24, 24
    rows = []
    for y in range(h):
        bits = 0
        for x in range(w):
            inside = math.hypot(x - 11.5, y - 11.5) < 11
            bits = bits << 1 | (0 if inside else 1)
        rows.append(bits.to_bytes(3, 'big'))
    xo['St'] = fstream(p, '<</Type /XObject /Subtype /Image /Width 24 /Height 24 /ImageMask true /BitsPerComponent 1>>', b''.join(rows))
    c.append('0.8 0.1 0.5 rg q 80 0 0 80 280 500 cm /St Do Q')
    # colour key masking: green is transparent
    w, h = 20, 20
    ck = bytes(v for y in range(h) for x in range(w) for v in ((0, 255, 0) if (x - 10) ** 2 + (y - 10) ** 2 < 40 else (200, 50, 50)))
    xo['Ck'] = fstream(p, '<</Type /XObject /Subtype /Image /Width 20 /Height 20 /BitsPerComponent 8 /ColorSpace /DeviceRGB /Mask [0 10 250 255 0 10]>>', ck)
    c.append('0 0 0 rg 380 520 100 10 re f q 100 0 0 100 380 480 cm /Ck Do Q')
    # an inline picture: 4 x 2 grey
    c.append('q 100 0 0 50 50 380 cm BI /W 4 /H 2 /BPC 8 /CS /G ID \x00\x55\xaa\xff\xff\xaa\x55\x00 EI Q')
    c.append('BT /F1 10 Tf 50 365 Td (inline) Tj ET')
    font = p.add('<</Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding>>')
    res = '<</Font <</F1 %d 0 R>> /XObject <<%s>>>>' % (font, ' '.join('/%s %d 0 R' % kv for kv in xo.items()))
    root = page_doc(p, '\n'.join(c), res)
    p.write(path, root)


# --------------------------------------------------------------------------
# fuentes.pdf

GSET = [None]


def load_glyphs(ttf):
    f = TTFont(ttf)
    gs = f.getGlyphSet()
    GSET[0] = gs
    cmap = f.getBestCmap()
    upem = f['head'].unitsPerEm
    out = {}
    for ch in set(WORD) | {'a', 'o', 'n', 'u'}:
        g = cmap.get(ord(ch))
        if g:
            out[ch] = (gs[g], gs[g].width * 1000 // upem)
    acute = cmap.get(0xB4)
    return f, gs, out, upem, (gs[acute] if acute else None)


class T1Pen:
    """a Type 1 charstring program: hsbw, then relative moves, lines and curves"""
    def __init__(self, width):
        self.prog = [0, width, 'hsbw']
        self.x = self.y = 0

    def _rel(self, p):
        dx, dy = p[0] - self.x, p[1] - self.y
        self.x, self.y = p
        return [dx, dy]

    def moveTo(self, p): self.prog += self._rel(p) + ['rmoveto']
    def lineTo(self, p): self.prog += self._rel(p) + ['rlineto']
    def curveTo(self, a, b, c): self.prog += self._rel(a) + self._rel(b) + self._rel(c) + ['rrcurveto']
    def closePath(self): self.prog.append('closepath')
    def endPath(self): pass

    def program(self):
        return self.prog + ['endchar']


def draw_into(glyph, pen, k):
    from fontTools.pens.basePen import BasePen

    class Cubic(BasePen):
        def __init__(self, out):
            super().__init__(GSET[0])
            self.out = out
        def _moveTo(self, p): self.out.moveTo((round(p[0] * k), round(p[1] * k)))
        def _lineTo(self, p): self.out.lineTo((round(p[0] * k), round(p[1] * k)))
        def _curveToOne(self, a, b, c): self.out.curveTo((round(a[0] * k), round(a[1] * k)), (round(b[0] * k), round(b[1] * k)), (round(c[0] * k), round(c[1] * k)))
        def _closePath(self): self.out.closePath()
        def _endPath(self): self.out.endPath()
    glyph.draw(Cubic(pen))


def make_cff(glyphs, upem, cid=False):
    chars = sorted(glyphs)
    order = ['.notdef'] + [gname(ch) for ch in chars]
    k = 1000 / upem
    css = {}
    for nm in order:
        pen = T2CharStringPen(500, None)
        if nm != '.notdef':
            ch = [c for c in chars if gname(c) == nm][0]
            draw_into(glyphs[ch][0], pen, k)
            css[nm] = pen.getCharString()
        else:
            css[nm] = pen.getCharString()
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({ord(c): gname(c) for c in chars})
    fb.setupCFF('ABCDEF+Prueba', {'FullName': 'Prueba'}, css, {})
    fb.setupHorizontalMetrics({nm: (glyphs[[c for c in chars if gname(c) == nm][0]][1] if nm != '.notdef' else 500, 0) for nm in order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'Prueba', 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    b = io.BytesIO()
    fb.save(b)
    otf = b.getvalue()
    f = TTFont(io.BytesIO(otf))
    return otf, f.reader['CFF '], order


def make_tt(glyphs, upem, symbolic=False):
    chars = sorted(glyphs)
    order = ['.notdef'] + [gname(ch) for ch in chars]
    k = 1000 / upem
    gl = {}
    for nm in order:
        pen = TTGlyphPen(None)
        if nm != '.notdef':
            ch = [c for c in chars if gname(c) == nm][0]
            from fontTools.pens.cu2quPen import Cu2QuPen
            draw_into(glyphs[ch][0], Cu2QuPen(pen, 1.0), k)
        gl[nm] = pen.glyph()
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    if symbolic:
        fb.setupCharacterMap({0xF000 + ord(c): gname(c) for c in chars})
        fb.font['cmap'].tables[0].platEncID = 0 if fb.font['cmap'].tables[0].platformID == 3 else fb.font['cmap'].tables[0].platEncID
        for t in fb.font['cmap'].tables:
            if t.platformID == 3:
                t.platEncID = 0
    else:
        fb.setupCharacterMap({ord(c): gname(c) for c in chars})
    fb.setupGlyf(gl)
    fb.setupHorizontalMetrics({nm: (glyphs[[c for c in chars if gname(c) == nm][0]][1] if nm != '.notdef' else 500, 0) for nm in order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'Prueba', 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    b = io.BytesIO()
    fb.save(b)
    return b.getvalue(), order


def make_t1(glyphs, upem, acute):
    """A Type 1 font: clear part, eexec part with Subrs and CharStrings"""
    k = 1000 / upem
    progs = {}
    for ch, (g, w) in glyphs.items():
        pen = T1Pen(w)
        draw_into(g, pen, k)
        progs[gname(ch)] = pen.program()
    if acute is not None:
        pen = T1Pen(333)
        draw_into(acute, pen, k)
        progs['acute'] = pen.program()
        # aacute and oacute by seac: asb adx ady bchar achar
        progs['aacute'] = [0, glyphs['a'][1], 'hsbw', 0, 110, 0, 97, 194, 'seac']
        progs['oacute'] = [0, glyphs['o'][1], 'hsbw', 0, 110, 0, 111, 194, 'seac']
    # 'o' through a subroutine
    o = progs['o']
    i = o.index('hsbw') + 1
    subr4 = o[i:-1] + ['return']
    progs['o'] = o[:i] + [4, 'callsubr', 'endchar']
    from fontTools.misc.psCharStrings import T1CharString
    def enc_cs(prog):
        cs = T1CharString(program=prog)
        cs.compile()
        data = bytes(4) + cs.bytecode
        return eexec.encrypt(data, 4330)[0]
    subrs = [[3, 0, 'callothersubr', 'pop', 'pop', 'setcurrentpoint', 'return'], [0, 1, 'callothersubr', 'return'],
             [0, 2, 'callothersubr', 'return'], ['return'], subr4]
    priv = b'dup /Private 8 dict dup begin /RD{string currentfile exch readstring pop}executeonly def /ND{noaccess def}executeonly def ' \
           b'/NP{noaccess put}executeonly def /lenIV 4 def /BlueValues [] def /MinFeature {16 16} def /password 5839 def\n'
    priv += b'/Subrs %d array\n' % len(subrs)
    for n, s in enumerate(subrs):
        e = enc_cs(s)
        priv += b'dup %d %d RD ' % (n, len(e)) + e + b' NP\n'
    priv += b'ND\n2 index /CharStrings %d dict dup begin\n' % (len(progs) + 1)
    for nm, prog in [('.notdef', [0, 500, 'hsbw', 'endchar'])] + sorted(progs.items()):
        e = enc_cs(prog)
        priv += b'/%s %d RD ' % (nm.encode(), len(e)) + e + b' ND\n'
    priv += b'end\nend\nreadonly put\nnoaccess put\ndup /FontName get exch definefont pop\nmark currentfile closefile\n'
    enc = eexec.encrypt(bytes(4) + priv, 55665)[0]
    clear = (b'%!PS-AdobeFont-1.0: Prueba-Uno 001.000\n11 dict begin\n/FontInfo 2 dict dup begin /FullName (Prueba Uno) readonly def end readonly def\n'
             b'/FontName /ABCDEF+PruebaUno def\n/Encoding StandardEncoding def\n/PaintType 0 def\n/FontType 1 def\n'
             b'/FontMatrix [0.001 0 0 0.001 0 0] readonly def\n/FontBBox {0 -200 1000 900} readonly def\ncurrentdict end\ncurrentfile eexec\n')
    tail = (b'0' * 64 + b'\n') * 8 + b'cleartomark\n'
    return clear, enc, tail


def fuentes(path, ttf):
    f, gs, glyphs, upem, acute = load_glyphs(ttf)
    p = Pdf()
    c = []
    res = {}
    win = WORD.encode('cp1252')
    widths = lambda: ' '.join(str(glyphs[chr(i)][1]) if chr(i) in glyphs else '0' for i in range(32, 256))
    lbl = p.add('<</Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding>>')
    res['L'] = lbl
    y = 790

    def line(label, font, size, text, extra=''):
        nonlocal y
        c.append('BT /L 9 Tf 40 %d Td (%s) Tj ET' % (y + 12, label))
        c.append('BT %s /%s %d Tf 40 %d Td %s Tj ET' % (extra, font, size, y - 14, text))
        y -= 62

    def hexs(b):
        return '<%s>' % b.hex()
    # TrueType, simple (WinAnsi; the accented letters are not in the font: they fall back)
    ttf_data, order = make_tt(glyphs, upem)
    ff2 = fstream(p, '<<>>', ttf_data)
    desc = p.add('<</Type /FontDescriptor /FontName /ABCDEF+PruebaTT /Flags 32 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 /FontFile2 %d 0 R>>' % ff2)
    res['TT'] = p.add('<</Type /Font /Subtype /TrueType /BaseFont /ABCDEF+PruebaTT /FirstChar 32 /LastChar 255 /Widths [%s] /Encoding /WinAnsiEncoding /FontDescriptor %d 0 R>>' % (widths(), desc))
    line('TrueType (FontFile2), WinAnsi', 'TT', 26, hexs(win))
    # TrueType symbolic: (3,0) cmap at F0xx
    sym_data, _ = make_tt(glyphs, upem, symbolic=True)
    ff2s = fstream(p, '<<>>', sym_data)
    descs = p.add('<</Type /FontDescriptor /FontName /ABCDEF+PruebaSym /Flags 4 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 /FontFile2 %d 0 R>>' % ff2s)
    res['TS'] = p.add('<</Type /Font /Subtype /TrueType /BaseFont /ABCDEF+PruebaSym /FirstChar 32 /LastChar 255 /Widths [%s] /FontDescriptor %d 0 R>>' % (widths(), descs))
    line('TrueType simbolica, cmap (3,0)', 'TS', 26, hexs(b'Canon guiro, 123 anos'))
    # CIDFontType2, Identity-H, with a CIDToGIDMap that moves the glyphs by 100
    cids = []
    gmap = bytearray(2 * 200)
    for ch in WORD:
        nm = gname(ch) if ch in glyphs else '.notdef'
        gid = order.index(nm) if nm in order else 0
        cids.append(100 + gid)
        gmap[2 * (100 + gid)] = gid >> 8
        gmap[2 * (100 + gid) + 1] = gid & 255
    c2g = fstream(p, '<<>>', bytes(gmap))
    descc = p.add('<</Type /FontDescriptor /FontName /ABCDEF+PruebaCID /Flags 4 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 /FontFile2 %d 0 R>>' % ff2)
    wlist = ' '.join('%d [%d]' % (100 + order.index(gname(ch)), glyphs[ch][1]) for ch in sorted(glyphs))
    cidf = p.add('<</Type /Font /Subtype /CIDFontType2 /BaseFont /ABCDEF+PruebaCID /CIDSystemInfo <</Registry (Adobe) /Ordering (Identity) /Supplement 0>> /FontDescriptor %d 0 R /DW 500 /W [%s] /CIDToGIDMap %d 0 R>>' % (descc, wlist, c2g))
    res['TC'] = p.add('<</Type /Font /Subtype /Type0 /BaseFont /ABCDEF+PruebaCID /Encoding /Identity-H /DescendantFonts [%d 0 R]>>' % cidf)
    line('CIDFontType2, Identity-H, CIDToGIDMap', 'TC', 26, hexs(b''.join(x.to_bytes(2, 'big') for x in cids)))
    # CFF, bare, with Differences on codes 1..
    otf, cff, corder = make_cff(glyphs, upem)
    ff3 = fstream(p, '<</Subtype /Type1C>>', cff)
    desc3 = p.add('<</Type /FontDescriptor /FontName /ABCDEF+Prueba /Flags 4 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 /FontFile3 %d 0 R>>' % ff3)
    names = corder[1:]
    diffs = '1 ' + ' '.join('/' + n for n in names)
    codes = bytes(1 + names.index(gname(ch)) if ch in glyphs else 32 for ch in WORD)
    wd = ' '.join(str(glyphs[[c for c in glyphs if gname(c) == n][0]][1]) for n in names)
    res['CF'] = p.add('<</Type /Font /Subtype /Type1 /BaseFont /ABCDEF+Prueba /FirstChar 1 /LastChar %d /Widths [%s] /Encoding <</Differences [%s]>> /FontDescriptor %d 0 R>>' % (len(names), wd, diffs, desc3))
    line('CFF (FontFile3 /Type1C), /Differences', 'CF', 26, hexs(codes))
    # the same font inside OpenType, StandardEncoding
    ff3o = fstream(p, '<</Subtype /OpenType>>', otf)
    desc3o = p.add('<</Type /FontDescriptor /FontName /ABCDEF+PruebaOT /Flags 32 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 /FontFile3 %d 0 R>>' % ff3o)
    res['OT'] = p.add('<</Type /Font /Subtype /Type1 /BaseFont /ABCDEF+PruebaOT /FirstChar 32 /LastChar 255 /Widths [%s] /Encoding /WinAnsiEncoding /FontDescriptor %d 0 R>>' % (widths(), desc3o))
    line('OpenType con CFF (FontFile3 /OpenType)', 'OT', 26, hexs(win))
    # CIDFontType0 over the bare CFF: CID = glyph
    gids = [corder.index(gname(ch)) if ch in glyphs else 0 for ch in WORD]
    desc0 = p.add('<</Type /FontDescriptor /FontName /ABCDEF+Prueba0 /Flags 4 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 /FontFile3 %d 0 R>>'
                  % fstream(p, '<</Subtype /CIDFontType0C>>', cff))
    w0 = ' '.join('%d [%d]' % (corder.index(gname(ch)), glyphs[ch][1]) for ch in sorted(glyphs))
    cid0 = p.add('<</Type /Font /Subtype /CIDFontType0 /BaseFont /ABCDEF+Prueba0 /CIDSystemInfo <</Registry (Adobe) /Ordering (Identity) /Supplement 0>> /FontDescriptor %d 0 R /DW 500 /W [%s]>>' % (desc0, w0))
    res['C0'] = p.add('<</Type /Font /Subtype /Type0 /BaseFont /ABCDEF+Prueba0 /Encoding /Identity-H /DescendantFonts [%d 0 R]>>' % cid0)
    line('CIDFontType0 (CFF), Identity-H', 'C0', 26, hexs(b''.join(x.to_bytes(2, 'big') for x in gids)))
    # Type 1, with seac accents (a and o acute, through StandardEncoding's acute)
    clear, enc, tail = make_t1(glyphs, upem, acute)
    ff1 = p.add('<</Length1 %d /Length2 %d /Length3 %d>>' % (len(clear), len(enc), len(tail)), clear + enc + tail)
    desc1 = p.add('<</Type /FontDescriptor /FontName /ABCDEF+PruebaUno /Flags 34 /FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80 /FontFile %d 0 R>>' % ff1)
    t1w = ' '.join(str(glyphs[chr(i)][1]) if chr(i) in glyphs else (str(glyphs['a'][1]) if i in (0xE1, 0xF3) else '0') for i in range(32, 256))
    res['T1'] = p.add('<</Type /Font /Subtype /Type1 /BaseFont /ABCDEF+PruebaUno /FirstChar 32 /LastChar 255 /Widths [%s] /Encoding /WinAnsiEncoding /FontDescriptor %d 0 R>>' % (t1w, desc1))
    line('Type 1 (FontFile, eexec, Subrs, seac)', 'T1', 26, hexs('Cañón güiro, 123 años á ó'.encode('cp1252')))
    # text modes, rise, scaling, turned
    c.append('BT /L 9 Tf 40 %d Td (Modos: contorno, relleno y contorno, invisible; subida; escala 150%%; girado) Tj ET' % (y + 12))
    c.append('BT /TT 26 Tf 1 Tr 0.8 0 0 RG 40 %d Td %s Tj 2 Tr 0 0 0.6 RG 0.9 0.9 0 rg 140 0 Td %s Tj 3 Tr 140 0 Td (oculto) Tj ET' % (y - 14, hexs(b'Canon'), hexs(b'guiro')))
    c.append('BT /TT 20 Tf 0 Tr 0 g 40 %d Td %s Tj 8 Ts %s Tj 0 Ts 150 Tz %s Tj ET' % (y - 70, hexs(b'123'), hexs(b'Ca'), hexs(b'nos')))
    c.append('BT /TT 22 Tf 0.866 0.5 -0.5 0.866 380 %d Tm %s Tj ET' % (y - 90, hexs(b'giro, 123')))
    res_s = '<</Font <<%s>>>>' % ' '.join('/%s %d 0 R' % kv for kv in res.items())
    root = page_doc(p, '\n'.join(c), res_s)
    p.write(path, root)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    ttf = sys.argv[2] if len(sys.argv) > 2 else '/System/Library/Fonts/Supplemental/Arial.ttf'
    random.seed(7)
    dibujo(os.path.join(d, 'dibujo.pdf'))
    imagenes(os.path.join(d, 'imagenes.pdf'))
    if os.path.exists(ttf):
        fuentes(os.path.join(d, 'fuentes.pdf'), ttf)
    else:
        print('no font to borrow outlines from: fuentes.pdf skipped')


if __name__ == '__main__':
    main()
