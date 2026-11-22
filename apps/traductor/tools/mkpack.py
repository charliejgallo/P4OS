#!/usr/bin/env python3
"""
TRADUCTOR - a language pair's pack, traductor_<a>_<b>_p4.pak.

    mkpack.py es-en --models <dir> [--dict <dir>] [-o out.pak]

<models> holds one folder a direction, as tools/fetch_models.py leaves
them: <models>/esen, <models>/enes (Mozilla's tiny models, MPL-2.0).
<dict> holds WikDict's StarDict files, one folder a direction:
<dict>/es-en/stardict.{idx,dict,syn}, <dict>/en-es/... (CC BY-SA 4.0).
Either may be missing; a pack with neither is refused.

The pack (main/td_pak.h), per direction "esen":
    esen.*        the model (tools/marian2pak.py); the vocabulary once when
                  both directions share it (enes.spm is then absent and
                  the engine takes esen.spm)
    esen.dic      the dictionary: u32 n, u32 key_off[n] (sorted by key),
                  then the records: key\\0 u32 body_off; bodies after:
                  head\\0 lines "pos\\tgloss\\ttranslations\\n" \\0
    info          pair=, nmt=, dict=, version=, and the licences
    license       the notices of what is inside
"""
import argparse
import html.parser
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import marian2pak  # noqa: E402
import trpak  # noqa: E402

VERSION = '1'

MPL = ('The translation models are Mozilla\'s Firefox Translations models '
       '(https://github.com/mozilla/translations), distributed under the '
       'Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/). Converted '
       'to this pack\'s layout by apps/traductor/tools/marian2pak.py of P4OS; '
       'the weights are unchanged.')
CCBYSA = ('The dictionaries are WikDict\'s (https://www.wikdict.com/), built '
          'from Wiktionary (https://www.wiktionary.org/) via DBnary, under '
          'the Creative Commons Attribution-ShareAlike 4.0 licence '
          '(https://creativecommons.org/licenses/by-sa/4.0/). Shortened to '
          'their parts of speech, glosses and translations by '
          'apps/traductor/tools/mkpack.py; this part of the pack is under '
          'the same licence.')


# ---- WikDict's StarDict entries -> "pos\tgloss\ttranslations" lines

class Node:
    def __init__(self, tag, attrs, parent):
        self.tag, self.attrs, self.parent = tag, dict(attrs), parent
        self.kids = []          # Node or str

    def text(self):
        return ' '.join(''.join(k if isinstance(k, str) else k.text() for k in self.kids).split())

    def own_text(self):
        return ' '.join(''.join(k for k in self.kids if isinstance(k, str)).split())

    def find(self, tag):
        for k in self.kids:
            if isinstance(k, Node):
                if k.tag == tag:
                    yield k
                yield from k.find(tag)


class Tree(html.parser.HTMLParser):
    """WikDict's entry as a small tree: <div><div><font class=grammar>noun
    </font></div><ol><li>gloss<ol><li><div>house</div></li></ol></li></ol>
    </div>. The translations are the divs with no element inside; a sense
    is an <li> of the outer list, its gloss the loose text in it."""
    VOID = ('br',)

    def __init__(self):
        super().__init__()
        self.root = Node('root', [], None)
        self.cur = self.root

    def handle_starttag(self, tag, attrs):
        n = Node(tag, attrs, self.cur)
        self.cur.kids.append(n)
        if tag not in self.VOID:
            self.cur = n

    def handle_endtag(self, tag):
        c = self.cur
        while c is not self.root and c.tag != tag:
            c = c.parent
        if c is not self.root:
            self.cur = c.parent

    def handle_data(self, data):
        self.cur.kids.append(data)


def leaf_divs(n):
    out = []
    for d in n.find('div'):
        if not any(isinstance(k, Node) for k in d.kids):
            t = d.text()
            if t:
                out.append(t)
    return out


def parse_entry(htm):
    t = Tree()
    t.feed(htm)
    pos = ''
    for f in t.root.find('font'):
        if f.attrs.get('class') == 'grammar':
            pos = f.text()
            break
    top = None
    for o in t.root.find('ol'):
        top = o
        break
    senses = []
    if top is None:
        body = next(t.root.find('div'), None)
        gloss = ''
        if body is not None:
            gloss = body.own_text()
        senses.append((gloss, leaf_divs(t.root)))
    else:
        for li in top.kids:
            if not isinstance(li, Node) or li.tag != 'li':
                continue
            gloss = li.own_text()
            if not gloss:
                for sub in li.find('li'):
                    gloss = sub.own_text()
                    if gloss:
                        break
            senses.append((gloss, leaf_divs(li)))
    return pos, senses


def clean_gloss(g):
    g = re.sub(r'^[\s/,;]+', '', g)          # what the pronunciations leave
    g = g.replace('|', ':')
    if len(g) > 70:
        g = g[:67].rsplit(' ', 1)[0] + '...'
    return g


def entry_lines(htm):
    pos, senses = parse_entry(htm)
    lines = []
    seen = set()
    for gloss, trs in senses:
        trs = [x for x in trs if x and len(x) < 60 and x not in seen][:5]
        seen.update(trs)
        if not trs:
            continue
        lines.append('%s\t%s\t%s' % (pos, clean_gloss(gloss), ', '.join(trs)))
        if len(lines) >= 5:
            break
    return lines


MAX_KEYS = 150_000
MAX_SYN = 60_000

POS_ORDER = {p: i for i, p in enumerate(['noun', 'verb', 'adjective', 'adverb', 'interjection', 'phrase', 'pronoun', 'preposition'])}


def fold(w):
    return w.strip().lower()


def read_stardict(d):
    idx = open(os.path.join(d, 'stardict.idx'), 'rb').read()
    dic = open(os.path.join(d, 'stardict.dict'), 'rb').read()
    words = []
    i = 0
    while i < len(idx):
        j = idx.index(b'\0', i)
        w = idx[i:j].decode('utf-8', 'replace')
        off, sz = struct.unpack('>II', idx[j + 1:j + 9])
        words.append((w, dic[off:off + sz].decode('utf-8', 'replace')))
        i = j + 9
    syn = []
    sp = os.path.join(d, 'stardict.syn')
    if os.path.exists(sp):
        b = open(sp, 'rb').read()
        i = 0
        while i < len(b):
            j = b.index(b'\0', i)
            syn.append((b[i:j].decode('utf-8', 'replace'), struct.unpack('>I', b[j + 1:j + 5])[0]))
            i = j + 5
    return words, syn


def dict_entry(d):
    words, syn = read_stardict(d)
    heads = {}              # folded -> (head, [lines])
    order = []
    index_of = []           # stardict index -> folded key
    for w, htm in words:
        k = fold(w)
        index_of.append(k)
        lines = entry_lines(htm)
        if not lines:
            continue
        if k not in heads:
            heads[k] = (w, [])
            order.append(k)
        for ln in lines:
            if ln not in heads[k][1]:
                heads[k][1].append(ln)
    bodies = bytearray()
    body_off = {}
    for k in order:
        head, lines = heads[k]
        body_off[k] = len(bodies)
        # the common parts of speech first, each keeping its order
        lines = sorted(lines, key=lambda ln: POS_ORDER.get(ln.split('\t', 1)[0], len(POS_ORDER)))
        bodies += head.encode() + b'\0' + '\n'.join(lines[:8]).encode() + b'\0'
    keys = {k: body_off[k] for k in order}
    nsyn = 0
    # the app keeps 4 bytes a key in memory: 150 000 keys at most (600 KB),
    # and no more than 60 000 other forms (German brings over a million)
    room = min(MAX_SYN, max(0, MAX_KEYS - len(keys)))
    for s, i in syn:
        if nsyn >= room:
            break
        k = fold(s)
        tgt = index_of[i] if i < len(index_of) else None
        if k not in keys and tgt in body_off and len(k) < 60:
            keys[k] = body_off[tgt]
            nsyn += 1
    sk = sorted(keys, key=lambda x: x.encode())
    recs = bytearray()
    offs = []
    for k in sk:
        offs.append(len(recs))
        recs += k.encode() + b'\0' + struct.pack('<I', keys[k])
    n = len(sk)
    hl = 8 + 4 * n                      # the offsets count from the entry's start
    head = struct.pack('<II', n, 0) + struct.pack('<%dI' % n, *[o + hl for o in offs])
    # bodies' offsets are after the records
    blob = head + bytes(recs)
    base = len(blob)
    blob = bytearray(blob)
    # rewrite each record's body_off as absolute
    o = len(head)
    for k in sk:
        o += len(k.encode()) + 1
        rel, = struct.unpack_from('<I', blob, o)
        struct.pack_into('<I', blob, o, rel + base)
        o += 4
    blob += bodies
    return bytes(blob), len(order), nsyn


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pair', help='es-en')
    ap.add_argument('--models')
    ap.add_argument('--dict')
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    x, y = a.pair.split('-')
    dirs = [x + y, y + x]
    out = a.out or 'traductor_%s_%s_p4.pak' % (x, y)
    w = trpak.Writer()
    nmt, dic, lic = [], [], []
    spm_seen = {}
    if a.models:
        for d in dirs:
            md = os.path.join(a.models, d)
            if not os.path.isdir(md):
                continue
            marian2pak.nmt_entries(w, md, d)
            # one vocabulary when both directions share it
            e = [k for k in w.entries if k[0] == d + '.spm'][0]
            for od, data in spm_seen.items():
                if data == e[2]:
                    w.entries.remove(e)
                    print('  %s shares the vocabulary of %s' % (d, od))
                    break
            else:
                spm_seen[d] = e[2]
            nmt.append(d)
        if nmt:
            lic.append(MPL)
    if a.dict:
        for d in dirs:
            dd = os.path.join(a.dict, '%s-%s' % (d[:2], d[2:]))
            if not os.path.isdir(dd):
                continue
            blob, n, nsyn = dict_entry(dd)
            w.add(d + '.dic', trpak.T_DICT, blob, 0, 0)
            dic.append(d)
            print('  %s.dic: %d words, %d other forms, %.1f MB' % (d, n, nsyn, len(blob) / 1e6))
        if dic:
            lic.append(CCBYSA)
    if not nmt and not dic:
        raise SystemExit('nothing to pack')
    info = 'pair=%s\nnmt=%s\ndict=%s\nversion=%s\n' % (a.pair, ','.join(nmt), ','.join(dic), VERSION)
    w.add('info', trpak.T_INFO, info.encode())
    w.add('license', trpak.T_BYTES, '\n\n'.join(lic).encode())
    size, parts = w.write(out)
    print('%s: %.1f MB in %d part(s), %d entries' % (out, size / 1e6, parts, len(w.entries)))


if __name__ == '__main__':
    main()
