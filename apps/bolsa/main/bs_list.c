/*
 * P4OS - Bolsa: the lists.
 *
 * The lists as tabs across the top; under them one row per symbol, with
 * its name, a sparkline of the day (or of the last month when the
 * provider has no intraday), the price and the change, green or red.
 * Tapping the change shows it in money instead of percent, for every row.
 *
 * The pencil turns the rows into editing ones: up, down and out. The +
 * opens the search: the catalog (indices, coins, the dollar, currencies)
 * filters as you type; the provider's search for shares and CoinGecko's for
 * coins go out when the keyboard's OK is pressed, so typing does not spend
 * the free plans' requests.
 *
 * The rows are built once and only their texts change when data comes
 * (rebuilding LVGL objects is what costs, as Cotizaciones measured).
 */
#include "bs.h"

#define ROW_H   124

typedef struct {
    lv_obj_t *row, *show, *name, *price, *pill, *pill_l, *spark;
    char sym[BS_SYM_LEN];
    char t_price[24], t_chg[28], t_name[64];
} row_t;

static struct {
    lv_obj_t *tabs, *list, *status, *foot, *edit_btn;
    row_t rows[BS_MAX_SYMS];
    int nrows;
    bool editing;
    char t_status[128], t_foot[160];
    /* search */
    lv_obj_t *ta, *kb, *results;
    char query[40];
    bs_hit_t net_hits[16];
    int n_net;
    bool searching;
    bool cg_next;
} L;

static bool s_show_abs;

static void build_rows(void);
static void search_open(void);

static void set_txt(lv_obj_t *o, char *cache, size_t cap, const char *t)
{
    if (!o || !strncmp(cache, t, cap - 1)) return;
    bs_scpy(cache, cap, t);
    lv_label_set_text(o, cache);
}

static const char *list_name(int i)
{
    return _(bs_cfg()->list[i].name);
}

/* ---- one row ---- */

static void row_paint(row_t *r)
{
    bs_item_t *it = bs_item(r->sym);
    if (!it) return;
    bs_quote_t q;
    char p[32], c[32], n[64];
    bool ok = bs_item_quote(it, &q);
    bs_prov_t pq = bs_sym_prov_quote(&it->info);
    lv_color_t pill_col = AOS_C_CARD2;
    if (ok) {
        bs_fmt_price(q.price, &it->info, p, sizeof p);
        if (q.has & BS_Q_PREV && q.prev > 0) {
            float chg = q.price - q.prev, pct = chg / q.prev * 100;
            if (s_show_abs) bs_fmt_chg(chg, &it->info, c, sizeof c);
            else bs_fmt_pct(pct, c, sizeof c);
            pill_col = chg > 0 ? BS_C_UP : chg < 0 ? BS_C_DOWN : AOS_C_CARD2;
        } else {
            bs_scpy(c, sizeof c, "--");
        }
    } else {
        bs_scpy(p, sizeof p, "--");
        bs_scpy(c, sizeof c, pq == BS_P_NONE ? _("sin clave") : it->err ? _("error") : "…");
    }
    if (pq == BS_P_NONE) snprintf(n, sizeof n, "%s", _("Necesita una clave: portal"));
    else if (it->info.name[0]) snprintf(n, sizeof n, "%s", it->info.name);
    else snprintf(n, sizeof n, "%s", bs_prov_name(pq));
    set_txt(r->price, r->t_price, sizeof r->t_price, p);
    set_txt(r->name, r->t_name, sizeof r->t_name, n);
    if (r->pill_l) {
        set_txt(r->pill_l, r->t_chg, sizeof r->t_chg, c);
        lv_obj_set_style_bg_color(r->pill, pill_col, 0);
    }
    if (r->spark) {
        const bs_pt_t *pts = NULL;
        float base = NAN;
        int cnt = bs_item_spark(it, &pts, &base);
        if (cnt >= 2) {
            float ref = !isnan(base) ? base : pts[0].c;
            float last = ok ? q.price : pts[cnt - 1].c;
            bs_spark_set(r->spark, pts, cnt, base, last >= ref);
        }
    }
}

static void row_click(lv_event_t *e)
{
    row_t *r = lv_event_get_user_data(e);
    if (L.editing) return;
    bs_go(BS_V_DETAIL, r->sym);
}

static void pill_click(lv_event_t *e)
{
    (void)e;
    s_show_abs = !s_show_abs;
    for (int i = 0; i < L.nrows; i++) row_paint(&L.rows[i]);
}

static void move_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int idx = (int)(v >> 1), up = (int)(v & 1);
    bs_cfg_t *c = bs_cfg();
    bs_cfg_move(c->cur_list, idx, up ? idx - 1 : idx + 1);
    bs_cfg_save();
    build_rows();
}

static void del_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bs_cfg_t *c = bs_cfg();
    bs_cfg_remove(c->cur_list, idx);
    bs_cfg_save();
    build_rows();
}

static void row_build(lv_obj_t *parent, int i, const char *sym, int32_t w)
{
    row_t *r = &L.rows[i];
    memset(r, 0, sizeof *r);
    bs_scpy(r->sym, sizeof r->sym, sym);
    bs_item_t *it = bs_item(sym);
    r->row = lv_obj_create(parent);
    lv_obj_remove_style_all(r->row);
    lv_obj_set_size(r->row, w, ROW_H);
    lv_obj_set_style_bg_color(r->row, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(r->row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r->row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r->row, 24, 0);
    lv_obj_remove_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->row, row_click, LV_EVENT_CLICKED, r);

    int32_t right = L.editing ? 236 : 196;
    int32_t spark_w = w > 640 ? 150 : 110;
    int32_t left_w = w - 24 - right - spark_w - 24 - 12;
    r->show = bo_label(r->row, it ? it->info.show : sym, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(r->show, left_w);
    lv_label_set_long_mode(r->show, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(r->show, 24, 20);
    r->name = bo_label(r->row, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(r->name, left_w);
    lv_label_set_long_mode(r->name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(r->name, 24, 70);
    aos_make_decorative(r->show);
    aos_make_decorative(r->name);

    if (!L.editing) {
        r->spark = bs_spark_create(r->row, spark_w, 72);
        lv_obj_set_pos(r->spark, 24 + left_w + 12, (ROW_H - 72) / 2);
        r->price = bo_label(r->row, "--", aos_font_body, AOS_C_TEXT);
        lv_obj_set_width(r->price, right);
        lv_obj_set_style_text_align(r->price, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r->price, LV_ALIGN_TOP_RIGHT, -20, 14);
        aos_make_decorative(r->price);
        r->pill = lv_obj_create(r->row);
        lv_obj_remove_style_all(r->pill);
        lv_obj_set_size(r->pill, 176, 46);
        lv_obj_set_style_radius(r->pill, 12, 0);
        lv_obj_set_style_bg_opa(r->pill, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r->pill, AOS_C_CARD2, 0);
        lv_obj_align(r->pill, LV_ALIGN_BOTTOM_RIGHT, -20, -14);
        lv_obj_remove_flag(r->pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r->pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(r->pill, 10);
        lv_obj_add_event_cb(r->pill, pill_click, LV_EVENT_CLICKED, NULL);
        r->pill_l = bo_label(r->pill, "", aos_font_small, AOS_C_TEXT);
        lv_obj_align(r->pill_l, LV_ALIGN_RIGHT_MID, -12, 0);
        aos_make_decorative(r->pill_l);
    } else {
        bs_list_t *l = &bs_cfg()->list[bs_cfg()->cur_list];
        if (i > 0) {
            lv_obj_t *b = bs_round_btn(r->row, AOS_SYM_CHEVRON_UP, 68, AOS_C_CARD2, move_cb, (void *)(intptr_t)(i << 1 | 1));
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -172, 0);
        }
        if (i < l->n - 1) {
            lv_obj_t *b = bs_round_btn(r->row, AOS_SYM_CHEVRON_DOWN, 68, AOS_C_CARD2, move_cb, (void *)(intptr_t)(i << 1));
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -94, 0);
        }
        lv_obj_t *d = bs_round_btn(r->row, AOS_SYM_DELETE, 68, lv_color_hex(0x5A1E1B), del_cb, (void *)(intptr_t)i);
        lv_obj_align(d, LV_ALIGN_RIGHT_MID, -16, 0);
    }
    row_paint(r);
}

static void add_row_cb(lv_event_t *e)
{
    (void)e;
    search_open();
}

static void build_rows(void)
{
    if (!L.list) return;
    lv_obj_clean(L.list);
    L.nrows = 0;
    bs_cfg_t *c = bs_cfg();
    if (c->cur_list < 0 || c->cur_list >= c->nlists) return;
    bs_list_t *l = &c->list[c->cur_list];
    lv_obj_update_layout(L.list);
    int32_t lw = lv_obj_get_content_width(L.list);
    int cols = bs_ui.land ? 2 : 1;
    int32_t w = (lw - (cols - 1) * 16) / cols;
    for (int i = 0; i < l->n; i++) {
        row_build(L.list, i, l->sym[i], w);
        L.nrows++;
    }
    if (!l->n || L.editing) {
        lv_obj_t *b = lv_obj_create(L.list);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, w, ROW_H - 20);
        lv_obj_set_style_radius(b, 24, 0);
        lv_obj_set_style_border_color(b, AOS_C_CARD2, 0);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_bg_color(b, AOS_C_CARD, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, add_row_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *t = bo_label(b, l->n ? _("Sumar un símbolo") : _("Lista vacía: tocá para sumar un símbolo"),
                               aos_font_small, AOS_C_DIM);
        lv_obj_center(t);
    }
}

/* ---- the top ---- */

static void tabs_build(void);

static void tab_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    bs_cfg_t *c = bs_cfg();
    if (i == c->cur_list) return;
    c->cur_list = i;
    bs_cfg_save();
    tabs_build();
    build_rows();
    bs_list_refresh(NULL);
}

static void tabs_build(void)
{
    lv_obj_clean(L.tabs);
    bs_cfg_t *c = bs_cfg();
    lv_obj_t *sel = NULL;
    for (int i = 0; i < c->nlists; i++) {
        lv_obj_t *ch = bs_chip(L.tabs, list_name(i), i == c->cur_list, tab_cb, (void *)(intptr_t)i);
        if (i == c->cur_list) sel = ch;
    }
    if (sel) {
        lv_obj_update_layout(L.tabs);
        lv_obj_scroll_to_view(sel, LV_ANIM_OFF);
    }
}

static void edit_cb(lv_event_t *e)
{
    (void)e;
    L.editing = !L.editing;
    lv_obj_t *l = lv_obj_get_child(L.edit_btn, 0);
    lv_label_set_text(l, L.editing ? AOS_SYM_CHECK : AOS_SYM_PENCIL);
    lv_obj_set_style_bg_color(L.edit_btn, L.editing ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    build_rows();
}

static void plus_cb(lv_event_t *e)
{
    (void)e;
    search_open();
}

void bs_list_build(void)
{
    memset(&L, 0, offsetof(typeof(L), query));
    lv_obj_t *root = bs_ui.root;
    int32_t W = bs_ui.W, H = bs_ui.H, pad = AOS_UI_PAD;
    int32_t top = bs_ui.land ? 8 : 12;

    lv_obj_t *t = bo_label(root, _("Bolsa"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_pos(t, pad + 4, top + 14);

    lv_obj_t *plus = bs_round_btn(root, AOS_SYM_PLUS, 76, AOS_C_CARD2, plus_cb, NULL);
    lv_obj_set_pos(plus, W - pad - 76, top);
    L.edit_btn = bs_round_btn(root, AOS_SYM_PENCIL, 76, AOS_C_CARD2, edit_cb, NULL);
    lv_obj_set_pos(L.edit_btn, W - pad - 76 - 92, top);

    L.tabs = lv_obj_create(root);
    lv_obj_remove_style_all(L.tabs);
    int32_t tabs_x = bs_ui.land ? 220 : pad;
    int32_t tabs_y = bs_ui.land ? top + 6 : top + 96;
    int32_t tabs_w = bs_ui.land ? W - tabs_x - 2 * 92 - pad - 8 : W - 2 * pad;
    lv_obj_set_size(L.tabs, tabs_w, 72);
    lv_obj_set_pos(L.tabs, tabs_x, tabs_y);
    lv_obj_set_flex_flow(L.tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(L.tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(L.tabs, 10, 0);
    lv_obj_set_scroll_dir(L.tabs, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(L.tabs, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(L.tabs, LV_OBJ_FLAG_CLICKABLE);

    int32_t list_y = tabs_y + 84;
    int32_t foot_h = 104;
    L.list = lv_obj_create(root);
    lv_obj_remove_style_all(L.list);
    lv_obj_set_size(L.list, W - 2 * pad, H - list_y - foot_h);
    lv_obj_set_pos(L.list, pad, list_y);
    lv_obj_set_flex_flow(L.list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(L.list, 12, 0);
    lv_obj_set_style_pad_column(L.list, 16, 0);
    lv_obj_set_style_pad_bottom(L.list, 12, 0);
    lv_obj_set_scroll_dir(L.list, LV_DIR_VER);
    lv_obj_add_flag(L.list, LV_OBJ_FLAG_CLICKABLE);     /* scrolls with the finger */

    L.status = bo_label(root, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(L.status, W - 2 * pad);
    lv_label_set_long_mode(L.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(L.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(L.status, pad, H - foot_h + 6);
    L.foot = bo_label(root, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(L.foot, W - 2 * pad);
    lv_label_set_long_mode(L.foot, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(L.foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(L.foot, pad, H - foot_h + 38);

    tabs_build();
    build_rows();
    bs_list_refresh(NULL);
}

void bs_list_refresh(const char *sym)
{
    if (!L.list) return;
    for (int i = 0; i < L.nrows; i++)
        if (!sym || !strcmp(sym, L.rows[i].sym)) row_paint(&L.rows[i]);

    char t[160], why[64];
    bs_footer_text(t, sizeof t, NULL);
    set_txt(L.foot, L.t_foot, sizeof L.t_foot, t);

    /* the status: busy, a provider's limit, or how old the data are */
    int32_t newest = 0;
    bool any_limited = false, need_key = false;
    bs_cfg_t *c = bs_cfg();
    if (c->cur_list >= 0 && c->cur_list < c->nlists) {
        bs_list_t *l = &c->list[c->cur_list];
        for (int i = 0; i < l->n; i++) {
            bs_item_t *it = bs_item_find(l->sym[i]);
            if (!it) continue;
            bs_quote_t q;
            bs_prov_t p = bs_sym_prov_quote(&it->info);
            need_key |= p == BS_P_NONE;
            if (!any_limited && bs_net_limited(p, why, sizeof why)) any_limited = true;
            if (bs_item_quote(it, &q) && q.fetched > newest) newest = q.fetched;
        }
    }
    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        char a[32];
        bs_fmt_ago(newest, a, sizeof a);
        if (a[0]) snprintf(t, sizeof t, _("Sin conexión · lo último, %s"), a);
        else snprintf(t, sizeof t, "%s", _("Sin conexión"));
    } else if (any_limited) {
        snprintf(t, sizeof t, "%s", why);
    } else if (bs_net_busy() || bs_net_pending()) {
        snprintf(t, sizeof t, "%s", _("Actualizando…"));
    } else if (need_key) {
        snprintf(t, sizeof t, "%s", _("Acciones e índices: cargá una clave en la página del portal"));
    } else if (newest) {
        char a[32];
        bs_fmt_ago(newest, a, sizeof a);
        snprintf(t, sizeof t, _("Actualizado %s"), a);
    } else {
        t[0] = 0;
    }
    set_txt(L.status, L.t_status, sizeof L.t_status, t);
}

bool bs_list_back(void)
{
    if (L.editing) {
        edit_cb(NULL);
        return true;
    }
    return false;
}

void bs_list_leave(void)
{
    L.list = NULL;
    L.nrows = 0;
}

/* ---- the search ---- */

static void results_fill(void);

static void hit_cb(lv_event_t *e)
{
    const bs_hit_t *h = lv_event_get_user_data(e);
    bs_cfg_t *c = bs_cfg();
    if (h->name[0]) {
        char learned[BS_NAME_LEN + 20];
        if (!strncmp(h->sym, "cg:", 3)) snprintf(learned, sizeof learned, "%.15s|%s", h->where, h->name);
        else snprintf(learned, sizeof learned, "%s", h->name);
        bool cat = false;
        bs_info_t in;
        for (int i = 0; bs_sym_catalog(i, &in); i++) cat |= !strcmp(in.sym, h->sym);
        if (!cat) bs_names_put(h->sym, learned);
    }
    char t[96];
    if (bs_cfg_has(c->cur_list, h->sym)) {
        snprintf(t, sizeof t, _("%s ya está en la lista"), h->sym);
    } else if (bs_cfg_add(c->cur_list, h->sym)) {
        bs_cfg_save();
        bs_item_t *it = bs_item(h->sym);
        if (it) bs_sym_info(h->sym, &it->info);
        snprintf(t, sizeof t, _("Sumado a %s"), list_name(c->cur_list));
    } else {
        snprintf(t, sizeof t, "%s", _("La lista está llena"));
    }
    aos_ui_toast(t, 1500);
    bs_sheet_close();
    build_rows();
    bs_list_refresh(NULL);
}

static bs_hit_t s_shown[40];

static void result_row(int k, const bs_hit_t *h, int32_t w)
{
    s_shown[k] = *h;
    lv_obj_t *r = lv_obj_create(L.results);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, 92);
    lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 18, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, hit_cb, LV_EVENT_CLICKED, &s_shown[k]);
    bs_info_t in;
    bs_sym_info(h->sym, &in);
    const char *show = !strncmp(h->sym, "cg:", 3) && h->where[0] ? h->where : in.show;
    lv_obj_t *a = bo_label(r, show, aos_font_body, AOS_C_TEXT);
    lv_obj_set_pos(a, 20, 12);
    lv_obj_set_width(a, 220);
    lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *b = bo_label(r, h->name, aos_font_caption, AOS_C_DIM);
    lv_obj_set_pos(b, 20, 54);
    lv_obj_set_width(b, w - 40);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_DOTS);
    if (h->where[0] && strncmp(h->sym, "cg:", 3)) {
        lv_obj_t *c = bo_label(r, h->where, aos_font_caption, AOS_C_DIM);
        lv_obj_align(c, LV_ALIGN_TOP_RIGHT, -20, 18);
    }
    aos_make_decorative(a);
    aos_make_decorative(b);
}

static bool match(const char *hay, const char *q)
{
    /* case-insensitive, ASCII */
    size_t n = strlen(q);
    if (!n) return true;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        for (; i < n && p[i]; i++) {
            char a = p[i], b = q[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
        }
        if (i == n) return true;
    }
    return false;
}

static void results_fill(void)
{
    if (!L.results) return;
    lv_obj_clean(L.results);
    lv_obj_update_layout(L.results);    /* the width is only known after a layout */
    int32_t w = lv_obj_get_width(L.results) - 4;
    int k = 0;
    const char *q = L.query;
    /* the catalog */
    bs_info_t in;
    for (int i = 0; bs_sym_catalog(i, &in) && k < 24; i++) {
        if (q[0] && !match(in.sym, q) && !match(in.show, q) && !match(in.name, q)) continue;
        bs_hit_t h;
        memset(&h, 0, sizeof h);
        bs_scpy(h.sym, sizeof h.sym, in.sym);
        bs_scpy(h.name, sizeof h.name, in.name);
        bs_scpy(h.where, sizeof h.where, in.kind == BS_K_CRYPTO ? in.show :
                in.kind == BS_K_INDEX ? _("índice") : in.kind == BS_K_RISK ? _("puntos básicos") : in.ccy);
        result_row(k++, &h, w);
    }
    /* what the providers found */
    for (int i = 0; i < L.n_net && k < 38; i++) result_row(k++, &L.net_hits[i], w);
    /* the text as a symbol */
    char s[BS_SYM_LEN];
    if (q[0] && bs_sym_clean(q, s, sizeof s) && k < 40) {
        bs_hit_t h;
        memset(&h, 0, sizeof h);
        bs_scpy(h.sym, sizeof h.sym, s);
        snprintf(h.name, sizeof h.name, _("Sumar «%s» tal cual"), s);
        result_row(k++, &h, w);
        /* the name of this one is not learned: it is not a name */
        s_shown[k - 1].name[0] = 0;
    }
    lv_obj_t *hint = bo_label(L.results, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(hint, w);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    bs_prov_t sp = BS_P_NONE;
    static const bs_prov_t SP[] = { BS_P_TWELVE, BS_P_FINNHUB, BS_P_ALPHA };
    for (int i = 0; i < 3; i++) if (bs_cfg()->key[SP[i]][0]) { sp = SP[i]; break; }
    char t[200];
    if (L.searching) snprintf(t, sizeof t, "%s", _("Buscando…"));
    else if (sp) snprintf(t, sizeof t, _("Tocá ✓ en el teclado para buscar acciones en %s y monedas en CoinGecko."), bs_prov_name(sp));
    else snprintf(t, sizeof t, "%s", _("Tocá ✓ en el teclado para buscar monedas en CoinGecko. Para buscar acciones por nombre, cargá una clave en la página del portal; si sabés el símbolo, escribilo."));
    lv_label_set_text(hint, t);
}

static void search_done(const bs_hit_t *hits, int n, int err)
{
    if (!L.results) return;
    for (int i = 0; i < n && L.n_net < 16; i++) L.net_hits[L.n_net++] = hits[i];
    if (L.cg_next) {
        L.cg_next = false;
        bs_net_search(L.query, BS_P_COINGECKO, search_done);
        return;
    }
    L.searching = false;
    if (err && !L.n_net) aos_ui_toast(err == BS_E_LIMIT ? _("El proveedor pidió esperar un minuto") : _("No se pudo buscar"), 1600);
    results_fill();
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bs_scpy(L.query, sizeof L.query, lv_textarea_get_text(L.ta));
        L.n_net = 0;
        results_fill();
    } else if (code == LV_EVENT_READY) {
        if (strlen(L.query) < 2) return;
        L.n_net = 0;
        L.searching = true;
        static const bs_prov_t SP[] = { BS_P_TWELVE, BS_P_FINNHUB, BS_P_ALPHA };
        bs_prov_t sp = BS_P_NONE;
        for (int i = 0; i < 3; i++) if (bs_cfg()->key[SP[i]][0]) { sp = SP[i]; break; }
        if (sp) {
            L.cg_next = true;
            bs_net_search(L.query, sp, search_done);
        } else {
            L.cg_next = false;
            bs_net_search(L.query, BS_P_COINGECKO, search_done);
        }
        results_fill();
    }
}

static void search_closed(lv_event_t *e)
{
    (void)e;
    L.ta = L.kb = L.results = NULL;
    L.searching = false;
}

static void search_open(void)
{
    char t[64];
    snprintf(t, sizeof t, _("Sumar a %s"), list_name(bs_cfg()->cur_list));
    lv_obj_t *body = bs_sheet(t, search_closed);
    lv_obj_update_layout(body);
    int32_t w = lv_obj_get_width(body), h = lv_obj_get_height(body);
    L.query[0] = 0;
    L.n_net = 0;
    L.ta = lv_textarea_create(body);
    lv_textarea_set_one_line(L.ta, true);
    lv_textarea_set_placeholder_text(L.ta, _("Nombre o símbolo"));
    lv_obj_set_style_text_font(L.ta, aos_font_body, 0);
    lv_obj_set_size(L.ta, w, 80);
    lv_obj_set_pos(L.ta, 0, 0);
    lv_obj_add_event_cb(L.ta, ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(L.ta, ta_cb, LV_EVENT_READY, NULL);
    int32_t kb_h = bs_ui.land ? h * 48 / 100 : 400;
    L.kb = lv_keyboard_create(lv_obj_get_parent(body));
    lv_obj_set_size(L.kb, bs_ui.W, kb_h);
    lv_obj_align(L.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(L.kb, aos_font_body);
    lv_keyboard_set_textarea(L.kb, L.ta);
    L.results = lv_obj_create(body);
    lv_obj_remove_style_all(L.results);
    lv_obj_set_size(L.results, w, h - 96 - kb_h + 4);
    lv_obj_set_pos(L.results, 0, 96);
    lv_obj_set_flex_flow(L.results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(L.results, 10, 0);
    lv_obj_set_scroll_dir(L.results, LV_DIR_VER);
    lv_obj_add_flag(L.results, LV_OBJ_FLAG_CLICKABLE);
    results_fill();
}
