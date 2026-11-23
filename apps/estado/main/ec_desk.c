/*
 * P4OS - Mi casa: desk mode.
 *
 * To leave the board on a shelf: the whole screen (over the status bar, on
 * the display's top layer), the time and one line of summary at the top,
 * and a tile per service as big as they fit, tinted by its state, with the
 * name and the latency or "Caído" in large type. The screen stays on while
 * it is up (aos_hal_activity). A tap anywhere, or the back gesture, closes
 * it; leaving the app does too.
 */
#include "ec.h"

#include <time.h>

#define A ec_app
#define GAP 14

typedef struct {
    lv_obj_t *card, *name, *big, *sub;
    uint32_t  seq;
} dtile_t;

static struct {
    lv_obj_t *root, *clock, *summary;
    dtile_t   t[EC_MAX];
    int       n;
    uint32_t  last_s, awake;
    const lv_font_t *f_name, *f_big;
} D;

bool ec_desk_on(void) { return D.root != NULL; }

static void close_cb(lv_event_t *e)
{
    (void)e;
    ec_desk_close();
}

static lv_color_t tint(int st)
{
    switch (st) {
    case EC_ST_UP: return lv_color_hex(0x0E2A19);
    case EC_ST_SLOW: return lv_color_hex(0x3A3110);
    case EC_ST_DOWN: return lv_color_hex(0x6A1616);
    default: return AOS_C_CARD;
    }
}

static void tile_fill(int i, const ec_rt_t *rt, uint32_t now)
{
    dtile_t *t = &D.t[i];
    int st = rt->state;
    lv_obj_set_style_bg_color(t->card, tint(st), 0);
    char big[48], sub[96];
    sub[0] = '\0';
    if (st == EC_ST_UP || st == EC_ST_SLOW) {
        ec_fmt_ms(big, sizeof big, rt->ms);
    } else if (st == EC_ST_DOWN) {
        ec_copy(big, sizeof big, _("Caído"));
        ec_fmt_ago(sub, sizeof sub, (now - rt->since_ms) / 1000u);
    } else {
        ec_copy(big, sizeof big, ec_state_name(st));
    }
    ec_set_text(t->big, big);
    lv_obj_set_style_text_color(t->big, st == EC_ST_UP ? AOS_C_GREEN : st == EC_ST_SLOW ? AOS_C_YELLOW
                                        : st == EC_ST_DOWN ? lv_color_hex(0xFFD1CF) : AOS_C_DIM, 0);
    if (t->sub) ec_set_text(t->sub, sub);
}

static void summary(void)
{
    ec_lock();
    int down = ec_count(EC_ST_DOWN), slow = ec_count(EC_ST_SLOW);
    ec_unlock();
    char b[96];
    lv_color_t c = AOS_C_GREEN;
    if (!A.cfg.n) ec_copy(b, sizeof b, _("Sin servicios"));
    else if (!ec_net_ok()) {
        ec_copy(b, sizeof b, _("Sin red"));
        c = AOS_C_ORANGE;
    } else if (down) {
        snprintf(b, sizeof b, down == 1 ? _("%d caído") : _("%d caídos"), down);
        c = AOS_C_RED;
    } else if (slow) {
        snprintf(b, sizeof b, slow == 1 ? _("%d lento") : _("%d lentos"), slow);
        c = AOS_C_YELLOW;
    } else {
        ec_copy(b, sizeof b, _("Todo anda"));
    }
    ec_set_text(D.summary, b);
    lv_obj_set_style_text_color(D.summary, c, 0);
    if (aos_hal_time_is_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
        ec_set_text(D.clock, b);
    }
}

static void layout(void)
{
    lv_obj_clean(D.root);
    memset(D.t, 0, sizeof D.t);
    lv_obj_t *top = lv_layer_top();
    int32_t W = lv_obj_get_width(top), H = lv_obj_get_height(top);
    lv_obj_set_size(D.root, W, H);
    bool land = W > H;
    const int32_t pad = 28, head = land ? 96 : 132;
    D.clock = aos_label(D.root, "", aos_font_large, AOS_C_TEXT);
    lv_obj_set_pos(D.clock, pad, land ? 18 : 40);
    aos_make_decorative(D.clock);
    D.summary = ec_text(D.root, "", aos_font_title, AOS_C_GREEN, 0);
    lv_obj_set_style_text_align(D.summary, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(D.summary, LV_ALIGN_TOP_RIGHT, -pad, land ? 28 : 52);

    int n = A.cfg.n;
    D.n = n;
    int32_t aw = W - 2 * pad, ah = H - head - pad;
    /* the column count that gives the biggest tiles */
    int best = 1;
    int32_t best_score = -1, tw = aw, th = ah;
    for (int cols = 1; cols <= 6 && n; cols++) {
        int rows = (n + cols - 1) / cols;
        int32_t w = (aw - (cols - 1) * GAP) / cols, h = (ah - (rows - 1) * GAP) / rows;
        int32_t hh = h > w * 3 / 4 ? w * 3 / 4 : h;
        /* the tallest tile whose name and number still fit across */
        int32_t score = w >= 280 || cols == 1 ? hh : -1;
        if (score > best_score) {
            best_score = score;
            best = cols;
            tw = w;
            th = h;
        }
    }
    if (th > tw * 3 / 4) th = tw * 3 / 4;
    if (th >= 220 && tw >= 320) {
        D.f_name = aos_font_title;
        D.f_big = aos_font_huge;
    } else if (th >= 150) {
        D.f_name = aos_font_body;
        D.f_big = aos_font_large;
    } else {
        D.f_name = aos_font_small;
        D.f_big = aos_font_title;
    }
    lv_obj_t *grid = ec_box(D.root, aw, ah);
    lv_obj_set_pos(grid, pad, head);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, GAP, 0);
    lv_obj_set_style_pad_row(grid, GAP, 0);
    int rows = n ? (n + best - 1) / best : 0;
    int32_t used = rows * th + (rows - 1) * GAP;
    if (used < ah) lv_obj_set_style_pad_top(grid, (ah - used) / 2, 0);
    aos_make_decorative(grid);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    int32_t in = th >= 150 ? 24 : 16;
    for (int i = 0; i < n; i++) {
        dtile_t *t = &D.t[i];
        t->card = ec_card(grid, tw, th);
        lv_obj_set_style_radius(t->card, th >= 150 ? 28 : 18, 0);
        char name[EC_NAME * 2];
        aos_text_safe(name, sizeof name, A.cfg.s[i].name);
        t->name = ec_text(t->card, name, D.f_name, AOS_C_TEXT, tw - 2 * in);
        lv_obj_set_pos(t->name, in, in - 4);
        t->big = ec_text(t->card, "", D.f_big, AOS_C_TEXT, tw - 2 * in);
        lv_obj_align(t->big, LV_ALIGN_BOTTOM_LEFT, in, -in + 6);
        if (th >= 190) {
            t->sub = ec_text(t->card, "", aos_font_small, lv_color_hex(0xFFD1CF), tw - 2 * in);
            lv_obj_set_pos(t->sub, in, in - 4 + lv_font_get_line_height(D.f_name) + 2);
        }
        ec_lock();
        ec_rt_t rt = *ec_rt(i);
        ec_unlock();
        t->seq = rt.seq;
        tile_fill(i, &rt, now);
    }
    if (!n) {
        lv_obj_t *l = aos_label(grid, _("Sin servicios todavía"), aos_font_title, AOS_C_DIM);
        aos_make_decorative(l);
    }
    summary();
}

void ec_desk_open(void)
{
    if (D.root) return;
    memset(&D, 0, sizeof D);
    D.root = ec_box(lv_layer_top(), 10, 10);
    lv_obj_set_style_bg_color(D.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(D.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(D.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(D.root, close_cb, LV_EVENT_CLICKED, NULL);
    aos_ui_statusbar_set_visible(false);
    A.desk = true;
    layout();
    aos_hal_activity();
}

void ec_desk_close(void)
{
    if (!D.root) return;
    lv_obj_delete(D.root);
    memset(&D, 0, sizeof D);
    A.desk = false;
    aos_ui_statusbar_set_visible(true);
}

void ec_desk_relayout(void)
{
    if (D.root) layout();
}

void ec_desk_tick(void)
{
    if (!D.root) return;
    if (D.n != A.cfg.n) {
        layout();
        return;
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    bool second = now / 1000u != D.last_s;
    D.last_s = now / 1000u;
    for (int i = 0; i < D.n; i++) {
        ec_lock();
        ec_rt_t rt = *ec_rt(i);
        ec_unlock();
        if (rt.seq == D.t[i].seq && !second) continue;
        D.t[i].seq = rt.seq;
        tile_fill(i, &rt, now);
    }
    if (second) summary();
    /* desk mode is meant to stay on */
    if (now - D.awake > 10000) {
        D.awake = now;
        aos_hal_activity();
    }
}
