#!/usr/bin/env python3
"""
TRADUCTOR - Mozilla's "tiny" translation models of a pair, for mkpack.py.

    fetch_models.py es-en <dir>         # -> <dir>/esen, <dir>/enes (~22 MB each)
    fetch_models.py --list              # the pairs that have a tiny model

They come from Firefox's own model collection (Remote Settings), which keeps
the "tiny" generation (version 1.x: 16.9 M parameters, 17.1 MB in int8)
beside the newer "base-memory" ones (31.2 M, 31.6 MB) that the engine does
not run. Each file is checked against the hash the record gives. The models
are MPL-2.0 (https://github.com/mozilla/translations).

Only for building packs on a computer: the app never downloads from here.
"""
import hashlib
import json
import os
import sys
import urllib.request

RECORDS = 'https://firefox.settings.services.mozilla.com/v1/buckets/main/collections/translations-models/records'
ATTACH = 'https://firefox-settings-attachments.cdn.mozilla.net/'
TINY_SIZE = 17_200_000      # the tiny models are 17.14 MB; base-memory 31.6 MB


def records():
    with urllib.request.urlopen(RECORDS, timeout=60) as r:
        return json.load(r)['data']


def tiny_sets(recs):
    """{(from, to): {version: {fileType: record}}} for versions whose model is tiny"""
    by = {}
    for r in recs:
        key = (r.get('fromLang'), r.get('toLang'))
        by.setdefault(key, {}).setdefault(r.get('version'), {})[r.get('fileType')] = r
    out = {}
    for key, vers in by.items():
        for v, files in vers.items():
            m = files.get('model')
            if m and m['attachment']['size'] <= TINY_SIZE and 'vocab' in files and 'lex' in files:
                out.setdefault(key, {})[v] = files
    return out


def fetch(rec, path):
    a = rec['attachment']
    if os.path.exists(path) and os.path.getsize(path) == a['size']:
        h = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        if h == a['hash']:
            return
    with urllib.request.urlopen(ATTACH + a['location'], timeout=120) as r:
        data = r.read()
    h = hashlib.sha256(data).hexdigest()
    if h != a['hash']:
        raise SystemExit('%s: hash %s, the record says %s' % (rec['name'], h, a['hash']))
    with open(path, 'wb') as f:
        f.write(data)


def main():
    if len(sys.argv) == 2 and sys.argv[1] == '--list':
        for (s, t), vers in sorted(tiny_sets(records()).items()):
            print('%s-%s  %s' % (s, t, ', '.join(sorted(vers))))
        return
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    x, y = sys.argv[1].split('-')
    sets = tiny_sets(records())
    for s, t in ((x, y), (y, x)):
        vers = sets.get((s, t))
        if not vers:
            print('%s-%s: no tiny model' % (s, t))
            continue
        # a release over a pre-release ("1.0a1"), then the newest
        v = sorted(vers, key=lambda x: (not any(c.isalpha() for c in x), x))[-1]
        d = os.path.join(sys.argv[2], s + t)
        os.makedirs(d, exist_ok=True)
        names = {'model': 'model.bin', 'lex': 'lex.bin', 'vocab': 'vocab.spm'}
        for ft, name in names.items():
            fetch(vers[v][ft], os.path.join(d, name))
        print('%s-%s: version %s in %s' % (s, t, v, d))


if __name__ == '__main__':
    main()
