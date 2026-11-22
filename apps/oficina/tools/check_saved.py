#!/usr/bin/env python3
"""
P4OS - Office: checks a document the app saved, against the one it opened.

    python3 apps/oficina/tools/check_saved.py original.xlsx saved.xlsx [A1 Sheet!B2 ...]
    python3 apps/oficina/tools/check_saved.py - new.ods [cells...]        (no original)

For a package (xlsx, ods): it opens as a ZIP with no errors, every XML
part is well-formed, every relationship points at a part that exists,
every part has a content type (xlsx) or is in the manifest (ods), mimetype
is first and stored (ods), and the parts are listed as kept (byte for byte),
changed or gone. Then the cells asked for are printed with their value and
formula, read the way a spreadsheet program reads them. Exit 1 on a fault.
Only the standard library: there is no LibreOffice on this Mac to open them.
"""
import posixpath
import re
import sys
import zipfile
import xml.etree.ElementTree as ET

M = '{http://schemas.openxmlformats.org/spreadsheetml/2006/main}'
R = '{http://schemas.openxmlformats.org/officeDocument/2006/relationships}'
PR = '{http://schemas.openxmlformats.org/package/2006/relationships}'
CT = '{http://schemas.openxmlformats.org/package/2006/content-types}'
T = '{urn:oasis:names:tc:opendocument:xmlns:table:1.0}'
O = '{urn:oasis:names:tc:opendocument:xmlns:office:1.0}'
TX = '{urn:oasis:names:tc:opendocument:xmlns:text:1.0}'
MF = '{urn:oasis:names:tc:opendocument:xmlns:manifest:1.0}'

faults = []


def fault(msg):
    faults.append(msg)
    print('FAULT:', msg)


def check_package(path):
    z = zipfile.ZipFile(path)
    bad = z.testzip()
    if bad:
        fault(f'{path}: bad CRC in {bad}')
    names = z.namelist()
    for n in names:
        if n.endswith('.xml') or n.endswith('.rels') or n.endswith('.vml'):
            try:
                ET.fromstring(z.read(n))
            except ET.ParseError as e:
                fault(f'{path}: {n} is not well-formed XML: {e}')
    if '[Content_Types].xml' in names:
        ct = ET.fromstring(z.read('[Content_Types].xml'))
        defaults = {d.get('Extension').lower() for d in ct.iter(CT + 'Default')}
        overrides = {o.get('PartName').lstrip('/') for o in ct.iter(CT + 'Override')}
        for o in overrides:
            if o not in names:
                fault(f'{path}: content type for a part that is not there: {o}')
        for n in names:
            if n.endswith('/') or n == '[Content_Types].xml':
                continue
            if n not in overrides and n.rsplit('.', 1)[-1].lower() not in defaults:
                fault(f'{path}: part without a content type: {n}')
        for n in names:
            if not n.endswith('.rels'):
                continue
            base = posixpath.dirname(posixpath.dirname(n))
            for rel in ET.fromstring(z.read(n)).iter(PR + 'Relationship'):
                if rel.get('TargetMode') == 'External':
                    continue
                t = rel.get('Target')
                tp = t.lstrip('/') if t.startswith('/') else posixpath.normpath(posixpath.join(base, t))
                if tp not in names:
                    fault(f'{path}: {n} points at {tp}, which is not there')
    if 'mimetype' in names:
        info = z.infolist()[0]
        if info.filename != 'mimetype' or info.compress_type != zipfile.ZIP_STORED:
            fault(f'{path}: mimetype must be the first entry, stored')
        man = ET.fromstring(z.read('META-INF/manifest.xml'))
        listed = {e.get(MF + 'full-path') for e in man.iter(MF + 'file-entry')}
        for n in names:
            if n in ('mimetype', 'META-INF/manifest.xml') or n.endswith('/'):
                continue
            if n not in listed:
                print(f'  note: {n} is not in the manifest')
    return z


def col_index(letters):
    n = 0
    for ch in letters:
        n = n * 26 + ord(ch.upper()) - 64
    return n - 1


def xlsx_cells(z):
    wb = ET.fromstring(z.read('xl/workbook.xml'))
    rels = {r.get('Id'): r.get('Target') for r in ET.fromstring(z.read('xl/_rels/workbook.xml.rels')).iter(PR + 'Relationship')}
    sst = []
    if 'xl/sharedStrings.xml' in z.namelist():
        for si in ET.fromstring(z.read('xl/sharedStrings.xml')).iter(M + 'si'):
            sst.append(''.join(t.text or '' for t in si.iter(M + 't')))
    out = {}
    for sh in wb.iter(M + 'sheet'):
        target = rels[sh.get(R + 'id')]
        p = target.lstrip('/') if target.startswith('/') else 'xl/' + target
        x = ET.fromstring(z.read(p))
        cells = {}
        for c in x.iter(M + 'c'):
            t = c.get('t', 'n')
            v = c.find(M + 'v')
            f = c.find(M + 'f')
            val = v.text if v is not None else None
            if t == 's' and val is not None:
                val = sst[int(val)]
            elif t == 'inlineStr':
                val = ''.join(e.text or '' for e in c.iter(M + 't'))
            ftxt = None
            if f is not None:
                ftxt = (f.text or '') + (f' [{f.get("t")} si={f.get("si")}]' if f.get('t') else '')
            cells[c.get('r')] = (t, val, ftxt, c.get('s'))
        out[sh.get('name')] = cells
    return out


def ods_cells(z):
    root = ET.fromstring(z.read('content.xml'))
    out = {}
    for tab in root.iter(T + 'table'):
        cells = {}
        r = 0
        for row in tab.iter(T + 'table-row'):
            rr = int(row.get(T + 'number-rows-repeated', '1'))
            c = 0
            for cell in row:
                if cell.tag not in (T + 'table-cell', T + 'covered-table-cell'):
                    continue
                cr = int(cell.get(T + 'number-columns-repeated', '1'))
                if cell.tag == T + 'table-cell' and (cell.get(O + 'value-type') or cell.get(T + 'formula')):
                    vt = cell.get(O + 'value-type')
                    val = cell.get(O + 'value') or cell.get(O + 'date-value') or cell.get(O + 'boolean-value') or cell.get(O + 'string-value')
                    text = '\n'.join(''.join(p.itertext()) for p in cell.findall(TX + 'p'))
                    if val is None:
                        val = text
                    for k in range(min(cr, 4)):
                        name = ''
                        n = c + k + 1
                        while n:
                            n, rem = divmod(n - 1, 26)
                            name = chr(65 + rem) + name
                        cells[f'{name}{r + 1}'] = (vt, val, cell.get(T + 'formula'), text)
                c += cr
            r += rr
        out[tab.get(T + 'name')] = cells
    return out


WN = '{http://schemas.openxmlformats.org/wordprocessingml/2006/main}'


def docx_paras(z):
    root = ET.fromstring(z.read('word/document.xml'))
    out = []
    for para in root.iter(WN + 'p'):
        txt = ''
        for el in para.iter():
            if el.tag == WN + 't' and el.text:
                txt += el.text
            elif el.tag == WN + 'tab':
                txt += '\t'
        out.append(txt)
    return out


def odt_paras(z):
    root = ET.fromstring(z.read('content.xml'))
    body = root.find(O + 'body')
    out = []
    for para in body.iter():
        if para.tag in (TX + 'p', TX + 'h'):
            # its own text, without that of notes and comments inside it
            def text(e):
                t = e.text or '' if e.tag not in (TX + 'note', O + 'annotation') else ''
                for c in e:
                    if c.tag == TX + 's':
                        t += ' ' * int(c.get(TX + 'c', '1'))
                    elif c.tag == TX + 'tab':
                        t += '\t'
                    elif c.tag not in (TX + 'note', O + 'annotation', TX + 'p'):
                        t += text(c)
                    t += c.tail or ''
                return t
            out.append(text(para))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    orig, saved, wanted = sys.argv[1], sys.argv[2], sys.argv[3:]
    z = check_package(saved)
    if orig != '-':
        zo = zipfile.ZipFile(orig)
        a, b = set(zo.namelist()), set(z.namelist())
        for n in sorted(a | b):
            if n not in b:
                print(f'  gone:    {n}')
            elif n not in a:
                print(f'  new:     {n}')
            elif zo.read(n) == z.read(n):
                print(f'  kept:    {n}')
            else:
                print(f'  changed: {n}')
    names = z.namelist()
    if 'word/document.xml' in names or ('content.xml' in names and b'office:text' in z.read('content.xml')):
        paras = docx_paras(z) if 'word/document.xml' in names else odt_paras(z)
        for i, t in enumerate(paras):
            if not wanted or str(i) in wanted:
                print(f'  {i:3d} {t}')
        sys.exit(1 if faults else 0)
    cells = xlsx_cells(z) if 'xl/workbook.xml' in names else ods_cells(z)
    first = next(iter(cells))
    for w in wanted:
        sheet, ref = (w.split('!', 1) if '!' in w else (first, w))
        print(f'  {sheet}!{ref} = {cells.get(sheet, {}).get(ref)}')
    sys.exit(1 if faults else 0)


if __name__ == '__main__':
    main()
