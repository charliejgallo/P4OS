/*
 * P4OS - Office: PowerPoint 97-2003 (.ppt, .pps, .pot), read into a deck.
 *
 * "Current User" points to the last edit; the chain of edits gives the
 * persist directory (object id -> offset), newest first. The document
 * container gives the slide size and, in its slide list, the slides in
 * order with their persist ids (and, in older files, their text). Each
 * slide's drawing (OfficeArt) gives its shapes: anchors in master units
 * (576 a inch), groups and their child coordinates, shape types (rectangle,
 * rounded, ellipse, diamond, triangle, arrow, line, picture), fill and line
 * colours from the property table, pictures from the BLIP store (the
 * "Pictures" stream) written to the cache, and text: its own atoms or a
 * reference into the slide list, with paragraph runs (indent level,
 * bullets, alignment) and character runs (bold, italics, underline, size,
 * colour) from the style atom. Notes from the notes slide; hidden slides.
 */
#include "of_cfb.h"
#include "of_slides.h"
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *(*of_tr)(const char *es);
#define _(s) (of_tr ? of_tr(s) : (s))

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct { const uint8_t *p; uint16_t ver, inst, type; uint32_t len; } hdr_t;

static bool hdr(const uint8_t *b, size_t n, size_t off, hdr_t *h)
{
    if (off + 8 > n) return false;
    uint16_t vi = rd16(b + off);
    h->ver = vi & 0xF;
    h->inst = vi >> 4;
    h->type = rd16(b + off + 2);
    h->len = rd32(b + off + 4);
    h->p = b + off + 8;
    if (off + 8 + h->len > n) h->len = (uint32_t)(n - off - 8);
    return true;
}

/* the first child of a type inside a container's data */
static bool child(const uint8_t *d, uint32_t n, uint16_t type, int inst, hdr_t *out)
{
    for (size_t o = 0; o + 8 <= n;) {
        hdr_t h;
        if (!hdr(d, n, o, &h)) break;
        if (h.type == type && (inst < 0 || h.inst == inst)) { *out = h; return true; }
        o += 8 + h.len;
    }
    return false;
}

typedef struct {
    const uint8_t *s;           /* PowerPoint Document */
    size_t         n;
    uint32_t      *pers;        /* persist id -> offset (0 none) */
    uint32_t       npers;
    const uint8_t *pics;        /* the Pictures stream */
    size_t         npics_len;
    struct { uint32_t off, size; uint8_t type; } *bse;
    int            nbse;
    char         **bse_file;    /* written to the cache, by BSE index */
    of_deck_t     *dk;
    uint32_t       file_id;
} ppt_t;

static const uint8_t *persist(const ppt_t *P, uint32_t id, hdr_t *h)
{
    /* offsets are kept plus one: 0 is "none", and an object can be at offset 0 */
    if (id >= P->npers || !P->pers[id]) return NULL;
    if (!hdr(P->s, P->n, P->pers[id] - 1, h)) return NULL;
    return h->p;
}

/* -------------------------------------------------------------------------- */
/* Text and its styles                                                         */
/* -------------------------------------------------------------------------- */

typedef struct { uint32_t count; uint16_t level; int8_t bullet; uint16_t buchar; int8_t align; } pf_t;
typedef struct { uint32_t count; int8_t b, i, u; uint16_t size; uint32_t color; } cf_t;

static void read_style(const uint8_t *d, uint32_t n, uint32_t chars, pf_t **pfs, int *npf, cf_t **cfs, int *ncf)
{
    const uint8_t *p = d, *e = d + n;
    uint32_t got = 0;
    while (got < chars && p + 10 <= e) {
        pf_t f = { rd32(p), rd16(p + 4), -1, 0, -1 };
        uint32_t m = rd32(p + 6);
        p += 10;
        if (m & 0x0F) { if (p + 2 > e) break; uint16_t bf = rd16(p); if (m & 1) f.bullet = (int8_t)(bf & 1); p += 2; }
        if (m & 0x80) { if (p + 2 > e) break; f.buchar = rd16(p); p += 2; }
        if (m & 0x10) p += 2;
        if (m & 0x40) p += 2;
        if (m & 0x20) p += 4;
        if (m & 0x800) { if (p + 2 > e) break; f.align = (int8_t)rd16(p); p += 2; }
        if (m & 0x1000) p += 2;
        if (m & 0x2000) p += 2;
        if (m & 0x4000) p += 2;
        if (m & 0x100) p += 2;
        if (m & 0x400) p += 2;
        if (m & 0x8000) p += 2;
        if (m & 0x100000) { if (p + 2 > e) break; p += 2 + 4 * rd16(p); }
        if (m & 0x10000) p += 2;
        if (m & 0xE0000) p += 2;
        if (m & 0x200000) p += 2;
        pf_t *np = of_realloc(*pfs, (size_t)(*npf + 1) * sizeof *np);
        if (!np) return;
        *pfs = np;
        (*pfs)[(*npf)++] = f;
        got += f.count;
    }
    got = 0;
    while (got < chars && p + 8 <= e) {
        cf_t c = { rd32(p), -1, -1, -1, 0, 0 };
        uint32_t m = rd32(p + 4);
        p += 8;
        if (m & 0xFFFF) {
            if (p + 2 > e) break;
            uint16_t fs = rd16(p);
            if (m & 1) c.b = (int8_t)(fs & 1);
            if (m & 2) c.i = (int8_t)((fs >> 1) & 1);
            if (m & 4) c.u = (int8_t)((fs >> 2) & 1);
            p += 2;
        }
        if (m & 0x10000) p += 2;
        if (m & 0x200000) p += 2;
        if (m & 0x400000) p += 2;
        if (m & 0x800000) p += 2;
        if (m & 0x20000) { if (p + 2 > e) break; c.size = rd16(p); p += 2; }
        if (m & 0x40000) {
            if (p + 4 > e) break;
            if (p[3] == 0xFE) c.color = 0x1000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            p += 4;
        }
        if (m & 0x80000) p += 2;
        cf_t *nc = of_realloc(*cfs, (size_t)(*ncf + 1) * sizeof *nc);
        if (!nc) return;
        *cfs = nc;
        (*cfs)[(*ncf)++] = c;
        got += c.count;
    }
}

/* text atoms (chars or bytes, with an optional style atom) into a text body */
static void fill_text(sl_text_t *tx, int type, const uint16_t *u, uint32_t n, const uint8_t *style, uint32_t style_n)
{
    pf_t *pf = NULL;
    cf_t *cf = NULL;
    int npf = 0, ncf = 0;
    if (style) read_style(style, style_n, n + 1, &pf, &npf, &cf, &ncf);
    bool title = type == 0 || type == 6;
    bool body = type == 1 || type == 5 || type == 7 || type == 8;
    float base = title ? 36 : body ? 24 : 18;
    int pi = 0, ci = 0;
    uint32_t pleft = npf ? pf[0].count : 0xFFFFFFFF, cleft = ncf ? cf[0].count : 0xFFFFFFFF;
    sl_para_t *p = NULL;
    of_sb_t run = { 0 };
    cf_t cur = { 0, -1, -1, -1, 0, 0 };
    if (ncf) cur = cf[0];
    for (uint32_t k = 0; k <= n; k++) {
        if (!p) {
            p = sl_add_para(tx);
            if (!p) break;
            const pf_t *f = pi < npf ? &pf[pi] : NULL;
            p->level = (uint8_t)(f && f->level < 9 ? f->level : 0);
            p->align = (uint8_t)(f && f->align == 1 ? OF_AL_CENTER : f && f->align == 2 ? OF_AL_RIGHT : f && f->align == 3 ? OF_AL_JUSTIFY
                               : title && type == 6 ? OF_AL_CENTER : OF_AL_LEFT);
            bool bullet = f && f->bullet >= 0 ? f->bullet : body;
            if (bullet) {
                uint32_t bc = f && f->buchar && f->buchar >= 32 && (f->buchar < 0xF000) ? f->buchar : 0x2022;
                char b8[8];
                b8[of_utf8_put(b8, bc)] = 0;
                snprintf(p->bullet, sizeof p->bullet, "%s", b8);
            }
            p->margin = bullet ? 22.0f + 27 * p->level : 0;
            p->indent = bullet ? -20 : 0;
            p->empty_size = base - p->level * 2;
            p->after = 4;
            p->bullet_color = 0x1000000;
        }
        bool end = k == n;
        uint16_t ch = end ? '\r' : u[k];
        bool style_change = !end && cleft == 0;
        if ((ch == '\r' || style_change || end) && run.n) {
            sl_run_t *r = sl_add_run(p, run.s, run.n);
            if (r) {
                r->size = cur.size ? cur.size : base - p->level * 2;
                r->b = cur.b > 0 || (title && cur.b < 0);
                r->i = cur.i > 0;
                r->u = cur.u > 0;
                r->color = cur.color ? cur.color : 0x1000000;
            }
            run.n = 0;
        }
        if (style_change && ci + 1 < ncf) { ci++; cur = cf[ci]; cleft = cf[ci].count; }
        if (end) break;
        if (ch == '\r') {
            p = NULL;
            if (pleft) pleft--;
            if (!pleft && pi + 1 < npf) { pi++; pleft = pf[pi].count; }
            if (cleft) cleft--;
            continue;
        }
        char b[4];
        uint32_t cp = ch == 0x0B ? '\n' : ch == 0x09 ? ' ' : ch;
        if (cp >= 32 || cp == '\n') of_sb_put(&run, b, (size_t)of_utf8_put(b, cp));
        if (pleft) pleft--;
        if (!pleft && pi + 1 < npf) { pi++; pleft = pf[pi].count; }
        if (cleft) cleft--;
    }
    of_sb_free(&run);
    of_free(pf);
    of_free(cf);
    tx->anchor = title ? 1 : 0;
}

/* the text atoms that follow a header in a container: their chars, and the style atom after */
static bool text_after(const uint8_t *d, uint32_t n, size_t from, uint16_t **out, uint32_t *nout, const uint8_t **style, uint32_t *sn)
{
    *out = NULL;
    *style = NULL;
    for (size_t o = from; o + 8 <= n;) {
        hdr_t h;
        if (!hdr(d, n, o, &h)) break;
        if (h.type == 0x0FA0 || h.type == 0x0FA8) {
            uint32_t cnt = h.type == 0x0FA0 ? h.len / 2 : h.len;
            uint16_t *u = of_alloc((size_t)cnt * 2 + 2);
            if (!u) return false;
            for (uint32_t i = 0; i < cnt; i++) {
                if (h.type == 0x0FA0) u[i] = rd16(h.p + i * 2);
                else {
                    uint8_t c = h.p[i];
                    if (c >= 0x80 && c < 0xA0) {
                        size_t m;
                        char *s = of_latin1_to_utf8((const char *)&c, 1, &m);
                        int l;
                        u[i] = s ? (uint16_t)of_utf8_get(s, &l) : c;
                        of_free(s);
                    } else {
                        u[i] = c;
                    }
                }
            }
            *out = u;
            *nout = cnt;
            /* a style atom right after */
            size_t o2 = o + 8 + h.len;
            hdr_t h2;
            if (hdr(d, n, o2, &h2) && h2.type == 0x0FA1) { *style = h2.p; *sn = h2.len; }
            return true;
        }
        if (h.type == 0x0F9F && o > from) break;        /* the next text's header */
        o += 8 + h.len;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Pictures                                                                    */
/* -------------------------------------------------------------------------- */

static const char *blip_file(ppt_t *P, int pib)
{
    if (pib < 1 || pib > P->nbse) return NULL;
    if (P->bse_file[pib - 1]) return P->bse_file[pib - 1][0] ? P->bse_file[pib - 1] : NULL;
    P->bse_file[pib - 1] = of_strdup("");
    uint32_t off = P->bse[pib - 1].off;
    if (!P->pics || off + 8 > P->npics_len) return NULL;
    hdr_t h;
    if (!hdr(P->pics, P->npics_len, off, &h)) return NULL;
    size_t skip;
    const char *ext;
    switch (h.type) {
    case 0xF01D: case 0xF02A: skip = (h.inst == 0x46B || h.inst == 0x6E3) ? 17 : 33; ext = "jpg"; break;
    case 0xF01E: skip = h.inst == 0x6E0 ? 17 : 33; ext = "png"; break;
    case 0xF01F: skip = h.inst == 0x7A8 ? 17 : 33; ext = "bmp"; break;
    default: return NULL;
    }
    if (h.len <= skip) return NULL;
    char name[64], path[300];
    snprintf(name, sizeof name, "ppt%08lx_%d.%s", (unsigned long)P->file_id, pib, ext);
    if (!strcmp(ext, "bmp")) {
        /* a DIB: a BMP file header in front */
        size_t dn = h.len - skip;
        uint8_t *bm = of_alloc(dn + 14);
        if (!bm) return NULL;
        uint32_t total = (uint32_t)(dn + 14), hdrsz = rd32(h.p + skip);
        uint16_t bpp = rd16(h.p + skip + 14);
        uint32_t ncol = rd32(h.p + skip + 32);
        if (!ncol && bpp <= 8) ncol = 1u << bpp;
        uint32_t offbits = 14 + hdrsz + ncol * 4;
        memcpy(bm, "BM", 2);
        memcpy(bm + 2, &total, 4);
        memset(bm + 6, 0, 4);
        memcpy(bm + 10, &offbits, 4);
        memcpy(bm + 14, h.p + skip, dn);
        bool ok = of_cache_file(name, bm, dn + 14, path, sizeof path);
        of_free(bm);
        if (!ok) return NULL;
    } else if (!of_cache_file(name, h.p + skip, h.len - skip, path, sizeof path)) {
        return NULL;
    }
    of_free(P->bse_file[pib - 1]);
    P->bse_file[pib - 1] = of_strdup(path);
    return P->bse_file[pib - 1];
}

/* -------------------------------------------------------------------------- */
/* Drawings                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct { float ox, oy, sx, sy; } xf_t;

typedef struct {
    ppt_t      *P;
    sl_slide_t *s;
    uint16_t  **list_txt;       /* the slide list's texts for this slide (OutlineTextRef) */
    uint32_t   *list_n;
    int        *list_type;
    const uint8_t **list_style;
    uint32_t   *list_sn;
    int         nlist;
    bool        notes;          /* reading a notes page: its text goes to the notes */
    of_sb_t     notes_text;
} dctx_t;

static void opt_props(const uint8_t *d, uint32_t n, int count, uint32_t *fill, bool *filled, uint32_t *line, bool *lined, float *lw, int *pib)
{
    for (int i = 0; i < count && (size_t)i * 6 + 6 <= n; i++) {
        uint16_t id = rd16(d + i * 6) & 0x3FFF;
        uint32_t v = rd32(d + i * 6 + 2);
        switch (id) {
        case 0x0181: if (!(v & 0xFF000000u) || (v >> 24) == 0) *fill = 0x1000000u | (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF); break;
        case 0x01BF: if (v & 0x100000) *filled = (v & 0x10) != 0; break;
        case 0x01C0: if (!(v >> 24)) *line = 0x1000000u | (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF); break;
        case 0x01FF: if (v & 0x80000) *lined = (v & 0x8) != 0; break;
        case 0x01CB: *lw = v / 12700.0f; break;
        case 0x0104: *pib = (int)v; break;
        default: break;
        }
    }
}

static int geom_of(int spt)
{
    switch (spt) {
    case 1: return SG_RECT;
    case 2: return SG_ROUND;
    case 3: return SG_ELLIPSE;
    case 4: return SG_DIAMOND;
    case 5: case 6: return SG_TRIANGLE;
    case 13: case 66: case 67: case 68: case 55: return SG_ARROW;
    default: return SG_RECT;
    }
}

static void walk_spgr(dctx_t *X, const uint8_t *d, uint32_t n, const xf_t *outer, int depth);

static void shape(dctx_t *X, const uint8_t *d, uint32_t n, const xf_t *g, bool is_group_head, xf_t *group_out)
{
    hdr_t h;
    int spt = 0;
    uint32_t flags = 0;
    if (child(d, n, 0xF00A, -1, &h) && h.len >= 8) { spt = h.inst; flags = rd32(h.p + 4); }
    /* the anchor: the client's (slide coordinates) or the child's (group coordinates) */
    float x = 0, y = 0, w = 0, hh = 0;
    bool anc = false;
    if (child(d, n, 0xF010, -1, &h)) {
        if (h.len >= 16) { int32_t t = (int32_t)rd32(h.p), l = (int32_t)rd32(h.p + 4), r = (int32_t)rd32(h.p + 8), b = (int32_t)rd32(h.p + 12); x = l / 8.0f; y = t / 8.0f; w = (r - l) / 8.0f; hh = (b - t) / 8.0f; anc = true; }
        else if (h.len >= 8) { int16_t t = (int16_t)rd16(h.p), l = (int16_t)rd16(h.p + 2), r = (int16_t)rd16(h.p + 4), b = (int16_t)rd16(h.p + 6); x = l / 8.0f; y = t / 8.0f; w = (r - l) / 8.0f; hh = (b - t) / 8.0f; anc = true; }
    } else if (child(d, n, 0xF00F, -1, &h) && h.len >= 16) {
        int32_t l = (int32_t)rd32(h.p), t = (int32_t)rd32(h.p + 4), r = (int32_t)rd32(h.p + 8), b = (int32_t)rd32(h.p + 12);
        x = g->ox + l * g->sx;
        y = g->oy + t * g->sy;
        w = (r - l) * g->sx;
        hh = (b - t) * g->sy;
        anc = true;
    }
    if (is_group_head) {
        /* the group's own coordinate system, mapped onto its anchor */
        if (child(d, n, 0xF009, -1, &h) && h.len >= 16 && anc) {
            int32_t l = (int32_t)rd32(h.p), t = (int32_t)rd32(h.p + 4), r = (int32_t)rd32(h.p + 8), b = (int32_t)rd32(h.p + 12);
            group_out->sx = r > l ? w / (float)(r - l) : 1;
            group_out->sy = b > t ? hh / (float)(b - t) : 1;
            group_out->ox = x - l * group_out->sx;
            group_out->oy = y - t * group_out->sy;
        }
        return;
    }
    /* FSP flags: 0x8 deleted, 0x400 the background shape (its fill is the slide's) */
    if (!anc || flags & 0x0008 || flags & 0x0400) return;
    uint32_t fill = 0x1FFFFFF, line = 0x1000000;
    bool filled = false, lined = false;
    float lw = 0.75f;
    int pib = 0;
    if (child(d, n, 0xF00B, -1, &h)) opt_props(h.p, h.len, h.inst, &fill, &filled, &line, &lined, &lw, &pib);
    /* text: its own atoms, or a reference into the slide list */
    uint16_t *u = NULL;
    uint32_t un = 0;
    const uint8_t *style = NULL;
    uint32_t sn = 0;
    int ttype = 4;
    bool own = false;
    hdr_t tb;
    if (child(d, n, 0xF00D, -1, &tb)) {
        hdr_t th, ref;
        if (child(tb.p, tb.len, 0x0F9F, -1, &th) && th.len >= 4) ttype = (int)rd32(th.p);
        if (child(tb.p, tb.len, 0x0F9E, -1, &ref) && ref.len >= 4) {
            int idx = (int)rd32(ref.p);
            if (idx >= 0 && idx < X->nlist) {
                u = X->list_txt[idx];
                un = X->list_n[idx];
                ttype = X->list_type[idx];
                style = X->list_style[idx];
                sn = X->list_sn[idx];
            }
        } else if (text_after(tb.p, tb.len, 0, &u, &un, &style, &sn)) {
            own = true;
        }
    }
    if (X->notes) {
        if (u && (ttype == 2 || ttype == 1 || ttype == 4)) {
            for (uint32_t k = 0; k < un; k++) {
                uint32_t c = u[k] == '\r' || u[k] == 0x0B ? '\n' : u[k];
                char b[4];
                of_sb_put(&X->notes_text, b, (size_t)of_utf8_put(b, c));
            }
            if (X->notes_text.n) of_sb_putc(&X->notes_text, '\n');
        }
        if (own) of_free(u);
        return;
    }
    sl_shape_t *sh = sl_add_shape(X->s);
    if (!sh) { if (own) of_free(u); return; }
    sh->placeholder = true;
    sh->x = x; sh->y = y; sh->w = w; sh->h = hh;
    if (spt == 20) {
        sh->kind = SL_LINE;
        sh->line = line;
        sh->line_w = lw;
        if (child(d, n, 0xF00A, -1, &h) && h.len >= 8) { uint32_t f = rd32(h.p + 4); sh->flip_h = f & 0x40; sh->flip_v = f & 0x80; }
    } else if (pib > 0 && (spt == 75 || !u)) {
        const char *f = blip_file(X->P, pib);
        if (f) { sh->kind = SL_PIC; sh->pic = of_strdup(f); }
    }
    if (sh->kind == SL_TEXT) {
        sh->geom = spt == 202 || spt == 0 ? SG_NONE : geom_of(spt);
        if (filled && spt != 202) { sh->fill = fill; if (!sh->geom) sh->geom = SG_RECT; }
        if (lined && spt != 202) { sh->line = line; sh->line_w = lw; if (!sh->geom) sh->geom = SG_RECT; }
        if (u) fill_text(&sh->tx, ttype, u, un, style, sn);
        if ((ttype == 0 || ttype == 6) && u && !X->s->title) {
            of_sb_t t = { 0 };
            for (uint32_t k = 0; k < un; k++) {
                char b[4];
                uint32_t c = u[k] == '\r' || u[k] == 0x0B ? ' ' : u[k];
                of_sb_put(&t, b, (size_t)of_utf8_put(b, c));
            }
            X->s->title = of_sb_take(&t);
        }
        if (!sh->tx.np && !sh->fill && !sh->line) X->s->nsh--;
    }
    if (own) of_free(u);
}

static void walk_spgr(dctx_t *X, const uint8_t *d, uint32_t n, const xf_t *outer, int depth)
{
    if (depth > 10) return;
    xf_t g = *outer;
    bool first = true;
    for (size_t o = 0; o + 8 <= n;) {
        hdr_t h;
        if (!hdr(d, n, o, &h)) break;
        if (h.type == 0xF004) {
            /* the first shape of a group is the group itself */
            if (first) { xf_t ng = g; shape(X, h.p, h.len, outer, true, &ng); g = ng; }
            else shape(X, h.p, h.len, &g, false, NULL);
            first = false;
        } else if (h.type == 0xF003) {
            /* a nested group: its first shape maps its children */
            hdr_t head;
            xf_t ng = g;
            if (child(h.p, h.len, 0xF004, -1, &head)) shape(X, head.p, head.len, &g, true, &ng);
            walk_spgr(X, h.p, h.len, &ng, depth + 1);
            first = false;
        }
        o += 8 + h.len;
    }
}

static void drawing(dctx_t *X, const uint8_t *cont, uint32_t n)
{
    hdr_t ppd, dg, spgr;
    if (!child(cont, n, 0x040C, -1, &ppd) || !child(ppd.p, ppd.len, 0xF002, -1, &dg) || !child(dg.p, dg.len, 0xF003, -1, &spgr)) return;
    xf_t id = { 0, 0, 1.0f / 8, 1.0f / 8 };
    walk_spgr(X, spgr.p, spgr.len, &id, 0);
    /* a background shape with a fill: the slide's colour */
    hdr_t bg;
    if (child(dg.p, dg.len, 0xF004, -1, &bg)) {
        hdr_t opt;
        if (child(bg.p, bg.len, 0xF00B, -1, &opt)) {
            uint32_t fill = 0, line = 0;
            bool filled = true, lined = false;
            float lw;
            int pib = 0;
            opt_props(opt.p, opt.len, opt.inst, &fill, &filled, &line, &lined, &lw, &pib);
            if (fill && filled && !X->notes) X->s->bg = fill;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* The file                                                                    */
/* -------------------------------------------------------------------------- */

struct of_deck_s *of_ppt_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, 96u * 1024 * 1024);
    if (!raw) { snprintf(err, errlen, "read"); return NULL; }
    of_cfb_t C;
    if (!of_cfb_open(&C, raw, len)) { of_free(raw); snprintf(err, errlen, "format"); return NULL; }
    size_t sn = 0, un = 0, pn = 0;
    int si = of_cfb_find(&C, "PowerPoint Document"), ui = of_cfb_find(&C, "Current User"), pi = of_cfb_find(&C, "Pictures");
    uint8_t *s = si >= 0 ? of_cfb_read(&C, si, &sn) : NULL;
    uint8_t *cu = ui >= 0 ? of_cfb_read(&C, ui, &un) : NULL;
    uint8_t *pics = pi >= 0 ? of_cfb_read(&C, pi, &pn) : NULL;
    of_deck_t *dk = NULL;
    ppt_t P = { .s = s, .n = sn, .pics = pics, .npics_len = pn };
    P.file_id = of_crc32(0, path, strlen(path)) ^ (uint32_t)len;
    if (!s || !cu || un < 20) { snprintf(err, errlen, of_cfb_find(&C, "EncryptedPackage") >= 0 ? "encrypted" : "format"); goto done; }
    if (rd32(cu + 12) == 0xF3D1C4DF) { snprintf(err, errlen, "encrypted"); goto done; }
    /* the edits, newest first: the persist directory */
    uint32_t edit = rd32(cu + 16), doc_ref = 0;
    P.npers = 4096;
    P.pers = of_alloc(P.npers * sizeof *P.pers);
    for (int steps = 0; P.pers && edit && edit + 8 + 28 <= sn && steps < 64; steps++) {
        hdr_t h;
        if (!hdr(s, sn, edit, &h) || h.type != 0x0FF5 || h.len < 28) break;
        if (!doc_ref) doc_ref = rd32(h.p + 16);
        if (rd32(h.p + 20) == 0) { /* no encryption reference: fine */ }
        uint32_t last = rd32(h.p + 8), dir = rd32(h.p + 12);
        hdr_t dh;
        if (hdr(s, sn, dir, &dh) && dh.type == 0x1772) {
            for (uint32_t o = 0; o + 4 <= dh.len;) {
                uint32_t v = rd32(dh.p + o);
                uint32_t id = v & 0xFFFFF, cnt = v >> 20;
                o += 4;
                for (uint32_t k = 0; k < cnt && o + 4 <= dh.len; k++, o += 4) {
                    uint32_t pid = id + k;
                    if (pid >= P.npers) {
                        uint32_t nn = pid + 1024;
                        uint32_t *np = of_realloc(P.pers, nn * sizeof *np);
                        if (!np) break;
                        memset(np + P.npers, 0, (nn - P.npers) * sizeof *np);
                        P.pers = np;
                        P.npers = nn;
                    }
                    if (!P.pers[pid]) P.pers[pid] = rd32(dh.p + o) + 1;
                }
            }
        }
        if (last == edit) break;
        edit = last;
    }
    hdr_t doc;
    const uint8_t *dd = P.pers ? persist(&P, doc_ref, &doc) : NULL;
    if (!dd || doc.type != 0x03E8) { snprintf(err, errlen, "format"); goto done; }
    dk = of_alloc(sizeof *dk);
    if (!dk) goto done;
    snprintf(dk->kind, sizeof dk->kind, "PPT");
    dk->pic_files = true;
    dk->w = 720;
    dk->h = 540;
    hdr_t da;
    if (child(doc.p, doc.len, 0x03E9, -1, &da) && da.len >= 8) {
        int32_t x = (int32_t)rd32(da.p), y = (int32_t)rd32(da.p + 4);
        if (x > 0 && y > 0) { dk->w = x / 8.0f; dk->h = y / 8.0f; }
    }
    /* the picture store */
    hdr_t dgg, bstore;
    hdr_t dgc;
    if (child(doc.p, doc.len, 0x040B, -1, &dgc) && child(dgc.p, dgc.len, 0xF000, -1, &dgg) && child(dgg.p, dgg.len, 0xF001, -1, &bstore)) {
        for (size_t o = 0; o + 8 <= bstore.len;) {
            hdr_t b;
            if (!hdr(bstore.p, bstore.len, o, &b)) break;
            void *nb = of_realloc(P.bse, (size_t)(P.nbse + 1) * sizeof *P.bse);
            if (!nb) break;
            P.bse = nb;
            P.bse[P.nbse].type = b.len >= 1 ? b.p[0] : 0;
            P.bse[P.nbse].size = b.len >= 24 ? rd32(b.p + 20) : 0;
            P.bse[P.nbse].off = b.len >= 32 ? rd32(b.p + 28) : 0xFFFFFFFF;
            P.nbse++;
            o += 8 + b.len;
        }
        P.bse_file = of_alloc((size_t)(P.nbse + 1) * sizeof(char *));
    }
    P.dk = dk;
    /* the slides, in the slide list's order */
    for (size_t o = 0; o + 8 <= doc.len;) {
        hdr_t sl;
        if (!hdr(doc.p, doc.len, o, &sl)) break;
        o += 8 + sl.len;
        if (sl.type != 0x0FF0 || sl.inst != 0) continue;
        for (size_t q = 0; q + 8 <= sl.len;) {
            hdr_t a;
            if (!hdr(sl.p, sl.len, q, &a)) break;
            size_t at = q;
            q += 8 + a.len;
            if (a.type != 0x03F3 || a.len < 8) continue;
            /* this slide's texts in the list, up to the next slide */
            dctx_t X = { .P = &P };
            for (size_t t = q; t + 8 <= sl.len;) {
                hdr_t th;
                if (!hdr(sl.p, sl.len, t, &th) || th.type == 0x03F3) break;
                if (th.type == 0x0F9F && th.len >= 4) {
                    uint16_t *u;
                    uint32_t un2, ssn = 0;
                    const uint8_t *st;
                    if (text_after(sl.p, sl.len, t + 8 + th.len, &u, &un2, &st, &ssn)) {
                        int k = X.nlist;
                        X.list_txt = of_realloc(X.list_txt, (size_t)(k + 1) * sizeof *X.list_txt);
                        X.list_n = of_realloc(X.list_n, (size_t)(k + 1) * sizeof *X.list_n);
                        X.list_type = of_realloc(X.list_type, (size_t)(k + 1) * sizeof *X.list_type);
                        X.list_style = of_realloc(X.list_style, (size_t)(k + 1) * sizeof *X.list_style);
                        X.list_sn = of_realloc(X.list_sn, (size_t)(k + 1) * sizeof *X.list_sn);
                        if (!X.list_txt || !X.list_n || !X.list_type || !X.list_style || !X.list_sn) { of_free(u); break; }
                        X.list_txt[k] = u;
                        X.list_n[k] = un2;
                        X.list_type[k] = (int)rd32(th.p);
                        X.list_style[k] = st;
                        X.list_sn[k] = ssn;
                        X.nlist++;
                    }
                }
                t += 8 + th.len;
            }
            (void)at;
            sl_slide_t *ns = of_realloc(dk->s, (size_t)(dk->ns + 1) * sizeof *ns);
            if (!ns) break;
            dk->s = ns;
            sl_slide_t *slide = &dk->s[dk->ns++];
            memset(slide, 0, sizeof *slide);
            slide->bg = 0x1FFFFFF;
            X.s = slide;
            hdr_t sc;
            const uint8_t *sd = persist(&P, rd32(a.p), &sc);
            if (sd && sc.type == 0x03EE) {
                drawing(&X, sc.p, sc.len);
                hdr_t sa, info;
                if (child(sc.p, sc.len, 0x03F9, -1, &info) && info.len >= 12 && (rd16(info.p + 10) & 4)) slide->hidden = true;
                if (child(sc.p, sc.len, 0x03EF, -1, &sa) && sa.len >= 20) {
                    hdr_t nc;
                    const uint8_t *nd = persist(&P, rd32(sa.p + 16), &nc);
                    if (nd && nc.type == 0x03F0) {
                        dctx_t N = { .P = &P, .s = slide, .notes = true };
                        drawing(&N, nc.p, nc.len);
                        if (N.notes_text.n) {
                            while (N.notes_text.n && N.notes_text.s[N.notes_text.n - 1] == '\n') N.notes_text.n--;
                            slide->notes = of_sb_take(&N.notes_text);
                        }
                        of_sb_free(&N.notes_text);
                    }
                }
            } else {
                /* no drawing found: the slide list's text, title and body */
                for (int k = 0; k < X.nlist; k++) {
                    sl_shape_t *sh = sl_add_shape(slide);
                    if (!sh) break;
                    sh->placeholder = true;
                    bool title = X.list_type[k] == 0 || X.list_type[k] == 6;
                    sh->x = dk->w * 0.08f;
                    sh->w = dk->w * 0.84f;
                    sh->y = title ? dk->h * 0.06f : dk->h * 0.26f;
                    sh->h = title ? dk->h * 0.16f : dk->h * 0.66f;
                    fill_text(&sh->tx, X.list_type[k], X.list_txt[k], X.list_n[k], X.list_style[k], X.list_sn[k]);
                }
            }
            for (int k = 0; k < X.nlist; k++) of_free(X.list_txt[k]);
            of_free(X.list_txt);
            of_free(X.list_n);
            of_free(X.list_type);
            of_free(X.list_style);
            of_free(X.list_sn);
        }
        break;
    }
    if (!dk->ns) { of_deck_free(dk); dk = NULL; snprintf(err, errlen, "empty"); }
done:
    for (int i = 0; P.bse_file && i < P.nbse; i++) of_free(P.bse_file[i]);
    of_free(P.bse_file);
    of_free(P.bse);
    of_free(P.pers);
    of_free(s);
    of_free(cu);
    of_free(pics);
    of_cfb_close(&C);
    /* the deck keeps no file: its pictures are in the cache */
    of_free(raw);
    if (dk) for (int i = 0; i < dk->ns; i++) for (int k = 0; k < dk->s[i].nsh; k++) sl_number(&dk->s[i].sh[k].tx);
    return dk;
}
