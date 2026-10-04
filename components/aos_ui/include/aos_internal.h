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
void aos_hwkbd_tick(void);               /* a USB keyboard types into the open keyboard (aos_hwkbd.c) */
void aos_hwkbd_app_gone(const char *id); /* the app closes: its key handler goes */
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

#ifdef __cplusplus
}
#endif
