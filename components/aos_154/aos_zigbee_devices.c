/*
 * P4OS - the devices that join the P4's Zigbee network, and what they say
 * (aos_zigbee.h, docs/plan/ZIGBEE.md stage 3).
 *
 * The network is closed until asked: aos_zigbee_permit() opens it for some
 * seconds (the BDB's own "open network", which also tells the routers in it
 * to let devices in). A device that joins announces itself (the ZDO's
 * Device_annce: its short and IEEE addresses), and is then set up the way
 * Zigbee2MQTT sets one up, one request at a time (its "jobs", below):
 *
 *   - its active endpoints;
 *   - the Basic cluster of the first: manufacturer and model, the strings
 *     that name it in Zigbee2MQTT's device list, asked together with the
 *     rest of the set Zigbee2MQTT asks (some Tuya devices answer nothing
 *     else until they have been asked that one set);
 *   - each endpoint's simple descriptor: the clusters it serves;
 *   - for each cluster this reads (battery, on/off, temperature, humidity,
 *     occupancy, IAS zone), a binding to the board, so its reports come
 *     here: a Sonoff sensor, for one, reports only to what it is bound to;
 *   - for an IAS zone (door, motion, water sensors), the board as its alarm
 *     panel (the CIE address written into it) and an enroll response: many
 *     send nothing until they are enrolled;
 *   - a first read of each value.
 *
 * Then whatever comes in, reports, read responses, the IAS zone's status
 * changes, is parsed from the raw ZCL frame (the stack's raw handler, which
 * sees every cluster, whether the board's endpoint has it or not) into the
 * device's values. A frame from a device this list does not know (after a
 * restart: the stack keeps its network, this list starts empty) starts its
 * setup again.
 *
 * Everything that calls the stack runs in its task: the signals and the
 * callbacks are there already, the portal's requests and the jobs' timer are
 * posted to its task queue. The table is read by the portal's task, under a
 * lock. It lives in RAM (PSRAM, psram.lf).
 */
#include "aos_zigbee.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_i18n.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zdo.h"
#include "ezbee/zcl.h"
#include "ezbee/zcl/cluster/power_config_desc.h"
#include "ezbee/zcl/cluster/on_off_desc.h"
#include "ezbee/zcl/cluster/temperature_measurement_desc.h"
#include "ezbee/zcl/cluster/rel_humidity_measurement_desc.h"
#include "ezbee/zcl/cluster/occupancy_sensing_desc.h"
#include "ezbee/zcl/cluster/ias_zone_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "zigbee"

#define CL_BASIC 0x0000
#define CL_POWER 0x0001
#define CL_ONOFF 0x0006
#define CL_TEMP  0x0402
#define CL_HUM   0x0405
#define CL_OCC   0x0406
#define CL_IAS   0x0500

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

static int find_nwk(uint16_t nwk)
{
    for (int i = 0; i < s_n; i++)
        if (s_dev[i].nwk == nwk && s_dev[i].step != AOS_ZB_DEV_LEFT) return i;
    return -1;
}

static int find_ieee(const uint8_t *ieee)
{
    for (int i = 0; i < s_n; i++)
        if (!memcmp(s_dev[i].ieee, ieee, 8)) return i;
    return -1;
}

/* a new entry, with its values unknown */
static int add(const uint8_t *ieee, uint16_t nwk)
{
    if (s_n == AOS_ZB_DEVICES_MAX) return -1;
    aos_zigbee_dev_t *d = &s_dev[s_n];
    memset(d, 0, sizeof *d);
    memcpy(d->ieee, ieee, 8);
    d->nwk = nwk;
    d->temp = INT16_MIN;
    d->hum = 0xFFFF;
    d->battery = 0xFF;
    d->occupancy = 0xFF;
    d->zone_type = 0xFFFF;
    return s_n++;
}

static int ep_index(const aos_zigbee_dev_t *d, uint8_t ep)
{
    for (int i = 0; i < d->neps; i++)
        if (d->eps_list[i] == ep) return i;
    return -1;
}

/* ---- the jobs: one request at a time per device ------------------------- */

/* A sleepy device holds its parent's queue for it short, and answers right
 * after it joined or spoke: its requests go out one after another, each on
 * the answer to the one before, or after JOB_WAIT_MS without one. */
enum { J_ACTIVE, J_BASIC, J_SIMPLE, J_BIND, J_CIE, J_ENROLL, J_READ };
typedef struct { uint8_t type, ep; uint16_t cluster; } job_t;
#define JOBS        32
#define JOB_WAIT_MS 5000

static struct {
    job_t   q[JOBS];
    uint8_t head, n;
    bool    busy;
    int64_t since_ms;
} s_jobs[AOS_ZB_DEVICES_MAX];

static void run(int i);

static void push(int i, uint8_t type, uint8_t ep, uint16_t cluster)
{
    if (s_jobs[i].n == JOBS) return;
    s_jobs[i].q[(s_jobs[i].head + s_jobs[i].n) % JOBS] = (job_t){ type, ep, cluster };
    s_jobs[i].n++;
}

/* the job that is running ended (answered, failed or timed out) */
static void done(int i)
{
    if (i < 0 || !s_jobs[i].busy) return;
    s_jobs[i].busy = false;
    s_jobs[i].head = (uint8_t)((s_jobs[i].head + 1) % JOBS);
    s_jobs[i].n--;
    run(i);
}

/* an answer that ends the running job only if it is the one it waits for */
static void done_if(int i, uint8_t type, uint16_t cluster)
{
    if (i < 0 || !s_jobs[i].busy) return;
    const job_t *j = &s_jobs[i].q[s_jobs[i].head];
    if (j->type == type && (type != J_READ || j->cluster == cluster)) done(i);
}

static void on_zdo_cb(void *ctx) { done((int)(uintptr_t)ctx); }

static void on_active_ep(const ezb_zdo_active_ep_req_result_t *res, void *ctx)
{
    int i = (int)(uintptr_t)ctx;
    aos_zigbee_dev_t *d = &s_dev[i];
    if (!res || res->error || !res->rsp || res->rsp->status != EZB_ZDP_STATUS_SUCCESS || !res->rsp->active_ep_count) {
        aos_hal_log(TAG, "0x%04x: no endpoints (%d)", d->nwk, res ? res->error : -1);
        notice(_("Zigbee: 0x%04x no contestó (dormido): tocale el botón"), d->nwk);
        lock();
        d->step = AOS_ZB_DEV_FAILED;
        unlock();
        done(i);
        return;
    }
    lock();
    d->eps = res->rsp->active_ep_count;
    d->neps = 0;
    for (int k = 0; k < d->eps && d->neps < sizeof d->eps_list; k++) d->eps_list[d->neps++] = res->rsp->active_ep_list[k];
    d->ep = d->eps_list[0];
    d->step = AOS_ZB_DEV_BASIC;
    unlock();
    aos_hal_log(TAG, "0x%04x: %u endpoints, the first %u", d->nwk, d->eps, d->ep);
    push(i, J_BASIC, d->ep, CL_BASIC);
    for (int k = 0; k < d->neps; k++) push(i, J_SIMPLE, d->eps_list[k], 0);
    done(i);
}

static void on_simple(const ezb_zdo_simple_desc_req_result_t *res, void *ctx)
{
    int i = (int)(uintptr_t)ctx;
    aos_zigbee_dev_t *d = &s_dev[i];
    if (res && !res->error && res->rsp && res->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        const ezb_af_simple_desc_t *sd = &res->rsp->desc;
        uint8_t ep = sd->ep_id;
        for (int k = 0; k < sd->app_input_cluster_count; k++) {
            uint16_t c = sd->app_cluster_list[k];
            uint16_t bit = c == CL_POWER ? AOS_ZB_CL_POWER : c == CL_ONOFF ? AOS_ZB_CL_ONOFF : c == CL_TEMP ? AOS_ZB_CL_TEMP
                         : c == CL_HUM ? AOS_ZB_CL_HUM : c == CL_OCC ? AOS_ZB_CL_OCC : c == CL_IAS ? AOS_ZB_CL_IAS : 0;
            lock();
            d->clusters |= bit;
            if (d->ncl < AOS_ZB_CLUSTERS) d->cl[d->ncl++] = (uint32_t)ep << 16 | c;
            unlock();
            /* any device, not only the ones the board knows: every cluster
             * that may report something is bound to the board, so what it
             * says comes here (and is kept raw if the board cannot name it);
             * not those that only take commands or are the device's own */
            bool quiet = c == CL_BASIC || c == 0x0003 || c == 0x0004 || c == 0x0005 || c == 0x000a || c == 0x0019 ||
                         c == 0x0020 || c == 0x1000;
            if (!quiet) push(i, J_BIND, ep, c);
            if (c == CL_IAS) {
                push(i, J_CIE, ep, c);
                push(i, J_ENROLL, ep, c);
            }
            if (bit) push(i, J_READ, ep, c);
        }
        aos_hal_log(TAG, "0x%04x: endpoint %u, profile 0x%04x, device 0x%04x, %u in clusters", d->nwk, ep,
                    sd->app_profile_id, sd->app_device_id, sd->app_input_cluster_count);
    }
    else aos_hal_log(TAG, "0x%04x: no simple descriptor (%d)", d->nwk, res ? res->error : -1);
    done(i);
}

static void on_bind(const ezb_zdp_bind_req_result_t *res, void *ctx)
{
    int i = (int)(uintptr_t)ctx;
    const job_t *j = &s_jobs[i].q[s_jobs[i].head];
    aos_hal_log(TAG, "0x%04x: binding 0x%04x on %u to the board: %d/%d", s_dev[i].nwk, j->cluster, j->ep,
                res ? res->error : -1, res && res->rsp ? res->rsp->status : -1);
    done(i);
}

static void on_cnf(ezb_af_user_cnf_t *cnf, void *ctx)
{
    (void)cnf;
    on_zdo_cb(ctx);
}

/* the attributes read from each cluster */
static int attrs_of(uint16_t cluster, uint16_t *out)
{
    switch (cluster) {
    case CL_BASIC: {
        /* Zigbee2MQTT's set: manufacturer, ZCL version, app version, model,
         * power source, and 0xFFFE (Tuya's "magic" read) */
        static const uint16_t a[] = { 0x0004, 0x0000, 0x0001, 0x0005, 0x0007, 0xFFFE };
        memcpy(out, a, sizeof a);
        return 6;
    }
    case CL_POWER: out[0] = 0x0021; out[1] = 0x0020; return 2;     /* battery %, voltage */
    case CL_IAS:   out[0] = 0x0001; out[1] = 0x0002; return 2;     /* zone type, zone status */
    default:       out[0] = 0x0000; return 1;                      /* the measured value / on-off */
    }
}

static void start_job(int i)
{
    aos_zigbee_dev_t *d = &s_dev[i];
    const job_t *j = &s_jobs[i].q[s_jobs[i].head];
    void *ctx = (void *)(uintptr_t)i;
    ezb_err_t e = EZB_ERR_NONE;
    switch (j->type) {
    case J_ACTIVE: {
        ezb_zdo_active_ep_req_t r = { .dst_nwk_addr = d->nwk, .field = { .nwk_addr_of_interest = d->nwk },
                                      .cb = on_active_ep, .user_ctx = ctx };
        e = ezb_zdo_active_ep_req(&r);
        break;
    }
    case J_SIMPLE: {
        ezb_zdo_simple_desc_req_t r = { .dst_nwk_addr = d->nwk,
                                        .field = { .nwk_addr_of_interest = d->nwk, .endpoint = j->ep },
                                        .cb = on_simple, .user_ctx = ctx };
        e = ezb_zdo_simple_desc_req(&r);
        break;
    }
    case J_BIND: {
        ezb_zdo_bind_req_t r = { .dst_nwk_addr = d->nwk, .cb = on_bind, .user_ctx = ctx };
        memcpy(r.field.src_addr.u8, d->ieee, 8);
        r.field.src_ep = j->ep;
        r.field.cluster_id = j->cluster;
        r.field.dst_addr_mode = 0x03;
        ezb_nwk_get_extended_address(&r.field.dst_addr.extended_addr);
        r.field.dst_ep = 1;
        e = ezb_zdo_bind_req(&r);
        break;
    }
    case J_CIE: {
        /* the IAS zone's CIE address (0x0010): the board's IEEE address */
        static ezb_extaddr_t me;
        ezb_nwk_get_extended_address(&me);
        ezb_zcl_attribute_t a = { .id = 0x0010, .data = { .type = EZB_ZCL_ATTR_TYPE_EUI64, .size = 8, .value = &me } };
        ezb_zcl_write_attr_cmd_t w = {
            .cmd_ctrl = { .dst_ep = j->ep, .src_ep = 1, .cluster_id = CL_IAS, .manuf_code = EZB_ZCL_STD_MANUF_CODE,
                          .cnf_ctx = { .cb = on_cnf, .user_ctx = ctx } },
            .payload = { .attr_number = 1, .attr_field = &a },
        };
        ezb_address_set_short(&w.cmd_ctrl.dst_addr, d->nwk);
        e = ezb_zcl_write_attr_cmd_req(&w);
        break;
    }
    case J_ENROLL: {
        ezb_zcl_ias_zone_enroll_rsp_cmd_t r = {
            .cmd_ctrl = { .dst_ep = j->ep, .src_ep = 1, .cnf_ctx = { .cb = on_cnf, .user_ctx = ctx } },
            .payload = { .enroll_rsp_code = 0, .zone_id = (uint8_t)i },
        };
        ezb_address_set_short(&r.cmd_ctrl.dst_addr, d->nwk);
        e = ezb_zcl_ias_zone_enroll_cmd_resp(&r);
        break;
    }
    case J_BASIC:
    case J_READ: {
        static uint16_t attrs[8];
        int n = attrs_of(j->cluster, attrs);
        ezb_zcl_read_attr_cmd_t r = {
            .cmd_ctrl = { .dst_ep = j->ep, .src_ep = 1, .cluster_id = j->cluster, .manuf_code = EZB_ZCL_STD_MANUF_CODE },
            .payload = { .attr_number = (uint8_t)n, .attr_field = attrs },
        };
        ezb_address_set_short(&r.cmd_ctrl.dst_addr, d->nwk);
        e = ezb_zcl_read_attr_cmd_req(&r);
        break;
    }
    }
    if (e) {
        aos_hal_log(TAG, "0x%04x: request %u (0x%04x on %u) not sent: %d", d->nwk, j->type, j->cluster, j->ep, e);
        done(i);
    }
}

static void run(int i)
{
    lock();
    s_dev[i].setup = s_jobs[i].n;
    unlock();
    if (s_jobs[i].busy || !s_jobs[i].n) return;
    s_jobs[i].busy = true;
    s_jobs[i].since_ms = (int64_t)aos_hal_uptime_ms();
    start_job(i);
}

/* every second, in the stack's task: a job with no answer is let go */
static void tick(void *ctx)
{
    (void)ctx;
    int64_t now = (int64_t)aos_hal_uptime_ms();
    for (int i = 0; i < s_n; i++)
        if (s_jobs[i].busy && now - s_jobs[i].since_ms > JOB_WAIT_MS) {
            const job_t *j = &s_jobs[i].q[s_jobs[i].head];
            aos_hal_log(TAG, "0x%04x: no answer to request %u (0x%04x on %u)", s_dev[i].nwk, j->type, j->cluster, j->ep);
            done(i);
        }
}

static void tick_timer(void *arg)
{
    (void)arg;
    esp_zigbee_task_queue_post(tick, NULL);
}

static void setup(int i)
{
    s_jobs[i].head = s_jobs[i].n = 0;
    s_jobs[i].busy = false;
    push(i, J_ACTIVE, 0, 0);
    run(i);
}

/* ---- what the devices say ----------------------------------------------- */

/* the size of a ZCL value of a type, or -1 for one not parsed here */
static int type_size(uint8_t t, const uint8_t *v, int left)
{
    switch (t) {
    case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: return 1;
    case 0x09: case 0x19: case 0x21: case 0x29: case 0x31: case 0x38: case 0xe8: case 0xe9: return 2;
    case 0x0a: case 0x1a: case 0x22: case 0x2a: return 3;
    case 0x0b: case 0x1b: case 0x23: case 0x2b: case 0x39: case 0xe0: case 0xe1: case 0xe2: case 0xea: return 4;
    case 0x24: case 0x2c: return 5;
    case 0x25: case 0x2d: return 6;
    case 0x26: case 0x2e: return 7;
    case 0x27: case 0x2f: case 0x3a: case 0xf0: return 8;
    case 0xf1: return 16;
    case 0x41: case 0x42: return left < 1 ? -1 : 1 + (v[0] == 0xFF ? 0 : v[0]);
    case 0x43: case 0x44: return left < 2 ? -1 : 2 + ((v[0] | v[1] << 8) == 0xFFFF ? 0 : (v[0] | v[1] << 8));
    default: return -1;
    }
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

static const char *zone_word(uint16_t type, bool alarm)
{
    switch (type) {
    case 0x0015: return alarm ? _("abierto") : _("cerrado");                    /* contact switch */
    case 0x000d: return alarm ? _("movimiento") : _("sin movimiento");          /* motion sensor */
    case 0x002a: return alarm ? _("¡agua!") : _("seco");                        /* water sensor */
    default:     return alarm ? _("alarma") : _("normal");
    }
}

static void zone_status(aos_zigbee_dev_t *d, uint16_t st)
{
    bool was = d->zone_known && (d->zone_status & 1), now = st & 1;
    bool first = !d->zone_known;
    d->zone_status = st;
    d->zone_known = 1;
    if (first || was != now)
        notice("Zigbee: %s %s", d->model[0] ? d->model : d->manuf, zone_word(d->zone_type, now));
}

/* what a device said, raw: the last of each cluster, attribute and endpoint
 * (a cluster-specific command is kept as type 0xFF, its id as the attribute:
 * Tuya's own cluster, 0xEF00, speaks only so) */
static void keep_raw(aos_zigbee_dev_t *d, uint16_t cl, uint8_t ep, uint16_t id, uint8_t type, const uint8_t *v, int size)
{
    int k;
    for (k = 0; k < d->nattr; k++)
        if (d->attr[k].cluster == cl && d->attr[k].attr == id && d->attr[k].ep == ep) break;
    if (k == d->nattr) {
        if (d->nattr < AOS_ZB_ATTRS) d->nattr++;
        else {                          /* full: the oldest gives way */
            k = 0;
            for (int m = 1; m < AOS_ZB_ATTRS; m++)
                if (d->attr[m].ms < d->attr[k].ms) k = m;
        }
    }
    aos_zigbee_attr_t *a = &d->attr[k];
    a->cluster = cl;
    a->attr = id;
    a->ep = ep;
    a->type = type;
    a->len = (uint8_t)(size > (int)sizeof a->v ? sizeof a->v : size);
    memcpy(a->v, v, a->len);
    a->ms = (uint32_t)aos_hal_uptime_ms();
}

/* one attribute's value, into the device (under the lock) */
static void apply(aos_zigbee_dev_t *d, uint16_t cl, uint8_t ep, uint16_t id, uint8_t type, const uint8_t *v, int size)
{
    uint16_t u16 = size >= 2 ? (uint16_t)(v[0] | v[1] << 8) : (size ? v[0] : 0);
    if (cl != CL_BASIC) keep_raw(d, cl, ep, id, type, v, size);
    switch (cl) {
    case CL_BASIC:
        if (id == 0x0004) copy_str(d->manuf, sizeof d->manuf, v);
        else if (id == 0x0005) copy_str(d->model, sizeof d->model, v);
        else if (id == 0x0007) d->power = v[0];
        break;
    case CL_POWER:
        if (id == 0x0021) d->battery = v[0];
        else if (id == 0x0020) d->voltage = v[0];
        break;
    case CL_ONOFF:
        if (id == 0x0000) {
            int k = ep_index(d, ep);
            if (k < 0) break;
            bool was = (d->onoff >> k) & 1, known = (d->onoff_known >> k) & 1;
            if (v[0]) d->onoff |= (uint8_t)(1 << k);
            else d->onoff &= (uint8_t)~(1 << k);
            d->onoff_known |= (uint8_t)(1 << k);
            if (!known || was != (v[0] != 0))
                notice(_("Zigbee: %s, salida %u %s"), d->model, (unsigned)(k + 1), v[0] ? _("prendida") : _("apagada"));
        }
        break;
    case CL_TEMP:
        if (id == 0x0000) d->temp = (int16_t)u16;
        break;
    case CL_HUM:
        if (id == 0x0000) d->hum = u16;
        break;
    case CL_OCC:
        if (id == 0x0000) d->occupancy = v[0];
        break;
    case CL_IAS:
        if (id == 0x0001) d->zone_type = u16;
        else if (id == 0x0002) zone_status(d, u16);
        break;
    }
}

/* every ZCL frame that comes in, before the stack goes on with it */
static bool on_raw(const ezb_zcl_raw_frame_t *f)
{
    const ezb_zcl_cmd_hdr_t *h = f ? f->header : NULL;
    if (!h || h->src_addr.addr_mode != EZB_ADDR_MODE_SHORT) return false;
    uint16_t nwk = h->src_addr.u.short_addr, cl = h->cluster_id;
    bool specific = (h->fc & 0x03) == 1;
    const uint8_t *p = f->payload;
    int n = f->payload_length;

    lock();
    int i = find_nwk(nwk);
    bool fresh = false;
    if (i < 0) {
        /* one this list does not know: one that joined before a restart */
        ezb_extaddr_t ext;
        if (ezb_address_extended_by_short(nwk, &ext) == EZB_ERR_NONE) {
            i = find_ieee(ext.u8);
            if (i >= 0) s_dev[i].nwk = nwk;
            else if ((i = add(ext.u8, nwk)) >= 0) fresh = true;
        }
    }
    if (i < 0) {
        unlock();
        return false;
    }
    aos_zigbee_dev_t *d = &s_dev[i];
    d->heard_ms = (uint32_t)aos_hal_uptime_ms();
    if (h->rssi) d->rssi = h->rssi;
    bool is_read = false;
    if (!specific && (h->cmd_id == 0x0A || h->cmd_id == 0x01)) {
        /* report attributes (0x0A): id, type, value; read attributes
         * response (0x01): id, status, and type and value on success */
        is_read = h->cmd_id == 0x01;
        while (n >= 3) {
            uint16_t id = (uint16_t)(p[0] | p[1] << 8);
            p += 2;
            n -= 2;
            if (is_read) {
                uint8_t st = *p++;
                n--;
                if (st) continue;
                if (n < 1) break;
            }
            uint8_t t = *p++;
            n--;
            int s = type_size(t, p, n);
            if (s < 0 || s > n) break;
            apply(d, cl, h->src_ep, id, t, p, s);
            p += s;
            n -= s;
        }
    }
    else if (specific && cl == CL_IAS && h->cmd_id == 0x00 && n >= 2)
        zone_status(d, (uint16_t)(p[0] | p[1] << 8));       /* zone status change notification */
    if (specific && !(cl == CL_IAS && h->cmd_id == 0x01)) keep_raw(d, cl, h->src_ep, h->cmd_id, 0xFF, f->payload, f->payload_length);
    bool enroll = specific && cl == CL_IAS && h->cmd_id == 0x01;
    char manuf[33], model[33];
    bool named = is_read && cl == CL_BASIC;
    if (named) {
        d->step = AOS_ZB_DEV_DONE;
        memcpy(manuf, d->manuf, sizeof manuf);
        memcpy(model, d->model, sizeof model);
    }
    unlock();

    if (fresh) {
        aos_hal_log(TAG, "0x%04x spoke and is not in the list: setting it up", nwk);
        setup(i);
    }
    if (named) {
        aos_hal_log(TAG, "0x%04x: \"%s\" \"%s\"", nwk, manuf, model);
        notice("Zigbee: %s %s", manuf[0] ? manuf : "?", model);
    }
    if (is_read) done_if(i, cl == CL_BASIC ? J_BASIC : J_READ, cl);
    if (enroll) {
        /* it asks to be enrolled: yes, as zone i (Zigbee2MQTT answers so too) */
        ezb_zcl_ias_zone_enroll_rsp_cmd_t r = { .cmd_ctrl = { .dst_ep = h->src_ep, .src_ep = 1 },
                                                .payload = { .enroll_rsp_code = 0, .zone_id = (uint8_t)i } };
        ezb_address_set_short(&r.cmd_ctrl.dst_addr, nwk);
        aos_hal_log(TAG, "0x%04x asks to be enrolled: %d", nwk, ezb_zcl_ias_zone_enroll_cmd_resp(&r));
    }
    return false;
}

/* ---- from the stack (aos_zigbee_p4.c) ----------------------------------- */

/* The board's endpoint as the client of what it reads: the stack hands a
 * cluster's commands to the application only for clusters the endpoint has */
void aos_zigbee_devices_endpoint(ezb_af_ep_desc_t ep)
{
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_on_off_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_rel_humidity_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_occupancy_sensing_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_ias_zone_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
}

void aos_zigbee_devices_init(void)
{
    ezb_zcl_raw_command_handler_register(on_raw);
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = { .callback = tick_timer, .name = "zb_jobs" };
    if (!t && esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_periodic(t, 1000000);
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
        int i = find_ieee(a->device_addr.u8);
        if (i < 0) i = add(a->device_addr.u8, a->short_addr);
        bool again = false;
        if (i >= 0) {
            aos_zigbee_dev_t *d = &s_dev[i];
            d->nwk = a->short_addr;
            d->cap = a->capability;
            d->seen_ms = d->heard_ms = (uint32_t)aos_hal_uptime_ms();
            again = d->step != AOS_ZB_DEV_DONE;
            if (again) d->step = AOS_ZB_DEV_EPS;
        }
        unlock();
        const uint8_t *e = a->device_addr.u8;
        aos_hal_log(TAG, "a device announced itself: 0x%04x, %02x%02x%02x%02x%02x%02x%02x%02x, capability 0x%02x",
                    a->short_addr, e[7], e[6], e[5], e[4], e[3], e[2], e[1], e[0], a->capability);
        if (i >= 0 && again) {
            notice(_("Zigbee: entró un equipo (0x%04x), preguntándole qué es"), a->short_addr);
            setup(i);
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
        int i = find_ieee(l->device_addr.u8);
        if (i >= 0) s_dev[i].step = AOS_ZB_DEV_LEFT;
        unlock();
        if (i >= 0) s_jobs[i].n = 0, s_jobs[i].busy = false;
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
