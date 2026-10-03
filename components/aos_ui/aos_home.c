/*
 * P4OS - Home screen.
 *
 *     status bar
 *     pages of icons      swipe sideways; the same ordered list fills a 4x6
 *                         grid upright and a 7x3 grid lying down, so turning
 *                         the screen never scrambles anything (UI.md)
 *     page dots
 *     dock                4 apps upright, 6 lying down, on every page
 *
 * The model is menu.txt (aos_menu.c, extended with dock / page / widget).
 * Folders open over the home screen as a panel with the apps inside, like
 * iOS. Edit mode (a long press) wiggles the icons; dragging one onto a slot
 * moves it, onto another app makes a folder with both, and "Listo" writes
 * menu.txt back.
 *
 * Costs, from the watch: what an animation pays for is the area it pushes,
 * so the wiggle moves the icons a few pixels and nothing is scaled or faded
 * as a whole page.
 */
#include "aos_internal.h"
#include "aos_hal.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_menu.h"
#include "aos_icon_ops.h"
#include "aos_folder_glyphs.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    int32_t cols, rows, icon, cell_w, cell_h, side;
    int32_t grid_y, grid_h;
    int32_t dots_y;
    int32_t dock_y, dock_h, dock_n;
} layout_t;

static layout_t L;

static void layout_compute(void)
{
    const aos_geo_t *g = aos_ui_geo();
    if (!g->landscape) {
        L.cols = 4; L.rows = 6; L.icon = AOS_UI_ICON; L.side = 24; L.dock_n = 4;
        L.dock_h = 156;
        L.dock_y = g->h - g->bottom_h - L.dock_h - 8;
        L.dots_y = L.dock_y - 40;
        L.grid_y = g->bar_h + 20;
        L.grid_h = L.dots_y - L.grid_y - 4;
    } else {
        L.cols = 7; L.rows = 3; L.icon = AOS_UI_ICON_LAND; L.side = 40; L.dock_n = 6;
        L.dock_h = 136;
        L.dock_y = g->h - g->bottom_h - L.dock_h - 4;
        L.dots_y = L.dock_y - 30;
        L.grid_y = g->bar_h + 8;
        L.grid_h = L.dots_y - L.grid_y - 2;
    }
    L.cell_w = (g->w - 2 * L.side) / L.cols;
    L.cell_h = L.grid_h / L.rows;
}

/* -------------------------------------------------------------------------- */
/* State                                                                       */
/* -------------------------------------------------------------------------- */

#define MAX_ITEMS  (AOS_MAX_APPS + AOS_MENU_FOLDERS_MAX + 32)
#define MAX_PAGES  16

/* One placed item: what it is and where it landed. */
typedef struct {
    aos_menu_item_t it;
    int page, row, col, w, h;
    lv_obj_t *cell;
    lv_obj_t *badge;
} placed_t;

static placed_t *s_items;           /* PSRAM on the board: ~300 entries */
static int s_nitems;
static const aos_app_t *s_dock[AOS_MENU_DOCK_MAX];
static lv_obj_t *s_dock_cells[AOS_MENU_DOCK_MAX];
static lv_obj_t *s_dock_badges[AOS_MENU_DOCK_MAX];
static int s_ndock;
static int s_npages;

static lv_obj_t *s_root, *s_pages, *s_dots, *s_dock_panel, *s_done_btn;
static lv_obj_t *s_page_obj[MAX_PAGES];
static int s_page;                  /* page shown */

static lv_obj_t *s_folder_ov;       /* the open folder, or NULL */
static int s_folder_open = -1;
static bool s_folder_editing;       /* the open folder shows its remove badges and a name to edit */

/* One change to an existing folder, applied by save_model() while it writes
 * that folder, then forgotten. The home's model only knows a folder by its
 * index in menu.txt; its contents and name come from aos_menu. */
static struct {
    int  folder;                    /* -1: none */
    char add[48];                   /* an app id to append */
    char remove[48];                /* an app id to take out */
    char name[AOS_MENU_NAME_MAX];   /* a new name */
} s_patch = { .folder = -1 };
static char s_reopen[AOS_MENU_FOLDER_ID_MAX];   /* the folder to show again after saving */
static bool s_editing;

/* dragging in edit mode */
static struct {
    placed_t *p;                    /* the item under the finger */
    lv_obj_t *ghost;
    bool moving;
    lv_point_t start;
} s_drag;

static void build(void);
static void folder_open(int folder, lv_obj_t *from);
static void folder_close(bool animate);
static void save_model(void);
static void dots_refresh(void);

/* -------------------------------------------------------------------------- */
/* Icons                                                                       */
/* -------------------------------------------------------------------------- */

lv_obj_t *aos_home_app_icon(lv_obj_t *parent, const aos_app_t *app, int32_t size)
{
    return aos_icon_create(parent, &app->desc, size);
}

static lv_obj_t *badge_create(lv_obj_t *parent, int count, int32_t icon_x2, int32_t icon_y1)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, AOS_C_RED, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_height(b, 40);
    lv_obj_set_style_min_width(b, 40, 0);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, aos_font_caption, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text_fmt(l, count > 99 ? "99+" : "%d", count);
    lv_obj_center(l);
    lv_obj_update_layout(b);
    lv_obj_set_pos(b, icon_x2 - lv_obj_get_width(b) + 12, icon_y1 - 12);
    lv_obj_set_flag(b, LV_OBJ_FLAG_HIDDEN, count <= 0);
    return b;
}

/* The folder's icon: a frosted squircle with up to nine of its apps in
 * miniature (or its glyph while it is empty). */
static lv_obj_t *folder_icon(lv_obj_t *parent, int folder, int32_t size)
{
    const aos_menu_folder_t *f = aos_menu_folder(folder);
    lv_obj_t *base = lv_obj_create(parent);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, size, size);
    lv_obj_set_style_radius(base, size * 225 / 1000, 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(base, lv_color_white(), 0);
    lv_obj_remove_flag(base, LV_OBJ_FLAG_SCROLLABLE);

    const aos_app_t *apps[9];
    int n = aos_menu_folder_apps(folder, apps, 9);
    if (n == 0) {
        lv_obj_t *g = lv_label_create(base);
        lv_obj_set_style_text_font(g, &aos_folder_font, 0);
        lv_obj_set_style_text_color(g, lv_color_white(), 0);
        uint32_t cp = aos_folder_glyph_codepoint(f ? f->glyph : "folder");
        char utf8[5] = { 0 };   /* MDI lives in plane 15: always four bytes */
        utf8[0] = (char)(0xF0 | (cp >> 18));
        utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (cp & 0x3F));
        lv_label_set_text(g, utf8);
        lv_obj_center(g);
    } else {
        int32_t pad = size / 8, gap = size / 24;
        int32_t mini = (size - 2 * pad - 2 * gap) / 3;
        for (int i = 0; i < n; i++) {
            lv_obj_t *m = aos_home_app_icon(base, apps[i], mini);
            lv_obj_set_pos(m, pad + (i % 3) * (mini + gap), pad + (i / 3) * (mini + gap));
        }
    }
    aos_make_decorative(base);
    return base;
}

/* -------------------------------------------------------------------------- */
/* Events                                                                      */
/* -------------------------------------------------------------------------- */

static void icon_area(lv_obj_t *cell, lv_area_t *out)
{
    lv_obj_t *icon = lv_obj_get_child(cell, 0);
    lv_obj_get_coords(icon ? icon : cell, out);
}

static void wiggle_cb(void *var, int32_t v) { lv_obj_set_style_translate_x(var, v, 0); }

static void wiggle(lv_obj_t *cell, bool on, int phase)
{
    lv_anim_delete(cell, wiggle_cb);
    lv_obj_set_style_translate_x(cell, 0, 0);
    if (!on) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cell);
    lv_anim_set_exec_cb(&a, wiggle_cb);
    lv_anim_set_values(&a, -3, 3);
    lv_anim_set_duration(&a, 130);
    lv_anim_set_playback_duration(&a, 130);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_delay(&a, (uint32_t)(phase * 37 % 130));
    lv_anim_start(&a);
}

static void open_app(const aos_app_t *app, lv_obj_t *cell)
{
    lv_area_t r;
    icon_area(cell, &r);
    aos_ui_set_launch_rect(&r);
    aos_ui_open(app->desc.id);
}

/* Where the finger is, as a slot of the page on screen. */
static void slot_at(lv_point_t p, int *row, int *col)
{
    int32_t x = p.x - L.side, y = p.y - L.grid_y;
    *col = LV_CLAMP(0, x / L.cell_w, L.cols - 1);
    *row = LV_CLAMP(0, y / L.cell_h, L.rows - 1);
}

static placed_t *item_at_slot(int page, int row, int col)
{
    for (int i = 0; i < s_nitems; i++) {
        placed_t *p = &s_items[i];
        if (p->page != page || p->it.kind == AOS_MENU_ITEM_PAGE) continue;
        if (row >= p->row && row < p->row + p->h && col >= p->col && col < p->col + p->w) return p;
    }
    return NULL;
}

static void drag_drop(placed_t *src, lv_point_t pt)
{
    int row, col;
    slot_at(pt, &row, &col);
    placed_t *dst = item_at_slot(s_page, row, col);
    if (dst == src) return;

    int si = (int)(src - s_items);
    aos_menu_item_t moved = src->it;

    /* an app onto a folder: into it, at the end */
    if (dst && dst->it.kind == AOS_MENU_ITEM_FOLDER && dst->it.folder < 1000 && moved.kind == AOS_MENU_ITEM_APP &&
        moved.app && dst->cell) {
        lv_area_t a;
        icon_area(dst->cell, &a);
        int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
        if (LV_ABS(pt.x - cx) < L.icon / 2 && LV_ABS(pt.y - cy) < L.icon / 2) {
            s_patch.folder = dst->it.folder;
            snprintf(s_patch.add, sizeof s_patch.add, "%s", moved.app->desc.id);
            src->it.kind = AOS_MENU_ITEM_PAGE;  /* a hole, dropped below */
            src->it.w = 255;
            save_model();
            return;
        }
    }

    /* onto another app: a folder with the two (only for two plain apps) */
    if (dst && dst->it.kind == AOS_MENU_ITEM_APP && moved.kind == AOS_MENU_ITEM_APP && dst->cell) {
        lv_area_t a;
        icon_area(dst->cell, &a);
        int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
        if (LV_ABS(pt.x - cx) < L.icon / 3 && LV_ABS(pt.y - cy) < L.icon / 3) {
            /* mark: dst becomes a new folder holding dst and src */
            dst->it.kind = AOS_MENU_ITEM_FOLDER;
            dst->it.folder = 1000 + si;        /* resolved by save_model() */
            dst->it.widget[0] = 0;
            snprintf(dst->it.arg, sizeof dst->it.arg, "%s\n%s", dst->it.app->desc.id, moved.app->desc.id);
            src->it.kind = AOS_MENU_ITEM_PAGE; /* a hole, dropped below */
            src->it.w = 255;
            save_model();
            return;
        }
    }

    /* onto a slot: move the item there (before whatever holds it) */
    int di = dst ? (int)(dst - s_items) : -1;
    if (di < 0) {
        /* an empty slot: after the last item of that page */
        for (int i = 0; i < s_nitems; i++) if (s_items[i].page == s_page) di = i + 1;
        if (di < 0) di = s_nitems;
    }
    placed_t tmp = *src;
    if (di > si) {
        memmove(&s_items[si], &s_items[si + 1], (size_t)(di - si - 1) * sizeof(placed_t));
        s_items[di - 1] = tmp;
    } else {
        memmove(&s_items[di + 1], &s_items[di], (size_t)(si - di) * sizeof(placed_t));
        s_items[di] = tmp;
    }
    save_model();
}

static lv_point_t s_press_pt;

static void cell_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *cell = lv_event_get_current_target(e);
    placed_t *p = lv_event_get_user_data(e);
    const aos_app_t *app = p ? p->it.app : (const aos_app_t *)lv_obj_get_user_data(cell);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(lv_indev_active(), &s_press_pt);
        /* in edit mode a drag starts right away, as on iOS */
        if (s_editing && p) { s_drag.p = p; s_drag.moving = false; s_drag.start = s_press_pt; }
        return;
    }
    if (code == LV_EVENT_PRESS_LOST && s_drag.p == p && p && s_drag.moving) {
        code = LV_EVENT_RELEASED;       /* finish the drop anyway */
    }
    if (code == LV_EVENT_LONG_PRESSED) {
        /* entering edit mode rebuilds the page: the icon under the finger is
         * a new object, so the drag starts with the next touch */
        if (!s_editing) aos_home_edit(true);
        return;
    }
    if (code == LV_EVENT_PRESSING && s_editing && s_drag.p == p && p) {
        lv_point_t pt;
        lv_indev_get_point(lv_indev_active(), &pt);
        if (!s_drag.moving && (LV_ABS(pt.x - s_drag.start.x) > 12 || LV_ABS(pt.y - s_drag.start.y) > 12)) {
            s_drag.moving = true;
            /* transparent, not HIDDEN: hiding the pressed object makes LVGL
             * reset the input device and the drop never arrives */
            lv_obj_set_style_opa(cell, LV_OPA_TRANSP, 0);
            s_drag.ghost = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(s_drag.ghost);
            lv_obj_set_size(s_drag.ghost, L.icon, L.icon);
            lv_obj_remove_flag(s_drag.ghost, LV_OBJ_FLAG_CLICKABLE);
            if (p->it.kind == AOS_MENU_ITEM_FOLDER) folder_icon(s_drag.ghost, p->it.folder, L.icon);
            else if (p->it.app) aos_home_app_icon(s_drag.ghost, p->it.app, L.icon);
            lv_obj_set_style_opa(s_drag.ghost, LV_OPA_80, 0);
        }
        if (s_drag.moving) {
            lv_obj_set_pos(s_drag.ghost, pt.x - L.icon / 2, pt.y - L.icon / 2);
            /* held against a side edge: the next page slides in */
            static uint32_t edge_since;
            const aos_geo_t *g = aos_ui_geo();
            bool left = pt.x < 40 && s_page > 0, right = pt.x > g->w - 40 && s_page < s_npages - 1;
            if (left || right) {
                if (!edge_since) edge_since = lv_tick_get();
                else if (lv_tick_elaps(edge_since) > 600) {
                    s_page += left ? -1 : 1;
                    lv_obj_scroll_to_x(s_pages, s_page * g->w, LV_ANIM_ON);
                    dots_refresh();
                    edge_since = 0;
                }
            } else {
                edge_since = 0;
            }
        }
        return;
    }
    if (code == LV_EVENT_RELEASED && s_drag.p == p && p) {
        if (s_drag.moving) {
            lv_point_t pt;
            lv_indev_get_point(lv_indev_active(), &pt);
            lv_obj_delete(s_drag.ghost);
            s_drag.ghost = NULL;
            s_drag.p = NULL;
            drag_drop(p, pt);       /* rebuilds the page */
            return;
        }
        s_drag.p = NULL;
        return;
    }
    if (code != LV_EVENT_SHORT_CLICKED || s_editing) return;
    /* a swipe that started on an icon is not a tap on it */
    lv_point_t now;
    lv_indev_get_point(lv_indev_active(), &now);
    if (LV_ABS(now.x - s_press_pt.x) > 24 || LV_ABS(now.y - s_press_pt.y) > 24) return;
    if (p && p->it.kind == AOS_MENU_ITEM_FOLDER) { folder_open(p->it.folder, cell); return; }
    if (app) open_app(app, cell);
}

/* -------------------------------------------------------------------------- */
/* Building                                                                    */
/* -------------------------------------------------------------------------- */

static lv_obj_t *cell_create(lv_obj_t *parent, int32_t w, int32_t h, bool label_on,
                             const char *name, placed_t *p, const aos_app_t *app, int folder)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, w, h);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);    /* page swipes still scroll */
    /* Sliding off an icon is not a tap on it - except while editing, when
     * the icon has to stay with the finger that drags it. */
    if (!s_editing) lv_obj_remove_flag(cell, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_opa(cell, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_user_data(cell, (void *)app);

    lv_obj_t *icon = folder >= 0 ? folder_icon(cell, folder, L.icon) : aos_home_app_icon(cell, app, L.icon);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, label_on ? 6 : (h - L.icon) / 2);

    if (label_on) {
        lv_obj_t *l = lv_label_create(cell);
        lv_obj_set_width(l, w - 8);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, aos_font_label, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, name);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 6 + L.icon + 8);
    }
    lv_obj_add_event_cb(cell, cell_event, LV_EVENT_ALL, p);
    return cell;
}

/* -------------------------------------------------------------------------- */
/* Widgets                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct { lv_obj_t *time, *date; } w_clock_t;

static lv_obj_t *widget_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS + 6, 0);
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(c, 22, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    return c;
}

static void clock_refresh(lv_obj_t *card)
{
    w_clock_t *w = lv_obj_get_user_data(card);
    if (!w) return;
    struct tm t;
    aos_hal_time_now(&t);
    if (aos_hal_time_is_valid()) lv_label_set_text_fmt(w->time, "%d:%02d", t.tm_hour, t.tm_min);
    else lv_label_set_text(w->time, "--:--");
    lv_label_set_text_fmt(w->date, "%s %d %s", aos_day_name(t.tm_wday), t.tm_mday, aos_month_name(t.tm_mon));
}

static void widget_deleted(lv_event_t *e) { free(lv_obj_get_user_data(lv_event_get_current_target(e))); }

/* Widget types registered from outside the shell (the HA widget lives with
 * the HA service in aos_apps, which the shell cannot depend on). */
#define EXT_WIDGETS 8
static struct { char type[16]; aos_widget_create_t create; } s_ext[EXT_WIDGETS];

void aos_ui_register_widget(const char *type, aos_widget_create_t create)
{
    for (int i = 0; i < EXT_WIDGETS; i++)
        if (!s_ext[i].create || !strcmp(s_ext[i].type, type)) {
            snprintf(s_ext[i].type, sizeof s_ext[i].type, "%s", type);
            s_ext[i].create = create;
            return;
        }
}

aos_widget_create_t aos_ui_widget_find(const char *type)
{
    for (int i = 0; i < EXT_WIDGETS && s_ext[i].create; i++)
        if (!strcmp(s_ext[i].type, type)) return s_ext[i].create;
    return NULL;
}

static lv_obj_t *widget_create(lv_obj_t *parent, const aos_menu_item_t *it, int32_t w, int32_t h)
{
    lv_obj_t *c = widget_card(parent, w, h);
    for (int i = 0; i < EXT_WIDGETS && s_ext[i].create; i++)
        if (!strcmp(s_ext[i].type, it->widget)) {
            s_ext[i].create(c, w, h, it->arg);
            return c;
        }
    if (!strcmp(it->widget, "clock")) {
        w_clock_t *wc = calloc(1, sizeof *wc);
        lv_obj_set_user_data(c, wc);
        lv_obj_add_event_cb(c, widget_deleted, LV_EVENT_DELETE, NULL);
        wc->time = lv_label_create(c);
        lv_obj_set_style_text_font(wc->time, aos_font_huge, 0);
        lv_obj_set_style_text_color(wc->time, lv_color_white(), 0);
        lv_obj_align(wc->time, LV_ALIGN_TOP_LEFT, 0, 0);
        wc->date = lv_label_create(c);
        lv_obj_set_style_text_font(wc->date, aos_font_body, 0);
        lv_obj_set_style_text_color(wc->date, lv_color_hex(0xDDE6F0), 0);
        lv_obj_align(wc->date, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        clock_refresh(c);
        lv_obj_add_flag(c, LV_OBJ_FLAG_USER_1);        /* ticks once a second */
    } else {
        /* a widget type the firmware does not know (yet): say so */
        lv_obj_t *g = lv_label_create(c);
        lv_obj_set_style_text_font(g, &aos_sym_44, 0);
        lv_obj_set_style_text_color(g, lv_color_white(), 0);
        lv_label_set_text(g, !strcmp(it->widget, "ha") ? AOS_SYM_HOME_ASSISTANT :
                             !strcmp(it->widget, "weather") ? AOS_SYM_WEATHER_PARTLY_CLOUDY : AOS_SYM_VIEW_GRID_OUTLINE);
        lv_obj_align(g, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *l = lv_label_create(c);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xDDE6F0), 0);
        lv_obj_set_width(l, w - 44);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(l, !strcmp(it->widget, "ha") ? _("Home Assistant: configurar en Ajustes")
                             : !strcmp(it->widget, "weather") ? _("Clima: próximamente") : it->widget);
        lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    return c;
}

/* -------------------------------------------------------------------------- */
/* Placing                                                                     */
/* -------------------------------------------------------------------------- */

static void default_dock(void)
{
    static const char *pref[] = { "aos.settings", "aos.files", "aos.music", "aos.calc",
                                  "aos.clock", "aos.photos" };
    s_ndock = 0;
    for (int i = 0; i < (int)(sizeof pref / sizeof pref[0]) && s_ndock < L.dock_n; i++) {
        const aos_app_t *a = aos_ui_app_find(pref[i]);
        if (a) s_dock[s_ndock++] = a;
    }
}

static bool in_dock(const aos_app_t *a)
{
    for (int i = 0; i < s_ndock; i++) if (s_dock[i] == a) return true;
    return false;
}

static void place_items(void)
{
    static aos_menu_item_t tmp[MAX_ITEMS] AOS_BSS_PSRAM;     /* 29 KB of scratch: not internal RAM */
    int n = aos_menu_root(tmp, MAX_ITEMS);
    if (!s_items) s_items = malloc(sizeof(placed_t) * MAX_ITEMS);

    if (aos_menu_has_dock()) s_ndock = aos_menu_dock(s_dock, L.dock_n);
    else default_dock();

    bool occ[MAX_PAGES][8][8];
    memset(occ, 0, sizeof occ);
    int page = 0, row = 0, col = 0;
    s_nitems = 0;

    /* with no menu.txt at all, the first page starts with the clock */
    bool virgin = !aos_menu_has_dock() && aos_menu_folder_count() == 0;
    if (virgin) {
        for (int i = n; i > 0; i--) tmp[i] = tmp[i - 1];
        memset(&tmp[0], 0, sizeof tmp[0]);
        tmp[0].kind = AOS_MENU_ITEM_WIDGET;
        tmp[0].folder = -1;
        snprintf(tmp[0].widget, sizeof tmp[0].widget, "clock");
        tmp[0].w = 4;
        tmp[0].h = 2;
        n++;
    }

    for (int i = 0; i < n && s_nitems < MAX_ITEMS; i++) {
        aos_menu_item_t *it = &tmp[i];
        if (it->kind == AOS_MENU_ITEM_APP && (!it->app || in_dock(it->app))) continue;
        placed_t *p = &s_items[s_nitems];
        memset(p, 0, sizeof *p);
        p->it = *it;
        if (it->kind == AOS_MENU_ITEM_PAGE) {
            if (row || col) { page++; row = col = 0; }
            p->page = page;
            s_nitems++;
            continue;
        }
        int w = 1, h = 1;
        if (it->kind == AOS_MENU_ITEM_WIDGET) {
            w = LV_MIN(it->w, L.cols);
            h = LV_MIN(it->h, L.rows);
        }
        /* first free spot from the cursor on, moving to new pages if needed */
        bool done = false;
        while (!done && page < MAX_PAGES) {
            for (int r = row; r <= L.rows - h && !done; r++) {
                for (int c = (r == row ? col : 0); c <= L.cols - w && !done; c++) {
                    bool free_ = true;
                    for (int y = 0; y < h && free_; y++)
                        for (int x = 0; x < w && free_; x++) free_ = !occ[page][r + y][c + x];
                    if (!free_) continue;
                    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) occ[page][r + y][c + x] = true;
                    p->page = page; p->row = r; p->col = c; p->w = w; p->h = h;
                    done = true;
                    /* the cursor advances past single cells only: an app after a
                     * widget fills the rest of the widget's rows first */
                    row = r; col = c + w;
                    if (col >= L.cols) { col = 0; row++; }
                }
            }
            if (!done) { page++; row = col = 0; }
        }
        if (page >= MAX_PAGES) break;
        s_nitems++;
        if (row >= L.rows) { row = col = 0; page++; }
    }
    s_npages = 1;
    for (int i = 0; i < s_nitems; i++)
        if (s_items[i].it.kind != AOS_MENU_ITEM_PAGE && s_items[i].page + 1 > s_npages) s_npages = s_items[i].page + 1;
    if (s_editing && s_npages < MAX_PAGES) s_npages++;   /* room to drop onto a new page */
}

static void dots_refresh(void)
{
    uint32_t n = lv_obj_get_child_count(s_dots);
    for (uint32_t i = 0; i < n; i++)
        lv_obj_set_style_bg_opa(lv_obj_get_child(s_dots, i), (int)i == s_page ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void pages_scrolled(lv_event_t *e)
{
    const aos_geo_t *g = aos_ui_geo();
    int p = (lv_obj_get_scroll_x(s_pages) + g->w / 2) / g->w;
    p = LV_CLAMP(0, p, s_npages - 1);
    if (p != s_page) { s_page = p; dots_refresh(); }
}

static void long_press_empty(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_pages || lv_obj_get_parent(lv_event_get_target(e)) == s_pages) aos_home_edit(true);
}

static void done_clicked(lv_event_t *e) { aos_home_edit(false); }

static void build(void)
{
    const aos_geo_t *g = aos_ui_geo();
    layout_compute();
    folder_close(false);
    lv_obj_clean(s_root);
    memset(s_page_obj, 0, sizeof s_page_obj);
    memset(s_dock_cells, 0, sizeof s_dock_cells);
    lv_obj_set_size(s_root, g->w, g->h);
    place_items();

    s_pages = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pages);
    lv_obj_set_pos(s_pages, 0, L.grid_y);
    lv_obj_set_size(s_pages, g->w, L.grid_h);
    lv_obj_set_scroll_dir(s_pages, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_pages, LV_SCROLL_SNAP_START);
    lv_obj_add_flag(s_pages, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(s_pages, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_pages, pages_scrolled, LV_EVENT_SCROLL, NULL);
    /* while editing, a drag moves an icon; pages change by dropping at an edge */
    if (s_editing) lv_obj_remove_flag(s_pages, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_pages, long_press_empty, LV_EVENT_LONG_PRESSED, NULL);

    for (int i = 0; i < s_npages; i++) {
        lv_obj_t *pg = lv_obj_create(s_pages);
        lv_obj_remove_style_all(pg);
        lv_obj_set_pos(pg, i * g->w, 0);
        lv_obj_set_size(pg, g->w, L.grid_h);
        lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pg, LV_OBJ_FLAG_EVENT_BUBBLE);
        s_page_obj[i] = pg;
    }

    for (int i = 0; i < s_nitems; i++) {
        placed_t *p = &s_items[i];
        if (p->it.kind == AOS_MENU_ITEM_PAGE) continue;
        lv_obj_t *pg = s_page_obj[p->page];
        int32_t x = L.side + p->col * L.cell_w, y = p->row * L.cell_h;
        if (p->it.kind == AOS_MENU_ITEM_WIDGET) {
            lv_obj_t *w = widget_create(pg, &p->it, p->w * L.cell_w - 20, p->h * L.cell_h - 20);
            lv_obj_set_pos(w, x + 10, y + 10);
            p->cell = w;
            continue;
        }
        const char *name;
        if (p->it.kind == AOS_MENU_ITEM_FOLDER) {
            const aos_menu_folder_t *f = aos_menu_folder(p->it.folder);
            name = f ? f->name : "";
            p->cell = cell_create(pg, L.cell_w, L.cell_h, true, name, p, NULL, p->it.folder);
        } else {
            name = aos_tr(p->it.app->desc.name);
            p->cell = cell_create(pg, L.cell_w, L.cell_h, true, name, p, p->it.app, -1);
            p->badge = badge_create(p->cell, p->it.app->badge, (L.cell_w + L.icon) / 2, 6);
        }
        lv_obj_set_pos(p->cell, x, y);
        if (s_editing) wiggle(p->cell, true, i);
    }

    /* page dots */
    s_dots = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, LV_SIZE_CONTENT, 16);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_dots, 14, 0);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < s_npages; i++) {
        lv_obj_t *d = lv_obj_create(s_dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 14, 14);
        lv_obj_set_style_radius(d, 7, 0);
        lv_obj_set_style_bg_color(d, lv_color_white(), 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_update_layout(s_dots);
    lv_obj_set_pos(s_dots, (g->w - lv_obj_get_width(s_dots)) / 2, L.dots_y + 12);
    lv_obj_set_flag(s_dots, LV_OBJ_FLAG_HIDDEN, s_npages < 2);
    if (s_page >= s_npages) s_page = s_npages - 1;
    dots_refresh();
    lv_obj_scroll_to_x(s_pages, s_page * g->w, LV_ANIM_OFF);

    /* dock */
    s_dock_panel = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_dock_panel);
    int32_t dock_w = g->w - 2 * (L.side - 6);
    lv_obj_set_size(s_dock_panel, dock_w, L.dock_h);
    lv_obj_set_pos(s_dock_panel, L.side - 6, L.dock_y);
    lv_obj_set_style_radius(s_dock_panel, 44, 0);
    lv_obj_set_style_bg_color(s_dock_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_dock_panel, LV_OPA_20, 0);
    lv_obj_remove_flag(s_dock_panel, LV_OBJ_FLAG_SCROLLABLE);
    int32_t slot = dock_w / L.dock_n;
    int32_t used = s_ndock * slot, x0 = (dock_w - used) / 2;
    for (int i = 0; i < s_ndock; i++) {
        lv_obj_t *c = cell_create(s_dock_panel, slot, L.dock_h, false, "", NULL, s_dock[i], -1);
        lv_obj_set_pos(c, x0 + i * slot, 0);
        s_dock_cells[i] = c;
        s_dock_badges[i] = badge_create(c, s_dock[i]->badge, (slot + L.icon) / 2, (L.dock_h - L.icon) / 2);
        if (s_editing) wiggle(c, true, 40 + i);
    }

    /* "Listo" while editing */
    s_done_btn = lv_button_create(s_root);
    lv_obj_set_style_radius(s_done_btn, 30, 0);
    lv_obj_set_style_bg_color(s_done_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_done_btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(s_done_btn, 0, 0);
    lv_obj_set_style_pad_hor(s_done_btn, 28, 0);
    lv_obj_set_style_pad_ver(s_done_btn, 12, 0);
    lv_obj_t *dl = lv_label_create(s_done_btn);
    lv_obj_set_style_text_font(dl, aos_font_small, 0);
    lv_label_set_text(dl, _("Listo"));
    lv_obj_align(s_done_btn, LV_ALIGN_TOP_RIGHT, -L.side, g->bar_h + 2);
    lv_obj_add_event_cb(s_done_btn, done_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flag(s_done_btn, LV_OBJ_FLAG_HIDDEN, !s_editing);
}

/* -------------------------------------------------------------------------- */
/* Folder panel                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_folder_from;

static void folder_edit_clicked(lv_event_t *e);
static void folder_name_clicked(lv_event_t *e);
static void folder_remove_badge(lv_obj_t *cell, const aos_app_t *app);

/* After a change the home is rebuilt (save_model), which closes the folder:
 * it is opened again, by id, as it was. */
static void folder_reopen(void)
{
    if (!s_reopen[0]) return;
    int idx = aos_menu_folder_find(s_reopen);
    s_reopen[0] = 0;
    if (idx >= 0 && aos_menu_folder_apps(idx, NULL, 0) > 0) folder_open(idx, NULL);
    else s_folder_editing = false;
}

static void folder_save_patch(void)
{
    const aos_menu_folder_t *f = aos_menu_folder(s_patch.folder);
    snprintf(s_reopen, sizeof s_reopen, "%s", f ? f->id : "");
    save_model();
    folder_reopen();
}

/* These run from an event of an object that the rebuild deletes, so the
 * rebuild waits for LVGL to be done with the event. */
static void folder_redraw_async(void *arg)
{
    (void)arg;
    if (s_folder_open >= 0) folder_open(s_folder_open, NULL);
}

static void folder_save_async(void *arg)
{
    (void)arg;
    folder_save_patch();
}

static void folder_edit_clicked(lv_event_t *e)
{
    (void)e;
    s_folder_editing = !s_folder_editing;
    lv_async_call(folder_redraw_async, NULL);
}

static void folder_remove_clicked(lv_event_t *e)
{
    const aos_app_t *app = lv_event_get_user_data(e);
    if (!app || s_folder_open < 0 || s_patch.folder >= 0) return;
    s_patch.folder = s_folder_open;
    snprintf(s_patch.remove, sizeof s_patch.remove, "%s", app->desc.id);
    lv_async_call(folder_save_async, NULL);
}

/* A round "-" on the icon's corner: the app leaves the folder for the home
 * screen (at the end, where every unplaced app goes). */
static void folder_remove_badge(lv_obj_t *cell, const aos_app_t *app)
{
    lv_obj_t *b = lv_button_create(cell);
    lv_obj_set_size(b, 56, 56);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 4, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, aos_font_title, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, "-");
    lv_obj_center(l);
    lv_obj_add_event_cb(b, folder_remove_clicked, LV_EVENT_CLICKED, (void *)app);
}

/* The folder's name, typed on a sheet over everything. */
static lv_obj_t *s_name_sheet, *s_name_ta;

static void name_sheet_close(void)
{
    if (s_name_sheet) lv_obj_delete_async(s_name_sheet);
    s_name_sheet = s_name_ta = NULL;
}

static void name_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) { name_sheet_close(); return; }
    if (code != LV_EVENT_READY || !s_name_ta) return;
    const char *t = lv_textarea_get_text(s_name_ta);
    while (*t == ' ') t++;
    if (*t && s_folder_open >= 0) {
        s_patch.folder = s_folder_open;
        snprintf(s_patch.name, sizeof s_patch.name, "%s", t);
        for (char *c = s_patch.name; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        name_sheet_close();
        lv_async_call(folder_save_async, NULL);
        return;
    }
    name_sheet_close();
}

static void folder_name_clicked(lv_event_t *e)
{
    (void)e;
    const aos_menu_folder_t *f = aos_menu_folder(s_folder_open);
    if (!f) return;
    const aos_geo_t *g = aos_ui_geo();
    name_sheet_close();
    s_name_sheet = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_name_sheet);
    lv_obj_set_size(s_name_sheet, g->w, g->h);
    lv_obj_set_style_bg_color(s_name_sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_name_sheet, LV_OPA_80, 0);
    lv_obj_add_flag(s_name_sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *cap = lv_label_create(s_name_sheet);
    lv_obj_set_style_text_font(cap, aos_font_body, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(0xAEAEB2), 0);
    lv_label_set_text(cap, _("Nombre de la carpeta"));
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, g->landscape ? 40 : 200);
    s_name_ta = lv_textarea_create(s_name_sheet);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, AOS_MENU_NAME_MAX - 1);
    lv_textarea_set_text(s_name_ta, f->name);
    lv_obj_set_width(s_name_ta, g->w - 120);
    lv_obj_set_style_text_font(s_name_ta, aos_font_title, 0);
    lv_obj_align_to(s_name_ta, cap, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_t *kb = lv_keyboard_create(s_name_sheet);
    aos_keyboard_style(kb, aos_font_body);     /* dark, as every other keyboard of the system */
    lv_keyboard_set_textarea(kb, s_name_ta);
    lv_obj_set_size(kb, g->w, g->landscape ? g->h / 2 : g->h * 2 / 5);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, name_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, name_kb_event, LV_EVENT_CANCEL, NULL);
}

static void folder_bg_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_folder_ov) { s_folder_editing = false; folder_close(true); }
}

static void folder_open(int folder, lv_obj_t *from)
{
    const aos_geo_t *g = aos_ui_geo();
    const aos_menu_folder_t *f = aos_menu_folder(folder);
    if (!f) return;
    folder_close(false);
    s_folder_open = folder;
    s_folder_from = from;

    s_folder_ov = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_folder_ov);
    lv_obj_set_size(s_folder_ov, g->w, g->h);
    lv_obj_set_style_bg_color(s_folder_ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_folder_ov, LV_OPA_70, 0);
    lv_obj_add_flag(s_folder_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_folder_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_folder_ov, folder_bg_clicked, LV_EVENT_CLICKED, NULL);

    int cols = 3, rows = 3;
    int32_t cw = g->landscape ? 176 : 180, ch = g->landscape ? 170 : 196;
    int32_t pw = cols * cw + 48, ph = rows * ch + 48;
    if (g->landscape) { cols = 4; rows = 2; pw = cols * cw + 48; ph = rows * ch + 48; }

    /* the name; editing, a field to tap with a pencil, as on iOS */
    lv_obj_t *title = lv_obj_create(s_folder_ov);
    lv_obj_remove_style_all(title);
    lv_obj_set_size(title, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title, 16, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tl = lv_label_create(title);
    lv_obj_set_style_text_font(tl, aos_font_title, 0);
    lv_obj_set_style_text_color(tl, lv_color_white(), 0);
    lv_label_set_text(tl, f->name);
    if (s_folder_editing) {
        lv_obj_t *pen = lv_label_create(title);
        lv_obj_set_style_text_font(pen, &aos_sym_28, 0);
        lv_obj_set_style_text_color(pen, lv_color_hex(0xAEAEB2), 0);
        lv_label_set_text(pen, AOS_SYM_PENCIL);
        lv_obj_set_style_bg_color(title, lv_color_hex(0x3C4250), 0);
        lv_obj_set_style_bg_opa(title, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(title, 20, 0);
        lv_obj_set_style_pad_hor(title, 28, 0);
        lv_obj_set_style_pad_ver(title, 10, 0);
        lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(title, folder_name_clicked, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *panel = lv_obj_create(s_folder_ov);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, pw, ph);
    lv_obj_set_style_radius(panel, 48, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x3C4250), 0);
    lv_obj_set_style_bg_opa(panel, 230, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(panel, LV_SCROLL_SNAP_START);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, g->landscape ? 20 : 0);
    lv_obj_align_to(title, panel, LV_ALIGN_OUT_TOP_MID, 0, -24);

    const aos_app_t *apps[AOS_MAX_APPS];
    int n = aos_menu_folder_apps(folder, apps, AOS_MAX_APPS);
    int per = cols * rows;
    int32_t saved_icon = L.icon;
    L.icon = g->landscape ? AOS_UI_ICON_LAND : AOS_UI_ICON;
    for (int i = 0; i < n; i++) {
        int pg = i / per, k = i % per;
        lv_obj_t *c = cell_create(panel, cw, ch, true, aos_tr(apps[i]->desc.name), NULL, apps[i], -1);
        lv_obj_set_pos(c, pg * (pw - 48 + 48) + (k % cols) * cw, (k / cols) * ch);
        if (s_folder_editing) folder_remove_badge(c, apps[i]);
    }
    L.icon = saved_icon;

    /* Editar / Listo, under the panel: the remove badges and the name */
    lv_obj_t *eb = lv_button_create(s_folder_ov);
    lv_obj_set_style_radius(eb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eb, s_folder_editing ? AOS_C_ACCENT : lv_color_hex(0x3C4250), 0);
    lv_obj_set_style_pad_hor(eb, 40, 0);
    lv_obj_set_style_pad_ver(eb, 16, 0);
    lv_obj_t *el = lv_label_create(eb);
    lv_obj_set_style_text_font(el, aos_font_body, 0);
    lv_label_set_text(el, s_folder_editing ? _("Listo") : _("Editar"));
    lv_obj_align_to(eb, panel, LV_ALIGN_OUT_BOTTOM_MID, 0, 28);
    lv_obj_add_event_cb(eb, folder_edit_clicked, LV_EVENT_CLICKED, NULL);
    if (n == 0) {
        lv_obj_t *l = lv_label_create(panel);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, _("Carpeta vacía"));
        lv_obj_center(l);
    }
}

static void folder_close(bool animate)
{
    (void)animate;
    if (s_folder_ov) lv_obj_delete(s_folder_ov);
    s_folder_ov = NULL;
    s_folder_open = -1;
}

/* -------------------------------------------------------------------------- */
/* Saving the model                                                            */
/* -------------------------------------------------------------------------- */

static void fprint_folder(FILE *f, const aos_menu_folder_t *fo)
{
    static const char FILLS[] = "svdr";
    fprintf(f, "folder %s %06lX %06lX %c %s %c %s\n", fo->id, (unsigned long)fo->color_a,
            (unsigned long)fo->color_b, FILLS[fo->fill & 3], fo->glyph[0] ? fo->glyph : "folder",
            fo->glyph_dark ? 'b' : 'w', fo->name);
}

/* Writes the placed items back to menu.txt, in their order on screen, and
 * reloads it. Hidden apps are in no placed item; their "hide" lines are
 * written back from aos_menu_hidden(). */
static void save_model(void)
{
    const char *path = aos_hal_path_menu();
    FILE *f = fopen(path, "w");
    if (!f) {
        s_patch.folder = -1;
        s_patch.add[0] = s_patch.remove[0] = s_patch.name[0] = 0;
        aos_ui_toast(_("No se pudo guardar el inicio"), 2000);
        return;
    }
    fprintf(f, "# P4OS home screen - written by the home screen's edit mode\n");
    fprintf(f, "dock\n");
    for (int i = 0; i < s_ndock; i++) fprintf(f, "  app %s\n", s_dock[i]->desc.id);
    fprintf(f, "end\n");
    int last_page = 0;
    int new_folders = 0;
    for (int i = 0; i < s_nitems; i++) {
        placed_t *p = &s_items[i];
        /* page breaks are written where the pages actually break, which
         * already includes every explicit one (and drops the holes a drag
         * leaves behind) */
        if (p->it.kind == AOS_MENU_ITEM_PAGE) continue;
        while (p->page > last_page) { fprintf(f, "page\n"); last_page++; }
        switch (p->it.kind) {
        case AOS_MENU_ITEM_APP:
            fprintf(f, "app %s\n", p->it.app->desc.id);
            break;
        case AOS_MENU_ITEM_WIDGET:
            fprintf(f, "widget %s %dx%d%s%s\n", p->it.widget, p->it.w, p->it.h, p->it.arg[0] ? " " : "", p->it.arg);
            break;
        case AOS_MENU_ITEM_FOLDER:
            if (p->it.folder >= 1000) {
                /* made by a drop: two app ids in arg */
                char a[48], b[48];
                if (sscanf(p->it.arg, "%47s\n%47s", a, b) == 2) {
                    fprintf(f, "folder nueva%d 5E6B80 3A4150 v folder w %s\n  app %s\n  app %s\nend\n",
                            ++new_folders + (int)aos_hal_uptime_ms() % 1000, _("Carpeta"), a, b);
                }
            } else {
                const aos_menu_folder_t *fo = aos_menu_folder(p->it.folder);
                if (!fo) break;
                const aos_app_t *apps[AOS_MAX_APPS];
                int n = aos_menu_folder_apps(p->it.folder, apps, AOS_MAX_APPS);
                const char *ids[AOS_MAX_APPS + 1];
                int m = 0;
                bool patched = s_patch.folder == p->it.folder;
                for (int k = 0; k < n; k++)
                    if (!patched || strcmp(apps[k]->desc.id, s_patch.remove)) ids[m++] = apps[k]->desc.id;
                if (patched && s_patch.add[0]) ids[m++] = s_patch.add;
                if (m == 0) break;                      /* emptied: the folder goes */
                if (m == 1) {                           /* one left: it goes back to the page, as on iOS */
                    fprintf(f, "app %s\n", ids[0]);
                    break;
                }
                aos_menu_folder_t copy = *fo;
                if (patched && s_patch.name[0]) snprintf(copy.name, sizeof copy.name, "%s", s_patch.name);
                fprint_folder(f, &copy);
                for (int k = 0; k < m; k++) fprintf(f, "  app %s\n", ids[k]);
                fprintf(f, "end\n");
            }
            break;
        default: break;
        }
    }
    /* the hidden apps are in no placed item: their lines go back as they were */
    for (int k = 0; aos_menu_hidden(k); k++) fprintf(f, "hide %s\n", aos_menu_hidden(k));
    fclose(f);
    s_patch.folder = -1;
    s_patch.add[0] = s_patch.remove[0] = s_patch.name[0] = 0;
    aos_menu_load();
    build();
}

/* -------------------------------------------------------------------------- */
/* Public                                                                      */
/* -------------------------------------------------------------------------- */

uint32_t aos_folder_glyph_codepoint(const char *name)
{
    for (int i = 0; i < aos_folder_glyph_count; i++)
        if (name && !strcmp(aos_folder_glyphs[i].name, name)) return aos_folder_glyphs[i].codepoint;
    return aos_folder_glyphs[0].codepoint;
}

void aos_home_create(lv_obj_t *parent)
{
    s_root = parent;
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    build();
}

void aos_home_rebuild(void) { if (s_root) build(); }

bool aos_home_editing(void) { return s_editing; }

void aos_home_edit(bool on)
{
    if (s_editing == on) return;
    s_editing = on;
    build();
}

bool aos_home_back(void)
{
    if (s_folder_ov) { folder_close(true); return true; }
    if (s_editing) { aos_home_edit(false); return true; }
    if (s_page != 0) { lv_obj_scroll_to_x(s_pages, 0, LV_ANIM_ON); return true; }
    return false;
}

void aos_home_badges_refresh(void)
{
    for (int i = 0; i < s_nitems; i++) {
        placed_t *p = &s_items[i];
        if (!p->badge || !p->it.app) continue;
        lv_obj_set_flag(p->badge, LV_OBJ_FLAG_HIDDEN, p->it.app->badge <= 0);
        lv_label_set_text_fmt(lv_obj_get_child(p->badge, 0), p->it.app->badge > 99 ? "99+" : "%d", p->it.app->badge);
    }
    for (int i = 0; i < s_ndock; i++) {
        if (!s_dock_badges[i]) continue;
        lv_obj_set_flag(s_dock_badges[i], LV_OBJ_FLAG_HIDDEN, s_dock[i]->badge <= 0);
        lv_label_set_text_fmt(lv_obj_get_child(s_dock_badges[i], 0), "%d", s_dock[i]->badge);
    }
}

bool aos_home_icon_rect(const char *id, lv_area_t *out)
{
    for (int i = 0; i < s_ndock; i++)
        if (s_dock_cells[i] && !strcmp(s_dock[i]->desc.id, id)) { icon_area(s_dock_cells[i], out); return true; }
    for (int i = 0; i < s_nitems; i++) {
        placed_t *p = &s_items[i];
        if (p->it.kind == AOS_MENU_ITEM_APP && p->it.app && p->cell && p->page == s_page && !strcmp(p->it.app->desc.id, id)) {
            icon_area(p->cell, out);
            return true;
        }
    }
    return false;
}

void aos_home_tick(void)
{
    for (int i = 0; i < s_nitems; i++) {
        placed_t *p = &s_items[i];
        if (p->it.kind == AOS_MENU_ITEM_WIDGET && p->cell && lv_obj_has_flag(p->cell, LV_OBJ_FLAG_USER_1)) clock_refresh(p->cell);
    }
}
