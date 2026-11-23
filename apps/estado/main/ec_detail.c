/*
 * P4OS - Mi casa: one service.
 *
 * Four numbers (the latency now, the availability of the last day and of
 * the last week, and the certificate's days or when it was last tried), the
 * latency of the last 6 or 24 hours from the five-minute buckets with a
 * strip under it that says how each stretch went (green all good, yellow
 * some failures, red nothing answered, grey no data), the last check in
 * words, and the last outages. Upright it is one column that scrolls; lying
 * down the chart and the numbers go left and the rest right.
 */
#include "ec.h"

#include <stdlib.h>

#define A ec_app
#define STRIP_MAX 96

static struct {
    lv_obj_t *state, *stat[4], *stat_l[4];
    lv_obj_t *chart, *ymax, *x0, *empty;
    lv_chart_series_t *ser;
    lv_obj_t *strip, *seg[STRIP_MAX];
    int       nseg;
    lv_obj_t *range_b[2];
    lv_obj_t *last, *cert, *trust, *outs, *pause_l;
    uint32_t  seq, last_s, slot;
    int       out_n;
    uint32_t  out_sig;
} V;

static int s_range;     /* 0: 6 hours, 1: 24 hours; kept between visits */

/* ---- filling in ----------------------------------------------------------------------------------- */

static void fill_chart(const ec_hist_t *h, uint32_t e)
{
    int npts = s_range ? EC_B5_N : 72;
    lv_chart_set_point_count(V.chart, (uint32_t)npts);
    int32_t *y = lv_chart_get_series_y_array(V.chart, V.ser);
    uint32_t now5 = e / 300;
    int maxv = 0, have = 0;
    for (int k = 0; k < npts; k++) {
        const ec_bk_t *b = h && e ? ec_hist_b5(h, now5 - (uint32_t)(npts - 1 - k)) : NULL;
        if (b && b->ok) {
            y[k] = b->avg_ms;
            if (b->avg_ms > maxv) maxv = b->avg_ms;
            have++;
        } else {
            y[k] = LV_CHART_POINT_NONE;
        }
    }
    /* a round top: 10, 20, 50, 100, 200, 500... */
    static const int TOPS[] = { 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 70000 };
    int top = TOPS[0];
    for (size_t k = 0; k < sizeof TOPS / sizeof TOPS[0] && TOPS[k] * 10 < maxv * 11; k++)
        top = k + 1 < sizeof TOPS / sizeof TOPS[0] ? TOPS[k + 1] : TOPS[k];
    lv_chart_set_axis_range(V.chart, LV_CHART_AXIS_PRIMARY_Y, 0, top);
    lv_obj_set_style_size(V.chart, have < 40 ? 8 : 0, have < 40 ? 8 : 0, LV_PART_INDICATOR);
    lv_chart_refresh(V.chart);
    char b[32];
    ec_fmt_ms(b, sizeof b, top);
    ec_set_text(V.ymax, b);
    ec_set_text(V.x0, s_range ? _("hace 24 h") : _("hace 6 h"));
    if (have) lv_obj_add_flag(V.empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(V.empty, LV_OBJ_FLAG_HIDDEN);

    /* the strip: each segment is 'per' buckets */
    int per = s_range ? 3 : 1;
    int nseg = npts / per;
    for (int s = 0; s < V.nseg; s++) {
        if (s >= nseg) {
            lv_obj_add_flag(V.seg[s], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(V.seg[s], LV_OBJ_FLAG_HIDDEN);
        uint32_t n = 0, ok = 0;
        for (int k = 0; k < per; k++) {
            const ec_bk_t *bk = h && e ? ec_hist_b5(h, now5 - (uint32_t)(npts - 1 - (s * per + k))) : NULL;
            if (bk) {
                n += bk->n;
                ok += bk->ok;
            }
        }
        lv_color_t c = !n ? lv_color_hex(0x3A3A3C) : ok == n ? AOS_C_GREEN : ok ? AOS_C_YELLOW : AOS_C_RED;
        lv_obj_set_style_bg_color(V.seg[s], c, 0);
    }
    lv_obj_t *strip = V.strip;
    int32_t sw = lv_obj_get_width(strip);
    for (int s = 0; s < nseg && s < V.nseg; s++) {
        int32_t x0 = sw * s / nseg, x1 = sw * (s + 1) / nseg;
        lv_obj_set_pos(V.seg[s], x0, 0);
        lv_obj_set_width(V.seg[s], x1 - x0 > 2 ? x1 - x0 - 2 : 1);
    }
}

static void fill_outages(const ec_hist_t *h)
{
    uint32_t sig = h ? (uint32_t)h->out_n * 31u : 0;
    if (h)
        for (int k = 0; k < h->out_n; k++) sig = sig * 17u + h->out[k].start + h->out[k].end * 3u;
    if (sig == V.out_sig && V.out_n >= 0) return;
    V.out_sig = sig;
    lv_obj_clean(V.outs);
    int32_t w = lv_obj_get_content_width(V.outs);
    V.out_n = h ? h->out_n : 0;
    if (!V.out_n) {
        ec_text(V.outs, _("Sin cortes registrados"), aos_font_small, AOS_C_DIM, w);
        return;
    }
    for (int k = h->out_n - 1; k >= 0; k--) {
        const ec_out_t *o = &h->out[k];
        char when[48], dur[32], b[160];
        ec_fmt_when(when, sizeof when, o->start);
        lv_obj_t *r = ec_box(V.outs, w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_ver(r, 6, 0);
        if (o->end) {
            ec_fmt_dur(dur, sizeof dur, o->end - o->start);
            snprintf(b, sizeof b, "%s · %s", when, dur);
        } else {
            snprintf(b, sizeof b, _("%s · sigue caído"), when);
        }
        ec_text(r, b, aos_font_small, o->end ? AOS_C_TEXT : AOS_C_RED, w);
        ec_text(r, ec_err_text(o->err), aos_font_caption, AOS_C_DIM, w);
    }
}

static void fill(void)
{
    int i = A.detail;
    const ec_svc_t *s = &A.cfg.s[i];
    uint32_t now = (uint32_t)aos_hal_uptime_ms(), e = ec_now_epoch();
    ec_lock();
    ec_rt_t rt = *ec_rt(i);
    const ec_hist_t *h = ec_hist(i);
    int dl = 0;
    bool can_trust = ec_rt_der(i, &dl) != NULL;
    char b[200], d[64], ago[48];

    /* the state */
    lv_color_t col = ec_state_color(rt.state);
    if (rt.state == EC_ST_DOWN) {
        ec_fmt_dur(ago, sizeof ago, (now - rt.since_ms) / 1000u);
        snprintf(b, sizeof b, _("Caído desde hace %s"), ago);
    } else if (rt.state == EC_ST_UP || rt.state == EC_ST_SLOW) {
        ec_fmt_dur(ago, sizeof ago, (now - rt.since_ms) / 1000u);
        snprintf(b, sizeof b, rt.state == EC_ST_SLOW ? _("Lento, anda desde hace %s") : _("Anda desde hace %s"), ago);
    } else {
        ec_copy(b, sizeof b, ec_state_name(rt.state));
    }
    ec_set_text(V.state, b);
    lv_obj_set_style_text_color(V.state, col, 0);

    /* the numbers */
    ec_fmt_ms(b, sizeof b, rt.ms);
    ec_set_text(V.stat[0], b);
    lv_obj_set_style_text_color(V.stat[0], rt.state == EC_ST_SLOW ? AOS_C_YELLOW : AOS_C_TEXT, 0);
    float a24 = h && e ? ec_hist_avail5(h, EC_B5_N, e / 300) : -1;
    float a7 = h && e ? ec_hist_availh(h, EC_H_N, e / 3600) : -1;
    for (int k = 0; k < 2; k++) {
        float a = k ? a7 : a24;
        ec_fmt_pct(b, sizeof b, a);
        ec_set_text(V.stat[1 + k], b);
        lv_obj_set_style_text_color(V.stat[1 + k], a < 0 ? AOS_C_DIM : a >= 99.5f ? AOS_C_GREEN : a >= 95 ? AOS_C_YELLOW : AOS_C_RED, 0);
    }
    bool https = s->kind == EC_HTTP && !strncasecmp(s->url, "https://", 8);
    if (https) {
        ec_set_text(V.stat_l[3], _("Certificado"));
        if (rt.cert_state == EC_CERT_OK || rt.cert_state == EC_CERT_SOON) snprintf(b, sizeof b, _("%d días"), rt.cert_days);
        else if (rt.cert_state == EC_CERT_EXPIRED) ec_copy(b, sizeof b, _("Venció"));
        else ec_copy(b, sizeof b, "--");
        lv_obj_set_style_text_color(V.stat[3], rt.cert_state == EC_CERT_SOON ? AOS_C_ORANGE : rt.cert_state == EC_CERT_EXPIRED ? AOS_C_RED : AOS_C_TEXT, 0);
    } else {
        ec_set_text(V.stat_l[3], _("Conexión"));
        ec_fmt_ms(b, sizeof b, rt.conn_ms);
        lv_obj_set_style_text_color(V.stat[3], AOS_C_TEXT, 0);
    }
    ec_set_text(V.stat[3], b);

    /* the last check */
    if (!rt.checked_ms) {
        ec_copy(b, sizeof b, s->paused ? _("En pausa: no se prueba") : _("Todavía no se probó"));
    } else {
        ec_fmt_dur(ago, sizeof ago, (now - rt.checked_ms) / 1000u);
        if (rt.err == EC_E_NONE) {
            char ms[24], cm[24];
            ec_fmt_ms(ms, sizeof ms, rt.ms);
            ec_fmt_ms(cm, sizeof cm, rt.conn_ms);
            if (s->kind == EC_TCP) snprintf(b, sizeof b, _("Último chequeo hace %s: el puerto abre en %s"), ago, ms);
            else snprintf(b, sizeof b, _("Último chequeo hace %s: %d, contestó en %s (conectar: %s)"), ago, rt.code, ms, cm);
        } else {
            if (rt.err == EC_E_CODE) snprintf(d, sizeof d, _("contestó %d"), rt.code);
            else ec_copy(d, sizeof d, ec_err_text(rt.err));
            snprintf(b, sizeof b, _("Último chequeo hace %s: %s"), ago, d);
            if (rt.why[0]) {
                size_t k = strlen(b);
                snprintf(b + k, sizeof b - k, " (%.80s)", rt.why);
            }
        }
    }
    ec_set_text(V.last, b);
    lv_obj_set_style_text_color(V.last, rt.err ? AOS_C_ORANGE : AOS_C_TEXT, 0);

    /* the certificate */
    b[0] = '\0';
    if (https) {
        if (rt.cert_state == EC_CERT_UNKNOWN) ec_copy(b, sizeof b, _("No se pudo leer el certificado (el servidor sólo habla TLS 1.3, o no es HTTPS)."));
        else if (rt.cert_state != EC_CERT_NONE) {
            char nm[96];
            aos_text_safe(nm, sizeof nm, rt.cert_name[0] ? rt.cert_name : "?");
            if (rt.cert_state == EC_CERT_EXPIRED) snprintf(b, sizeof b, _("Certificado de %s: venció."), nm);
            else snprintf(b, sizeof b, _("Certificado de %s: vence en %d días."), nm, rt.cert_days);
            if (rt.cert_self) {
                size_t k = strlen(b);
                snprintf(b + k, sizeof b - k, " %s", _("Lo firmó él mismo."));
            }
            if (s->trust) {
                size_t k = strlen(b);
                snprintf(b + k, sizeof b - k, " %s", _("Se confía en él."));
            }
        }
    }
    ec_set_text(V.cert, b);
    if (b[0]) lv_obj_remove_flag(V.cert, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(V.cert, LV_OBJ_FLAG_HIDDEN);
    if (can_trust && rt.err == EC_E_TLS) lv_obj_remove_flag(V.trust, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(V.trust, LV_OBJ_FLAG_HIDDEN);

    ec_set_text(V.pause_l, s->paused ? _("Seguir") : _("Pausar"));
    if (V.seq != rt.seq || V.slot != e / 300) {
        V.slot = e / 300;
        fill_chart(h, e);
    }
    fill_outages(h);
    ec_unlock();
    V.seq = rt.seq;
}

/* ---- the buttons ----------------------------------------------------------------------------------- */

static void back_cb(lv_event_t *e)
{
    (void)e;
    ec_go_list();
}

static void edit_cb(lv_event_t *e)
{
    (void)e;
    ec_go_edit(A.detail);
}

static void now_cb(lv_event_t *e)
{
    (void)e;
    ec_core_check_now(A.detail);
    aos_ui_toast(_("Probando"), 1200);
}

static void pause_cb(lv_event_t *e)
{
    (void)e;
    ec_svc_t *s = &A.cfg.s[A.detail];
    s->paused = !s->paused;
    ec_cfg_commit();
    V.seq = 0;
    fill();
}

static void trust_cb(lv_event_t *e)
{
    (void)e;
    int i = A.detail;
    if (!ec_cert_trust(i)) {
        aos_ui_toast(_("No se pudo guardar el certificado"), 2500);
        return;
    }
    A.cfg.s[i].trust = true;
    ec_cfg_commit();
    ec_core_check_now(i);
    aos_ui_toast(_("Listo: se confía en este certificado"), 2500);
}

static void range_cb(lv_event_t *e)
{
    s_range = (int)(intptr_t)lv_event_get_user_data(e);
    for (int k = 0; k < 2; k++) {
        lv_obj_set_style_bg_color(V.range_b[k], k == s_range ? AOS_C_TEXT : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(V.range_b[k], 0), k == s_range ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
    }
    V.seq = 0;
    fill();
}

/* ---- building ------------------------------------------------------------------------------------- */

static lv_obj_t *section(lv_obj_t *parent, int32_t w, const char *title)
{
    lv_obj_t *c = ec_card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, 24, 0);
    lv_obj_set_style_pad_all(c, 22, 0);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    if (title) ec_text(c, title, aos_font_small, AOS_C_DIM, w - 44);
    return c;
}

static void stats(lv_obj_t *parent, int32_t w, int cols)
{
    static const char *const L[4] = { N_("Latencia"), N_("En 24 h"), N_("En 7 días"), N_("Certificado") };
    lv_obj_t *g = ec_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, 14, 0);
    lv_obj_set_style_pad_row(g, 14, 0);
    int32_t bw = (w - (cols - 1) * 14) / cols;
    for (int k = 0; k < 4; k++) {
        lv_obj_t *c = ec_card(g, bw, 116);
        lv_obj_set_style_radius(c, 22, 0);
        V.stat_l[k] = ec_text(c, _(L[k]), aos_font_caption, AOS_C_DIM, bw - 40);
        lv_obj_set_pos(V.stat_l[k], 20, 16);
        V.stat[k] = ec_text(c, "--", aos_font_title, AOS_C_TEXT, bw - 40);
        lv_obj_set_pos(V.stat[k], 20, 52);
    }
}

static void chart_card(lv_obj_t *parent, int32_t w, int32_t ch)
{
    lv_obj_t *c = section(parent, w, NULL);
    int32_t iw = w - 44;
    lv_obj_t *head = ec_box(c, iw, 56);
    ec_text(head, _("Latencia"), aos_font_small, AOS_C_DIM, 200);
    lv_obj_align(lv_obj_get_child(head, 0), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *seg = ec_box(head, 220, 52);
    lv_obj_align(seg, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, 8, 0);
    static const char *const R[2] = { "6 h", "24 h" };
    for (int k = 0; k < 2; k++) {
        lv_obj_t *b = ec_box(seg, 104, 52);
        lv_obj_set_style_radius(b, 26, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, range_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        lv_obj_t *l = aos_label(b, R[k], aos_font_small, AOS_C_TEXT);
        lv_obj_center(l);
        aos_make_decorative(l);
        V.range_b[k] = b;
    }
    lv_obj_t *area = ec_box(c, iw, ch);
    V.chart = lv_chart_create(area);
    lv_obj_set_size(V.chart, iw, ch);
    lv_obj_set_style_bg_opa(V.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(V.chart, 0, 0);
    lv_obj_set_style_pad_all(V.chart, 4, 0);
    lv_obj_set_style_line_color(V.chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_type(V.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(V.chart, 4, 0);
    lv_obj_set_style_line_width(V.chart, 4, LV_PART_ITEMS);
    V.ser = lv_chart_add_series(V.chart, AOS_C_TEAL, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_point_count(V.chart, 72);
    lv_chart_set_all_values(V.chart, V.ser, LV_CHART_POINT_NONE);
    aos_make_decorative(V.chart);
    V.ymax = ec_text(area, "", aos_font_caption, AOS_C_DIM, 0);
    lv_obj_align(V.ymax, LV_ALIGN_TOP_LEFT, 6, 4);
    V.empty = aos_label(area, _("Sin datos todavía"), aos_font_small, AOS_C_DIM);
    lv_obj_center(V.empty);
    aos_make_decorative(V.empty);

    V.strip = ec_box(c, iw, 16);
    V.nseg = STRIP_MAX;
    for (int s = 0; s < V.nseg; s++) {
        V.seg[s] = ec_box(V.strip, 4, 16);
        lv_obj_set_style_radius(V.seg[s], 3, 0);
        lv_obj_set_style_bg_opa(V.seg[s], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(V.seg[s], lv_color_hex(0x3A3A3C), 0);
    }
    aos_make_decorative(V.strip);
    lv_obj_t *xs = ec_box(c, iw, 28);
    V.x0 = ec_text(xs, "", aos_font_caption, AOS_C_DIM, 0);
    lv_obj_align(V.x0, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *x1 = ec_text(xs, _("ahora"), aos_font_caption, AOS_C_DIM, 0);
    lv_obj_align(x1, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void check_card(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *c = section(parent, w, NULL);
    int32_t iw = w - 44;
    V.last = aos_label(c, "", aos_font_small, AOS_C_TEXT);
    lv_label_set_long_mode(V.last, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(V.last, iw);
    V.cert = aos_label(c, "", aos_font_small, AOS_C_DIM);
    lv_label_set_long_mode(V.cert, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(V.cert, iw);
    lv_obj_t *btns = ec_box(c, iw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(btns, 12, 0);
    lv_obj_set_style_pad_row(btns, 12, 0);
    lv_obj_set_style_pad_top(btns, 8, 0);
    ec_pill(btns, AOS_SYM_UPDATE, _("Probar ahora"), AOS_C_ACCENT, now_cb, NULL);
    lv_obj_t *p = ec_pill(btns, AOS_SYM_PAUSE, _("Pausar"), AOS_C_CARD2, pause_cb, NULL);
    V.pause_l = lv_obj_get_child(p, 1);
    V.trust = ec_pill(btns, AOS_SYM_SHIELD_CHECK, _("Confiar en este certificado"), lv_color_hex(0x8A5A00), trust_cb, NULL);
    lv_obj_add_flag(V.trust, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(V.cert, LV_OBJ_FLAG_HIDDEN);
}

static void outages_card(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *c = section(parent, w, _("Últimos cortes"));
    V.outs = ec_box(c, w - 44, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(V.outs, LV_FLEX_FLOW_COLUMN);
    V.out_n = -1;
}

static lv_obj_t *column(lv_obj_t *parent, int32_t x, int32_t w, int32_t h)
{
    lv_obj_t *col = ec_box(parent, w, h);
    lv_obj_set_pos(col, x, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 16, 0);
    lv_obj_set_style_pad_bottom(col, 40, 0);
    return col;
}

void ec_detail_build(void)
{
    memset(&V, 0, sizeof V);
    int i = A.detail;
    const ec_svc_t *s = &A.cfg.s[i];
    const int32_t head = A.land ? 116 : 150;

    lv_obj_t *page = ec_box(A.root, A.W, A.H);
    lv_obj_t *bk = ec_round_btn(page, AOS_SYM_CHEVRON_LEFT, back_cb, NULL);
    lv_obj_set_pos(bk, AOS_UI_PAD - 8, A.land ? 12 : 20);
    lv_obj_t *ed = ec_round_btn(page, AOS_SYM_PENCIL, edit_cb, NULL);
    lv_obj_align(ed, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, A.land ? 12 : 20);
    char name[EC_NAME * 2], addr[EC_URL + 40], line[EC_URL + 80];
    aos_text_safe(name, sizeof name, s->name);
    int32_t tx = AOS_UI_PAD - 8 + AOS_UI_TAP_MIN + 16, tw = A.W - tx - AOS_UI_PAD - AOS_UI_TAP_MIN - 16;
    lv_obj_t *t = ec_text(page, name, aos_font_title, AOS_C_TEXT, tw);
    lv_obj_set_pos(t, tx, A.land ? 10 : 18);
    V.state = ec_text(page, "", aos_font_small, AOS_C_DIM, tw);
    lv_obj_set_pos(V.state, tx, A.land ? 56 : 64);
    ec_svc_addr(s, addr, sizeof addr);
    char every[32];
    ec_fmt_dur(every, sizeof every, (uint32_t)s->every_s);
    snprintf(line, sizeof line, _("%s · cada %s"), addr, every);
    lv_obj_t *al = ec_text(page, line, aos_font_caption, AOS_C_DIM, A.land ? tw : A.W - 2 * AOS_UI_PAD);
    lv_obj_set_pos(al, A.land ? tx : AOS_UI_PAD, A.land ? 86 : 112);

    int32_t inner = A.W - 2 * AOS_UI_PAD;
    if (A.land) {
        int32_t lw = inner * 55 / 100, rw = inner - lw - 20;
        lv_obj_t *l = column(page, AOS_UI_PAD, lw, A.H - head);
        lv_obj_set_y(l, head);
        lv_obj_t *r = column(page, AOS_UI_PAD + lw + 20, rw, A.H - head);
        lv_obj_set_y(r, head);
        /* left the chart and, under it, the outages; right the numbers and the last check */
        chart_card(l, lw, 250);
        outages_card(l, lw);
        stats(r, rw, 2);
        check_card(r, rw);
    } else {
        lv_obj_t *col = column(page, AOS_UI_PAD, inner, A.H - head);
        lv_obj_set_y(col, head);
        stats(col, inner, 2);
        chart_card(col, inner, 300);
        check_card(col, inner);
        outages_card(col, inner);
    }
    lv_obj_update_layout(page);
    /* the range buttons in their colours, and everything filled */
    for (int k = 0; k < 2; k++) {
        lv_obj_set_style_bg_color(V.range_b[k], k == s_range ? AOS_C_TEXT : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(V.range_b[k], 0), k == s_range ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
    }
    V.seq = 0;
    fill();
}

void ec_detail_tick(void)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    ec_lock();
    uint32_t seq = ec_rt(A.detail)->seq;
    ec_unlock();
    if (seq == V.seq && now / 1000u == V.last_s) return;
    V.last_s = now / 1000u;
    fill();
}

bool ec_detail_back(void)
{
    return false;
}
