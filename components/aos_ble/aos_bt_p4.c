/*
 * P4OS - the HAL's Bluetooth and phone-media functions, on aos_ble.
 *
 * The HAL only translates, as on the watch (AmoledOS' aos_hal_esp32.c): the
 * stack lives in aos_ble.c, and what comes in over ANCS goes out through
 * aos_notif_push(), the same store and policy the simulator runs. These
 * replace the weak stubs in aos_hal_stubs.c (this component is a whole
 * archive).
 *
 * The phone's music (AMS) answers aos_hal_media_*: the card's own player
 * has its functions of its own (aos_hal_player_*), so there is no clash.
 *
 * Bluetooth needs the link to the C6 up, so it starts from net_task once the
 * radio is (aos_bt_p4_radio_up), and only if the preference says so: born
 * off, like the watch's. It costs no executable RAM here (the P4 runs the
 * apps' code from PSRAM). Measured on the board (2026-10-03): 6 KB of
 * internal RAM in the firmware (4.5 KB of IRAM, the rest .bss), and 7 KB more
 * with it on, 5 of them the stack of NimBLE's host task. That stack stays
 * internal: the task writes the phone's keys to NVS, and a PSRAM stack
 * cannot be used while the flash is written. NimBLE's own memory is in
 * PSRAM (BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL).
 */
#include "aos_hal.h"
#include "aos_ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "bt";

/* ---- the phone's music (AMS) ---- */

void aos_hal_media_enable(bool enable) { aos_ble_media_enable(enable); }
bool aos_hal_media_enabled(void) { return aos_ble_media_enabled(); }

aos_media_link_t aos_hal_media_link(void)
{
    if (!aos_ble_media_enabled()) return AOS_MEDIA_OFF;
    /* CONNECTED means AMS subscribed and answering, not just a phone on the
     * other end: without AMS there is nothing to show and nothing to send */
    return aos_ble_media_ready() ? AOS_MEDIA_CONNECTED : AOS_MEDIA_ADVERTISING;
}

const char *aos_hal_media_peer(void) { return aos_ble_peer(); }
const char *aos_hal_media_player(void) { return aos_ble_media_player(); }

bool aos_hal_media_info(aos_media_info_t *out)
{
    aos_ams_state_t st;
    uint32_t pos = 0;
    if (!out || !aos_ble_media_info(&st, &pos)) return false;
    memset(out, 0, sizeof *out);
    snprintf(out->title, sizeof out->title, "%s", st.title);
    snprintf(out->artist, sizeof out->artist, "%s", st.artist);
    snprintf(out->album, sizeof out->album, "%s", st.album);
    out->playing = st.playing;
    out->has_metadata = st.hay_datos;
    out->duration_s = st.duration_s;
    out->position_s = pos;
    return true;
}

bool aos_hal_media_command(aos_media_cmd_t cmd) { return aos_ble_media_command((int)cmd); }

/* ---- the link ---- */

void aos_hal_bt_enable(bool on)
{
    aos_hal_pref_set_i32("bt_on", on ? 1 : 0);
    if (!on) {
        aos_ble_stop();
    } else if (!aos_ble_start()) {
        ESP_LOGE(TAG, "could not bring up the BLE stack");
    }
}

bool aos_hal_bt_enabled(void)
{
    /* the preference, not the stack: it is what the switch shows while the
     * stack is still coming up, and what decides the next boot */
    int32_t v = 0;
    aos_hal_pref_get_i32("bt_on", &v);
    return v != 0;
}

aos_bt_state_t aos_hal_bt_state(void) { return aos_ble_state(); }
const char *aos_hal_bt_peer(void) { return aos_ble_peer(); }
bool aos_hal_bt_bonded(void) { return aos_ble_bonded(); }
bool aos_hal_bt_phone_battery(int *p) { return aos_ble_phone_battery(p); }
void aos_hal_bt_forget(void) { aos_ble_forget(); }

void aos_hal_bt_pair_begin(void) { aos_ble_pair_begin(); }
uint32_t aos_hal_bt_pair_code(void) { return aos_ble_pair_code(); }
void aos_hal_bt_pair_confirm(bool accept) { aos_ble_pair_confirm(accept); }
void aos_hal_bt_pair_cancel(void) { aos_ble_pair_confirm(false); }

bool aos_hal_notif_action(uint32_t uid, bool positive) { return aos_ble_notif_action(uid, positive); }

/* ---- the keyboard mode (aos_ble_hid.c) ---- */

void aos_hal_bt_keyboard_enable(bool on) { aos_ble_hid_enable(on); }
bool aos_hal_bt_keyboard_enabled(void) { return aos_ble_hid_wanted(); }
const char *aos_hal_bt_keyboard_host(void) { return aos_ble_host_name(); }
bool aos_hal_bt_keyboard_ready(void) { return aos_ble_hid_ready(); }

/* main.c's loop, every 3 s: the phone's time is applied here and not in
 * NimBLE's task, and advertising that stopped by itself comes back */
void aos_bt_p4_tick(void) { aos_ble_tick(); }

/* net_task, once the link to the C6 is up (aos_net_p4.c) */
void aos_bt_p4_radio_up(void)
{
    if (!aos_hal_bt_enabled()) {
        ESP_LOGI(TAG, "bluetooth off by preference");
        return;
    }
    if (!aos_ble_start()) ESP_LOGE(TAG, "could not bring up the BLE stack");
}
