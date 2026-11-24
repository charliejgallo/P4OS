/*
 * DJ - a small FLAC decoder of our own.
 *
 * The whole format a deck needs: STREAMINFO, frames with fixed or variable
 * blocking, the four subframe kinds (constant, verbatim, fixed predictors,
 * LPC up to order 32), Rice residuals with escapes, wasted bits, and the
 * three stereo decorrelations; 8 to 24 bits. The CRC-16 of a frame is not
 * checked (the card is not a radio); the CRC-8 of a header is, because
 * that is how a frame is told from a sync pattern in the middle of audio.
 *
 * Seeking without a seek table: bisection over the file. Look at the
 * middle, find the next frame header there, read the sample it starts
 * at, keep the half that holds the target; when the span is small, walk
 * the headers forward. The I/O thread usually decodes chunk after chunk,
 * and then the next frame is just where the last one ended.
 *
 * Two things the app's loader cannot resolve, avoided: a 64-bit shift by a
 * variable (__ashrdi3) and __builtin_clz on a core without Zbb (__clzsi2).
 */
#include "dj.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#define WIN      (160 * 1024)       /* the file window the frames are decoded from */
#define MAXBLK   16384              /* the largest block we take (the format allows 65535) */
#define MAXCH    8

struct dj_flac {
    FILE     *f;
    uint32_t  fsize;
    uint32_t  data0;            /* the first frame */
    uint32_t  sr, nsamp;
    int       ch, bps;
    int       blk_fixed;        /* STREAMINFO's block size, when min == max */
    uint32_t  maxframe;

    uint8_t  *win;
    uint32_t  win_off, win_len;

    int32_t  *s[MAXCH];          /* the decoded channels of the frame at hand */
    uint32_t  next_off, next_samp;  /* the frame after the last decoded one */
};

static uint8_t CRC8[256];
static uint8_t CLZ8[256];

static void tables(void)
{
    if (CLZ8[0] == 8) return;
    for (int i = 0; i < 256; i++) {
        uint8_t c = (uint8_t)i;
        for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
        CRC8[i] = c;
        int z = 0;
        while (z < 8 && !(i & (0x80 >> z))) z++;
        CLZ8[i] = (uint8_t)z;
    }
}

/* ---- the window over the file ---- */

/* make [off, off + need) be in the window; false at the end of the file */
static bool win_at(dj_flac_t *f, uint32_t off, uint32_t need)
{
    if (off >= f->win_off && off + need <= f->win_off + f->win_len) return true;
    if (off >= f->fsize) return false;
    if (fseek(f->f, (long)off, SEEK_SET)) return false;
    uint32_t want = WIN - 8;
    if (want > f->fsize - off) want = f->fsize - off;
    f->win_len = (uint32_t)fread(f->win, 1, want, f->f);
    f->win_off = off;
    memset(f->win + f->win_len, 0, 8);          /* the bit reader looks 4 bytes ahead */
    return f->win_len > 0;
}

/* ---- bits ---- */

typedef struct {
    const uint8_t *p;
    uint32_t pos;               /* in bits */
    uint32_t end;               /* in bits: past it the frame is broken */
} br_t;

static inline uint32_t peek32(const br_t *b)
{
    const uint8_t *q = b->p + (b->pos >> 3);
    return ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
}

/* n = 1..24 */
static inline uint32_t bits(br_t *b, int n)
{
    uint32_t w = peek32(b) << (b->pos & 7);
    b->pos += (uint32_t)n;
    return w >> (32 - n);
}

static inline uint32_t bitsn(br_t *b, int n)        /* 0..32 */
{
    if (n == 0) return 0;
    if (n <= 24) return bits(b, n);
    uint32_t hi = bits(b, n - 16);
    return (hi << 16) | bits(b, 16);
}

static inline int32_t sbits(br_t *b, int n)         /* signed, 1..32 */
{
    uint32_t v = bitsn(b, n);
    if (n < 32 && (v & (1u << (n - 1)))) v |= ~0u << n;
    return (int32_t)v;
}

static inline uint32_t unary(br_t *b)
{
    uint32_t q = 0;
    for (;;) {
        uint32_t byte = b->pos >> 3, sh = b->pos & 7;
        uint8_t v = (uint8_t)(b->p[byte] << sh);
        if (v) {
            int z = CLZ8[v];
            q += (uint32_t)z;
            b->pos += (uint32_t)z + 1;
            return q;
        }
        q += 8 - sh;
        b->pos += 8 - sh;
        if (b->pos > b->end) return q;
    }
}

/* an arithmetic shift right of a 64-bit sum by 0..31, the result in 32 bits */
static inline int32_t shr64(int64_t x, int s)
{
    uint32_t lo = (uint32_t)x;
    uint32_t hi = (uint32_t)((uint64_t)x >> 32);
    if (s == 0) return (int32_t)lo;
    return (int32_t)((lo >> s) | (hi << (32 - s)));
}

/* ---- headers ---- */

typedef struct {
    uint32_t len;               /* bytes of the header, CRC-8 included */
    uint32_t blk;
    uint32_t samp;              /* the first sample */
    int      chan;              /* the assignment, 0..10 */
    int      nch;
    int      bps;
} fhdr_t;

static bool utf8num(const uint8_t *p, uint32_t avail, uint32_t *v, uint32_t *len)
{
    uint8_t c = p[0];
    int n;
    uint32_t x;
    if (!(c & 0x80)) { *v = c; *len = 1; return true; }
    else if ((c & 0xE0) == 0xC0) { n = 1; x = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; x = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; x = c & 0x07; }
    else if ((c & 0xFC) == 0xF8) { n = 4; x = c & 0x03; }
    else if ((c & 0xFE) == 0xFC) { n = 5; x = c & 0x01; }
    else if (c == 0xFE) { n = 6; x = 0; }
    else return false;
    if ((uint32_t)n + 1 > avail) return false;
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) return false;
        x = (x << 6) | (p[i] & 0x3F);
    }
    *v = x;
    *len = (uint32_t)n + 1;
    return true;
}

static bool frame_hdr(const dj_flac_t *f, const uint8_t *p, uint32_t avail, fhdr_t *h)
{
    if (avail < 16 || p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) return false;
    bool var = p[1] & 1;
    int bs = p[2] >> 4, rs = p[2] & 15, ca = p[3] >> 4, ss = (p[3] >> 1) & 7;
    if (bs == 0 || rs == 15 || ca > 10 || ss == 3 || (p[3] & 1)) return false;
    uint32_t n, i = 4, ul;
    if (!utf8num(p + 4, avail - 4, &n, &ul)) return false;
    i += ul;
    uint32_t blk;
    if (bs == 1) blk = 192;
    else if (bs <= 5) blk = 576u << (bs - 2);
    else if (bs == 6) blk = p[i++] + 1u;
    else if (bs == 7) { blk = ((uint32_t)p[i] << 8 | p[i + 1]) + 1u; i += 2; }
    else blk = 256u << (bs - 8);
    if (rs == 12) i += 1;
    else if (rs == 13 || rs == 14) i += 2;
    uint8_t crc = 0;
    for (uint32_t k = 0; k < i; k++) crc = CRC8[crc ^ p[k]];
    if (crc != p[i]) return false;
    h->len = i + 1;
    h->blk = blk;
    if (var) h->samp = n;
    else h->samp = n * (uint32_t)(f->blk_fixed ? f->blk_fixed : (int)blk);
    h->chan = ca;
    h->nch = ca < 8 ? ca + 1 : 2;
    static const int8_t BPS[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };
    h->bps = ss ? BPS[ss] : f->bps;
    return blk <= MAXBLK && h->nch <= MAXCH && h->bps >= 4 && h->bps <= 24;
}

/* the next frame header at or after 'off', within 'span' bytes */
static bool find_frame(dj_flac_t *f, uint32_t off, uint32_t span, uint32_t *at, fhdr_t *h)
{
    uint32_t end = off + span < f->fsize ? off + span : f->fsize;
    while (off + 16 <= end) {
        if (!win_at(f, off, 64)) return false;
        const uint8_t *w = f->win + (off - f->win_off);
        uint32_t lim = f->win_off + f->win_len;
        if (lim > end) lim = end;
        for (uint32_t o = off; o + 16 <= lim; o++, w++) {
            if (w[0] != 0xFF || (w[1] & 0xFE) != 0xF8) continue;
            if (frame_hdr(f, w, lim - o, h) && h->samp < f->nsamp + MAXBLK) {
                *at = o;
                return true;
            }
        }
        if (lim <= off + 16) break;
        off = lim - 15;
    }
    return false;
}

/* ---- subframes ---- */

static bool residual(br_t *b, int32_t *out, uint32_t blk, int order)
{
    int method = (int)bits(b, 2);
    if (method > 1) return false;
    int pbits = method ? 5 : 4, esc = method ? 31 : 15;
    int porder = (int)bits(b, 4);
    uint32_t parts = 1u << porder, per = blk >> porder;
    if ((per << porder) != blk || per < (uint32_t)order) return false;
    uint32_t i = (uint32_t)order;
    for (uint32_t p = 0; p < parts; p++) {
        uint32_t n = p == 0 ? per - (uint32_t)order : per;
        int k = (int)bits(b, pbits);
        if (k == esc) {
            int nb = (int)bits(b, 5);
            for (uint32_t j = 0; j < n; j++) out[i++] = nb ? sbits(b, nb) : 0;
        } else {
            for (uint32_t j = 0; j < n; j++) {
                uint32_t q = unary(b);
                uint32_t v = (q << k) | bitsn(b, k);
                out[i++] = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
            }
        }
        if (b->pos > b->end) return false;
    }
    return true;
}

static bool subframe(br_t *b, int32_t *s, uint32_t blk, int bps)
{
    if (bits(b, 1)) return false;
    int type = (int)bits(b, 6);
    int wasted = 0;
    if (bits(b, 1)) wasted = (int)unary(b) + 1;
    bps -= wasted;
    if (bps <= 0) return false;
    if (type == 0) {
        int32_t v = sbits(b, bps);
        for (uint32_t i = 0; i < blk; i++) s[i] = v;
    } else if (type == 1) {
        for (uint32_t i = 0; i < blk; i++) s[i] = sbits(b, bps);
    } else if (type >= 8 && type <= 12) {
        int order = type - 8;
        if ((uint32_t)order > blk) return false;
        for (int i = 0; i < order; i++) s[i] = sbits(b, bps);
        if (!residual(b, s, blk, order)) return false;
        switch (order) {
        case 1: for (uint32_t i = 1; i < blk; i++) s[i] += s[i - 1]; break;
        case 2: for (uint32_t i = 2; i < blk; i++) s[i] += 2 * s[i - 1] - s[i - 2]; break;
        case 3: for (uint32_t i = 3; i < blk; i++) s[i] += 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3]; break;
        case 4: for (uint32_t i = 4; i < blk; i++) s[i] += 4 * s[i - 1] - 6 * s[i - 2] + 4 * s[i - 3] - s[i - 4]; break;
        default: break;
        }
    } else if (type >= 32) {
        int order = type - 31;
        if ((uint32_t)order > blk) return false;
        for (int i = 0; i < order; i++) s[i] = sbits(b, bps);
        int prec = (int)bits(b, 4) + 1;
        if (prec == 16) return false;
        int shift = sbits(b, 5);
        if (shift < 0) return false;
        int32_t c[32];
        for (int i = 0; i < order; i++) c[i] = sbits(b, prec);
        if (!residual(b, s, blk, order)) return false;
        for (uint32_t i = (uint32_t)order; i < blk; i++) {
            int64_t sum = 0;
            const int32_t *h = s + i - 1;
            for (int j = 0; j < order; j++) sum += (int64_t)c[j] * h[-j];
            s[i] += shr64(sum, shift);
        }
    } else {
        return false;
    }
    if (wasted)
        for (uint32_t i = 0; i < blk; i++) s[i] = (int32_t)((uint32_t)s[i] << wasted);
    return b->pos <= b->end;
}

/* decode the frame at 'off' into f->s[]; returns its header, sets next_off */
static bool decode_frame(dj_flac_t *f, uint32_t off, fhdr_t *h)
{
    uint32_t need = f->maxframe ? f->maxframe + 16 : 64 * 1024;
    if (need > WIN - 64) need = WIN - 64;
    if (off + need > f->fsize) need = f->fsize - off;
    if (!win_at(f, off, need)) return false;
    const uint8_t *p = f->win + (off - f->win_off);
    uint32_t avail = f->win_off + f->win_len - off;
    if (!frame_hdr(f, p, avail, h)) return false;
    br_t b = { p, h->len * 8, avail * 8 };
    for (int c = 0; c < h->nch; c++) {
        int bps = h->bps;
        if ((h->chan == 8 && c == 1) || (h->chan == 9 && c == 0) || (h->chan == 10 && c == 1)) bps++;
        if (!subframe(&b, f->s[c], h->blk, bps)) return false;
    }
    int32_t *l = f->s[0], *r = f->s[1];
    if (h->chan == 8) {
        for (uint32_t i = 0; i < h->blk; i++) r[i] = l[i] - r[i];
    } else if (h->chan == 9) {
        for (uint32_t i = 0; i < h->blk; i++) l[i] += r[i];
    } else if (h->chan == 10) {
        for (uint32_t i = 0; i < h->blk; i++) {
            int32_t side = r[i], mid = (int32_t)((uint32_t)l[i] << 1) | (side & 1);
            l[i] = (mid + side) >> 1;
            r[i] = (mid - side) >> 1;
        }
    }
    uint32_t endbyte = (b.pos + 7) >> 3;
    f->next_off = off + endbyte + 2;            /* the CRC-16 */
    f->next_samp = h->samp + h->blk;
    return true;
}

/* ---- opening ---- */

dj_flac_t *dj_flac_open(const char *path, uint32_t *sr, uint32_t *nsamp, int *ch)
{
    tables();
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    dj_flac_t *f = dj_alloc(sizeof *f);
    if (!f) { fclose(fp); return NULL; }
    f->f = fp;
    fseek(fp, 0, SEEK_END);
    f->fsize = (uint32_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t h[4];
    bool ok = fread(h, 1, 4, fp) == 4 && !memcmp(h, "fLaC", 4);
    bool si = false;
    uint32_t off = 4;
    while (ok) {
        uint8_t m[4];
        if (fseek(fp, (long)off, SEEK_SET) || fread(m, 1, 4, fp) != 4) { ok = false; break; }
        uint32_t len = ((uint32_t)m[1] << 16) | (m[2] << 8) | m[3];
        if ((m[0] & 0x7F) == 0 && len >= 34) {
            uint8_t s[34];
            if (fread(s, 1, 34, fp) != 34) { ok = false; break; }
            int minb = s[0] << 8 | s[1], maxb = s[2] << 8 | s[3];
            f->maxframe = ((uint32_t)s[7] << 16) | (s[8] << 8) | s[9];
            f->sr = ((uint32_t)s[10] << 12) | (s[11] << 4) | (s[12] >> 4);
            f->ch = ((s[12] >> 1) & 7) + 1;
            f->bps = (((s[12] & 1) << 4) | (s[13] >> 4)) + 1;
            /* 36 bits of total samples: the top 4 would make it longer than we play */
            f->nsamp = ((s[13] & 15) ? 0xFFFFFFFFu : 0) | ((uint32_t)s[14] << 24) | (s[15] << 16) | (s[16] << 8) | s[17];
            f->blk_fixed = minb == maxb ? minb : 0;
            if (maxb > MAXBLK || f->bps > 24 || f->bps < 4) ok = false;
            si = true;
        }
        off += 4 + len;
        if (m[0] & 0x80) break;
    }
    f->data0 = off;
    if (ok && si) {
        f->win = dj_alloc(WIN);
        for (int c = 0; c < MAXCH && f->win; c++) {
            f->s[c] = dj_alloc(MAXBLK * sizeof(int32_t));
            if (!f->s[c]) ok = false;
        }
        if (!f->win) ok = false;
    }
    if (ok && si && f->nsamp == 0) {
        /* no length said: the last frame's start plus its block */
        uint32_t at;
        fhdr_t fh;
        f->nsamp = 0xFFFFFFFFu;
        uint32_t back = f->fsize > f->data0 + 256 * 1024 ? f->fsize - 256 * 1024 : f->data0;
        uint32_t last = 0;
        while (find_frame(f, back, f->fsize - back, &at, &fh)) {
            last = fh.samp + fh.blk;
            back = at + 1;
        }
        f->nsamp = last;
    }
    if (!ok || !si || f->nsamp == 0 || f->nsamp == 0xFFFFFFFFu) {
        dj_flac_close(f);
        return NULL;
    }
    f->next_off = f->data0;
    f->next_samp = 0;
    *sr = f->sr;
    *nsamp = f->nsamp;
    *ch = f->ch;
    return f;
}

void dj_flac_close(dj_flac_t *f)
{
    if (!f) return;
    if (f->f) fclose(f->f);
    dj_free(f->win);
    for (int c = 0; c < MAXCH; c++) dj_free(f->s[c]);
    dj_free(f);
}

/* the frame that holds sample 'target': bisection, then a walk */
static bool seek_to(dj_flac_t *f, uint32_t target, uint32_t *off_out)
{
    uint32_t lo = f->data0, hi = f->fsize, lo_samp = 0;
    fhdr_t h;
    uint32_t at;
    for (int it = 0; it < 32 && hi - lo > 96 * 1024; it++) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!find_frame(f, mid, hi - mid, &at, &h)) { hi = mid; continue; }
        if (h.samp <= target) { lo = at; lo_samp = h.samp; }
        else hi = mid;
    }
    (void)lo_samp;
    /* walk the headers from lo */
    uint32_t best = lo, o = lo;
    for (int it = 0; it < 4096; it++) {
        if (!find_frame(f, o, 128 * 1024, &at, &h)) break;
        if (h.samp > target) break;
        best = at;
        if (target < h.samp + h.blk) break;
        o = at + h.len;
    }
    *off_out = best;
    return true;
}

int dj_flac_read(dj_flac_t *f, uint32_t from, int16_t *out, int n)
{
    if (!f || from >= f->nsamp) return 0;
    uint32_t off;
    if (from >= f->next_samp && from < f->next_samp + MAXBLK && f->next_off < f->fsize) off = f->next_off;
    else if (!seek_to(f, from, &off)) return 0;
    int done = 0;
    int shift = 0;
    while (done < n) {
        fhdr_t h;
        if (!decode_frame(f, off, &h)) {
            /* a broken frame: look for the next one */
            uint32_t at;
            if (!find_frame(f, off + 1, 256 * 1024, &at, &h)) break;
            off = at;
            if (!decode_frame(f, off, &h)) break;
        }
        off = f->next_off;
        shift = h.bps - 16;
        uint32_t a = from + (uint32_t)done;
        if (h.samp + h.blk <= a) continue;          /* before what we want (the walk ends a frame early) */
        uint32_t i0 = a > h.samp ? a - h.samp : 0;
        if (h.samp > a) {                           /* a hole in the stream: leave it silent */
            uint32_t gap = h.samp - a;
            done += (int)(gap < (uint32_t)(n - done) ? gap : (uint32_t)(n - done));
            if (done >= n) break;
        }
        int32_t *l = f->s[0], *r = h.nch > 1 ? f->s[1] : f->s[0];
        for (uint32_t i = i0; i < h.blk && done < n; i++, done++) {
            int32_t x = l[i], y = r[i];
            if (shift > 0) { x >>= shift; y >>= shift; }
            else if (shift < 0) { x = (int32_t)((uint32_t)x << -shift); y = (int32_t)((uint32_t)y << -shift); }
            out[done * 2] = (int16_t)x;
            out[done * 2 + 1] = (int16_t)y;
        }
    }
    return done;
}
