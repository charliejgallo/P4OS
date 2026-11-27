/*
 * P4OS - Mensajes: SHA-1, AES-256-IGE, bignum, pq, PBKDF2 and SRP
 * (mj_crypto.h says why they are here and not borrowed from mbedTLS).
 */
#include "mj_crypto.h"

#include <string.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "mj_port.h"

/* ---------------------------------------------------------------- SHA-1 */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void mj_sha1_init(mj_sha1_t *c)
{
    static const uint32_t iv[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

void mj_sha1_update(mj_sha1_t *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    while (n) {
        if (c->n == 0 && n >= 64) {
            sha1_block(c->h, p);
            p += 64;
            n -= 64;
            continue;
        }
        size_t k = 64 - c->n;
        if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += (uint32_t)k;
        p += k;
        n -= k;
        if (c->n == 64) {
            sha1_block(c->h, c->buf);
            c->n = 0;
        }
    }
}

void mj_sha1_final(mj_sha1_t *c, uint8_t out[20])
{
    uint32_t bits_hi = (uint32_t)(c->len >> 29), bits_lo = (uint32_t)(c->len << 3);
    uint8_t pad = 0x80;
    mj_sha1_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->n != 56) mj_sha1_update(c, &z, 1);
    uint8_t l[8] = {(uint8_t)(bits_hi >> 24), (uint8_t)(bits_hi >> 16), (uint8_t)(bits_hi >> 8), (uint8_t)bits_hi,
                    (uint8_t)(bits_lo >> 24), (uint8_t)(bits_lo >> 16), (uint8_t)(bits_lo >> 8), (uint8_t)bits_lo};
    mj_sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void mj_sha1(const void *d, size_t n, uint8_t out[20])
{
    mj_sha1_t c;
    mj_sha1_init(&c);
    mj_sha1_update(&c, d, n);
    mj_sha1_final(&c, out);
}

void mj_sha1_2(const void *a, size_t an, const void *b, size_t bn, uint8_t out[20])
{
    mj_sha1_t c;
    mj_sha1_init(&c);
    mj_sha1_update(&c, a, an);
    mj_sha1_update(&c, b, bn);
    mj_sha1_final(&c, out);
}

/* -------------------------------------------------------------- AES-256 */

/* The tables are built once, the first time a key is set: 8 KB of
 * T-tables and the two S-boxes, about 20 cycles a byte instead of a
 * hundred for a byte-at-a-time AES. */
static uint8_t SB[256], ISB[256];
static uint32_t TE[4][256], TD[4][256];
static bool s_tables;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static void aes_tables(void)
{
    if (s_tables) return;
    /* the S-box from the multiplicative inverse and the affine map */
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^
                              (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4)));
        SB[p] = x ^ 0x63;
    } while (p != 1);
    SB[0] = 0x63;
    for (int i = 0; i < 256; i++) ISB[SB[i]] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        uint8_t s = SB[i];
        uint32_t t = (uint32_t)xt(s) | (uint32_t)s << 8 | (uint32_t)s << 16 | (uint32_t)(xt(s) ^ s) << 24;
        uint8_t is = ISB[i];
        uint32_t u = (uint32_t)gmul(is, 14) | (uint32_t)gmul(is, 9) << 8 | (uint32_t)gmul(is, 13) << 16 |
                     (uint32_t)gmul(is, 11) << 24;
        for (int k = 0; k < 4; k++) {
            TE[k][i] = t;
            TD[k][i] = u;
            t = t << 8 | t >> 24;
            u = u << 8 | u >> 24;
        }
    }
    s_tables = true;
}

static uint32_t ld32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t subw(uint32_t w)
{
    return (uint32_t)SB[w & 0xff] | (uint32_t)SB[(w >> 8) & 0xff] << 8 | (uint32_t)SB[(w >> 16) & 0xff] << 16 |
           (uint32_t)SB[w >> 24] << 24;
}

void mj_aes_setkey(mj_aes_t *a, const uint8_t key[32])
{
    aes_tables();
    uint32_t *w = a->ek;
    for (int i = 0; i < 8; i++) w[i] = ld32(key + 4 * i);
    uint8_t rc = 1;
    for (int i = 8; i < 60; i++) {
        uint32_t t = w[i - 1];
        if (i % 8 == 0) {
            t = subw(t >> 8 | t << 24) ^ rc;
            rc = xt(rc);
        } else if (i % 8 == 4) {
            t = subw(t);
        }
        w[i] = w[i - 8] ^ t;
    }
    /* the decryption keys: in reverse order, InvMixColumns on the middle ones */
    for (int r = 0; r <= 14; r++)
        for (int c = 0; c < 4; c++) {
            uint32_t k = a->ek[(14 - r) * 4 + c];
            if (r > 0 && r < 14)
                k = TD[0][SB[k & 0xff]] ^ TD[1][SB[(k >> 8) & 0xff]] ^ TD[2][SB[(k >> 16) & 0xff]] ^ TD[3][SB[k >> 24]];
            a->dk[r * 4 + c] = k;
        }
}

void mj_aes_encrypt(const mj_aes_t *a, const uint8_t in[16], uint8_t out[16])
{
    const uint32_t *k = a->ek;
    uint32_t s0 = ld32(in) ^ k[0], s1 = ld32(in + 4) ^ k[1], s2 = ld32(in + 8) ^ k[2], s3 = ld32(in + 12) ^ k[3];
    for (int r = 1; r < 14; r++) {
        k += 4;
        uint32_t t0 = TE[0][s0 & 0xff] ^ TE[1][(s1 >> 8) & 0xff] ^ TE[2][(s2 >> 16) & 0xff] ^ TE[3][s3 >> 24] ^ k[0];
        uint32_t t1 = TE[0][s1 & 0xff] ^ TE[1][(s2 >> 8) & 0xff] ^ TE[2][(s3 >> 16) & 0xff] ^ TE[3][s0 >> 24] ^ k[1];
        uint32_t t2 = TE[0][s2 & 0xff] ^ TE[1][(s3 >> 8) & 0xff] ^ TE[2][(s0 >> 16) & 0xff] ^ TE[3][s1 >> 24] ^ k[2];
        uint32_t t3 = TE[0][s3 & 0xff] ^ TE[1][(s0 >> 8) & 0xff] ^ TE[2][(s1 >> 16) & 0xff] ^ TE[3][s2 >> 24] ^ k[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }
    k += 4;
#define LASTE(a, b, c, d) \
    ((uint32_t)SB[(a) & 0xff] | (uint32_t)SB[((b) >> 8) & 0xff] << 8 | (uint32_t)SB[((c) >> 16) & 0xff] << 16 | (uint32_t)SB[(d) >> 24] << 24)
    st32(out, LASTE(s0, s1, s2, s3) ^ k[0]);
    st32(out + 4, LASTE(s1, s2, s3, s0) ^ k[1]);
    st32(out + 8, LASTE(s2, s3, s0, s1) ^ k[2]);
    st32(out + 12, LASTE(s3, s0, s1, s2) ^ k[3]);
}

void mj_aes_decrypt(const mj_aes_t *a, const uint8_t in[16], uint8_t out[16])
{
    const uint32_t *k = a->dk;
    uint32_t s0 = ld32(in) ^ k[0], s1 = ld32(in + 4) ^ k[1], s2 = ld32(in + 8) ^ k[2], s3 = ld32(in + 12) ^ k[3];
    for (int r = 1; r < 14; r++) {
        k += 4;
        uint32_t t0 = TD[0][s0 & 0xff] ^ TD[1][(s3 >> 8) & 0xff] ^ TD[2][(s2 >> 16) & 0xff] ^ TD[3][s1 >> 24] ^ k[0];
        uint32_t t1 = TD[0][s1 & 0xff] ^ TD[1][(s0 >> 8) & 0xff] ^ TD[2][(s3 >> 16) & 0xff] ^ TD[3][s2 >> 24] ^ k[1];
        uint32_t t2 = TD[0][s2 & 0xff] ^ TD[1][(s1 >> 8) & 0xff] ^ TD[2][(s0 >> 16) & 0xff] ^ TD[3][s3 >> 24] ^ k[2];
        uint32_t t3 = TD[0][s3 & 0xff] ^ TD[1][(s2 >> 8) & 0xff] ^ TD[2][(s1 >> 16) & 0xff] ^ TD[3][s0 >> 24] ^ k[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }
    k += 4;
#define LASTD(a, b, c, d) \
    ((uint32_t)ISB[(a) & 0xff] | (uint32_t)ISB[((b) >> 8) & 0xff] << 8 | (uint32_t)ISB[((c) >> 16) & 0xff] << 16 | (uint32_t)ISB[(d) >> 24] << 24)
    st32(out, LASTD(s0, s3, s2, s1) ^ k[0]);
    st32(out + 4, LASTD(s1, s0, s3, s2) ^ k[1]);
    st32(out + 8, LASTD(s2, s1, s0, s3) ^ k[2]);
    st32(out + 12, LASTD(s3, s2, s1, s0) ^ k[3]);
}

static void xor16(uint8_t *d, const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++) d[i] = a[i] ^ b[i];
}

void mj_ige_encrypt(const uint8_t key[32], const uint8_t iv[32], uint8_t *d, size_t n)
{
    mj_aes_t a;
    mj_aes_setkey(&a, key);
    uint8_t prev_c[16], prev_p[16], t[16], p[16];
    memcpy(prev_c, iv, 16);
    memcpy(prev_p, iv + 16, 16);
    for (size_t i = 0; i + 16 <= n; i += 16) {
        memcpy(p, d + i, 16);
        xor16(t, p, prev_c);
        mj_aes_encrypt(&a, t, t);
        xor16(d + i, t, prev_p);
        memcpy(prev_c, d + i, 16);
        memcpy(prev_p, p, 16);
    }
    crypto_wipe(&a, sizeof a);
}

void mj_ige_decrypt(const uint8_t key[32], const uint8_t iv[32], uint8_t *d, size_t n)
{
    mj_aes_t a;
    mj_aes_setkey(&a, key);
    uint8_t prev_c[16], prev_p[16], t[16], c[16];
    memcpy(prev_c, iv, 16);
    memcpy(prev_p, iv + 16, 16);
    for (size_t i = 0; i + 16 <= n; i += 16) {
        memcpy(c, d + i, 16);
        xor16(t, c, prev_p);
        mj_aes_decrypt(&a, t, t);
        xor16(d + i, t, prev_c);
        memcpy(prev_p, d + i, 16);
        memcpy(prev_c, c, 16);
    }
    crypto_wipe(&a, sizeof a);
}

/* --------------------------------------------------------------- bignum */

static void bn_norm(mj_bn_t *r)
{
    while (r->n > 0 && r->d[r->n - 1] == 0) r->n--;
}

void mj_bn_zero(mj_bn_t *r)
{
    memset(r, 0, sizeof *r);
}

void mj_bn_set_u32(mj_bn_t *r, uint32_t v)
{
    mj_bn_zero(r);
    r->d[0] = v;
    r->n = v ? 1 : 0;
}

bool mj_bn_from_be(mj_bn_t *r, const uint8_t *b, size_t len)
{
    mj_bn_zero(r);
    if (len > MJ_BN_LIMBS * 4) return false;
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i);
        r->d[bit / 4] |= (uint32_t)b[i] << (8 * (bit % 4));
    }
    r->n = (int)((len + 3) / 4);
    bn_norm(r);
    return true;
}

bool mj_bn_to_be(const mj_bn_t *a, uint8_t *out, size_t len)
{
    if ((size_t)(mj_bn_bits(a) + 7) / 8 > len) return false;
    for (size_t i = 0; i < len; i++) {
        size_t bit = len - 1 - i;
        out[i] = bit / 4 < (size_t)a->n ? (uint8_t)(a->d[bit / 4] >> (8 * (bit % 4))) : 0;
    }
    return true;
}

int mj_bn_cmp(const mj_bn_t *a, const mj_bn_t *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

int mj_bn_bits(const mj_bn_t *a)
{
    if (a->n == 0) return 0;
    uint32_t top = a->d[a->n - 1];
    int b = 0;
    while (top) {
        b++;
        top >>= 1;
    }
    return (a->n - 1) * 32 + b;
}

bool mj_bn_is_zero(const mj_bn_t *a)
{
    return a->n == 0;
}

void mj_bn_add(mj_bn_t *r, const mj_bn_t *a, const mj_bn_t *b)
{
    int n = a->n > b->n ? a->n : b->n;
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (uint64_t)(i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)c;
        c >>= 32;
    }
    if (c && n < MJ_BN_LIMBS) r->d[n++] = (uint32_t)c;
    for (int i = n; i < r->n; i++) r->d[i] = 0;
    r->n = n;
    bn_norm(r);
}

void mj_bn_sub(mj_bn_t *r, const mj_bn_t *a, const mj_bn_t *b)
{
    int64_t c = 0;
    int n = a->n;
    for (int i = 0; i < n; i++) {
        c += (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)c;
        c = c < 0 ? -1 : 0;
    }
    for (int i = n; i < r->n; i++) r->d[i] = 0;
    r->n = n;
    bn_norm(r);
}

void mj_bn_mul(mj_bn_t *r, const mj_bn_t *a, const mj_bn_t *b)
{
    uint32_t t[MJ_BN_LIMBS];
    int n = a->n + b->n;
    if (n > MJ_BN_LIMBS) n = MJ_BN_LIMBS;
    memset(t, 0, sizeof t);
    for (int i = 0; i < a->n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < b->n && i + j < MJ_BN_LIMBS; j++) {
            c += (uint64_t)a->d[i] * b->d[j] + t[i + j];
            t[i + j] = (uint32_t)c;
            c >>= 32;
        }
        if (i + b->n < MJ_BN_LIMBS) t[i + b->n] = (uint32_t)c;
    }
    memcpy(r->d, t, sizeof t);
    r->n = n;
    bn_norm(r);
}

/* a mod m, one bit at a time: slow (4096 steps for a product of two 2048-bit
 * numbers), and only used a few times per handshake or password. */
void mj_bn_mod(mj_bn_t *r, const mj_bn_t *a, const mj_bn_t *m)
{
    mj_bn_t *acc = (mj_bn_t *)mj_calloc(sizeof(mj_bn_t));
    if (!acc) {
        mj_bn_zero(r);
        return;
    }
    int bits = mj_bn_bits(a);
    for (int i = bits - 1; i >= 0; i--) {
        /* acc = acc << 1 | bit */
        uint32_t carry = (a->d[i / 32] >> (i % 32)) & 1;
        int n = acc->n;
        for (int k = 0; k < n; k++) {
            uint32_t v = acc->d[k];
            acc->d[k] = v << 1 | carry;
            carry = v >> 31;
        }
        if (carry && n < MJ_BN_LIMBS) acc->d[n++] = carry;
        acc->n = n;
        bn_norm(acc);
        if (mj_bn_cmp(acc, m) >= 0) mj_bn_sub(acc, acc, m);
    }
    *r = *acc;
    mj_free(acc);
}

/* Montgomery: numbers of k limbs, R = 2^(32k). */
typedef struct {
    int k;
    uint32_t m[MJ_BN_LIMBS];
    uint32_t minv;              /* -m^-1 mod 2^32 */
} mont_t;

static void mont_mul(const mont_t *M, uint32_t *r, const uint32_t *a, const uint32_t *b, uint32_t *t)
{
    int k = M->k;
    memset(t, 0, (size_t)(k + 2) * 4);
    for (int i = 0; i < k; i++) {
        uint64_t c = 0;
        uint32_t bi = b[i];
        for (int j = 0; j < k; j++) {
            c += (uint64_t)a[j] * bi + t[j];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        c += t[k];
        t[k] = (uint32_t)c;
        t[k + 1] = (uint32_t)(c >> 32);
        uint32_t q = t[0] * M->minv;
        c = (uint64_t)q * M->m[0] + t[0];
        c >>= 32;
        for (int j = 1; j < k; j++) {
            c += (uint64_t)q * M->m[j] + t[j];
            t[j - 1] = (uint32_t)c;
            c >>= 32;
        }
        c += t[k];
        t[k - 1] = (uint32_t)c;
        t[k] = t[k + 1] + (uint32_t)(c >> 32);
    }
    /* t < 2m: subtract m once if t >= m */
    bool ge = t[k] != 0;
    if (!ge) {
        ge = true;
        for (int j = k - 1; j >= 0; j--)
            if (t[j] != M->m[j]) {
                ge = t[j] > M->m[j];
                break;
            }
    }
    if (ge) {
        int64_t c = 0;
        for (int j = 0; j < k; j++) {
            c += (int64_t)t[j] - M->m[j];
            r[j] = (uint32_t)c;
            c = c < 0 ? -1 : 0;
        }
    } else {
        memcpy(r, t, (size_t)k * 4);
    }
}

bool mj_bn_modexp(mj_bn_t *r, const mj_bn_t *base, const mj_bn_t *exp, const mj_bn_t *m)
{
    if (m->n == 0 || !(m->d[0] & 1)) return false;
    int k = m->n;
    if (k > MJ_BN_LIMBS / 2) return false;
    mont_t *M = (mont_t *)mj_calloc(sizeof *M);
    uint32_t(*tab)[MJ_BN_LIMBS / 2] = (uint32_t(*)[MJ_BN_LIMBS / 2])mj_calloc(16 * sizeof *tab);
    uint32_t *acc = (uint32_t *)mj_calloc((size_t)(k + 2) * 4 * 3);
    mj_bn_t *x = (mj_bn_t *)mj_calloc(sizeof *x);
    if (!M || !tab || !acc || !x) {
        mj_free(M);
        mj_free(tab);
        mj_free(acc);
        mj_free(x);
        return false;
    }
    uint32_t *tmp = acc + (k + 2), *t = acc + 2 * (k + 2);
    M->k = k;
    memcpy(M->m, m->d, (size_t)k * 4);
    /* -m^-1 mod 2^32 by Newton's iteration */
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2 - m->d[0] * inv;
    M->minv = (uint32_t)(0u - inv);

    /* base * R mod m, and R mod m (one, in Montgomery form) */
    mj_bn_t *sh = (mj_bn_t *)mj_calloc(sizeof *sh);
    if (!sh) {
        mj_free(M);
        mj_free(tab);
        mj_free(acc);
        mj_free(x);
        return false;
    }
    mj_bn_mod(x, base, m);
    memset(sh, 0, sizeof *sh);
    for (int i = 0; i < x->n; i++) sh->d[i + k] = x->d[i];
    sh->n = x->n ? x->n + k : 0;
    mj_bn_mod(x, sh, m);
    memset(tab[1], 0, sizeof tab[1]);
    memcpy(tab[1], x->d, (size_t)x->n * 4);
    memset(sh, 0, sizeof *sh);
    sh->d[k] = 1;
    sh->n = k + 1;
    mj_bn_mod(x, sh, m);
    memset(tab[0], 0, sizeof tab[0]);
    memcpy(tab[0], x->d, (size_t)x->n * 4);
    mj_free(sh);
    for (int i = 2; i < 16; i++) mont_mul(M, tab[i], tab[i - 1], tab[1], t);

    memcpy(acc, tab[0], (size_t)k * 4);
    int bits = mj_bn_bits(exp);
    int top = (bits + 3) / 4 * 4;
    for (int i = top - 4; i >= 0; i -= 4) {
        for (int s = 0; s < 4; s++) {
            mont_mul(M, tmp, acc, acc, t);
            memcpy(acc, tmp, (size_t)k * 4);
        }
        uint32_t w = (exp->d[i / 32] >> (i % 32)) & 15;
        if (w) {
            mont_mul(M, tmp, acc, tab[w], t);
            memcpy(acc, tmp, (size_t)k * 4);
        }
    }
    /* out of Montgomery form: times 1 */
    uint32_t *one = tab[2];
    memset(one, 0, (size_t)k * 4);
    one[0] = 1;
    mont_mul(M, tmp, acc, one, t);
    mj_bn_zero(r);
    memcpy(r->d, tmp, (size_t)k * 4);
    r->n = k;
    bn_norm(r);
    crypto_wipe(tab, 16 * sizeof *tab);
    crypto_wipe(acc, (size_t)(k + 2) * 4 * 3);
    mj_free(M);
    mj_free(tab);
    mj_free(acc);
    mj_free(x);
    return true;
}

bool mj_modexp_be(uint8_t *out, size_t out_len, const uint8_t *b, size_t bl, const uint8_t *e, size_t el,
                  const uint8_t *m, size_t ml)
{
    mj_bn_t *v = (mj_bn_t *)mj_malloc(4 * sizeof(mj_bn_t));
    if (!v) return false;
    bool ok = mj_bn_from_be(&v[0], b, bl) && mj_bn_from_be(&v[1], e, el) && mj_bn_from_be(&v[2], m, ml) &&
              mj_bn_modexp(&v[3], &v[0], &v[1], &v[2]) && mj_bn_to_be(&v[3], out, out_len);
    crypto_wipe(v, 4 * sizeof(mj_bn_t));
    mj_free(v);
    return ok;
}

/* ------------------------------------------------------------------- pq */

static uint64_t mulmod64(uint64_t a, uint64_t b, uint64_t m)
{
    /* double and add, minding the carry: m may be up to 2^64 - 1 */
    uint64_t r = 0;
    a %= m;
    while (b) {
        if (b & 1) r = r >= m - a ? r - (m - a) : r + a;
        a = a >= m - a ? a - (m - a) : a + a;
        b >>= 1;
    }
    return r;
}

static uint64_t gcd64(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

bool mj_factor_pq(uint64_t pq, uint32_t *p, uint32_t *q)
{
    if (pq < 4) return false;
    if (!(pq & 1)) {
        *p = 2;
        *q = (uint32_t)(pq / 2);
        return true;
    }
    /* Pollard-Brent with a few different constants until one splits it */
    for (uint64_t c = 1; c < 30; c++) {
        uint64_t y = 2 + c, x = y, g = 1, r = 1, qq = 1, ys = y;
        const uint64_t mstep = 128;
        while (g == 1) {
            x = y;
            for (uint64_t i = 0; i < r; i++) y = (mulmod64(y, y, pq) + c) % pq;
            uint64_t k = 0;
            while (k < r && g == 1) {
                ys = y;
                uint64_t lim = mstep < r - k ? mstep : r - k;
                for (uint64_t i = 0; i < lim; i++) {
                    y = (mulmod64(y, y, pq) + c) % pq;
                    qq = mulmod64(qq, x > y ? x - y : y - x, pq);
                }
                g = gcd64(qq, pq);
                k += mstep;
            }
            r *= 2;
            if (r > (1u << 22)) break;
        }
        if (g == pq) {
            do {
                ys = (mulmod64(ys, ys, pq) + c) % pq;
                g = gcd64(x > ys ? x - ys : ys - x, pq);
            } while (g == 1);
        }
        if (g != 1 && g != pq) {
            uint64_t a = g, b = pq / g;
            if (a > b) {
                uint64_t t = a;
                a = b;
                b = t;
            }
            if (b >> 32) return false;
            *p = (uint32_t)a;
            *q = (uint32_t)b;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------- DH */

const uint8_t MJ_DH_PRIME[256] = {
    0xc7, 0x1c, 0xae, 0xb9, 0xc6, 0xb1, 0xc9, 0x04, 0x8e, 0x6c, 0x52, 0x2f, 0x70, 0xf1, 0x3f, 0x73, 0x98, 0x0d,
    0x40, 0x23, 0x8e, 0x3e, 0x21, 0xc1, 0x49, 0x34, 0xd0, 0x37, 0x56, 0x3d, 0x93, 0x0f, 0x48, 0x19, 0x8a, 0x0a,
    0xa7, 0xc1, 0x40, 0x58, 0x22, 0x94, 0x93, 0xd2, 0x25, 0x30, 0xf4, 0xdb, 0xfa, 0x33, 0x6f, 0x6e, 0x0a, 0xc9,
    0x25, 0x13, 0x95, 0x43, 0xae, 0xd4, 0x4c, 0xce, 0x7c, 0x37, 0x20, 0xfd, 0x51, 0xf6, 0x94, 0x58, 0x70, 0x5a,
    0xc6, 0x8c, 0xd4, 0xfe, 0x6b, 0x6b, 0x13, 0xab, 0xdc, 0x97, 0x46, 0x51, 0x29, 0x69, 0x32, 0x84, 0x54, 0xf1,
    0x8f, 0xaf, 0x8c, 0x59, 0x5f, 0x64, 0x24, 0x77, 0xfe, 0x96, 0xbb, 0x2a, 0x94, 0x1d, 0x5b, 0xcd, 0x1d, 0x4a,
    0xc8, 0xcc, 0x49, 0x88, 0x07, 0x08, 0xfa, 0x9b, 0x37, 0x8e, 0x3c, 0x4f, 0x3a, 0x90, 0x60, 0xbe, 0xe6, 0x7c,
    0xf9, 0xa4, 0xa4, 0xa6, 0x95, 0x81, 0x10, 0x51, 0x90, 0x7e, 0x16, 0x27, 0x53, 0xb5, 0x6b, 0x0f, 0x6b, 0x41,
    0x0d, 0xba, 0x74, 0xd8, 0xa8, 0x4b, 0x2a, 0x14, 0xb3, 0x14, 0x4e, 0x0e, 0xf1, 0x28, 0x47, 0x54, 0xfd, 0x17,
    0xed, 0x95, 0x0d, 0x59, 0x65, 0xb4, 0xb9, 0xdd, 0x46, 0x58, 0x2d, 0xb1, 0x17, 0x8d, 0x16, 0x9c, 0x6b, 0xc4,
    0x65, 0xb0, 0xd6, 0xff, 0x9c, 0xa3, 0x92, 0x8f, 0xef, 0x5b, 0x9a, 0xe4, 0xe4, 0x18, 0xfc, 0x15, 0xe8, 0x3e,
    0xbe, 0xa0, 0xf8, 0x7f, 0xa9, 0xff, 0x5e, 0xed, 0x70, 0x05, 0x0d, 0xed, 0x28, 0x49, 0xf4, 0x7b, 0xf9, 0x59,
    0xd9, 0x56, 0x85, 0x0c, 0xe9, 0x29, 0x85, 0x1f, 0x0d, 0x81, 0x15, 0xf6, 0x35, 0xb1, 0x05, 0xee, 0x2e, 0x4e,
    0x15, 0xd0, 0x4b, 0x24, 0x54, 0xbf, 0x6f, 0x4f, 0xad, 0xf0, 0x34, 0xb1, 0x04, 0x03, 0x11, 0x9c, 0xd8, 0xe3,
    0xb9, 0x2f, 0xcc, 0x5b,
};

bool mj_dh_check(int g, const uint8_t prime[256], const uint8_t *ga, size_t ga_len)
{
    if (g < 2 || g > 7) return false;
    if (memcmp(prime, MJ_DH_PRIME, 256) != 0) {
        mj_log("mtp: the server's DH prime is not the known one");
        return false;
    }
    /* the known prime is a safe prime, and of the conditions of
     * core.telegram.org/mtproto/auth_key it meets those of g = 3 (p mod 3 is
     * 2), 4 (none) and 7 (p mod 7 is 6), not those of 2, 5 or 6 (p mod 8 is
     * 3, p mod 5 is 3, p mod 24 is 11) */
    if (g != 3 && g != 4 && g != 7) return false;
    if (ga_len != 256 && ga_len > 256) return false;
    /* 2^1984 <= ga <= p - 2^1984: the top 8 bytes of a 256-byte ga are not
     * all zero, and p - ga is not smaller than 2^1984 */
    uint8_t v[256] = {0};
    memcpy(v + 256 - ga_len, ga, ga_len);
    bool hi = false;
    for (int i = 0; i < 8; i++) hi |= v[i] != 0;
    if (!hi) return false;
    /* p - ga, big-endian */
    uint8_t d[256];
    int borrow = 0;
    for (int i = 255; i >= 0; i--) {
        int x = (int)prime[i] - v[i] - borrow;
        borrow = x < 0;
        d[i] = (uint8_t)(x & 0xff);
    }
    if (borrow) return false;
    bool dhi = false;
    for (int i = 0; i < 8; i++) dhi |= d[i] != 0;
    return dhi;
}

/* ------------------------------------------------------- PBKDF2 and SRP */

void mj_pbkdf2_sha512(const uint8_t *pass, size_t pl, const uint8_t *salt, size_t sl, uint32_t rounds,
                      uint8_t *out, size_t out_len)
{
    /* the inner and outer states of the key are computed once and copied
     * for each of the 100 000 HMACs: two compressions per round instead of
     * four */
    crypto_sha512_ctx in, outer;
    uint8_t k[128] = {0}, pad[128];
    if (pl > 128) crypto_sha512(k, pass, pl);
    else memcpy(k, pass, pl);
    for (int i = 0; i < 128; i++) pad[i] = k[i] ^ 0x36;
    crypto_sha512_init(&in);
    crypto_sha512_update(&in, pad, 128);
    for (int i = 0; i < 128; i++) pad[i] = k[i] ^ 0x5c;
    crypto_sha512_init(&outer);
    crypto_sha512_update(&outer, pad, 128);

    for (uint32_t blk = 1; out_len; blk++) {
        uint8_t u[64], t[64];
        uint8_t be[4] = {(uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8), (uint8_t)blk};
        crypto_sha512_ctx c = in;
        crypto_sha512_update(&c, salt, sl);
        crypto_sha512_update(&c, be, 4);
        crypto_sha512_final(&c, u);
        c = outer;
        crypto_sha512_update(&c, u, 64);
        crypto_sha512_final(&c, u);
        memcpy(t, u, 64);
        for (uint32_t i = 1; i < rounds; i++) {
            c = in;
            crypto_sha512_update(&c, u, 64);
            crypto_sha512_final(&c, u);
            c = outer;
            crypto_sha512_update(&c, u, 64);
            crypto_sha512_final(&c, u);
            for (int j = 0; j < 64; j++) t[j] ^= u[j];
        }
        size_t n = out_len < 64 ? out_len : 64;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
    crypto_wipe(k, sizeof k);
    crypto_wipe(pad, sizeof pad);
    crypto_wipe(&in, sizeof in);
    crypto_wipe(&outer, sizeof outer);
}

/* SH(data, salt) = SHA256(salt | data | salt) */
static void sh(const uint8_t *d, size_t dn, const uint8_t *s, size_t sn, uint8_t out[32])
{
    uint8_t *b = (uint8_t *)mj_malloc(dn + 2 * sn);
    if (!b) {
        memset(out, 0, 32);
        return;
    }
    memcpy(b, s, sn);
    memcpy(b + sn, d, dn);
    memcpy(b + sn + dn, s, sn);
    mj_sha256(b, dn + 2 * sn, out);
    crypto_wipe(b, dn + 2 * sn);
    mj_free(b);
}

static void h2(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t out[32])
{
    mj_sha256_2(a, an, b, bn, out);
}

bool mj_srp(const char *password, const uint8_t *salt1, size_t s1, const uint8_t *salt2, size_t s2, int g,
            const uint8_t p[256], const uint8_t B[256], const uint8_t a[256], uint8_t A_out[256],
            uint8_t M1_out[32])
{
    bool ok = false;
    enum { BP, BG, BB, BX, BA, BV, BK, BU, BT, BE, BS, BTMP, BN };
    mj_bn_t *v = (mj_bn_t *)mj_calloc(BN * sizeof(mj_bn_t));
    uint8_t *buf = (uint8_t *)mj_malloc(2048);
    if (!v || !buf) goto out;

    /* x = PH2(password, salt1, salt2) */
    uint8_t ph[32], x[64];
    sh((const uint8_t *)password, strlen(password), salt1, s1, ph);
    sh(ph, 32, salt2, s2, ph);
    mj_pbkdf2_sha512(ph, 32, salt1, s1, 100000, x, 64);
    sh(x, 64, salt2, s2, ph);
    uint8_t gb[256] = {0};
    gb[255] = (uint8_t)g;

    mj_bn_from_be(&v[BP], p, 256);
    mj_bn_from_be(&v[BG], gb, 256);
    mj_bn_from_be(&v[BB], B, 256);
    mj_bn_from_be(&v[BX], ph, 32);
    mj_bn_from_be(&v[BA], a, 256);
    if (mj_bn_is_zero(&v[BB]) || mj_bn_cmp(&v[BB], &v[BP]) >= 0) goto out;

    /* v = g^x, A = g^a */
    if (!mj_bn_modexp(&v[BV], &v[BG], &v[BX], &v[BP])) goto out;
    if (!mj_bn_modexp(&v[BTMP], &v[BG], &v[BA], &v[BP])) goto out;
    mj_bn_to_be(&v[BTMP], A_out, 256);

    /* k = H(p | g), u = H(A | B) */
    uint8_t kh[32], uh[32];
    h2(p, 256, gb, 256, kh);
    h2(A_out, 256, B, 256, uh);
    mj_bn_from_be(&v[BK], kh, 32);
    mj_bn_from_be(&v[BU], uh, 32);
    if (mj_bn_is_zero(&v[BU])) goto out;

    /* t = (B - k*v) mod p, kept positive */
    mj_bn_mul(&v[BTMP], &v[BK], &v[BV]);
    mj_bn_mod(&v[BTMP], &v[BTMP], &v[BP]);
    if (mj_bn_cmp(&v[BB], &v[BTMP]) >= 0) {
        mj_bn_sub(&v[BT], &v[BB], &v[BTMP]);
    } else {
        mj_bn_add(&v[BT], &v[BB], &v[BP]);
        mj_bn_sub(&v[BT], &v[BT], &v[BTMP]);
    }
    /* S = t^(a + u*x) mod p */
    mj_bn_mul(&v[BE], &v[BU], &v[BX]);
    mj_bn_add(&v[BE], &v[BE], &v[BA]);
    if (!mj_bn_modexp(&v[BS], &v[BT], &v[BE], &v[BP])) goto out;
    uint8_t sb[256], ka[32];
    mj_bn_to_be(&v[BS], sb, 256);
    mj_sha256(sb, 256, ka);

    /* M1 = H(H(p) xor H(g) | H(salt1) | H(salt2) | A | B | K) */
    uint8_t hp[32], hg[32];
    mj_sha256(p, 256, hp);
    mj_sha256(gb, 256, hg);
    for (int i = 0; i < 32; i++) buf[i] = hp[i] ^ hg[i];
    mj_sha256(salt1, s1, buf + 32);
    mj_sha256(salt2, s2, buf + 64);
    memcpy(buf + 96, A_out, 256);
    memcpy(buf + 352, B, 256);
    memcpy(buf + 608, ka, 32);
    mj_sha256(buf, 640, M1_out);
    ok = true;
    crypto_wipe(ph, sizeof ph);
    crypto_wipe(x, sizeof x);
    crypto_wipe(sb, sizeof sb);
    crypto_wipe(ka, sizeof ka);
out:
    if (v) crypto_wipe(v, BN * sizeof(mj_bn_t));
    mj_free(v);
    mj_free(buf);
    return ok;
}
