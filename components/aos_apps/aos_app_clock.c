/*
 * P4OS - Reloj: world clock, alarms, stopwatch, timer and pomodoro in one
 * app with tabs, as on iOS. On the watch they were five apps; the logic and
 * the storage come from those (_pending/aos_app_*.c): the city table and the
 * TZ trick of the world clock, the alarms' NVS keys ("alarm0".."alarm5", so a
 * backup from the watch restores here), the pomodoro's phases.
 *
 * Everything lives in statics, so leaving the app or turning the screen
 * loses nothing, and the app is BACKGROUND: the stopwatch, the timer and the
 * pomodoro keep counting with any other app in front. Alarms do not even
 * need the app alive: aos_clock_service_tick() runs from the main loop.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_fonts.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TAB_WORLD = 0, TAB_ALARM, TAB_STOPWATCH, TAB_TIMER, TAB_POMO, TAB_COUNT };

static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_EARTH, AOS_SYM_ALARM, AOS_SYM_AV_TIMER,
                                                  AOS_SYM_TIMER_SAND, AOS_SYM_BRAIN };
static const char *const TAB_NAME[TAB_COUNT] = { N_("Mundial"), N_("Alarmas"), N_("Cronómetro"),
                                                 N_("Temporizador"), N_("Pomodoro") };

#define C_ORANGE lv_color_hex(0xFF9F0A)

/* -------------------------------------------------------------------------- */
/* State (survives everything but a reboot)                                    */
/* -------------------------------------------------------------------------- */

static struct {
    int tab;

    /* stopwatch */
    bool     sw_run;
    uint64_t sw_start, sw_acc;          /* ms */
    uint32_t laps[99];
    int      nlaps;

    /* timer */
    int      tm_set_s;                  /* chosen duration */
    bool     tm_run, tm_paused;
    uint64_t tm_end;                    /* uptime when it reaches zero */
    uint32_t tm_left_ms;                /* while paused */

    /* pomodoro */
    int      po_phase;                  /* 0 focus, 1 short break, 2 long break */
    bool     po_run;
    uint64_t po_end;
    uint32_t po_left_ms;
    int      po_done;                   /* focus blocks in this cycle */

    /* ringing (timer, pomodoro or alarm) */
    int      ring_left;                 /* beeps still to go */
    char     ring_what[48];
} S = { .tm_set_s = 5 * 60, .po_left_ms = 25 * 60 * 1000 };

/* widgets of the page on screen (NULL when not built) */
static struct {
    lv_obj_t *root, *content, *tabs[TAB_COUNT];
    lv_obj_t *big, *sub, *btn_l, *btn_r, *lbl_l, *lbl_r, *list, *arc;
    lv_obj_t *roll_h, *roll_m, *roll_s, *overlay;
    lv_obj_t *day_btn[7];
    int       edit_index;
    int32_t   W, H;
    bool      land;
    lv_timer_t *timer;
} U;

static void build_page(void);

/* -------------------------------------------------------------------------- */
/* Small helpers                                                               */
/* -------------------------------------------------------------------------- */

static uint64_t now_ms(void) { return aos_hal_uptime_ms(); }

static void fmt_hms(char *out, size_t n, uint32_t ms, bool cents)
{
    uint32_t s = ms / 1000, h = s / 3600, m = (s / 60) % 60;
    s %= 60;
    if (cents) {
        if (h) snprintf(out, n, "%u:%02u:%02u,%02u", (unsigned)h, (unsigned)m, (unsigned)s, (unsigned)(ms % 1000 / 10));
        else snprintf(out, n, "%02u:%02u,%02u", (unsigned)m, (unsigned)s, (unsigned)(ms % 1000 / 10));
    } else {
        if (h) snprintf(out, n, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
        else snprintf(out, n, "%02u:%02u", (unsigned)m, (unsigned)s);
    }
}

static lv_obj_t *round_btn(lv_obj_t *parent, int32_t d, lv_color_t col, lv_event_cb_t cb, lv_obj_t **lbl)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, col, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, lv_color_lighten(col, LV_OPA_40), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (lbl) *lbl = l;
    return b;
}

static lv_obj_t *text_btn(lv_obj_t *parent, const char *txt, lv_color_t col, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, col, 0);
    lv_obj_set_style_pad_hor(b, 30, 0);
    lv_obj_set_style_pad_ver(b, 16, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_label_set_text(l, txt);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *plus_btn(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *b = round_btn(parent, 84, C_ORANGE, cb, NULL);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_obj_set_style_text_font(l, &aos_sym_44, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, AOS_SYM_PLUS);
    return b;
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 24, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *vlist(lv_obj_t *parent)
{
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 14, 0);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    return l;
}

static void ring(const char *what)
{
    snprintf(S.ring_what, sizeof S.ring_what, "%s", what);
    S.ring_left = 24;
    aos_hal_display_on(true);
    aos_hal_activity();
    char buf[64];
    snprintf(buf, sizeof buf, "%s", what);
    aos_ui_toast(buf, 6000);
}

/* -------------------------------------------------------------------------- */
/* World clock                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct { const char *name, *tz; } city_t;

/* From the watch: west to east, POSIX TZ rules of 2026. */
static const city_t CITIES[] = {
    { N_("Honolulu"), "HST10" }, { N_("Anchorage"), "AKST9AKDT,M3.2.0,M11.1.0" },
    { N_("Los Ángeles"), "PST8PDT,M3.2.0,M11.1.0" }, { N_("Denver"), "MST7MDT,M3.2.0,M11.1.0" },
    { N_("Ciudad de México"), "CST6" }, { N_("Chicago"), "CST6CDT,M3.2.0,M11.1.0" },
    { N_("Bogotá"), "COT5" }, { N_("Lima"), "PET5" }, { N_("Nueva York"), "EST5EDT,M3.2.0,M11.1.0" },
    { N_("Toronto"), "EST5EDT,M3.2.0,M11.1.0" }, { N_("Caracas"), "VET4" },
    { N_("Santiago"), "CLT4CLST,M9.1.6/24,M4.1.6/24" }, { N_("Buenos Aires"), "ART3" },
    { N_("Montevideo"), "UYT3" }, { N_("São Paulo"), "BRT3" },
    { N_("Londres"), "GMT0BST,M3.5.0/1,M10.5.0/2" }, { N_("Lisboa"), "WET0WEST,M3.5.0/1,M10.5.0/2" },
    { N_("Madrid"), "CET-1CEST,M3.5.0,M10.5.0/3" }, { N_("París"), "CET-1CEST,M3.5.0,M10.5.0/3" },
    { N_("Berlín"), "CET-1CEST,M3.5.0,M10.5.0/3" }, { N_("Roma"), "CET-1CEST,M3.5.0,M10.5.0/3" },
    { N_("Lagos"), "WAT-1" }, { N_("Atenas"), "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { N_("Helsinki"), "EET-2EEST,M3.5.0/3,M10.5.0/4" }, { N_("El Cairo"), "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { N_("Johannesburgo"), "SAST-2" }, { N_("Estambul"), "TRT-3" }, { N_("Moscú"), "MSK-3" },
    { N_("Nairobi"), "EAT-3" }, { N_("Dubái"), "GST-4" }, { N_("Nueva Delhi"), "IST-5:30" },
    { N_("Bangkok"), "ICT-7" }, { N_("Yakarta"), "WIB-7" }, { N_("Pekín"), "CST-8" },
    { N_("Hong Kong"), "HKT-8" }, { N_("Singapur"), "SGT-8" }, { N_("Tokio"), "JST-9" },
    { N_("Seúl"), "KST-9" }, { N_("Sídney"), "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { N_("Auckland"), "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define N_CITIES ((int)(sizeof CITIES / sizeof CITIES[0]))
#define MAX_SEL 10

static int s_sel[MAX_SEL], s_nsel = -1;
static lv_obj_t *s_wc_time[MAX_SEL], *s_wc_day[MAX_SEL];

static void sel_load(void)
{
    if (s_nsel >= 0) return;
    char buf[64] = "";
    s_nsel = 0;
    if (!aos_hal_pref_get_str("wc_sel", buf, sizeof buf)) snprintf(buf, sizeof buf, "8,15,17,36");
    for (char *p = buf; *p && s_nsel < MAX_SEL; ) {
        int i = (int)strtol(p, &p, 10);
        if (i >= 0 && i < N_CITIES) s_sel[s_nsel++] = i;
        while (*p == ',') p++;
    }
}

static void sel_save(void)
{
    char buf[64] = "";
    for (int i = 0; i < s_nsel; i++) {
        char n[8];
        snprintf(n, sizeof n, "%s%d", i ? "," : "", s_sel[i]);
        strlcat(buf, n, sizeof buf);
    }
    aos_hal_pref_set_str("wc_sel", buf);
}

/* A city's local time: the C library only converts through TZ. */
static void city_time(const char *tz, struct tm *out)
{
    char saved[64];
    snprintf(saved, sizeof saved, "%s", aos_hal_timezone_get());
    time_t t = time(NULL);
    setenv("TZ", tz, 1);
    tzset();
    localtime_r(&t, out);
    setenv("TZ", saved, 1);
    tzset();
}

static void world_refresh(void)
{
    struct tm here;
    aos_hal_time_now(&here);
    if (U.big) lv_label_set_text_fmt(U.big, "%d:%02d", here.tm_hour, here.tm_min);
    if (U.sub) lv_label_set_text_fmt(U.sub, "%s %d %s", aos_day_name(here.tm_wday), here.tm_mday, aos_month_name(here.tm_mon));
    for (int i = 0; i < s_nsel; i++) {
        if (!s_wc_time[i]) continue;
        struct tm t;
        city_time(CITIES[s_sel[i]].tz, &t);
        lv_label_set_text_fmt(s_wc_time[i], "%d:%02d", t.tm_hour, t.tm_min);
        int dm = (t.tm_yday - here.tm_yday + 366) % 366;
        const char *day = dm == 0 ? _("Hoy") : dm == 1 ? _("Mañana") : _("Ayer");
        int diff = (t.tm_hour * 60 + t.tm_min) - (here.tm_hour * 60 + here.tm_min);
        if (dm == 1) diff += 24 * 60;
        else if (dm != 0) diff -= 24 * 60;
        if (diff % 60) lv_label_set_text_fmt(s_wc_day[i], "%s, %+d:%02d h", day, diff / 60, abs(diff % 60));
        else lv_label_set_text_fmt(s_wc_day[i], "%s, %+d h", day, diff / 60);
    }
}

static void city_del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    memmove(&s_sel[i], &s_sel[i + 1], (size_t)(s_nsel - i - 1) * sizeof(int));
    s_nsel--;
    sel_save();
    build_page();
}

static void city_pick_cb(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_nsel < MAX_SEL) s_sel[s_nsel++] = c;
    sel_save();
    lv_obj_delete(U.overlay);
    U.overlay = NULL;
    build_page();
}

static void overlay_close_cb(lv_event_t *e)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
}

static lv_obj_t *overlay(const char *title)
{
    lv_obj_t *o = lv_obj_create(U.root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, U.W, U.H);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, AOS_UI_PAD, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = aos_label(o, title, aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 12);
    lv_obj_t *x = text_btn(o, _("Cerrar"), AOS_C_CARD2, overlay_close_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);
    U.overlay = o;
    return o;
}

static void city_add_cb(lv_event_t *e)
{
    lv_obj_t *o = overlay(_("Agregar ciudad"));
    lv_obj_t *l = vlist(o);
    lv_obj_set_size(l, lv_pct(100), U.H - 140);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_pad_row(l, 8, 0);
    for (int c = 0; c < N_CITIES; c++) {
        lv_obj_t *r = card(l);
        lv_obj_set_style_pad_ver(r, 18, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        struct tm t;
        city_time(CITIES[c].tz, &t);
        lv_obj_t *n = aos_label(r, aos_tr(CITIES[c].name), aos_font_body, AOS_C_TEXT);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *h = lv_label_create(r);
        lv_obj_set_style_text_font(h, aos_font_body, 0);
        lv_obj_set_style_text_color(h, AOS_C_DIM, 0);
        lv_label_set_text_fmt(h, "%d:%02d", t.tm_hour, t.tm_min);
        lv_obj_align(h, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(r, city_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)c);
    }
}

static void build_world(lv_obj_t *p)
{
    sel_load();
    memset(s_wc_time, 0, sizeof s_wc_time);
    lv_obj_t *head = lv_obj_create(p);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), U.land ? 150 : 220);
    U.big = lv_label_create(head);
    lv_obj_set_style_text_font(U.big, &aos_inter_num_144, 0);
    lv_obj_set_style_text_color(U.big, AOS_C_TEXT, 0);
    lv_obj_align(U.big, LV_ALIGN_TOP_LEFT, 0, U.land ? -16 : 0);
    U.sub = aos_label(head, "", aos_font_body, AOS_C_DIM);
    lv_obj_align(U.sub, LV_ALIGN_BOTTOM_LEFT, 6, 0);
    lv_obj_t *add = plus_btn(head, city_add_cb);
    lv_obj_align(add, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *l = vlist(p);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_flex_grow(l, 1);
    if (U.land) {
        lv_obj_set_flex_flow(l, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(l, 14, 0);
    }
    for (int i = 0; i < s_nsel; i++) {
        lv_obj_t *r = card(l);
        if (U.land) lv_obj_set_width(r, (U.W - 2 * AOS_UI_PAD - 14) / 2);
        lv_obj_t *n = aos_label(r, aos_tr(CITIES[s_sel[i]].name), aos_font_title, AOS_C_TEXT);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        s_wc_day[i] = aos_label(r, "", aos_font_small, AOS_C_DIM);
        lv_obj_align(s_wc_day[i], LV_ALIGN_TOP_LEFT, 0, 50);
        s_wc_time[i] = lv_label_create(r);
        lv_obj_set_style_text_font(s_wc_time[i], &aos_inter_num_96, 0);
        lv_obj_set_style_text_color(s_wc_time[i], AOS_C_TEXT, 0);
        lv_obj_align(s_wc_time[i], LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_height(r, 124);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, city_del_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
    lv_obj_t *hint = aos_label(l, _("Mantené apretada una ciudad para quitarla"), aos_font_caption, AOS_C_DIM);
    (void)hint;
    world_refresh();
}

/* -------------------------------------------------------------------------- */
/* Alarms (storage from the watch: alarmN = minute | enabled<<16 | days<<17)   */
/* -------------------------------------------------------------------------- */

#define MAX_ALARMS 6
#define ALL_DAYS   0x7F

bool aos_alarm_get(int index, int *minute_of_day, bool *enabled, int *days)
{
    if (index < 0 || index >= MAX_ALARMS) return false;
    char key[16];
    snprintf(key, sizeof key, "alarm%d", index);
    int32_t v = 0;
    bool have = aos_hal_pref_get_i32(key, &v) && (v & 0xFFFF) != 0xFFFF && v != 0;
    int mask = (int)((v >> 17) & ALL_DAYS);
    if (minute_of_day) *minute_of_day = have ? (int)(v & 0xFFFF) : -1;
    if (enabled) *enabled = have && ((v >> 16) & 1);
    if (days) *days = have ? (mask ? mask : ALL_DAYS) : ALL_DAYS;
    return true;
}

bool aos_alarm_set(int index, int minute_of_day, bool enabled, int days)
{
    if (index < 0 || index >= MAX_ALARMS || minute_of_day >= 24 * 60) return false;
    days &= ALL_DAYS;
    if (minute_of_day >= 0 && !days) return false;
    char key[16];
    snprintf(key, sizeof key, "alarm%d", index);
    int32_t v = minute_of_day < 0 ? 0xFFFF : (minute_of_day | (enabled ? 1 << 16 : 0) | ((int32_t)days << 17));
    return aos_hal_pref_set_i32(key, v);
}

static const int DAY_WDAY[7] = { 1, 2, 3, 4, 5, 6, 0 };     /* Monday first */
static const char *const DAY_INITIAL[7] = { NC_("lun", "L"), NC_("mar", "M"), NC_("mie", "M"), NC_("jue", "J"),
                                            NC_("vie", "V"), NC_("sab", "S"), NC_("dom", "D") };
static const char *const DAY_CTX[7] = { "lun", "mar", "mie", "jue", "vie", "sab", "dom" };

static void days_text(int days, char *out, size_t n)
{
    if (days == ALL_DAYS) { snprintf(out, n, "%s", _("Todos los días")); return; }
    if (days == 0x3E) { snprintf(out, n, "%s", _("Lunes a viernes")); return; }
    if (days == 0x41) { snprintf(out, n, "%s", _("Fines de semana")); return; }
    out[0] = 0;
    for (int i = 0; i < 7; i++)
        if (days & (1 << DAY_WDAY[i])) { strlcat(out, aos_trc(DAY_CTX[i], DAY_INITIAL[i]), n); strlcat(out, " ", n); }
}

static void alarm_toggle_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int m, d;
    bool en;
    aos_alarm_get(i, &m, &en, &d);
    aos_alarm_set(i, m, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED), d);
}

static void day_toggle_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    lv_obj_set_state(b, LV_STATE_CHECKED, !lv_obj_has_state(b, LV_STATE_CHECKED));
}

static lv_obj_t *roller(lv_obj_t *parent, int max, int value, int32_t w)
{
    char opts[400] = "";
    for (int i = 0; i < max; i++) {
        char n[6];
        snprintf(n, sizeof n, "%s%02d", i ? "\n" : "", i);
        strlcat(opts, n, sizeof opts);
    }
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(r, 3);
    lv_roller_set_selected(r, value, LV_ANIM_OFF);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, &aos_inter_num_96, LV_PART_SELECTED);
    lv_obj_set_style_text_font(r, aos_font_large, 0);
    lv_obj_set_style_bg_color(r, AOS_C_BG, 0);
    lv_obj_set_style_text_color(r, AOS_C_DIM, 0);
    lv_obj_set_style_text_color(r, AOS_C_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, AOS_C_CARD, LV_PART_SELECTED);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_text_line_space(r, 30, 0);
    return r;
}

static void alarm_save_cb(lv_event_t *e)
{
    int days = 0;
    for (int i = 0; i < 7; i++) if (lv_obj_has_state(U.day_btn[i], LV_STATE_CHECKED)) days |= 1 << DAY_WDAY[i];
    if (!days) days = ALL_DAYS;
    int m = (int)lv_roller_get_selected(U.roll_h) * 60 + (int)lv_roller_get_selected(U.roll_m);
    aos_alarm_set(U.edit_index, m, true, days);
    overlay_close_cb(e);
    build_page();
}

static void alarm_delete_cb(lv_event_t *e)
{
    aos_alarm_set(U.edit_index, -1, false, ALL_DAYS);
    overlay_close_cb(e);
    build_page();
}

static void alarm_edit(int index)
{
    int m = 7 * 60, d = 0x3E;
    bool en;
    if (index >= 0) aos_alarm_get(index, &m, &en, &d);
    else {
        for (int i = 0; i < MAX_ALARMS; i++) {
            int mm;
            aos_alarm_get(i, &mm, &en, NULL);
            if (mm < 0) { index = i; break; }
        }
        if (index < 0) { aos_ui_toast(_("Ya hay seis alarmas"), 1800); return; }
        m = 7 * 60;
    }
    U.edit_index = index;
    lv_obj_t *o = overlay(_("Alarma"));
    lv_obj_t *rows = lv_obj_create(o);
    lv_obj_remove_style_all(rows);
    lv_obj_set_size(rows, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(rows, 20, 0);
    lv_obj_align(rows, LV_ALIGN_TOP_MID, 0, 120);
    U.roll_h = roller(rows, 24, m / 60, 200);
    U.roll_m = roller(rows, 60, m % 60, 200);

    lv_obj_t *days = lv_obj_create(o);
    lv_obj_remove_style_all(days);
    lv_obj_set_size(days, lv_pct(100), 96);
    lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(days, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align_to(days, rows, LV_ALIGN_OUT_BOTTOM_MID, 0, 40);
    for (int i = 0; i < 7; i++) {
        lv_obj_t *b = lv_obj_create(days);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 80, 80);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_color(b, C_ORANGE, LV_STATE_CHECKED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = aos_label(b, aos_trc(DAY_CTX[i], DAY_INITIAL[i]), aos_font_body, AOS_C_TEXT);
        lv_obj_center(l);
        lv_obj_set_state(b, LV_STATE_CHECKED, (d >> DAY_WDAY[i]) & 1);
        lv_obj_add_event_cb(b, day_toggle_cb, LV_EVENT_CLICKED, NULL);
        U.day_btn[i] = b;
    }
    lv_obj_t *bar = lv_obj_create(o);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), 90);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -10);
    text_btn(bar, _("Borrar"), AOS_C_RED, alarm_delete_cb, NULL);
    text_btn(bar, _("Guardar"), C_ORANGE, alarm_save_cb, NULL);
}

static void alarm_row_cb(lv_event_t *e) { alarm_edit((int)(intptr_t)lv_event_get_user_data(e)); }
static void alarm_add_cb(lv_event_t *e) { alarm_edit(-1); }

static void build_alarm(lv_obj_t *p)
{
    lv_obj_t *head = lv_obj_create(p);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 90);
    lv_obj_t *t = aos_label(head, _("Alarmas"), aos_font_large, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *add = plus_btn(head, alarm_add_cb);
    lv_obj_align(add, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *l = vlist(p);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_flex_grow(l, 1);
    int shown = 0;
    for (int i = 0; i < MAX_ALARMS; i++) {
        int m, d;
        bool en;
        aos_alarm_get(i, &m, &en, &d);
        if (m < 0) continue;
        shown++;
        lv_obj_t *r = card(l);
        lv_obj_set_height(r, 150);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, alarm_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *h = lv_label_create(r);
        lv_obj_set_style_text_font(h, &aos_inter_num_96, 0);
        lv_obj_set_style_text_color(h, en ? AOS_C_TEXT : AOS_C_DIM, 0);
        lv_label_set_text_fmt(h, "%d:%02d", m / 60, m % 60);
        lv_obj_align(h, LV_ALIGN_LEFT_MID, 0, -10);
        char dt[64];
        days_text(d, dt, sizeof dt);
        lv_obj_t *dl = aos_label(r, dt, aos_font_small, AOS_C_DIM);
        lv_obj_align(dl, LV_ALIGN_BOTTOM_LEFT, 4, 4);
        lv_obj_t *sw = lv_switch_create(r);
        lv_obj_set_size(sw, 110, 60);
        lv_obj_set_style_bg_color(sw, C_ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_state(sw, LV_STATE_CHECKED, en);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(sw, alarm_toggle_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
    if (!shown) aos_label(l, _("Sin alarmas. Tocá + para agregar una."), aos_font_body, AOS_C_DIM);
}

/* -------------------------------------------------------------------------- */
/* Stopwatch                                                                   */
/* -------------------------------------------------------------------------- */

static uint32_t sw_elapsed(void) { return (uint32_t)(S.sw_acc + (S.sw_run ? now_ms() - S.sw_start : 0)); }

static void sw_refresh(void)
{
    char b[24];
    fmt_hms(b, sizeof b, sw_elapsed(), true);
    if (U.big) lv_label_set_text(U.big, b);
    if (U.lbl_l) lv_label_set_text(U.lbl_l, S.sw_run ? _("Vuelta") : _("Reiniciar"));
    if (U.lbl_r) lv_label_set_text(U.lbl_r, S.sw_run ? _("Detener") : _("Iniciar"));
    if (U.btn_r) lv_obj_set_style_bg_color(U.btn_r, S.sw_run ? AOS_C_RED : AOS_C_GREEN, 0);
    if (U.lbl_r) lv_obj_set_style_text_color(U.lbl_r, S.sw_run ? lv_color_hex(0xFF8A80) : lv_color_hex(0x8AF5A6), 0);
}

static void sw_laps_fill(void)
{
    if (!U.list) return;
    lv_obj_clean(U.list);
    uint32_t best = UINT32_MAX, worst = 0;
    for (int i = 0; i < S.nlaps; i++) { if (S.laps[i] < best) best = S.laps[i]; if (S.laps[i] > worst) worst = S.laps[i]; }
    for (int i = S.nlaps - 1; i >= 0; i--) {
        lv_obj_t *r = lv_obj_create(U.list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), 64);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
        lv_color_t c = S.nlaps > 2 && S.laps[i] == best ? AOS_C_GREEN : S.nlaps > 2 && S.laps[i] == worst ? AOS_C_RED : AOS_C_TEXT;
        lv_obj_t *n = lv_label_create(r);
        lv_obj_set_style_text_font(n, aos_font_body, 0);
        lv_obj_set_style_text_color(n, c, 0);
        lv_label_set_text_fmt(n, "%s %d", _("Vuelta"), i + 1);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        char b[24];
        fmt_hms(b, sizeof b, S.laps[i], true);
        lv_obj_t *v = aos_label(r, b, aos_font_body, c);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void sw_left_cb(lv_event_t *e)
{
    if (S.sw_run) {
        uint32_t total = sw_elapsed(), prev = 0;
        for (int i = 0; i < S.nlaps; i++) prev += S.laps[i];
        if (S.nlaps < 99) S.laps[S.nlaps++] = total - prev;
    } else {
        S.sw_acc = 0;
        S.nlaps = 0;
    }
    sw_laps_fill();
    sw_refresh();
}

static void sw_right_cb(lv_event_t *e)
{
    if (S.sw_run) { S.sw_acc += now_ms() - S.sw_start; S.sw_run = false; }
    else { S.sw_start = now_ms(); S.sw_run = true; }
    sw_refresh();
}

static void build_buttons(lv_obj_t *p, lv_event_cb_t left, lv_event_cb_t right)
{
    lv_obj_t *bar = lv_obj_create(p);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), 180);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    U.btn_l = round_btn(bar, 170, lv_color_hex(0x8E8E93), left, &U.lbl_l);
    lv_obj_align(U.btn_l, LV_ALIGN_LEFT_MID, 0, 0);
    U.btn_r = round_btn(bar, 170, AOS_C_GREEN, right, &U.lbl_r);
    lv_obj_align(U.btn_r, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void build_stopwatch(lv_obj_t *p)
{
    lv_obj_t *top = p;
    if (U.land) {
        lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
        top = lv_obj_create(p);
        lv_obj_remove_style_all(top);
        lv_obj_set_size(top, U.W / 2, lv_pct(100));
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    U.big = lv_label_create(top);
    lv_obj_set_style_text_font(U.big, &aos_inter_num_144, 0);
    lv_obj_set_style_text_color(U.big, AOS_C_TEXT, 0);
    lv_obj_set_style_pad_ver(U.big, U.land ? 0 : 80, 0);
    build_buttons(top, sw_left_cb, sw_right_cb);
    U.list = vlist(p);
    lv_obj_set_width(U.list, U.land ? U.W / 2 - 2 * AOS_UI_PAD : lv_pct(100));
    lv_obj_set_flex_grow(U.list, 1);
    lv_obj_set_style_pad_row(U.list, 0, 0);
    if (U.land) lv_obj_set_height(U.list, lv_pct(100));
    sw_laps_fill();
    sw_refresh();
}

/* -------------------------------------------------------------------------- */
/* Timer                                                                       */
/* -------------------------------------------------------------------------- */

static uint32_t tm_left(void)
{
    if (!S.tm_run) return (uint32_t)S.tm_set_s * 1000;
    if (S.tm_paused) return S.tm_left_ms;
    uint64_t n = now_ms();
    return S.tm_end > n ? (uint32_t)(S.tm_end - n) : 0;
}

static void tm_refresh(void)
{
    if (!S.tm_run) return;
    char b[16];
    fmt_hms(b, sizeof b, tm_left() + 999, false);
    if (U.big) lv_label_set_text(U.big, b);
    if (U.arc) lv_arc_set_value(U.arc, (int32_t)(tm_left() / 100));
    if (U.lbl_r) lv_label_set_text(U.lbl_r, S.tm_paused ? _("Seguir") : _("Pausa"));
}

static void tm_start_cb(lv_event_t *e)
{
    if (!S.tm_run) {
        int s = (int)lv_roller_get_selected(U.roll_h) * 3600 + (int)lv_roller_get_selected(U.roll_m) * 60 +
                (int)lv_roller_get_selected(U.roll_s);
        if (!s) return;
        S.tm_set_s = s;
        S.tm_run = true;
        S.tm_paused = false;
        S.tm_end = now_ms() + (uint64_t)s * 1000;
        build_page();
    } else if (S.tm_paused) {
        S.tm_end = now_ms() + S.tm_left_ms;
        S.tm_paused = false;
    } else {
        S.tm_left_ms = tm_left();
        S.tm_paused = true;
    }
    tm_refresh();
}

static void tm_cancel_cb(lv_event_t *e)
{
    S.tm_run = false;
    S.ring_left = 0;
    build_page();
}

static void tm_preset_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    lv_roller_set_selected(U.roll_h, m / 60, LV_ANIM_ON);
    lv_roller_set_selected(U.roll_m, m % 60, LV_ANIM_ON);
    lv_roller_set_selected(U.roll_s, 0, LV_ANIM_ON);
}

static lv_obj_t *big_arc(lv_obj_t *p, int32_t d, lv_color_t c, int32_t max)
{
    lv_obj_t *a = lv_arc_create(p);
    lv_obj_set_size(a, d, d);
    lv_arc_set_rotation(a, 270);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, max);
    lv_arc_set_mode(a, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_width(a, 18, 0);
    lv_obj_set_style_arc_width(a, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, AOS_C_CARD2, 0);
    lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

static void build_timer(lv_obj_t *p)
{
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (!S.tm_run) {
        lv_obj_t *rows = lv_obj_create(p);
        lv_obj_remove_style_all(rows);
        lv_obj_set_size(rows, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(rows, 16, 0);
        U.roll_h = roller(rows, 24, S.tm_set_s / 3600, 190);
        U.roll_m = roller(rows, 60, (S.tm_set_s / 60) % 60, 190);
        U.roll_s = roller(rows, 60, S.tm_set_s % 60, 190);
        lv_obj_t *units = aos_label(p, _("horas            minutos            segundos"), aos_font_small, AOS_C_DIM);
        (void)units;
        lv_obj_t *chips = lv_obj_create(p);
        lv_obj_remove_style_all(chips);
        lv_obj_set_size(chips, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(chips, 20, 0);
        lv_obj_set_style_pad_gap(chips, 14, 0);
        static const int P[] = { 1, 3, 5, 10, 15, 25, 45, 60 };
        for (int i = 0; i < 8; i++) {
            char t[12];
            snprintf(t, sizeof t, P[i] < 60 ? "%d min" : "%d h", P[i] < 60 ? P[i] : P[i] / 60);
            text_btn(chips, t, AOS_C_CARD2, tm_preset_cb, (void *)(intptr_t)P[i]);
        }
        lv_obj_t *go = text_btn(p, _("Iniciar"), AOS_C_GREEN, tm_start_cb, NULL);
        lv_obj_set_style_pad_hor(go, 80, 0);
        return;
    }
    int32_t d = U.land ? U.H - 120 : U.W - 2 * AOS_UI_PAD - 40;
    U.arc = big_arc(p, d, C_ORANGE, S.tm_set_s * 10);
    U.big = lv_label_create(U.arc);
    lv_obj_set_style_text_font(U.big, &aos_inter_num_144, 0);
    lv_obj_set_style_text_color(U.big, AOS_C_TEXT, 0);
    lv_obj_center(U.big);
    if (U.land) {
        lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(p, 60, 0);
    }
    lv_obj_t *bar = lv_obj_create(p);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, U.land ? 180 : lv_pct(100), U.land ? 400 : 180);
    U.btn_l = round_btn(bar, 170, lv_color_hex(0x8E8E93), tm_cancel_cb, &U.lbl_l);
    lv_label_set_text(U.lbl_l, _("Cancelar"));
    U.btn_r = round_btn(bar, 170, C_ORANGE, tm_start_cb, &U.lbl_r);
    lv_obj_align(U.btn_l, U.land ? LV_ALIGN_TOP_MID : LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(U.btn_r, U.land ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    tm_refresh();
}

/* -------------------------------------------------------------------------- */
/* Pomodoro                                                                    */
/* -------------------------------------------------------------------------- */

static const int PO_MIN[3] = { 25, 5, 15 };
static const char *const PO_NAME[3] = { N_("Foco"), N_("Pausa corta"), N_("Pausa larga") };
static const uint32_t PO_COLOR[3] = { 0xFF453A, 0x30D158, 0x0A84FF };

static uint32_t po_left(void)
{
    if (!S.po_run) return S.po_left_ms;
    uint64_t n = now_ms();
    return S.po_end > n ? (uint32_t)(S.po_end - n) : 0;
}

static void po_refresh(void)
{
    char b[16];
    fmt_hms(b, sizeof b, po_left() + 999, false);
    if (U.big) lv_label_set_text(U.big, b);
    if (U.arc) {
        lv_arc_set_value(U.arc, (int32_t)(po_left() / 1000));
        lv_obj_set_style_arc_color(U.arc, lv_color_hex(PO_COLOR[S.po_phase]), LV_PART_INDICATOR);
    }
    if (U.sub) lv_label_set_text_fmt(U.sub, "%s  ·  %d/4", aos_tr(PO_NAME[S.po_phase]), S.po_done % 4 + (S.po_phase == 0));
    if (U.lbl_r) lv_label_set_text(U.lbl_r, S.po_run ? _("Pausa") : _("Iniciar"));
}

static void po_next(bool completed)
{
    if (S.po_phase == 0 && completed) S.po_done++;
    S.po_phase = S.po_phase == 0 ? (S.po_done % 4 == 0 && S.po_done ? 2 : 1) : 0;
    S.po_left_ms = (uint32_t)PO_MIN[S.po_phase] * 60000;
    S.po_run = false;
    if (U.arc) lv_arc_set_range(U.arc, 0, PO_MIN[S.po_phase] * 60);
}

static void po_start_cb(lv_event_t *e)
{
    if (S.po_run) { S.po_left_ms = po_left(); S.po_run = false; }
    else { S.po_end = now_ms() + S.po_left_ms; S.po_run = true; }
    po_refresh();
}

static void po_skip_cb(lv_event_t *e) { po_next(false); po_refresh(); }

static void build_pomo(lv_obj_t *p)
{
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (U.land) {
        lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(p, 60, 0);
    }
    int32_t d = U.land ? U.H - 120 : U.W - 2 * AOS_UI_PAD - 40;
    U.arc = big_arc(p, d, lv_color_hex(PO_COLOR[S.po_phase]), PO_MIN[S.po_phase] * 60);
    U.big = lv_label_create(U.arc);
    lv_obj_set_style_text_font(U.big, &aos_inter_num_144, 0);
    lv_obj_set_style_text_color(U.big, AOS_C_TEXT, 0);
    lv_obj_align(U.big, LV_ALIGN_CENTER, 0, -20);
    U.sub = aos_label(U.arc, "", aos_font_body, AOS_C_DIM);
    lv_obj_align(U.sub, LV_ALIGN_CENTER, 0, 90);
    lv_obj_t *bar = lv_obj_create(p);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, U.land ? 180 : lv_pct(100), U.land ? 400 : 180);
    U.btn_l = round_btn(bar, 170, lv_color_hex(0x8E8E93), po_skip_cb, &U.lbl_l);
    lv_label_set_text(U.lbl_l, _("Saltar"));
    U.btn_r = round_btn(bar, 170, AOS_C_GREEN, po_start_cb, &U.lbl_r);
    lv_obj_align(U.btn_l, U.land ? LV_ALIGN_TOP_MID : LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(U.btn_r, U.land ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    po_refresh();
}

/* -------------------------------------------------------------------------- */
/* Tabs and life cycle                                                         */
/* -------------------------------------------------------------------------- */

static void build_page(void)
{
    if (!U.content) return;
    if (U.overlay) { lv_obj_delete(U.overlay); U.overlay = NULL; }
    lv_obj_clean(U.content);
    U.big = U.sub = U.btn_l = U.btn_r = U.lbl_l = U.lbl_r = U.list = U.arc = NULL;
    U.roll_h = U.roll_m = U.roll_s = NULL;
    lv_obj_set_flex_flow(U.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(U.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(U.content, 0, 0);
    switch (S.tab) {
    case TAB_WORLD: build_world(U.content); break;
    case TAB_ALARM: build_alarm(U.content); break;
    case TAB_STOPWATCH: build_stopwatch(U.content); break;
    case TAB_TIMER: build_timer(U.content); break;
    default: build_pomo(U.content); break;
    }
    for (int i = 0; i < TAB_COUNT; i++)
        lv_obj_set_style_text_color(U.tabs[i], i == S.tab ? C_ORANGE : AOS_C_DIM, 0);
}

static void tab_cb(lv_event_t *e)
{
    S.tab = (int)(intptr_t)lv_event_get_user_data(e);
    build_page();
}

static void ui_timer_cb(lv_timer_t *t)
{
    switch (S.tab) {
    case TAB_STOPWATCH: if (S.sw_run) sw_refresh(); break;
    case TAB_TIMER: tm_refresh(); break;
    case TAB_POMO: po_refresh(); break;
    case TAB_WORLD: { static uint32_t n; if (++n % 50 == 0) world_refresh(); break; }
    default: break;
    }
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    const int32_t tab_h = U.land ? 96 : 116;

    U.content = lv_obj_create(root);
    lv_obj_remove_style_all(U.content);
    lv_obj_set_size(U.content, U.W, U.H - tab_h);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_ver(U.content, 10, 0);
    lv_obj_set_style_pad_row(U.content, 18, 0);
    lv_obj_remove_flag(U.content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, U.W, tab_h);
    lv_obj_set_pos(bar, 0, U.H - tab_h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *t = lv_obj_create(bar);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, U.W / TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *g = lv_label_create(t);
        lv_obj_set_style_text_font(g, &aos_sym_44, 0);
        lv_label_set_text(g, TAB_GLYPH[i]);
        lv_obj_align(g, LV_ALIGN_CENTER, 0, U.land ? -14 : -16);
        lv_obj_t *n = lv_label_create(t);
        lv_obj_set_style_text_font(n, aos_font_tiny, 0);
        lv_label_set_text(n, aos_tr(TAB_NAME[i]));
        lv_obj_align(n, LV_ALIGN_CENTER, 0, U.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = t;
    }
    build_page();
    U.timer = lv_timer_create(ui_timer_cb, 50, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    memset(&U, 0, sizeof U);
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.overlay) { lv_obj_delete(U.overlay); U.overlay = NULL; return true; }
    return false;
}

/* 5 Hz, with the app hidden too (BACKGROUND): the ends of the timer and the
 * pomodoro. */
static void tick(aos_app_t *self, void *inst)
{
    if (S.tm_run && !S.tm_paused && tm_left() == 0) {
        S.tm_run = false;
        ring(_("Temporizador terminado"));
        if (S.tab == TAB_TIMER) build_page();
    }
    if (S.po_run && po_left() == 0) {
        int was = S.po_phase;
        po_next(true);
        ring(was == 0 ? _("Pomodoro: a descansar") : _("Pomodoro: a trabajar"));
        if (S.tab == TAB_POMO) po_refresh();
    }
}

/* From the main loop, 5 Hz, with or without the app: the alarms and the
 * ringing itself. */
void aos_clock_service_tick(void)
{
    static int last_minute = -1;
    if (S.ring_left > 0) {
        if (S.ring_left % 2 == 0) aos_hal_beep(2200, 150);
        S.ring_left--;
    }
    struct tm now;
    aos_hal_time_now(&now);
    if (!aos_hal_time_is_valid()) return;
    int mod = now.tm_hour * 60 + now.tm_min;
    if (mod == last_minute) return;
    last_minute = mod;
    for (int i = 0; i < MAX_ALARMS; i++) {
        int m, d;
        bool en;
        aos_alarm_get(i, &m, &en, &d);
        if (en && m == mod && (d & (1 << now.tm_wday))) {
            char b[48];
            snprintf(b, sizeof b, "%s  %d:%02d", _("Alarma"), m / 60, m % 60);
            ring(b);
            break;
        }
    }
}

void aos_app_clock_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.clock", .name = "Reloj", .icon = AOS_SYM_CLOCK_OUTLINE,
            .color_a = 0x2C2C2E, .color_b = 0x000000,
            .flags = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_BACKGROUND,
            .order = 20,
        },
        .create = create, .destroy = destroy, .back = back, .tick = tick,
    };
}
