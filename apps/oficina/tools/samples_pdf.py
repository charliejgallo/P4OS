#!/usr/bin/env python3
"""Invented PDF files for Office's PDF reader (phase 4), written by hand so
that each one exercises a part of the format:

    python3 apps/oficina/tools/samples_pdf.py DIR [--external]

  clasico.pdf     classic xref, standard fonts (WinAnsi and /Differences),
                  two columns, running heads and page numbers, a hyphen at
                  the end of a line, a list, a JPEG (when sips is there) and
                  a Flate RGB picture, TJ with kerning and without spaces
  moderno.pdf     cross-reference stream and object streams, ASCII85 and
                  LZW, a Type0 font with a ToUnicode CMap (bfchar, bfrange)
  girado.pdf      a page with /Rotate 90
  roto.pdf        a wrong startxref and wrong offsets: the reader rebuilds
  clave_rc4.pdf   RC4 128 bits (R3), empty user password, owner "dueño"
  clave_aes.pdf   AES-128 (V4 AESV2), user password "1234"
  clave_256.pdf   AES-256 (R6), user password "abc"

--external also makes, with what the Mac has (each skipped if missing):
quartz.pdf (cupsfilter), cairo.pdf (pango-view), columnas.pdf
(rsvg-convert, an SVG in two columns), escaneo.pdf (a page drawn by
pango-view and read by tesseract: a picture with invisible text) and
imagen.pdf (the same page as a picture only).

Everything is invented. The AES and RC4 here are slow, plain Python, only
to make the tests.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(__file__))
import samples_docs as sd  # noqa: E402

# --------------------------------------------------------------------------
# AES and RC4, just enough to encrypt

SBOX = []


def _tables():
    def gmul(a, b):
        r = 0
        while b:
            if b & 1:
                r ^= a
            a = ((a << 1) ^ (0x1B if a & 0x80 else 0)) & 0xFF
            b >>= 1
        return r
    inv = [0] * 256
    for i in range(1, 256):
        for k in range(1, 256):
            if gmul(i, k) == 1:
                inv[i] = k
                break
    for i in range(256):
        x = s = inv[i]
        for _ in range(4):
            x = ((x << 1) | (x >> 7)) & 0xFF
            s ^= x
        SBOX.append(s ^ 0x63)


def _xt(x):
    return ((x << 1) ^ (0x1B if x & 0x80 else 0)) & 0xFF


def aes_keys(key):
    if not SBOX:
        _tables()
    nk = len(key) // 4
    rounds = nk + 6
    w = [list(key[i * 4:i * 4 + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (rounds + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = [SBOX[t[1]] ^ rcon, SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]
            rcon = _xt(rcon)
        elif nk > 6 and i % nk == 4:
            t = [SBOX[b] for b in t]
        w.append([w[i - nk][k] ^ t[k] for k in range(4)])
    return [sum(w[r * 4:r * 4 + 4], []) for r in range(rounds + 1)]


def aes_block(rk, b):
    s = [b[i] ^ rk[0][i] for i in range(16)]
    for r in range(1, len(rk)):
        t = [SBOX[x] for x in s]
        s = [t[((c + w) % 4) * 4 + w] for c in range(4) for w in range(4)]
        if r != len(rk) - 1:
            n = []
            for c in range(4):
                a0, a1, a2, a3 = s[c * 4:c * 4 + 4]
                n += [_xt(a0) ^ _xt(a1) ^ a1 ^ a2 ^ a3, a0 ^ _xt(a1) ^ _xt(a2) ^ a2 ^ a3,
                      a0 ^ a1 ^ _xt(a2) ^ _xt(a3) ^ a3, _xt(a0) ^ a0 ^ a1 ^ a2 ^ _xt(a3)]
            s = n
        s = [s[i] ^ rk[r][i] for i in range(16)]
    return bytes(s)


def aes_cbc(key, iv, data, pad=True):
    rk = aes_keys(key)
    if pad:
        p = 16 - len(data) % 16
        data += bytes([p]) * p
    out, prev = b'', iv
    for i in range(0, len(data), 16):
        prev = aes_block(rk, bytes(a ^ b for a, b in zip(data[i:i + 16], prev)))
        out += prev
    return out


def rc4(key, data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 255
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for c in data:
        i = (i + 1) & 255
        j = (j + s[i]) & 255
        s[i], s[j] = s[j], s[i]
        out.append(c ^ s[(s[i] + s[j]) & 255])
    return bytes(out)


PAD = bytes.fromhex('28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A')

# --------------------------------------------------------------------------
# A small PDF builder


class Pdf:
    def __init__(self):
        self.objs = {}          # num -> (dict bytes, stream bytes or None)
        self.n = 0
        self.crypt = None       # (kind, key) after encrypt()
        self.id0 = hashlib.md5(b'invented').digest()

    def add(self, body, stream=None, num=None):
        if num is None:
            self.n += 1
            num = self.n
        self.objs[num] = (body if isinstance(body, bytes) else body.encode('latin-1'), stream)
        return num

    def reserve(self):
        self.n += 1
        return self.n

    # -- encryption
    def objkey(self, num):
        kind, key = self.crypt
        if kind == 'aes256':
            return key
        h = hashlib.md5(key + struct.pack('<I', num)[:3] + b'\0\0' + (b'sAlT' if kind == 'aes' else b'')).digest()
        return h[:min(len(key) + 5, 16)]

    def enc(self, num, data):
        if not self.crypt:
            return data
        kind = self.crypt[0]
        k = self.objkey(num)
        if kind == 'rc4':
            return rc4(k, data)
        iv = hashlib.md5(b'iv%d' % num).digest()
        return iv + aes_cbc(k, iv, data)

    def encrypt_r3(self, owner, user=b''):
        P = -3904
        u32 = (user + PAD)[:32]
        h = hashlib.md5((owner + PAD)[:32]).digest()
        for _ in range(50):
            h = hashlib.md5(h[:16]).digest()
        o = u32
        for i in range(20):
            o = rc4(bytes(b ^ i for b in h[:16]), o)
        k = hashlib.md5(u32 + o + struct.pack('<i', P) + self.id0).digest()
        for _ in range(50):
            k = hashlib.md5(k[:16]).digest()
        key = k[:16]
        x = hashlib.md5(PAD + self.id0).digest()
        for i in range(20):
            x = rc4(bytes(b ^ i for b in key), x)
        U = x + bytes(16)
        self.enc_dict = '<</Filter /Standard /V 2 /R 3 /Length 128 /P %d /O <%s> /U <%s>>>' % (P, o.hex(), U.hex())
        self.crypt = ('rc4', key)

    def encrypt_aes(self, user, owner):
        P = -3904
        u32 = (user + PAD)[:32]
        h = hashlib.md5((owner + PAD)[:32]).digest()
        for _ in range(50):
            h = hashlib.md5(h[:16]).digest()
        o = u32
        for i in range(20):
            o = rc4(bytes(b ^ i for b in h[:16]), o)
        k = hashlib.md5(u32 + o + struct.pack('<i', P) + self.id0).digest()
        for _ in range(50):
            k = hashlib.md5(k[:16]).digest()
        key = k[:16]
        x = hashlib.md5(PAD + self.id0).digest()
        for i in range(20):
            x = rc4(bytes(b ^ i for b in key), x)
        U = x + bytes(16)
        self.enc_dict = ('<</Filter /Standard /V 4 /R 4 /Length 128 /P %d /O <%s> /U <%s> '
                         '/CF <</StdCF <</CFM /AESV2 /AuthEvent /DocOpen /Length 16>>>> /StmF /StdCF /StrF /StdCF>>'
                         % (P, o.hex(), U.hex()))
        self.crypt = ('aes', key)

    def encrypt_256(self, user, owner):
        def hash6(pw, salt, udata):
            K = hashlib.sha256(pw + salt + udata).digest()
            i = 0
            while True:
                K1 = (pw + K + udata) * 64
                E = aes_cbc(K[:16], K[16:32], K1, pad=False)
                m = sum(E[:16]) % 3
                K = [hashlib.sha256, hashlib.sha384, hashlib.sha512][m](E).digest()
                i += 1
                if i >= 64 and E[-1] <= i - 32:
                    break
            return K[:32]
        key = hashlib.sha256(b'file key').digest()
        uvs, uks, ovs, oks = b'uvsalt01', b'uksalt01', b'ovsalt01', b'oksalt01'
        U = hash6(user, uvs, b'') + uvs + uks
        UE = aes_cbc(hash6(user, uks, b''), bytes(16), key, pad=False)
        O = hash6(owner, ovs, U) + ovs + oks
        OE = aes_cbc(hash6(owner, oks, U), bytes(16), key, pad=False)
        self.enc_dict = ('<</Filter /Standard /V 5 /R 6 /Length 256 /P -3904 /O <%s> /U <%s> /OE <%s> /UE <%s> '
                         '/Perms <%s> /CF <</StdCF <</CFM /AESV3 /AuthEvent /DocOpen /Length 32>>>> /StmF /StdCF /StrF /StdCF>>'
                         % (O.hex(), U.hex(), OE.hex(), UE.hex(), bytes(16).hex()))
        self.crypt = ('aes256', key)

    # -- strings inside dictionaries: <hex> literals marked with \x01...\x02
    def strings(self, num, body):
        out = b''
        while b'\x01' in body:
            a = body.index(b'\x01')
            z = body.index(b'\x02', a)
            out += body[:a] + b'<' + self.enc(num, body[a + 1:z]).hex().encode() + b'>'
            body = body[z + 1:]
        return out + body

    def write(self, path, root, info=None, xref_stream=False, objstm=(), broken=False):
        out = b'%PDF-1.7\n%\xe2\xe3\xcf\xd3\n'
        offs = {}
        enc_num = None
        if self.crypt:
            enc_num = self.add(self.enc_dict)
        # objects inside object streams
        inside = {}
        if objstm:
            head, data = b'', b''
            for k, num in enumerate(objstm):
                body = self.strings(num, self.objs[num][0])
                head += b'%d %d ' % (num, len(data))
                data += body + b'\n'
                inside[num] = k
            stm = self.reserve()
            payload = zlib.compress(head + data)
            self.objs[stm] = (b'<</Type /ObjStm /N %d /First %d /Filter /FlateDecode>>' % (len(objstm), len(head)), payload)
            self.stm = stm
        for num in sorted(self.objs):
            if num in inside:
                continue
            body, stream = self.objs[num]
            if num != enc_num:
                body = self.strings(num, body)
            offs[num] = len(out)
            out += b'%d 0 obj\n' % num
            if stream is not None:
                data = self.enc(num, stream) if num != enc_num else stream
                if body.endswith(b'>>'):
                    body = body[:-2] + b' /Length %d>>' % len(data)
                out += body + b'\nstream\n' + data + b'\nendstream\nendobj\n'
            else:
                out += body + b'\nendobj\n'
        size = max(list(self.objs) + [0]) + 1
        trailer = '/Size %d /Root %d 0 R /ID [<%s> <%s>]' % (size, root, self.id0.hex(), self.id0.hex())
        if info:
            trailer += ' /Info %d 0 R' % info
        if enc_num:
            trailer += ' /Encrypt %d 0 R' % enc_num
        if xref_stream:
            xs = size
            size += 1
            offs[xs] = len(out)
            rows = b''
            for i in range(size):
                if i in inside:
                    rows += struct.pack('>BIH', 2, self.stm, inside[i])
                elif i in offs:
                    rows += struct.pack('>BIH', 1, offs[i], 0)
                else:
                    rows += struct.pack('>BIH', 0, 0, 65535)
            # the PNG "up" predictor, as producers write it
            pred, prev = b'', bytes(7)
            for r in range(0, len(rows), 7):
                row = rows[r:r + 7]
                pred += b'\x02' + bytes((a - b) & 255 for a, b in zip(row, prev))
                prev = row
            data = zlib.compress(pred)
            trailer_x = trailer.replace('/Size %d' % (size - 1), '/Size %d' % size)
            out += (b'%d 0 obj\n<</Type /XRef %s /W [1 4 2] /Filter /FlateDecode /DecodeParms <</Predictor 12 /Columns 7>> /Length %d>>\nstream\n'
                    % (xs, trailer_x.encode(), len(data))) + data + b'\nendstream\nendobj\n'
            out += b'startxref\n%d\n%%%%EOF\n' % offs[xs]
        else:
            x = len(out)
            out += b'xref\n0 %d\n0000000000 65535 f \n' % size
            for i in range(1, size):
                o = offs.get(i, 0)
                if broken:
                    o = o + 37 if o else 0      # wrong on purpose
                out += b'%010d 00000 %s \n' % (o, b'n' if i in offs else b'f')
            out += b'trailer\n<<' + trailer.encode() + b'>>\nstartxref\n%d\n%%%%EOF\n' % (x + (11 if broken else 0))
        open(path, 'wb').write(out)


def text_str(s):
    """a string for a content stream, WinAnsi"""
    b = s.encode('cp1252')
    return b'(' + b.replace(b'\\', b'\\\\').replace(b'(', b'\\(').replace(b')', b'\\)') + b')'


T = {
    'title': 'Informe de la cooperativa',
    'head': 'Cooperativa Los Álamos — Informe anual',
    'intro': ('Durante el año la cooperativa sumó nuevos socios, amplió el galpón y ordenó las cuentas. '
              'Este informe resume lo hecho y lo que falta, con números inventados para la prueba.'),
    'left': ['La asamblea de marzo aprobó el plan de', 'obras. Se pidieron tres presupuestos y', 'se eligió el más barato, que además in-',
             'cluía la pintura del portón y el arreglo', 'del techo. Las obras empezaron en abril', 'y terminaron en junio.'],
    'right': ['En la segunda mitad del año se compró', 'una balanza nueva y una cámara de frío.', 'El dinero salió del fondo de reserva,',
              'que se repuso con la venta de la cosecha', 'de otoño. Para el año que viene quedan', 'pendientes el portón y el agua.'],
    'list': ['Socios nuevos: doce', 'Horas de trabajo voluntario: 340', 'Gastos de obra: 1.250.000 pesos'],
    'page2': 'Segunda parte: las cuentas',
    'acc': 'Ingresos y egresos del ejercicio, en pesos. Las cifras son ficticias.',
}


def content(lines):
    return '\n'.join(lines).encode('latin-1')


def clasico(path):
    p = Pdf()
    cat, pages = p.reserve(), p.reserve()
    f1 = p.add('<</Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding>>')
    f2 = p.add('<</Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding>>')
    f3 = p.add('<</Type /Font /Subtype /Type1 /BaseFont /Times-Italic /Encoding <</Type /Encoding /BaseEncoding /StandardEncoding '
               '/Differences [128 /aacute /eacute /iacute /oacute /uacute /ntilde /Eacute /emdash /quotedblleft /quotedblright]>>>>')
    land = sd.landscape(160, 100)
    # the PNG's pixels as a Flate RGB picture
    import zlib as z
    raw = z.decompress(land[land.index(b'IDAT') + 4:land.index(b'IEND') - 8])
    px = b''.join(raw[r * (160 * 3 + 1) + 1:(r + 1) * (160 * 3 + 1)] for r in range(100))
    im1 = p.add(b'<</Type /XObject /Subtype /Image /Width 160 /Height 100 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode>>',
                zlib.compress(px))
    jpg = None
    if shutil.which('sips'):
        import samples_read as sr
        jpg = sr.jpeg_of(land)
    im2 = p.add(b'<</Type /XObject /Subtype /Image /Width 240 /Height 160 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode>>',
                jpg) if jpg else None

    def page(n, body):
        c = ['BT /F1 8 Tf 56 810 Td %s Tj ET' % text_str(T['head']).decode('latin-1'),
             'BT /F1 8 Tf 290 30 Td (%d) Tj ET' % n] + body
        cs = p.add(b'<<>>', zlib.compress(content(c)) if n != 2 else content(c))
        if n != 2:
            p.objs[cs] = (b'<</Filter /FlateDecode>>', p.objs[cs][1])
        xo = ' /XObject <</Im1 %d 0 R%s>>' % (im1, ' /Im2 %d 0 R' % im2 if im2 else '')
        return p.add('<</Type /Page /Parent %d 0 R /MediaBox [0 0 595 842] /Contents %d 0 R '
                     '/Resources <</Font <</F1 %d 0 R /F2 %d 0 R /F3 %d 0 R>>%s>>>>' % (pages, cs, f1, f2, f3, xo))
    b1 = ['BT /F2 20 Tf 56 760 Td %s Tj ET' % text_str(T['title']).decode('latin-1')]
    # the intro, wrapped by hand at about 90 characters
    words, line, y = T['intro'].split(), '', 730
    for w in words + ['']:
        if w and len(line) + len(w) < 95:
            line += (' ' if line else '') + w
            continue
        b1.append('BT /F1 10 Tf 56 %d Td %s Tj ET' % (y, text_str(line).decode('latin-1')))
        y -= 13
        line = w
    # two columns: TJ with kerning, and words placed without spaces on the right
    y = 680
    for a, b in zip(T['left'], T['right']):
        tj = ' '.join('%s -250' % text_str(w).decode('latin-1') for w in a.split())
        b1.append('BT /F1 9 Tf 56 %d Td [%s] TJ ET' % (y, tj))
        x = 310
        parts = []
        for w in b.split():
            parts.append('1 0 0 1 %.1f %d Tm %s Tj' % (x, y, text_str(w).decode('latin-1')))
            x += sum(sd_w(c) for c in w) * 9 / 1000 + 2.5
        b1.append('BT /F1 9 Tf ' + ' '.join(parts) + ' ET')
        y -= 12
    b1.append('BT /F2 12 Tf 56 590 Td (Resumen) Tj ET')
    y = 572
    for it in T['list']:
        b1.append('BT /F1 10 Tf 66 %d Td (\\225 %s) Tj ET' % (y, text_str(it).decode('latin-1')[1:-1]))
        y -= 13
    # italic in the /Differences font: "Él dijo: “qué año”" with codes 128...
    b1.append('BT /F3 11 Tf 56 510 Td <86> Tj (l dijo ) Tj <87 88> Tj (qu) Tj <81> Tj ( a) Tj <85> Tj (o) Tj <89> Tj ET')
    b1.append('q 160 0 0 100 56 380 cm /Im1 Do Q')
    pg1 = page(1, b1)
    b2 = ['BT /F2 16 Tf 56 760 Td %s Tj ET' % text_str(T['page2']).decode('latin-1'),
          'BT /F1 10 Tf 56 735 Td %s Tj ET' % text_str(T['acc']).decode('latin-1')]
    if im2:
        b2.append('q 240 0 0 160 56 540 cm /Im2 Do Q')
    pg2 = page(2, b2)
    pg3 = page(3, ['BT /F1 10 Tf 56 760 Td (Fin del informe.) Tj ET'])
    p.add('<</Type /Pages /Kids [%d 0 R %d 0 R %d 0 R] /Count 3>>' % (pg1, pg2, pg3), num=pages)
    p.add('<</Type /Catalog /Pages %d 0 R>>' % pages, num=cat)
    info = p.add(b'<</Title \x01' + T['title'].encode('utf-16-be').join([b'\xfe\xff', b'']) + b'\x02 /Producer (samples_pdf.py)>>')
    p.write(path, cat, info)


def sd_w(c):
    import of_widths
    return of_widths.helv(c)


def moderno(path):
    p = Pdf()
    cat, pages = p.reserve(), p.reserve()
    # a Type0 font: two-byte codes, ToUnicode tells what they are
    text = 'Ñandú, pingüino y cigüeña: aves del sur.'
    codes = {}
    for ch in text:
        codes.setdefault(ch, 0x0101 + len(codes))
    bfchar = ''.join('<%04X> <%04X>\n' % (c, ord(ch)) for ch, c in codes.items() if ch not in 'abcdefg')
    rng = sorted((codes[ch], ch) for ch in codes if ch in 'abcdefg')
    bfrange = ''.join('<%04X> <%04X> <%04X>\n' % (c, c, ord(ch)) for c, ch in rng)
    cmap = ('/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def '
            '/CMapName /Adobe-Identity-UCS def /CMapType 2 def 1 begincodespacerange <0000> <FFFF> endcodespacerange\n'
            '%d beginbfchar\n%sendbfchar\n%d beginbfrange\n%sendbfrange\nendcmap CMapName currentdict /CMap defineresource pop end end'
            % (len(codes) - len(rng), bfchar, len(rng), bfrange))
    tu = p.add(b'<<>>', cmap.encode())
    desc = p.add('<</Type /Font /Subtype /CIDFontType2 /BaseFont /ABCDEF+Inventada-Bold /CIDSystemInfo <</Registry (Adobe) /Ordering (Identity) /Supplement 0>> '
                 '/DW 600 /W [257 [700 650 600]]>>')
    f0 = p.add('<</Type /Font /Subtype /Type0 /BaseFont /ABCDEF+Inventada-Bold /Encoding /Identity-H /DescendantFonts [%d 0 R] /ToUnicode %d 0 R>>' % (desc, tu))
    f1 = p.add('<</Type /Font /Subtype /Type1 /BaseFont /Helvetica>>')
    hexs = ''.join('%04X' % codes[ch] for ch in text)
    body = ('BT /F0 18 Tf 56 760 Td <%s> Tj ET\n' % hexs +
            'BT /F1 11 Tf 56 730 Td (Un texto en ASCII85 y LZW, con objetos dentro de un stream de objetos.) Tj ET\n'
            'BT /F1 11 Tf 56 716 Td (La tabla de referencias es un stream con el predictor PNG.) Tj ET').encode('latin-1')
    lzw_data = lzw(body)
    a85 = ascii85(lzw_data)
    cs = p.add(b'<</Filter [/ASCII85Decode /LZWDecode]>>', a85)
    pg = p.add('<</Type /Page /Parent %d 0 R /MediaBox [0 0 595 842] /Contents %d 0 R /Resources <</Font <</F0 %d 0 R /F1 %d 0 R>>>>>>' % (pages, cs, f0, f1))
    p.add('<</Type /Pages /Kids [%d 0 R] /Count 1>>' % pg, num=pages)
    p.add('<</Type /Catalog /Pages %d 0 R>>' % pages, num=cat)
    p.write(path, cat, xref_stream=True, objstm=(desc, f0, f1, pg, pages, cat))


def lzw(data):
    """LZW with early change, as PDF wants it"""
    table = {bytes([i]): i for i in range(256)}
    nxt, width, bits, acc, out = 258, 9, 0, 0, bytearray()

    def put(code):
        nonlocal acc, bits
        acc = (acc << width) | code
        bits += width
        while bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 255)
    put(256)
    w = b''
    for c in data:
        wc = w + bytes([c])
        if wc in table:
            w = wc
            continue
        put(table[w])
        table[wc] = nxt
        nxt += 1
        if nxt + 1 >= (1 << width) and width < 12:
            width += 1
        if nxt >= 4094:
            put(256)
            table = {bytes([i]): i for i in range(256)}
            nxt, width = 258, 9
        w = bytes([c])
    if w:
        put(table[w])
    put(257)
    if bits:
        out.append((acc << (8 - bits)) & 255)
    return bytes(out)


def ascii85(data):
    import base64
    return base64.a85encode(data) + b'~>'


def simple(path, text, rotate=0, broken=False, enc=None):
    p = Pdf()
    if enc:
        enc(p)
    cat, pages = p.reserve(), p.reserve()
    f1 = p.add('<</Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding>>')
    lines = ['BT /F1 16 Tf 56 %d Td %s Tj ET' % (760 if not rotate else 540, text_str(text[0]).decode('latin-1'))]
    y = 730 if not rotate else 510
    for t in text[1:]:
        lines.append('BT /F1 11 Tf 56 %d Td %s Tj ET' % (y, text_str(t).decode('latin-1')))
        y -= 14
    cs = p.add(b'<</Filter /FlateDecode>>', zlib.compress(content(lines)))
    box = '0 0 842 595' if rotate else '0 0 595 842'
    pg = p.add('<</Type /Page /Parent %d 0 R /MediaBox [%s] /Rotate %d /Contents %d 0 R /Resources <</Font <</F1 %d 0 R>>>>>>'
               % (pages, box, rotate, cs, f1))
    p.add('<</Type /Pages /Kids [%d 0 R] /Count 1>>' % pg, num=pages)
    p.add('<</Type /Catalog /Pages %d 0 R>>' % pages, num=cat)
    info = p.add(b'<</Title \x01' + text[0].encode('latin-1') + b'\x02>>')
    p.write(path, cat, info, broken=broken)


def external(d):
    tmp = tempfile.mkdtemp()
    txt = os.path.join(tmp, 'carta.txt')
    open(txt, 'w', encoding='utf-8').write('Carta de prueba\n\nEsta carta se imprimió con el sistema de la Mac.\n'
                                           'Tiene acentos (camión, pingüino, año) y dos párrafos.\n\nSaludos.\n')
    if shutil.which('cupsfilter'):
        with open(os.path.join(d, 'quartz.pdf'), 'wb') as f:
            subprocess.run(['cupsfilter', '-m', 'application/pdf', txt], stdout=f, stderr=subprocess.DEVNULL)
    pv = shutil.which('pango-view') or '/opt/homebrew/bin/pango-view'
    if os.path.exists(pv):
        markup = ('<span size="xx-large" weight="bold">Informe breve</span>\n\n'
                  'Un párrafo dibujado por <b>cairo</b> con <i>cursivas</i>, negritas y acentos: canción, corazón, güiro.\n'
                  'La segunda línea del mismo párrafo sigue acá, para ver cómo se juntan.\n\n'
                  'Otro párrafo, separado por una línea en blanco.')
        subprocess.run([pv, '--markup', '--width=480', '--font=DejaVu Sans 11', '--output=' + os.path.join(d, 'cairo.pdf'),
                        '-q', '--text=' + markup], stderr=subprocess.DEVNULL)
        png = os.path.join(tmp, 'pagina.png')
        subprocess.run([pv, '--width=1200', '--font=Sans 28', '--dpi=150', '--output=' + png, '-q',
                        '--text=Hoja escaneada\n\nEste texto está en una imagen y lo lee el reconocimiento de caracteres.\n'
                                '\nSegundo párrafo de la hoja escaneada.'], stderr=subprocess.DEVNULL)
        tess = shutil.which('tesseract') or '/opt/homebrew/bin/tesseract'
        if os.path.exists(png) and os.path.exists(tess):
            subprocess.run([tess, png, os.path.join(d, 'escaneo'), '-l', 'spa', 'pdf'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if not os.path.exists(os.path.join(d, 'escaneo.pdf')):
                subprocess.run([tess, png, os.path.join(d, 'escaneo'), 'pdf'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        c2p = shutil.which('convertfilestopdf') or '/opt/homebrew/bin/convertfilestopdf'
        if os.path.exists(png) and os.path.exists(c2p):
            sub = os.path.join(tmp, 'img')
            os.makedirs(sub)
            shutil.copy(png, sub)
            subprocess.run([c2p, sub, 'png', '150', '0', '0', 'Imagen', os.path.join(d, 'imagen.pdf')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rsvg = shutil.which('rsvg-convert') or '/opt/homebrew/bin/rsvg-convert'
    if os.path.exists(rsvg):
        left = ['La primera columna cuenta', 'cómo empezó el proyecto y', 'quiénes participaron en la', 'primera reunión del grupo.']
        right = ['La segunda columna sigue', 'con los planes para el año', 'que viene y las tareas que', 'quedan por hacer todavía.']
        svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="595" height="842">',
               '<text x="56" y="70" font-family="DejaVu Sans" font-size="22" font-weight="bold">Dos columnas</text>']
        for i, (a, b) in enumerate(zip(left, right)):
            svg.append('<text x="56" y="%d" font-family="DejaVu Sans" font-size="11">%s</text>' % (110 + i * 15, a))
            svg.append('<text x="310" y="%d" font-family="DejaVu Sans" font-size="11">%s</text>' % (110 + i * 15, b))
        svg.append('</svg>')
        s = os.path.join(tmp, 'c.svg')
        open(s, 'w', encoding='utf-8').write('\n'.join(svg))
        subprocess.run([rsvg, '-f', 'pdf', '-o', os.path.join(d, 'columnas.pdf'), s], stderr=subprocess.DEVNULL)
    shutil.rmtree(tmp, ignore_errors=True)


def make(d, ext=False):
    os.makedirs(d, exist_ok=True)
    # widths of Helvetica for placing words by hand
    sys.modules['of_widths'] = type(sys)('of_widths')
    W = [278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278] + [556] * 10 + \
        [278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778,
         722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222,
         222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584]
    sys.modules['of_widths'].helv = lambda c: W[ord(c) - 32] if 32 <= ord(c) < 127 else 556
    clasico(os.path.join(d, 'clasico.pdf'))
    moderno(os.path.join(d, 'moderno.pdf'))
    simple(os.path.join(d, 'girado.pdf'), ['Página apaisada', 'Esta página está girada 90 grados con /Rotate.',
                                           'El texto tiene que leerse igual.'], rotate=90)
    simple(os.path.join(d, 'roto.pdf'), ['Archivo con la tabla rota', 'Las posiciones de la tabla están corridas.',
                                         'La app la arma de nuevo buscando los objetos.'], broken=True)
    simple(os.path.join(d, 'clave_rc4.pdf'), ['Cifrado RC4 de 128 bits', 'Se abre sin contraseña de usuario.',
                                              'La del dueño es otra y no hace falta para leer.'],
           enc=lambda p: p.encrypt_r3('dueño'.encode('latin-1')))
    simple(os.path.join(d, 'clave_aes.pdf'), ['Cifrado AES de 128 bits', 'La contraseña para abrirlo es 1234.'],
           enc=lambda p: p.encrypt_aes(b'1234', b'otra'))
    simple(os.path.join(d, 'clave_256.pdf'), ['Cifrado AES de 256 bits', 'La contraseña para abrirlo es abc.'],
           enc=lambda p: p.encrypt_256(b'abc', b'duenio'))
    if ext:
        external(d)


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    make(args[0] if args else 'sim/sim_fs/documentos', '--external' in sys.argv)
    print('PDF samples in', args[0] if args else 'sim/sim_fs/documentos')
