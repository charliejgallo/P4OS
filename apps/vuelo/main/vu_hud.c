/*
 * PLANEO - the instruments (see vu_hud.h)
 */
#include "vu_hud.h"
#include "vu_plane.h"
#include "vu_render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

/* --------------------------------------------------------------------------
 * The font: 5 x 7, a row a byte, bit 4 the left column
 * -------------------------------------------------------------------------- */

typedef struct {
    char    c;
    uint8_t r[7];
} glyph_t;

static const glyph_t FONT[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } }, { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } }, { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } }, { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } }, { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } }, { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'D', { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } }, { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } }, { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } }, { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } }, { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } }, { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } }, { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } }, { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 } }, { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } }, { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } }, { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } }, { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
    { ',', { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 } },
};

static const uint8_t *glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof FONT / sizeof FONT[0]; i++)
        if (FONT[i].c == c) return FONT[i].r;
    return NULL;
}

static inline void put(uint16_t *out, int W, int H, int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) out[y * W + x] = c;
}

static void block(uint16_t *out, int W, int H, int x, int y, int s, uint16_t c)
{
    for (int j = 0; j < s; j++)
        for (int i = 0; i < s; i++) put(out, W, H, x + i, y + j, c);
}

int vh_text_w(const char *txt, int s)
{
    int n = (int)strlen(txt);
    return n ? n * 6 * s - s : 0;
}

int vh_text(uint16_t *out, int W, int H, int x, int y, int s, const char *txt, uint16_t col)
{
    /* the edge first: every lit pixel's neighbours in dark */
    for (int pass = 0; pass < 2; pass++) {
        int cx = x;
        for (const char *p = txt; *p; p++, cx += 6 * s) {
            const uint8_t *g = glyph(*p);
            if (!g) continue;
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++) {
                    if (!(g[r] & (0x10 >> b))) continue;
                    int px = cx + b * s, py = y + r * s;
                    if (pass == 0) {
                        block(out, W, H, px - 1, py - 1, s + 2, 0x18C3);
                    } else {
                        block(out, W, H, px, py, s, col);
                    }
                }
        }
    }
    return vh_text_w(txt, s);
}

/* --------------------------------------------------------------------------
 * Shapes
 * -------------------------------------------------------------------------- */

static void rect(uint16_t *out, int W, int H, int x, int y, int w, int h, uint16_t c, int a)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int j = y0; j < y1; j++) {
        uint16_t *o = out + j * W;
        if (a >= 32)
            for (int i = x0; i < x1; i++) o[i] = c;
        else
            for (int i = x0; i < x1; i++) o[i] = vr_blend(o[i], c, a);
    }
}

/* a disc (r1 = 0) or a ring between r1 and r2, soft at the edges */
static void ring(uint16_t *out, int W, int H, float cx, float cy, float r1, float r2, uint16_t c, int a)
{
    int x0 = (int)(cx - r2 - 1), x1 = (int)(cx + r2 + 2), y0 = (int)(cy - r2 - 1), y1 = (int)(cy + r2 + 2);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) {
        uint16_t *o = out + y * W;
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = 1.0f;
            if (d > r2) cov = 1.0f - (d - r2);
            else if (r1 > 0 && d < r1) cov = 1.0f - (r1 - d);
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            o[x] = vr_blend(o[x], c, (int)((float)a * cov));
        }
    }
}

/* --------------------------------------------------------------------------
 * Layout and drawing
 * -------------------------------------------------------------------------- */

#include "vu_game.h"

void vh_layout(vh_layout_t *L, int W, int H)
{
    memset(L, 0, sizeof *L);
    L->W = W;
    L->H = H;
    float u = (float)H / 240.0f;
    L->u = u;
    L->btn_s = (int)(24 * u);
    L->btn_y = (int)(6 * u);
    L->btn_x[VH_BTN_PAUSE] = (int)(6 * u);
    L->btn_x[VH_BTN_VIEW] = W - (int)(6 * u) - L->btn_s;
    L->thr_x0 = W - (int)(30 * u);
    L->thr_x1 = W - (int)(14 * u);
    L->thr_y0 = (int)(76 * u);
    L->thr_y1 = H - (int)(26 * u);
    L->rud_x0 = W - (int)(150 * u);
    L->rud_x1 = W - (int)(46 * u);
    L->rud_y = H - (int)(20 * u);
    L->stick_r = 30 * u;
    L->stick_home_x = 58 * u;
    L->stick_home_y = (float)H - 58 * u;
}

static void icon(uint16_t *out, int W, int H, int i, int x, int y, int s, uint16_t c, float u)
{
    float cx = (float)x + (float)s * 0.5f, cy = (float)y + (float)s * 0.5f, r = (float)s * 0.3f;
    int t = (int)(2.2f * u + 0.5f);
    if (i == VH_BTN_PAUSE) {
        rect(out, W, H, (int)(cx - r * 0.7f), (int)(cy - r), t, (int)(r * 2), c, 32);
        rect(out, W, H, (int)(cx + r * 0.7f) - t, (int)(cy - r), t, (int)(r * 2), c, 32);
    } else {
        /* a camera: a body and its lens */
        rect(out, W, H, (int)(cx - r * 1.1f), (int)(cy - r * 0.6f), (int)(r * 2.2f), (int)(r * 1.4f), c, 32);
        ring(out, W, H, cx, cy + r * 0.1f, 0, r * 0.45f, 0x2124, 32);
        rect(out, W, H, (int)(cx - r * 0.4f), (int)(cy - r * 0.95f), (int)(r * 0.8f), (int)(r * 0.4f), c, 32);
    }
}

/* an arrowhead at (x, y) pointing along (dx, dy) */
static void arrow(uint16_t *out, int W, int H, float x, float y, float dx, float dy, float sz, uint16_t c)
{
    float px = -dy, py = dx;
    float xs[3] = { x + dx * sz, x - dx * sz * 0.6f + px * sz * 0.8f, x - dx * sz * 0.6f - px * sz * 0.8f };
    float ys[3] = { y + dy * sz, y - dy * sz * 0.6f + py * sz * 0.8f, y - dy * sz * 0.6f - py * sz * 0.8f };
    float xo[3], yo[3];
    for (int k = 0; k < 3; k++) {
        xo[k] = x + (xs[k] - x) * 1.35f;
        yo[k] = y + (ys[k] - y) * 1.35f;
    }
    vp_fill(out, W, 0, H, xo, yo, 0x0841, 18);
    vp_fill(out, W, 0, H, xs, ys, c, 32);
}

static const int KIND_WORD[] = { VT_W_RINGS, VT_W_RACE, VT_W_DELIVER };

void vh_draw(const vh_layout_t *L, const vh_state_t *s, const vt_set_t *t, uint16_t *out)
{
    int W = L->W, H = L->H;
    float u = L->u;
    char b[48];
    uint16_t white = 0xFFFF, amber = vr_565(0xFFC23C), red = vr_565(0xFF4A3A), green = vr_565(0x7CFF8C);
    int top = (int)(6 * u);
    /* the buttons */
    for (int i = 0; i < VH_BTNS; i++) {
        bool on = (s->pressed >> i) & 1;
        int bx = L->btn_x[i], by = L->btn_y, bs = L->btn_s;
        ring(out, W, H, (float)bx + (float)bs * 0.5f, (float)by + (float)bs * 0.5f, 0, (float)bs * 0.5f,
             on ? vr_565(0xFF8A1E) : 0x0000, on ? 26 : 13);
        icon(out, W, H, i, bx, by, bs, white, u);
    }
    if (t && t->ok) {
        /* the speed, after the pause button; the height before the camera's */
        int x = L->btn_x[VH_BTN_PAUSE] + L->btn_s + (int)(8 * u);
        snprintf(b, sizeof b, "%d", (int)(s->speed_kmh + 0.5f));
        int w = vt_num(out, W, H, t, VT_M, b, x, top, white, VT_LEFT);
        int dy = t->dig[VT_M][0].h - t->word[VT_S][VT_W_KMH].h - (int)(2 * u);
        vt_draw(out, W, H, &t->word[VT_S][VT_W_KMH], x + w + (int)(3 * u), top + dy, white, VT_LEFT);
        int xr = L->btn_x[VH_BTN_VIEW] - (int)(8 * u);
        int wm = vt_draw(out, W, H, &t->word[VT_S][VT_W_M], xr, top + dy, white, VT_RIGHT);
        snprintf(b, sizeof b, "%d", (int)(s->alt_m + 0.5f));
        vt_num(out, W, H, t, VT_M, b, xr - wm - (int)(3 * u), top, s->agl_m < 20 ? amber : white, VT_RIGHT);
        /* the mission: what, how far along, the clock */
        if (s->kind != VG_FREE && s->kind >= 0 && s->kind < VG_KINDS) {
            int cx = W / 2;
            int y = top;
            const vt_mask_t *kw = &t->word[VT_S][KIND_WORD[s->kind]];
            int prog = s->kind == VG_DELIVER ? s->next / 2 : s->next;
            int total = s->kind == VG_DELIVER ? s->npt / 2 : s->npt;
            snprintf(b, sizeof b, "%d/%d", prog, total);
            int wn = vt_num_w(t, VT_S, b);
            int ww = kw->w + (int)(5 * u) + wn;
            vt_draw(out, W, H, kw, cx - ww / 2, y, amber, VT_LEFT);
            vt_num(out, W, H, t, VT_S, b, cx - ww / 2 + kw->w + (int)(5 * u), y, white, VT_LEFT);
            y += kw->h;
            if (s->clock >= 0 || s->done) {
                vg_fmt_time(b, sizeof b, s->clock);
                vt_num(out, W, H, t, VT_M, b, cx, y, white, VT_CENTER);
                if (s->place) {
                    const vt_mask_t *pm = &t->word[VT_S][s->place == 1 ? VT_W_FIRST : VT_W_SECOND];
                    vt_draw(out, W, H, pm, cx + vt_num_w(t, VT_M, b) / 2 + (int)(6 * u),
                            y + t->dig[VT_M][0].h - pm->h - (int)(2 * u), s->place == 1 ? green : amber, VT_LEFT);
                }
                y += t->dig[VT_M][0].h;
            }
            if (s->kind == VG_DELIVER && !s->done)
                vt_draw(out, W, H, &t->word[VT_S][s->carrying ? VT_W_TODROP : VT_W_TOFIND], cx, y, white, VT_CENTER);
            /* the countdown */
            if (s->clock < 0) {
                int n = (int)ceilf(-s->clock);
                snprintf(b, sizeof b, "%d", n);
                vt_num(out, W, H, t, VT_L, b, cx, y, amber, VT_CENTER);   /* where the clock goes */
            }
        }
        /* a banner, a moment */
        if (s->banner >= 0 && s->banner < VT_WORDS && (s->banner_t < 1.6f || s->done)) {
            const vt_mask_t *m = &t->word[VT_L][s->banner];
            uint16_t c = s->banner == VT_W_CRASH || s->banner == VT_W_LOST ? red : amber;
            vt_draw(out, W, H, m, W / 2, H / 2 - (int)(52 * u), c, VT_CENTER);
        } else if (s->stall && !s->crashed) {
            vt_draw(out, W, H, &t->word[VT_M][VT_W_STALL], W / 2, (int)(64 * u), amber, VT_CENTER);
        } else if (s->agl_m < 12 && !s->crashed && s->kind != VG_DELIVER) {
            vt_draw(out, W, H, &t->word[VT_M][VT_W_LOW], W / 2, (int)(64 * u), amber, VT_CENTER);
        }
        /* the arrow to the next target, at the edge when it is off the frame */
        if (s->target && !s->done) {
            float ax, ay;
            float m = 26 * u;
            if (s->target_on) {
                ax = s->tx;
                ay = s->ty - 16 * u;
                /* never over the clock */
                if (ay < 84 * u) ay = 84 * u;
            } else {
                /* from the centre along the direction, to the frame's edge */
                float cx = (float)W * 0.5f, cy = (float)H * 0.5f;
                /* below the top's numbers, inside the thumbs' sides */
                float top_m = 50 * u;
                float kx = s->tdx != 0 ? ((s->tdx > 0 ? (float)W - m - 20 * u : m) - cx) / s->tdx : 1e9f;
                float ky = s->tdy != 0 ? ((s->tdy > 0 ? (float)H - m : top_m) - cy) / s->tdy : 1e9f;
                float k = kx < ky ? kx : ky;
                ax = cx + s->tdx * k;
                ay = cy + s->tdy * k;
            }
            if (s->target_on) arrow(out, W, H, ax, ay, 0, 1, 6 * u, amber);
            else arrow(out, W, H, ax, ay, s->tdx, s->tdy, 7 * u, amber);
            snprintf(b, sizeof b, "%d", (int)(s->tdist + 0.5f));
            float lx = ax - s->tdx * 14 * u, ly = ay - s->tdy * 14 * u - (float)t->dig[VT_S][0].h * 0.5f;
            if (s->target_on) ly = ay - 12 * u - (float)t->dig[VT_S][0].h;
            vt_num(out, W, H, t, VT_S, b, (int)lx, (int)ly, white, VT_CENTER);
        }
    }
    /* the throttle */
    {
        int x0 = L->thr_x0, x1 = L->thr_x1, y0 = L->thr_y0, y1 = L->thr_y1;
        rect(out, W, H, x0, y0, x1 - x0, y1 - y0, 0x0000, 11);
        int fy = y1 - (int)((float)(y1 - y0) * s->thr);
        rect(out, W, H, x0 + 2, fy, x1 - x0 - 4, y1 - fy, vr_565(0xFF8A1E), 22);
        rect(out, W, H, x0 - 3, fy - (int)(2 * u), x1 - x0 + 6, (int)(4 * u), white, 30);
    }
    /* the pedals: a track that springs back to the middle */
    {
        int x0 = L->rud_x0, x1 = L->rud_x1, y = L->rud_y, h = (int)(12 * u);
        rect(out, W, H, x0, y - h / 2, x1 - x0, h, 0x0000, 11);
        int cx = (x0 + x1) / 2;
        rect(out, W, H, cx - (int)u, y - h / 2, (int)(2 * u + 0.5f), h, white, 14);
        float kx = (float)cx + s->rud * (float)(x1 - x0) * 0.5f;
        int kw = (int)(14 * u);
        if (s->rud > 0.02f || s->rud < -0.02f) {
            int a = s->rud > 0 ? cx : (int)kx, b = s->rud > 0 ? (int)kx : cx;
            rect(out, W, H, a, y - h / 2 + 2, b - a, h - 4, vr_565(0xFF8A1E), 22);
        }
        rect(out, W, H, (int)kx - kw / 2, y - (int)(9 * u), kw, (int)(18 * u), white, s->rud_on ? 30 : 18);
        /* little arrows at its ends */
        float ah = 4 * u;
        float lx[3] = { (float)x0 - 3 * u, (float)x0 + 3 * u, (float)x0 + 3 * u }, ly[3] = { (float)y, (float)y - ah, (float)y + ah };
        float rx[3] = { (float)x1 + 3 * u, (float)x1 - 3 * u, (float)x1 - 3 * u }, ry[3] = { (float)y, (float)y - ah, (float)y + ah };
        vp_fill(out, W, 0, H, lx, ly, white, 20);
        vp_fill(out, W, 0, H, rx, ry, white, 20);
    }
    /* the stick */
    {
        float bx = s->stick_on ? s->stick_bx : L->stick_home_x, by = s->stick_on ? s->stick_by : L->stick_home_y;
        float r = L->stick_r;
        ring(out, W, H, bx, by, r - 1.5f * u, r + 0.5f * u, white, s->stick_on ? 20 : 9);
        float kx = bx + s->stick_x * r, ky = by - s->stick_y * r;
        ring(out, W, H, kx, ky, 0, 9.0f * u, white, s->stick_on ? 22 : 8);
    }
    /* the measurement */
    if (s->fps10 > 0) {
        snprintf(b, sizeof b, "%d.%d FPS %dX%d", s->fps10 / 10, s->fps10 % 10, s->res_w, s->res_h);
        vh_text(out, W, H, (W - vh_text_w(b, 1)) / 2, H - (int)(10 * u), 1, b, white);
    }
}
