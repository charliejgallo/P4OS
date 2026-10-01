/*
 * P4OS - Monitor: the Activity Monitor of a microcontroller.
 *
 * Four live tiles - a row on top in portrait, a column on the left in
 * landscape, like the Banco - that are at once the tabs and a dashboard:
 *
 *   CPU      the load of each core, live (two minutes, one sample a second,
 *            kept by the app) and over the last hour (the HAL's minute
 *            history, aos_stats.c), the chip's temperature and clock
 *   Tareas   the FreeRTOS tasks: core, priority, state, the least stack
 *            they ever had free and their CPU since the last refresh,
 *            sortable; a tap on one says the rest
 *   Memoria  internal RAM and PSRAM with their largest blocks and history,
 *            heap_caps by capability, and what LVGL holds
 *   Sistema  the Wi-Fi link, the microSD, the firmware and the chip
 *
 * Everything comes from the HAL (aos_hal_sys_stats, aos_hal_tasks,
 * aos_hal_sys_info, aos_hal_sd_info) and is refreshed once a second. The
 * same data is at GET /api/sysmon (aos_portal_sysmon.c). It also brings a
 * home-screen widget, "widget sysmon 2x2" in menu.txt.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define C_SLATE     lv_color_hex(0x94A3B8)
#define C_SEL_BG    lv_color_hex(0x1E293B)
#define C_CORE0     AOS_C_ACCENT
#define C_CORE1     AOS_C_ORANGE
#define C_INT       AOS_C_GREEN
#define C_PSRAM     AOS_C_PURPLE
#define C_TEMP      AOS_C_PINK

enum { SEC_CPU, SEC_TASKS, SEC_MEM, SEC_SYS, SEC_COUNT };
static const char *const SEC_NAME[SEC_COUNT] = { N_("CPU"), N_("Tareas"), N_("Memoria"), N_("Sistema") };
static const char *const SEC_GLYPH[SEC_COUNT] = { AOS_SYM_CPU_64_BIT, AOS_SYM_PULSE, AOS_SYM_MEMORY, AOS_SYM_INFORMATION_OUTLINE };

enum { SORT_CPU, SORT_STACK, SORT_NAME, SORT_COUNT };
static const char *const SORT_NAME_T[SORT_COUNT] = { N_("CPU"), N_("Pila"), N_("Nombre") };

#define LIVE_N  120                 /* two minutes, one a second */

/* Kept across rotations and section changes. */
static struct {
    bool loaded;
    int sec, sort;
    int n;                          /* samples in the live ring */
    uint32_t last_ms;
} S;

/* The live ring: allocated once (lv_malloc, so PSRAM on the board) and kept,
 * not in .bss - the P4's internal RAM is what everyone is short of. */
static struct live_s {
    int16_t cpu[2][LIVE_N];         /* percent, -1 none */
    int32_t int_kb[LIVE_N], ps_kb[LIVE_N];
} *L;

typedef struct {
    lv_obj_t *row, *name, *core, *core_l, *prio, *dot, *state, *stack, *stack_bar, *cpu, *cpu_bar;
} trow_t;

/* the facts of Sistema, in the order they are laid out */
enum {
    K_SIGNAL, K_IP, K_GW, K_MASK, K_DNS, K_MAC, K_CHANNEL, K_BSSID, K_NET_UP, K_DROPS, K_TRAFFIC,
    K_SD_KIND, K_SD_SIZE, K_SD_FREE, K_SD_BUS, K_SD_MAKER,
    K_FW, K_IDF, K_SLOT, K_FLASH, K_CHIP, K_UPTIME, K_RESET,
    K_COUNT
};

static struct {
    lv_obj_t *root, *content, *tile[SEC_COUNT], *tile_val[SEC_COUNT], *tile_sub[SEC_COUNT];
    lv_timer_t *timer;
    int32_t W, H, cw, ch;
    bool land;
    aos_sys_stats_t ss;
    aos_sys_info_t si;
    bool have_ss, have_si;
    aos_task_info_t *tasks;         /* AOS_TASKS_MAX, lv_malloc'd */
    int ntasks;
    int order[AOS_TASKS_MAX];       /* tasks[] sorted */
    /* CPU */
    lv_obj_t *c_title, *c_sub, *c_val[2], *c_bar[2], *c_hsub[2], *c_live, *c_hour, *c_hour_empty, *c_fact[3], *c_fsub[3];
    lv_chart_series_t *c_live_s[2], *c_hour_s[2];
    /* Tareas */
    lv_obj_t *t_title, *t_sub, *t_list, *t_sort[SORT_COUNT];
    trow_t *trow;                   /* AOS_TASKS_MAX, lv_malloc'd */
    int t_rows;
    int32_t t_col[6], t_colw[6];
    /* Memoria */
    lv_obj_t *m_val[2], *m_sub[2], *m_bar[2], *m_live, *m_hour, *m_hour_empty, *m_leg[2], *m_cap[AOS_MEM_COUNT][4], *m_lv[4];
    lv_chart_series_t *m_live_s[2], *m_hour_s[2];
    /* Sistema */
    lv_obj_t *s_wifi_glyph, *s_ssid, *s_net_chip, *s_net_chip_l, *s_sd_name, *s_sd_bar, *s_kv[K_COUNT], *s_kv_row[K_COUNT];
} U;

static void build_section(void);

/* -------------------------------------------------------------------------- */
/* Small pieces (the Banco's)                                                  */
/* -------------------------------------------------------------------------- */

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

static lv_obj_t *column(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

static lv_obj_t *row_flex(lv_obj_t *parent, int32_t w, int32_t gap)
{
    lv_obj_t *r = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    return r;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 24, 0);
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    lv_obj_center(aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT));
    return c;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 6, 0);
    return t;
}

static lv_obj_t *bar(lv_obj_t *parent, int32_t w, int32_t h, lv_color_t c)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, c, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
    lv_bar_set_range(b, 0, 1000);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/* A label whose text changes only when it has to: a second's refresh
 * of forty rows should not invalidate what did not move. */
static void set_text(lv_obj_t *l, const char *s)
{
    if (l && strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static lv_obj_t *mk_chart(lv_obj_t *parent, int32_t w, int32_t h, int points)
{
    lv_obj_t *c = lv_chart_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_line_color(c, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, (uint32_t)points);
    lv_chart_set_div_line_count(c, 4, 0);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(c, 4, LV_PART_ITEMS);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static lv_obj_t *chart_empty(lv_obj_t *chart, const char *text)
{
    lv_obj_t *l = aos_label(chart, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, lv_pct(90));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return l;
}

/* a row of coloured names over a chart */
static lv_obj_t *legend(lv_obj_t *parent, int32_t w, const char *title)
{
    lv_obj_t *r = row_flex(parent, w, 20);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 4, 0);
    lv_obj_set_style_pad_left(r, 10, 0);
    lv_obj_set_style_pad_top(r, 6, 0);
    aos_label(r, title, aos_font_caption, AOS_C_DIM);
    return r;
}

/* ---- numbers ---- */

static void comma(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

static void fmt_bytes(char *out, size_t n, uint64_t v)
{
    static const char *const U_[] = { "KB", "MB", "GB", "TB" };
    if (v < 1024) { snprintf(out, n, "%u B", (unsigned)v); return; }
    double x = (double)v / 1024;
    int u = 0;
    while (x >= 1024 && u < 3) { x /= 1024; u++; }
    if (x >= 100 || (u == 0 && x >= 10)) snprintf(out, n, "%.0f %s", x, U_[u]);
    else snprintf(out, n, "%.1f %s", x, U_[u]);
    comma(out);
}

static void fmt_dur(char *out, size_t n, uint32_t s)
{
    if (s < 60) snprintf(out, n, "%u s", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%u min", (unsigned)(s / 60));
    else if (s < 86400) snprintf(out, n, "%u h %u min", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
    else snprintf(out, n, "%u d %u h", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24));
}

static void fmt_pct(char *out, size_t n, int x10)
{
    if (x10 < 0) { snprintf(out, n, "--"); return; }
    if (x10 >= 100) snprintf(out, n, "%d %%", (x10 + 5) / 10);
    else { snprintf(out, n, "%d.%d %%", x10 / 10, x10 % 10); comma(out); }
}

static void fmt_temp(char *out, size_t n, float c)
{
    if (isnan(c)) { snprintf(out, n, "--"); return; }
    snprintf(out, n, "%.1f °C", (double)c);
    comma(out);
}

/* avg and max of an hour's samples; false when there is none */
static bool hist_stats(const int16_t *v, int n, int *avg, int *mx, int *mn)
{
    long sum = 0;
    int k = 0, hi = INT16_MIN, lo = INT16_MAX;
    for (int i = 0; i < n; i++) {
        if (v[i] == AOS_HIST_NONE) continue;
        sum += v[i];
        k++;
        if (v[i] > hi) hi = v[i];
        if (v[i] < lo) lo = v[i];
    }
    if (!k) return false;
    if (avg) *avg = (int)(sum / k);
    if (mx) *mx = hi;
    if (mn) *mn = lo;
    return true;
}

static void chart_fill_hist(lv_obj_t *chart, lv_chart_series_t *s, const int16_t *v, int n, int32_t scale_div)
{
    int32_t *y = lv_chart_get_series_y_array(chart, s);
    for (int i = 0; i < n; i++) y[i] = v[i] == AOS_HIST_NONE ? LV_CHART_POINT_NONE : v[i] / scale_div;
}

static const char *reset_text(const char *k)
{
    if (!k) return "--";
    if (!strcmp(k, "power-on"))   return _("al encender");
    if (!strcmp(k, "software"))   return _("por software");
    if (!strcmp(k, "panic"))      return _("por un error (pánico)");
    if (!strcmp(k, "int-wdt"))    return _("watchdog de interrupciones");
    if (!strcmp(k, "task-wdt"))   return _("watchdog de tareas");
    if (!strcmp(k, "wdt"))        return _("watchdog");
    if (!strcmp(k, "brownout"))   return _("caída de tensión");
    if (!strcmp(k, "deep-sleep")) return _("al salir del sueño profundo");
    if (!strcmp(k, "external"))   return _("botón de reset");
    if (!strcmp(k, "usb"))        return _("por el USB");
    if (!strcmp(k, "jtag"))       return _("por JTAG");
    if (!strcmp(k, "efuse"))      return _("error de eFuse");
    if (!strcmp(k, "pwr-glitch")) return _("pico en la alimentación");
    if (!strcmp(k, "cpu-lockup")) return _("CPU trabada");
    return _("desconocido");
}

/* -------------------------------------------------------------------------- */
/* Sampling                                                                    */
/* -------------------------------------------------------------------------- */

static void sample(void)
{
    U.have_ss = aos_hal_sys_stats(&U.ss);
    U.have_si = aos_hal_sys_info(&U.si);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!L || (S.last_ms && now - S.last_ms < 900)) return;     /* a rotation rebuilt the app */
    S.last_ms = now;
    if (S.n == LIVE_N) {
        for (int c = 0; c < 2; c++) memmove(L->cpu[c], L->cpu[c] + 1, sizeof L->cpu[c] - sizeof L->cpu[c][0]);
        memmove(L->int_kb, L->int_kb + 1, sizeof L->int_kb - sizeof L->int_kb[0]);
        memmove(L->ps_kb, L->ps_kb + 1, sizeof L->ps_kb - sizeof L->ps_kb[0]);
        S.n--;
    }
    for (int c = 0; c < 2; c++) L->cpu[c][S.n] = U.have_ss ? (int16_t)U.ss.cpu_load[c] : -1;
    L->int_kb[S.n] = U.have_ss ? (int32_t)(U.ss.int_free / 1024) : -1;
    L->ps_kb[S.n] = U.have_ss ? (int32_t)(U.ss.psram_free / 1024) : -1;
    S.n++;
}

static int cmp_order(const void *a, const void *b)
{
    const aos_task_info_t *x = &U.tasks[*(const int *)a], *y = &U.tasks[*(const int *)b];
    if (S.sort == SORT_CPU && x->cpu_x10 != y->cpu_x10) return y->cpu_x10 - x->cpu_x10;
    if (S.sort == SORT_STACK && x->stack_min_free != y->stack_min_free) {
        if (x->stack_min_free < 0) return 1;            /* unknown last */
        if (y->stack_min_free < 0) return -1;
        return x->stack_min_free < y->stack_min_free ? -1 : 1;
    }
    return strcasecmp(x->name, y->name);
}

static void tasks_read(void)
{
    if (!U.tasks) return;
    int n = aos_hal_tasks(U.tasks, AOS_TASKS_MAX);
    U.ntasks = n > AOS_TASKS_MAX ? AOS_TASKS_MAX : n;
    for (int i = 0; i < U.ntasks; i++) U.order[i] = i;
    qsort(U.order, (size_t)U.ntasks, sizeof U.order[0], cmp_order);
}

static bool is_idle(const aos_task_info_t *t)
{
    return !strncmp(t->name, "IDLE", 4);
}

/* -------------------------------------------------------------------------- */
/* The tiles                                                                   */
/* -------------------------------------------------------------------------- */

static void tiles_refresh(void)
{
    char v[48], s[64], a[24], b[24];
    /* CPU: both cores' mean, each below */
    if (U.have_ss && U.ss.cpu_load[0] >= 0) {
        snprintf(v, sizeof v, "%d %%", (U.ss.cpu_load[0] + U.ss.cpu_load[1] + 1) / 2);
        snprintf(s, sizeof s, "%d %% · %d %%", U.ss.cpu_load[0], U.ss.cpu_load[1]);
    } else {
        snprintf(v, sizeof v, "--");
        snprintf(s, sizeof s, "%s", _("midiendo…"));
    }
    set_text(U.tile_val[SEC_CPU], v);
    set_text(U.tile_sub[SEC_CPU], s);
    /* Tareas: how many, and the busiest that is not an idle task */
    snprintf(v, sizeof v, "%d", U.ntasks);
    s[0] = 0;
    for (int i = 0, best = -1; i < U.ntasks; i++) {
        const aos_task_info_t *t = &U.tasks[i];
        if (is_idle(t) || t->cpu_x10 < 0) continue;
        if (best < 0 || t->cpu_x10 > U.tasks[best].cpu_x10) {
            best = i;
            fmt_pct(a, sizeof a, t->cpu_x10);
            snprintf(s, sizeof s, "%s %s", t->name, a);
        }
    }
    set_text(U.tile_val[SEC_TASKS], v);
    set_text(U.tile_sub[SEC_TASKS], s[0] ? s : _("tareas"));
    /* Memoria: internal free, PSRAM below */
    if (U.have_ss) {
        fmt_bytes(v, sizeof v, U.ss.int_free);
        fmt_bytes(b, sizeof b, U.ss.psram_free);
        snprintf(s, sizeof s, "PSRAM %s", b);
    } else {
        snprintf(v, sizeof v, "--");
        s[0] = 0;
    }
    set_text(U.tile_val[SEC_MEM], v);
    set_text(U.tile_sub[SEC_MEM], s);
    /* Sistema: the uptime, the signal below */
    fmt_dur(v, sizeof v, (uint32_t)(aos_hal_uptime_ms() / 1000));
    if (aos_hal_net_state() == AOS_NET_CONNECTED) snprintf(s, sizeof s, "WiFi %d dBm", aos_hal_net_rssi());
    else snprintf(s, sizeof s, "%s", _("sin WiFi"));
    set_text(U.tile_val[SEC_SYS], v);
    set_text(U.tile_sub[SEC_SYS], s);
}

static void tile_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == S.sec) return;
    S.sec = t;
    aos_hal_pref_set_i32("sysmon_tab", t);
    build_section();
}

static void build_tiles(lv_obj_t *strip, int32_t tw, int32_t th)
{
    for (int i = 0; i < SEC_COUNT; i++) {
        lv_obj_t *t = card(strip, tw, th);
        lv_obj_set_style_radius(t, 22, 0);
        lv_obj_set_style_pad_all(t, 14, 0);
        lv_obj_set_style_border_color(t, C_SLATE, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(t, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *head = box(t, lv_pct(100), 32);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, 8, 0);
        aos_make_decorative(aos_label(head, SEC_GLYPH[i], &aos_sym_28, AOS_C_DIM));
        lv_obj_t *n = aos_label(head, aos_tr(SEC_NAME[i]), aos_font_tiny, AOS_C_DIM);
        lv_obj_set_height(n, lv_font_get_line_height(aos_font_tiny));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(n, 1);
        const lv_font_t *vf = U.land ? aos_font_body : aos_font_small;
        U.tile_val[i] = aos_label(t, "", vf, AOS_C_TEXT);
        lv_obj_set_size(U.tile_val[i], lv_pct(100), lv_font_get_line_height(vf));
        lv_label_set_long_mode(U.tile_val[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(U.tile_val[i], LV_ALIGN_LEFT_MID, 0, 6);
        U.tile_sub[i] = aos_label(t, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_set_size(U.tile_sub[i], lv_pct(100), lv_font_get_line_height(aos_font_tiny));
        lv_label_set_long_mode(U.tile_sub[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(U.tile_sub[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        U.tile[i] = t;
    }
}

/* two columns in landscape, one in portrait */
static void columns(lv_obj_t **left, lv_obj_t **right, int32_t *lw, int32_t *rw, int num, int den)
{
    int32_t w = U.cw;
    if (U.land) {
        *lw = (w - AOS_UI_PAD) * num / den;
        *rw = w - *lw - AOS_UI_PAD;
        *left = column(U.content, *lw, U.ch);
        *right = column(U.content, *rw, U.ch);
        lv_obj_set_x(*right, *lw + AOS_UI_PAD);
    } else {
        *lw = *rw = w;
        *left = *right = column(U.content, w, U.ch);
    }
}

static void note(lv_obj_t *col, int32_t w)
{
    if (U.have_si && U.si.note) {
        lv_obj_t *c = caption(col, aos_tr(U.si.note), w);
        lv_obj_set_style_text_color(c, lv_color_hex(0x6E6E73), 0);
    }
}

/* -------------------------------------------------------------------------- */
/* CPU                                                                         */
/* -------------------------------------------------------------------------- */

static void cpu_refresh(void)
{
    if (!U.c_live) return;
    char t[64], a[24], b[24];
    int16_t h[2][AOS_MIN_HIST_LEN];
    for (int c = 0; c < 2; c++) aos_hal_minute_history(AOS_HIST_CPU0 + c, h[c], AOS_MIN_HIST_LEN);
    bool any_hour = false;
    for (int c = 0; c < 2; c++) {
        int l = U.have_ss ? U.ss.cpu_load[c] : -1;
        if (l >= 0) snprintf(t, sizeof t, "%d %%", l);
        else snprintf(t, sizeof t, "--");
        set_text(U.c_val[c], t);
        lv_bar_set_value(U.c_bar[c], l > 0 ? l * 10 : 0, LV_ANIM_OFF);
        int avg, mx;
        if (hist_stats(h[c], AOS_MIN_HIST_LEN, &avg, &mx, NULL)) {
            any_hour = true;
            snprintf(t, sizeof t, _("hora: prom. %d %% · máx. %d %%"), avg, mx);
        } else {
            snprintf(t, sizeof t, "%s", _("hora: sin muestras todavía"));
        }
        set_text(U.c_hsub[c], t);
    }
    /* the live chart */
    for (int c = 0; c < 2; c++) {
        int32_t *y = lv_chart_get_series_y_array(U.c_live, U.c_live_s[c]);
        int off = LIVE_N - S.n;
        for (int i = 0; i < LIVE_N; i++) y[i] = i < off || L->cpu[c][i - off] < 0 ? LV_CHART_POINT_NONE : L->cpu[c][i - off];
    }
    lv_chart_refresh(U.c_live);
    /* the hour */
    for (int c = 0; c < 2; c++) chart_fill_hist(U.c_hour, U.c_hour_s[c], h[c], AOS_MIN_HIST_LEN, 1);
    lv_chart_refresh(U.c_hour);
    if (any_hour) lv_obj_add_flag(U.c_hour_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(U.c_hour_empty, LV_OBJ_FLAG_HIDDEN);
    /* facts */
    fmt_temp(t, sizeof t, U.have_ss ? U.ss.chip_c : NAN);
    set_text(U.c_fact[0], t);
    int16_t th[AOS_MIN_HIST_LEN];
    aos_hal_minute_history(AOS_HIST_CHIP_T, th, AOS_MIN_HIST_LEN);
    int mn, mx;
    if (hist_stats(th, AOS_MIN_HIST_LEN, NULL, &mx, &mn)) {
        snprintf(a, sizeof a, "%.1f", mn / 10.0);
        comma(a);
        fmt_temp(b, sizeof b, mx / 10.0f);
        snprintf(t, sizeof t, _("hora: %s a %s"), a, b);
    } else {
        snprintf(t, sizeof t, "%s", _("sensor del chip"));
    }
    set_text(U.c_fsub[0], t);
    if (U.have_si) snprintf(t, sizeof t, "%d MHz", U.si.cpu_mhz);
    else snprintf(t, sizeof t, "--");
    set_text(U.c_fact[1], t);
    fmt_dur(t, sizeof t, (uint32_t)(aos_hal_uptime_ms() / 1000));
    set_text(U.c_fact[2], t);
}

static lv_obj_t *fact(lv_obj_t *parent, int32_t w, const char *title, lv_obj_t **val, lv_obj_t **sub)
{
    lv_obj_t *c = card(parent, w, 124);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_align(aos_label(c, title, aos_font_tiny, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
    *val = aos_label(c, "--", w < 220 ? aos_font_body : aos_font_title, AOS_C_TEXT);
    lv_obj_align(*val, LV_ALIGN_LEFT_MID, 0, 4);
    *sub = aos_label(c, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_set_size(*sub, w - 32, lv_font_get_line_height(aos_font_tiny));
    lv_label_set_long_mode(*sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(*sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return c;
}

static void build_cpu(void)
{
    lv_obj_t *left, *right;
    int32_t lw, rw;
    columns(&left, &right, &lw, &rw, 11, 20);

    lv_obj_t *head = box(left, lw, 72);
    char t[64];
    if (U.have_si) snprintf(t, sizeof t, "%s v%d.%d", U.si.chip, U.si.chip_rev / 100, U.si.chip_rev % 100);
    else snprintf(t, sizeof t, "CPU");
    U.c_title = aos_label(head, t, aos_font_title, AOS_C_TEXT);
    lv_obj_align(U.c_title, LV_ALIGN_TOP_LEFT, 4, 0);
    if (U.have_si) snprintf(t, sizeof t, _("%d núcleos RISC-V a %d MHz"), U.si.cores, U.si.cpu_mhz);
    else t[0] = 0;
    U.c_sub = aos_label(head, t, aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.c_sub, LV_ALIGN_BOTTOM_LEFT, 6, 0);

    /* one card per core */
    lv_obj_t *cores = row_flex(left, lw, 12);
    int32_t cwid = (lw - 12) / 2;
    for (int c = 0; c < 2; c++) {
        lv_obj_t *k = card(cores, cwid, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(k, 20, 0);
        lv_obj_set_flex_flow(k, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(k, 10, 0);
        lv_obj_t *hr = row_flex(k, lv_pct(100), 10);
        aos_label(hr, AOS_SYM_CPU_64_BIT, &aos_sym_28, c ? C_CORE1 : C_CORE0);
        char n[24];
        snprintf(n, sizeof n, _("Núcleo %d"), c);
        aos_label(hr, n, aos_font_small, AOS_C_TEXT);
        U.c_val[c] = aos_label(k, "--", aos_font_huge, AOS_C_TEXT);
        U.c_bar[c] = bar(k, lv_pct(100), 12, c ? C_CORE1 : C_CORE0);
        U.c_hsub[c] = aos_label(k, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(U.c_hsub[c], lv_pct(100));
        lv_label_set_long_mode(U.c_hsub[c], LV_LABEL_LONG_MODE_WRAP);
    }

    /* temperature, clock, uptime */
    lv_obj_t *facts = row_flex(left, lw, 12);
    int32_t fw = (lw - 24) / 3;
    fact(facts, fw, _("Temperatura"), &U.c_fact[0], &U.c_fsub[0]);
    fact(facts, fw, _("Frecuencia"), &U.c_fact[1], &U.c_fsub[1]);
    fact(facts, fw, _("Encendida hace"), &U.c_fact[2], &U.c_fsub[2]);
    lv_obj_set_style_text_color(U.c_fact[0], C_TEMP, 0);
    set_text(U.c_fsub[1], _("fija, sin DFS"));
    set_text(U.c_fsub[2], U.have_si ? reset_text(U.si.reset_reason) : "");

    /* the charts */
    lv_obj_t *lg = legend(right, rw, _("ÚLTIMOS 2 MINUTOS"));
    aos_label(lg, _("Núcleo 0"), aos_font_caption, C_CORE0);
    aos_label(lg, _("Núcleo 1"), aos_font_caption, C_CORE1);
    int32_t chh = U.land ? 200 : 220;
    U.c_live = mk_chart(right, rw, chh, LIVE_N);
    lv_chart_set_axis_range(U.c_live, LV_CHART_AXIS_PRIMARY_Y, -3, 100);    /* a 0 % line stays visible */
    U.c_live_s[0] = lv_chart_add_series(U.c_live, C_CORE0, LV_CHART_AXIS_PRIMARY_Y);
    U.c_live_s[1] = lv_chart_add_series(U.c_live, C_CORE1, LV_CHART_AXIS_PRIMARY_Y);
    legend(right, rw, _("ÚLTIMA HORA, PROMEDIO POR MINUTO"));
    U.c_hour = mk_chart(right, rw, chh, AOS_MIN_HIST_LEN);
    lv_chart_set_axis_range(U.c_hour, LV_CHART_AXIS_PRIMARY_Y, -3, 100);
    U.c_hour_s[0] = lv_chart_add_series(U.c_hour, C_CORE0, LV_CHART_AXIS_PRIMARY_Y);
    U.c_hour_s[1] = lv_chart_add_series(U.c_hour, C_CORE1, LV_CHART_AXIS_PRIMARY_Y);
    U.c_hour_empty = chart_empty(U.c_hour, _("La primera muestra llega al minuto de encender."));
    caption(right, _("La carga de cada núcleo es el tiempo que no pasó en su tarea IDLE. LVGL y la pantalla corren en el núcleo 1; la red, el audio y los servicios, casi todos en el 0."), rw);
    note(right, rw);
    cpu_refresh();
}

/* -------------------------------------------------------------------------- */
/* Tareas                                                                      */
/* -------------------------------------------------------------------------- */

static const char *state_name(uint8_t s)
{
    switch (s) {
    case AOS_TASK_RUNNING:   return _("activa");
    case AOS_TASK_READY:     return _("lista");
    case AOS_TASK_BLOCKED:   return _("en espera");
    case AOS_TASK_SUSPENDED: return _("suspendida");
    case AOS_TASK_DELETED:   return _("borrada");
    default:                 return "?";
    }
}

static lv_color_t state_color(uint8_t s)
{
    switch (s) {
    case AOS_TASK_RUNNING:   return AOS_C_GREEN;
    case AOS_TASK_READY:     return AOS_C_TEAL;
    case AOS_TASK_SUSPENDED: return AOS_C_ORANGE;
    case AOS_TASK_DELETED:   return AOS_C_RED;
    default:                 return lv_color_hex(0x48484A);
    }
}

static lv_color_t stack_color(int32_t free_b)
{
    if (free_b < 0) return AOS_C_DIM;
    if (free_b < 512) return AOS_C_RED;
    if (free_b < 1024) return AOS_C_ORANGE;
    return AOS_C_TEXT;
}

static void task_tap_cb(lv_event_t *e)
{
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    if (r >= U.ntasks) return;
    const aos_task_info_t *t = &U.tasks[U.order[r]];
    char msg[200], size[24], run[24];
    if (t->stack_size > 0) fmt_bytes(size, sizeof size, (uint64_t)t->stack_size);
    else snprintf(size, sizeof size, "?");
    fmt_dur(run, sizeof run, (uint32_t)(t->run_us / 1000000));
    if (t->run_us < 1000000) snprintf(run, sizeof run, "%u ms", (unsigned)(t->run_us / 1000));
    snprintf(msg, sizeof msg, _("%s (n.º %u): pila de %s en %s, prioridad base %u, %s de CPU desde el arranque"),
             t->name, (unsigned)t->id, size, t->stack_psram ? "PSRAM" : _("RAM interna"), t->base_prio, run);
    aos_ui_toast(msg, 3500);
}

static void tasks_layout(int32_t inner)
{
    /* name, core, priority, state, stack, CPU */
    static const int PORT[6] = { 206, 64, 56, 130, 100, 80 };
    static const int LAND[6] = { 290, 80, 80, 170, 180, 146 };
    const int *w = U.land ? LAND : PORT;
    int sum = 0;
    for (int i = 0; i < 6; i++) sum += w[i];
    int32_t x = 0;
    for (int i = 0; i < 6; i++) {
        U.t_colw[i] = w[i] * inner / sum;
        U.t_col[i] = x;
        x += U.t_colw[i];
    }
}

static trow_t *task_row(int r, int32_t w)
{
    trow_t *t = &U.trow[r];
    if (t->row) return t;
    t->row = box(U.t_list, w, 72);
    lv_obj_set_style_pad_hor(t->row, 18, 0);
    lv_obj_add_flag(t->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(t->row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(t->row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(t->row, task_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    if (r) {
        lv_obj_set_style_border_side(t->row, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(t->row, 1, 0);
        lv_obj_set_style_border_color(t->row, AOS_C_CARD2, 0);
    }
    t->name = aos_label(t->row, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_size(t->name, U.t_colw[0] - 10, lv_font_get_line_height(aos_font_small));
    lv_label_set_long_mode(t->name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t->name, LV_ALIGN_LEFT_MID, U.t_col[0], 0);
    t->core = box(t->row, 52, 34);
    lv_obj_set_style_radius(t->core, 10, 0);
    lv_obj_set_style_bg_opa(t->core, LV_OPA_COVER, 0);
    lv_obj_align(t->core, LV_ALIGN_LEFT_MID, U.t_col[1], 0);
    t->core_l = aos_label(t->core, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_center(t->core_l);
    t->prio = aos_label(t->row, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(t->prio, LV_ALIGN_LEFT_MID, U.t_col[2] + 8, 0);
    t->dot = box(t->row, 12, 12);
    lv_obj_set_style_radius(t->dot, 6, 0);
    lv_obj_set_style_bg_opa(t->dot, LV_OPA_COVER, 0);
    lv_obj_align(t->dot, LV_ALIGN_LEFT_MID, U.t_col[3], 0);
    t->state = aos_label(t->row, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_align(t->state, LV_ALIGN_LEFT_MID, U.t_col[3] + 20, 0);
    t->stack = aos_label(t->row, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_align(t->stack, LV_ALIGN_LEFT_MID, U.t_col[4], -8);
    t->stack_bar = bar(t->row, U.t_colw[4] - 18, 6, AOS_C_TEXT);
    lv_obj_align(t->stack_bar, LV_ALIGN_LEFT_MID, U.t_col[4], 18);
    t->cpu = aos_label_boxed(t->row, "", aos_font_caption, AOS_C_TEXT, U.t_colw[5], lv_font_get_line_height(aos_font_caption));
    lv_obj_set_style_text_align(t->cpu, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(t->cpu, LV_ALIGN_LEFT_MID, U.t_col[5], -8);
    t->cpu_bar = bar(t->row, U.t_colw[5] - 14, 6, C_CORE0);
    lv_obj_align(t->cpu_bar, LV_ALIGN_LEFT_MID, U.t_col[5] + 14, 18);
    return t;
}

static void tasks_refresh(void)
{
    if (!U.t_list || !U.trow) return;
    char t[48], a[24];
    snprintf(t, sizeof t, _("%d tareas"), U.ntasks);
    set_text(U.t_title, t);
    int run = 0, ready = 0, blk = 0, susp = 0;
    for (int i = 0; i < U.ntasks; i++) {
        uint8_t s = U.tasks[i].state;
        run += s == AOS_TASK_RUNNING;
        ready += s == AOS_TASK_READY;
        blk += s == AOS_TASK_BLOCKED;
        susp += s == AOS_TASK_SUSPENDED;
    }
    /* the states there are, and how many of each */
    const struct { int n; const char *one, *many; } ST[] = {
        { run, N_("activa"), N_("activas") }, { ready, N_("lista"), N_("listas") },
        { blk, N_("en espera"), N_("en espera") }, { susp, N_("suspendida"), N_("suspendidas") },
    };
    t[0] = 0;
    for (size_t i = 0; i < sizeof ST / sizeof ST[0]; i++) {
        if (!ST[i].n) continue;
        size_t l = strlen(t);
        snprintf(t + l, sizeof t - l, "%s%d %s", l ? " · " : "", ST[i].n, aos_tr(ST[i].n == 1 ? ST[i].one : ST[i].many));
    }
    set_text(U.t_sub, t);
    int32_t w = U.cw;               /* the list's own width is not laid out yet on the first pass */
    for (int r = 0; r < U.ntasks; r++) {
        const aos_task_info_t *k = &U.tasks[U.order[r]];
        trow_t *row = task_row(r, w);
        lv_obj_remove_flag(row->row, LV_OBJ_FLAG_HIDDEN);
        set_text(row->name, k->name);
        lv_obj_set_style_text_color(row->name, is_idle(k) ? AOS_C_DIM : AOS_C_TEXT, 0);
        if (k->core >= 0) snprintf(t, sizeof t, "%d", k->core);
        else snprintf(t, sizeof t, "0/1");
        set_text(row->core_l, t);
        lv_obj_set_style_bg_color(row->core, k->core == 0 ? lv_color_hex(0x0A2A4D) : k->core == 1 ? lv_color_hex(0x4A2E05) : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(row->core_l, k->core == 0 ? C_CORE0 : k->core == 1 ? C_CORE1 : AOS_C_DIM, 0);
        if (k->prio != k->base_prio) snprintf(t, sizeof t, "%u↑", k->prio);     /* inherited from a mutex */
        else snprintf(t, sizeof t, "%u", k->prio);
        set_text(row->prio, t);
        lv_obj_set_style_bg_color(row->dot, state_color(k->state), 0);
        set_text(row->state, state_name(k->state));
        if (k->stack_min_free >= 0) fmt_bytes(t, sizeof t, (uint64_t)k->stack_min_free);
        else snprintf(t, sizeof t, "--");
        set_text(row->stack, t);
        lv_color_t sc = stack_color(k->stack_min_free);
        lv_obj_set_style_text_color(row->stack, sc, 0);
        if (k->stack_size > 0 && k->stack_min_free >= 0) {
            lv_obj_remove_flag(row->stack_bar, LV_OBJ_FLAG_HIDDEN);
            int used = (int)(1000 - (int64_t)k->stack_min_free * 1000 / k->stack_size);
            lv_bar_set_value(row->stack_bar, used < 0 ? 0 : used, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(row->stack_bar, k->stack_min_free < 1024 ? sc : C_SLATE, LV_PART_INDICATOR);
        } else {
            lv_obj_add_flag(row->stack_bar, LV_OBJ_FLAG_HIDDEN);
        }
        fmt_pct(a, sizeof a, k->cpu_x10);
        set_text(row->cpu, a);
        lv_bar_set_value(row->cpu_bar, k->cpu_x10 > 0 ? k->cpu_x10 : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(row->cpu_bar, is_idle(k) ? lv_color_hex(0x48484A) : k->core == 1 ? C_CORE1 : C_CORE0, LV_PART_INDICATOR);
    }
    for (int r = U.ntasks; r < AOS_TASKS_MAX; r++)
        if (U.trow[r].row) lv_obj_add_flag(U.trow[r].row, LV_OBJ_FLAG_HIDDEN);
}

static void sort_cb(lv_event_t *e)
{
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    if (s == S.sort) return;
    S.sort = s;
    aos_hal_pref_set_i32("sysmon_sort", s);
    for (int i = 0; i < SORT_COUNT; i++) {
        lv_obj_set_style_bg_color(U.t_sort[i], i == s ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.t_sort[i], 0), i == s ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
    }
    tasks_read();
    tasks_refresh();
}

static void build_tasks(void)
{
    int32_t w = U.cw;
    lv_obj_t *col = column(U.content, w, U.ch);

    lv_obj_t *head = box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(head, 12, 0);
    lv_obj_t *tl = box(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(tl, 4, 0);
    U.t_title = aos_label(tl, "", aos_font_title, AOS_C_TEXT);
    U.t_sub = aos_label(tl, "", aos_font_caption, AOS_C_DIM);
    lv_obj_t *sr = row_flex(head, LV_SIZE_CONTENT, 10);
    aos_label(sr, AOS_SYM_SORT, &aos_sym_28, AOS_C_DIM);
    for (int i = 0; i < SORT_COUNT; i++) U.t_sort[i] = chip(sr, aos_tr(SORT_NAME_T[i]), i == S.sort, sort_cb, (void *)(intptr_t)i);

    lv_obj_t *c = card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(c, 6, 0);
    tasks_layout(w - 36);
    /* the column heads */
    lv_obj_t *hr = box(c, w, 44);
    lv_obj_set_style_pad_hor(hr, 18, 0);
    static const char *const HEAD[6] = { N_("TAREA"), N_("NÚCLEO"), N_("PRIO"), N_("ESTADO"), N_("PILA LIBRE"), N_("CPU") };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *l = aos_label(hr, i == 1 && !U.land ? _("NÚC.") : aos_tr(HEAD[i]), aos_font_tiny, AOS_C_DIM);
        if (i == 5) {
            lv_obj_set_width(l, U.t_colw[5]);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        }
        lv_obj_align(l, LV_ALIGN_LEFT_MID, U.t_col[i] + (i == 2 ? 8 : 0), 4);
    }
    U.t_list = box(c, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.t_list, LV_FLEX_FLOW_COLUMN);
    if (U.trow) memset(U.trow, 0, sizeof *U.trow * AOS_TASKS_MAX);
    caption(col, _("Pila libre: lo mínimo que tuvo libre cada tarea desde que arrancó (la marca de agua); en rojo, menos de 512 bytes. CPU: el porcentaje de un núcleo desde el refresco anterior. Una flecha junto a la prioridad es una heredada de un mutex. Tocá una tarea para ver el resto."), w);
    note(col, w);
    tasks_refresh();
}

/* -------------------------------------------------------------------------- */
/* Memoria                                                                     */
/* -------------------------------------------------------------------------- */

static int count_objs(lv_obj_t *o)
{
    int n = 1;
    uint32_t c = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < c; i++) n += count_objs(lv_obj_get_child(o, (int32_t)i));
    return n;
}

static void mem_refresh(void)
{
    if (!U.m_live) return;
    char t[80], a[24], b[24], c[24];
    for (int k = 0; k < 2; k++) {
        uint32_t fr = k ? U.ss.psram_free : U.ss.int_free, tot = k ? U.ss.psram_total : U.ss.int_total;
        uint32_t lg = k ? (U.have_si ? U.si.mem[AOS_MEM_SPIRAM].largest : 0) : U.ss.int_largest;
        if (!U.have_ss || !tot) {
            set_text(U.m_val[k], "--");
            continue;
        }
        fmt_bytes(t, sizeof t, fr);
        set_text(U.m_val[k], t);
        fmt_bytes(a, sizeof a, tot);
        fmt_bytes(b, sizeof b, lg);
        snprintf(t, sizeof t, _("libres de %s\nbloque mayor %s"), a, b);
        set_text(U.m_sub[k], t);
        lv_bar_set_value(U.m_bar[k], (int32_t)((uint64_t)(tot - fr) * 1000 / tot), LV_ANIM_OFF);
    }
    /* live: internal on the left axis, PSRAM on the right one, each from 0 to its total */
    int32_t *yi = lv_chart_get_series_y_array(U.m_live, U.m_live_s[0]), *yp = lv_chart_get_series_y_array(U.m_live, U.m_live_s[1]);
    int off = LIVE_N - S.n;
    for (int i = 0; i < LIVE_N; i++) {
        yi[i] = i < off || L->int_kb[i - off] < 0 ? LV_CHART_POINT_NONE : L->int_kb[i - off];
        yp[i] = i < off || L->ps_kb[i - off] < 0 ? LV_CHART_POINT_NONE : L->ps_kb[i - off];
    }
    if (U.have_ss) {
        lv_chart_set_axis_range(U.m_live, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)(U.ss.int_total / 1024));
        lv_chart_set_axis_range(U.m_live, LV_CHART_AXIS_SECONDARY_Y, 0, (int32_t)(U.ss.psram_total / 1024));
        lv_chart_set_axis_range(U.m_hour, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)(U.ss.int_total / 1024));
        lv_chart_set_axis_range(U.m_hour, LV_CHART_AXIS_SECONDARY_Y, 0, (int32_t)(U.ss.psram_total / 1024));
    }
    lv_chart_refresh(U.m_live);
    int16_t hi[AOS_MIN_HIST_LEN], hp[AOS_MIN_HIST_LEN];
    aos_hal_minute_history(AOS_HIST_INT_FREE_KB, hi, AOS_MIN_HIST_LEN);
    aos_hal_minute_history(AOS_HIST_PSRAM_FREE_KB, hp, AOS_MIN_HIST_LEN);
    chart_fill_hist(U.m_hour, U.m_hour_s[0], hi, AOS_MIN_HIST_LEN, 1);
    chart_fill_hist(U.m_hour, U.m_hour_s[1], hp, AOS_MIN_HIST_LEN, 1);
    lv_chart_refresh(U.m_hour);
    int mn;
    if (hist_stats(hi, AOS_MIN_HIST_LEN, NULL, NULL, &mn)) lv_obj_add_flag(U.m_hour_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(U.m_hour_empty, LV_OBJ_FLAG_HIDDEN);
    /* the legend carries the values */
    if (U.have_ss) {
        fmt_bytes(a, sizeof a, U.ss.int_free);
        snprintf(t, sizeof t, _("Interna %s"), a);
        set_text(U.m_leg[0], t);
        fmt_bytes(a, sizeof a, U.ss.psram_free);
        snprintf(t, sizeof t, "PSRAM %s", a);
        set_text(U.m_leg[1], t);
    }
    /* by capability */
    for (int k = 0; k < AOS_MEM_COUNT && U.have_si; k++) {
        const aos_mem_region_t *m = &U.si.mem[k];
        uint32_t v[4] = { m->free, m->total, m->largest, m->min_free };
        for (int j = 0; j < 4; j++) {
            if (m->total) fmt_bytes(t, sizeof t, v[j]);
            else snprintf(t, sizeof t, "--");
            set_text(U.m_cap[k][j], t);
        }
    }
    /* LVGL */
    lv_display_t *d = lv_display_get_default();
    int objs = 0;
    if (d) objs = count_objs(lv_display_get_screen_active(d)) + count_objs(lv_display_get_layer_top(d)) +
                  count_objs(lv_display_get_layer_sys(d));
    snprintf(t, sizeof t, "%d", objs);
    set_text(U.m_lv[0], t);
    int timers = 0;
    for (lv_timer_t *tm = lv_timer_get_next(NULL); tm; tm = lv_timer_get_next(tm)) timers++;
    snprintf(t, sizeof t, "%d", timers);
    set_text(U.m_lv[1], t);
    lv_draw_buf_t *db = d ? lv_display_get_buf_active(d) : NULL;
    if (db) {
        fmt_bytes(a, sizeof a, db->data_size);
        snprintf(t, sizeof t, lv_display_is_double_buffered(d) ? _("2 × %s") : "%s", a);
    } else {
        snprintf(t, sizeof t, "--");
    }
    set_text(U.m_lv[2], t);
    lv_mem_monitor_t mon;
    memset(&mon, 0, sizeof mon);
    lv_mem_monitor(&mon);
    if (mon.total_size) {
        fmt_bytes(a, sizeof a, mon.total_size - mon.free_size);
        fmt_bytes(b, sizeof b, mon.total_size);
        fmt_bytes(c, sizeof c, mon.free_biggest_size);
        snprintf(t, sizeof t, _("%s de %s, bloque %s"), a, b, c);
    } else {
        snprintf(t, sizeof t, "%s", _("el malloc del sistema"));
    }
    set_text(U.m_lv[3], t);
}

static lv_obj_t *kv_row(lv_obj_t *parent, const char *key, bool line, lv_obj_t **val)
{
    lv_obj_t *r = box(parent, lv_pct(100), 64);
    lv_obj_set_style_pad_hor(r, 22, 0);
    if (line) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
    }
    lv_obj_t *k = aos_label(r, key, aos_font_small, AOS_C_TEXT);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    *val = aos_label(r, "--", aos_font_small, AOS_C_DIM);
    lv_obj_set_style_text_align(*val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(*val, LV_ALIGN_RIGHT_MID, 0, 0);
    return r;
}

static lv_obj_t *region_card(lv_obj_t *parent, int32_t w, const char *title, lv_color_t col, int k)
{
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_t *hr = row_flex(c, lv_pct(100), 10);
    aos_label(hr, AOS_SYM_MEMORY, &aos_sym_28, col);
    aos_label(hr, title, aos_font_small, AOS_C_TEXT);
    U.m_val[k] = aos_label(c, "--", aos_font_large, AOS_C_TEXT);
    U.m_sub[k] = aos_label(c, "", aos_font_caption, AOS_C_DIM);
    U.m_bar[k] = bar(c, lv_pct(100), 12, col);
    return c;
}

static void build_mem(void)
{
    lv_obj_t *left, *right;
    int32_t lw, rw;
    columns(&left, &right, &lw, &rw, 11, 20);

    lv_obj_t *rr = row_flex(left, lw, 12);
    int32_t hw = (lw - 12) / 2;
    region_card(rr, hw, _("RAM interna"), C_INT, 0);
    region_card(rr, hw, "PSRAM", C_PSRAM, 1);

    section(left, _("POR CAPACIDAD (HEAP_CAPS)"));
    lv_obj_t *tc = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(tc, 8, 0);
    static const char *const CAP[AOS_MEM_COUNT] = { N_("Interna"), "DMA", "PSRAM", N_("Ejecutable") };
    static const char *const COL[4] = { N_("LIBRE"), N_("TOTAL"), N_("MAYOR"), N_("MÍNIMO") };
    int32_t inner = lw - 44, kw = inner * 25 / 100, vw = (inner - kw) / 4;
    for (int k = -1; k < AOS_MEM_COUNT; k++) {
        lv_obj_t *r = box(tc, lw, k < 0 ? 44 : 60);
        lv_obj_set_style_pad_hor(r, 22, 0);
        if (k > 0) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
        }
        if (k >= 0) lv_obj_align(aos_label(r, aos_tr(CAP[k]), aos_font_small, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
        for (int j = 0; j < 4; j++) {
            lv_obj_t *l = aos_label_boxed(r, k < 0 ? aos_tr(COL[j]) : "--", k < 0 ? aos_font_tiny : aos_font_caption,
                                          k < 0 ? AOS_C_DIM : j == 0 ? AOS_C_TEXT : AOS_C_DIM, vw,
                                          lv_font_get_line_height(k < 0 ? aos_font_tiny : aos_font_caption));
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, kw + j * vw, k < 0 ? 4 : 0);
            if (k >= 0) U.m_cap[k][j] = l;
        }
    }

    section(left, "LVGL");
    lv_obj_t *lc = card(left, lw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lc, LV_FLEX_FLOW_COLUMN);
    static const char *const LV[4] = { N_("Objetos en pantalla"), N_("Temporizadores"), N_("Buffers de dibujo"), N_("Memoria") };
    for (int i = 0; i < 4; i++) kv_row(lc, aos_tr(LV[i]), i > 0, &U.m_lv[i]);

    lv_obj_t *lg = legend(right, rw, _("LIBRE, ÚLTIMOS 2 MINUTOS"));
    U.m_leg[0] = aos_label(lg, _("Interna"), aos_font_caption, C_INT);
    U.m_leg[1] = aos_label(lg, "PSRAM", aos_font_caption, C_PSRAM);
    int32_t chh = U.land ? 200 : 220;
    U.m_live = mk_chart(right, rw, chh, LIVE_N);
    U.m_live_s[0] = lv_chart_add_series(U.m_live, C_INT, LV_CHART_AXIS_PRIMARY_Y);
    U.m_live_s[1] = lv_chart_add_series(U.m_live, C_PSRAM, LV_CHART_AXIS_SECONDARY_Y);
    legend(right, rw, _("LIBRE, ÚLTIMA HORA"));
    U.m_hour = mk_chart(right, rw, chh, AOS_MIN_HIST_LEN);
    U.m_hour_s[0] = lv_chart_add_series(U.m_hour, C_INT, LV_CHART_AXIS_PRIMARY_Y);
    U.m_hour_s[1] = lv_chart_add_series(U.m_hour, C_PSRAM, LV_CHART_AXIS_SECONDARY_Y);
    U.m_hour_empty = chart_empty(U.m_hour, _("La primera muestra llega al minuto de encender."));
    caption(right, _("Cada curva va de cero a su total. Si el bloque mayor es mucho más chico que lo libre, la memoria está fragmentada: hay lugar, pero no en un solo pedazo. El mínimo es lo menos libre que hubo desde el arranque. LVGL pide su memoria a la PSRAM."), rw);
    note(right, rw);
    mem_refresh();
}

/* -------------------------------------------------------------------------- */
/* Sistema                                                                     */
/* -------------------------------------------------------------------------- */

static const char *signal_word(int rssi)
{
    return rssi >= -55 ? _("excelente") : rssi >= -67 ? _("buena") : rssi >= -75 ? _("regular") : _("débil");
}

static const char *wifi_glyph(int rssi)
{
    return rssi >= -55 ? AOS_SYM_WIFI_STRENGTH_4 : rssi >= -67 ? AOS_SYM_WIFI_STRENGTH_3
         : rssi >= -75 ? AOS_SYM_WIFI_STRENGTH_2 : AOS_SYM_WIFI_STRENGTH_1;
}

static void kv_set(int k, const char *v)
{
    set_text(U.s_kv[k], v && v[0] ? v : "--");
}

static void sys_refresh(void)
{
    if (!U.s_ssid) return;
    char t[96], a[24], b[24];
    const aos_sys_info_t *si = &U.si;
    aos_net_state_t ns = aos_hal_net_state();
    bool up = ns == AOS_NET_CONNECTED;
    int rssi = up ? aos_hal_net_rssi() : 0;
    set_text(U.s_ssid, up ? aos_hal_net_ssid() : ns == AOS_NET_CONNECTING ? _("Conectando…") : _("Sin conexión"));
    set_text(U.s_wifi_glyph, up ? wifi_glyph(rssi) : AOS_SYM_WIFI_OFF);
    lv_obj_set_style_text_color(U.s_wifi_glyph, up ? AOS_C_ACCENT : AOS_C_DIM, 0);
    set_text(U.s_net_chip_l, up ? _("conectado") : ns == AOS_NET_CONNECTING ? _("conectando") : ns == AOS_NET_FAILED ? _("falló") : _("apagado"));
    lv_obj_set_style_bg_color(U.s_net_chip, up ? lv_color_hex(0x0F3D1E) : AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(U.s_net_chip_l, up ? AOS_C_GREEN : AOS_C_DIM, 0);
    if (up) snprintf(t, sizeof t, "%d dBm · %s", rssi, signal_word(rssi));
    else t[0] = 0;
    kv_set(K_SIGNAL, t);
    kv_set(K_IP, up ? aos_hal_net_ip() : "");
    kv_set(K_GW, U.have_si ? si->net_gw : "");
    kv_set(K_MASK, U.have_si ? si->net_mask : "");
    kv_set(K_DNS, U.have_si ? si->net_dns : "");
    kv_set(K_MAC, U.have_si ? si->net_mac : "");
    if (U.have_si && si->net_channel) snprintf(t, sizeof t, "%d (%s)", si->net_channel, si->net_channel > 14 ? "5 GHz" : "2,4 GHz");
    else t[0] = 0;
    kv_set(K_CHANNEL, t);
    kv_set(K_BSSID, U.have_si ? si->net_bssid : "");
    if (U.have_si && si->net_up_s) fmt_dur(t, sizeof t, si->net_up_s);
    else t[0] = 0;
    kv_set(K_NET_UP, t);
    snprintf(t, sizeof t, "%u", U.have_si ? (unsigned)si->net_drops : 0u);
    kv_set(K_DROPS, t);
    if (U.s_kv[K_TRAFFIC] && U.have_si && si->net_rx_bytes >= 0) {
        fmt_bytes(a, sizeof a, (uint64_t)si->net_rx_bytes);
        fmt_bytes(b, sizeof b, (uint64_t)(si->net_tx_bytes > 0 ? si->net_tx_bytes : 0));
        snprintf(t, sizeof t, "↓ %s  ↑ %s", a, b);
        kv_set(K_TRAFFIC, t);
    }

    /* the card */
    aos_sd_info_t sd;
    uint64_t tot = 0, fr = 0;
    bool card_ok = aos_hal_sd_present() && aos_hal_sd_info(&sd);
    bool use_ok = card_ok && aos_hal_sd_usage(&tot, &fr) && tot;
    if (card_ok) {
        snprintf(t, sizeof t, "%s", sd.name[0] ? sd.name : _("Tarjeta microSD"));
        set_text(U.s_sd_name, t);
        snprintf(t, sizeof t, "%s%s%s", sd.kind ? sd.kind : "", sd.fs && sd.fs[0] ? " · " : "", sd.fs ? sd.fs : "");
        kv_set(K_SD_KIND, t);
        fmt_bytes(t, sizeof t, sd.capacity);
        kv_set(K_SD_SIZE, t);
        if (use_ok) {
            fmt_bytes(a, sizeof a, fr);
            snprintf(t, sizeof t, _("%s (%d %% usado)"), a, (int)((tot - fr) * 100 / tot));
            kv_set(K_SD_FREE, t);
            lv_bar_set_value(U.s_sd_bar, (int32_t)((tot - fr) * 1000 / tot), LV_ANIM_OFF);
        } else {
            kv_set(K_SD_FREE, "");
        }
        if (sd.freq_khz) {
            snprintf(t, sizeof t, _("%d bits a %u MHz"), sd.bus_width, (unsigned)((sd.freq_khz + 500) / 1000));
            kv_set(K_SD_BUS, t);
        } else {
            kv_set(K_SD_BUS, "");
        }
        if (sd.year) snprintf(t, sizeof t, _("id %d · %s %d"), sd.manufacturer, aos_month_name(sd.month > 0 ? sd.month - 1 : 0), sd.year);
        else snprintf(t, sizeof t, "id %d", sd.manufacturer);
        kv_set(K_SD_MAKER, t);
        lv_obj_remove_flag(U.s_sd_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_text(U.s_sd_name, _("Sin tarjeta"));
        for (int k = K_SD_KIND; k <= K_SD_MAKER; k++) kv_set(k, "");
        lv_obj_add_flag(U.s_sd_bar, LV_OBJ_FLAG_HIDDEN);
    }

    /* firmware */
    kv_set(K_FW, aos_hal_firmware_version());
    kv_set(K_IDF, U.have_si ? si->idf_version : "");
    if (U.have_si && si->app_slot) {
        fmt_bytes(b, sizeof b, si->app_slot_bytes);
        if (si->app_bytes) {
            fmt_bytes(a, sizeof a, si->app_bytes);
            snprintf(t, sizeof t, _("%s · %s de %s"), si->app_slot, a, b);
        } else {
            snprintf(t, sizeof t, "%s · %s", si->app_slot, b);
        }
    } else {
        t[0] = 0;
    }
    kv_set(K_SLOT, t);
    if (U.have_si && si->flash_bytes) fmt_bytes(t, sizeof t, si->flash_bytes);
    else t[0] = 0;
    kv_set(K_FLASH, t);
    if (U.have_si) snprintf(t, sizeof t, _("%s v%d.%d, %d núcleos"), si->chip, si->chip_rev / 100, si->chip_rev % 100, si->cores);
    else t[0] = 0;
    kv_set(K_CHIP, t);
    fmt_dur(t, sizeof t, (uint32_t)(aos_hal_uptime_ms() / 1000));
    kv_set(K_UPTIME, t);
    kv_set(K_RESET, U.have_si ? reset_text(si->reset_reason) : "");
}

static lv_obj_t *kv_card(lv_obj_t *parent, int32_t w, const int *keys, const char *const *names, int n)
{
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < n; i++) U.s_kv_row[keys[i]] = kv_row(c, aos_tr(names[i]), i > 0, &U.s_kv[keys[i]]);
    return c;
}

static lv_obj_t *card_head(lv_obj_t *parent, int32_t w, const char *glyph, lv_color_t col, lv_obj_t **glyph_l, lv_obj_t **title)
{
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_t *r = row_flex(c, lv_pct(100), 14);
    lv_obj_t *g = aos_label(r, glyph, &aos_sym_44, col);
    if (glyph_l) *glyph_l = g;
    *title = aos_label(r, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_flex_grow(*title, 1);
    lv_label_set_long_mode(*title, LV_LABEL_LONG_MODE_DOTS);
    return c;
}

static void build_sys(void)
{
    lv_obj_t *left, *right;
    int32_t lw, rw;
    columns(&left, &right, &lw, &rw, 1, 2);

    section(left, "WI-FI");
    lv_obj_t *wh = card_head(left, lw, AOS_SYM_WIFI, AOS_C_ACCENT, &U.s_wifi_glyph, &U.s_ssid);
    lv_obj_t *hr = lv_obj_get_child(wh, 0);
    U.s_net_chip = box(hr, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_radius(U.s_net_chip, 20, 0);
    lv_obj_set_style_pad_hor(U.s_net_chip, 16, 0);
    lv_obj_set_style_bg_opa(U.s_net_chip, LV_OPA_COVER, 0);
    U.s_net_chip_l = aos_label(U.s_net_chip, "", aos_font_caption, AOS_C_GREEN);
    lv_obj_center(U.s_net_chip_l);
    static const int NK[] = { K_SIGNAL, K_IP, K_GW, K_MASK, K_DNS, K_MAC, K_CHANNEL, K_BSSID, K_NET_UP, K_DROPS, K_TRAFFIC };
    static const char *const NN[] = { N_("Señal"), N_("IP"), N_("Puerta de enlace"), N_("Máscara"), N_("DNS"), N_("MAC"),
                                      N_("Canal"), N_("Punto de acceso"), N_("Conectada hace"), N_("Caídas desde el arranque"),
                                      N_("Tráfico") };
    int nn = sizeof NK / sizeof NK[0];
    if (!U.have_si || U.si.net_rx_bytes < 0) nn--;                /* no byte counts: no row */
    kv_card(left, lw, NK, NN, nn);

    section(right, _("TARJETA"));
    lv_obj_t *sc = card_head(right, rw, AOS_SYM_SD, AOS_C_YELLOW, NULL, &U.s_sd_name);
    U.s_sd_bar = bar(sc, lv_pct(100), 12, AOS_C_YELLOW);
    static const int SK[] = { K_SD_KIND, K_SD_SIZE, K_SD_FREE, K_SD_BUS, K_SD_MAKER };
    static const char *const SN[] = { N_("Tipo"), N_("Capacidad"), N_("Libre"), N_("Bus"), N_("Fabricante") };
    kv_card(right, rw, SK, SN, 5);

    section(right, _("FIRMWARE"));
    static const int FK[] = { K_FW, K_IDF, K_SLOT, K_FLASH, K_CHIP, K_UPTIME, K_RESET };
    static const char *const FN[] = { N_("Versión"), "ESP-IDF", N_("Partición"), N_("Flash"), N_("Chip"),
                                      N_("Encendida hace"), N_("Último reinicio") };
    kv_card(right, rw, FK, FN, 7);
    if (!U.have_si || U.si.net_rx_bytes < 0)
        caption(right, _("El tráfico en bytes no se cuenta: lwIP lo haría con sus estadísticas, que este firmware tiene apagadas."), rw);
    note(right, rw);
    sys_refresh();
}

/* -------------------------------------------------------------------------- */
/* The app                                                                     */
/* -------------------------------------------------------------------------- */

static void build_section(void)
{
    lv_obj_clean(U.content);
    U.c_live = U.m_live = NULL;
    U.t_list = NULL;
    U.s_ssid = NULL;
    memset(U.s_kv, 0, sizeof U.s_kv);
    for (int i = 0; i < SEC_COUNT; i++) {
        lv_obj_set_style_border_width(U.tile[i], i == S.sec ? 3 : 0, 0);
        lv_obj_set_style_bg_color(U.tile[i], i == S.sec ? C_SEL_BG : AOS_C_CARD, 0);
    }
    if (S.sec == SEC_CPU) build_cpu();
    else if (S.sec == SEC_TASKS) build_tasks();
    else if (S.sec == SEC_MEM) build_mem();
    else build_sys();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    sample();
    tasks_read();
    tiles_refresh();
    if (S.sec == SEC_CPU) cpu_refresh();
    else if (S.sec == SEC_TASKS) tasks_refresh();
    else if (S.sec == SEC_MEM) mem_refresh();
    else sys_refresh();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    if (!S.loaded) {
        S.loaded = true;
        int32_t v;
        if (aos_hal_pref_get_i32("sysmon_tab", &v) && v >= 0 && v < SEC_COUNT) S.sec = v;
        if (aos_hal_pref_get_i32("sysmon_sort", &v) && v >= 0 && v < SORT_COUNT) S.sort = v;
    }
    memset(&U, 0, sizeof U);
    U.tasks = lv_malloc(sizeof *U.tasks * AOS_TASKS_MAX);
    U.trow = lv_malloc(sizeof *U.trow * AOS_TASKS_MAX);
    if (U.trow) memset(U.trow, 0, sizeof *U.trow * AOS_TASKS_MAX);
    if (!L && (L = lv_malloc(sizeof *L)) != NULL) memset(L, 0, sizeof *L);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    sample();
    tasks_read();
    const int32_t gap = 12;
    lv_obj_t *strip;
    if (U.land) {
        const int32_t sw = 250;
        strip = box(root, sw, U.H);
        lv_obj_set_style_pad_left(strip, AOS_UI_PAD, 0);
        lv_obj_set_style_pad_ver(strip, 8, 0);
        lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(strip, gap, 0);
        int32_t th = (U.H - 16 - 24 - 3 * gap) / SEC_COUNT;
        build_tiles(strip, sw - AOS_UI_PAD, th);
        U.cw = U.W - sw - 2 * AOS_UI_PAD;
        U.ch = U.H - 8;
        U.content = box(root, U.cw, U.ch);
        lv_obj_set_pos(U.content, sw + AOS_UI_PAD, 8);
    } else {
        const int32_t sh = 136;
        strip = box(root, U.W, sh);
        lv_obj_set_style_pad_hor(strip, AOS_UI_PAD, 0);
        lv_obj_set_style_pad_top(strip, 8, 0);
        lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(strip, gap, 0);
        int32_t tw = (U.W - 2 * AOS_UI_PAD - 3 * gap) / SEC_COUNT;
        build_tiles(strip, tw, sh - 8);
        U.cw = U.W - 2 * AOS_UI_PAD;
        U.ch = U.H - sh - 16;
        U.content = box(root, U.cw, U.ch);
        lv_obj_set_pos(U.content, AOS_UI_PAD, sh + 16);
    }
    tiles_refresh();
    build_section();
    U.timer = lv_timer_create(timer_cb, 1000, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.timer) lv_timer_delete(U.timer);
    lv_free(U.tasks);
    lv_free(U.trow);
    memset(&U, 0, sizeof U);
}

/* -------------------------------------------------------------------------- */
/* The home-screen widget: "widget sysmon 2x2" in menu.txt                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    lv_obj_t *val[3], *bar[3], *up;
    lv_timer_t *timer;
} widget_t;

static void w_refresh(widget_t *w)
{
    aos_sys_stats_t s;
    char t[32];
    bool ok = aos_hal_sys_stats(&s);
    int cpu = ok && s.cpu_load[0] >= 0 ? (s.cpu_load[0] + s.cpu_load[1] + 1) / 2 : -1;
    if (cpu >= 0) snprintf(t, sizeof t, "%d %%", cpu);
    else snprintf(t, sizeof t, "--");
    set_text(w->val[0], t);
    lv_bar_set_value(w->bar[0], cpu > 0 ? cpu * 10 : 0, LV_ANIM_OFF);
    for (int k = 0; k < 2; k++) {
        uint32_t fr = k ? s.psram_free : s.int_free, tot = k ? s.psram_total : s.int_total;
        if (ok && tot) {
            fmt_bytes(t, sizeof t, fr);
            lv_bar_set_value(w->bar[1 + k], (int32_t)((uint64_t)(tot - fr) * 1000 / tot), LV_ANIM_OFF);
        } else {
            snprintf(t, sizeof t, "--");
        }
        set_text(w->val[1 + k], t);
    }
    fmt_dur(t, sizeof t, (uint32_t)(aos_hal_uptime_ms() / 1000));
    set_text(w->up, t);
}

static void w_timer_cb(lv_timer_t *t) { w_refresh(lv_timer_get_user_data(t)); }
static void w_open_cb(lv_event_t *e) { (void)e; aos_ui_open("aos.sysmon"); }

static void w_deleted_cb(lv_event_t *e)
{
    widget_t *w = lv_event_get_user_data(e);
    lv_timer_delete(w->timer);
    free(w);
}

static void widget_create(lv_obj_t *card_, int32_t cw, int32_t ch, const char *arg)
{
    (void)arg;
    widget_t *w = calloc(1, sizeof *w);
    if (!w) return;
    lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card_, w_open_cb, LV_EVENT_SHORT_CLICKED, NULL);
    int32_t iw = cw - 44;
    lv_obj_t *g = aos_label(card_, AOS_SYM_MEMORY, &aos_sym_28, lv_color_white());
    lv_obj_align(g, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(aos_label(card_, "Monitor", aos_font_small, lv_color_white()), LV_ALIGN_TOP_LEFT, 40, 0);
    w->up = aos_label(card_, "", aos_font_caption, lv_color_hex(0xDDE6F0));
    lv_obj_align(w->up, LV_ALIGN_TOP_RIGHT, 0, 2);
    static const char *const NAME[3] = { "CPU", N_("RAM"), "PSRAM" };
    const lv_color_t COL[3] = { lv_color_hex(0x64D2FF), AOS_C_GREEN, lv_color_hex(0xDA8FFF) };
    int32_t top = 52, rowh = (ch - 44 - top) / 3;
    for (int k = 0; k < 3; k++) {
        lv_obj_t *r = box(card_, iw, rowh);
        lv_obj_set_pos(r, 0, top + k * rowh);
        lv_obj_align(aos_label(r, aos_tr(NAME[k]), aos_font_caption, lv_color_hex(0xDDE6F0)), LV_ALIGN_TOP_LEFT, 0, 0);
        w->val[k] = aos_label(r, "--", aos_font_small, lv_color_white());
        lv_obj_align(w->val[k], LV_ALIGN_TOP_RIGHT, 0, -2);
        w->bar[k] = bar(r, iw, 10, COL[k]);
        lv_obj_set_style_bg_color(w->bar[k], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(w->bar[k], LV_OPA_20, LV_PART_MAIN);
        lv_obj_align(w->bar[k], LV_ALIGN_TOP_LEFT, 0, lv_font_get_line_height(aos_font_small) + 4);
        aos_make_decorative(r);
    }
    aos_make_decorative(g);
    w->timer = lv_timer_create(w_timer_cb, 2000, w);
    lv_obj_add_event_cb(card_, w_deleted_cb, LV_EVENT_DELETE, w);
    w_refresh(w);
}

void aos_app_sysmon_get(aos_app_t *app)
{
    /* The widget registers with the app: this getter runs once, from
     * aos_apps_register_builtin(), before aos_ui_init() as the widget
     * registry requires. */
    static bool widget;
    if (!widget) {
        widget = true;
        aos_ui_register_widget("sysmon", widget_create);
    }
    *app = (aos_app_t){
        .desc = {
            .id = "aos.sysmon", .name = "Monitor", .icon = AOS_SYM_MEMORY,
            .color_a = 0x64748B, .color_b = 0x1E293B,
            .order = 190,
        },
        .create = create, .destroy = destroy,
    };
}
