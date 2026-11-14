/*
 * P4OS - Logic: the view, the channels and the decoders drawn in time.
 *
 * One RGB565 canvas in PSRAM: a column of names on the left, a ruler on
 * top, a row per channel shown and a row per decoder on. The pixels are
 * written directly (each column of the plot asks the capture's summaries
 * for the OR and AND of the samples under it: a channel either sits at a
 * level there or moves, drawn as a full bar); the texts go through LVGL's
 * label drawing on the canvas' layer, copied (text_local) because they are
 * made in this function.
 *
 * Fingers: drag to move in time, pinch to zoom, double tap to zoom in, tap
 * to put a cursor (A, then B, then the nearest moves), long press to take
 * them away; dragging on the ruler moves the nearest cursor. A tap on a
 * channel's name opens its settings, on a decoder's its settings, on an
 * annotation the packet it belongs to.
 */
#include "la.h"

#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define RULER   36
#define DEC_H   40
#define TXT_MAX 420

static const uint32_t CH_COL[LA_NCH] = { 0x30D158, 0xFFD60A, 0xFF9F0A, 0xFF6961, 0xBF5AF2, 0x40C8E0, 0x5E9EFF, 0xFF7AA2 };
static const uint32_t K_COL[LA_K_COUNT] = { 0x2F5FB3, 0x9C7A00, 0x22803C, 0x1D6B3A, 0xB3261E, 0x7A3E9D, 0x4A4F57 };

typedef struct {
    int16_t x, y, w, lines;
    uint32_t col;
    const lv_font_t *font;
    char t[64];
} txt_t;

static struct {
    lv_obj_t *cv;
    uint16_t *px;
    int W, H, lbl;
    double off;                 /* the sample at the plot's left edge */
    double spp;                 /* samples per pixel */
    int64_t ca, cb;             /* cursors, -1 none */
    bool dirty;
    uint32_t seen_cap, seen_an;
    /* layout of the last draw */
    int nrows;
    struct { int y, h, ch, dec; } rows[LA_NCH + LA_DEC_MAX];
    /* gesture */
    bool drag_cursor;
    int drag_which;
    txt_t *txt;
    int ntxt;
} V = { .ca = -1, .cb = -1 };

static inline uint16_t c565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}

static void fill(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > V.W) x1 = V.W;
    if (y1 > V.H) y1 = V.H;
    for (int y = y0; y < y1; y++) {
        uint16_t *p = V.px + (size_t)y * V.W;
        for (int x = x0; x < x1; x++) p[x] = c;
    }
}

static inline void vline(int x, int y0, int y1, uint16_t c)
{
    if (x < 0 || x >= V.W) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= V.H) y1 = V.H - 1;
    for (int y = y0; y <= y1; y++) V.px[(size_t)y * V.W + x] = c;
}

static void dashed(int x, int y0, int y1, uint16_t c)
{
    if (x < 0 || x >= V.W) return;
    for (int y = y0; y < y1 && y < V.H; y++)
        if ((y / 4) & 1) V.px[(size_t)y * V.W + x] = c;
}

static void text_n(int x, int y, int w, int lines, uint32_t col, const lv_font_t *f, const char *s)
{
    if (V.ntxt >= TXT_MAX || !V.txt) return;
    txt_t *t = &V.txt[V.ntxt++];
    t->lines = (int16_t)lines;
    t->x = (int16_t)x;
    t->y = (int16_t)y;
    t->w = (int16_t)w;
    t->col = col;
    t->font = f;
    snprintf(t->t, sizeof t->t, "%s", s);
}

static void text(int x, int y, int w, uint32_t col, const lv_font_t *f, const char *s)
{
    text_n(x, y, w, 1, col, f, s);
}

static inline long dround(double v) { return v >= 0 ? (long)(v + 0.5) : -(long)(-v + 0.5); }
static inline double dabs(double v) { return v < 0 ? -v : v; }

static int plot_w(void) { return V.W - V.lbl; }
static double s_at(double x) { return V.off + (x - V.lbl) * V.spp; }
static double x_of(double s) { return V.lbl + (s - V.off) / V.spp; }

static void clamp_view(void)
{
    uint32_t n = LA.cap.n;
    if (!n) {
        V.off = 0;
        V.spp = 1;
        return;
    }
    double maxspp = (double)n / plot_w();
    if (maxspp < 1.0 / 32) maxspp = 1.0 / 32;
    if (V.spp > maxspp) V.spp = maxspp;
    if (V.spp < 1.0 / 32) V.spp = 1.0 / 32;
    double vis = V.spp * plot_w();
    double lo = -vis * 0.05, hi = (double)n - vis * 0.95;
    if (hi < lo) hi = lo;
    if (V.off < lo) V.off = lo;
    if (V.off > hi) V.off = hi;
}

void la_view_dirty(void) { V.dirty = true; }

void la_view_fit(void)
{
    V.spp = LA.cap.n ? (double)LA.cap.n / plot_w() : 1;
    V.off = 0;
    clamp_view();
    V.dirty = true;
}

void la_view_zoom(float f)
{
    double mid = s_at(V.lbl + plot_w() / 2.0);
    V.spp /= f;
    clamp_view();
    V.off = mid - plot_w() / 2.0 * V.spp;
    clamp_view();
    V.dirty = true;
}

void la_view_to_trigger(void)
{
    if (LA.cap.trig < 0) return;
    V.off = LA.cap.trig - plot_w() / 2.0 * V.spp;
    clamp_view();
    V.dirty = true;
}

void la_view_goto(uint32_t s0, uint32_t s1)
{
    double span = (double)(s1 > s0 ? s1 - s0 : 1);
    double want = span * 1.6 / plot_w();
    if (want > V.spp || span / V.spp < 20) V.spp = want;
    V.off = (s0 + s1) / 2.0 - plot_w() / 2.0 * V.spp;
    clamp_view();
    V.dirty = true;
}

void la_view_reset(void)
{
    V.ca = V.cb = -1;
    V.seen_cap = 0xFFFFFFFFu;
    V.dirty = true;
}

bool la_view_cursors(uint32_t *a, uint32_t *b)
{
    if (V.ca < 0 || V.cb < 0) return false;
    *a = (uint32_t)(V.ca < V.cb ? V.ca : V.cb);
    *b = (uint32_t)(V.ca < V.cb ? V.cb : V.ca);
    return true;
}

void la_view_cursors_clear(void)
{
    V.ca = V.cb = -1;
    V.dirty = true;
    la_ui_status();
}

void la_view_visible(uint32_t *a, uint32_t *b)
{
    double s0 = s_at(V.lbl), s1 = s_at(V.W);
    if (s0 < 0) s0 = 0;
    if (s1 > LA.cap.n) s1 = LA.cap.n;
    *a = (uint32_t)s0;
    *b = (uint32_t)s1;
}

/* ---- drawing ---- */

static void layout(void)
{
    V.nrows = 0;
    int nch = 0, ndec = 0;
    for (int i = 0; i < LA_NCH; i++) nch += LA.chshow[i];
    for (int r = 0; r < LA_DEC_MAX; r++) ndec += LA.dec[r].on;
    /* everything has to fit: the decoders' rows give way first, down to
     * 20 px, then the channels' */
    int avail = V.H - RULER - 4;
    int dech = DEC_H;
    if (ndec && nch * 26 + ndec * dech > avail) {
        dech = (avail - nch * 26) / ndec;
        if (dech < 20) dech = 20;
    }
    int chh = nch ? (avail - ndec * dech) / nch : 40;
    if (chh > 90) chh = 90;
    if (chh < 16) chh = 16;
    int y = RULER + 2;
    for (int i = 0; i < LA_NCH; i++) {
        if (!LA.chshow[i]) continue;
        V.rows[V.nrows].y = y;
        V.rows[V.nrows].h = chh;
        V.rows[V.nrows].ch = i;
        V.rows[V.nrows].dec = -1;
        V.nrows++;
        y += chh;
    }
    for (int r = 0; r < LA_DEC_MAX; r++) {
        if (!LA.dec[r].on) continue;
        V.rows[V.nrows].y = y;
        V.rows[V.nrows].h = dech;
        V.rows[V.nrows].ch = -1;
        V.rows[V.nrows].dec = r;
        V.nrows++;
        y += dech;
    }
}

static void draw_ruler(const la_cap_t *c)
{
    fill(0, 0, V.W, RULER, c565(0x15191F));
    if (!c->rate) return;
    const double rate = c->rate;
    const double t0 = c->trig >= 0 ? c->trig : 0;
    float stepf = (float)(120.0 * V.spp / rate);    /* seconds per ~120 px */
    float p = powf(10.0f, floorf(log10f(stepf)));
    float m = stepf / p;
    double step = (double)((m < 1.5f ? 1 : m < 3.5f ? 2 : m < 7.5f ? 5 : 10) * p);
    double ta = (s_at(V.lbl) - t0) / rate, tb = (s_at(V.W) - t0) / rate;
    double first = (double)ceilf((float)(ta / step)) * step;
    uint16_t grid = c565(0x1E242C), tick = c565(0x6E7781);
    for (int k = 0; k < 40; k++) {
        double t = first + k * step;
        if (t > tb) break;
        int x = (int)dround(x_of(t * rate + t0));
        if (x < V.lbl || x >= V.W) continue;
        vline(x, RULER - 8, RULER - 1, tick);
        for (int y = RULER; y < V.H; y += 3) V.px[(size_t)y * V.W + x] = grid;
        char s[24];
        la_fmt_time((float)(dabs(t) < step * 1e-3 ? 0 : t), s, sizeof s);
        text(x + 4, 6, 110, 0x8E8E93, aos_font_tiny, s);
    }
}

static void draw_channels(const la_cap_t *c)
{
    int last_lvl[LA_NCH];
    for (int i = 0; i < LA_NCH; i++) last_lvl[i] = -1;
    uint32_t ia_prev = 0xFFFFFFFFu;
    uint8_t o = 0, a = 0, lastv = 0;
    for (int x = V.lbl; x < V.W; x++) {
        double sa = s_at(x), sb = s_at(x + 1);
        if (sb <= 0) continue;
        if (sa >= c->n) break;
        uint32_t ia = sa < 0 ? 0 : (uint32_t)sa, ib = (uint32_t)sb;
        if (ib > c->n) ib = c->n;
        if (ib <= ia) ib = ia + 1;
        if (ia != ia_prev) {
            if (ib - ia == 1) o = a = c->buf[ia];
            else la_cap_range(c, ia, ib, &o, &a);
            lastv = c->buf[ib - 1];
            ia_prev = ia;
        }
        for (int r = 0; r < V.nrows; r++) {
            int ch = V.rows[r].ch;
            if (ch < 0) continue;
            int yh = V.rows[r].y + 6, yl = V.rows[r].y + V.rows[r].h - 6;
            uint16_t col = c565(CH_COL[ch]);
            int bo = (o >> ch) & 1, ba = (a >> ch) & 1;
            if (bo == ba) {
                int y = bo ? yh : yl;
                if (last_lvl[ch] >= 0 && last_lvl[ch] != bo) vline(x, yh, yl, col);
                V.px[(size_t)y * V.W + x] = col;
                V.px[(size_t)(y + (bo ? 1 : -1)) * V.W + x] = col;
                last_lvl[ch] = bo;
            } else {
                vline(x, yh, yl, col);
                last_lvl[ch] = (lastv >> ch) & 1;
            }
        }
    }
}

static void draw_decoders(void)
{
    double s0 = s_at(V.lbl), s1 = s_at(V.W);
    if (s0 < 0) s0 = 0;
    for (int r = 0; r < V.nrows; r++) {
        int d = V.rows[r].dec;
        if (d < 0 || !LA.an.row_n[d]) continue;
        int y0 = V.rows[r].y + 4, y1 = V.rows[r].y + V.rows[r].h - 4;
        uint32_t i = la_ann_first(d, (uint32_t)s0);
        uint32_t end = LA.an.row_first[d] + LA.an.row_n[d];
        int last_x = -10;
        for (; i < end; i++) {
            const la_ann_t *an = &LA.an.ann[i];
            if (an->s0 > s1) break;
            int x0 = (int)x_of(an->s0), x1 = (int)x_of(an->s1);
            if (x0 < V.lbl) x0 = V.lbl;
            if (x1 > V.W) x1 = V.W;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 <= last_x) continue;     /* too dense to tell apart here */
            if (x0 <= last_x) x0 = last_x + 1;
            if (x1 <= x0) x1 = x0 + 1;
            uint16_t col = c565(K_COL[an->kind < LA_K_COUNT ? an->kind : 0]);
            fill(x0, y0, x1 - (x1 - x0 > 3 ? 1 : 0), y1, col);
            last_x = x1;
            int w = x1 - x0 - 6;
            if (w > 14) {
                lv_point_t sz;
                lv_text_get_size(&sz, an->t, aos_font_tiny, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                int tw = sz.x;
                if (tw <= w) text(x0 + (x1 - x0 - tw) / 2, y0 + (y1 - y0 - 18) / 2, tw + 2, 0xFFFFFF, aos_font_tiny, an->t);
                else if (w > 24) text(x0 + 3, y0 + (y1 - y0 - 18) / 2, w, 0xFFFFFF, aos_font_tiny, an->t);
            }
        }
    }
}

static void draw_labels(void)
{
    fill(0, RULER, V.lbl, V.H, c565(0x14181E));
    for (int r = 0; r < V.nrows; r++) {
        int y = V.rows[r].y, h = V.rows[r].h;
        fill(V.lbl, y + h - 1, V.W, y + h, c565(0x1A1F26));
        if (V.rows[r].ch >= 0) {
            int ch = V.rows[r].ch;
            fill(0, y + 3, 6, y + h - 3, c565(CH_COL[ch]));
            char s[24];
            snprintf(s, sizeof s, "%s", LA.chname[ch][0] ? LA.chname[ch] : "");
            if (!s[0]) snprintf(s, sizeof s, "D%d", ch);
            text(14, y + (h - 26) / 2, V.lbl - 18, 0xFFFFFF, aos_font_caption, s);
            /* the FX2 boxes print CH1..CH8 on their case for D0..D7 */
            char d[32];
            bool fx2 = LA.ndevs && LA.devs[LA.dev].drv == LA_DRV_FX2;
            if (fx2) snprintf(d, sizeof d, "D%d · CH%d", ch, ch + 1);
            else snprintf(d, sizeof d, "D%d", ch);
            if (h >= 50 && (LA.chname[ch][0] || fx2)) text(14, y + h / 2 + 6, V.lbl - 18, 0x6E7781, aos_font_tiny, d);
        } else {
            int dn = V.rows[r].dec;
            fill(0, y + 3, 6, y + h - 3, c565(0x8E8E93));
            text(14, y + (h - 22) / 2, V.lbl - 18, 0xC7C7CC, aos_font_tiny, la_dec_name(LA.dec[dn].type));
        }
    }
}

static void draw_cursors(const la_cap_t *c)
{
    if (c->trig >= 0) {
        int x = (int)x_of(c->trig);
        if (x >= V.lbl && x < V.W) {
            dashed(x, RULER, V.H, c565(0xFF9F0A));
            fill(x - 5, RULER - 8, x + 6, RULER, c565(0xFF9F0A));
        }
    }
    const int64_t cur[2] = { V.ca, V.cb };
    static const uint32_t col[2] = { 0x40C8E0, 0xFF375F };
    static const char *const nm[2] = { "A", "B" };
    for (int i = 0; i < 2; i++) {
        if (cur[i] < 0) continue;
        int x = (int)x_of((double)cur[i]);
        if (x < V.lbl || x >= V.W) continue;
        vline(x, RULER - 2, V.H - 1, c565(col[i]));
        fill(x - 10, 0, x + 11, 22, c565(col[i]));
        text(x - 5, 0, 20, 0x000000, aos_font_tiny, nm[i]);
    }
}

static void flush_text(void)
{
    if (!V.ntxt) return;
    lv_layer_t layer;
    lv_canvas_init_layer(V.cv, &layer);
    for (int i = 0; i < V.ntxt; i++) {
        txt_t *t = &V.txt[i];
        if (t->lines == 1) {
            /* one line: cut it to its width rather than let it wrap */
            size_t l = strlen(t->t);
            lv_point_t sz;
            lv_text_get_size(&sz, t->t, t->font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            while (l > 1 && sz.x > t->w) {
                l--;
                while (l > 1 && ((unsigned char)t->t[l] & 0xC0) == 0x80) l--;   /* not inside a UTF-8 character */
                t->t[l] = 0;
                lv_text_get_size(&sz, t->t, t->font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            }
        }
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.text = t->t;
        d.text_local = 1;
        d.font = t->font;
        d.color = lv_color_hex(t->col);
        lv_area_t a = { t->x, t->y, t->x + t->w, t->y + t->lines * lv_font_get_line_height(t->font) };
        lv_draw_label(&layer, &d, &a);
    }
    lv_canvas_finish_layer(V.cv, &layer);
    V.ntxt = 0;
}

static void draw(void)
{
    if (!V.px) return;
    V.ntxt = 0;
    fill(0, 0, V.W, V.H, c565(0x0B0E12));
    la_lock();
    layout();
    const la_cap_t *c = &LA.cap;
    if (LA.cap_ok && c->n && c->buf) {
        draw_ruler(c);
        draw_labels();
        draw_channels(c);
        if (LA.an_ok) draw_decoders();
        draw_cursors(c);
    } else {
        draw_labels();
        fill(0, 0, V.W, RULER, c565(0x15191F));
        const char *m = LA.busy ? _("Capturando...") : _("Sin captura: elegí el analizador y tocá Capturar");
        text_n(V.lbl + 24, V.H / 2 - 40, V.W - V.lbl - 48, 3, 0x8E8E93, aos_font_body, m);
    }
    la_unlock();
    flush_text();
    lv_obj_invalidate(V.cv);
}

void la_view_tick(void)
{
    if (!V.cv) return;
    la_lock();
    uint32_t cs = LA.cap.seq, as = LA.an.seq;
    bool ok = LA.cap_ok;
    la_unlock();
    if (ok && cs != V.seen_cap) {
        V.seen_cap = cs;
        V.ca = V.cb = -1;
        la_view_fit();
    }
    if (as != V.seen_an) {
        V.seen_an = as;
        V.dirty = true;
    }
    if (V.dirty) {
        V.dirty = false;
        draw();
    }
}

/* ---- fingers ---- */

static int row_at(int y)
{
    for (int r = 0; r < V.nrows; r++)
        if (y >= V.rows[r].y && y < V.rows[r].y + V.rows[r].h) return r;
    return -1;
}

static void put_cursor(int x)
{
    int64_t s = (int64_t)dround(s_at(x));
    if (s < 0) s = 0;
    if (s >= LA.cap.n) s = LA.cap.n ? LA.cap.n - 1 : 0;
    if (V.ca < 0) V.ca = s;
    else if (V.cb < 0) V.cb = s;
    else if (dabs((double)(V.ca - s)) < dabs((double)(V.cb - s))) V.ca = s;
    else V.cb = s;
    V.dirty = true;
    la_ui_status();
}

static void on_gesture(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    lv_area_t co;
    lv_obj_get_coords(V.cv, &co);
    float x = ev->x - co.x1, y = ev->y - co.y1;
    switch (ev->type) {
    case AOS_GESTURE_DRAG_BEGIN:
        V.drag_cursor = y < RULER && (V.ca >= 0 || V.cb >= 0);
        if (V.drag_cursor) {
            double s = s_at(x);
            V.drag_which = V.cb < 0 || (V.ca >= 0 && dabs((double)V.ca - s) < dabs((double)V.cb - s)) ? 0 : 1;
        }
        break;
    case AOS_GESTURE_DRAG:
        if (V.drag_cursor) {
            int64_t s = (int64_t)dround(s_at(x));
            if (s < 0) s = 0;
            if (s >= LA.cap.n) s = LA.cap.n - 1;
            if (V.drag_which == 0) V.ca = s;
            else V.cb = s;
            la_ui_status();
        } else {
            V.off -= ev->dx * V.spp;
            clamp_view();
        }
        V.dirty = true;
        break;
    case AOS_GESTURE_PINCH: {
        if (ev->scale <= 0) break;
        double at = s_at(x);
        V.spp /= ev->scale;
        clamp_view();
        V.off = at - (x - V.lbl) * V.spp - ev->dx * V.spp;
        clamp_view();
        V.dirty = true;
        break;
    }
    case AOS_GESTURE_DOUBLE_TAP: {
        double at = s_at(x);
        V.spp /= 2.5;
        clamp_view();
        V.off = at - (x - V.lbl) * V.spp;
        clamp_view();
        V.dirty = true;
        break;
    }
    case AOS_GESTURE_TAP: {
        int r = row_at((int)y);
        if (x < V.lbl) {
            if (r >= 0 && V.rows[r].ch >= 0) la_ui_channel_sheet(V.rows[r].ch);
            else if (r >= 0) la_ui_decoder_sheet(V.rows[r].dec);
            break;
        }
        if (!LA.cap.n) break;
        if (r >= 0 && V.rows[r].dec >= 0 && LA.an_ok) {
            la_ui_packet_toast((uint32_t)(s_at(x) < 0 ? 0 : s_at(x)), V.rows[r].dec);
            break;
        }
        put_cursor((int)x);
        break;
    }
    case AOS_GESTURE_LONG_PRESS:
        la_view_cursors_clear();
        break;
    default:
        break;
    }
}

static void delete_cb(lv_event_t *e)
{
    (void)e;
    if (V.px) la_big_free(V.px);
    if (V.txt) la_big_free(V.txt);
    V.px = NULL;
    V.txt = NULL;
    V.cv = NULL;
}

void la_view_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    if (w > 1280) w = 1280;
    V.W = (int)w;
    V.H = (int)h;
    V.lbl = w > 900 ? 132 : 112;
    V.px = la_big_alloc((size_t)w * h * 2);
    V.txt = la_big_alloc(TXT_MAX * sizeof(txt_t));
    V.cv = lv_canvas_create(parent);
    if (!V.px || !V.txt) {
        lv_obj_delete(V.cv);
        V.cv = NULL;
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, _("No hay memoria para la vista"));
        return;
    }
    lv_canvas_set_buffer(V.cv, V.px, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_event_cb(V.cv, delete_cb, LV_EVENT_DELETE, NULL);
    aos_gesture_attach(V.cv, 0, on_gesture, NULL);
    V.seen_cap = 0xFFFFFFFFu;
    V.seen_an = 0xFFFFFFFFu;
    V.dirty = true;
    clamp_view();
}
