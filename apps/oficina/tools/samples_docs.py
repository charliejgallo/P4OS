"""
P4OS - Office: the test documents (docx and odt), for make_samples.py.

Invented text, written the way Word and LibreOffice write it, with what the
app has to keep when it saves: styles, numbering, a table, a picture, a
footnote, a field, a bookmark, a comment, a hyperlink, a section break, a
header and a footer, w14 paragraph ids, and one paragraph with a tracked
insertion (which the app must show and lock).
"""
import struct
import zipfile
import zlib

HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
W = ('xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
     'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
     'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" '
     'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
     'xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture" '
     'xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml" '
     'xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" mc:Ignorable="w14"')


def png(rgb, w=48, h=32):
    raw = b''.join(b'\x00' + b''.join(bytes(rgb if (x // 8 + y // 8) % 2 else (255, 255, 255)) for x in range(w)) for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')


def landscape(w=240, h=160):
    """a drawn picture: sky, sun, two hills (no photo of anyone's)"""
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = 90 + y // 2, 150 + y // 3, 230
            if (x - 180) ** 2 + (y - 40) ** 2 < 22 ** 2:
                r, g, b = 255, 214, 90
            if y > 110 - 30 * ((x / w) ** 2) and y > 95 + 25 * abs((x - 70) / 120):
                r, g, b = 60, 150 + (y % 7) * 3, 70
            if y > 130 + 8 * ((x - 150) / 150) ** 2:
                r, g, b = 40, 120, 55
            row += bytes((r, g, b))
        rows.append(b'\x00' + bytes(row))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')


def esc(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def write_zip(path, parts, stored=()):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in parts:
            if isinstance(data, str):
                data = data.encode('utf-8')
            z.writestr(zipfile.ZipInfo(name, (2026, 11, 22, 10, 0, 0)), data,
                       zipfile.ZIP_STORED if name in stored else zipfile.ZIP_DEFLATED)


def docx(path, t):
    """t: the texts, Spanish or English"""
    pid = [0]

    def P(inner, ppr=''):
        pid[0] += 1
        return f'<w:p w14:paraId="{pid[0]:08X}" w14:textId="77777777" w:rsidR="00A1B2C3">{ppr}{inner}</w:p>'

    def R(text, rpr=''):
        return f'<w:r>{"<w:rPr>" + rpr + "</w:rPr>" if rpr else ""}<w:t xml:space="preserve">{esc(text)}</w:t></w:r>'

    body = []
    body.append(P(R(t['title']), '<w:pPr><w:pStyle w:val="Title"/></w:pPr>'))
    body.append(P(R(t['intro1']) + R(t['bold'], '<w:b/><w:color w:val="C00000"/>') + R(t['intro2']) +
                  R(t['italic'], '<w:rFonts w:ascii="Georgia" w:hAnsi="Georgia"/><w:i/><w:lang w:val="es-AR"/>') + R('.')))
    body.append(P(R(t['h1']), '<w:pPr><w:pStyle w:val="Heading1"/></w:pPr>'))
    body.append(P(R(t['p2a']) +
                  '<w:hyperlink r:id="rId20" w:history="1"><w:r><w:rPr><w:rStyle w:val="Hyperlink"/></w:rPr>'
                  f'<w:t>{esc(t["link"])}</w:t></w:r></w:hyperlink>' + R(t['p2b']) +
                  '<w:r><w:rPr><w:rStyle w:val="FootnoteReference"/></w:rPr><w:footnoteReference w:id="1"/></w:r>' + R('.')))
    for item in t['bullets']:
        body.append(P(R(item), '<w:pPr><w:pStyle w:val="ListParagraph"/><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr>'))
    body.append(P(R(t['h2']), '<w:pPr><w:pStyle w:val="Heading2"/></w:pPr>'))
    for item in t['steps']:
        body.append(P(R(item), '<w:pPr><w:pStyle w:val="ListParagraph"/><w:numPr><w:ilvl w:val="0"/><w:numId w:val="2"/></w:numPr></w:pPr>'))
    # a table
    rows = t['table']
    tbl = ('<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="0" w:type="auto"/><w:tblLook w:val="04A0"/></w:tblPr>'
           '<w:tblGrid><w:gridCol w:w="3600"/><w:gridCol w:w="2400"/><w:gridCol w:w="2400"/></w:tblGrid>')
    for i, row in enumerate(rows):
        tbl += '<w:tr>'
        for cell in row:
            rpr = '<w:b/>' if i == 0 else ''
            tbl += f'<w:tc><w:tcPr><w:tcW w:w="2400" w:type="dxa"/></w:tcPr>{P(R(cell, rpr))}</w:tc>'
        tbl += '</w:tr>'
    tbl += '</w:tbl>'
    body.append(tbl)
    # a picture, a bookmark, a field, a comment
    body.append(P('<w:bookmarkStart w:id="0" w:name="foto"/>' + R(t['pic']) +
                  '<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="1828800" cy="1219200"/>'
                  '<wp:docPr id="1" name="Imagen 1"/><a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">'
                  '<pic:pic><pic:nvPicPr><pic:cNvPr id="0" name="foto.png"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>'
                  '<a:blip r:embed="rId21"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x="0" y="0"/>'
                  '<a:ext cx="1828800" cy="1219200"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>'
                  '</a:graphicData></a:graphic></wp:inline></w:drawing></w:r><w:bookmarkEnd w:id="0"/>'))
    body.append(P(R(t['page']) + '<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> PAGE </w:instrText></w:r>'
                  '<w:r><w:fldChar w:fldCharType="separate"/></w:r>' + R('1') + '<w:r><w:fldChar w:fldCharType="end"/></w:r>' +
                  '<w:commentRangeStart w:id="0"/>' + R(t['commented']) + '<w:commentRangeEnd w:id="0"/>'
                  '<w:r><w:rPr><w:rStyle w:val="CommentReference"/></w:rPr><w:commentReference w:id="0"/></w:r>'))
    # a section break in a paragraph
    body.append(P(R(t['sect']), '<w:pPr><w:sectPr><w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1417" w:right="1701" '
                  'w:bottom="1417" w:left="1701" w:header="708" w:footer="708" w:gutter="0"/><w:cols w:space="708"/></w:sectPr></w:pPr>'))
    # tracked changes: shown, locked
    body.append(P(R(t['track1']) + '<w:ins w:id="5" w:author="Prueba" w:date="2026-11-20T10:00:00Z">' + R(t['track2']) + '</w:ins>' +
                  '<w:del w:id="6" w:author="Prueba" w:date="2026-11-20T10:00:00Z"><w:r><w:delText>' + esc(t['track3']) + '</w:delText></w:r></w:del>'))
    body.append(P(R(t['end'])))
    body.append('<w:p/>')
    document = (HEAD + f'<w:document {W}><w:body>' + ''.join(body) +
                '<w:sectPr w:rsidR="00A1B2C3"><w:headerReference w:type="default" r:id="rId22"/><w:footerReference w:type="default" r:id="rId23"/>'
                '<w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1417" w:right="1701" w:bottom="1417" w:left="1701" w:header="708" '
                'w:footer="708" w:gutter="0"/><w:cols w:space="708"/></w:sectPr></w:body></w:document>')
    styles = (HEAD + '<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
              '<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri"/><w:sz w:val="22"/><w:szCs w:val="22"/>'
              '<w:lang w:val="es-AR"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="259" w:lineRule="auto"/>'
              '</w:pPr></w:pPrDefault></w:docDefaults>'
              '<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/></w:style>'
              '<w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/>'
              '<w:qFormat/><w:rPr><w:sz w:val="52"/><w:color w:val="1F3864"/></w:rPr></w:style>'
              '<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/>'
              '<w:qFormat/><w:pPr><w:keepNext/><w:spacing w:before="240"/><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:b/><w:sz w:val="32"/>'
              '<w:color w:val="2F5496"/></w:rPr></w:style>'
              '<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/>'
              '<w:qFormat/><w:pPr><w:keepNext/><w:outlineLvl w:val="1"/></w:pPr><w:rPr><w:b/><w:sz w:val="26"/><w:color w:val="2F5496"/></w:rPr></w:style>'
              '<w:style w:type="paragraph" w:styleId="ListParagraph"><w:name w:val="List Paragraph"/><w:basedOn w:val="Normal"/>'
              '<w:pPr><w:ind w:left="720"/></w:pPr></w:style>'
              '<w:style w:type="character" w:styleId="Hyperlink"><w:name w:val="Hyperlink"/><w:rPr><w:color w:val="0563C1"/>'
              '<w:u w:val="single"/></w:rPr></w:style>'
              '<w:style w:type="character" w:styleId="FootnoteReference"><w:name w:val="footnote reference"/><w:rPr><w:vertAlign w:val="superscript"/></w:rPr></w:style>'
              '<w:style w:type="character" w:styleId="CommentReference"><w:name w:val="annotation reference"/><w:rPr><w:sz w:val="16"/></w:rPr></w:style>'
              '<w:style w:type="table" w:styleId="TableGrid"><w:name w:val="Table Grid"/><w:tblPr><w:tblBorders>'
              '<w:top w:val="single" w:sz="4"/><w:left w:val="single" w:sz="4"/><w:bottom w:val="single" w:sz="4"/>'
              '<w:right w:val="single" w:sz="4"/><w:insideH w:val="single" w:sz="4"/><w:insideV w:val="single" w:sz="4"/></w:tblBorders></w:tblPr></w:style>'
              '</w:styles>')
    numbering = (HEAD + '<w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                 '<w:abstractNum w:abstractNumId="0"><w:multiLevelType w:val="hybridMultilevel"/><w:lvl w:ilvl="0"><w:start w:val="1"/>'
                 '<w:numFmt w:val="bullet"/><w:lvlText w:val=""/><w:lvlJc w:val="left"/><w:pPr><w:ind w:left="720" w:hanging="360"/></w:pPr>'
                 '<w:rPr><w:rFonts w:ascii="Symbol" w:hAnsi="Symbol" w:hint="default"/></w:rPr></w:lvl></w:abstractNum>'
                 '<w:abstractNum w:abstractNumId="1"><w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="decimal"/><w:lvlText w:val="%1."/>'
                 '<w:lvlJc w:val="left"/><w:pPr><w:ind w:left="720" w:hanging="360"/></w:pPr></w:lvl></w:abstractNum>'
                 '<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num><w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num></w:numbering>')
    footnotes = (HEAD + '<w:footnotes xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                 '<w:footnote w:type="separator" w:id="-1"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>'
                 '<w:footnote w:type="continuationSeparator" w:id="0"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>'
                 f'<w:footnote w:id="1"><w:p><w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:footnoteRef/></w:r><w:r><w:t xml:space="preserve"> {esc(t["note"])}</w:t></w:r></w:p></w:footnote>'
                 '</w:footnotes>')
    comments = (HEAD + '<w:comments xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                f'<w:comment w:id="0" w:author="Prueba" w:date="2026-11-20T10:00:00Z" w:initials="P"><w:p><w:r><w:t>{esc(t["comment"])}</w:t></w:r></w:p></w:comment></w:comments>')
    header = (HEAD + f'<w:hdr xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:p><w:r><w:t>{esc(t["header"])}</w:t></w:r></w:p></w:hdr>')
    footer = (HEAD + f'<w:ftr xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:p><w:r><w:t>{esc(t["footer"])}</w:t></w:r></w:p></w:ftr>')
    settings = (HEAD + '<w:settings xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:zoom w:percent="100"/>'
                '<w:defaultTabStop w:val="708"/><w:characterSpacingControl w:val="doNotCompress"/></w:settings>')
    rels = (HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>'
            '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/>'
            '<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>'
            '<Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes" Target="footnotes.xml"/>'
            '<Relationship Id="rId5" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments" Target="comments.xml"/>'
            '<Relationship Id="rId20" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://example.com/" TargetMode="External"/>'
            '<Relationship Id="rId21" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/image1.png"/>'
            '<Relationship Id="rId22" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/header" Target="header1.xml"/>'
            '<Relationship Id="rId23" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer" Target="footer1.xml"/>'
            '</Relationships>')
    ct = (HEAD + '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="png" ContentType="image/png"/>'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          '<Default Extension="xml" ContentType="application/xml"/>'
          '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
          '<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>'
          '<Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/>'
          '<Override PartName="/word/settings.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml"/>'
          '<Override PartName="/word/footnotes.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml"/>'
          '<Override PartName="/word/comments.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.comments+xml"/>'
          '<Override PartName="/word/header1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"/>'
          '<Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>'
          '<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>'
          '</Types>')
    top = (HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
           '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>'
           '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>'
           '</Relationships>')
    core = (HEAD + '<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" '
            f'xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>{esc(t["title"])}</dc:title><dc:creator>Prueba</dc:creator></cp:coreProperties>')
    write_zip(path, [('[Content_Types].xml', ct), ('_rels/.rels', top), ('docProps/core.xml', core), ('word/document.xml', document),
                     ('word/_rels/document.xml.rels', rels), ('word/styles.xml', styles), ('word/numbering.xml', numbering),
                     ('word/settings.xml', settings), ('word/footnotes.xml', footnotes), ('word/comments.xml', comments),
                     ('word/header1.xml', header), ('word/footer1.xml', footer), ('word/media/image1.png', landscape())],
              stored=('word/media/image1.png',))


O_NS = ('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
        'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
        'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
        'xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:dc="http://purl.org/dc/elements/1.1/" '
        'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
        'xmlns:loext="urn:org:documentfoundation:names:experimental:office:xmlns:loext:1.0" office:version="1.3"')


def odt(path, t):
    def p(inner, style='P1'):
        return f'<text:p text:style-name="{style}">{inner}</text:p>'

    body = [f'<text:h text:style-name="Heading_20_1" text:outline-level="1">{esc(t["title"])}</text:h>',
            p(f'{esc(t["intro1"])}<text:span text:style-name="T1">{esc(t["bold"])}</text:span>{esc(t["intro2"])}'
              f'<text:span text:style-name="T2">{esc(t["italic"])}<text:span text:style-name="T3"> {esc(t["red"])}</text:span></text:span>.'),
            f'<text:h text:style-name="Heading_20_2" text:outline-level="2">{esc(t["h1"])}</text:h>',
            p(f'{esc(t["p2a"])}<text:a xlink:type="simple" xlink:href="https://example.com/">{esc(t["link"])}</text:a>{esc(t["p2b"])}'
              f'<text:note text:id="ftn1" text:note-class="footnote"><text:note-citation>1</text:note-citation><text:note-body>'
              f'<text:p text:style-name="Footnote">{esc(t["note"])}</text:p></text:note-body></text:note>.'),
            '<text:list text:style-name="L1">' + ''.join(f'<text:list-item>{p(esc(x))}</text:list-item>' for x in t['bullets'][:2]) +
            f'<text:list-item>{p(esc(t["bullets"][2]))}<text:list>' +
            ''.join(f'<text:list-item>{p(esc(x))}</text:list-item>' for x in t['sub']) + '</text:list></text:list-item></text:list>',
            f'<text:h text:style-name="Heading_20_2" text:outline-level="2">{esc(t["h2"])}</text:h>',
            '<text:list text:style-name="L2">' + ''.join(f'<text:list-item>{p(esc(x))}</text:list-item>' for x in t['steps']) + '</text:list>']
    tbl = '<table:table table:name="Tabla1" table:style-name="Tabla1"><table:table-column table:number-columns-repeated="3"/>'
    for i, row in enumerate(t['table']):
        tbl += '<table:table-row>'
        for c in row:
            inner = '<text:span text:style-name="T1">' + esc(c) + '</text:span>' if i == 0 else esc(c)
            tbl += '<table:table-cell office:value-type="string">' + p(inner) + '</table:table-cell>'
        tbl += '</table:table-row>'
    tbl += '</table:table>'
    body.append(tbl)
    body.append(p(f'<text:bookmark text:name="foto"/>{esc(t["pic"])}<draw:frame draw:name="Imagen1" text:anchor-type="as-char" '
                  'svg:width="5.08cm" svg:height="3.387cm" draw:z-index="0"><draw:image xlink:href="Pictures/foto.png" '
                  'xlink:type="simple" xlink:show="embed" xlink:actuate="onLoad"/></draw:frame>'))
    body.append(p(f'{esc(t["page"])}<text:date style:data-style-name="N37" text:date-value="2026-11-22">22/11/2026</text:date>'
                  f'<office:annotation><dc:creator>Prueba</dc:creator><text:p>{esc(t["comment"])}</text:p></office:annotation>'
                  f' {esc(t["commented"])}  con<text:s text:c="3"/>espacios<text:tab/>y tab.'))
    body.append(p(f'{esc(t["track1"])}<text:change-start text:change-id="ct1"/>{esc(t["track2"])}<text:change-end text:change-id="ct1"/>'))
    body.append(p(esc(t['end'])))
    body.append('<text:p text:style-name="P1"/>')
    content = (f'<?xml version="1.0" encoding="UTF-8"?>\n<office:document-content {O_NS}><office:scripts/><office:font-face-decls/>'
               '<office:automatic-styles>'
               '<style:style style:name="P1" style:family="paragraph" style:parent-style-name="Text_20_body"/>'
               '<style:style style:name="T1" style:family="text"><style:text-properties fo:font-weight="bold" style:font-weight-asian="bold"/></style:style>'
               '<style:style style:name="T2" style:family="text"><style:text-properties fo:font-style="italic"/></style:style>'
               '<style:style style:name="T3" style:family="text"><style:text-properties fo:color="#c9211e"/></style:style>'
               '<style:style style:name="Tabla1" style:family="table"><style:table-properties style:width="16cm"/></style:style>'
               '</office:automatic-styles><office:body><office:text>'
               '<text:tracked-changes><text:changed-region text:id="ct1"><text:insertion><office:change-info><dc:creator>Prueba</dc:creator>'
               '<dc:date>2026-11-20T10:00:00</dc:date></office:change-info></text:insertion></text:changed-region></text:tracked-changes>'
               '<text:sequence-decls><text:sequence-decl text:display-outline-level="0" text:name="Illustration"/></text:sequence-decls>'
               + ''.join(body) + '</office:text></office:body></office:document-content>')
    styles = (f'<?xml version="1.0" encoding="UTF-8"?>\n<office:document-styles {O_NS}><office:styles>'
              '<style:default-style style:family="paragraph"><style:text-properties fo:font-size="12pt"/></style:default-style>'
              '<style:style style:name="Standard" style:family="paragraph" style:class="text"/>'
              '<style:style style:name="Text_20_body" style:display-name="Text body" style:family="paragraph" style:parent-style-name="Standard">'
              '<style:paragraph-properties fo:margin-bottom="0.247cm"/></style:style>'
              '<style:style style:name="Heading" style:family="paragraph" style:parent-style-name="Standard"><style:text-properties fo:font-size="14pt"/></style:style>'
              '<style:style style:name="Heading_20_1" style:display-name="Heading 1" style:family="paragraph" style:parent-style-name="Heading" '
              'style:default-outline-level="1"><style:text-properties fo:font-size="130%" fo:font-weight="bold"/></style:style>'
              '<style:style style:name="Heading_20_2" style:display-name="Heading 2" style:family="paragraph" style:parent-style-name="Heading" '
              'style:default-outline-level="2"><style:text-properties fo:font-size="115%" fo:font-weight="bold"/></style:style>'
              '<style:style style:name="Footnote" style:family="paragraph" style:parent-style-name="Standard"><style:text-properties fo:font-size="10pt"/></style:style>'
              '<text:list-style style:name="L1"><text:list-level-style-bullet text:level="1" text:bullet-char="•"/>'
              '<text:list-level-style-bullet text:level="2" text:bullet-char="◦"/></text:list-style>'
              '<text:list-style style:name="L2"><text:list-level-style-number text:level="1" style:num-suffix="." style:num-format="1"/></text:list-style>'
              '</office:styles><office:master-styles><style:master-page style:name="Standard"><style:header>'
              f'<text:p text:style-name="Standard">{esc(t["header"])}</text:p></style:header></style:master-page></office:master-styles></office:document-styles>')
    meta = (f'<?xml version="1.0" encoding="UTF-8"?>\n<office:document-meta {O_NS} xmlns:meta="urn:oasis:names:tc:opendocument:xmlns:meta:1.0">'
            f'<office:meta><dc:title>{esc(t["title"])}</dc:title></office:meta></office:document-meta>')
    manifest = ('<?xml version="1.0" encoding="UTF-8"?>\n<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.3">'
                '<manifest:file-entry manifest:full-path="/" manifest:version="1.3" manifest:media-type="application/vnd.oasis.opendocument.text"/>'
                '<manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="meta.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="Pictures/foto.png" manifest:media-type="image/png"/>'
                '</manifest:manifest>')
    write_zip(path, [('mimetype', 'application/vnd.oasis.opendocument.text'), ('content.xml', content), ('styles.xml', styles),
                     ('meta.xml', meta), ('Pictures/foto.png', landscape()), ('META-INF/manifest.xml', manifest)],
              stored=('mimetype', 'Pictures/foto.png'))


ES = {
    'title': 'Informe del taller', 'intro1': 'Este documento tiene texto en ', 'bold': 'negrita roja',
    'intro2': ', en ', 'italic': 'cursiva con otra letra', 'red': 'y roja',
    'h1': 'Lo que pasó en noviembre', 'p2a': 'El detalle está en ', 'link': 'la página del proyecto',
    'p2b': ', con una nota al pie', 'note': 'Una nota al pie de prueba.',
    'bullets': ['Ordenar las herramientas', 'Pintar la puerta', 'Arreglar la mesa'], 'sub': ['Lijar', 'Barnizar'],
    'h2': 'Pasos para la semana que viene', 'steps': ['Comprar tornillos', 'Medir dos veces', 'Cortar una vez'],
    'table': [['Material', 'Cantidad', 'Precio'], ['Tornillos M4', '100', '$ 2.500'], ['Barniz 1 l', '2', '$ 18.000']],
    'pic': 'Una foto: ', 'page': 'Página ', 'commented': ' texto con un comentario', 'comment': 'Revisar esto.',
    'sect': 'Fin de la primera sección.', 'track1': 'Un párrafo con ', 'track2': 'texto insertado con control de cambios',
    'track3': ' y texto borrado', 'end': 'Último párrafo, con ñ, acentos y «comillas».',
    'header': 'Taller - noviembre', 'footer': 'Hoja de prueba',
}
EN = {
    'title': 'Workshop report', 'intro1': 'This document has text in ', 'bold': 'red bold',
    'intro2': ', in ', 'italic': 'italics in another font', 'red': 'and red',
    'h1': 'What happened in November', 'p2a': 'The details are on ', 'link': 'the project page',
    'p2b': ', with a footnote', 'note': 'A test footnote.',
    'bullets': ['Sort out the tools', 'Paint the door', 'Fix the table'], 'sub': ['Sand it', 'Varnish it'],
    'h2': 'Steps for next week', 'steps': ['Buy screws', 'Measure twice', 'Cut once'],
    'table': [['Material', 'Quantity', 'Price'], ['M4 screws', '100', '$ 25.00'], ['Varnish 1 l', '2', '$ 18.00']],
    'pic': 'A picture: ', 'page': 'Page ', 'commented': ' text with a comment', 'comment': 'Check this.',
    'sect': 'End of the first section.', 'track1': 'A paragraph with ', 'track2': 'text inserted with track changes',
    'track3': ' and deleted text', 'end': 'Last paragraph, with “quotes” and an accent: café.',
    'header': 'Workshop - November', 'footer': 'Test sheet',
}
