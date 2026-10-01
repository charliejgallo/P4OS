/*
 * MILA - the pack (see ml_art.h)
 *
 * Layout (little endian), written by tools/pack_assets.py:
 *     "MHPK" u16 version u16 count
 *     count x entry (48 bytes, sorted by name):
 *         char name[32]  u8 type  u8 frames  u16 ms  u32 off  u32 clen  u32 rawlen
 *     data: one LZ4 block per entry
 * A sheet unpacks to:
 *     per frame: i16 w, h, ax, ay  u32 datalen
 *                u16 span[h][2]  u32 rowoff[h]  data (datalen, padded to 4)
 * P4OS: a sheet with ML_ZIP in its type is stored as it is (clen = rawlen):
 *     per frame: i16 w, h, ax, ay  u32 rawlen  u32 zlen
 *     then per frame its LZ4 block (zlen, padded to 4), which unpacks to the
 *     frame above with the data's bytes in planes (see weave())
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "ml_art.h"

#include "aos_hal.h"

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

/* The pack may come in parts: mila.pak, then mila.pak.1, .2...
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

void ml_art_close(void)
{
    for (int i = 0; i < s_nparts; i++) fclose(s_part[i]);
    s_nparts = 0;
    s_fp = NULL;
    free(s_ent);
    s_ent = NULL;
    s_nent = 0;
}

uint32_t ml_art_hash(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < s_nent; i++) {
        for (int k = 0; k < NAME_LEN && s_ent[i].name[k]; k++) h = (h ^ (uint8_t)s_ent[i].name[k]) * 16777619u;
        h = (h ^ s_ent[i].rawlen) * 16777619u;
    }
    return h;
}

bool ml_art_ok(void)
{
    return s_fp != NULL;
}

uint32_t ml_art_bytes(void)
{
    return s_bytes;
}

uint32_t ml_art_read(void)
{
    return s_read;
}

uint32_t ml_art_prefix_bytes(const char *prefix)
{
    size_t n = strlen(prefix);
    uint32_t sum = 0;
    for (int i = 0; i < s_nent; i++)
        if (strncmp(s_ent[i].name, prefix, n) == 0) sum += s_ent[i].clen;
    return sum;
}

bool ml_art_open(const char *path)
{
    ml_art_close();
    s_fp = fopen(path, "rb");
    if (!s_fp) return false;
    s_len = 0;
    for (int i = 0; i < MAX_PARTS; i++) {
        FILE *f = s_fp;
        if (i > 0) {
            char pp[160];
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
        ml_art_close();
        return false;
    }
    int n = rd16(hdr + 6);
    uint8_t *tab = (uint8_t *)ml_malloc((size_t)n * ENTRY_SIZE);
    s_ent = (entry_t *)ml_calloc((size_t)n, sizeof(entry_t));
    if (!tab || !s_ent || !read_at(8, tab, (size_t)n * ENTRY_SIZE)) {
        free(tab);
        ml_art_close();
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

bool ml_art_has(const char *name)
{
    return find(name) >= 0;
}

static uint8_t *unpack(int idx)
{
    const entry_t *e = &s_ent[idx];
    if (e->type & ML_ZIP) {
        /* packed frame by frame: kept as it is on the card */
        if (!s_fp || e->off + e->clen > s_len || e->clen != e->rawlen) return NULL;
        uint8_t *raw = (uint8_t *)aos_hal_io_alloc(e->clen ? e->clen : 1);
        bool ok = raw && read_at(e->off, raw, e->clen);
        s_read += e->clen;
        ml_yield();
        if (!ok) {
            aos_hal_io_free(raw);
            return NULL;
        }
        return raw;
    }
    if (!s_fp || e->off + e->clen > s_len) return NULL;
    uint8_t *raw = (uint8_t *)ml_malloc(e->rawlen ? e->rawlen : 1);
    /* P4OS: on a 128-byte line, so the card's DMA reads straight into it
     * (a big fread goes to the filesystem in one piece) */
    uint8_t *src = (uint8_t *)aos_hal_io_alloc(e->clen ? e->clen : 1);
    bool ok = raw && src && read_at(e->off, src, e->clen) && lz4_decode(src, e->clen, raw, e->rawlen);
    s_read += e->clen;
    aos_hal_io_free(src);
    ml_yield();
    if (!ok) {
        free(raw);
        return NULL;
    }
    return raw;
}

uint8_t *ml_art_blob(const char *name, uint32_t *len)
{
    int i = find(name);
    if (i < 0) return NULL;
    uint8_t *raw = unpack(i);
    if (raw && len) *len = s_ent[i].rawlen;
    return raw;
}

static int fmt_bpp(int fmt)
{
    switch (fmt & ~ML_ZIP) {
    case ML_PX_COL: return 4;
    case ML_PX_LID: case ML_PX_IMG: return 3;
    case ML_PX_GLOW: case ML_PX_RGB: return 2;
    default: return 1;
    }
}

bool ml_art_load(const char *name, ml_anim_t *out)
{
    memset(out, 0, sizeof(*out));
    int i = find(name);
    if (i < 0) return false;
    const entry_t *e = &s_ent[i];
    if (e->type == ML_BLOB || e->frames == 0) return false;
    uint8_t *raw = unpack(i);
    if (!raw) return false;
    bool zip = (e->type & ML_ZIP) != 0;
    ml_spr_t *f = (ml_spr_t *)ml_calloc(e->frames, sizeof(ml_spr_t));
    if (!f) goto bad;
    int bpp = fmt_bpp(e->type);
    size_t o = 0;
    if (zip) {
        size_t blk = (size_t)e->frames * 16;
        if (blk > e->rawlen) goto bad;
        for (int k = 0; k < e->frames; k++) {
            const uint8_t *h = raw + (size_t)k * 16;
            ml_spr_t *s = &f[k];
            s->w = (int16_t)rd16(h);
            s->h = (int16_t)rd16(h + 2);
            s->ax = (int16_t)rd16(h + 4);
            s->ay = (int16_t)rd16(h + 6);
            s->rawlen = rd32(h + 8);
            s->zlen = rd32(h + 12);
            s->bpp = (uint8_t)bpp;
            if (blk + s->zlen > e->rawlen || s->rawlen < 12 + (uint32_t)s->h * 8) goto bad;
            s->z = raw + blk;
            blk = (blk + s->zlen + 3) & ~(size_t)3;
        }
    } else {
        for (int k = 0; k < e->frames; k++) {
            if (o + 12 > e->rawlen) goto bad;
            ml_spr_t *s = &f[k];
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
            s->bpp = (uint8_t)bpp;
            o += need;
            o = (o + 3) & ~(size_t)3;
        }
    }
    out->fmt = (uint8_t)(e->type & ~ML_ZIP);
    out->n = e->frames;
    out->ms = e->ms;
    out->f = f;
    out->mem = raw;
    out->zip = zip;
    out->bytes = e->rawlen + e->frames * (uint32_t)sizeof(ml_spr_t);
    s_bytes += out->bytes;
    return true;
bad:
    free(f);
    if (zip) aos_hal_io_free(raw);
    else free(raw);
    return false;
}

void ml_anim_free(ml_anim_t *a)
{
    if (!a) return;
    if (a->mem) s_bytes -= a->bytes;
    free(a->f);
    if (a->zip) aos_hal_io_free(a->mem);
    else free(a->mem);
    memset(a, 0, sizeof(*a));
}

/* ---- packed frames (ML_ZIP) ---- */

static uint8_t *s_zscr;             /* a block unpacked, its planes apart */
static uint32_t s_zscr_cap, s_zslot_bytes, s_zunpacks;

uint32_t ml_zunpacks(void)
{
    return s_zunpacks;
}

void ml_zscratch_free(void)
{
    free(s_zscr);
    s_zscr = NULL;
    s_zscr_cap = 0;
}

uint32_t ml_zbytes(void)
{
    return s_zslot_bytes + s_zscr_cap;
}

/* the data's planes (every pixel's first byte, then every second...) back
 * into pixels; the bytes past the last whole pixel (padding) as they are */
static void weave(uint8_t *dst, const uint8_t *src, uint32_t len, int bpp)
{
    uint32_t n = len / (uint32_t)bpp;
    switch (bpp) {
    case 4: {
        const uint8_t *p0 = src, *p1 = src + n, *p2 = src + 2 * n, *p3 = src + 3 * n;
        for (uint32_t i = 0; i < n; i++, dst += 4) {
            dst[0] = p0[i];
            dst[1] = p1[i];
            dst[2] = p2[i];
            dst[3] = p3[i];
        }
        break;
    }
    case 3: {
        const uint8_t *p0 = src, *p1 = src + n, *p2 = src + 2 * n;
        for (uint32_t i = 0; i < n; i++, dst += 3) {
            dst[0] = p0[i];
            dst[1] = p1[i];
            dst[2] = p2[i];
        }
        break;
    }
    default:
        memcpy(dst, src, (size_t)n * (size_t)bpp);
        dst += (size_t)n * (size_t)bpp;
        break;
    }
    memcpy(dst, src + (size_t)n * (size_t)bpp, len - n * (uint32_t)bpp);
}

static bool unzip_into(ml_zslot_t *sl, const ml_spr_t *s)
{
    if (sl->cap < s->rawlen) {
        s_zslot_bytes -= sl->cap;
        free(sl->buf);
        sl->cap = 0;
        sl->buf = (uint8_t *)ml_malloc(s->rawlen);
        if (!sl->buf) return false;
        sl->cap = s->rawlen;
        s_zslot_bytes += sl->cap;
    }
    if (s_zscr_cap < s->rawlen) {
        free(s_zscr);
        s_zscr_cap = 0;
        s_zscr = (uint8_t *)ml_malloc(s->rawlen);
        if (!s_zscr) return false;
        s_zscr_cap = s->rawlen;
    }
    if (!lz4_decode(s->z, s->zlen, s_zscr, s->rawlen)) return false;
    size_t hdr = 12 + (size_t)s->h * 8;
    uint32_t dlen = rd32(s_zscr + 8);
    if (hdr + dlen > s->rawlen) return false;
    memcpy(sl->buf, s_zscr, hdr);
    weave(sl->buf + hdr, s_zscr + hdr, dlen, s->bpp);
    ml_spr_t *d = &sl->spr;
    *d = *s;
    d->z = NULL;
    d->span = (const uint16_t *)(sl->buf + 12);
    d->row = (const uint32_t *)(sl->buf + 12 + (size_t)s->h * 4);
    d->data = sl->buf + hdr;
    sl->src = s;
    s_zunpacks++;
    return true;
}

const ml_spr_t *ml_zstream_get(ml_zstream_t *z, const ml_spr_t *s)
{
    if (!s || !s->z) return s;
    if (z->s[z->last].src == s) return &z->s[z->last].spr;
    int k = 1 - z->last;
    if (z->s[k].src != s && !unzip_into(&z->s[k], s)) {
        z->s[k].src = NULL;
        return NULL;
    }
    z->last = k;
    return &z->s[k].spr;
}

void ml_zstream_reset(ml_zstream_t *z)
{
    z->s[0].src = z->s[1].src = NULL;
}

void ml_zstream_free(ml_zstream_t *z)
{
    for (int k = 0; k < 2; k++) {
        s_zslot_bytes -= z->s[k].cap;
        free(z->s[k].buf);
    }
    memset(z, 0, sizeof(*z));
}

/* -------------------------------------------------------------------------- */

void ml_lut_build(ml_lut_t *l, const ml_pal_t *p, uint32_t tint, uint16_t keep)
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
            l->c[id][k] = ml_rgb(rr > 255 ? 255 : rr, gg > 255 ? 255 : gg, bb > 255 ? 255 : bb);
        }
    }
}

bool ml_pal_load(const char *name, ml_pal_t *out)
{
    uint32_t len = 0;
    uint8_t *b = ml_art_blob(name, &len);
    if (!b) return false;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 16 && (uint32_t)(i * 3 + 2) < len; i++) {
        out->c[i] = ((uint32_t)b[i * 3] << 16) | ((uint32_t)b[i * 3 + 1] << 8) | b[i * 3 + 2];
    }
    free(b);
    return true;
}
