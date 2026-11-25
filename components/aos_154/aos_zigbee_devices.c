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
 * The list is kept in the preferences ("zbd00".."zbd31": who each one is,
 * not what it said) and comes back at the start: a device that joined is in
 * it until it is removed, "with no news" until it speaks again (a sensor on
 * battery may say nothing for an hour), so a restart never looks like the
 * devices went away. Those on mains are asked at the start whether they are
 * there; the stack's neighbor table adds any it has that the list lost.
 * Removing one asks it to leave the network (Mgmt_Leave) and forgets it.
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
#include "ezbee/zcl/cluster/on_off.h"
#include "ezbee/zcl/cluster/level.h"
#include "ezbee/zcl/cluster/level_desc.h"
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
#define CL_LEVEL 0x0008
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
        if (s_dev[i].nwk == nwk && s_dev[i].step != AOS_ZB_DEV_LEFT && s_dev[i].step != AOS_ZB_DEV_REMOVED) return i;
    return -1;
}

static int find_ieee(const uint8_t *ieee)
{
    for (int i = 0; i < s_n; i++)
        if (!memcmp(s_dev[i].ieee, ieee, 8) && s_dev[i].step != AOS_ZB_DEV_REMOVED) return i;
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
    d->level = 0xFF;
    return s_n++;
}

/* ---- the list in the preferences ----------------------------------------- */

#define SAVE_KEY "zbd%02d"
static int s_saved;                     /* how many keys the last save wrote */

/* "ieee nwk cap power step neps ep,ep,ep,ep zone_type clusters onoff_eps level_ep|manuf|model"
 * (before 2026-11-24's last, without onoff_eps and level_ep: still read) */
static void save_all(void)
{
    char key[16], v[160];
    lock();
    int n = 0;
    for (int i = 0; i < s_n; i++) {
        const aos_zigbee_dev_t *d = &s_dev[i];
        if (d->step == AOS_ZB_DEV_REMOVED) continue;
        snprintf(key, sizeof key, SAVE_KEY, n++);
        snprintf(v, sizeof v, "%02x%02x%02x%02x%02x%02x%02x%02x %u %u %u %u %u %u,%u,%u,%u %u %u %u %u|%s|%s", d->ieee[7],
                 d->ieee[6], d->ieee[5], d->ieee[4], d->ieee[3], d->ieee[2], d->ieee[1], d->ieee[0], d->nwk, d->cap, d->power,
                 d->step == AOS_ZB_DEV_DONE ? AOS_ZB_DEV_DONE : AOS_ZB_DEV_EPS, d->neps, d->eps_list[0], d->eps_list[1],
                 d->eps_list[2], d->eps_list[3], d->zone_type, d->clusters, d->onoff_eps, d->level_ep, d->manuf, d->model);
        unlock();
        aos_hal_pref_set_str(key, v);
        lock();
    }
    unlock();
    for (int i = n; i < s_saved; i++) {
        snprintf(key, sizeof key, SAVE_KEY, i);
        aos_hal_pref_erase(key);
    }
    s_saved = n;
}

static void load_all(void)
{
    char key[16], v[160];
    lock();
    int keys = 0;
    for (int i = 0; i < AOS_ZB_DEVICES_MAX; i++) {
        snprintf(key, sizeof key, SAVE_KEY, i);
        if (!aos_hal_pref_get_str(key, v, sizeof v)) break;
        keys = i + 1;
        uint8_t ieee[8];
        unsigned nwk, cap, power, step, neps, e0, e1, e2, e3, zt, cls, oe = 0, le = 0;
        int at = 0;
        bool ok = true;
        for (int k = 0; k < 8 && ok; k++) {
            unsigned b;
            ok = sscanf(v + 2 * k, "%2x", &b) == 1;
            ieee[7 - k] = (uint8_t)b;
        }
        if (!ok) continue;
        if (sscanf(v + 16, " %u %u %u %u %u %u,%u,%u,%u %u %u %u %u%n", &nwk, &cap, &power, &step, &neps, &e0, &e1, &e2, &e3,
                   &zt, &cls, &oe, &le, &at) != 13 &&
            sscanf(v + 16, " %u %u %u %u %u %u,%u,%u,%u %u %u%n", &nwk, &cap, &power, &step, &neps, &e0, &e1, &e2, &e3, &zt,
                   &cls, &at) != 11)
            continue;
        static const uint8_t none[8] = { 0 }, all[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if (!memcmp(ieee, none, 8) || !memcmp(ieee, all, 8)) continue;     /* saved by mistake once */
        int j = add(ieee, (uint16_t)nwk);
        if (j < 0) break;
        aos_zigbee_dev_t *d = &s_dev[j];
        d->cap = (uint8_t)cap;
        d->power = (uint8_t)power;
        d->step = (uint8_t)step;
        d->neps = (uint8_t)(neps > 4 ? 4 : neps);
        d->eps_list[0] = (uint8_t)e0, d->eps_list[1] = (uint8_t)e1, d->eps_list[2] = (uint8_t)e2, d->eps_list[3] = (uint8_t)e3;
        d->ep = d->eps_list[0];
        d->eps = d->neps;
        d->zone_type = (uint16_t)zt;
        d->clusters = (uint16_t)cls;
        d->onoff_eps = (uint8_t)oe;
        d->level_ep = (uint8_t)le;
        const char *m = strchr(v + 16 + at, '|');
        if (m) {
            const char *m2 = strchr(m + 1, '|');
            size_t l = m2 ? (size_t)(m2 - m - 1) : strlen(m + 1);
            if (l >= sizeof d->manuf) l = sizeof d->manuf - 1;
            memcpy(d->manuf, m + 1, l);
            d->manuf[l] = '\0';
            if (m2) snprintf(d->model, sizeof d->model, "%s", m2 + 1);
        }
        /* heard_ms 0: no news since the board started */
    }
    s_saved = keys;                     /* the keys there: a save erases what is left over */
    unlock();
    aos_hal_log(TAG, "%d devices in the saved list", s_n);
    if (s_n != keys) save_all();        /* one left out: the list rewritten without it */
}

/* one that listens all the time: a router, or on mains or DC (Basic's power
 * source), so it can be asked anything at any moment */
static bool awake(const aos_zigbee_dev_t *d)
{
    uint8_t p = d->power & 0x7F;
    return (d->cap & 0x0A) || p == 0x01 || p == 0x02 || p == 0x04;
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
enum { J_ACTIVE, J_BASIC, J_SIMPLE, J_BIND, J_CIE, J_ENROLL, J_READ, J_ONOFF, J_LEVEL };
/* J_ONOFF's "cluster": 0 off, 1 on, 2 toggle; J_LEVEL's: the level */
typedef struct { uint8_t type, ep; uint16_t cluster; uint8_t tries; } job_t;
#define JOBS        32
#define JOB_WAIT_MS 5000

static struct {
    job_t   q[JOBS];
    uint8_t head, n;
    bool    busy;
    int64_t since_ms;
    int64_t setup_ms;                   /* when its setup last started */
    int64_t reask_ms;                   /* when what was unknown was last asked again */
} s_jobs[AOS_ZB_DEVICES_MAX];

static void run(int i);

static void push(int i, uint8_t type, uint8_t ep, uint16_t cluster)
{
    if (s_jobs[i].n == JOBS) return;
    s_jobs[i].q[(s_jobs[i].head + s_jobs[i].n) % JOBS] = (job_t){ type, ep, cluster, 0 };
    s_jobs[i].n++;
}

/* a command the person is waiting for: ahead of the setup's requests, just
 * after the one under way (a tap waited behind a device's whole setup after
 * a restart, a request without answer costing 5 s each, and two taps
 * crossed; on the board, 2026-11-24) */
static void push_first(int i, uint8_t type, uint8_t ep, uint16_t cluster)
{
    if (s_jobs[i].n == JOBS) return;
    int at = s_jobs[i].busy ? 1 : 0;    /* the running one stays at the head */
    for (int k = s_jobs[i].n; k > at; k--)
        s_jobs[i].q[(s_jobs[i].head + k) % JOBS] = s_jobs[i].q[(s_jobs[i].head + k - 1) % JOBS];
    s_jobs[i].q[(s_jobs[i].head + at) % JOBS] = (job_t){ type, ep, cluster, 0 };
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
                         : c == CL_HUM ? AOS_ZB_CL_HUM : c == CL_OCC ? AOS_ZB_CL_OCC : c == CL_IAS ? AOS_ZB_CL_IAS
                         : c == CL_LEVEL ? AOS_ZB_CL_LEVEL : 0;
            lock();
            d->clusters |= bit;
            int k = ep_index(d, ep);
            if (c == CL_ONOFF && k >= 0) d->onoff_eps |= (uint8_t)(1 << k);
            if (c == CL_LEVEL && !d->level_ep) d->level_ep = ep;
            if (d->ncl < AOS_ZB_CLUSTERS) d->cl[d->ncl++] = (uint32_t)ep << 16 | c;
            unlock();
            /* any device, not only the ones the board knows: every cluster
             * that may report something is bound to the board, so what it
             * says comes here (and is kept raw if the board cannot name it);
             * not those that only take commands or are the device's own */
            bool quiet = c == CL_BASIC || c == 0x0003 || c == 0x0004 || c == 0x0005 || c == 0x000a || c == 0x0019 ||
                         c == 0x0020 || c == 0x1000 ||
                         /* a maker's own (0xFC00 and up): what they say comes
                          * unbound, and a bind they do not take costs a
                          * sleepy device's few awake seconds (an SNZB-02D's
                          * 0xFC57 and 0xFC11, an ORVIBO's 0xFFFF: 5 s each) */
                         c >= 0xFC00;
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
    int i = (int)(uintptr_t)ctx;
    if (cnf && cnf->status) {
        aos_hal_log(TAG, "0x%04x: cluster 0x%04x on %u: not delivered (%u)", s_dev[i].nwk, cnf->cluster_id, cnf->dst_ep,
                    cnf->status);
        /* A command the person asked for, not delivered: sent again, twice
         * at most. A Tuya relay module took no frame for a few seconds after
         * it switched (saving its state to flash, it seems: on the board,
         * 2026-11-24, an Off right after an On came back "not delivered",
         * and the same Off went through 8 s later); Zigbee2MQTT retries too. */
        job_t j = s_jobs[i].q[s_jobs[i].head];
        if (s_jobs[i].busy && (j.type == J_ONOFF || j.type == J_LEVEL) && j.tries < 2) {
            done(i);
            push_first(i, j.type, j.ep, j.cluster);
            s_jobs[i].q[(s_jobs[i].head + (s_jobs[i].busy ? 1 : 0)) % JOBS].tries = (uint8_t)(j.tries + 1);
            aos_hal_log(TAG, "0x%04x: it goes again (%u)", s_dev[i].nwk, j.tries + 1);
            run(i);
            return;
        }
    }
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
    case J_ONOFF: {
        ezb_zcl_on_off_cmd_t c = { .cmd_ctrl = { .dst_ep = j->ep, .src_ep = 1, .cnf_ctx = { .cb = on_cnf, .user_ctx = ctx } } };
        ezb_address_set_short(&c.cmd_ctrl.dst_addr, d->nwk);
        e = j->cluster == 0 ? ezb_zcl_on_off_off_cmd_req(&c) : j->cluster == 1 ? ezb_zcl_on_off_on_cmd_req(&c)
                                                                                : ezb_zcl_on_off_toggle_cmd_req(&c);
        aos_hal_log(TAG, "0x%04x: output on %u: %s (%d)", d->nwk, j->ep,
                    j->cluster == 0 ? "off" : j->cluster == 1 ? "on" : "toggle", e);
        break;
    }
    case J_LEVEL: {
        /* with On/Off: a dimmer that is off goes on at that level (and off at 0) */
        ezb_zcl_level_move_to_level_cmd_t c = {
            .cmd_ctrl = { .dst_ep = j->ep, .src_ep = 1, .cnf_ctx = { .cb = on_cnf, .user_ctx = ctx } },
            .payload = { .level = (uint8_t)j->cluster, .transition_time = 5 },     /* 0.5 s */
        };
        ezb_address_set_short(&c.cmd_ctrl.dst_addr, d->nwk);
        e = ezb_zcl_level_move_to_level_with_on_off_cmd_req(&c);
        aos_hal_log(TAG, "0x%04x: level on %u: %u (%d)", d->nwk, j->ep, (unsigned)j->cluster, e);
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
    bool finished = s_dev[i].setup && !s_jobs[i].n;
    s_dev[i].setup = s_jobs[i].n;
    unlock();
    if (finished) save_all();           /* who it is, now known */
    if (s_jobs[i].busy || !s_jobs[i].n) return;
    s_jobs[i].busy = true;
    s_jobs[i].since_ms = (int64_t)aos_hal_uptime_ms();
    start_job(i);
}

static int64_t s_net_up_ms;              /* when the network came up, for the neighbor table's second looks */
void aos_zigbee_devices_network_up(void);

/* every second, in the stack's task: a job with no answer is let go */
static void switch_task(void *ctx);

static void tick(void *ctx)
{
    (void)ctx;
    int64_t now = (int64_t)aos_hal_uptime_ms();
    switch_task(NULL);                  /* a command whose post did not go in */
    /* routers show in the neighbor table after their link status (every
     * 15 s): it is looked at again 30 s and 2 min after the start */
    static int looks;
    if (s_net_up_ms && ((looks == 0 && now - s_net_up_ms > 30000) || (looks == 1 && now - s_net_up_ms > 120000))) {
        looks++;
        aos_zigbee_devices_network_up();
    }
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
    s_jobs[i].setup_ms = (int64_t)aos_hal_uptime_ms();
    lock();
    s_dev[i].ncl = 0;
    s_dev[i].clusters = 0;
    s_dev[i].onoff_eps = 0;
    s_dev[i].level_ep = 0;
    unlock();
    push(i, J_ACTIVE, 0, 0);
    run(i);
}

/* a setup under way: requests still to go. (A minute's grace after one
 * started kept a dimmer half set up: its first request went unanswered,
 * still busy joining, and its announce 40 s later was let pass; on the
 * board, 2026-11-24.) */
static bool setting_up(int i)
{
    return s_jobs[i].n != 0;
}

/* a device the network let in: into the list and set up, if it is not
 * already. The announce that should follow its join (a broadcast) did not
 * reach the board for five devices that came in through a router (on the
 * board, 2026-11-24): the network's own word that it is in is enough. */
static void joined(const uint8_t *ieee, uint16_t nwk)
{
    lock();
    int i = find_ieee(ieee);
    bool fresh = i < 0;
    if (fresh) i = add(ieee, nwk);
    if (i >= 0) {
        s_dev[i].nwk = nwk;
        s_dev[i].seen_ms = s_dev[i].heard_ms = (uint32_t)aos_hal_uptime_ms();
        if (s_dev[i].step == AOS_ZB_DEV_LEFT || s_dev[i].step == AOS_ZB_DEV_FAILED) s_dev[i].step = AOS_ZB_DEV_EPS;
    }
    bool go = i >= 0 && s_dev[i].step != AOS_ZB_DEV_DONE && !setting_up(i);
    unlock();
    if (fresh && i >= 0) {
        notice(_("Zigbee: entró un equipo (0x%04x), preguntándole qué es"), nwk);
        save_all();
    }
    if (go) setup(i);
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
    aos_hal_log(TAG, "0x%04x: zone status 0x%04x", d->nwk, st);
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
    case CL_LEVEL:
        if (id == 0x0000 && v[0] != 0xFF) d->level = v[0];
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
    bool unfinished = d->step != AOS_ZB_DEV_DONE;
    if (named) {
        d->step = AOS_ZB_DEV_DONE;
        memcpy(manuf, d->manuf, sizeof manuf);
        memcpy(model, d->model, sizeof model);
    }
    unlock();

    if (fresh) {
        aos_hal_log(TAG, "0x%04x spoke and is not in the list: setting it up", nwk);
        setup(i);
    } else if (!unfinished && !s_jobs[i].n && (int64_t)aos_hal_uptime_ms() - s_jobs[i].reask_ms > 60000) {
        /* set up, and awake now (it just spoke): what is still unknown of
         * what it serves is asked for, at most once a minute (a sleepy
         * sensor that slept through its first reads: an ORVIBO's zone type) */
        s_jobs[i].reask_ms = (int64_t)aos_hal_uptime_ms();
        lock();
        const aos_zigbee_dev_t *e = &s_dev[i];
        uint8_t ep1 = e->ep ? e->ep : 1;
        bool ias = (e->clusters & AOS_ZB_CL_IAS) && e->zone_type == 0xFFFF;
        bool bat = (e->clusters & AOS_ZB_CL_POWER) && e->battery == 0xFF && !e->voltage;
        bool lvl = e->level_ep && e->level == 0xFF;
        uint8_t lep = e->level_ep, oe = (uint8_t)(e->onoff_eps & ~e->onoff_known), eps[4];
        memcpy(eps, e->eps_list, 4);
        unlock();
        if (ias) push(i, J_READ, ep1, CL_IAS);
        if (bat) push(i, J_READ, ep1, CL_POWER);
        if (lvl) push(i, J_READ, lep, CL_LEVEL);
        for (int k = 0; k < 4; k++)
            if ((oe >> k) & 1) push(i, J_READ, eps[k], CL_ONOFF);
        if (s_jobs[i].n) {
            aos_hal_log(TAG, "0x%04x spoke: asking again what is unknown (%u requests)", nwk, s_jobs[i].n);
            run(i);
        }
    } else if (!named && unfinished && !setting_up(i) && (int64_t)aos_hal_uptime_ms() - s_jobs[i].setup_ms > 20000) {
        /* half set up (asleep before it was done): it is awake now */
        aos_hal_log(TAG, "0x%04x spoke, its setup was not finished: again", nwk);
        setup(i);
    }
    if (specific && cl == CL_IAS)
        aos_hal_log(TAG, "0x%04x: IAS command 0x%02x, %u bytes", nwk, h->cmd_id, (unsigned)f->payload_length);
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
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_level_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_rel_humidity_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_occupancy_sensing_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_ias_zone_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
}

void aos_zigbee_devices_init(void)
{
    if (!s_n) load_all();
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
        const uint8_t *e = a->device_addr.u8;
        aos_hal_log(TAG, "a device announced itself: 0x%04x, %02x%02x%02x%02x%02x%02x%02x%02x, capability 0x%02x",
                    a->short_addr, e[7], e[6], e[5], e[4], e[3], e[2], e[1], e[0], a->capability);
        /* a device that joins again (reset) is asked everything again */
        lock();
        int i = find_ieee(a->device_addr.u8);
        if (i >= 0) {
            s_dev[i].cap = a->capability;
            if (s_dev[i].nwk != a->short_addr) s_dev[i].step = AOS_ZB_DEV_EPS;
        }
        unlock();
        joined(a->device_addr.u8, a->short_addr);
        lock();
        if ((i = find_ieee(a->device_addr.u8)) >= 0) s_dev[i].cap = a->capability;
        unlock();
        return true;
    }
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *a = p;
        if (!a) return true;
        aos_hal_log(TAG, "0x%04x authorized: type %u, status %u", a->short_addr, a->type, a->status);
        if (a->status == 0) joined(a->device_addr.u8, a->short_addr);
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
        if (i >= 0) save_all();
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

/* The network is up (formed, or back after a restart): any device of the
 * stack's neighbor table the list does not have goes in, and those that
 * listen all the time (routers, mains) are asked their Basic, to know at
 * once whether they are there. Those asleep say so when they speak. */
void aos_zigbee_devices_network_up(void)
{
    if (!s_net_up_ms) s_net_up_ms = (int64_t)aos_hal_uptime_ms();
    ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t nb;
    int added = 0;
    while (ezb_nwk_get_next_neighbor(&it, &nb) == EZB_ERR_NONE) {
        if (ezb_eui64_is_invalid(&nb.ieee_addr)) {          /* an entry with no long address yet */
            if (it == EZB_NWK_INFO_ITERATOR_EOT) break;
            continue;
        }
        lock();
        int i = find_ieee(nb.ieee_addr.u8);
        if (i < 0 && (i = add(nb.ieee_addr.u8, nb.short_addr)) >= 0) added++;
        if (i >= 0) {
            s_dev[i].nwk = nb.short_addr;
            if (nb.rx_on_when_idle) s_dev[i].cap |= 0x08;
            if (nb.device_type == 1) s_dev[i].cap |= 0x02;          /* a router */
            if (nb.rssi) s_dev[i].rssi = nb.rssi;
        }
        unlock();
        if (it == EZB_NWK_INFO_ITERATOR_EOT) break;
    }
    aos_hal_log(TAG, "network up: %d devices in the list, %d more from the neighbor table", s_n, added);
    if (added) save_all();
    for (int i = 0; i < s_n; i++) {
        if (s_dev[i].step == AOS_ZB_DEV_LEFT || s_dev[i].step == AOS_ZB_DEV_REMOVED) continue;
        if (s_dev[i].step != AOS_ZB_DEV_DONE) {
            if (awake(&s_dev[i]) && !setting_up(i)) setup(i);       /* awake: set it up now */
        } else if (awake(&s_dev[i]) && !s_dev[i].heard_ms && !s_jobs[i].n) {
            lock();
            s_dev[i].ncl = 0;
            unlock();
            push(i, J_BASIC, s_dev[i].ep ? s_dev[i].ep : 1, CL_BASIC);  /* are you there? */
            for (int k = 0; k < s_dev[i].neps; k++) push(i, J_SIMPLE, s_dev[i].eps_list[k], 0);
            run(i);
        }
    }
}

/* ---- removing one ---------------------------------------------------------- */

static uint8_t s_rm_ieee[8];

static void on_leave(const ezb_zdo_nwk_mgmt_leave_req_result_t *res, void *ctx)
{
    (void)ctx;
    aos_hal_log(TAG, "the leave request: %d/%d", res ? res->error : -1, res && res->rsp ? res->rsp->status : -1);
}

static void remove_task(void *ctx)
{
    (void)ctx;
    lock();
    int i = find_ieee(s_rm_ieee);
    uint16_t nwk = i >= 0 ? s_dev[i].nwk : 0xFFFF;
    unlock();
    if (i < 0) return;
    /* asked to leave, for good (a sleepy one gets it from its parent when
     * it wakes); out of the list either way */
    ezb_zdo_nwk_mgmt_leave_req_t r = { .dst_nwk_addr = nwk, .cb = on_leave };
    memcpy(r.field.device_addr.u8, s_rm_ieee, 8);
    r.field.remove_children = false;
    r.field.rejoin = false;
    aos_hal_log(TAG, "0x%04x: asked to leave: %d", nwk, ezb_zdo_nwk_mgmt_leave_req(&r));
    /* marked, not moved: answers still on their way carry the slot's index */
    lock();
    i = find_ieee(s_rm_ieee);
    if (i >= 0) s_dev[i].step = AOS_ZB_DEV_REMOVED;
    unlock();
    if (i >= 0) s_jobs[i].n = 0, s_jobs[i].busy = false;
    save_all();
    notice(_("Zigbee: 0x%04x, sacado de la red"), nwk);
}

bool aos_zigbee_remove(const uint8_t ieee[8])
{
    aos_zigbee_info_t z;
    aos_zigbee_info(&z);
    if (z.state != AOS_ZB_FORMED) return false;
    memcpy(s_rm_ieee, ieee, 8);
    return esp_zigbee_task_queue_post(remove_task, NULL) == ESP_OK;
}

/* ---- an output switched (a relay, a plug, a light) ------------------------- */

/* a few taps may come before the stack's task takes them */
static struct { uint8_t ieee[8], ep, cmd; int level; } s_sw[4];     /* level < 0: an on/off */
static volatile uint8_t s_sw_head, s_sw_tail;

static void switch_task(void *ctx)
{
    (void)ctx;
    while (s_sw_tail != s_sw_head) {
        uint8_t ieee[8], ep, cmd;
        int level;
        lock();
        memcpy(ieee, s_sw[s_sw_tail % 4].ieee, 8);
        ep = s_sw[s_sw_tail % 4].ep;
        cmd = s_sw[s_sw_tail % 4].cmd;
        level = s_sw[s_sw_tail % 4].level;
        s_sw_tail++;
        int i = find_ieee(ieee);
        unlock();
        if (i < 0) {
            aos_hal_log(TAG, "switch: %02x%02x... is not in the list", ieee[7], ieee[6]);
            continue;
        }
        aos_hal_log(TAG, "0x%04x: %s on %u asked (%u requests waiting%s)", s_dev[i].nwk,
                    level >= 0 ? "level" : cmd == 0 ? "off" : cmd == 1 ? "on" : "toggle", ep, s_jobs[i].n,
                    s_jobs[i].busy ? ", one under way" : "");
        /* the command, and then its state read back: a device that reports
         * it says so anyway, one that does not is asked */
        /* pushed first in reverse: the command, then its reads */
        push_first(i, J_READ, ep, CL_ONOFF);
        if (level >= 0) {
            push_first(i, J_READ, ep, CL_LEVEL);
            push_first(i, J_LEVEL, ep, (uint16_t)level);
        } else push_first(i, J_ONOFF, ep, cmd);
        run(i);
    }
}

static bool queue_switch(const uint8_t ieee[8], uint8_t ep, int cmd, int level)
{
    aos_zigbee_info_t z;
    aos_zigbee_info(&z);
    if (z.state != AOS_ZB_FORMED) return false;
    lock();
    bool room = (uint8_t)(s_sw_head - s_sw_tail) < 4;
    if (room) {
        memcpy(s_sw[s_sw_head % 4].ieee, ieee, 8);
        s_sw[s_sw_head % 4].ep = ep;
        s_sw[s_sw_head % 4].cmd = (uint8_t)cmd;
        s_sw[s_sw_head % 4].level = level;
        s_sw_head++;
    }
    unlock();
    if (!room) {
        aos_hal_log(TAG, "switch: four already waiting");
        return false;
    }
    /* one post for the ring: switch_task takes all that is in it */
    esp_err_t e = esp_zigbee_task_queue_post(switch_task, NULL);
    if (e != ESP_OK) aos_hal_log(TAG, "switch: the stack's queue did not take it (%s)", esp_err_to_name(e));
    return true;                        /* in the ring: the next post (or tick) takes it */
}

bool aos_zigbee_onoff(const uint8_t ieee[8], uint8_t ep, int cmd)
{
    return cmd >= 0 && cmd <= 2 && queue_switch(ieee, ep, cmd, -1);
}

bool aos_zigbee_level(const uint8_t ieee[8], uint8_t ep, int level)
{
    if (level < 0) level = 0;
    if (level > 254) level = 254;
    return queue_switch(ieee, ep, 0, level);
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
    int n = 0;
    for (int i = 0; i < s_n && n < max; i++)
        if (s_dev[i].step != AOS_ZB_DEV_REMOVED) out[n++] = s_dev[i];
    unlock();
    return n;
}
