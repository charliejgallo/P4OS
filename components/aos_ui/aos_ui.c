/*
 * P4OS - UI runtime: the host of the apps and the glue of the shell.
 *
 * The active screen holds three containers, bottom to top:
 *
 *     wall     the wallpaper
 *     home     the home screen (aos_home.c)
 *     apps     one root per living app; only the one in front is visible
 *
 * and lv_layer_top() holds what floats over everything: the status bar, the
 * home indicator, the pull-down panels. Banners and toasts go on
 * lv_layer_sys(), over the panels.
 *
 * System gestures are read at the input device, before LVGL sees the touch
 * (read_wrap): a drag that starts on an edge and goes the right way is taken
 * from the app (lv_indev_wait_release, so nothing under the finger clicks)
 * and drives the shell instead. Bottom edge up: home, or the switcher if
 * the finger rests. Left edge right: back. Top edge down: notifications on
 * the left half, the control centre on the right half.
 *
 * Apps keep AmoledOS's life cycle. What P4OS adds is multitasking: leaving
 * an app with AOS_APP_FLAG_KEEP or BACKGROUND hides it instead of destroying
 * it (up to AOS_UI_ALIVE_MAX of them; the least recently used goes first).
 * Watch apps have neither flag and are destroyed on leaving, exactly as on
 * the watch, because some of them only stop their timers in destroy().
 */
#include "aos_ui.h"
#include "aos_internal.h"
#include "aos_access.h"
#include "aos_lock.h"
#include "aos_pair_ui.h"
#include "aos_hal.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_menu.h"
#include "aos_icon_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* State                                                                       */
/* -------------------------------------------------------------------------- */

/* Apps live in fixed slots: a pointer to an app stays valid while it is
 * registered (the watch sorted the array in place, which moved apps under
 * the pointers the runtime kept). The order is a separate index. */
static aos_app_t s_apps[AOS_MAX_APPS] AOS_BSS_PSRAM;     /* 23 KB, read by the UI task only */
static bool      s_used[AOS_MAX_APPS];
static int       s_order[AOS_MAX_APPS];
static int       s_count;

static aos_app_t *s_cur;              /* in front, NULL on the home screen */
static aos_geo_t  s_geo;
static bool       s_landscape;        /* the person's choice               */
static int        s_forced_rot = -1;  /* an app's orientation, while in front */

static lv_obj_t *s_wall, *s_home, *s_appl;
static lv_obj_t *s_indicator;
static lv_obj_t *s_curtain;           /* the zoom from/to an icon */
static lv_area_t s_launch_rect;
static bool      s_launch_rect_valid;

static bool s_block_gestures;
static int  s_last_swipe = LV_DIR_NONE;

/* requests from other tasks, applied in aos_ui_tick() */
static volatile bool s_req_menu, s_req_icons, s_req_nav_pending;
static volatile aos_ui_nav_t s_req_nav;
static volatile int  s_req_landscape = -2;
static char s_req_open[48], s_req_lang[8], s_req_toast[96];
static volatile bool s_req_open_pending, s_req_lang_pending, s_req_toast_pending;

static void relayout_all(void);
static void leave_current(bool destroy_now);

/* -------------------------------------------------------------------------- */
/* Geometry                                                                    */
/* -------------------------------------------------------------------------- */

static void geo_update(void)
{
    s_geo.w = aos_hal_screen_w();
    s_geo.h = aos_hal_screen_h();
    s_geo.landscape = s_geo.w > s_geo.h;
    s_geo.bar_h = s_geo.landscape ? 40 : 48;
    s_geo.bottom_h = s_geo.landscape ? 20 : 28;
}

const aos_geo_t *aos_ui_geo(void) { return &s_geo; }
lv_obj_t *aos_ui_wall(void) { return s_wall; }
lv_obj_t *aos_ui_home_layer(void) { return s_home; }
lv_obj_t *aos_ui_app_layer(void) { return s_appl; }

static void app_root_area(const aos_app_t *app, lv_area_t *a)
{
    bool full = app && (app->desc.flags & (AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_UNDER_BAR));
    a->x1 = 0;
    a->y1 = full ? 0 : s_geo.bar_h;
    a->x2 = s_geo.w - 1;
    a->y2 = s_geo.h - 1 - ((app && (app->desc.flags & AOS_APP_FLAG_FULLSCREEN)) ? 0 : s_geo.bottom_h);
}

void aos_ui_app_area(int32_t *w, int32_t *h)
{
    lv_area_t a;
    app_root_area(s_cur, &a);
    if (w) *w = lv_area_get_width(&a);
    if (h) *h = lv_area_get_height(&a);
}

/* -------------------------------------------------------------------------- */
/* Registry                                                                    */
/* -------------------------------------------------------------------------- */

static int slot_of(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        aos_app_t *a = &s_apps[s_order[i]];
        if (a->desc.id && strcmp(a->desc.id, id) == 0) return s_order[i];
    }
    return -1;
}

static void sort_order(void)
{
    for (int i = 1; i < s_count; i++) {
        int key = s_order[i], j = i - 1;
        while (j >= 0 && s_apps[s_order[j]].desc.order > s_apps[key].desc.order) {
            s_order[j + 1] = s_order[j];
            j--;
        }
        s_order[j + 1] = key;
    }
}

static bool s_hold_home, s_home_dirty;

bool aos_ui_register_app(const aos_app_t *app)
{
    if (!app || !app->desc.id) return false;
    if (slot_of(app->desc.id) >= 0) {
        aos_hal_log("ui", "duplicate app: %s", app->desc.id);
        return false;
    }
    int slot = -1;
    for (int i = 0; i < AOS_MAX_APPS; i++) if (!s_used[i]) { slot = i; break; }
    if (slot < 0) {
        aos_hal_log("ui", "%s does not fit: all %d app slots are taken", app->desc.id, AOS_MAX_APPS);
        return false;
    }
    s_apps[slot] = *app;
    s_apps[slot].inst = NULL;
    s_apps[slot].root = NULL;
    s_apps[slot].running = false;
    s_apps[slot].badge = 0;
    s_used[slot] = true;
    s_order[s_count++] = slot;
    sort_order();
    if (s_hold_home) s_home_dirty = true;
    else if (s_home) aos_home_rebuild();
    return true;
}

void aos_ui_hold_home(bool hold)
{
    s_hold_home = hold;
    if (!hold && s_home_dirty) {
        s_home_dirty = false;
        if (s_home) aos_home_rebuild();
    }
}

static void destroy_app(aos_app_t *app)
{
    if (!app->running && !app->root) return;
    aos_hwkbd_app_gone(app->desc.id);
    aos_hwmouse_app_gone(app->desc.id);
    if (app->running && app->destroy) app->destroy(app, app->inst);
    if (app->root) lv_obj_delete(app->root);
    app->inst = NULL;
    app->root = NULL;
    app->running = false;
}

bool aos_ui_unregister_app(const char *id)
{
    int slot = id ? slot_of(id) : -1;
    if (slot < 0) return false;
    aos_app_t *app = &s_apps[slot];
    if (s_cur == app) aos_ui_home();
    destroy_app(app);
    aos_icon_clear_ops(id);
    s_used[slot] = false;
    for (int i = 0; i < s_count; i++) {
        if (s_order[i] == slot) {
            memmove(&s_order[i], &s_order[i + 1], (size_t)(s_count - i - 1) * sizeof(int));
            s_count--;
            break;
        }
    }
    if (s_home) aos_home_rebuild();
    return true;
}

int aos_ui_app_count(void) { return s_count; }
const aos_app_t *aos_ui_app_at(int index) { return (index >= 0 && index < s_count) ? &s_apps[s_order[index]] : NULL; }
aos_app_t *aos_ui_app_find(const char *id) { int s = id ? slot_of(id) : -1; return s >= 0 ? &s_apps[s] : NULL; }
const char *aos_ui_current_app(void) { return s_cur ? s_cur->desc.id : NULL; }
lv_obj_t *aos_ui_app_root(aos_app_t *app) { return app ? app->root : NULL; }

int aos_ui_alive(aos_app_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < AOS_MAX_APPS; i++)
        if (s_used[i] && s_apps[i].running && n < max) out[n++] = &s_apps[i];
    /* most recent first */
    for (int i = 1; i < n; i++) {
        aos_app_t *k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j]->last_used_ms < k->last_used_ms) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return n;
}

void aos_ui_set_badge(const char *id, int count)
{
    aos_app_t *a = aos_ui_app_find(id);
    if (!a || a->badge == count) return;
    a->badge = count;
    aos_home_badges_refresh();
}

/* -------------------------------------------------------------------------- */
/* Orientation                                                                 */
/* -------------------------------------------------------------------------- */

static int wanted_rotation(const aos_app_t *app)
{
    int want = s_landscape ? 90 : 0;
    if (app) {
        uint32_t f = app->desc.flags & (AOS_APP_FLAG_PORTRAIT | AOS_APP_FLAG_LANDSCAPE);
        if (f == AOS_APP_FLAG_PORTRAIT) want = 0;
        else if (f == AOS_APP_FLAG_LANDSCAPE) want = 90;
    }
    return want;
}

/* Turns the screen if 'app' needs it; returns true if it turned. */
static bool apply_rotation(const aos_app_t *app)
{
    int want = wanted_rotation(app);
    if (aos_hal_display_get_rotation() == want) return false;
    aos_hal_display_set_rotation(want);
    s_forced_rot = (want != (s_landscape ? 90 : 0)) ? want : -1;
    relayout_all();
    return true;
}

static void place_root(aos_app_t *app)
{
    lv_area_t a;
    app_root_area(app, &a);
    lv_obj_set_pos(app->root, a.x1, a.y1);
    lv_obj_set_size(app->root, lv_area_get_width(&a), lv_area_get_height(&a));
}

/* The app's UI no longer matches the screen: resize() if it can, else a new
 * one. Background apps are dealt with when they come back to the front. */
static void refit_app(aos_app_t *app)
{
    if (!app->running || app->created_rot == aos_hal_display_get_rotation()) return;
    place_root(app);
    lv_obj_update_layout(app->root);
    if (app->resize && app->resize(app, app->inst, app->root)) {
        app->created_rot = aos_hal_display_get_rotation();
        return;
    }
    aos_hwkbd_app_gone(app->desc.id);
    aos_hwmouse_app_gone(app->desc.id);
    if (app->destroy) app->destroy(app, app->inst);
    lv_obj_clean(app->root);
    app->inst = app->create ? app->create(app, app->root) : NULL;
    app->created_rot = aos_hal_display_get_rotation();
}

static void relayout_all(void)
{
    geo_update();
    lv_obj_t *layers[] = { s_wall, s_home, s_appl };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_size(layers[i], s_geo.w, s_geo.h);
        lv_obj_set_pos(layers[i], 0, 0);
    }
    aos_statusbar_layout();
    aos_panels_layout();
    aos_banner_layout();
    aos_home_rebuild();
    lv_obj_set_pos(s_indicator, (s_geo.w - lv_obj_get_width(s_indicator)) / 2, s_geo.h - s_geo.bottom_h / 2 - 3);
    if (s_cur) refit_app(s_cur);
    aos_lock_layout();
    aos_pair_ui_layout();
}

void aos_ui_set_landscape(bool landscape)
{
    s_landscape = landscape;
    aos_hal_pref_set_i32("landscape", landscape);
    apply_rotation(s_cur);
    /* the app in front forced the other orientation: nothing turns now, but
     * it will when that app leaves */
}

bool aos_ui_landscape(void) { return s_landscape; }

/* -------------------------------------------------------------------------- */
/* Opening and closing                                                         */
/* -------------------------------------------------------------------------- */

void aos_ui_set_launch_rect(const lv_area_t *icon)
{
    if (icon) { s_launch_rect = *icon; s_launch_rect_valid = true; }
    else s_launch_rect_valid = false;
}

static void curtain_anim_cb(void *var, int32_t v)
{
    /* v: 0 = the icon's rectangle, 1024 = the whole screen */
    lv_obj_t *c = var;
    lv_area_t from = s_launch_rect_valid ? s_launch_rect
                                         : (lv_area_t){ s_geo.w / 2 - 60, s_geo.h / 2 - 60, s_geo.w / 2 + 60, s_geo.h / 2 + 60 };
    int32_t x1 = from.x1 + (0 - from.x1) * v / 1024;
    int32_t y1 = from.y1 + (0 - from.y1) * v / 1024;
    int32_t x2 = from.x2 + (s_geo.w - 1 - from.x2) * v / 1024;
    int32_t y2 = from.y2 + (s_geo.h - 1 - from.y2) * v / 1024;
    lv_obj_set_pos(c, x1, y1);
    lv_obj_set_size(c, x2 - x1 + 1, y2 - y1 + 1);
    lv_obj_set_style_radius(c, AOS_UI_ICON_RADIUS + (40 - AOS_UI_ICON_RADIUS) * v / 1024, 0);
}

static void curtain_opa_cb(void *var, int32_t v) { lv_obj_set_style_opa(var, (lv_opa_t)v, 0); }

static void curtain_done(lv_anim_t *a)
{
    lv_obj_add_flag(s_curtain, LV_OBJ_FLAG_HIDDEN);
}

/* Once the app covers the screen: the home screen goes out of the draw tree,
 * and the strips above and below the app's root (status bar, home
 * indicator) take the app's background instead of the wallpaper's. */
static void home_hide_done(lv_anim_t *a)
{
    if (!s_cur) return;
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_appl, lv_obj_get_style_bg_color(s_cur->root, 0), 0);
    lv_obj_set_style_bg_opa(s_appl, LV_OPA_COVER, 0);
}

/* The zoom: a plain rectangle in the app's colour grows from the icon to
 * the screen (or shrinks back), and fades while the real content shows
 * underneath. Animating the app's own tree (a scale transform) would make
 * LVGL render it into a full-screen layer on every frame. */
static void curtain_run(uint32_t color, bool opening)
{
    lv_obj_set_style_bg_color(s_curtain, lv_color_hex(color), 0);
    lv_obj_set_style_opa(s_curtain, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_curtain, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_curtain);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_curtain);
    lv_anim_set_exec_cb(&a, curtain_anim_cb);
    lv_anim_set_values(&a, opening ? 0 : 1024, opening ? 1024 : 0);
    lv_anim_set_duration(&a, opening ? 220 : 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (opening) lv_anim_set_completed_cb(&a, home_hide_done);
    lv_anim_start(&a);

    lv_anim_t f;
    lv_anim_init(&f);
    lv_anim_set_var(&f, s_curtain);
    lv_anim_set_exec_cb(&f, curtain_opa_cb);
    lv_anim_set_values(&f, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&f, opening ? 140 : 60);
    lv_anim_set_duration(&f, 160);
    lv_anim_set_completed_cb(&f, curtain_done);
    lv_anim_start(&f);
}

static bool keepable(const aos_app_t *app)
{
    return app->desc.flags & (AOS_APP_FLAG_KEEP | AOS_APP_FLAG_BACKGROUND);
}

static void enforce_alive_max(aos_app_t *keep)
{
    aos_app_t *alive[AOS_MAX_APPS];
    int n = aos_ui_alive(alive, AOS_MAX_APPS);
    for (int i = n - 1; i >= 0 && n > AOS_UI_ALIVE_MAX; i--) {
        if (alive[i] == keep || alive[i] == s_cur) continue;
        aos_hal_log("ui", "closing %s: more than %d apps alive", alive[i]->desc.id, AOS_UI_ALIVE_MAX);
        destroy_app(alive[i]);
        n--;
    }
}

/* Takes the app in front off the screen (hide(), and destroy() unless it is
 * one that is kept). */
static void leave_current(bool destroy_now)
{
    aos_app_t *app = s_cur;
    if (!app) return;
    s_cur = NULL;
    if (app->hide && app->running) app->hide(app, app->inst);
    if (destroy_now || !keepable(app)) destroy_app(app);
    else if (app->root) lv_obj_add_flag(app->root, LV_OBJ_FLAG_HIDDEN);
    aos_hal_audio_foreground(NULL);
    aos_i18n_app_unload();
}

bool aos_ui_open(const char *id)
{
    aos_app_t *app = aos_ui_app_find(id);
    if (!app) {
        aos_hal_log("ui", "no such app: %s", id ? id : "(null)");
        return false;
    }
    aos_panel_close();
    aos_switcher_close();
    if (aos_home_editing()) aos_home_edit(false);
    if (s_cur == app) return true;

    aos_app_t *prev = s_cur;
    if (prev) leave_current(false);

    apply_rotation(app);

    /* The app's own catalogue, before create(): that is where it builds its
     * UI and calls _(). In every path -created now or back from the
     * background- because a live app goes on translating from its tick and
     * its show(). Without this only _sistema.lang ever applied, and an app's
     * own strings stayed Spanish whatever the language. */
    aos_i18n_app_load(app->desc.id);

    if (app->running && app->created_rot != aos_hal_display_get_rotation()) {
        /* was alive in the other orientation */
        lv_obj_remove_flag(app->root, LV_OBJ_FLAG_HIDDEN);
        refit_app(app);
    } else if (app->running) {
        lv_obj_remove_flag(app->root, LV_OBJ_FLAG_HIDDEN);
    } else {
        app->root = lv_obj_create(s_appl);
        lv_obj_remove_style_all(app->root);
        lv_obj_set_style_bg_color(app->root, AOS_C_BG, 0);
        lv_obj_set_style_bg_opa(app->root, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(app->root, AOS_C_TEXT, 0);
        lv_obj_set_style_text_font(app->root, aos_font_body, 0);
        lv_obj_remove_flag(app->root, LV_OBJ_FLAG_SCROLLABLE);
        place_root(app);
        lv_obj_update_layout(app->root);
        app->running = true;
        app->created_rot = aos_hal_display_get_rotation();
        app->inst = app->create ? app->create(app, app->root) : NULL;
    }
    s_cur = app;
    app->last_used_ms = (uint32_t)aos_hal_uptime_ms();
    lv_obj_set_style_translate_x(app->root, 0, 0);
    lv_obj_set_style_translate_y(app->root, 0, 0);
    lv_obj_move_foreground(app->root);
    if (app->show) app->show(app, app->inst);
    aos_hal_audio_foreground(app->desc.id);

    bool full = app->desc.flags & AOS_APP_FLAG_FULLSCREEN;
    aos_ui_statusbar_set_visible(!full);
    lv_obj_set_flag(s_indicator, LV_OBJ_FLAG_HIDDEN, full);
    if (app->desc.flags & AOS_APP_FLAG_KEEP_AWAKE) aos_hal_activity();

    enforce_alive_max(app);
    curtain_run(app->desc.color_a ? app->desc.color_a : 0x2C2C2E, true);
    s_launch_rect_valid = false;
    return true;
}

void aos_ui_close(const char *id)
{
    aos_app_t *app = aos_ui_app_find(id);
    if (!app) return;
    if (app == s_cur) {
        leave_current(true);
        aos_ui_home();
    } else {
        destroy_app(app);
    }
}

static bool s_safe_mode;
void aos_ui_set_safe_mode(bool on) { s_safe_mode = on; }
bool aos_ui_safe_mode(void) { return s_safe_mode; }

void aos_ui_close_others(void)
{
    for (int i = 0; i < AOS_MAX_APPS; i++)
        if (s_used[i] && &s_apps[i] != s_cur) destroy_app(&s_apps[i]);
}

void aos_ui_home(void)
{
    aos_panel_close();
    aos_switcher_close();
    aos_app_t *app = s_cur;
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(s_appl, LV_OPA_TRANSP, 0);
    if (app) {
        uint32_t color = app->desc.color_a ? app->desc.color_a : 0x2C2C2E;
        lv_area_t r;
        s_launch_rect_valid = aos_home_icon_rect(app->desc.id, &r);
        if (s_launch_rect_valid) s_launch_rect = r;
        leave_current(false);
        if (s_forced_rot >= 0) { s_forced_rot = -1; apply_rotation(NULL); }
        curtain_run(color, false);
    } else {
        aos_home_back();
    }
    aos_ui_statusbar_set_visible(true);
    aos_ui_statusbar_style(AOS_BAR_LIGHT);
    lv_obj_add_flag(s_indicator, LV_OBJ_FLAG_HIDDEN);
}

void aos_ui_back(void)
{
    if (aos_pair_ui_visible()) { aos_pair_ui_cancel(); return; }     /* a Bluetooth code waiting: Back is a no */
    if (aos_panel_close() || aos_switcher_close()) return;
    if (s_cur) {
        if (s_cur->back && s_cur->back(s_cur, s_cur->inst)) return;
        aos_ui_home();
        return;
    }
    aos_home_back();
}

bool aos_ui_button(int action)
{
    if (s_cur && s_cur->button) return s_cur->button(s_cur, s_cur->inst, action);
    return false;
}

void aos_ui_edit_home(void)
{
    aos_ui_home();
    aos_home_edit(true);
}

/* -------------------------------------------------------------------------- */
/* Status bar and indicator                                                    */
/* -------------------------------------------------------------------------- */

static aos_bar_style_t s_bar_style;

void aos_ui_statusbar_set_visible(bool visible)
{
    lv_obj_t *bar = aos_statusbar_obj();
    if (bar) lv_obj_set_flag(bar, LV_OBJ_FLAG_HIDDEN, !visible);
}

void aos_ui_statusbar_style(aos_bar_style_t style)
{
    s_bar_style = style;
    lv_obj_t *bar = aos_statusbar_obj();
    if (bar) lv_obj_set_style_text_color(bar, style == AOS_BAR_DARK ? lv_color_black() : lv_color_white(), 0);
    if (s_indicator) lv_obj_set_style_bg_color(s_indicator, style == AOS_BAR_DARK ? lv_color_black() : lv_color_white(), 0);
}

void aos_ui_statusbar_refresh(void) { aos_statusbar_tick(); }

void aos_ui_toast(const char *text, uint32_t ms) { aos_toast_show(text, ms); }

/* -------------------------------------------------------------------------- */
/* System gestures                                                             */
/* -------------------------------------------------------------------------- */

typedef enum { G_NONE = 0, G_HOME, G_BACK, G_NOTIF, G_CONTROL, G_DEAD } gkind_t;

static struct {
    bool     down;
    gkind_t  kind;
    bool     captured;
    int32_t  x0, y0, x, y;
    int32_t  px, py;            /* previous sample, for the speed */
    int32_t  vx, vy;            /* px per sample, smoothed */
    uint32_t t0, still_since;
} g;

static lv_indev_t *s_indev;
static lv_indev_read_cb_t s_orig_read;
static uint32_t s_reads, s_presses;

/* injected input (tests, the portal) */
static struct {
    bool active;
    int32_t x, y, x2, y2;
    uint32_t start, hold, rest;     /* rest: still pressed at the end, ms */
    bool drag;
    bool started;       /* the first read reports the start point exactly */
} s_inj;

#define EDGE_BOTTOM 36
#define EDGE_LEFT   28
#define CAPTURE_PX  18

static void gesture_begin(int32_t x, int32_t y)
{
    memset(&g, 0, sizeof g);
    g.down = true;
    g.x0 = g.x = g.px = x;
    g.y0 = g.y = g.py = y;
    g.t0 = g.still_since = lv_tick_get();
    if (s_block_gestures && s_cur) { g.kind = G_DEAD; return; }
    if (aos_lock_is_locked()) { g.kind = G_DEAD; return; }     /* the lock screen's own swipe, nothing under it */
    if (aos_switcher_is_open()) { g.kind = G_DEAD; return; }
    if (aos_panel_current() != AOS_PANEL_NONE) { g.kind = G_DEAD; return; }
    if (y >= s_geo.h - EDGE_BOTTOM) g.kind = G_HOME;
    else if (y <= s_geo.bar_h) g.kind = x < s_geo.w / 2 ? G_NOTIF : G_CONTROL;
    else if (x <= EDGE_LEFT && s_cur && !(s_cur->desc.flags & AOS_APP_FLAG_NO_SWIPE)) g.kind = G_BACK;
    else g.kind = G_DEAD;
}

static void root_follow(void)
{
    if (!s_cur || !s_cur->root) return;
    if (g.kind == G_HOME) {
        int32_t up = g.y0 - g.y;
        if (up < 0) up = 0;
        lv_obj_set_style_translate_y(s_cur->root, -up / 2, 0);
        lv_obj_set_style_opa(s_cur->root, (lv_opa_t)LV_CLAMP(90, 255 - up / 3, 255), 0);
    } else if (g.kind == G_BACK) {
        int32_t dx = g.x - g.x0;
        lv_obj_set_style_translate_x(s_cur->root, dx > 0 ? dx : 0, 0);
    }
}

static void root_anim_x(void *var, int32_t v) { lv_obj_set_style_translate_x(var, v, 0); }
static void root_anim_y(void *var, int32_t v) { lv_obj_set_style_translate_y(var, v, 0); }

static void root_snap_back(void)
{
    if (!s_cur || !s_cur->root) return;
    lv_obj_set_style_opa(s_cur->root, LV_OPA_COVER, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cur->root);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (g.kind == G_BACK) {
        lv_anim_set_exec_cb(&a, root_anim_x);
        lv_anim_set_values(&a, lv_obj_get_style_translate_x(s_cur->root, 0), 0);
    } else {
        lv_anim_set_exec_cb(&a, root_anim_y);
        lv_anim_set_values(&a, lv_obj_get_style_translate_y(s_cur->root, 0), 0);
    }
    lv_anim_start(&a);
}

static void gesture_move(int32_t x, int32_t y)
{
    g.vx = (g.vx + (x - g.px) * 2) / 3;
    g.vy = (g.vy + (y - g.py) * 2) / 3;
    if (LV_ABS(x - g.px) > 3 || LV_ABS(y - g.py) > 3) g.still_since = lv_tick_get();
    g.px = x; g.py = y;
    g.x = x; g.y = y;

    if (!g.captured && g.kind != G_DEAD) {
        int32_t dx = x - g.x0, dy = y - g.y0;
        bool go = false;
        switch (g.kind) {
        case G_HOME:    go = -dy > CAPTURE_PX && -dy > LV_ABS(dx); break;
        case G_BACK:    go = dx > CAPTURE_PX && dx > LV_ABS(dy); break;
        case G_NOTIF:
        case G_CONTROL: go = dy > CAPTURE_PX && dy > LV_ABS(dx); break;
        default: break;
        }
        if (go) {
            g.captured = true;
            /* nothing under the finger clicks or keeps scrolling */
            if (s_indev) lv_indev_wait_release(s_indev);
        } else if (LV_ABS(dx) > 40 || LV_ABS(dy) > 40) {
            g.kind = G_DEAD;    /* went the wrong way: it is the app's */
        }
    }
    if (!g.captured) return;

    switch (g.kind) {
    case G_HOME:
    case G_BACK:
        root_follow();
        if (g.kind == G_HOME && g.y0 - g.y > 90 && lv_tick_elaps(g.still_since) > 320 && !aos_switcher_is_open()) {
            /* the finger rests halfway up: the switcher */
            root_snap_back();
            aos_switcher_open();
            g.kind = G_DEAD;
        }
        break;
    case G_NOTIF:   aos_panel_drag(AOS_PANEL_NOTIF, y - g.y0); break;
    case G_CONTROL: aos_panel_drag(AOS_PANEL_CONTROL, y - g.y0); break;
    default: break;
    }
}

static void gesture_end(void)
{
    g.down = false;
    if (!g.captured) {
        /* an ordinary swipe over an app, for aos_ui_take_gesture() */
        int32_t dx = g.x - g.x0, dy = g.y - g.y0;
        if (LV_ABS(dx) > 60 || LV_ABS(dy) > 60) {
            s_last_swipe = LV_ABS(dx) > LV_ABS(dy) ? (dx > 0 ? LV_DIR_RIGHT : LV_DIR_LEFT)
                                                   : (dy > 0 ? LV_DIR_BOTTOM : LV_DIR_TOP);
        }
        return;
    }
    switch (g.kind) {
    case G_HOME: {
        int32_t up = g.y0 - g.y;
        if (!s_cur) {
            /* on the home screen the bottom edge closes a folder or the edit
             * mode, or goes back to the first page */
            aos_home_back();
        } else if (up > s_geo.h / 7 || g.vy < -12) {
            if (s_cur && s_cur->root) {
                lv_obj_set_style_opa(s_cur->root, LV_OPA_COVER, 0);
                lv_obj_set_style_translate_y(s_cur->root, 0, 0);
            }
            aos_ui_home();
        } else {
            root_snap_back();
        }
        break;
    }
    case G_BACK:
        if (g.x - g.x0 > 110 || g.vx > 12) {
            if (s_cur && s_cur->root) lv_obj_set_style_translate_x(s_cur->root, 0, 0);
            aos_app_t *before = s_cur;
            if (before && before->back && before->back(before, before->inst)) {
                /* consumed: internal navigation */
            } else {
                aos_ui_home();
            }
        } else {
            root_snap_back();
        }
        break;
    case G_NOTIF:   aos_panel_release(AOS_PANEL_NOTIF, g.y - g.y0, g.vy); break;
    case G_CONTROL: aos_panel_release(AOS_PANEL_CONTROL, g.y - g.y0, g.vy); break;
    default: break;
    }
}

static void read_wrap(lv_indev_t *indev, lv_indev_data_t *d)
{
    s_orig_read(indev, d);

    if (s_inj.active) {
        if (!s_inj.started) { s_inj.started = true; s_inj.start = lv_tick_get(); }
        uint32_t el = lv_tick_elaps(s_inj.start);
        if (el > s_inj.hold && el <= s_inj.hold + s_inj.rest) el = s_inj.hold;
        if (el <= s_inj.hold && lv_tick_elaps(s_inj.start) <= s_inj.hold + s_inj.rest) {
            d->state = LV_INDEV_STATE_PRESSED;
            if (s_inj.drag) {
                d->point.x = s_inj.x + (s_inj.x2 - s_inj.x) * (int32_t)el / (int32_t)LV_MAX(s_inj.hold, 1);
                d->point.y = s_inj.y + (s_inj.y2 - s_inj.y) * (int32_t)el / (int32_t)LV_MAX(s_inj.hold, 1);
            } else {
                d->point.x = s_inj.x;
                d->point.y = s_inj.y;
            }
        } else {
            d->state = LV_INDEV_STATE_RELEASED;
            d->point.x = s_inj.drag ? s_inj.x2 : s_inj.x;
            d->point.y = s_inj.drag ? s_inj.y2 : s_inj.y;
            s_inj.active = false;
        }
    }

    s_reads++;
    bool pressed = d->state == LV_INDEV_STATE_PRESSED;
    /* A touch on a dark screen only wakes it: that finger does
     * not reach anything under it until it lifts. */
    static bool s_wake_touch;
    if (pressed && !g.down && !s_wake_touch && !aos_hal_display_is_on()) {
        s_wake_touch = true;
        aos_hal_activity();
    }
    if (s_wake_touch) {
        if (!pressed) s_wake_touch = false;
        d->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (pressed && !g.down) {
        s_presses++;
        aos_hal_activity();
        gesture_begin(d->point.x, d->point.y);
    } else if (pressed) {
        gesture_move(d->point.x, d->point.y);
    } else if (g.down) {
        gesture_end();
    }
}

void aos_ui_block_gestures(bool block) { s_block_gestures = block; }

uint32_t aos_ui_overlay(void)
{
    uint32_t o = 0;
    if (!s_cur || !s_cur->running || !s_cur->root) return AOS_UI_OVER_NOT_FRONT;
    /* g.captured lasts until the next touch begins: only while the finger
     * is down does it mean a gesture in progress */
    bool held = g.down && g.captured;
    if (aos_panel_current() != AOS_PANEL_NONE || (held && (g.kind == G_NOTIF || g.kind == G_CONTROL)))
        o |= AOS_UI_OVER_PANEL;
    if (aos_switcher_is_open()) o |= AOS_UI_OVER_SWITCHER;
    if ((held && (g.kind == G_HOME || g.kind == G_BACK)) ||
        lv_obj_get_style_translate_x(s_cur->root, 0) != 0 || lv_obj_get_style_translate_y(s_cur->root, 0) != 0 ||
        lv_obj_get_style_opa(s_cur->root, 0) != LV_OPA_COVER)
        o |= AOS_UI_OVER_GESTURE;
    if (s_curtain && !lv_obj_has_flag(s_curtain, LV_OBJ_FLAG_HIDDEN)) o |= AOS_UI_OVER_CURTAIN;
    if (aos_banner_up()) o |= AOS_UI_OVER_BANNER;
    if (aos_toast_up()) o |= AOS_UI_OVER_TOAST;
    if (aos_lock_covers()) o |= AOS_UI_OVER_LOCK;
    return o;
}

int aos_ui_take_gesture(void)
{
    int d = s_last_swipe;
    s_last_swipe = LV_DIR_NONE;
    return d;
}

void aos_ui_inject_tap(int x, int y, int hold_ms)
{
    s_inj = (typeof(s_inj)){ .active = true, .x = x, .y = y, .start = lv_tick_get(), .hold = hold_ms > 0 ? hold_ms : 80 };
}

void aos_ui_inject_drag(int x, int y, int x2, int y2, int hold_ms)
{
    s_inj = (typeof(s_inj)){ .active = true, .x = x, .y = y, .x2 = x2, .y2 = y2, .drag = true,
                             .start = lv_tick_get(), .hold = hold_ms > 0 ? hold_ms : 300 };
}

void aos_ui_inject_drag_rest(int x, int y, int x2, int y2, int hold_ms, int rest_ms)
{
    aos_ui_inject_drag(x, y, x2, y2, hold_ms);
    s_inj.rest = rest_ms > 0 ? rest_ms : 0;
}

void aos_ui_touch_stats(uint32_t *reads, uint32_t *presses)
{
    if (reads) *reads = s_reads;
    if (presses) *presses = s_presses;
}

/* -------------------------------------------------------------------------- */
/* Screen capture                                                              */
/* -------------------------------------------------------------------------- */

typedef enum { SNAP_IDLE = 0, SNAP_WANTED, SNAP_READY, SNAP_FAILED, SNAP_RELEASING } snap_slot_t;
static volatile snap_slot_t s_snap_state;
static lv_draw_buf_t *s_snap_buf;

bool aos_ui_request_snapshot(bool wake)
{
    if (s_snap_state != SNAP_IDLE) return false;
    if (wake) aos_hal_activity();
    s_snap_state = SNAP_WANTED;
    return true;
}

aos_snapshot_state_t aos_ui_snapshot_peek(aos_ui_snapshot_t *out)
{
    if (s_snap_state == SNAP_FAILED) return AOS_SNAPSHOT_FAILED;
    if (s_snap_state != SNAP_READY || !s_snap_buf) return AOS_SNAPSHOT_PENDING;
    if (out) {
        out->data = s_snap_buf->data;
        out->stride = s_snap_buf->header.stride;
        out->w = (uint16_t)s_snap_buf->header.w;
        out->h = (uint16_t)s_snap_buf->header.h;
    }
    return AOS_SNAPSHOT_READY;
}

void aos_ui_snapshot_release(void)
{
    if (s_snap_state == SNAP_IDLE || s_snap_state == SNAP_WANTED) return;
    s_snap_state = SNAP_RELEASING;
}

/* Composites an ARGB8888 layer onto the RGB565 capture (LVGL's ARGB8888 is
 * B, G, R, A in memory; from AmoledOS, where the order was once wrong). */
static void blend_layer(lv_draw_buf_t *base, const lv_draw_buf_t *top)
{
    int32_t w = LV_MIN(base->header.w, top->header.w);
    int32_t h = LV_MIN(base->header.h, top->header.h);
    for (int32_t y = 0; y < h; y++) {
        uint16_t *d = (uint16_t *)(base->data + (size_t)y * base->header.stride);
        const uint8_t *s = top->data + (size_t)y * top->header.stride;
        for (int32_t x = 0; x < w; x++, s += 4) {
            uint8_t a = s[3];
            if (!a) continue;
            uint8_t sb = s[0], sg = s[1], sr = s[2];
            if (a != 255) {
                uint16_t p = d[x];
                uint8_t r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
                uint8_t dr = (r5 << 3) | (r5 >> 2), dg = (g6 << 2) | (g6 >> 4), db = (b5 << 3) | (b5 >> 2);
                sr = (sr * a + dr * (255 - a)) / 255;
                sg = (sg * a + dg * (255 - a)) / 255;
                sb = (sb * a + db * (255 - a)) / 255;
            }
            d[x] = ((sr & 0xF8) << 8) | ((sg & 0xFC) << 3) | (sb >> 3);
        }
    }
}

static void snapshot_tick(void)
{
    if (s_snap_state == SNAP_RELEASING) {
        if (s_snap_buf) { lv_draw_buf_destroy(s_snap_buf); s_snap_buf = NULL; }
        s_snap_state = SNAP_IDLE;
        return;
    }
    if (s_snap_state != SNAP_WANTED) return;
    s_snap_buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!s_snap_buf) { s_snap_state = SNAP_FAILED; return; }
    lv_obj_t *layers[] = { lv_layer_top(), lv_layer_sys() };
    for (int i = 0; i < 2; i++) {
        if (!lv_obj_get_child_count(layers[i])) continue;
        lv_draw_buf_t *t = lv_snapshot_take(layers[i], LV_COLOR_FORMAT_ARGB8888);
        if (t) { blend_layer(s_snap_buf, t); lv_draw_buf_destroy(t); }
    }
    s_snap_state = SNAP_READY;
}

/* -------------------------------------------------------------------------- */
/* Requests from other tasks                                                   */
/* -------------------------------------------------------------------------- */

#define CALLS 8
static struct { void (*fn)(void *); void *arg; } s_calls[CALLS];
static int s_calls_n;
static void *s_calls_mx;

bool aos_ui_request_call(void (*fn)(void *arg), void *arg)
{
    if (!s_calls_mx) return false;
    aos_hal_mutex_lock(s_calls_mx);
    bool ok = s_calls_n < CALLS;
    if (ok) { s_calls[s_calls_n].fn = fn; s_calls[s_calls_n].arg = arg; s_calls_n++; }
    aos_hal_mutex_unlock(s_calls_mx);
    return ok;
}

static void calls_tick(void)
{
    if (!s_calls_mx) { s_calls_mx = aos_hal_mutex_create(); return; }
    aos_hal_mutex_lock(s_calls_mx);
    int n = s_calls_n;
    typeof(s_calls) c;
    memcpy(c, s_calls, sizeof c);
    s_calls_n = 0;
    aos_hal_mutex_unlock(s_calls_mx);
    for (int i = 0; i < n; i++) c[i].fn(c[i].arg);
}

void aos_ui_request_menu(void) { s_req_menu = true; }
void aos_ui_request_icons(void) { s_req_icons = true; }
void aos_ui_request_nav(aos_ui_nav_t nav) { s_req_nav = nav; s_req_nav_pending = true; }
void aos_ui_request_landscape(int landscape) { s_req_landscape = landscape; }

void aos_ui_request_open(const char *id)
{
    snprintf(s_req_open, sizeof s_req_open, "%s", id ? id : "");
    s_req_open_pending = true;
}

void aos_ui_request_language(const char *code)
{
    snprintf(s_req_lang, sizeof s_req_lang, "%s", code ? code : "");
    s_req_lang_pending = true;
}

void aos_ui_request_toast(const char *text)
{
    snprintf(s_req_toast, sizeof s_req_toast, "%s", text ? text : "");
    s_req_toast_pending = true;
}

static void apply_language(const char *code)
{
    /* text is copied into the objects, and apps may keep pointers into the
     * catalogue about to be freed: every app is closed (as on the watch) */
    aos_ui_home();
    for (int i = 0; i < AOS_MAX_APPS; i++) if (s_used[i]) destroy_app(&s_apps[i]);
    aos_i18n_set(code);
    aos_home_rebuild();
    aos_statusbar_layout();
    aos_panels_layout();
}

static void requests_tick(void)
{
    if (s_req_menu) { s_req_menu = false; aos_menu_load(); aos_home_rebuild(); }
    if (s_req_icons) { s_req_icons = false; aos_icon_scan_files(); aos_home_rebuild(); }
    if (s_req_open_pending) { s_req_open_pending = false; aos_ui_open(s_req_open); }
    if (s_req_nav_pending) {
        s_req_nav_pending = false;
        if (s_req_nav == AOS_UI_NAV_BACK) aos_ui_back();
        else if (s_req_nav != AOS_UI_NAV_NONE) aos_ui_home();
    }
    if (s_req_lang_pending) { s_req_lang_pending = false; apply_language(s_req_lang); }
    if (s_req_toast_pending) { s_req_toast_pending = false; aos_ui_toast(s_req_toast, 2000); }
    if (s_req_landscape != -2) {
        int l = s_req_landscape;
        s_req_landscape = -2;
        aos_ui_set_landscape(l < 0 ? !s_landscape : l != 0);
    }
}

/* -------------------------------------------------------------------------- */
/* Tick                                                                        */
/* -------------------------------------------------------------------------- */

void aos_ui_tick(void)
{
    static uint32_t n;
    n++;
    requests_tick();
    calls_tick();
    snapshot_tick();
    for (int i = 0; i < AOS_MAX_APPS; i++) {
        aos_app_t *a = &s_apps[i];
        if (!s_used[i] || !a->running || !a->tick) continue;
        if (a == s_cur || (a->desc.flags & AOS_APP_FLAG_BACKGROUND)) a->tick(a, a->inst);
    }
    aos_banner_tick();
    aos_lock_tick();
    aos_pair_ui_tick();
    aos_access_tick();              /* mDNS by the portal's rules; once a second inside */
    aos_hwkbd_tick();               /* a USB keyboard on the host types into the open keyboard */
    aos_hwmouse_tick();             /* and a USB mouse points */
    /* the screen's auto-off; an app in front with KEEP_AWAKE holds it on
     * for as long as it is in front (the flag used to count only when the
     * app opened) */
    aos_hal_screen_idle_tick(s_cur && s_cur->running && (s_cur->desc.flags & AOS_APP_FLAG_KEEP_AWAKE));
    if (n % 5 == 0) {
        aos_statusbar_tick();
        aos_home_tick();
        aos_panels_tick();
    }
}

/* -------------------------------------------------------------------------- */
/* Start                                                                       */
/* -------------------------------------------------------------------------- */

static const uint32_t WALL[AOS_UI_WALLPAPERS][2] = {
    { 0x1B2A4A, 0x3A1C4A },     /* night: the default */
    { 0x0F3D3E, 0x0B1F2A },     /* lagoon */
    { 0x4A1C1C, 0x1C0F2A },     /* ember */
    { 0x1E3A1E, 0x0E1A14 },     /* moss */
    { 0x2B2B30, 0x0A0A0C },     /* graphite */
    { 0x5B3A8C, 0x1A2E5C },     /* dusk */
    { 0x7A4A12, 0x2A1A0A },     /* amber: the bench at night */
    { 0x000000, 0x000000 },     /* black */
};

void aos_ui_wallpaper_colors(int index, uint32_t *top, uint32_t *bottom)
{
    if (index < 0 || index >= AOS_UI_WALLPAPERS) index = 0;
    if (top) *top = WALL[index][0];
    if (bottom) *bottom = WALL[index][1];
}

static int s_wallpaper;

void aos_ui_set_wallpaper(int index)
{
    if (index < 0 || index >= AOS_UI_WALLPAPERS) index = 0;
    s_wallpaper = index;
    aos_hal_pref_set_i32("wall", index);
    if (!s_wall) return;
    lv_obj_set_style_bg_color(s_wall, lv_color_hex(WALL[index][0]), 0);
    lv_obj_set_style_bg_grad_color(s_wall, lv_color_hex(WALL[index][1]), 0);
}

int aos_ui_wallpaper(void) { return s_wallpaper; }

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* The BOOT button (aos_hal.h). From the HAL's button thread: the press is
 * the app's first (a game may use it), under the LVGL lock; what the app
 * leaves is the system's. A press that wakes the screen does only that. A
 * click goes home; a long press saves what the screen shows to the card
 * (written here, out of the lock: it takes a few hundred ms). */
static bool s_btn_woke;

static void ui_button_cb(aos_button_t button, aos_button_action_t action)
{
    if (button != AOS_BUTTON_BOOT) return;
    if (action == AOS_BUTTON_PRESS) {
        s_btn_woke = !aos_hal_display_is_on();
        if (s_btn_woke) aos_hal_display_on(true);
        aos_hal_activity();
    }
    if (s_btn_woke) {
        if (action != AOS_BUTTON_PRESS) s_btn_woke = false;
        return;
    }
    bool used = false;
    if (action == AOS_BUTTON_CLICK && aos_pair_ui_visible()) {
        if (aos_hal_lock(300)) { aos_pair_ui_cancel(); aos_hal_unlock(); }
        return;
    }
    if (aos_lock_is_locked()) used = action != AOS_BUTTON_LONG;     /* locked: only the screenshot */
    else if (aos_hal_lock(300)) {
        if (s_cur && s_cur->running && s_cur->button) used = s_cur->button(s_cur, s_cur->inst, (int)action);
        if (!used && action == AOS_BUTTON_CLICK) aos_ui_home();
        aos_hal_unlock();
    }
    if (!used && action == AOS_BUTTON_LONG) {
        char path[60], msg[90];             /* a toast holds 96 */
        if (aos_hal_display_save(path, sizeof path)) snprintf(msg, sizeof msg, "%s %s", _("Captura guardada en"), path);
        else snprintf(msg, sizeof msg, "%s", _("No se pudo guardar la captura (¿hay tarjeta?)"));
        aos_ui_request_toast(msg);
    }
}

void aos_ui_init(void)
{
    aos_hal_set_button_cb(ui_button_cb);
    aos_theme_init();
    aos_i18n_init();
    aos_icon_scan_files();
    aos_menu_load();

    int32_t land = 0;
    aos_hal_pref_get_i32("landscape", &land);
    s_landscape = land != 0;
    aos_hal_display_set_rotation(s_landscape ? 90 : 0);
    geo_update();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, AOS_C_BG, 0);

    s_wall = plain(scr);
    s_home = plain(scr);
    s_appl = plain(scr);
    lv_obj_remove_flag(s_appl, LV_OBJ_FLAG_CLICKABLE);
    for (lv_obj_t **o = (lv_obj_t *[]){ s_wall, s_home, s_appl, NULL }; *o; o++)
        lv_obj_set_size(*o, s_geo.w, s_geo.h);

    /* the wallpaper: one of the gradients (a JPEG from the card, later) */
    lv_obj_set_style_bg_opa(s_wall, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(s_wall, LV_GRAD_DIR_VER, 0);
    int32_t wp = 0;
    aos_hal_pref_get_i32("wall", &wp);
    aos_ui_set_wallpaper(wp);

    s_curtain = plain(lv_layer_top());
    lv_obj_set_style_bg_opa(s_curtain, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_curtain, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_curtain, LV_OBJ_FLAG_CLICKABLE);

    aos_home_create(s_home);
    aos_statusbar_create(lv_layer_top());

    s_indicator = plain(lv_layer_top());
    lv_obj_set_size(s_indicator, 150, 6);
    lv_obj_set_style_radius(s_indicator, 3, 0);
    lv_obj_set_style_bg_opa(s_indicator, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_indicator, lv_color_white(), 0);
    lv_obj_remove_flag(s_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_indicator, (s_geo.w - 150) / 2, s_geo.h - s_geo.bottom_h / 2 - 3);

    aos_panels_create(lv_layer_top());
    aos_banner_create(lv_layer_sys());
    aos_lock_init();                /* over the panels; locked at boot when it is on */
    aos_dev_init();                 /* Settings, Developer: touches, fps, the log's level */

    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER && !s_indev) {
            s_indev = in;
            s_orig_read = lv_indev_get_read_cb(in);
            lv_indev_set_read_cb(in, read_wrap);
            /* Read on every refresh, not only when the driver has an event:
             * the GT911 is polled (its INT may not be wired, HARDWARE.md 7),
             * and injected touches have no event of their own. */
            lv_indev_set_mode(in, LV_INDEV_MODE_TIMER);
        }
    }
    aos_statusbar_tick();
}
