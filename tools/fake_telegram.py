#!/usr/bin/env python3
"""
A make-believe Telegram, for the Mensajes app (apps/mensajes): it speaks
MTProto 2.0 for real, with an account and its chats made up.

    tools/fake_telegram.py [--port 9443] [--bind 127.0.0.1] [--lang es|en]
                           [--client-config <data>/mensajes/servidor.txt]
                           [--password pescado | --no-password]
                           [--chatter 20] [--control 9450] [--state <file>] [--quiet]

Three data centres, DC n on port+n-1. The account lives on DC 1 and some of
its photos on DC 3, so a client that starts on DC 2 (as Mensajes does) meets
PHONE_MIGRATE_1, and then exportAuthorization / importAuthorization and
FILE_MIGRATE_3 for those photos. What it does:

  - the transport: intermediate (0xeeeeeeee) and abridged (0xef);
  - the auth key the long way: req_pq_multi, a real pq, RSA_PAD with a key
    of its own (made once, kept in ~/.cache/p4os_fake_telegram.json; its
    public half goes to --client-config so the app trusts it), DH with
    Telegram's prime;
  - sealed messages both ways, msg_key checked, salts (the first message
    always gets bad_server_salt), new_session_created, acks, containers,
    gzip_packed for the big answers, the client's clock checked
    (bad_msg_notification 16/17);
  - signing in: sendCode (the code is 12345, "sent to the app"), signIn,
    SESSION_PASSWORD_NEEDED, account.getPassword and checkPassword with
    SRP (the password is "pescado" unless --no-password), logOut;
  - the chats: getDialogs (pinned first), getHistory, sending text and
    photos (the parts uploaded), editing, deleting, forwarding, reactions,
    marking read, searching, getFile for photos, avatars, stickers' thumbs,
    a voice note and a document;
  - updates: a contact answers what you write to her (typing first), and
    with --chatter N someone writes every N seconds; updates.getState and
    getDifference with the box's pts.

The control port (--control) takes lines, for the tests:

    say <user|chat|channel> <id> <text>   a message arrives there
    gap                                   the next update skips a pts (getDifference)
    revoke                                the session is closed from elsewhere
    list                                  the peers and their ids

Nothing here is Telegram's: the keys, the people, the photos (drawn with
Pillow when it is installed, plain colours when not) are made up. The api_id
it accepts is 12345 with the api_hash 00112233445566778899aabbccddeeff
(--api-id / --api-hash), test values that mean nothing anywhere else.
"""

import argparse
import gzip
import io
import json
import os
import random
import socket
import socketserver
import struct
import sys
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'apps', 'mensajes', 'tools'))
import mtproto_ref as R  # noqa: E402
import tl_schema  # noqa: E402

SCHEMA = tl_schema.Schema.load(os.path.join(HERE, '..', 'apps', 'mensajes', 'tools', 'schema', 'layer234.tl'))
TL = tl_schema.TLCodec(SCHEMA)
LOCK = threading.RLock()
ARGS = None


def log(*a):
    if not ARGS.quiet:
        print(time.strftime('%H:%M:%S'), *a, flush=True)


# ---------------------------------------------------------------- the RSA key

def is_prime(n, k=24):
    if n < 4:
        return n in (2, 3)
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29):
        if n % p == 0:
            return n == p
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for _ in range(k):
        a = random.randrange(2, n - 2)
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def rand_prime(bits):
    while True:
        p = random.getrandbits(bits) | (1 << (bits - 1)) | 1
        if is_prime(p):
            return p


def load_key(path):
    try:
        with open(path) as f:
            k = json.load(f)
        return int(k['n'], 16), int(k['d'], 16)
    except (OSError, ValueError, KeyError):
        pass
    log('making an RSA key of 2048 bits (once)...')
    e = 65537
    while True:
        p, q = rand_prime(1024), rand_prime(1024)
        n = p * q
        phi = (p - 1) * (q - 1)
        if n.bit_length() == 2048 and phi % e:
            break
    d = pow(e, -1, phi)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        json.dump({'n': '%x' % n, 'd': '%x' % d}, f)
    return n, d


# ---------------------------------------------------------------- pictures

def jpeg(w, h, draw):
    """A JPEG of w x h drawn by draw(img, d), or a plain one without Pillow."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return SOLID_JPEG
    img = Image.new('RGB', (w, h), (40, 40, 60))
    draw(img, ImageDraw.Draw(img))
    b = io.BytesIO()
    img.save(b, 'JPEG', quality=85)
    return b.getvalue()


# an 8x8 grey JPEG, for machines without Pillow
SOLID_JPEG = bytes.fromhex(
    'ffd8ffe000104a46494600010100000100010000ffdb004300080606070605080707070909080a0c140d0c0b0b0c1912130f141d1a1f1e1d1a1c'
    '1c20242e2720222c231c1c2837292c30313434341f27393d38323c2e333432ffc0000b080008000801011100ffc4001f0000010501010101010100'
    '000000000000000102030405060708090a0bffc400b5100002010303020403050504040000017d01020300041105122131410613516107227114'
    '328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465'
    '666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9ca'
    'd2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffda0008010100003f00fbfcffd9')


def avatar(kind, rgb, hair=(60, 40, 30), skin=(240, 200, 170)):
    """A made-up profile picture: a person (head, hair, shoulders) on a
    coloured background, or for a group one of a few simple drawings."""
    import math

    def d(img, g):
        w, h = img.size
        for i in range(h):
            k = i / h
            g.line([0, i, w, i], fill=tuple(int(c * (1 - 0.4 * k) + 40 * k) for c in rgb))
        if kind == 'person':
            g.ellipse([w * 0.18, h * 0.66, w * 0.82, h * 1.3], fill=tuple(max(0, c - 70) for c in rgb))
            g.ellipse([w * 0.3, h * 0.2, w * 0.7, h * 0.62], fill=skin)
            g.chord([w * 0.27, h * 0.14, w * 0.73, h * 0.5], 180, 360, fill=hair)
            g.ellipse([w * 0.4, h * 0.38, w * 0.45, h * 0.43], fill=(40, 40, 40))
            g.ellipse([w * 0.55, h * 0.38, w * 0.6, h * 0.43], fill=(40, 40, 40))
            g.arc([w * 0.42, h * 0.42, w * 0.58, h * 0.54], 20, 160, fill=(150, 60, 60), width=3)
        elif kind == 'house':
            g.polygon([(w * 0.2, h * 0.5), (w * 0.5, h * 0.22), (w * 0.8, h * 0.5)], fill=(250, 240, 230))
            g.rectangle([w * 0.28, h * 0.5, w * 0.72, h * 0.8], fill=(250, 240, 230))
            g.rectangle([w * 0.45, h * 0.6, w * 0.56, h * 0.8], fill=tuple(max(0, c - 60) for c in rgb))
        elif kind == 'ball':
            g.ellipse([w * 0.22, h * 0.22, w * 0.78, h * 0.78], fill=(250, 250, 250))
            g.regular_polygon((w * 0.5, h * 0.5, w * 0.11), 5, fill=(30, 30, 30))
            for a in range(5):
                x = w * 0.5 + math.cos(a * 1.2566 - 1.57) * w * 0.22
                y = h * 0.5 + math.sin(a * 1.2566 - 1.57) * h * 0.22
                g.ellipse([x - w * 0.05, y - h * 0.05, x + w * 0.05, y + h * 0.05], fill=(30, 30, 30))
        elif kind == 'board':
            g.rectangle([w * 0.22, h * 0.3, w * 0.78, h * 0.7], fill=(20, 110, 60))
            g.rectangle([w * 0.38, h * 0.4, w * 0.62, h * 0.6], fill=(30, 30, 30))
            for i in range(6):
                g.rectangle([w * (0.26 + i * 0.085), h * 0.32, w * (0.29 + i * 0.085), h * 0.36], fill=(220, 190, 90))
    return jpeg(160, 160, d)


def scene(kind, w, h):
    def d(img, g):
        if kind == 'sunset':
            for y in range(h):
                k = y / h
                g.line([0, y, w, y], fill=(int(250 - 90 * k), int(140 - 90 * k), int(60 + 80 * k)))
            g.ellipse([w * 0.55, h * 0.35, w * 0.8, h * 0.35 + w * 0.25], fill=(255, 220, 120))
            g.rectangle([0, h * 0.72, w, h], fill=(30, 40, 70))
            for i in range(0, w, max(4, w // 40)):
                g.line([i, h * 0.74 + (i % 7), i + w // 60, h * 0.74 + (i % 7)], fill=(250, 190, 120))
        elif kind == 'mountains':
            for y in range(h):
                k = y / h
                g.line([0, y, w, y], fill=(int(120 + 100 * k), int(180 + 50 * k), 240))
            g.polygon([(0, h), (w * 0.3, h * 0.3), (w * 0.55, h)], fill=(70, 90, 110))
            g.polygon([(w * 0.3, h), (w * 0.65, h * 0.2), (w, h)], fill=(50, 70, 90))
            g.polygon([(w * 0.58, h * 0.32), (w * 0.65, h * 0.2), (w * 0.72, h * 0.32)], fill=(240, 245, 250))
            g.rectangle([0, h * 0.85, w, h], fill=(60, 120, 60))
        elif kind == 'cake':
            g.rectangle([0, 0, w, h], fill=(250, 230, 235))
            g.rectangle([w * 0.25, h * 0.45, w * 0.75, h * 0.8], fill=(200, 120, 90))
            g.rectangle([w * 0.25, h * 0.45, w * 0.75, h * 0.52], fill=(255, 250, 250))
            for i in range(5):
                x = w * (0.32 + i * 0.09)
                g.rectangle([x, h * 0.3, x + w * 0.02, h * 0.45], fill=(120, 170, 250))
                g.ellipse([x - w * 0.005, h * 0.24, x + w * 0.025, h * 0.3], fill=(255, 200, 60))
        elif kind == 'board':
            g.rectangle([0, 0, w, h], fill=(20, 60, 40))
            g.rectangle([w * 0.12, h * 0.2, w * 0.88, h * 0.8], fill=(10, 90, 50))
            for i in range(12):
                x = w * (0.16 + i * 0.06)
                g.rectangle([x, h * 0.25, x + w * 0.03, h * 0.3], fill=(220, 200, 100))
            g.rectangle([w * 0.35, h * 0.38, w * 0.65, h * 0.62], fill=(30, 30, 30))
            g.rectangle([w * 0.37, h * 0.4, w * 0.63, h * 0.6], fill=(80, 160, 230))
        elif kind == 'cat':
            g.rectangle([0, 0, w, h], fill=(255, 255, 255))
            g.ellipse([w * 0.15, h * 0.2, w * 0.85, h * 0.9], fill=(250, 190, 80))
            g.polygon([(w * 0.2, h * 0.35), (w * 0.25, h * 0.05), (w * 0.45, h * 0.25)], fill=(250, 190, 80))
            g.polygon([(w * 0.8, h * 0.35), (w * 0.75, h * 0.05), (w * 0.55, h * 0.25)], fill=(250, 190, 80))
            g.ellipse([w * 0.32, h * 0.45, w * 0.42, h * 0.58], fill=(40, 40, 40))
            g.ellipse([w * 0.58, h * 0.45, w * 0.68, h * 0.58], fill=(40, 40, 40))
            g.arc([w * 0.4, h * 0.6, w * 0.6, h * 0.75], 20, 160, fill=(40, 40, 40), width=3)
        else:
            g.rectangle([0, 0, w, h], fill=(90, 90, 120))
    return jpeg(w, h, d)


# ---------------------------------------------------------------- the account

TEXTS = {
    # key: (es, en)
    'self_first': ('Lucía', 'Lucy'),
    'self_last': ('Prueba', 'Test'),
    'family': ('Familia', 'Family'),
    'football': ('Fútbol de los jueves', 'Thursday football'),
    'news': ('Novedades de la placa', 'Board news'),
    'm1': ('¿Vienen el domingo a almorzar? Hago ñoquis 🥟', 'Are you coming for lunch on Sunday? I am making gnocchi 🥟'),
    'm2': ('¡Sí! Llevo el postre 🍰', 'Yes! I will bring dessert 🍰'),
    'm3': ('Miren lo que hice ayer', 'Look what I made yesterday'),
    'm4': ('¡Qué rico se ve! 😍', 'That looks delicious! 😍'),
    'm5': ('Abuela, ¿te llegó la foto?', 'Grandma, did you get the photo?'),
    'm6': ('Sí querida, ¡qué lindo atardecer! ¿Dónde fue?', 'Yes dear, what a lovely sunset! Where was it?'),
    'm7': ('En la costanera, el sábado', 'By the river, on Saturday'),
    'm8': ('Mañana a las 20 en la cancha de siempre ⚽', 'Tomorrow at 8 pm at the usual pitch ⚽'),
    'm9': ('Confirmo. ¿Quién lleva la pelota?', 'I am in. Who brings the ball?'),
    'm10': ('Yo, y los petos', 'Me, and the bibs'),
    'm11': ('Somos 9, falta uno 🙏', 'We are 9, one short 🙏'),
    'm12': ('¿Me pasás el presupuesto?', 'Can you send me the quote?'),
    'm13': ('Ahí va', 'Here it is'),
    'm14': ('Gracias, lo miro esta noche 👍', 'Thanks, I will look at it tonight 👍'),
    'm15': ('Te dejé un audio', 'I left you a voice note'),
    'm16': ('Salió la 0.34: fotos de la cámara, la Tienda más rápida y un montón de arreglos.',
            'Version 0.34 is out: camera photos, a faster Store and lots of fixes.'),
    'm17': ('La placa ahora manda y recibe mensajes 📨', 'The board now sends and receives messages 📨'),
    'm18': ('¿Viste el partido anoche?', 'Did you watch the match last night?'),
    'm19': ('¡Increíble el segundo tiempo!', 'The second half was amazing!'),
    'm20': ('Hola! Te escribo por lo del alquiler de la sala', 'Hi! I am writing about renting the room'),
    'm21': ('Buenísimo, ¿qué día te sirve?', 'Great, which day works for you?'),
    'm22': ('El jueves a la tarde', 'Thursday afternoon'),
    'reply': ('¡Dale! Hablamos más tarde 😊', 'Sure! Talk to you later 😊'),
    'reply2': ('Jaja, sí 😄', 'Haha, yes 😄'),
    'chatter': ('¿Cómo va todo por ahí?', 'How is everything over there?'),
    'chatter2': ('Mañana llueve, llevá paraguas ☔', 'It will rain tomorrow, take an umbrella ☔'),
    'joined': ('', ''),
}


def T(k):
    return TEXTS[k][1 if ARGS.lang == 'en' else 0]


class Store:
    """The account and everything in it, shared by the three data centres."""

    def __init__(self):
        self.now = int(time.time())
        self.self_id = 1000001
        self.users = {}
        self.chats = {}            # basic groups
        self.channels = {}         # supergroups and channels
        self.box = {}              # peer key -> {id: message dict}, users and basic groups share ids
        self.chan_box = {}         # channel id -> {id: message}
        self.next_id = 1
        self.chan_next = {}
        self.pts = 1
        self.chan_pts = {}
        self.qts = 0
        self.seq = 0
        self.log = []              # (pts, pts_count, update) of the common box
        self.files = {}            # (kind, id, thumb) -> bytes
        self.photos = {}           # photo id -> photo dict
        self.photo_dc = {}         # photo id -> where its bytes are
        self.uploads = {}          # file_id -> {part: bytes}
        self.pinned = []
        self.unread = {}
        self.read_out = {}
        self.read_in = {}
        self.access = 0x5A5A0000
        self.gap_next = False
        self.make()

    # -- making things
    def user(self, uid, first, last='', photo=None, rgb=(90, 120, 200), bot=False, online=False, hair=(60, 40, 30)):
        u = {'_': 'user', 'id': uid, 'access_hash': self.access + uid, 'first_name': first}
        if last:
            u['last_name'] = last
        if bot:
            u['bot'] = True
            u['bot_info_version'] = 1
        if photo:
            pid = 7100000 + uid
            u['photo'] = {'_': 'userProfilePhoto', 'photo_id': pid, 'dc_id': 1}
            self.files[('avatar', pid)] = avatar('person', rgb, hair)
        u['status'] = ({'_': 'userStatusOnline', 'expires': self.now + 300} if online
                       else {'_': 'userStatusOffline', 'was_online': self.now - 3600})
        self.users[uid] = u
        return u

    def photo(self, kind, dc=1, real_dc=None):
        """real_dc: where the bytes really are, when the photo says another
        data centre (FILE_MIGRATE_X for whoever asks the one it says)."""
        pid = 8000000 + len(self.photos)
        self.photo_dc[pid] = real_dc or dc
        sizes = []
        for t, (w, h) in (('m', (320, 240)), ('x', (800, 600))):
            b = scene(kind, w, h)
            self.files[('photo', pid, t)] = b
            sizes.append({'_': 'photoSize', 'type': t, 'w': w, 'h': h, 'size': len(b)})
        p = {'_': 'photo', 'id': pid, 'access_hash': self.access + pid, 'file_reference': b'\x01ref' + bytes([pid % 256]),
             'date': self.now - 86400, 'sizes': sizes, 'dc_id': dc}
        self.photos[pid] = p
        return p

    def photo_from_bytes(self, data):
        pid = 8000000 + len(self.photos)
        w, h = 800, 600
        try:
            from PIL import Image
            im = Image.open(io.BytesIO(data))
            w, h = im.size
            m = im.copy()
            m.thumbnail((320, 320))
            b = io.BytesIO()
            m.convert('RGB').save(b, 'JPEG', quality=85)
            small = b.getvalue()
            mw, mh = m.size
        except Exception:  # noqa: BLE001
            small, mw, mh = data, w, h
        self.files[('photo', pid, 'x')] = data
        self.files[('photo', pid, 'm')] = small
        self.photo_dc[pid] = 1
        p = {'_': 'photo', 'id': pid, 'access_hash': self.access + pid, 'file_reference': b'\x01up',
             'date': int(time.time()), 'dc_id': 1,
             'sizes': [{'_': 'photoSize', 'type': 'm', 'w': mw, 'h': mh, 'size': len(small)},
                       {'_': 'photoSize', 'type': 'x', 'w': w, 'h': h, 'size': len(data)}]}
        self.photos[pid] = p
        return p

    def document(self, did, mime, data, attrs, thumb=None, dc=1):
        d = {'_': 'document', 'id': did, 'access_hash': self.access + did, 'file_reference': b'\x02ref',
             'date': self.now - 7200, 'mime_type': mime, 'size': len(data), 'dc_id': dc, 'attributes': attrs}
        self.files[('doc', did, '')] = data
        if thumb:
            self.files[('doc', did, 'm')] = thumb
            d['thumbs'] = [{'_': 'photoSize', 'type': 'm', 'w': 128, 'h': 128, 'size': len(thumb)}]
        return d

    def peer(self, kind, pid):
        return {'_': {'user': 'peerUser', 'chat': 'peerChat', 'channel': 'peerChannel'}[kind],
                {'user': 'user_id', 'chat': 'chat_id', 'channel': 'channel_id'}[kind]: pid}

    def add(self, kind, pid, text, out=False, frm=None, ago=0, media=None, reply=None, extra=None, count=True):
        """A message in a chat; returns it. The common box (users, groups)
        moves pts, a channel its own."""
        date = int(time.time()) - ago
        if kind == 'channel':
            n = self.chan_next.get(pid, 1)
            self.chan_next[pid] = n + 1
            mid = n
            box = self.chan_box.setdefault(pid, {})
        else:
            mid = self.next_id
            self.next_id += 1
            box = self.box.setdefault((kind, pid), {})
        m = {'_': 'message', 'id': mid, 'peer_id': self.peer(kind, pid), 'date': date, 'message': text}
        if out:
            m['out'] = True
            m['from_id'] = self.peer('user', self.self_id)
        elif kind != 'user' or frm:
            m['from_id'] = self.peer('user', frm or pid)
        if kind == 'channel' and self.channels[pid].get('broadcast'):
            m.pop('from_id', None)
            m['post'] = True
            m['views'] = 120 + mid * 7
            m['forwards'] = 3
        if media:
            m['media'] = media
        if reply:
            m['reply_to'] = {'_': 'messageReplyHeader', 'reply_to_msg_id': reply}
        if extra:
            m.update(extra)
        box[mid] = m
        if count and not out and kind != 'channel':
            self.unread[(kind, pid)] = self.unread.get((kind, pid), 0) + 1
        return m

    def make(self):
        U = self.user
        U(self.self_id, T('self_first'), T('self_last'), True, (120, 90, 200), online=True, hair=(150, 80, 40))
        self.users[self.self_id]['self'] = True
        self.users[self.self_id]['phone'] = ARGS.phone.lstrip('+')
        U(2001, 'Martín', 'Gómez', True, (60, 140, 210), online=True, hair=(40, 30, 20))
        U(2002, 'Rosa', 'Abuela' if ARGS.lang == 'es' else 'Grandma', True, (220, 110, 120), hair=(210, 210, 215))
        U(2003, 'Caro', '', True, (70, 170, 120), hair=(170, 60, 30))
        U(2004, 'Diego', 'Fernández', None)
        U(2005, 'Paula', 'Ríos', True, (230, 160, 60), hair=(30, 20, 20))
        U(2006, 'Tomás', '', None)
        self.chats[3001] = {'_': 'chat', 'id': 3001, 'title': T('family'), 'participants_count': 5,
                            'date': self.now - 900000, 'version': 1,
                            'photo': {'_': 'chatPhoto', 'photo_id': 7310001, 'dc_id': 1}}
        self.files[('avatar', 7310001)] = avatar('house', (200, 90, 160))
        self.channels[4001] = {'_': 'channel', 'id': 4001, 'access_hash': self.access + 4001, 'title': T('football'),
                               'megagroup': True, 'date': self.now - 500000,
                               'photo': {'_': 'chatPhoto', 'photo_id': 7410001, 'dc_id': 1}}
        self.files[('avatar', 7410001)] = avatar('ball', (40, 150, 90))
        self.channels[4002] = {'_': 'channel', 'id': 4002, 'access_hash': self.access + 4002, 'title': T('news'),
                               'broadcast': True, 'date': self.now - 400000, 'username': 'p4os_news',
                               'photo': {'_': 'chatPhoto', 'photo_id': 7410002, 'dc_id': 1}}
        self.files[('avatar', 7410002)] = avatar('board', (30, 60, 110))

        H = 3600
        a = self.add
        # the board's news, a channel
        a('channel', 4002, T('m16'), ago=30 * H, count=False)
        a('channel', 4002, T('m17'), ago=5 * H, media={'_': 'messageMediaPhoto', 'photo': self.photo('board', dc=1, real_dc=3)},
          count=False)
        # Diego, a document
        a('user', 2004, T('m12'), out=True, ago=50 * H, count=False)
        pdf = (b'%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj 2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj'
               b' 3 0 obj<</Type/Page/MediaBox[0 0 300 144]/Parent 2 0 R>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n')
        doc = self.document(9100001, 'application/pdf', pdf * 40,
                            [{'_': 'documentAttributeFilename', 'file_name': 'presupuesto.pdf'}])
        a('user', 2004, T('m13'), ago=49 * H, media={'_': 'messageMediaDocument', 'document': doc}, count=False)
        a('user', 2004, T('m14'), out=True, ago=48 * H, count=False)
        # Paula
        a('user', 2005, T('m20'), ago=26 * H, count=False)
        a('user', 2005, T('m21'), out=True, ago=25 * H, count=False)
        a('user', 2005, T('m22'), ago=24 * H)
        # Tomás, a voice note and a sticker
        a('user', 2006, T('m18'), out=True, ago=8 * H, count=False)
        voice = self.document(9100002, 'audio/ogg', b'OggS' + os.urandom(9000),
                              [{'_': 'documentAttributeAudio', 'voice': True, 'duration': 7,
                                'waveform': bytes(range(0, 63, 2))}])
        a('user', 2006, T('m15'), ago=7 * H, media={'_': 'messageMediaDocument', 'document': voice, 'voice': True})
        stk = self.document(9100003, 'image/webp', b'RIFF' + os.urandom(3000),
                            [{'_': 'documentAttributeSticker', 'alt': '😺',
                              'stickerset': {'_': 'inputStickerSetEmpty'}},
                             {'_': 'documentAttributeImageSize', 'w': 512, 'h': 512}],
                            thumb=scene('cat', 128, 128))
        a('user', 2006, '', ago=7 * H - 60, media={'_': 'messageMediaDocument', 'document': stk})
        # football, a supergroup
        a('channel', 4001, T('m8'), frm=2001, ago=6 * H, count=False)
        svc = {'_': 'messageService', 'id': 0, 'peer_id': self.peer('channel', 4001),
               'from_id': self.peer('user', 2004), 'date': int(time.time()) - 5 * H,
               'action': {'_': 'messageActionChatJoinedByLink', 'inviter_id': 2001}}
        n = self.chan_next.get(4001, 1)
        self.chan_next[4001] = n + 1
        svc['id'] = n
        self.chan_box[4001][n] = svc
        m9 = a('channel', 4001, T('m9'), frm=2004, ago=5 * H, count=False)
        a('channel', 4001, T('m10'), out=True, ago=4 * H, reply=m9['id'], count=False)
        a('channel', 4001, T('m11'), frm=2001, ago=2 * H,
          extra={'reactions': {'_': 'messageReactions', 'results': [
              {'_': 'reactionCount', 'reaction': {'_': 'reactionEmoji', 'emoticon': '👍'}, 'count': 4},
              {'_': 'reactionCount', 'reaction': {'_': 'reactionEmoji', 'emoticon': '🔥'}, 'count': 1}]}})
        self.chan_unread = {4001: 2, 4002: 1}
        # grandma, with a photo from the self
        a('user', 2002, T('m5'), out=True, ago=3 * H, media={'_': 'messageMediaPhoto', 'photo': self.photo('sunset')},
          count=False)
        a('user', 2002, T('m6'), ago=2 * H + 600, count=False)
        a('user', 2002, T('m7'), out=True, ago=2 * H, count=False)
        # family, a basic group with a photo and reactions
        a('chat', 3001, T('m1'), frm=2002, ago=90 * 60, count=False)
        m2 = a('chat', 3001, T('m2'), out=True, ago=80 * 60, count=False)
        a('chat', 3001, T('m3'), frm=2003, ago=40 * 60, media={'_': 'messageMediaPhoto', 'photo': self.photo('cake', dc=3)},
          extra={'reactions': {'_': 'messageReactions', 'results': [
              {'_': 'reactionCount', 'reaction': {'_': 'reactionEmoji', 'emoticon': '❤️'}, 'count': 3,
               'chosen_order': 0},
              {'_': 'reactionCount', 'reaction': {'_': 'reactionEmoji', 'emoticon': '😋'}, 'count': 1}]}})
        a('chat', 3001, T('m4'), frm=2002, ago=30 * 60, reply=m2['id'] + 1)
        # Martín, edited and forwarded
        a('user', 2001, T('m19'), ago=20 * 60, count=False, extra={'edit_date': int(time.time()) - 19 * 60})
        fwd = {'_': 'messageFwdHeader', 'from_id': self.peer('channel', 4002), 'date': int(time.time()) - 5 * H,
               'channel_post': 2}
        a('user', 2001, T('m17'), out=True, ago=15 * 60, count=False, extra={'fwd_from': fwd})
        a('user', 2001, T('reply2'), ago=10 * 60)
        # Caro
        a('user', 2003, T('chatter2'), ago=5 * 60)
        self.pinned = [('chat', 3001), ('user', 2002)]
        self.read_out = {('user', 2001): 10 ** 6, ('user', 2002): 10 ** 6}
        self.pts = 100 + self.next_id

    # -- views
    def peer_of(self, p):
        if p['_'] == 'peerUser':
            return 'user', p['user_id']
        if p['_'] == 'peerChat':
            return 'chat', p['chat_id']
        return 'channel', p['channel_id']

    def input_peer(self, ip):
        k = ip['_']
        if k == 'inputPeerSelf':
            return 'user', self.self_id
        if k == 'inputPeerUser':
            return 'user', ip['user_id']
        if k == 'inputPeerChat':
            return 'chat', ip['chat_id']
        if k == 'inputPeerChannel':
            return 'channel', ip['channel_id']
        if k == 'inputChannel':
            return 'channel', ip['channel_id']
        return None, 0

    def msgs_of(self, kind, pid):
        if kind == 'channel':
            return self.chan_box.get(pid, {})
        return self.box.get((kind, pid), {})

    def all_chats(self):
        return [self.chats[c] for c in self.chats] + [self.channels[c] for c in self.channels]

    def all_users(self):
        return list(self.users.values())

    def dialogs(self):
        out = []
        keys = [('user', u) for u in self.users if u != self.self_id and self.box.get(('user', u))]
        keys += [('chat', c) for c in self.chats] + [('channel', c) for c in self.channels]
        tops = []
        for k in keys:
            ms = self.msgs_of(*k)
            if not ms:
                continue
            top = ms[max(ms)]
            tops.append((k in self.pinned, -self.pinned.index(k) if k in self.pinned else 0, top['date'], k, top))
        tops.sort(key=lambda t: (t[0], t[1], t[2]), reverse=True)
        for pinned, _, _, k, top in tops:
            unread = self.chan_unread.get(k[1], 0) if k[0] == 'channel' else self.unread.get(k, 0)
            d = {'_': 'dialog', 'peer': self.peer(*k), 'top_message': top['id'],
                 'read_inbox_max_id': self.read_in.get(k, top['id'] - unread if unread else top['id']),
                 'read_outbox_max_id': min(self.read_out.get(k, 0), top['id']),
                 'unread_count': unread, 'unread_mentions_count': 0, 'unread_reactions_count': 0,
                 'unread_poll_votes_count': 0,
                 'notify_settings': {'_': 'peerNotifySettings'}}
            if pinned:
                d['pinned'] = True
            if k[0] == 'channel':
                d['pts'] = self.chan_pts.get(k[1], 50)
            out.append((d, top))
        return out

    # -- updates of the common box
    def bump(self, update, count=1):
        self.pts += count
        if self.gap_next:
            self.pts += 1
            self.gap_next = False
        update['pts'] = self.pts
        update['pts_count'] = count
        self.log.append((self.pts, count, update))
        return update


STORE = None


# ---------------------------------------------------------------- the server

class Key:
    def __init__(self, key, dc):
        self.key = key
        self.id = R.auth_key_id(key)
        self.dc = dc
        self.user = None
        self.salt = struct.unpack('<q', os.urandom(8))[0]
        self.sessions = set()
        self.layer_ok = False


KEYS = {}
EXPORTED = {}


def keys_save():
    """The auth keys and who signed in with each, so a restart of the fake
    does not send the app back to signing in (--state)."""
    if not ARGS.state:
        return
    with LOCK:
        data = [{'key': k.key.hex(), 'dc': k.dc, 'user': k.user, 'salt': k.salt} for k in KEYS.values()]
    tmp = ARGS.state + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(data, f)
    os.replace(tmp, ARGS.state)


def keys_load():
    if not ARGS.state:
        return
    try:
        with open(ARGS.state) as f:
            data = json.load(f)
    except (OSError, ValueError):
        return
    for d in data:
        k = Key(bytes.fromhex(d['key']), d['dc'])
        k.user = d.get('user')
        k.salt = d.get('salt', k.salt)
        KEYS[k.id] = k
    log(f'{len(data)} auth keys from {ARGS.state}')
CONNS = set()
PENDING_CODE = {}


class Conn(socketserver.BaseRequestHandler):
    def setup(self):
        self.dc = self.server.dc
        self.abridged = False
        self.key = None
        self.session = None
        self.seq = 0
        self.hs = {}
        self.alive = True
        self.wlock = threading.Lock()
        with LOCK:
            CONNS.add(self)

    def finish(self):
        self.alive = False
        with LOCK:
            CONNS.discard(self)

    # -- framing
    def read_exact(self, n):
        b = b''
        while len(b) < n:
            c = self.request.recv(n - len(b))
            if not c:
                raise EOFError
            b += c
        return b

    def read_packet(self):
        if self.abridged:
            n = self.read_exact(1)[0]
            if n == 0x7f:
                n = int.from_bytes(self.read_exact(3), 'little')
            return self.read_exact(n * 4)
        n = struct.unpack('<I', self.read_exact(4))[0]
        return self.read_exact(n)

    def write_packet(self, p):
        with self.wlock:
            if self.abridged:
                n = len(p) // 4
                head = bytes([n]) if n < 0x7f else b'\x7f' + n.to_bytes(3, 'little')
            else:
                head = struct.pack('<I', len(p))
            self.request.sendall(head + p)

    def handle(self):
        try:
            first = self.read_exact(1)
            if first == b'\xef':
                self.abridged = True
            else:
                rest = self.read_exact(3)
                if first + rest != b'\xee\xee\xee\xee':
                    log(f'dc{self.dc}: unknown transport {first + rest!r}')
                    return
            log(f'dc{self.dc}: a client connected ({"abridged" if self.abridged else "intermediate"})')
            while True:
                p = self.read_packet()
                if p[:8] == b'\0' * 8:
                    self.plain(p)
                else:
                    self.sealed(p)
        except (EOFError, ConnectionError, OSError):
            pass
        except Exception as e:  # noqa: BLE001
            log(f'dc{self.dc}: dropped: {e!r}')
            import traceback
            traceback.print_exc()

    # -- the handshake, unencrypted
    def send_plain(self, obj):
        body = TL.encode(obj)
        mid = self.server_msg_id()
        self.write_packet(b'\0' * 8 + struct.pack('<qI', mid, len(body)) + body)

    def server_msg_id(self, reply=True):
        t = time.time()
        mid = (int(t) << 32) | (int((t % 1) * 2 ** 32) & ~3)
        mid |= 1 if reply else 3
        last = getattr(self, '_last_mid', 0)
        if mid <= last:
            mid = last + 4
        self._last_mid = mid
        return mid

    def plain(self, p):
        n = struct.unpack('<I', p[16:20])[0]
        req = TL.decode(p[20:20 + n])
        k = req['_']
        if k in ('req_pq_multi', 'req_pq'):
            pp, qq = R_PRIMES()
            self.hs = {'nonce': req['nonce'], 'server_nonce': os.urandom(16), 'p': pp, 'q': qq}
            pq = (pp * qq).to_bytes(8, 'big')
            self.send_plain({'_': 'resPQ', 'nonce': req['nonce'], 'server_nonce': self.hs['server_nonce'],
                             'pq': pq, 'server_public_key_fingerprints': [FINGERPRINT]})
        elif k == 'req_DH_params':
            hs = self.hs
            if req['nonce'] != hs['nonce'] or req['server_nonce'] != hs['server_nonce']:
                log('handshake: wrong nonce')
                return
            if int.from_bytes(tl_schema.tb(req['p']), 'big') != hs['p'] or int.from_bytes(tl_schema.tb(req['q']), 'big') != hs['q']:
                log('handshake: wrong p and q')
                return
            if req['public_key_fingerprint'] != FINGERPRINT:
                log('handshake: unknown key')
                return
            data = R.rsa_pad_decrypt(tl_schema.tb(req['encrypted_data']), RSA_N, RSA_D)
            if data is None:
                log('handshake: RSA_PAD does not check')
                return
            inner = TL.decode(data)
            if inner['_'] != 'p_q_inner_data_dc' or inner['nonce'] != hs['nonce']:
                log('handshake: bad inner data', inner['_'])
                return
            hs['new_nonce'] = inner['new_nonce']
            hs['a'] = int.from_bytes(os.urandom(256), 'big')
            ga = pow(3, hs['a'], R.DH_PRIME)
            ans = TL.encode({'_': 'server_DH_inner_data', 'nonce': hs['nonce'], 'server_nonce': hs['server_nonce'],
                             'g': 3, 'dh_prime': R.DH_PRIME.to_bytes(256, 'big'), 'g_a': ga.to_bytes(256, 'big'),
                             'server_time': int(time.time())})
            awh = R.sha1(ans) + ans
            awh += os.urandom((-len(awh)) % 16)
            key, iv = R.tmp_aes(hs['new_nonce'], hs['server_nonce'])
            self.send_plain({'_': 'server_DH_params_ok', 'nonce': hs['nonce'], 'server_nonce': hs['server_nonce'],
                             'encrypted_answer': R.ige_encrypt(awh, key, iv)})
        elif k == 'set_client_DH_params':
            hs = self.hs
            key, iv = R.tmp_aes(hs['new_nonce'], hs['server_nonce'])
            dwh = R.ige_decrypt(tl_schema.tb(req['encrypted_data']), key, iv)
            inner = TL.decode(dwh[20:])
            body = TL.encode(dict(inner, g_b=tl_schema.tb(inner['g_b'])))
            if R.sha1(body) != dwh[:20]:
                log('handshake: client_DH_inner_data hash is wrong')
                return
            gb = int.from_bytes(tl_schema.tb(inner['g_b']), 'big')
            if not (1 < gb < R.DH_PRIME - 1):
                log('handshake: bad g_b')
                return
            ak = pow(gb, hs['a'], R.DH_PRIME).to_bytes(256, 'big')
            k = Key(ak, self.dc)
            salt = bytes(a ^ b for a, b in zip(hs['new_nonce'][:8], hs['server_nonce'][:8]))
            k.salt = struct.unpack('<q', salt)[0]
            with LOCK:
                KEYS[k.id] = k
            keys_save()
            self.send_plain({'_': 'dh_gen_ok', 'nonce': hs['nonce'], 'server_nonce': hs['server_nonce'],
                             'new_nonce_hash1': R.new_nonce_hash(hs['new_nonce'], ak, 1)})
            log(f'dc{self.dc}: an auth key made ({k.id.hex()})')
        else:
            log('handshake: unexpected', k)

    # -- sealed messages
    def sealed(self, p):
        kid = p[:8]
        with LOCK:
            k = KEYS.get(kid)
        if not k or k.dc != self.dc:
            log(f'dc{self.dc}: unknown auth key, -404')
            self.write_packet(struct.pack('<i', -404))
            return
        pt = R.open_sealed(k.key, p, 0)
        if pt is None:
            log(f'dc{self.dc}: msg_key does not check')
            return
        salt, session, mid, seqno, n = struct.unpack('<qqqII', pt[:32])
        body = pt[32:32 + n]
        self.key = k
        if self.session != session:
            self.session = session
            self.seq = 0
            if session not in k.sessions:
                k.sessions.add(session)
                self.send({'_': 'new_session_created', 'first_msg_id': mid, 'unique_id': random.getrandbits(63),
                           'server_salt': k.salt}, content=False)
        # the client's clock: 300 s either way
        t = mid >> 32
        if abs(t - time.time()) > 300:
            log(f'dc{self.dc}: the client\'s clock is off by {int(t - time.time())} s')
            self.send({'_': 'bad_msg_notification', 'bad_msg_id': mid, 'bad_msg_seqno': seqno,
                       'error_code': 16 if t < time.time() else 17}, content=False)
            return
        if salt != k.salt:
            self.send({'_': 'bad_server_salt', 'bad_msg_id': mid, 'bad_msg_seqno': seqno, 'error_code': 48,
                       'new_server_salt': k.salt}, content=False)
            return
        self.message(mid, seqno, body)

    def send(self, obj, content=True, raw=None):
        if not self.key or self.session is None:
            return
        body = raw if raw is not None else TL.encode(obj)
        if content:
            seqno = self.seq * 2 + 1
            self.seq += 1
        else:
            seqno = self.seq * 2
        mid = self.server_msg_id()
        pt = struct.pack('<qqqII', self.key.salt, self.session, mid, seqno, len(body)) + body
        pad = 12 + (-(len(pt) + 12)) % 16
        pt += os.urandom(pad)
        self.write_packet(R.seal(self.key.key, pt, 8))

    def message(self, mid, seqno, body):
        cid = struct.unpack('<I', body[:4])[0]
        if cid == 0x73f1f8dc:                      # msg_container
            n = struct.unpack('<I', body[4:8])[0]
            off = 8
            for _ in range(n):
                m2, s2, ln = struct.unpack('<qII', body[off:off + 16])
                self.message(m2, s2, body[off + 16:off + 16 + ln])
                off += 16 + ln
            return
        obj = TL.decode(body)
        k = obj['_']
        if k == 'msgs_ack':
            return
        if k in ('ping', 'ping_delay_disconnect'):
            self.send({'_': 'pong', 'msg_id': mid, 'ping_id': obj['ping_id']}, content=False)
            return
        if seqno & 1 and random.random() < 0.3:
            self.send({'_': 'msgs_ack', 'msg_ids': [mid]}, content=False)
        try:
            result = self.rpc(obj)
        except RpcError as e:
            result = {'_': 'rpc_error', 'error_code': e.code, 'error_message': e.msg}
            log(f'dc{self.dc}: {k} -> {e.code} {e.msg}')
        if result is None:
            return
        raw = TL.encode(result) if isinstance(result, dict) else result_bytes(result)
        if len(raw) > 600:
            raw = struct.pack('<I', 0x3072cfa1) + tl_schema.write_bytes(gzip.compress(raw, mtime=0))
        self.send(None, raw=struct.pack('<Iq', 0xf35c6d01, mid) + raw)

    # -- the API
    def rpc(self, q):
        k = q['_']
        while k in ('invokeWithLayer', 'initConnection', 'invokeWithoutUpdates'):
            if k == 'initConnection':
                if q['api_id'] != ARGS.api_id:
                    raise RpcError(400, 'API_ID_INVALID')
                log(f'dc{self.dc}: initConnection from "{q["device_model"]}", {q["app_version"]}, {q["lang_code"]}')
            if k == 'invokeWithLayer':
                log(f'dc{self.dc}: layer {q["layer"]}')
                self.key.layer_ok = True
            q = q['query']
            k = q['_']
        log(f'dc{self.dc}: {k}')
        fn = getattr(self, 'f_' + k.replace('.', '_'), None)
        if not fn:
            raise RpcError(400, 'METHOD_NOT_FAKED')
        open_ = ('help.getConfig', 'help.getNearestDc', 'auth.sendCode', 'auth.signIn', 'auth.checkPassword',
                 'account.getPassword', 'auth.importAuthorization', 'auth.resendCode')
        with LOCK:
            if k not in open_ and self.key.user is None:
                raise RpcError(401, 'AUTH_KEY_UNREGISTERED')
            return fn(q)

    def f_help_getConfig(self, q):
        opts = [{'_': 'dcOption', 'id': i, 'ip_address': ARGS.bind, 'port': ARGS.port + i - 1} for i in (1, 2, 3)]
        return {'_': 'config', 'date': int(time.time()), 'expires': int(time.time()) + 3600, 'test_mode': False,
                'this_dc': self.dc, 'dc_options': opts, 'dc_txt_domain_name': 'fake', 'chat_size_max': 200,
                'megagroup_size_max': 200000, 'forwarded_count_max': 100, 'online_update_period_ms': 210000,
                'offline_blur_timeout_ms': 5000, 'offline_idle_timeout_ms': 30000, 'online_cloud_timeout_ms': 300000,
                'notify_cloud_delay_ms': 30000, 'notify_default_delay_ms': 1500, 'push_chat_period_ms': 60000,
                'push_chat_limit': 2, 'edit_time_limit': 172800, 'revoke_time_limit': 2147483647,
                'revoke_pm_time_limit': 2147483647, 'rating_e_decay': 2419200, 'stickers_recent_limit': 200,
                'channels_read_media_period': 604800, 'call_receive_timeout_ms': 20000,
                'call_ring_timeout_ms': 90000, 'call_connect_timeout_ms': 30000,
                'call_packet_timeout_ms': 10000, 'me_url_prefix': 'https://t.me/', 'caption_length_max': 1024,
                'message_length_max': 4096, 'webfile_dc_id': 1}

    def f_help_getNearestDc(self, q):
        return {'_': 'nearestDc', 'country': 'AR', 'this_dc': self.dc, 'nearest_dc': ARGS.user_dc}

    def f_auth_sendCode(self, q):
        if q['api_id'] != ARGS.api_id or q['api_hash'] != ARGS.api_hash:
            raise RpcError(400, 'API_ID_INVALID')
        if q['phone_number'].replace(' ', '').lstrip('+') != ARGS.phone.lstrip('+'):
            raise RpcError(400, 'PHONE_NUMBER_INVALID')
        if self.dc != ARGS.user_dc:
            raise RpcError(303, f'PHONE_MIGRATE_{ARGS.user_dc}')
        h = os.urandom(8).hex()
        PENDING_CODE[h] = True
        log(f'dc{self.dc}: the code is {ARGS.code}')
        return {'_': 'auth.sentCode', 'type': {'_': 'auth.sentCodeTypeApp', 'length': len(ARGS.code)},
                'phone_code_hash': h, 'next_type': {'_': 'auth.codeTypeSms'}, 'timeout': 60}

    def authorization(self):
        self.key.user = STORE.self_id
        keys_save()
        return {'_': 'auth.authorization', 'user': STORE.users[STORE.self_id]}

    def f_auth_signIn(self, q):
        if q['phone_code_hash'] not in PENDING_CODE:
            raise RpcError(400, 'PHONE_CODE_EXPIRED')
        if q.get('phone_code') != ARGS.code:
            raise RpcError(400, 'PHONE_CODE_INVALID')
        if ARGS.password:
            self.key.pw_pending = True
            raise RpcError(401, 'SESSION_PASSWORD_NEEDED')
        return self.authorization()

    def f_account_getPassword(self, q):
        if not ARGS.password:
            return {'_': 'account.password', 'new_algo': {'_': 'passwordKdfAlgoUnknown'},
                    'new_secure_algo': {'_': 'securePasswordKdfAlgoUnknown'}, 'secure_random': os.urandom(32)}
        b = int.from_bytes(os.urandom(256), 'big')
        B = R.srp_server_B(SRP_V, 3, R.DH_PRIME, b)
        sid = random.getrandbits(62)
        SRP_PENDING[sid] = (b, B)
        algo = {'_': 'passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow',
                'salt1': SRP_SALT1, 'salt2': SRP_SALT2, 'g': 3, 'p': R.DH_PRIME.to_bytes(256, 'big')}
        return {'_': 'account.password', 'has_password': True, 'current_algo': algo, 'srp_B': B.to_bytes(256, 'big'),
                'srp_id': sid, 'hint': 'pez' if ARGS.lang == 'es' else 'fish', 'new_algo': algo,
                'new_secure_algo': {'_': 'securePasswordKdfAlgoUnknown'}, 'secure_random': os.urandom(32)}

    def f_auth_checkPassword(self, q):
        pw = q['password']
        got = SRP_PENDING.pop(pw['srp_id'], None)
        if not got:
            raise RpcError(400, 'SRP_ID_INVALID')
        b, B = got
        if not R.srp_server_check(SRP_V, 3, R.DH_PRIME, b, B, SRP_SALT1, SRP_SALT2, pw['A'], pw['M1']):
            raise RpcError(400, 'PASSWORD_HASH_INVALID')
        log(f'dc{self.dc}: the two-step password checks')
        return self.authorization()

    def f_auth_logOut(self, q):
        self.key.user = None
        keys_save()
        return {'_': 'auth.loggedOut'}

    def f_auth_exportAuthorization(self, q):
        i = random.getrandbits(62)
        b = os.urandom(32)
        EXPORTED[i] = (b, q['dc_id'], self.key.user)
        return {'_': 'auth.exportedAuthorization', 'id': i, 'bytes': b}

    def f_auth_importAuthorization(self, q):
        e = EXPORTED.pop(q['id'], None)
        if not e or e[0] != q['bytes'] or e[1] != self.dc:
            raise RpcError(400, 'AUTH_BYTES_INVALID')
        self.key.user = e[2]
        keys_save()
        log(f'dc{self.dc}: signed in by importAuthorization')
        return {'_': 'auth.authorization', 'user': STORE.users[e[2]]}

    def f_users_getUsers(self, q):
        out = []
        for iu in q['id']:
            uid = STORE.self_id if iu['_'] == 'inputUserSelf' else iu.get('user_id')
            if uid in STORE.users:
                out.append(STORE.users[uid])
        return Vector(out)

    def f_updates_getState(self, q):
        return {'_': 'updates.state', 'pts': STORE.pts, 'qts': STORE.qts, 'date': int(time.time()), 'seq': STORE.seq,
                'unread_count': sum(STORE.unread.values())}

    def f_updates_getDifference(self, q):
        pts = q['pts']
        news, others = [], []
        for p, c, u in STORE.log:
            if p > pts:
                if u['_'] == 'updateNewMessage':
                    news.append(u['message'])
                else:
                    others.append(u)
        state = self.f_updates_getState(q)
        if not news and not others:
            return {'_': 'updates.differenceEmpty', 'date': int(time.time()), 'seq': STORE.seq}
        return {'_': 'updates.difference', 'new_messages': news, 'new_encrypted_messages': [], 'other_updates': others,
                'chats': STORE.all_chats(), 'users': STORE.all_users(), 'state': state}

    def f_updates_getChannelDifference(self, q):
        cid = q['channel']['channel_id']
        return {'_': 'updates.channelDifferenceEmpty', 'final': True, 'pts': STORE.chan_pts.get(cid, 50)}

    def f_messages_getDialogs(self, q):
        ds = STORE.dialogs()
        if q.get('offset_id'):
            # after the one named
            ids = [i for i, (d, t) in enumerate(ds) if t['id'] == q['offset_id']]
            ds = ds[ids[0] + 1:] if ids else []
        ds = ds[:q['limit']]
        return {'_': 'messages.dialogs', 'dialogs': [d for d, t in ds], 'messages': [t for d, t in ds],
                'chats': STORE.all_chats(), 'users': STORE.all_users()}

    def f_messages_getPinnedDialogs(self, q):
        ds = [x for x in STORE.dialogs() if x[0].get('pinned')]
        return {'_': 'messages.peerDialogs', 'dialogs': [d for d, t in ds], 'messages': [t for d, t in ds],
                'chats': STORE.all_chats(), 'users': STORE.all_users(), 'state': self.f_updates_getState(q)}

    def history(self, kind, pid, offset_id, limit):
        ms = STORE.msgs_of(kind, pid)
        ids = sorted((i for i in ms if not offset_id or i < offset_id), reverse=True)[:limit]
        res = [ms[i] for i in ids]
        if kind == 'channel':
            return {'_': 'messages.channelMessages', 'pts': STORE.chan_pts.get(pid, 50), 'count': len(ms),
                    'messages': res, 'topics': [], 'chats': STORE.all_chats(), 'users': STORE.all_users()}
        return {'_': 'messages.messages', 'messages': res, 'topics': [], 'chats': STORE.all_chats(),
                'users': STORE.all_users()}

    def f_messages_getHistory(self, q):
        kind, pid = STORE.input_peer(q['peer'])
        if not kind:
            raise RpcError(400, 'PEER_ID_INVALID')
        if kind in ('user', 'channel'):
            want = STORE.access + pid
            if q['peer'].get('access_hash') != want and q['peer']['_'] != 'inputPeerSelf':
                raise RpcError(400, 'PEER_ID_INVALID')
        return self.history(kind, pid, q['offset_id'], q['limit'])

    def f_messages_readHistory(self, q):
        kind, pid = STORE.input_peer(q['peer'])
        STORE.unread[(kind, pid)] = 0
        STORE.read_in[(kind, pid)] = q['max_id']
        u = STORE.bump({'_': 'updateReadHistoryInbox', 'peer': STORE.peer(kind, pid), 'max_id': q['max_id'],
                        'still_unread_count': 0})
        return {'_': 'messages.affectedMessages', 'pts': u['pts'], 'pts_count': 1}

    def f_channels_readHistory(self, q):
        STORE.chan_unread[q['channel']['channel_id']] = 0
        return True

    def new_updates(self, ups, kind=None):
        return {'_': 'updates', 'updates': ups, 'users': STORE.all_users(), 'chats': STORE.all_chats(),
                'date': int(time.time()), 'seq': 0}

    def f_messages_sendMessage(self, q):
        kind, pid = STORE.input_peer(q['peer'])
        if not kind:
            raise RpcError(400, 'PEER_ID_INVALID')
        reply = q.get('reply_to', {}).get('reply_to_msg_id')
        m = STORE.add(kind, pid, q['message'], out=True, reply=reply, count=False)
        log(f'dc{self.dc}: sent to {kind} {pid}: {q["message"]!r}')
        if kind == 'channel':
            STORE.chan_pts[pid] = STORE.chan_pts.get(pid, 50) + 1
            return self.new_updates([{'_': 'updateMessageID', 'id': m['id'], 'random_id': q['random_id']},
                                     {'_': 'updateNewChannelMessage', 'message': m, 'pts': STORE.chan_pts[pid],
                                      'pts_count': 1}])
        u = STORE.bump({'_': 'updateNewMessage', 'message': m})
        if kind == 'user' and pid in STORE.users:
            threading.Thread(target=answer_later, args=(pid, m['id']), daemon=True).start()
        if kind == 'user':
            return {'_': 'updateShortSentMessage', 'out': True, 'id': m['id'], 'pts': u['pts'], 'pts_count': 1,
                    'date': m['date']}
        return self.new_updates([{'_': 'updateMessageID', 'id': m['id'], 'random_id': q['random_id']}, u])

    def f_upload_saveFilePart(self, q):
        STORE.uploads.setdefault(q['file_id'], {})[q['file_part']] = q['bytes']
        return True

    def f_messages_sendMedia(self, q):
        kind, pid = STORE.input_peer(q['peer'])
        media = q['media']
        if media['_'] != 'inputMediaUploadedPhoto':
            raise RpcError(400, 'MEDIA_INVALID')
        f = media['file']
        parts = STORE.uploads.pop(f['id'], {})
        if len(parts) != f['parts']:
            raise RpcError(400, 'FILE_PARTS_INVALID')
        data = b''.join(parts[i] for i in range(f['parts']))
        if data[:2] != b'\xff\xd8':
            raise RpcError(400, 'IMAGE_PROCESS_FAILED')
        ph = STORE.photo_from_bytes(data)
        log(f'dc{self.dc}: a photo of {len(data)} bytes to {kind} {pid}')
        reply = q.get('reply_to', {}).get('reply_to_msg_id')
        m = STORE.add(kind, pid, q.get('message', ''), out=True, reply=reply, count=False,
                      media={'_': 'messageMediaPhoto', 'photo': ph})
        if kind == 'channel':
            STORE.chan_pts[pid] = STORE.chan_pts.get(pid, 50) + 1
            up = {'_': 'updateNewChannelMessage', 'message': m, 'pts': STORE.chan_pts[pid], 'pts_count': 1}
        else:
            up = STORE.bump({'_': 'updateNewMessage', 'message': m})
        return self.new_updates([{'_': 'updateMessageID', 'id': m['id'], 'random_id': q['random_id']}, up])

    def f_messages_editMessage(self, q):
        kind, pid = STORE.input_peer(q['peer'])
        m = STORE.msgs_of(kind, pid).get(q['id'])
        if not m or not m.get('out'):
            raise RpcError(400, 'MESSAGE_ID_INVALID')
        if m['message'] == q.get('message'):
            raise RpcError(400, 'MESSAGE_NOT_MODIFIED')
        m['message'] = q.get('message', m['message'])
        m['edit_date'] = int(time.time())
        if kind == 'channel':
            STORE.chan_pts[pid] = STORE.chan_pts.get(pid, 50) + 1
            up = {'_': 'updateEditChannelMessage', 'message': m, 'pts': STORE.chan_pts[pid], 'pts_count': 1}
        else:
            up = STORE.bump({'_': 'updateEditMessage', 'message': m})
        return self.new_updates([up])

    def f_messages_deleteMessages(self, q):
        gone = 0
        for (k, p), ms in STORE.box.items():
            for i in q['id']:
                if i in ms:
                    del ms[i]
                    gone += 1
        u = STORE.bump({'_': 'updateDeleteMessages', 'messages': q['id']}, max(1, gone))
        return {'_': 'messages.affectedMessages', 'pts': u['pts'], 'pts_count': u['pts_count']}

    def f_channels_deleteMessages(self, q):
        cid = q['channel']['channel_id']
        for i in q['id']:
            STORE.chan_box.get(cid, {}).pop(i, None)
        STORE.chan_pts[cid] = STORE.chan_pts.get(cid, 50) + 1
        return {'_': 'messages.affectedMessages', 'pts': STORE.chan_pts[cid], 'pts_count': 1}

    def f_messages_forwardMessages(self, q):
        fk, fp = STORE.input_peer(q['from_peer'])
        tk, tp = STORE.input_peer(q['to_peer'])
        ups = []
        for i, rid in zip(q['id'], q['random_id']):
            src = STORE.msgs_of(fk, fp).get(i)
            if not src:
                raise RpcError(400, 'MESSAGE_ID_INVALID')
            frm = src.get('from_id') or STORE.peer(fk, fp)
            fwd = {'_': 'messageFwdHeader', 'from_id': frm, 'date': src['date']}
            m = STORE.add(tk, tp, src.get('message', ''), out=True, media=src.get('media'), extra={'fwd_from': fwd},
                          count=False)
            ups.append({'_': 'updateMessageID', 'id': m['id'], 'random_id': rid})
            if tk == 'channel':
                STORE.chan_pts[tp] = STORE.chan_pts.get(tp, 50) + 1
                ups.append({'_': 'updateNewChannelMessage', 'message': m, 'pts': STORE.chan_pts[tp], 'pts_count': 1})
            else:
                ups.append(STORE.bump({'_': 'updateNewMessage', 'message': m}))
        return self.new_updates(ups)

    def f_messages_sendReaction(self, q):
        kind, pid = STORE.input_peer(q['peer'])
        m = STORE.msgs_of(kind, pid).get(q['msg_id'])
        if not m:
            raise RpcError(400, 'MESSAGE_ID_INVALID')
        res = m.get('reactions', {'_': 'messageReactions', 'results': []})['results']
        for r in res:                      # ours out of where it was
            if 'chosen_order' in r:
                del r['chosen_order']
                r['count'] -= 1
        res = [r for r in res if r['count'] > 0]
        for want in q.get('reaction', []):
            e = want.get('emoticon')
            hit = [r for r in res if r['reaction'].get('emoticon') == e]
            if hit:
                hit[0]['count'] += 1
                hit[0]['chosen_order'] = 0
            else:
                res.append({'_': 'reactionCount', 'reaction': {'_': 'reactionEmoji', 'emoticon': e}, 'count': 1,
                            'chosen_order': 0})
        m['reactions'] = {'_': 'messageReactions', 'results': res}
        return self.new_updates([{'_': 'updateMessageReactions', 'peer': STORE.peer(kind, pid), 'msg_id': m['id'],
                                  'reactions': m['reactions']}])

    def f_messages_setTyping(self, q):
        return True

    def f_messages_getAvailableReactions(self, q):
        return {'_': 'messages.availableReactionsNotModified'}

    def search(self, text):
        found = []
        for (k, p), ms in STORE.box.items():
            found += [m for m in ms.values() if text.lower() in m.get('message', '').lower()]
        for c, ms in STORE.chan_box.items():
            found += [m for m in ms.values() if text.lower() in m.get('message', '').lower()]
        found.sort(key=lambda m: m['date'], reverse=True)
        return found

    def f_messages_searchGlobal(self, q):
        text = q['q'] if isinstance(q['q'], str) else q['q'].decode('utf-8', 'replace')
        log(f'dc{self.dc}: searching {q["q"]!r}')
        res = self.search(text)[:q['limit']]
        return {'_': 'messages.messages', 'messages': res, 'topics': [], 'chats': STORE.all_chats(),
                'users': STORE.all_users()}

    def f_messages_search(self, q):
        return self.f_messages_searchGlobal(q)

    def f_contacts_search(self, q):
        t = (q['q'] if isinstance(q['q'], str) else q['q'].decode('utf-8', 'replace')).lower()
        mine = [STORE.peer('user', u['id']) for u in STORE.users.values()
                if u['id'] != STORE.self_id and t in (u['first_name'] + ' ' + u.get('last_name', '')).lower()]
        mine += [STORE.peer('channel', c['id']) for c in STORE.channels.values() if t in c['title'].lower()]
        mine += [STORE.peer('chat', c['id']) for c in STORE.chats.values() if t in c['title'].lower()]
        return {'_': 'contacts.found', 'my_results': mine[:q['limit']], 'results': [], 'chats': STORE.all_chats(),
                'users': STORE.all_users()}

    def f_upload_getFile(self, q):
        loc = q['location']
        k = loc['_']
        if k == 'inputPeerPhotoFileLocation':
            data = STORE.files.get(('avatar', loc['photo_id']))
            dc = 1
        elif k == 'inputPhotoFileLocation':
            data = STORE.files.get(('photo', loc['id'], loc['thumb_size']))
            dc = STORE.photo_dc.get(loc['id'], 1)
        elif k == 'inputDocumentFileLocation':
            data = STORE.files.get(('doc', loc['id'], loc['thumb_size']))
            dc = 1
        else:
            raise RpcError(400, 'LOCATION_INVALID')
        if data is None:
            raise RpcError(400, 'FILE_ID_INVALID')
        if dc != self.dc:
            raise RpcError(303, f'FILE_MIGRATE_{dc}')
        off, lim = q['offset'], q['limit']
        if off % 1024 or lim % 1024 or (1048576 % lim):
            raise RpcError(400, 'LIMIT_INVALID')
        ftype = {'_': 'storage.fileJpeg'} if data[:2] == b'\xff\xd8' else {'_': 'storage.filePartial'}
        return {'_': 'upload.file', 'type': ftype, 'mtime': int(time.time()), 'bytes': data[off:off + lim]}


class Vector(list):
    pass


def result_bytes(v):
    if v is True or v is False:
        return struct.pack('<I', tl_schema.BOOL_TRUE if v else tl_schema.BOOL_FALSE)
    if isinstance(v, Vector):
        return struct.pack('<Ii', tl_schema.VECTOR_ID, len(v)) + b''.join(TL.encode(x) for x in v)
    raise TypeError(v)


class RpcError(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code
        self.msg = msg


def R_PRIMES():
    a, b = rand_prime(31), rand_prime(31)
    return min(a, b), max(a, b)


# ---------------------------------------------------------------- pushing

def push(updates_obj):
    """To every connection signed in as the account on its home data centre."""
    raw = TL.encode(updates_obj)
    with LOCK:
        conns = [c for c in CONNS if c.alive and c.key and c.key.user and c.dc == ARGS.user_dc and c.key.layer_ok]
        if not conns:
            log(f'{updates_obj["_"]}: no connection signed in on dc{ARGS.user_dc} that said its layer')
    for c in conns:
        try:
            c.send(None, raw=raw)
        except OSError:
            pass


def arrive(kind, pid, text, frm=None):
    with LOCK:
        if kind == 'channel':
            m = STORE.add(kind, pid, text, frm=frm)
            STORE.chan_pts[pid] = STORE.chan_pts.get(pid, 50) + 1
            STORE.chan_unread[pid] = STORE.chan_unread.get(pid, 0) + 1
            up = {'_': 'updates', 'updates': [{'_': 'updateNewChannelMessage', 'message': m,
                                                'pts': STORE.chan_pts[pid], 'pts_count': 1}],
                  'users': STORE.all_users(), 'chats': STORE.all_chats(), 'date': int(time.time()), 'seq': 0}
        elif kind == 'user':
            m = STORE.add(kind, pid, text)
            u = STORE.bump({'_': 'updateNewMessage', 'message': m})
            up = {'_': 'updateShortMessage', 'id': m['id'], 'user_id': pid, 'message': text, 'pts': u['pts'],
                  'pts_count': 1, 'date': m['date']}
        else:
            m = STORE.add(kind, pid, text, frm=frm)
            u = STORE.bump({'_': 'updateNewMessage', 'message': m})
            up = {'_': 'updateShortChatMessage', 'id': m['id'], 'from_id': frm, 'chat_id': pid, 'message': text,
                  'pts': u['pts'], 'pts_count': 1, 'date': m['date']}
    log(f'a message arrives in {kind} {pid}: {text!r}')
    push(up)


def answer_later(uid, our_id):
    time.sleep(1.5)
    push({'_': 'updateShort', 'update': {'_': 'updateUserTyping', 'user_id': uid,
                                          'action': {'_': 'sendMessageTypingAction'}}, 'date': int(time.time())})
    time.sleep(2.5)
    with LOCK:
        STORE.read_out[('user', uid)] = our_id
        u = STORE.bump({'_': 'updateReadHistoryOutbox', 'peer': STORE.peer('user', uid), 'max_id': our_id})
    push({'_': 'updateShort', 'update': u, 'date': int(time.time())})
    arrive('user', uid, T('reply'))


def chatter():
    who = [('user', 2003, None, 'chatter'), ('chat', 3001, 2002, 'chatter2'), ('channel', 4001, 2001, 'm19'),
           ('user', 2001, None, 'reply2')]
    i = 0
    while True:
        time.sleep(ARGS.chatter)
        k, p, f, t = who[i % len(who)]
        arrive(k, p, T(t), frm=f)
        i += 1


def control():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((ARGS.bind, ARGS.control))
    srv.listen(4)
    log(f'control on {ARGS.bind}:{ARGS.control}')
    while True:
        c, _ = srv.accept()
        f = c.makefile('rw')
        for line in f:
            w = line.strip().split(' ', 3)
            try:
                if w[0] == 'say':
                    kind, pid = w[1], int(w[2])
                    frm = 2002 if kind == 'chat' else 2001 if kind == 'channel' else None
                    arrive(kind, pid, w[3], frm)
                    f.write('ok\n')
                elif w[0] == 'gap':
                    STORE.gap_next = True
                    f.write('ok\n')
                elif w[0] == 'revoke':
                    with LOCK:
                        for k in KEYS.values():
                            k.user = None
                    keys_save()
                    f.write('ok\n')
                elif w[0] == 'list':
                    for u in STORE.users.values():
                        f.write(f'user {u["id"]} {u["first_name"]}\n')
                    for c2 in STORE.chats.values():
                        f.write(f'chat {c2["id"]} {c2["title"]}\n')
                    for c2 in STORE.channels.values():
                        f.write(f'channel {c2["id"]} {c2["title"]}\n')
                    f.write('ok\n')
                else:
                    f.write('?\n')
            except Exception as e:  # noqa: BLE001
                f.write(f'error {e}\n')
            f.flush()
        c.close()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


SRP_PENDING = {}


def main():
    global ARGS, STORE, RSA_N, RSA_D, FINGERPRINT, SRP_SALT1, SRP_SALT2, SRP_V
    ap = argparse.ArgumentParser(description='A make-believe Telegram for the Mensajes app')
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=9443)
    ap.add_argument('--lang', default='es', choices=('es', 'en'))
    ap.add_argument('--api-id', type=int, default=12345)
    ap.add_argument('--api-hash', default='00112233445566778899aabbccddeeff')
    ap.add_argument('--phone', default='+5491155550100')
    ap.add_argument('--code', default='12345')
    ap.add_argument('--password', default='pescado')
    ap.add_argument('--no-password', action='store_true')
    ap.add_argument('--user-dc', type=int, default=1)
    ap.add_argument('--chatter', type=float, default=0)
    ap.add_argument('--control', type=int, default=0)
    ap.add_argument('--key', default=os.path.expanduser('~/.cache/p4os_fake_telegram.json'))
    ap.add_argument('--client-config', default='')
    ap.add_argument('--state', default='', help='a file to keep the auth keys and sign-ins across restarts')
    ap.add_argument('--quiet', action='store_true')
    ARGS = ap.parse_args()
    if ARGS.no_password:
        ARGS.password = ''
    RSA_N, RSA_D = load_key(ARGS.key)
    FINGERPRINT = R.rsa_fingerprint(RSA_N, 65537)
    SRP_SALT1 = os.urandom(32)
    SRP_SALT2 = os.urandom(16)
    if ARGS.password:
        SRP_V = R.srp_verifier(ARGS.password, SRP_SALT1, SRP_SALT2, 3, R.DH_PRIME)
    STORE = Store()
    keys_load()
    if ARGS.client_config:
        os.makedirs(os.path.dirname(os.path.abspath(ARGS.client_config)), exist_ok=True)
        with open(ARGS.client_config, 'w') as f:
            f.write(f'# tools/fake_telegram.py: a test server, not Telegram\nhost {ARGS.bind}\nport {ARGS.port}\n'
                    f'rsa {RSA_N:0512x}\n')
        log(f'wrote {ARGS.client_config}')
    for dc in (1, 2, 3):
        s = Server((ARGS.bind, ARGS.port + dc - 1), Conn)
        s.dc = dc
        threading.Thread(target=s.serve_forever, daemon=True).start()
    log(f'fake Telegram on {ARGS.bind}:{ARGS.port}..{ARGS.port + 2} (dc1..dc3), account on dc{ARGS.user_dc}, '
        f'phone {ARGS.phone}, code {ARGS.code}, password {"none" if not ARGS.password else repr(ARGS.password)}')
    if ARGS.chatter:
        threading.Thread(target=chatter, daemon=True).start()
    if ARGS.control:
        threading.Thread(target=control, daemon=True).start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
