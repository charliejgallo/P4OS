#!/usr/bin/env python3
"""Invented files for Office's readers (phase 3): HTML, EPUB, mail, FB2, RTF,
Markdown, contacts, a calendar, data files, a picture and an iWork preview.

    python3 apps/oficina/tools/samples_read.py DIR [--en]

Everything here is made up: names, addresses, numbers. make_samples.py calls
it too.
"""
import base64
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(__file__))
import samples_docs as sd  # noqa: E402


def zipped(path, parts, stored=()):
    with zipfile.ZipFile(path, 'w') as z:
        for name, data in parts:
            if isinstance(data, str):
                data = data.encode('utf-8')
            z.writestr(name, data, zipfile.ZIP_STORED if name in stored else zipfile.ZIP_DEFLATED)


def jpeg_of(png_bytes):
    """a JPEG through macOS sips; None elsewhere"""
    if not shutil.which('sips'):
        return None
    with tempfile.TemporaryDirectory() as t:
        a, b = os.path.join(t, 'a.png'), os.path.join(t, 'b.jpg')
        open(a, 'wb').write(png_bytes)
        subprocess.run(['sips', '-s', 'format', 'jpeg', a, '--out', b], capture_output=True)
        return open(b, 'rb').read() if os.path.exists(b) else None


TXT = {
    'es': dict(
        html_title='Club de ciclismo Los Alerces',
        html_intro='Salimos todos los <b>sábados a las 8</b> desde la plaza. Traé casco, agua y <i>ganas</i>.',
        html_list=['Ruta corta: 25 km, para empezar', 'Ruta media: 50 km, con una subida', 'Ruta larga: 90 km, sólo con buen tiempo'],
        html_table=[('Mes', 'Salidas', 'Kilómetros'), ('Septiembre', '4', '210'), ('Octubre', '5', '265'), ('Noviembre', '3', '180')],
        html_quote='«La bicicleta es la forma más civilizada de moverse.»',
        html_link='Escribinos a la secretaría',
        epub_title='Cuentos del faro', epub_author='Marina Ezcurra',
        epub_ch=[('El guardián', ['El faro de punta Rasa tenía un guardián que hablaba con las gaviotas.',
                                  'Cada noche subía los ciento doce escalones con una lámpara de aceite, aunque el faro ya era eléctrico.']),
                 ('La tormenta', ['Aquella noche el viento arrancó las tejas del galpón.',
                                  'El guardián bajó a la playa y encontró un bote vacío, pintado de azul.'])],
        mail_subject='Reunión del jueves y planilla de gastos',
        mail_from='Laura Pérez <laura@ejemplo.com>', mail_to='Equipo <equipo@ejemplo.com>',
        mail_body=['Hola a todos:', '', 'Les recuerdo que la reunión del jueves es a las 10 en la sala chica.',
                   'Adjunto la planilla de gastos del mes para que la revisen antes.', '',
                   '> ¿Llevamos el proyector?', 'Sí, lo llevo yo.', '', 'Saludos,', 'Laura'],
        fb2_title='El jardín de invierno',
        fb2_p=['La casa tenía un jardín de invierno lleno de helechos.', 'Nadie recordaba quién lo había plantado.'],
        rtf_title='Receta de pan casero',
        rtf_items=['500 g de harina', '10 g de sal', '300 ml de agua tibia', '7 g de levadura seca'],
        rtf_text='Mezclar todo, amasar diez minutos y dejar levar una hora. Hornear a 220 \\u176?C durante 35 minutos.',
        md_name='guia.md',
        vcf=[('Ana', 'Ríos', 'Estudio Ríos', 'Arquitecta', '+54 11 5555-0101', 'ana@ejemplo.com', 'Calle Falsa 123;Ciudad Ejemplo'),
             ('Bruno', 'Díaz', 'Taller Norte', 'Mecánico', '+54 11 5555-0102', 'bruno@ejemplo.com', 'Av. Siempreviva 742;Pueblo Ejemplo'),
             ('Carla', 'Méndez', '', '', '+54 11 5555-0103', 'carla@ejemplo.com', '')],
        ics_name='Agenda del club',
        ics=[('20261205T090000', '20261205T120000', 'Salida a la costa', 'Plaza central', 'Traer repuesto de cámara'),
             ('20261128T190000', '20261128T203000', 'Asamblea anual', 'Salón del club', 'Orden del día en la cartelera'),
             ('20261212', '', 'Cena de fin de año', 'Parrilla La Esquina', '')],
        files=dict(html='club.html', epub='cuentos.epub', eml='reunion.eml', fb2='jardin.fb2', rtf='receta.rtf',
                   vcf='contactos.vcf', ics='agenda.ics', xml='config.xml', yaml='ajustes.yaml', toml='proyecto.toml',
                   png='paisaje.png', pages='folleto.pages', json='pedido.json'),
    ),
    'en': dict(
        html_title='Riverside cycling club',
        html_intro='We ride every <b>Saturday at 8</b> from the square. Bring a helmet, water and <i>energy</i>.',
        html_list=['Short route: 25 km, to start with', 'Middle route: 50 km, one climb', 'Long route: 90 km, good weather only'],
        html_table=[('Month', 'Rides', 'Kilometres'), ('September', '4', '210'), ('October', '5', '265'), ('November', '3', '180')],
        html_quote='“The bicycle is the most civilised way to move.”',
        html_link='Write to the secretary',
        epub_title='Tales of the lighthouse', epub_author='Marina Ezcurra',
        epub_ch=[('The keeper', ['The lighthouse at Rasa Point had a keeper who talked to the gulls.',
                                 'Every night he climbed the hundred and twelve steps with an oil lamp, though the light was electric.']),
                 ('The storm', ['That night the wind tore the tiles off the shed.',
                                'The keeper went down to the beach and found an empty boat, painted blue.'])],
        mail_subject='Thursday meeting and the expenses sheet',
        mail_from='Laura Perez <laura@example.com>', mail_to='Team <team@example.com>',
        mail_body=['Hi all,', '', 'A reminder that Thursday\'s meeting is at 10 in the small room.',
                   'The month\'s expenses sheet is attached, please look at it before.', '',
                   '> Shall we bring the projector?', 'Yes, I will.', '', 'Best,', 'Laura'],
        fb2_title='The winter garden',
        fb2_p=['The house had a winter garden full of ferns.', 'Nobody remembered who had planted it.'],
        rtf_title='Home-made bread',
        rtf_items=['500 g flour', '10 g salt', '300 ml warm water', '7 g dry yeast'],
        rtf_text='Mix everything, knead for ten minutes and let it rise for an hour. Bake at 220 \\u176?C for 35 minutes.',
        md_name='guide.md',
        vcf=[('Ann', 'Rivers', 'Rivers Studio', 'Architect', '+1 555 0101', 'ann@example.com', '123 Fake Street;Example City'),
             ('Bruce', 'Day', 'North Garage', 'Mechanic', '+1 555 0102', 'bruce@example.com', '742 Evergreen Av.;Example Town'),
             ('Carol', 'Mendes', '', '', '+1 555 0103', 'carol@example.com', '')],
        ics_name='Club calendar',
        ics=[('20261205T090000', '20261205T120000', 'Ride to the coast', 'Main square', 'Bring a spare tube'),
             ('20261128T190000', '20261128T203000', 'Annual meeting', 'Club hall', 'Agenda on the board'),
             ('20261212', '', 'End of year dinner', 'The Corner Grill', '')],
        files=dict(html='club.html', epub='tales.epub', eml='meeting.eml', fb2='garden.fb2', rtf='bread.rtf',
                   vcf='contacts.vcf', ics='calendar.ics', xml='config.xml', yaml='settings.yaml', toml='project.toml',
                   png='landscape.png', pages='brochure.pages', json='order.json'),
    ),
}


def html_page(t, img='paisaje.png'):
    rows = ''.join('<tr>' + ''.join(('<th>%s</th>' if i == 0 else '<td>%s</td>') % c for c in r) + '</tr>'
                   for i, r in enumerate(t['html_table']))
    return f'''<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>{t['html_title']}</title>
<style>body {{ font-family: sans-serif }} .x {{ color: red }}</style>
<script>console.log("not shown");</script></head>
<body>
<h1>{t['html_title']}</h1>
<p>{t['html_intro']}</p>
<p style="color:#2060a0">&iquest;Primera vez? &rarr; empez&aacute; por la ruta corta &mdash; 25&nbsp;km.</p>
<h2>Rutas</h2>
<ul>{''.join('<li>%s</li>' % x for x in t['html_list'])}</ul>
<ol><li>Uno</li><li>Dos<ul><li>Dos y medio</li></ul></li><li>Tres</li></ol>
<table border="1">{rows}</table>
<blockquote>{t['html_quote']}</blockquote>
<p><img src="{img}" width="240" height="160" alt=""></p>
<p><a href="mailto:secretaria@ejemplo.com">{t['html_link']}</a> &middot; <code>v1.2</code> &middot; H<sub>2</sub>O &middot; m<sup>2</sup></p>
<pre>  ruta   km
  corta  25
  larga  90</pre>
<hr>
<p align="center"><small>&copy; 2026</small></p>
</body></html>
'''


def epub(path, t):
    chap = []
    for i, (title, ps) in enumerate(t['epub_ch']):
        body = ''.join('<p>%s</p>' % sd.esc(p) for p in ps)
        if i == 0:
            body += '<p><img src="../images/faro.png" alt=""/></p>'
        chap.append(('OEBPS/text/cap%d.xhtml' % (i + 1),
                     '<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>%s</title>'
                     '<link rel="stylesheet" href="../style.css"/></head><body><h2>%s</h2>%s</body></html>' % (title, title, body)))
    manifest = ''.join('<item id="c%d" href="text/cap%d.xhtml" media-type="application/xhtml+xml"/>' % (i + 1, i + 1)
                       for i in range(len(chap)))
    spine = ''.join('<itemref idref="c%d"/>' % (i + 1) for i in range(len(chap)))
    opf = ('<?xml version="1.0" encoding="utf-8"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>%s</dc:title><dc:creator>%s</dc:creator>'
           '<dc:identifier id="id">urn:uuid:00000000-0000-0000-0000-000000000001</dc:identifier><dc:language>es</dc:language></metadata>'
           '<manifest>%s<item id="css" href="style.css" media-type="text/css"/>'
           '<item id="img" href="images/faro.png" media-type="image/png"/></manifest><spine>%s</spine></package>'
           % (sd.esc(t['epub_title']), sd.esc(t['epub_author']), manifest, spine))
    zipped(path, [('mimetype', 'application/epub+zip'),
                  ('META-INF/container.xml', '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                   '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'),
                  ('OEBPS/content.opf', opf), ('OEBPS/style.css', 'p { text-indent: 1em }'),
                  ('OEBPS/images/faro.png', sd.landscape(160, 100))] + chap, stored=('mimetype',))


def eml(path, t, attach_name):
    import email.header
    import quopri
    subj = email.header.Header(t['mail_subject'], 'utf-8').encode()
    text = '\r\n'.join(t['mail_body'])
    qp = quopri.encodestring(text.encode('utf-8')).decode('ascii')
    html = '<html><body>' + ''.join('<p>%s</p>' % sd.esc(x) if not x.startswith('>') else '<blockquote>%s</blockquote>' % sd.esc(x[1:])
                                    for x in t['mail_body'] if x) + '</body></html>'
    att = base64.encodebytes(b'Mes;Concepto;Importe\r\nNov;Papel;1200\r\n').decode('ascii')
    msg = (f'From: {t["mail_from"]}\r\nTo: {t["mail_to"]}\r\nSubject: {subj}\r\nDate: Mon, 23 Nov 2026 09:15:00 -0300\r\n'
           'MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary="XYZ1"\r\n\r\n'
           '--XYZ1\r\nContent-Type: multipart/alternative; boundary="ALT2"\r\n\r\n'
           f'--ALT2\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n{qp}\r\n'
           f'--ALT2\r\nContent-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n'
           f'{base64.encodebytes(html.encode("utf-8")).decode("ascii")}\r\n--ALT2--\r\n'
           f'--XYZ1\r\nContent-Type: text/csv; name="{attach_name}"\r\nContent-Disposition: attachment; filename="{attach_name}"\r\n'
           f'Content-Transfer-Encoding: base64\r\n\r\n{att}\r\n--XYZ1--\r\n')
    open(path, 'w', newline='').write(msg)


def fb2(path, t):
    img = base64.b64encode(sd.landscape(120, 80)).decode('ascii')
    ps = ''.join('<p>%s</p>' % sd.esc(p) for p in t['fb2_p'])
    x = ('<?xml version="1.0" encoding="utf-8"?>\n<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" '
         'xmlns:l="http://www.w3.org/1999/xlink"><description><title-info><book-title>%s</book-title></title-info></description>'
         '<body><section><title><p>I</p></title>%s<empty-line/><p><emphasis>Fin</emphasis> de la <strong>primera</strong> parte.</p>'
         '<image l:href="#tapa.png"/></section><section><title><p>II</p></title>%s</section></body>'
         '<binary id="tapa.png" content-type="image/png">%s</binary></FictionBook>' % (sd.esc(t['fb2_title']), ps, ps, img))
    open(path, 'w', encoding='utf-8').write(x)


def rtf(path, t):
    def r(s):
        out = ''
        for ch in s:
            o = ord(ch)
            out += ch if o < 128 else ("\\'%02x" % o if o < 256 else '\\u%d?' % (o if o < 32768 else o - 65536))
        return out
    png = sd.landscape(120, 80).hex()
    items = ''.join('{\\listtext\\f0 \\u8226?\\tab}%s\\par\n' % r(x) for x in t['rtf_items'])
    doc = ('{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}{\\f1\\fmodern Courier;}}\n'
           '{\\colortbl;\\red200\\green40\\blue40;\\red30\\green90\\blue160;}\n'
           '{\\stylesheet{\\s0 Normal;}{\\s1\\b\\fs36 heading 1;}}\n'
           '{\\info{\\title secret}{\\author nobody}}\n'
           '\\pard\\s1\\qc\\b\\fs36 %s\\b0\\fs24\\par\n'
           '\\pard\\qj Un texto con {\\b negrita}, {\\i cursiva}, {\\ul subrayado}, {\\strike tachado} y {\\cf1 color rojo}.\\par\n'
           '%s'
           '\\pard %s\\par\n'
           '{\\field{\\*\\fldinst HYPERLINK "https://ejemplo.com/pan"}{\\fldrslt {\\cf2\\ul Ver la receta en la web}}}\\par\n'
           '\\pard\\par\n'
           '\\trowd\\cellx3000\\cellx6000\\intbl {\\b Paso}\\cell {\\b Minutos}\\cell\\row\n'
           '\\trowd\\cellx3000\\cellx6000\\intbl Amasar\\cell 10\\cell\\row\n'
           '\\trowd\\cellx3000\\cellx6000\\intbl Hornear\\cell 35\\cell\\row\n'
           '\\pard\\par\n'
           '{\\pict\\pngblip\\picw120\\pich80\\picwgoal1800\\pichgoal1200\n%s}\\par\n'
           'Nota al pie{\\super\\chftn}{\\footnote\\pard\\plain\\chftn Con harina 000.}.\\line Segunda l\\\'ednea.\\par\n'
           '}\n') % (r(t['rtf_title']), items, r(t['rtf_text']).replace('\\\\u', '\\u'), png)
    open(path, 'w', encoding='ascii').write(doc)


MD_ES = '''Guía rápida del club
====================

Bienvenido. Esta guía está escrita en **Markdown** y se ve *con formato*.

## Antes de salir

1. Revisá los frenos
2. Inflá las ruedas a **60 psi**
3. Llevá una cámara de repuesto

- [x] Casco
- [ ] Luces
- [ ] Inflador
    - de mano
    - o de CO₂

> Si llueve, la salida se suspende.
> Avisamos por el grupo.

## Horarios

| Día | Hora | Ruta |
|-----|:----:|------|
| Sábado | 8:00 | Costa |
| Domingo | 9:30 | Sierras |

El comando para ver la agenda es `agenda --mes`, y la página es <https://ejemplo.com/club>.

```
salida: 08:00
regreso: 12:30
```

***

![Paisaje](paisaje.png)

Texto con ~~tachado~~ y un [enlace](https://ejemplo.com).
'''

MD_EN = MD_ES.replace('Guía rápida del club', 'Quick club guide').replace('====================', '================') \
    .replace('Bienvenido. Esta guía está escrita en', 'Welcome. This guide is written in').replace('y se ve *con formato*', 'and shows *formatted*') \
    .replace('Antes de salir', 'Before riding').replace('Revisá los frenos', 'Check the brakes').replace('Inflá las ruedas a', 'Pump the tyres to') \
    .replace('Llevá una cámara de repuesto', 'Bring a spare tube').replace('Casco', 'Helmet').replace('Luces', 'Lights').replace('Inflador', 'Pump') \
    .replace('de mano', 'a hand one').replace('o de CO₂', 'or CO₂').replace('Si llueve, la salida se suspende.', 'If it rains, the ride is off.') \
    .replace('Avisamos por el grupo.', 'We tell the group.').replace('Horarios', 'Times').replace('Día', 'Day').replace('Hora', 'Time') \
    .replace('Ruta', 'Route').replace('Sábado', 'Saturday').replace('Domingo', 'Sunday').replace('Costa', 'Coast').replace('Sierras', 'Hills') \
    .replace('El comando para ver la agenda es', 'The command for the calendar is').replace('y la página es', 'and the page is') \
    .replace('salida', 'start').replace('regreso', 'back').replace('Paisaje', 'Landscape').replace('paisaje.png', 'landscape.png') \
    .replace('Texto con ~~tachado~~ y un [enlace]', 'Text with ~~strikethrough~~ and a [link]')


def vcf(path, t):
    out = []
    for i, (g, f, org, title, tel, mail, adr) in enumerate(t['vcf']):
        out += ['BEGIN:VCARD', 'VERSION:3.0', 'N:%s;%s;;;' % (f, g), 'FN:%s %s' % (g, f)]
        if org:
            out.append('ORG:%s' % org)
        if title:
            out.append('TITLE:%s' % title)
        out += ['TEL;TYPE=CELL:%s' % tel, 'EMAIL;TYPE=WORK:%s' % mail]
        if adr:
            street, city = adr.split(';')
            out.append('ADR;TYPE=HOME:;;%s;%s;;;' % (street, city))
        if i == 0:
            out.append('BDAY:1990-04-12')
            out.append('NOTE:Una nota larga que sigue en la línea de abajo\\, porque\r\n  el archivo la dobló.')
        out.append('END:VCARD')
    open(path, 'w', newline='').write('\r\n'.join(out) + '\r\n')


def ics(path, t):
    out = ['BEGIN:VCALENDAR', 'VERSION:2.0', 'PRODID:-//Ejemplo//ES', 'X-WR-CALNAME:%s' % t['ics_name']]
    for i, (s, e, summ, loc, desc) in enumerate(t['ics']):
        out += ['BEGIN:VEVENT', 'UID:ev%d@ejemplo' % i, 'DTSTAMP:20261101T120000Z']
        out.append(('DTSTART;VALUE=DATE:%s' % s) if len(s) == 8 else 'DTSTART:%s' % s)
        if e:
            out.append('DTEND:%s' % e)
        out += ['SUMMARY:%s' % summ, 'LOCATION:%s' % loc]
        if desc:
            out.append('DESCRIPTION:%s' % desc)
        if i == 0:
            out.append('RRULE:FREQ=MONTHLY;COUNT=3')
            out += ['BEGIN:VALARM', 'TRIGGER:-PT30M', 'ACTION:DISPLAY', 'DESCRIPTION:alarm', 'END:VALARM']
        out.append('END:VEVENT')
    out += ['BEGIN:VTODO', 'UID:todo1@ejemplo', 'DUE:20261130T180000', 'SUMMARY:Pagar la cuota', 'STATUS:NEEDS-ACTION', 'END:VTODO']
    out.append('END:VCALENDAR')
    open(path, 'w', newline='').write('\r\n'.join(out) + '\r\n')


def data(d, t):
    f = t['files']
    open(os.path.join(d, f['xml']), 'w').write(
        '<?xml version="1.0" encoding="UTF-8"?>\n<!-- invented -->\n<config version="2">\n  <screen brightness="80" theme="dark"/>\n'
        '  <wifi>\n    <network ssid="EjemploWiFi" auto="true"/>\n  </wifi>\n  <owner>Club &amp; amigos</owner>\n</config>\n')
    open(os.path.join(d, f['yaml']), 'w').write(
        '# invented settings\nname: club\nversion: 1.4\nactive: true\nroutes:\n  - name: costa\n    km: 50\n  - name: sierras\n    km: 90\n'
        'contact:\n  mail: "club@ejemplo.com"\n  phone: null\n')
    open(os.path.join(d, f['toml']), 'w').write(
        '# invented project\ntitle = "Proyecto ejemplo"\n\n[owner]\nname = "Ana"\nsince = 2024\n\n[servers.alpha]\nip = "10.0.0.1"\n'
        'enabled = true\n\n[[products]]\nname = "Martillo"\nsku = 738594937\n')
    import json
    order = {'pedido': 1042, 'cliente': {'nombre': 'Ana Ríos', 'vip': True, 'notas': None},
             'items': [{'cod': 'A-1', 'cant': 2, 'precio': 1500.5}, {'cod': 'B-7', 'cant': 1, 'precio': 320}],
             'etiquetas': ['urgente', 'regalo'], 'vacio': {}, 'lista': []}
    open(os.path.join(d, f['json']), 'w').write(json.dumps(order, ensure_ascii=False, indent=2))


def archives(d, t, en):
    import gzip
    import io
    import tarfile
    f = t['files']
    land = sd.landscape()
    names = ('fotos', 'carta.txt', 'copia', 'notas', 'historieta') if not en else ('photos', 'letter.txt', 'backup', 'notes', 'comic')
    zipped(os.path.join(d, names[0] + '.zip'), [('viaje/' + ('playa.png' if not en else 'beach.png'), land),
                                                 ('viaje/' + ('lista.txt' if not en else 'list.txt'), 'agua\nprotector\ngorra\n'),
                                                 (f['html'], html_page(t, 'viaje/playa.png' if not en else 'viaje/beach.png')),
                                                 ('LEEME.md' if not en else 'README.md', '# Fotos\n\nInventadas para probar.\n')])
    body = '\n'.join(t['mail_body']).encode('utf-8')
    with gzip.open(os.path.join(d, names[1] + '.gz'), 'wb') as g:
        g.write(body)

    def tar(path, mode):
        with tarfile.open(path, mode, format=tarfile.GNU_FORMAT) as tf:
            for name, data in [(f['md'] if 'md' in f else 'guia.md', (MD_EN if en else MD_ES).encode('utf-8')),
                               ('carpeta/con/un/nombre/bastante/largo/que/pasa/de/los/cien/caracteres/del/encabezado/antiguo/de/tar/archivo.txt',
                                b'nombre largo\n'),
                               ('datos/' + f['json'], open(os.path.join(d, f['json']), 'rb').read())]:
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                ti.mtime = 1795000000
                tf.addfile(ti, io.BytesIO(data))
    tar(os.path.join(d, names[2] + '.tar.gz'), 'w:gz')
    tar(os.path.join(d, names[3] + '.tar'), 'w')
    zipped(os.path.join(d, names[4] + '.cbz'), [('%02d.png' % (i + 1), sd.png(c, 120, 160)) for i, c in
                                                 enumerate([(200, 60, 60), (60, 160, 80), (60, 90, 200)])], stored=('01.png', '02.png', '03.png'))


def make(d, en=False):
    t = TXT['en' if en else 'es']
    f = t['files']
    os.makedirs(d, exist_ok=True)
    land = sd.landscape()
    open(os.path.join(d, f['png']), 'wb').write(land)
    open(os.path.join(d, f['html']), 'w', encoding='utf-8').write(html_page(t, f['png']))
    epub(os.path.join(d, f['epub']), t)
    eml(os.path.join(d, f['eml']), t, 'gastos.csv' if not en else 'expenses.csv')
    fb2(os.path.join(d, f['fb2']), t)
    rtf(os.path.join(d, f['rtf']), t)
    open(os.path.join(d, t['md_name']), 'w', encoding='utf-8').write(MD_EN if en else MD_ES)
    vcf(os.path.join(d, f['vcf']), t)
    ics(os.path.join(d, f['ics']), t)
    data(d, t)
    archives(d, t, en)
    jpg = jpeg_of(land)
    if jpg:
        zipped(os.path.join(d, f['pages']), [('Index/Document.iwa', b'\x00' * 64), ('Metadata/Properties.plist', '<plist/>'),
                                              ('preview.jpg', jpg), ('preview-micro.jpg', jpg)])


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--en']
    make(args[0] if args else 'sim/sim_fs/documentos', '--en' in sys.argv)
    print('reader samples in', args[0] if args else 'sim/sim_fs/documentos')
