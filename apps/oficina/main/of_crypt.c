/*
 * P4OS - Office: the hashes and ciphers PDF's standard security handler
 * needs (of_crypt.h): MD5 and RC4 (revisions 2 to 4), AES-128 and AES-256
 * in CBC (4 to 6), SHA-256, SHA-384 and SHA-512 (5 and 6). The firmware
 * gives apps none of these. Small, plain, checked against Python's hashlib
 * and a known AES vector by tools/host_test.c ("crypt").
 */
#include "of_crypt.h"

#include <string.h>

/* -------------------------------------------------------------------------- */
/* MD5 (RFC 1321)                                                              */
/* -------------------------------------------------------------------------- */

#define ROL32(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

static void md5_block(uint32_t h[4], const uint8_t *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + ROL32(a + f + K[i] + w[g], R[i]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

void of_md5_init(of_md5_t *m)
{
    m->h[0] = 0x67452301; m->h[1] = 0xefcdab89; m->h[2] = 0x98badcfe; m->h[3] = 0x10325476;
    m->n = 0;
    m->used = 0;
}

void of_md5_update(of_md5_t *m, const void *data, size_t n)
{
    const uint8_t *p = data;
    m->n += n;
    while (n) {
        size_t k = 64 - m->used;
        if (k > n) k = n;
        memcpy(m->buf + m->used, p, k);
        m->used += k;
        p += k;
        n -= k;
        if (m->used == 64) { md5_block(m->h, m->buf); m->used = 0; }
    }
}

void of_md5_final(of_md5_t *m, uint8_t out[16])
{
    uint64_t bits = m->n * 8;
    uint8_t pad = 0x80;
    of_md5_update(m, &pad, 1);
    pad = 0;
    while (m->used != 56) of_md5_update(m, &pad, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (8 * i));
    of_md5_update(m, l, 8);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(m->h[i] >> (8 * k));
}

void of_md5(const void *data, size_t n, uint8_t out[16])
{
    of_md5_t m;
    of_md5_init(&m);
    of_md5_update(&m, data, n);
    of_md5_final(&m, out);
}

/* -------------------------------------------------------------------------- */
/* RC4                                                                         */
/* -------------------------------------------------------------------------- */

void of_rc4(const uint8_t *key, size_t klen, uint8_t *data, size_t n)
{
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + s[i] + key[i % klen]) & 255;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
    }
    for (size_t k = 0, i = 0, j = 0; k < n; k++) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
        data[k] ^= s[(s[i] + s[j]) & 255];
    }
}

/* -------------------------------------------------------------------------- */
/* AES (FIPS 197), the tables worked out once                                  */
/* -------------------------------------------------------------------------- */

static uint8_t SBOX[256], INV[256];
static bool s_aes_ready;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

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
    if (s_aes_ready) return;
    /* the inverse in GF(2^8), then the affine map */
    for (int i = 0; i < 256; i++) {
        uint8_t inv = 0;
        if (i) for (int k = 1; k < 256; k++) if (gmul((uint8_t)i, (uint8_t)k) == 1) { inv = (uint8_t)k; break; }
        uint8_t s = inv;
        uint8_t x = inv;
        for (int r = 0; r < 4; r++) { x = (uint8_t)((x << 1) | (x >> 7)); s ^= x; }
        s ^= 0x63;
        SBOX[i] = s;
        INV[s] = (uint8_t)i;
    }
    s_aes_ready = true;
}

void of_aes_init(of_aes_t *a, const uint8_t *key, int bits)
{
    aes_tables();
    int nk = bits / 32;
    a->rounds = nk + 6;
    int total = 4 * (a->rounds + 1);
    for (int i = 0; i < nk; i++) memcpy(&a->rk[i * 4], key + i * 4, 4);
    uint8_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint8_t t[4];
        memcpy(t, &a->rk[(i - 1) * 4], 4);
        if (i % nk == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon); t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[u];
            rcon = xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++) t[k] = SBOX[t[k]];
        }
        for (int k = 0; k < 4; k++) a->rk[i * 4 + k] = a->rk[(i - nk) * 4 + k] ^ t[k];
    }
}

static void add_key(uint8_t *s, const uint8_t *k) { for (int i = 0; i < 16; i++) s[i] ^= k[i]; }

void of_aes_encrypt(const of_aes_t *a, uint8_t b[16])
{
    add_key(b, a->rk);
    for (int r = 1; r <= a->rounds; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = SBOX[b[i]];
        /* shift rows: column c row r takes from column c+r */
        for (int c = 0; c < 4; c++)
            for (int w = 0; w < 4; w++) b[c * 4 + w] = t[((c + w) & 3) * 4 + w];
        if (r != a->rounds)
            for (int c = 0; c < 4; c++) {
                uint8_t *p = b + c * 4, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                p[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                p[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                p[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        add_key(b, a->rk + r * 16);
    }
}

void of_aes_decrypt(const of_aes_t *a, uint8_t b[16])
{
    add_key(b, a->rk + a->rounds * 16);
    for (int r = a->rounds - 1; r >= 0; r--) {
        uint8_t t[16];
        memcpy(t, b, 16);
        for (int c = 0; c < 4; c++)
            for (int w = 0; w < 4; w++) b[((c + w) & 3) * 4 + w] = INV[t[c * 4 + w]];
        add_key(b, a->rk + r * 16);
        if (r)
            for (int c = 0; c < 4; c++) {
                uint8_t *p = b + c * 4, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                p[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                p[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
    }
}

size_t of_aes_cbc_decrypt(const uint8_t *key, int bits, const uint8_t *iv, uint8_t *data, size_t n, bool unpad)
{
    of_aes_t a;
    of_aes_init(&a, key, bits);
    uint8_t prev[16], cur[16];
    memcpy(prev, iv, 16);
    n &= ~(size_t)15;
    for (size_t i = 0; i < n; i += 16) {
        memcpy(cur, data + i, 16);
        of_aes_decrypt(&a, data + i);
        for (int k = 0; k < 16; k++) data[i + k] ^= prev[k];
        memcpy(prev, cur, 16);
    }
    if (unpad && n) {
        uint8_t p = data[n - 1];
        if (p >= 1 && p <= 16) n -= p;
    }
    return n;
}

void of_aes_cbc_encrypt(const uint8_t *key, int bits, const uint8_t *iv, uint8_t *data, size_t n)
{
    of_aes_t a;
    of_aes_init(&a, key, bits);
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i + 16 <= n; i += 16) {
        for (int k = 0; k < 16; k++) data[i + k] ^= prev[k];
        of_aes_encrypt(&a, data + i);
        memcpy(prev, data + i, 16);
    }
}

/* -------------------------------------------------------------------------- */
/* SHA-256 and SHA-512/384 (FIPS 180-4)                                        */
/* -------------------------------------------------------------------------- */

#define ROR32(x, c) (((x) >> (c)) | ((x) << (32 - (c))))
#define ROR64(x, c) (((x) >> (c)) | ((x) << (64 - (c))))

void of_sha256(const void *data, size_t n, uint8_t out[32])
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = data;
    size_t total = n;
    uint8_t blk[64];
    bool done = false, padded = false;
    while (!done) {
        size_t k = n >= 64 ? 64 : n;
        memcpy(blk, p, k);
        p += k;
        n -= k;
        if (k < 64) {
            if (!padded) { blk[k++] = 0x80; padded = true; }
            if (k > 56) { memset(blk + k, 0, 64 - k); }
            else {
                memset(blk + k, 0, 56 - k);
                uint64_t bits = (uint64_t)total * 8;
                for (int i = 0; i < 8; i++) blk[63 - i] = (uint8_t)(bits >> (8 * i));
                done = true;
            }
        }
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[i * 4] << 24 | (uint32_t)blk[i * 4 + 1] << 16 | (uint32_t)blk[i * 4 + 2] << 8 | blk[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(h[i] >> (24 - 8 * k));
}

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };

static void sha512_core(const void *data, size_t n, uint64_t h[8])
{
    const uint8_t *p = data;
    size_t total = n;
    uint8_t blk[128];
    bool done = false, padded = false;
    while (!done) {
        size_t k = n >= 128 ? 128 : n;
        memcpy(blk, p, k);
        p += k;
        n -= k;
        if (k < 128) {
            if (!padded) { blk[k++] = 0x80; padded = true; }
            if (k > 112) { memset(blk + k, 0, 128 - k); }
            else {
                memset(blk + k, 0, 120 - k);
                uint64_t bits = (uint64_t)total * 8;
                for (int i = 0; i < 8; i++) blk[127 - i] = (uint8_t)(bits >> (8 * i));
                done = true;
            }
        }
        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int k2 = 0; k2 < 8; k2++) w[i] = w[i] << 8 | blk[i * 8 + k2];
        }
        for (int i = 16; i < 80; i++) {
            uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; i++) {
            uint64_t t1 = hh + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
            uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
}

void of_sha512(const void *data, size_t n, uint8_t out[64])
{
    uint64_t h[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                      0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    sha512_core(data, n, h);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 8; k++) out[i * 8 + k] = (uint8_t)(h[i] >> (56 - 8 * k));
}

void of_sha384(const void *data, size_t n, uint8_t out[48])
{
    uint64_t h[8] = { 0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                      0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL };
    sha512_core(data, n, h);
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 8; k++) out[i * 8 + k] = (uint8_t)(h[i] >> (56 - 8 * k));
}
