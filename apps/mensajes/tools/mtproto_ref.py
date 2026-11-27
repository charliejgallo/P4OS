#!/usr/bin/env python3
"""
P4OS - Mensajes: MTProto 2.0's cryptography in Python, the reference.

Written from core.telegram.org (mtproto/description, mtproto/auth_key,
api/srp) and nothing of the app's C: the test vectors of tests/vectors.py
come from here, and the fake server (tools/fake_telegram.py) speaks with
it. AES comes from pycryptodome when there is one and from the small one
below when there is not (slow, but enough for a test).
"""
import hashlib
import os
import struct

try:
    from Crypto.Cipher import AES as _AES   # pycryptodome

    def aes_ecb_encrypt(key, block):
        return _AES.new(key, _AES.MODE_ECB).encrypt(block)

    def aes_ecb_decrypt(key, block):
        return _AES.new(key, _AES.MODE_ECB).decrypt(block)
except ImportError:  # pragma: no cover - a plain AES, for machines without it
    _SB = []
    _ISB = [0] * 256

    def _init():
        p = q = 1
        sb = [0] * 256
        while True:
            p = (p ^ (p << 1) ^ (0x1b if p & 0x80 else 0)) & 0xff
            q ^= q << 1
            q ^= q << 2
            q ^= q << 4
            q &= 0xff
            if q & 0x80:
                q ^= 0x09
            r = lambda x, n: ((x << n) | (x >> (8 - n))) & 0xff
            sb[p] = q ^ r(q, 1) ^ r(q, 2) ^ r(q, 3) ^ r(q, 4) ^ 0x63
            if p == 1:
                break
        sb[0] = 0x63
        _SB.extend(sb)
        for i, v in enumerate(sb):
            _ISB[v] = i

    _init()

    def _xt(x):
        return ((x << 1) ^ (0x1b if x & 0x80 else 0)) & 0xff

    def _gm(a, b):
        r = 0
        while b:
            if b & 1:
                r ^= a
            a = _xt(a)
            b >>= 1
        return r

    def _expand(key):
        w = [list(key[4 * i:4 * i + 4]) for i in range(8)]
        rc = 1
        for i in range(8, 60):
            t = list(w[i - 1])
            if i % 8 == 0:
                t = [_SB[b] for b in t[1:] + t[:1]]
                t[0] ^= rc
                rc = _xt(rc)
            elif i % 8 == 4:
                t = [_SB[b] for b in t]
            w.append([a ^ b for a, b in zip(w[i - 8], t)])
        return [sum(w[4 * r:4 * r + 4], []) for r in range(15)]

    def aes_ecb_encrypt(key, block):
        rk = _expand(key)
        s = [b ^ k for b, k in zip(block, rk[0])]
        for r in range(1, 15):
            s = [_SB[b] for b in s]
            s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]
            if r != 14:
                m = []
                for c in range(4):
                    a = s[4 * c:4 * c + 4]
                    m += [_gm(a[0], 2) ^ _gm(a[1], 3) ^ a[2] ^ a[3], a[0] ^ _gm(a[1], 2) ^ _gm(a[2], 3) ^ a[3],
                          a[0] ^ a[1] ^ _gm(a[2], 2) ^ _gm(a[3], 3), _gm(a[0], 3) ^ a[1] ^ a[2] ^ _gm(a[3], 2)]
                s = m
            s = [b ^ k for b, k in zip(s, rk[r])]
        return bytes(s)

    def aes_ecb_decrypt(key, block):
        rk = _expand(key)
        s = [b ^ k for b, k in zip(block, rk[14])]
        for r in range(13, -1, -1):
            s = [s[(i - 4 * (i % 4)) % 16] for i in range(16)]
            s = [_ISB[b] for b in s]
            s = [b ^ k for b, k in zip(s, rk[r])]
            if r != 0:
                m = []
                for c in range(4):
                    a = s[4 * c:4 * c + 4]
                    m += [_gm(a[0], 14) ^ _gm(a[1], 11) ^ _gm(a[2], 13) ^ _gm(a[3], 9),
                          _gm(a[0], 9) ^ _gm(a[1], 14) ^ _gm(a[2], 11) ^ _gm(a[3], 13),
                          _gm(a[0], 13) ^ _gm(a[1], 9) ^ _gm(a[2], 14) ^ _gm(a[3], 11),
                          _gm(a[0], 11) ^ _gm(a[1], 13) ^ _gm(a[2], 9) ^ _gm(a[3], 14)]
                s = m
        return bytes(s)


def sha1(*parts):
    h = hashlib.sha1()
    for p in parts:
        h.update(p)
    return h.digest()


def sha256(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p)
    return h.digest()


def xor(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


def ige_encrypt(data, key, iv):
    assert len(data) % 16 == 0
    prev_c, prev_p = iv[:16], iv[16:]
    out = b''
    for i in range(0, len(data), 16):
        p = data[i:i + 16]
        c = xor(aes_ecb_encrypt(key, xor(p, prev_c)), prev_p)
        out += c
        prev_c, prev_p = c, p
    return out


def ige_decrypt(data, key, iv):
    assert len(data) % 16 == 0
    prev_c, prev_p = iv[:16], iv[16:]
    out = b''
    for i in range(0, len(data), 16):
        c = data[i:i + 16]
        p = xor(aes_ecb_decrypt(key, xor(c, prev_p)), prev_c)
        out += p
        prev_c, prev_p = c, p
    return out


# ---- MTProto 2.0 messages

def auth_key_id(auth_key):
    return sha1(auth_key)[12:20]


def msg_keys(auth_key, msg_key, x):
    """x = 0 from the client, 8 from the server."""
    a = sha256(msg_key, auth_key[x:x + 36])
    b = sha256(auth_key[40 + x:40 + x + 36], msg_key)
    key = a[:8] + b[8:24] + a[24:32]
    iv = b[:8] + a[8:24] + b[24:32]
    return key, iv


def seal(auth_key, plaintext, x):
    """plaintext already padded to 16 with 12..1024 bytes of padding."""
    msg_key = sha256(auth_key[88 + x:88 + x + 32], plaintext)[8:24]
    key, iv = msg_keys(auth_key, msg_key, x)
    return auth_key_id(auth_key) + msg_key + ige_encrypt(plaintext, key, iv)


def open_sealed(auth_key, data, x):
    """The plaintext of a sealed message, or None if its msg_key is wrong."""
    if data[:8] != auth_key_id(auth_key):
        return None
    msg_key = data[8:24]
    key, iv = msg_keys(auth_key, msg_key, x)
    pt = ige_decrypt(data[24:], key, iv)
    if sha256(auth_key[88 + x:88 + x + 32], pt)[8:24] != msg_key:
        return None
    return pt


# ---- the handshake

def tmp_aes(new_nonce, server_nonce):
    key = sha1(new_nonce, server_nonce) + sha1(server_nonce, new_nonce)[:12]
    iv = sha1(server_nonce, new_nonce)[12:20] + sha1(new_nonce, new_nonce) + new_nonce[:4]
    return key, iv


def new_nonce_hash(new_nonce, auth_key, n):
    aux = sha1(auth_key)[:8]
    return sha1(new_nonce, bytes([n]), aux)[4:20]


def rsa_pad_encrypt(data, n, e, temp_key=None, padding=None):
    """RSA_PAD of core.telegram.org/mtproto/auth_key (the client's side)."""
    assert len(data) <= 144
    while True:
        pad = padding if padding is not None else os.urandom(192 - len(data))
        data_with_padding = data + pad
        rev = data_with_padding[::-1]
        tk = temp_key if temp_key is not None else os.urandom(32)
        data_with_hash = rev + sha256(tk, data_with_padding)
        aes_encrypted = ige_encrypt(data_with_hash, tk, b'\0' * 32)
        temp_key_xor = xor(tk, sha256(aes_encrypted))
        kae = temp_key_xor + aes_encrypted
        if int.from_bytes(kae, 'big') < n:
            return pow(int.from_bytes(kae, 'big'), e, n).to_bytes(256, 'big')
        if temp_key is not None:
            raise ValueError('the fixed temp_key does not fit under the modulus')


def rsa_pad_decrypt(enc, n, d):
    """The server's side: the data, or None when the hash does not match."""
    kae = pow(int.from_bytes(enc, 'big'), d, n).to_bytes(256, 'big')
    temp_key_xor, aes_encrypted = kae[:32], kae[32:]
    tk = xor(temp_key_xor, sha256(aes_encrypted))
    data_with_hash = ige_decrypt(aes_encrypted, tk, b'\0' * 32)
    rev, h = data_with_hash[:192], data_with_hash[192:]
    data_with_padding = rev[::-1]
    if sha256(tk, data_with_padding) != h:
        return None
    return data_with_padding


def tl_bytes(b):
    n = len(b)
    o = bytes([n]) + b if n < 254 else bytes([254]) + n.to_bytes(3, 'little') + b
    return o + b'\0' * ((-len(o)) % 4)


def rsa_fingerprint(n, e):
    nb = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    eb = e.to_bytes((e.bit_length() + 7) // 8, 'big')
    return struct.unpack('<q', sha1(tl_bytes(nb) + tl_bytes(eb))[12:20])[0]


DH_PRIME = int(
    'c71caeb9c6b1c9048e6c522f70f13f73980d40238e3e21c14934d037563d930f48198a0aa7c14058229493d22530f4db'
    'fa336f6e0ac925139543aed44cce7c3720fd51f69458705ac68cd4fe6b6b13abdc9746512969328454f18faf8c595f64'
    '2477fe96bb2a941d5bcd1d4ac8cc49880708fa9b378e3c4f3a9060bee67cf9a4a4a695811051907e162753b56b0f6b41'
    '0dba74d8a84b2a14b3144e0ef1284754fd17ed950d5965b4b9dd46582db1178d169c6bc465b0d6ff9ca3928fef5b9ae4'
    'e418fc15e83ebea0f87fa9ff5eed70050ded2849f47bf959d956850ce929851f0d8115f635b105ee2e4e15d04b2454bf'
    '6f4fadf034b10403119cd8e3b92fcc5b', 16)


# ---- SRP, core.telegram.org/api/srp

def _sh(data, salt):
    return sha256(salt, data, salt)


def srp_x(password, salt1, salt2):
    ph1 = _sh(_sh(password.encode(), salt1), salt2)
    return _sh(hashlib.pbkdf2_hmac('sha512', ph1, salt1, 100000), salt2)


def srp_verifier(password, salt1, salt2, g, p):
    """What the server keeps of the password: v = g^x mod p."""
    x = int.from_bytes(srp_x(password, salt1, salt2), 'big')
    return pow(g, x, p)


def srp_client(password, salt1, salt2, g, p, B, a):
    """A and M1 for inputCheckPasswordSRP. p, B, a are ints."""
    pb = p.to_bytes(256, 'big')
    gb = g.to_bytes(256, 'big')
    Bb = B.to_bytes(256, 'big')
    x = int.from_bytes(srp_x(password, salt1, salt2), 'big')
    v = pow(g, x, p)
    k = int.from_bytes(sha256(pb, gb), 'big')
    A = pow(g, a, p)
    Ab = A.to_bytes(256, 'big')
    u = int.from_bytes(sha256(Ab, Bb), 'big')
    t = (B - k * v) % p
    S = pow(t, a + u * x, p)
    K = sha256(S.to_bytes(256, 'big'))
    M1 = sha256(xor(sha256(pb), sha256(gb)), sha256(salt1), sha256(salt2), Ab, Bb, K)
    return Ab, M1


def srp_server_B(v, g, p, b):
    """The server's B = k*v + g^b mod p."""
    pb = p.to_bytes(256, 'big')
    gb = g.to_bytes(256, 'big')
    k = int.from_bytes(sha256(pb, gb), 'big')
    return (k * v + pow(g, b, p)) % p


def srp_server_check(v, g, p, b, B, salt1, salt2, A_bytes, M1):
    pb = p.to_bytes(256, 'big')
    gb = g.to_bytes(256, 'big')
    Bb = B.to_bytes(256, 'big')
    A = int.from_bytes(A_bytes, 'big')
    if A <= 0 or A >= p:
        return False
    u = int.from_bytes(sha256(A_bytes, Bb), 'big')
    S = pow(A * pow(v, u, p), b, p)
    K = sha256(S.to_bytes(256, 'big'))
    want = sha256(xor(sha256(pb), sha256(gb)), sha256(salt1), sha256(salt2), A_bytes, Bb, K)
    return want == M1
