/*
 * P4OS - CAN: the signals.
 *
 * The signals of the .dbc in use (can_dbc.c), each with its value now,
 * decoded from the last frame of its id, under the name of its message;
 * and one of them drawn over the last 30 seconds. A value with a name in
 * the file (VAL_: a gear "D", a door "abierta") shows the name.
 *
 * A car's .dbc has hundreds of messages and thousands of signals, more
 * than a list on this screen can hold or anybody can read, so the tab shows
 * those of some messages: the ones seen on the bus since the connection
 * (the default; with the bus closed and a small file, all of them), or the
 * ones chosen by hand in the "Mensajes" sheet (or the portal page). At most
 * VIEW_MAX signals are listed; the rest are counted.
 *
 * A signal can also come from the "Por id" view - a byte, or two bytes in
 * either order - without writing a .dbc: it goes at the end of the list as
 * "0C0 B2-3" and is lost with the app.
 *
 * The chart is sampled every 100 ms from the table of ids, so it shows what
 * the bus said at that moment, not every frame: a 100 Hz signal is seen at
 * 10 Hz. That is what a chart on this screen can show anyway. An id that
 * stopped coming leaves a gap; a multiplexed signal keeps its last value
 * while the frames carry the others.
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

#define HIST      300           /* 30 s at 10 Hz */
#define STALE_US  2000000
#define VIEW_MAX  160           /* signal rows in the tab */
#define ADHOC     4
#define PICK_MAX  300           /* message rows in the sheet */
#define FILES_MAX 64

static cn_sig_t s_adhoc[ADHOC];
static int s_nadhoc;
static const cn_sig_t *s_view[VIEW_MAX + ADHOC];
static int s_nview, s_nfile_view, s_more, s_nmsg_view;
static uint32_t s_view_key;
static char s_err[96];
static const cn_sig_t *s_cur;   /* the one charted */
static float s_hist[HIST];
static bool s_have[HIST];
static int s_head, s_n;
static uint32_t s_last_check;

static struct {
    lv_obj_t *chart, *title, *value, *range, *list, *info;
    lv_chart_series_t *ser;
    lv_obj_t *rows[VIEW_MAX + ADHOC], *vals[VIEW_MAX + ADHOC];
} G;

static void hist_clear(void)
{
    s_head = s_n = 0;
}

/* ---- what the tab shows ---- */

static int msg_cmp(const void *a, const void *b)
{
    const cn_msg_t *x = &CN_DBC.msg[*(const uint16_t *)a], *y = &CN_DBC.msg[*(const uint16_t *)b];
    if (x->ext != y->ext) return x->ext ? 1 : -1;
    return x->id < y->id ? -1 : x->id > y->id;
}

/* the messages to show, sorted by id; how many */
static int shown_msgs(uint16_t *out, int cap)
{
    int n = 0;
    if (!CN_DBC.msg) return 0;
    if (!CN_DBC.show_bus) {
        for (int i = 0; i < CN_DBC.nmsg && n < cap; i++)
            if (cn_dbc_shown(i)) out[n++] = (uint16_t)i;
    } else {
        cn_lock();
        for (int i = 0; CB.ids && i < CB.nids && n < cap; i++) {
            int k = cn_dbc_find(CB.ids[i].id, CB.ids[i].fl & CN_EXT);
            if (k >= 0) out[n++] = (uint16_t)k;
        }
        cn_unlock();
        /* nothing on the bus yet: a small file is shown whole */
        if (!n && CN_DBC.nsig <= VIEW_MAX)
            for (int i = 0; i < CN_DBC.nmsg && n < cap; i++) out[n++] = (uint16_t)i;
    }
    qsort(out, (size_t)n, sizeof out[0], msg_cmp);
    return n;
}

static uint32_t view_key(const uint16_t *m, int n)
{
    uint32_t h = 2166136261u ^ (uint32_t)n;
    for (int i = 0; i < n; i++) h = (h ^ m[i]) * 16777619u;
    return h ^ (uint32_t)s_nadhoc << 24;
}

static void view_compute(void)
{
    static uint16_t msgs[CN_DBC_MSG_MAX];
    int nm = shown_msgs(msgs, CN_DBC_MSG_MAX);
    s_view_key = view_key(msgs, nm);
    s_nview = s_more = 0;
    s_nmsg_view = nm;
    for (int i = 0; i < nm; i++) {
        const cn_msg_t *m = &CN_DBC.msg[msgs[i]];
        for (uint32_t k = m->sig0; k < m->sig0 + m->nsig; k++) {
            if (s_nview < VIEW_MAX) s_view[s_nview++] = &CN_DBC.sig[k];
            else s_more++;
        }
    }
    s_nfile_view = s_nview;
    for (int i = 0; i < s_nadhoc; i++) s_view[s_nview++] = &s_adhoc[i];
    bool found = false;
    for (int i = 0; i < s_nview && !found; i++) found = s_view[i] == s_cur;
    if (!found) {
        s_cur = s_nview ? s_view[0] : NULL;
        hist_clear();
    }
}

void cn_sig_reload(void)
{
    s_cur = NULL;
    cn_dbc_load(s_err, sizeof s_err);
    view_compute();
    hist_clear();
}

void cn_sig_adhoc(uint32_t id, uint8_t fl, int byte, int bits, bool motorola)
{
    if (s_nadhoc >= ADHOC) {
        /* the ad hoc ones take turns */
        memmove(&s_adhoc[0], &s_adhoc[1], sizeof(cn_sig_t) * (ADHOC - 1));
        s_nadhoc--;
    }
    cn_sig_t *g = &s_adhoc[s_nadhoc++];
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
    s_cur = g;
    view_compute();
    s_cur = g;
    hist_clear();
}

/* the signal's value from the last frame of its id. 0: none recent;
 * -1: the frame carries another multiplexed set; 1: *v (and *raw) */
static int value_now(const cn_sig_t *g, float *v, int32_t *raw, uint64_t now)
{
    int r = 0;
    cn_lock();
    if (g->muxed && CB.muxc && CB.nmuxspec) {
        /* the last frame of its id that carries it */
        const cn_muxframe_t *best = NULL;
        for (int i = 0; i < CN_MUXCACHE; i++) {
            const cn_muxframe_t *m = &CB.muxc[i];
            if (!m->t_us || m->id != g->id || !m->ext != !g->ext || now - m->t_us >= STALE_US) continue;
            if ((!best || m->t_us > best->t_us) && cn_sig_present(g, m->data, m->len)) best = m;
        }
        if (best && cn_sig_value(g, best->data, best->len, v) && cn_sig_raw(g, best->data, best->len, raw)) {
            cn_unlock();
            return 1;
        }
    }
    for (int i = 0; CB.ids && i < CB.nids; i++) {
        const cn_id_t *e = &CB.ids[i];
        if (e->id != g->id || !(e->fl & CN_EXT) != !g->ext) continue;
        if (now - e->last_us >= STALE_US) break;
        if (!cn_sig_present(g, e->data, e->len)) r = -1;
        else if (cn_sig_value(g, e->data, e->len, v) && cn_sig_raw(g, e->data, e->len, raw)) r = 1;
        break;
    }
    cn_unlock();
    return r;
}

void cn_sig_sample(void)
{
    if (!CB.open || U.paused || !s_cur || !CB.ids) return;
    float v = 0;
    int32_t raw;
    int r = value_now(s_cur, &v, &raw, cn_us());
    int prev = (s_head - 1 + HIST) % HIST;
    if (r < 0) {
        /* another multiplexed set: the last value holds */
        if (!s_n || !s_have[prev]) return;
        v = s_hist[prev];
        r = 1;
    }
    s_hist[s_head] = v;
    s_have[s_head] = r > 0;
    s_head = (s_head + 1) % HIST;
    if (s_n < HIST) s_n++;
}

/* lroundf is not in the firmware's table */
static long round_l(float v) { return (long)(v < 0 ? v - 0.5f : v + 0.5f); }

static void fmt_val(float v, const char *unit, char *out, size_t cap)
{
    float a = fabsf(v);
    long c;
    const char *sp = unit[0] ? " " : "";
    if (a >= 1000 || v == (float)(int32_t)v) snprintf(out, cap, "%ld%s%s", round_l(v), sp, unit);
    else if (a >= 10) {
        c = round_l(v * 10);
        snprintf(out, cap, "%s%ld.%01ld%s%s", c < 0 ? "-" : "", labs(c) / 10, labs(c) % 10, sp, unit);
    } else {
        c = round_l(v * 100);
        snprintf(out, cap, "%s%ld.%02ld%s%s", c < 0 ? "-" : "", labs(c) / 100, labs(c) % 100, sp, unit);
    }
}

static void view_rebuild(void)
{
    int32_t y = G.list ? lv_obj_get_scroll_y(G.list) : 0;
    cn_ui_set_tab(CN_TAB_SIG);
    if (G.list) lv_obj_scroll_to_y(G.list, y, LV_ANIM_OFF);
}

void cn_sig_refresh(void)
{
    /* the messages on the bus come and the hand-picked ones change: once a
     * second, is the list still the one drawn? */
    uint32_t now_ms = cn_ms();
    if (now_ms - s_last_check >= 1000) {
        s_last_check = now_ms;
        static uint16_t msgs[CN_DBC_MSG_MAX];
        int nm = shown_msgs(msgs, CN_DBC_MSG_MAX);
        if (view_key(msgs, nm) != s_view_key) {
            view_compute();
            if (U.tab == CN_TAB_SIG && G.list && !U.sheet) {
                view_rebuild();
                return;
            }
        }
    }
    if (U.tab != CN_TAB_SIG || !G.list) return;
    uint64_t now = cn_us();
    char t[48];
    for (int i = 0; i < s_nview; i++) {
        if (!G.vals[i]) continue;
        float v;
        int32_t raw;
        int r = value_now(s_view[i], &v, &raw, now);
        if (r < 0) continue;            /* keeps the last one */
        const char *nm = r > 0 ? cn_sig_val_name(s_view[i], raw) : NULL;
        if (nm) snprintf(t, sizeof t, "%s", nm);
        else if (r > 0) fmt_val(v, s_view[i]->unit, t, sizeof t);
        else snprintf(t, sizeof t, "-");
        lv_label_set_text(G.vals[i], t);
    }
    if (!G.chart || !s_cur) return;

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
    if (s_n && s_have[last]) {
        const char *nm = cn_sig_val_name(s_cur, (int32_t)round_l((s_hist[last] - s_cur->offset) /
                                                                 (s_cur->scale ? s_cur->scale : 1)));
        if (nm) snprintf(t, sizeof t, "%s", nm);
        else fmt_val(s_hist[last], s_cur->unit, t, sizeof t);
    } else snprintf(t, sizeof t, "-");
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
    if (!s_cur) {
        lv_label_set_text(G.title, _("Sin señales"));
        return;
    }
    const cn_sig_t *g = s_cur;
    cn_frame_t f = { .id = g->id, .fl = g->ext ? CN_EXT : 0 };
    char id[12], t[64];
    cn_fmt_id(&f, id, sizeof id);
    if (!strncmp(g->name, id, strlen(id))) snprintf(t, sizeof t, "%s", g->name);      /* an ad hoc one */
    else snprintf(t, sizeof t, "%s · %s", g->name, id);
    lv_label_set_text(G.title, t);
}

static void row_style(int i)
{
    if (i < 0 || i >= s_nview || !G.rows[i]) return;
    lv_obj_set_style_bg_color(G.rows[i], s_view[i] == s_cur ? lv_color_hex(0x123A66) : AOS_C_CARD, 0);
}

static void pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_nview) return;
    const cn_sig_t *old = s_cur;
    s_cur = s_view[i];
    hist_clear();
    for (int k = 0; k < s_nview; k++)
        if (s_view[k] == old) row_style(k);
    row_style(i);
    title_show();
    cn_sig_refresh();
}

static void reload_cb(lv_event_t *e)
{
    (void)e;
    cn_sig_reload();
    cn_ui_set_tab(CN_TAB_SIG);
    aos_ui_toast(s_err[0] ? s_err : _("Señales leídas de nuevo"), 1800);
}

/* ---- the sheets: which file, which messages ---- */

static lv_obj_t *note(lv_obj_t *body, const char *text, lv_color_t c)
{
    lv_obj_t *l = aos_label(body, text, aos_font_caption, c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *wrap_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(r, 10, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static lv_obj_t *card_row(lv_obj_t *parent, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(r, 14, 0);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_set_style_bg_color(r, on ? lv_color_hex(0x123A66) : AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    return r;
}

static char (*s_files)[64];
static int s_nfiles;

static void file_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_nfiles) return;
    cn_dbc_choose(s_files[i]);
    cn_sig_reload();
    cn_sheet_close();
    cn_ui_set_tab(CN_TAB_SIG);
    char t[128];
    if (s_err[0]) snprintf(t, sizeof t, "%s", s_err);
    else snprintf(t, sizeof t, _("%d mensajes, %d señales"), CN_DBC.nmsg, CN_DBC.nsig);
    aos_ui_toast(t, 2000);
}

static void file_sheet(void)
{
    static char names[FILES_MAX][64];
    static uint32_t sizes[FILES_MAX];
    s_files = names;
    s_nfiles = cn_dbc_files(names, sizes, FILES_MAX);
    lv_obj_t *body = cn_sheet_open(_("Archivo .dbc"));
    note(body, _("Los .dbc de la carpeta can/dbc de la tarjeta, y can/senales.dbc. Se bajan de bibliotecas abiertas "
                 "(opendbc de comma.ai, nberlette/canbus; las dos MIT) y se copian ahí desde la computadora o se "
                 "suben desde la página CAN del portal."), AOS_C_DIM);
    if (!s_nfiles) {
        note(body, _("Todavía no hay ninguno."), AOS_C_TEXT);
        return;
    }
    for (int i = 0; i < s_nfiles; i++) {
        lv_obj_t *r = card_row(body, !strcmp(names[i], CN_DBC.file), file_pick_cb, (void *)(intptr_t)i);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(r, 4, 0);
        lv_obj_t *l = aos_label(r, names[i], &aos_mono_22, AOS_C_TEXT);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        char t[48];
        snprintf(t, sizeof t, "%u KB", (unsigned)((sizes[i] + 1023) / 1024));
        aos_label(r, t, aos_font_caption, AOS_C_DIM);
        aos_make_decorative(l);
    }
}

static uint16_t s_pick[PICK_MAX];
static lv_obj_t *s_pick_rows[PICK_MAX];
static int s_npick;
static lv_obj_t *s_bus_pill, *s_hand_pill;

static void pills_show(void)
{
    if (!s_bus_pill) return;
    lv_obj_set_style_bg_color(s_bus_pill, CN_DBC.show_bus ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(s_hand_pill, CN_DBC.show_bus ? AOS_C_CARD2 : AOS_C_ACCENT, 0);
    char t[48];
    snprintf(t, sizeof t, _("Elegidos: %d"), CN_DBC.nshow);
    lv_label_set_text(lv_obj_get_child(s_hand_pill, 0), t);
}

static void bus_pill_cb(lv_event_t *e)
{
    (void)e;
    CN_DBC.show_bus = true;
    cn_dbc_save_sel();
    pills_show();
}

static void hand_pill_cb(lv_event_t *e)
{
    (void)e;
    if (!CN_DBC.nshow) {
        aos_ui_toast(_("Tocá los mensajes que querés ver"), 1800);
        return;
    }
    CN_DBC.show_bus = false;
    cn_dbc_save_sel();
    pills_show();
}

static void msg_toggle_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_npick) return;
    const cn_msg_t *m = &CN_DBC.msg[s_pick[i]];
    int at = -1;
    for (int k = 0; k < CN_DBC.nshow; k++)
        if (CN_DBC.show[k] == m->id && !CN_DBC.show_ext[k] == !m->ext) at = k;
    if (at >= 0) {
        memmove(&CN_DBC.show[at], &CN_DBC.show[at + 1], sizeof(uint32_t) * (size_t)(CN_DBC.nshow - at - 1));
        memmove(&CN_DBC.show_ext[at], &CN_DBC.show_ext[at + 1], (size_t)(CN_DBC.nshow - at - 1));
        CN_DBC.nshow--;
    } else if (CN_DBC.nshow < CN_SHOW_MAX) {
        CN_DBC.show[CN_DBC.nshow] = m->id;
        CN_DBC.show_ext[CN_DBC.nshow] = m->ext;
        CN_DBC.nshow++;
    } else {
        aos_ui_toast(_("Hasta 64 mensajes"), 1800);
        return;
    }
    CN_DBC.show_bus = CN_DBC.nshow == 0;
    cn_dbc_save_sel();
    lv_obj_set_style_bg_color(s_pick_rows[i], at < 0 ? lv_color_hex(0x123A66) : AOS_C_CARD, 0);
    pills_show();
}

static void none_cb(lv_event_t *e)
{
    (void)e;
    CN_DBC.nshow = 0;
    CN_DBC.show_bus = true;
    cn_dbc_save_sel();
    for (int i = 0; i < s_npick; i++) lv_obj_set_style_bg_color(s_pick_rows[i], AOS_C_CARD, 0);
    pills_show();
}

static bool on_bus(const cn_msg_t *m)
{
    bool r = false;
    cn_lock();
    for (int i = 0; CB.ids && i < CB.nids && !r; i++)
        r = CB.ids[i].id == m->id && !(CB.ids[i].fl & CN_EXT) == !m->ext;
    cn_unlock();
    return r;
}

static void msg_sheet(void)
{
    s_bus_pill = s_hand_pill = NULL;
    if (!CN_DBC.nmsg) {
        aos_ui_toast(s_err[0] ? s_err : _("El archivo no tiene mensajes"), 1800);
        return;
    }
    lv_obj_t *body = cn_sheet_open(_("Mensajes"));
    lv_obj_t *pr = wrap_row(body);
    s_bus_pill = cn_pill(pr, _("Los que pasan por el bus"), CN_DBC.show_bus, bus_pill_cb, NULL);
    s_hand_pill = cn_pill(pr, "", !CN_DBC.show_bus, hand_pill_cb, NULL);
    lv_obj_t *nb = cn_btn(pr, NULL, _("Ninguno"), none_cb, NULL);
    lv_obj_set_height(nb, 64);
    pills_show();
    char t[200];
    snprintf(t, sizeof t, _("%s: %d mensajes. Tocá uno para sumarlo o sacarlo (hasta 64); arriba, los que se vieron en "
                            "el bus. La lista entera, con buscador, está en la página CAN del portal."),
             CN_DBC.file, CN_DBC.nmsg);
    note(body, t, AOS_C_DIM);

    /* the ones on the bus first, then the rest, each by id */
    static uint16_t all[CN_DBC_MSG_MAX];
    for (int i = 0; i < CN_DBC.nmsg; i++) all[i] = (uint16_t)i;
    qsort(all, (size_t)CN_DBC.nmsg, sizeof all[0], msg_cmp);
    s_npick = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < CN_DBC.nmsg && s_npick < PICK_MAX; i++)
            if (on_bus(&CN_DBC.msg[all[i]]) == !pass) s_pick[s_npick++] = all[i];

    for (int i = 0; i < s_npick; i++) {
        const cn_msg_t *m = &CN_DBC.msg[s_pick[i]];
        lv_obj_t *r = card_row(body, cn_dbc_shown(s_pick[i]), msg_toggle_cb, (void *)(intptr_t)i);
        s_pick_rows[i] = r;
        cn_frame_t f = { .id = m->id, .fl = m->ext ? CN_EXT : 0 };
        char id[12];
        cn_fmt_id(&f, id, sizeof id);
        snprintf(t, sizeof t, "%s  %s", id, m->name);
        lv_obj_t *l = aos_label(r, t, &aos_mono_22, AOS_C_TEXT);
        lv_obj_set_width(l, LV_PCT(70));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        bool seen = on_bus(m);
        snprintf(t, sizeof t, seen ? _("%u · en el bus") : "%u", (unsigned)m->nsig);
        lv_obj_t *n = aos_label(r, t, aos_font_caption, seen ? AOS_C_GREEN : AOS_C_DIM);
        lv_obj_align(n, LV_ALIGN_RIGHT_MID, 0, 0);
        aos_make_decorative(l);
        aos_make_decorative(n);
    }
    if (s_npick < CN_DBC.nmsg) {
        snprintf(t, sizeof t, _("Y %d más: se eligen desde el portal."), CN_DBC.nmsg - s_npick);
        note(body, t, AOS_C_DIM);
    }
}

static void file_cb(lv_event_t *e) { (void)e; file_sheet(); }
static void msgs_cb(lv_event_t *e) { (void)e; msg_sheet(); }

/* ---- the tab ---- */

void cn_sig_build(lv_obj_t *parent)
{
    memset(&G, 0, sizeof G);
    view_compute();
    int32_t w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);
    int32_t gw = U.land ? w * 3 / 5 : w, gh = U.land ? h : h * 2 / 5;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, gw, gh);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* the value takes the right half at most: a value's name (VAL_) can be long */
    G.title = aos_label(card, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_size(G.title, gw / 2 - 24, lv_font_get_line_height(aos_font_small));
    lv_label_set_long_mode(G.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(G.title, 16, 12);
    G.value = aos_label(card, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_size(G.value, gw / 2 - 16, lv_font_get_line_height(aos_font_title));
    lv_label_set_long_mode(G.value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(G.value, LV_TEXT_ALIGN_RIGHT, 0);
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
    lv_obj_add_flag(G.list, LV_OBJ_FLAG_CLICKABLE);

    /* the file and the messages */
    lv_obj_t *tools = wrap_row(G.list);
    const char *base = strrchr(CN_DBC.file, '/');
    lv_obj_t *fb = cn_btn(tools, AOS_SYM_FILE_DOCUMENT_OUTLINE, base ? base + 1 : CN_DBC.file, file_cb, NULL);
    lv_obj_t *fl = lv_obj_get_child(fb, 1);
    lv_label_set_long_mode(fl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(fl, U.land ? w - gw - 120 : w / 2 - 60, 0);
    char mt[48];
    if (CN_DBC.show_bus) snprintf(mt, sizeof mt, "%s", _("En el bus"));
    else snprintf(mt, sizeof mt, _("%d elegidos"), CN_DBC.nshow);
    cn_btn(tools, AOS_SYM_MAGNIFY, mt, msgs_cb, NULL);

    uint16_t last_msg = 0xFFFF;
    for (int i = 0; i < s_nview; i++) {
        const cn_sig_t *g = s_view[i];
        /* each message's name above its signals */
        bool adhoc = i >= s_nfile_view;
        if (!adhoc && g->msg != last_msg && CN_DBC.msg) {
            last_msg = g->msg;
            const cn_msg_t *m = &CN_DBC.msg[g->msg];
            cn_frame_t f = { .id = m->id, .fl = m->ext ? CN_EXT : 0 };
            char id[12], t[64];
            cn_fmt_id(&f, id, sizeof id);
            snprintf(t, sizeof t, "%s · %s", id, m->name);
            lv_obj_t *hl = aos_label(G.list, t, aos_font_caption, AOS_C_DIM);
            lv_obj_set_style_pad_top(hl, 6, 0);
        } else if (adhoc && i == s_nfile_view) {
            lv_obj_t *hl = aos_label(G.list, _("De Por id"), aos_font_caption, AOS_C_DIM);
            lv_obj_set_style_pad_top(hl, 6, 0);
        }
        lv_obj_t *r = lv_obj_create(G.list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), 64);
        lv_obj_set_style_radius(r, 16, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        G.rows[i] = r;
        row_style(i);
        lv_obj_t *n = aos_label(r, g->name, aos_font_small, AOS_C_TEXT);
        lv_obj_set_size(n, LV_PCT(52), lv_font_get_line_height(aos_font_small));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        G.vals[i] = aos_label(r, "-", &aos_mono_22, AOS_C_TEAL);
        lv_obj_set_size(G.vals[i], LV_PCT(46), lv_font_get_line_height(&aos_mono_22));
        lv_label_set_long_mode(G.vals[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(G.vals[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(G.vals[i], LV_ALIGN_RIGHT_MID, 0, 0);
        aos_make_decorative(n);
        aos_make_decorative(G.vals[i]);
    }

    char info[220];
    int n = 0;
    if (s_err[0] && !CN_DBC.nsig) n = snprintf(info, sizeof info, "%s", s_err);
    else {
        n = snprintf(info, sizeof info, _("%s: %d mensajes, %d señales."), CN_DBC.file, CN_DBC.nmsg, CN_DBC.nsig);
        if (CN_DBC.skipped)
            n += snprintf(info + n, sizeof info - (size_t)n, _(" %d de más de 32 bits, salteadas."), CN_DBC.skipped);
        if (CN_DBC.show_bus && !s_nfile_view && CN_DBC.nsig)
            n += snprintf(info + n, sizeof info - (size_t)n, "%s",
                          _(" Se muestran los que pasan por el bus: todavía no pasó ninguno."));
        if (s_more)
            snprintf(info + n, sizeof info - (size_t)n, _(" Y %d señales más: elegí menos mensajes."), s_more);
    }
    G.info = aos_label(G.list, info, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(G.info, LV_PCT(100));
    lv_label_set_long_mode(G.info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *rb = cn_btn(G.list, AOS_SYM_RESTART, _("Leer de nuevo"), reload_cb, NULL);
    lv_obj_set_width(rb, LV_SIZE_CONTENT);

    title_show();
    for (int i = 0; i < s_nview; i++)
        if (s_view[i] == s_cur && G.rows[i]) lv_obj_scroll_to_view(G.rows[i], LV_ANIM_OFF);
    cn_sig_refresh();
}
