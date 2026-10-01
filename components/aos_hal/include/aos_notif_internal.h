/*
 * AmoledOS - entry door of the notification store.
 *
 * Used by the providers, and by nobody else: the ANCS client on the board and
 * the simulator's imaginary phone. The UI and the apps see only aos_hal.h.
 *
 * It lives apart from aos_hal.h precisely for that: if this were in the public
 * header, anybody could invent a notification.
 */
#pragma once

#include "aos_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hands over a freshly arrived notification. The store applies the policy -do
 * not disturb, category filter, the phone's silence, calls always-, fills in
 * 'alert' and 'sound', and leaves it in the queue and in the history. Returns
 * false if the filter discarded it.
 *
 * Every field of 'in' is read except 'alert' and 'sound', which are
 * overwritten.
 *
 * Callable from any task. */
bool aos_notif_push(const aos_notif_t *in);

/* The phone withdrew a notification (the user dismissed it over there). */
void aos_notif_push_removed(uint32_t uid);

/* A provider that can dismiss at the source (Home Assistant's persistent
 * notifications) hears about every one the user dismisses HERE: one at a
 * time with aos_hal_notif_remove, or all with aos_hal_notif_clear. Not
 * called for the ones the provider itself withdrew (aos_notif_push_removed),
 * so nothing echoes. Runs in the caller's task (the UI's): queue, don't block. */
typedef void (*aos_notif_dismiss_hook_t)(uint32_t uid);
void aos_notif_set_dismiss_hook(aos_notif_dismiss_hook_t fn);

/* On disconnecting: the pending queue stops making sense. The history is
 * kept. */
void aos_notif_reset_pending(void);

/* The phone rejected the last action. The interface picks it up with
 * aos_hal_notif_action_failed(). */
void aos_notif_action_failed(void);

#ifdef __cplusplus
}
#endif
