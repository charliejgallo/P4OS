/*
 * P4OS - Bolsa: one symbol.
 *
 * Upright: the price and the day's change, the big chart, the ranges
 * (1D to all), the tools (line, area or candles; volume, the 20/50
 * averages, the RSI; comparing), and the numbers: open, high, low, the
 * previous close, the volume and where the price sits in its 52 weeks.
 *
 * Lying down the chart takes the screen: a slim bar on top with the price
 * and the ranges, the tools in a strip at the bottom.
 *
 * A finger on the chart replaces the day's change with what it reads; two
 * fingers, with the change between the two dates.
 */
#include "bs.h"

static const struct { const char *name; int32_t span; uint8_t res; } RANGES[] = {
    { "1D", 0, BS_R_DAY },
    { N_("5D"), 7 * 86400, BS_R_WEEK },
    { N_("1M"), 31 * 86400, BS_R_YEAR },
    { N_("6M"), 183 * 86400, BS_R_YEAR },
    { N_("1A"), 366 * 86400, BS_R_YEAR },
    { N_("5A"), 5 * 366 * 86400, BS_R_FULL },
    { N_("Máx"), 0, BS_R_FULL },
};
#define NRANGES 7

static const char *TYPE_NAME[BS_CT_COUNT] = { N_("Línea"), N_("Área"), N_("Velas") };

static struct {
    bs_info_t info;
    bs_chart_t *chart;
    lv_obj_t *price, *ccy, *chg, *read, *when, *foot, *chips, *tools, *stats, *bar52, *mark52, *alert_lbl;
    lv_obj_t *range_btn[NRANGES];
    lv_obj_t *type_btn, *vol_btn, *ma_btn, *rsi_btn, *cmp_btn;
    lv_obj_t *st_val[8];
    lv_obj_t *lo52, *hi52;
    char cmp[BS_MAX_CMP][BS_SYM_LEN];
    int ncmp;
    bool reading;
    uint32_t last_when;
    char t_price[32], t_chg[64], t_when[96];
} D;

static void apply(void);
static void tools_build(void);

/* ---- the price ---- */

/* the provider's time is a real time (not only a date) */
static bool has_intraday_time(int prov)
{
    return prov == BS_P_COINGECKO || prov == BS_P_TWELVE || prov == BS_P_FINNHUB;
}

static void head_paint(void)
{
    if (!D.price) return;
    bs_item_t *it = bs_item(D.info.sym);
    bs_quote_t q;
    char p[32], c[32], pc[32], t[96];
    if (it && bs_item_quote(it, &q)) {
        bs_fmt_price(q.price, &D.info, p, sizeof p);
        if (strcmp(p, D.t_price)) { bs_scpy(D.t_price, sizeof D.t_price, p); lv_label_set_text(D.price, p); }
        if ((q.has & BS_Q_PREV) && q.prev > 0) {
            float chg = q.price - q.prev;
            bs_fmt_chg(chg, &D.info, c, sizeof c);
            bs_fmt_pct(chg / q.prev * 100, pc, sizeof pc);
            snprintf(t, sizeof t, "%s  (%s)  %s", c, pc, _("hoy"));
            lv_obj_set_style_text_color(D.chg, chg > 0 ? BS_C_UP : chg < 0 ? BS_C_DOWN : AOS_C_DIM, 0);
        } else {
            bs_scpy(t, sizeof t, "");
        }
        if (strcmp(t, D.t_chg)) { bs_scpy(D.t_chg, sizeof D.t_chg, t); lv_label_set_text(D.chg, t); }
        char when[48], ago[32];
        int32_t qt = q.t ? q.t : q.fetched;
        bool daily = !has_intraday_time(q.prov) && q.t;
        if (daily) bs_fmt_date(qt, false, when, sizeof when);
        else bs_fmt_date(qt, true, when, sizeof when);
        bs_fmt_ago(q.fetched, ago, sizeof ago);
        if (ago[0]) snprintf(t, sizeof t, _("Dato del %s · %s · consultado %s"), when, bs_prov_name((bs_prov_t)q.prov), ago);
        else snprintf(t, sizeof t, _("Dato del %s · %s · de la tarjeta"), when, bs_prov_name((bs_prov_t)q.prov));
    } else {
        lv_label_set_text(D.price, "--");
        lv_label_set_text(D.chg, "");
        D.t_price[0] = D.t_chg[0] = 0;
        bs_prov_t pq = bs_sym_prov_quote(&D.info);
        if (pq == BS_P_NONE) snprintf(t, sizeof t, "%s", _("Necesita una clave de un proveedor: se carga en la página del portal"));
        else if (it && it->err) snprintf(t, sizeof t, "%s", bs_net_last_error());
        else snprintf(t, sizeof t, _("Consultando %s…"), bs_prov_name(pq));
    }
    if (strcmp(t, D.t_when)) { bs_scpy(D.t_when, sizeof D.t_when, t); lv_label_set_text(D.when, t); }
    if (D.ccy) {
        const char *u = D.info.kind == BS_K_RISK ? _("puntos básicos") : D.info.ccy;
        lv_label_set_text(D.ccy, u);
        lv_obj_align_to(D.ccy, D.price, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -8);
    }
}

static void stats_paint(void)
{
    if (!D.stats) return;
    bs_item_t *it = bs_item(D.info.sym);
    bs_quote_t q;
    bool ok = it && bs_item_quote(it, &q);
    char v[8][32];
    for (int i = 0; i < 8; i++) bs_scpy(v[i], sizeof v[i], "--");
    if (ok) {
        if (q.has & BS_Q_OHL) {
            bs_fmt_price(q.open, &D.info, v[0], sizeof v[0]);
            bs_fmt_price(q.high, &D.info, v[1], sizeof v[1]);
            bs_fmt_price(q.low, &D.info, v[2], sizeof v[2]);
        }
        if (q.has & BS_Q_PREV) bs_fmt_price(q.prev, &D.info, v[3], sizeof v[3]);
        if ((q.has & BS_Q_VOL) && q.vol > 0) bs_fmt_big(q.vol, v[4], sizeof v[4]);
        if (q.has & BS_Q_52W) {
            bs_fmt_price(q.lo52, &D.info, v[5], sizeof v[5]);
            bs_fmt_price(q.hi52, &D.info, v[6], sizeof v[6]);
        }
    }
    for (int i = 0; i < 5; i++) if (D.st_val[i]) lv_label_set_text(D.st_val[i], v[i]);
    if (D.lo52) lv_label_set_text(D.lo52, v[5]);
    if (D.hi52) lv_label_set_text(D.hi52, v[6]);
    if (D.mark52 && D.bar52) {
        if (ok && (q.has & BS_Q_52W) && q.hi52 > q.lo52) {
            lv_obj_update_layout(D.bar52);
            int32_t bw = lv_obj_get_width(D.bar52);
            float f = (q.price - q.lo52) / (q.hi52 - q.lo52);
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            lv_obj_remove_flag(D.mark52, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_x(D.mark52, (int32_t)(f * (float)(bw - 20)));
        } else {
            lv_obj_add_flag(D.mark52, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (D.alert_lbl) {
        bs_cfg_t *c = bs_cfg();
        char t[160] = "";
        size_t w = 0;
        int n = 0;
        for (int i = bs_alert_find(D.info.sym, 0); i >= 0 && w < sizeof t; i = bs_alert_find(D.info.sym, i + 1)) {
            char a[32];
            bs_fmt_price(c->alert[i].value, &D.info, a, sizeof a);
            w += (size_t)snprintf(t + w, sizeof t - w, "%s%s %s%s", n ? " · " : "",
                                  c->alert[i].dir == BS_AL_ABOVE ? "≥" : "≤", a,
                                  c->alert[i].fired ? _(" (avisado)") : "");
            n++;
        }
        if (!n) bs_scpy(t, sizeof t, _("Sin avisos de precio"));
        lv_label_set_text(D.alert_lbl, t);
    }
}

/* ---- the chart ---- */

static int slice(const bs_series_t *s, int range, int *off)
{
    *off = 0;
    if (!s || s->n < 1) return 0;
    int32_t last = s->p[s->n - 1].t;
    int32_t from;
    if (range == 0) {
        /* the day: the last session, or the last 24 h around the clock */
        from = last - (D.info.kind == BS_K_CRYPTO ? 86400 : 18 * 3600);
    } else if (RANGES[range].span) {
        from = last - RANGES[range].span;
    } else {
        return s->n;
    }
    *off = bs_series_find(s, from + 1);
    return s->n - *off;
}

static void apply(void)
{
    if (!D.chart) return;
    tools_build();
    bs_cfg_t *c = bs_cfg();
    int range = c->range;
    bs_item_t *it = bs_item(D.info.sym);
    if (!it) return;
    bs_prov_t ph = bs_sym_prov_hist(&D.info);
    if (ph == BS_P_NONE) {
        bs_chart_set(D.chart, &D.info, NULL, 0, false, false, 0, false);
        bs_chart_message(D.chart, _("Para ver el gráfico de acciones e índices hace falta la clave gratis de un proveedor (Stooq, Twelve Data o Alpha Vantage), que se escribe en la página Bolsa del portal."));
        return;
    }
    int res = RANGES[range].res;
    bs_series_t *s = bs_item_series(it, res, true);
    const char *note = NULL;
    if (!s && res == BS_R_DAY) {
        bs_chart_set(D.chart, &D.info, NULL, 0, false, false, 0, false);
        char m[200];
        snprintf(m, sizeof m, _("%s no da datos de a minutos: probá 5D o más."), bs_prov_name(ph));
        bs_chart_message(D.chart, m);
        return;
    }
    if (!s && res == BS_R_WEEK) s = bs_item_series(it, BS_R_YEAR, true);
    if (!s) s = bs_item_series(it, BS_R_YEAR, true);
    if (s && res == BS_R_FULL && ph == BS_P_COINGECKO) note = _("Sin clave, CoinGecko da hasta un año.");
    if (!s || s->n < 2) {
        bs_chart_set(D.chart, &D.info, NULL, 0, false, false, 0, false);
        bs_chart_message(D.chart, it->err ? bs_net_last_error() : _("Cargando…"));
        return;
    }
    int off = 0;
    int n = slice(s, range, &off);
    if (n < 2 && s->n >= 2) { off = s->n - 2; n = 2; }
    bs_quote_t q;
    float ref = 0;
    if (range == 0 && bs_item_quote(it, &q) && (q.has & BS_Q_PREV)) ref = q.prev;
    bs_chart_set(D.chart, &D.info, s->p + off, n, s->ohlc, s->vol, ref, res <= BS_R_WEEK && s == &it->s[res]);
    bs_chart_set_full(D.chart, s->p, s->n, off);
    bs_chart_cmp_clear(D.chart);
    for (int k = 0; k < D.ncmp; k++) {
        bs_item_t *ci = bs_item(D.cmp[k]);
        if (!ci) continue;
        bs_series_t *cs = bs_item_series(ci, res, true);
        if (!cs && res <= BS_R_WEEK) cs = bs_item_series(ci, BS_R_YEAR, true);
        if (!cs || cs->n < 2) continue;
        int32_t from = s->p[off].t - 86400;
        int coff = bs_series_find(cs, from);
        if (cs->n - coff >= 2) bs_chart_cmp_add(D.chart, ci->info.show, cs->p + coff, cs->n - coff);
    }
    int type = c->chart_type;
    if (type == BS_CT_CANDLE && !bs_chart_candles_ok(D.chart)) type = BS_CT_LINE;
    bs_chart_options(D.chart, type, c->show_vol, c->show_ma, c->show_rsi);
    if (note) bs_chart_message(D.chart, note);
    tools_build();
}

static void read_cb(const char *text, lv_color_t col, bool active)
{
    if (!D.read) return;
    D.reading = active;
    if (active) {
        lv_label_set_text(D.read, text);
        lv_obj_set_style_text_color(D.read, col, 0);
        lv_obj_remove_flag(D.read, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(D.chg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(D.read, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(D.chg, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- the buttons ---- */

static void ranges_paint(void)
{
    int cur = bs_cfg()->range;
    for (int i = 0; i < NRANGES; i++) {
        lv_obj_t *b = D.range_btn[i];
        if (!b) continue;
        lv_obj_set_style_bg_color(b, i == cur ? AOS_C_TEXT : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), i == cur ? AOS_C_BG : AOS_C_TEXT, 0);
    }
}

static void range_cb(lv_event_t *e)
{
    bs_cfg()->range = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    bs_cfg_save();
    ranges_paint();
    apply();
}

static void toggle_paint(lv_obj_t *b, bool on, bool enabled)
{
    if (!b) return;
    lv_obj_set_style_bg_color(b, on ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_opa(b, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void tools_build(void)
{
    bs_cfg_t *c = bs_cfg();
    if (D.type_btn) {
        int type = c->chart_type;
        bool candles = D.chart && bs_chart_candles_ok(D.chart);
        char t[32];
        snprintf(t, sizeof t, "%s", _(TYPE_NAME[type]));
        lv_label_set_text(lv_obj_get_child(D.type_btn, 0), t);
        toggle_paint(D.type_btn, false, type != BS_CT_CANDLE || candles);
    }
    toggle_paint(D.vol_btn, c->show_vol, true);
    toggle_paint(D.ma_btn, c->show_ma, D.ncmp == 0);
    toggle_paint(D.rsi_btn, c->show_rsi, D.ncmp == 0);
    toggle_paint(D.cmp_btn, D.ncmp > 0, true);
}

static void type_cb(lv_event_t *e)
{
    (void)e;
    bs_cfg_t *c = bs_cfg();
    c->chart_type = (uint8_t)((c->chart_type + 1) % BS_CT_COUNT);
    if (c->chart_type == BS_CT_CANDLE && D.chart && !bs_chart_candles_ok(D.chart)) {
        aos_ui_toast(_("Velas: esta fuente da sólo cierres en este rango"), 1800);
    }
    bs_cfg_save();
    apply();
}

static void opt_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    bs_cfg_t *c = bs_cfg();
    if (k == 0) c->show_vol = !c->show_vol;
    else if (k == 1) c->show_ma = !c->show_ma;
    else c->show_rsi = !c->show_rsi;
    bs_cfg_save();
    apply();
}

/* ---- comparing ---- */

static void cmp_pick(lv_event_t *e);

static void cmp_sheet_fill(lv_obj_t *col)
{
    lv_obj_clean(col);
    lv_obj_update_layout(col);          /* the width is only known after a layout */
    int32_t w = lv_obj_get_width(col) - 4;
    bs_cfg_t *c = bs_cfg();
    lv_obj_t *h = bo_label(col, _("Elegí hasta dos para ver todo en % desde el comienzo del rango."), aos_font_small, AOS_C_DIM);
    lv_obj_set_width(h, w);
    lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
    static char seen[BS_MAX_ITEMS][BS_SYM_LEN];
    int nseen = 0;
    for (int l = 0; l < c->nlists; l++) {
        for (int i = 0; i < c->list[l].n; i++) {
            const char *s = c->list[l].sym[i];
            if (!strcmp(s, D.info.sym)) continue;
            bool dup = false;
            for (int k = 0; k < nseen; k++) dup |= !strcmp(seen[k], s);
            if (dup || nseen >= BS_MAX_ITEMS) continue;
            bs_scpy(seen[nseen], BS_SYM_LEN, s);
            bs_info_t in;
            bs_sym_info(s, &in);
            bool on = false;
            for (int k = 0; k < D.ncmp; k++) on |= !strcmp(D.cmp[k], s);
            lv_obj_t *r = lv_obj_create(col);
            lv_obj_remove_style_all(r);
            lv_obj_set_size(r, w, 84);
            lv_obj_set_style_radius(r, 18, 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r, on ? lv_color_hex(0x0A3A6B) : AOS_C_CARD, 0);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(r, cmp_pick, LV_EVENT_CLICKED, seen[nseen]);
            lv_obj_t *a = bo_label(r, in.show, aos_font_body, AOS_C_TEXT);
            lv_obj_align(a, LV_ALIGN_LEFT_MID, 20, 0);
            lv_obj_t *b = bo_label(r, in.name, aos_font_caption, AOS_C_DIM);
            lv_obj_align(b, LV_ALIGN_LEFT_MID, 200, 0);
            lv_obj_set_width(b, w - 300);
            lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
            if (on) {
                lv_obj_t *ck = bo_label(r, AOS_SYM_CHECK, &aos_sym_28, AOS_C_TEXT);
                lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -20, 0);
            }
            aos_make_decorative(a);
            aos_make_decorative(b);
            nseen++;
        }
    }
}

static lv_obj_t *s_cmp_col;

static void cmp_refill(void *ud) { (void)ud; if (s_cmp_col) cmp_sheet_fill(s_cmp_col); }

static void cmp_pick(lv_event_t *e)
{
    const char *s = lv_event_get_user_data(e);
    for (int k = 0; k < D.ncmp; k++) {
        if (!strcmp(D.cmp[k], s)) {
            memmove(D.cmp[k], D.cmp[k + 1], (size_t)(D.ncmp - k - 1) * BS_SYM_LEN);
            D.ncmp--;
            apply();
            lv_async_call(cmp_refill, NULL);
            return;
        }
    }
    if (D.ncmp >= BS_MAX_CMP) {
        aos_ui_toast(_("Hasta dos a la vez"), 1200);
        return;
    }
    bs_scpy(D.cmp[D.ncmp++], BS_SYM_LEN, s);
    bs_item(s);
    apply();
    lv_async_call(cmp_refill, NULL);
}

static void cmp_closed(lv_event_t *e)
{
    (void)e;
    if (s_cmp_col) lv_async_call_cancel(cmp_refill, NULL);
    s_cmp_col = NULL;
}

static void cmp_cb(lv_event_t *e)
{
    (void)e;
    char t[64];
    snprintf(t, sizeof t, _("Comparar %s"), D.info.show);
    lv_obj_t *body = bs_sheet(t, cmp_closed);
    lv_obj_update_layout(body);
    s_cmp_col = lv_obj_create(body);
    lv_obj_remove_style_all(s_cmp_col);
    lv_obj_set_size(s_cmp_col, lv_obj_get_width(body), lv_obj_get_height(body));
    lv_obj_set_flex_flow(s_cmp_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_cmp_col, 10, 0);
    lv_obj_set_scroll_dir(s_cmp_col, LV_DIR_VER);
    lv_obj_add_flag(s_cmp_col, LV_OBJ_FLAG_CLICKABLE);
    cmp_sheet_fill(s_cmp_col);
}

/* ---- alerts ---- */

static struct {
    lv_obj_t *ta, *dir_btn, *list;
    uint8_t dir;
    char sym[BS_SYM_LEN];
} A;

static void alert_list_fill(void);

static void alert_del(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bs_cfg_t *c = bs_cfg();
    if (i < 0 || i >= c->nalerts) return;
    memmove(&c->alert[i], &c->alert[i + 1], sizeof c->alert[0] * (size_t)(c->nalerts - i - 1));
    c->nalerts--;
    bs_cfg_save();
    lv_async_call((lv_async_cb_t)alert_list_fill, NULL);
    stats_paint();
}

static void alert_list_fill(void)
{
    if (!A.list) return;
    lv_obj_clean(A.list);
    bs_cfg_t *c = bs_cfg();
    bs_info_t in;
    bs_sym_info(A.sym, &in);
    lv_obj_update_layout(A.list);
    int32_t w = lv_obj_get_width(A.list);
    int n = 0;
    for (int i = bs_alert_find(A.sym, 0); i >= 0; i = bs_alert_find(A.sym, i + 1)) {
        lv_obj_t *r = bs_box(A.list, w, 76);
        lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 18, 0);
        char v[32], t[96];
        bs_fmt_price(c->alert[i].value, &in, v, sizeof v);
        snprintf(t, sizeof t, c->alert[i].dir == BS_AL_ABOVE ? _("Si sube a %s") : _("Si baja a %s"), v);
        if (c->alert[i].fired) { size_t L = strlen(t); snprintf(t + L, sizeof t - L, "%s", _(" · ya avisó")); }
        lv_obj_t *l = bo_label(r, t, aos_font_small, c->alert[i].fired ? AOS_C_DIM : AOS_C_TEXT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t *d = bs_round_btn(r, AOS_SYM_DELETE, 60, lv_color_hex(0x5A1E1B), alert_del, (void *)(intptr_t)i);
        lv_obj_align(d, LV_ALIGN_RIGHT_MID, -8, 0);
        n++;
    }
    if (!n) {
        lv_obj_t *l = bo_label(A.list, _("Todavía no hay avisos para este símbolo."), aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, w);
    }
}

static void alert_dir_cb(lv_event_t *e)
{
    (void)e;
    A.dir = A.dir == BS_AL_ABOVE ? BS_AL_BELOW : BS_AL_ABOVE;
    lv_label_set_text(lv_obj_get_child(A.dir_btn, 0), A.dir == BS_AL_ABOVE ? _("Si sube a") : _("Si baja a"));
}

static void alert_add_cb(lv_event_t *e)
{
    (void)e;
    if (!A.ta) return;
    char num[32];
    bs_scpy(num, sizeof num, lv_textarea_get_text(A.ta));
    /* both decimal signs are accepted; a thousands dot in "1.234,5" too */
    bool comma = strchr(num, ',') != NULL;
    char clean[32];
    size_t w = 0;
    for (char *p = num; *p && w + 1 < sizeof clean; p++) {
        if (*p == ',') clean[w++] = '.';
        else if (*p == '.' && comma) continue;
        else clean[w++] = *p;
    }
    clean[w] = 0;
    char *end = NULL;
    float v = strtof(clean, &end);
    bs_cfg_t *c = bs_cfg();
    if (end == clean || !(v > 0)) { aos_ui_toast(_("Escribí un precio"), 1200); return; }
    if (c->nalerts >= BS_MAX_ALERTS) { aos_ui_toast(_("Hay demasiados avisos"), 1200); return; }
    bs_alert_t *a = &c->alert[c->nalerts++];
    memset(a, 0, sizeof *a);
    bs_scpy(a->sym, sizeof a->sym, A.sym);
    a->dir = A.dir;
    a->value = v;
    bs_cfg_save();
    lv_textarea_set_text(A.ta, "");
    alert_list_fill();
    stats_paint();
    aos_ui_toast(_("Aviso guardado: avisa mientras la app esté abierta, aunque esté atrás"), 2200);
}

static void alert_closed(lv_event_t *e)
{
    (void)e;
    lv_async_call_cancel((lv_async_cb_t)alert_list_fill, NULL);
    A.ta = A.list = A.dir_btn = NULL;
}

void bs_alert_sheet(const char *sym)
{
    memset(&A, 0, sizeof A);
    bs_scpy(A.sym, sizeof A.sym, sym);
    bs_info_t in;
    bs_sym_info(sym, &in);
    char t[64];
    snprintf(t, sizeof t, _("Avisos de %s"), in.show);
    lv_obj_t *body = bs_sheet(t, alert_closed);
    lv_obj_update_layout(body);
    int32_t w = lv_obj_get_width(body);
    bs_item_t *it = bs_item(sym);
    bs_quote_t q;
    bool hq = it && bs_item_quote(it, &q);
    A.dir = BS_AL_ABOVE;
    A.dir_btn = bs_chip(body, _("Si sube a"), false, alert_dir_cb, NULL);
    lv_obj_set_pos(A.dir_btn, 0, 8);
    A.ta = lv_textarea_create(body);
    lv_textarea_set_one_line(A.ta, true);
    lv_textarea_set_accepted_chars(A.ta, "0123456789.,");
    char ph[32] = "";
    if (hq) bs_fmt_price(q.price, &in, ph, sizeof ph);
    lv_textarea_set_placeholder_text(A.ta, ph);
    lv_obj_set_style_text_font(A.ta, aos_font_body, 0);
    lv_obj_set_size(A.ta, w - 260 - 120, 72);
    lv_obj_set_pos(A.ta, 250, 4);
    lv_obj_t *ok = bs_round_btn(body, AOS_SYM_CHECK, 72, AOS_C_ACCENT, alert_add_cb, NULL);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, 0, 4);
    lv_obj_t *n = bo_label(body, _("Avisa con un mensaje y un pitido cuando el precio cruza el valor, mientras la app esté abierta (aunque esté atrás). También se cargan en el portal."),
                           aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(n, w);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(n, 0, 96);
    int32_t kb_h = bs_ui.land ? 300 : 380;
    A.list = lv_obj_create(body);
    lv_obj_remove_style_all(A.list);
    lv_obj_set_size(A.list, w, lv_obj_get_height(body) - 200 - kb_h);
    lv_obj_set_pos(A.list, 0, 190);
    lv_obj_set_flex_flow(A.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(A.list, 10, 0);
    lv_obj_add_flag(A.list, LV_OBJ_FLAG_CLICKABLE);
    alert_list_fill();
    lv_obj_t *kb = lv_keyboard_create(lv_obj_get_parent(body));
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(kb, bs_ui.W, kb_h);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(kb, aos_font_body);
    lv_keyboard_set_textarea(kb, A.ta);
    lv_obj_add_event_cb(A.ta, (lv_event_cb_t)alert_add_cb, LV_EVENT_READY, NULL);
}

static void bell_cb(lv_event_t *e)
{
    (void)e;
    bs_alert_sheet(D.info.sym);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    bs_go(BS_V_LIST, NULL);
}

/* ---- building ---- */

static lv_obj_t *small_chip(lv_obj_t *parent, const char *txt, int32_t w, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = bs_chip(parent, txt, false, cb, ud);
    lv_obj_set_height(b, 60);
    if (w > 0) {
        lv_obj_set_width(b, w);
        lv_obj_set_style_pad_hor(b, 0, 0);
    } else {
        lv_obj_set_style_pad_hor(b, 18, 0);
    }
    return b;
}

static void build_ranges(lv_obj_t *parent, int32_t bw)
{
    for (int i = 0; i < NRANGES; i++)
        D.range_btn[i] = small_chip(parent, _(RANGES[i].name), bw, range_cb, (void *)(intptr_t)i);
    ranges_paint();
}

static void build_tools(lv_obj_t *parent)
{
    D.type_btn = small_chip(parent, "", 120, type_cb, NULL);
    D.vol_btn = small_chip(parent, _("Vol"), 0, opt_cb, (void *)0);
    D.ma_btn = small_chip(parent, _("MM"), 0, opt_cb, (void *)1);
    D.rsi_btn = small_chip(parent, "RSI", 0, opt_cb, (void *)2);
    D.cmp_btn = small_chip(parent, _("Comparar"), 0, cmp_cb, NULL);
}

static lv_obj_t *row_flex(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *r = bs_box(parent, w, h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static void stat_cell(lv_obj_t *grid, int i, const char *name, int32_t w)
{
    lv_obj_t *c = bs_box(grid, w, 50);
    lv_obj_t *a = bo_label(c, name, aos_font_small, AOS_C_DIM);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, 0, 0);
    D.st_val[i] = bo_label(c, "--", aos_font_small, AOS_C_TEXT);
    lv_obj_align(D.st_val[i], LV_ALIGN_RIGHT_MID, 0, 0);
}

void bs_detail_build(const char *sym)
{
    memset(&D, 0, offsetof(typeof(D), cmp));
    D.reading = false;
    D.t_price[0] = D.t_chg[0] = D.t_when[0] = 0;     /* the labels are new */
    bs_item_t *it = bs_item(sym);
    if (it) D.info = it->info;
    else bs_sym_info(sym, &D.info);
    /* the comparisons are kept only while it is the same symbol */
    static char last_sym[BS_SYM_LEN];
    if (strcmp(last_sym, sym)) { D.ncmp = 0; bs_scpy(last_sym, sizeof last_sym, sym); }

    lv_obj_t *root = bs_ui.root;
    int32_t W = bs_ui.W, H = bs_ui.H, pad = AOS_UI_PAD;
    lv_obj_t *back = bs_round_btn(root, AOS_SYM_CHEVRON_LEFT, 72, AOS_C_CARD2, back_cb, NULL);
    lv_obj_set_pos(back, pad, 10);

    if (!bs_ui.land) {
        lv_obj_t *bell = bs_round_btn(root, AOS_SYM_BELL_OUTLINE, 72, AOS_C_CARD2, bell_cb, NULL);
        lv_obj_set_pos(bell, W - pad - 72, 10);
        lv_obj_t *s = bo_label(root, D.info.show, aos_font_title, AOS_C_TEXT);
        lv_obj_set_pos(s, pad + 92, 6);
        lv_obj_set_width(s, W - 2 * pad - 2 * 92);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_t *nm = bo_label(root, D.info.name, aos_font_caption, AOS_C_DIM);
        lv_obj_set_pos(nm, pad + 94, 54);
        lv_obj_set_width(nm, W - 2 * pad - 2 * 92);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);

        D.price = bo_label(root, "--", aos_font_large, AOS_C_TEXT);
        lv_obj_set_pos(D.price, pad + 4, 96);
        D.ccy = bo_label(root, "", aos_font_small, AOS_C_DIM);
        D.chg = bo_label(root, "", aos_font_body, AOS_C_DIM);
        lv_obj_set_pos(D.chg, pad + 4, 156);
        D.read = bo_label(root, "", aos_font_small, AOS_C_TEXT);
        lv_obj_set_pos(D.read, pad + 4, 160);
        lv_obj_set_width(D.read, W - 2 * pad);
        lv_label_set_long_mode(D.read, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_add_flag(D.read, LV_OBJ_FLAG_HIDDEN);
        D.when = bo_label(root, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_set_pos(D.when, pad + 4, 200);
        lv_obj_set_width(D.when, W - 2 * pad);
        lv_label_set_long_mode(D.when, LV_LABEL_LONG_MODE_DOTS);

        /* the lower part, from the bottom up */
        int32_t foot_y = H - 40;
        int32_t stats_h = 3 * 50 + 98 + 50;
        int32_t stats_y = foot_y - stats_h - 6;
        int32_t tools_y = stats_y - 76;
        int32_t ranges_y = tools_y - 72;
        int32_t chart_y = 230;
        int32_t chart_h = ranges_y - chart_y - 10;

        lv_obj_t *card = bs_box(root, W - 2 * pad, chart_h);
        lv_obj_set_pos(card, pad, chart_y);
        lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 24, 0);
        D.chart = bs_chart_create(card, W - 2 * pad - 24, chart_h - 20);
        lv_obj_set_pos(bs_chart_obj(D.chart), 12, 10);

        lv_obj_t *rr = row_flex(root, W - 2 * pad, 64);
        lv_obj_set_pos(rr, pad, ranges_y);
        build_ranges(rr, (W - 2 * pad - 6 * 8) / NRANGES);
        lv_obj_t *tr = row_flex(root, W - 2 * pad, 64);
        lv_obj_set_pos(tr, pad, tools_y);
        build_tools(tr);

        D.stats = bs_box(root, W - 2 * pad, stats_h);
        lv_obj_set_pos(D.stats, pad, stats_y);
        lv_obj_set_flex_flow(D.stats, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(D.stats, 32, 0);
        int32_t cw = (W - 2 * pad - 32) / 2;
        stat_cell(D.stats, 0, _("Apertura"), cw);
        stat_cell(D.stats, 1, _("Máximo"), cw);
        stat_cell(D.stats, 2, _("Mínimo"), cw);
        stat_cell(D.stats, 3, _("Cierre anterior"), cw);
        stat_cell(D.stats, 4, _("Volumen"), cw);
        lv_obj_t *al = bs_box(D.stats, cw, 50);
        D.alert_lbl = bo_label(al, "", aos_font_caption, AOS_C_ORANGE);
        lv_obj_set_width(D.alert_lbl, cw);
        lv_label_set_long_mode(D.alert_lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(D.alert_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_text_align(D.alert_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(al, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(al, bell_cb, LV_EVENT_CLICKED, NULL);
        /* the 52 weeks: low, a bar with the price on it, high */
        lv_obj_t *w52 = bs_box(D.stats, W - 2 * pad, 98);
        lv_obj_t *t52 = bo_label(w52, _("Rango de 52 semanas"), aos_font_small, AOS_C_DIM);
        lv_obj_set_pos(t52, 0, 6);
        D.lo52 = bo_label(w52, "--", aos_font_small, AOS_C_TEXT);
        lv_obj_set_pos(D.lo52, 0, 50);
        D.hi52 = bo_label(w52, "--", aos_font_small, AOS_C_TEXT);
        lv_obj_align(D.hi52, LV_ALIGN_TOP_RIGHT, 0, 50);
        D.bar52 = bs_box(w52, W - 2 * pad - 2 * 200, 10);
        lv_obj_set_pos(D.bar52, 200, 62);
        lv_obj_set_style_bg_color(D.bar52, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(D.bar52, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(D.bar52, 5, 0);
        D.mark52 = bs_box(D.bar52, 20, 10);
        lv_obj_set_style_bg_color(D.mark52, AOS_C_TEXT, 0);
        lv_obj_set_style_bg_opa(D.mark52, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(D.mark52, 5, 0);
        lv_obj_add_flag(D.mark52, LV_OBJ_FLAG_HIDDEN);

        D.foot = bo_label(root, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(D.foot, W - 2 * pad);
        lv_label_set_long_mode(D.foot, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(D.foot, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(D.foot, pad, foot_y + 8);
    } else {
        /* lying down: the chart on the whole screen */
        lv_obj_t *s = bo_label(root, D.info.show, aos_font_title, AOS_C_TEXT);
        lv_obj_set_pos(s, pad + 90, 4);
        lv_obj_set_width(s, 200);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        D.price = bo_label(root, "--", aos_font_title, AOS_C_TEXT);
        lv_obj_set_pos(D.price, pad + 300, 4);
        D.chg = bo_label(root, "", aos_font_small, AOS_C_DIM);
        lv_obj_set_pos(D.chg, pad + 92, 52);
        D.read = bo_label(root, "", aos_font_small, AOS_C_TEXT);
        lv_obj_set_pos(D.read, pad + 92, 52);
        lv_obj_set_width(D.read, W - pad - 92 - 7 * 80 - 2 * pad);
        lv_label_set_long_mode(D.read, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_add_flag(D.read, LV_OBJ_FLAG_HIDDEN);
        D.when = bo_label(root, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_add_flag(D.when, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *rr = row_flex(root, 7 * 80, 64);
        lv_obj_set_pos(rr, W - pad - 7 * 80, 10);
        build_ranges(rr, 74);

        int32_t bottom_h = 64;
        lv_obj_t *card = bs_box(root, W - 2 * pad, H - 90 - bottom_h - 8);
        lv_obj_set_pos(card, pad, 86);
        lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 20, 0);
        D.chart = bs_chart_create(card, W - 2 * pad - 20, H - 90 - bottom_h - 8 - 16);
        lv_obj_set_pos(bs_chart_obj(D.chart), 10, 8);

        lv_obj_t *tr = bs_box(root, 620, 64);
        lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tr, 8, 0);
        lv_obj_set_pos(tr, pad, H - bottom_h);
        build_tools(tr);
        lv_obj_t *bell = bs_round_btn(tr, AOS_SYM_BELL_OUTLINE, 60, AOS_C_CARD2, bell_cb, NULL);
        (void)bell;
        D.foot = bo_label(root, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(D.foot, W - 2 * pad - 640);
        lv_label_set_long_mode(D.foot, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(D.foot, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(D.foot, pad + 640, H - bottom_h + 6);
    }

    bs_chart_on_read(D.chart, read_cb);
    char f[160];
    bs_footer_text(f, sizeof f, &D.info);
    lv_label_set_text(D.foot, f);
    head_paint();
    stats_paint();
    apply();
}

void bs_detail_refresh(const char *sym, int what)
{
    if (!D.chart) return;
    bool mine = !sym || !strcmp(sym, D.info.sym);
    bool cmp = false;
    for (int k = 0; sym && k < D.ncmp; k++) cmp |= !strcmp(sym, D.cmp[k]);
    if (!mine && !cmp) return;
    if (mine) {
        head_paint();
        stats_paint();
    }
    if (what != BS_D_QUOTE || (mine && bs_cfg()->range == 0)) apply();
    else if (mine && sym == NULL) apply();
}

void bs_detail_tick(void)
{
    if (!D.chart) return;
    bs_chart_poll(D.chart);
    uint32_t n = (uint32_t)aos_hal_uptime_ms();
    if (n - D.last_when > 15000) {
        D.last_when = n;
        head_paint();
    }
}

bool bs_detail_back(void) { return false; }

void bs_detail_leave(void)
{
    if (D.chart) {
        bs_chart_delete(D.chart);    /* its object goes with the screen */
        D.chart = NULL;
    }
    D.price = NULL;
    D.stats = NULL;
    s_cmp_col = NULL;
    A.ta = A.list = A.dir_btn = NULL;
}
