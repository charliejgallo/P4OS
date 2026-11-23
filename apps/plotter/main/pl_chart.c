/*
 * P4OS - Monitor serie: the chart.
 *
 * Drawn by hand in the object's draw event, as Bolsa does: lv_chart joins
 * no points across a hole (the trap Datalog measured) and has one value per
 * point, where here a pixel can hold a thousand samples. The timer turns the
 * window into columns (pl_data_columns: low, high and last of each series
 * per pixel) and asks for a redraw; the draw event only turns the columns
 * into lines, so drawing in several pieces costs nothing more.
 *
 * Each series is one polyline that goes down and up through every column's
 * low and high: a spike in a crowded column shows, and a slow signal is a
 * plain line. Two samples further apart than five times the series' usual
 * pace are not joined (a device that stopped and came back).
 *
 * Fingers (aos_gesture): one finger reads the point under it, with the time
 * and every series' value, and the reading stays four seconds after the
 * finger lifts; a tap on the legend shows that series big. Two fingers
 * change the window (pinching, about the point between them) and drag it
 * back in time, which pauses the view: the data keeps coming underneath.
 *
 * XY: one series against another over the window, from the rows.
 */
#include "pl.h"

#include <time.h>

#define HOLD_MS   4000
#define AXIS_W    118
#define TIME_H    34
#define LEG_ROW   38
#define XY_MAX    3000
#define WIN_MIN   2000u
#define WIN_MAX   (10u * 3600u * 1000u)

static struct {
    lv_obj_t *obj;
    pl_cols_t cols;
    int32_t px, py, pw, ph;         /* the plot, in the object's own coordinates */
    float lo, hi;                   /* the value axis */
    float step;
    int   dec;
    /* XY */
    float *xy;
    int nxy;
    float xlo, xhi;
    /* the legend's boxes, for taps */
    lv_area_t leg[PL_MAXS];
    int nleg;
    int32_t leg_h;
    /* the reading */
    bool cursor;
    int32_t cx;                     /* the plot's column */
    uint32_t hold_until;
    bool dragging;
    /* pinch */
    bool pinching;
    float pan_acc;
    char msg[96];
    uint32_t built_at;
    bool want;
} C;

uint32_t pl_now(void) { return (uint32_t)aos_hal_uptime_ms(); }

/* ---- small drawing helpers ---- */

static void label_at(lv_layer_t *layer, const char *txt, const lv_font_t *f, lv_color_t col,
                     int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = col;
    d.text = txt;
    d.text_local = 1;               /* built on the stack (HANDOFF-APPS 8) */
    d.align = al;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_label(layer, &d, &a);
}

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t col, lv_opa_t opa,
                 int32_t radius)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t col, int32_t w,
                 lv_opa_t opa, int32_t dash)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = col;
    d.width = w;
    d.opa = opa;
    d.dash_width = dash;
    d.dash_gap = dash;
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(layer, &d);
}

static int32_t text_w(const char *s, const lv_font_t *f)
{
    int32_t w = 0;
    const char *p = s;
    while (*p) {
        uint32_t cp;
        unsigned char c = (unsigned char)*p;
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int i = 1; i < n && p[i]; i++) cp = (cp << 6) | ((unsigned char)p[i] & 0x3F);
        p += n;
        w += lv_font_get_glyph_width(f, cp, 0);
    }
    return w;
}

/* ---- the axes ---- */

static float nice_step(float range, int ticks)
{
    if (range <= 0) return 1;
    float raw = range / (float)ticks;
    float mag = powf(10.0f, floorf(log10f(raw)));
    float n = raw / mag;
    float s = n < 1.5f ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10;
    return s * mag;
}

static int step_dec(float step)
{
    if (step >= 1) return 0;
    int d = (int)ceilf(-log10f(step) - 0.001f);
    return d > 6 ? 6 : d;
}

static void fmt_ago(char *out, size_t cap, uint32_t s)
{
    if (!s) snprintf(out, cap, "0");
    else if (s % 3600 == 0) snprintf(out, cap, "-%u h", (unsigned)(s / 3600));
    else if (s % 60 == 0) snprintf(out, cap, "-%u min", (unsigned)(s / 60));
    else if (s >= 60) snprintf(out, cap, "-%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    else snprintf(out, cap, "-%u s", (unsigned)s);
}

static void fmt_clock(char *out, size_t cap, uint32_t t)
{
    uint32_t now = pl_now();
    if (aos_hal_time_is_valid()) {
        time_t w = time(NULL) - (time_t)((now - t) / 1000);
        struct tm tm;
        localtime_r(&w, &tm);
        snprintf(out, cap, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        uint32_t ago = (now - t) / 100;
        snprintf(out, cap, "-%u.%u s", (unsigned)(ago / 10), (unsigned)(ago % 10));
    }
}

static void win_range(uint32_t *t0, uint32_t *t1)
{
    uint32_t now = pl_now();
    uint32_t end = PLA.paused ? PLA.end_t : now;
    if (end > now) end = now;
    *t1 = end;
    *t0 = end > PLA.win_ms ? end - PLA.win_ms : 0;
}

/* ---- the legend: chips that wrap, measured in the timer ---- */

static void legend_text(int s, char *out, size_t cap)
{
    const pl_series_t *se = &PLD.s[s];
    char v[24];
    float last = C.cols.ncols ? C.cols.slast[s] : NAN;
    if (PL_ISNAN(last) && se->has && !PLA.paused) last = se->last;
    pl_fmt(v, sizeof v, last, se->dec);
    snprintf(out, cap, "%s  %s%s%s", se->name, v, se->unit[0] ? " " : "", se->unit);
}

static void legend_layout(int32_t w)
{
    const lv_font_t *f = aos_font_caption;
    int32_t x = 0, y = 0;
    C.nleg = 0;
    for (int s = 0; s < PLD.ns; s++) {
        char t[64];
        legend_text(s, t, sizeof t);
        int32_t iw = 34 + text_w(t, f) + 18;
        if (iw > w) iw = w;
        if (x && x + iw > w) { x = 0; y += LEG_ROW; }
        C.leg[s] = (lv_area_t){ x, y, x + iw - 1, y + LEG_ROW - 6 };
        x += iw + 8;
        C.nleg = s + 1;
    }
    C.leg_h = PLD.ns ? y + LEG_ROW + 4 : 0;
}

/* ---- the timer's part ---- */

static void xy_cb(const pl_row_t *r, void *ud)
{
    float *carry = ud;
    if (!PL_ISNAN(r->v[PLS.xs])) carry[0] = r->v[PLS.xs];
    if (!PL_ISNAN(r->v[PLS.ys])) carry[1] = r->v[PLS.ys];
    if (PL_ISNAN(carry[0]) || PL_ISNAN(carry[1]) || C.nxy >= XY_MAX) return;
    C.xy[2 * C.nxy] = carry[0];
    C.xy[2 * C.nxy + 1] = carry[1];
    C.nxy++;
}

static void autorange(float *lo, float *hi)
{
    if (PL_ISNAN(*lo)) { *lo = 0; *hi = 1; return; }
    if (*hi - *lo < 1e-9f) {
        float m = fabsf(*lo) * 0.05f;
        if (m < 1e-6f) m = 1;
        *lo -= m;
        *hi += m;
    }
    float pad = (*hi - *lo) * 0.08f;
    *lo -= pad;
    *hi += pad;
}

static void build(void)
{
    if (!C.obj) return;
    lv_area_t a;
    lv_obj_get_coords(C.obj, &a);
    int32_t W = lv_area_get_width(&a), H = lv_area_get_height(&a);
    if (W < 100 || H < 100) return;
    legend_layout(W - 24);
    C.px = 14;
    C.pw = W - AXIS_W - C.px;
    C.py = 12 + C.leg_h;
    C.ph = H - C.py - TIME_H - 8;
    if (C.pw < 50 || C.ph < 50) return;
    if (!pl_cols_alloc(&C.cols, C.pw)) return;

    uint32_t t0, t1;
    win_range(&t0, &t1);
    pl_data_columns(&C.cols, t0, t1, C.pw);
    /* the legend's values come from the columns: measure again */
    legend_layout(W - 24);
    int32_t py = 12 + C.leg_h;
    if (py != C.py) {
        C.py = py;
        C.ph = H - C.py - TIME_H - 8;
    }

    if (PLS.mode == PL_MODE_XY) {
        C.nxy = 0;
        if (!C.xy) C.xy = pl_ps_alloc(sizeof(float) * 2 * XY_MAX);
        float carry[2] = { NAN, NAN };
        if (C.xy && PLS.xs < PLD.ns && PLS.ys < PLD.ns) pl_data_rows(t0, t1, XY_MAX, xy_cb, carry);
        float xl = NAN, xh = NAN, yl = NAN, yh = NAN;
        for (int i = 0; i < C.nxy; i++) {
            float x = C.xy[2 * i], y = C.xy[2 * i + 1];
            if (PL_ISNAN(xl) || x < xl) xl = x;
            if (PL_ISNAN(xh) || x > xh) xh = x;
            if (PL_ISNAN(yl) || y < yl) yl = y;
            if (PL_ISNAN(yh) || y > yh) yh = y;
        }
        autorange(&xl, &xh);
        if (PLS.yfix && PLS.ymax > PLS.ymin) { yl = PLS.ymin; yh = PLS.ymax; }
        else autorange(&yl, &yh);
        C.xlo = xl;
        C.xhi = xh;
        C.lo = yl;
        C.hi = yh;
    } else if (PLS.yfix && PLS.ymax > PLS.ymin) {
        C.lo = PLS.ymin;
        C.hi = PLS.ymax;
    } else {
        float lo = NAN, hi = NAN;
        for (int s = 0; s < PLD.ns; s++) {
            if (PLD.s[s].hidden || !C.cols.sn[s]) continue;
            if (PL_ISNAN(lo) || C.cols.smin[s] < lo) lo = C.cols.smin[s];
            if (PL_ISNAN(hi) || C.cols.smax[s] > hi) hi = C.cols.smax[s];
        }
        autorange(&lo, &hi);
        C.lo = lo;
        C.hi = hi;
    }
    C.step = nice_step(C.hi - C.lo, C.ph > 500 ? 6 : 4);
    C.dec = step_dec(C.step);

    /* what to say when there is nothing to draw */
    C.msg[0] = 0;
    aos_serial_stat_t st;
    aos_serial_stat(PLS.ch, &st);
    if (!st.running && !PLD.rows) pl_scpy(C.msg, sizeof C.msg, _("Elegí el puerto y tocá Conectar"));
    else if (!PLD.rows && PLA.lines) pl_scpy(C.msg, sizeof C.msg, _("Llegan líneas pero sin números: mirá Formato"));
    else if (!PLD.rows) pl_scpy(C.msg, sizeof C.msg, _("Esperando datos…"));
    else if (PLS.mode == PL_MODE_XY && C.nxy < 2) pl_scpy(C.msg, sizeof C.msg, _("Hacen falta dos series con datos"));
    C.built_at = pl_now();
    lv_obj_invalidate(C.obj);
}

/* ---- the draw event ---- */

static int32_t ys(float v)
{
    float f = (C.hi - v) / (C.hi - C.lo);
    if (f < -0.5f) f = -0.5f;
    if (f > 1.5f) f = 1.5f;
    return C.py + (int32_t)(f * (float)C.ph);
}

static void draw_axes(lv_layer_t *L, int32_t ox, int32_t oy)
{
    lv_color_t grid = lv_color_hex(0x2A3038), dim = lv_color_hex(0x8E8E93);
    char b[32];
    /* values */
    float first = ceilf(C.lo / C.step) * C.step;
    for (float v = first; v <= C.hi + C.step * 0.001f; v += C.step) {
        int32_t y = oy + ys(v);
        line(L, ox + C.px, y, ox + C.px + C.pw, y, grid, 1, LV_OPA_COVER, 0);
        pl_fmt(b, sizeof b, fabsf(v) < C.step * 1e-3f ? 0 : v, C.dec);
        label_at(L, b, aos_font_caption, dim, ox + C.px + C.pw + 8, y - 12, ox + C.px + C.pw + AXIS_W - 4, y + 14,
                 LV_TEXT_ALIGN_LEFT);
    }
    int32_t by = oy + C.py + C.ph;
    if (PLS.mode == PL_MODE_XY) {
        float xs = nice_step(C.xhi - C.xlo, C.pw > 800 ? 6 : 4);
        int xd = step_dec(xs);
        for (float v = ceilf(C.xlo / xs) * xs; v <= C.xhi; v += xs) {
            int32_t x = ox + C.px + (int32_t)((v - C.xlo) / (C.xhi - C.xlo) * (float)C.pw);
            line(L, x, oy + C.py, x, by, grid, 1, LV_OPA_COVER, 0);
            pl_fmt(b, sizeof b, fabsf(v) < xs * 1e-3f ? 0 : v, xd);
            label_at(L, b, aos_font_caption, dim, x - 70, by + 6, x + 70, by + 32, LV_TEXT_ALIGN_CENTER);
        }
        return;
    }
    /* time: steps of 1 s to 2 h, about one per 150 px */
    static const uint32_t STEPS[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200 };
    uint32_t span = (C.cols.t1 - C.cols.t0) / 1000;
    uint32_t step = STEPS[13];
    for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++)
        if ((float)STEPS[i] * (float)C.pw / (float)(span ? span : 1) >= 150) { step = STEPS[i]; break; }
    uint32_t now = pl_now();
    uint32_t ago_end = (now - C.cols.t1) / 1000;
    /* ticks at whole multiples of the step back from now */
    uint32_t k0 = (ago_end + step - 1) / step;
    for (uint32_t k = k0;; k++) {
        uint32_t ago = k * step;
        uint32_t t = now - ago * 1000;
        if (ago * 1000 > now || t < C.cols.t0) break;
        int32_t x = ox + C.px + (int32_t)((float)(t - C.cols.t0) * (float)C.pw / (float)(C.cols.t1 - C.cols.t0));
        line(L, x, oy + C.py, x, by, grid, 1, LV_OPA_COVER, 0);
        fmt_ago(b, sizeof b, ago);
        /* the first and the last stay inside the card */
        int32_t l = x - 70, r = x + 70;
        lv_text_align_t al = LV_TEXT_ALIGN_CENTER;
        if (l < ox + 8) { l = ox + 8; al = LV_TEXT_ALIGN_LEFT; }
        label_at(L, b, aos_font_caption, dim, l, by + 6, r, by + 32, al);
        if (k > k0 + 40) break;
    }
}

static void draw_series(lv_layer_t *L, int32_t ox, int32_t oy, int s)
{
    const pl_series_t *se = &PLD.s[s];
    const pl_cols_t *c = &C.cols;
    lv_point_precise_t *pts = lv_malloc(sizeof *pts * (size_t)(c->ncols * 3 + 2));
    if (!pts) return;
    float gap = se->dt > 0 ? se->dt * 5 : 0;
    if (c->buckets && gap < 3000) gap = 3000;
    float colms = (float)(c->t1 - c->t0) / (float)c->ncols;
    if (gap < colms * 2.5f) gap = colms * 2.5f;
    lv_color_t col = lv_color_hex(se->col);
    int n = 0;
    int32_t prev_y = 0;
    uint32_t prev_t = 0;
    int dots = 0;
    for (int k = 0; k < c->ncols; k++) {
        int i = s * c->cap + k;
        float last = c->last[i];
        if (PL_ISNAN(last)) continue;
        int32_t x = ox + C.px + k;
        if (n && gap > 0 && (float)(c->lt[i] - prev_t) > gap) {
            if (n >= 2) {
                lv_draw_line_dsc_t d;
                lv_draw_line_dsc_init(&d);
                d.color = col;
                d.width = 3;
                d.round_start = d.round_end = 1;
                d.points = pts;
                d.point_cnt = (uint32_t)n;
                lv_draw_line(L, &d);
            } else {
                rect(L, (int32_t)pts[0].x - 3, (int32_t)pts[0].y - 3, (int32_t)pts[0].x + 3, (int32_t)pts[0].y + 3, col,
                     LV_OPA_COVER, 3);
            }
            n = 0;
        }
        int32_t ymn = oy + ys(c->mn[i]), ymx = oy + ys(c->mx[i]), yl = oy + ys(last);
        if (ymn == ymx) {
            pts[n].x = x; pts[n].y = yl; n++;
        } else {
            /* through the far end first, so the line ends at the last value */
            bool down_first = n ? abs(prev_y - ymx) < abs(prev_y - ymn) : true;
            pts[n].x = x; pts[n].y = down_first ? ymx : ymn; n++;
            pts[n].x = x; pts[n].y = down_first ? ymn : ymx; n++;
            if (pts[n - 1].y != yl) { pts[n].x = x; pts[n].y = yl; n++; }
        }
        prev_y = yl;
        prev_t = c->lt[i];
        dots++;
    }
    if (n >= 2) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = col;
        d.width = 3;
        d.round_start = d.round_end = 1;
        d.points = pts;
        d.point_cnt = (uint32_t)n;
        lv_draw_line(L, &d);
    } else if (n == 1) {
        rect(L, (int32_t)pts[0].x - 4, (int32_t)pts[0].y - 4, (int32_t)pts[0].x + 4, (int32_t)pts[0].y + 4, col,
             LV_OPA_COVER, 4);
    }
    /* few samples: a dot on each, so a slow sensor is readable */
    if (dots <= 24) {
        for (int k = 0; k < c->ncols; k++) {
            int i = s * c->cap + k;
            if (PL_ISNAN(c->last[i])) continue;
            int32_t x = ox + C.px + k, y = oy + ys(c->last[i]);
            rect(L, x - 4, y - 4, x + 4, y + 4, col, LV_OPA_COVER, 4);
        }
    }
    lv_free(pts);
}

static void draw_xy(lv_layer_t *L, int32_t ox, int32_t oy)
{
    if (C.nxy < 1) return;
    lv_point_precise_t *pts = lv_malloc(sizeof *pts * (size_t)C.nxy);
    if (!pts) return;
    float xr = C.xhi - C.xlo;
    for (int i = 0; i < C.nxy; i++) {
        pts[i].x = ox + C.px + (int32_t)((C.xy[2 * i] - C.xlo) / xr * (float)C.pw);
        pts[i].y = oy + ys(C.xy[2 * i + 1]);
    }
    lv_color_t col = lv_color_hex(PLD.s[PLS.ys].col);
    if (C.nxy >= 2) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = col;
        d.width = 2;
        d.opa = LV_OPA_70;
        d.points = pts;
        d.point_cnt = (uint32_t)C.nxy;
        lv_draw_line(L, &d);
    }
    int32_t x = (int32_t)pts[C.nxy - 1].x, y = (int32_t)pts[C.nxy - 1].y;
    rect(L, x - 9, y - 9, x + 9, y + 9, lv_color_white(), LV_OPA_COVER, 9);
    rect(L, x - 6, y - 6, x + 6, y + 6, col, LV_OPA_COVER, 6);
    lv_free(pts);
    char b[64];
    snprintf(b, sizeof b, "X: %s   Y: %s", PLD.s[PLS.xs].name, PLD.s[PLS.ys].name);
    label_at(L, b, aos_font_caption, lv_color_hex(0x8E8E93), ox + C.px + 8, oy + C.py + 6, ox + C.px + C.pw - 8,
             oy + C.py + 32, LV_TEXT_ALIGN_RIGHT);
}

static void draw_alarms(lv_layer_t *L, int32_t ox, int32_t oy)
{
    for (int s = 0; s < PLD.ns; s++) {
        const pl_series_t *se = &PLD.s[s];
        if (se->hidden) continue;
        for (int k = 0; k < 2; k++) {
            if (!(k ? se->hi_on : se->lo_on)) continue;
            float v = k ? se->hi : se->lo;
            if (v < C.lo || v > C.hi) continue;
            int32_t y = oy + ys(v);
            line(L, ox + C.px, y, ox + C.px + C.pw, y, lv_color_hex(se->col), 2, LV_OPA_60, 10);
        }
    }
}

static void draw_cursor(lv_layer_t *L, int32_t ox, int32_t oy)
{
    const pl_cols_t *c = &C.cols;
    if (!C.cursor || !c->ncols) return;
    int32_t k = C.cx;
    if (k < 0) k = 0;
    if (k >= c->ncols) k = c->ncols - 1;
    int32_t x = ox + C.px + k;
    line(L, x, oy + C.py, x, oy + C.py + C.ph, lv_color_white(), 2, LV_OPA_70, 0);
    /* each series: the nearest column with something, eight pixels either way */
    char rows[PL_MAXS + 1][128];
    lv_color_t cols[PL_MAXS + 1];
    int nr = 1;
    uint32_t t = c->t0 + (uint32_t)((float)k * (float)(c->t1 - c->t0) / (float)c->ncols);
    for (int s = 0; s < PLD.ns; s++) {
        if (PLD.s[s].hidden) continue;
        int best = -1;
        for (int d = 0; d <= 8 && best < 0; d++) {
            if (k - d >= 0 && !PL_ISNAN(c->last[s * c->cap + k - d])) best = k - d;
            else if (k + d < c->ncols && !PL_ISNAN(c->last[s * c->cap + k + d])) best = k + d;
        }
        if (best < 0) continue;
        int i = s * c->cap + best;
        char v[24];
        pl_fmt(v, sizeof v, c->last[i], PLD.s[s].dec);
        if (best == k) t = c->lt[i];
        if (c->mx[i] > c->mn[i] && !c->buckets) {
            char a[24], b[24];
            pl_fmt(a, sizeof a, c->mn[i], PLD.s[s].dec);
            pl_fmt(b, sizeof b, c->mx[i], PLD.s[s].dec);
            snprintf(rows[nr], sizeof rows[0], "%.14s %s %s (%s…%s)", PLD.s[s].name, v, PLD.s[s].unit, a, b);
        } else {
            snprintf(rows[nr], sizeof rows[0], "%.14s %s %s", PLD.s[s].name, v, PLD.s[s].unit);
        }
        cols[nr] = lv_color_hex(PLD.s[s].col);
        nr++;
        int32_t y = oy + ys(c->last[i]);
        rect(L, ox + C.px + best - 7, y - 7, ox + C.px + best + 7, y + 7, lv_color_white(), LV_OPA_COVER, 7);
        rect(L, ox + C.px + best - 5, y - 5, ox + C.px + best + 5, y + 5, cols[nr - 1], LV_OPA_COVER, 5);
    }
    fmt_clock(rows[0], sizeof rows[0], t);
    cols[0] = lv_color_white();
    int32_t w = 0;
    for (int r = 0; r < nr; r++) {
        int32_t tw = text_w(rows[r], aos_font_caption);
        if (tw > w) w = tw;
    }
    w += 28;
    int32_t h = nr * 28 + 14;
    int32_t bx = x + 16;
    if (bx + w > ox + C.px + C.pw) bx = x - 16 - w;
    if (bx < ox + C.px) bx = ox + C.px;
    int32_t by = oy + C.py + 8;
    rect(L, bx, by, bx + w, by + h, lv_color_hex(0x1C1F26), LV_OPA_90, 14);
    for (int r = 0; r < nr; r++)
        label_at(L, rows[r], aos_font_caption, cols[r], bx + 14, by + 8 + r * 28, bx + w - 4, by + 8 + r * 28 + 28,
                 LV_TEXT_ALIGN_LEFT);
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *L = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(C.obj, &a);
    int32_t ox = a.x1, oy = a.y1;
    rect(L, a.x1, a.y1, a.x2, a.y2, lv_color_hex(0x0E1116), LV_OPA_COVER, 20);
    if (C.pw < 50 || C.ph < 50 || !C.cols.mn) return;
    /* the legend */
    for (int s = 0; s < C.nleg; s++) {
        const lv_area_t *r = &C.leg[s];
        const pl_series_t *se = &PLD.s[s];
        bool big = s == PLS.big;
        rect(L, ox + 12 + r->x1, oy + 10 + r->y1, ox + 12 + r->x2, oy + 10 + r->y2,
             big ? lv_color_hex(0x2C3440) : lv_color_hex(0x181C22), LV_OPA_COVER, 16);
        rect(L, ox + 12 + r->x1 + 12, oy + 10 + r->y1 + 10, ox + 12 + r->x1 + 24, oy + 10 + r->y1 + 22,
             se->hidden ? lv_color_hex(0x555555) : lv_color_hex(se->col), LV_OPA_COVER, 6);
        char t[64];
        legend_text(s, t, sizeof t);
        label_at(L, t, aos_font_caption, se->hidden ? lv_color_hex(0x666666) : lv_color_white(),
                 ox + 12 + r->x1 + 34, oy + 10 + r->y1 + 4, ox + 12 + r->x2 - 6, oy + 10 + r->y2, LV_TEXT_ALIGN_LEFT);
    }
    if (C.msg[0]) {
        label_at(L, C.msg, aos_font_small, lv_color_hex(0x8E8E93), ox + 30, oy + C.py + C.ph / 2 - 40,
                 ox + C.px + C.pw - 30, oy + C.py + C.ph / 2 + 60, LV_TEXT_ALIGN_CENTER);
        if (!PLD.rows) return;
    }
    draw_axes(L, ox, oy);
    if (PLS.mode == PL_MODE_XY) {
        draw_xy(L, ox, oy);
        return;
    }
    draw_alarms(L, ox, oy);
    for (int s = 0; s < PLD.ns; s++)
        if (!PLD.s[s].hidden && s != PLS.big) draw_series(L, ox, oy, s);
    if (PLS.big < PLD.ns && !PLD.s[PLS.big].hidden) draw_series(L, ox, oy, PLS.big);     /* on top */
    draw_cursor(L, ox, oy);
}

/* ---- fingers ---- */

static void win_set(uint32_t w)
{
    if (w < WIN_MIN) w = WIN_MIN;
    if (w > WIN_MAX) w = WIN_MAX;
    PLA.win_ms = w;
}

static void gesture_cb(const aos_gesture_event_t *ev, void *ud)
{
    (void)ud;
    lv_area_t a;
    lv_obj_get_coords(C.obj, &a);
    int32_t lx = (int32_t)ev->x - a.x1, ly = (int32_t)ev->y - a.y1;
    switch (ev->type) {
    case AOS_GESTURE_TAP:
        for (int s = 0; s < C.nleg; s++) {
            const lv_area_t *r = &C.leg[s];
            if (lx >= 12 + r->x1 && lx <= 12 + r->x2 && ly >= 10 + r->y1 && ly <= 10 + r->y2) {
                if (PLS.big == s) PLD.s[s].hidden = !PLD.s[s].hidden;
                else { PLS.big = s; PLD.s[s].hidden = false; }
                pl_set_save();
                C.want = true;
                pl_refresh(true);
                return;
            }
        }
        if (PLS.mode == PL_MODE_XY) return;
        if (C.cursor && (int32_t)(C.hold_until - pl_now()) > 0 && abs(lx - C.px - C.cx) < 30) {
            C.cursor = false;
        } else {
            C.cursor = true;
            C.cx = lx - C.px;
            C.hold_until = pl_now() + HOLD_MS;
        }
        lv_obj_invalidate(C.obj);
        break;
    case AOS_GESTURE_DRAG_BEGIN:
    case AOS_GESTURE_DRAG:
        if (PLS.mode == PL_MODE_XY) return;
        C.cursor = true;
        C.dragging = true;
        C.cx = lx - C.px;
        C.hold_until = pl_now() + HOLD_MS;
        lv_obj_invalidate(C.obj);
        break;
    case AOS_GESTURE_DRAG_END:
        C.dragging = false;
        C.hold_until = pl_now() + HOLD_MS;
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        C.pinching = true;
        C.cursor = false;
        C.pan_acc = 0;
        break;
    case AOS_GESTURE_PINCH: {
        if (ev->scale <= 0) break;
        uint32_t t0, t1;
        win_range(&t0, &t1);
        float frac = (float)(lx - C.px) / (float)(C.pw > 0 ? C.pw : 1);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        float tc = (float)t0 + frac * (float)PLA.win_ms;
        float nw = (float)PLA.win_ms / ev->scale;
        C.pan_acc += ev->dx;
        bool pan = PLA.paused || fabsf(C.pan_acc) > 24;
        uint32_t old = PLA.win_ms;
        win_set((uint32_t)nw);
        if (pan) {
            float end = tc + (1 - frac) * (float)PLA.win_ms - ev->dx * (float)PLA.win_ms / (float)C.pw;
            uint32_t now = pl_now();
            if (!PLA.paused) { PLA.paused = true; pl_refresh(true); }
            if (end > (float)now) end = (float)now;
            uint32_t first = pl_data_first_t();
            if (end < (float)first + (float)PLA.win_ms * 0.2f) end = (float)first + (float)PLA.win_ms * 0.2f;
            PLA.end_t = (uint32_t)end;
        }
        if (old != PLA.win_ms || pan) {
            C.want = true;
            pl_refresh(false);
        }
        break;
    }
    case AOS_GESTURE_PINCH_END:
        C.pinching = false;
        pl_refresh(true);
        break;
    default:
        break;
    }
}

lv_obj_t *pl_chart_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    C.obj = lv_obj_create(parent);
    lv_obj_remove_style_all(C.obj);
    lv_obj_set_size(C.obj, w, h);
    lv_obj_remove_flag(C.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(C.obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    aos_gesture_attach(C.obj, AOS_GESTURE_FLAG_FAST_TAP, gesture_cb, NULL);
    C.cursor = false;
    C.pw = C.ph = 0;
    C.want = true;
    return C.obj;
}

void pl_chart_free(void)
{
    pl_cols_free(&C.cols);
    free(C.xy);
    C.xy = NULL;
    C.obj = NULL;
}

void pl_chart_want(void) { C.want = true; }

/* From the app's timer: columns again when something changed, at ten a
 * second at most; the reading goes away four seconds after the finger. */
void pl_chart_poll(void)
{
    if (!C.obj) return;
    uint32_t now = pl_now();
    if (C.cursor && !C.dragging && (int32_t)(C.hold_until - now) <= 0) {
        C.cursor = false;
        lv_obj_invalidate(C.obj);
    }
    bool live = !PLA.paused && (PLD.rows != 0);
    if (!C.want && !(live && PLA.dirty) && (int32_t)(now - C.built_at) < 1000) return;
    if (!C.want && (int32_t)(now - C.built_at) < 100) return;
    C.want = false;
    PLA.dirty = false;
    build();
}

/* The statistics of the window, for the big value */
bool pl_chart_stats(int s, float *mn, float *mx, float *avg, float *last)
{
    if (s < 0 || s >= PLD.ns || !C.cols.ncols || !C.cols.sn[s]) return false;
    *mn = C.cols.smin[s];
    *mx = C.cols.smax[s];
    *avg = C.cols.ssum[s] / (float)C.cols.sn[s];
    *last = C.cols.slast[s];
    return true;
}
