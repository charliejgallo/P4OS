/*
 * P4OS - the devices that join the P4's Zigbee network (aos_zigbee.h,
 * docs/plan/ZIGBEE.md stage 3).
 *
 * The network is closed until asked: aos_zigbee_permit() opens it for some
 * seconds (the BDB's own "open network", which also tells the routers in it
 * to let devices in). A device that joins announces itself (the ZDO's
 * Device_annce: its short and IEEE addresses); then it is "interviewed" the
 * way Zigbee2MQTT does it: its active endpoints, and on the first one, the
 * Basic cluster's manufacturer and model, the strings that name it in
 * Zigbee2MQTT's device list (stage 3's lookup). The read asks for the same
 * attributes Zigbee2MQTT asks for, together: some Tuya devices answer
 * nothing else until they have been asked that one set.
 *
 * Everything that calls the stack runs in its task: the signals and the
 * callbacks are there already, the portal's requests are posted to its task
 * queue. The table is read by the portal's task, under a lock. For now it
 * lives in RAM (PSRAM, psram.lf): after a restart the stack still knows the
 * devices, this list starts empty until they speak again.
 */
#include "aos_zigbee.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_i18n.h"

#include <stdarg.h>
#include <stdio.h>

#include <string.h>
#include "esp_zigbee.h"
#include "ezbee/zdo.h"
#include "ezbee/zcl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "zigbee"

static aos_zigbee_dev_t s_dev[AOS_ZB_DEVICES_MAX];
static int s_n;
static SemaphoreHandle_t s_lock;
static int64_t s_permit_until_ms;

/* What happens, on the screen too: a notice at the bottom, whatever is open
 * (a device joining shows nowhere else, and pairing is done with the
 * device in hand, away from a computer) */
static void notice(const char *fmt, ...)
{
    char t[96];                         /* a toast holds 96 */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    aos_ui_request_toast(t);
}

static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_lock); }

/* the entry of a device by its short address or its IEEE one, or a new one */
static aos_zigbee_dev_t *find(uint16_t nwk, const uint8_t *ieee, bool add)
{
    for (int i = 0; i < s_n; i++)
        if ((ieee && !memcmp(s_dev[i].ieee, ieee, 8)) || (!ieee && s_dev[i].nwk == nwk)) return &s_dev[i];
    if (!add || !ieee) return NULL;
    if (s_n == AOS_ZB_DEVICES_MAX) return NULL;
    aos_zigbee_dev_t *d = &s_dev[s_n++];
    memset(d, 0, sizeof *d);
    memcpy(d->ieee, ieee, 8);
    return d;
}

/* ---- the interview ------------------------------------------------------ */

static void read_basic(uint16_t nwk, uint8_t ep)
{
    /* Zigbee2MQTT's set: manufacturer, ZCL version, app version, model,
     * power source, and 0xFFFE (Tuya's "magic" read) */
    static uint16_t attrs[] = { 0x0004, 0x0000, 0x0001, 0x0005, 0x0007, 0xFFFE };
    ezb_zcl_read_attr_cmd_t cmd = {
        .cmd_ctrl = { .dst_ep = ep, .src_ep = 1, .cluster_id = EZB_ZCL_CLUSTER_ID_BASIC,
                      .manuf_code = EZB_ZCL_STD_MANUF_CODE },
        .payload = { .attr_number = sizeof attrs / sizeof attrs[0], .attr_field = attrs },
    };
    ezb_address_set_short(&cmd.cmd_ctrl.dst_addr, nwk);
    ezb_err_t e = ezb_zcl_read_attr_cmd_req(&cmd);
    aos_hal_log(TAG, "0x%04x: asking its Basic cluster on endpoint %u (%d)", nwk, ep, e);
}

static void on_active_ep(const ezb_zdo_active_ep_req_result_t *res, void *ctx)
{
    uint16_t nwk = (uint16_t)(uintptr_t)ctx;
    if (!res || res->error || !res->rsp || res->rsp->status != EZB_ZDP_STATUS_SUCCESS || !res->rsp->active_ep_count) {
        aos_hal_log(TAG, "0x%04x: no endpoints (%d)", nwk, res ? res->error : -1);
        notice(_("Zigbee: 0x%04x no contestó (dormido): tocale el botón"), nwk);
        lock();
        aos_zigbee_dev_t *d = find(nwk, NULL, false);
        if (d) d->step = AOS_ZB_DEV_FAILED;
        unlock();
        return;
    }
    uint8_t ep = res->rsp->active_ep_list[0];
    lock();
    aos_zigbee_dev_t *d = find(nwk, NULL, false);
    if (d) {
        d->eps = res->rsp->active_ep_count;
        d->ep = ep;
        d->step = AOS_ZB_DEV_BASIC;
    }
    unlock();
    aos_hal_log(TAG, "0x%04x: %u endpoints, the first %u", nwk, res->rsp->active_ep_count, ep);
    read_basic(nwk, ep);
}

static void interview(uint16_t nwk)
{
    ezb_zdo_active_ep_req_t req = {
        .dst_nwk_addr = nwk, .field = { .nwk_addr_of_interest = nwk }, .cb = on_active_ep,
        .user_ctx = (void *)(uintptr_t)nwk,
    };
    ezb_err_t e = ezb_zdo_active_ep_req(&req);
    if (e) aos_hal_log(TAG, "0x%04x: the endpoints request failed (%d)", nwk, e);
}

/* a ZCL character string: its length first */
static void copy_str(char *dst, size_t size, const uint8_t *v)
{
    size_t n = v ? v[0] : 0;
    if (n == 0xFF) n = 0;               /* "invalid" */
    if (n >= size) n = size - 1;
    if (n) memcpy(dst, v + 1, n);
    dst[n] = '\0';
    /* some devices pad with zeros or spaces */
    while (n && (dst[n - 1] == ' ' || dst[n - 1] == '\0')) dst[--n] = '\0';
}

static void on_zcl(ezb_zcl_core_action_callback_id_t id, void *message)
{
    if (id != EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID) return;
    ezb_zcl_cmd_read_attr_rsp_message_t *m = message;
    m->out.result = EZB_ZCL_STATUS_SUCCESS;
    if (!m->in.header || m->in.header->cluster_id != EZB_ZCL_CLUSTER_ID_BASIC) return;
    if (m->in.header->src_addr.addr_mode != EZB_ADDR_MODE_SHORT) return;
    uint16_t nwk = m->in.header->src_addr.u.short_addr;
    lock();
    aos_zigbee_dev_t *d = find(nwk, NULL, false);
    if (d) {
        d->rssi = m->in.header->rssi;
        for (ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v; v = v->next) {
            if (v->status != EZB_ZCL_STATUS_SUCCESS || !v->attr_value) continue;
            if (v->attr_id == 0x0004) copy_str(d->manuf, sizeof d->manuf, v->attr_value);
            else if (v->attr_id == 0x0005) copy_str(d->model, sizeof d->model, v->attr_value);
            else if (v->attr_id == 0x0007) d->power = *(const uint8_t *)v->attr_value;
        }
        d->step = AOS_ZB_DEV_DONE;
        aos_hal_log(TAG, "0x%04x: \"%s\" \"%s\", power source %u", nwk, d->manuf, d->model, d->power);
        notice("Zigbee: %s %s", d->manuf[0] ? d->manuf : "?", d->model);
    }
    unlock();
}

/* ---- from the stack's signals (aos_zigbee_p4.c) ------------------------- */

void aos_zigbee_devices_init(void)
{
    ezb_zcl_core_action_handler_register(on_zcl);
}

bool aos_zigbee_devices_signal(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t t = ezb_app_signal_get_type(sig);
    const void *p = ezb_app_signal_get_params(sig);
    switch (t) {
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *a = p;
        if (!a) return true;
        lock();
        aos_zigbee_dev_t *d = find(a->short_addr, a->device_addr.u8, true);
        if (d) {
            d->nwk = a->short_addr;
            d->cap = a->capability;
            d->seen_ms = (uint32_t)aos_hal_uptime_ms();
            if (d->step != AOS_ZB_DEV_DONE) d->step = AOS_ZB_DEV_EPS;
        }
        unlock();
        const uint8_t *e = a->device_addr.u8;
        aos_hal_log(TAG, "a device announced itself: 0x%04x, %02x%02x%02x%02x%02x%02x%02x%02x, capability 0x%02x",
                    a->short_addr, e[7], e[6], e[5], e[4], e[3], e[2], e[1], e[0], a->capability);
        if (d && d->step != AOS_ZB_DEV_DONE) {
            notice(_("Zigbee: entró un equipo (0x%04x), preguntándole qué es"), a->short_addr);
            interview(a->short_addr);
        }
        return true;
    }
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *a = p;
        if (a) aos_hal_log(TAG, "0x%04x authorized: type %u, status %u", a->short_addr, a->type, a->status);
        return true;
    }
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const ezb_zdo_signal_device_update_params_t *u = p;
        if (u) aos_hal_log(TAG, "0x%04x update: status %u, parent 0x%04x", u->short_addr, u->status, u->parent_short);
        return true;
    }
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *l = p;
        if (!l) return true;
        lock();
        aos_zigbee_dev_t *d = find(l->short_addr, l->device_addr.u8, false);
        if (d) d->step = AOS_ZB_DEV_LEFT;
        unlock();
        aos_hal_log(TAG, "0x%04x left the network (rejoin %u)", l->short_addr, l->leave_type);
        notice(_("Zigbee: 0x%04x salió de la red"), l->short_addr);
        return true;
    }
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const ezb_nwk_signal_permit_join_status_params_t *j = p;
        if (!j) return true;
        s_permit_until_ms = j->duration ? (int64_t)aos_hal_uptime_ms() + (int64_t)j->duration * 1000 : 0;
        aos_hal_log(TAG, j->duration ? "the network is open for %u s" : "the network is closed", j->duration);
        if (j->duration) notice(_("Zigbee: se pueden sumar equipos por %u s"), j->duration);
        else notice("%s", _("Zigbee: ya no se pueden sumar equipos"));
        return true;
    }
    default:
        return false;
    }
}

/* ---- the portal's side -------------------------------------------------- */

static void permit(void *ctx)
{
    uint8_t s = (uint8_t)(uintptr_t)ctx;
    if (s) aos_hal_log(TAG, "opening the network for %u s: %d", s, ezb_bdb_open_network(s));
    else aos_hal_log(TAG, "closing the network: %d", ezb_bdb_close_network());
}

bool aos_zigbee_permit(int seconds)
{
    aos_zigbee_info_t z;
    aos_zigbee_info(&z);
    if (z.state != AOS_ZB_FORMED) return false;
    if (seconds < 0) seconds = 0;
    if (seconds > 254) seconds = 254;   /* 255 would be "for ever" */
    return esp_zigbee_task_queue_post(permit, (void *)(uintptr_t)seconds) == ESP_OK;
}

int aos_zigbee_permit_left(void)
{
    int64_t left = s_permit_until_ms - (int64_t)aos_hal_uptime_ms();
    return left > 0 ? (int)((left + 999) / 1000) : 0;
}

int aos_zigbee_devices(aos_zigbee_dev_t *out, int max)
{
    lock();
    int n = s_n < max ? s_n : max;
    memcpy(out, s_dev, (size_t)n * sizeof *out);
    unlock();
    return n;
}
