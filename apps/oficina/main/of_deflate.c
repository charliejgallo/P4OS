/*
 * P4OS - Office: deflate and inflate.
 *
 * Deflate is Dibujo's PNG compressor (apps/dibujo/main/dib_png.c), cut down
 * to a whole buffer in memory: LZ77 over hash chains and deflate's fixed
 * Huffman codes, one block. The XML of a sheet comes out at about a sixth of
 * its size, near enough to what zlib's level 1 gives, for 150 lines.
 *
 * Inflate is miniz's tinfl (MIT, third/tinfl.c, as the Emulador carries it),
 * with its name prefixed: in the simulator every app is one program.
 */
#include "of.h"

#define tinfl_decompress of_tinfl_decompress
#include "../third/tinfl.h"

#include <string.h>

#define WSIZE       32768
#define WMASK       (WSIZE - 1)
#define HBITS       15
#define HSIZE       (1 << HBITS)
#define MIN_MATCH   3
#define MAX_MATCH   258
#define MAX_CHAIN   32
#define GOOD_LEN    64

static const uint16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577 };
static const uint8_t DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

typedef struct {
    of_sb_t  *out;
    uint32_t  bits;
    int       nbits;
    uint32_t *head;             /* hash -> position + 1   */
    uint32_t *prev;             /* position & WMASK -> previous + 1 */
    uint8_t   lcode[MAX_MATCH + 1];
} z_t;

static void put_bits(z_t *z, uint32_t v, int n)
{
    z->bits |= v << z->nbits;
    z->nbits += n;
    while (z->nbits >= 8) {
        of_sb_putc(z->out, (char)(z->bits & 0xFF));
        z->bits >>= 8;
        z->nbits -= 8;
    }
}

static uint32_t rev(uint32_t v, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

static void put_sym(z_t *z, int s)
{
    if (s < 144) put_bits(z, rev(0x30 + s, 8), 8);
    else if (s < 256) put_bits(z, rev(0x190 + s - 144, 9), 9);
    else if (s < 280) put_bits(z, rev(s - 256, 7), 7);
    else put_bits(z, rev(0xC0 + s - 280, 8), 8);
}

static void put_match(z_t *z, int len, int dist)
{
    int li = z->lcode[len];
    put_sym(z, 257 + li);
    if (LEXT[li]) put_bits(z, len - LBASE[li], LEXT[li]);
    int di = 29;
    while (DBASE[di] > dist) di--;
    put_bits(z, rev(di, 5), 5);
    if (DEXT[di]) put_bits(z, dist - DBASE[di], DEXT[di]);
}

static inline uint32_t hash3(const uint8_t *p)
{
    return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & (HSIZE - 1);
}

bool of_deflate(const uint8_t *in, size_t n, of_sb_t *out)
{
    z_t z = { .out = out };
    z.head = of_alloc(HSIZE * sizeof(uint32_t));
    z.prev = of_alloc(WSIZE * sizeof(uint32_t));
    if (!z.head || !z.prev) {
        of_free(z.head);
        of_free(z.prev);
        return false;
    }
    for (int i = 0, l = 3; l <= MAX_MATCH; l++) {
        while (i < 28 && LBASE[i + 1] <= l) i++;
        z.lcode[l] = (uint8_t)i;
    }
    put_bits(&z, 1, 1);         /* the last block */
    put_bits(&z, 1, 2);         /* fixed codes    */
    size_t i = 0;
    while (i < n) {
        size_t avail = n - i;
        int best = 0;
        uint32_t bdist = 0;
        if (avail >= MIN_MATCH) {
            uint32_t cand = z.head[hash3(in + i)];
            int maxlen = avail < MAX_MATCH ? (int)avail : MAX_MATCH;
            for (int chain = 0; cand && chain < MAX_CHAIN; chain++) {
                uint32_t c = cand - 1;
                if (i - c > WSIZE - 1) break;
                const uint8_t *a = in + i, *b = in + c;
                if (b[best] == a[best] && b[0] == a[0]) {
                    int l = 0;
                    while (l < maxlen && a[l] == b[l]) l++;
                    if (l > best) {
                        best = l;
                        bdist = (uint32_t)(i - c);
                        if (l >= GOOD_LEN || l == maxlen) break;
                    }
                }
                uint32_t nx = z.prev[c & WMASK];
                if (nx >= cand) break;
                cand = nx;
            }
        }
        int adv = best >= MIN_MATCH ? best : 1;
        if (best >= MIN_MATCH) put_match(&z, best, (int)bdist);
        else put_sym(&z, in[i]);
        for (int k = 0; k < adv; k++) {
            size_t p = i + (size_t)k;
            if (p + MIN_MATCH <= n) {
                uint32_t h = hash3(in + p);
                z.prev[p & WMASK] = z.head[h];
                z.head[h] = (uint32_t)p + 1;
            }
        }
        i += (size_t)adv;
    }
    put_sym(&z, 256);
    if (z.nbits) put_bits(&z, 0, 8 - z.nbits);
    of_free(z.head);
    of_free(z.prev);
    return !out->oom;
}

uint8_t *of_inflate(const uint8_t *in, size_t n, size_t expect, size_t *out_len)
{
    tinfl_decompressor *d = of_alloc(sizeof *d);
    size_t cap = expect ? expect + 1 : n * 4 + 1024;
    uint8_t *o = of_alloc(cap + 1);
    if (!d || !o) {
        of_free(d);
        of_free(o);
        return NULL;
    }
    tinfl_init(d);
    size_t in_pos = 0, out_pos = 0;
    for (;;) {
        size_t in_n = n - in_pos, out_n = cap - out_pos;
        tinfl_status st = tinfl_decompress(d, in + in_pos, &in_n, o, o + out_pos, &out_n,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                                           (in_pos + in_n < n ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        in_pos += in_n;
        out_pos += out_n;
        if (st == TINFL_STATUS_DONE) break;
        if (st == TINFL_STATUS_HAS_MORE_OUTPUT) {
            size_t nc = cap * 2;
            uint8_t *no = of_realloc(o, nc + 1);
            if (!no) { st = TINFL_STATUS_FAILED; }
            else { o = no; cap = nc; continue; }
        }
        of_free(d);
        of_free(o);
        return NULL;
    }
    of_free(d);
    o[out_pos] = 0;
    if (out_len) *out_len = out_pos;
    return o;
}

/* For PDF: a zlib or raw stream, and whatever came out before an error (a
 * damaged or cut stream still gives its text); NULL only if nothing did. */
uint8_t *of_inflate_lax(const uint8_t *in, size_t n, bool zlib, size_t *out_len)
{
    tinfl_decompressor *d = of_alloc(sizeof *d);
    size_t cap = n * 4 + 1024;
    uint8_t *o = of_alloc(cap + 1);
    if (!d || !o) {
        of_free(d);
        of_free(o);
        return NULL;
    }
    tinfl_init(d);
    size_t in_pos = 0, out_pos = 0;
    for (;;) {
        size_t in_n = n - in_pos, out_n = cap - out_pos;
        tinfl_status st = tinfl_decompress(d, in + in_pos, &in_n, o, o + out_pos, &out_n,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (zlib ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0) |
                                           (in_pos + in_n < n ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        in_pos += in_n;
        out_pos += out_n;
        if (st == TINFL_STATUS_HAS_MORE_OUTPUT) {
            size_t nc = cap * 2;
            uint8_t *no = of_realloc(o, nc + 1);
            if (!no) break;
            o = no;
            cap = nc;
            continue;
        }
        break;          /* done, or an error: keep what there is */
    }
    of_free(d);
    if (!out_pos) { of_free(o); return NULL; }
    o[out_pos] = 0;
    if (out_len) *out_len = out_pos;
    return o;
}
