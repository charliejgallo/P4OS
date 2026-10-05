/*
 * MONSTER HOP - the panels: title, the world map, Tommy's house (wardrobe,
 * shop, sticker album, trophies, stats), settings, loading, pause, results
 *
 * Pictures come from the pack: the worker unpacks and colours them into
 * RGB565A8 buffers (mh_ui_job, no LVGL there) and the LVGL side only wraps
 * them in image descriptors (mh_ui_job_done). The world map (5.9 MB at the
 * P4's 720-pixel column) is let go while a level plays and comes back after.
 *
 * P4OS: every panel is laid out for the screen it gets, standing up (a
 * column) or lying down (the desktop's two sides), and again when it turns
 * (mh_ui_layout). Nothing is at the watch's 368 x 448 positions.
 */
#pragma once

#include "aos_pad.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct app app_t;

enum { UJ_MENU = 1, UJ_TURN, UJ_CARDS, UJ_FREE_MAP };

void mh_ui_build(app_t *a, lv_obj_t *root);
/* the screen turned: the panels again for the root's new size, the state's
 * shown and filled as it was */
void mh_ui_layout(app_t *a);
void mh_ui_show(app_t *a, int state);           /* the panel of a state      */
void mh_ui_tick(app_t *a, int dt_ms);
void mh_ui_free(app_t *a);
/* a USB gamepad on the panel showing, every tick (the play's own input is
 * monsterhop.c's): the d-pad through its buttons, A clicks, B goes back */
void mh_ui_gamepad(app_t *a, const aos_pad_t *p);

/* the worker's half of a picture job, and the LVGL half once it is done */
void mh_ui_job(app_t *a, int what);
void mh_ui_job_done(app_t *a, int what);
void mh_ui_before_job(app_t *a, int what);    /* LVGL: drop what it will rewrite */

void mh_ui_pause_fill(app_t *a);                /* the level map in the pause */
void mh_ui_result_fill(app_t *a, bool won, int stars, int earned, bool best, bool sticker, uint32_t new_trophies);
void mh_ui_boot_text(app_t *a, const char *txt);
void mh_ui_loading_text(app_t *a, const char *txt);
/* the loading bar of the boot or the level panel: hidden, or at pct */
void mh_ui_load_bar(app_t *a, bool show, int pct);
/* the wardrobe (buy = false) or the shop */
void mh_ui_open_shop(app_t *a, bool buy);
/* the lobby: what it says, the level picked, and the host's buttons */
void mh_ui_lobby_fill(app_t *a, const char *txt, const char *level, bool host);
/* a race's end: outcome 1 won, 0 lost, -1 left */
void mh_ui_race_fill(app_t *a, int outcome, int me, int them, int earned, uint32_t new_trophies);
