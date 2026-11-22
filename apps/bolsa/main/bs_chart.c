/*
 * P4OS - Bolsa: the charts.
 *
 * Drawn by hand in the object's draw event, not with lv_chart: lv_chart
 * has no candles, no area under a line and no second panel, and it joins
 * no points across a hole (the trap Datalog measured). Here the series is
 * cut into as many slots as fit across (one candle needs ~7 px, a line
 * point 2), each slot keeps the open, high, low, close and volume of its
 * points, and the draw event turns slots into a few hundred draw tasks.
 *
 * The area under the line is a single gradient rectangle with the part
 * above the line painted over in the card's colour, column by column:
 * one gradient instead of hundreds, and no seams between triangles.
 *
 * Touch: one finger reads the slot under it (a cross with the date and
 * the value); two fingers mark two dates and say the change between them.
 * Both stay four seconds after the fingers lift, to be read.
 *
 * The chart copies what it draws, so the series behind it can be replaced
 * by a new answer at any time.
 */
#include "bs.h"

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#define MAX_SLOTS 640
#define HOLD_MS   4000

static void *ps_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return p;
#endif
    return malloc(n);
}

typedef struct {
    int32_t t;
    float o, h, l, c, v;
    float ma1, ma2, rsi;
} slot_t;

typedef struct {
    bs_pt_t *p;
    int n;
    char label[16];
    lv_color_t col;
    float base;
    /* per slot of the main series, NAN where it has nothing */
    float *y;
} cmp_t;

struct bs_chart_s {
    lv_obj_t *obj;
    bs_info_t info;
    bs_pt_t *p;
    int n, cap;
    float *ma1, *ma2, *rsi;
    bool ohlc, vol, intraday;
    float ref;
    cmp_t cmp[BS_MAX_CMP];
    int ncmp;
    int type;
    bool show_vol, show_ma, show_rsi;
    char msg[120];
    /* slots */
    slot_t *s;
    int ns, ns_w;
    bool dirty;
    float base;                 /* % mode: the first close */
    /* the geometry of the last draw */
    int32_t px, py, pw, ph;     /* the price plot */
    int32_t vy, vh;             /* the volume band, inside the plot */
    int32_t ry, rh;             /* the RSI panel */
    float lo, hi, vmax;
    bool pct;
    /* touch */
    int ca, cb;                 /* slots under the fingers, -1 none */
    bool two;
    uint32_t hold_until;
    bs_chart_read_cb_t read_cb;
    char read_txt[128];
};

lv_obj_t *bs_chart_obj(bs_chart_t *c) { return c->obj; }
void bs_chart_on_read(bs_chart_t *c, bs_chart_read_cb_t cb) { c->read_cb = cb; }

static void free_cmp(cmp_t *k)
{
    free(k->p);
    free(k->y);
    memset(k, 0, sizeof *k);
}

void bs_chart_delete(bs_chart_t *c)
{
    if (!c) return;
    free(c->p);
    free(c->ma1);
    free(c->ma2);
    free(c->rsi);
    free(c->s);
    for (int i = 0; i < BS_MAX_CMP; i++) free_cmp(&c->cmp[i]);
    free(c);
}

/* ---- slots ---- */

bool bs_chart_candles_ok(bs_chart_t *c);

static float val(const bs_chart_t *c, float v)
{
    return c->pct ? (c->base > 0 ? (v / c->base - 1.0f) * 100.0f : 0) : v;
}

static void rebuild(bs_chart_t *c, int32_t w)
{
    c->dirty = false;
    c->ns_w = w;
    c->ns = 0;
    c->pct = c->ncmp > 0;
    if (c->n < 1 || w < 20) return;
    bool candles = c->type == BS_CT_CANDLE && !c->pct;
    int max = candles ? w / 7 : w / 2;
    if (max > MAX_SLOTS) max = MAX_SLOTS;
    if (max < 2) max = 2;
    int ns = c->n < max ? c->n : max;
    if (!c->s) c->s = ps_alloc(sizeof *c->s * MAX_SLOTS);
    if (!c->s) return;
    for (int k = 0; k < ns; k++) {
        int i0 = (int)((int64_t)c->n * k / ns), i1 = (int)((int64_t)c->n * (k + 1) / ns);
        if (i1 <= i0) i1 = i0 + 1;
        slot_t *s = &c->s[k];
        const bs_pt_t *a = &c->p[i0];
        s->t = c->p[i1 - 1].t;
        s->o = a->o;
        s->c = c->p[i1 - 1].c;
        s->h = -INFINITY;
        s->l = INFINITY;
        s->v = 0;
        for (int i = i0; i < i1; i++) {
            float h = c->ohlc ? c->p[i].h : c->p[i].c, l = c->ohlc ? c->p[i].l : c->p[i].c;
            if (h > s->h) s->h = h;
            if (l < s->l) s->l = l;
            s->v += c->p[i].v;
        }
        if (!c->ohlc && i1 - i0 > 1) s->o = c->p[i0].c;
        /* the indicators are the slot's last point's */
        s->ma1 = c->ma1 ? c->ma1[i1 - 1] : NAN;
        s->ma2 = c->ma2 ? c->ma2[i1 - 1] : NAN;
        s->rsi = c->rsi ? c->rsi[i1 - 1] : NAN;
    }
    c->ns = ns;
    c->base = c->p[0].c;
    /* comparisons: each one's close at each slot's time, in % of its own
     * first value in the range */
    for (int k = 0; k < c->ncmp; k++) {
        cmp_t *m = &c->cmp[k];
        free(m->y);
        m->y = ps_alloc(sizeof(float) * (size_t)ns);
        if (!m->y) continue;
        int j = 0;
        m->base = NAN;
        for (int s = 0; s < ns; s++) {
            m->y[s] = NAN;
            int32_t t = c->s[s].t;
            while (j + 1 < m->n && m->p[j + 1].t <= t) j++;
            if (m->n && m->p[j].t <= t + 86400) {
                if (isnan(m->base)) m->base = m->p[j].c;
                m->y[s] = m->base > 0 ? (m->p[j].c / m->base - 1.0f) * 100.0f : NAN;
            }
        }
    }
}

bool bs_chart_candles_ok(bs_chart_t *c)
{
    if (c->ohlc) return true;
    /* closes only: a candle is fair when it gathers several real samples */
    return c->n >= 3 * 40;
}

/* ---- data in ---- */

static bool grow(bs_chart_t *c, int n)
{
    if (n <= c->cap) return true;
    bs_pt_t *p = ps_alloc(sizeof *p * (size_t)n);
    float *a = ps_alloc(sizeof(float) * (size_t)n), *b = ps_alloc(sizeof(float) * (size_t)n),
          *r = ps_alloc(sizeof(float) * (size_t)n);
    if (!p || !a || !b || !r) { free(p); free(a); free(b); free(r); return false; }
    free(c->p); free(c->ma1); free(c->ma2); free(c->rsi);
    c->p = p; c->ma1 = a; c->ma2 = b; c->rsi = r;
    c->cap = n;
    return true;
}

void bs_chart_set(bs_chart_t *c, const bs_info_t *in, const bs_pt_t *p, int n,
                  bool ohlc, bool vol, float ref, bool intraday)
{
    if (in) c->info = *in;
    c->n = 0;
    if (n > 0 && grow(c, n)) {
        memcpy(c->p, p, sizeof *p * (size_t)n);
        c->n = n;
        for (int i = 0; i < n; i++) c->ma1[i] = c->ma2[i] = c->rsi[i] = NAN;
    }
    c->ohlc = ohlc;
    c->vol = vol;
    c->ref = ref;
    c->intraday = intraday;
    c->msg[0] = 0;
    c->ca = c->cb = -1;
    c->dirty = true;
    lv_obj_invalidate(c->obj);
}

void bs_chart_set_full(bs_chart_t *c, const bs_pt_t *full, int full_n, int off)
{
    if (!c->n || off < 0 || off + c->n > full_n) return;
    float *tmp = ps_alloc(sizeof(float) * (size_t)full_n);
    if (!tmp) return;
    /* 20 and 50 periods: the usual pair */
    bs_sma(full, full_n, 20, tmp);
    memcpy(c->ma1, tmp + off, sizeof(float) * (size_t)c->n);
    bs_sma(full, full_n, 50, tmp);
    memcpy(c->ma2, tmp + off, sizeof(float) * (size_t)c->n);
    bs_rsi(full, full_n, 14, tmp);
    memcpy(c->rsi, tmp + off, sizeof(float) * (size_t)c->n);
    free(tmp);
    c->dirty = true;
    lv_obj_invalidate(c->obj);
}

void bs_chart_cmp_clear(bs_chart_t *c)
{
    for (int i = 0; i < BS_MAX_CMP; i++) free_cmp(&c->cmp[i]);
    c->ncmp = 0;
    c->dirty = true;
    lv_obj_invalidate(c->obj);
}

void bs_chart_cmp_add(bs_chart_t *c, const char *label, const bs_pt_t *p, int n)
{
    if (c->ncmp >= BS_MAX_CMP || n < 1) return;
    cmp_t *k = &c->cmp[c->ncmp];
    k->p = ps_alloc(sizeof *p * (size_t)n);
    if (!k->p) return;
    memcpy(k->p, p, sizeof *p * (size_t)n);
    k->n = n;
    bs_scpy(k->label, sizeof k->label, label);
    k->col = c->ncmp ? BS_C_CMP2 : BS_C_CMP1;
    c->ncmp++;
    c->dirty = true;
    lv_obj_invalidate(c->obj);
}

void bs_chart_options(bs_chart_t *c, int type, bool vol, bool ma, bool rsi)
{
    c->type = type;
    c->show_vol = vol;
    c->show_ma = ma;
    c->show_rsi = rsi;
    c->dirty = true;
    lv_obj_invalidate(c->obj);
}

void bs_chart_message(bs_chart_t *c, const char *msg)
{
    bs_scpy(c->msg, sizeof c->msg, msg ? msg : "");
    lv_obj_invalidate(c->obj);
}

/* ---- drawing ---- */

static void label_at(lv_layer_t *layer, const char *txt, const lv_font_t *f, lv_color_t col,
                     int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = col;
    d.text = txt;
    d.text_local = 1;           /* the text is built here, on the stack (HANDOFF-APPS 8) */
    d.align = al;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_label(layer, &d, &a);
}

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t col,
                 lv_opa_t opa, int32_t radius)
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

static void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t col,
                 int32_t w, lv_opa_t opa, int32_t dash)
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

static int32_t sx(const bs_chart_t *c, int k)
{
    return c->px + (int32_t)(((int64_t)(2 * k + 1) * c->pw) / (2 * c->ns));
}

static int32_t sy(const bs_chart_t *c, float v)
{
    float f = (c->hi - val(c, v)) / (c->hi - c->lo);
    return c->py + (int32_t)(f * (float)c->ph);
}

static int32_t sy_raw(const bs_chart_t *c, float pv)
{
    float f = (c->hi - pv) / (c->hi - c->lo);
    return c->py + (int32_t)(f * (float)c->ph);
}

static void polyline(lv_layer_t *layer, const bs_chart_t *c, const float *ys, bool raw, int stride_field,
                     lv_color_t col, int32_t w)
{
    (void)stride_field;
    lv_point_precise_t *pts = lv_malloc(sizeof *pts * (size_t)c->ns);
    if (!pts) return;
    int n = 0;
    for (int k = 0; k < c->ns; k++) {
        float v = ys[k];
        if (isnan(v)) {
            if (n) { pts[n].x = LV_DRAW_LINE_POINT_NONE; pts[n].y = LV_DRAW_LINE_POINT_NONE; n++; }
            continue;
        }
        pts[n].x = sx(c, k);
        pts[n].y = raw ? sy_raw(c, v) : sy(c, v);
        n++;
    }
    if (n >= 2) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = col;
        d.width = w;
        d.opa = LV_OPA_COVER;
        d.round_start = d.round_end = 1;
        d.points = pts;
        d.point_cnt = n;
        lv_draw_line(layer, &d);     /* it copies the points */
    }
    lv_free(pts);
}

static void axis_labels(lv_layer_t *layer, bs_chart_t *c, int32_t right)
{
    char t[32];
    const lv_font_t *f = aos_font_tiny;
    for (int i = 0; i <= 4; i++) {
        float v = c->hi - (c->hi - c->lo) * (float)i / 4.0f;
        int32_t y = c->py + c->ph * i / 4;
        if (i > 0 && i < 4) line(layer, c->px, y, c->px + c->pw, y, BS_C_GRID, 1, LV_OPA_70, 6);
        if (c->pct) bs_fmt_pct(v, t, sizeof t);
        else bs_fmt_price(v, &c->info, t, sizeof t);
        int32_t ty = y - 10;
        if (i == 0) ty = y;
        if (i == 4) ty = y - 20;
        label_at(layer, t, f, AOS_C_DIM, c->px + c->pw + 8, ty, right, ty + 20, LV_TEXT_ALIGN_LEFT);
    }
}

static void time_labels(lv_layer_t *layer, bs_chart_t *c, int32_t y)
{
    if (c->ns < 2) return;
    char t[24];
    int marks = c->pw > 900 ? 5 : 3;
    int32_t span = c->s[c->ns - 1].t - c->s[0].t;
    for (int i = 0; i < marks; i++) {
        int k = (c->ns - 1) * i / (marks - 1);
        int32_t x = sx(c, k);
        if (c->intraday && span < 2 * 86400) bs_fmt_hm(c->s[k].t, t, sizeof t);
        else bs_fmt_date(c->s[k].t, false, t, sizeof t);
        lv_text_align_t al = i == 0 ? LV_TEXT_ALIGN_LEFT : i == marks - 1 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
        int32_t x1 = i == 0 ? c->px : i == marks - 1 ? c->px + c->pw - 160 : x - 80;
        label_at(layer, t, aos_font_tiny, AOS_C_DIM, x1, y, x1 + 160, y + 20, al);
    }
}

static void range_of(bs_chart_t *c)
{
    float lo = INFINITY, hi = -INFINITY, vmax = 0;
    for (int k = 0; k < c->ns; k++) {
        const slot_t *s = &c->s[k];
        float l = c->type == BS_CT_CANDLE && !c->pct ? s->l : s->c;
        float h = c->type == BS_CT_CANDLE && !c->pct ? s->h : s->c;
        if (c->type != BS_CT_CANDLE && c->ohlc && !c->pct) { l = s->c; h = s->c; }
        l = val(c, l);
        h = val(c, h);
        if (l < lo) lo = l;
        if (h > hi) hi = h;
        if (c->show_ma && !c->pct) {
            if (!isnan(s->ma1)) { lo = fminf(lo, s->ma1); hi = fmaxf(hi, s->ma1); }
            if (!isnan(s->ma2)) { lo = fminf(lo, s->ma2); hi = fmaxf(hi, s->ma2); }
        }
        if (s->v > vmax) vmax = s->v;
        for (int m = 0; m < c->ncmp; m++)
            if (c->cmp[m].y && !isnan(c->cmp[m].y[k])) { lo = fminf(lo, c->cmp[m].y[k]); hi = fmaxf(hi, c->cmp[m].y[k]); }
    }
    if (!c->pct && c->intraday && c->ref > 0) {
        float span = hi - lo;
        if (c->ref > lo - span && c->ref < hi + span) { lo = fminf(lo, c->ref); hi = fmaxf(hi, c->ref); }
    }
    float span = hi - lo;
    float mid = (hi + lo) / 2, min = fabsf(mid) * 0.004f + 1e-6f;
    if (span < min) { lo = mid - min; hi = mid + min; }
    else { lo -= span * 0.06f; hi += span * 0.06f; }
    c->lo = lo;
    c->hi = hi;
    c->vmax = vmax;
}

static void draw_cb(lv_event_t *e)
{
    bs_chart_t *c = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(c->obj, &a);
    int32_t W = lv_area_get_width(&a), H = lv_area_get_height(&a);
    int32_t axis_w = W > 1000 ? 150 : 128;

    c->px = a.x1 + 4;
    c->pw = W - axis_w - 8;
    c->py = a.y1 + 12;
    int32_t bottom = a.y2 - 30;
    bool rsi = c->show_rsi && !c->pct && c->rsi && c->n > 15;
    c->rh = rsi ? (bottom - c->py) * 22 / 100 : 0;
    c->ph = bottom - c->py - (rsi ? c->rh + 18 : 0);
    c->ry = c->py + c->ph + 18;
    c->vh = c->ph * 18 / 100;
    c->vy = c->py + c->ph - c->vh;

    if (c->dirty || c->ns_w != c->pw) rebuild(c, c->pw);

    if (c->ns < 2) {
        const char *m = c->msg[0] ? c->msg : _("Cargando…");
        label_at(layer, m, aos_font_small, AOS_C_DIM, a.x1 + 20, a.y1 + H / 2 - 40, a.x2 - 20, a.y1 + H / 2 + 60,
                 LV_TEXT_ALIGN_CENTER);
        return;
    }
    range_of(c);
    bool up = c->pct ? c->s[c->ns - 1].c >= c->base
                     : c->s[c->ns - 1].c >= (c->intraday && c->ref > 0 ? c->ref : c->s[0].c);
    lv_color_t main_col = c->pct ? BS_C_LINE : up ? BS_C_UP : BS_C_DOWN;

    /* the area: one gradient, then the card's colour above the line */
    if (c->type == BS_CT_AREA || c->pct == false) {
        if (c->type == BS_CT_AREA && !c->pct) {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.bg_opa = LV_OPA_COVER;
            d.bg_grad.dir = LV_GRAD_DIR_VER;
            d.bg_grad.stops_count = 2;
            d.bg_grad.stops[0].color = main_col;
            d.bg_grad.stops[0].opa = LV_OPA_50;
            d.bg_grad.stops[0].frac = 0;
            d.bg_grad.stops[1].color = main_col;
            d.bg_grad.stops[1].opa = LV_OPA_0;
            d.bg_grad.stops[1].frac = 255;
            int32_t top = sy(c, c->hi);
            for (int k = 0; k < c->ns; k++) {
                int32_t y = sy(c, c->s[k].c);
                if (y < top) top = y;
            }
            lv_area_t g = { sx(c, 0), top, sx(c, c->ns - 1), c->py + c->ph };
            lv_draw_rect(layer, &d, &g);
            /* paint over what is above the line, 3 px at a time */
            int32_t x0 = sx(c, 0), x1 = sx(c, c->ns - 1);
            for (int32_t x = x0; x <= x1; x += 3) {
                /* the line's height at x, between the two slots around it */
                float fk = (float)(x - c->px) * (float)c->ns / (float)c->pw - 0.5f;
                int k = (int)fk;
                if (k < 0) k = 0;
                if (k > c->ns - 2) k = c->ns - 2;
                float t = fk - (float)k;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                float y = (float)sy(c, c->s[k].c) * (1 - t) + (float)sy(c, c->s[k + 1].c) * t;
                if ((int32_t)y > top) rect(layer, x, top, x + 2, (int32_t)y, AOS_C_CARD, LV_OPA_COVER, 0);
            }
        }
    }

    axis_labels(layer, c, a.x2);

    /* the previous close, dotted, for the day */
    if (c->intraday && c->ref > 0 && !c->pct) {
        int32_t y = sy(c, c->ref);
        if (y > c->py && y < c->py + c->ph) line(layer, c->px, y, c->px + c->pw, y, AOS_C_DIM, 2, LV_OPA_80, 4);
    }

    /* volume */
    if (c->show_vol && c->vol && c->vmax > 0 && !c->pct) {
        int32_t bw = c->pw / c->ns;
        int32_t half = bw > 3 ? (bw * 3 / 4) / 2 : 0;
        for (int k = 0; k < c->ns; k++) {
            const slot_t *s = &c->s[k];
            int32_t hh = (int32_t)((float)c->vh * s->v / c->vmax);
            if (hh < 1) continue;
            int32_t x = sx(c, k);
            bool u = k == 0 ? s->c >= s->o : s->c >= c->s[k - 1].c;
            rect(layer, x - half, c->vy + c->vh - hh, x + half + (bw > 1 ? 0 : 0), c->vy + c->vh,
                 u ? BS_C_UP : BS_C_DOWN, LV_OPA_40, 0);
        }
    }

    /* the price */
    if (c->type == BS_CT_CANDLE && !c->pct) {
        int32_t bw = c->pw / c->ns;
        int32_t half = (bw * 7 / 10) / 2;
        if (half < 1) half = 1;
        for (int k = 0; k < c->ns; k++) {
            const slot_t *s = &c->s[k];
            int32_t x = sx(c, k);
            bool u = s->c >= s->o;
            lv_color_t col = u ? BS_C_UP : BS_C_DOWN;
            line(layer, x, sy(c, s->h), x, sy(c, s->l), col, 2, LV_OPA_COVER, 0);
            int32_t y1 = sy(c, u ? s->c : s->o), y2 = sy(c, u ? s->o : s->c);
            if (y2 - y1 < 2) y2 = y1 + 2;
            rect(layer, x - half, y1, x + half, y2, col, LV_OPA_COVER, 1);
        }
    } else {
        float *ys = lv_malloc(sizeof(float) * (size_t)c->ns);
        if (ys) {
            for (int k = 0; k < c->ns; k++) ys[k] = c->s[k].c;
            polyline(layer, c, ys, false, 0, main_col, c->pw > 900 ? 4 : 3);
            if (c->pct) {
                for (int m = 0; m < c->ncmp; m++)
                    if (c->cmp[m].y) polyline(layer, c, c->cmp[m].y, true, 0, c->cmp[m].col, 3);
            }
            lv_free(ys);
        }
    }

    /* the averages */
    if (c->show_ma && !c->pct) {
        float *ys = lv_malloc(sizeof(float) * (size_t)c->ns);
        if (ys) {
            for (int k = 0; k < c->ns; k++) ys[k] = c->s[k].ma1;
            polyline(layer, c, ys, true, 0, BS_C_MA1, 2);
            for (int k = 0; k < c->ns; k++) ys[k] = c->s[k].ma2;
            polyline(layer, c, ys, true, 0, BS_C_MA2, 2);
            lv_free(ys);
        }
        char m1[16], m2[16];
        snprintf(m1, sizeof m1, "%s20", _("MM"));
        snprintf(m2, sizeof m2, "%s50", _("MM"));
        label_at(layer, m1, aos_font_tiny, BS_C_MA1, c->px + 4, c->py, c->px + 80, c->py + 20, LV_TEXT_ALIGN_LEFT);
        label_at(layer, m2, aos_font_tiny, BS_C_MA2, c->px + 72, c->py, c->px + 160, c->py + 20, LV_TEXT_ALIGN_LEFT);
    }

    /* the legend of the comparison */
    if (c->pct) {
        int32_t x = c->px + 4;
        char t[48];
        snprintf(t, sizeof t, "%s", c->info.show);
        label_at(layer, t, aos_font_tiny, BS_C_LINE, x, c->py, x + 150, c->py + 20, LV_TEXT_ALIGN_LEFT);
        for (int m = 0; m < c->ncmp; m++) {
            x += 130;
            label_at(layer, c->cmp[m].label, aos_font_tiny, c->cmp[m].col, x, c->py, x + 150, c->py + 20, LV_TEXT_ALIGN_LEFT);
        }
    }

    /* the RSI */
    if (rsi) {
        int32_t y70 = c->ry + c->rh * 30 / 100, y30 = c->ry + c->rh * 70 / 100;
        line(layer, c->px, c->ry, c->px + c->pw, c->ry, BS_C_GRID, 1, LV_OPA_COVER, 0);
        line(layer, c->px, y70, c->px + c->pw, y70, BS_C_DOWN, 1, LV_OPA_60, 5);
        line(layer, c->px, y30, c->px + c->pw, y30, BS_C_UP, 1, LV_OPA_60, 5);
        lv_point_precise_t *pts = lv_malloc(sizeof *pts * (size_t)c->ns);
        if (pts) {
            int n = 0;
            for (int k = 0; k < c->ns; k++) {
                if (isnan(c->s[k].rsi)) continue;
                pts[n].x = sx(c, k);
                pts[n].y = c->ry + (int32_t)((100.0f - c->s[k].rsi) / 100.0f * (float)c->rh);
                n++;
            }
            if (n >= 2) {
                lv_draw_line_dsc_t d;
                lv_draw_line_dsc_init(&d);
                d.color = BS_C_MA2;
                d.width = 2;
                d.points = pts;
                d.point_cnt = n;
                lv_draw_line(layer, &d);
            }
            lv_free(pts);
        }
        char t[24];
        float last = c->s[c->ns - 1].rsi;
        if (isnan(last)) snprintf(t, sizeof t, "RSI 14");
        else snprintf(t, sizeof t, "RSI %d", (int)roundf(last));
        label_at(layer, t, aos_font_tiny, AOS_C_DIM, c->px + c->pw + 8, c->ry, a.x2, c->ry + 20, LV_TEXT_ALIGN_LEFT);
        label_at(layer, "70", aos_font_tiny, AOS_C_DIM, c->px + c->pw + 8, y70 - 10, a.x2, y70 + 10, LV_TEXT_ALIGN_LEFT);
        label_at(layer, "30", aos_font_tiny, AOS_C_DIM, c->px + c->pw + 8, y30 - 10, a.x2, y30 + 10, LV_TEXT_ALIGN_LEFT);
    }

    time_labels(layer, c, a.y2 - 22);

    /* the fingers */
    if (c->ca >= 0 && c->ca < c->ns) {
        bool two = c->two && c->cb >= 0 && c->cb < c->ns;
        if (two) {
            int k1 = c->ca < c->cb ? c->ca : c->cb, k2 = c->ca < c->cb ? c->cb : c->ca;
            float pct = c->s[k1].c > 0 ? (c->s[k2].c / c->s[k1].c - 1) * 100 : 0;
            rect(layer, sx(c, k1), c->py, sx(c, k2), c->py + c->ph, pct >= 0 ? BS_C_UP : BS_C_DOWN, LV_OPA_20, 0);
        }
        for (int f = 0; f < (two ? 2 : 1); f++) {
            int k = f ? c->cb : c->ca;
            int32_t x = sx(c, k);
            float v = c->pct ? c->s[k].c : c->s[k].c;
            int32_t y = sy(c, v);
            line(layer, x, c->py, x, c->py + c->ph + (rsi ? c->rh + 18 : 0), AOS_C_TEXT, 2, LV_OPA_80, 0);
            if (!two) line(layer, c->px, y, c->px + c->pw, y, AOS_C_TEXT, 1, LV_OPA_50, 4);
            rect(layer, x - 8, y - 8, x + 8, y + 8, AOS_C_TEXT, LV_OPA_COVER, LV_RADIUS_CIRCLE);
            rect(layer, x - 5, y - 5, x + 5, y + 5, main_col, LV_OPA_COVER, LV_RADIUS_CIRCLE);
            /* the date over the line */
            char t[24];
            bool hm = c->intraday && c->s[c->ns - 1].t - c->s[0].t < 6 * 86400;
            bs_fmt_date(c->s[k].t, hm, t, sizeof t);
            int32_t bx = x - 80;
            if (bx < c->px) bx = c->px;
            if (bx + 160 > c->px + c->pw) bx = c->px + c->pw - 160;
            rect(layer, bx, c->py - 12, bx + 160, c->py + 14, AOS_C_CARD2, LV_OPA_COVER, 8);
            label_at(layer, t, aos_font_tiny, AOS_C_TEXT, bx, c->py - 10, bx + 160, c->py + 14, LV_TEXT_ALIGN_CENTER);
        }
    }
}

/* ---- touch ---- */

static int slot_at(const bs_chart_t *c, int32_t x)
{
    if (c->ns < 1 || c->pw <= 0) return -1;
    int k = (int)((int64_t)(x - c->px) * c->ns / c->pw);
    if (k < 0) k = 0;
    if (k >= c->ns) k = c->ns - 1;
    return k;
}

static void report(bs_chart_t *c)
{
    if (!c->read_cb) return;
    if (c->ca < 0 || c->ns < 1) {
        c->read_cb("", AOS_C_TEXT, false);
        return;
    }
    char a[32], b[32], t[128];
    bool hm = c->intraday && c->s[c->ns - 1].t - c->s[0].t < 6 * 86400;
    if (c->two && c->cb >= 0) {
        int k1 = c->ca < c->cb ? c->ca : c->cb, k2 = c->ca < c->cb ? c->cb : c->ca;
        const slot_t *s1 = &c->s[k1], *s2 = &c->s[k2];
        float pct = s1->c > 0 ? (s2->c / s1->c - 1) * 100 : 0;
        char d1[24], d2[24];
        bs_fmt_date(s1->t, hm, d1, sizeof d1);
        bs_fmt_date(s2->t, hm, d2, sizeof d2);
        bs_fmt_pct(pct, a, sizeof a);
        bs_fmt_chg(s2->c - s1->c, &c->info, b, sizeof b);
        snprintf(t, sizeof t, "%s > %s   %s  (%s)", d1, d2, a, b);
        c->read_cb(t, pct >= 0 ? BS_C_UP : BS_C_DOWN, true);
        return;
    }
    const slot_t *s = &c->s[c->ca];
    char d[24];
    bs_fmt_date(s->t, hm, d, sizeof d);
    if (c->pct) {
        bs_fmt_pct(val(c, s->c), a, sizeof a);
        size_t w = (size_t)snprintf(t, sizeof t, "%s  %s %s", d, c->info.show, a);
        for (int m = 0; m < c->ncmp && w < sizeof t; m++) {
            float v = c->cmp[m].y ? c->cmp[m].y[c->ca] : NAN;
            if (isnan(v)) continue;
            bs_fmt_pct(v, b, sizeof b);
            w += (size_t)snprintf(t + w, sizeof t - w, "  %s %s", c->cmp[m].label, b);
        }
    } else if (c->type == BS_CT_CANDLE && (c->ohlc || bs_chart_candles_ok(c))) {
        char o[20], h[20], l[20], cl[20];
        bs_fmt_price(s->o, &c->info, o, sizeof o);
        bs_fmt_price(s->h, &c->info, h, sizeof h);
        bs_fmt_price(s->l, &c->info, l, sizeof l);
        bs_fmt_price(s->c, &c->info, cl, sizeof cl);
        snprintf(t, sizeof t, _("%s  A %s  Mx %s  Mn %s  C %s"), d, o, h, l, cl);
    } else {
        bs_fmt_price(s->c, &c->info, a, sizeof a);
        snprintf(t, sizeof t, "%s   %s", d, a);
        if (c->vol && s->v > 0) {
            bs_fmt_big(s->v, b, sizeof b);
            size_t w = strlen(t);
            snprintf(t + w, sizeof t - w, _("   vol. %s"), b);
        }
    }
    bool up = c->ca == 0 || s->c >= c->s[c->ca - 1].c;
    c->read_cb(t, c->pct ? AOS_C_TEXT : up ? BS_C_UP : BS_C_DOWN, true);
}

static void touch_cb(lv_event_t *e)
{
    bs_chart_t *c = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        aos_touch_point_t tp[2];
        int n = aos_touch_points(tp);
        int ca = slot_at(c, p.x), cb = -1;
        bool two = false;
        if (n >= 2 && tp[0].down && tp[1].down) {
            ca = slot_at(c, (int32_t)tp[0].x);
            cb = slot_at(c, (int32_t)tp[1].x);
            two = true;
        } else if (c->two && (int32_t)(c->hold_until - aos_hal_uptime_ms()) > 0 && code == LV_EVENT_PRESSING) {
            /* a finger of the two lifted first: keep the pair a moment */
            return;
        }
        if (ca != c->ca || cb != c->cb || two != c->two) {
            c->ca = ca;
            c->cb = cb;
            c->two = two;
            lv_obj_invalidate(c->obj);
            report(c);
        }
        c->hold_until = (uint32_t)aos_hal_uptime_ms() + (two ? 600 : 0);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        c->hold_until = (uint32_t)aos_hal_uptime_ms() + HOLD_MS;
    }
}

void bs_chart_poll(bs_chart_t *c)
{
    if (!c || c->ca < 0) return;
    /* the second finger is read here too: LVGL only follows the first */
    if (lv_obj_has_state(c->obj, LV_STATE_PRESSED)) {
        aos_touch_point_t tp[2];
        if (aos_touch_points(tp) >= 2 && tp[0].down && tp[1].down) {
            int ca = slot_at(c, (int32_t)tp[0].x), cb = slot_at(c, (int32_t)tp[1].x);
            if (ca != c->ca || cb != c->cb || !c->two) {
                c->ca = ca;
                c->cb = cb;
                c->two = true;
                lv_obj_invalidate(c->obj);
                report(c);
            }
        }
        return;
    }
    if ((int32_t)(c->hold_until - aos_hal_uptime_ms()) <= 0) {
        c->ca = c->cb = -1;
        c->two = false;
        lv_obj_invalidate(c->obj);
        report(c);
    }
}

bs_chart_t *bs_chart_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    bs_chart_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ca = c->cb = -1;
    c->type = BS_CT_AREA;
    c->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(c->obj);
    lv_obj_set_size(c->obj, w, h);
    lv_obj_remove_flag(c->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(c->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c->obj, draw_cb, LV_EVENT_DRAW_MAIN, c);
    lv_obj_add_event_cb(c->obj, touch_cb, LV_EVENT_PRESSED, c);
    lv_obj_add_event_cb(c->obj, touch_cb, LV_EVENT_PRESSING, c);
    lv_obj_add_event_cb(c->obj, touch_cb, LV_EVENT_RELEASED, c);
    lv_obj_add_event_cb(c->obj, touch_cb, LV_EVENT_PRESS_LOST, c);
    return c;
}

/* ---- the sparkline ---- */

typedef struct {
    float v[64];
    int n;
    float base;
    bool up;
} spark_t;

static void spark_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    spark_t *s = lv_obj_get_user_data(o);
    if (!s || s->n < 2) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t W = lv_area_get_width(&a) - 4, H = lv_area_get_height(&a) - 8;
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < s->n; i++) { lo = fminf(lo, s->v[i]); hi = fmaxf(hi, s->v[i]); }
    if (!isnan(s->base)) {
        float span = hi - lo;
        if (s->base > lo - span && s->base < hi + span) { lo = fminf(lo, s->base); hi = fmaxf(hi, s->base); }
    }
    if (hi - lo < 1e-6f * fabsf(hi) + 1e-9f) { hi += 1; lo -= 1; }
    lv_point_precise_t pts[64];
    for (int i = 0; i < s->n; i++) {
        pts[i].x = a.x1 + 2 + W * i / (s->n - 1);
        pts[i].y = a.y1 + 4 + (int32_t)((hi - s->v[i]) / (hi - lo) * (float)H);
    }
    if (!isnan(s->base)) {
        int32_t y = a.y1 + 4 + (int32_t)((hi - s->base) / (hi - lo) * (float)H);
        line(layer, a.x1, y, a.x2, y, AOS_C_DIM, 1, LV_OPA_70, 3);
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = s->up ? BS_C_UP : BS_C_DOWN;
    d.width = 3;
    d.round_start = d.round_end = 1;
    d.points = pts;
    d.point_cnt = s->n;
    lv_draw_line(layer, &d);
}

static void spark_free(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    free(lv_obj_get_user_data(o));
    lv_obj_set_user_data(o, NULL);
}

lv_obj_t *bs_spark_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    spark_t *s = calloc(1, sizeof *s);
    lv_obj_set_user_data(o, s);
    lv_obj_add_event_cb(o, spark_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, spark_free, LV_EVENT_DELETE, NULL);
    return o;
}

void bs_spark_set(lv_obj_t *o, const bs_pt_t *p, int n, float base, bool up)
{
    spark_t *s = lv_obj_get_user_data(o);
    if (!s) return;
    /* down to 64 points: the last of each slice */
    int m = n < 64 ? n : 64;
    bool same = s->n == m && s->up == up && ((isnan(s->base) && isnan(base)) || s->base == base);
    for (int k = 0; k < m; k++) {
        int i = (int)((int64_t)n * (k + 1) / m) - 1;
        if (i < 0) i = 0;
        same &= s->v[k] == p[i].c;
        s->v[k] = p[i].c;
    }
    s->n = m;
    s->base = base;
    s->up = up;
    if (!same) lv_obj_invalidate(o);
}
