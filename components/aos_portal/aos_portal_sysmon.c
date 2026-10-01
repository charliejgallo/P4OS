/*
 * P4OS - the portal's Monitor: what the aos.sysmon app shows, as JSON
 * (aos_portal.c hands every /api/sysmon request here).
 *
 *   GET /api/sysmon            everything: CPU, tasks, memory, LVGL, network,
 *                              card, firmware, and the hour of history
 *   GET /api/sysmon?hist=0     the same without the history (a lighter poll)
 *
 * Sizes are bytes, times are seconds unless the key says otherwise, and a
 * value the platform does not know is null. The task CPU is tenths of a
 * percent of one core over the last refresh window (see aos_hal.h), sent
 * as a percentage with one decimal.
 */
#include "aos_portal_sysmon.h"
#include "aos_hal.h"
#include "lvgl.h"
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const STATE[] = { "running", "ready", "blocked", "suspended", "deleted", "unknown" };
static const char *const MEM_KEY[AOS_MEM_COUNT] = { "internal", "dma", "spiram", "exec" };
static const struct { aos_hist_t h; const char *key; } HIST[] = {
    { AOS_HIST_CPU0, "cpu0" }, { AOS_HIST_CPU1, "cpu1" }, { AOS_HIST_CHIP_T, "chip_t_x10" },
    { AOS_HIST_INT_FREE_KB, "internal_free_kb" }, { AOS_HIST_PSRAM_FREE_KB, "psram_free_kb" },
};

static void add_str_or_null(cJSON *o, const char *k, const char *v)
{
    if (v && v[0]) cJSON_AddStringToObject(o, k, v);
    else cJSON_AddNullToObject(o, k);
}

static void add_num(cJSON *o, const char *k, double v, int dec)
{
    if (isnan(v) || isinf(v)) { cJSON_AddNullToObject(o, k); return; }
    char t[24];
    snprintf(t, sizeof t, "%.*f", dec, v);
    cJSON_AddNumberToObject(o, k, strtod(t, NULL));
}

static int count_objs(lv_obj_t *o)
{
    int n = 1;
    uint32_t c = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < c; i++) n += count_objs(lv_obj_get_child(o, (int32_t)i));
    return n;
}

static void add_lvgl(cJSON *o)
{
    cJSON *l = cJSON_AddObjectToObject(o, "lvgl");
    if (!aos_hal_lock(200)) { cJSON_AddBoolToObject(l, "busy", true); return; }
    lv_display_t *d = lv_display_get_default();
    int objs = 0;
    if (d) {
        objs = count_objs(lv_display_get_screen_active(d));
        objs += count_objs(lv_display_get_layer_top(d)) + count_objs(lv_display_get_layer_sys(d));
    }
    int timers = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) timers++;
    lv_mem_monitor_t mon;
    memset(&mon, 0, sizeof mon);
    lv_mem_monitor(&mon);
    int anims = (int)lv_anim_count_running();
    aos_hal_unlock();
    cJSON_AddNumberToObject(l, "objects", objs);
    cJSON_AddNumberToObject(l, "timers", timers);
    cJSON_AddNumberToObject(l, "anims", anims);
    /* the redraw rate since the previous call: a still screen should be
     * near zero */
    static uint64_t last_px;
    static uint32_t last_frames;
    static int64_t last_ms;
    uint64_t px;
    uint32_t frames;
    int64_t now = (int64_t)aos_hal_uptime_ms();
    if (aos_hal_display_flush_count(&px, &frames)) {
        if (last_ms && now > last_ms) {
            double s = (double)(now - last_ms) / 1000.0;
            cJSON_AddNumberToObject(l, "fps", (double)(uint32_t)((frames - last_frames) / s * 10) / 10);
            cJSON_AddNumberToObject(l, "screens_per_s", (double)(uint32_t)((double)(px - last_px) /
                                    (aos_hal_screen_w() * aos_hal_screen_h()) / s * 100) / 100);
        }
        last_px = px;
        last_frames = frames;
        last_ms = now;
    }
    if (mon.total_size) {       /* 0 with LVGL on the C library's malloc, as P4OS runs it */
        cJSON_AddNumberToObject(l, "pool_total", (double)mon.total_size);
        cJSON_AddNumberToObject(l, "pool_free", (double)mon.free_size);
        cJSON_AddNumberToObject(l, "pool_largest", (double)mon.free_biggest_size);
    } else {
        cJSON_AddStringToObject(l, "allocator", "clib");
    }
}

static void api_sysmon(aos_httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    aos_sys_info_t si;
    bool have_si = aos_hal_sys_info(&si);
    aos_sys_stats_t ss;
    bool have_ss = aos_hal_sys_stats(&ss);

    /* firmware and chip */
    cJSON *sy = cJSON_AddObjectToObject(o, "system");
    cJSON_AddStringToObject(sy, "board", aos_hal_board_name());
    cJSON_AddStringToObject(sy, "firmware", aos_hal_firmware_version());
    cJSON_AddNumberToObject(sy, "uptime_s", (double)(aos_hal_uptime_ms() / 1000));
    if (have_si) {
        add_str_or_null(sy, "chip", si.chip);
        cJSON_AddNumberToObject(sy, "chip_rev", si.chip_rev);
        cJSON_AddNumberToObject(sy, "cores", si.cores);
        cJSON_AddNumberToObject(sy, "cpu_mhz", si.cpu_mhz);
        add_str_or_null(sy, "idf", si.idf_version);
        add_str_or_null(sy, "reset_reason", si.reset_reason);
        cJSON_AddNumberToObject(sy, "flash_bytes", si.flash_bytes);
        add_str_or_null(sy, "app_slot", si.app_slot);
        if (si.app_bytes) cJSON_AddNumberToObject(sy, "app_bytes", si.app_bytes);
        else cJSON_AddNullToObject(sy, "app_bytes");
        cJSON_AddNumberToObject(sy, "app_slot_bytes", si.app_slot_bytes);
        add_str_or_null(sy, "note", si.note);
    }

    /* CPU and temperature */
    cJSON *cpu = cJSON_AddObjectToObject(o, "cpu");
    cJSON *load = cJSON_AddArrayToObject(cpu, "load");
    for (int c = 0; c < 2; c++) {
        if (have_ss && ss.cpu_load[c] >= 0) cJSON_AddItemToArray(load, cJSON_CreateNumber(ss.cpu_load[c]));
        else cJSON_AddItemToArray(load, cJSON_CreateNull());
    }
    add_num(cpu, "chip_c", have_ss ? ss.chip_c : NAN, 1);

    /* memory */
    cJSON *mem = cJSON_AddObjectToObject(o, "memory");
    for (int k = 0; k < AOS_MEM_COUNT && have_si; k++) {
        cJSON *m = cJSON_AddObjectToObject(mem, MEM_KEY[k]);
        cJSON_AddNumberToObject(m, "free", si.mem[k].free);
        cJSON_AddNumberToObject(m, "total", si.mem[k].total);
        cJSON_AddNumberToObject(m, "largest", si.mem[k].largest);
        cJSON_AddNumberToObject(m, "min_free", si.mem[k].min_free);
    }
    add_lvgl(o);

    /* tasks, the busiest first */
    aos_task_info_t *t = malloc(sizeof *t * AOS_TASKS_MAX);
    if (t) {
        int n = aos_hal_tasks(t, AOS_TASKS_MAX);
        if (n > AOS_TASKS_MAX) n = AOS_TASKS_MAX;
        for (int i = 1; i < n; i++) {           /* insertion sort: a few dozen */
            aos_task_info_t x = t[i];
            int j = i - 1;
            while (j >= 0 && t[j].cpu_x10 < x.cpu_x10) { t[j + 1] = t[j]; j--; }
            t[j + 1] = x;
        }
        cJSON *ta = cJSON_AddArrayToObject(o, "tasks");
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "name", t[i].name);
            cJSON_AddNumberToObject(e, "id", t[i].id);
            if (t[i].core >= 0) cJSON_AddNumberToObject(e, "core", t[i].core);
            else cJSON_AddNullToObject(e, "core");
            cJSON_AddStringToObject(e, "state", STATE[t[i].state <= AOS_TASK_UNKNOWN ? t[i].state : AOS_TASK_UNKNOWN]);
            cJSON_AddNumberToObject(e, "prio", t[i].prio);
            cJSON_AddNumberToObject(e, "base_prio", t[i].base_prio);
            if (t[i].stack_size >= 0) cJSON_AddNumberToObject(e, "stack_size", t[i].stack_size);
            else cJSON_AddNullToObject(e, "stack_size");
            if (t[i].stack_min_free >= 0) cJSON_AddNumberToObject(e, "stack_min_free", t[i].stack_min_free);
            else cJSON_AddNullToObject(e, "stack_min_free");
            cJSON_AddBoolToObject(e, "stack_psram", t[i].stack_psram);
            add_num(e, "cpu", t[i].cpu_x10 >= 0 ? t[i].cpu_x10 / 10.0 : NAN, 1);
            cJSON_AddNumberToObject(e, "run_ms", (double)(t[i].run_us / 1000));
            cJSON_AddItemToArray(ta, e);
        }
        free(t);
    }

    /* network */
    cJSON *net = cJSON_AddObjectToObject(o, "network");
    aos_net_state_t ns = aos_hal_net_state();
    cJSON_AddStringToObject(net, "state", ns == AOS_NET_CONNECTED ? "connected" : ns == AOS_NET_CONNECTING ? "connecting"
                                          : ns == AOS_NET_FAILED ? "failed" : "off");
    if (ns == AOS_NET_CONNECTED) {
        add_str_or_null(net, "ssid", aos_hal_net_ssid());
        cJSON_AddNumberToObject(net, "rssi", aos_hal_net_rssi());
        add_str_or_null(net, "ip", aos_hal_net_ip());
    }
    if (have_si) {
        cJSON_AddNumberToObject(net, "up_s", si.net_up_s);
        cJSON_AddNumberToObject(net, "drops", si.net_drops);
        if (si.net_channel) cJSON_AddNumberToObject(net, "channel", si.net_channel);
        add_str_or_null(net, "bssid", si.net_bssid);
        add_str_or_null(net, "mac", si.net_mac);
        add_str_or_null(net, "gateway", si.net_gw);
        add_str_or_null(net, "netmask", si.net_mask);
        add_str_or_null(net, "dns", si.net_dns);
        if (si.net_rx_bytes >= 0) cJSON_AddNumberToObject(net, "rx_bytes", (double)si.net_rx_bytes);
        if (si.net_tx_bytes >= 0) cJSON_AddNumberToObject(net, "tx_bytes", (double)si.net_tx_bytes);
    }

    /* the card */
    aos_sd_info_t sd;
    uint64_t tot = 0, fr = 0;
    if (aos_hal_sd_present() && aos_hal_sd_info(&sd)) {
        cJSON *c = cJSON_AddObjectToObject(o, "sd");
        cJSON_AddStringToObject(c, "name", sd.name);
        add_str_or_null(c, "kind", sd.kind);
        add_str_or_null(c, "fs", sd.fs);
        cJSON_AddNumberToObject(c, "capacity", (double)sd.capacity);
        if (aos_hal_sd_usage(&tot, &fr)) {
            cJSON_AddNumberToObject(c, "fs_total", (double)tot);
            cJSON_AddNumberToObject(c, "fs_free", (double)fr);
        }
        cJSON_AddNumberToObject(c, "freq_khz", sd.freq_khz);
        cJSON_AddNumberToObject(c, "bus_width", sd.bus_width);
        cJSON_AddNumberToObject(c, "manufacturer", sd.manufacturer);
        if (sd.year) {
            char d[12];
            snprintf(d, sizeof d, "%04d-%02d", sd.year, sd.month);
            cJSON_AddStringToObject(c, "date", d);
        }
    } else {
        cJSON_AddNullToObject(o, "sd");
    }

    /* the hour, a sample a minute, oldest first */
    if (aos_httpd_query_int(r, "hist", 1)) {
        cJSON *h = cJSON_AddObjectToObject(o, "history");
        int16_t v[AOS_MIN_HIST_LEN];
        for (size_t k = 0; k < sizeof HIST / sizeof HIST[0]; k++) {
            int n = aos_hal_minute_history(HIST[k].h, v, AOS_MIN_HIST_LEN);
            cJSON *a = cJSON_AddArrayToObject(h, HIST[k].key);
            for (int i = 0; i < n; i++)
                cJSON_AddItemToArray(a, v[i] == AOS_HIST_NONE ? cJSON_CreateNull() : cJSON_CreateNumber(v[i]));
        }
    }

    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, 200, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

bool aos_portal_sysmon(aos_httpd_req_t *r, const char *method, const char *p)
{
    if (strcmp(p, "sysmon")) return false;
    if (strcmp(method, "GET")) {
        aos_httpd_send_json(r, 405, "{\"ok\":false,\"error\":\"solo GET\"}");
        return true;
    }
    api_sysmon(r);
    return true;
}
