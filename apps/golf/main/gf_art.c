/*
 * GOLF - the art rendered in Blender (see gf_art.h)
 */
/* The .so is compiled with -Os (components/elf_loader/elf_loader.cmake) and
 * per-file CMake options do not reach that compile: this is the only way to
 * give the pixel and physics loops -O2. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "gf_art.h"
#include "gf_view3d.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTRY_SIZE  50
#define NAME_LEN    24
#define HATS        6

enum { T_SPRITE = 1, T_PLANE = 2, T_GOLFER = 3 };

typedef struct {
    char     name[NAME_LEN];
    uint8_t  type, flags;
    uint16_t w, h;
    int16_t  ax, ay;
    uint32_t off, clen, rawlen;
    uint16_t ppm8, extra;
} entry_t;

struct gf_tree_art {
    gf_mip_t side[GF_TREE_KINDS], top[GF_TREE_KINDS], shadow[GF_TREE_KINDS];
    float    height[GF_TREE_KINDS], diam[GF_TREE_KINDS];
    bool     ok[GF_TREE_KINDS];
    gf_mip_t flag[4];
    int      nflag;
};

#define MAX_FRAMES  32

typedef struct {
    int16_t     body, shadow, hat[HATS];    /* entry indices, -1 = none      */
} frame_t;

/* The pack stays on the card: only its table is in memory, and an entry is
 * read when it is needed (the trees at load, the golfer's frames when a
 * sequence is prepared, always from the worker task).
 *
 * P4OS: the golfer is coloured when he is DRAWN, not before. At twice the
 * watch's pixels the coloured frames of the swing and the wait alone were
 * 5.5 MB (8 MB with the hole card's cheer and sadness); what stays in PSRAM
 * is each frame's LZ4 block as it is in the pack (~1 MB for the swing and
 * the wait), and drawing a frame decodes it into a scratch buffer and lights
 * each pixel from a table of the outfit's colours (s_lut). A new outfit is
 * a new table: nothing is coloured again. */
static FILE         *s_fp;
static size_t        s_pak_len;
static entry_t      *s_ent;
static uint8_t     **s_blob;             /* per entry: its LZ4 block, NULL = on the card */
static int           s_nent;
static struct gf_tree_art s_trees;
static bool          s_trees_ok;
static frame_t       s_frames[SEQ_N][MAX_FRAMES];
static int           s_nframes[SEQ_N];
static uint32_t      s_pal[RG_N];
static uint32_t      s_lut[16][256];     /* region x grey -> RGB888          */
static int           s_hat = -1;
static volatile unsigned s_ready_mask;   /* sequences prepared since the last outfit */
/* the decoded layers of the frame drawn last: body, hat, shadow */
static uint8_t      *s_scr[3];
static uint32_t      s_scr_cap[3];
static int           s_scr_ent[3] = { -1, -1, -1 };
static int           s_version;

static const char *const SEQ_NAME[SEQ_N] = { "swing", "idle", "cheer", "sad", "turn" };
static const char *const TREE_NAME[GF_TREE_KINDS] = { "tree_pine", "tree_oak", "tree_poplar", "tree_palm", "bush" };

/* --------------------------------------------------------------------------
 * The pack
 * -------------------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* The standard LZ4 block format. */
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
        for (size_t k = 0; k < m; k++, o++) {
            dst[o] = dst[o - off];
        }
    }
    return o == dlen;
}

static int find(const char *name)
{
    for (int i = 0; i < s_nent; i++) {
        if (!strncmp(s_ent[i].name, name, NAME_LEN)) return i;
    }
    return -1;
}

static uint8_t *unpack(int idx)
{
    const entry_t *e = &s_ent[idx];
    if (!s_fp || e->off + e->clen > s_pak_len) return NULL;
    uint8_t *raw = (uint8_t *)gf_malloc(e->rawlen ? e->rawlen : 1);
    uint8_t *src = (uint8_t *)gf_malloc(e->clen ? e->clen : 1);
    bool ok = raw && src && fseek(s_fp, (long)e->off, SEEK_SET) == 0 &&
              fread(src, 1, e->clen, s_fp) == e->clen && lz4_decode(src, e->clen, raw, e->rawlen);
    gf_free(src);
    if (!ok) {
        gf_free(raw);
        return NULL;
    }
    return raw;
}

/* a colour sprite or an alpha plane, into a mip chain */
static bool load_mip(const char *name, gf_mip_t *m)
{
    int i = find(name);
    if (i < 0) return false;
    const entry_t *e = &s_ent[i];
    uint8_t *raw = unpack(i);
    if (!raw) return false;
    gf_sprite_t s = { 0 };
    s.w = (int16_t)e->w;
    s.h = (int16_t)e->h;
    s.ox = e->ax;
    s.oy = e->ay;
    size_t n = (size_t)e->w * e->h;
    if (e->type == T_SPRITE) {
        s.px = (uint16_t *)gf_malloc(n * 2);
        s.a = (uint8_t *)gf_malloc(n);
        if (!s.px || !s.a) {
            gf_free(s.px);
            gf_free(s.a);
            gf_free(raw);
            return false;
        }
        for (size_t k = 0; k < n; k++) {
            s.px[k] = rd16(raw + 2 * k);
        }
        memcpy(s.a, raw + 2 * n, n);
        gf_free(raw);
    } else {
        s.px = NULL;
        s.a = raw;          /* the plane is the alpha */
    }
    gf_mip_build(m, &s);
    m->ppm = (float)e->ppm8 / 8.0f;
    return true;
}

static void load_trees(void)
{
    memset(&s_trees, 0, sizeof(s_trees));
    char nm[NAME_LEN];
    int any = 0;
    for (int k = 0; k < GF_TREE_KINDS; k++) {
        snprintf(nm, sizeof nm, "%.12s_s", TREE_NAME[k]);
        bool a = load_mip(nm, &s_trees.side[k]);
        int si = find(nm);
        snprintf(nm, sizeof nm, "%.12s_t", TREE_NAME[k]);
        bool b = load_mip(nm, &s_trees.top[k]);
        int ti = find(nm);
        snprintf(nm, sizeof nm, "%.12s_h", TREE_NAME[k]);
        bool c = load_mip(nm, &s_trees.shadow[k]);
        s_trees.ok[k] = a && b && c;
        if (si >= 0) s_trees.height[k] = (float)s_ent[si].extra / 100.0f;
        if (ti >= 0) s_trees.diam[k] = (float)s_ent[ti].extra / 100.0f;
        any += s_trees.ok[k];
    }
    for (int f = 0; f < 4; f++) {
        snprintf(nm, sizeof nm, "flag_%d", f);
        if (load_mip(nm, &s_trees.flag[f])) s_trees.nflag = f + 1;
    }
    s_trees_ok = any > 0;
}

static void index_golfer(void)
{
    char nm[NAME_LEN];
    for (int q = 0; q < SEQ_N; q++) {
        s_nframes[q] = 0;
        for (int f = 0; f < MAX_FRAMES; f++) {
            frame_t *fr = &s_frames[q][f];
            memset(fr, 0, sizeof(*fr));
            snprintf(nm, sizeof nm, "%s_%02d", SEQ_NAME[q], f);
            fr->body = (int16_t)find(nm);
            if (fr->body < 0) break;
            snprintf(nm, sizeof nm, "%s_%02ds", SEQ_NAME[q], f);
            fr->shadow = (int16_t)find(nm);
            for (int h = 0; h < HATS; h++) {
                snprintf(nm, sizeof nm, "h%d%.2s%02d", h, SEQ_NAME[q], f);
                fr->hat[h] = (int16_t)find(nm);
            }
            s_nframes[q] = f + 1;
        }
    }
}

bool gf_art_load(const char *path)
{
    gf_art_free();
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t hdr[8];
    if (len < 8 || fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "GFPK", 4) != 0) {
        fclose(f);
        return false;
    }
    s_fp = f;
    s_pak_len = (size_t)len;
    s_version = rd16(hdr + 4);
    s_nent = rd16(hdr + 6);
    uint8_t *table = (uint8_t *)gf_malloc((size_t)s_nent * ENTRY_SIZE);
    s_ent = (entry_t *)gf_calloc((size_t)s_nent, sizeof(entry_t));
    s_blob = (uint8_t **)gf_calloc((size_t)s_nent, sizeof(uint8_t *));
    if (!table || !s_ent || !s_blob || fread(table, ENTRY_SIZE, (size_t)s_nent, f) != (size_t)s_nent) {
        gf_free(table);
        gf_art_free();
        return false;
    }
    /* <24s B B H H h h I I I H H>: 50 bytes, see tools/pack_assets.py */
    for (int i = 0; i < s_nent; i++) {
        const uint8_t *p = table + (size_t)i * ENTRY_SIZE;
        entry_t *e = &s_ent[i];
        memcpy(e->name, p, NAME_LEN);
        e->name[NAME_LEN - 1] = 0;
        e->type = p[24];
        e->flags = p[25];
        e->w = rd16(p + 26);
        e->h = rd16(p + 28);
        e->ax = (int16_t)rd16(p + 30);
        e->ay = (int16_t)rd16(p + 32);
        e->off = rd32(p + 34);
        e->clen = rd32(p + 38);
        e->rawlen = rd32(p + 42);
        e->ppm8 = rd16(p + 46);
        e->extra = rd16(p + 48);
    }
    gf_free(table);
    uint64_t t0 = aos_hal_uptime_ms();
    load_trees();
    /* the golfer only if he was rendered for this screen: version 2 is the
     * watch's cameras at twice its pixels (GF_ART_SCALE) */
    if (s_version == GF_ART_SCALE) index_golfer();
    else aos_hal_log("golf", "pak version %d: its golfer is for another screen, not used", s_version);
    aos_hal_log("golf", "pak: %d entries, %u KB, trees and index %u ms", s_nent, (unsigned)(s_pak_len / 1024),
                (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0));
    return true;
}

void gf_art_free(void)
{
    for (int q = 0; q < SEQ_N; q++) {
        gf_art_release(q);
    }
    for (int k = 0; k < GF_TREE_KINDS; k++) {
        gf_mip_free(&s_trees.side[k]);
        gf_mip_free(&s_trees.top[k]);
        gf_mip_free(&s_trees.shadow[k]);
    }
    for (int f = 0; f < 4; f++) {
        gf_mip_free(&s_trees.flag[f]);
    }
    memset(&s_trees, 0, sizeof(s_trees));
    s_trees_ok = false;
    if (s_blob) {
        for (int i = 0; i < s_nent; i++) gf_free(s_blob[i]);
    }
    gf_free(s_blob);
    s_blob = NULL;
    for (int k = 0; k < 3; k++) {
        gf_free(s_scr[k]);
        s_scr[k] = NULL;
        s_scr_cap[k] = 0;
        s_scr_ent[k] = -1;
    }
    gf_free(s_ent);
    s_ent = NULL;
    s_nent = 0;
    if (s_fp) fclose(s_fp);
    s_fp = NULL;
    s_pak_len = 0;
    memset(s_nframes, 0, sizeof(s_nframes));
}

/* --------------------------------------------------------------------------
 * Trees
 * -------------------------------------------------------------------------- */

const gf_tree_art_t *gf_art_trees(void)
{
    return s_trees_ok ? &s_trees : NULL;
}

bool gf_art_tree_top(const gf_tree_art_t *a, int kind)
{
    return a && kind >= 0 && kind < GF_TREE_KINDS && a->ok[kind];
}

void gf_art_tree_draw_top(const gf_tree_art_t *a, gf_img_t *im, int kind, float cx, float cy, float ppm, float size, int tint)
{
    const gf_mip_t *m = &a->top[kind];
    gf_mip_draw(im, m, cx, cy, ppm * size / m->ppm, 255, tint, NULL, 0, 0, 0, 0, 0);
}

void gf_art_tree_shadow(const gf_tree_art_t *a, gf_img_t *im, int kind, float cx, float cy, float ppm, float size)
{
    const gf_mip_t *m = &a->shadow[kind];
    gf_mip_shadow(im, m, cx, cy, ppm * size / m->ppm, 150);
}

const gf_mip_t *gf_art_tree_side(int kind)
{
    return s_trees_ok && s_trees.ok[kind] ? &s_trees.side[kind] : NULL;
}

const gf_mip_t *gf_art_flag(int frame)
{
    return s_trees.nflag ? &s_trees.flag[frame % s_trees.nflag] : NULL;
}

float gf_art_tree_height(int kind)
{
    return s_trees.height[kind];
}

float gf_art_tree_radius(int kind)
{
    return s_trees.diam[kind] * 0.5f;
}

/* --------------------------------------------------------------------------
 * The golfer
 * -------------------------------------------------------------------------- */

bool gf_art_have_golfer(void)
{
    return s_nframes[SEQ_SWING] > 0;
}

int gf_art_frames(int seq)
{
    return seq >= 0 && seq < SEQ_N ? s_nframes[seq] : 0;
}

/* frees the blocks of one sequence: its bodies, shadows and hats */
void gf_art_release(int seq)
{
    if (!s_blob) return;
    for (int f = 0; f < s_nframes[seq]; f++) {
        frame_t *fr = &s_frames[seq][f];
        int16_t idx[2 + HATS] = { fr->body, fr->shadow };
        for (int h = 0; h < HATS; h++) idx[2 + h] = fr->hat[h];
        for (int k = 0; k < 2 + HATS; k++) {
            int i = idx[k];
            if (i < 0 || !s_blob[i]) continue;
            for (int q = 0; q < 3; q++) if (s_scr_ent[q] == i) s_scr_ent[q] = -1;
            gf_free(s_blob[i]);
            s_blob[i] = NULL;
        }
    }
    s_ready_mask &= ~(1u << seq);
}

unsigned gf_art_ready(void)
{
    return s_ready_mask;
}

void gf_art_hold(void)
{
    s_ready_mask = 0;
}

/* the outfit's table: every region in every grey, the watch's colouring */
static void build_lut(void)
{
    for (int id = 0; id < 16; id++) {
        uint32_t c = s_pal[id < RG_N ? id : 0];
        int cr = (int)(c >> 16) & 255, cg = (int)(c >> 8) & 255, cb = (int)c & 255;
        for (int g = 0; g < 256; g++) {
            /* the neutral grey renders around 200 in full light: that is "as is" */
            int k = g * 256 / 196;
            int rr = cr * k >> 8, gg = cg * k >> 8, bb = cb * k >> 8;
            /* past white, towards white: highlights keep their hue */
            int over = rr > 255 ? rr - 255 : 0;
            if (gg - 255 > over) over = gg - 255;
            if (bb - 255 > over) over = bb - 255;
            if (over > 0) {
                rr += over / 2; gg += over / 2; bb += over / 2;
            }
            rr = rr > 255 ? 255 : rr;
            gg = gg > 255 ? 255 : gg;
            bb = bb > 255 ? 255 : bb;
            s_lut[id][g] = (uint32_t)rr << 16 | (uint32_t)gg << 8 | (uint32_t)bb;
        }
    }
}

void gf_art_outfit(const uint8_t eq[CAT_N])
{
    s_ready_mask = 0;
    int old_hat = s_hat;
    gf_outfit_palette(eq, s_pal, &s_hat);
    build_lut();
    /* another hat: the old one's blocks go (the UI draws nothing until the
     * sequences are prepared again, gfp_outfit) */
    if (old_hat != s_hat && old_hat >= 0 && old_hat < HATS && s_blob) {
        for (int q = 0; q < SEQ_N; q++) {
            for (int f = 0; f < s_nframes[q]; f++) {
                int i = s_frames[q][f].hat[old_hat];
                if (i < 0 || !s_blob[i]) continue;
                for (int k = 0; k < 3; k++) if (s_scr_ent[k] == i) s_scr_ent[k] = -1;
                gf_free(s_blob[i]);
                s_blob[i] = NULL;
            }
        }
    }
}

/* an entry's LZ4 block, read from the card into PSRAM */
static size_t load_blob(int i)
{
    if (i < 0) return 0;
    if (s_blob[i]) return s_ent[i].clen;
    const entry_t *e = &s_ent[i];
    if (!s_fp || e->off + e->clen > s_pak_len) return 0;
    uint8_t *b = (uint8_t *)gf_malloc(e->clen ? e->clen : 1);
    if (!b) return 0;
    if (fseek(s_fp, (long)e->off, SEEK_SET) != 0 || fread(b, 1, e->clen, s_fp) != e->clen) {
        gf_free(b);
        return 0;
    }
    s_blob[i] = b;
    return e->clen;
}

/* Reads the blocks of every frame of the sequences in 'mask' (1 << SEQ_*)
 * that is not in memory yet: body, shadow and the hat being worn. From the
 * worker: it reads the card. */
size_t gf_art_prepare(unsigned mask)
{
    /* the wait first: it is what the menu shows */
    static const uint8_t ORDER[SEQ_N] = { SEQ_IDLE, SEQ_SWING, SEQ_CHEER, SEQ_SAD, SEQ_TURN };
    size_t bytes = 0;
    if (!s_blob) return 0;
    for (int oi = 0; oi < SEQ_N; oi++) {
        int q = ORDER[oi];
        if (!(mask & (1u << q))) continue;
        for (int f = 0; f < s_nframes[q]; f++) {
            frame_t *fr = &s_frames[q][f];
            bytes += load_blob(fr->body);
            bytes += load_blob(fr->shadow);
            if (s_hat >= 0 && s_hat < HATS) bytes += load_blob(fr->hat[s_hat]);
        }
        s_ready_mask |= 1u << q;
        gf_yield();
    }
    return bytes;
}

/* an entry decoded into scratch slot k (0 body, 1 hat, 2 shadow); the slot
 * keeps it, so the same frame drawn again costs no decoding */
static const uint8_t *decoded(int i, int k)
{
    if (i < 0 || !s_blob || !s_blob[i]) return NULL;
    if (s_scr_ent[k] == i) return s_scr[k];
    const entry_t *e = &s_ent[i];
    if (e->rawlen > s_scr_cap[k]) {
        gf_free(s_scr[k]);
        s_scr[k] = (uint8_t *)gf_malloc(e->rawlen);
        s_scr_cap[k] = s_scr[k] ? e->rawlen : 0;
        if (!s_scr[k]) return NULL;
    }
    s_scr_ent[k] = -1;
    if (!lz4_decode(s_blob[i], e->clen, s_scr[k], e->rawlen)) return NULL;
    s_scr_ent[k] = i;
    return s_scr[k];
}

/* one golfer layer (id << 4 | alpha >> 4, grey) coloured into the picture */
static void draw_layer(gf_img_t *im, int i, int k, int dx, int dy, int *box)
{
    const uint8_t *raw = decoded(i, k);
    if (!raw) return;
    const entry_t *e = &s_ent[i];
    int X0 = e->ax + dx, Y0 = e->ay + dy;
    int x0 = X0 < im->cx0 ? im->cx0 : X0, x1 = X0 + e->w > im->cx1 ? im->cx1 : X0 + e->w;
    int y0 = Y0 < im->cy0 ? im->cy0 : Y0, y1 = Y0 + e->h > im->cy1 ? im->cy1 : Y0 + e->h;
    for (int Y = y0; Y < y1; Y++) {
        const uint8_t *p = raw + 2 * ((size_t)(Y - Y0) * e->w + (x0 - X0));
        uint16_t *row = im->px + (size_t)Y * im->w;
        for (int X = x0; X < x1; X++, p += 2) {
            int a4 = p[0] & 15;
            if (!a4) continue;
            uint32_t c = s_lut[p[0] >> 4][p[1]];
            uint16_t col = gf_dither((int)(c >> 16), (int)(c >> 8) & 255, (int)c & 255, X, Y);
            row[X] = a4 == 15 ? col : gf_blend(row[X], col, a4 * 17);
        }
    }
    if (X0 < box[0]) box[0] = X0;
    if (Y0 < box[1]) box[1] = Y0;
    if (X0 + e->w > box[2]) box[2] = X0 + e->w;
    if (Y0 + e->h > box[3]) box[3] = Y0 + e->h;
}

/* the ground shadow: stored at half the pixels, sampled back up */
static void draw_shadow(gf_img_t *im, int i, int dx, int dy, int *box)
{
    const uint8_t *raw = decoded(i, 2);
    if (!raw) return;
    const entry_t *e = &s_ent[i];
    int half = e->flags & 1;
    int W = half ? e->w * 2 : e->w, H = half ? e->h * 2 : e->h;
    int X0 = (half ? e->ax * 2 : e->ax) + dx, Y0 = (half ? e->ay * 2 : e->ay) + dy;
    int x0 = X0 < im->cx0 ? im->cx0 : X0, x1 = X0 + W > im->cx1 ? im->cx1 : X0 + W;
    int y0 = Y0 < im->cy0 ? im->cy0 : Y0, y1 = Y0 + H > im->cy1 ? im->cy1 : Y0 + H;
    for (int Y = y0; Y < y1; Y++) {
        int y = Y - Y0, sy0, sy1, ty;
        if (half) {
            /* (y - 0.5) / 2 in quarters: 0.25 / 0.75 weights, in 1/4 */
            int q = 2 * y - 1;
            sy0 = q < 0 ? 0 : q / 4;
            ty = q < 0 ? 0 : q & 3;
            sy1 = sy0 + 1 < e->h ? sy0 + 1 : e->h - 1;
        } else {
            sy0 = sy1 = y;
            ty = 0;
        }
        const uint8_t *r0 = raw + (size_t)sy0 * e->w, *r1 = raw + (size_t)sy1 * e->w;
        uint16_t *row = im->px + (size_t)Y * im->w;
        for (int X = x0; X < x1; X++) {
            int x = X - X0, s;
            if (half) {
                int q = 2 * x - 1;
                int sx0 = q < 0 ? 0 : q / 4, tx = q < 0 ? 0 : q & 3;
                int sx1 = sx0 + 1 < e->w ? sx0 + 1 : e->w - 1;
                int a = r0[sx0] * (4 - tx) + r0[sx1] * tx;
                int b = r1[sx0] * (4 - tx) + r1[sx1] * tx;
                s = (a * (4 - ty) + b * ty) >> 4;
            } else {
                s = r0[x];
            }
            if (s) row[X] = gf_scale(row[X], 256 - (s * 150 >> 8));
        }
    }
    if (X0 < box[0]) box[0] = X0;
    if (Y0 < box[1]) box[1] = Y0;
    if (X0 + W > box[2]) box[2] = X0 + W;
    if (Y0 + H > box[3]) box[3] = Y0 + H;
}

bool gf_art_draw(gf_img_t *im, int seq, int frame, int dx, int dy, bool shadow, int box[4])
{
    box[0] = box[1] = 1 << 30;
    box[2] = box[3] = -(1 << 30);
    if (seq < 0 || seq >= SEQ_N || frame < 0 || frame >= s_nframes[seq]) return false;
    const frame_t *fr = &s_frames[seq][frame];
    if (!s_blob || !s_blob[fr->body]) return false;     /* not prepared: gf_art_prepare() */
    if (shadow && fr->shadow >= 0) draw_shadow(im, fr->shadow, dx, dy, box);
    draw_layer(im, fr->body, 0, dx, dy, box);
    if (s_hat >= 0 && s_hat < HATS && fr->hat[s_hat] >= 0) draw_layer(im, fr->hat[s_hat], 1, dx, dy, box);
    return box[2] > box[0];
}

bool gf_art_frame_box(int seq, int frame, int *x0, int *y0, int *w, int *h)
{
    if (seq < 0 || seq >= SEQ_N || frame < 0 || frame >= s_nframes[seq]) return false;
    const entry_t *e = &s_ent[s_frames[seq][frame].body];
    *x0 = e->ax;
    *y0 = e->ay;
    *w = e->w;
    *h = e->h;
    return true;
}
