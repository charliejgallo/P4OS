/*
 * DIBUJO - a PNG writer. See dib_png.h.
 */
#include "dib_png.h"
#include "dib_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WSIZE       32768
#define WMASK       (WSIZE - 1)
#define HBITS       15
#define HSIZE       (1 << HBITS)
#define MIN_MATCH   3
#define MAX_MATCH   258
#define LOOKAHEAD   (MAX_MATCH + MIN_MATCH + 1)
#define MAX_CHAIN   24
#define GOOD_LEN    64              /* a match this long stops the search */
#define IDAT_MAX    65536

static const uint16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577 };
static const uint8_t DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

uint32_t dib_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

typedef struct {
    FILE    *f;
    bool     ok;
    /* the IDAT being filled */
    uint8_t *out;
    size_t   on;
    uint32_t bits;
    int      nbits;
    /* the zlib stream */
    uint32_t a1, a2;                /* Adler-32 */
    /* LZ77 */
    uint8_t *win;                   /* 2 * WSIZE + a row, at least        */
    size_t   wcap;
    size_t   wlen;                  /* bytes in win                       */
    size_t   pos;                   /* the next one to code, in win       */
    uint32_t base;                  /* the stream offset of win[0]        */
    uint32_t *head;                 /* hash -> stream offset + 1          */
    uint32_t *prev;                 /* offset & WMASK -> previous + 1     */
    uint16_t lcode[MAX_MATCH + 1];  /* length -> its index in LBASE       */
} z_t;

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}

static void chunk(z_t *z, const char *type, const uint8_t *data, size_t n)
{
    if (!z->ok) return;
    uint8_t hd[8];
    put_be32(hd, (uint32_t)n);
    memcpy(hd + 4, type, 4);
    uint32_t crc = dib_crc32(0, (const uint8_t *)type, 4);
    crc = dib_crc32(crc, data, n);
    uint8_t tl[4];
    put_be32(tl, crc);
    z->ok = fwrite(hd, 1, 8, z->f) == 8 && (n == 0 || fwrite(data, 1, n, z->f) == n) &&
            fwrite(tl, 1, 4, z->f) == 4;
}

static void out_byte(z_t *z, uint8_t b)
{
    z->out[z->on++] = b;
    if (z->on == IDAT_MAX) {
        chunk(z, "IDAT", z->out, z->on);
        z->on = 0;
    }
}

static void put_bits(z_t *z, uint32_t v, int n)
{
    z->bits |= v << z->nbits;
    z->nbits += n;
    while (z->nbits >= 8) {
        out_byte(z, z->bits & 0xFF);
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

/* deflate's fixed literal/length code, sent most significant bit first */
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

static inline void insert(z_t *z, size_t i)
{
    uint32_t h = hash3(z->win + i), abs = z->base + (uint32_t)i;
    z->prev[abs & WMASK] = z->head[h];
    z->head[h] = abs + 1;
}

/* Codes what is in the window, leaving LOOKAHEAD bytes unless flushing. */
static void compress(z_t *z, bool flush)
{
    size_t lim = flush ? z->wlen : (z->wlen > LOOKAHEAD ? z->wlen - LOOKAHEAD : 0);
    while (z->pos < lim) {
        size_t i = z->pos, avail = z->wlen - i;
        int best = 0;
        uint32_t bdist = 0;
        if (avail >= MIN_MATCH) {
            uint32_t abs = z->base + (uint32_t)i;
            uint32_t cand = z->head[hash3(z->win + i)];
            int maxlen = avail < MAX_MATCH ? (int)avail : MAX_MATCH;
            for (int chain = 0; cand && chain < MAX_CHAIN; chain++) {
                uint32_t c = cand - 1;
                if (abs - c > WSIZE || c < z->base) break;
                const uint8_t *a = z->win + i, *b = z->win + (c - z->base);
                if (b[best] == a[best] && b[0] == a[0]) {
                    int l = 0;
                    while (l < maxlen && a[l] == b[l]) l++;
                    if (l > best) {
                        best = l;
                        bdist = abs - c;
                        if (l >= GOOD_LEN || l == maxlen) break;
                    }
                }
                uint32_t nx = z->prev[c & WMASK];
                if (nx >= cand) break;          /* the chain went stale */
                cand = nx;
            }
        }
        if (best >= MIN_MATCH) {
            put_match(z, best, (int)bdist);
            for (int k = 0; k < best; k++) {
                if (i + k + MIN_MATCH <= z->wlen) insert(z, i + k);
            }
            z->pos += best;
        } else {
            put_sym(z, z->win[i]);
            if (avail >= MIN_MATCH) insert(z, i);
            z->pos++;
        }
    }
    /* slide: keep the last WSIZE bytes behind pos */
    if (z->pos > 2 * WSIZE - 16 || (z->wlen + 8192 > z->wcap && z->pos > WSIZE)) {
        size_t cut = z->pos - WSIZE;
        memmove(z->win, z->win + cut, z->wlen - cut);
        z->wlen -= cut;
        z->pos -= cut;
        z->base += (uint32_t)cut;
    }
}

static void feed(z_t *z, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        z->a1 = (z->a1 + p[i]) % 65521;
        z->a2 = (z->a2 + z->a1) % 65521;
    }
    while (n) {
        size_t room = z->wcap - z->wlen, k = n < room ? n : room;
        memcpy(z->win + z->wlen, p, k);
        z->wlen += k;
        p += k;
        n -= k;
        compress(z, false);
    }
}

static inline int iabs(int v) { return v < 0 ? -v : v; }

static inline int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = iabs(p - a), pb = iabs(p - b), pc = iabs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

bool dib_png_write(const char *path, int w, int h, dib_png_row_fn rows, void *user,
                   volatile int *progress)
{
    z_t z;
    memset(&z, 0, sizeof z);
    size_t rb = (size_t)w * 4;
    uint8_t *cur = dib_alloc(rb), *prv = dib_calloc(rb), *cand = dib_alloc((rb + 1) * 4);
    z.out = dib_alloc(IDAT_MAX);
    z.wcap = 2 * WSIZE + rb + 1 + LOOKAHEAD + 16384;
    z.win = dib_alloc(z.wcap);
    z.head = dib_calloc(sizeof(uint32_t) * HSIZE);
    z.prev = dib_calloc(sizeof(uint32_t) * WSIZE);
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    z.f = (cur && prv && cand && z.out && z.win && z.head && z.prev) ? fopen(tmp, "wb") : NULL;
    z.ok = z.f != NULL;
    for (int li = 0, l = MIN_MATCH; l <= MAX_MATCH; l++) {
        while (li < 28 && LBASE[li + 1] <= l) li++;
        z.lcode[l] = (uint16_t)li;
    }
    z.a1 = 1;

    if (z.ok) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        z.ok = fwrite(sig, 1, 8, z.f) == 8;
        uint8_t ihdr[13];
        put_be32(ihdr, (uint32_t)w);
        put_be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;        /* bits per channel */
        ihdr[9] = 6;        /* RGBA             */
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        chunk(&z, "IHDR", ihdr, 13);
        out_byte(&z, 0x78);     /* zlib: deflate, 32 KB window */
        out_byte(&z, 0x01);
        put_bits(&z, 1, 1);     /* the one and last block      */
        put_bits(&z, 1, 2);     /* fixed Huffman codes         */
    }
    for (int y = 0; z.ok && y < h; y++) {
        if (!rows(user, y, cur)) {
            z.ok = false;
            break;
        }
        /* four candidates side by side; keep the cheapest */
        uint8_t *c[4] = { cand, cand + rb + 1, cand + 2 * (rb + 1), cand + 3 * (rb + 1) };
        static const uint8_t ftype[4] = { 0, 1, 2, 4 };
        unsigned long best = ~0ul;
        int bi = 0;
        for (int f = 0; f < 4; f++) {
            uint8_t *o = c[f];
            o[0] = ftype[f];
            unsigned long sum = 0;
            for (size_t i = 0; i < rb; i++) {
                int a = i >= 4 ? cur[i - 4] : 0, b = y ? prv[i] : 0, cc = (i >= 4 && y) ? prv[i - 4] : 0;
                int v = f == 0 ? cur[i] : f == 1 ? cur[i] - a : f == 2 ? cur[i] - b : cur[i] - paeth(a, b, cc);
                o[i + 1] = (uint8_t)v;
                sum += (unsigned long)iabs((int8_t)o[i + 1]);
            }
            if (sum < best) {
                best = sum;
                bi = f;
            }
        }
        feed(&z, c[bi], rb + 1);
        uint8_t *t = prv; prv = cur; cur = t;
        if (progress) *progress = (int)((long)(y + 1) * 1000 / h);
    }
    if (z.ok) {
        compress(&z, true);
        put_sym(&z, 256);                       /* end of block */
        if (z.nbits) put_bits(&z, 0, 8 - z.nbits);
        uint8_t ad[4];
        put_be32(ad, (z.a2 << 16) | z.a1);
        for (int i = 0; i < 4; i++) out_byte(&z, ad[i]);
        if (z.on) chunk(&z, "IDAT", z.out, z.on);
        chunk(&z, "IEND", NULL, 0);
    }
    if (z.f) z.ok = (fclose(z.f) == 0) && z.ok;
    dib_free(cur);
    dib_free(prv);
    dib_free(cand);
    dib_free(z.out);
    dib_free(z.win);
    dib_free(z.head);
    dib_free(z.prev);
    if (z.ok) {
        remove(path);
        z.ok = rename(tmp, path) == 0;
    }
    if (!z.ok) remove(tmp);
    return z.ok;
}
