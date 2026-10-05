/*
 * P4OS - EEPROM: CRC-32, MD5 and SHA-256 of an image, in one pass.
 *
 * Written out here because the firmware's table exports none of mbedTLS's
 * digests to apps, and an image is small enough that the plain C versions
 * are quick: a 128 KB chip is a few milliseconds, a 16 MB flash about a
 * second. CRC-32 is the zlib one (what `crc32`, 7-Zip and the portal's page
 * print); MD5 is RFC 1321 and SHA-256 FIPS 180-4.
 */
#include "ee.h"

#include <stdio.h>
#include <string.h>

/* ---- CRC-32, reflected 0xEDB88320, a nibble at a time ---- */

static const uint32_t CRC_NIB[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ CRC_NIB[crc & 15];
        crc = (crc >> 4) ^ CRC_NIB[crc & 15];
    }
    return crc;
}

/* ---- MD5 ---- */

#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t MD5_R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(uint32_t h[4], const uint8_t *b)
{
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (bb & c) | (~bb & d); g = i; }
        else if (i < 32) { f = (d & bb) | (~d & c);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = bb ^ c ^ d;           g = (3 * i + 5) & 15; }
        else             { f = c ^ (bb | ~d);        g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = bb;
        uint32_t x = a + f + MD5_K[i] + w[g];
        bb = bb + ROL(x, MD5_R[i]);
        a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
}

/* ---- SHA-256 ---- */

#define ROR(x, c) (((x) >> (c)) | ((x) << (32 - (c))))

static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha_block(uint32_t h[8], const uint8_t *b)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---- the three at once ---- */

void ee_hash_init(ee_hash_t *h)
{
    memset(h, 0, sizeof *h);
    h->crc = 0xFFFFFFFFu;
    static const uint32_t M0[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    static const uint32_t S0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(h->md5, M0, sizeof M0);
    memcpy(h->sha, S0, sizeof S0);
}

void ee_hash_update(ee_hash_t *h, const uint8_t *p, size_t n)
{
    h->crc = crc_update(h->crc, p, n);
    h->len += n;
    /* MD5 and SHA-256 share the 64-byte block; each keeps its own tail */
    while (n) {
        if (h->mn == 0 && n >= 64) {
            md5_block(h->md5, p);
            sha_block(h->sha, p);
            p += 64;
            n -= 64;
            continue;
        }
        size_t k = 64 - h->mn;
        if (k > n) k = n;
        memcpy(h->mbuf + h->mn, p, k);
        h->mn += (uint32_t)k;
        p += k;
        n -= k;
        if (h->mn == 64) {
            md5_block(h->md5, h->mbuf);
            sha_block(h->sha, h->mbuf);
            h->mn = 0;
        }
    }
}

void ee_hash_final(ee_hash_t *h, ee_sums_t *out)
{
    /* the length in bits as two halves: a variable shift of a 64-bit value
     * is a libgcc call (__lshrdi3) the firmware does not export */
    uint64_t bits = h->len * 8;
    uint32_t lo = (uint32_t)bits, hi = (uint32_t)(bits >> 32);
    uint8_t pad[72];
    size_t padn = (h->mn < 56 ? 56 : 120) - h->mn;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    /* the same padding for both, but MD5 wants the length little-endian and
     * SHA-256 big-endian: finish them one at a time on copies of the tail */
    uint8_t blk[128];
    size_t tn = h->mn + padn;
    memcpy(blk, h->mbuf, h->mn);
    memcpy(blk + h->mn, pad, padn);
    for (int i = 0; i < 8; i++) blk[tn + i] = (uint8_t)((i < 4 ? lo : hi) >> (8 * (i & 3)));
    uint32_t m[4];
    memcpy(m, h->md5, sizeof m);
    for (size_t o = 0; o < tn + 8; o += 64) md5_block(m, blk + o);
    for (int i = 0; i < 8; i++) blk[tn + i] = (uint8_t)((i < 4 ? hi : lo) >> (24 - 8 * (i & 3)));
    uint32_t s[8];
    memcpy(s, h->sha, sizeof s);
    for (size_t o = 0; o < tn + 8; o += 64) sha_block(s, blk + o);

    out->crc = h->crc ^ 0xFFFFFFFFu;
    for (int i = 0; i < 16; i++) snprintf(out->md5 + 2 * i, 3, "%02x", (unsigned)(m[i / 4] >> (8 * (i % 4))) & 0xFF);
    for (int i = 0; i < 32; i++) snprintf(out->sha + 2 * i, 3, "%02x", (unsigned)(s[i / 4] >> (24 - 8 * (i % 4))) & 0xFF);
}

void ee_sums_of(const uint8_t *p, size_t n, ee_sums_t *out)
{
    ee_hash_t h;
    ee_hash_init(&h);
    ee_hash_update(&h, p, n);
    ee_hash_final(&h, out);
}
