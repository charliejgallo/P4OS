/*
 * P4OS - an SSH key asking to be let in (aos_sshkey_ui.c): its fingerprint,
 * over everything once the board is unlocked, until it is answered.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void aos_sshkey_ui_tick(void);      /* aos_ui_tick: opens and closes it */
void aos_sshkey_ui_layout(void);    /* the orientation changed */
bool aos_sshkey_ui_visible(void);
void aos_sshkey_ui_cancel(void);    /* Back and the BOOT button: a no */

#ifdef __cplusplus
}
#endif
