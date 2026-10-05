/*
 * P4OS - PWM generator: drawings.
 *
 * The signal card draws what the pin does, to scale: two periods of the
 * square wave (one for a servo, whose 20 ms make the pulse a sliver, as it
 * is), the period and the time high; for the analog level, the pulse
 * density the sigma-delta puts out and the level the RC filter makes of it.
 *
 * The wiring sheet draws small schematics from tables of parts on a
 * 200 x 120 grid, scaled to the space there is.
 */
#include "pw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- primitives */

static void line(lv_layer_t *L, float x1, float y1, float x2, float y2, lv_color_t c, int32_t w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.round_start = d.round_end = 1;
    d.p1.x = (int32_t)x1;
    d.p1.y = (int32_t)y1;
    d.p2.x = (int32_t)x2;
    d.p2.y = (int32_t)y2;
    lv_draw_line(L, &d);
}

static void dashed(lv_layer_t *L, float x1, float y1, float x2, float y2, lv_color_t c)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = 2;
    d.dash_width = 6;
    d.dash_gap = 6;
    d.p1.x = (int32_t)x1;
    d.p1.y = (int32_t)y1;
    d.p2.x = (int32_t)x2;
    d.p2.y = (int32_t)y2;
    lv_draw_line(L, &d);
}

static void text(lv_layer_t *L, const char *t, float cx, float cy, const lv_font_t *f, lv_color_t c,
                 lv_text_align_t align)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1;
    d.font = f;
    d.color = c;
    d.align = align;
    int32_t h = lv_font_get_line_height(f);
    lv_area_t a;
    if (align == LV_TEXT_ALIGN_LEFT) { a.x1 = (int32_t)cx; a.x2 = (int32_t)cx + 600; }
    else if (align == LV_TEXT_ALIGN_RIGHT) { a.x1 = (int32_t)cx - 600; a.x2 = (int32_t)cx; }
    else { a.x1 = (int32_t)cx - 300; a.x2 = (int32_t)cx + 300; }
    a.y1 = (int32_t)cy - h / 2;
    a.y2 = a.y1 + h;
    lv_draw_label(L, &d, &a);
}

static void disc(lv_layer_t *L, float x, float y, int32_t r, lv_color_t c)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.center.x = (int32_t)x;
    d.center.y = (int32_t)y;
    d.radius = (uint16_t)r;
    d.width = r;
    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(L, &d);
}

static void ring(lv_layer_t *L, float x, float y, int32_t r, int32_t w, lv_color_t c)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.center.x = (int32_t)x;
    d.center.y = (int32_t)y;
    d.radius = (uint16_t)r;
    d.width = w;
    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(L, &d);
}

/* ---------------------------------------------------------------- the signal */

void pw_draw_signal(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *L = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float x0 = (float)a.x1 + 24, x1 = (float)a.x2 - 24;
    float top = (float)a.y1 + 20;
    const pw_cfg_t *c = &h->c;
    bool live = h->pwm || h->dac;
    lv_color_t col = live ? lv_color_hex(pw_mode_color(c->mode)) : AOS_C_DIM;
    lv_color_t dim = AOS_C_DIM;
    const lv_font_t *f = aos_font_caption;
    char t1[40], t2[48], t3[40];
    float yh = top + 50, yl = (float)a.y2 - 28;

    if (c->mode == PW_M_DAC) {
        float v = h->live;
        char n[16];
        pw_fnum(n, sizeof n, v * 3.3f, 2);
        snprintf(t1, sizeof t1, _("%s V a la salida del filtro"), n);
        text(L, t1, x0, top + 10, f, AOS_C_TEXT, LV_TEXT_ALIGN_LEFT);
        text(L, _("pulsos a 1 MHz"), x1, top + 10, f, dim, LV_TEXT_ALIGN_RIGHT);
        /* the pulse density: one bit of sigma-delta per slot */
        float xm = x0 + (x1 - x0) * 0.62f;
        const int N = 32;
        float sw = (xm - x0) / (float)N, acc = 0.5f;
        int prev = 0;
        for (int i = 0; i < N; i++) {
            acc += v;
            int bit = acc >= 1.0f;
            if (bit) acc -= 1.0f;
            float xa = x0 + sw * (float)i, xb = xa + sw;
            if (bit != prev) line(L, xa, yh, xa, yl, col, 3);
            line(L, xa, bit ? yh : yl, xb, bit ? yh : yl, col, 3);
            prev = bit;
        }
        /* and what the RC makes of it */
        float xr = xm + 30;
        dashed(L, xr, yh, x1, yh, dim);
        dashed(L, xr, yl, x1, yl, dim);
        float yv = yl - (yl - yh) * v;
        line(L, xr, yv, x1, yv, AOS_C_TEXT, 5);
        text(L, "3,3 V", x1, yh - 14, aos_font_tiny, dim, LV_TEXT_ALIGN_RIGHT);
        text(L, "0 V", x1, yl + 14, aos_font_tiny, dim, LV_TEXT_ALIGN_RIGHT);
        text(L, "RC", (xm + xr) / 2, (yh + yl) / 2, f, dim, LV_TEXT_ALIGN_CENTER);
        return;
    }

    float hz = h->pwm ? (float)aos_io_pwm_freq(h->pwm) : (float)pw_freq_of(c);
    if (hz < 1.0f) hz = 1.0f;
    float d = pw_out(h, h->live);
    float T = 1.0f / hz;
    pw_fmt_time(t1, sizeof t1, T);
    pw_fmt_time(t3, sizeof t3, d * T);
    char p[16];
    pw_fnum(p, sizeof p, d * 100.0f, 1);
    if (d < 0.0005f) snprintf(t2, sizeof t2, "%s", _("siempre abajo"));
    else if (d > 0.9995f) snprintf(t2, sizeof t2, "%s", _("siempre arriba"));
    else snprintf(t2, sizeof t2, _("alto %s (%s %%)"), t3, p);
    text(L, t1, x0, top + 10, f, AOS_C_TEXT, LV_TEXT_ALIGN_LEFT);
    text(L, t2, x1, top + 10, f, AOS_C_TEXT, LV_TEXT_ALIGN_RIGHT);

    /* what the pin does: inverted, low is the active level */
    int periods = c->mode == PW_M_SERVO ? 1 : 2;
    float pw = (x1 - x0) / (float)periods;
    float hi = c->invert ? yl : yh, lo = c->invert ? yh : yl;
    for (int k = 0; k <= periods; k++) dashed(L, x0 + pw * (float)k, yh - 6, x0 + pw * (float)k, yl + 6, AOS_C_CARD2);
    for (int k = 0; k < periods; k++) {
        float xs = x0 + pw * (float)k, xe = xs + pw, xh = xs + d * pw;
        if (d > 0.0005f) {
            if (k == 0 || d < 0.9995f) line(L, xs, lo, xs, hi, col, 4);
            line(L, xs, hi, xh, hi, col, 4);
        }
        if (d < 0.9995f) {
            if (d > 0.0005f) line(L, xh, hi, xh, lo, col, 4);
            line(L, d > 0.0005f ? xh : xs, lo, xe, lo, col, 4);
        }
    }
    if (c->invert) text(L, _("invertida"), (x0 + x1) / 2, top + 10, aos_font_tiny, AOS_C_ORANGE, LV_TEXT_ALIGN_CENTER);
}

/* ---------------------------------------------------------------- schematics */

enum { S_END, S_WIRE, S_RES, S_LED, S_CAP, S_GND, S_DOT, S_TEXT, S_BOX, S_NPN };

typedef struct {
    uint8_t     op;
    int16_t     x1, y1, x2, y2;
    uint32_t    rgb;            /* 0: the text colour */
    const char *t;              /* S_TEXT; "$PIN" is the channel's pin */
} sk_t;

#define ORANGE 0xFF9F0A
#define RED    0xFF453A
#define BROWN  0xA2845E

static const sk_t SK_LED[] = {
    { S_DOT,  20, 50, 0, 0, 0, NULL },        { S_TEXT, 20, 30, 0, 0, 0, "$PIN" },
    { S_WIRE, 20, 50, 50, 50, 0, NULL },      { S_RES, 50, 50, 95, 50, 0, NULL },
    { S_TEXT, 72, 70, 0, 0, 0, "220-470 Ω" }, { S_WIRE, 95, 50, 115, 50, 0, NULL },
    { S_LED, 115, 50, 150, 50, 0, NULL },     { S_TEXT, 132, 78, 0, 0, 0, "LED" },
    { S_WIRE, 150, 50, 178, 50, 0, NULL },    { S_WIRE, 178, 50, 178, 84, 0, NULL },
    { S_GND, 178, 84, 0, 0, 0, NULL },        { S_TEXT, 178, 108, 0, 0, 0, "GND" },
    { S_END, 0, 0, 0, 0, 0, NULL },
};

static const sk_t SK_LED_3V3[] = {
    { S_DOT,  20, 50, 0, 0, 0, NULL },        { S_TEXT, 20, 30, 0, 0, 0, "3V3" },
    { S_WIRE, 20, 50, 50, 50, 0, NULL },      { S_RES, 50, 50, 95, 50, 0, NULL },
    { S_TEXT, 72, 70, 0, 0, 0, "220-470 Ω" }, { S_WIRE, 95, 50, 115, 50, 0, NULL },
    { S_LED, 115, 50, 150, 50, 0, NULL },     { S_TEXT, 132, 78, 0, 0, 0, "LED" },
    { S_WIRE, 150, 50, 180, 50, 0, NULL },    { S_DOT, 180, 50, 0, 0, 0, NULL },
    { S_TEXT, 180, 30, 0, 0, 0, "$PIN" },
    { S_END, 0, 0, 0, 0, 0, NULL },
};

static const sk_t SK_SERVO[] = {
    { S_DOT,  16, 26, 0, 0, 0, NULL },          { S_TEXT, 16, 8, 0, 0, 0, "$PIN" },
    { S_WIRE, 16, 26, 140, 26, ORANGE, NULL },  { S_TEXT, 100, 36, 0, 0, ORANGE, N_("señal") },
    { S_BOX, 50, 46, 100, 96, 0, NULL },        { S_TEXT, 75, 62, 0, 0, 0, N_("fuente") },
    { S_TEXT, 75, 82, 0, 0, 0, N_("5 V aparte") },
    { S_WIRE, 100, 56, 140, 56, RED, NULL },    { S_TEXT, 108, 46, 0, 0, RED, "+" },
    { S_WIRE, 100, 86, 140, 86, BROWN, NULL },  { S_TEXT, 108, 96, 0, 0, BROWN, "−" },
    { S_DOT,  16, 110, 0, 0, 0, NULL },         { S_TEXT, 30, 100, 0, 0, 0, "GND" },
    { S_WIRE, 16, 110, 120, 110, BROWN, NULL }, { S_WIRE, 120, 110, 120, 86, BROWN, NULL },
    { S_DOT,  120, 86, 0, 0, BROWN, NULL },
    { S_BOX, 140, 14, 192, 98, 0, NULL },       { S_TEXT, 166, 50, 0, 0, 0, N_("servo") },
    { S_TEXT, 166, 74, 0, 0, 0, "SG90" },
    { S_END, 0, 0, 0, 0, 0, NULL },
};

static const sk_t SK_IR[] = {
    { S_DOT,  14, 80, 0, 0, 0, NULL },        { S_TEXT, 22, 62, 0, 0, 0, "$PIN" },
    { S_WIRE, 14, 80, 30, 80, 0, NULL },      { S_RES, 30, 80, 80, 80, 0, NULL },
    { S_TEXT, 55, 98, 0, 0, 0, "1 kΩ" },     { S_WIRE, 80, 80, 104, 80, 0, NULL },
    { S_NPN, 120, 80, 0, 0, 0, NULL },        { S_TEXT, 166, 88, 0, 0, 0, "2N2222" },
    { S_WIRE, 128, 104, 128, 106, 0, NULL },  { S_GND, 128, 106, 0, 0, 0, NULL },
    { S_LED, 128, 30, 128, 56, 0, NULL },     { S_TEXT, 162, 44, 0, 0, 0, N_("LED IR") },
    { S_RES, 128, 6, 128, 30, 0, NULL },      { S_TEXT, 158, 18, 0, 0, 0, "47 Ω" },
    { S_DOT, 128, 4, 0, 0, RED, NULL },       { S_TEXT, 100, 6, 0, 0, RED, "5 V" },
    { S_END, 0, 0, 0, 0, 0, NULL },
};

static const sk_t SK_RC[] = {
    { S_DOT,  16, 40, 0, 0, 0, NULL },        { S_TEXT, 22, 20, 0, 0, 0, "$PIN" },
    { S_WIRE, 16, 40, 40, 40, 0, NULL },      { S_RES, 40, 40, 90, 40, 0, NULL },
    { S_TEXT, 65, 58, 0, 0, 0, "1 kΩ" },     { S_WIRE, 90, 40, 178, 40, 0, NULL },
    { S_DOT, 125, 40, 0, 0, 0, NULL },        { S_DOT, 178, 40, 0, 0, 0x40C8E0, NULL },
    { S_TEXT, 170, 20, 0, 0, 0x40C8E0, N_("salida") },
    { S_WIRE, 125, 40, 125, 62, 0, NULL },    { S_CAP, 125, 62, 125, 86, 0, NULL },
    { S_TEXT, 152, 74, 0, 0, 0, "1 µF" },    { S_WIRE, 125, 86, 125, 94, 0, NULL },
    { S_GND, 125, 94, 0, 0, 0, NULL },
    { S_END, 0, 0, 0, 0, 0, NULL },
};

static const sk_t *const SKETCHES[] = { SK_LED, SK_LED_3V3, SK_SERVO, SK_IR, SK_RC };

typedef struct { float ox, oy, s; } tf_t;

static float X(const tf_t *t, float x) { return t->ox + x * t->s; }
static float Y(const tf_t *t, float y) { return t->oy + y * t->s; }

static void two_terminal(lv_layer_t *L, const tf_t *t, const sk_t *k, lv_color_t c, int32_t w)
{
    float ax = X(t, k->x1), ay = Y(t, k->y1), bx = X(t, k->x2), by = Y(t, k->y2);
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    float ux = dx / len, uy = dy / len, nx = -uy, ny = ux;        /* along, across */
    float s = t->s;
    switch (k->op) {
    case S_RES: {
        float l0 = len * 0.2f, l1 = len * 0.8f, amp = 5.0f * s;
        line(L, ax, ay, ax + ux * l0, ay + uy * l0, c, w);
        line(L, ax + ux * l1, ay + uy * l1, bx, by, c, w);
        const int Z = 6;
        float px = ax + ux * l0, py = ay + uy * l0;
        for (int i = 1; i <= Z; i++) {
            float along = l0 + (l1 - l0) * (float)i / (float)Z;
            float off = i == Z ? 0 : (i % 2 ? amp : -amp);
            float qx = ax + ux * along + nx * off, qy = ay + uy * along + ny * off;
            line(L, px, py, qx, qy, c, w);
            px = qx;
            py = qy;
        }
        break;
    }
    case S_LED: {
        float l0 = len * 0.3f, l1 = len * 0.7f, half = 8.0f * s;
        float bx0 = ax + ux * l0, by0 = ay + uy * l0, tx = ax + ux * l1, ty = ay + uy * l1;
        line(L, ax, ay, bx0, by0, c, w);
        line(L, tx, ty, bx, by, c, w);
        line(L, bx0 + nx * half, by0 + ny * half, bx0 - nx * half, by0 - ny * half, c, w);
        line(L, bx0 + nx * half, by0 + ny * half, tx, ty, c, w);
        line(L, bx0 - nx * half, by0 - ny * half, tx, ty, c, w);
        line(L, tx + nx * half, ty + ny * half, tx - nx * half, ty - ny * half, c, w);
        /* the light going out */
        for (int i = 0; i < 2; i++) {
            float sx = bx0 + ux * (len * 0.12f * (float)i) - nx * (half + 3 * s);
            float sy = by0 + uy * (len * 0.12f * (float)i) - ny * (half + 3 * s);
            float ex = sx - nx * 7 * s + ux * 4 * s, ey = sy - ny * 7 * s + uy * 4 * s;
            line(L, sx, sy, ex, ey, AOS_C_ORANGE, 2);
            line(L, ex, ey, ex + nx * 2.5f * s - ux * 0.5f * s, ey + ny * 2.5f * s - uy * 0.5f * s, AOS_C_ORANGE, 2);
            line(L, ex, ey, ex - ux * 2.5f * s + nx * 0.5f * s, ey - uy * 2.5f * s + ny * 0.5f * s, AOS_C_ORANGE, 2);
        }
        break;
    }
    case S_CAP: {
        float mid = len / 2, gap = 3.0f * s, half = 9.0f * s;
        float p1x = ax + ux * (mid - gap), p1y = ay + uy * (mid - gap);
        float p2x = ax + ux * (mid + gap), p2y = ay + uy * (mid + gap);
        line(L, ax, ay, p1x, p1y, c, w);
        line(L, p2x, p2y, bx, by, c, w);
        line(L, p1x + nx * half, p1y + ny * half, p1x - nx * half, p1y - ny * half, c, w + 1);
        line(L, p2x + nx * half, p2y + ny * half, p2x - nx * half, p2y - ny * half, c, w + 1);
        break;
    }
    default: break;
    }
}

void pw_draw_schematic(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *L = lv_event_get_layer(e);
    int tab = pw_g->wire_tab;
    if (tab < 0 || tab >= (int)(sizeof SKETCHES / sizeof SKETCHES[0])) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float w = (float)lv_area_get_width(&a) - 32, hh = (float)lv_area_get_height(&a) - 32;
    tf_t t;
    t.s = w / 200.0f < hh / 120.0f ? w / 200.0f : hh / 120.0f;
    t.ox = (float)a.x1 + 16 + (w - 200.0f * t.s) / 2;
    t.oy = (float)a.y1 + 16 + (hh - 120.0f * t.s) / 2;
    int32_t lw = t.s >= 3 ? 3 : 2;

    char pin[32] = "GPIO";
    pw_ch_t *h = pw_cur();
    if (h && h->c.gpio >= 0) {
        const aos_io_pin_t *p = aos_io_pin_of_gpio(h->c.gpio);
        snprintf(pin, sizeof pin, _("GPIO%d (pata %d)"), h->c.gpio, p ? p->pin : 0);
    }

    for (const sk_t *k = SKETCHES[tab]; k->op != S_END; k++) {
        lv_color_t c = k->rgb ? lv_color_hex(k->rgb) : AOS_C_TEXT;
        switch (k->op) {
        case S_WIRE: line(L, X(&t, k->x1), Y(&t, k->y1), X(&t, k->x2), Y(&t, k->y2), c, lw); break;
        case S_RES:
        case S_LED:
        case S_CAP: two_terminal(L, &t, k, c, lw); break;
        case S_GND: {
            float x = X(&t, k->x1), y = Y(&t, k->y1);
            for (int i = 0; i < 3; i++) {
                float hw = (10.0f - 3.5f * (float)i) * t.s, yy = y + (float)i * 4.0f * t.s;
                line(L, x - hw, yy, x + hw, yy, c, lw);
            }
            break;
        }
        case S_DOT: disc(L, X(&t, k->x1), Y(&t, k->y1), (int32_t)(2.5f * t.s), c); break;
        case S_TEXT: {
            /* the marked ones are in the catalog; the rest come back as they are */
            const char *s = !strcmp(k->t, "$PIN") ? pin : _(k->t);
            /* near an edge of the drawing, a text grows inwards */
            if (k->x1 <= 30) text(L, s, X(&t, 2), Y(&t, k->y1), aos_font_caption, c, LV_TEXT_ALIGN_LEFT);
            else if (k->x1 >= 170) text(L, s, X(&t, 198), Y(&t, k->y1), aos_font_caption, c, LV_TEXT_ALIGN_RIGHT);
            else text(L, s, X(&t, k->x1), Y(&t, k->y1), aos_font_caption, c, LV_TEXT_ALIGN_CENTER);
            break;
        }
        case S_BOX: {
            float x1 = X(&t, k->x1), y1 = Y(&t, k->y1), x2 = X(&t, k->x2), y2 = Y(&t, k->y2);
            line(L, x1, y1, x2, y1, c, lw);
            line(L, x2, y1, x2, y2, c, lw);
            line(L, x2, y2, x1, y2, c, lw);
            line(L, x1, y2, x1, y1, c, lw);
            break;
        }
        case S_NPN: {
            float x = X(&t, k->x1), y = Y(&t, k->y1), s = t.s;
            ring(L, x + 2 * s, y, (int32_t)(16 * s), 2, AOS_C_DIM);
            line(L, x - 16 * s, y, x - 6 * s, y, c, lw);                 /* base */
            line(L, x - 6 * s, y - 10 * s, x - 6 * s, y + 10 * s, c, lw + 1);
            line(L, x - 6 * s, y - 5 * s, x + 8 * s, y - 14 * s, c, lw);  /* collector */
            line(L, x + 8 * s, y - 14 * s, x + 8 * s, y - 24 * s, c, lw);
            line(L, x - 6 * s, y + 5 * s, x + 8 * s, y + 14 * s, c, lw);  /* emitter */
            line(L, x + 8 * s, y + 14 * s, x + 8 * s, y + 24 * s, c, lw);
            line(L, x + 8 * s, y + 14 * s, x + 2 * s, y + 13 * s, c, lw); /* its arrow */
            line(L, x + 8 * s, y + 14 * s, x + 5 * s, y + 8 * s, c, lw);
            break;
        }
        default: break;
        }
    }
}
