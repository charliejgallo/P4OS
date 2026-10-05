/*
 * P4OS - VNC viewer: DES, for the classic VNC password (RFB security type 2).
 *
 * Written for P4OS from the standard (FIPS 46-3): the tables below are the
 * standard's, the code is ours. The one copy of D3DES at hand (Outerbridge's
 * public-domain DES as VNC ships it) carries AT&T's changes under the GPL, so
 * it was not vendored.
 *
 * Speed does not matter here: one 16-byte challenge per connection. So the
 * bits are spread one per byte and every permutation is a table walk; no
 * 64-bit shifts, which on the P4's RV32 would be calls into libgcc.
 *
 * VNC's twist: the key is the password, up to 8 characters padded with
 * zeros, with the bits of each byte mirrored (VNC's D3DES reversed its
 * bytebit[] table, and every server since expects that).
 */
#include "vnc_des.h"

#include <string.h>

static const uint8_t IP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
};

static const uint8_t FP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25,
};

static const uint8_t E[48] = {
    32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,
    8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1,
};

static const uint8_t P[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
    2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25,
};

static const uint8_t PC1[56] = {
    57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
    10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
    14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4,
};

static const uint8_t PC2[48] = {
    14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10,
    23, 19, 12, 4,  26, 8,  16, 7,  27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32,
};

static const uint8_t SHIFTS[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };

static const uint8_t S[8][64] = {
    { 14, 4,  13, 1,  2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0,  7,
      0,  15, 7,  4,  14, 2,  13, 1,  10, 6,  12, 11, 9,  5,  3,  8,
      4,  1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,  3,  10, 5,  0,
      15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0,  6,  13 },
    { 15, 1,  8,  14, 6,  11, 3,  4,  9,  7,  2,  13, 12, 0,  5,  10,
      3,  13, 4,  7,  15, 2,  8,  14, 12, 0,  1,  10, 6,  9,  11, 5,
      0,  14, 7,  11, 10, 4,  13, 1,  5,  8,  12, 6,  9,  3,  2,  15,
      13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0,  5,  14, 9 },
    { 10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,
      13, 7,  0,  9,  3,  4,  6,  10, 2,  8,  5,  14, 12, 11, 15, 1,
      13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,
      1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12 },
    { 7,  13, 14, 3,  0,  6,  9,  10, 1,  2,  8,  5,  11, 12, 4,  15,
      13, 8,  11, 5,  6,  15, 0,  3,  4,  7,  2,  12, 1,  10, 14, 9,
      10, 6,  9,  0,  12, 11, 7,  13, 15, 1,  3,  14, 5,  2,  8,  4,
      3,  15, 0,  6,  10, 1,  13, 8,  9,  4,  5,  11, 12, 7,  2,  14 },
    { 2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0,  14, 9,
      14, 11, 2,  12, 4,  7,  13, 1,  5,  0,  15, 10, 3,  9,  8,  6,
      4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3,  0,  14,
      11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3 },
    { 12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11,
      10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14, 0,  11, 3,  8,
      9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,
      4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13 },
    { 4,  11, 2,  14, 15, 0,  8,  13, 3,  12, 9,  7,  5,  10, 6,  1,
      13, 0,  11, 7,  4,  9,  1,  10, 14, 3,  5,  12, 2,  15, 8,  6,
      1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,  0,  5,  9,  2,
      6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12 },
    { 13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,
      1,  15, 13, 8,  10, 3,  7,  4,  12, 5,  6,  11, 0,  14, 9,  2,
      7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,
      2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11 },
};

/* out[i] = in[table[i] - 1]: the standard counts bits from 1, MSB first */
static void permute(uint8_t *out, const uint8_t *in, const uint8_t *table, int n)
{
    for (int i = 0; i < n; i++) out[i] = in[table[i] - 1];
}

static void to_bits(uint8_t *bits, const uint8_t *bytes, int n)
{
    for (int i = 0; i < n * 8; i++) bits[i] = (bytes[i >> 3] >> (7 - (i & 7))) & 1;
}

static void from_bits(uint8_t *bytes, const uint8_t *bits, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) b = (uint8_t)(b << 1 | bits[i * 8 + k]);
        bytes[i] = b;
    }
}

static void rotate28(uint8_t *h, int n)
{
    uint8_t t[28];
    for (int i = 0; i < 28; i++) t[i] = h[(i + n) % 28];
    memcpy(h, t, 28);
}

/* One 8-byte block, encrypted with the 8-byte key as the standard has it. */
void vnc_des_block(const uint8_t key[8], const uint8_t in[8], uint8_t out[8])
{
    uint8_t kb[64], cd[56], sub[16][48];
    to_bits(kb, key, 8);
    permute(cd, kb, PC1, 56);
    for (int r = 0; r < 16; r++) {
        rotate28(cd, SHIFTS[r]);
        rotate28(cd + 28, SHIFTS[r]);
        permute(sub[r], cd, PC2, 48);
    }

    uint8_t b[64], lr[64];
    to_bits(b, in, 8);
    permute(lr, b, IP, 64);
    uint8_t *l = lr, *rr = lr + 32;
    for (int r = 0; r < 16; r++) {
        uint8_t e[48], f[32], s[32];
        permute(e, rr, E, 48);
        for (int i = 0; i < 48; i++) e[i] ^= sub[r][i];
        for (int k = 0; k < 8; k++) {
            const uint8_t *x = e + k * 6;
            int row = x[0] << 1 | x[5];
            int col = x[1] << 3 | x[2] << 2 | x[3] << 1 | x[4];
            int v = S[k][row * 16 + col];
            for (int j = 0; j < 4; j++) s[k * 4 + j] = (v >> (3 - j)) & 1;
        }
        permute(f, s, P, 32);
        for (int i = 0; i < 32; i++) {
            uint8_t t = rr[i];
            rr[i] = l[i] ^ f[i];
            l[i] = t;
        }
    }
    /* the last round does not swap: R16 L16 goes into the final permutation */
    uint8_t pre[64];
    memcpy(pre, rr, 32);
    memcpy(pre + 32, l, 32);
    permute(b, pre, FP, 64);
    from_bits(out, b, 8);
}

static uint8_t mirror(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r = (uint8_t)(r << 1 | ((b >> i) & 1));
    return r;
}

void vnc_des_response(const char *password, const uint8_t challenge[16], uint8_t response[16])
{
    uint8_t key[8] = { 0 };
    for (int i = 0; i < 8 && password && password[i]; i++) key[i] = mirror((uint8_t)password[i]);
    vnc_des_block(key, challenge, response);
    vnc_des_block(key, challenge + 8, response + 8);
}
