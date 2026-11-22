#!/usr/bin/env python3
"""
Fake translation server for the Traductor app (apps/traductor).

One HTTP server that answers like the two online engines the app knows,
so the app is tested without touching the real services:

    GET  /get?q=&langpair=es|en[&de=email]       MyMemory
    POST /translate  {q, source, target, format, api_key}   LibreTranslate
    GET  /languages                              LibreTranslate
    POST /detect     {q, api_key}                LibreTranslate

In the simulator the app's MyMemory is already http://127.0.0.1:8790 (the
default port here); for LibreTranslate write "server http://127.0.0.1:8790"
in sim_fs/data/traductor/traductor.txt, or on the portal's page.

The translations are made up: a few whole sentences, then word by word
from a small table, marked with the target's code so nobody takes them for
real ones ("[en] the house is big").

Errors on purpose (in the text sent):
    error500      the server answers 500 (LibreTranslate) or responseStatus 500
    lento / slow  the answer takes 8 s (the app's waiting and Cancel)
    cuota / quota MyMemory's daily-limit answer; LibreTranslate's 429
    --quota N     MyMemory's daily characters (5000; with de=, ten times more)
    --key K       LibreTranslate wants api_key K: none or another is a 400/403
    --port P      8790
    --host H      127.0.0.1; 0.0.0.0 to reach it from the board on the LAN

GET /_stats says how many requests and characters came; GET /_reset zeroes
them (the quota too).
"""
import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

SENTENCES = {
    ('es', 'en'): {
        'hola': 'Hello',
        'buenos días': 'Good morning',
        '¿dónde está la estación de tren?': 'Where is the train station?',
        'la cuenta, por favor.': 'The bill, please.',
    },
    ('en', 'es'): {
        'hello': 'Hola',
        'good morning': 'Buenos días',
        'where is the train station?': '¿Dónde está la estación de tren?',
        'the bill, please.': 'La cuenta, por favor.',
    },
}

WORDS = {
    ('es', 'en'): {'el': 'the', 'la': 'the', 'casa': 'house', 'es': 'is', 'grande': 'big', 'perro': 'dog',
                   'gato': 'cat', 'y': 'and', 'un': 'a', 'una': 'a', 'mañana': 'tomorrow', 'agua': 'water'},
    ('en', 'es'): {'the': 'el', 'house': 'casa', 'is': 'es', 'big': 'grande', 'dog': 'perro', 'cat': 'gato',
                   'and': 'y', 'a': 'un', 'tomorrow': 'mañana', 'water': 'agua'},
}

LANGS = ['es', 'en', 'pt', 'it', 'fr', 'de', 'ca', 'nl', 'ja', 'zh', 'ru']
NAMES = {'es': 'Spanish', 'en': 'English', 'pt': 'Portuguese', 'it': 'Italian', 'fr': 'French',
         'de': 'German', 'ca': 'Catalan', 'nl': 'Dutch', 'ja': 'Japanese', 'zh': 'Chinese', 'ru': 'Russian'}

STATS = {'mymemory': 0, 'libre': 0, 'chars': 0, 'chars_by_email': {}}
LOCK = threading.Lock()
ARGS = None


def detect(text):
    t = ' ' + text.lower() + ' '
    es = sum(t.count(w) for w in (' el ', ' la ', ' de ', ' que ', ' y ', ' es ', 'ñ', '¿'))
    en = sum(t.count(w) for w in (' the ', ' is ', ' and ', ' of ', ' to ', ' you '))
    return ('es', 0.9) if es >= en else ('en', 0.9)


def invent(text, src, dst):
    s = SENTENCES.get((src, dst), {}).get(text.strip().lower())
    if s:
        return s
    table = WORDS.get((src, dst), {})
    out = []
    for w in text.split():
        core = w.strip('.,;:!?¿¡').lower()
        out.append(w.lower().replace(core, table.get(core, core)) if core else w)
    return '[%s] %s' % (dst, ' '.join(out))


class H(BaseHTTPRequestHandler):
    server_version = 'FakeTranslate/1.0'

    def log_message(self, fmt, *a):
        print('[fake_translate] ' + fmt % a)

    def send(self, code, obj):
        b = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(b)))
        self.send_header('Access-Control-Allow-Origin', '*')    # as LibreTranslate does
        self.end_headers()
        self.wfile.write(b)

    def special(self, text):
        t = text.lower()
        if 'lento' in t or 'slow' in t:
            time.sleep(8)
        return 'error500' in t, 'cuota' in t or 'quota' in t

    # ---- MyMemory

    def mymemory(self, qs):
        q = qs.get('q', [''])[0]
        pair = qs.get('langpair', [''])[0]
        email = qs.get('de', [''])[0]
        with LOCK:
            STATS['mymemory'] += 1
        if not q:
            return self.send(200, {'responseData': {'translatedText': 'NO QUERY SPECIFIED. EXAMPLE REQUEST: GET?Q=HELLO&LANGPAIR=EN|IT', 'match': 0},
                                   'quotaFinished': False, 'responseDetails': 'NO QUERY SPECIFIED',
                                   'responseStatus': 403, 'matches': ''})
        if len(q.encode()) > 500:
            return self.send(200, {'responseData': {'translatedText': 'QUERY LENGTH LIMIT EXCEEDED. MAX ALLOWED QUERY : 500 CHARS', 'match': 0},
                                   'quotaFinished': False, 'responseDetails': 'QUERY LENGTH LIMIT EXCEEDED',
                                   'responseStatus': 403, 'matches': ''})
        if '|' not in pair:
            return self.send(200, {'responseData': {'translatedText': "'' IS AN INVALID SOURCE LANGUAGE . EXAMPLE: LANGPAIR=EN|IT USING 2 LETTER ISO OR RFC3066 LIKE ZH-CN. ALMOST ALL LANGUAGES SUPPORTED BUT SOME MAY HAVE NO CONTENT", 'match': 0},
                                   'quotaFinished': False, 'responseDetails': 'INVALID LANGPAIR',
                                   'responseStatus': '403', 'matches': ''})
        src, dst = pair.split('|', 1)
        err500, quota = self.special(q)
        limit = ARGS.quota * (10 if email else 1)
        with LOCK:
            used = STATS['chars_by_email'].get(email, 0)
            over = quota or used + len(q) > limit
            if not over:
                STATS['chars_by_email'][email] = used + len(q)
                STATS['chars'] += len(q)
        if over:
            return self.send(200, {
                'responseData': {'translatedText': 'MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE TRANSLATIONS FOR TODAY. NEXT AVAILABLE IN  10 HOURS 20 MINUTES 33 SECONDS VISIT HTTPS://MYMEMORY.TRANSLATED.NET/DOC/USAGELIMITS.PHP TO TRANSLATE MORE', 'match': 1},
                'quotaFinished': True, 'mtLangSupported': None, 'responseDetails': '', 'responseStatus': 429,
                'responderId': None, 'exception_code': None, 'matches': []})
        if err500:
            return self.send(200, {'responseData': {'translatedText': 'INTERNAL ERROR', 'match': 0},
                                   'quotaFinished': False, 'responseDetails': 'INTERNAL ERROR',
                                   'responseStatus': 500, 'matches': ''})
        if src.lower() == 'autodetect':
            src = detect(q)[0]
        tr = invent(q, src[:2], dst[:2]).replace("'", '&#39;')
        self.send(200, {
            'responseData': {'translatedText': tr, 'match': 0.85},
            'quotaFinished': False, 'mtLangSupported': None, 'responseDetails': '', 'responseStatus': 200,
            'responderId': None, 'exception_code': None,
            'matches': [{'id': '0', 'segment': q, 'translation': tr, 'source': src + '-XN', 'target': dst + '-XN',
                         'quality': 74, 'reference': None, 'usage-count': 2, 'subject': '', 'created-by': 'MT!',
                         'last-updated-by': 'MT!', 'create-date': '2026-11-22 00:00:00',
                         'last-update-date': '2026-11-22 00:00:00', 'match': 0.85, 'penalty': 0}]})

    # ---- LibreTranslate

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n) if n else b''
        ct = self.headers.get('Content-Type', '')
        if 'json' in ct:
            try:
                return json.loads(raw.decode() or '{}')
            except ValueError:
                return None
        return {k: v[0] for k, v in parse_qs(raw.decode()).items()}

    def key_ok(self, d):
        if not ARGS.key:
            return True
        k = d.get('api_key', '')
        if not k:
            self.send(400, {'error': 'Visit https://portal.libretranslate.example to get an API key'})
            return False
        if k != ARGS.key:
            self.send(403, {'error': 'Invalid API key'})
            return False
        return True

    def libre(self, path):
        d = self.body()
        with LOCK:
            STATS['libre'] += 1
        if d is None:
            return self.send(400, {'error': 'Invalid request: the body is not JSON'})
        if not self.key_ok(d):
            return
        q = d.get('q', '')
        if path == '/detect':
            lang, conf = detect(q)
            return self.send(200, [{'confidence': conf * 100, 'language': lang}])
        src, dst = d.get('source', ''), d.get('target', '')
        if not q:
            return self.send(400, {'error': 'Invalid request: missing q parameter'})
        if not dst or dst not in LANGS:
            return self.send(400, {'error': '%s is not supported' % dst})
        err500, quota = self.special(q)
        if quota:
            return self.send(429, {'error': 'Slowdown: 20 per 1 minute'})
        if err500:
            return self.send(500, {'error': 'Cannot translate text: something went wrong (fake)'})
        with LOCK:
            STATS['chars'] += len(q)
        out = {}
        if src == 'auto':
            src, conf = detect(q)
            out['detectedLanguage'] = {'confidence': conf * 100, 'language': src}
        elif src not in LANGS:
            return self.send(400, {'error': '%s is not supported' % src})
        out['translatedText'] = invent(q, src, dst)
        self.send(200, out)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == '/get':
            return self.mymemory(parse_qs(u.query))
        if u.path == '/languages':
            return self.send(200, [{'code': c, 'name': NAMES[c], 'targets': [t for t in LANGS if t != c]}
                                   for c in LANGS])
        if u.path == '/_stats':
            with LOCK:
                return self.send(200, STATS)
        if u.path == '/_reset':
            with LOCK:
                STATS.update({'mymemory': 0, 'libre': 0, 'chars': 0, 'chars_by_email': {}})
            return self.send(200, {'ok': True})
        self.send(404, {'error': 'Not Found'})

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def do_POST(self):
        u = urlparse(self.path)
        if u.path in ('/translate', '/detect'):
            return self.libre(u.path)
        self.send(404, {'error': 'Not Found'})


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8790)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--quota', type=int, default=5000)
    ap.add_argument('--key', default='')
    ARGS = ap.parse_args()
    srv = ThreadingHTTPServer((ARGS.host, ARGS.port), H)
    print('[fake_translate] on http://%s:%d (MyMemory /get, LibreTranslate /translate)' % (ARGS.host, ARGS.port))
    srv.serve_forever()


if __name__ == '__main__':
    main()
