/*
 * P4OS - the lock screen (aos_lock.c).
 *
 * The time, the date, the messages that came (notifications from the
 * phone, Home Assistant, the apps) and the music playing, over the
 * wallpaper; a swipe up unlocks, or opens the code pad when there is a
 * code. It comes up when the screen has been off for the time chosen in
 * Settings, and at boot. The code is kept as a salted SHA-256, never as
 * itself; wrong codes make the pad wait longer each time. Forgotten, the
 * BOOT button's safe mode lets anyone in without it, to remove it from
 * Settings.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     enabled;
    uint32_t after_s;           /* screen off this long locks it; 0: as soon as it goes off */
    bool     show_notifs;       /* the messages */
    bool     show_music;        /* what plays, with its buttons */
    bool     hide_content;      /* with a code: only the app and how many, not the text */
} aos_lock_cfg_t;

void aos_lock_init(void);       /* aos_ui_init, after the layers */
void aos_lock_tick(void);       /* aos_ui_tick: decides when to lock */
void aos_lock_layout(void);     /* the orientation changed */
bool aos_lock_is_locked(void);
bool aos_lock_covers(void);     /* locked, or its layer still sliding away */
void aos_lock_now(void);

void aos_lock_get_cfg(aos_lock_cfg_t *out);
void aos_lock_set_cfg(const aos_lock_cfg_t *cfg);

int  aos_lock_pin_len(void);    /* 0: no code; 4 or 6 */
bool aos_lock_pin_check(const char *pin);
void aos_lock_pin_set(const char *pin);     /* NULL or "": no code */

/* The code pad as a sheet over 'parent': 'len' digits; done() gets them
 * and returns true to close the sheet, false to shake it and start again.
 * cancel may be NULL (no Cancel key). */
typedef bool (*aos_lock_pad_cb)(const char *pin, void *ud);
void aos_lock_pad_open(lv_obj_t *parent, const char *title, int len, aos_lock_pad_cb done, void (*cancel)(void *ud),
                       void *ud);
void aos_lock_pad_close(void);

#ifdef __cplusplus
}
#endif
