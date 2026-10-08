/*
 * P4OS - UI runtime: the shell (home screen, status bar, control and
 * notification centres, app switcher) and the host of the apps.
 *
 * Only LVGL and aos_hal.h underneath, so the same code runs on the board and
 * in the simulator (the AmoledOS rule).
 *
 * Every function here runs with the LVGL lock held, from the LVGL task. The
 * web portal and other tasks go through the aos_ui_request_*() calls, which
 * only note the request down for the next aos_ui_tick().
 */
#pragma once

#include "lvgl.h"
#include "aos_app.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slots in the app table, built-in and dynamic together. */
#define AOS_MAX_APPS        256
#define AOS_BUILTIN_RESERVE 32

/* Apps kept alive at once when the person switches between them (each keeps
 * its LVGL tree; the least recently used is closed when one more opens). */
#define AOS_UI_ALIVE_MAX    4

/* Starts the shell on LVGL's active screen and registers the built-in apps. */
void aos_ui_init(void);

/* Called by the main loop every ~200 ms: ticks for the apps, the clock in the
 * status bar, the requests from other tasks. */
void aos_ui_tick(void);

/* ---- registry ---- */
bool             aos_ui_register_app(const aos_app_t *app);   /* copied by value */
/* While held, registering an app does not rebuild the home screen; letting
 * go rebuilds it once if anything changed. The boot scan holds it: the
 * rebuild per app was 5.6 of its 8.3 s with 40 apps (2026-09-30). */
void             aos_ui_hold_home(bool hold);

/* ---- what is over the app in front ---- */
/* For the apps that write to the panel themselves (blits, page flipping):
 * what LVGL is drawing over them right now, 0 when nothing. While a bit is
 * set, a blit or a flip would paint over it, or hide it at the next flip.
 * Stop presenting under PANEL, SWITCHER, GESTURE, CURTAIN and BANNER; a
 * TOAST is a two-second line an app may choose to cover. When it goes back
 * to 0, present a whole frame again: LVGL has drawn the app's own objects
 * where the overlay was. NOT_FRONT: the app is not the one in front. */
enum {
    AOS_UI_OVER_PANEL    = 1u << 0,     /* control or notification centre, open or dragged */
    AOS_UI_OVER_SWITCHER = 1u << 1,
    AOS_UI_OVER_GESTURE  = 1u << 2,     /* the app follows the finger home or back */
    AOS_UI_OVER_CURTAIN  = 1u << 3,     /* the zoom to or from its icon */
    AOS_UI_OVER_BANNER   = 1u << 4,     /* a notification's banner */
    AOS_UI_OVER_TOAST    = 1u << 5,
    AOS_UI_OVER_NOT_FRONT = 1u << 6,
    AOS_UI_OVER_LOCK     = 1u << 7,     /* the lock screen, up or sliding away (since 0.10.2: the
                                         * app under it is still "in front" and kept blitting over it) */
};
uint32_t aos_ui_overlay(void);

/* ---- boot screen (aos_boot.c) ---- */
/* The name on black with the real progress under it, over everything, until
 * boot_done() fades it out (or 30 s pass). All three with LVGL's lock. */
void aos_ui_boot_show(void);
void aos_ui_boot_progress(int done, int total);
void aos_ui_boot_done(void);
bool             aos_ui_unregister_app(const char *id);
int              aos_ui_app_count(void);
const aos_app_t *aos_ui_app_at(int index);
aos_app_t       *aos_ui_app_find(const char *id);

/* ---- navigation ---- */
bool        aos_ui_open(const char *id);   /* brings an app to the front */
void        aos_ui_back(void);             /* the app's back(), else home */
void        aos_ui_home(void);             /* the home screen */
void        aos_ui_close(const char *id);  /* ends an app (switcher's swipe up) */

/* Apps in the background (aos_ui.c explains it): what happens to an app with
 * KEEP when it is left. AUTO, the default: closed if it holds 1 MB of PSRAM
 * or more and has nothing running; CLOSE: closed unless it has something
 * running; KEEP: kept, as before. "Something running": BACKGROUND,
 * aos_ui_set_busy(), header pins, the speaker, the microphone, a recording. */
enum { AOS_UI_BG_AUTO = 0, AOS_UI_BG_CLOSE = 1, AOS_UI_BG_KEEP = 2 };
int         aos_ui_bg_mode(void);
void        aos_ui_set_bg_mode(int mode);
/* An app says it has something that must go on while it is hidden (a remote
 * session, a download) that the shell cannot see by itself. */
void        aos_ui_set_busy(const char *id, bool busy);
/* What the shell measured when the app last left: the PSRAM it holds, KB. */
uint32_t    aos_ui_app_held_kb(const aos_app_t *app);
/* What a live app has running ("background", "busy", "pins", "audio",
 * "microphone", "recording"), or NULL. */
const char *aos_ui_app_busy(const aos_app_t *app);
/* The live apps (in front or hidden), most recent first. */
int         aos_ui_alive(aos_app_t **out, int max);
void        aos_ui_close_others(void);     /* ends every app but the one in front (USB disk mode) */
void        aos_ui_set_safe_mode(bool on); /* main.c: this boot is the BOOT button's safe mode */
/* Settings, Developer (aos_devtools.c): overlays and the log's level, kept as
 * preferences and applied again at boot */
void aos_dev_set_touches(bool on);          /* a ring under every finger */
bool aos_dev_touches(void);
void aos_dev_set_fps(bool on);              /* frames a second, top right */
bool aos_dev_fps(void);
void aos_dev_set_log_level(int level);      /* 1 errors, 2 warnings, 3 info */
int  aos_dev_log_level(void);
bool        aos_ui_safe_mode(void);        /* for Settings' Diagnostics */
const char *aos_ui_current_app(void);      /* NULL on the home screen */

/* P4OS: keys from a USB keyboard (aos_hwkbd.c; aos_hal.h has the codes). By
 * default they type into the text area of the LVGL keyboard that is open.
 * An app with a keyboard of its own asks for them while its keyboard is up,
 * and gives them back with NULL: the handler gets them only while that app
 * is in front, and the shell forgets it when the app closes. key is a
 * Unicode code point (accents already composed) or an AOS_KEY_*; mods is
 * HID's modifier byte. Return true for a key used; false lets it go on to
 * an open LVGL keyboard. */
typedef bool (*aos_hwkbd_cb_t)(uint32_t key, uint8_t mods);
void aos_ui_hwkbd_handler(aos_hwkbd_cb_t cb);

/* P4OS: a USB mouse, raw (aos_hwmouse.c), for an app that wants more of it
 * than a finger: a VNC viewer, a drawing app. By default the left button
 * is a finger on the glass, the right one "back", the middle one "home"
 * and the wheel scrolls what is under the arrow. An app asks for the
 * reports while it is in front, like the keyboard's handler, and the
 * handler says with a mask what it took for itself; what it did not take
 * goes on as before. x/y is where the arrow is, in the screen's
 * coordinates; dx/dy what the mouse moved (0 on an absolute pointer);
 * buttons bit 0 left, 1 right, 2 middle, with pressed/released the edges
 * of this report. Called from LVGL's task, once a report. */
typedef struct {
    int32_t x, y;
    int16_t dx, dy;
    int8_t  wheel;              /* notches, positive away from the user */
    uint8_t buttons, pressed, released;
} aos_hwmouse_event_t;
enum {
    AOS_HWMOUSE_LEFT   = 1,     /* the left button does not press LVGL */
    AOS_HWMOUSE_RIGHT  = 2,     /* no "back" */
    AOS_HWMOUSE_MIDDLE = 4,     /* no "home" */
    AOS_HWMOUSE_WHEEL  = 8,     /* no scrolling */
    AOS_HWMOUSE_ALL    = 15,
};
typedef uint8_t (*aos_hwmouse_cb_t)(const aos_hwmouse_event_t *ev);
void aos_ui_hwmouse_handler(aos_hwmouse_cb_t cb);

/* ---- opening an app on something (aos_open_arg.c) ----
 * Archivos opens a photo in Fotos, a song in Música, a .bin in the
 * Programador. The caller says which app and hands it an argument, a path
 * usually; the app takes it with aos_ui_take_open_arg(its own id) from its
 * show() - show() runs on every open, right after create() when the app was
 * not alive and on its own when it was kept, so that one place covers both.
 * aos_ui_open() is synchronous: an argument the app does not take during the
 * open is dropped when aos_ui_open_app_with() returns, so it can never reach
 * a later, ordinary open. The pointer returned by take() is valid until the
 * next call of either function. An app that knows nothing of this simply
 * opens as usual. */
bool        aos_ui_open_app_with(const char *id, const char *arg);
const char *aos_ui_take_open_arg(const char *id);

/* The size of the container an app gets, for the current orientation. */
void aos_ui_app_area(int32_t *w, int32_t *h);

/* ---- orientation ---- */
/* The person's choice (Control Centre, Settings). Saved, and applied at
 * once: the shell and the app in front re-lay out. */
void aos_ui_set_landscape(bool landscape);
bool aos_ui_landscape(void);

/* ---- home screen ---- */
/* The number on an app's icon (0 clears it). */
void aos_ui_set_badge(const char *id, int count);
/* Starts the edit mode of the home screen (as a long press would). */
void aos_ui_edit_home(void);

/* ---- wallpaper ---- */
/* One of the built-in gradients (0..AOS_UI_WALLPAPERS-1), saved. */
#define AOS_UI_WALLPAPERS 8
void aos_ui_set_wallpaper(int index);
int  aos_ui_wallpaper(void);
void aos_ui_wallpaper_colors(int index, uint32_t *top, uint32_t *bottom);

/* ---- status bar ---- */
typedef enum {
    AOS_BAR_LIGHT = 0,      /* white glyphs, for dark content (the default) */
    AOS_BAR_DARK,           /* black glyphs, for light content              */
} aos_bar_style_t;
void aos_ui_statusbar_set_visible(bool visible);
void aos_ui_statusbar_style(aos_bar_style_t style);
void aos_ui_statusbar_refresh(void);

/* ---- feedback ---- */
/* A short notice in a capsule at the bottom. */
void aos_ui_toast(const char *text, uint32_t ms);

/* Home-screen widget types from outside the shell, for menu.txt's
 * "widget <type> <w>x<h> [arg]". create() fills the card it is given (sized,
 * rounded, translucent, 22 px of padding) and owns what it adds - timers
 * included: hang their deletion off the card's LV_EVENT_DELETE. The card
 * lets clicks bubble to the home screen, so a child that acts on a tap must
 * clear LV_OBJ_FLAG_EVENT_BUBBLE on itself. Register before aos_ui_init(). */
typedef void (*aos_widget_create_t)(lv_obj_t *card, int32_t w, int32_t h, const char *arg);
void aos_ui_register_widget(const char *type, aos_widget_create_t create);

/* ---- gestures ---- */
/* An app that needs whole drags (a drawing canvas, a game) switches the
 * system's edge gestures off while it has them. */
void aos_ui_block_gestures(bool block);
/* The last swipe the runtime saw over an app, for apps that react to
 * swipes themselves; LV_DIR_NONE if none since the last call. */
int  aos_ui_take_gesture(void);
/* Physical button (BOOT, as a spare): routed to the app in front. */
bool aos_ui_button(int action);

/* ---- requests from other tasks (the web portal) ---- */
typedef enum { AOS_UI_NAV_NONE = 0, AOS_UI_NAV_BACK, AOS_UI_NAV_HOME, AOS_UI_NAV_LAUNCHER } aos_ui_nav_t;
void aos_ui_request_language(const char *code);
void aos_ui_request_icons(void);
void aos_ui_request_menu(void);
void aos_ui_request_open(const char *id);
void aos_ui_request_nav(aos_ui_nav_t nav);
void aos_ui_request_toast(const char *text);
void aos_ui_request_landscape(int landscape);   /* 0, 1; -1 toggles */
/* Runs fn(arg) in the LVGL task at the next aos_ui_tick (within 200 ms): for
 * anything else that must touch LVGL or the shell. false if the queue (8) is
 * full. */
bool aos_ui_request_call(void (*fn)(void *arg), void *arg);

/* ---- screen capture (portal /api/captura, the switcher's thumbnails) ---- */
typedef enum {
    AOS_SNAPSHOT_IDLE = 0,
    AOS_SNAPSHOT_PENDING,
    AOS_SNAPSHOT_READY,
    AOS_SNAPSHOT_FAILED,
} aos_snapshot_state_t;

typedef struct {
    const uint8_t *data;        /* RGB565, row by row */
    uint32_t       stride;      /* bytes per row */
    uint16_t       w, h;
} aos_ui_snapshot_t;

bool                 aos_ui_request_snapshot(bool wake);
aos_snapshot_state_t aos_ui_snapshot_peek(aos_ui_snapshot_t *out);
void                 aos_ui_snapshot_release(void);

/* ---- input injection (tests, the portal's remote touch) ---- */
void aos_ui_inject_tap(int x, int y, int hold_ms);
void aos_ui_inject_drag(int x, int y, int x2, int y2, int hold_ms);
/* The same, with the finger resting rest_ms at the end before lifting. */
void aos_ui_inject_drag_rest(int x, int y, int x2, int y2, int hold_ms, int rest_ms);
void aos_ui_touch_stats(uint32_t *reads, uint32_t *presses);

/* ---- kept from AmoledOS so that ported code compiles; no effect here ---- */
typedef enum { AOS_LAUNCHER_LIST = 0, AOS_LAUNCHER_GRID, AOS_LAUNCHER_HONEYCOMB } aos_launcher_style_t;
static inline void aos_ui_launcher_set_style(aos_launcher_style_t s) { (void)s; }
static inline aos_launcher_style_t aos_ui_launcher_get_style(void) { return AOS_LAUNCHER_GRID; }
static inline void aos_ui_request_launcher_style(int s) { (void)s; }
static inline void aos_ui_request_watchface(const char *id) { (void)id; }
static inline void aos_ui_request_watchface_picker(void) {}
static inline void aos_ui_show_launcher(void) { aos_ui_home(); }
static inline void aos_ui_touch_raw(bool raw) { (void)raw; }
static inline void aos_ui_touch_map(int32_t rx, int32_t ry, int32_t *sx, int32_t *sy) { *sx = rx; *sy = ry; }
static inline void aos_ui_touch_calibration_save(float a, float b, float c, float d) { (void)a; (void)b; (void)c; (void)d; }
static inline void aos_ui_touch_calibration_reset(void) {}
static inline bool aos_ui_touch_calibration_get(float *a, float *b, float *c, float *d) { *a = *c = 1; *b = *d = 0; return false; }

#ifdef __cplusplus
}
#endif
