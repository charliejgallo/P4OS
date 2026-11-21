/*
 * P4OS - a new board with a backup on its card (aos_backup_ui.c): the offer
 * to look at it, once, over the home screen.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void aos_backup_ui_tick(void);      /* aos_ui_tick: looks once, a few seconds after the start */
void aos_backup_ui_layout(void);    /* the orientation changed */
bool aos_backup_ui_visible(void);
void aos_backup_ui_cancel(void);    /* Back and the BOOT button: not now */

#ifdef __cplusplus
}
#endif
