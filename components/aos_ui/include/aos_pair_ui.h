/*
 * P4OS - the Bluetooth pairing request (aos_pair_ui.c): the six-digit code of
 * numeric comparison, over everything, until it is answered.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void aos_pair_ui_tick(void);        /* aos_ui_tick: opens and closes it */
void aos_pair_ui_layout(void);      /* the orientation changed */
bool aos_pair_ui_visible(void);
void aos_pair_ui_cancel(void);      /* Back and the BOOT button: a no */

#ifdef __cplusplus
}
#endif
