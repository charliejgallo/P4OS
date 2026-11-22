/*
 * P4OS - Bolsa: stocks, indices, currencies and crypto, with charts.
 *
 * The app: its life cycle, the symbols it holds in memory and when each
 * one is refreshed, the price alerts, the navigation between the lists and
 * one symbol, and the small widgets both screens use.
 *
 * It lives in the background (AOS_APP_FLAG_BACKGROUND) so the alerts keep
 * working while another app is in front: hidden, it only asks for the
 * symbols that have an alert, and no faster than every ten minutes.
 *
 * Nothing it shows is advice: the footer says so, says the data are
 * delayed and names the source, on every screen.
 */
#include "bs.h"
#include "aos_icon_ops.h"

#include <time.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

/* Three candles over a dark blue: down, up, up. Percent of the icon. */
static const uint8_t BOLSA_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -24,  12,  4, 36, 2, AIC_C_RED,   255),
    AIC_RECT(AIC_CENTER, -24,  14, 14, 20, 3, AIC_C_RED,   255),
    AIC_RECT(AIC_CENTER,   0,   0,  4, 44, 2, AIC_C_GREEN, 255),
    AIC_RECT(AIC_CENTER,   0,   2, 14, 24, 3, AIC_C_GREEN, 255),
    AIC_RECT(AIC_CENTER,  24, -12,  4, 40, 2, AIC_C_GREEN, 255),
    AIC_RECT(AIC_CENTER,  24, -10, 14, 26, 3, AIC_C_GREEN, 255),
    AIC_END
};

bs_ui_t bs_ui;

static void *ps_calloc(size_t n)
{
    void *p = NULL;
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
#endif
    if (!p) p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

/* ---- items ---- */

static bs_item_t *s_items[BS_MAX_ITEMS];
static int s_nitems;
static bool s_hidden;
static uint32_t s_quotes_dirty_ms;

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

int bs_items_count(void) { return s_nitems; }
bs_item_t *bs_item_at(int i) { return i >= 0 && i < s_nitems ? s_items[i] : NULL; }

bs_item_t *bs_item_find(const char *sym)
{
    for (int i = 0; i < s_nitems; i++)
        if (!strcmp(s_items[i]->sym, sym)) return s_items[i];
    return NULL;
}

static bool in_any_list(const char *sym)
{
    bs_cfg_t *c = bs_cfg();
    for (int l = 0; l < c->nlists; l++)
        if (bs_cfg_has(l, sym)) return true;
    return bs_alert_find(sym, 0) >= 0;
}

static void item_free(bs_item_t *it)
{
    for (int r = 0; r < BS_R_COUNT; r++) bs_series_free(&it->s[r]);
    free(it);
}

bs_item_t *bs_item(const char *sym)
{
    bs_item_t *it = bs_item_find(sym);
    if (it) return it;
    if (s_nitems >= BS_MAX_ITEMS) {
        /* the one used longest ago that is in no list goes */
        int old = -1;
        for (int i = 0; i < s_nitems; i++)
            if (!in_any_list(s_items[i]->sym) && strcmp(s_items[i]->sym, bs_ui.sym) &&
                (old < 0 || (int32_t)(s_items[i]->used_ms - s_items[old]->used_ms) < 0))
                old = i;
        if (old < 0) return NULL;
        item_free(s_items[old]);
        s_items[old] = s_items[--s_nitems];
    }
    it = ps_calloc(sizeof *it);
    if (!it) return NULL;
    bs_scpy(it->sym, sizeof it->sym, sym);
    bs_sym_info(sym, &it->info);
    it->last_alert_price = NAN;
    s_items[s_nitems++] = it;
    return it;
}

static void items_free(void)
{
    for (int i = 0; i < s_nitems; i++) item_free(s_items[i]);
    s_nitems = 0;
}

/* Which resolution a provider really fetches when one is asked: -1 none */
static int src_res(bs_prov_t p, int res)
{
    if (p == BS_P_ARGDATOS) return BS_R_FULL;
    if (p == BS_P_COINGECKO && res == BS_R_FULL) return BS_R_YEAR;
    return bs_prov_can(p, res) ? res : -1;
}

/* Does the provider have quotes of its own, or do they come from a series? */
static bool has_quotes(bs_prov_t p)
{
    return p == BS_P_COINGECKO || p == BS_P_TWELVE || p == BS_P_FINNHUB || p == BS_P_ALPHA;
}

static int32_t age_s(int32_t fetched, uint32_t fetched_ms)
{
    if (fetched_ms) return (int32_t)((now_ms() - fetched_ms) / 1000);
    int32_t now = bs_now();
    if (now && fetched) return now - fetched;
    return INT32_MAX;
}

static int32_t series_max_age(bs_item_t *it, int res, bs_prov_t p)
{
    int32_t q = bs_cfg()->refresh_min * 60;
    switch (res) {
    case BS_R_DAY:  return it->info.kind == BS_K_CRYPTO ? 600 : q > 900 ? q : 900;
    case BS_R_WEEK: return 1800;
    case BS_R_YEAR: return has_quotes(p) ? 6 * 3600 : q;
    default:        return p == BS_P_ARGDATOS ? (q > 1800 ? q : 1800) : 12 * 3600;
    }
}

static void ask_series(bs_item_t *it, int r, bs_prov_t p, bool front)
{
    if ((int32_t)(now_ms() - it->asked_ms[r]) < 20000 && it->asked_ms[r]) return;
    it->asked_ms[r] = now_ms() | 1;
    bs_job_t j;
    memset(&j, 0, sizeof j);
    j.kind = BS_J_SERIES;
    j.prov = (uint8_t)p;
    j.res = (uint8_t)r;
    bs_scpy(j.sym, sizeof j.sym, it->sym);
    bs_net_queue(&j, front);
}

bs_series_t *bs_item_series(bs_item_t *it, int res, bool ask)
{
    bs_prov_t p = bs_sym_prov_hist(&it->info);
    if (p == BS_P_NONE) return NULL;
    int r = src_res(p, res);
    if (r < 0) return NULL;
    it->used_ms = now_ms();
    if (!it->loaded[r]) {
        it->loaded[r] = true;
        bs_cache_load(it->sym, r, &it->s[r]);
    }
    bs_series_t *s = &it->s[r];
    if (ask && (s->n == 0 || s->prov != p || age_s(s->fetched, s->fetched_ms) >= series_max_age(it, r, p)))
        ask_series(it, r, p, !strcmp(it->sym, bs_ui.sym));
    return s;
}

/* Quotes wanted but not asked yet, gathered per provider into one request */
static char s_want[BS_P_COUNT][256];

void bs_item_want_quote(bs_item_t *it)
{
    bs_prov_t p = bs_sym_prov_quote(&it->info);
    if (p == BS_P_NONE) return;
    if (!has_quotes(p)) {
        /* the quote is the series' last point */
        bs_item_series(it, p == BS_P_ARGDATOS ? BS_R_FULL : BS_R_YEAR, true);
        return;
    }
    int32_t max = bs_cfg()->refresh_min * 60;
    if (p == BS_P_ALPHA && max < 3600) max = 3600;          /* 25 a day */
    if (it->q.ok && it->q.prov == p && age_s(it->q.fetched, it->q.fetched_ms) < max) return;
    if (it->q_asked_ms && (int32_t)(now_ms() - it->q_asked_ms) < 30000) return;
    it->q_asked_ms = now_ms() | 1;
    size_t L = strlen(s_want[p]);
    if (L + strlen(it->sym) + 2 < sizeof s_want[p])
        snprintf(s_want[p] + L, sizeof s_want[p] - L, "%s%s", L ? "," : "", it->sym);
}

static void flush_quotes(void)
{
    for (int p = 1; p < BS_P_COUNT; p++) {
        if (!s_want[p][0]) continue;
        /* CoinGecko takes them all at once; Twelve Data eight (its credits a
         * minute); the others one by one */
        int per = p == BS_P_COINGECKO ? 25 : p == BS_P_TWELVE ? 8 : 1;
        const char *s = s_want[p];
        while (*s) {
            bs_job_t j;
            memset(&j, 0, sizeof j);
            j.kind = BS_J_QUOTES;
            j.prov = (uint8_t)p;
            size_t w = 0;
            int k = 0;
            while (*s && k < per) {
                const char *e = strchr(s, ',');
                size_t len = e ? (size_t)(e - s) : strlen(s);
                if (w + len + 2 < sizeof j.syms) {
                    if (w) j.syms[w++] = ',';
                    memcpy(j.syms + w, s, len);
                    w += len;
                    j.syms[w] = 0;
                }
                if (!k) {
                    size_t c = len < sizeof j.sym - 1 ? len : sizeof j.sym - 1;
                    memcpy(j.sym, s, c);
                    j.sym[c] = 0;
                }
                k++;
                s += len;
                if (*s == ',') s++;
            }
            /* quotes before sparklines: the price is what the row is for */
            bs_net_queue(&j, true);
        }
        s_want[p][0] = 0;
    }
}

void bs_item_quote_set(const char *sym, const bs_quote_t *q)
{
    bs_item_t *it = bs_item(sym);
    if (!it) return;
    it->q = *q;
    if (!it->info.name[0] || it->info.kind == BS_K_STOCK) bs_sym_info(sym, &it->info);   /* a name learned now */
    it->q.fetched = bs_now();
    it->q.fetched_ms = now_ms() | 1;
    it->err = 0;
    s_quotes_dirty_ms = now_ms() | 1;
    bs_check_alerts(it);
}

bool bs_item_quote(bs_item_t *it, bs_quote_t *out)
{
    memset(out, 0, sizeof *out);
    bs_prov_t pq = bs_sym_prov_quote(&it->info);
    bs_prov_t ph = bs_sym_prov_hist(&it->info);
    bs_series_t *s = NULL;
    if (ph != BS_P_NONE) {
        int r = src_res(ph, BS_R_YEAR);
        if (r >= 0) {
            if (!it->loaded[r]) { it->loaded[r] = true; bs_cache_load(it->sym, r, &it->s[r]); }
            s = &it->s[r];
        }
    }
    bs_quote_t fs;
    bs_quote_from_series(s, &fs, false);
    if (it->q.ok && (it->q.prov == pq || !fs.ok)) {
        *out = it->q;
        /* what the provider's quote lacks, from the series */
        if (!(out->has & BS_Q_52W) && (fs.has & BS_Q_52W)) {
            out->lo52 = fminf(fs.lo52, out->price);
            out->hi52 = fmaxf(fs.hi52, out->price);
            out->has |= BS_Q_52W;
        }
        if (!(out->has & BS_Q_PREV) && (fs.has & BS_Q_PREV)) {
            /* the last daily close before today's price */
            out->prev = s && s->n && s->p[s->n - 1].c != out->price ? s->p[s->n - 1].c : fs.prev;
            out->has |= BS_Q_PREV;
        }
        return true;
    }
    if (fs.ok) { *out = fs; return true; }
    return false;
}

int bs_item_spark(bs_item_t *it, const bs_pt_t **pts, float *base)
{
    *base = NAN;
    bs_quote_t q;
    bool hq = bs_item_quote(it, &q);
    bs_prov_t p = bs_sym_prov_hist(&it->info);
    if (p == BS_P_NONE) return 0;
    if (src_res(p, BS_R_DAY) == BS_R_DAY) {
        bs_series_t *d = bs_item_series(it, BS_R_DAY, !s_hidden);
        int32_t now = bs_now();
        if (d && d->n >= 4 && (!now || now - d->p[d->n - 1].t < 20 * 3600)) {
            *pts = d->p;
            if (hq && (q.has & BS_Q_PREV)) *base = q.prev;
            return d->n;
        }
    }
    bs_series_t *y = bs_item_series(it, BS_R_YEAR, !s_hidden);
    if (!y || y->n < 2) return 0;
    int n = y->n < 22 ? y->n : 22;
    *pts = y->p + y->n - n;
    *base = y->p[y->n - n].c;
    return n;
}

/* ---- alerts ---- */

void bs_check_alerts(bs_item_t *it)
{
    bs_quote_t q;
    if (!bs_item_quote(it, &q) || !q.fetched_ms) return;     /* never on a cached price */
    bs_cfg_t *c = bs_cfg();
    bool changed = false;
    for (int i = bs_alert_find(it->sym, 0); i >= 0; i = bs_alert_find(it->sym, i + 1)) {
        bs_alert_t *a = &c->alert[i];
        if (a->fired) continue;
        bool hit = a->dir == BS_AL_ABOVE ? q.price >= a->value : q.price <= a->value;
        if (!hit) continue;
        a->fired = true;
        changed = true;
        char v[32], p[32], t[160];
        bs_fmt_price(a->value, &it->info, v, sizeof v);
        bs_fmt_price(q.price, &it->info, p, sizeof p);
        snprintf(t, sizeof t, a->dir == BS_AL_ABOVE ? _("%s superó %s: ahora %s") : _("%s bajó de %s: ahora %s"),
                 it->info.show, v, p);
        aos_ui_toast(t, 6000);
        aos_hal_beep(1760, 120);
        aos_hal_log("bolsa", "alert: %s", t);
    }
    if (changed) bs_cfg_save();
}

/* ---- the footer ---- */

void bs_footer_text(char *out, size_t n, const bs_info_t *only)
{
    bool used[BS_P_COUNT] = { false };
    bool missing = false;
    if (only) {
        bs_prov_t a = bs_sym_prov_quote(only), b = bs_sym_prov_hist(only);
        used[a] = used[b] = true;
        missing = a == BS_P_NONE;
    } else {
        bs_cfg_t *c = bs_cfg();
        if (c->cur_list >= 0 && c->cur_list < c->nlists) {
            bs_list_t *l = &c->list[c->cur_list];
            for (int i = 0; i < l->n; i++) {
                bs_info_t in;
                bs_sym_info(l->sym[i], &in);
                bs_prov_t a = bs_sym_prov_quote(&in);
                used[a] = true;
                used[bs_sym_prov_hist(&in)] = true;
                missing |= a == BS_P_NONE;
            }
        }
    }
    char src[96] = "";
    size_t w = 0;
    for (int p = 1; p < BS_P_COUNT; p++)
        if (used[p]) w += (size_t)snprintf(src + w, sizeof src - w, "%s%s", w ? ", " : "", bs_prov_name((bs_prov_t)p));
    if (w) snprintf(out, n, _("Datos con demora · Fuente: %s · No es un consejo de inversión"), src);
    else snprintf(out, n, "%s", _("Datos con demora · No es un consejo de inversión"));
    (void)missing;
}

/* ---- widgets ---- */

lv_obj_t *bs_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *bo_label(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

lv_obj_t *bs_round_btn(lv_obj_t *parent, const char *glyph, int32_t size, lv_color_t bg,
                       lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = bo_label(b, glyph, &aos_sym_28, AOS_C_TEXT);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

lv_obj_t *bs_chip(lv_obj_t *parent, const char *txt, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_bg_color(b, on ? AOS_C_TEXT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t *l = bo_label(b, txt, aos_font_small, on ? AOS_C_BG : AOS_C_TEXT);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* ---- sheets: a full overlay over the screen ---- */

static lv_obj_t *s_sheet;
static lv_obj_t *s_sheet_body;
static lv_event_cb_t s_sheet_close_cb;

static void sheet_del_cb(void *o) { lv_obj_delete(o); }

static void sheet_x_cb(lv_event_t *e)
{
    (void)e;
    bs_sheet_close();
}

lv_obj_t *bs_sheet(const char *title, lv_event_cb_t on_close)
{
    bs_sheet_close();
    s_sheet_close_cb = on_close;
    s_sheet = lv_obj_create(bs_ui.root);
    lv_obj_remove_style_all(s_sheet);
    lv_obj_set_size(s_sheet, bs_ui.W, bs_ui.H);
    lv_obj_set_style_bg_color(s_sheet, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = bo_label(s_sheet, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_pos(t, AOS_UI_PAD + 4, 18);
    lv_obj_set_width(t, bs_ui.W - 2 * AOS_UI_PAD - 100);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *x = bs_round_btn(s_sheet, AOS_SYM_CLOSE, 72, AOS_C_CARD2, sheet_x_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 10);
    s_sheet_body = bs_box(s_sheet, bs_ui.W - 2 * AOS_UI_PAD, bs_ui.H - 100);
    lv_obj_set_pos(s_sheet_body, AOS_UI_PAD, 96);
    return s_sheet_body;
}

lv_obj_t *bs_sheet_obj(void) { return s_sheet ? s_sheet_body : NULL; }

void bs_sheet_close(void)
{
    if (!s_sheet) return;
    lv_obj_t *o = s_sheet;
    lv_event_cb_t cb = s_sheet_close_cb;
    s_sheet = NULL;
    s_sheet_body = NULL;
    s_sheet_close_cb = NULL;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    /* deleted later, not here: this may be one of its own buttons' events */
    lv_async_call(sheet_del_cb, o);
    if (cb) cb(NULL);
}

/* ---- navigation ---- */

static void leave(void)
{
    if (bs_ui.view == BS_V_DETAIL) bs_detail_leave();
    else bs_list_leave();
}

static void build(void)
{
    if (bs_ui.view == BS_V_DETAIL) bs_detail_build(bs_ui.sym);
    else bs_list_build();
}

static void clean_root(void)
{
    if (s_sheet) {
        /* a sheet open over the screen goes with it, and its pending delete
         * is cancelled so the object is not deleted twice */
        lv_obj_t *o = s_sheet;
        s_sheet = NULL;
        s_sheet_close_cb = NULL;
        lv_async_call_cancel(sheet_del_cb, o);
    }
    lv_obj_clean(bs_ui.root);
}

void bs_go(int view, const char *sym)
{
    leave();
    clean_root();
    bs_ui.view = view;
    if (sym) bs_scpy(bs_ui.sym, sizeof bs_ui.sym, sym);
    if (view == BS_V_LIST) bs_ui.sym[0] = 0;
    build();
}

static void on_data(const char *sym, int what)
{
    if (s_hidden) return;
    if (bs_sheet_obj() && bs_ui.view == BS_V_LIST) return;
    if (bs_ui.view == BS_V_DETAIL) bs_detail_refresh(sym, what);
    else bs_list_refresh(sym);
}

/* ---- what is refreshed ---- */

static void want_visible(void)
{
    bs_cfg_t *c = bs_cfg();
    if (bs_ui.view == BS_V_DETAIL && bs_ui.sym[0]) {
        bs_item_t *it = bs_item(bs_ui.sym);
        if (it) bs_item_want_quote(it);
    } else if (c->cur_list >= 0 && c->cur_list < c->nlists) {
        bs_list_t *l = &c->list[c->cur_list];
        for (int i = 0; i < l->n; i++) {
            bs_item_t *it = bs_item(l->sym[i]);
            if (it) bs_item_want_quote(it);
        }
    }
}

static void want_alerts(void)
{
    bs_cfg_t *c = bs_cfg();
    for (int i = 0; i < c->nalerts; i++) {
        if (c->alert[i].fired) continue;
        bs_item_t *it = bs_item(c->alert[i].sym);
        if (it) bs_item_want_quote(it);
    }
}

static lv_timer_t *s_timer;
static uint32_t s_last_want, s_last_cfg, s_last_alert;

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t n = now_ms();
    bs_net_poll();
    if (!s_hidden) {
        if (bs_ui.view == BS_V_DETAIL) bs_detail_tick();
        if (n - s_last_want > 2000) {
            s_last_want = n;
            want_visible();
            want_alerts();
            flush_quotes();
        }
    } else if (n - s_last_alert > 60000) {
        /* hidden: only the alerts, and want_quote keeps each provider's pace */
        s_last_alert = n;
        int keep = bs_cfg()->refresh_min;
        if (keep < 10) bs_cfg()->refresh_min = 10;
        want_alerts();
        flush_quotes();
        bs_cfg()->refresh_min = keep;
    }
    if (n - s_last_cfg > 3000) {
        s_last_cfg = n;
        if (bs_cfg_changed()) {
            aos_hal_log("bolsa", "bolsa.txt changed: reloading");
            bs_cfg_load();
            bs_net_init();
            for (int i = 0; i < s_nitems; i++) {
                bs_sym_info(s_items[i]->sym, &s_items[i]->info);
                bs_check_alerts(s_items[i]);     /* a new alert the price already crossed */
            }
            if (!s_hidden) bs_go(bs_ui.view, bs_ui.view == BS_V_DETAIL ? bs_ui.sym : NULL);
        }
    }
    if (s_quotes_dirty_ms && (int32_t)(n - s_quotes_dirty_ms) > 20000) {
        s_quotes_dirty_ms = 0;
        bs_quotes_save();
    }
}

/* ---- life cycle ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    bs_i18n_load();
    memset(&bs_ui, 0, sizeof bs_ui);
    bs_ui.root = root;
    bs_ui.W = lv_obj_get_width(root);
    bs_ui.H = lv_obj_get_height(root);
    bs_ui.land = bs_ui.W > bs_ui.H;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    bs_cfg_load();
    bs_names_load();
    bs_net_init();
    bs_on_data = on_data;
    /* every symbol of every list, with its last quote from the card */
    bs_cfg_t *c = bs_cfg();
    for (int l = 0; l < c->nlists; l++)
        for (int i = 0; i < c->list[l].n; i++) bs_item(c->list[l].sym[i]);
    bs_quotes_load();
    s_hidden = false;
    s_last_want = 0;
    s_last_alert = s_last_cfg = now_ms();
    bs_ui.view = BS_V_LIST;
    bs_list_build();
    s_timer = lv_timer_create(timer_cb, 200, NULL);
    aos_hal_log("bolsa", "started: %d lists, %d symbols, %d alerts", c->nlists, s_nitems, c->nalerts);
    return &bs_ui;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    leave();
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    bs_net_stop();
    bs_on_data = NULL;
    bs_quotes_save();
    bs_cfg_save();
    clean_root();
    items_free();
    memset(&bs_ui, 0, sizeof bs_ui);
    memset(s_want, 0, sizeof s_want);
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    leave();
    bs_ui.root = root;
    clean_root();
    bs_ui.W = lv_obj_get_width(root);
    bs_ui.H = lv_obj_get_height(root);
    bs_ui.land = bs_ui.W > bs_ui.H;
    build();
    return true;
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_sheet) { bs_sheet_close(); return true; }
    if (bs_ui.view == BS_V_DETAIL) {
        if (bs_detail_back()) return true;
        bs_go(BS_V_LIST, NULL);
        return true;
    }
    return bs_list_back();
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    s_hidden = false;
    s_last_want = 0;
    bs_i18n_load();
    if (bs_ui.view == BS_V_LIST) bs_list_refresh(NULL);
    else bs_detail_refresh(NULL, BS_D_QUOTE);
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    s_hidden = true;
    s_last_alert = 0;
}

static bool bolsa_init(aos_app_t *app)
{
    app->desc.id       = "aos.bolsa";
    app->desc.name     = "Bolsa";
    app->desc.icon     = "$";
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x1E3A5F;
    app->desc.color_b  = 0x0A1424;
    app->desc.order    = 79;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND;
    aos_icon_set_ops(app, BOLSA_ICON, sizeof BOLSA_ICON);

    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    app->back    = back;
    app->show    = show;
    app->hide    = hide;
    return true;
}

AOS_APP_ENTRY(bolsa_init);
