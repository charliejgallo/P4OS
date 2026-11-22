#!/usr/bin/env python3
"""
P4OS - Office: test documents, invented, written by hand.

    python3 apps/oficina/tools/make_samples.py <dir>

    python3 apps/oficina/tools/make_samples.py --en <dir>   (the store's screenshots)

Writes into <dir> (sim/sim_fs/documentos for the simulator) spreadsheets and
texts with everything the app must keep when it saves, whether or not it
understands it: styles, merged cells, frozen panes, comments, a drawing with
a picture, defined names, shared and array formulas, functions the app does
not have, calcChain.xml, extension lists; in ODS repeated rows and cells,
covered cells, annotations, cross-sheet formulas. Only the standard library:
the XML is written as Excel and LibreOffice write it.

Nothing here comes from anyone's documents: names, products and figures are
made up.
"""
import os
import struct
import sys
import zipfile
import zlib

X_NS = 'xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" ' \
       'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'


def png_1x1(rgb=(0x30, 0x90, 0xE0), w=8, h=8):
    raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')


def write_zip(path, parts, stored=()):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in parts:
            if isinstance(data, str):
                data = data.encode('utf-8')
            z.writestr(zipfile.ZipInfo(name, (2026, 11, 22, 10, 0, 0)), data,
                       zipfile.ZIP_STORED if name in stored else zipfile.ZIP_DEFLATED)


def esc(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


# ---------------------------------------------------------------------------
# XLSX: ventas.xlsx
# ---------------------------------------------------------------------------

EN = False      # --en: the same spreadsheet in English, for the store's screenshots


def w(es, en):
    return en if EN else es


def xlsx_ventas(path):
    global SALES, SUMMARY
    SALES, SUMMARY = w('Ventas', 'Sales'), w('Resumen', 'Summary')
    products = [(w('Yerba 1 kg', 'Coffee beans 1 kg'), 4200, 12), (w('Alfajores x12', 'Shortbread x12'), 6300, 8),
                (w('Dulce de leche', 'Honey jar'), 2850, 20), (w('Mate de calabaza', 'Ceramic mug'), 15400, 3),
                (w('Bombilla', 'Tea strainer'), 5100, 6), (w('Termo 1 l', 'Flask 1 l'), 38900, 2),
                (w('Galletitas', 'Biscuits'), 1450, 30), (w('Facturas x6', 'Croissants x6'), 3900, 10)]
    sst = [w('Producto', 'Product'), w('Precio', 'Price'), w('Cantidad', 'Quantity'), 'Total', w('IVA', 'VAT'),
           w('Fecha', 'Date'), w('Total general', 'Grand total'), w('Promedio', 'Average'), w('Máximo', 'Maximum'),
           w('Hoja con ñ y acentos: áéíóú', 'Summary of the month')]
    for p, _, _ in products:
        sst.append(p)

    def si(s):
        return sst.index(s)

    rows = []
    # header, bold with a fill (style 1)
    rows.append('<row r="1" spans="1:6" ht="20" customHeight="1">' + ''.join(
        f'<c r="{c}1" s="1" t="s"><v>{i}</v></c>' for i, c in enumerate('ABCDEF')) + '</row>')
    for i, (p, price, qty) in enumerate(products):
        r = i + 2
        total = price * qty
        f_total = f'<f t="shared" ref="D2:D9" si="0">B2*C2</f>' if i == 0 else '<f t="shared" si="0"/>'
        rows.append(f'<row r="{r}" spans="1:6"><c r="A{r}" t="s"><v>{si(p)}</v></c>'
                    f'<c r="B{r}" s="2"><v>{price}</v></c><c r="C{r}"><v>{qty}</v></c>'
                    f'<c r="D{r}" s="2">{f_total}<v>{total}</v></c>'
                    f'<c r="E{r}" s="3"><f>D{r}*0.21</f><v>{total * 0.21}</v></c>'
                    f'<c r="F{r}" s="4"><v>{46300 + i * 7}</v></c></row>')
    grand = sum(p * q for _, p, q in products)
    rows.append(f'<row r="11"><c r="A11" s="1" t="s"><v>6</v></c>'
                f'<c r="D11" s="5"><f>SUM(D2:D9)</f><v>{grand}</v></c>'
                f'<c r="E11" s="3"><f>SUM(E2:E9)</f><v>{grand * 0.21}</v></c></row>')
    rows.append(f'<row r="12"><c r="A12" t="s"><v>7</v></c>'
                f'<c r="D12" s="2"><f>AVERAGE(D2:D9)</f><v>{grand / 8}</v></c>'
                # a function the app does not have: its value must stay
                f'<c r="E12"><f>_xlfn.STDEV.S(D2:D9)</f><v>34567.89</v></c></row>')
    rows.append(f'<row r="13"><c r="A13" t="s"><v>8</v></c>'
                f'<c r="D13" s="2"><f>MAX(D2:D9)</f><v>{max(p * q for _, p, q in products)}</v></c>'
                f'<c r="E13" t="b"><f>D13&gt;50000</f><v>1</v></c>'
                f'<c r="F13" t="e"><f>1/0</f><v>#DIV/0!</v></c></row>')
    # an array formula, an inline string with rich runs, a lookup into the other sheet
    rows.append('<row r="15"><c r="A15" t="inlineStr"><is><r><rPr><b/></rPr><t>' + w('Buscar:', 'Look up:') + '</t></r><r><t xml:space="preserve"> ' + products[4][0] + '</t></r></is></c>'
                '<c r="B15"><f>VLOOKUP("' + products[4][0] + '",A2:D9,4,FALSE)</f><v>30600</v></c>'
                '<c r="C15"><f t="array" ref="C15">SUM(B2:B9*C2:C9)</f><v>' + str(grand) + '</v></c>'
                '<c r="D15" t="str"><f>CONCATENATE(A2," ' + w('y', 'and') + ' ",A3)</f><v>' + products[0][0] + w(' y ', ' and ') + products[1][0] + '</v></c>'
                '<c r="E15"><f>' + SUMMARY + '!B2*2</f><v>2</v></c>'
                '<c r="F15"><f>ROUND(B2/3,2)</f><v>1400</v></c></row>')
    sheet1 = (HEAD + f'<worksheet {X_NS} xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" '
              'xmlns:x14ac="http://schemas.microsoft.com/office/spreadsheetml/2009/9/ac" mc:Ignorable="x14ac">'
              '<sheetPr><tabColor rgb="FF2E75B6"/></sheetPr><dimension ref="A1:F15"/>'
              '<sheetViews><sheetView tabSelected="1" workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>'
              '<selection pane="bottomLeft" activeCell="B2" sqref="B2"/></sheetView></sheetViews>'
              '<sheetFormatPr defaultRowHeight="15" x14ac:dyDescent="0.25"/>'
              '<cols><col min="1" max="1" width="22.7109375" customWidth="1"/><col min="2" max="5" width="13.28515625" style="2" customWidth="1"/>'
              '<col min="6" max="6" width="12" customWidth="1"/></cols>'
              '<sheetData>' + ''.join(rows) + '</sheetData>'
              '<mergeCells count="1"><mergeCell ref="A11:C11"/></mergeCells>'
              '<conditionalFormatting sqref="D2:D9"><cfRule type="dataBar" priority="1"><dataBar><cfvo type="min"/><cfvo type="max"/>'
              '<color rgb="FF638EC6"/></dataBar></cfRule></conditionalFormatting>'
              '<pageMargins left="0.7" right="0.7" top="0.75" bottom="0.75" header="0.3" footer="0.3"/>'
              '<pageSetup paperSize="9" orientation="portrait"/>'
              '<drawing r:id="rId1"/><legacyDrawing r:id="rId2"/>'
              '<extLst><ext uri="{78C0D931-6437-407d-A8EE-F0AAD7539E65}" xmlns:x14="http://schemas.microsoft.com/office/spreadsheetml/2009/9/main">'
              '<x14:conditionalFormattings/></ext></extLst></worksheet>')
    sheet2 = (HEAD + f'<worksheet {X_NS}><dimension ref="A1:B4"/><sheetViews><sheetView workbookViewId="0"/></sheetViews>'
              '<sheetFormatPr defaultRowHeight="15"/><sheetData>'
              '<row r="1"><c r="A1" t="s"><v>' + '9' + '</v></c></row>'
              '<row r="2"><c r="A2" t="inlineStr"><is><t>' + w('Uno', 'One') + '</t></is></c><c r="B2"><v>1</v></c></row>'
              '<row r="3"><c r="A3" t="inlineStr"><is><t>' + w('Ventas', 'Sales') + '</t></is></c><c r="B3"><f>' + SALES + '!D11</f><v>' + str(grand) + '</v></c></row>'
              '<row r="4"><c r="A4" t="inlineStr"><is><t>' + w('Doble', 'Double') + '</t></is></c><c r="B4"><f>B3*2</f><v>' + str(grand * 2) + '</v></c></row>'
              '</sheetData><pageMargins left="0.7" right="0.7" top="0.75" bottom="0.75" header="0.3" footer="0.3"/></worksheet>')
    sst_xml = HEAD + f'<sst {X_NS.split(" ")[0]} count="{len(sst) + 20}" uniqueCount="{len(sst)}">' + ''.join(
        f'<si><t>{esc(s)}</t></si>' for s in sst) + '</sst>'
    styles = (HEAD + f'<styleSheet {X_NS.split(" ")[0]}>'
              '<numFmts count="2"><numFmt numFmtId="164" formatCode="&quot;$&quot;\\ #,##0.00"/>'
              '<numFmt numFmtId="165" formatCode="dd/mm/yyyy"/></numFmts>'
              '<fonts count="2"><font><sz val="11"/><color theme="1"/><name val="Calibri"/><family val="2"/><scheme val="minor"/></font>'
              '<font><b/><sz val="11"/><color rgb="FFFFFFFF"/><name val="Calibri"/><family val="2"/><scheme val="minor"/></font></fonts>'
              '<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>'
              '<fill><patternFill patternType="solid"><fgColor rgb="FF2E75B6"/><bgColor indexed="64"/></patternFill></fill></fills>'
              '<borders count="2"><border><left/><right/><top/><bottom/><diagonal/></border>'
              '<border><left/><right/><top style="thin"><color indexed="64"/></top><bottom style="double"><color indexed="64"/></bottom><diagonal/></border></borders>'
              '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
              '<cellXfs count="6"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>'
              '<xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment horizontal="center"/></xf>'
              '<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
              '<xf numFmtId="4" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
              '<xf numFmtId="165" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>'
              '<xf numFmtId="164" fontId="1" fillId="2" borderId="1" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1"/>'
              '</cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>'
              '<dxfs count="0"/><tableStyles count="0" defaultTableStyle="TableStyleMedium2" defaultPivotStyle="PivotStyleLight16"/></styleSheet>')
    workbook = (HEAD + f'<workbook {X_NS}><fileVersion appName="xl" lastEdited="7" lowestEdited="7" rupBuild="27425"/>'
                '<workbookPr defaultThemeVersion="164011"/><bookViews><workbookView xWindow="0" yWindow="0" windowWidth="28800" windowHeight="12300"/></bookViews>'
                '<sheets><sheet name="' + SALES + '" sheetId="1" r:id="rId1"/><sheet name="' + SUMMARY + '" sheetId="2" r:id="rId2"/></sheets>'
                '<definedNames><definedName name="Precios">' + SALES + '!$B$2:$B$9</definedName></definedNames>'
                '<calcPr calcId="171027"/></workbook>')
    wb_rels = (HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
               '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
               '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>'
               '<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="theme/theme1.xml"/>'
               '<Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>'
               '<Relationship Id="rId5" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>'
               '<Relationship Id="rId6" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/calcChain" Target="calcChain.xml"/>'
               '</Relationships>')
    s1_rels = (HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
               '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/drawing" Target="../drawings/drawing1.xml"/>'
               '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/vmlDrawing" Target="../drawings/vmlDrawing1.vml"/>'
               '<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments" Target="../comments1.xml"/>'
               '</Relationships>')
    drawing = (HEAD + '<xdr:wsDr xmlns:xdr="http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing" '
               'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><xdr:oneCellAnchor><xdr:from><xdr:col>7</xdr:col>'
               '<xdr:colOff>0</xdr:colOff><xdr:row>1</xdr:row><xdr:rowOff>0</xdr:rowOff></xdr:from><xdr:ext cx="762000" cy="762000"/>'
               '<xdr:pic><xdr:nvPicPr><xdr:cNvPr id="2" name="Logo"/><xdr:cNvPicPr/></xdr:nvPicPr><xdr:blipFill>'
               '<a:blip xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" r:embed="rId1"/><a:stretch><a:fillRect/></a:stretch></xdr:blipFill>'
               '<xdr:spPr><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></xdr:spPr></xdr:pic><xdr:clientData/></xdr:oneCellAnchor></xdr:wsDr>')
    d_rels = (HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
              '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/image1.png"/>'
              '</Relationships>')
    comments = (HEAD + f'<comments {X_NS.split(" ")[0]}><authors><author>Prueba</author></authors><commentList>'
                '<comment ref="B2" authorId="0"><text><r><t>Precio de lista, sin descuento.</t></r></text></comment>'
                '</commentList></comments>')
    vml = ('<xml xmlns:v="urn:schemas-microsoft-com:vml" xmlns:o="urn:schemas-microsoft-com:office:office" '
           'xmlns:x="urn:schemas-microsoft-com:office:excel"><v:shape type="#_x0000_t202" style="visibility:hidden">'
           '<x:ClientData ObjectType="Note"><x:Row>1</x:Row><x:Column>1</x:Column></x:ClientData></v:shape></xml>')
    calc = HEAD + f'<calcChain {X_NS.split(" ")[0]}>' + ''.join(f'<c r="D{r}" i="1"/>' for r in range(2, 10)) + '<c r="D11"/></calcChain>'
    theme = HEAD + '<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme"><a:themeElements/></a:theme>'
    ct = (HEAD + '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="png" ContentType="image/png"/>'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          '<Default Extension="vml" ContentType="application/vnd.openxmlformats-officedocument.vmlDrawing"/>'
          '<Default Extension="xml" ContentType="application/xml"/>'
          '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
          '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
          '<Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
          '<Override PartName="/xl/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>'
          '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>'
          '<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>'
          '<Override PartName="/xl/drawings/drawing1.xml" ContentType="application/vnd.openxmlformats-officedocument.drawing+xml"/>'
          '<Override PartName="/xl/comments1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml"/>'
          '<Override PartName="/xl/calcChain.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.calcChain+xml"/>'
          '<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>'
          '<Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>'
          '</Types>')
    rels = (HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>'
            '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
            '</Relationships>')
    core = (HEAD + '<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" '
            'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" '
            'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>Ventas de prueba</dc:title><dc:creator>Prueba</dc:creator>'
            '<dcterms:created xsi:type="dcterms:W3CDTF">2026-11-22T10:00:00Z</dcterms:created></cp:coreProperties>')
    app = (HEAD + '<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"><Application>Microsoft Excel</Application></Properties>')
    write_zip(path, [('[Content_Types].xml', ct), ('_rels/.rels', rels), ('docProps/app.xml', app), ('docProps/core.xml', core),
                     ('xl/workbook.xml', workbook), ('xl/_rels/workbook.xml.rels', wb_rels), ('xl/worksheets/sheet1.xml', sheet1),
                     ('xl/worksheets/sheet2.xml', sheet2), ('xl/worksheets/_rels/sheet1.xml.rels', s1_rels),
                     ('xl/theme/theme1.xml', theme), ('xl/styles.xml', styles), ('xl/sharedStrings.xml', sst_xml),
                     ('xl/drawings/drawing1.xml', drawing), ('xl/drawings/_rels/drawing1.xml.rels', d_rels),
                     ('xl/drawings/vmlDrawing1.vml', vml), ('xl/comments1.xml', comments), ('xl/media/image1.png', png_1x1()),
                     ('xl/calcChain.xml', calc)], stored=('xl/media/image1.png',))


# ---------------------------------------------------------------------------
# ODS: presupuesto.ods
# ---------------------------------------------------------------------------

O_NS = ('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
        'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
        'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
        'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
        'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
        'xmlns:dc="http://purl.org/dc/elements/1.1/" '
        'xmlns:number="urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0" '
        'xmlns:of="urn:oasis:names:tc:opendocument:xmlns:of:1.2" '
        'xmlns:calcext="urn:org:documentfoundation:names:experimental:calc:xmlns:calcext:1.0" office:version="1.3"')


def ods_presupuesto(path):
    items = [('Pintura látex 20 l', 89000, 2), ('Rodillo y bandeja', 12500, 1), ('Cinta de papel', 2300, 4),
             ('Lija al agua', 900, 10), ('Enduido 5 kg', 18700, 1)]

    def num(v, style='', fmt=None):
        st = f' table:style-name="{style}"' if style else ''
        shown = fmt(v) if fmt else str(v)
        return f'<table:table-cell{st} office:value-type="float" office:value="{v}" calcext:value-type="float"><text:p>{shown}</text:p></table:table-cell>'

    def txt(s, style=''):
        st = f' table:style-name="{style}"' if style else ''
        return f'<table:table-cell{st} office:value-type="string" calcext:value-type="string"><text:p>{esc(s)}</text:p></table:table-cell>'

    money = lambda v: '$ ' + f'{v:,.2f}'.replace(',', 'X').replace('.', ',').replace('X', '.')
    rows = ['<table:table-row table:style-name="ro1">' + txt('Material', 'ce1') + txt('Precio', 'ce1') + txt('Cantidad', 'ce1') +
            txt('Subtotal', 'ce1') + '<table:table-cell table:number-columns-repeated="1020"/></table:table-row>']
    for i, (n, p, q) in enumerate(items):
        r = i + 2
        rows.append('<table:table-row table:style-name="ro1">' + txt(n) + num(p, 'ce2', money) + num(q) +
                    f'<table:table-cell table:style-name="ce2" table:formula="of:=[.B{r}]*[.C{r}]" office:value-type="float" '
                    f'office:value="{p * q}" calcext:value-type="float"><text:p>{money(p * q)}</text:p></table:table-cell>'
                    '<table:table-cell table:number-columns-repeated="1020"/></table:table-row>')
    total = sum(p * q for _, p, q in items)
    rows.append('<table:table-row table:style-name="ro1" table:number-rows-repeated="2"><table:table-cell table:number-columns-repeated="1024"/></table:table-row>')
    rows.append('<table:table-row table:style-name="ro1">' + txt('Total', 'ce1') +
                '<table:table-cell table:number-columns-spanned="2" table:number-rows-spanned="1"/><table:covered-table-cell/>'
                f'<table:table-cell table:style-name="ce3" table:formula="of:=SUM([.D2:.D6])" office:value-type="float" office:value="{total}" '
                f'calcext:value-type="float"><text:p>{money(total)}</text:p></table:table-cell>'
                '<table:table-cell table:number-columns-repeated="1020"/></table:table-row>')
    rows.append('<table:table-row table:style-name="ro1">' + txt('Descuento') +
                '<table:table-cell table:style-name="ce4" office:value-type="percentage" office:value="0.1" calcext:value-type="percentage">'
                '<text:p>10,00%</text:p></table:table-cell><table:table-cell/>'
                f'<table:table-cell table:style-name="ce2" table:formula="of:=[.D9]*(1-[.B10])" office:value-type="float" office:value="{total * 0.9}" '
                f'calcext:value-type="float"><text:p>{money(total * 0.9)}</text:p></table:table-cell>'
                '<table:table-cell table:number-columns-repeated="1020"/></table:table-row>')
    rows.append('<table:table-row table:style-name="ro1">' + txt('Fecha') +
                '<table:table-cell table:style-name="ce5" office:value-type="date" office:date-value="2026-11-22" calcext:value-type="date">'
                '<text:p>22/11/2026</text:p></table:table-cell>'
                '<table:table-cell office:value-type="string" calcext:value-type="string"><office:annotation><dc:creator>Prueba</dc:creator>'
                '<text:p>Pedir factura A</text:p></office:annotation><text:p>con<text:s/>  dos<text:tab/>espacios</text:p></table:table-cell>'
                '<table:table-cell table:formula="of:=[$Notas.B1]" office:value-type="float" office:value="3" calcext:value-type="float"><text:p>3</text:p></table:table-cell>'
                '<table:table-cell table:number-columns-repeated="1020"/></table:table-row>')
    rows.append('<table:table-row table:style-name="ro1" table:number-rows-repeated="1048564"><table:table-cell table:number-columns-repeated="1024"/></table:table-row>')
    t1 = ('<table:table table:name="Presupuesto" table:style-name="ta1"><table:table-column table:style-name="co1" table:default-cell-style-name="Default"/>'
          '<table:table-column table:style-name="co2" table:number-columns-repeated="3" table:default-cell-style-name="Default"/>'
          '<table:table-column table:style-name="co3" table:number-columns-repeated="1020" table:default-cell-style-name="Default"/>' + ''.join(rows) + '</table:table>')
    t2 = ('<table:table table:name="Notas" table:style-name="ta1"><table:table-column table:style-name="co3" table:default-cell-style-name="Default"/>'
          '<table:table-column table:style-name="co3" table:default-cell-style-name="Default"/>'
          '<table:table-row table:style-name="ro1">' + txt('Manos de pintura') + num(3) + '</table:table-row>'
          '<table:table-row table:style-name="ro1">' + txt('Verdadero') +
          '<table:table-cell office:value-type="boolean" office:boolean-value="true" calcext:value-type="boolean"><text:p>VERDADERO</text:p></table:table-cell></table:table-row>'
          '</table:table>')
    content = (f'<?xml version="1.0" encoding="UTF-8"?>\n<office:document-content {O_NS}><office:scripts/>'
               '<office:font-face-decls/><office:automatic-styles>'
               '<style:style style:name="co1" style:family="table-column"><style:table-column-properties fo:break-before="auto" style:column-width="5.2cm"/></style:style>'
               '<style:style style:name="co2" style:family="table-column"><style:table-column-properties fo:break-before="auto" style:column-width="3.1cm"/></style:style>'
               '<style:style style:name="co3" style:family="table-column"><style:table-column-properties fo:break-before="auto" style:column-width="2.258cm"/></style:style>'
               '<style:style style:name="ro1" style:family="table-row"><style:table-row-properties style:row-height="0.452cm" fo:break-before="auto" style:use-optimal-row-height="true"/></style:style>'
               '<style:style style:name="ta1" style:family="table" style:master-page-name="Default"><style:table-properties table:display="true" style:writing-mode="lr-tb"/></style:style>'
               '<number:currency-style style:name="N104P0" style:volatile="true"><number:currency-symbol>$</number:currency-symbol><number:text> </number:text>'
               '<number:number number:decimal-places="2" number:min-decimal-places="2" number:min-integer-digits="1" number:grouping="true"/></number:currency-style>'
               '<number:currency-style style:name="N104"><style:text-properties fo:color="#ff0000"/><number:text>-</number:text><number:currency-symbol>$</number:currency-symbol>'
               '<number:text> </number:text><number:number number:decimal-places="2" number:min-decimal-places="2" number:min-integer-digits="1" number:grouping="true"/>'
               '<style:map style:condition="value()&gt;=0" style:apply-style-name="N104P0"/></number:currency-style>'
               '<number:percentage-style style:name="N11"><number:number number:decimal-places="2" number:min-decimal-places="2" number:min-integer-digits="1"/><number:text>%</number:text></number:percentage-style>'
               '<number:date-style style:name="N37"><number:day number:style="long"/><number:text>/</number:text><number:month number:style="long"/><number:text>/</number:text><number:year number:style="long"/></number:date-style>'
               '<style:style style:name="ce1" style:family="table-cell" style:parent-style-name="Default"><style:table-cell-properties fo:background-color="#729fcf"/>'
               '<style:text-properties fo:font-weight="bold"/></style:style>'
               '<style:style style:name="ce2" style:family="table-cell" style:parent-style-name="Default" style:data-style-name="N104"/>'
               '<style:style style:name="ce3" style:family="table-cell" style:parent-style-name="Default" style:data-style-name="N104"><style:text-properties fo:font-weight="bold"/></style:style>'
               '<style:style style:name="ce4" style:family="table-cell" style:parent-style-name="Default" style:data-style-name="N11"/>'
               '<style:style style:name="ce5" style:family="table-cell" style:parent-style-name="Default" style:data-style-name="N37"/>'
               '</office:automatic-styles><office:body><office:spreadsheet><table:calculation-settings table:automatic-find-labels="false"/>'
               + t1 + t2 + '<table:named-expressions/></office:spreadsheet></office:body></office:document-content>')
    styles = (f'<?xml version="1.0" encoding="UTF-8"?>\n<office:document-styles {O_NS}><office:styles>'
              '<style:style style:name="Default" style:family="table-cell"/></office:styles></office:document-styles>')
    meta = (f'<?xml version="1.0" encoding="UTF-8"?>\n<office:document-meta {O_NS} xmlns:meta="urn:oasis:names:tc:opendocument:xmlns:meta:1.0">'
            '<office:meta><meta:generator>make_samples.py</meta:generator><dc:title>Presupuesto de prueba</dc:title></office:meta></office:document-meta>')
    settings = (f'<?xml version="1.0" encoding="UTF-8"?>\n<office:document-settings {O_NS} xmlns:config="urn:oasis:names:tc:opendocument:xmlns:config:1.0">'
                '<office:settings><config:config-item-set config:name="ooo:view-settings"/></office:settings></office:document-settings>')
    manifest = ('<?xml version="1.0" encoding="UTF-8"?>\n<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.3">'
                '<manifest:file-entry manifest:full-path="/" manifest:version="1.3" manifest:media-type="application/vnd.oasis.opendocument.spreadsheet"/>'
                '<manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="meta.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="settings.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="Thumbnails/thumbnail.png" manifest:media-type="image/png"/>'
                '</manifest:manifest>')
    write_zip(path, [('mimetype', 'application/vnd.oasis.opendocument.spreadsheet'), ('content.xml', content), ('styles.xml', styles),
                     ('meta.xml', meta), ('settings.xml', settings), ('Thumbnails/thumbnail.png', png_1x1((0xE0, 0x80, 0x30))),
                     ('META-INF/manifest.xml', manifest)], stored=('mimetype',))


# ---------------------------------------------------------------------------
# Text and CSV
# ---------------------------------------------------------------------------

def texts(d):
    with open(os.path.join(d, 'gastos.csv'), 'wb') as f:
        lines = ['Fecha;Concepto;Monto;Pagado', '01/11/2026;Alquiler;350.000,00;VERDADERO', '03/11/2026;Luz;28.450,50;VERDADERO',
                 '05/11/2026;"Supermercado; semana 1";61.230,75;FALSO', '12/11/2026;Internet;19.999,00;VERDADERO',
                 '15/11/2026;"Regalo ""sorpresa""";15.000,00;FALSO', '20/11/2026;Peluquería;12.000,00;VERDADERO']
        f.write('\r\n'.join(lines).encode('cp1252') + b'\r\n')
    with open(os.path.join(d, 'temperaturas.csv'), 'w', encoding='utf-8') as f:
        f.write('city,jan,feb,mar,apr\nBuenos Aires,24.9,23.9,21.9,18.0\nCórdoba,24.6,23.4,21.6,18.1\n'
                'Ushuaia,9.6,9.3,7.8,5.0\nMendoza,25.3,23.9,21.0,16.4\n')
    with open(os.path.join(d, 'carta.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('Querida Ana:\n\nTe escribo desde la placa, con ñ y acentos: á é í ó ú ü.\n'
                'Esta carta tiene finales de línea de Windows (CRLF), que la app tiene que conservar.\n\n'
                'Un abrazo,\nLuis\n')
    with open(os.path.join(d, 'config.ini'), 'w', encoding='utf-8') as f:
        f.write('; configuración de prueba\n[red]\nnombre = taller\npuerto = 8080\n\n[pantalla]\nbrillo = 80\n# un comentario\n')
    with open(os.path.join(d, 'datos.json'), 'w', encoding='utf-8') as f:
        f.write('{\n  "nombre": "Prueba",\n  "version": 3,\n  "activo": true,\n  "medidas": [1.5, 2.25, -3e4],\n'
                '  "sin": null,\n  "anidado": { "clave": "valor con \\"comillas\\"" }\n}\n')
    with open(os.path.join(d, 'LEEME.md'), 'w', encoding='utf-8') as f:
        f.write('# Oficina\n\nUn archivo **Markdown** de prueba, con *cursiva* y `código`.\n\n- uno\n- dos\n\n> una cita\n')
    with open(os.path.join(d, 'programa.py'), 'w', encoding='utf-8') as f:
        f.write('# un programa de prueba\ndef suma(a, b):\n    """Devuelve la suma."""\n    return a + b  # fácil\n\n'
                'for i in range(10):\n    print(f"{i}: {suma(i, 2)}")\n')
    with open(os.path.join(d, 'registro.log'), 'w', encoding='utf-8') as f:
        import random
        rnd = random.Random(7)
        levels = ['INFO', 'INFO', 'INFO', 'WARN', 'ERROR', 'DEBUG']
        for i in range(60000):
            f.write(f'2026-11-22 {i // 3600 % 24:02d}:{i // 60 % 60:02d}:{i % 60:02d}.{rnd.randint(0, 999):03d} '
                    f'{rnd.choice(levels):5s} sensor{rnd.randint(1, 9)}: lectura {rnd.uniform(10, 40):.2f} C, ciclo {i}\n')
    with open(os.path.join(d, 'latin1.txt'), 'wb') as f:
        f.write('Texto en Latin-1: año, pingüino, café.\n'.encode('latin-1'))


def texts_en(d):
    with open(os.path.join(d, 'notes.md'), 'w', encoding='utf-8') as f:
        f.write('# Workshop notes\n\nThings to bring on **Saturday**:\n\n- the long ruler\n- two clamps\n'
                '- `M4` screws, *not* the M3 ones\n\n> Measure twice, cut once.\n\n## Paint\n\n'
                'One coat of primer, then [the blue one](https://example.com).\n')
    with open(os.path.join(d, 'stock.py'), 'w', encoding='utf-8') as f:
        f.write('# Stock levels from the sales sheet\nimport csv\n\nLOW = 5  # reorder below this\n\n\n'
                'def low_items(path):\n    """The products to order again."""\n    with open(path, newline="") as f:\n'
                '        for row in csv.DictReader(f):\n            if int(row["quantity"]) < LOW:\n'
                '                yield row["product"], int(row["quantity"])\n\n\n'
                'for name, n in low_items("sales.csv"):\n    print(f"{name}: only {n} left")\n')
    with open(os.path.join(d, 'settings.json'), 'w', encoding='utf-8') as f:
        f.write('{\n  "name": "Workshop",\n  "version": 3,\n  "enabled": true,\n  "limits": [1.5, 2.25, -30000]\n}\n')
    with open(os.path.join(d, 'temperatures.csv'), 'w', encoding='utf-8') as f:
        f.write('city,jan,feb,mar,apr\nLisbon,15.0,16.2,18.5,19.8\nOslo,-2.3,-2.0,1.6,6.3\nCairo,19.1,20.8,23.7,27.6\n')


def main():
    global EN
    args = [a for a in sys.argv[1:] if a != '--en']
    EN = '--en' in sys.argv
    d = args[0] if args else 'sim/sim_fs/documentos'
    os.makedirs(d, exist_ok=True)
    import samples_docs as sd
    if EN:
        sd.docx(os.path.join(d, 'report.docx'), sd.EN)
        sd.odt(os.path.join(d, 'letter.odt'), sd.EN)
        xlsx_ventas(os.path.join(d, 'sales.xlsx'))
        texts_en(d)
        print('samples (English) in', d)
        return
    sd.docx(os.path.join(d, 'informe.docx'), sd.ES)
    sd.odt(os.path.join(d, 'carta.odt'), sd.ES)
    xlsx_ventas(os.path.join(d, 'ventas.xlsx'))
    ods_presupuesto(os.path.join(d, 'presupuesto.ods'))
    texts(d)
    print('samples in', d)


if __name__ == '__main__':
    main()
