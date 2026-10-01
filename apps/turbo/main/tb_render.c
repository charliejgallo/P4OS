/*
 * TURBO - the pseudo-3D renderer (see tb_render.h)
 *
 * The watch's renderer, with what the P4 changed:
 *   - the screen's size, horizon and car row are tb_view's (720 x 1280 or
 *     1280 x 720), the focal length twice the watch's;
 *   - prepare works every road row out to whole pixels (rowinfo_t), so a
 *     band only clips and fills: lying down the frame is drawn in strips of
 *     8-16 columns, each of which crosses all 720 rows, and working a row's
 *     floats again in every strip was most of the road's time;
 *   - bands draw only inside their clip rectangle, columns included, and
 *     write nothing but pixels: two of them run at once, one per core;
 *   - the sky above the backdrop (a tall one, upright) is a gradient by
 *     height over the horizon, the same whatever the screen's shape, so the
 *     panorama is composited once per stage for both orientations.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "tb_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PANO_W      2048        /* the sky + backdrop panorama, wraps around */
#define PANO_H      TB_BG_H
#define SKY_SPAN    560.0f      /* rows over the horizon the gradient spans   */
#define TEX         64
#define TEX_NEAR    48.0f       /* textures closer than this, flat beyond     */
#define NEAR_Z      0.8f
#define NSTARS      360
#define STAR_D      1100        /* stars up to this far over the horizon      */
#define BG_SCROLL   15.0f       /* backdrop px per (curve x metre)            */
#define LITPOOL     640         /* headlight palettes per frame               */

typedef struct {
    float z;                    /* camera depth of the segment's start        */
    float scale;                /* px per metre there                          */
    float cx, sy;               /* screen x of the road's centre, screen y     */
    int16_t clip;               /* rows from here down are hidden              */
    bool  vis;
    /* the rows it paints: [ya, ybot), and what they need */
    int16_t ya, ybot;
    float sx2, sy2, w1, w2, iz1, iz2, hw;
    int16_t fk;                 /* fog, 0..256                                 */
} drawn_t;

typedef struct {
    uint16_t road[4], gl[4], gr[4], rumble, line, yellow;
    uint16_t wall;                  /* a tunnel's wall on this row           */
    bool void_l, void_r, tunnel;
} rowpal_t;

/* one road row, in whole pixels (tb_render_prepare) */
enum { RI_NEAR = 1, RI_LIT = 2, RI_TUNNEL = 4 };
typedef struct {
    int16_t  n;                     /* the segment painting it, -1 none      */
    uint8_t  flags, nl;
    int16_t  xl, xr, rl, rr;        /* asphalt [xl,xr), rumbles to rl and rr */
    int16_t  glow;                  /* the neon fringe outside the rumble    */
    int16_t  wl, wr;                /* a tunnel's walls: [0,wl) and [wr,W)   */
    int16_t  v;                     /* the texture's row                     */
    int32_t  du, u0;                /* texture step; the ground's u at x = 0 */
    int16_t  lx0[4], lx1[4];        /* the painted lines                     */
    uint8_t  la[4], lc[4];          /* coverage (255 solid), 0 white 1 yellow */
    int16_t  k[4];                  /* the headlights' pieces                */
    uint16_t pm, pl;                /* their palettes in the pool            */
} rowinfo_t;

/* one sprite of the frame, placed by tb_render_prepare() */
enum { DL_PROP = 0, DL_VEH, DL_STANDIN_PROP, DL_STANDIN_CAR };
#define DL_MAX 640
typedef struct {
    uint8_t type, mirror, opa, kind;    /* kind: the prop, or the vehicle model */
    uint8_t span;
    int16_t clip, ytop, ybot, fk, lut;  /* lut: traffic index, -1 the rival    */
    int16_t cx0, cx1, cy0;              /* a tunnel's mouth it is seen through */
    int16_t bx0, bx1;                   /* the columns it can touch            */
    float   z;                          /* its depth                           */
    float   x, y, sc, shsc, ppm;
    const void *m;                      /* tb_mip_t or tb_vmip_t              */
    const tb_mip_t *sh;
} dl_t;

struct tb_render {
    const tb_track_t *trk;
    tb_theme_t th;
    uint16_t *pano;             /* PANO_W x PANO_H                             */
    uint8_t   tex_road[TEX * TEX];
    uint8_t   tex_gnd[TEX * TEX];
    int16_t   star_u[NSTARS], star_d[NSTARS];
    uint8_t   star_b[NSTARS];
    float     bg_off;
    tb_lut_t  lut_rival;
    tb_paint_t paint_player, paint_rival;
    drawn_t   dr[TB_DRAW + 1];
    rowpal_t  pal[TB_DRAW + 1];
    int16_t   rowseg[TB_HMAX];  /* which segment paints each row, -1 none      */
    rowinfo_t ri[TB_HMAX];
    rowpal_t  litpool[LITPOOL];
    int       nlit;
    uint16_t  skyrow[TB_HMAX][4];   /* rows over the panorama: 4 dithered px */
    int       sky_hor;          /* the horizon skyrow was made for             */
    tb_lut_t  car_lut[TB_TRAFFIC];
    uint32_t  lut_key[TB_TRAFFIC];  /* what each LUT was built for, ~0 none   */
    dl_t      dl[DL_MAX];           /* this frame's sprites, far to near       */
    int       ndl;
    /* the player's car, coloured once per paint (car_cache) */
    tb_sprite_t car_spr[TB_NEAR_FRAMES];
    uint32_t *tail[TB_NEAR_FRAMES];  /* its tail lights: index << 8 | light     */
    uint32_t  ntail[TB_NEAR_FRAMES];
    int       cc_car;
    bool      cc_night, cc_valid;
    tb_paint_t cc_paint;
    tb_lut_t  lut_brake;
    /* the tunnel in view (prepare), as depths and screen rectangles */
    struct {
        bool  on, inside, far;          /* far: its exit beyond the view    */
        float zt0, zt1, hc;             /* entry, exit, ceiling over camera */
        int   wx0, wx1, wy0, wy1;       /* what is seen through its mouth   */
        int   ex0, ex1, ey0, ey1;       /* its exit                         */
    } tun;
    /* the frame being drawn, from tb_render_prepare() */
    float     camz, camx, camy;
    int       base, maxy, nfar, bgo, rival_n;
    bool      void_below;
    int       carx, cary, car_fr;
    bool      car_brake, car_standin;
    tb_lut_t  lut_car;
    int16_t   car_head[TB_DRAW + 1];
    int16_t   car_next[TB_TRAFFIC + 1];
};

/* --------------------------------------------------------------------------
 * Set-up
 * -------------------------------------------------------------------------- */

static void car_cache_free(tb_render_t *r);

tb_render_t *tb_render_new(void)
{
    tb_render_t *r = (tb_render_t *)tb_calloc(1, sizeof(tb_render_t));
    if (!r) return NULL;
    r->pano = (uint16_t *)tb_malloc((size_t)PANO_W * PANO_H * 2);
    if (!r->pano) {
        free(r);
        return NULL;
    }
    memset(r->pano, 0, (size_t)PANO_W * PANO_H * 2);
    /* textures: 4 levels of noise, a little streaky along the road */
    uint32_t s = 0xC0FFEEu;
    for (int y = 0; y < TEX; y++) {
        for (int x = 0; x < TEX; x++) {
            uint32_t n = tb_rand(&s);
            int v = (int)(n & 7) + (int)((n >> 3) & 7);            /* 0..14, peaked */
            int l = v < 4 ? 0 : (v < 7 ? 1 : (v < 11 ? 2 : 3));
            r->tex_road[y * TEX + x] = (uint8_t)l;
            int g = (int)((n >> 8) & 3);
            if (((x + (int)((n >> 12) & 3)) & 7) == 0) g = 3;      /* tufts */
            r->tex_gnd[y * TEX + x] = (uint8_t)g;
        }
    }
    for (int i = 0; i < NSTARS; i++) {
        r->star_u[i] = (int16_t)(tb_rand(&s) % PANO_W);
        r->star_d[i] = (int16_t)(2 + tb_rand(&s) % STAR_D);
        r->star_b[i] = (uint8_t)(90 + tb_rand(&s) % 166);
    }
    r->sky_hor = -1;
    tb_paint_t p;
    tb_paint_get(0, &p);
    tb_render_paint(r, &p, &p);
    return r;
}

void tb_render_free(tb_render_t *r)
{
    if (!r) return;
    car_cache_free(r);
    free(r->pano);
    free(r);
}

void tb_render_paint(tb_render_t *r, const tb_paint_t *p, const tb_paint_t *rival)
{
    r->paint_player = *p;
    r->paint_rival = *rival;
    memset(r->lut_key, 0xFF, sizeof r->lut_key);
}

uint32_t tb_render_bytes(const tb_render_t *r)
{
    uint32_t n = (uint32_t)sizeof(tb_render_t) + (uint32_t)PANO_W * PANO_H * 2;
    for (int f = 0; f < TB_NEAR_FRAMES; f++) {
        const tb_sprite_t *s = &r->car_spr[f];
        if (s->px) n += (uint32_t)s->w * s->h * 3 + (uint32_t)s->h * 8 + r->ntail[f] * 4;
    }
    return n;
}

/* the sky's colour d rows over the horizon: the watch's curve (it stays
 * blue longer and pales low) over a fixed height, so both orientations
 * share the panorama */
static uint32_t sky_d(const tb_theme_t *th, int d)
{
    float f = 1.0f - (float)d / SKY_SPAN;
    int tl = f <= 0 ? 0 : (f >= 1 ? 256 : (int)(f * 256.0f));
    int t = ((tl * tl >> 8) + tl) >> 1;
    return tb_mix(th->sky_top, th->sky_hor, t);
}

void tb_render_stage(tb_render_t *r, const tb_track_t *t)
{
    r->trk = t;
    r->th = t->theme;
    r->bg_off = (float)((t->theme.bg_start * (PANO_W / 1024)) % PANO_W);
    memset(r->lut_key, 0xFF, sizeof r->lut_key);
    r->sky_hor = -1;
    const tb_theme_t *th = &r->th;
    const tb_sprite_t *bd = tb_art_backdrop();
    for (int j = 0; j < PANO_H; j++) {
        uint32_t c = sky_d(th, PANO_H - j);
        int cr = (int)(c >> 16) & 255, cg = (int)(c >> 8) & 255, cb = (int)c & 255;
        uint16_t *row = r->pano + (size_t)j * PANO_W;
        for (int x = 0; x < PANO_W; x++) row[x] = tb_dither(cr, cg, cb, x, j);
        if (bd && bd->px) {
            int by = j - (PANO_H - bd->h);
            if (by >= 0 && by < bd->h) {
                for (int x = 0; x < PANO_W; x++) {
                    int bx = x % bd->w;
                    size_t i = (size_t)by * bd->w + bx;
                    int a = bd->a ? bd->a[i] : 255;
                    if (a) row[x] = tb_blend(row[x], bd->px[i], a);
                }
            }
        }
        if ((j & 31) == 31) tb_yield();
    }
    if (th->stars) {
        for (int i = 0; i < NSTARS; i++) {
            int d = r->star_d[i];
            if (d > PANO_H) continue;
            int v = PANO_H - d;
            uint16_t *p = r->pano + (size_t)v * PANO_W + r->star_u[i];
            int rr, gg, bb;
            tb_unpack(*p, &rr, &gg, &bb);
            if (rr + gg + bb < 200) {
                int b = r->star_b[i];
                *p = tb_rgb(b, b, b > 200 ? 255 : b + 20);
            }
        }
    }
}

/* the rows over the panorama, for the horizon of this view */
static void sky_rows(tb_render_t *r)
{
    if (r->sky_hor == TB_HOR) return;
    r->sky_hor = TB_HOR;
    for (int y = 0; y < TB_HMAX; y++) {
        int d = TB_HOR - y;
        uint32_t c = sky_d(&r->th, d < 0 ? 0 : d);
        int cr = (int)(c >> 16) & 255, cg = (int)(c >> 8) & 255, cb = (int)c & 255;
        for (int x = 0; x < 4; x++) r->skyrow[y][x] = tb_dither(cr, cg, cb, x, y);
    }
}

/* --------------------------------------------------------------------------
 * The road, row by row
 * -------------------------------------------------------------------------- */

/* [x0, x1), already clipped by the caller */
static inline void fill16(uint16_t *p, int x0, int x1, uint16_t c)
{
    if (x0 >= x1) return;
    uint16_t *d = p + x0;
    int n = x1 - x0;
    if (((uintptr_t)d & 2) && n) {
        *d++ = c;
        n--;
    }
    uint32_t c2 = (uint32_t)c | ((uint32_t)c << 16);
    uint32_t *d32 = (uint32_t *)d;
    for (int i = 0; i < (n >> 1); i++) d32[i] = c2;
    if (n & 1) d[n - 1] = c;
}

static uint16_t fogc(uint32_t c, uint32_t fog, int fk)
{
    return tb_hex(fk > 0 ? tb_mix(c, fog, fk) : c);
}

/* inside a tunnel the lamps light what is near: darker with depth, one
 * rule for the walls beside the road, the ceiling and the exit's frame */
static inline int tunnel_dk(float z)
{
    int dk = 256 - (int)(z * (800.0f / ((float)TB_DRAW * TB_SEG_LEN)));
    return dk < 60 ? 60 : (dk > 256 ? 256 : dk);
}

static void make_pal(const tb_render_t *r, rowpal_t *p, const tb_seg_t *s, int fk, float z)
{
    const tb_theme_t *th = &r->th;
    p->tunnel = (s->flags & SF_TUNNEL) != 0;
    int band = (s->flags & SF_DARK) ? 1 : 0;
    static const int dl[4] = { -9, -3, 3, 9 };
    uint32_t rc = th->road[band];
    for (int i = 0; i < 4; i++) {
        int d = dl[i];
        int cr = (int)(rc >> 16) + d, cg = (int)((rc >> 8) & 255) + d, cb = (int)(rc & 255) + d;
        uint32_t c = ((uint32_t)(cr < 0 ? 0 : cr) << 16) | ((uint32_t)(cg < 0 ? 0 : cg) << 8) | (uint32_t)(cb < 0 ? 0 : cb);
        p->road[i] = fogc(c, th->fog, fk);
    }
    for (int side = 0; side < 2; side++) {
        int kind = side ? s->gr : s->gl;
        uint16_t *dst = side ? p->gr : p->gl;
        uint32_t gc = th->ground[kind][band];
        for (int i = 0; i < 4; i++) {
            int d = kind == GR_SEA ? (i == 3 ? 40 : (i - 1) * 3) : dl[i] / 2;
            int cr = (int)(gc >> 16) + d, cg = (int)((gc >> 8) & 255) + d, cb = (int)(gc & 255) + d;
            if (cr < 0) cr = 0;
            if (cg < 0) cg = 0;
            if (cb < 0) cb = 0;
            if (cr > 255) cr = 255;
            if (cg > 255) cg = 255;
            if (cb > 255) cb = 255;
            dst[i] = fogc(((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb, th->fog, fk);
        }
        if (side) p->void_r = kind == GR_VOID;
        else p->void_l = kind == GR_VOID;
    }
    p->rumble = fogc(th->rumble[band], th->neon ? 0x000000 : th->fog, th->neon ? fk / 3 : fk);
    p->line = fogc(th->line, th->fog, th->neon ? fk / 3 : fk);
    p->yellow = fogc(0xF2C230, th->fog, fk);
    if (p->tunnel) {
        /* inside: lamp-lit, darker with depth; the ground beside the road
         * is the walkway, the wall is painted over it past 8 m */
        int dk = tunnel_dk(z);
        uint16_t walk = tb_scale(tb_hex(tb_mix(th->tunnel_wall, 0x000000, 90)), dk);
        for (int i = 0; i < 4; i++) {
            p->road[i] = tb_scale(tb_hex(tb_mix(th->road[band], 0x201408, 80)), dk - (i - 2) * 6);
            p->gl[i] = p->gr[i] = walk;
        }
        p->rumble = tb_scale(tb_hex(th->tunnel_wall), dk);
        p->line = tb_scale(tb_hex(th->line), dk);
        p->wall = tb_scale(tb_hex(th->tunnel_wall), dk);
        p->void_l = p->void_r = false;
    }
}

static void pal_scaled(rowpal_t *o, const rowpal_t *p, int k)
{
    for (int i = 0; i < 4; i++) {
        o->road[i] = tb_scale(p->road[i], k);
        o->gl[i] = tb_scale(p->gl[i], k);
        o->gr[i] = tb_scale(p->gr[i], k);
    }
    o->rumble = tb_scale(p->rumble, k);
    o->line = tb_scale(p->line, k);
    o->yellow = tb_scale(p->yellow, k);
    o->wall = p->wall;
    o->void_l = p->void_l;
    o->void_r = p->void_r;
    o->tunnel = p->tunnel;
}

/* a painted line at lateral position lx (metres) of width lw, into the row's list */
static inline void add_line(rowinfo_t *ri, float cx, float ppm, float lx, float lw, int col)
{
    if (ri->nl >= 4) return;
    float a = cx + (lx - lw * 0.5f) * ppm, b = cx + (lx + lw * 0.5f) * ppm;
    float wpx = b - a;
    if (wpx < 0.4f) return;
    int x0 = tb_ifloor(a + 0.5f), x1 = tb_ifloor(b + 0.5f);
    if (x1 <= x0) x1 = x0 + 1;
    int k = ri->nl++;
    ri->lx0[k] = (int16_t)(x0 < -30000 ? -30000 : (x0 > 30000 ? 30000 : x0));
    ri->lx1[k] = (int16_t)(x1 < -30000 ? -30000 : (x1 > 30000 ? 30000 : x1));
    ri->la[k] = (uint8_t)(wpx < 1.0f ? (int)(wpx * 255.0f) : 255);
    ri->lc[k] = (uint8_t)col;
}

static inline int16_t clamp16(int v)
{
    return (int16_t)(v < -30000 ? -30000 : (v > 30000 ? 30000 : v));
}

/* a row's geometry in whole pixels, once per frame */
static void row_prepare(tb_render_t *r, rowinfo_t *ri, float cx, float w, float z, float zw,
                        const tb_seg_t *s, const rowpal_t *p, float hw, float carx)
{
    float ppm = w / hw;                               /* px per metre on this row */
    float rw = TB_RUMBLE_W * ppm;
    ri->xl = clamp16(tb_ifloor(cx - w + 0.5f));
    ri->xr = clamp16(tb_ifloor(cx + w + 0.5f));
    ri->rl = clamp16(tb_ifloor(cx - w - rw + 0.5f));
    ri->rr = clamp16(tb_ifloor(cx + w + rw + 0.5f));
    ri->flags = z < TEX_NEAR ? RI_NEAR : 0;
    /* the texture's density halves with distance, about one texel per
     * pixel or finer, so the grain never turns into blocks nor sparkles */
    int pi = (int)ppm;
    int dens = 2;
    while (dens * 2 <= pi && dens < 64) dens <<= 1;
    ri->v = (int16_t)((int)(zw * (float)dens) & (TEX - 1));
    ri->du = (int32_t)((float)dens * 65536.0f / ppm);
    ri->u0 = (int32_t)((0.0f - cx) * (float)ri->du);
    ri->glow = r->th.neon && ri->rr - ri->rl > 2 ? (int16_t)((int)(rw * 0.6f) + 1) : 0;
    /* lines */
    ri->nl = 0;
    bool dash = ((int)(zw * (1.0f / 3.0f)) & 3) == 0;   /* 3 m of 12 */
    if (s->lanes == 2) {
        add_line(ri, cx, ppm, -0.12f, 0.1f, 1);
        add_line(ri, cx, ppm, 0.12f, 0.1f, 1);
    } else if (dash) {
        add_line(ri, cx, ppm, -TB_LANE_W * 0.5f, 0.15f, 0);
        add_line(ri, cx, ppm, TB_LANE_W * 0.5f, 0.15f, 0);
    }
    add_line(ri, cx, ppm, -hw + 0.3f, 0.15f, 0);
    add_line(ri, cx, ppm, hw - 0.3f, 0.15f, 0);
    /* a tunnel's walls, 8 m either side */
    if (p->tunnel) {
        ri->flags |= RI_TUNNEL;
        float wk = TB_TUNNEL_HW * w / hw;
        ri->wl = clamp16(tb_ifloor(cx - wk + 0.5f));
        ri->wr = clamp16(tb_ifloor(cx + wk + 0.5f));
    }
    /* at night the headlights light a cone of the road ahead: the row is
     * painted in five pieces, dark, half, lit, half, dark, each from its
     * own palette (scaling every pixel after painting it was 13 ms a frame
     * on the watch) */
    if (r->th.night && z <= 70.0f && z >= 1.0f) {
        float k = 1.0f - z / 70.0f;
        int boost = 256 + (int)(k * k * 520.0f);
        float c = cx + carx * ppm;
        float hwm = 1.4f + z * 0.18f;
        int x0 = tb_ifloor(c - hwm * ppm), x1 = tb_ifloor(c + hwm * ppm);
        int e = (int)(0.5f * ppm) + 1;
        if (x1 - x0 < 2 * e + 2) e = (x1 - x0) / 3;
        /* the palettes, shared with the row above while they would match */
        int q = boost >> 3;
        static int s_q = -1, s_n = -1;
        if (r->nlit == 0) s_q = s_n = -1;
        if ((q != s_q || ri->n != s_n) && r->nlit + 2 <= LITPOOL) {
            pal_scaled(&r->litpool[r->nlit], p, 256 + (boost - 256) / 2);
            pal_scaled(&r->litpool[r->nlit + 1], p, boost);
            r->nlit += 2;
            s_q = q;
            s_n = ri->n;
        }
        if (r->nlit >= 2) {
            ri->flags |= RI_LIT;
            ri->pm = (uint16_t)(r->nlit - 2);
            ri->pl = (uint16_t)(r->nlit - 1);
            ri->k[0] = clamp16(x0);
            ri->k[1] = clamp16(x0 + e);
            ri->k[2] = clamp16(x1 - e);
            ri->k[3] = clamp16(x1);
        }
    }
}

/* the texture span [x0, x1), u at x0 */
static inline void span_tex(uint16_t *row, int x0, int x1, const uint16_t pal[4], const uint8_t *trow,
                            int32_t u, int32_t du)
{
    for (int x = x0; x < x1; x++, u += du) row[x] = pal[trow[(u >> 16) & (TEX - 1)]];
}

/* one row of ground, rumble, asphalt and lines, only columns [xa, xb) */
static void road_fill(const tb_render_t *r, uint16_t *row, const rowinfo_t *ri, const rowpal_t *p, int xa, int xb)
{
    if (xa >= xb) return;
#define CL(v) ((v) < xa ? xa : ((v) > xb ? xb : (v)))
    bool near = (ri->flags & RI_NEAR) != 0;
    const uint8_t *tg = r->tex_gnd + ri->v * TEX, *tr = r->tex_road + ri->v * TEX;
    int rl = CL(ri->rl), rr = CL(ri->rr), xl = CL(ri->xl), xr = CL(ri->xr);
    /* ground */
    if (!p->void_l && xa < rl) {
        if (near) span_tex(row, xa, rl, p->gl, tg, ri->u0 + ri->du * xa, ri->du);
        else fill16(row, xa, rl, p->gl[1]);
    }
    if (!p->void_r && rr < xb) {
        if (near) span_tex(row, rr, xb, p->gr, tg, ri->u0 + ri->du * rr, ri->du);
        else fill16(row, rr, xb, p->gr[1]);
    }
    /* rumble strips and asphalt */
    fill16(row, rl, xl, p->rumble);
    fill16(row, xr, rr, p->rumble);
    if (near) span_tex(row, xl, xr, p->road, tr, (int32_t)(xl - ri->xl) * ri->du, ri->du);
    else fill16(row, xl, xr, p->road[1]);
    if (ri->glow) {
        /* a glowing fringe outside the rumble */
        int g = ri->glow;
        for (int x = CL(ri->rl - g); x < rl; x++) row[x] = tb_blend(row[x], p->rumble, 120 * (x - ri->rl + g) / g);
        for (int x = rr; x < CL(ri->rr + g); x++) row[x] = tb_blend(row[x], p->rumble, 120 * (ri->rr + g - x) / g);
    }
    /* lines */
    for (int k = 0; k < ri->nl; k++) {
        uint16_t c = ri->lc[k] ? p->yellow : p->line;
        if (ri->la[k] < 255) {
            int x0 = ri->lx0[k];
            if (x0 >= xa && x0 < xb) row[x0] = tb_blend(row[x0], c, ri->la[k]);
        } else {
            fill16(row, CL(ri->lx0[k]), CL(ri->lx1[k]), c);
        }
    }
#undef CL
}

static void road_row(const tb_render_t *r, uint16_t *row, const rowinfo_t *ri, int xa, int xb)
{
    const rowpal_t *p = &r->pal[ri->n];
    if (ri->flags & RI_LIT) {
        const rowpal_t *pm = &r->litpool[ri->pm], *pl = &r->litpool[ri->pl];
        int k0 = ri->k[0], k1 = ri->k[1], k2 = ri->k[2], k3 = ri->k[3];
#define MN(a, b) ((a) < (b) ? (a) : (b))
#define MX(a, b) ((a) > (b) ? (a) : (b))
        road_fill(r, row, ri, p, xa, MN(xb, k0));
        road_fill(r, row, ri, pm, MX(xa, k0), MN(xb, k1));
        road_fill(r, row, ri, pl, MX(xa, k1), MN(xb, k2));
        road_fill(r, row, ri, pm, MX(xa, k2), MN(xb, k3));
        road_fill(r, row, ri, p, MX(xa, k3), xb);
#undef MN
#undef MX
    } else {
        road_fill(r, row, ri, p, xa, xb);
    }
    if (ri->flags & RI_TUNNEL) {
        int wl = ri->wl < xa ? xa : (ri->wl > xb ? xb : ri->wl);
        int wr = ri->wr < xa ? xa : (ri->wr > xb ? xb : ri->wr);
        fill16(row, xa, wl, p->wall);
        fill16(row, wr, xb, p->wall);
    }
}

/* --------------------------------------------------------------------------
 * Stand-ins while there is no pack
 * -------------------------------------------------------------------------- */

static float prop_height(int kind)
{
    switch (kind) {
    case PR_TOWER_BRICK: case PR_TOWER_GLASS: return 60.0f;
    case PR_BUTTE: return 60.0f;
    case PR_LIGHTHOUSE: case PR_CLIFF: return 16.0f;
    case PR_LAMP: case PR_LAMP_NIGHT: return 10.0f;
    case PR_PALM_TALL: return 14.0f;
    case PR_OVERPASS: case PR_GANTRY: case PR_CHECKPOINT: case PR_FINISH: return 8.0f;
    case PR_RING_GATE: return 16.0f;
    case PR_CONE: case PR_BARRIER: case PR_GUARDRAIL: case PR_SNOWBANK: return 0.9f;
    default: return 7.0f;
    }
}

static uint32_t prop_color(int kind)
{
    switch (kind) {
    case PR_TOWER_BRICK: return 0x9A5A44;
    case PR_TOWER_GLASS: return 0x5A8CB0;
    case PR_BUTTE: case PR_ROCK_RED: return 0xB05A30;
    case PR_LAMP: case PR_BARRIER: case PR_GUARDRAIL: return 0x9098A0;
    case PR_CHECKPOINT: return 0xF0C020;
    case PR_FINISH: return 0xF0F0F0;
    case PR_RING_GATE: case PR_CRYSTAL: case PR_BEACON: return 0x30E0FF;
    case PR_SNOWBANK: case PR_ROCK_SNOW: return 0xE0E8F0;
    default: return 0x2E7A30;
    }
}

static float standin_hw(int kind, bool span)
{
    if (span) return kind == PR_RING_GATE ? 8.0f : 13.0f;
    return kind == PR_TOWER_BRICK || kind == PR_TOWER_GLASS || kind == PR_BUTTE ? 10.0f : 0.8f;
}

static void stand_in_prop(tb_img_t *im, int kind, float x, float y, float ppm, int clip, bool span, uint16_t fog, int fk)
{
    uint16_t c = tb_blend(tb_hex(prop_color(kind)), fog, fk);
    float h = prop_height(kind) * ppm;
    tb_img_t cl = *im;
    if (clip < cl.cy1) cl.cy1 = (int16_t)clip;
    float hw = standin_hw(kind, span) * ppm;
    if (span) {
        tb_rect(&cl, (int)(x - hw), (int)(y - h), (int)(hw * 2), (int)(1.2f * ppm) + 1, c);
        tb_rect(&cl, (int)(x - hw), (int)(y - h), (int)(0.8f * ppm) + 1, (int)h, c);
        tb_rect(&cl, (int)(x + hw - 0.8f * ppm), (int)(y - h), (int)(0.8f * ppm) + 1, (int)h, c);
        return;
    }
    tb_rect(&cl, (int)(x - hw), (int)(y - h), (int)(hw * 2) + 1, (int)h + 1, c);
}

static void stand_in_car(tb_img_t *im, const tb_lut_t *lut, float x, float y, float ppm, int clip, bool truck)
{
    tb_img_t cl = *im;
    if (clip < cl.cy1) cl.cy1 = (int16_t)clip;
    float w = (truck ? 2.5f : 1.9f) * ppm, h = (truck ? 3.3f : 1.3f) * ppm;
    int x0 = (int)(x - w * 0.5f);
    tb_rect(&cl, x0, (int)(y - h), (int)w + 1, (int)h + 1, lut->c[RG_PAINT_A][24]);
    if (!truck) tb_rect(&cl, x0 + (int)(w * 0.15f), (int)(y - h), (int)(w * 0.7f) + 1, (int)(h * 0.35f) + 1, lut->c[RG_GLASS][20]);
    tb_rect(&cl, x0, (int)(y - h * 0.55f), (int)(w * 0.18f) + 1, (int)(h * 0.15f) + 1, lut->c[RG_TAIL][28]);
    tb_rect(&cl, x0 + (int)(w * 0.82f), (int)(y - h * 0.55f), (int)(w * 0.18f) + 1, (int)(h * 0.15f) + 1, lut->c[RG_TAIL][28]);
    tb_rect(&cl, x0, (int)(y - h * 0.12f), (int)w + 1, (int)(h * 0.12f) + 1, lut->c[RG_TYRE][10]);
}

/* --------------------------------------------------------------------------
 * The frame
 * -------------------------------------------------------------------------- */

/* The player's car in colour, once per car, paint and night: seven yaw
 * frames coloured through the LUT into plain sprites with opaque runs, so
 * drawing it is mostly memcpy. The tail lights stay a list of pixels, lit
 * through a LUT when braking. ~1.8 MB of PSRAM at the P4's size. */
static void car_cache_free(tb_render_t *r)
{
    for (int f = 0; f < TB_NEAR_FRAMES; f++) {
        tb_sprite_free(&r->car_spr[f]);
        free(r->tail[f]);
        r->tail[f] = NULL;
        r->ntail[f] = 0;
    }
    r->cc_valid = false;
}

void tb_render_car_drop(tb_render_t *r)
{
    car_cache_free(r);
}

static void car_cache(tb_render_t *r, int car)
{
    bool night = r->th.night;
    if (r->cc_valid && r->cc_car == car && r->cc_night == night &&
        !memcmp(&r->cc_paint, &r->paint_player, sizeof(tb_paint_t))) return;
    car_cache_free(r);
    /* the frames as ids and light, read again if they were dropped */
    tb_art_load_near(car);
    tb_lut_t lut;
    tb_lut_build(&lut, &r->paint_player, 0, 0, false, night);
    for (int f = 0; f < TB_NEAR_FRAMES; f++) {
        const tb_vspr_t *v = tb_art_near(car, f);
        if (!v || !v->px) continue;
        size_t np = (size_t)v->w * v->h;
        tb_sprite_t *sp = &r->car_spr[f];
        memset(sp, 0, sizeof(*sp));
        sp->px = (uint16_t *)tb_malloc(np * 2);
        sp->a = (uint8_t *)tb_malloc(np);
        if (!sp->px || !sp->a) {
            tb_sprite_free(sp);
            continue;
        }
        sp->w = v->w;
        sp->h = v->h;
        sp->ox = v->ox;
        sp->oy = v->oy;
        uint32_t nt = 0;
        for (size_t i = 0; i < np; i++) {
            uint16_t q = v->px[i];
            int a4 = (q >> 8) & 15;
            sp->a[i] = (uint8_t)(a4 * 17);
            sp->px[i] = a4 ? lut.c[q >> 12][(q & 255) >> 3] : 0;
            if (a4 && (q >> 12) == RG_TAIL) nt++;
        }
        r->tail[f] = nt ? (uint32_t *)tb_malloc((size_t)nt * 4) : NULL;
        if (r->tail[f]) {
            uint32_t k = 0;
            for (size_t i = 0; i < np; i++) {
                uint16_t q = v->px[i];
                if (((q >> 8) & 15) && (q >> 12) == RG_TAIL) r->tail[f][k++] = ((uint32_t)i << 8) | (q & 255);
            }
            r->ntail[f] = nt;
        }
        tb_sprite_runs(sp);
        tb_yield();
    }
    r->cc_car = car;
    r->cc_night = night;
    r->cc_paint = r->paint_player;
    r->cc_valid = true;
    tb_lut_build(&r->lut_brake, &r->paint_player, 0, 0, true, night);
    /* coloured: the id + light frames are not needed until the paint changes */
    tb_art_drop_near();
}

/* the road's centre (screen x), its px per metre and the floor's row at a
 * camera depth, from the projected segments */
static void at_depth(const tb_render_t *r, float z, float *cx, float *k, float *fy)
{
    int n = 0;
    while (n < r->nfar && r->dr[n + 1].vis && r->dr[n + 1].z <= z) n++;
    const drawn_t *a = &r->dr[n], *b = &r->dr[n + 1];
    float f = 0;
    if (n < r->nfar && b->vis && b->z > a->z) f = (z - a->z) / (b->z - a->z);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    *cx = a->cx + (b->cx - a->cx) * f;
    *fy = a->sy + (b->sy - a->sy) * f;
    *k = TB_F / (z > NEAR_Z ? z : NEAR_Z);
}

static int clampi(float v, int lo, int hi)
{
    int i = tb_ifloor(v + 0.5f);
    return i < lo ? lo : (i > hi ? hi : i);
}

/* The nearest tunnel that is not behind: where its mouth and its exit fall
 * on the screen. Tunnels are flat inside (tb_track.c), so the ceiling is
 * one height over the camera. */
static void tunnel_prepare(tb_render_t *r, const tb_track_t *t, float camz, float camy)
{
    r->tun.on = false;
    float far = r->dr[r->nfar].z;
    for (int i = 0; i < t->ntunnels; i++) {
        float zt0 = (float)t->tunnel_s[i] * TB_SEG_LEN - camz;
        float zt1 = (float)t->tunnel_e[i] * TB_SEG_LEN - camz;
        if (zt1 <= NEAR_Z || zt0 >= far) continue;
        r->tun.on = true;
        r->tun.zt0 = zt0;
        r->tun.zt1 = zt1;
        r->tun.hc = tb_seg(t, t->tunnel_s[i])->y + TB_TUNNEL_H - camy;
        r->tun.inside = zt0 <= NEAR_Z;
        float cx, k, fy;
        if (r->tun.inside) {
            r->tun.wx0 = 0; r->tun.wx1 = TB_W; r->tun.wy0 = 0; r->tun.wy1 = TB_H;
        } else {
            at_depth(r, zt0, &cx, &k, &fy);
            r->tun.wx0 = clampi(cx - TB_TUNNEL_HW * k, 0, TB_W);
            r->tun.wx1 = clampi(cx + TB_TUNNEL_HW * k, 0, TB_W);
            r->tun.wy0 = clampi((float)TB_HOR - r->tun.hc * k, 0, TB_H);
            r->tun.wy1 = clampi(fy, 0, TB_H);
        }
        r->tun.far = zt1 > far;
        float ze = r->tun.far ? far : zt1;
        at_depth(r, ze, &cx, &k, &fy);
        r->tun.ex0 = clampi(cx - TB_TUNNEL_HW * k, 0, TB_W);
        r->tun.ex1 = clampi(cx + TB_TUNNEL_HW * k, 0, TB_W);
        r->tun.ey0 = clampi((float)TB_HOR - r->tun.hc * k, 0, TB_H);
        r->tun.ey1 = clampi(fy, 0, TB_H);
        return;
    }
}

/* The tunnel's ceiling and the walls above the road, over a band: a row
 * above the exit looks up at the ceiling at depth hc * F / (HOR - y), and
 * the ceiling spans the road's width there, walls either side; the rows of
 * the exit show the outside through it and walls around it. */
static void tunnel_band(const tb_render_t *r, tb_img_t *im)
{
    const tb_theme_t *th = &r->th;
    int y0 = im->cy0, y1 = im->cy1, xa = im->cx0, xb = im->cx1;
    int ya = y0 > r->tun.wy0 ? y0 : r->tun.wy0;
    int yb = y1 < r->tun.ey1 ? y1 : r->tun.ey1;
    if (yb > r->tun.wy1) yb = r->tun.wy1;
    int wx0 = r->tun.wx0 > xa ? r->tun.wx0 : xa, wx1 = r->tun.wx1 < xb ? r->tun.wx1 : xb;
    if (wx0 >= wx1) return;
#define CW(v) ((v) < wx0 ? wx0 : ((v) > wx1 ? wx1 : (v)))
    for (int y = ya; y < yb; y++) {
        uint16_t *row = tb_row(im, y);
        if (y < r->tun.ey0 && y < TB_HOR && r->tun.hc > 0.1f) {
            float zc = r->tun.hc * TB_F / (float)(TB_HOR - y);
            float cx, k, fy;
            at_depth(r, zc, &cx, &k, &fy);
            int dk = tunnel_dk(zc);
            uint16_t wall = tb_scale(tb_hex(th->tunnel_wall), dk);
            uint16_t ceil = tb_scale(tb_hex(th->tunnel_ceiling), dk);
            int c0 = CW(tb_ifloor(cx - TB_TUNNEL_HW * k + 0.5f)), c1 = CW(tb_ifloor(cx + TB_TUNNEL_HW * k + 0.5f));
            fill16(row, wx0, c0, wall);
            fill16(row, c1, wx1, wall);
            fill16(row, c0, c1, ceil);
            /* a lamp every 12 m: a strip down the middle third */
            float zw = r->camz + zc;
            if (zw - (float)tb_ifloor(zw * (1.0f / 12.0f)) * 12.0f < 1.4f) {
                int l0 = CW(tb_ifloor(cx - TB_TUNNEL_HW * k * 0.3f + 0.5f));
                int l1 = CW(tb_ifloor(cx + TB_TUNNEL_HW * k * 0.3f + 0.5f));
                fill16(row, l0, l1, tb_scale(tb_hex(th->tunnel_lamp), dk + 60));
            }
        } else if (y >= r->tun.ey0) {
            int dk = tunnel_dk(r->tun.zt1);
            uint16_t wall = tb_scale(tb_hex(th->tunnel_wall), dk);
            fill16(row, wx0, CW(r->tun.ex0), wall);
            fill16(row, CW(r->tun.ex1), wx1, wall);
            if (r->tun.far) fill16(row, CW(r->tun.ex0), CW(r->tun.ex1), tb_scale(tb_hex(th->tunnel_ceiling), 60));
        }
    }
#undef CW
}

/* the columns a vehicle's sprite can touch */
static void vmip_box(const tb_vmip_t *m, float x, float scale, int *x0, int *x1)
{
    int l = 0;
    float ls = scale;
    while (l + 1 < m->n && ls < 0.5f) {
        ls *= 2.0f;
        l++;
    }
    const tb_vspr_t *s = &m->lv[l];
    float x0f = x - (float)s->ox * ls;
    *x0 = tb_ifloor(x0f);
    *x1 = tb_ifloor(x0f + s->w * ls) + 1;
}

static inline int16_t box16(int v)
{
    return (int16_t)(v < -32000 ? -32000 : (v > 32000 ? 32000 : v));
}

/* the frame's geometry, once: where every segment lands, which rows each
 * paints and how, the colours of the cars; the bands then only fill pixels */
void tb_render_prepare(tb_render_t *r, const tb_game_t *g, float dt)
{
    const tb_track_t *t = r->trk;
    const tb_theme_t *th = &r->th;
    if (!t) return;
    sky_rows(r);
    float camz = g->z - TB_CAM_BACK;
    /* 2 m over the road under the car, but never below the road under the
     * camera itself: in a dip that road is higher, and the nearest rows
     * then folded up over the farther ones */
    float ry_car = tb_track_y(t, g->z + 2.2f), ry_cam = tb_track_y(t, camz);
    float camy = (ry_car > ry_cam ? ry_car : ry_cam) + TB_CAM_H;
    float camx = g->cam_x;
    int base = tb_ifloor(camz / TB_SEG_LEN);
    float frac = camz / TB_SEG_LEN - (float)base;
    const tb_seg_t *s0 = tb_seg(t, base);
    r->camz = camz;
    r->camx = camx;
    r->camy = camy;
    r->base = base;
    r->void_below = th->stars && s0->gl == GR_VOID;

    /* the backdrop turns with the road */
    const tb_seg_t *sp = tb_seg(t, (int)(g->z / TB_SEG_LEN));
    r->bg_off += sp->curve * g->v * dt * BG_SCROLL;
    while (r->bg_off < 0) r->bg_off += PANO_W;
    while (r->bg_off >= PANO_W) r->bg_off -= PANO_W;
    r->bgo = (int)r->bg_off;

    for (int y = 0; y < TB_H; y++) r->rowseg[y] = -1;
    float x = 0, dx = -(s0->curve * frac);
    int maxy = TB_H;
    int nfar = 0;
    float fs = (float)th->fog_start * TB_SEG_LEN;
    for (int n = 0; n < TB_DRAW; n++) {
        const tb_seg_t *s = tb_seg(t, base + n);
        const tb_seg_t *sn = tb_seg(t, base + n + 1);
        float z1 = (float)(base + n) * TB_SEG_LEN - camz;
        float z2 = z1 + TB_SEG_LEN;
        float x1 = x, x2 = x + dx;
        float y1 = s->y, y2 = sn->y;
        x += dx;
        dx += s->curve;
        drawn_t *d = &r->dr[n];
        d->vis = false;
        d->clip = (int16_t)maxy;
        d->ya = d->ybot = 0;
        if (z2 <= NEAR_Z) continue;
        if (z1 < NEAR_Z) {
            float f = (NEAR_Z - z1) / TB_SEG_LEN;
            x1 += (x2 - x1) * f;
            y1 += (y2 - y1) * f;
            z1 = NEAR_Z;
        }
        float k1 = TB_F / z1, k2 = TB_F / z2;
        float sx1 = (float)TB_CX + (x1 - camx) * k1, sx2 = (float)TB_CX + (x2 - camx) * k2;
        float sy1 = (float)TB_HOR - (y1 - camy) * k1, sy2 = (float)TB_HOR - (y2 - camy) * k2;
        float hw = tb_road_hw(s);
        d->z = z1;
        d->scale = k1;
        d->cx = sx1;
        d->sy = sy1;
        d->sx2 = sx2;
        d->sy2 = sy2;
        d->w1 = hw * k1;
        d->w2 = hw * k2;
        d->iz1 = 1.0f / z1;
        d->iz2 = 1.0f / z2;
        d->hw = hw;
        d->vis = true;
        nfar = n;
        float zm = (z1 + z2) * 0.5f;
        int fk = zm <= fs ? 0 : (int)((zm - fs) * 256.0f / ((float)TB_DRAW * TB_SEG_LEN - fs));
        d->fk = (int16_t)(fk > 256 ? 256 : fk);
        int ytop = tb_ifloor(sy2 + 0.5f), ybot = tb_ifloor(sy1 + 0.5f);
        if (ytop >= maxy || ybot <= ytop) continue;
        if (ybot > maxy) ybot = maxy;
        int ya = ytop < 0 ? 0 : ytop;
        d->ya = (int16_t)ya;
        d->ybot = (int16_t)ybot;
        make_pal(r, &r->pal[n], s, d->fk, zm);
        for (int yy = ya; yy < ybot; yy++) r->rowseg[yy] = (int16_t)n;
        maxy = ya;
        if (maxy <= 0) break;
    }
    r->maxy = maxy;
    r->nfar = nfar;

    /* every road row in whole pixels */
    r->nlit = 0;
    for (int yy = maxy; yy < TB_H; yy++) {
        rowinfo_t *ri = &r->ri[yy];
        int n = r->rowseg[yy];
        ri->n = (int16_t)n;
        if (n < 0) continue;
        const drawn_t *d = &r->dr[n];
        const tb_seg_t *s = tb_seg(t, base + n);
        float span = d->sy - d->sy2;
        if (span < 0.001f) span = 0.001f;
        float tt = (d->sy - ((float)yy + 0.5f)) / span;
        if (tt < 0) tt = 0;
        if (tt > 1) tt = 1;
        float cx = d->cx + (d->sx2 - d->cx) * tt;
        float w = d->w1 + (d->w2 - d->w1) * tt;
        float z = 1.0f / (d->iz1 + (d->iz2 - d->iz1) * tt);
        row_prepare(r, ri, cx, w, z, camz + z, s, &r->pal[n], d->hw, g->x);
    }

    /* the cars in each segment, and their colours: a LUT is rebuilt only
     * when its fog step, the brake or the night changes */
    for (int n = 0; n <= nfar; n++) r->car_head[n] = -1;
    for (int i = 0; i < g->ntraffic; i++) {
        int n = tb_ifloor(g->traffic[i].z / TB_SEG_LEN) - base;
        if (n < 0 || n >= nfar) continue;
        r->car_next[i] = r->car_head[n];
        r->car_head[n] = (int16_t)i;
        const tb_traffic_t *c = &g->traffic[i];
        int fk = r->dr[n].fk > 230 ? 230 : r->dr[n].fk;
        if (th->neon) fk /= 2;
        fk &= ~7;
        uint32_t key = (uint32_t)c->paint | ((uint32_t)fk << 8) | ((uint32_t)c->braking << 17) |
                       ((uint32_t)th->night << 18) | ((uint32_t)c->ghost << 19);
        if (r->lut_key[i] != key) {
            tb_paint_t pt;
            tb_paint_traffic(c->paint, &pt);
            if (c->ghost) {
                /* a ghost: pale and cold all over, drawn see-through */
                for (int q = 1; q < RG_N; q++) pt.c[q] = 0xB8E6F4;
                pt.c[RG_GLASS] = 0x5C8898;
                pt.c[RG_TYRE] = 0x6C8C98;
                pt.c[RG_TAIL] = 0x9CFFD8;
            }
            tb_lut_build(&r->car_lut[i], &pt, th->fog, fk, c->braking, th->night);
            r->lut_key[i] = key;
        }
    }
    r->rival_n = -1;
    /* the other board's car only when it is ahead of ours: one right behind
     * us drew itself huge over the bottom of the screen */
    if (g->rival_on && g->rival_z > g->z + 1.0f) {
        int n = tb_ifloor(g->rival_z / TB_SEG_LEN) - base;
        if (n >= 0 && n < nfar) {
            r->rival_n = n;
            tb_lut_build(&r->lut_rival, &r->paint_rival, th->fog, r->dr[n].fk > 230 ? 230 : r->dr[n].fk, false, th->night);
        }
    }

    /* the draw list, back to front, each with the rows it can touch */
    r->ndl = 0;
    const float cull = (float)TB_W * 0.8f + 400.0f;
    for (int n = nfar - 1; n >= 0 && r->ndl < DL_MAX - 24; n--) {
        const drawn_t *d = &r->dr[n];
        const drawn_t *dn = &r->dr[n + 1];
        if (!d->vis || d->clip <= 0) continue;
        const tb_seg_t *s = tb_seg(t, base + n);
        int fk = d->fk > 230 ? 230 : d->fk;
        if (th->neon) fk /= 2;
        /* nothing on a segment reaches much below its road row: shadows and
         * the wheels under a vehicle's anchor, a metre and a half at most */
        int below = (int)(d->sy + d->scale * 1.5f + 4.0f);
        for (int p = 0; p < s->nprop && r->ndl < DL_MAX - 4; p++) {
            const tb_prop_t *pr = &t->prop[s->prop0 + p];
            float px = d->cx + (float)pr->x10 * 0.1f * d->scale;
            if (px < -cull || px > (float)TB_W + cull) continue;
            const tb_mip_t *m = tb_art_prop(pr->kind);
            dl_t *e = &r->dl[r->ndl];
            e->x = px;
            e->y = d->sy;
            e->clip = d->clip;
            e->fk = (int16_t)fk;
            e->kind = pr->kind;
            e->mirror = (pr->flags & PF_MIRROR) != 0;
            e->span = (pr->flags & PF_SPAN) != 0;
            e->z = d->z;
            if (!m || !m->n) {
                e->type = DL_STANDIN_PROP;
                e->sc = d->scale;
                e->ytop = (int16_t)(d->sy - 70.0f * d->scale - 2.0f);
                e->ybot = (int16_t)below;
                float hw = standin_hw(pr->kind, e->span) * d->scale + 2.0f;
                e->bx0 = box16((int)(px - hw) - 1);
                e->bx1 = box16((int)(px + hw) + 2);
                r->ndl++;
                continue;
            }
            float sc = d->scale / m->ppm;
            float hpx = (float)m->lv[0].h * sc;
            /* under 4 px tall it is a speck: its draw costs more than it shows */
            if (hpx < 4.0f) continue;
            float top = d->sy - (float)m->lv[0].oy * sc;
            if (top >= (float)d->clip) continue;
            e->type = DL_PROP;
            e->z = d->z;
            e->m = m;
            e->sc = sc;
            e->sh = fk < 200 ? tb_art_prop_shadow(pr->kind) : NULL;
            e->shsc = sc;
            e->ytop = (int16_t)(top < -32000 ? -32000 : top - 1.0f);
            e->ybot = (int16_t)below;
            int bx0, bx1;
            tb_mip_box(m, px, d->sy, sc, e->mirror, &bx0, &bx1);
            if (e->sh && e->sh->n) {
                int sx0, sx1;
                tb_mip_box(e->sh, px, d->sy, sc, false, &sx0, &sx1);
                if (sx0 < bx0) bx0 = sx0;
                if (sx1 > bx1) bx1 = sx1;
            }
            e->bx0 = box16(bx0);
            e->bx1 = box16(bx1);
            if (e->bx1 <= 0 || e->bx0 >= TB_W) continue;
            r->ndl++;
        }
        for (int i = r->car_head[n]; i >= 0 && r->ndl < DL_MAX; i = r->car_next[i]) {
            const tb_traffic_t *c = &g->traffic[i];
            float f = c->z / TB_SEG_LEN - (float)(base + n);
            float cx = d->cx + (dn->cx - d->cx) * f;
            float sy = d->sy + (dn->sy - d->sy) * f;
            float sc = d->scale + (dn->scale - d->scale) * f;
            float sx = cx + c->x * sc;
            if (sx < -400 || sx > TB_W + 400) continue;
            dl_t *e = &r->dl[r->ndl++];
            e->x = sx;
            e->y = sy;
            e->z = c->z - camz;
            e->ppm = sc;
            e->clip = d->clip;
            e->lut = (int16_t)i;
            e->opa = c->ghost ? 120 : 255;
            e->kind = c->model;
            e->ytop = (int16_t)(sy - 4.0f * sc - 2.0f);
            e->ybot = (int16_t)(sy + 1.5f * sc + 4.0f);
            e->type = DL_VEH;
        }
        if (n == r->rival_n && r->ndl < DL_MAX) {
            float f = g->rival_z / TB_SEG_LEN - (float)(base + n);
            float cx = d->cx + (dn->cx - d->cx) * f;
            float sy = d->sy + (dn->sy - d->sy) * f;
            float sc = d->scale + (dn->scale - d->scale) * f;
            dl_t *e = &r->dl[r->ndl++];
            e->x = cx + g->rival_x * sc;
            e->y = sy;
            e->z = g->rival_z - camz;
            e->ppm = sc;
            e->clip = d->clip;
            e->lut = -1;
            e->opa = 150;
            e->kind = (uint8_t)g->rival_car;
            e->ytop = (int16_t)(sy - 4.0f * sc - 2.0f);
            e->ybot = (int16_t)(sy + 1.5f * sc + 4.0f);
            e->type = DL_VEH;
        }
    }
    /* a tunnel in view: its mouth and its exit, and what is seen through them */
    tunnel_prepare(r, t, camz, camy);
    for (int k = 0; k < r->ndl; k++) {
        dl_t *e = &r->dl[k];
        e->cx0 = 0;
        e->cx1 = (int16_t)TB_W;
        e->cy0 = 0;
        if (!r->tun.on || (e->type == DL_PROP && e->kind == PR_PORTAL)) continue;
        if (e->z > r->tun.zt1) {
            /* beyond the exit: only through it (and through the mouth) */
            e->cx0 = (int16_t)(r->tun.ex0 > r->tun.wx0 ? r->tun.ex0 : r->tun.wx0);
            e->cx1 = (int16_t)(r->tun.ex1 < r->tun.wx1 ? r->tun.ex1 : r->tun.wx1);
            e->cy0 = (int16_t)(r->tun.ey0 > r->tun.wy0 ? r->tun.ey0 : r->tun.wy0);
        } else if (e->z >= r->tun.zt0 - 1.0f) {
            e->cx0 = (int16_t)r->tun.wx0;
            e->cx1 = (int16_t)r->tun.wx1;
            e->cy0 = (int16_t)r->tun.wy0;
        }
    }

    /* the vehicles' view, sprite and shadow, and the columns they touch */
    for (int k = 0; k < r->ndl; k++) {
        dl_t *e = &r->dl[k];
        if (e->type != DL_VEH) continue;
        float ang = (e->x - (float)TB_CX) / TB_F;
        int view = ang < -0.12f ? 0 : (ang > 0.12f ? 2 : 1);
        const tb_vmip_t *vm = tb_art_far(e->kind, view);
        if (!vm || !vm->n) {
            e->type = DL_STANDIN_CAR;
            float hw = 1.4f * e->ppm + 2.0f;
            e->bx0 = box16((int)(e->x - hw) - 1);
            e->bx1 = box16((int)(e->x + hw) + 2);
            continue;
        }
        e->m = vm;
        e->sc = e->ppm / vm->ppm;
        e->sh = tb_art_far_shadow(e->kind, view);
        /* the shadows are stored at half the car's resolution */
        e->shsc = e->sh && e->sh->n && e->sh->ppm > 0 ? e->ppm / e->sh->ppm : e->sc;
        int bx0, bx1;
        vmip_box(vm, e->x, e->sc, &bx0, &bx1);
        if (e->sh && e->sh->n) {
            int sx0, sx1;
            tb_mip_box(e->sh, e->x, e->y, e->shsc, false, &sx0, &sx1);
            if (sx0 < bx0) bx0 = sx0;
            if (sx1 > bx1) bx1 = sx1;
        }
        e->bx0 = box16(bx0);
        e->bx1 = box16(bx1);
    }

    /* the player's car */
    car_cache(r, g->car);
    r->car_brake = g->in_brake && g->v > 1.0f;
    float yaw = g->yaw;
    if (g->crash > 0) yaw = sinf(g->spin);
    int fr = (int)(yaw * 3.0f + (yaw >= 0 ? 0.5f : -0.5f)) + 3;
    r->car_fr = fr < 0 ? 0 : (fr > 6 ? 6 : fr);
    int bob = 0;
    if (g->bump > 0) bob = (int)(g->bump * 8.0f * sinf((float)tb_clock() * 0.06f));
    else if (g->v > 5.0f) bob = ((int)(g->z * 3.0f) & 7) == 0 ? 2 : 0;
    r->carx = TB_CX + (int)((g->x - g->cam_x) * TB_F / TB_CAM_BACK);
    r->cary = TB_CAR_Y + bob;
    r->car_standin = !(r->cc_valid && r->car_spr[r->car_fr].px);
    if (r->car_standin) tb_lut_build(&r->lut_car, &r->paint_player, 0, 0, false, th->night);
}

/* the stars under a road floating in space */
static void void_band(const tb_render_t *r, tb_img_t *im, int xa, int xb)
{
    int ya = im->cy0 > TB_HOR ? im->cy0 : TB_HOR;
    for (int y = ya; y < im->cy1; y++) memset(tb_row(im, y) + xa, 0, (size_t)(xb - xa) * 2);
    for (int i = 0; i < NSTARS; i++) {
        int y = TB_HOR + (int)r->star_d[i] * (TB_H - TB_HOR) / STAR_D;
        if (y < ya || y >= im->cy1) continue;
        int x = (r->star_u[i] - r->bgo * 2) & (PANO_W - 1);
        if (x >= xa && x < xb) {
            int b = r->star_b[i];
            tb_row(im, y)[x] = tb_rgb(b, b, b);
        }
    }
}

/* the pixels of im's clip rectangle */
void tb_render_band(const tb_render_t *r, tb_img_t *im, const tb_game_t *g)
{
    const tb_track_t *t = r->trk;
    const tb_theme_t *th = &r->th;
    if (!t) return;
    const int y0 = im->cy0, y1 = im->cy1, xa = im->cx0, xb = im->cx1;
    if (xa >= xb || y0 >= y1) return;
    const int bgo = r->bgo;

    /* space: nothing under the road but stars */
    if (r->void_below && y1 > TB_HOR) void_band(r, im, xa, xb);

    /* the sky above what the road covered, far ground below the horizon;
     * over the void the sky goes on behind the road down to the horizon:
     * the road's rows do not paint beside it */
    int sky_end = r->void_below && r->maxy < TB_HOR ? TB_HOR : r->maxy;
    int ys = y1 < sky_end ? y1 : sky_end;
    const int ptop = TB_HOR - PANO_H;           /* the panorama's first row */
    for (int y = y0; y < ys; y++) {
        uint16_t *row = tb_row(im, y);
        if (y < TB_HOR && y >= ptop) {
            const uint16_t *src = r->pano + (size_t)(y - ptop) * PANO_W;
            int u = (bgo + xa) % PANO_W;
            int n = xb - xa;
            int n1 = PANO_W - u < n ? PANO_W - u : n;
            memcpy(row + xa, src + u, (size_t)n1 * 2);
            if (n1 < n) memcpy(row + xa + n1, src, (size_t)(n - n1) * 2);
        } else if (y < TB_HOR) {
            const uint16_t *p = r->skyrow[y];
            for (int x = xa; x < xb; x++) row[x] = p[x & 3];
        } else if (!r->void_below) {
            fill16(row, xa, xb, tb_hex(th->fog));
        }
    }
    /* the stars over the panorama */
    if (th->stars && y0 < ptop) {
        int yl = ys < ptop ? ys : ptop;
        for (int i = 0; i < NSTARS; i++) {
            int y = TB_HOR - r->star_d[i];
            if (y < y0 || y >= yl) continue;
            int x = r->star_u[i] - bgo;
            if (x < 0) x += PANO_W;
            if (x >= xa && x < xb) {
                int b = r->star_b[i];
                tb_row(im, y)[x] = tb_rgb(b, b, b > 200 ? 255 : b + 20);
            }
        }
    }

    /* the road */
    for (int yy = y0 > r->maxy ? y0 : r->maxy; yy < y1; yy++) {
        const rowinfo_t *ri = &r->ri[yy];
        if (ri->n < 0) continue;
        road_row(r, tb_row(im, yy), ri, xa, xb);
    }
    if (r->tun.on) tunnel_band(r, im);

    /* the sprites, back to front */
    uint16_t fog565 = tb_hex(th->fog);
    tb_img_t whole = *im;
    for (int k = 0; k < r->ndl; k++) {
        const dl_t *e = &r->dl[k];
        if (e->ytop >= y1 || e->ybot < y0 || e->clip <= y0 || e->bx1 <= xa || e->bx0 >= xb) continue;
        /* seen through a tunnel's mouth: clipped to it */
        *im = whole;
        if (e->cx0 > im->cx0) im->cx0 = e->cx0;
        if (e->cx1 < im->cx1) im->cx1 = e->cx1;
        if (e->cy0 > im->cy0) im->cy0 = e->cy0;
        if (im->cx0 >= im->cx1 || im->cy0 >= im->cy1) continue;
        switch (e->type) {
        case DL_PROP:
            if (e->sh && e->sh->n) tb_mip_shadow(im, e->sh, e->x, e->y, e->shsc, e->clip, 160);
            tb_mip_draw(im, (const tb_mip_t *)e->m, e->x, e->y, e->sc, e->clip, e->mirror, fog565, e->fk, 256);
            break;
        case DL_VEH: {
            const tb_lut_t *lut = e->lut >= 0 ? &r->car_lut[e->lut] : &r->lut_rival;
            if (e->sh && e->sh->n) tb_mip_shadow(im, e->sh, e->x, e->y, e->shsc, e->clip, 200);
            tb_vmip_draw(im, (const tb_vmip_t *)e->m, lut, e->x, e->y, e->sc, e->clip, e->opa);
            break;
        }
        case DL_STANDIN_PROP:
            stand_in_prop(im, e->kind, e->x, e->y, e->sc, e->clip, e->span, fog565, e->fk);
            break;
        case DL_STANDIN_CAR: {
            const tb_lut_t *lut = e->lut >= 0 ? &r->car_lut[e->lut] : &r->lut_rival;
            stand_in_car(im, lut, e->x, e->y, e->ppm, e->clip, e->kind == VH_TRUCK);
            break;
        }
        }
    }
    *im = whole;

    /* the player's car */
    int carx = r->carx, cary = r->cary;
    if (!r->car_standin) {
        const tb_sprite_t *cs = &r->car_spr[r->car_fr];
        const tb_sprite_t *sh = tb_art_near_shadow(g->car, r->car_fr);
        int top = cary - cs->oy, bot = top + cs->h;
        if (sh && sh->a) {
            int st = TB_CAR_Y - sh->oy;
            if (st < top) top = st;
            if (st + sh->h > bot) bot = st + sh->h;
        }
        if (top >= y1 + 4 || bot + 4 < y0 || carx + cs->w < xa || carx - cs->w > xb) goto glow;
        if (sh && sh->a) {
            int sx0 = carx - sh->ox, sy0 = TB_CAR_Y - sh->oy;
            int ca = xa - sx0, cb = xb - sx0;
            if (ca < 0) ca = 0;
            if (cb > sh->w) cb = sh->w;
            for (int yy = 0; yy < sh->h && ca < cb; yy++) {
                int y = sy0 + yy;
                if (y < y0 || y >= y1) continue;
                uint16_t *row = tb_row(im, y) + sx0;
                const uint8_t *a = sh->a + (size_t)yy * sh->w;
                for (int xx = ca; xx < cb; xx++) {
                    if (a[xx] < 6) continue;
                    row[xx] = tb_darken(row[xx], 256 - (a[xx] * 220 >> 8));
                }
            }
        }
        tb_sprite(im, cs, carx, cary);
        if (r->car_brake && r->tail[r->car_fr]) {
            /* the brake lights, over the coloured car */
            const uint32_t *tl = r->tail[r->car_fr];
            int sx0 = carx - cs->ox, sy0 = cary - cs->oy;
            for (uint32_t k = 0; k < r->ntail[r->car_fr]; k++) {
                uint32_t i = tl[k] >> 8;
                int x = sx0 + (int)(i % (uint32_t)cs->w), y = sy0 + (int)(i / (uint32_t)cs->w);
                if (y < y0 || y >= y1 || x < xa || x >= xb) continue;
                uint16_t *p = tb_row(im, y) + x;
                *p = tb_blend(*p, r->lut_brake.c[RG_TAIL][(tl[k] & 255) >> 3], cs->a[i]);
            }
        }
    } else {
        tb_shadow(im, carx, TB_CAR_Y, 200, 28, 150);
        stand_in_car(im, &r->lut_car, (float)carx, (float)cary, TB_F / TB_CAM_BACK, TB_H, false);
    }
glow:
    if (th->night) {
        tb_glow(im, carx - 140, cary - 110, 36, 0xFF2010, g->in_brake ? 200 : 90);
        tb_glow(im, carx + 140, cary - 110, 36, 0xFF2010, g->in_brake ? 200 : 90);
    }
}

void tb_render_world(tb_render_t *r, tb_img_t *im, const tb_game_t *g, float dt)
{
    tb_render_prepare(r, g, dt);
    tb_render_band(r, im, g);
}
