#!/usr/bin/env python3
"""
TRADUCTOR - a Bergamot "tiny" model (Firefox Translations, MPL-2.0) into the
entries of a pair pack (main/td_nmt.c reads them).

    marian2pak.py <dir> <src><trg> out.pak       # one direction, for a test
    mkpack.py calls nmt_entries() to put both directions in a pair's pack

<dir> holds the three files of one direction as Mozilla ships them
(uncompressed): model.<st>.intgemm.alphas.bin, lex.50.50.<st>.s2t.bin,
vocab.<st>.spm (any names: the first *.bin with "model", "lex", *.spm).

What changes on the way: names get short ("esen.e3.aq"), the int8 matrices
stay as Marian stores them (each output's inputs in a row) with 1/QuantMultB
as the scale and QuantMultA beside it, the shortlist's ids go to 16 bits and
the vocabulary drops the protobuf. Only the "tiny" shape is accepted
(6 encoder layers, 2 SSRU decoder layers, 256 wide, tied embeddings).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trpak  # noqa: E402

ENC_ATT = {'Wq': 'aq', 'Wk': 'ak', 'Wv': 'av', 'Wo': 'ao',
           'bq': 'abq', 'bk': 'abk', 'bv': 'abv', 'bo': 'abo',
           'Wo_ln_scale': 'als', 'Wo_ln_bias': 'alb'}
FFN = {'W1': 'f1', 'W2': 'f2', 'b1': 'fb1', 'b2': 'fb2',
       'ffn_ln_scale': 'fls', 'ffn_ln_bias': 'flb'}
RNN = {'W': 'r', 'Wf': 'rf', 'bf': 'rbf', 'ffn_ln_scale': 'rls', 'ffn_ln_bias': 'rlb'}


def read_marian(path):
    f = open(path, 'rb').read()
    ver, n = struct.unpack_from('<QQ', f, 0)
    if ver != 1:
        raise SystemExit('%s: not a Marian binary model' % path)
    off = 16
    hdr = [struct.unpack_from('<QQQQ', f, off + 32 * i) for i in range(n)]
    off += 32 * n
    names = []
    for h in hdr:
        names.append(f[off:off + h[0]].rstrip(b'\0').decode())
        off += h[0]
    shapes = []
    for h in hdr:
        shapes.append(struct.unpack_from('<%di' % h[2], f, off))
        off += 4 * h[2]
    pad, = struct.unpack_from('<Q', f, off)
    off += 8 + pad
    T = {}
    for nm, h, sh in zip(names, hdr, shapes):
        T[nm] = (h[1], sh, f[off:off + h[3]])
        off += h[3]
    return T


def short_name(nm):
    """encoder_l3_self_Wq -> e3.aq ; decoder_l1_rnn_Wf -> d1.rf"""
    if nm == 'Wemb':
        return 'emb'
    if nm == 'decoder_ff_logit_out_b':
        return 'ob'
    p = nm.split('_')
    side, layer, block, rest = p[0], p[1][1:], p[2], '_'.join(p[3:])
    s = 'e' if side == 'encoder' else 'd'
    if block in ('self', 'context'):
        m = ENC_ATT[rest]
        if block == 'context':
            m = 'c' + m[1:]
    elif block == 'ffn':
        m = FFN[rest]
    elif block == 'rnn':
        m = RNN[rest]
    else:
        raise ValueError(nm)
    return '%s%s.%s' % (s, layer, m)


def pb_fields(buf):
    i = 0

    def varint():
        nonlocal i
        v = s = 0
        while True:
            b = buf[i]
            i += 1
            v |= (b & 0x7f) << s
            s += 7
            if b < 0x80:
                return v
    while i < len(buf):
        key = varint()
        fn, wt = key >> 3, key & 7
        if wt == 0:
            yield fn, varint()
        elif wt == 2:
            ln = varint()
            yield fn, buf[i:i + ln]
            i += ln
        elif wt == 5:
            yield fn, buf[i:i + 4]
            i += 4
        elif wt == 1:
            yield fn, buf[i:i + 8]
            i += 8
        else:
            raise ValueError('protobuf wire type %d' % wt)


def spm_entry(path):
    """A SentencePiece model -> u32 n, then n x (f32 score, u8 type, u8 len, bytes)"""
    buf = open(path, 'rb').read()
    out = bytearray()
    n = 0
    model_type = 1
    for fn, v in pb_fields(buf):
        if fn == 1:
            piece, score, typ = b'', 0.0, 1
            for a, c in pb_fields(v):
                if a == 1:
                    piece = c
                elif a == 2:
                    score = struct.unpack('<f', c)[0]
                elif a == 3:
                    typ = c
            assert len(piece) < 256
            out += struct.pack('<fBB', score, typ, len(piece)) + piece
            n += 1
        elif fn == 2:
            for a, c in pb_fields(v):
                if a == 3:
                    model_type = c
    if model_type != 1:
        raise SystemExit('%s: only unigram SentencePiece models' % path)
    return struct.pack('<I', n) + bytes(out), n


def lex_entry(path, vocab):
    """Marian's binary shortlist -> u16 first, u16 0, u32 nsrc, u32 off[nsrc+1], u16 ids[]"""
    b = open(path, 'rb').read()
    magic, chk, first, best, nofs, nlist = struct.unpack_from('<QQQQQQ', b, 0)
    o = 48
    offs = struct.unpack_from('<%dQ' % nofs, b, o)
    o += 8 * nofs
    ids = struct.unpack_from('<%dI' % nlist, b, o)
    assert max(ids) < 65536 and max(ids) < vocab
    return (struct.pack('<HHI', first, 0, nofs - 1) + struct.pack('<%dI' % nofs, *offs)
            + struct.pack('<%dH' % nlist, *ids)), first, best


def find(d, pred):
    for f in sorted(os.listdir(d)):
        if pred(f):
            return os.path.join(d, f)
    raise SystemExit('%s: missing file' % d)


def nmt_entries(w, d, pair):
    """add the entries of one direction (pair is 'esen') to the trpak.Writer w"""
    T = read_marian(find(d, lambda f: 'model' in f and f.endswith('.bin')))
    yml = T['special:model.yml'][2].decode()
    cfg = dict(l.split(': ', 1) for l in yml.splitlines() if ': ' in l)
    want = {'dim-emb': '256', 'enc-depth': '6', 'dec-depth': '2', 'dec-cell': 'ssru',
            'transformer-heads': '8', 'transformer-dim-ffn': '1536',
            'transformer-postprocess': 'dan', 'tied-embeddings-all': 'true',
            'transformer-decoder-autoreg': 'rnn', 'transformer-ffn-activation': 'relu'}
    for k, v in want.items():
        if cfg.get(k, '').strip() != v:
            raise SystemExit('%s: %s is %r, the engine wants %r (only "tiny" models)'
                             % (d, k, cfg.get(k), v))
    mults = {nm[:-len('_QuantMultA')]: struct.unpack_from('<f', t[2], 0)[0]
             for nm, t in T.items() if nm.endswith('_QuantMultA')}
    for nm, (typ, sh, raw) in sorted(T.items()):
        if nm.startswith('special:') or nm.endswith('_QuantMultA'):
            continue
        name = pair + '.' + short_name(nm)
        if typ == 0x4101:                     # intgemm8: int8, then QuantMultB
            r, c = sh
            mb, = struct.unpack_from('<f', raw, r * c)
            if nm == 'Wemb':                  # rows are the words
                w.add(name, trpak.T_MAT8, raw[:r * c], r, c, 1.0 / mb, mults.get(nm, 0.0))
            else:                             # Marian keeps B transposed: out x in
                w.add(name, trpak.T_MAT8, raw[:r * c], c, r, 1.0 / mb, mults.get(nm, 0.0))
        elif typ == 0x404:
            n = 1
            for s in sh:
                n *= s
            w.add(name, trpak.T_VECF, raw[:4 * n], 1, n)
        else:
            raise SystemExit('%s: tensor %s of type %x' % (d, nm, typ))
    spm, nv = spm_entry(find(d, lambda f: f.endswith('.spm')))
    w.add(pair + '.spm', trpak.T_SPM, spm, nv, 0)
    lex, first, best = lex_entry(find(d, lambda f: 'lex' in f and f.endswith('.bin')), nv)
    w.add(pair + '.lex', trpak.T_LEX, lex, first, best)


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    w = trpak.Writer()
    nmt_entries(w, sys.argv[1], sys.argv[2])
    size, parts = w.write(sys.argv[3])
    print('%s: %d bytes in %d part(s), %d entries' % (sys.argv[3], size, parts, len(w.entries)))


if __name__ == '__main__':
    main()
