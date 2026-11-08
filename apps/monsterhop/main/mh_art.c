/*
 * MONSTER HOP - the pack (see mh_art.h)
 *
 * Layout (little endian), written by tools/pack_assets.py:
 *     "MHPK" u16 version u16 count
 *     count x entry (48 bytes, sorted by name):
 *         char name[32]  u8 type  u8 frames  u16 ms  u32 off  u32 clen  u32 rawlen
 *     data: one LZ4 block per entry
 * A sheet unpacks to:
 *     per frame: i16 w, h, ax, ay  u32 datalen
 *                u16 span[h][2]  u32 rowoff[h]  data (datalen, padded to 4)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "mh_art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_LEN   32
#define ENTRY_SIZE 48
#define PAK_VERSION 1

typedef struct {
    char     name[NAME_LEN];
    uint8_t  type, frames;
    uint16_t ms;
    uint32_t off, clen, rawlen;
} entry_t;

/* The pack may come in parts: monsterhop.pak, then monsterhop.pak.1, .2...
 * (the portal takes 8 MB per upload), read as one file end to end */
#define MAX_PARTS 8
static FILE    *s_part[MAX_PARTS];
static size_t   s_part_len[MAX_PARTS];
static int      s_nparts;
static FILE    *s_fp;                   /* the first part: "is it open"      */
static size_t   s_len;
static entry_t *s_ent;
static int      s_nent;
static uint32_t s_bytes;
static volatile uint32_t s_read;         /* compressed bytes read, ever (the loading bar) */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool lz4_decode(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen)
{
    size_t i = 0, o = 0;
    while (i < slen) {
        uint8_t tok = src[i++];
        size_t lit = tok >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (i >= slen) return false;
                b = src[i++];
                lit += b;
            } while (b == 255);
        }
        if (i + lit > slen || o + lit > dlen) return false;
        memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i >= slen) break;
        if (i + 2 > slen) return false;
        size_t off = (size_t)src[i] | ((size_t)src[i + 1] << 8);
        i += 2;
        size_t m = (size_t)(tok & 15) + 4;
        if ((tok & 15) == 15) {
            uint8_t b;
            do {
                if (i >= slen) return false;
                b = src[i++];
                m += b;
            } while (b == 255);
        }
        if (off == 0 || off > o || o + m > dlen) return false;
        for (size_t k = 0; k < m; k++, o++) dst[o] = dst[o - off];
    }
    return o == dlen;
}

static bool read_at(size_t off, void *dst, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < s_nparts && n > 0; i++) {
        if (off >= s_part_len[i]) {
            off -= s_part_len[i];
            continue;
        }
        size_t k = s_part_len[i] - off;
        if (k > n) k = n;
        if (fseek(s_part[i], (long)off, SEEK_SET) != 0 || fread(d, 1, k, s_part[i]) != k) return false;
        d += k;
        n -= k;
        off = 0;
    }
    return n == 0;
}

void mh_art_close(void)
{
    for (int i = 0; i < s_nparts; i++) fclose(s_part[i]);
    s_nparts = 0;
    s_fp = NULL;
    free(s_ent);
    s_ent = NULL;
    s_nent = 0;
}

bool mh_art_ok(void)
{
    return s_fp != NULL;
}

uint32_t mh_art_bytes(void)
{
    return s_bytes;
}

uint32_t mh_art_read(void)
{
    return s_read;
}

uint32_t mh_art_prefix_bytes(const char *prefix)
{
    size_t n = strlen(prefix);
    uint32_t sum = 0;
    for (int i = 0; i < s_nent; i++)
        if (strncmp(s_ent[i].name, prefix, n) == 0) sum += s_ent[i].clen;
    return sum;
}

bool mh_art_open(const char *path)
{
    mh_art_close();
    s_fp = fopen(path, "rb");
    if (!s_fp) return false;
    s_len = 0;
    for (int i = 0; i < MAX_PARTS; i++) {
        FILE *f = s_fp;
        if (i > 0) {
            char pp[520];
            snprintf(pp, sizeof pp, "%s.%d", path, i);
            f = fopen(pp, "rb");
            if (!f) break;
        }
        fseek(f, 0, SEEK_END);
        s_part[i] = f;
        s_part_len[i] = (size_t)ftell(f);
        s_len += s_part_len[i];
        s_nparts = i + 1;
    }
    uint8_t hdr[8];
    if (!read_at(0, hdr, 8) || memcmp(hdr, "MHPK", 4) != 0 || rd16(hdr + 4) != PAK_VERSION) {
        mh_art_close();
        return false;
    }
    int n = rd16(hdr + 6);
    uint8_t *tab = (uint8_t *)mh_malloc((size_t)n * ENTRY_SIZE);
    s_ent = (entry_t *)mh_calloc((size_t)n, sizeof(entry_t));
    if (!tab || !s_ent || !read_at(8, tab, (size_t)n * ENTRY_SIZE)) {
        free(tab);
        mh_art_close();
        return false;
    }
    for (int i = 0; i < n; i++) {
        const uint8_t *p = tab + (size_t)i * ENTRY_SIZE;
        entry_t *e = &s_ent[i];
        memcpy(e->name, p, NAME_LEN);
        e->name[NAME_LEN - 1] = 0;
        e->type = p[32];
        e->frames = p[33];
        e->ms = rd16(p + 34);
        e->off = rd32(p + 36);
        e->clen = rd32(p + 40);
        e->rawlen = rd32(p + 44);
    }
    free(tab);
    s_nent = n;
    return true;
}

static int find(const char *name)
{
    int lo = 0, hi = s_nent - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = strncmp(s_ent[mid].name, name, NAME_LEN);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

bool mh_art_has(const char *name)
{
    return find(name) >= 0;
}

static uint8_t *unpack(int idx)
{
    const entry_t *e = &s_ent[idx];
    if (!s_fp || e->off + e->clen > s_len) return NULL;
    uint8_t *raw = (uint8_t *)mh_malloc(e->rawlen ? e->rawlen : 1);
    uint8_t *src = (uint8_t *)mh_malloc(e->clen ? e->clen : 1);
    bool ok = raw && src && read_at(e->off, src, e->clen) && lz4_decode(src, e->clen, raw, e->rawlen);
    s_read += e->clen;
    free(src);
    mh_yield();
    if (!ok) {
        free(raw);
        return NULL;
    }
    return raw;
}

uint8_t *mh_art_blob(const char *name, uint32_t *len)
{
    int i = find(name);
    if (i < 0) return NULL;
    uint8_t *raw = unpack(i);
    if (raw && len) *len = s_ent[i].rawlen;
    return raw;
}

bool mh_art_load(const char *name, mh_anim_t *out)
{
    memset(out, 0, sizeof(*out));
    int i = find(name);
    if (i < 0) return false;
    const entry_t *e = &s_ent[i];
    if (e->type == MH_BLOB || e->frames == 0) return false;
    uint8_t *raw = unpack(i);
    if (!raw) return false;
    mh_spr_t *f = (mh_spr_t *)mh_calloc(e->frames, sizeof(mh_spr_t));
    if (!f) {
        free(raw);
        return false;
    }
    size_t o = 0;
    for (int k = 0; k < e->frames; k++) {
        if (o + 12 > e->rawlen) goto bad;
        mh_spr_t *s = &f[k];
        s->w = (int16_t)rd16(raw + o);
        s->h = (int16_t)rd16(raw + o + 2);
        s->ax = (int16_t)rd16(raw + o + 4);
        s->ay = (int16_t)rd16(raw + o + 6);
        uint32_t dlen = rd32(raw + o + 8);
        o += 12;
        size_t need = (size_t)s->h * 8 + dlen;
        if (o + need > e->rawlen) goto bad;
        s->span = (const uint16_t *)(raw + o);
        s->row = (const uint32_t *)(raw + o + (size_t)s->h * 4);
        s->data = raw + o + (size_t)s->h * 8;
        o += need;
        o = (o + 3) & ~(size_t)3;
    }
    out->fmt = e->type;
    out->n = e->frames;
    out->ms = e->ms;
    out->f = f;
    out->mem = raw;
    out->bytes = e->rawlen + e->frames * (uint32_t)sizeof(mh_spr_t);
    s_bytes += out->bytes;
#ifdef AOS_SIM_BUILTIN
    if (getenv("MH_ARTLOG")) printf("ART %s %u\n", name, (unsigned)out->bytes);
#endif
    return true;
bad:
    free(f);
    free(raw);
    return false;
}

/* -------------------------------------------------------------------------- */
/* A sheet's first frame, streamed (mh_art_rows)                              */
/* -------------------------------------------------------------------------- */

/* LZ4 copies reach at most 64 KB back, so that much of what came out stays
 * in the window when it is full; the rest of it is new rows. A larger
 * window moves the history less often (every 192 KB of output). */
#define RS_HIST  65536
#define RS_WIN   (RS_HIST + 196608)
#define RS_IN    16384

typedef struct {
    /* the compressed block, read from the card a piece at a time */
    size_t   src_off, src_left;
    uint8_t *in;
    size_t   in_pos, in_len;
    /* what came out: win[0] is byte `base` of the unpacked sheet */
    uint8_t *win;
    size_t   o, base;
    /* the frame */
    int      fmt, bpp, w, h, y;     /* y: next row; -1 before the header */
    size_t   data0;                 /* where the frame's pixels start      */
    uint16_t *span;
    uint32_t *rowoff;
    mh_rows_fn fn;
    void    *ctx;
    bool     fail;
} rs_t;

static int px_bytes(int fmt)
{
    switch (fmt) {
    case MH_PX_COL: return 4;
    case MH_PX_LID: return 3;
    case MH_PX_PLANE: return 1;
    case MH_PX_GLOW: return 2;
    default: return 3;              /* MH_PX_IMG */
    }
}

static bool rs_fill(rs_t *s)
{
    if (!s->src_left) return false;
    size_t n = s->src_left < RS_IN ? s->src_left : RS_IN;
    if (!read_at(s->src_off, s->in, n)) return false;
    s->src_off += n;
    s->src_left -= n;
    s->in_pos = 0;
    s->in_len = n;
    s_read += (uint32_t)n;
    return true;
}

static inline int rs_byte(rs_t *s)
{
    if (s->in_pos == s->in_len && !rs_fill(s)) return -1;
    return s->in[s->in_pos++];
}

static inline bool rs_more(const rs_t *s)
{
    return s->in_pos < s->in_len || s->src_left > 0;
}

/* hands fn every row that is complete in the window; at the first call
 * also the header and the row tables, which must be in it whole */
static void rs_rows(rs_t *s)
{
    size_t upto = s->base + s->o;
    if (s->y < 0) {
        if (s->base != 0 || upto < 12) return;
        s->w = (int16_t)rd16(s->win);
        s->h = (int16_t)rd16(s->win + 2);
        size_t tab = 12 + (size_t)s->h * 8;
        if (s->w <= 0 || s->h <= 0) {
            s->fail = true;
            return;
        }
        if (upto < tab) return;
        s->span = (uint16_t *)mh_malloc((size_t)s->h * 4);
        s->rowoff = (uint32_t *)mh_malloc((size_t)s->h * 4);
        if (!s->span || !s->rowoff) {
            s->fail = true;
            return;
        }
        for (int r = 0; r < s->h; r++) {
            s->span[2 * r] = rd16(s->win + 12 + (size_t)r * 4);
            s->span[2 * r + 1] = rd16(s->win + 14 + (size_t)r * 4);
            s->rowoff[r] = rd32(s->win + 12 + (size_t)s->h * 4 + (size_t)r * 4);
        }
        s->data0 = tab;
        s->y = 0;
        if (!s->fn(s->ctx, s->fmt, s->w, s->h, -1, 0, 0, NULL)) {
            s->fail = true;
            return;
        }
    }
    while (s->y < s->h) {
        int x0 = s->span[2 * s->y], x1 = s->span[2 * s->y + 1];
        if (x1 <= x0) {
            s->y++;
            continue;
        }
        size_t a = s->data0 + s->rowoff[s->y], b = a + (size_t)(x1 - x0) * s->bpp;
        if (b > upto) return;
        if (a < s->base || x1 > s->w) {
            s->fail = true;     /* rows out of order: not what the packer writes */
            return;
        }
        if (!s->fn(s->ctx, s->fmt, s->w, s->h, s->y, x0, x1, s->win + (a - s->base))) {
            s->fail = true;
            return;
        }
        s->y++;
    }
}

/* the window is full: rows out, then keep only the history */
static void rs_flush(rs_t *s)
{
    rs_rows(s);
    if (s->fail) return;
    if (s->y < 0) {         /* the tables did not fit in the window */
        s->fail = true;
        return;
    }
    memmove(s->win, s->win + s->o - RS_HIST, RS_HIST);
    s->base += s->o - RS_HIST;
    s->o = RS_HIST;
    mh_yield();
}

static bool rs_done(const rs_t *s)
{
    return s->fail || (s->y >= 0 && s->y >= s->h);
}

/* lz4_decode() above, reading and writing a piece at a time */
static void rs_decode(rs_t *s, size_t rawlen)
{
    while (rs_more(s) && !rs_done(s)) {
        int tok = rs_byte(s);
        if (tok < 0) break;
        size_t lit = (size_t)tok >> 4;
        if (lit == 15) {
            int b;
            do {
                if ((b = rs_byte(s)) < 0) goto bad;
                lit += (size_t)b;
            } while (b == 255);
        }
        if (s->base + s->o + lit > rawlen) goto bad;
        while (lit) {
            if (s->in_pos == s->in_len && !rs_fill(s)) goto bad;
            if (s->o == RS_WIN) {
                rs_flush(s);
                if (rs_done(s)) return;
            }
            size_t n = s->in_len - s->in_pos;
            if (n > lit) n = lit;
            if (n > RS_WIN - s->o) n = RS_WIN - s->o;
            memcpy(s->win + s->o, s->in + s->in_pos, n);
            s->in_pos += n;
            s->o += n;
            lit -= n;
        }
        if (!rs_more(s)) break;
        int lo = rs_byte(s), hi = rs_byte(s);
        if (lo < 0 || hi < 0) goto bad;
        size_t off = (size_t)lo | ((size_t)hi << 8);
        size_t m = (size_t)(tok & 15) + 4;
        if ((tok & 15) == 15) {
            int b;
            do {
                if ((b = rs_byte(s)) < 0) goto bad;
                m += (size_t)b;
            } while (b == 255);
        }
        if (off == 0 || off > s->base + s->o || s->base + s->o + m > rawlen) goto bad;
        while (m) {
            if (s->o == RS_WIN) {
                rs_flush(s);
                if (rs_done(s)) return;
            }
            size_t n = RS_WIN - s->o;
            if (n > m) n = m;
            uint8_t *d = s->win + s->o;
            for (size_t k = 0; k < n; k++) d[k] = d[(ptrdiff_t)k - (ptrdiff_t)off];
            s->o += n;
            m -= n;
        }
    }
    rs_rows(s);
    return;
bad:
    s->fail = true;
}

bool mh_art_rows(const char *name, mh_rows_fn fn, void *ctx)
{
    int i = find(name);
    if (i < 0) return false;
    const entry_t *e = &s_ent[i];
    if (e->type == MH_BLOB || e->frames == 0 || !s_fp || e->off + e->clen > s_len) return false;
    rs_t s;
    memset(&s, 0, sizeof s);
    s.src_off = e->off;
    s.src_left = e->clen;
    s.fmt = e->type;
    s.bpp = px_bytes(e->type);
    s.y = -1;
    s.fn = fn;
    s.ctx = ctx;
    s.in = (uint8_t *)mh_malloc(RS_IN);
    s.win = (uint8_t *)mh_malloc(RS_WIN);
    if (s.in && s.win) rs_decode(&s, e->rawlen);
    bool ok = s.in && s.win && !s.fail && s.y >= 0 && s.y >= s.h;
    free(s.in);
    free(s.win);
    free(s.span);
    free(s.rowoff);
    mh_yield();
    return ok;
}

void mh_anim_free(mh_anim_t *a)
{
    if (!a) return;
    if (a->mem) s_bytes -= a->bytes;
    free(a->f);
    free(a->mem);
    memset(a, 0, sizeof(*a));
}

/* -------------------------------------------------------------------------- */

void mh_lut_build(mh_lut_t *l, const mh_pal_t *p, uint32_t tint, uint16_t keep)
{
    int tr = (int)(tint >> 16) & 255, tg = (int)(tint >> 8) & 255, tbb = (int)tint & 255;
    for (int id = 0; id < 16; id++) {
        uint32_t c = p->c[id];
        int r = (int)(c >> 16) & 255, g = (int)(c >> 8) & 255, b = (int)c & 255;
        if (!(keep & (1u << id))) {
            r = r * tr / 255;
            g = g * tg / 255;
            b = b * tbb / 255;
        }
        for (int k = 0; k < 64; k++) {
            /* light 196 = the colour as painted; above it, highlights */
            int li = k * 4 + 2;
            int rr = r * li / 196, gg = g * li / 196, bb = b * li / 196;
            if (keep & (1u << id)) {
                /* glowing regions stay bright whatever the shading */
                int f = li < 160 ? 160 : li;
                rr = r * f / 196;
                gg = g * f / 196;
                bb = b * f / 196;
            }
            l->c[id][k] = mh_rgb(rr > 255 ? 255 : rr, gg > 255 ? 255 : gg, bb > 255 ? 255 : bb);
        }
    }
}

bool mh_pal_load(const char *name, mh_pal_t *out)
{
    uint32_t len = 0;
    uint8_t *b = mh_art_blob(name, &len);
    if (!b) return false;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 16 && (uint32_t)(i * 3 + 2) < len; i++) {
        out->c[i] = ((uint32_t)b[i * 3] << 16) | ((uint32_t)b[i * 3 + 1] << 8) | b[i * 3 + 2];
    }
    free(b);
    return true;
}
