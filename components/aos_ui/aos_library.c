/*
 * P4OS - the App Library: the home screen's last page (aos_home.c).
 *
 * Every app on the board, the ones taken off the home screen too, grouped
 * by category (the Store's: games, sound and picture, network...), a card
 * for each: its first three apps big, to open with a tap, and the rest as a
 * cluster of small ones that opens the whole category as a sheet. A search
 * pill at the top opens the system's search. The categories come from the
 * services above the shell (aos_ui_set_app_category: the catalogue, and a
 * table for the built-in apps).
 *
 * Its icons are pictures of an icon pack (57 KB each in PSRAM): the page is
 * filled only while it is the one on screen and emptied when the home
 * screen moves to another (aos_home.c calls aos_library_fill and
 * aos_library_drop), so it costs nothing the rest of the time.
 */
#include "aos_ui.h"
#include "aos_internal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_fonts.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CARD_GAP   24
#define CARD_PAD   20
#define ICON_GAP   18
#define MAX_LIB    160              /* apps listed */

/* The Store's categories (store/source.json), in the library's order. */
static const struct { const char *id, *name; } CATS[] = {
    { "juegos", N_("Juegos") }, { "medios", N_("Sonido e imagen") }, { "red", N_("Red") },
    { "taller", N_("Taller") }, { "crear", N_("Crear") }, { "programar", N_("Programar") },
    { "utiles", N_("Útiles") }, { "sistema", N_("Sistema") },
};
#define NCATS ((int)(sizeof CATS / sizeof CATS[0]))

static aos_app_category_fn s_cat_fn;
static lv_obj_t *s_page, *s_body, *s_sheet;
static int32_t s_w;

void aos_ui_set_app_category(aos_app_category_fn fn) { s_cat_fn = fn; }

bool aos_library_on(void) { return s_cat_fn != NULL; }

static int cat_of(const aos_app_t *a)
{
    const char *c = s_cat_fn ? s_cat_fn(a->desc.id) : NULL;
    for (int i = 0; c && i < NCATS; i++)
        if (!strcmp(CATS[i].id, c)) return i;
    return NCATS - 2;               /* "Útiles": what nobody placed */
}

/* Each registry slot's category, worked out once when the page fills: the
 * Store's answer walks its whole list, and 8 cards would ask 8 times. */
static uint8_t s_cat[AOS_MAX_APPS];
static int s_ncat_apps;

static void cats_compute(void)
{
    s_ncat_apps = LV_MIN(aos_ui_app_count(), AOS_MAX_APPS);
    for (int i = 0; i < s_ncat_apps; i++) {
        const aos_app_t *a = aos_ui_app_at(i);
        s_cat[i] = (uint8_t)(a && a->desc.id ? cat_of(a) : NCATS);
    }
}

static int by_name(const void *x, const void *y)
{
    const aos_app_t *a = *(const aos_app_t *const *)x, *b = *(const aos_app_t *const *)y;
    return strcasecmp(aos_tr(a->desc.name), aos_tr(b->desc.name));
}

/* The apps of category 'c', by name; how many. */
static int apps_of(int c, const aos_app_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < s_ncat_apps && n < max; i++) {
        const aos_app_t *a = aos_ui_app_at(i);
        if (a && a->desc.id && a->create && s_cat[i] == c) out[n++] = a;
    }
    qsort(out, (size_t)n, sizeof out[0], by_name);
    return n;
}

static void open_cb(lv_event_t *e)
{
    const aos_app_t *a = lv_event_get_user_data(e);
    aos_hal_activity();
    aos_library_sheet_close();
    if (a && a->desc.id) aos_ui_open(a->desc.id);
}

/* An app's icon that opens it; it lets the page's swipes through. */
static lv_obj_t *app_icon(lv_obj_t *parent, const aos_app_t *a, int32_t size)
{
    lv_obj_t *ic = aos_home_app_icon(parent, a, size);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_opa(ic, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ic, open_cb, LV_EVENT_SHORT_CLICKED, (void *)a);
    return ic;
}

/* ---- a category's sheet: all its apps with their names ---- */

void aos_library_sheet_close(void)
{
    if (s_sheet) lv_obj_delete(s_sheet);
    s_sheet = NULL;
}

bool aos_library_sheet_open(void) { return s_sheet != NULL; }

static void sheet_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) aos_library_sheet_close();
}

static void sheet_open(int c)
{
    aos_library_sheet_close();
    const aos_geo_t *g = aos_ui_geo();
    static const aos_app_t *apps[MAX_LIB];
    int n = apps_of(c, apps, MAX_LIB);
    s_sheet = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sheet);
    lv_obj_set_size(s_sheet, g->w, g->h);
    lv_obj_set_style_bg_color(s_sheet, lv_color_hex(0x0B0D14), 0);
    lv_obj_set_style_bg_opa(s_sheet, 245, 0);         /* nearly whole: the page under it must not read through the title */
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sheet, sheet_bg_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = aos_label(s_sheet, _(CATS[c].name), aos_font_title, lv_color_white());
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, g->bar_h + 30);

    int cols = g->landscape ? 7 : 4;
    int32_t side = g->landscape ? 40 : 24, cw = (g->w - 2 * side) / cols, icon = g->landscape ? AOS_UI_ICON_LAND : AOS_UI_ICON;
    lv_obj_t *grid = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, g->w - 2 * side, g->h - g->bar_h - 120);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, g->bar_h + 100);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 26, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < n; i++) {
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cw, icon + 46);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_set_style_opa(cell, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_event_cb(cell, open_cb, LV_EVENT_SHORT_CLICKED, (void *)apps[i]);
        lv_obj_t *ic = aos_home_app_icon(cell, apps[i], icon);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_t *l = aos_label(cell, aos_tr(apps[i]->desc.name), aos_font_label, lv_color_white());
        lv_obj_set_width(l, cw - 8);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, icon + 10);
    }
}

static void cluster_cb(lv_event_t *e)
{
    aos_hal_activity();
    sheet_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void search_cb(lv_event_t *e)
{
    (void)e;
    aos_hal_activity();
    aos_search_open();
}

/* ---- the page ---- */

void aos_library_create(lv_obj_t *page, int32_t w)
{
    s_page = page;
    s_body = NULL;
    s_w = w;
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
}

void aos_library_forget(void)
{
    s_page = s_body = NULL;
}

void aos_library_drop(void)
{
    if (s_body) lv_obj_delete(s_body);
    s_body = NULL;
    if (s_page) lv_obj_scroll_to_y(s_page, 0, LV_ANIM_OFF);
}

/* One category's card: three apps big and the rest small, two by two, in
 * the fourth corner (a fourth app big when there are only four). */
static void card(lv_obj_t *parent, int c, int32_t cw)
{
    static const aos_app_t *apps[MAX_LIB];
    int n = apps_of(c, apps, MAX_LIB);
    if (!n) return;
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, cw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *box = lv_obj_create(col);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, cw, cw);
    lv_obj_set_style_radius(box, cw * 14 / 100, 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_20, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);

    int32_t big = (cw - 2 * CARD_PAD - ICON_GAP) / 2;
    int32_t xy[4][2] = { { CARD_PAD, CARD_PAD }, { CARD_PAD + big + ICON_GAP, CARD_PAD },
                         { CARD_PAD, CARD_PAD + big + ICON_GAP }, { CARD_PAD + big + ICON_GAP, CARD_PAD + big + ICON_GAP } };
    int bigs = n <= 4 ? n : 3;
    for (int i = 0; i < bigs; i++) {
        lv_obj_t *ic = app_icon(box, apps[i], big);
        lv_obj_set_pos(ic, xy[i][0], xy[i][1]);
    }
    if (n > 4) {
        /* the cluster: the next four, small; a tap opens the whole category */
        lv_obj_t *cl = lv_obj_create(box);
        lv_obj_remove_style_all(cl);
        lv_obj_set_size(cl, big, big);
        lv_obj_set_pos(cl, xy[3][0], xy[3][1]);
        lv_obj_remove_flag(cl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(cl, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(cl, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_set_style_opa(cl, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_event_cb(cl, cluster_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)c);
        int32_t small = (big - 10) / 2;
        for (int k = 0; k < 4 && 3 + k < n; k++) {
            lv_obj_t *ic = aos_home_app_icon(cl, apps[3 + k], small);
            lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(ic, (k % 2) * (small + 10), (k / 2) * (small + 10));
        }
    }

    lv_obj_t *l = aos_label(col, _(CATS[c].name), aos_font_label, lv_color_white());
    lv_obj_set_width(l, cw);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

void aos_library_fill(void)
{
    if (!s_page || s_body) return;
    cats_compute();
    const aos_geo_t *g = aos_ui_geo();
    int32_t side = g->landscape ? 40 : 24;
    int cols = g->landscape ? 4 : 2;
    int32_t inner = s_w - 2 * side, cw = (inner - (cols - 1) * CARD_GAP) / cols;

    s_body = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, s_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_body, side, 0);
    lv_obj_set_style_pad_bottom(s_body, 40, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 26, 0);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* the search pill */
    lv_obj_t *pill = lv_obj_create(s_body);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, inner, 68);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 12, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(pill, search_cb, LV_EVENT_SHORT_CLICKED, NULL);
    aos_label(pill, AOS_SYM_MAGNIFY, &aos_sym_28, lv_color_hex(0xD8DCE6));
    aos_label(pill, _("Biblioteca de apps"), aos_font_body, lv_color_hex(0xD8DCE6));

    lv_obj_t *grid = lv_obj_create(s_body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, inner, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, CARD_GAP, 0);
    lv_obj_set_style_pad_row(grid, 28, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int c = 0; c < NCATS; c++) card(grid, c, cw);
}
