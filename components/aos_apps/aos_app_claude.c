/*
 * P4OS - Claude: how much of the Claude plan is used, the same numbers as
 * Claude Code's /usage, straight from Anthropic (aos_claude.c).
 *
 * The two windows as gauges (5 hours, the week) with the time each
 * resets, the per-model weekly limits when the plan has them, the pace
 * measured from the board's own history, when the limit comes at that pace,
 * and a day or a week of that history. They are the account's numbers, so
 * they are right however many machines use it.
 *
 * Signing in is done once from the portal (http://<name>.local/#claude):
 * typing a code on a 5" keyboard is not what anybody wants.
 *
 * Also a home screen widget, "widget claude 2x2" or "4x2" in menu.txt.
 */
#include "aos_apps.h"
#include "aos_claude.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"
#include "aos_fonts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C_CLAUDE  lv_color_hex(0xD97757)

/* seconds -> "2 h 13 min", "3 d 4 h", "5 min" */
static void fmt_span(char *out, size_t n, long s)
{
    if (s < 0) s = 0;
    if (s >= 86400) snprintf(out, n, "%ld d %ld h", s / 86400, s % 86400 / 3600);
    else if (s >= 3600) snprintf(out, n, "%ld h %ld min", s / 3600, s % 3600 / 60);
    else snprintf(out, n, "%ld min", (s + 59) / 60);
}

static lv_color_t pct_color(double p)
{
    return p >= 90 ? AOS_C_RED : p >= 75 ? AOS_C_ORANGE : C_CLAUDE;
}


/* The pace of a window from the board's history: percent per hour over the
 * last hour (at least 20 minutes of it), and false while there is not
 * enough. A reset (a big drop) cuts the stretch there. */
/* One scratch copy of the history for the pace and the chart: 24 KB, on
 * the heap (PSRAM), not in internal RAM. */
static float *s_hp;
static time_t *s_ht;

static bool hist_scratch(void)
{
    if (!s_hp) s_hp = malloc(AOS_CLAUDE_HIST_MAX * sizeof *s_hp);
    if (!s_ht) s_ht = malloc(AOS_CLAUDE_HIST_MAX * sizeof *s_ht);
    return s_hp && s_ht;
}

static bool pace(int which, float *per_hour)
{
    if (!hist_scratch()) return false;
    float *p = s_hp;
    time_t *t = s_ht;
    int n = aos_claude_history(which, p, t, AOS_CLAUDE_HIST_MAX);
    if (n < 2) return false;
    time_t now = time(NULL);
    int first = n - 1;
    while (first > 0 && now - t[first - 1] <= 3600 && p[first - 1] <= p[first] + 1) first--;
    if (t[n - 1] - t[first] < 20 * 60) return false;
    *per_hour = (p[n - 1] - p[first]) / (float)(t[n - 1] - t[first]) * 3600.0f;
    return true;
}

/* -------------------------------------------------------------------------- */
/* The app                                                                     */
/* -------------------------------------------------------------------------- */

static struct {
    int chart;                      /* 0: the 5 h window over a day, 1: the week over 7 days */
} S;

typedef struct {
    lv_obj_t *arc, *pct, *reset, *extra;
} gauge_t;

static struct {
    lv_obj_t *root, *sub, *signin, *signin_txt, *body, *pace, *eta, *chart, *chart_msg, *chips[2], *err;
    lv_chart_series_t *ser;
    gauge_t g[2];
    lv_timer_t *timer;
    int32_t W, H;
    bool land;
    uint32_t seen, hist_n;
} U;

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, f, c);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static void gauge_build(gauge_t *g, lv_obj_t *parent, int32_t w, const char *title)
{
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_t *t = aos_label(c, title, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, lv_pct(100));
    int32_t d = w - 40 > 260 ? 260 : w - 40;
    g->arc = lv_arc_create(c);
    lv_obj_set_size(g->arc, d, d);
    lv_arc_set_rotation(g->arc, 135);
    lv_arc_set_bg_angles(g->arc, 0, 270);
    lv_arc_set_range(g->arc, 0, 1000);
    lv_arc_set_value(g->arc, 0);
    lv_obj_remove_style(g->arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(g->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g->arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g->arc, 22, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g->arc, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g->arc, C_CLAUDE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g->arc, true, LV_PART_INDICATOR);
    g->pct = aos_label(g->arc, "--", d >= 240 ? &aos_inter_num_96 : aos_font_huge, AOS_C_TEXT);
    lv_obj_align(g->pct, LV_ALIGN_CENTER, 0, 0);
    g->reset = aos_label(c, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_margin_top(g->reset, -d / 8, 0);
    g->extra = aos_label(c, "", aos_font_caption, AOS_C_DIM);
}

static void gauge_fill(gauge_t *g, const aos_claude_window_t *w, const char *extra)
{
    char a[48], b[64];
    if (w->valid) {
        lv_arc_set_value(g->arc, (int32_t)lroundf(fminf(100, fmaxf(0, w->pct)) * 10));
        lv_obj_set_style_arc_color(g->arc, pct_color(w->pct), LV_PART_INDICATOR);
        snprintf(a, sizeof a, "%.0f%%", (double)w->pct);
        lv_label_set_text(g->pct, a);
    } else {
        lv_arc_set_value(g->arc, 0);
        lv_label_set_text(g->pct, "--");
    }
    if (w->valid && w->resets) {
        fmt_span(a, sizeof a, (long)(w->resets - time(NULL)));
        snprintf(b, sizeof b, _("se renueva en %s"), a);
        lv_label_set_text(g->reset, b);
    } else {
        lv_label_set_text(g->reset, "");
    }
    lv_label_set_text(g->extra, extra ? extra : "");
}

static void chart_fill(void)
{
    if (!hist_scratch()) return;
    float *p = s_hp;
    time_t *t = s_ht;
    int n = aos_claude_history(S.chart, p, t, AOS_CLAUDE_HIST_MAX);
    const int P = 144;
    time_t now = time(NULL), span = S.chart ? 7 * 86400 : 86400, t0 = now - span;
    lv_chart_set_point_count(U.chart, P);
    int32_t *y = lv_chart_get_series_y_array(U.chart, U.ser);
    int j = 0, have = 0;
    for (int i = 0; i < P; i++) {
        time_t at = t0 + span * i / (P - 1);
        while (j < n && t[j] < at - span / P) j++;
        /* the last point at or before 'at', if it is recent enough to count */
        int k = j;
        while (k + 1 < n && t[k + 1] <= at) k++;
        if (k < n && t[k] <= at && at - t[k] <= span / P + 600) { y[i] = (int32_t)lroundf(p[k] * 10); have++; }
        else y[i] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_axis_range(U.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_refresh(U.chart);
    lv_label_set_text(U.chart_msg, have > 1 ? "" : _("La placa guarda el historial desde que inicia sesión: vuelve a mirar en un rato."));
}

static void refresh(void)
{
    aos_claude_status_t st;
    aos_claude_status(&st);
    bool in = st.state == AOS_CLAUDE_OK || st.state == AOS_CLAUDE_ERROR;
    lv_obj_set_flag(U.signin, LV_OBJ_FLAG_HIDDEN, in && st.state != AOS_CLAUDE_EXPIRED);
    lv_obj_set_flag(U.body, LV_OBJ_FLAG_HIDDEN, !in);
    char a[48], b[200];
    if (!in) {
        const char *host = aos_hal_device_name();
        const char *ip = aos_hal_net_ip();
        snprintf(b, sizeof b, _("En la computadora abrí http://%s.local/#claude (o http://%s/#claude), tocá «Iniciar sesión» y seguí los pasos."),
                 host, ip && ip[0] ? ip : "?");
        if (st.state == AOS_CLAUDE_WAITING_CODE || st.state == AOS_CLAUDE_EXCHANGING)
            snprintf(b, sizeof b, "%s", st.state == AOS_CLAUDE_EXCHANGING ? _("Verificando el código…") : _("Esperando el código que te dio la página de Claude."));
        lv_label_set_text(U.signin_txt, b);
    }
    lv_label_set_text(U.err, st.error);
    lv_obj_set_flag(U.err, LV_OBJ_FLAG_HIDDEN, !st.error[0]);
    if (st.fetched) {
        long ago = (long)(time(NULL) - st.fetched);
        fmt_span(a, sizeof a, ago);
        snprintf(b, sizeof b, _("actualizado hace %s"), a);
        lv_label_set_text(U.sub, ago < 60 ? _("recién actualizado") : b);
    } else {
        lv_label_set_text(U.sub, in ? _("pidiendo los datos…") : _("sin sesión"));
    }
    char opus[48] = "", son[48] = "", wk[100] = "";
    if (st.seven_day_opus.valid) snprintf(opus, sizeof opus, "Opus %.0f%%", (double)st.seven_day_opus.pct);
    if (st.seven_day_sonnet.valid) snprintf(son, sizeof son, "Sonnet %.0f%%", (double)st.seven_day_sonnet.pct);
    snprintf(wk, sizeof wk, "%s%s%s", opus, opus[0] && son[0] ? "  ·  " : "", son);
    char ex[48] = "";
    if (st.extra_enabled && st.extra_pct >= 0) snprintf(ex, sizeof ex, _("uso extra %.0f%%"), (double)st.extra_pct);
    gauge_fill(&U.g[0], &st.five_hour, ex);
    gauge_fill(&U.g[1], &st.seven_day, wk);
    float ph;
    if (!st.five_hour.valid) {
        lv_label_set_text(U.pace, "--");
        lv_label_set_text(U.eta, "");
    } else if (!pace(0, &ph)) {
        lv_label_set_text(U.pace, _("midiendo…"));
        lv_label_set_text(U.eta, _("El ritmo sale de lo que la placa va anotando: hace falta al menos 20 minutos de historial."));
    } else {
        snprintf(a, sizeof a, "%+.1f %% %s", (double)ph, _("por hora"));
        for (char *c = a; *c; c++) if (*c == '.') *c = ',';
        lv_label_set_text(U.pace, a);
        if (ph < 0.2f) snprintf(b, sizeof b, "%s", _("Casi sin consumo: el tope no se acerca."));
        else {
            time_t eta = time(NULL) + (time_t)((100.0f - st.five_hour.pct) / ph * 3600.0f);
            if (st.five_hour.resets && eta >= st.five_hour.resets)
                snprintf(b, sizeof b, "%s", _("A este ritmo la ventana se renueva antes de llegar al tope."));
            else {
                struct tm lt;
                localtime_r(&eta, &lt);
                fmt_span(a, sizeof a, (long)(eta - time(NULL)));
                snprintf(b, sizeof b, _("A este ritmo llegás al tope de 5 h a las %02d:%02d, en %s."), lt.tm_hour, lt.tm_min, a);
            }
        }
        lv_label_set_text(U.eta, b);
    }
    chart_fill();
}

static void chip_style(void)
{
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(U.chips[i], i == S.chart ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.chips[i], 0), i == S.chart ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
    }
}

static void chip_cb(lv_event_t *e)
{
    S.chart = (int)(intptr_t)lv_event_get_user_data(e);
    chip_style();
    refresh();
}

static void timer_cb(lv_timer_t *t)
{
    static uint32_t n;
    aos_claude_refresh_now();           /* someone is looking: every minute, not every three */
    aos_claude_status_t st;
    aos_claude_status(&st);
    if (st.seq != U.seen || ++n % 20 == 0) {     /* the countdowns move on their own */
        U.seen = st.seq;
        refresh();
    }
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    aos_claude_start();
    aos_claude_refresh_now();
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    int32_t w = U.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = lv_obj_create(root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, U.W, U.H);
    lv_obj_set_style_pad_hor(col, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(col, 8, 0);
    lv_obj_set_style_pad_bottom(col, 24, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 16, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    lv_obj_t *head = box(col, w, 72);
    lv_obj_align(aos_label(head, AOS_SYM_ROBOT, &aos_sym_44, C_CLAUDE), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(aos_label(head, _("Uso de Claude"), aos_font_title, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 60, -12);
    U.sub = aos_label(head, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.sub, LV_ALIGN_LEFT_MID, 62, 22);
    U.err = wrap_label(col, "", aos_font_caption, AOS_C_ORANGE, w);

    /* signed out */
    U.signin = card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(U.signin, 24, 0);
    lv_obj_set_flex_flow(U.signin, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.signin, 10, 0);
    aos_label(U.signin, _("Iniciá sesión con tu cuenta de Claude"), aos_font_body, AOS_C_TEXT);
    U.signin_txt = wrap_label(U.signin, "", aos_font_small, AOS_C_TEXT, w - 48);
    wrap_label(U.signin, _("La placa tiene su propio inicio de sesión: no usa ni toca el de Claude Code en ninguna computadora, y los números son los de la cuenta, uses las máquinas que uses. Los lee del mismo lugar que el /usage de Claude Code, que no es una API pública: si Anthropic la cambia, esta app avisa."),
               aos_font_caption, AOS_C_DIM, w - 48);

    /* signed in */
    U.body = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(U.body, 16, 0);
    lv_obj_set_style_pad_column(U.body, AOS_UI_PAD, 0);
    int32_t gw = U.land ? (w - 2 * AOS_UI_PAD) / 4 + 40 : (w - AOS_UI_PAD) / 2;
    gauge_build(&U.g[0], U.body, gw, _("Ventana de 5 h"));
    gauge_build(&U.g[1], U.body, gw, _("Semana"));
    int32_t rw = U.land ? w - 2 * gw - 2 * AOS_UI_PAD : w;
    lv_obj_t *right = box(U.body, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 12, 0);
    lv_obj_t *pc = card(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(pc, 22, 0);
    lv_obj_set_flex_flow(pc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pc, 8, 0);
    lv_obj_t *pr = box(pc, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pr, 12, 0);
    aos_label(pr, AOS_SYM_FIRE, &aos_sym_28, C_CLAUDE);
    aos_label(pr, _("Ritmo de la ventana de 5 h"), aos_font_small, AOS_C_DIM);
    U.pace = aos_label(pc, "--", aos_font_large, AOS_C_TEXT);
    U.eta = wrap_label(pc, "", aos_font_small, AOS_C_TEXT, rw - 44);

    lv_obj_t *hs = aos_label(right, _("HISTORIAL"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(hs, 10, 0);
    lv_obj_t *cr = box(right, rw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cr, 10, 0);
    static const char *const CH[2] = { N_("5 h, último día"), N_("Semana, 7 días") };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = box(cr, LV_SIZE_CONTENT, 60);
        lv_obj_set_style_radius(c, 30, 0);
        lv_obj_set_style_pad_hor(c, 20, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(aos_label(c, aos_tr(CH[i]), aos_font_small, AOS_C_TEXT));
        U.chips[i] = c;
    }
    U.chart = lv_chart_create(right);
    lv_obj_set_size(U.chart, rw, U.land ? 230 : 260);
    lv_obj_set_style_bg_color(U.chart, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(U.chart, 0, 0);
    lv_obj_set_style_radius(U.chart, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(U.chart, 16, 0);
    lv_obj_set_style_line_color(U.chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_type(U.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(U.chart, 5, 0);
    lv_obj_set_style_size(U.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.chart, 4, LV_PART_ITEMS);
    U.ser = lv_chart_add_series(U.chart, C_CLAUDE, LV_CHART_AXIS_PRIMARY_Y);
    U.chart_msg = wrap_label(U.chart, "", aos_font_caption, AOS_C_DIM, rw - 80);
    lv_obj_set_style_text_align(U.chart_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(U.chart_msg);
    chip_style();

    aos_claude_status_t st;
    aos_claude_status(&st);
    U.seen = st.seq;
    refresh();
    U.timer = lv_timer_create(timer_cb, 500, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    memset(&U, 0, sizeof U);
}

void aos_app_claude_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.claude", .name = "Claude", .icon = AOS_SYM_ROBOT,
            .color_a = 0xD97757, .color_b = 0xA8492B,
            .order = 210,
        },
        .create = create, .destroy = destroy,
    };
}

/* -------------------------------------------------------------------------- */
/* The home widget: the two windows as bars                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    lv_obj_t *bar[2], *val[2], *sub;
    lv_timer_t *timer;
    uint32_t seen;
} widget_t;

static void w_refresh(widget_t *w)
{
    aos_claude_status_t st;
    aos_claude_status(&st);
    const aos_claude_window_t *win[2] = { &st.five_hour, &st.seven_day };
    for (int i = 0; i < 2; i++) {
        char a[16];
        if (win[i]->valid) {
            lv_bar_set_value(w->bar[i], (int32_t)lroundf(fminf(100, fmaxf(0, win[i]->pct))), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(w->bar[i], pct_color(win[i]->pct), LV_PART_INDICATOR);
            snprintf(a, sizeof a, "%.0f %%", (double)win[i]->pct);
        } else {
            lv_bar_set_value(w->bar[i], 0, LV_ANIM_OFF);
            snprintf(a, sizeof a, "--");
        }
        lv_label_set_text(w->val[i], a);
    }
    char s[40];
    if (st.five_hour.valid && st.five_hour.resets) {
        fmt_span(s, sizeof s, (long)(st.five_hour.resets - time(NULL)));
        lv_label_set_text_fmt(w->sub, _("5 h: se renueva en %s"), s);
    } else {
        lv_label_set_text(w->sub, st.state == AOS_CLAUDE_SIGNED_OUT ? _("iniciá sesión desde el portal") : "");
    }
}

static void w_timer_cb(lv_timer_t *t)
{
    widget_t *w = lv_timer_get_user_data(t);
    static uint32_t n;
    aos_claude_status_t st;
    aos_claude_status(&st);
    if (st.seq != w->seen || ++n % 60 == 0) { w->seen = st.seq; w_refresh(w); }
}

static void w_deleted_cb(lv_event_t *e)
{
    widget_t *w = lv_event_get_user_data(e);
    lv_timer_delete(w->timer);
    free(w);
}

static void w_open_cb(lv_event_t *e) { aos_ui_open("aos.claude"); }

static void widget_create(lv_obj_t *cardo, int32_t cw, int32_t ch, const char *arg)
{
    (void)arg;
    aos_claude_start();
    widget_t *w = calloc(1, sizeof *w);
    if (!w) return;
    lv_obj_add_flag(cardo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cardo, w_open_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_align(aos_label(cardo, AOS_SYM_ROBOT, &aos_sym_28, C_CLAUDE), LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(aos_label(cardo, "Claude", aos_font_small, lv_color_white()), LV_ALIGN_TOP_LEFT, 40, 0);
    static const char *const NAME[2] = { N_("5 h"), N_("Semana") };
    int32_t iw = cw - 44, top = 52, rowh = (ch - 44 - top - 36) / 2;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *n = aos_label(cardo, aos_tr(NAME[i]), aos_font_caption, lv_color_hex(0xDDE6F0));
        lv_obj_set_pos(n, 0, top + i * rowh);
        w->val[i] = aos_label(cardo, "--", aos_font_body, lv_color_white());
        lv_obj_align(w->val[i], LV_ALIGN_TOP_RIGHT, 0, top + i * rowh - 6);
        w->bar[i] = lv_bar_create(cardo);
        lv_obj_set_size(w->bar[i], iw, 14);
        lv_obj_set_pos(w->bar[i], 0, top + i * rowh + 34);
        lv_bar_set_range(w->bar[i], 0, 100);
        lv_obj_set_style_bg_color(w->bar[i], lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(w->bar[i], LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_bg_color(w->bar[i], C_CLAUDE, LV_PART_INDICATOR);
        lv_obj_remove_flag(w->bar[i], LV_OBJ_FLAG_CLICKABLE);
    }
    w->sub = aos_label(cardo, "", aos_font_caption, lv_color_hex(0xDDE6F0));
    lv_obj_set_size(w->sub, iw, lv_font_get_line_height(aos_font_caption));
    lv_label_set_long_mode(w->sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(w->sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    aos_claude_status_t st;
    aos_claude_status(&st);
    w->seen = st.seq;
    w_refresh(w);
    w->timer = lv_timer_create(w_timer_cb, 1000, w);
    lv_obj_add_event_cb(cardo, w_deleted_cb, LV_EVENT_DELETE, w);
}

void aos_claude_widget_register(void)
{
    aos_ui_register_widget("claude", widget_create);
}
