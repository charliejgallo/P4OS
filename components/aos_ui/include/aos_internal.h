/* P4OS - Declarations shared between the pieces of the shell. */
#pragma once

#include "lvgl.h"
#include "aos_ui.h"
#include "aos_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- geometry of the current orientation (aos_ui.c) ---- */
typedef struct {
    int32_t w, h;           /* logical screen                                  */
    int32_t bar_h;          /* status bar                                      */
    int32_t bottom_h;       /* home-indicator strip under the apps             */
    bool    landscape;
} aos_geo_t;
const aos_geo_t *aos_ui_geo(void);

/* The shell's containers on the active screen, bottom to top. */
lv_obj_t *aos_ui_wall(void);        /* wallpaper                            */
lv_obj_t *aos_ui_home_layer(void);  /* the home screen                      */
lv_obj_t *aos_ui_app_layer(void);   /* the apps' roots                      */

/* Where the last app opened from (its icon on the home screen), for the zoom
 * in and out. Set by the home screen before aos_ui_open(). */
void aos_ui_set_launch_rect(const lv_area_t *icon);

/* ---- the screen recording's controls (aos_screenrec_ui.c) ---- */
void aos_screenrec_toggle(void);
bool aos_screenrec_on(void);

/* ---- the wallpaper's photos (aos_wallpaper.c) ---- */
void aos_wall_photo_reserve(void);  /* first thing: the photos' PSRAM slots, at the bottom */
void aos_wall_photo_init(void);
void aos_wall_photo_start(void);    /* the boot is over: decode them */
void aos_wall_photo_relayout(void);
const lv_image_dsc_t *aos_wall_photo_dsc(int which);     /* NULL: the gradient */
void aos_wall_photo_want(int which, bool want);           /* let it go / decode it again */
bool aos_wall_photo_release(int which);                   /* let it go, its PSRAM slot too */
uint8_t aos_wall_photo_lock_dim(void);                    /* black over the lock's photo when it is the home's */
uint32_t aos_wall_photo_tint(int which);                  /* 0xRRGGBB, 0 before the first decode */
bool aos_wall_photo_next(void);                           /* locking: the album's next photo */

/* ---- home screen (aos_home.c) ---- */
void aos_home_create(lv_obj_t *parent);
void aos_home_rebuild(void);             /* the model or the orientation changed */
bool aos_home_back(void);                /* closes a folder / leaves edit mode   */
void aos_home_edit(bool on);
bool aos_home_editing(void);
void aos_home_badges_refresh(void);
/* The on-screen rectangle of an app's icon (for the zoom out); false if the
 * app is not visible on the current page. */
bool aos_home_icon_rect(const char *id, lv_area_t *out);
void aos_home_tick(void);                /* once a second: widgets */

/* An app's icon at 'size', squircle with the app's gradient. */
lv_obj_t *aos_home_app_icon(lv_obj_t *parent, const aos_app_t *app, int32_t size);

/* ---- status bar (aos_statusbar.c) ---- */
void aos_statusbar_create(lv_obj_t *layer);
void aos_statusbar_layout(void);         /* orientation changed */
void aos_statusbar_tick(void);           /* once a second */
void aos_statusbar_app_gone(const char *app_id);   /* destroy_app(): its glyph goes */
void aos_statusbar_apps_tick(void);      /* the apps' glyphs, from aos_ui_tick() */
void aos_statusbar_set_title(const char *title);
lv_obj_t *aos_statusbar_obj(void);

/* ---- pull-down panels (aos_panels.c) ---- */
typedef enum { AOS_PANEL_NONE = 0, AOS_PANEL_CONTROL, AOS_PANEL_NOTIF } aos_panel_t;
void        aos_panels_create(lv_obj_t *layer);
void        aos_panels_layout(void);
void        aos_panel_drag(aos_panel_t which, int32_t dy);    /* follows the finger */
void        aos_panel_release(aos_panel_t which, int32_t dy, int32_t vy);
void        aos_panel_open(aos_panel_t which);
bool        aos_panel_close(void);          /* true if one was open */
aos_panel_t aos_panel_current(void);
void        aos_panels_tick(void);

/* ---- what is playing: the board's player or the iPhone's (aos_nowplaying.c) ---- */
typedef struct {
    bool on;                /* something to show */
    bool phone;             /* the iPhone's, over AMS */
    bool playing;
    char title[96], artist[96];
    char from[40];          /* the phone's player ("Spotify"); "" for the board */
} aos_np_t;
bool aos_np_get(aos_np_t *out);
void aos_np_command(aos_media_cmd_t cmd);   /* PLAY_PAUSE, NEXT, PREV to the one shown */

/* ---- banners and toasts (aos_banner.c) ---- */
void aos_banner_create(lv_obj_t *layer);
void aos_banner_layout(void);
void aos_banner_tick(void);              /* polls aos_hal_notif_* for new ones */
void aos_textfield_init(void);           /* aos_textfield.c: the cursor and the ring of the field being edited */
void aos_textfield_accent(void);         /* the accent changed: recolour them */
void aos_keyboard_init(void);            /* aos_keyboard.c: the Spanish maps, once */
void aos_keyboard_attach(lv_obj_t *kb);  /* each lv_keyboard, from the theme */
void aos_hwkbd_tick(void);               /* a USB keyboard types into the open keyboard (aos_hwkbd.c) */
void aos_hwkbd_app_gone(const char *id); /* the app closes: its key handler goes */
void aos_hwmouse_tick(void);             /* a USB mouse points on the screen (aos_hwmouse.c) */
void aos_hwmouse_app_gone(const char *id); /* the app closes: its mouse handler goes */
void aos_toast_show(const char *text, uint32_t ms);
void aos_notif_act(uint32_t uid, bool positive);
void aos_dev_init(void);                 /* aos_devtools.c: the developer overlays, from the prefs */   /* the phone's answer / reject / clear */
bool aos_banner_up(void);                /* a notification's banner is on the screen */
bool aos_toast_up(void);

/* ---- app switcher (aos_switcher.c) ---- */
void aos_switcher_open(void);
bool aos_switcher_close(void);
bool aos_switcher_is_open(void);

/* ---- the runtime's view of running apps, for the switcher ---- */
int        aos_ui_alive(aos_app_t **out, int max);   /* most recent first */
lv_obj_t  *aos_ui_app_root(aos_app_t *app);

/* A widget type registered with aos_ui_register_widget(), or NULL. The
 * Control Centre asks for "cc.ha" (Home Assistant's scenes). */
aos_widget_create_t aos_ui_widget_find(const char *type);

/* aos_widget_edit.c: the edit mode's sheets for widgets. add() lists the
 * described types and hands the one tapped to picked(); config() shows one
 * widget's size and what it shows, and done() gets what to do with it. */
typedef enum { AOS_WEDIT_APPLY, AOS_WEDIT_REMOVE } aos_wedit_action_t;
typedef void (*aos_wedit_done_t)(aos_wedit_action_t action, int w, int h, const char *arg);
void        aos_wedit_add(void (*picked)(const char *type));
void        aos_wedit_config(const char *type, int w, int h, const char *arg, aos_wedit_done_t done);
bool        aos_wedit_close(void);                  /* true if a sheet was open */
const char *aos_wedit_glyph(const char *type);      /* the described glyph, or a generic one */

#ifdef __cplusplus
}
#endif
