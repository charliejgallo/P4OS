/*
 * P4OS - Office: a PDF's pages as they are printed.
 *
 * The pages in a column, at the screen's width; a drag scrolls (with
 * momentum), two fingers or a double tap zoom up to four times, and the
 * number of the page shows while it moves. Every page is drawn by
 * of_pdfdraw.c in the system's worker (never in LVGL's task): the whole
 * page while it fits in 2.6 million pixels, else the part in sight and
 * half a screen around it. Four pictures are kept; while a zoom is being
 * drawn the one there is shows, scaled. The letters of fonts the file
 * names and does not carry come from the firmware's Inter, taken out of
 * LVGL's fonts here, on LVGL's side, when the screen opens (the worker
 * does not touch LVGL). The menu goes back to the reflowed text, to a page,
 * to the width, and tells what the file is.
 */
#include "of_ui.h"
#include "of_pdf.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTILES     4
#define FULL_MAX   2600000          /* pixels: a page drawn whole up to this */
#define BUDGET     6000000          /* pixels kept in all, with the one being drawn (12 MB) */
#define MARGIN     16
#define ZOOM_MAX   4.0f

typedef struct {
    int       page;
    float     scale;
    int       ox, oy, w, h;         /* the part of the page, in its pixels at scale */
    uint16_t *px;
    lv_image_dsc_t dsc;
    uint32_t  used;
} tile_t;

/* Inter's letters, as coverage, for the fonts a file does not carry */
typedef struct { uint32_t cp; int16_t w, h, l, t, adv; uint8_t *a; } fbg_t;
typedef struct { int px; fbg_t *g; int n; } fbsize_t;

static struct {
    of_pdfr_t *R;
    char      path[300];
    int       np;
    float    *pw, *ph;              /* points, turned as shown */
    float     maxw;
    lv_obj_t *root, *top, *title, *area, *badge;
    int32_t   W, H, ax, ay, aw, ah;
    float     zoom, scale;
    float    *ypos;                 /* each page's top in the column, at scale */
    float     total_w, total_h;
    float     sx, sy, vx, vy;
    lv_timer_t *fling, *poll;
    bool      pinching;
    uint32_t  moved_ms;             /* the page number shows a while after a move */
    tile_t    tiles[NTILES];
    uint32_t  tick;
    /* the job the worker has */
    volatile int  state;            /* 0 none, 1 asked, 2 drawing, 3 done */
    volatile bool cancel;
    struct { int page; float scale; int ox, oy, w, h; } job;
    uint16_t *job_px;
    bool      job_ok, worker;
    bool      failed_note;
    /* the fallback letters */
    fbsize_t  fb[5];
    uint8_t  *fbbuf;
    size_t    fbcap;
} V;

static void relayout(void);

/* -------------------------------------------------------------------------- */
/* The fonts a file names and does not carry                                   */
/* -------------------------------------------------------------------------- */

static const lv_font_t *fb_font(int i)
{
    static const lv_font_t *const F[5] = { &aos_inter_16, &aos_inter_24, &aos_inter_32, &aos_inter_48, &aos_inter_64 };
    return F[i];
}

static const int FB_PX[5] = { 16, 24, 32, 48, 64 };

static const uint16_t FB_EXTRA[] = { 0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2020, 0x2021, 0x2022,
                                     0x2026, 0x2030, 0x2039, 0x203A, 0x20AC, 0x2122, 0x0152, 0x0153, 0x0160, 0x0161, 0x0178,
                                     0x017D, 0x017E, 0x0192, 0x02C6, 0x02DC, 0x2212, 0x2190, 0x2192, 0x2264, 0x2265, 0x221E };

/* on LVGL's side: every letter of every size, copied out of the fonts */
static void fb_load(void)
{
    if (V.fb[0].g) return;
    uint32_t cps[96 + 96 + sizeof FB_EXTRA / sizeof FB_EXTRA[0]];
    int ncp = 0;
    for (uint32_t c = 33; c < 127; c++) cps[ncp++] = c;
    for (uint32_t c = 161; c < 256; c++) cps[ncp++] = c;
    for (size_t i = 0; i < sizeof FB_EXTRA / sizeof FB_EXTRA[0]; i++) cps[ncp++] = FB_EXTRA[i];
    /* LVGL's own buffer: the font writes A8 rows into it, with its stride */
    lv_draw_buf_t *db = lv_draw_buf_create(128, 128, LV_COLOR_FORMAT_A8, 0);
    if (!db) return;
    for (int s = 0; s < 5; s++) {
        const lv_font_t *f = fb_font(s);
        fbsize_t *S = &V.fb[s];
        S->px = FB_PX[s];
        S->g = of_alloc((size_t)ncp * sizeof *S->g);
        if (!S->g) break;
        for (int i = 0; i < ncp; i++) {
            lv_font_glyph_dsc_t d;
            memset(&d, 0, sizeof d);
            if (!lv_font_get_glyph_dsc(f, &d, cps[i], 0) || d.is_placeholder || d.resolved_font != f) continue;
            fbg_t *G = &S->g[S->n];
            G->cp = cps[i];
            G->adv = (int16_t)d.adv_w;
            G->w = (int16_t)d.box_w;
            G->h = (int16_t)d.box_h;
            G->l = d.ofs_x;
            /* LVGL: ofs_y from the baseline to the box's bottom */
            G->t = (int16_t)(d.ofs_y + d.box_h);
            if (d.box_w && d.box_h) {
                if (d.box_w > 128 || d.box_h > 128) continue;
                d.req_raw_bitmap = 0;
                /* LVGL 9 gives the buffer back, the rows in its data */
                if (!lv_font_get_glyph_bitmap(&d, db)) continue;
                const uint8_t *bm = db->data;
                uint32_t stride = lv_draw_buf_width_to_stride(d.box_w, LV_COLOR_FORMAT_A8);
                G->a = of_alloc((size_t)d.box_w * d.box_h);
                if (G->a)
                    for (int y = 0; y < d.box_h; y++) memcpy(G->a + (size_t)y * d.box_w, bm + (size_t)y * stride, d.box_w);
                lv_font_glyph_release_draw_data(&d);
                if (!G->a) continue;
            }
            S->n++;
        }
    }
    lv_draw_buf_destroy(db);
}

static void fb_free(void)
{
    for (int s = 0; s < 5; s++) {
        for (int i = 0; i < V.fb[s].n; i++) of_free(V.fb[s].g[i].a);
        of_free(V.fb[s].g);
    }
    memset(V.fb, 0, sizeof V.fb);
    of_free(V.fbbuf);
    V.fbbuf = NULL;
    V.fbcap = 0;
}

/* in the worker: the nearest size, bolder by a pixel or two when asked */
static bool fb_glyph(uint32_t cp, float px, uint8_t flags, of_fbglyph_t *out)
{
    if (cp == 0xA0) return false;
    int s = 0;
    while (s < 4 && FB_PX[s] < px * 0.9f) s++;
    const fbsize_t *S = &V.fb[s];
    const fbg_t *G = NULL;
    for (int i = 0; i < S->n; i++) if (S->g[i].cp == cp) { G = &S->g[i]; break; }
    if (!G) {
        /* the accents of the private-use symbol fonts and the like: nothing */
        return false;
    }
    if (!G->a) return false;
    int grow = flags & 1 ? (S->px >= 40 ? 2 : 1) : 0;
    int w = G->w + grow;
    size_t need = (size_t)w * (size_t)G->h;
    if (need > V.fbcap) {
        uint8_t *n = of_realloc(V.fbbuf, need);
        if (!n) return false;
        V.fbbuf = n;
        V.fbcap = need;
    }
    for (int y = 0; y < G->h; y++) {
        const uint8_t *r = G->a + (size_t)y * (size_t)G->w;
        uint8_t *o = V.fbbuf + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            int v = 0;
            for (int k = 0; k <= grow; k++) {
                int sx = x - k;
                if (sx >= 0 && sx < G->w && r[sx] > v) v = r[sx];
            }
            o[x] = (uint8_t)v;
        }
    }
    *out = (of_fbglyph_t){ V.fbbuf, w, G->h, G->l, G->t, (float)S->px, (float)(G->adv + grow) };
    return true;
}

/* -------------------------------------------------------------------------- */
/* The worker                                                                  */
/* -------------------------------------------------------------------------- */

static void worker_fn(void *arg)
{
    (void)arg;
    while (!aos_hal_worker_should_stop()) {
        if (V.state != 1) { aos_hal_sleep_ms(12); continue; }
        V.state = 2;
        int w = V.job.w, h = V.job.h;
        uint32_t t0 = aos_hal_uptime_ms();
        uint16_t *px = of_alloc((size_t)w * (size_t)h * 2);
        bool ok = px && !V.cancel && of_pdfr_draw(V.R, V.job.page, V.job.scale, V.job.ox, V.job.oy, px, w, h, &V.cancel);
        /* the measure, for the log: tools/plog.sh */
        aos_hal_log("oficina", "pdf page %d %dx%d at %.2f: %u ms%s", V.job.page + 1, w, h, (double)V.job.scale,
                    (unsigned)(aos_hal_uptime_ms() - t0), !px ? " (no memory)" : V.cancel ? " (stopped)" : ok ? "" : " (failed)");
        V.job_px = px;
        V.job_ok = ok && !V.cancel;
        V.state = 3;
    }
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

static void layout(void)
{
    of_free(V.ypos);
    V.ypos = of_alloc((size_t)(V.np + 1) * sizeof *V.ypos);
    if (!V.ypos) return;
    V.scale = V.zoom * (float)(V.aw - 2 * MARGIN) / V.maxw;
    float y = MARGIN;
    for (int i = 0; i < V.np; i++) {
        V.ypos[i] = y;
        y += V.ph[i] * V.scale + MARGIN;
    }
    V.ypos[V.np] = y;
    V.total_h = y;
    V.total_w = V.maxw * V.scale + 2 * MARGIN;
    if (V.total_w < (float)V.aw) V.total_w = (float)V.aw;
}

static void clamp(void)
{
    float mx = V.total_w - (float)V.aw, my = V.total_h - (float)V.ah;
    if (V.sx > mx) V.sx = mx;
    if (V.sy > my) V.sy = my;
    if (V.sx < 0) V.sx = 0;
    if (V.sy < 0) V.sy = 0;
}

static float page_x(int i) { return (V.total_w - V.pw[i] * V.scale) / 2; }

/* the page in the middle of the screen */
static int page_at_middle(void)
{
    float mid = V.sy + (float)V.ah / 2;
    for (int i = 0; i < V.np; i++) if (mid < V.ypos[i + 1]) return i;
    return V.np - 1;
}

/* -------------------------------------------------------------------------- */
/* Tiles                                                                       */
/* -------------------------------------------------------------------------- */

static void tile_free(tile_t *t)
{
    of_free(t->px);
    memset(t, 0, sizeof *t);
    t->page = -1;
}

/* the part of page i in sight, in its pixels; false if none */
static bool visible_part(int i, int *x0, int *y0, int *x1, int *y1)
{
    float px = page_x(i), py = V.ypos[i];
    float pw = V.pw[i] * V.scale, ph = V.ph[i] * V.scale;
    float a = fmaxf(V.sx, px) - px, b = fminf(V.sx + (float)V.aw, px + pw) - px;
    float c = fmaxf(V.sy, py) - py, d = fminf(V.sy + (float)V.ah, py + ph) - py;
    if (b <= a || d <= c) return false;
    *x0 = (int)floorf(a); *x1 = (int)ceilf(b); *y0 = (int)floorf(c); *y1 = (int)ceilf(d);
    return true;
}

static tile_t *tile_for(int page, float scale, int x0, int y0, int x1, int y1)
{
    for (int k = 0; k < NTILES; k++) {
        tile_t *t = &V.tiles[k];
        if (t->px && t->page == page && fabsf(t->scale - scale) < 1e-4f && t->ox <= x0 && t->oy <= y0 && t->ox + t->w >= x1 && t->oy + t->h >= y1)
            return t;
    }
    return NULL;
}

/* the region to ask for: the whole page, or what is in sight and half a screen around */
static void wanted_region(int i, int *ox, int *oy, int *w, int *h)
{
    int pw = (int)ceilf(V.pw[i] * V.scale), ph = (int)ceilf(V.ph[i] * V.scale);
    if ((int64_t)pw * ph <= FULL_MAX) { *ox = 0; *oy = 0; *w = pw; *h = ph; return; }
    int x0, y0, x1, y1;
    if (!visible_part(i, &x0, &y0, &x1, &y1)) { x0 = 0; y0 = 0; x1 = V.aw; y1 = V.ah; }
    x0 -= V.aw / 2; x1 += V.aw / 2; y0 -= V.ah / 2; y1 += V.ah / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > pw) x1 = pw;
    if (y1 > ph) y1 = ph;
    *ox = x0; *oy = y0; *w = x1 - x0; *h = y1 - y0;
}

static void ask(int page)
{
    int ox, oy, w, h;
    wanted_region(page, &ox, &oy, &w, &h);
    if (w <= 0 || h <= 0) return;
    /* room for it: the pictures used longest ago go, those out of sight first */
    for (;;) {
        int64_t held = 0;
        for (int k = 0; k < NTILES; k++) if (V.tiles[k].px) held += (int64_t)V.tiles[k].w * V.tiles[k].h;
        if (held + (int64_t)w * h <= BUDGET) break;
        int victim = -1;
        for (int pass = 0; pass < 2 && victim < 0; pass++)
            for (int k = 0; k < NTILES; k++) {
                tile_t *t = &V.tiles[k];
                int a, b, c, d;
                if (!t->px || (pass == 0 && visible_part(t->page, &a, &b, &c, &d))) continue;
                if (victim < 0 || t->used < V.tiles[victim].used) victim = k;
            }
        if (victim < 0) break;
        tile_free(&V.tiles[victim]);
        lv_obj_invalidate(V.area);
    }
    V.job.page = page;
    V.job.scale = V.scale;
    V.job.ox = ox; V.job.oy = oy; V.job.w = w; V.job.h = h;
    V.cancel = false;
    V.state = 1;
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!V.R) return;
    if (V.state == 3) {
        if (V.job_ok && V.job_px) {
            /* the oldest tile goes */
            int k = 0;
            for (int i = 1; i < NTILES; i++) {
                if (!V.tiles[i].px) { k = i; break; }
                if (V.tiles[k].px && V.tiles[i].used < V.tiles[k].used) k = i;
            }
            if (!V.tiles[0].px) k = 0;
            tile_free(&V.tiles[k]);
            tile_t *T = &V.tiles[k];
            T->page = V.job.page; T->scale = V.job.scale;
            T->ox = V.job.ox; T->oy = V.job.oy; T->w = V.job.w; T->h = V.job.h;
            T->px = V.job_px;
            T->used = ++V.tick;
            memset(&T->dsc, 0, sizeof T->dsc);
            T->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            T->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            T->dsc.header.w = (uint32_t)T->w;
            T->dsc.header.h = (uint32_t)T->h;
            T->dsc.header.stride = (uint32_t)T->w * 2;
            T->dsc.data = (const uint8_t *)T->px;
            T->dsc.data_size = (uint32_t)(T->w * T->h * 2);
            lv_obj_invalidate(V.area);
        } else {
            of_free(V.job_px);
            if (!V.job_px && !V.failed_note) { V.failed_note = true; of_app_toast(_("Falta memoria para dibujar la página")); }
        }
        V.job_px = NULL;
        V.state = 0;
    }
    if (V.state == 2) {
        /* drawing something no longer needed: stopped */
        int x0, y0, x1, y1;
        bool seen = V.job.page >= 0 && V.job.page < V.np && visible_part(V.job.page, &x0, &y0, &x1, &y1);
        bool near = abs(V.job.page - page_at_middle()) <= 1;
        if (fabsf(V.job.scale - V.scale) > 1e-4f || (!seen && !near)) V.cancel = true;
        return;
    }
    if (V.state != 0 || V.pinching) {
        /* the page number fades with time */
        if (V.moved_ms && aos_hal_uptime_ms() - V.moved_ms > 1500) { V.moved_ms = 0; lv_obj_invalidate(V.area); }
        return;
    }
    /* what to draw next: the pages in sight, then the ones beside */
    int mid = page_at_middle();
    int order[8], n = 0;
    for (int i = 0; i < V.np && n < 6; i++) {
        int x0, y0, x1, y1;
        if (visible_part(i, &x0, &y0, &x1, &y1)) {
            tile_t *T = tile_for(i, V.scale, x0, y0, x1, y1);
            if (T) T->used = ++V.tick;
            else order[n++] = i;
        }
    }
    if (n) {
        /* the one in the middle first */
        int best = 0;
        for (int k = 1; k < n; k++) if (abs(order[k] - mid) < abs(order[best] - mid)) best = k;
        ask(order[best]);
        return;
    }
    for (int d = 1; d <= 1; d++)
        for (int s = -1; s <= 1; s += 2) {
            int i = mid + s * d;
            if (i < 0 || i >= V.np) continue;
            int pw = (int)ceilf(V.pw[i] * V.scale), ph = (int)ceilf(V.ph[i] * V.scale);
            if ((int64_t)pw * ph > FULL_MAX) continue;
            if (!tile_for(i, V.scale, 0, 0, pw, ph)) { ask(i); return; }
        }
    if (V.moved_ms && aos_hal_uptime_ms() - V.moved_ms > 1500) { V.moved_ms = 0; lv_obj_invalidate(V.area); }
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

static void area_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(V.area, &a);
    V.ax = a.x1;
    V.ay = a.y1;
    lv_area_t keep = layer->_clip_area, cl;
    if (!ofu_clip(&cl, &keep, &a)) return;
    layer->_clip_area = cl;
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = OFT.dark ? lv_color_hex(0x202020) : lv_color_hex(0xC8C8C8);
    lv_draw_rect(layer, &bg, &cl);
    for (int i = 0; V.ypos && i < V.np; i++) {
        float py = V.ypos[i] - V.sy, px = page_x(i) - V.sx;
        float pw = V.pw[i] * V.scale, ph = V.ph[i] * V.scale;
        if (py > (float)V.ah || py + ph < 0 || px > (float)V.aw || px + pw < 0) continue;
        int32_t X = V.ax + (int32_t)floorf(px), Y = V.ay + (int32_t)floorf(py);
        lv_area_t pa = { X, Y, X + (int32_t)ceilf(pw) - 1, Y + (int32_t)ceilf(ph) - 1 };
        lv_draw_rect_dsc_t pd;
        lv_draw_rect_dsc_init(&pd);
        pd.bg_color = lv_color_white();
        pd.shadow_width = 10;
        pd.shadow_opa = LV_OPA_30;
        pd.shadow_color = lv_color_black();
        lv_draw_rect(layer, &pd, &pa);
        /* the tile at this scale, else any of this page, scaled */
        tile_t *best = NULL;
        for (int k = 0; k < NTILES; k++) {
            tile_t *t = &V.tiles[k];
            if (!t->px || t->page != i) continue;
            bool exact = fabsf(t->scale - V.scale) < 1e-4f;
            if (!best || (exact && fabsf(best->scale - V.scale) >= 1e-4f) ||
                (!exact && fabsf(best->scale - V.scale) >= 1e-4f && fabsf(t->scale - V.scale) < fabsf(best->scale - V.scale)))
                best = t;
        }
        for (int pass = 0; pass < 2; pass++) {
            for (int k = 0; k < NTILES; k++) {
                tile_t *t = &V.tiles[k];
                if (!t->px || t->page != i) continue;
                bool exact = fabsf(t->scale - V.scale) < 1e-4f;
                /* first the others, scaled, then the exact ones over them */
                if ((pass == 0) == exact) continue;
                if (pass == 0 && t != best) continue;
                float k2 = V.scale / t->scale;
                lv_draw_image_dsc_t d;
                lv_draw_image_dsc_init(&d);
                d.src = &t->dsc;
                int32_t tx = X + (int32_t)floorf((float)t->ox * k2), ty = Y + (int32_t)floorf((float)t->oy * k2);
                if (!exact) {
                    d.scale_x = d.scale_y = (int32_t)(k2 * 256 + 0.5f);
                    d.pivot.x = 0;
                    d.pivot.y = 0;
                }
                lv_area_t ia = { tx, ty, tx + t->w - 1, ty + t->h - 1 };
                lv_draw_image(layer, &d, &ia);
            }
        }
    }
    /* the page's number, a while after a move */
    if (V.moved_ms && V.np > 0) {
        char n[32];
        snprintf(n, sizeof n, "%d / %d", page_at_middle() + 1, V.np);
        const lv_font_t *f = ofu_font(OF_ST_BOLD, OF_SZ_S);
        int32_t tw = 0;
        for (const char *c = n; *c; c++) tw += lv_font_get_glyph_width(f, (uint8_t)*c, 0);
        lv_area_t ba = { V.ax + (V.aw - tw) / 2 - 18, V.ay + V.ah - 70, V.ax + (V.aw + tw) / 2 + 18, V.ay + V.ah - 70 + f->line_height + 16 };
        lv_draw_rect_dsc_t bd;
        lv_draw_rect_dsc_init(&bd);
        bd.bg_color = lv_color_black();
        bd.bg_opa = LV_OPA_60;
        bd.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &bd, &ba);
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.font = f;
        ld.color = lv_color_white();
        ld.text = n;
        ld.text_local = 1;
        lv_area_t la = { ba.x1 + 18, ba.y1 + 8, ba.x2, ba.y2 };
        lv_draw_label(layer, &ld, &la);
    }
    layer->_clip_area = keep;
}

/* -------------------------------------------------------------------------- */
/* Gestures                                                                    */
/* -------------------------------------------------------------------------- */

static void moved(void)
{
    V.moved_ms = aos_hal_uptime_ms();
    if (!V.moved_ms) V.moved_ms = 1;
    lv_obj_invalidate(V.area);
}

static void stop_fling(void)
{
    if (V.fling) { lv_timer_delete(V.fling); V.fling = NULL; }
}

static void fling_cb(lv_timer_t *t)
{
    (void)t;
    float ox = V.sx, oy = V.sy;
    V.sx -= V.vx / 60;
    V.sy -= V.vy / 60;
    V.vx *= 0.93f;
    V.vy *= 0.93f;
    clamp();
    if (ox == V.sx) V.vx = 0;
    if (oy == V.sy) V.vy = 0;
    moved();
    if (V.vx * V.vx + V.vy * V.vy < 400) stop_fling();
}

/* the zoom set, with the point (cx, cy) of the area staying where it is */
static void zoom_to(float z, float cx, float cy)
{
    if (z < 1) z = 1;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    float k = z / V.zoom;
    float bx = V.sx + cx, by = V.sy + cy;
    /* the column's left margin scales too, but stays a margin when narrower than the screen */
    float old_tw = V.total_w;
    V.zoom = z;
    layout();
    float off_old = old_tw > (float)V.aw ? 0 : 0;
    (void)off_old;
    V.sx = bx * k - cx;
    V.sy = MARGIN + (by - MARGIN) * k - cy;
    clamp();
    moved();
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    if (ofu_sheet_open()) return;
    float cx = ev->x - (float)V.ax, cy = ev->y - (float)V.ay;
    switch (ev->type) {
    case AOS_GESTURE_DOUBLE_TAP:
        stop_fling();
        zoom_to(V.zoom > 1.01f ? 1 : 2.0f, cx, cy);
        break;
    case AOS_GESTURE_DRAG_BEGIN: stop_fling(); break;
    case AOS_GESTURE_DRAG:
        V.sx -= ev->dx;
        V.sy -= ev->dy;
        clamp();
        moved();
        break;
    case AOS_GESTURE_DRAG_END:
        if (ev->vx * ev->vx + ev->vy * ev->vy > 90000) {
            V.vx = ev->vx;
            V.vy = ev->vy;
            if (!V.fling) V.fling = lv_timer_create(fling_cb, 16, NULL);
        }
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        stop_fling();
        V.pinching = true;
        break;
    case AOS_GESTURE_PINCH:
        V.sx -= ev->dx;
        V.sy -= ev->dy;
        zoom_to(V.zoom * ev->scale, cx, cy);
        break;
    case AOS_GESTURE_PINCH_END:
        V.pinching = false;
        /* not a zoom between two steps of no use: snapped to tenths */
        zoom_to(roundf(V.zoom * 10) / 10, (float)V.aw / 2, (float)V.ah / 2);
        break;
    default: break;
    }
}

/* -------------------------------------------------------------------------- */
/* Menus                                                                       */
/* -------------------------------------------------------------------------- */

static void goto_page(int n)
{
    if (n < 0 || n >= V.np) return;
    stop_fling();
    V.sy = V.ypos[n] - MARGIN;
    clamp();
    moved();
}

static void goto_done(const char *text, void *ud)
{
    (void)ud;
    int n = atoi(text);
    if (n < 1 || n > V.np) { of_app_toast(_("No hay una página con ese número")); return; }
    goto_page(n - 1);
}

static void info_open(void)
{
    char msg[300];
    int mid = page_at_middle();
    snprintf(msg, sizeof msg, _("%d páginas; esta, de %.1f × %.1f cm. La app dibuja las páginas con las letras que trae el archivo; las que nombra y no trae, con las suyas."),
             V.np, V.pw[mid] / 28.3465, V.ph[mid] / 28.3465);
    ofu_confirm(of_basename(V.path), msg, _("Listo"), false, NULL, NULL);
}

static void menu_pick(int i, void *ud)
{
    (void)ud;
    switch (i) {
    case 0: {
        char p[300];
        snprintf(p, sizeof p, "%s", V.path);
        of_app_pdf_mode(p, false);
        break;
    }
    case 1: ofu_prompt(_("Ir a la página"), "", _("Ir"), goto_done, NULL); break;
    case 2: zoom_to(1, (float)V.aw / 2, 0); break;
    case 3: zoom_to(V.zoom * 1.5f, (float)V.aw / 2, (float)V.ah / 2); break;
    case 4: info_open(); break;
    }
}

static void menu_open(void)
{
    ofu_item_t items[5] = {
        { _("Ver el texto acomodado"), OF_IC_DOC, false, false },
        { _("Ir a la página..."), OF_IC_LIST, false, false },
        { _("Ajustar al ancho"), OF_IC_ZOOM_OUT, false, V.zoom < 1.01f },
        { _("Más grande"), OF_IC_ZOOM_IN, false, false },
        { _("Información"), OF_IC_INFO, false, false },
    };
    ofu_sheet(NULL, items, 5, menu_pick, NULL);
}

/* -------------------------------------------------------------------------- */
/* The screen                                                                  */
/* -------------------------------------------------------------------------- */

static void relayout(void)
{
    V.W = lv_obj_get_width(V.root);
    V.H = lv_obj_get_height(V.root);
    int32_t top = V.W > V.H ? 76 : 88;
    lv_obj_set_size(V.top, V.W, top);
    lv_obj_set_pos(V.area, 0, top);
    /* the page in the middle stays in the middle */
    int mid = V.ypos ? page_at_middle() : 0;
    float frac = V.ypos && V.ph[mid] > 0 ? (V.sy + (float)V.ah / 2 - V.ypos[mid]) / (V.ph[mid] * V.scale) : 0;
    V.aw = V.W;
    V.ah = V.H - top;
    lv_obj_set_size(V.area, V.aw, V.ah);
    lv_obj_set_width(V.title, V.W - 2 * 84 - 120);
    layout();
    if (V.ypos) V.sy = V.ypos[mid] + frac * V.ph[mid] * V.scale - (float)V.ah / 2;
    clamp();
    lv_obj_invalidate(V.area);
}

static void back_cb(lv_event_t *e) { (void)e; of_app_home(); }
static void more_cb(lv_event_t *e) { (void)e; menu_open(); }

bool of_pdfv_open(lv_obj_t *parent, const char *path, const char *password, char *err, size_t errlen)
{
    if (path) {
        of_pdfv_close();
        memset(&V, 0, sizeof V);
        for (int k = 0; k < NTILES; k++) V.tiles[k].page = -1;
        of_pdfr_t *R = of_pdfr_open(path, password, err, errlen);
        if (!R) return false;
        V.R = R;
        snprintf(V.path, sizeof V.path, "%s", path);
        V.np = of_pdfr_count(R);
        V.pw = of_alloc((size_t)V.np * sizeof *V.pw);
        V.ph = of_alloc((size_t)V.np * sizeof *V.ph);
        if (!V.pw || !V.ph) { of_pdfv_close(); snprintf(err, errlen, "memory"); return false; }
        V.maxw = 1;
        for (int i = 0; i < V.np; i++) {
            of_pdfr_size(R, i, &V.pw[i], &V.ph[i]);
            if (V.pw[i] > V.maxw) V.maxw = V.pw[i];
        }
        V.zoom = 1;
        fb_load();
        of_pdfr_fallback(fb_glyph);
        V.worker = aos_hal_worker_start("oficina_pdf", worker_fn, NULL, 32 * 1024);
        if (!V.worker) { of_pdfv_close(); snprintf(err, errlen, "worker"); return false; }
    } else if (!V.R) {
        return false;
    }
    stop_fling();
    V.root = parent;
    ofu_ui_root = parent;
    lv_obj_set_style_bg_color(parent, OFT.bg, 0);
    V.top = ofu_box(parent, 10, 88);
    lv_obj_set_style_bg_color(V.top, OFT.surface, 0);
    lv_obj_set_style_bg_opa(V.top, LV_OPA_COVER, 0);
    lv_obj_t *bk = ofu_icon_btn(V.top, OF_IC_BACK, 84, back_cb, NULL);
    lv_obj_align(bk, LV_ALIGN_LEFT_MID, 4, 0);
    V.title = ofu_text(V.top, of_basename(V.path), ofu_font(OF_ST_BOLD, OF_SZ_M), OFT.text);
    lv_label_set_long_mode(V.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(V.title, ofu_font(OF_ST_BOLD, OF_SZ_M)->line_height);
    lv_obj_align(V.title, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_t *mo = ofu_icon_btn(V.top, OF_IC_MORE, 84, more_cb, NULL);
    lv_obj_align(mo, LV_ALIGN_RIGHT_MID, -4, 0);
    V.badge = ofu_text(V.top, "PDF", ofu_font(OF_ST_BOLD, OF_SZ_XS), OFT.dim);
    lv_obj_align(V.badge, LV_ALIGN_RIGHT_MID, -100, 0);
    V.area = ofu_box(parent, 10, 10);
    lv_obj_add_flag(V.area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(V.area, area_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    aos_gesture_attach(V.area, 0, gesture_cb, NULL);
    if (!V.poll) V.poll = lv_timer_create(poll_cb, 30, NULL);
    relayout();
    if (path) moved();
    return true;
}

void of_pdfv_close(void)
{
    stop_fling();
    if (V.poll) { lv_timer_delete(V.poll); V.poll = NULL; }
    if (V.worker) {
        V.cancel = true;
        aos_hal_worker_stop();
        V.worker = false;
    }
    if (V.state == 3) of_free(V.job_px);
    V.job_px = NULL;
    V.state = 0;
    for (int k = 0; k < NTILES; k++) tile_free(&V.tiles[k]);
    of_pdfr_fallback(NULL);
    fb_free();
    if (V.R) of_pdfr_close(V.R);
    of_free(V.pw);
    of_free(V.ph);
    of_free(V.ypos);
    memset(&V, 0, sizeof V);
}
