/*
 * P4OS - BLE stack: link with the phone and ANCS client.
 *
 * Only the board compiles it. The simulator implements the same HAL API with
 * an imaginary phone (sim/hal_sim.c), and that is why the whole interface
 * -Settings, the bar icon, the overlay- was designed and verified on the Mac
 * before a line of this existed.
 *
 * The caller is aos_hal_esp32.c, which translates the public aos_hal_bt_* and
 * aos_hal_notif_action() calls. Nobody else includes this header.
 *
 * What comes in over ANCS goes out through aos_notif_push()
 * (aos_notif_internal.h), which is the same store and the same policy the
 * simulator uses. Whether a notification is shown is not decided here: that is
 * already written and tested.
 *
 * Everything here runs in NimBLE's host task. NONE of it may touch LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aos_hal.h"
#include "aos_ams.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the controller, the host and advertising. Returns false on
 * failure. Costs ~30 KB of executable RAM: see docs/HANDOFF-BLE-ANCS.md
 * section 2.4. */
bool aos_ble_start(void);

/* Brings it all down and gives the memory back. Measured: 30,352 B return to
 * the pool. */
void aos_ble_stop(void);

bool           aos_ble_running(void);

/* From the heartbeat: resumes advertising if it stopped on its own. */
void           aos_ble_tick(void);
aos_bt_state_t aos_ble_state(void);
const char    *aos_ble_peer(void);
bool           aos_ble_bonded(void);

/* --------------------------------------------------------------------------
 * Control of the phone's music (AMS)
 *
 * Born SWITCHED OFF and turned on from the BT Control app. It is deliberate:
 * notifications are worth more than music, so music control does not go on the
 * air until somebody asks for it, and switching it off removes it entirely
 * -0 is written to the CCCD and the phone stops sending-.
 * -------------------------------------------------------------------------- */
void aos_ble_media_enable(bool on);
bool aos_ble_media_enabled(void);
bool aos_ble_media_ready(void);
bool aos_ble_media_info(aos_ams_state_t *out, uint32_t *pos_s);
bool aos_ble_media_command(int aos_media_cmd);
const char *aos_ble_media_player(void);

/* The phone's battery, 0..100. false if it has not been read yet. */
bool           aos_ble_phone_battery(int *percent);
void           aos_ble_forget(void);

/* Pairing by numeric comparison. */
void     aos_ble_pair_begin(void);
uint32_t aos_ble_pair_code(void);
void     aos_ble_pair_confirm(bool accept);

/* Accept or reject a notification (answer/hang up a call). */
bool aos_ble_notif_action(uint32_t uid, bool positive);

/* ---- the board as a computer's keyboard, mouse and media keys
 * (aos_ble_hid.c). With the mode on, a second connection is allowed: the
 * phone (the one with ANCS) and a computer. ---- */
void aos_ble_hid_enable(bool on);       /* preference; restarts the stack */
bool aos_ble_hid_wanted(void);
bool aos_ble_hid_ready(void);           /* a computer is connected and listens to the keyboard */
bool aos_ble_hid_send(int report_id, const void *data, size_t len);     /* 1 keyboard, 2 media, 3 mouse */
const char *aos_ble_host_name(void);    /* the computer's name, "" when none */

/* between aos_ble.c and aos_ble_hid.c */
void     aos_ble_hid_register(void);
void     aos_ble_hid_subscribe(uint16_t attr, bool notify);
void     aos_ble_hid_host_gone(void);
uint16_t aos_ble_host_conn(void);       /* 0xFFFF when none */
bool     aos_ble_is_phone(uint16_t conn);


#ifdef __cplusplus
}
#endif
