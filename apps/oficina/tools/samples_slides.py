#!/usr/bin/env python3
"""Invented presentations for Office's slide reader (phase 5):

    python3 apps/oficina/tools/samples_slides.py DIR [--en]

  charla.pptx / talk.pptx   PowerPoint: a theme, a master with a decorative
                            band and its text styles, a title layout (no
                            master shapes) and a content layout; slides with
                            a title and subtitle placed by their layout,
                            bullets in three levels and speaker notes, a
                            picture with a caption and a link, shapes filled
                            by the theme's style (rectangle, ellipse, rounded
                            rectangle, arrow, a connector, a group with child
                            offsets), a table with a first row and bands, a
                            hidden slide with a chart, a slide with its own
                            background
  charla.odp / talk.odp     Impress: the same deck, ODF's way

Everything is invented.
"""
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(__file__))
import samples_docs as sd  # noqa: E402

EMU = 12700

TXT = {
    'es': dict(file='charla', title='La huerta del barrio', subtitle='Charla para vecinos · primavera 2026',
               h2='Qué vamos a plantar', bullets=[(0, 'Verduras de hoja'), (1, 'Lechuga, acelga y espinaca'), (1, 'Rúcula, que crece rápido'),
                                                  (2, 'Se siembra cada dos semanas'), (0, 'Aromáticas'), (1, 'Albahaca, perejil y menta'),
                                                  (0, 'Tomates, si hay sol')],
               notes='Contar la historia del terreno. Preguntar quién tiene semillas.',
               h3='El terreno', caption='El lote de la esquina, antes de limpiarlo.', link='Mapa del barrio',
               h4='Cómo nos organizamos', boxes=['Riego', 'Compost', 'Semillas', 'Turnos'], arrow='Cosecha',
               h5='Turnos de riego', table=[('Día', 'Mañana', 'Tarde'), ('Lunes', 'Ana', 'Bruno'), ('Miércoles', 'Carla', 'Diego'), ('Sábado', 'Todos', '—')],
               h6='Gastos (borrador)', chart='Gráfico de gastos', h7='¡Gracias!', end='Preguntas y sugerencias: huerta@ejemplo.com'),
    'en': dict(file='talk', title='The neighbourhood garden', subtitle='A talk for neighbours · spring 2026',
               h2='What we will plant', bullets=[(0, 'Leafy greens'), (1, 'Lettuce, chard and spinach'), (1, 'Rocket, which grows fast'),
                                                 (2, 'Sown every two weeks'), (0, 'Herbs'), (1, 'Basil, parsley and mint'), (0, 'Tomatoes, if there is sun')],
               notes='Tell the story of the plot. Ask who has seeds.',
               h3='The plot', caption='The corner lot, before cleaning it.', link='Neighbourhood map',
               h4='How we organise', boxes=['Watering', 'Compost', 'Seeds', 'Shifts'], arrow='Harvest',
               h5='Watering shifts', table=[('Day', 'Morning', 'Afternoon'), ('Monday', 'Ann', 'Bruce'), ('Wednesday', 'Carol', 'Dave'), ('Saturday', 'Everyone', '—')],
               h6='Expenses (draft)', chart='Expenses chart', h7='Thank you!', end='Questions and ideas: garden@example.com'),
}

NS = ('xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
      'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
      'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"')
REL = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'


def esc(s):
    return sd.esc(s)


def rels(items):
    return HEAD + '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' + ''.join(
        '<Relationship Id="%s" Type="%s/%s" Target="%s"%s/>' % (i, REL, t, tg, ' TargetMode="External"' if ext else '')
        for i, t, tg, ext in items) + '</Relationships>'


def xfrm(x, y, w, h, tag='a:xfrm'):
    return '<%s><a:off x="%d" y="%d"/><a:ext cx="%d" cy="%d"/></%s>' % (tag, x * EMU, y * EMU, w * EMU, h * EMU, tag)


def para(text, lvl=0, extra='', rpr=''):
    return '<a:p>%s<a:r><a:rPr lang="es-AR"%s</a:rPr><a:t>%s</a:t></a:r></a:p>' % (
        '<a:pPr lvl="%d"%s/>' % (lvl, extra) if lvl or extra else '', rpr or '>', esc(text))


def sp(nid, name, ph=None, pos=None, body='', geom=None, fill=None, style=False, ln=None):
    nv = '<p:nvSpPr><p:cNvPr id="%d" name="%s"/><p:cNvSpPr/><p:nvPr>%s</p:nvPr></p:nvSpPr>' % (nid, name, ph or '')
    sppr = '<p:spPr>' + (xfrm(*pos) if pos else '') + ('<a:prstGeom prst="%s"><a:avLst/></a:prstGeom>' % geom if geom else '') + (fill or '') + (ln or '') + '</p:spPr>'
    st = ('<p:style><a:lnRef idx="2"><a:schemeClr val="accent1"><a:shade val="50000"/></a:schemeClr></a:lnRef>'
          '<a:fillRef idx="1"><a:schemeClr val="accent1"/></a:fillRef><a:effectRef idx="0"><a:schemeClr val="accent1"/></a:effectRef>'
          '<a:fontRef idx="minor"><a:schemeClr val="lt1"/></a:fontRef></p:style>') if style else ''
    tx = '<p:txBody><a:bodyPr%s/><a:lstStyle/>%s</p:txBody>' % (' anchor="ctr"' if style else '', body) if body else ''
    return '<p:sp>' + nv + sppr + st + tx + '</p:sp>'


def pptx(path, t):
    W, H = 960, 540
    land = sd.landscape(320, 200)
    theme = (HEAD + '<a:theme %s name="Huerta"><a:themeElements><a:clrScheme name="Huerta">'
             '<a:dk1><a:sysClr val="windowText" lastClr="1B2A1E"/></a:dk1><a:lt1><a:sysClr val="window" lastClr="FFFFFF"/></a:lt1>'
             '<a:dk2><a:srgbClr val="2E4A32"/></a:dk2><a:lt2><a:srgbClr val="EEF3E8"/></a:lt2>'
             '<a:accent1><a:srgbClr val="3C8D4F"/></a:accent1><a:accent2><a:srgbClr val="E59A2E"/></a:accent2>'
             '<a:accent3><a:srgbClr val="7A5C3E"/></a:accent3><a:accent4><a:srgbClr val="5B9BD5"/></a:accent4>'
             '<a:accent5><a:srgbClr val="C0504D"/></a:accent5><a:accent6><a:srgbClr val="8064A2"/></a:accent6>'
             '<a:hlink><a:srgbClr val="1F6FB2"/></a:hlink><a:folHlink><a:srgbClr val="954F72"/></a:folHlink></a:clrScheme>'
             '<a:fontScheme name="H"><a:majorFont><a:latin typeface="Calibri Light"/><a:ea typeface=""/><a:cs typeface=""/></a:majorFont>'
             '<a:minorFont><a:latin typeface="Calibri"/><a:ea typeface=""/><a:cs typeface=""/></a:minorFont></a:fontScheme>'
             '<a:fmtScheme name="H"><a:fillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>'
             '<a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w="9525"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln>'
             '<a:ln w="12700"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln><a:ln w="19050"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln></a:lnStyleLst>'
             '<a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst>'
             '<a:bgFillStyleLst><a:solidFill><a:schemeClr val="phClr"/></a:solidFill><a:solidFill><a:schemeClr val="phClr"/></a:solidFill>'
             '<a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>') % NS.split(' ')[0]

    def lvl(n, sz, mar, ind, bu=True):
        return ('<a:lvl%dpPr marL="%d" indent="%d">%s<a:defRPr sz="%d"><a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl%dpPr>'
                % (n, mar * EMU, ind * EMU, '<a:buFont typeface="Arial"/><a:buChar char="•"/>' if bu else '<a:buNone/>', sz * 100, n))
    master = (HEAD + '<p:sldMaster %s><p:cSld><p:bg><p:bgRef idx="1001"><a:schemeClr val="bg1"/></p:bgRef></p:bg><p:spTree>'
              '<p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>' % NS
              + sp(2, 'Title', '<p:ph type="title"/>', (48, 30, 864, 80))
              + sp(3, 'Body', '<p:ph type="body" idx="1"/>', (48, 130, 864, 340))
              + sp(4, 'Banda', None, (0, 516, 960, 24), geom='rect', fill='<a:solidFill><a:schemeClr val="accent1"/></a:solidFill>', ln='<a:ln><a:noFill/></a:ln>')
              + '</p:spTree></p:cSld><p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2" accent3="accent3" '
              'accent4="accent4" accent5="accent5" accent6="accent6" hlink="hlink" folHlink="folHlink"/>'
              '<p:sldLayoutIdLst><p:sldLayoutId id="2147483649" r:id="rId1"/><p:sldLayoutId id="2147483650" r:id="rId2"/></p:sldLayoutIdLst>'
              '<p:txStyles><p:titleStyle><a:lvl1pPr algn="l"><a:buNone/><a:defRPr sz="4000" b="1"><a:solidFill><a:schemeClr val="tx2"/></a:solidFill></a:defRPr></a:lvl1pPr></p:titleStyle>'
              '<p:bodyStyle>' + lvl(1, 26, 22, -22) + lvl(2, 22, 50, -20) + lvl(3, 18, 78, -18) + '</p:bodyStyle>'
              '<p:otherStyle><a:lvl1pPr><a:defRPr sz="18"><a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl1pPr></p:otherStyle>'
              '</p:txStyles></p:sldMaster>')
    lay_title = (HEAD + '<p:sldLayout %s type="title" showMasterSp="0"><p:cSld name="Title"><p:bg><p:bgPr><a:solidFill><a:schemeClr val="accent1"><a:lumMod val="40000"/><a:lumOff val="60000"/></a:schemeClr></a:solidFill></p:bgPr></p:bg><p:spTree>'
                 '<p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>' % NS
                 + sp(2, 'Title', '<p:ph type="ctrTitle"/>', (80, 170, 800, 110)).replace('<p:txBody>', '<p:txBody>', 1)
                 .replace('</p:spPr>', '</p:spPr><p:txBody><a:bodyPr anchor="b"/><a:lstStyle><a:lvl1pPr algn="ctr"><a:defRPr sz="5400"/></a:lvl1pPr></a:lstStyle><a:p/></p:txBody>', 1)
                 + sp(3, 'Subtitle', '<p:ph type="subTitle" idx="1"/>', (160, 290, 640, 60))
                 .replace('</p:spPr>', '</p:spPr><p:txBody><a:bodyPr/><a:lstStyle><a:lvl1pPr marL="0" indent="0" algn="ctr"><a:buNone/><a:defRPr sz="2400"><a:solidFill><a:schemeClr val="tx2"/></a:solidFill></a:defRPr></a:lvl1pPr></a:lstStyle><a:p/></p:txBody>', 1)
                 + '</p:spTree></p:cSld></p:sldLayout>')
    lay_content = (HEAD + '<p:sldLayout %s type="obj"><p:cSld name="Content"><p:spTree>'
                   '<p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>' % NS
                   + sp(2, 'Title', '<p:ph type="title"/>') + sp(3, 'Content', '<p:ph idx="1"/>') + '</p:spTree></p:cSld></p:sldLayout>')

    def slide(shapes, extra='', show=True, bg=''):
        return (HEAD + '<p:sld %s%s><p:cSld>%s<p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>'
                % (NS, '' if show else ' show="0"', bg) + shapes + '</p:spTree></p:cSld>' + extra + '</p:sld>')

    def title(text, nid=2, ph='<p:ph type="title"/>'):
        return sp(nid, 'Title', ph, body=para(text))
    s1 = slide(title(t['title'], ph='<p:ph type="ctrTitle"/>') + sp(3, 'Sub', '<p:ph type="subTitle" idx="1"/>', body=para(t['subtitle'])))
    bl = ''.join(para(x, lvl) for lvl, x in t['bullets'])
    s2 = slide(title(t['h2']) + sp(3, 'Content', '<p:ph idx="1"/>', body=bl))
    pic = ('<p:pic><p:nvPicPr><p:cNvPr id="4" name="Foto"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed="rId2"/>'
           '<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>' + xfrm(60, 130, 480, 300) + '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr></p:pic>')
    cap = sp(5, 'Caption', None, (570, 150, 340, 200), body=para(t['caption'], rpr=' sz="2200" i="1"><a:solidFill><a:schemeClr val="accent3"/></a:solidFill>')
             + '<a:p><a:r><a:rPr lang="es-AR" sz="2000"><a:hlinkClick r:id="rId3"/></a:rPr><a:t>%s</a:t></a:r></a:p>' % esc(t['link']))
    s3 = slide(title(t['h3']) + pic + cap)
    boxes = ''
    for i, b in enumerate(t['boxes']):
        geom = ['rect', 'ellipse', 'roundRect', 'diamond'][i]
        boxes += sp(10 + i, 'Caja %d' % i, None, (60 + i * 210, 150, 180, 110), body=para(b, rpr=' sz="2000" b="1">'), geom=geom, style=True)
    arrow = sp(20, 'Flecha', None, (300, 330, 360, 80), body=para(t['arrow'], rpr=' sz="2000">'), geom='rightArrow',
               fill='<a:solidFill><a:schemeClr val="accent2"/></a:solidFill>', style=True)
    cxn = ('<p:cxnSp><p:nvCxnSpPr><p:cNvPr id="21" name="Conector"/><p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr>' + xfrm(60, 300, 840, 0)
           + '<a:prstGeom prst="line"><a:avLst/></a:prstGeom><a:ln w="19050"><a:solidFill><a:schemeClr val="accent3"/></a:solidFill></a:ln></p:spPr></p:cxnSp>')
    grp = ('<p:grpSp><p:nvGrpSpPr><p:cNvPr id="30" name="Grupo"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="%d" y="%d"/>'
           '<a:ext cx="%d" cy="%d"/><a:chOff x="0" y="0"/><a:chExt cx="%d" cy="%d"/></a:xfrm></p:grpSpPr>' % (700 * EMU, 420 * EMU, 200 * EMU, 80 * EMU, 400 * EMU, 160 * EMU)
           + sp(31, 'g1', None, (0, 0, 180, 160), geom='ellipse', fill='<a:solidFill><a:srgbClr val="5B9BD5"/></a:solidFill>')
           + sp(32, 'g2', None, (220, 0, 180, 160), geom='ellipse', fill='<a:solidFill><a:srgbClr val="C0504D"/></a:solidFill>') + '</p:grpSp>')
    s4 = slide(title(t['h4']) + boxes + cxn + arrow + grp, bg='<p:bg><p:bgPr><a:solidFill><a:schemeClr val="bg2"/></a:solidFill></p:bgPr></p:bg>')
    rows = ''
    for r in t['table']:
        rows += '<a:tr h="%d">' % (40 * EMU) + ''.join('<a:tc><a:txBody><a:bodyPr/><a:lstStyle/>%s</a:txBody><a:tcPr/></a:tc>' % para(c, rpr=' sz="1800">') for c in r) + '</a:tr>'
    tbl = ('<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="4" name="Tabla"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>'
           + xfrm(80, 140, 800, 160, 'p:xfrm') + '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table">'
           '<a:tbl><a:tblPr firstRow="1" bandRow="1"/><a:tblGrid>' + ''.join('<a:gridCol w="%d"/>' % (w * EMU) for w in (240, 280, 280))
           + '</a:tblGrid>' + rows + '</a:tbl></a:graphicData></a:graphic></p:graphicFrame>')
    s5 = slide(title(t['h5']) + tbl)
    chart = ('<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="4" name="Chart"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>'
             + xfrm(120, 140, 720, 320, 'p:xfrm') + '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">'
             '<c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" r:id="rId9"/></a:graphicData></a:graphic></p:graphicFrame>')
    s6 = slide(title(t['h6']) + chart, show=False)
    s7 = slide(title(t['h7'], ph='<p:ph type="ctrTitle"/>') + sp(3, 'Sub', '<p:ph type="subTitle" idx="1"/>', body=para(t['end'])))
    notes = (HEAD + '<p:notes %s><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>' % NS
             + sp(2, 'Notes', '<p:ph type="body" idx="1"/>', body=para(t['notes'])) + '</p:spTree></p:cSld></p:notes>')
    slides = [(s1, 1), (s2, 2), (s3, 2), (s4, 2), (s5, 2), (s6, 2), (s7, 1)]
    parts = [('[Content_Types].xml', HEAD + '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
              '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>'
              '<Default Extension="png" ContentType="image/png"/>'
              '<Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>'
              '<Override PartName="/ppt/slideMasters/slideMaster1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml"/>'
              '<Override PartName="/ppt/slideLayouts/slideLayout1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"/>'
              '<Override PartName="/ppt/slideLayouts/slideLayout2.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"/>'
              '<Override PartName="/ppt/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>'
              '<Override PartName="/ppt/notesSlides/notesSlide1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml"/>'
              + ''.join('<Override PartName="/ppt/slides/slide%d.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>' % (i + 1)
                        for i in range(len(slides))) + '</Types>'),
             ('_rels/.rels', rels([('rId1', 'officeDocument', 'ppt/presentation.xml', False)])),
             ('ppt/presentation.xml', HEAD + '<p:presentation %s><p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rId1"/></p:sldMasterIdLst>'
              '<p:sldIdLst>%s</p:sldIdLst><p:sldSz cx="%d" cy="%d"/><p:notesSz cx="6858000" cy="9144000"/>'
              '<p:defaultTextStyle><a:lvl1pPr><a:defRPr sz="1800"/></a:lvl1pPr></p:defaultTextStyle></p:presentation>'
              % (NS, ''.join('<p:sldId id="%d" r:id="rId%d"/>' % (256 + i, i + 2) for i in range(len(slides))), W * EMU, H * EMU)),
             ('ppt/_rels/presentation.xml.rels', rels([('rId1', 'slideMaster', 'slideMasters/slideMaster1.xml', False)] +
                                                      [('rId%d' % (i + 2), 'slide', 'slides/slide%d.xml' % (i + 1), False) for i in range(len(slides))])),
             ('ppt/slideMasters/slideMaster1.xml', master),
             ('ppt/slideMasters/_rels/slideMaster1.xml.rels', rels([('rId1', 'slideLayout', '../slideLayouts/slideLayout1.xml', False),
                                                                   ('rId2', 'slideLayout', '../slideLayouts/slideLayout2.xml', False),
                                                                   ('rId3', 'theme', '../theme/theme1.xml', False)])),
             ('ppt/slideLayouts/slideLayout1.xml', lay_title), ('ppt/slideLayouts/slideLayout2.xml', lay_content),
             ('ppt/slideLayouts/_rels/slideLayout1.xml.rels', rels([('rId1', 'slideMaster', '../slideMasters/slideMaster1.xml', False)])),
             ('ppt/slideLayouts/_rels/slideLayout2.xml.rels', rels([('rId1', 'slideMaster', '../slideMasters/slideMaster1.xml', False)])),
             ('ppt/theme/theme1.xml', theme), ('ppt/media/image1.png', land),
             ('ppt/notesSlides/notesSlide1.xml', notes),
             ('ppt/notesSlides/_rels/notesSlide1.xml.rels', rels([('rId1', 'slide', '../slides/slide2.xml', False)]))]
    for i, (x, lay) in enumerate(slides):
        parts.append(('ppt/slides/slide%d.xml' % (i + 1), x))
        rl = [('rId1', 'slideLayout', '../slideLayouts/slideLayout%d.xml' % lay, False)]
        if i == 1:
            rl.append(('rId5', 'notesSlide', '../notesSlides/notesSlide1.xml', False))
        if i == 2:
            rl += [('rId2', 'image', '../media/image1.png', False), ('rId3', 'hyperlink', 'https://ejemplo.com/mapa', True)]
        parts.append(('ppt/slides/_rels/slide%d.xml.rels' % (i + 1), rels(rl)))
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for n, d in parts:
            z.writestr(n, d.encode('utf-8') if isinstance(d, str) else d)


def odp(path, t):
    land = sd.landscape(320, 200)
    O = ('xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
         'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
         'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
         'xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
         'xmlns:presentation="urn:oasis:names:tc:opendocument:xmlns:presentation:1.0" office:version="1.3"')

    def frame(cls, x, y, w, h, body, style=''):
        return ('<draw:frame presentation:class="%s"%s svg:x="%gcm" svg:y="%gcm" svg:width="%gcm" svg:height="%gcm"><draw:text-box>%s</draw:text-box></draw:frame>'
                % (cls, style, x, y, w, h, body))

    def p(text, style=''):
        return '<text:p%s>%s</text:p>' % (' text:style-name="%s"' % style if style else '', esc(text))

    def lst(items):
        out, cur = '', -1
        for lvl, x in items:
            out += '<text:list><text:list-item>' * 0
        # nested lists as ODF writes them: each item at its depth
        out = ''
        for lvl, x in items:
            out += '<text:list>' + '<text:list-item><text:list>' * lvl + '<text:list-item>' + p(x) + '</text:list-item>' + '</text:list></text:list-item>' * lvl + '</text:list>'
        return out
    pages = []
    pages.append(frame('title', 2, 5.5, 29.9, 3.5, p(t['title'])) + frame('subtitle', 5, 9.5, 23.9, 2, p(t['subtitle'])))
    pages.append(frame('title', 1.7, 1, 30.5, 2.8, p(t['h2'])) + frame('outline', 1.7, 4.4, 30.5, 12, lst(t['bullets']))
                 + '<presentation:notes><draw:frame presentation:class="notes" svg:x="2cm" svg:y="14cm" svg:width="17cm" svg:height="10cm"><draw:text-box>'
                 + p(t['notes']) + '</draw:text-box></draw:frame></presentation:notes>')
    pages.append(frame('title', 1.7, 1, 30.5, 2.8, p(t['h3']))
                 + '<draw:frame svg:x="2cm" svg:y="4.6cm" svg:width="16.9cm" svg:height="10.6cm"><draw:image xlink:href="Pictures/paisaje.png"/></draw:frame>'
                 + '<draw:frame draw:style-name="gr2" svg:x="20cm" svg:y="5.3cm" svg:width="12cm" svg:height="7cm"><draw:text-box>'
                 + '<text:p><text:span text:style-name="T1">%s</text:span></text:p><text:p><text:a xlink:href="https://ejemplo.com/mapa">%s</text:a></text:p>'
                 % (esc(t['caption']), esc(t['link'])) + '</draw:text-box></draw:frame>')
    shapes = ''
    for i, b in enumerate(t['boxes']):
        kind = ['draw:rect', 'draw:ellipse', 'draw:custom-shape', 'draw:custom-shape'][i]
        geo = ['', '', '<draw:enhanced-geometry draw:type="round-rectangle"/>', '<draw:enhanced-geometry draw:type="diamond"/>'][i]
        shapes += '<%s draw:style-name="gr1" svg:x="%gcm" svg:y="5.3cm" svg:width="6.3cm" svg:height="3.9cm"><text:p text:style-name="P1">%s</text:p>%s</%s>' % (
            kind, 2.1 + i * 7.4, esc(b), geo, kind)
    shapes += '<draw:line draw:style-name="gr3" svg:x1="2.1cm" svg:y1="10.6cm" svg:x2="31.7cm" svg:y2="10.6cm"/>'
    shapes += ('<draw:custom-shape draw:style-name="gr4" svg:x="10.6cm" svg:y="11.6cm" svg:width="12.7cm" svg:height="2.8cm"><text:p text:style-name="P1">%s</text:p>'
               '<draw:enhanced-geometry draw:type="right-arrow"/></draw:custom-shape>' % esc(t['arrow']))
    pages.append(frame('title', 1.7, 1, 30.5, 2.8, p(t['h4'])) + shapes)
    rows = ''.join('<table:table-row>' + ''.join('<table:table-cell%s><text:p>%s</text:p></table:table-cell>' % (' table:style-name="ce1"' if ri == 0 else '', esc(c))
                                                 for c in r) + '</table:table-row>' for ri, r in enumerate(t['table']))
    pages.append(frame('title', 1.7, 1, 30.5, 2.8, p(t['h5']))
                 + '<draw:frame svg:x="2.8cm" svg:y="4.9cm" svg:width="28.2cm" svg:height="5.6cm"><table:table><table:table-column table:number-columns-repeated="3"/>'
                 + rows + '</table:table></draw:frame>')
    pages.append(frame('title', 1.7, 1, 30.5, 2.8, p(t['h6']))
                 + '<draw:frame svg:x="4.2cm" svg:y="4.9cm" svg:width="25.4cm" svg:height="11.3cm"><draw:object xlink:href="./Object 1"/></draw:frame>')
    pages.append(frame('title', 2, 5.5, 29.9, 3.5, p(t['h7'])) + frame('subtitle', 5, 9.5, 23.9, 2, p(t['end'])))
    body = ''.join('<draw:page draw:name="page%d" draw:master-page-name="Huerta"%s%s>%s</draw:page>'
                   % (i + 1, ' draw:style-name="dp2"' if i == 3 else '', ' presentation:visibility="hidden"' if i == 5 else '', pg) for i, pg in enumerate(pages))
    content = (HEAD + '<office:document-content %s><office:automatic-styles>'
               '<style:style style:name="dp2" style:family="drawing-page"><style:drawing-page-properties draw:fill="solid" draw:fill-color="#eef3e8"/></style:style>'
               '<style:style style:name="gr1" style:family="graphic"><style:graphic-properties draw:fill="solid" draw:fill-color="#3c8d4f" draw:stroke="solid" svg:stroke-color="#2a6337" draw:textarea-vertical-align="middle"/></style:style>'
               '<style:style style:name="gr2" style:family="graphic"><style:graphic-properties draw:fill="none" draw:stroke="none"/></style:style>'
               '<style:style style:name="gr3" style:family="graphic"><style:graphic-properties draw:stroke="solid" svg:stroke-color="#7a5c3e" svg:stroke-width="0.07cm"/></style:style>'
               '<style:style style:name="gr4" style:family="graphic"><style:graphic-properties draw:fill="solid" draw:fill-color="#e59a2e" draw:stroke="none" draw:textarea-vertical-align="middle"/></style:style>'
               '<style:style style:name="P1" style:family="paragraph"><style:paragraph-properties fo:text-align="center"/><style:text-properties fo:font-size="20pt" fo:font-weight="bold" fo:color="#ffffff"/></style:style>'
               '<style:style style:name="T1" style:family="text"><style:text-properties fo:font-size="22pt" fo:font-style="italic" fo:color="#7a5c3e"/></style:style>'
               '<style:style style:name="ce1" style:family="table-cell"><style:table-cell-properties fo:background-color="#3c8d4f"/></style:style>'
               '</office:automatic-styles><office:body><office:presentation>%s</office:presentation></office:body></office:document-content>') % (O, body)
    styles = (HEAD + '<office:document-styles %s><office:styles>'
              '<style:style style:name="Huerta-title" style:family="presentation"><style:paragraph-properties fo:text-align="center"/><style:text-properties fo:font-size="40pt" fo:font-weight="bold" fo:color="#2e4a32"/></style:style>'
              '<style:style style:name="Huerta-subtitle" style:family="presentation"><style:paragraph-properties fo:text-align="center"/><style:text-properties fo:font-size="24pt" fo:color="#2e4a32"/></style:style>'
              '<style:style style:name="Huerta-outline1" style:family="presentation"><style:text-properties fo:font-size="26pt" fo:color="#1b2a1e"/></style:style>'
              '<style:style style:name="Huerta-outline2" style:family="presentation" style:parent-style-name="Huerta-outline1"><style:text-properties fo:font-size="22pt"/></style:style>'
              '<style:style style:name="Huerta-outline3" style:family="presentation" style:parent-style-name="Huerta-outline2"><style:text-properties fo:font-size="18pt"/></style:style>'
              '</office:styles><office:automatic-styles>'
              '<style:page-layout style:name="PM1"><style:page-layout-properties fo:page-width="33.867cm" fo:page-height="19.05cm"/></style:page-layout>'
              '<style:style style:name="Mdp1" style:family="drawing-page"><style:drawing-page-properties draw:fill="solid" draw:fill-color="#ffffff"/></style:style>'
              '<style:style style:name="Mgr1" style:family="graphic"><style:graphic-properties draw:fill="solid" draw:fill-color="#3c8d4f" draw:stroke="none"/></style:style>'
              '</office:automatic-styles><office:master-styles>'
              '<style:master-page style:name="Huerta" style:page-layout-name="PM1" draw:style-name="Mdp1">'
              '<draw:frame presentation:class="title" presentation:placeholder="true" svg:x="1.7cm" svg:y="1cm" svg:width="30.5cm" svg:height="2.8cm"/>'
              '<draw:rect draw:style-name="Mgr1" svg:x="0cm" svg:y="18.2cm" svg:width="33.867cm" svg:height="0.85cm"/>'
              '</style:master-page></office:master-styles></office:document-styles>') % O
    manifest = ('<?xml version="1.0" encoding="UTF-8"?>\n<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.3">'
                '<manifest:file-entry manifest:full-path="/" manifest:media-type="application/vnd.oasis.opendocument.presentation"/>'
                '<manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/>'
                '<manifest:file-entry manifest:full-path="Pictures/paisaje.png" manifest:media-type="image/png"/></manifest:manifest>')
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(zipfile.ZipInfo('mimetype'), 'application/vnd.oasis.opendocument.presentation', zipfile.ZIP_STORED)
        z.writestr('META-INF/manifest.xml', manifest)
        z.writestr('content.xml', content)
        z.writestr('styles.xml', styles)
        z.writestr('Pictures/paisaje.png', land)


def make(d, en=False):
    t = TXT['en' if en else 'es']
    os.makedirs(d, exist_ok=True)
    pptx(os.path.join(d, t['file'] + '.pptx'), t)
    odp(os.path.join(d, t['file'] + '.odp'), t)


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    make(args[0] if args else 'sim/sim_fs/documentos', '--en' in sys.argv)
    print('slide samples in', args[0] if args else 'sim/sim_fs/documentos')
