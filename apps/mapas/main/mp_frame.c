/*
 * MAPAS - a frame: the screen cut out of the best buffer the worker has, the
 * buttons and the scale drawn on top, and the whole of it to the panel.
 *
 * Why a whole frame and not the buffer blitted where it lies: the HAL's 1:1
 * blit of a packed, upright, little-endian frame goes by the DMA2D (4.4 ms
 * for 720 x 1280 on the board, 2026-09-29), while any blit with a stride, a
 * scale or a turn goes through the PPA (48 ms for the same frame at x1). A
 * cut on the CPU is the cheaper, and it is where the overlay gets drawn:
 * LVGL cannot put anything see-through over pixels it did not draw.
 *
 * Where the frame goes (page flipping, aos_hal_display_back/flip):
 *   - upright, the cut and the overlay are drawn straight into the panel's
 *     free buffer, which is then flipped to: no copy at all, no tearing;
 *   - turned (lying down, or upside down), the frame is cut into a->frame
 *     and the PPA turns it into the free buffer (blit_into), then the flip;
 *   - no free buffer (the simulator, pref fbs = 1): the old blit into the
 *     buffer on screen, or LVGL's canvas.
 * Nothing of LVGL's is over the map while it shows (the menu and the
 * search are screens of their own), so a flip hides nothing.
 *
 * Three ways the cut goes, by what the chosen buffer is to the screen:
 *   - the same scale (a FULL buffer, the zoom it was drawn at): rows copied;
 *   - anything else (HALF x2, a pinch, a zoom on its way): the nearest pixel,
 *     columns from a table, and a row that samples the same source row as
 *     the one above is a copy of it (half the rows of an x2);
 *   - no buffer yet, or a piece the buffer does not reach: the land's colour.
 *
 * a->frame is also the pixels of an LVGL canvas under everything, so when
 * LVGL redraws a part of the screen (a toast fading, a panel closing, the
 * system's pull-downs) it draws the map there too. While aos_ui_overlay()
 * says anything is over the app (a toast too: its text is worth reading),
 * the frame goes through LVGL instead, which composes it under them. A
 * frame drawn straight into the panel's buffer leaves a->frame behind:
 * mp_frame_sync() cuts it again once the map is still, and a frame LVGL
 * painted over since (aos_hal_display_back_age) goes up again whole.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mapas.h"

#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PAD         24
#define BTN_R       38
#define BTN_HIT     50
#define PILL_H      80

/* ---------------------------------------------------------------------------
 * The layout
 * ------------------------------------------------------------------------- */

void mp_layout(app_t *a)
{
    lay_t *L = &a->L;
    int sw = a->sw, sh = a->sh;
    L->sw = sw;
    L->sh = sh;
    /* the search pill across the top, the menu at its right; in landscape
     * the pill stops at 640 px so the map keeps the top right corner */
    L->menu = (disc_t){ sw - PAD - BTN_R, PAD + PILL_H / 2, BTN_R };
    L->px0 = PAD;
    L->py0 = PAD;
    L->py1 = PAD + PILL_H;
    L->px1 = L->menu.x - BTN_R - 16;
    if (L->px1 - L->px0 > 640) L->px1 = L->px0 + 640;
    /* + and - at the right, above the bottom edge, where the thumb is */
    L->zout = (disc_t){ sw - PAD - BTN_R, sh - 150, BTN_R };
    L->zin = (disc_t){ sw - PAD - BTN_R, sh - 150 - 2 * BTN_R - 20, BTN_R };
    L->scale_x = PAD + 6;
    L->scale_y = sh - 78;
    L->attr_y = sh - 46;
    L->status_y = L->py1 + 14;
}

bool mp_hit_disc(const disc_t *d, float x, float y)
{
    return fabsf(x - (float)d->x) < BTN_HIT && fabsf(y - (float)d->y) < BTN_HIT;
}

bool mp_hit_pill(const app_t *a, float x, float y)
{
    const lay_t *L = &a->L;
    return x >= L->px0 - 8 && x < L->px1 + 8 && y >= L->py0 - 8 && y < L->py1 + 8;
}

/* The overlay's areas in b's pixels, for a buffer centred on the view: the
 * labels keep out of them (where the screen will sit when it is drawn). */
int mp_frame_reserve(const app_t *a, const back_t *b, int16_t (*out)[4], int max)
{
    const lay_t *L = &a->L;
    int sr[5][4] = {
        { L->px0 - 6, L->py0 - 6, L->menu.x + BTN_R + 6, L->py1 + 6 },
        { L->zin.x - BTN_R - 6, L->zin.y - BTN_R - 6, L->zout.x + BTN_R + 6, L->zout.y + BTN_R + 6 },
        { L->scale_x - 6, L->scale_y - 30, L->scale_x + 260, L->scale_y + 8 },
        { PAD, L->attr_y - 4, L->sw - PAD, L->attr_y + 26 },
        { L->px0, L->status_y - 4, L->px1, L->status_y + 30 },
    };
    int n = 0;
    for (int i = 0; i < 5 && n < max; i++) {
        float ox = b->w * 0.5f - a->sw * 0.5f * b->res, oy = b->h * 0.5f - a->sh * 0.5f * b->res;
        out[n][0] = (int16_t)(ox + sr[i][0] * b->res);
        out[n][1] = (int16_t)(oy + sr[i][1] * b->res);
        out[n][2] = (int16_t)(ox + sr[i][2] * b->res);
        out[n][3] = (int16_t)(oy + sr[i][3] * b->res);
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * The cut
 * ------------------------------------------------------------------------- */

typedef struct {
    const back_t *b;
    float bxc, byc;             /* the screen's centre, in b's pixels */
    float inv;                  /* b's pixels per screen pixel */
    bool  exact;
} cut_t;

static void cut_of(const app_t *a, const back_t *b, cut_t *c)
{
    const mp_view_t *v = &a->view;
    float s = mp_px_per_unit(b->v.z) * b->res;
    c->b = b;
    c->bxc = b->w * 0.5f + (float)(int32_t)(v->cx - b->v.cx) * s;
    c->byc = b->h * 0.5f + (float)(int32_t)(v->cy - b->v.cy) * s;
    c->inv = b->res * exp2f(b->v.z - v->z);
    c->exact = fabsf(c->inv - 1.0f) < 0.002f;
}

/* how much of the screen b reaches, 0..1 */
static float coverage(const app_t *a, const cut_t *c)
{
    float hw = a->sw * 0.5f * c->inv, hh = a->sh * 0.5f * c->inv;
    float x0 = c->bxc - hw, x1 = c->bxc + hw, y0 = c->byc - hh, y1 = c->byc + hh;
    float ix = (x1 < c->b->w ? x1 : c->b->w) - (x0 > 0 ? x0 : 0);
    float iy = (y1 < c->b->h ? y1 : c->b->h) - (y0 > 0 ? y0 : 0);
    if (ix <= 0 || iy <= 0) return 0;
    return ix * iy / ((x1 - x0) * (y1 - y0));
}

/* The buffer a frame comes from: of those that reach the whole screen, the
 * one nearest 1:1 (a FULL at its own zoom beats a HALF x2, a HALF x2 beats a
 * FULL blown up x4 by a pinch); if none does, the one that reaches most. */
static bool choose(app_t *a, cut_t *out)
{
    bool have = false;
    float best_cov = 0, best_mag = 0;
    for (int q = 0; q < Q_N; q++) {
        int f = a->pair[q].front;
        if (f < 0) continue;
        back_t *b = &a->pair[q].b[f];
        if (b->geo != a->geo) continue;
        cut_t c;
        cut_of(a, b, &c);
        float cov = coverage(a, &c);
        float mag = fabsf(log2f(c.inv));
        bool full = cov > 0.9999f;
        bool better = !have || (full && best_cov <= 0.9999f) ||
                      (full && best_cov > 0.9999f && mag < best_mag - 0.01f) ||
                      (!full && best_cov <= 0.9999f && cov > best_cov + 0.001f);
        if (better) {
            *out = c;
            best_cov = cov;
            best_mag = mag;
            have = true;
        }
    }
    return have;
}

static int16_t s_col[AOS_PANEL_H];      /* the source column of every screen column */

static void cut_frame(app_t *a, const cut_t *c, uint16_t *dst)
{
    const int sw = a->sw, sh = a->sh;
    const uint16_t bg = mp_565(MP_BG);
    if (!c) {
        for (size_t i = 0; i < (size_t)sw * sh; i++) dst[i] = bg;
        return;
    }
    const back_t *b = c->b;
    if (c->exact) {
        int ox = (int)floorf(c->bxc - sw * 0.5f + 0.5f), oy = (int)floorf(c->byc - sh * 0.5f + 0.5f);
        int x0 = ox < 0 ? -ox : 0, x1 = ox + sw > b->w ? b->w - ox : sw;
        if (x1 < x0) x1 = x0;
        for (int y = 0; y < sh; y++) {
            uint16_t *d = dst + (size_t)y * sw;
            int by = oy + y;
            if (by < 0 || by >= b->h) {
                for (int x = 0; x < sw; x++) d[x] = bg;
                continue;
            }
            const uint16_t *srow = b->px + (size_t)by * b->w;
            for (int x = 0; x < x0; x++) d[x] = bg;
            if (x1 > x0) memcpy(d + x0, srow + ox + x0, (size_t)(x1 - x0) * 2);
            for (int x = x1; x < sw; x++) d[x] = bg;
        }
        return;
    }
    for (int x = 0; x < sw; x++) {
        int bx = (int)floorf(c->bxc + ((float)x + 0.5f - sw * 0.5f) * c->inv);
        s_col[x] = (int16_t)(bx < 0 || bx >= b->w ? -1 : bx);
    }
    int prev = -2;
    for (int y = 0; y < sh; y++) {
        uint16_t *d = dst + (size_t)y * sw;
        int by = (int)floorf(c->byc + ((float)y + 0.5f - sh * 0.5f) * c->inv);
        if (by < 0 || by >= b->h) by = -1;
        if (by == prev && y > 0) {
            memcpy(d, d - sw, (size_t)sw * 2);
            continue;
        }
        prev = by;
        if (by < 0) {
            for (int x = 0; x < sw; x++) d[x] = bg;
            continue;
        }
        const uint16_t *srow = b->px + (size_t)by * b->w;
        for (int x = 0; x < sw; x++) d[x] = s_col[x] < 0 ? bg : srow[s_col[x]];
    }
}

/* ---------------------------------------------------------------------------
 * What goes on top
 * ------------------------------------------------------------------------- */

#define INK     0xE8EAEE
#define DIM     0x9AA1AD
#define CARD    0x1E222B

static void pill(mp_fb_t *fb, float x0, float y0, float x1, float y1, uint32_t rgb, int alpha)
{
    float h = y1 - y0, r = h * 0.5f, yc = (y0 + y1) * 0.5f;
    float l[4] = { x0 + r, yc, x1 - r, yc };
    mp_polyline(fb, l, 2, h, rgb, alpha);
}

static void button(mp_fb_t *fb, const disc_t *d)
{
    mp_disc(fb, (float)d->x, (float)d->y, d->r + 2.0f, 0x000000, 140);
    mp_disc(fb, (float)d->x, (float)d->y, (float)d->r, CARD, 238);
}

/* s cut to fit w pixels, with "..." when it does not */
static void fit(const mp_font_t *f, const char *s, int w, char *out, size_t n)
{
    snprintf(out, n, "%s", s);
    if (mp_text_width(f, out) <= w) return;
    size_t l = strlen(out);
    while (l > 0) {
        l--;
        while (l > 0 && ((unsigned char)out[l] & 0xC0) == 0x80) l--;
        if (l + 4 > n) continue;
        memcpy(out + l, "...", 4);
        if (mp_text_width(f, out) <= w) return;
    }
}

static void draw_overlay(app_t *a, mp_fb_t *fb)
{
    const lay_t *L = &a->L;
    const mp_font_t *fs = &a->fonts[Q_FULL][MP_FONT_S];
    const mp_font_t *fm = &a->fonts[Q_FULL][MP_FONT_M];
    const mp_font_t *fl = &a->fonts[Q_FULL][MP_FONT_L];

    /* the search pill: a magnifier and the hint, or the pin's name and a
     * cross that takes the pin off */
    pill(fb, L->px0 - 2, L->py0 - 2, L->px1 + 2, L->py1 + 2, 0x000000, 140);
    pill(fb, L->px0, L->py0, L->px1, L->py1, CARD, 238);
    float mx = L->px0 + 40.0f, my = (L->py0 + L->py1) * 0.5f - 3;
    mp_disc(fb, mx, my, 13, INK, 255);
    mp_disc(fb, mx, my, 9, CARD, 255);
    float handle[4] = { mx + 9, my + 9, mx + 17, my + 17 };
    mp_polyline(fb, handle, 2, 4.0f, INK, 255);
    if (fl->ok) {
        int tx = L->px0 + 72, tw = L->px1 - tx - (a->pin_on ? 70 : 28);
        char txt[96];
        fit(fl, a->pin_on ? a->pin_name : _("Buscar un lugar o una calle"), tw, txt, sizeof txt);
        mp_text(fb, fl, tx, (L->py0 + L->py1 - fl->line_h) / 2, txt, a->pin_on ? INK : DIM, CARD, false);
    }
    if (a->pin_on) {
        float cx = L->px1 - 40.0f, cy = (L->py0 + L->py1) * 0.5f;
        float l1[4] = { cx - 11, cy - 11, cx + 11, cy + 11 }, l2[4] = { cx - 11, cy + 11, cx + 11, cy - 11 };
        mp_polyline(fb, l1, 2, 3.0f, DIM, 255);
        mp_polyline(fb, l2, 2, 3.0f, DIM, 255);
    }

    button(fb, &L->menu);
    for (int i = -1; i <= 1; i++) {
        float l[4] = { L->menu.x - 14.0f, L->menu.y + i * 9.0f, L->menu.x + 14.0f, L->menu.y + i * 9.0f };
        mp_polyline(fb, l, 2, 3.2f, INK, 255);
    }
    button(fb, &L->zin);
    float h[4] = { L->zin.x - 14.0f, (float)L->zin.y, L->zin.x + 14.0f, (float)L->zin.y };
    float vv[4] = { (float)L->zin.x, L->zin.y - 14.0f, (float)L->zin.x, L->zin.y + 14.0f };
    mp_polyline(fb, h, 2, 4.0f, INK, 255);
    mp_polyline(fb, vv, 2, 4.0f, INK, 255);
    button(fb, &L->zout);
    float m[4] = { L->zout.x - 14.0f, (float)L->zout.y, L->zout.x + 14.0f, (float)L->zout.y };
    mp_polyline(fb, m, 2, 4.0f, INK, 255);

    /* the pin of the last place found, and its name over it */
    if (a->pin_on && fm->ok) {
        float s = mp_px_per_unit(a->view.z);
        float px = a->sw * 0.5f + (float)(int32_t)(a->pin_cx - a->view.cx) * s;
        float py = a->sh * 0.5f + (float)(int32_t)(a->pin_cy - a->view.cy) * s;
        if (px > -200 && px < a->sw + 200 && py > -20 && py < a->sh + 60) {
            float stem[4] = { px, py - 16, px, py };
            mp_polyline(fb, stem, 2, 5.0f, 0x000000, 160);
            mp_polyline(fb, stem, 2, 3.0f, 0xFF5A4E, 255);
            mp_disc(fb, px, py - 27, 15.5f, 0x000000, 150);
            mp_disc(fb, px, py - 27, 14, 0xFF5A4E, 255);
            mp_disc(fb, px, py - 27, 5.5f, 0xFFFFFF, 255);
            int w = mp_text_width(fm, a->pin_name);
            mp_text(fb, fm, (int)px - w / 2, (int)py - 48 - fm->line_h, a->pin_name, 0xFFFFFF, MP_BG, true);
        }
    }

    if (!fs->ok) return;
    const int lh = fs->line_h;

    /* the scale: a round length of at least 80 px */
    float mpp = mp_metres_per_px(&a->view);
    static const float NICE[] = { 1, 2, 5 };
    float best = 0;
    for (float dec = 1; dec < 1e7f && !best; dec *= 10) {
        for (int i = 0; i < 3; i++) {
            if (NICE[i] * dec / mpp >= 80.0f) {
                best = NICE[i] * dec;
                break;
            }
        }
    }
    if (best > 0) {
        char txt[16];
        int spx = (int)(best / mpp);
        if (best >= 1000) snprintf(txt, sizeof txt, "%d km", (int)(best / 1000));
        else snprintf(txt, sizeof txt, "%d m", (int)best);
        float x0 = (float)L->scale_x, yb = (float)L->scale_y;
        float bar[6] = { x0, yb - 8, x0, yb, x0 + spx, yb };
        float end[4] = { x0 + spx, yb, x0 + spx, yb - 8 };
        mp_polyline(fb, bar, 3, 5.0f, MP_BG, 200);
        mp_polyline(fb, end, 2, 5.0f, MP_BG, 200);
        mp_polyline(fb, bar, 3, 2.2f, 0xD8DCE4, 255);
        mp_polyline(fb, end, 2, 2.2f, 0xD8DCE4, 255);
        mp_text(fb, fs, (int)x0 + spx + 8, (int)yb - lh + 4, txt, 0xD8DCE4, MP_BG, true);
    }
    /* the attribution the licence asks for */
    const char *attr = "© OpenFreeMap © OpenMapTiles © OpenStreetMap";
    mp_text(fb, fs, (a->sw - mp_text_width(fs, attr)) / 2, L->attr_y, attr, 0x8A919E, MP_BG, true);

    /* what is going on with the data, when something is */
    char st[64] = "";
    bool net = aos_hal_net_state() == AOS_NET_CONNECTED;
    if (mp_inflight(a) || (a->missing && a->online && a->tpl[0] && net)) {
        snprintf(st, sizeof st, "%s %d", _("Descargando"), a->missing ? a->missing : 1);
    } else if (a->missing && !a->online) {
        snprintf(st, sizeof st, "%s", _("Sin datos aquí"));
    } else if (a->missing && !net) {
        snprintf(st, sizeof st, "%s", _("Sin conexión"));
    } else if (a->missing && a->last_err == AOS_HTTP_ERR_SIN_HORA) {
        snprintf(st, sizeof st, "%s", _("Esperando la hora"));
    } else if (a->missing && a->last_err) {
        snprintf(st, sizeof st, "%s", _("Error de descarga"));
    }
    if (st[0] && fm->ok) {
        int w = mp_text_width(fm, st);
        int cx = (L->px0 + L->px1) / 2;
        pill(fb, cx - w / 2 - 18.0f, (float)L->status_y, cx + w / 2 + 18.0f, L->status_y + fm->line_h + 12.0f,
             0x000000, 170);
        mp_text(fb, fm, cx - w / 2, L->status_y + 6, st, 0xFFD27A, MP_BG, false);
    }
}

/* ---------------------------------------------------------------------------
 * To the panel
 * ------------------------------------------------------------------------- */

/* The screen into dst (sw x sh, packed rows): the cut and what goes on top. */
static void compose(app_t *a, uint16_t *dst)
{
    cut_t c;
    bool have = choose(a, &c);
    if (have) {
        a->composing = (back_t *)c.b;
        __sync_synchronize();
        /* swapped under us: that pair's newer buffer, which the worker is
         * not writing */
        if (!choose(a, &c)) have = false;
        a->composing = have ? (back_t *)c.b : NULL;
    }
    cut_frame(a, have ? &c : NULL, dst);
    a->composing = NULL;
    mp_fb_t fb = { dst, a->sw, a->sh, 0, 0, a->sw, a->sh };
    draw_overlay(a, &fb);
}

enum { PRESENT_NONE = 0, PRESENT_LVGL, PRESENT_BLIT, PRESENT_FLIP_TURNED, PRESENT_FLIP };

static void present_log(app_t *a, int how)
{
    if (a->present == how) return;
    a->present = how;
    static const char *const WHAT[] = {
        "", "through LVGL's canvas", "blitted into the buffer on screen",
        "cut, turned into the panel's free buffer by the PPA and flipped to",
        "cut straight into the panel's free buffer and flipped to",
    };
    aos_hal_log("mapas", "frames: %s%s", WHAT[how],
                how == PRESENT_LVGL && a->over_bits ? " (something of the system over the map)" : "");
}

void mp_frame_push(app_t *a)
{
    uint64_t t0 = aos_hal_uptime_us();
    /* A HAL that never blits (the simulator) is asked three times and then
     * left alone; one that did blit is always asked again (it refuses now
     * and then, while LVGL's own flush is still copying). Under anything of
     * the system's (a->over_bits, read by the tick in this same task) LVGL
     * draws the frame. */
    bool may = a->use_blit && !a->over_bits && (a->blit_ok || a->blit_fails < 3);
    uint16_t *back = may ? aos_hal_display_back() : NULL;
    bool direct = back && aos_hal_display_get_rotation() == 0 && a->sw == AOS_PANEL_W && a->sh == AOS_PANEL_H;
    compose(a, direct ? back : a->frame);
    uint64_t t1 = aos_hal_uptime_us();

    int how = PRESENT_LVGL;
    if (direct) {
        if (aos_hal_display_flip(back)) {
            how = PRESENT_FLIP;
            a->flipped = back;
            a->frame_synced = false;
        } else {
            compose(a, a->frame);           /* the flip refused: the old way */
            back = NULL;
        }
    }
    if (how == PRESENT_LVGL) a->frame_synced = true;
    if (how == PRESENT_LVGL && may) {
        if (back && aos_hal_display_blit_into(back, 0, 0, a->sw, a->sh, a->frame) && aos_hal_display_flip(back)) {
            how = PRESENT_FLIP_TURNED;
            a->flipped = back;
        } else if (aos_hal_display_blit_scaled(0, 0, a->sw, a->sh, a->frame, 1, false)) {
            how = PRESENT_BLIT;
            a->flipped = NULL;
        } else if (++a->blit_fails == 3 && !a->blit_ok) {
            aos_hal_log("mapas", "no direct blit here: the frames go through LVGL");
        }
    }
    if (how == PRESENT_LVGL) {
        a->flipped = NULL;
        lv_obj_invalidate(a->canvas);
    } else {
        a->blit_ok = true;
    }
    present_log(a, how);
    uint64_t t2 = aos_hal_uptime_us();
    a->pushed_ms = (uint32_t)aos_hal_uptime_ms();
    a->b_frames++;
    a->b_compose += (uint32_t)(t1 - t0);
    a->b_present += (uint32_t)(t2 - t1);
    if (how == PRESENT_FLIP || how == PRESENT_FLIP_TURNED) a->b_flips++;
    else if (how == PRESENT_BLIT) a->b_blits++;
    else a->b_lvgl++;
}

bool mp_frame_sync(app_t *a, uint32_t now)
{
    if (a->frame_synced) return false;
    /* LVGL painted over the frame we flipped to (a panel's last frame, the
     * map coming back from the menu): up again, whole */
    bool touched = false;
    if (a->flipped && !a->over_bits && aos_hal_display_back_age(a->flipped, NULL, &touched) && touched) {
        a->b_retouch++;
        return true;
    }
    /* still for a moment: a->frame the same as the screen again, for
     * whatever LVGL redraws next (a pull-down, a toast) */
    if (now - a->pushed_ms > SYNC_MS) {
        compose(a, a->frame);
        a->frame_synced = true;
    }
    return false;
}
