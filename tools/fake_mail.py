#!/usr/bin/env python3
"""
P4OS - A fake mail server for the Correo app (apps/correo): IMAP4rev1 with
IDLE, UIDPLUS, MOVE, CONDSTORE and SPECIAL-USE, and SMTP with AUTH, both in
the clear with STARTTLS and over TLS from the first byte, on local ports.

    tools/fake_mail.py [--host 127.0.0.1] [--base 10000] [--certs DIR]
                       [--drip SECONDS] [--quiet]

Ports, from --base (10000 by default):
    +143  IMAP, clear text, STARTTLS offered      (10143)
    +993  IMAP over TLS                           (10993)
    +587  SMTP submission, STARTTLS offered       (10587)
    +465  SMTP over TLS                           (10465)

Two accounts, both made up (RFC 2606 domains, nothing real):
    ana@example.com     password  prueba-ana
    bruno@example.net   password  prueba-bruno

Every mailbox lives in memory and starts again on each run. Ana's is the
interesting one: about thirty messages in UTF-8, ISO-8859-1 and
Windows-1252, encoded headers (RFC 2047, Q and B), quoted-printable and
base64 bodies, HTML with paragraphs, lists, links, quotes and a table,
inline pictures (multipart/related with cid:), attachments (PNG, JPEG,
PDF, a text file), conversations of several messages tied by Message-ID,
In-Reply-To and References, and folders with sub-folders.

A message sent through SMTP to an address of one of the accounts lands in
its INBOX, so a session in IDLE sees it arrive; every message sent is also
appended to the sender's Sent folder only if the client APPENDs it (as a
real server would not do it for it), and kept in --spool DIR if given.
--drip N delivers a new made-up message to Ana's INBOX every N seconds.

TLS needs a certificate the simulator trusts. --certs DIR makes (once) a
throwaway authority and a certificate for 127.0.0.1 and localhost with
openssl; run the simulator with P4_SIM_EXTRA_CA=DIR/ca.pem. Without
--certs, only the clear-text ports open.

Only the standard library, Pillow (for the pictures) and openssl.
"""
import argparse
import base64
import io
import os
import random
import re
import select
import socket
import ssl
import subprocess
import sys
import threading
import time
from email.utils import formatdate

LOCK = threading.RLock()
CHANGED = threading.Condition(LOCK)
QUIET = False


def log(*a):
    if not QUIET:
        print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---- the store ---------------------------------------------------------------------------------

class Msg:
    def __init__(self, uid, raw, flags=(), when=None):
        self.uid = uid
        self.raw = raw if isinstance(raw, bytes) else raw.encode("utf-8")
        self.flags = set(flags)
        self.modseq = 1
        self.when = when or time.time()


class Box:
    def __init__(self, name, special=None):
        self.name = name
        self.special = special
        self.uidvalidity = int(time.time()) & 0x7FFFFFFF
        self.uidnext = 1
        self.msgs = []
        self.modseq = 1
        self.subscribed = True

    def add(self, raw, flags=(), when=None):
        m = Msg(self.uidnext, raw, flags, when)
        self.uidnext += 1
        self.modseq += 1
        m.modseq = self.modseq
        self.msgs.append(m)
        return m


class Account:
    def __init__(self, user, password, name):
        self.user = user
        self.password = password
        self.name = name
        self.boxes = {}
        self.delim = "/"
        for n, sp in (("INBOX", None), ("Sent", "\\Sent"), ("Drafts", "\\Drafts"),
                      ("Archive", "\\Archive"), ("Trash", "\\Trash"), ("Junk", "\\Junk")):
            self.boxes[n] = Box(n, sp)

    def box(self, name):
        if name.upper() == "INBOX":
            name = "INBOX"
        return self.boxes.get(name)


ACCOUNTS = {}


# ---- the made-up mail ----------------------------------------------------------------------------

def png_bytes(w, h, color, text=None):
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (w, h), color)
    d = ImageDraw.Draw(im)
    for i in range(0, w, 24):
        d.line([(i, 0), (i + h // 2, h)], fill=tuple(min(255, c + 40) for c in color), width=6)
    if text:
        d.rectangle([10, h - 40, 10 + 9 * len(text), h - 14], fill=(255, 255, 255))
        d.text((14, h - 36), text, fill=(0, 0, 0))
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


def jpeg_bytes(w, h, seed):
    from PIL import Image, ImageDraw
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h), (90, 150, 210))
    d = ImageDraw.Draw(im)
    d.rectangle([0, h * 2 // 3, w, h], fill=(70, 140, 60))
    d.ellipse([w * 3 // 4, h // 10, w * 3 // 4 + h // 5, h // 10 + h // 5], fill=(250, 220, 80))
    for _ in range(12):
        x = rnd.randint(0, w)
        y = rnd.randint(h * 2 // 3, h)
        d.ellipse([x - 20, y - 30, x + 20, y + 10], fill=(rnd.randint(150, 255), rnd.randint(40, 120), 60))
    b = io.BytesIO()
    im.save(b, "JPEG", quality=85)
    return b.getvalue()


def pdf_bytes(title, lines):
    """a one-page PDF with a few lines of Helvetica, written by hand"""
    text = "BT /F1 22 Tf 60 760 Td (%s) Tj ET\n" % title
    y = 720
    for ln in lines:
        text += "BT /F1 13 Tf 60 %d Td (%s) Tj ET\n" % (y, ln.replace("(", "\\(").replace(")", "\\)"))
        y -= 20
    objs = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length %d >>\nstream\n%sendstream" % (len(text), text),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = "%PDF-1.4\n"
    offs = []
    for i, o in enumerate(objs):
        offs.append(len(out))
        out += "%d 0 obj\n%s\nendobj\n" % (i + 1, o)
    x = len(out)
    out += "xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        out += "%010d 00000 n \n" % o
    out += "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
    return out.encode("latin-1")


def b64lines(data):
    s = base64.b64encode(data).decode()
    return "\r\n".join(s[i:i + 76] for i in range(0, len(s), 76))


def qp(text, charset):
    import quopri
    return quopri.encodestring(text.encode(charset)).decode("ascii").replace("\n", "\r\n")


def q_word(text, charset="utf-8"):
    """an RFC 2047 encoded word, Q encoding"""
    out = ""
    for b in text.encode(charset):
        c = chr(b)
        if c.isalnum() and b < 128:
            out += c
        elif c == " ":
            out += "_"
        else:
            out += "=%02X" % b
    return "=?%s?Q?%s?=" % (charset, out)


def b_word(text, charset="utf-8"):
    return "=?%s?B?%s?=" % (charset, base64.b64encode(text.encode(charset)).decode())


T0 = time.time()


def date_ago(hours):
    return formatdate(T0 - hours * 3600, localtime=True)


SEQ = [0]


def msgid(tag):
    SEQ[0] += 1
    return "<%s.%d@correo.example.org>" % (tag, SEQ[0])


def head(frm, to, subject, hours, mid, extra=""):
    h = "From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\nMessage-ID: %s\r\nMIME-Version: 1.0\r\n" % (
        frm, to, subject, date_ago(hours), mid)
    return h + extra


ANA = "Ana Ferreyra <ana@example.com>"
BRUNO = "Bruno Iturralde <bruno@example.net>"
PEOPLE = {
    "lucia": "=?utf-8?Q?Luc=C3=ADa_Pe=C3=B1a?= <lucia.pena@example.org>",
    "tomas": "Tomás Arregui <tomas@example.com>",
    "club": "Club de Ajedrez del Barrio <socios@club.example.org>",
    "huerta": "Huerta Comunitaria <huerta@example.net>",
    "jose": "=?iso-8859-1?Q?Jos=E9_Mar=EDa_Gim=E9nez?= <jm.gimenez@example.org>",
    "marta": "Marta Quiroga <marta.q@example.com>",
    "noticias": "=?utf-8?B?Tm90aWNpYXMgZGVsIFRhbGxlciDwn5ug77iP?= <boletin@taller.example.net>",
    "bici": "Bicicletería La Rueda <ventas@larueda.example.com>",
    "facu": "Facundo Rivas <facu.rivas@example.net>",
}


def fill_ana(acc):
    inbox, sent, arch, drafts = acc.boxes["INBOX"], acc.boxes["Sent"], acc.boxes["Archive"], acc.boxes["Drafts"]
    for n in ("Proyectos", "Proyectos/Huerta", "Proyectos/Ajedrez", "Facturas"):
        acc.boxes[n] = Box(n)

    # 1. A conversation of four: the asado, across Inbox and Sent.
    m1 = msgid("asado")
    inbox.add(head(PEOPLE["tomas"], ANA, "Asado del sábado", 70, m1,
                   "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n") +
              "\r\nHola Ana,\r\n\r\n¿Te sumás al asado del sábado en lo de Marta? Arrancamos a las 13.\r\n"
              "Cada uno lleva algo: vos podrías traer la ensalada.\r\n\r\nAbrazo,\r\nTomás\r\n",
              flags={"\\Seen"})
    m2 = msgid("asado")
    sent.add(head(ANA, PEOPLE["tomas"], "Re: Asado del sábado", 68, m2,
                  "In-Reply-To: %s\r\nReferences: %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                  "Content-Transfer-Encoding: quoted-printable\r\n" % (m1, m1)) + "\r\n" +
             qp("¡Dale! Llevo la ensalada y un vino.\n\nEl jueves 28, Tomás Arregui escribió:\n"
                "> ¿Te sumás al asado del sábado en lo de Marta? Arrancamos a las 13.\n"
                "> Cada uno lleva algo: vos podrías traer la ensalada.\n", "utf-8"),
             flags={"\\Seen"})
    m3 = msgid("asado")
    inbox.add(head(PEOPLE["marta"], ANA + ", " + PEOPLE["tomas"], "Re: Asado del sábado", 30, m3,
                   "In-Reply-To: %s\r\nReferences: %s %s\r\nCc: %s\r\n"
                   "Content-Type: text/plain; charset=utf-8\r\n" % (m2, m1, m2, PEOPLE["facu"])) +
              "\r\nLes paso la dirección de nuevo: Calle Falsa 123, timbre B.\r\n"
              "Si llueve lo hacemos adentro, hay parrilla techada.\r\n\r\n"
              "> ¡Dale! Llevo la ensalada y un vino.\r\n>\r\n>> ¿Te sumás al asado del sábado?\r\n",
              flags={"\\Seen"})
    m4 = msgid("asado")
    inbox.add(head(PEOPLE["facu"], ANA + ", " + PEOPLE["tomas"] + ", " + PEOPLE["marta"],
                   "Re: Asado del sábado", 3, m4,
                   "In-Reply-To: %s\r\nReferences: %s %s %s\r\nContent-Type: text/plain; charset=utf-8\r\n" %
                   (m3, m1, m2, m3)) +
              "\r\nYo llevo el carbón y el helado 🍦. ¿Alguien tiene sillas de más?\r\n")

    # 2. ISO-8859-1 with quoted-printable and an encoded subject.
    inbox.add(head(PEOPLE["jose"], ANA, q_word("Reunión de la comisión: orden del día", "iso-8859-1"), 50,
                   msgid("comision"),
                   "Content-Type: text/plain; charset=ISO-8859-1\r\nContent-Transfer-Encoding: quoted-printable\r\n") +
              "\r\n" + qp("Estimada Ana:\n\nLe envío el orden del día de la reunión del miércoles:\n\n"
                          "  1. Lectura del acta anterior.\n  2. Balance del año (¡con superávit!).\n"
                          "  3. Elección de la nueva comisión.\n  4. Varios.\n\n"
                          "Por favor confirme su asistencia. Atentamente,\nJosé María Giménez\nSecretario\n",
                          "iso-8859-1"), flags={"\\Seen"})

    # 3. Windows-1252: curly quotes, the euro sign, a dash, an ellipsis.
    inbox.add(head(PEOPLE["bici"], ANA, "Tu presupuesto =?windows-1252?Q?=96_service_completo?=", 46,
                   msgid("bici"),
                   "Content-Type: text/plain; charset=windows-1252\r\nContent-Transfer-Encoding: 8bit\r\n").encode(
                       "cp1252") + "\r\nHola Ana:\r\n\r\nEl \u201cservice completo\u201d de tu bici sale \u20ac 85 "
              "(cambio de cadena, frenos y centrado) \u2013 lo tenemos listo el viernes\u2026\r\n\r\n"
              "Saludos,\r\nBicicletería La Rueda\r\n".encode("cp1252"))

    # 4. A newsletter in HTML only, base64, with lists, links, a quote, a table.
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>Boletín</title>
<style>p{margin:0} .x{color:red}</style><script>alert(1)</script></head>
<body><div style="max-width:600px">
<h1>Boletín del Taller &mdash; noviembre</h1>
<p>¡Hola! Este mes en el taller de electrónica armamos <b>relojes con matrices de LED</b> y
<i>reparamos</i> tres radios a válvulas. Las fotos están en <a href="https://taller.example.net/fotos">la galería</a>.</p>
<h2>Próximos encuentros</h2>
<ul>
 <li>Sábado 6: soldadura para principiantes</li>
 <li>Sábado 13: <strong>osciloscopios</strong>, cómo leerlos</li>
 <li>Sábado 20: noche de proyectos libres</li>
</ul>
<h2>Lo que hace falta</h2>
<ol><li>Estaño sin plomo</li><li>Puntas para el soldador&nbsp;de&nbsp;60&nbsp;W</li><li>Ganas</li></ol>
<blockquote>«La mejor herramienta es la que te prestan.»<br>&mdash; un socio fundador</blockquote>
<table border="1"><tr><th>Cuota</th><th>Precio</th></tr><tr><td>Mensual</td><td>$ 4.500</td></tr>
<tr><td>Anual</td><td>$ 45.000</td></tr></table>
<p>Para darte de baja, <a href="https://taller.example.net/baja?u=123">tocá acá</a>. &copy; 2026 Taller &amp; Cía.</p>
<img src="https://taller.example.net/pixel.gif" width="1" height="1">
</div></body></html>"""
    inbox.add(head(PEOPLE["noticias"], ANA, b_word("Boletín de noviembre: relojes, radios y más 📻"), 40,
                   msgid("boletin"),
                   "Content-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n") +
              "\r\n" + b64lines(html.encode("utf-8")) + "\r\n")

    # 5. multipart/alternative with a plain part and an HTML one.
    bnd = "alt-0001"
    inbox.add(head(PEOPLE["club"], ANA, "Torneo relámpago: resultados", 26, msgid("torneo"),
                   "Content-Type: multipart/alternative; boundary=\"%s\"\r\n" % bnd) +
              "\r\nThis is a multi-part message in MIME format.\r\n--%s\r\n"
              "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n%s\r\n"
              "--%s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n%s\r\n"
              "--%s--\r\n" % (
                  bnd, qp("Resultados del torneo relámpago:\n1. Facundo Rivas, 5 puntos\n2. Ana Ferreyra, 4,5\n"
                          "3. Lucía Peña, 4\n", "utf-8"),
                  bnd, qp("<p>Resultados del <b>torneo relámpago</b>:</p><ol><li>Facundo Rivas, 5 puntos</li>"
                          "<li><b>Ana Ferreyra</b>, 4,5</li><li>Lucía Peña, 4</li></ol>"
                          "<p>¡Felicitaciones a todos! Las partidas están en "
                          "<a href=\"https://club.example.org/partidas\">la página del club</a>.</p>", "utf-8"),
                  bnd), flags={"\\Seen", "\\Flagged"})

    # 6. multipart/related: HTML with two inline pictures (cid:).
    rel, alt = "rel-0002", "alt-0003"
    p1 = png_bytes(320, 200, (40, 120, 70), "tomates")
    p2 = jpeg_bytes(480, 320, 7)
    inbox.add(head(PEOPLE["huerta"], ANA, "Fotos de la cosecha 🍅", 20, msgid("huerta"),
                   "Content-Type: multipart/related; boundary=\"%s\"; type=\"multipart/alternative\"\r\n" % rel) +
              ("\r\n--{rel}\r\nContent-Type: multipart/alternative; boundary=\"{alt}\"\r\n\r\n"
               "--{alt}\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nMirá cómo vienen los tomates y la huerta.\r\n"
               "--{alt}\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
               "<p>¡Hola, Ana! Mirá cómo vienen los tomates:</p><p><img src=\"cid:tomates@huerta\" alt=\"tomates\"></p>"
               "<p>Y así quedó el cantero nuevo:</p><p><img src=\"cid:cantero@huerta\"></p>"
               "<p>El domingo cosechamos. Traé bolsas.</p>\r\n--{alt}--\r\n"
               "--{rel}\r\nContent-Type: image/png\r\nContent-Transfer-Encoding: base64\r\n"
               "Content-ID: <tomates@huerta>\r\nContent-Disposition: inline; filename=\"tomates.png\"\r\n\r\n{p1}\r\n"
               "--{rel}\r\nContent-Type: image/jpeg; name=\"cantero.jpg\"\r\nContent-Transfer-Encoding: base64\r\n"
               "Content-ID: <cantero@huerta>\r\n\r\n{p2}\r\n--{rel}--\r\n").format(
                  rel=rel, alt=alt, p1=b64lines(p1), p2=b64lines(p2)))

    # 7. multipart/mixed with attachments: a PDF, a JPEG, a text file with an RFC 2231 name.
    mix = "mix-0004"
    pdf = pdf_bytes("Factura B 0003-00012345", ["Bicicleteria La Rueda", "Service completo ......... 85,00",
                                                  "Total ...................... 85,00"])
    photo = jpeg_bytes(1024, 768, 3)
    note = "Lista de repuestos:\n- cadena 8v\n- pastillas de freno\n- cámara 29\"\n".encode("utf-8")
    inbox.add(head(PEOPLE["bici"], ANA, "Factura y fotos del service", 12, msgid("factura"),
                   "Content-Type: multipart/mixed; boundary=\"%s\"\r\n" % mix) +
              ("\r\n--{m}\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nTe adjuntamos la factura, una foto de "
               "cómo quedó y la lista de repuestos.\r\n¡Gracias por elegirnos!\r\n"
               "--{m}\r\nContent-Type: application/pdf; name=\"factura-12345.pdf\"\r\n"
               "Content-Disposition: attachment; filename=\"factura-12345.pdf\"\r\nContent-Transfer-Encoding: base64\r\n\r\n{pdf}\r\n"
               "--{m}\r\nContent-Type: image/jpeg\r\nContent-Disposition: attachment; filename=\"bici.jpg\"\r\n"
               "Content-Transfer-Encoding: base64\r\n\r\n{jpg}\r\n"
               "--{m}\r\nContent-Type: text/plain; charset=utf-8\r\n"
               "Content-Disposition: attachment; filename*=utf-8''lista%20de%20repuestos%20%C3%B1.txt\r\n"
               "Content-Transfer-Encoding: quoted-printable\r\n\r\n{txt}\r\n--{m}--\r\n").format(
                  m=mix, pdf=b64lines(pdf), jpg=b64lines(photo), txt=qp(note.decode(), "utf-8")))
    acc.boxes["Facturas"].add(head(PEOPLE["bici"], ANA, "Factura de octubre", 24 * 30, msgid("factura"),
                                   "Content-Type: multipart/mixed; boundary=\"%s\"\r\n" % mix) +
                              ("\r\n--{m}\r\nContent-Type: text/plain\r\n\r\nFactura adjunta.\r\n"
                               "--{m}\r\nContent-Type: application/pdf; name=\"factura-11002.pdf\"\r\n"
                               "Content-Transfer-Encoding: base64\r\n\r\n{pdf}\r\n--{m}--\r\n").format(
                                  m=mix, pdf=b64lines(pdf_bytes("Factura B 0003-00011002", ["Octubre"]))),
                              flags={"\\Seen"})

    # 8. A second conversation, in the project folder, with Lucía.
    l1 = msgid("plano")
    acc.boxes["Proyectos/Huerta"].add(
        head(PEOPLE["lucia"], ANA, "Plano del riego por goteo", 120, l1,
             "Content-Type: text/plain; charset=utf-8\r\n") +
        "\r\nAna, ¿viste el plano que armé? Son tres líneas de 20 m con goteros cada 30 cm.\r\n", flags={"\\Seen"})
    l2 = msgid("plano")
    acc.boxes["Proyectos/Huerta"].add(
        head(ANA, PEOPLE["lucia"], "Re: Plano del riego por goteo", 118, l2,
             "In-Reply-To: %s\r\nReferences: %s\r\nContent-Type: text/plain; charset=utf-8\r\n" % (l1, l1)) +
        "\r\nMe encanta. ¿Y la bomba? Pensaba en una de 0,5 HP.\r\n", flags={"\\Seen", "\\Answered"})
    inbox.add(head(PEOPLE["lucia"], ANA, "Re: Plano del riego por goteo", 8, msgid("plano"),
                   "In-Reply-To: %s\r\nReferences: %s %s\r\nContent-Type: text/plain; charset=utf-8; format=flowed\r\n"
                   % (l2, l1, l2)) +
              "\r\nCon 0,5 HP sobra. Te paso el presupuesto de la ferretería mañana. \r\nAprovecho y te pregunto "
              "si el sábado podés dar una mano con los caños, son pesados \r\ny entre dos se hace en una mañana.\r\n\r\n"
              "> Me encanta. ¿Y la bomba? Pensaba en una de 0,5 HP.\r\n")

    # 9. Plain ASCII, short ones, to fill the list.
    for i, (who, subj, body, hrs) in enumerate([
        ("club", "Cuota de diciembre", "Recordá que la cuota vence el día 10.", 90),
        ("marta", "Receta del budín", "Harina, azúcar, huevos, limón. Horno moderado 40 minutos.", 100),
        ("facu", "Partida por correo", "1. e4 ... te toca a vos.", 110),
        ("tomas", "El libro que te debo", "Te lo dejo en la portería el lunes.", 130),
        ("lucia", "Semillas", "Conseguí semillas de zapallo anco. ¿Querés?", 150),
        ("club", "Simultánea con el maestro", "El viernes 19 a las 20, cupo de 20 tableros.", 160),
        ("jose", "Acta de la reunión", "Adjunto el acta en el próximo correo.", 170),
        ("marta", "Fotos del cumple", "Te las paso por otro lado, pesan mucho.", 190),
        ("bici", "Ofertas de primavera", "20 % en cascos y luces hasta fin de mes.", 200),
        ("facu", "Re: Partida por correo", "2. Cf3 ... ¿seguís?", 210),
    ]):
        flags = {"\\Seen"} if i % 3 else set()
        inbox.add(head(PEOPLE[who], ANA, subj, hrs, msgid("varios"),
                       "Content-Type: text/plain; charset=us-ascii\r\n" if all(ord(c) < 128 for c in body)
                       else "Content-Type: text/plain; charset=utf-8\r\n") + "\r\n" + body + "\r\n", flags=flags)

    # 10. Older ones, archived; a draft; one in the chess folder.
    for i in range(6):
        arch.add(head(PEOPLE["club" if i % 2 else "huerta"], ANA, "Aviso %d de la temporada pasada" % (i + 1),
                      24 * (40 + i * 7), msgid("viejo"), "Content-Type: text/plain\r\n") +
                 "\r\nUn aviso viejo, guardado por las dudas.\r\n", flags={"\\Seen"})
    drafts.add(head(ANA, PEOPLE["jose"], "Confirmo asistencia", 5, msgid("borrador"),
                    "Content-Type: text/plain; charset=utf-8\r\n") + "\r\nEstimado José María: confirmo que\r\n",
               flags={"\\Seen", "\\Draft"})
    acc.boxes["Proyectos/Ajedrez"].add(head(PEOPLE["club"], ANA, "Fixture del interclubes", 300, msgid("fixture"),
                                            "Content-Type: text/plain; charset=utf-8\r\n") +
                                       "\r\nFecha 1: contra Villa Sur, de local.\r\n", flags={"\\Seen"})


def fill_bruno(acc):
    inbox = acc.boxes["INBOX"]
    inbox.add(head(ANA, BRUNO, "¿Venís al taller?", 6, msgid("taller"),
                   "Content-Type: text/plain; charset=utf-8\r\n") +
              "\r\nBruno, el sábado hay soldadura para principiantes. ¿Te anoto?\r\n")
    inbox.add(head(PEOPLE["noticias"], BRUNO, "Bienvenido al boletín", 48, msgid("bienvenida"),
                   "Content-Type: text/plain; charset=utf-8\r\n") + "\r\nGracias por suscribirte.\r\n",
              flags={"\\Seen"})


DRIP = [
    ("tomas", "¿Llevo hielo?", "Para el asado, digo. Avisame."),
    ("lucia", "Llegó el presupuesto", "La bomba sale menos de lo que pensábamos."),
    ("club", "Cambio de horario", "La simultánea pasa a las 21."),
    ("marta", "Gracias por venir", "La pasamos muy bien. ¡La ensalada estaba buenísima!"),
]


def drip_one(i):
    who, subj, body = DRIP[i % len(DRIP)]
    raw = head(PEOPLE[who], ANA, subj, 0, msgid("nuevo"), "Content-Type: text/plain; charset=utf-8\r\n") + \
        "\r\n" + body + "\r\n"
    with LOCK:
        ACCOUNTS["ana@example.com"].boxes["INBOX"].add(raw)
        CHANGED.notify_all()
    log("drip: '%s' delivered to ana" % subj)


# ---- IMAP --------------------------------------------------------------------------------------

class Bye(Exception):
    pass


def imap_quote(s):
    if s is None:
        return "NIL"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse_args(s):
    """the arguments of a command line: atoms, quoted strings, literals
    (already inlined as bytes) and parenthesised lists"""
    out = []
    stack = [out]
    i = 0
    while i < len(s):
        c = s[i]
        if c == " ":
            i += 1
        elif c == "(":
            n = []
            stack[-1].append(n)
            stack.append(n)
            i += 1
        elif c == ")":
            stack.pop()
            i += 1
        elif c == '"':
            j = i + 1
            v = ""
            while j < len(s) and s[j] != '"':
                if s[j] == "\\":
                    j += 1
                v += s[j]
                j += 1
            stack[-1].append(v)
            i = j + 1
        elif c == "\x00":            # a literal's placeholder
            j = s.index("\x00", i + 1)
            stack[-1].append(LITERALS[int(s[i + 1:j])])
            i = j + 1
        else:
            j = i
            depth = 0
            while j < len(s) and (depth or (s[j] not in " ()")):
                if s[j] == "[":
                    depth += 1
                elif s[j] == "]":
                    depth -= 1
                j += 1
            stack[-1].append(s[i:j])
            i = j
    return out


LITERALS = []


def seq_set(spec, maxv):
    """a sequence set as a predicate on numbers; '*' is maxv"""
    ranges = []
    for part in spec.split(","):
        if ":" in part:
            a, b = part.split(":")
            a = maxv if a == "*" else int(a)
            b = maxv if b == "*" else int(b)
            if a > b:
                a, b = b, a
            ranges.append((a, b))
        else:
            v = maxv if part == "*" else int(part)
            ranges.append((v, v))
    return lambda n: any(a <= n <= b for a, b in ranges)


def header_text(raw):
    k = raw.find(b"\r\n\r\n")
    return raw[:k + 4] if k >= 0 else raw


def body_text(raw):
    k = raw.find(b"\r\n\r\n")
    return raw[k + 4:] if k >= 0 else b""


def header_fields(raw, names, negate=False):
    hdr = header_text(raw).decode("latin-1")
    lines = re.split(r"\r\n(?![ \t])", hdr)
    out = []
    want = {n.upper() for n in names}
    for ln in lines:
        if ":" not in ln:
            continue
        name = ln.split(":", 1)[0].strip().upper()
        if (name in want) != negate:
            out.append(ln)
    return ("\r\n".join(out) + "\r\n\r\n").encode("latin-1")


def mime_part(raw, path):
    """the bytes of a numbered part (IMAP's section numbers), headers left out"""
    import email
    import email.policy
    msg = email.message_from_bytes(raw, policy=email.policy.compat32)
    cur = msg
    for n in path:
        if cur.is_multipart():
            cur = cur.get_payload()[n - 1]
        elif n != 1:
            return b""
    payload = cur.as_bytes()
    k = payload.find(b"\n\n")
    data = payload[k + 2:] if k >= 0 else payload
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def decode_words(s):
    from email.header import decode_header
    try:
        out = ""
        for t, cs in decode_header(s):
            out += t.decode(cs or "latin-1", "replace") if isinstance(t, bytes) else t
        return out
    except Exception:
        return s


def search_text(m):
    hdr = header_text(m.raw).decode("latin-1")
    return (decode_words(hdr) + body_text(m.raw).decode("utf-8", "replace")).lower()


class ImapSession:
    def __init__(self, sock, tls_ctx, implicit):
        self.sock = sock
        self.tls_ctx = tls_ctx
        self.tls = implicit
        self.buf = b""
        self.acc = None
        self.box = None
        self.readonly = False
        self.condstore = False
        self.seen_count = 0        # what this session has been told: EXISTS
        self.seen_uids = []
        self.flag_seq = 0

    def send(self, s):
        if isinstance(s, str):
            s = s.encode("utf-8")
        self.sock.sendall(s)

    def readline(self, timeout=None):
        while b"\r\n" not in self.buf:
            if timeout is not None:
                if not isinstance(self.sock, ssl.SSLSocket) or not self.sock.pending():
                    r, _, _ = select.select([self.sock], [], [], timeout)
                    if not r:
                        return None
            d = self.sock.recv(65536)
            if not d:
                raise Bye()
            self.buf += d
        ln, self.buf = self.buf.split(b"\r\n", 1)
        return ln

    def readn(self, n):
        while len(self.buf) < n:
            d = self.sock.recv(65536)
            if not d:
                raise Bye()
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read_command(self):
        """one command, its literals inlined as placeholders"""
        del LITERALS[:]
        line = self.readline()
        text = ""
        while True:
            m = re.search(rb"\{(\d+)(\+?)\}$", line)
            if not m:
                text += line.decode("utf-8", "replace")
                return text
            text += line[:m.start()].decode("utf-8", "replace")
            if not m.group(2):
                self.send("+ Ready for literal\r\n")
            LITERALS.append(self.readn(int(m.group(1))))
            text += "\x00%d\x00" % (len(LITERALS) - 1)
            line = self.readline()

    def caps(self):
        c = "IMAP4rev1 LITERAL+ IDLE UIDPLUS MOVE CONDSTORE ENABLE SPECIAL-USE NAMESPACE ID"
        if not self.tls and self.tls_ctx:
            c += " STARTTLS"
        if not self.acc:
            c += " AUTH=PLAIN AUTH=LOGIN"
        return c

    def run(self):
        self.send("* OK [CAPABILITY %s] Correo falso de P4OS listo\r\n" % self.caps())
        while True:
            line = self.read_command()
            if not line:
                continue
            parts = line.split(" ", 2)
            tag = parts[0]
            cmd = parts[1].upper() if len(parts) > 1 else ""
            rest = parts[2] if len(parts) > 2 else ""
            uid = False
            if cmd == "UID":
                uid = True
                p2 = rest.split(" ", 1)
                cmd = p2[0].upper()
                rest = p2[1] if len(p2) > 1 else ""
            log("imap %s %s" % (self.acc.user if self.acc else "-", line[:120].replace("\x00", "#")))
            try:
                fn = getattr(self, "c_" + cmd.lower(), None)
                if not fn:
                    self.send("%s BAD unknown command\r\n" % tag)
                    continue
                with LOCK:
                    pass
                fn(tag, rest, uid)
            except Bye:
                raise
            except Exception as e:
                import traceback
                traceback.print_exc()
                self.send("%s BAD %s\r\n" % (tag, str(e).replace("\r", " ").replace("\n", " ")))

    # -- not authenticated
    def c_capability(self, tag, rest, uid):
        self.send("* CAPABILITY %s\r\n%s OK CAPABILITY done\r\n" % (self.caps(), tag))

    def c_noop(self, tag, rest, uid):
        with LOCK:
            self.updates()
        self.send("%s OK NOOP done\r\n" % tag)

    def c_id(self, tag, rest, uid):
        self.send('* ID ("name" "fake_mail" "vendor" "P4OS")\r\n%s OK ID done\r\n' % tag)

    def c_logout(self, tag, rest, uid):
        self.send("* BYE hasta luego\r\n%s OK LOGOUT done\r\n" % tag)
        raise Bye()

    def c_starttls(self, tag, rest, uid):
        if self.tls or not self.tls_ctx:
            self.send("%s BAD no STARTTLS here\r\n" % tag)
            return
        self.send("%s OK Begin TLS negotiation now\r\n" % tag)
        self.sock = self.tls_ctx.wrap_socket(self.sock, server_side=True)
        self.tls = True
        self.buf = b""

    def login_ok(self, tag, user, pw):
        acc = ACCOUNTS.get(user.lower())
        if not acc or acc.password != pw:
            time.sleep(0.5)
            self.send("%s NO [AUTHENTICATIONFAILED] Usuario o contraseña incorrectos\r\n" % tag)
            return
        self.acc = acc
        self.send("%s OK [CAPABILITY %s] Logged in\r\n" % (tag, self.caps()))

    def c_login(self, tag, rest, uid):
        a = parse_args(rest)
        u = a[0].decode() if isinstance(a[0], bytes) else a[0]
        p = a[1].decode() if isinstance(a[1], bytes) else a[1]
        self.login_ok(tag, u, p)

    def c_authenticate(self, tag, rest, uid):
        a = rest.split()
        mech = a[0].upper()
        if mech == "PLAIN":
            if len(a) > 1:
                data = a[1]
            else:
                self.send("+ \r\n")
                data = self.readline().decode()
            try:
                _, u, p = base64.b64decode(data).decode("utf-8").split("\x00")
            except Exception:
                self.send("%s BAD bad PLAIN\r\n" % tag)
                return
            self.login_ok(tag, u, p)
        elif mech == "LOGIN":
            self.send("+ VXNlcm5hbWU6\r\n")
            u = base64.b64decode(self.readline()).decode()
            self.send("+ UGFzc3dvcmQ6\r\n")
            p = base64.b64decode(self.readline()).decode()
            self.login_ok(tag, u, p)
        else:
            self.send("%s NO unsupported mechanism\r\n" % tag)

    def need_auth(self, tag):
        if not self.acc:
            self.send("%s NO log in first\r\n" % tag)
            return False
        return True

    def need_box(self, tag):
        if not self.need_auth(tag):
            return False
        if not self.box:
            self.send("%s NO select a mailbox first\r\n" % tag)
            return False
        return True

    # -- authenticated
    def c_enable(self, tag, rest, uid):
        if "CONDSTORE" in rest.upper():
            self.condstore = True
            self.send("* ENABLED CONDSTORE\r\n")
        self.send("%s OK ENABLE done\r\n" % tag)

    def c_namespace(self, tag, rest, uid):
        self.send('* NAMESPACE (("" "/")) NIL NIL\r\n%s OK NAMESPACE done\r\n' % tag)

    def c_list(self, tag, rest, uid, lsub=False):
        if not self.need_auth(tag):
            return
        a = parse_args(rest)
        ref = a[0] if isinstance(a[0], str) else ""
        pat = a[1] if len(a) > 1 else ""
        if isinstance(pat, list):
            pat = pat[0] if pat else ""
        if pat == "":
            self.send('* LIST (\\Noselect) "/" ""\r\n%s OK LIST done\r\n' % tag)
            return
        rx = "^" + re.escape(ref + pat).replace(r"\*", ".*").replace("%", "[^/]*") + "$"
        with LOCK:
            names = sorted(self.acc.boxes.keys(), key=lambda n: (n != "INBOX", n))
            for n in names:
                b = self.acc.boxes[n]
                if not re.match(rx, n, re.I):
                    continue
                if lsub and not b.subscribed:
                    continue
                fl = []
                kids = any(k.startswith(n + "/") for k in names)
                fl.append("\\HasChildren" if kids else "\\HasNoChildren")
                if b.special:
                    fl.append(b.special)
                self.send('* %s (%s) "/" %s\r\n' % ("LSUB" if lsub else "LIST", " ".join(fl), imap_quote(n)))
        self.send("%s OK %s done\r\n" % (tag, "LSUB" if lsub else "LIST"))

    def c_lsub(self, tag, rest, uid):
        self.c_list(tag, rest, uid, True)

    def c_status(self, tag, rest, uid):
        if not self.need_auth(tag):
            return
        a = parse_args(rest)
        name = a[0].decode() if isinstance(a[0], bytes) else a[0]
        with LOCK:
            b = self.acc.box(name)
            if not b:
                self.send("%s NO no such mailbox\r\n" % tag)
                return
            items = []
            for it in a[1]:
                it = it.upper()
                v = {"MESSAGES": len(b.msgs), "UIDNEXT": b.uidnext, "UIDVALIDITY": b.uidvalidity,
                     "UNSEEN": sum(1 for m in b.msgs if "\\Seen" not in m.flags),
                     "RECENT": 0, "HIGHESTMODSEQ": b.modseq}.get(it)
                if v is not None:
                    items.append("%s %d" % (it, v))
            self.send("* STATUS %s (%s)\r\n%s OK STATUS done\r\n" % (imap_quote(b.name), " ".join(items), tag))

    def c_select(self, tag, rest, uid, readonly=False):
        if not self.need_auth(tag):
            return
        a = parse_args(rest)
        name = a[0].decode() if isinstance(a[0], bytes) else a[0]
        if len(a) > 1 and isinstance(a[1], list) and any(x.upper() == "CONDSTORE" for x in a[1]):
            self.condstore = True
        with LOCK:
            b = self.acc.box(name)
            if not b:
                self.box = None
                self.send("%s NO [NONEXISTENT] no such mailbox\r\n" % tag)
                return
            self.box = b
            self.readonly = readonly
            self.seen_uids = [m.uid for m in b.msgs]
            self.flag_seq = b.modseq
            unseen = next((i + 1 for i, m in enumerate(b.msgs) if "\\Seen" not in m.flags), None)
            out = "* %d EXISTS\r\n* 0 RECENT\r\n" % len(b.msgs)
            out += "* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n"
            out += "* OK [PERMANENTFLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft \\*)] ok\r\n"
            if unseen:
                out += "* OK [UNSEEN %d] first unseen\r\n" % unseen
            out += "* OK [UIDVALIDITY %d] UIDs valid\r\n* OK [UIDNEXT %d] next\r\n" % (b.uidvalidity, b.uidnext)
            out += "* OK [HIGHESTMODSEQ %d] modseq\r\n" % b.modseq
            out += "%s OK [%s] %s done\r\n" % (tag, "READ-ONLY" if readonly else "READ-WRITE",
                                              "EXAMINE" if readonly else "SELECT")
            self.send(out)

    def c_examine(self, tag, rest, uid):
        self.c_select(tag, rest, uid, True)

    def c_create(self, tag, rest, uid):
        if not self.need_auth(tag):
            return
        name = parse_args(rest)[0]
        with LOCK:
            if self.acc.box(name):
                self.send("%s NO [ALREADYEXISTS] exists\r\n" % tag)
                return
            self.acc.boxes[name] = Box(name)
        self.send("%s OK CREATE done\r\n" % tag)

    def c_delete(self, tag, rest, uid):
        if not self.need_auth(tag):
            return
        name = parse_args(rest)[0]
        with LOCK:
            if name == "INBOX" or not self.acc.box(name):
                self.send("%s NO cannot\r\n" % tag)
                return
            del self.acc.boxes[name]
        self.send("%s OK DELETE done\r\n" % tag)

    def c_subscribe(self, tag, rest, uid):
        self.send("%s OK SUBSCRIBE done\r\n" % tag)

    def c_unsubscribe(self, tag, rest, uid):
        self.send("%s OK UNSUBSCRIBE done\r\n" % tag)

    def c_append(self, tag, rest, uid):
        if not self.need_auth(tag):
            return
        a = parse_args(rest)
        name = a[0]
        flags = set()
        data = a[-1]
        for x in a[1:-1]:
            if isinstance(x, list):
                flags = set(x)
        with LOCK:
            b = self.acc.box(name)
            if not b:
                self.send("%s NO [TRYCREATE] no such mailbox\r\n" % tag)
                return
            m = b.add(data, flags)
            CHANGED.notify_all()
            if b is self.box:
                self.updates()
        self.send("%s OK [APPENDUID %d %d] APPEND done\r\n" % (tag, b.uidvalidity, m.uid))

    def c_close(self, tag, rest, uid):
        if self.box and not self.readonly:
            with LOCK:
                self.box.msgs = [m for m in self.box.msgs if "\\Deleted" not in m.flags]
                self.box.modseq += 1
                CHANGED.notify_all()
        self.box = None
        self.send("%s OK CLOSE done\r\n" % tag)

    def c_unselect(self, tag, rest, uid):
        self.box = None
        self.send("%s OK UNSELECT done\r\n" % tag)

    # -- selected
    def updates(self):
        """untagged EXPUNGE / EXISTS / FETCH for what changed since the
        session last looked (another session, SMTP, the drip)"""
        b = self.box
        if not b:
            return
        now = [m.uid for m in b.msgs]
        known = set(now)
        out = ""
        # expunges, highest sequence number first so the numbers stay right
        for i in range(len(self.seen_uids) - 1, -1, -1):
            if self.seen_uids[i] not in known:
                out += "* %d EXPUNGE\r\n" % (i + 1)
                del self.seen_uids[i]
        if len(now) != len(self.seen_uids):
            out += "* %d EXISTS\r\n" % len(now)
        old = set(self.seen_uids)
        for i, m in enumerate(b.msgs):
            if m.uid in old and m.modseq > self.flag_seq:
                out += "* %d FETCH (UID %d FLAGS (%s)%s)\r\n" % (
                    i + 1, m.uid, " ".join(sorted(m.flags)), " MODSEQ (%d)" % m.modseq if self.condstore else "")
        self.seen_uids = now
        self.flag_seq = b.modseq
        if out:
            self.send(out)

    def pick(self, spec, uid):
        b = self.box
        if uid:
            maxuid = b.msgs[-1].uid if b.msgs else 0
            if spec.startswith("*") and b.msgs:
                pass
            f = seq_set(spec, maxuid)
            res = [(i + 1, m) for i, m in enumerate(b.msgs) if f(m.uid)]
            # "n:*" always includes the last message, even when n is above it
            if not res and b.msgs and spec.endswith(":*"):
                res = [(len(b.msgs), b.msgs[-1])]
            return res
        f = seq_set(spec, len(b.msgs))
        return [(i + 1, m) for i, m in enumerate(b.msgs) if f(i + 1)]

    def c_fetch(self, tag, rest, uid):
        if not self.need_box(tag):
            return
        spec, items = rest.split(" ", 1)
        a = parse_args(items)
        changed = None
        if len(a) > 1 and isinstance(a[1], list) and a[1] and a[1][0].upper() == "CHANGEDSINCE":
            changed = int(a[1][1])
            self.condstore = True
        want = a[0] if isinstance(a[0], list) else [a[0]]
        names = []
        for w in want:
            u = w.upper()
            if u == "ALL":
                names += ["FLAGS", "INTERNALDATE", "RFC822.SIZE"]
            elif u == "FAST":
                names += ["FLAGS", "INTERNALDATE", "RFC822.SIZE"]
            else:
                names.append(w)
        if uid and "UID" not in [n.upper() for n in names]:
            names.insert(0, "UID")
        with LOCK:
            out = []
            for seq, m in self.pick(spec, uid):
                if changed is not None and m.modseq <= changed:
                    continue
                parts = []
                mark_seen = False
                for n in names:
                    u = n.upper()
                    if u == "UID":
                        parts.append(b"UID %d" % m.uid)
                    elif u == "FLAGS":
                        parts.append(("FLAGS (%s)" % " ".join(sorted(m.flags))).encode())
                    elif u == "RFC822.SIZE":
                        parts.append(b"RFC822.SIZE %d" % len(m.raw))
                    elif u == "INTERNALDATE":
                        parts.append(('INTERNALDATE "%s"' % time.strftime("%d-%b-%Y %H:%M:%S +0000",
                                                                          time.gmtime(m.when))).encode())
                    elif u == "MODSEQ":
                        parts.append(b"MODSEQ (%d)" % m.modseq)
                    elif u.startswith("BODY") or u.startswith("RFC822"):
                        peek = ".PEEK" in u
                        if u in ("RFC822", "RFC822.HEADER", "RFC822.TEXT"):
                            sec = {"RFC822": "", "RFC822.HEADER": "HEADER", "RFC822.TEXT": "TEXT"}[u]
                            label = u
                            partial = None
                            peek = u != "RFC822"
                        else:
                            mm = re.match(r"BODY(?:\.PEEK)?\[(.*?)\](?:<(\d+)(?:\.(\d+))?>)?$", n, re.I | re.S)
                            if not mm:
                                raise ValueError("bad fetch item %s" % n)
                            sec = mm.group(1)
                            partial = (int(mm.group(2)), int(mm.group(3) or 1 << 30)) if mm.group(2) else None
                            label = "BODY[%s]" % sec
                        data = self.section(m, sec)
                        if partial:
                            data = data[partial[0]:partial[0] + partial[1]]
                            label += "<%d>" % partial[0]
                        parts.append(label.encode() + b" {%d}\r\n" % len(data) + data)
                        if not peek and not self.readonly and "\\Seen" not in m.flags:
                            mark_seen = True
                    elif u == "ENVELOPE" or u == "BODYSTRUCTURE":
                        pass
                if mark_seen:
                    m.flags.add("\\Seen")
                    self.box.modseq += 1
                    m.modseq = self.box.modseq
                    parts.append(("FLAGS (%s)" % " ".join(sorted(m.flags))).encode())
                if self.condstore and not any(p.startswith(b"MODSEQ") for p in parts):
                    parts.append(b"MODSEQ (%d)" % m.modseq)
                out.append(b"* %d FETCH (" % seq + b" ".join(parts) + b")\r\n")
            self.send(b"".join(out) + ("%s OK FETCH done\r\n" % tag).encode())
            self.flag_seq = max(self.flag_seq, 0)

    def section(self, m, sec):
        s = sec.upper()
        if s == "":
            return m.raw
        if s == "HEADER":
            return header_text(m.raw)
        if s == "TEXT":
            return body_text(m.raw)
        mm = re.match(r"HEADER\.FIELDS(\.NOT)?\s*\((.*)\)", sec, re.I | re.S)
        if mm:
            return header_fields(m.raw, mm.group(2).split(), bool(mm.group(1)))
        if re.match(r"^[\d.]+$", s):
            return mime_part(m.raw, [int(x) for x in s.split(".")])
        mm = re.match(r"^([\d.]+)\.MIME$", s)
        if mm:
            return b""
        raise ValueError("unsupported section %s" % sec)

    def c_store(self, tag, rest, uid):
        if not self.need_box(tag):
            return
        a = parse_args(rest)
        spec = a[0]
        i = 1
        unchanged = None
        if isinstance(a[1], list):
            unchanged = int(a[1][1])
            i = 2
        op = a[i].upper()
        flags = a[i + 1] if isinstance(a[i + 1], list) else a[i + 1:]
        silent = op.endswith(".SILENT")
        with LOCK:
            out = ""
            for seq, m in self.pick(spec, uid):
                if unchanged is not None and m.modseq > unchanged:
                    continue
                before = set(m.flags)
                if op.startswith("+"):
                    m.flags |= set(flags)
                elif op.startswith("-"):
                    m.flags -= set(flags)
                else:
                    m.flags = set(flags)
                if m.flags != before:
                    self.box.modseq += 1
                    m.modseq = self.box.modseq
                if not silent:
                    out += "* %d FETCH (%sFLAGS (%s)%s)\r\n" % (
                        seq, "UID %d " % m.uid if uid else "", " ".join(sorted(m.flags)),
                        " MODSEQ (%d)" % m.modseq if self.condstore else "")
            self.flag_seq = self.box.modseq
            CHANGED.notify_all()
        self.send(out + "%s OK STORE done\r\n" % tag)

    def c_copy(self, tag, rest, uid, move=False):
        if not self.need_box(tag):
            return
        spec, dest = rest.split(" ", 1)
        dest = parse_args(dest)[0]
        with LOCK:
            d = self.acc.box(dest)
            if not d:
                self.send("%s NO [TRYCREATE] no such mailbox\r\n" % tag)
                return
            src, dst = [], []
            picked = self.pick(spec, uid)
            for seq, m in picked:
                n = d.add(m.raw, m.flags - {"\\Recent"}, m.when)
                src.append(m.uid)
                dst.append(n.uid)
            out = ""
            code = "[COPYUID %d %s %s]" % (d.uidvalidity, ",".join(map(str, src)), ",".join(map(str, dst))) if src else ""
            if move:
                if code:
                    out += "* OK %s moved\r\n" % code
                gone = {m.uid for _, m in picked}
                for i in range(len(self.box.msgs) - 1, -1, -1):
                    if self.box.msgs[i].uid in gone:
                        out += "* %d EXPUNGE\r\n" % (i + 1)
                        del self.box.msgs[i]
                self.box.modseq += 1
                self.seen_uids = [m.uid for m in self.box.msgs]
            CHANGED.notify_all()
        self.send(out + "%s OK %s%s done\r\n" % (tag, "" if move else code + " ", "MOVE" if move else "COPY"))

    def c_move(self, tag, rest, uid):
        self.c_copy(tag, rest, uid, True)

    def c_expunge(self, tag, rest, uid):
        if not self.need_box(tag):
            return
        with LOCK:
            keep = None
            if uid and rest:
                keep = seq_set(rest.strip(), self.box.msgs[-1].uid if self.box.msgs else 0)
            out = ""
            for i in range(len(self.box.msgs) - 1, -1, -1):
                m = self.box.msgs[i]
                if "\\Deleted" in m.flags and (keep is None or keep(m.uid)):
                    out += "* %d EXPUNGE\r\n" % (i + 1)
                    del self.box.msgs[i]
            self.box.modseq += 1
            self.seen_uids = [m.uid for m in self.box.msgs]
            CHANGED.notify_all()
        self.send(out + "%s OK EXPUNGE done\r\n" % tag)

    def c_search(self, tag, rest, uid):
        if not self.need_box(tag):
            return
        a = parse_args(rest)
        if a and isinstance(a[0], str) and a[0].upper() == "CHARSET":
            a = a[2:]
        with LOCK:
            msgs = list(enumerate(self.box.msgs))

            def val(x):
                return x.decode("utf-8", "replace") if isinstance(x, bytes) else x

            def crit(toks, i, m, seq):
                t = toks[i]
                if isinstance(t, list):
                    j = 0
                    ok = True
                    while j < len(t):
                        r, j = crit(t, j, m, seq)
                        ok = ok and r
                    return ok, i + 1
                u = t.upper()
                if u == "ALL":
                    return True, i + 1
                if u in ("SEEN", "UNSEEN", "FLAGGED", "UNFLAGGED", "ANSWERED", "DELETED", "UNDELETED", "DRAFT"):
                    neg = u.startswith("UN")
                    f = "\\" + (u[2:] if neg else u).capitalize()
                    return (f in m.flags) != neg, i + 1
                if u == "NOT":
                    r, j = crit(toks, i + 1, m, seq)
                    return not r, j
                if u == "OR":
                    r1, j = crit(toks, i + 1, m, seq)
                    r2, k = crit(toks, j, m, seq)
                    return r1 or r2, k
                if u in ("TEXT", "BODY"):
                    q = val(toks[i + 1]).lower()
                    return q in search_text(m), i + 2
                if u in ("FROM", "TO", "CC", "SUBJECT"):
                    q = val(toks[i + 1]).lower()
                    h = decode_words(header_fields(m.raw, [u]).decode("latin-1")).lower()
                    return q in h, i + 2
                if u == "HEADER":
                    q = val(toks[i + 2]).lower()
                    h = decode_words(header_fields(m.raw, [toks[i + 1]]).decode("latin-1")).lower()
                    return q in h, i + 3
                if u == "UID":
                    f = seq_set(toks[i + 1], self.box.msgs[-1].uid if self.box.msgs else 0)
                    return f(m.uid), i + 2
                if u == "MODSEQ":
                    return m.modseq > int(toks[i + 1]), i + 2
                if u in ("SINCE", "BEFORE", "ON", "SENTSINCE", "SENTBEFORE"):
                    when = time.mktime(time.strptime(toks[i + 1], "%d-%b-%Y"))
                    if "SINCE" in u:
                        return m.when >= when, i + 2
                    if "BEFORE" in u:
                        return m.when < when, i + 2
                    return when <= m.when < when + 86400, i + 2
                if u == "LARGER":
                    return len(m.raw) > int(toks[i + 1]), i + 2
                if u == "SMALLER":
                    return len(m.raw) < int(toks[i + 1]), i + 2
                if re.match(r"^[\d:*,]+$", u):
                    return seq_set(u, len(self.box.msgs))(seq), i + 1
                raise ValueError("unsupported search key %s" % t)

            found = []
            for idx, m in msgs:
                j = 0
                ok = True
                while j < len(a):
                    r, j = crit(a, j, m, idx + 1)
                    ok = ok and r
                if ok:
                    found.append(m.uid if uid else idx + 1)
        self.send("* SEARCH%s\r\n%s OK SEARCH done\r\n" % ("".join(" %d" % n for n in found), tag))

    def c_idle(self, tag, rest, uid):
        if not self.need_auth(tag):
            return
        self.send("+ idling\r\n")
        while True:
            with LOCK:
                self.updates()
            ln = self.readline(timeout=0.3)
            if ln is None:
                with LOCK:
                    CHANGED.wait(0.3)
                continue
            if ln.strip().upper() == b"DONE":
                break
        self.send("%s OK IDLE terminated\r\n" % tag)


# ---- SMTP --------------------------------------------------------------------------------------

class SmtpSession:
    def __init__(self, sock, tls_ctx, implicit, spool):
        self.sock = sock
        self.tls_ctx = tls_ctx
        self.tls = implicit
        self.buf = b""
        self.user = None
        self.spool = spool
        self.reset()

    def reset(self):
        self.mail_from = None
        self.rcpt = []

    def send(self, s):
        self.sock.sendall(s.encode() if isinstance(s, str) else s)

    def readline(self):
        while b"\r\n" not in self.buf:
            d = self.sock.recv(65536)
            if not d:
                raise Bye()
            self.buf += d
        ln, self.buf = self.buf.split(b"\r\n", 1)
        return ln

    def run(self):
        self.send("220 smtp.example.org ESMTP falso de P4OS\r\n")
        while True:
            ln = self.readline().decode("utf-8", "replace")
            log("smtp %s %s" % (self.user or "-", ln[:100] if not ln.upper().startswith("AUTH") else "AUTH ..."))
            verb = ln.split(" ", 1)[0].upper()
            arg = ln[len(verb) + 1:]
            if verb in ("EHLO", "HELO"):
                ext = ["smtp.example.org hola", "PIPELINING", "SIZE 26214400", "8BITMIME", "ENHANCEDSTATUSCODES"]
                if not self.tls and self.tls_ctx:
                    ext.append("STARTTLS")
                ext.append("AUTH PLAIN LOGIN")
                self.send("".join("250%s%s\r\n" % ("-" if i < len(ext) - 1 else " ", e) for i, e in enumerate(ext)))
            elif verb == "STARTTLS":
                if self.tls or not self.tls_ctx:
                    self.send("502 no\r\n")
                    continue
                self.send("220 2.0.0 Ready to start TLS\r\n")
                self.sock = self.tls_ctx.wrap_socket(self.sock, server_side=True)
                self.tls = True
                self.buf = b""
                self.user = None
            elif verb == "AUTH":
                a = arg.split()
                mech = a[0].upper()
                try:
                    if mech == "PLAIN":
                        if len(a) > 1:
                            data = a[1]
                        else:
                            self.send("334 \r\n")
                            data = self.readline().decode()
                        _, u, p = base64.b64decode(data).decode().split("\x00")
                    elif mech == "LOGIN":
                        self.send("334 VXNlcm5hbWU6\r\n")
                        u = base64.b64decode(self.readline()).decode()
                        self.send("334 UGFzc3dvcmQ6\r\n")
                        p = base64.b64decode(self.readline()).decode()
                    else:
                        self.send("504 5.5.4 unsupported\r\n")
                        continue
                except Exception:
                    self.send("501 5.5.2 bad auth data\r\n")
                    continue
                acc = ACCOUNTS.get(u.lower())
                if acc and acc.password == p:
                    self.user = acc.user
                    self.send("235 2.7.0 Authentication successful\r\n")
                else:
                    time.sleep(0.5)
                    self.send("535 5.7.8 Usuario o contraseña incorrectos\r\n")
            elif verb == "MAIL":
                if not self.user:
                    self.send("530 5.7.0 Authentication required\r\n")
                    continue
                m = re.search(r"<([^>]*)>", arg)
                self.mail_from = m.group(1) if m else ""
                self.rcpt = []
                self.send("250 2.1.0 OK\r\n")
            elif verb == "RCPT":
                if self.mail_from is None:
                    self.send("503 5.5.1 MAIL first\r\n")
                    continue
                m = re.search(r"<([^>]*)>", arg)
                to = m.group(1) if m else ""
                if "@" not in to:
                    self.send("553 5.1.3 bad address\r\n")
                    continue
                self.rcpt.append(to)
                self.send("250 2.1.5 OK\r\n")
            elif verb == "DATA":
                if not self.rcpt:
                    self.send("503 5.5.1 RCPT first\r\n")
                    continue
                self.send("354 End data with <CR><LF>.<CR><LF>\r\n")
                lines = []
                while True:
                    ln = self.readline()
                    if ln == b".":
                        break
                    if ln.startswith(b".."):
                        ln = ln[1:]
                    lines.append(ln)
                raw = b"\r\n".join(lines) + b"\r\n"
                self.deliver(raw)
                self.send("250 2.0.0 OK queued as %d\r\n" % random.randint(10000, 99999))
                self.reset()
            elif verb == "RSET":
                self.reset()
                self.send("250 2.0.0 OK\r\n")
            elif verb == "NOOP":
                self.send("250 2.0.0 OK\r\n")
            elif verb == "QUIT":
                self.send("221 2.0.0 Bye\r\n")
                raise Bye()
            else:
                self.send("502 5.5.2 unknown command\r\n")

    def deliver(self, raw):
        import email
        subj = decode_words(str(email.message_from_bytes(header_text(raw)).get("Subject", "")))
        log("smtp: %s -> %s: '%s' (%d bytes)" % (self.mail_from, ", ".join(self.rcpt), subj, len(raw)))
        received = "Received: from p4os by smtp.example.org; %s\r\n" % formatdate(localtime=True)
        with LOCK:
            for to in self.rcpt:
                acc = ACCOUNTS.get(to.lower())
                if acc:
                    acc.boxes["INBOX"].add(received.encode() + raw)
            CHANGED.notify_all()
        if self.spool:
            os.makedirs(self.spool, exist_ok=True)
            fn = os.path.join(self.spool, "%d-%04d.eml" % (int(time.time()), random.randint(0, 9999)))
            with open(fn, "wb") as f:
                f.write(raw)


# ---- the listeners -----------------------------------------------------------------------------

def serve(host, port, make, tls_ctx, implicit, label):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(16)
    log("%s on %s:%d" % (label, host, port))

    def one(c, addr):
        try:
            if implicit:
                c = tls_ctx.wrap_socket(c, server_side=True)
            make(c).run()
        except (Bye, ConnectionError, ssl.SSLError, OSError) as e:
            if not isinstance(e, Bye):
                log("%s: %s closed (%s)" % (label, addr[0], e))
        finally:
            try:
                c.close()
            except OSError:
                pass

    def loop():
        while True:
            c, addr = s.accept()
            threading.Thread(target=one, args=(c, addr), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()


def make_certs(d):
    """a throwaway authority, and a certificate for 127.0.0.1 and localhost"""
    os.makedirs(d, exist_ok=True)
    ca, cak = os.path.join(d, "ca.pem"), os.path.join(d, "ca.key")
    crt, key = os.path.join(d, "server.pem"), os.path.join(d, "server.key")
    if not os.path.exists(crt):
        def run(*a):
            subprocess.run(a, check=True, capture_output=True)
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "30", "-keyout", cak, "-out", ca,
            "-subj", "/CN=P4OS fake mail CA", "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        csr = os.path.join(d, "server.csr")
        run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr, "-subj", "/CN=127.0.0.1")
        ext = os.path.join(d, "ext.cnf")
        with open(ext, "w") as f:
            # mbedtls compares the name it was given with the dNSNames as text, so
            # the address goes there too, as well as an iPAddress
            f.write("subjectAltName=IP:127.0.0.1,DNS:127.0.0.1,DNS:localhost\nbasicConstraints=CA:FALSE\n"
                    "extendedKeyUsage=serverAuth\n")
        run("openssl", "x509", "-req", "-in", csr, "-CA", ca, "-CAkey", cak, "-CAcreateserial", "-out", crt,
            "-days", "30", "-extfile", ext)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    return ctx


def main():
    global QUIET
    ap = argparse.ArgumentParser(description="fake IMAP/SMTP server for P4OS's Correo")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--base", type=int, default=10000)
    ap.add_argument("--certs", help="folder for a throwaway CA and certificate (enables TLS)")
    ap.add_argument("--drip", type=float, default=0, help="a new message for Ana every N seconds")
    ap.add_argument("--spool", help="keep every message sent through SMTP in this folder")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    QUIET = a.quiet

    ACCOUNTS["ana@example.com"] = Account("ana@example.com", "prueba-ana", "Ana Ferreyra")
    ACCOUNTS["bruno@example.net"] = Account("bruno@example.net", "prueba-bruno", "Bruno Iturralde")
    fill_ana(ACCOUNTS["ana@example.com"])
    fill_bruno(ACCOUNTS["bruno@example.net"])

    ctx = make_certs(a.certs) if a.certs else None
    b = a.base
    serve(a.host, b + 143, lambda c: ImapSession(c, ctx, False), ctx, False, "IMAP")
    serve(a.host, b + 587, lambda c: SmtpSession(c, ctx, False, a.spool), ctx, False, "SMTP")
    if ctx:
        serve(a.host, b + 993, lambda c: ImapSession(c, ctx, True), ctx, True, "IMAPS")
        serve(a.host, b + 465, lambda c: SmtpSession(c, ctx, True, a.spool), ctx, True, "SMTPS")
        log("TLS: run the simulator with P4_SIM_EXTRA_CA=%s" % os.path.join(os.path.abspath(a.certs), "ca.pem"))
    i = 0
    try:
        while True:
            time.sleep(a.drip if a.drip > 0 else 3600)
            if a.drip > 0:
                drip_one(i)
                i += 1
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
