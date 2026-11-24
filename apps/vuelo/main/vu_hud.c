/*
 * PLANEO - the instruments (see vu_hud.h)
 */
#include "vu_hud.h"
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

static void line(uint16_t *out, int W, int H, float x0, float y0, float x1, float y1, float w, uint16_t c, int a)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    int n = (int)(len * 2) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        ring(out, W, H, x0 + dx * t, y0 + dy * t, 0, w * 0.5f, c, a);
    }
}

/* --------------------------------------------------------------------------
 * Layout and drawing
 * -------------------------------------------------------------------------- */

void vh_layout(vh_layout_t *L, int W, int H)
{
    memset(L, 0, sizeof *L);
    L->W = W;
    L->H = H;
    float u = (float)H / 240.0f;
    L->u = u;
    L->btn_s = (int)(22 * u);
    L->btn_y = (int)(6 * u);
    int gap = (int)(6 * u), total = VH_BTNS * L->btn_s + (VH_BTNS - 1) * gap;
    int x0 = (W - total) / 2;
    for (int i = 0; i < VH_BTNS; i++) L->btn_x[i] = x0 + i * (L->btn_s + gap);
    L->thr_x0 = W - (int)(30 * u);
    L->thr_x1 = W - (int)(14 * u);
    L->thr_y0 = (int)(70 * u);
    L->thr_y1 = H - (int)(26 * u);
    L->stick_r = 30 * u;
    L->stick_home_x = 58 * u;
    L->stick_home_y = (float)H - 58 * u;
}

static void icon(uint16_t *out, int W, int H, int i, int x, int y, int s, uint16_t c, float u)
{
    float cx = (float)x + (float)s * 0.5f, cy = (float)y + (float)s * 0.5f, r = (float)s * 0.32f;
    switch (i) {
    case VH_BTN_WORLD:      /* a globe */
        ring(out, W, H, cx, cy, r - 1.2f * u, r, c, 32);
        line(out, W, H, cx - r, cy, cx + r, cy, 1.0f * u, c, 32);
        line(out, W, H, cx, cy - r, cx, cy + r, 1.0f * u, c, 32);
        ring(out, W, H, cx, cy, r * 0.45f - 1.0f * u, r * 0.45f, c, 24);
        break;
    case VH_BTN_VIEW:       /* an eye */
        ring(out, W, H, cx, cy, r * 0.9f - 1.2f * u, r * 0.9f, c, 32);
        ring(out, W, H, cx, cy, 0, r * 0.38f, c, 32);
        break;
    case VH_BTN_AUTO: {     /* the letter A in a ring: the autopilot */
        ring(out, W, H, cx, cy, r - 1.2f * u, r, c, 32);
        int sc = u >= 1.4f ? 2 : 1;
        vh_text(out, W, H, (int)(cx - 2.5f * (float)sc), (int)(cy - 3.5f * (float)sc), sc, "A", c);
        break;
    }
    default: {              /* the resolution: a small screen */
        int w = (int)(r * 1.8f), h = (int)(r * 1.2f);
        rect(out, W, H, (int)cx - w / 2, (int)cy - h / 2, w, (int)(1.2f * u + 0.5f), c, 32);
        rect(out, W, H, (int)cx - w / 2, (int)cy + h / 2 - (int)(1.2f * u + 0.5f), w, (int)(1.2f * u + 0.5f), c, 32);
        rect(out, W, H, (int)cx - w / 2, (int)cy - h / 2, (int)(1.2f * u + 0.5f), h, c, 32);
        rect(out, W, H, (int)cx + w / 2 - (int)(1.2f * u + 0.5f), (int)cy - h / 2, (int)(1.2f * u + 0.5f), h, c, 32);
        break;
    }
    }
}

void vh_draw(const vh_layout_t *L, const vh_state_t *s, uint16_t *out)
{
    int W = L->W, H = L->H;
    float u = L->u;
    int sc = u >= 1.4f ? 2 : 1;
    char b[48];
    uint16_t white = 0xFFFF, amber = vr_565(0xFFC23C), red = vr_565(0xFF4A3A);
    /* the speed, left; the height, right */
    snprintf(b, sizeof b, "%d", (int)(s->speed_kmh + 0.5f));
    int x = (int)(10 * u), y = (int)(8 * u);
    int w = vh_text(out, W, H, x, y, sc * 2, b, white);
    vh_text(out, W, H, x + w + 3 * sc, y + 7 * sc, sc, "KM/H", white);
    snprintf(b, sizeof b, "%d", (int)(s->alt_m + 0.5f));
    int wa = vh_text_w(b, sc * 2), wm = vh_text_w("M", sc);
    int xr = W - (int)(10 * u) - wa - wm - 3 * sc;
    vh_text(out, W, H, xr, y, sc * 2, b, s->agl_m < 25 ? amber : white);
    vh_text(out, W, H, xr + wa + 3 * sc, y + 7 * sc, sc, "M", white);
    /* the buttons */
    for (int i = 0; i < VH_BTNS; i++) {
        bool on = (s->pressed >> i) & 1;
        if (i == VH_BTN_AUTO && s->autopilot) on = true;
        int bx = L->btn_x[i], by = L->btn_y, bs = L->btn_s;
        ring(out, W, H, (float)bx + (float)bs * 0.5f, (float)by + (float)bs * 0.5f, 0, (float)bs * 0.5f,
             on ? vr_565(0xFF8A1E) : 0x0000, on ? 26 : 13);
        icon(out, W, H, i, bx, by, bs, white, u);
    }
    /* the throttle */
    {
        int x0 = L->thr_x0, x1 = L->thr_x1, y0 = L->thr_y0, y1 = L->thr_y1;
        rect(out, W, H, x0, y0, x1 - x0, y1 - y0, 0x0000, 11);
        int fy = y1 - (int)((float)(y1 - y0) * s->thr);
        rect(out, W, H, x0 + 2, fy, x1 - x0 - 4, y1 - fy, vr_565(0xFF8A1E), 22);
        rect(out, W, H, x0 - 3, fy - (int)(2 * u), x1 - x0 + 6, (int)(4 * u), white, 30);
    }
    /* the stick */
    {
        float bx = s->stick_on ? s->stick_bx : L->stick_home_x, by = s->stick_on ? s->stick_by : L->stick_home_y;
        float r = L->stick_r;
        ring(out, W, H, bx, by, r - 1.5f * u, r + 0.5f * u, white, s->stick_on ? 20 : 9);
        float kx = bx + s->stick_x * r, ky = by - s->stick_y * r;
        ring(out, W, H, kx, ky, 0, 9.0f * u, white, s->stick_on ? 22 : 8);
    }
    /* warnings */
    if (s->crashed) {
        const char *t = "!!!";
        vh_text(out, W, H, (W - vh_text_w(t, sc * 3)) / 2, H / 2 - 30 * sc, sc * 3, t, red);
    } else if (s->stall) {
        const char *t = "STALL";
        vh_text(out, W, H, (W - vh_text_w(t, sc * 2)) / 2, (int)(40 * u), sc * 2, t, amber);
    }
    /* the measurement */
    if (s->fps10 > 0) {
        snprintf(b, sizeof b, "%d.%d FPS %dX%d", s->fps10 / 10, s->fps10 % 10, s->res_w, s->res_h);
        vh_text(out, W, H, (W - vh_text_w(b, sc)) / 2, H - (int)(12 * u), sc, b, white);
    }
    if (s->world) vh_text(out, W, H, x, y + 18 * sc, sc, s->world, white);
}
