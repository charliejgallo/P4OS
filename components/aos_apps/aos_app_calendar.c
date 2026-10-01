/*
 * P4OS - Calendario (from AmoledOS)
 *
 * The month as iOS draws it: the name in red, the week starting on Monday,
 * today's number in red and the selected day inside a circle (red if it is
 * today, white otherwise), the days of the neighbouring months dimmed so the
 * six rows never look lopsided, and the ISO week number down the left edge.
 *
 * On the watch there was room for the grid and a one-line footer. Here the
 * selected day gets a panel of its own -under the grid upright, beside it
 * lying down, where next month's page also fits-, and the panel carries the
 * events list. There are no events yet: the Home Assistant calendar arrives
 * in a later phase (APPS.md), and events_fill() is the one place it will
 * plug into. Everything else is ready for it: the list is a flex column that
 * scrolls, and the cells have a spare dot under the number for "this day has
 * something".
 *
 * Navigation, as on the watch, in three forms that do the same thing: the
 * header's chevrons, swiping over the grid (up or left = next month, down or
 * right = previous; the system's back is the left EDGE only, so a swipe in
 * the middle is ours), and tapping the month's name, which opens the year
 * with the twelve months drawn small, as iOS does.
 *
 * Dates are computed with integers (Sakamoto for the day of the week, days
 * since the epoch for the distances), not with mktime(): a 32-bit time_t
 * runs out in 2038 and here you can navigate as far as 2099.
 *
 * The state lives in statics, so leaving the app (KEEP) or turning the
 * screen (resize() rebuilds) loses neither the month nor the selection.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_fonts.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define YEAR_MIN    1970
#define YEAR_MAX    2099

#define C_OTHER     lv_color_hex(0x48484A)  /* days of the neighbouring month */
#define C_WEEKEND   lv_color_hex(0x9A9AA0)  /* Saturday and Sunday of this month */
#define C_LINE      lv_color_hex(0x2C2C2E)  /* hairline between the weeks */
#define C_RED       lv_color_hex(0xFF453A)

static const char *const MES_LARGO[12] = {
    N_("Enero"), N_("Febrero"), N_("Marzo"), N_("Abril"), N_("Mayo"), N_("Junio"),
    N_("Julio"), N_("Agosto"), N_("Septiembre"), N_("Octubre"), N_("Noviembre"),
    N_("Diciembre"),
};

static const char *const MES_MIN[12] = {
    N_("enero"), N_("febrero"), N_("marzo"), N_("abril"), N_("mayo"), N_("junio"),
    N_("julio"), N_("agosto"), N_("septiembre"), N_("octubre"), N_("noviembre"),
    N_("diciembre"),
};

static const char *const DIA_LARGO[7] = {
    N_("Domingo"), N_("Lunes"), N_("Martes"), N_("Miércoles"), N_("Jueves"),
    N_("Viernes"), N_("Sábado"),
};

/* Header of the grid, Monday first. With context: in Spanish martes and
 * miércoles both start with M, in English they are T and W, and one "M" key
 * cannot give both (the watch's lesson, kept). */
static const char *const DIA_INICIAL[7] = {
    NC_("lun", "L"), NC_("mar", "M"), NC_("mie", "M"), NC_("jue", "J"),
    NC_("vie", "V"), NC_("sab", "S"), NC_("dom", "D"),
};
static const char *const DIA_INICIAL_CTX[7] = {
    "lun", "mar", "mie", "jue", "vie", "sab", "dom",
};

/* -------------------------------------------------------------------------- */
/* State (survives leaving the app and turning the screen)                     */
/* -------------------------------------------------------------------------- */

static struct {
    bool started;
    int  view_y, view_m;            /* month on screen (m: 0..11)          */
    int  sel_d;                     /* selected day of that month, 1..31   */
    bool year_open;
    int  yv_year;                   /* year of the year view               */
    int  today_y, today_m, today_d; /* today_y < 0 = the clock is not set  */
    bool time_ok;
    uint32_t last_check_ms;
    uint32_t last_swipe_ms;
} S;

/* -------------------------------------------------------------------------- */
/* Widgets (rebuilt on every create/resize)                                    */
/* -------------------------------------------------------------------------- */

static struct {
    lv_obj_t *root, *page;
    int32_t W, H;
    bool land;

    /* month view */
    lv_obj_t *month_view;
    lv_obj_t *lbl_month, *lbl_year, *btn_today;
    lv_obj_t *cell[42], *cell_lbl[42], *cell_dot[42];
    lv_obj_t *week_lbl[6];
    int       cell_day[42], cell_off[42];

    /* the selected day's panel */
    lv_obj_t *p_num, *p_wday, *p_date, *p_rel, *p_meta, *p_events;
    lv_obj_t *next_box;             /* landscape: next month, small */

    /* year view */
    lv_obj_t *year_view, *lbl_yv, *yv_grid;
} U;

/* -------------------------------------------------------------------------- */
/* Dates, all with integers                                                    */
/* -------------------------------------------------------------------------- */

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static int days_in_month(int y, int m)
{
    static const int len[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 1 && leap(y)) ? 29 : len[m];
}

/* Sakamoto: 0 = Sunday. */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 2) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m] + d) % 7;
}

/* Column of the grid, with the week starting on Monday. */
static int col_of(int wday) { return (wday + 6) % 7; }

/* Days since 1970-01-01 (Howard Hinnant's days_from_civil); m is 0..11. */
static long days_from_civil(int y, int m, int d)
{
    m += 1;
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int day_of_year(int y, int m, int d)
{
    return (int)(days_from_civil(y, m, d) - days_from_civil(y, 0, 1)) + 1;
}

/* ISO 8601 week: the week belongs to the year its Thursday falls in. */
static int iso_week(int y, int m, int d)
{
    int wd = col_of(weekday(y, m, d));          /* 0 = Monday */
    long thu = days_from_civil(y, m, d) - wd + 3;
    int ty = y;
    if (thu < days_from_civil(y, 0, 1)) ty = y - 1;
    else if (thu >= days_from_civil(y + 1, 0, 1)) ty = y + 1;
    return (int)((thu - days_from_civil(ty, 0, 1)) / 7) + 1;
}

static void read_today(void)
{
    /* aos_hal_time_is_valid() may go all the way to NVS: asked once, and
     * after that only while there is still no time. */
    if (!S.time_ok) S.time_ok = aos_hal_time_is_valid();
    if (!S.time_ok) {
        S.today_y = -1;
        return;
    }
    struct tm now;
    aos_hal_time_now(&now);
    S.today_y = now.tm_year + 1900;
    S.today_m = now.tm_mon;
    S.today_d = now.tm_mday;
}

static bool is_today(int y, int m, int d)
{
    return S.today_y >= 0 && S.today_y == y && S.today_m == m && S.today_d == d;
}

static bool viewing_today_month(void)
{
    return S.today_y >= 0 && S.today_y == S.view_y && S.today_m == S.view_m;
}

/* The day a month opens on: today if it is today's month, the 1st if not
 * (what iOS does). */
static int default_sel(void)
{
    return viewing_today_month() ? S.today_d : 1;
}

/* -------------------------------------------------------------------------- */
/* Events (the hook for Home Assistant)                                        */
/* -------------------------------------------------------------------------- */

/* Fills the list of the selected day. For now it only has the empty state;
 * when the HA calendar arrives this is where its events go, one row each
 * (time on the left, title, the calendar's colour as a bar), and a day with
 * events turns on its cell's dot in month_refresh() (U.cell_dot). */
static void events_fill(lv_obj_t *list, int y, int m, int d)
{
    (void)y; (void)m; (void)d;
    lv_obj_clean(list);

    lv_obj_t *box = lv_obj_create(list);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 18, 0);

    lv_obj_t *g = aos_label(box, AOS_SYM_CALENDAR_TODAY, &aos_sym_44, C_OTHER);
    (void)g;
    lv_obj_t *col = lv_obj_create(box);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0);
    aos_label(col, _("Sin eventos"), aos_font_body, AOS_C_TEXT);
    lv_obj_t *h = aos_label(col, _("Los eventos del calendario de Home Assistant van a aparecer acá."),
                            aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(h, lv_pct(100));
    lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
    aos_make_decorative(box);
}

/* -------------------------------------------------------------------------- */
/* Refresh                                                                     */
/* -------------------------------------------------------------------------- */

static void panel_refresh(void)
{
    if (!U.p_num) return;
    char b[64];
    const int y = S.view_y, m = S.view_m, d = S.sel_d;
    const int wd = weekday(y, m, d);
    const bool today = is_today(y, m, d);

    snprintf(b, sizeof b, "%d", d);
    lv_label_set_text(U.p_num, b);
    lv_obj_set_style_text_color(U.p_num, today ? C_RED : AOS_C_TEXT, 0);

    lv_label_set_text(U.p_wday, _(DIA_LARGO[wd]));
    lv_obj_set_style_text_color(U.p_wday, today ? C_RED : AOS_C_DIM, 0);

    /* no year: the header says it, and the selection is always in the month
     * on screen */
    snprintf(b, sizeof b, _("%d de %s"), d, _(MES_MIN[m]));
    lv_label_set_text(U.p_date, b);

    if (S.today_y < 0) {
        lv_label_set_text(U.p_rel, _("Reloj sin ajustar"));
        lv_obj_set_style_text_color(U.p_rel, AOS_C_ORANGE, 0);
    } else {
        long diff = days_from_civil(y, m, d) - days_from_civil(S.today_y, S.today_m, S.today_d);
        if (diff == 0)       snprintf(b, sizeof b, "%s", _("Hoy"));
        else if (diff == 1)  snprintf(b, sizeof b, "%s", _("Mañana"));
        else if (diff == -1) snprintf(b, sizeof b, "%s", _("Ayer"));
        else if (diff > 0)   snprintf(b, sizeof b, _("Dentro de %ld días"), diff);
        else                 snprintf(b, sizeof b, _("Hace %ld días"), -diff);
        lv_label_set_text(U.p_rel, b);
        lv_obj_set_style_text_color(U.p_rel, diff == 0 ? C_RED : AOS_C_DIM, 0);
    }

    snprintf(b, sizeof b, _("Semana %d  ·  día %d del año"), iso_week(y, m, d), day_of_year(y, m, d));
    lv_label_set_text(U.p_meta, b);

    events_fill(U.p_events, y, m, d);
}

static void mini_month(lv_obj_t *parent, int y, int m, int32_t w, int32_t h,
                       const lv_font_t *title_font, const lv_font_t *df, bool initials);

static void month_refresh(void)
{
    char b[16];
    if (!U.lbl_month) return;

    lv_label_set_text(U.lbl_month, _(MES_LARGO[S.view_m]));
    snprintf(b, sizeof b, "%d", S.view_y);
    lv_label_set_text(U.lbl_year, b);

    const int start = col_of(weekday(S.view_y, S.view_m, 1));
    const int dim = days_in_month(S.view_y, S.view_m);
    const int prev_m = (S.view_m + 11) % 12;
    const int prev_y = S.view_m == 0 ? S.view_y - 1 : S.view_y;
    const int prev_dim = days_in_month(prev_y, prev_m);
    const bool tm = viewing_today_month();

    for (int i = 0; i < 42; i++) {
        int day, off;
        if (i < start) { day = prev_dim - start + 1 + i; off = -1; }
        else if (i - start < dim) { day = i - start + 1; off = 0; }
        else { day = i - start - dim + 1; off = +1; }
        U.cell_day[i] = day;
        U.cell_off[i] = off;

        snprintf(b, sizeof b, "%d", day);
        lv_label_set_text(U.cell_lbl[i], b);

        bool today = off == 0 && tm && day == S.today_d;
        bool sel = off == 0 && day == S.sel_d;
        bool weekend = (i % 7) >= 5;

        lv_color_t fg = C_OTHER;
        if (off == 0) fg = today ? (sel ? AOS_C_TEXT : C_RED) : sel ? lv_color_black() : weekend ? C_WEEKEND : AOS_C_TEXT;
        lv_obj_set_style_text_color(U.cell_lbl[i], fg, 0);

        lv_obj_t *circle = lv_obj_get_child(U.cell[i], 0);
        if (sel) {
            lv_obj_set_style_bg_color(circle, today ? C_RED : AOS_C_TEXT, 0);
            lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(circle, LV_OPA_TRANSP, 0);
        }
        /* the dot is for "this day has events" (events_fill) */
        lv_obj_add_flag(U.cell_dot[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* the ISO week of each row, read off its Monday */
    for (int r = 0; r < 6; r++) {
        if (!U.week_lbl[r]) continue;
        int i = r * 7, y = S.view_y, m = S.view_m;
        if (U.cell_off[i] < 0) { m = prev_m; y = prev_y; }
        else if (U.cell_off[i] > 0) { m = (S.view_m + 1) % 12; y = S.view_m == 11 ? S.view_y + 1 : S.view_y; }
        snprintf(b, sizeof b, "%d", iso_week(y, m, U.cell_day[i]));
        lv_label_set_text(U.week_lbl[r], b);
    }

    /* the shortcut to today only when today is not on screen */
    if (U.btn_today) {
        bool show = S.today_y >= 0 && !(tm && S.sel_d == S.today_d);
        lv_obj_set_style_opa(U.btn_today, show ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    if (U.next_box) {
        lv_obj_clean(U.next_box);
        int nm = (S.view_m + 1) % 12, ny = S.view_m == 11 ? S.view_y + 1 : S.view_y;
        mini_month(U.next_box, ny, nm, lv_obj_get_content_width(U.next_box),
                   lv_obj_get_content_height(U.next_box), aos_font_small, aos_font_caption, false);
    }
    panel_refresh();
}

static void go_month(int delta)
{
    int total = S.view_y * 12 + S.view_m + delta;
    int y = total / 12, m = total % 12;
    if (y < YEAR_MIN || y > YEAR_MAX) return;
    S.view_y = y;
    S.view_m = m;
    S.sel_d = default_sel();
    month_refresh();
}

static void go_today(void)
{
    read_today();
    if (S.today_y < 0) return;
    S.view_y = S.today_y;
    S.view_m = S.today_m;
    S.sel_d = S.today_d;
    month_refresh();
}

/* -------------------------------------------------------------------------- */
/* Year view                                                                   */
/* -------------------------------------------------------------------------- */

/* A month drawn small in a w x h box: its name, optionally the weekday
 * initials, and the numbers, no neighbours. Labels only, nothing clickable:
 * the tile around it takes the touch. */
static void mini_month(lv_obj_t *parent, int y, int m, int32_t w, int32_t h,
                       const lv_font_t *title_font, const lv_font_t *df, bool initials)
{
    const int32_t cw = w / 7;
    const bool cur = S.today_y == y && S.today_m == m;

    lv_obj_t *t = aos_label(parent, _(MES_LARGO[m]), title_font, cur ? C_RED : AOS_C_TEXT);
    lv_obj_set_pos(t, 4, 0);
    int32_t gy = lv_font_get_line_height(title_font) + 8;
    if (initials) {
        for (int c = 0; c < 7; c++) {
            lv_obj_t *l = aos_label_boxed(parent, C_(DIA_INICIAL_CTX[c], DIA_INICIAL[c]), aos_font_tiny,
                                          C_OTHER, cw, 20);
            lv_obj_set_pos(l, c * cw, gy);
        }
        gy += 26;
    }
    const int32_t rh = LV_MIN((h - gy) / 6, cw * 3 / 2);
    const int start = col_of(weekday(y, m, 1));
    const int dim = days_in_month(y, m);
    const int32_t dh = lv_font_get_line_height(df);
    const int32_t dd = LV_MIN(LV_MIN(rh, cw) - 2, dh + 8);   /* today's disc */
    char b[12];
    for (int d = 1; d <= dim; d++) {
        int i = start + d - 1;
        int32_t x = (i % 7) * cw, yy = gy + (i / 7) * rh;
        if (is_today(y, m, d)) {
            lv_obj_t *disc = lv_obj_create(parent);
            lv_obj_remove_style_all(disc);
            lv_obj_set_size(disc, dd, dd);
            lv_obj_set_pos(disc, x + (cw - dd) / 2, yy + (rh - dd) / 2);
            lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(disc, C_RED, 0);
            lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
        }
        snprintf(b, sizeof b, "%d", d);
        lv_obj_t *l = aos_label_boxed(parent, b, df, is_today(y, m, d) ? AOS_C_TEXT : (i % 7) >= 5 ? C_WEEKEND : AOS_C_TEXT,
                                      cw, dh);
        lv_obj_set_pos(l, x, yy + (rh - dh) / 2);
    }
    aos_make_decorative(parent);
}

static void tile_cb(lv_event_t *e);

static void year_refresh(void)
{
    if (!U.yv_grid) return;
    char b[12];
    snprintf(b, sizeof b, "%d", S.yv_year);
    lv_label_set_text(U.lbl_yv, b);
    lv_obj_set_style_text_color(U.lbl_yv, S.today_y == S.yv_year ? C_RED : AOS_C_TEXT, 0);

    lv_obj_clean(U.yv_grid);
    const int cols = U.land ? 6 : 3, rows = 12 / cols;
    const int32_t gap = U.land ? 16 : 20;
    const int32_t gw = lv_obj_get_content_width(U.yv_grid), gh = lv_obj_get_content_height(U.yv_grid);
    const int32_t tw = (gw - (cols - 1) * gap) / cols, th = (gh - (rows - 1) * gap) / rows;
    for (int m = 0; m < 12; m++) {
        lv_obj_t *tile = lv_obj_create(U.yv_grid);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, tw, th);
        lv_obj_set_pos(tile, (m % cols) * (tw + gap), (m / cols) * (th + gap));
        lv_obj_set_style_radius(tile, 22, 0);
        lv_obj_set_style_pad_all(tile, 12, 0);
        bool viewing = S.view_y == S.yv_year && S.view_m == m;
        lv_obj_set_style_bg_color(tile, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(tile, viewing ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(tile, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *inner = lv_obj_create(tile);
        lv_obj_remove_style_all(inner);
        lv_obj_set_size(inner, lv_pct(100), lv_pct(100));
        mini_month(inner, S.yv_year, m, tw - 24, th - 24, aos_font_body,
                   aos_font_tiny, false);

        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)m);
    }
}

static void year_view_set(bool open)
{
    S.year_open = open;
    if (!U.year_view) return;
    lv_obj_set_flag(U.year_view, LV_OBJ_FLAG_HIDDEN, !open);
    lv_obj_set_flag(U.month_view, LV_OBJ_FLAG_HIDDEN, open);
    if (open) year_refresh();
    else if (U.yv_grid) lv_obj_clean(U.yv_grid);     /* 400 labels we do not need */
}

static void go_year(int delta)
{
    int y = S.yv_year + delta;
    if (y < YEAR_MIN || y > YEAR_MAX) return;
    S.yv_year = y;
    year_refresh();
}

/* -------------------------------------------------------------------------- */
/* Events                                                                      */
/* -------------------------------------------------------------------------- */

static void prev_cb(lv_event_t *e)   { (void)e; aos_hal_activity(); go_month(-1); }
static void next_cb(lv_event_t *e)   { (void)e; aos_hal_activity(); go_month(+1); }
static void today_cb(lv_event_t *e)  { (void)e; aos_hal_activity(); go_today(); }
static void yprev_cb(lv_event_t *e)  { (void)e; aos_hal_activity(); go_year(-1); }
static void ynext_cb(lv_event_t *e)  { (void)e; aos_hal_activity(); go_year(+1); }

static void header_cb(lv_event_t *e)
{
    (void)e;
    aos_hal_activity();
    S.yv_year = S.view_y;
    year_view_set(true);
}

static void year_close_cb(lv_event_t *e) { (void)e; aos_hal_activity(); year_view_set(false); }

static void tile_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    aos_hal_activity();
    S.view_y = S.yv_year;
    S.view_m = m;
    S.sel_d = default_sel();
    year_view_set(false);
    month_refresh();
}

static void cell_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    aos_hal_activity();
    /* a day of the neighbouring month takes you to that month, selected */
    if (U.cell_off[i] != 0) {
        int d = U.cell_day[i];
        go_month(U.cell_off[i]);
        S.sel_d = d;
        month_refresh();
        return;
    }
    S.sel_d = U.cell_day[i];
    month_refresh();
}

static void swipe(lv_dir_t dir)
{
    uint32_t t = lv_tick_get();
    if (t - S.last_swipe_ms < 300) return;      /* LVGL's and the runtime's report the same swipe */
    S.last_swipe_ms = t;
    aos_hal_activity();
    int d = (dir == LV_DIR_TOP || dir == LV_DIR_LEFT) ? +1 : (dir == LV_DIR_BOTTOM || dir == LV_DIR_RIGHT) ? -1 : 0;
    if (!d) return;
    if (S.year_open) go_year(d);
    else go_month(d);
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    swipe(lv_indev_get_gesture_dir(indev));
    /* or the release clicks the cell the finger started on, which after the
     * page turn is a different day (or the next month again) */
    lv_indev_wait_release(indev);
}

/* -------------------------------------------------------------------------- */
/* Construction                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, 80, 80);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = aos_label(b, glyph, &aos_sym_44, C_RED);
    lv_obj_center(l);
    aos_make_decorative(l);
    return b;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 80);
    lv_obj_set_style_pad_hor(b, 30, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = aos_label(b, text, aos_font_body, C_RED);
    lv_obj_center(l);
    aos_make_decorative(l);
    return b;
}

/* The header: month + year on the left (tap = the year), today and the
 * chevrons on the right. */
static void build_header(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *hdr = box(parent);
    lv_obj_set_size(hdr, w, h);

    lv_obj_t *title = box(hdr);
    lv_obj_set_size(title, LV_SIZE_CONTENT, h);
    lv_obj_set_flex_flow(title, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(title, 14, 0);
    lv_obj_set_style_pad_bottom(title, U.land ? 8 : 12, 0);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(title, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(title, header_cb, LV_EVENT_CLICKED, NULL);
    U.lbl_month = aos_label(title, "", aos_font_large, C_RED);
    U.lbl_year = aos_label(title, "", aos_font_title, AOS_C_DIM);
    lv_obj_set_style_pad_bottom(U.lbl_year, 3, 0);
    aos_make_decorative(U.lbl_month);
    aos_make_decorative(U.lbl_year);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *right = box(hdr);
    lv_obj_set_size(right, LV_SIZE_CONTENT, 80);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 12, 0);
    U.btn_today = pill(right, _("Hoy"), today_cb);
    round_btn(right, AOS_SYM_CHEVRON_LEFT, prev_cb);
    round_btn(right, AOS_SYM_CHEVRON_RIGHT, next_cb);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* The grid: week numbers, the weekday initials and the 42 cells. */
static void build_grid(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, const lv_font_t *df)
{
    const int32_t wk = U.land ? 52 : 56;          /* the week-number column */
    const int32_t head = 44;
    const int32_t cw = (w - wk) / 7;
    const int32_t rh = (h - head) / 6;
    const int32_t x0 = x + wk + (w - wk - 7 * cw) / 2;

    lv_obj_t *wl = aos_label_boxed(parent, _("sem"), aos_font_tiny, C_OTHER, wk, 24);
    lv_obj_set_pos(wl, x, y + 8);
    for (int c = 0; c < 7; c++) {
        lv_obj_t *l = aos_label_boxed(parent, C_(DIA_INICIAL_CTX[c], DIA_INICIAL[c]), aos_font_caption,
                                      c >= 5 ? C_OTHER : AOS_C_DIM, cw, 28);
        lv_obj_set_pos(l, x0 + c * cw, y + 6);
    }

    const int32_t d = LV_MIN(LV_MIN(cw, rh) - 10, 80);   /* the circle */
    for (int r = 0; r < 6; r++) {
        lv_obj_t *line = box(parent);
        lv_obj_set_size(line, 7 * cw, 1);
        lv_obj_set_pos(line, x0, y + head + r * rh);
        lv_obj_set_style_bg_color(line, C_LINE, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

        U.week_lbl[r] = aos_label_boxed(parent, "", aos_font_tiny, C_OTHER, wk, 22);
        lv_obj_set_pos(U.week_lbl[r], x, y + head + r * rh + (rh - 22) / 2);
    }
    for (int i = 0; i < 42; i++) {
        lv_obj_t *cell = box(parent);
        lv_obj_set_size(cell, cw, rh);
        lv_obj_set_pos(cell, x0 + (i % 7) * cw, y + head + (i / 7) * rh);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        /* gestures over the grid reach the month view (see create) */
        lv_obj_add_flag(cell, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *circle = box(cell);
        lv_obj_set_size(circle, d, d);
        lv_obj_align(circle, LV_ALIGN_CENTER, 0, -4);
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(circle, LV_OPA_TRANSP, 0);

        lv_obj_t *lbl = aos_label(circle, "", df, AOS_C_TEXT);
        lv_obj_center(lbl);

        lv_obj_t *dot = box(cell);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, AOS_C_DIM, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

        aos_make_decorative(circle);
        aos_make_decorative(dot);
        lv_obj_set_style_bg_color(cell, AOS_C_TEXT, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_set_style_radius(cell, 18, 0);

        U.cell[i] = cell;
        U.cell_lbl[i] = lbl;
        U.cell_dot[i] = dot;
    }
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 24, 0);
    return c;
}

/* The selected day: its number big, the weekday, the date, how far from
 * today, and the events. */
static void build_panel(lv_obj_t *c)
{
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, U.land ? 8 : 14, 0);

    lv_obj_t *top = box(c);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 24, 0);

    U.p_num = aos_label(top, "", &aos_inter_num_96, AOS_C_TEXT);
    lv_obj_set_style_min_width(U.p_num, 120, 0);
    lv_obj_set_style_text_align(U.p_num, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *txt = box(top);
    lv_obj_set_height(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 2, 0);
    U.p_wday = aos_label(txt, "", aos_font_small, AOS_C_DIM);
    U.p_date = aos_label(txt, "", aos_font_body, AOS_C_TEXT);
    U.p_rel = aos_label(txt, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(U.p_date, lv_pct(100));
    lv_label_set_long_mode(U.p_date, LV_LABEL_LONG_MODE_DOTS);
    aos_make_decorative(top);

    U.p_meta = aos_label(c, "", aos_font_caption, AOS_C_DIM);

    lv_obj_t *sep = box(c);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, C_LINE, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    aos_label(c, _("Eventos"), aos_font_caption, AOS_C_DIM);
    U.p_events = box(c);
    lv_obj_set_width(U.p_events, lv_pct(100));
    lv_obj_set_flex_grow(U.p_events, 1);
    lv_obj_set_flex_flow(U.p_events, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.p_events, 12, 0);
    lv_obj_add_flag(U.p_events, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.p_events, LV_DIR_VER);
}

static void build_month_view(void)
{
    const int32_t pad = AOS_UI_PAD;
    lv_obj_t *v = box(U.page);
    lv_obj_set_size(v, U.W, U.H);
    lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v, gesture_cb, LV_EVENT_GESTURE, NULL);
    U.month_view = v;

    if (!U.land) {
        const int32_t hdr_h = 104;
        lv_obj_t *hdr_box = box(v);
        lv_obj_set_size(hdr_box, U.W - 2 * pad, hdr_h);
        lv_obj_set_pos(hdr_box, pad, 4);
        build_header(hdr_box, U.W - 2 * pad, hdr_h);

        /* the grid takes what the day panel leaves: that one needs ~330 */
        const int32_t gy = hdr_h + 8;
        const int32_t gh = LV_MIN(U.H - gy - 410, 44 + 6 * 112);
        build_grid(v, pad - 8, gy, U.W - 2 * pad + 8, gh, aos_font_title);

        lv_obj_t *c = card(v);
        const int32_t cy = gy + gh + 16;
        lv_obj_set_size(c, U.W - 2 * pad, U.H - cy - pad);
        lv_obj_set_pos(c, pad, cy);
        build_panel(c);
    } else {
        const int32_t side = 440;
        const int32_t lw = U.W - 3 * pad - side;
        const int32_t hdr_h = 92;
        lv_obj_t *hdr_box = box(v);
        lv_obj_set_size(hdr_box, lw, hdr_h);
        lv_obj_set_pos(hdr_box, pad, 0);
        build_header(hdr_box, lw, hdr_h);
        build_grid(v, pad - 8, hdr_h + 4, lw + 8, U.H - hdr_h - 4 - pad, aos_font_body);

        /* the side: the day on top, next month underneath */
        const int32_t sx = 2 * pad + lw;
        const int32_t next_h = 236;
        lv_obj_t *c = card(v);
        lv_obj_set_size(c, side, U.H - 2 * pad - next_h - 16);
        lv_obj_set_pos(c, sx, pad / 2);
        build_panel(c);

        lv_obj_t *n = card(v);
        lv_obj_set_size(n, side, next_h);
        lv_obj_set_pos(n, sx, U.H - pad - next_h + pad / 2);
        lv_obj_set_style_pad_all(n, 20, 0);
        lv_obj_set_style_pad_hor(n, 28, 0);
        lv_obj_add_flag(n, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(n, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(n, next_cb, LV_EVENT_CLICKED, NULL);
        U.next_box = box(n);
        lv_obj_set_size(U.next_box, lv_pct(100), lv_pct(100));
        lv_obj_update_layout(n);
    }
}

static void build_year_view(void)
{
    const int32_t pad = AOS_UI_PAD;
    lv_obj_t *v = box(U.page);
    lv_obj_set_size(v, U.W, U.H);
    lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(v, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    U.year_view = v;

    const int32_t hdr_h = U.land ? 92 : 104;
    lv_obj_t *hdr = box(v);
    lv_obj_set_size(hdr, U.W - 2 * pad, hdr_h);
    lv_obj_set_pos(hdr, pad, U.land ? 0 : 4);

    U.lbl_yv = aos_label(hdr, "", aos_font_large, AOS_C_TEXT);
    lv_obj_align(U.lbl_yv, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *right = box(hdr);
    lv_obj_set_size(right, LV_SIZE_CONTENT, 80);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 12, 0);
    pill(right, _("Volver al mes"), year_close_cb);
    round_btn(right, AOS_SYM_CHEVRON_LEFT, yprev_cb);
    round_btn(right, AOS_SYM_CHEVRON_RIGHT, ynext_cb);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);

    U.yv_grid = box(v);
    lv_obj_set_size(U.yv_grid, U.W - 2 * pad, U.H - hdr_h - 16 - pad);
    lv_obj_set_pos(U.yv_grid, pad, hdr_h + 12);
    lv_obj_add_flag(U.yv_grid, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void build(lv_obj_t *root)
{
    lv_obj_clean(root);
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.page = aos_page(root);

    build_month_view();
    build_year_view();
    lv_obj_update_layout(U.page);
    month_refresh();
    year_view_set(S.year_open);
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    read_today();
    if (!S.started) {
        S.started = true;
        if (S.today_y >= YEAR_MIN && S.today_y <= YEAR_MAX) {
            S.view_y = S.today_y;
            S.view_m = S.today_m;
        } else {                                /* clock not set: something has to show */
            struct tm now;
            aos_hal_time_now(&now);
            S.view_y = LV_CLAMP(YEAR_MIN, now.tm_year + 1900, YEAR_MAX);
            S.view_m = now.tm_mon;
        }
        S.sel_d = default_sel();
    }
    S.last_check_ms = lv_tick_get();
    build(root);
    return &U;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self; (void)inst;
    build(root);
    return true;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    /* the objects go with the context still standing, so no event of the
     * deletion reaches a zeroed U */
    if (self && self->root) lv_obj_clean(self->root);
    memset(&U, 0, sizeof U);
}

static void show(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    int y = S.today_y, m = S.today_m, d = S.today_d;
    read_today();
    if (y != S.today_y || m != S.today_m || d != S.today_d) month_refresh();
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (S.year_open) {
        year_view_set(false);
        return true;
    }
    return false;
}

static void tick(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    /* at midnight, today moves to another cell; once a second is enough */
    uint32_t t = lv_tick_get();
    if (t - S.last_check_ms < 1000) return;
    S.last_check_ms = t;
    int y = S.today_y, m = S.today_m, d = S.today_d;
    read_today();
    if (y != S.today_y || m != S.today_m || d != S.today_d) {
        month_refresh();
        if (S.year_open) year_refresh();
    }
}

void aos_app_calendar_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id       = "aos.calendar",
            .name     = "Calendario",
            .icon     = AOS_SYM_CALENDAR,
            .icon_vec = AOS_ICON_CALENDAR,
            .color_a  = 0xF87171,
            .color_b  = 0xDC2626,
            .flags    = AOS_APP_FLAG_KEEP,
            .order    = 280,
        },
        .create  = create,
        .destroy = destroy,
        .show    = show,
        .back    = back,
        .tick    = tick,
        .resize  = resize,
    };
}
