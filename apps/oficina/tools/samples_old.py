#!/usr/bin/env python3
"""Invented Office 97-2003 files for Office's old-binary readers (phase 6):

    python3 apps/oficina/tools/samples_old.py DIR [--en]

  cuentas.xls / accounts.xls   BIFF8 in a compound file: two sheets (one
                               hidden), shared strings (one crossing a
                               CONTINUE record), fonts, cell formats (bold,
                               red, a fill, centred), number formats (date,
                               currency, percent), numbers, RK and MULRK,
                               formulas' cached results (number, string,
                               boolean), an error, merged cells, frozen
                               panes, column widths, a hidden row
  charla.ppt / talk.ppt        a PowerPoint 97-2003 stream with its current
                               user, edit and persist directory: three slides
                               with title and body (bullets in two levels,
                               bold and a colour), a filled rectangle, an
                               ellipse in a group, a picture from the BLIP
                               store, speaker notes, a hidden slide

The .doc files come from macOS's textutil (see the README). Everything is
invented. The compound file writer is small: a flat directory, a mini
stream for the small streams.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import samples_docs as sd  # noqa: E402

# --------------------------------------------------------------------------
# A compound file

FREE, END, FATSECT = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD


def cfb(streams):
    """streams: [(name, bytes)] -> the file"""
    SS, MS = 512, 64
    sectors = []                       # 512-byte blocks after the header
    fat = []

    def alloc(data):
        if not data:
            return END
        first = len(sectors)
        n = (len(data) + SS - 1) // SS
        for i in range(n):
            sectors.append(data[i * SS:(i + 1) * SS].ljust(SS, b'\0'))
            fat.append(first + i + 1 if i + 1 < n else END)
        return first
    # small streams in the mini stream
    mini, minifat, starts = b'', [], {}
    for name, data in streams:
        if len(data) < 4096:
            first = len(mini) // MS
            n = max(1, (len(data) + MS - 1) // MS)
            for i in range(n):
                minifat.append(first + i + 1 if i + 1 < n else END)
            mini += data.ljust(n * MS, b'\0')
            starts[name] = first
    for name, data in streams:
        if len(data) >= 4096:
            starts[name] = alloc(data)
    mini_start = alloc(mini) if mini else END
    mf = b''.join(struct.pack('<I', x) for x in minifat)
    mf_start = alloc(mf) if mf else END
    # the directory: root and the streams, sorted, chained to the right
    order = sorted(range(len(streams)), key=lambda i: (len(streams[i][0]), streams[i][0].upper()))

    def entry(name, typ, left, right, child, start, size):
        nm = name.encode('utf-16-le') + b'\0\0'
        return (nm.ljust(64, b'\0') + struct.pack('<HBB', len(nm), typ, 1) + struct.pack('<III', left, right, child)
                + b'\0' * 16 + b'\0' * 4 + b'\0' * 16 + struct.pack('<IIi', start, size, 0))
    ents = [entry('Root Entry', 5, FREE, FREE, 1 if streams else FREE, mini_start, len(mini))]
    for k, i in enumerate(order):
        name, data = streams[i]
        ents.append(entry(name, 2, FREE, k + 2 if k + 1 < len(order) else FREE, FREE, starts[name], len(data)))
    dirdata = b''.join(ents)
    dir_start = alloc(dirdata)
    # the FAT itself
    nfat = 1
    while (len(sectors) + nfat) > nfat * (SS // 4):
        nfat += 1
    fat_first = len(sectors)
    for i in range(nfat):
        fat.append(FATSECT)
        sectors.append(b'')
    fat += [FREE] * (nfat * (SS // 4) - len(fat))
    fatdata = b''.join(struct.pack('<I', x) for x in fat)
    for i in range(nfat):
        sectors[fat_first + i] = fatdata[i * SS:(i + 1) * SS]
    difat = [fat_first + i for i in range(nfat)] + [FREE] * (109 - nfat)
    head = (b'\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1' + b'\0' * 16 + struct.pack('<HHHHH', 0x3E, 3, 0xFFFE, 9, 6) + b'\0' * 6
            + struct.pack('<IIIIIIIII', 0, nfat, dir_start, 0, 4096, mf_start, (len(mf) + SS - 1) // SS if mf else 0, END, 0)
            + b''.join(struct.pack('<I', x) for x in difat))
    return head + b''.join(sectors)


# --------------------------------------------------------------------------
# Excel 97-2003

def rec(t, d=b''):
    return struct.pack('<HH', t, len(d)) + d


def xstr(s, len8=False):
    """an XLUnicodeString, compressed when it can be"""
    try:
        b = s.encode('latin-1')
        hi = 0
    except UnicodeEncodeError:
        b = s.encode('utf-16-le')
        hi = 1
    return (struct.pack('<B', len(s)) if len8 else struct.pack('<H', len(s))) + bytes([hi]) + b


TX = {
    'es': dict(file='cuentas', sheets=('Gastos', 'Oculta'), head=('Fecha', 'Concepto', 'Importe', 'Pagado', '% del total'),
               rows=[('Alquiler del salón', 45000, True), ('Semillas', 12500.5, True), ('Herramientas', 30990, False), ('Café para la asamblea', 3200, True)],
               total='Total', title='Cuentas de la huerta 2026', long='Nota: ' + 'este texto es largo a propósito para que cruce un registro CONTINUE. ' * 4,
               secret='No se muestra', month='Mes', ok='SÍ'),
    'en': dict(file='accounts', sheets=('Expenses', 'Hidden'), head=('Date', 'Item', 'Amount', 'Paid', '% of total'),
               rows=[('Hall rent', 45000, True), ('Seeds', 12500.5, True), ('Tools', 30990, False), ('Coffee for the meeting', 3200, True)],
               total='Total', title='Garden accounts 2026', long='Note: ' + 'this text is long on purpose so that it crosses a CONTINUE record. ' * 4,
               secret='Not shown', month='Month', ok='YES'),
}


def xls(path, t):
    strings = [t['title']] + list(t['head']) + [r[0] for r in t['rows']] + [t['total'], t['long'], t['secret']]
    sst_index = {s: i for i, s in enumerate(strings)}
    # the SST, cut into a record and CONTINUEs at 200 bytes, a string split in the middle
    body = struct.pack('<II', len(strings), len(strings))
    recs, cur = [], body
    for s in strings:
        enc = xstr(s)
        if len(cur) + len(enc) > 200 and s == t['long']:
            # split inside the characters: the continue repeats the flag byte
            head = enc[:3]
            chars = enc[3:]
            room = max(10, 200 - len(cur) - 3)
            cur += head + chars[:room]
            recs.append(cur)
            cur = b'\0' + chars[room:]
            continue
        if len(cur) + len(enc) > 200:
            recs.append(cur)
            cur = b''
        cur += enc
    recs.append(cur)
    sst = rec(0x00FC, recs[0]) + b''.join(rec(0x003C, r) for r in recs[1:])
    fonts = b''.join(rec(0x0031, struct.pack('<HHHHHBBBB', 200, 0, 0x7FFF, w, 0, 0, 0, 0, 0) + xstr('Arial', True))
                     for w in (400, 400, 400, 400))
    fonts += rec(0x0031, struct.pack('<HHHHHBBBB', 220, 0, 0x7FFF, 700, 0, 0, 0, 0, 0) + xstr('Arial', True))      # 5: bold
    fonts += rec(0x0031, struct.pack('<HHHHHBBBB', 200, 2, 10, 400, 0, 0, 0, 0, 0) + xstr('Arial', True))          # 6: italic red
    formats = rec(0x041E, struct.pack('<H', 164) + xstr('"$" #,##0.00')) + rec(0x041E, struct.pack('<H', 165) + xstr('dd/mm/yyyy'))

    def xf(font, fmt, style=False, align=0, fill=None):
        d14 = (1 << 26) if fill else 0
        fore = fill if fill else 64
        return rec(0x00E0, struct.pack('<HHHBBBBII', font, fmt, 0xFFF5 if style else 0x0001, align, 0, 0, 0, 0, d14) + struct.pack('<H', fore | (65 << 7)))
    xfs = b''.join(xf(0, 0, True) for _ in range(15)) + xf(0, 0)          # 15: the default cell format
    xfs += xf(5, 0, align=2, fill=22)       # 16: bold, centred, grey fill (palette 22 = C0C0C0)
    xfs += xf(0, 165)                        # 17: date
    xfs += xf(0, 164)                        # 18: currency
    xfs += xf(0, 10)                         # 19: 0.00%
    xfs += xf(6, 0)                          # 20: italic red
    xfs += xf(5, 164)                        # 21: bold currency

    def sheet(cells_fn):
        d = rec(0x0809, struct.pack('<HHHHII', 0x0600, 0x0010, 0x0DBB, 0x07CC, 0, 0x0006))
        d += rec(0x0055, struct.pack('<H', 10))
        d += cells_fn()
        d += rec(0x000A)
        return d

    def cells1():
        o = b''
        o += rec(0x007D, struct.pack('<HHHHH', 0, 0, 12 * 256, 15, 0)) + rec(0x007D, struct.pack('<HHHHH', 1, 1, 26 * 256, 15, 0))
        o += rec(0x007D, struct.pack('<HHHHH', 2, 4, 14 * 256, 15, 0))
        o += rec(0x0208, struct.pack('<HHHHHHI', 7, 0, 5, 0x0120, 0, 0, 0x01 | 0x20 | 0x40 | (15 << 16)))     # row 8 hidden
        o += rec(0x00FD, struct.pack('<HHHI', 0, 0, 16, sst_index[t['title']]))
        for c, h in enumerate(t['head']):
            o += rec(0x00FD, struct.pack('<HHHI', 1, c, 16, sst_index[h]))
        for r, (item, amount, paid) in enumerate(t['rows']):
            row = 2 + r
            # dates as numbers (days since 1899-12-30): 46023 = 2026-01-01
            if r < 2:
                # MULRK: date in A as RK (an integer), then nothing
                o += rec(0x00BD, struct.pack('<HH', row, 0) + struct.pack('<HI', 17, ((46023 + r * 31) << 2) | 2) + struct.pack('<H', 0))
            else:
                o += rec(0x0203, struct.pack('<HHH', row, 0, 17) + struct.pack('<d', 46023 + r * 31))
            o += rec(0x00FD, struct.pack('<HHHI', row, 1, 0 if r != 2 else 20, sst_index[item]))
            if float(amount).is_integer() and amount < (1 << 29):
                o += rec(0x027E, struct.pack('<HHHI', row, 2, 18, (int(amount) << 2) | 2))
            else:
                o += rec(0x027E, struct.pack('<HHHI', row, 2, 18, (int(amount * 100) << 2) | 3))
            o += rec(0x0205, struct.pack('<HHHBB', row, 3, 15, 1 if paid else 0, 0))
            share = amount / sum(x[1] for x in t['rows'])
            o += rec(0x0006, struct.pack('<HHH', row, 4, 19) + struct.pack('<d', share) + struct.pack('<HI', 0, 0) + struct.pack('<H', 3) + b'\x1E\x01\x00')
        tr = 2 + len(t['rows'])
        o += rec(0x00FD, struct.pack('<HHHI', tr, 1, 16, sst_index[t['total']]))
        o += rec(0x0006, struct.pack('<HHH', tr, 2, 21) + struct.pack('<d', sum(x[1] for x in t['rows'])) + struct.pack('<HI', 0, 0) + struct.pack('<H', 3) + b'\x1E\x01\x00')
        # a formula giving a string, and one giving a boolean, and an error
        o += rec(0x0006, struct.pack('<HHH', tr, 3, 15) + b'\x00\x00\x00\x00\x00\x00\xFF\xFF' + struct.pack('<HI', 0, 0) + struct.pack('<H', 3) + b'\x1E\x01\x00')
        o += rec(0x0207, xstr(t['ok']))
        o += rec(0x0205, struct.pack('<HHHBB', tr, 4, 15, 0x07, 1))
        o += rec(0x00FD, struct.pack('<HHHI', tr + 2, 0, 0, sst_index[t['long']]))
        o += rec(0x00E5, struct.pack('<H', 1) + struct.pack('<HHHH', 0, 0, 0, 4))        # A1:E1 merged
        o += rec(0x023E, struct.pack('<HHHH', 0x06B6 | 0x0008, 0, 0, 64) + b'\0' * 10)  # frozen
        o += rec(0x0041, struct.pack('<HHHHH', 0, 2, 2, 0, 2))                          # two rows frozen
        return o

    def cells2():
        return rec(0x00FD, struct.pack('<HHHI', 0, 0, 15, sst_index[t['secret']]))
    s1, s2 = sheet(cells1), sheet(cells2)

    def globals_(offs):
        g = rec(0x0809, struct.pack('<HHHHII', 0x0600, 0x0005, 0x0DBB, 0x07CC, 0, 0x0006))
        g += rec(0x0042, struct.pack('<H', 1200)) + rec(0x003D, struct.pack('<HHHHHHHHH', 0, 0, 0x4000, 0x2000, 0x38, 0, 0, 1, 600))
        g += fonts + formats + xfs + rec(0x0022, struct.pack('<H', 0))
        g += rec(0x0085, struct.pack('<IBB', offs[0], 0, 0) + xstr(t['sheets'][0], True))
        g += rec(0x0085, struct.pack('<IBB', offs[1], 1, 0) + xstr(t['sheets'][1], True))
        g += sst + rec(0x000A)
        return g
    g = globals_((0, 0))
    wb = globals_((len(g), len(g) + len(s1))) + s1 + s2
    open(path, 'wb').write(cfb([('Workbook', wb)]))


# --------------------------------------------------------------------------
# PowerPoint 97-2003

def atom(t, d, inst=0, ver=0):
    return struct.pack('<HHI', ver | (inst << 4), t, len(d)) + d


def cont(t, kids, inst=0):
    return struct.pack('<HHI', 0xF | (inst << 4), t, len(kids)) + kids


PT = {
    'es': dict(file='charla', slides=[('La huerta del barrio', ['Charla para vecinos'], False),
                                      ('Qué vamos a plantar', ['Verduras de hoja', '\tLechuga y acelga', 'Aromáticas', 'Tomates, si hay sol'], False),
                                      ('Borrador oculto', ['Esta diapositiva no se muestra'], True),
                                      ('El terreno', ['El lote de la esquina'], False)],
               notes='Contar la historia del terreno.', box='Riego'),
    'en': dict(file='talk', slides=[('The neighbourhood garden', ['A talk for neighbours'], False),
                                    ('What we will plant', ['Leafy greens', '\tLettuce and chard', 'Herbs', 'Tomatoes, if there is sun'], False),
                                    ('Hidden draft', ['This slide is not shown'], True),
                                    ('The plot', ['The corner lot'], False)],
               notes='Tell the story of the plot.', box='Watering'),
}


def text_atoms(kind, s, bold_first=False):
    body = s.replace('\n', '\r')
    out = atom(0x0F9F, struct.pack('<I', kind))
    try:
        out += atom(0x0FA8, body.encode('latin-1'))
    except UnicodeEncodeError:
        out += atom(0x0FA0, body.encode('utf-16-le'))
    # the style: one paragraph run per line (level by leading tab), one character run
    lines = body.split('\r')
    pf = b''
    for i, ln in enumerate(lines):
        n = len(ln) + 1
        lvl = 1 if ln.startswith('\t') else 0
        pf += struct.pack('<IH', n, lvl) + struct.pack('<I', 0x0F if kind in (1, 5) else 0x01) + struct.pack('<H', 1 if kind in (1, 5) else 0)
    cf = struct.pack('<I', len(body) + 1) + struct.pack('<I', 0x1 | 0x20000 | 0x40000) + struct.pack('<H', 1 if bold_first else 0) + struct.pack('<H', 40 if kind == 0 else 24) + bytes([0x2E, 0x4A, 0x32, 0xFE])
    return out + atom(0x0FA1, pf + cf)


def anchor(l, t, r, b):
    return atom(0xF010, struct.pack('<hhhh', t * 8, l * 8, r * 8, b * 8))


def sp(spid, spt, props=b'', anchor_rec=b'', text=b'', group=False, child_anchor=b''):
    fsp = atom(0xF00A, struct.pack('<II', spid, 0x0A00 | (0x1 if group else 0)), inst=spt, ver=2)
    opt = atom(0xF00B, props, inst=len(props) // 6, ver=3) if props else b''
    tb = cont(0xF00D, text) if text else b''
    return cont(0xF004, (atom(0xF009, struct.pack('<iiii', 0, 0, 1000, 1000), ver=1) if group else b'') + fsp + opt + child_anchor + anchor_rec + tb)


def prop(pid, v):
    return struct.pack('<HI', pid, v)


def ppt(path, t):
    land = sd.landscape(240, 160)
    blip = atom(0xF01E, b'\x11' * 16 + b'\xFF' + land, inst=0x6E0)
    pictures = blip
    bse = atom(0xF007, struct.pack('<BB', 6, 6) + b'\x11' * 16 + struct.pack('<HIIIIBBBB', 0xFF, len(blip), 1, 0, 0, 0, 0, 0, 0), inst=6, ver=2)
    dgg = cont(0xF000, atom(0xF006, struct.pack('<IIII', 2048, 2, 10, 1)) + cont(0xF001, bse, inst=1))
    stream = bytearray()
    persist = {}

    def put(pid, data):
        persist[pid] = len(stream)
        stream.extend(data)
    slides = t['slides']
    # slide list: persist ids 3.. for slides, notes at 20
    slist = b''
    for i, (title, body, hidden) in enumerate(slides):
        slist += atom(0x03F3, struct.pack('<IIiII', 3 + i, 4, len(slides) + i, 0, 256 + i))
        slist += text_atoms(0, title, True) + text_atoms(1, '\r'.join(body))
    doc = cont(0x03E8, atom(0x03E9, struct.pack('<ii', 5760, 4320) + struct.pack('<ii', 4320, 5760) + b'\0' * 28)
               + cont(0x040B, dgg) + cont(0x0FF0, slist, inst=0))
    put(1, doc)
    for i, (title, body, hidden) in enumerate(slides):
        shapes = sp(1024, 0, group=True)
        # the title from the slide list (OutlineTextRef 0), the body from it too (1)
        shapes += sp(1025 + i * 10, 1, anchor_rec=anchor(54, 30, 666, 100), text=atom(0x0F9F, struct.pack('<I', 0)) + atom(0x0F9E, struct.pack('<I', 0)))
        shapes += sp(1026 + i * 10, 1, anchor_rec=anchor(54, 120, 666, 470), text=atom(0x0F9F, struct.pack('<I', 1)) + atom(0x0F9E, struct.pack('<I', 1)))
        if i == 1:
            # a filled rectangle with its own text, and a group with an ellipse
            shapes += sp(1030, 1, props=prop(0x0181, 0x4F8D3C) + prop(0x01BF, 0x100010) + prop(0x01C0, 0x37632A) + prop(0x01FF, 0x80008),
                         anchor_rec=anchor(480, 300, 660, 380), text=text_atoms(4, t['box'], True))
            grp = sp(1031, 0, group=True, anchor_rec=anchor(480, 400, 660, 470))
            grp += sp(1032, 3, props=prop(0x0181, 0xD59B5B) + prop(0x01BF, 0x100010), child_anchor=atom(0xF00F, struct.pack('<iiii', 0, 0, 400, 1000)))
            shapes += cont(0xF003, grp)
        if i == 3:
            shapes += sp(1040, 75, props=prop(0x4104, 1), anchor_rec=anchor(360, 200, 600, 360))
        dg = cont(0xF002, atom(0xF008, struct.pack('<II', 8, 1024 + i * 10 + 9)) + cont(0xF003, shapes))
        sa = atom(0x03EF, struct.pack('<I', 0) + b'\0' * 8 + struct.pack('<II', 0x80000000, 20 if i == 1 else 0) + struct.pack('<HH', 7, 0), ver=2)
        info = atom(0x03F9, struct.pack('<IIBBHB', 0, 0, 0, 0, 4 if hidden else 0, 0) + b'\0' * 3) if hidden else b''
        put(3 + i, cont(0x03EE, sa + info + cont(0x040C, dg)))
    # the notes of slide 2
    nsh = sp(2048, 0, group=True) + sp(2049, 1, anchor_rec=anchor(50, 400, 500, 700), text=text_atoms(2, t['notes']))
    put(20, cont(0x03F0, atom(0x03F1, struct.pack('<IHH', 4, 0, 0)) + cont(0x040C, cont(0xF002, atom(0xF008, struct.pack('<II', 2, 2049)) + cont(0xF003, nsh)))))
    # the persist directory and the edit
    ids = sorted(persist)
    pd = b''
    for pid in ids:
        pd += struct.pack('<I', pid | (1 << 20)) + struct.pack('<I', persist[pid])
    dir_off = len(stream)
    stream.extend(atom(0x1772, pd))
    edit_off = len(stream)
    stream.extend(atom(0x0FF5, struct.pack('<IHBBIIII', 256, 0, 0, 3, 0, dir_off, 1, 21) + struct.pack('<HH', 1, 0)))
    cu = atom(0x0FF6, struct.pack('<IIIHHHBBB', 20, 0xE391C05F, edit_off, 0, 0x03F4, 3, 0, 0, 0) + b'\0' * 3)
    open(path, 'wb').write(cfb([('PowerPoint Document', bytes(stream)), ('Current User', cu), ('Pictures', pictures)]))


# --------------------------------------------------------------------------
# Apple's iWork (IWA): Snappy (literals only) around protobuf messages

def varint(v):
    o = b''
    while True:
        b = v & 0x7F
        v >>= 7
        o += bytes([b | (0x80 if v else 0)])
        if not v:
            return o


def pb_str(field, data):
    data = data.encode('utf-8') if isinstance(data, str) else data
    return varint(field << 3 | 2) + varint(len(data)) + data


def iwa(messages):
    """[(type, payload)] -> an .iwa file: one archive per message"""
    raw = b''
    for i, (typ, payload) in enumerate(messages):
        info = varint(1 << 3) + varint(i + 1) + pb_str(2, varint(1 << 3) + varint(typ) + varint(3 << 3) + varint(len(payload)))
        raw += varint(len(info)) + info + payload
    # Snappy: the length, then literals of at most 60 bytes
    sn = varint(len(raw))
    for k in range(0, len(raw), 60):
        chunk = raw[k:k + 60]
        sn += bytes([(len(chunk) - 1) << 2]) + chunk
    return bytes([0]) + len(sn).to_bytes(3, 'little') + sn


IW = {
    'es': dict(pages=['Folleto de la huerta', 'Te invitamos a sumarte los sábados.', 'Traé guantes y ganas.'],
               numbers=['Turno', 'Ana', 'Bruno', 'Carla'], key=[['La huerta'], ['Qué plantamos', 'Lechuga y tomates']],
               files=('folleto.pages', 'turnos.numbers', 'charla.key')),
    'en': dict(pages=['Garden leaflet', 'Join us on Saturdays.', 'Bring gloves and energy.'],
               numbers=['Shift', 'Ann', 'Bruce', 'Carol'], key=[['The garden'], ['What we plant', 'Lettuce and tomatoes']],
               files=('leaflet.pages', 'shifts.numbers', 'talk.key')),
}


def iwork(d, en):
    import zipfile
    t = IW['en' if en else 'es']
    jpg = None
    try:
        import samples_read as sr
        jpg = sr.jpeg_of(sd.landscape(240, 160))
    except Exception:
        pass
    storage = pb_str(1, varint(0)) + pb_str(3, '\n'.join(t['pages']) + '\ufffc')
    with zipfile.ZipFile(os.path.join(d, t['files'][0]), 'w') as z:
        z.writestr('Index/Document.iwa', iwa([(1, b'\x08\x01'), (2001, storage)]))
        if jpg:
            z.writestr('preview.jpg', jpg)
    entries = b''.join(pb_str(3, varint(1 << 3) + varint(i) + pb_str(3, x)) for i, x in enumerate(t['numbers']))
    with zipfile.ZipFile(os.path.join(d, t['files'][1]), 'w') as z:
        z.writestr('Index/Document.iwa', iwa([(1, b'\x08\x01')]))
        z.writestr('Index/Tables/DataList.iwa', iwa([(6005, varint(1 << 3) + varint(1) + entries)]))
    with zipfile.ZipFile(os.path.join(d, t['files'][2]), 'w') as z:
        z.writestr('Index/Document.iwa', iwa([(1, b'\x08\x01')]))
        for i, texts in enumerate(t['key']):
            z.writestr('Index/Slide-%d.iwa' % (100 + i), iwa([(2001, pb_str(3, x)) for x in texts]))


def make(d, en=False):
    os.makedirs(d, exist_ok=True)
    t = TX['en' if en else 'es']
    xls(os.path.join(d, t['file'] + '.xls'), t)
    p = PT['en' if en else 'es']
    ppt(os.path.join(d, p['file'] + '.ppt'), p)
    iwork(d, en)


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    make(args[0] if args else 'sim/sim_fs/documentos', '--en' in sys.argv)
    print('old-format samples in', args[0] if args else 'sim/sim_fs/documentos')
