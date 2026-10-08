/*
 * BLE - what the portal's page sees (apps/ble/web/ble.js), through the
 * system's live channel (aos_hal_live_*, docs/PORTAL-PAGES.md "Live data").
 *
 * Only while a page asks (idle < 3 s), once a second:
 *   state     the scan and the air's two minutes
 *   devices   the table, the living first, at most what fits in 64 KB
 *   dev       the device the page has open: its packets explained and raw,
 *             and its signal over two minutes
 * The page sends lines of key=value: sel, pause, active, duty, fav, alias,
 * forget, csv, mqtt, env, minrssi.
 */
#include "bl.h"
#include "bl_crypt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_ID "aos.ble"
#define JMAX    (60 * 1024)

static char *s_j;                   /* JMAX, PSRAM-ish: malloc'd once */
static uint8_t s_sel[6];
static bool s_has_sel;

/* JSON text: quotes, backslashes and control characters out of the way */
static size_t jstr(char *out, size_t n, const char *s)
{
    size_t k = 0;
    if (n < 3) return 0;
    out[k++] = '"';
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && k < n - 3; p++) {
        if (*p == '"' || *p == '\\') {
            if (k >= n - 4) break;
            out[k++] = '\\';
            out[k++] = (char)*p;
        } else if (*p < 0x20) {
            out[k++] = ' ';
        } else {
            out[k++] = (char)*p;
        }
    }
    out[k++] = '"';
    out[k] = 0;
    return k;
}

#define PUT(...) do { if (k < n) k += snprintf(j + k, n - k, __VA_ARGS__); } while (0)
#define PUTS(s) do { if (k < n) k += jstr(j + k, n - k, (s)); } while (0)

static void put_state(void)
{
    char *j = s_j;
    size_t n = JMAX, k = 0;
    uint32_t s = (uint32_t)(aos_hal_uptime_ms() / 1000);
    int alive = 0;
    for (int i = 0; i < BL.ndev; i++) alive += bl_alive(&BL.dev[i]);
    PUT("{\"scanning\":%s,\"paused\":%s,\"bt_off\":%s,\"active\":%s,\"duty\":%d,\"env\":%d,\"min_rssi\":%d,"
        "\"csv\":%s,\"mqtt\":%s,\"mqtt_ready\":%s,\"pps\":%.1f,\"total\":%u,\"lost\":%u,\"alive\":%d,\"known\":%d,\"pkts\":[",
        aos_hal_ble_scanning() ? "true" : "false", BL.paused ? "true" : "false", BL.bt_off ? "true" : "false",
        BL.active ? "true" : "false", BL.duty, BL.env, BL.min_rssi, BL.log_csv ? "true" : "false", BL.mqtt ? "true" : "false",
        bl_mqtt_ready() ? "true" : "false", (double)BL.air.pps, (unsigned)BL.air.total, (unsigned)BL.air.lost, alive, BL.ndev);
    for (int kk = BL_AIR_HIST - 1; kk >= 1; kk--) {
        uint32_t sec = s - kk;
        int v = (sec <= BL.air.sec && BL.air.sec - sec < BL_AIR_HIST) ? BL.air.pkts[sec % BL_AIR_HIST] : 0;
        PUT("%s%d", kk == BL_AIR_HIST - 1 ? "" : ",", v);
    }
    PUT("],\"devs\":[");
    for (int kk = BL_AIR_HIST - 1; kk >= 1; kk--) {
        uint32_t sec = s - kk;
        int v = (sec <= BL.air.sec && BL.air.sec - sec < BL_AIR_HIST) ? BL.air.devs[sec % BL_AIR_HIST] : 0;
        PUT("%s%d", kk == BL_AIR_HIST - 1 ? "" : ",", v);
    }
    /* the mask (1 phone, 2 computer, 4 Wi-Fi): the page words it in Spanish */
    PUT("],\"lost\":%d,\"lost_age\":%u}", BL.lost, BL.lost ? (unsigned)((uint32_t)aos_hal_uptime_ms() - BL.lost_ms) : 0);
    if (k < n) aos_hal_live_put(LIVE_ID, "state", "application/json", j, k);
}

static void sensor_obj(char *j, size_t n, size_t *kp, const bl_sensor_t *s)
{
    size_t k = *kp;
    PUT(",\"sen\":{\"format\":");
    PUTS(s->format ? s->format : "");
    if (s->encrypted) PUT(",\"encrypted\":true");
#define F(bit, key, v, fmt) if (s->mask & (bit)) PUT(",\"" key "\":" fmt, v)
    F(BL_V_TEMP, "temp", (double)s->temp, "%.2f");
    F(BL_V_HUM, "hum", (double)s->hum, "%.1f");
    F(BL_V_PRESS, "press", (double)s->press, "%.1f");
    F(BL_V_BATT, "batt", s->batt, "%d");
    F(BL_V_VOLT, "volt", (double)s->volt, "%.3f");
    F(BL_V_CO2, "co2", (double)s->co2, "%.0f");
    F(BL_V_PM25, "pm25", (double)s->pm25, "%.0f");
    F(BL_V_LUX, "lux", (double)s->lux, "%.0f");
    F(BL_V_MOIST, "moist", (double)s->moist, "%.0f");
    F(BL_V_WEIGHT, "weight", (double)s->weight, "%.2f");
    F(BL_V_OPEN, "open", s->open, "%d");
    F(BL_V_MOTION, "motion", s->motion, "%d");
    F(BL_V_BUTTON, "button", s->button, "%d");
    F(BL_V_HR, "hr", s->hr, "%d");
#undef F
    PUT("}");
    *kp = k;
}

static void put_devices(void)
{
    static int idx[BL_DEV_MAX];
    int cnt = bl_sorted(idx, BL_DEV_MAX, BL_FILT_ALL, BL_SORT_RSSI);
    char *j = s_j;
    size_t n = JMAX - 64, k = 0;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    PUT("[");
    int put = 0;
    for (int q = 0; q < cnt && k < n - 1200; q++) {
        const bl_dev_t *d = &BL.dev[idx[q]];
        char a[20], sub[120];
        bl_fmt_addr(d->addr, a, sizeof a);
        PUT("%s{\"a\":\"%s\",\"at\":%d,\"k\":", put ? "," : "", a, d->addr_type);
        PUTS(bl_addr_kind_name(bl_addr_kind(d->addr, d->addr_type)));
        PUT(",\"n\":");
        PUTS(d->ad.name);
        PUT(",\"al\":");
        PUTS(d->alias);
        PUT(",\"lbl\":");
        PUTS(d->label ? d->label : "");
        PUT(",\"cls\":");
        PUTS(bl_class_name(d->cls));
        const char *co = d->ad.nmfg ? bl_company_name(d->ad.mfg[0].company) : NULL;
        PUT(",\"co\":");
        PUTS(co ? co : "");
        if (d->ad.nmfg) PUT(",\"cid\":%u", d->ad.mfg[0].company);
        bl_dev_sub(d, sub, sizeof sub);
        PUT(",\"sub\":");
        PUTS(sub);
        int p1m = bl_p1m_guess(&d->ad, d->has_bc ? &d->bc : NULL);
        PUT(",\"r\":%d,\"ra\":%.1f,\"rmin\":%d,\"rmax\":%d,\"dist\":%.2f,\"age\":%u,\"seen\":%u,\"adv\":%u,\"rsp\":%u,"
            "\"chg\":%u,\"iv\":%d,\"fav\":%s,\"conn\":%s,\"kinds\":%d",
            d->rssi, (double)d->rssi_avg, d->rssi_min > 0 ? 0 : d->rssi_min, d->rssi_max < -126 ? 0 : d->rssi_max,
            (double)bl_distance_m(lroundf_safe(d->rssi_avg), p1m, bl_env_n()), (unsigned)(now - d->last_ms),
            (unsigned)(now - d->first_ms), (unsigned)d->n_adv, (unsigned)d->n_rsp, (unsigned)d->n_changes, (int)d->itvl_ms,
            d->fav ? "true" : "false",
            (d->kinds & ((1u << AOS_BLE_ADV_IND) | (1u << AOS_BLE_ADV_DIRECT_IND))) ? "true" : "false", d->kinds);
        if (d->has_sen) sensor_obj(j, n, &k, &d->sen);
        if (d->has_bc) PUT(",\"bc\":%d", d->bc.kind);
        /* the key: -2 none, else BL_KEY_* of the last try (-1 not tried yet) */
        PUT(",\"key\":%d", d->has_key ? d->key_state : -2);
        PUT("}");
        put++;
    }
    PUT("]");
    if (k < JMAX) aos_hal_live_put(LIVE_ID, "devices", "application/json", j, k);
}

static void put_hex(char *j, size_t n, size_t *kp, const uint8_t *v, int len)
{
    size_t k = *kp;
    PUT("\"");
    for (int i = 0; i < len; i++) PUT("%02X", v[i]);
    PUT("\"");
    *kp = k;
}

static void put_lines(char *j, size_t n, size_t *kp, const uint8_t *v, int len)
{
    static bl_line_t lines[24];
    size_t k = *kp;
    int c = bl_explain(v, len, lines, 24, NULL);
    PUT("[");
    for (int i = 0; i < c; i++) {
        PUT("%s[", i ? "," : "");
        PUTS(lines[i].key);
        PUT(",");
        PUTS(lines[i].val);
        PUT("]");
    }
    PUT("]");
    *kp = k;
}

static void put_dev(void)
{
    if (!s_has_sel) return;
    int i = bl_find(s_sel);
    if (i < 0) return;
    const bl_dev_t *d = &BL.dev[i];
    char *j = s_j;
    size_t n = JMAX, k = 0;
    char a[20];
    bl_fmt_addr(d->addr, a, sizeof a);
    PUT("{\"a\":\"%s\",\"adv\":", a);
    put_hex(j, n, &k, d->adv, d->adv_len);
    PUT(",\"rsp\":");
    put_hex(j, n, &k, d->rsp, d->rsp_len);
    PUT(",\"adv_lines\":");
    put_lines(j, n, &k, d->adv, d->adv_len);
    PUT(",\"rsp_lines\":");
    put_lines(j, n, &k, d->rsp, d->rsp_len);
    PUT(",\"hist\":[");
    uint32_t s = (uint32_t)(aos_hal_uptime_ms() / 1000);
    for (int q = BL_HIST - 1; q >= 0; q--) {
        uint32_t sec = s - q;
        int v = (sec <= d->hist_sec && d->hist_sec - sec < BL_HIST) ? d->hist[sec % BL_HIST] : BL_NO_RSSI;
        PUT("%s%d", q == BL_HIST - 1 ? "" : ",", v == BL_NO_RSSI ? 0 : v);
    }
    PUT("],\"temps\":[");
    uint32_t m = (uint32_t)(aos_hal_uptime_ms() / 60000);
    for (int q = BL_SEN_HIST - 1; q >= 0; q--) {
        uint32_t slot = m - q;
        float v = (slot <= d->sen_slot && d->sen_slot - slot < BL_SEN_HIST) ? d->sen_t[slot % BL_SEN_HIST] : NAN;
        if (v != v) PUT("%snull", q == BL_SEN_HIST - 1 ? "" : ",");
        else PUT("%s%.2f", q == BL_SEN_HIST - 1 ? "" : ",", (double)v);
    }
    PUT("]");
    if (d->has_bc) {
        char u[48];
        bl_uuid_str(d->bc.uuid, 16, u, sizeof u);
        PUT(",\"beacon\":{\"kind\":%d,\"uuid\":\"%s\",\"major\":%u,\"minor\":%u,\"tx1m\":%d,\"url\":", d->bc.kind, u, d->bc.major,
            d->bc.minor, d->bc.tx1m);
        PUTS(d->bc.url);
        PUT("}");
    }
    PUT("}");
    if (k < n) aos_hal_live_put(LIVE_ID, "dev", "application/json", j, k);
}

static void take(void)
{
    char msg[AOS_LIVE_MSG_MAX];
    int len;
    bool changed = false;
    while ((len = aos_hal_live_take(LIVE_ID, msg, sizeof msg - 1)) > 0) {
        msg[len] = 0;
        /* line by line (strtok_r is not in the firmware's table) */
        for (char *line = msg, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *key = line, *v = eq + 1;
            int num = atoi(v);
            if (!strcmp(key, "sel")) s_has_sel = bl_parse_addr(v, s_sel);
            else if (!strcmp(key, "pause")) { BL.paused = num != 0; changed = true; }
            else if (!strcmp(key, "active")) { BL.active = num != 0; changed = true; }
            else if (!strcmp(key, "duty")) { BL.duty = num < 5 ? 5 : num > 100 ? 100 : num; changed = true; }
            else if (!strcmp(key, "env")) { BL.env = num < 0 || num > 2 ? 1 : num; changed = true; }
            else if (!strcmp(key, "minrssi")) { BL.min_rssi = num; changed = true; }
            else if (!strcmp(key, "csv")) { BL.log_csv = num != 0; changed = true; }
            else if (!strcmp(key, "mqtt")) { BL.mqtt = num != 0; changed = true; }
            else if (!strcmp(key, "forget")) { bl_forget_all(); changed = true; }
            else if (!strcmp(key, "lost_ok")) { BL.lost = 0; changed = true; }
            else if (!strcmp(key, "key")) {
                /* key=AA:..:FF,<32 hex>   key=AA:..:FF,   (clears it) */
                uint8_t a[6], k[16];
                char *comma = strchr(v, ',');
                if (!comma) continue;
                *comma = 0;
                if (!bl_parse_addr(v, a)) continue;
                int i = bl_find(a);
                if (i < 0) continue;
                if (!comma[1]) bl_key_set(i, NULL);
                else if (bl_parse_key(comma + 1, k)) bl_key_set(i, k);
                changed = true;
            }
            else if (!strcmp(key, "fav") || !strcmp(key, "alias")) {
                /* fav=AA:..:FF,1   alias=AA:..:FF,some name */
                uint8_t a[6];
                char *comma = strchr(v, ',');
                if (!comma) continue;
                *comma = 0;
                if (!bl_parse_addr(v, a)) continue;
                int i = bl_find(a);
                if (i < 0) continue;
                if (key[0] == 'f') BL.dev[i].fav = atoi(comma + 1) != 0;
                else {
                    snprintf(BL.dev[i].alias, sizeof BL.dev[i].alias, "%.27s", comma + 1);
                    bl_utf8_trim(BL.dev[i].alias);
                    for (char *p = BL.dev[i].alias; *p; p++) if (*p == '|') *p = ' ';
                }
                bl_names_save();
                changed = true;
            }
        }
    }
    if (changed) {
        bl_settings_save();
        bl_scan_apply();
        if (!BL.hidden && !BL.overlay && BL.content) bl_rebuild();
    }
}

void bl_live_tick(void)
{
    static uint32_t last;
    take();
    if (aos_hal_live_idle_ms(LIVE_ID) > 3000) return;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - last < 1000) return;
    last = now;
    if (!s_j) s_j = malloc(JMAX);
    if (!s_j) return;
    put_state();
    put_devices();
    put_dev();
}

void bl_live_clear(void)
{
    aos_hal_live_clear(LIVE_ID);
    free(s_j);
    s_j = NULL;
    s_has_sel = false;
}
