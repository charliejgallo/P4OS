/*
 * P4OS - CAN: the signals.
 *
 * The signals of <card>/can/senales.dbc, each with its value now, decoded
 * from the last frame of its id; and one of them drawn over the last 30
 * seconds. A signal can also come from the "Por id" view - a byte, or two
 * bytes in either order - without writing a .dbc: it goes at the end of the
 * list as "0C0 B2-3" and is lost with the app.
 *
 * The chart is sampled every 100 ms from the table of ids, so it shows what
 * the bus said at that moment, not every frame: a 100 Hz signal is seen at
 * 10 Hz. That is what a chart on this screen can show anyway. An id that
 * stopped coming leaves a gap.
 */
#include "can.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HIST     300            /* 30 s at 10 Hz */
#define STALE_US 2000000

static cn_sig_t s_sig[CN_SIG_MAX + 4];
static int s_nsig, s_nfile;
static char s_err[96];
static int s_cur = 0;
static float s_hist[HIST];
static bool s_have[HIST];
static int s_head, s_n;

static struct {
    lv_obj_t *chart, *title, *value, *range, *list, *info;
    lv_chart_series_t *ser;
    lv_obj_t *rows[CN_SIG_MAX + 4], *vals[CN_SIG_MAX + 4];
} G;

static void hist_clear(void)
{
    s_head = s_n = 0;
}

void cn_sig_reload(void)
{
    s_nfile = s_nsig = cn_sig_load(s_sig, CN_SIG_MAX, s_err, sizeof s_err);
    if (s_cur >= s_nsig) s_cur = 0;
    hist_clear();
}

void cn_sig_adhoc(uint32_t id, uint8_t fl, int byte, int bits, bool motorola)
{
    if (s_nsig >= CN_SIG_MAX + 4) {
        /* the ad hoc ones take turns in the last places */
        memmove(&s_sig[s_nfile], &s_sig[s_nfile + 1], sizeof(cn_sig_t) * (size_t)(s_nsig - s_nfile - 1));
        s_nsig--;
    }
    cn_sig_t *g = &s_sig[s_nsig];
    memset(g, 0, sizeof *g);
    g->id = id;
    g->ext = fl & CN_EXT;
    g->len = (uint16_t)bits;
    g->motorola = bits == 16 && motorola;
    g->start = (uint16_t)(g->motorola ? byte * 8 + 7 : byte * 8);
    g->scale = 1;
    cn_frame_t f = { .id = id, .fl = fl };
    char t[12];
    cn_fmt_id(&f, t, sizeof t);
    unsigned b = (unsigned)byte & 7;
    if (bits == 8) snprintf(g->name, sizeof g->name, "%.8s B%u", t, b);
    else snprintf(g->name, sizeof g->name, "%.8s B%u-%u%s", t, b, (b + 1) & 7, motorola ? "" : " LE");
    s_cur = s_nsig++;
    hist_clear();
}

/* the signal's value from the last frame of its id; false if none recent */
static bool value_now(const cn_sig_t *g, float *v, uint64_t now)
{
    bool ok = false;
    cn_lock();
    for (int i = 0; i < CB.nids; i++) {
        const cn_id_t *e = &CB.ids[i];
        if (e->id != g->id || !(e->fl & CN_EXT) != !g->ext) continue;
        ok = now - e->last_us < STALE_US && cn_sig_value(g, e->data, e->len, v);
        break;
    }
    cn_unlock();
    return ok;
}

void cn_sig_sample(void)
{
    if (!CB.open || U.paused || s_cur >= s_nsig || !CB.ids) return;
    float v = 0;
    bool ok = value_now(&s_sig[s_cur], &v, cn_us());
    s_hist[s_head] = v;
    s_have[s_head] = ok;
    s_head = (s_head + 1) % HIST;
    if (s_n < HIST) s_n++;
}

/* lroundf is not in the firmware's table */
static long round_l(float v) { return (long)(v < 0 ? v - 0.5f : v + 0.5f); }

static void fmt_val(float v, const char *unit, char *out, size_t cap)
{
    float a = fabsf(v);
    long c;
    if (a >= 1000 || v == (float)(int32_t)v) snprintf(out, cap, "%ld %s", round_l(v), unit);
    else if (a >= 10) {
        c = round_l(v * 10);
        snprintf(out, cap, "%s%ld.%01ld %s", c < 0 ? "-" : "", labs(c) / 10, labs(c) % 10, unit);
    } else {
        c = round_l(v * 100);
        snprintf(out, cap, "%s%ld.%02ld %s", c < 0 ? "-" : "", labs(c) / 100, labs(c) % 100, unit);
    }
}

void cn_sig_refresh(void)
{
    if (U.tab != CN_TAB_SIG || !G.list) return;
    uint64_t now = cn_us();
    char t[48];
    for (int i = 0; i < s_nsig; i++) {
        if (!G.vals[i]) continue;
        float v;
        if (value_now(&s_sig[i], &v, now)) fmt_val(v, s_sig[i].unit, t, sizeof t);
        else snprintf(t, sizeof t, "—");
        lv_label_set_text(G.vals[i], t);
    }
    if (!G.chart || s_cur >= s_nsig) return;

    /* the chart: the window's min and max, with a little air */
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < s_n; i++) {
        int k = (s_head - s_n + i + HIST) % HIST;
        if (!s_have[k]) continue;
        lo = fminf(lo, s_hist[k]);
        hi = fmaxf(hi, s_hist[k]);
    }
    bool any = lo <= hi;
    if (!any) { lo = 0; hi = 1; }
    float span = hi - lo;
    if (span < 1e-3f) span = fabsf(hi) > 1 ? fabsf(hi) * 0.1f : 1;
    float bot = lo - span * 0.08f, top = hi + span * 0.08f;
    /* the chart takes integers: 1000 steps over the range */
    lv_chart_set_axis_range(G.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    int32_t *ys = lv_chart_get_series_y_array(G.chart, G.ser);
    for (int i = 0; i < HIST; i++) {
        int k = (s_head - HIST + i + HIST) % HIST;
        bool in = i >= HIST - s_n && s_have[k];
        ys[i] = in ? (int32_t)((s_hist[k] - bot) / (top - bot) * 1000) : LV_CHART_POINT_NONE;
    }
    lv_chart_refresh(G.chart);
    int last = (s_head - 1 + HIST) % HIST;
    if (s_n && s_have[last]) fmt_val(s_hist[last], s_sig[s_cur].unit, t, sizeof t);
    else snprintf(t, sizeof t, "—");
    lv_label_set_text(G.value, t);
    if (any) {
        char a[24], b[24], r[64];
        fmt_val(lo, "", a, sizeof a);
        fmt_val(hi, "", b, sizeof b);
        snprintf(r, sizeof r, _("30 s · de %s a %s"), a, b);
        lv_label_set_text(G.range, r);
    } else {
        lv_label_set_text(G.range, CB.open ? _("Esperando su id…") : _("Sin bus"));
    }
}

static void title_show(void)
{
    if (!G.title) return;
    if (s_cur >= s_nsig) {
        lv_label_set_text(G.title, _("Sin señales"));
        return;
    }
    const cn_sig_t *g = &s_sig[s_cur];
    cn_frame_t f = { .id = g->id, .fl = g->ext ? CN_EXT : 0 };
    char id[12], t[64];
    cn_fmt_id(&f, id, sizeof id);
    if (!strncmp(g->name, id, strlen(id))) snprintf(t, sizeof t, "%s", g->name);      /* an ad hoc one */
    else snprintf(t, sizeof t, "%s · %s", g->name, id);
    lv_label_set_text(G.title, t);
}

static void row_style(int i)
{
    if (!G.rows[i]) return;
    lv_obj_set_style_bg_color(G.rows[i], i == s_cur ? lv_color_hex(0x123A66) : AOS_C_CARD, 0);
}

static void pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int old = s_cur;
    s_cur = i;
    hist_clear();
    row_style(old);
    row_style(i);
    title_show();
    if (s_cur < s_nsig && G.rows[s_cur]) lv_obj_scroll_to_view(G.rows[s_cur], LV_ANIM_OFF);
    cn_sig_refresh();
}

static void reload_cb(lv_event_t *e)
{
    (void)e;
    cn_sig_reload();
    cn_ui_set_tab(CN_TAB_SIG);
    aos_ui_toast(s_err[0] ? s_err : _("Señales leídas de nuevo"), 1800);
}

void cn_sig_build(lv_obj_t *parent)
{
    memset(&G, 0, sizeof G);
    int32_t w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);
    int32_t gw = U.land ? w * 3 / 5 : w, gh = U.land ? h : h * 2 / 5;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, gw, gh);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    G.title = aos_label(card, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(G.title, gw - 260);
    lv_label_set_long_mode(G.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(G.title, 16, 12);
    G.value = aos_label(card, "", aos_font_title, AOS_C_TEXT);
    lv_obj_align(G.value, LV_ALIGN_TOP_RIGHT, -16, 6);
    G.range = aos_label(card, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_pos(G.range, 16, 44);
    G.chart = lv_chart_create(card);
    lv_obj_set_size(G.chart, gw - 32, gh - 96);
    lv_obj_set_pos(G.chart, 16, 80);
    lv_chart_set_type(G.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(G.chart, HIST);
    lv_chart_set_div_line_count(G.chart, 4, 6);
    lv_obj_set_style_bg_opa(G.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(G.chart, 0, 0);
    lv_obj_set_style_line_color(G.chart, lv_color_hex(0x222A35), 0);
    lv_obj_set_style_size(G.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(G.chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(G.chart, 0, 0);
    G.ser = lv_chart_add_series(G.chart, AOS_C_TEAL, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(G.chart, G.ser, LV_CHART_POINT_NONE);

    G.list = lv_obj_create(parent);
    lv_obj_remove_style_all(G.list);
    if (U.land) {
        lv_obj_set_pos(G.list, gw + 16, 0);
        lv_obj_set_size(G.list, w - gw - 16, h);
    } else {
        lv_obj_set_pos(G.list, 0, gh + 12);
        lv_obj_set_size(G.list, w, h - gh - 12);
    }
    lv_obj_set_flex_flow(G.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(G.list, 8, 0);
    lv_obj_set_scroll_dir(G.list, LV_DIR_VER);

    for (int i = 0; i < s_nsig; i++) {
        const cn_sig_t *g = &s_sig[i];
        lv_obj_t *r = lv_obj_create(G.list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), 72);
        lv_obj_set_style_radius(r, 16, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        G.rows[i] = r;
        row_style(i);
        lv_obj_t *n = aos_label(r, g->name, aos_font_small, AOS_C_TEXT);
        lv_obj_set_width(n, LV_PCT(55));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        G.vals[i] = aos_label(r, "—", &aos_mono_22, AOS_C_TEAL);
        lv_obj_align(G.vals[i], LV_ALIGN_RIGHT_MID, 0, 0);
        aos_make_decorative(n);
        aos_make_decorative(G.vals[i]);
    }
    char info[160];
    if (s_err[0] && !s_nfile) snprintf(info, sizeof info, "%s", s_err);
    else snprintf(info, sizeof info, _("%d señales de can/senales.dbc (se edita desde el portal)"), s_nfile);
    G.info = aos_label(G.list, info, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(G.info, LV_PCT(100));
    lv_label_set_long_mode(G.info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *rb = cn_btn(G.list, AOS_SYM_RESTART, _("Leer de nuevo"), reload_cb, NULL);
    lv_obj_set_width(rb, LV_SIZE_CONTENT);

    title_show();
    if (s_cur < s_nsig && G.rows[s_cur]) lv_obj_scroll_to_view(G.rows[s_cur], LV_ANIM_OFF);
    cn_sig_refresh();
}
