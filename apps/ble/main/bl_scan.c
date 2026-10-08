/*
 * BLE - the table of devices heard, and what is kept of it.
 *
 * The HAL's ring is drained from the app's timer (ble.c) a few times a
 * second; each report lands on its device's record: the RSSI (last,
 * smoothed, min, max, and the strongest of each second for two minutes),
 * the gap between packets, the raw packets, and when the bytes change, the
 * merged AD structures with what they say (class, sensor readings, beacon).
 *
 * Kept on the card, under <card>/ble/:
 *   nombres.txt          favourites and aliases ("AA:BB:..|*|Heladera"), which
 *                        the portal's page edits too;
 *   sensores-<day>.csv   one line a minute per sensor, when asked.
 * And published, when asked, to MQTT: <board>/ble/<address> with the readings.
 */
#include "bl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

/* the MQTT service (components/aos_apps/include/aos_mqtt.h), lent by the
 * firmware's symbol table; its header is not in the apps' build */
int aos_mqtt_state(void);
bool aos_mqtt_publish(const char *topic, const char *payload, int qos, bool retain);
#define BL_MQTT_CONNECTED 5         /* AOS_MQTT_CONNECTED */

#define RX_MAX 256

static void *ps_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
#else
    void *p = malloc(n);
#endif
    if (p) memset(p, 0, n);
    return p;
}

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static const char *card_root(void)
{
    const char *r = aos_hal_path_sd_root();
    return r ? r : aos_hal_path_data();
}

/* -------------------------------------------------------------------------- */
/* Formatting                                                                  */
/* -------------------------------------------------------------------------- */

void bl_fmt_num(char *out, size_t n, float v, int dec)
{
    snprintf(out, n, "%.*f", dec, (double)v);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

void bl_fmt_age(char *out, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s < 2) snprintf(out, n, "%s", _("ahora"));
    else if (s < 60) snprintf(out, n, "%u s", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%u min", (unsigned)(s / 60));
    else snprintf(out, n, "%u h", (unsigned)(s / 3600));
}

void bl_fmt_addr(const uint8_t a[6], char *out, size_t n)
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

bool bl_parse_addr(const char *s, uint8_t a[6])
{
    unsigned v[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 255) return false;
        a[i] = (uint8_t)v[i];
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Looks                                                                       */
/* -------------------------------------------------------------------------- */

const char *bl_class_glyph(bl_class_t c)
{
    switch (c) {
    case BL_CLS_PHONE:    return AOS_SYM_PHONE;
    case BL_CLS_COMPUTER: return AOS_SYM_MONITOR;
    case BL_CLS_TABLET:   return AOS_SYM_MONITOR;
    case BL_CLS_WATCH:    return AOS_SYM_CLOCK_OUTLINE;
    case BL_CLS_AUDIO:    return AOS_SYM_MUSIC;
    case BL_CLS_TRACKER:  return AOS_SYM_PIN;
    case BL_CLS_SENSOR:   return AOS_SYM_THERMOMETER;
    case BL_CLS_BEACON:   return AOS_SYM_RADAR;
    case BL_CLS_HID:      return AOS_SYM_KEYBOARD;
    case BL_CLS_GAMEPAD:  return AOS_SYM_GAMEPAD_VARIANT;
    case BL_CLS_TV:       return AOS_SYM_TELEVISION;
    case BL_CLS_HEALTH:   return AOS_SYM_PULSE;
    case BL_CLS_LIGHT:    return AOS_SYM_LIGHTBULB;
    case BL_CLS_FITNESS:  return AOS_SYM_SPEEDOMETER;
    case BL_CLS_DEVBOARD: return AOS_SYM_DEVELOPER_BOARD;
    default:              return AOS_SYM_BLUETOOTH;
    }
}

lv_color_t bl_class_color(bl_class_t c)
{
    switch (c) {
    case BL_CLS_PHONE:    return lv_color_hex(0x0A84FF);
    case BL_CLS_COMPUTER: return lv_color_hex(0x5E5CE6);
    case BL_CLS_TABLET:   return lv_color_hex(0x5E5CE6);
    case BL_CLS_WATCH:    return lv_color_hex(0xFF9F0A);
    case BL_CLS_AUDIO:    return lv_color_hex(0xBF5AF2);
    case BL_CLS_TRACKER:  return lv_color_hex(0xFF375F);
    case BL_CLS_SENSOR:   return lv_color_hex(0x30D158);
    case BL_CLS_BEACON:   return lv_color_hex(0x40C8E0);
    case BL_CLS_HID:      return lv_color_hex(0x64D2FF);
    case BL_CLS_GAMEPAD:  return lv_color_hex(0xFF6482);
    case BL_CLS_TV:       return lv_color_hex(0xAC8E68);
    case BL_CLS_HEALTH:   return lv_color_hex(0xFF453A);
    case BL_CLS_LIGHT:    return lv_color_hex(0xFFD60A);
    case BL_CLS_FITNESS:  return lv_color_hex(0x66D4CF);
    case BL_CLS_DEVBOARD: return lv_color_hex(0xE5484D);
    default:              return lv_color_hex(0x636366);
    }
}

/* path loss exponents: open air, a house, an office full of things */
static const float ENV_N[] = { 2.0f, 2.7f, 3.3f };

float bl_env_n(void)
{
    int e = BL.env < 0 || BL.env > 2 ? 1 : BL.env;
    return ENV_N[e];
}

/* -------------------------------------------------------------------------- */
/* The table                                                                   */
/* -------------------------------------------------------------------------- */

bool bl_scan_init(void)
{
    if (!BL.dev) BL.dev = ps_alloc(sizeof(bl_dev_t) * BL_DEV_MAX);
    if (!BL.rx) BL.rx = ps_alloc(sizeof(aos_ble_adv_t) * RX_MAX);
    return BL.dev && BL.rx;
}

void bl_scan_free(void)
{
    free(BL.dev);
    free(BL.rx);
    BL.dev = NULL;
    BL.rx = NULL;
    BL.ndev = 0;
}

void bl_scan_apply(void)
{
    bool want = !BL.paused && (!BL.hidden || BL.log_csv || BL.mqtt);
    if (!want) {
        if (aos_hal_ble_scanning()) aos_hal_ble_scan_stop();
        return;
    }
    if (!aos_hal_bt_enabled()) {
        BL.bt_off = true;
        return;
    }
    BL.bt_off = !aos_hal_ble_scan_start(BL.active, BL.duty);
}

int bl_find(const uint8_t addr[6])
{
    for (int i = 0; i < BL.ndev; i++)
        if (!memcmp(BL.dev[i].addr, addr, 6)) return i;
    return -1;
}

bool bl_alive(const bl_dev_t *d)
{
    return now_ms() - d->last_ms < BL_GONE_MS;
}

/* A new record; when the table is full, the one not heard for longest that
 * is neither a favourite nor open on the screen makes room. */
static bl_dev_t *dev_new(const aos_ble_adv_t *a)
{
    int i = BL.ndev;
    if (i >= BL_DEV_MAX) {
        int old = -1;
        for (int k = 0; k < BL.ndev; k++) {
            if (BL.dev[k].fav || k == BL.sel) continue;
            if (old < 0 || (int32_t)(BL.dev[k].last_ms - BL.dev[old].last_ms) < 0) old = k;
        }
        if (old < 0) return NULL;
        i = old;
    } else {
        BL.ndev++;
    }
    bl_dev_t *d = &BL.dev[i];
    memset(d, 0, sizeof *d);
    memcpy(d->addr, a->addr, 6);
    d->addr_type = a->addr_type;
    d->first_ms = a->t_ms;
    d->rssi_min = 127;
    d->rssi_max = -127;
    d->rssi_avg = a->rssi;
    for (int k = 0; k < BL_HIST; k++) d->hist[k] = BL_NO_RSSI;
    for (int k = 0; k < BL_SEN_HIST; k++) d->sen_t[k] = d->sen_h[k] = NAN;
    d->hist_sec = a->t_ms / 1000;
    bl_ad_clear(&d->ad);
    BL.gen++;
    return d;
}

static void hist_roll(int8_t *hist, uint32_t *last, uint32_t sec)
{
    if (sec <= *last) return;
    uint32_t gap = sec - *last;
    if (gap > BL_HIST) gap = BL_HIST;
    for (uint32_t k = 1; k <= gap; k++) hist[(*last + k) % BL_HIST] = BL_NO_RSSI;
    *last = sec;
}

static void air_roll(uint32_t sec)
{
    bl_air_t *A = &BL.air;
    if (!A->sec) A->sec = sec;
    if (sec <= A->sec) return;
    uint32_t gap = sec - A->sec;
    if (gap > BL_AIR_HIST) gap = BL_AIR_HIST;
    for (uint32_t k = 1; k <= gap; k++) {
        A->pkts[(A->sec + k) % BL_AIR_HIST] = 0;
        A->devs[(A->sec + k) % BL_AIR_HIST] = 0;
    }
    A->sec = sec;
}

static void sensor_keep(bl_dev_t *d, uint32_t t)
{
    uint32_t slot = t / 60000;
    if (!d->sen_slot) d->sen_slot = slot;
    if (slot > d->sen_slot) {
        uint32_t gap = slot - d->sen_slot;
        if (gap > BL_SEN_HIST) gap = BL_SEN_HIST;
        for (uint32_t k = 1; k <= gap; k++) d->sen_t[(d->sen_slot + k) % BL_SEN_HIST] = d->sen_h[(d->sen_slot + k) % BL_SEN_HIST] = NAN;
        d->sen_slot = slot;
    }
    int i = slot % BL_SEN_HIST;
    if (d->sen.mask & BL_V_TEMP) d->sen_t[i] = d->sen.temp;
    if (d->sen.mask & BL_V_HUM) d->sen_h[i] = d->sen.hum;
}

/* What the bytes say, again: after a change in the advertisement or the
 * scan response. */
static void dev_decode(bl_dev_t *d, const uint8_t *data, int len, uint32_t t)
{
    bl_ad_merge(&d->ad, data, len);
    d->cls = bl_classify(&d->ad, &d->label);
    bl_sensor_t s;
    /* a beacon's calibrated power alone is not a reading */
    if (bl_sensor_decode(&d->ad, d->addr, &s) && ((s.mask & ~(uint32_t)BL_V_RSSI1M) || s.encrypted)) {
        /* some formats send their values a few at a time (BTHome objects,
         * Eddystone's frames): what did not come this time is kept */
        uint32_t had = d->has_sen ? d->sen.mask : 0;
        bl_sensor_t old = d->sen;
        d->sen = s;
        if (had & ~s.mask & BL_V_TEMP) { d->sen.temp = old.temp; d->sen.mask |= BL_V_TEMP; }
        if (had & ~s.mask & BL_V_HUM) { d->sen.hum = old.hum; d->sen.mask |= BL_V_HUM; }
        if (had & ~s.mask & BL_V_BATT) { d->sen.batt = old.batt; d->sen.mask |= BL_V_BATT; }
        if (had & ~s.mask & BL_V_VOLT) { d->sen.volt = old.volt; d->sen.mask |= BL_V_VOLT; }
        if (had & ~s.mask & BL_V_PRESS) { d->sen.press = old.press; d->sen.mask |= BL_V_PRESS; }
        d->has_sen = true;
        d->sen_ms = t;
        sensor_keep(d, t);
    }
    bl_beacon_t b;
    if (bl_beacon_decode(&d->ad, &b)) {
        d->bc = b;
        d->has_bc = true;
    }
}

static void ingest(const aos_ble_adv_t *a)
{
    int i = bl_find(a->addr);
    bl_dev_t *d = i >= 0 ? &BL.dev[i] : dev_new(a);
    if (!d) return;
    uint32_t sec = a->t_ms / 1000;

    air_roll(sec);
    BL.air.pkts[sec % BL_AIR_HIST]++;
    BL.air.total++;

    hist_roll(d->hist, &d->hist_sec, sec);
    int8_t *bin = &d->hist[sec % BL_HIST];
    if (*bin == BL_NO_RSSI) BL.air.devs[sec % BL_AIR_HIST]++;
    if (a->rssi > *bin) *bin = a->rssi;

    d->rssi = a->rssi;
    if (a->rssi < d->rssi_min) d->rssi_min = a->rssi;
    if (a->rssi > d->rssi_max) d->rssi_max = a->rssi;
    d->rssi_avg += (a->rssi - d->rssi_avg) * 0.2f;
    d->last_ms = a->t_ms;
    d->addr_type = a->addr_type;
    d->kinds |= (uint8_t)(1u << (a->kind & 7));

    if (a->kind == AOS_BLE_ADV_SCAN_RSP) {
        d->n_rsp++;
        if (a->len != d->rsp_len || memcmp(a->data, d->rsp, a->len)) {
            memcpy(d->rsp, a->data, a->len);
            d->rsp_len = a->len;
            d->n_changes++;
            dev_decode(d, a->data, a->len, a->t_ms);
        }
        return;
    }
    d->n_adv++;
    /* The gap: a device sends each advertisement on three channels within a
     * few ms and the scanner hears one at a time, missing some while it
     * looks elsewhere. The interval is the low envelope of the gaps: down
     * fast, up slowly. */
    if (d->prev_adv_ms) {
        uint32_t gap = a->t_ms - d->prev_adv_ms;
        if (gap >= 15 && gap < 20000) {
            if (d->itvl_ms <= 0) d->itvl_ms = gap;
            else if (gap < d->itvl_ms) d->itvl_ms += (gap - d->itvl_ms) * 0.35f;
            else d->itvl_ms += (gap - d->itvl_ms) * 0.01f;
        }
    }
    d->prev_adv_ms = a->t_ms;
    if (a->len != d->adv_len || memcmp(a->data, d->adv, a->len)) {
        memcpy(d->adv, a->data, a->len);
        d->adv_len = a->len;
        d->n_changes++;
        dev_decode(d, a->data, a->len, a->t_ms);
    }
}

void bl_scan_drain(void)
{
    if (!BL.dev || !BL.rx) return;
    uint32_t t = now_ms();
    air_roll(t / 1000);
    int got = 0;
    for (int round = 0; round < 8; round++) {
        int n = aos_hal_ble_scan_read(BL.rx, RX_MAX);
        for (int k = 0; k < n; k++) ingest(&BL.rx[k]);
        got += n;
        if (n < RX_MAX) break;
    }
    BL.air.lost = aos_hal_ble_scan_lost();
    /* packets a second over the last two whole seconds, smoothed */
    uint32_t s = t / 1000;
    float pps = (BL.air.pkts[(s + BL_AIR_HIST - 1) % BL_AIR_HIST] + BL.air.pkts[(s + BL_AIR_HIST - 2) % BL_AIR_HIST]) * 0.5f;
    BL.air.pps += (pps - BL.air.pps) * 0.3f;
    (void)got;
    /* the open device's history rolls even when it is silent */
    for (int i = 0; i < BL.ndev; i++) hist_roll(BL.dev[i].hist, &BL.dev[i].hist_sec, s);
}

int bl_rssi_recent(const bl_dev_t *d, int secs)
{
    uint32_t s = now_ms() / 1000;
    int best = BL_NO_RSSI;
    if (s - d->hist_sec > (uint32_t)secs) return BL_NO_RSSI;
    for (int k = 0; k < secs && k < BL_HIST; k++) {
        uint32_t sec = s - k;
        if (sec > d->hist_sec) continue;
        int v = d->hist[sec % BL_HIST];
        if (v > best) best = v;
    }
    return best;
}

void bl_forget_all(void)
{
    /* the favourites stay, emptied of what they had */
    int n = 0;
    for (int i = 0; i < BL.ndev; i++) {
        if (!BL.dev[i].fav) continue;
        bl_dev_t keep = BL.dev[i];
        bl_dev_t *d = &BL.dev[n++];
        memset(d, 0, sizeof *d);
        memcpy(d->addr, keep.addr, 6);
        d->addr_type = keep.addr_type;
        d->fav = true;
        memcpy(d->alias, keep.alias, sizeof d->alias);
        d->ad = keep.ad;
        d->cls = keep.cls;
        d->label = keep.label;
        d->rssi_min = 127;
        d->rssi_max = -127;
        for (int k = 0; k < BL_HIST; k++) d->hist[k] = BL_NO_RSSI;
        for (int k = 0; k < BL_SEN_HIST; k++) d->sen_t[k] = d->sen_h[k] = NAN;
    }
    BL.ndev = n;
    BL.sel = -1;
    memset(&BL.air, 0, sizeof BL.air);
    BL.gen++;
}

/* -------------------------------------------------------------------------- */
/* Names and lines                                                             */
/* -------------------------------------------------------------------------- */

const char *bl_dev_name(const bl_dev_t *d)
{
    if (d->alias[0]) return d->alias;
    if (d->ad.name[0]) return d->ad.name;
    if (d->label) return _(d->label);
    if (d->ad.nmfg) {
        const char *c = bl_company_name(d->ad.mfg[0].company);
        if (c) return c;
    }
    if (d->has_bc) return _("Baliza");
    return _("Sin nombre");
}

void bl_sensor_line(const bl_sensor_t *s, char *out, size_t n)
{
    size_t k = 0;
    out[0] = 0;
    char v[24];
#define ADD(...) do { if (k < n) { if (k) k += snprintf(out + k, n - k, " · "); if (k < n) k += snprintf(out + k, n - k, __VA_ARGS__); } } while (0)
    if (s->encrypted) { ADD("%s", _("cifrado")); return; }
    if (s->mask & BL_V_TEMP) { bl_fmt_num(v, sizeof v, s->temp, 1); ADD("%s °C", v); }
    if (s->mask & BL_V_HUM) { bl_fmt_num(v, sizeof v, s->hum, 0); ADD("%s %%", v); }
    if (s->mask & BL_V_PRESS) { bl_fmt_num(v, sizeof v, s->press, 0); ADD("%s hPa", v); }
    if (s->mask & BL_V_CO2) ADD("%d ppm", (int)s->co2);
    if (s->mask & BL_V_PM25) ADD("PM2,5 %d", (int)s->pm25);
    if (s->mask & BL_V_LUX) ADD("%d lx", (int)s->lux);
    if (s->mask & BL_V_MOIST) ADD("%s %d %%", _("suelo"), (int)s->moist);
    if (s->mask & BL_V_WEIGHT) { bl_fmt_num(v, sizeof v, s->weight, 2); ADD("%s kg", v); }
    if (s->mask & BL_V_POWER) { bl_fmt_num(v, sizeof v, s->power, 1); ADD("%s W", v); }
    if (s->mask & BL_V_OPEN) ADD("%s", s->open ? _("abierto") : _("cerrado"));
    if (s->mask & BL_V_MOTION) ADD("%s", s->motion ? _("movimiento") : _("quieto"));
    if (s->mask & BL_V_HR) ADD("%d lpm", s->hr);
    if (s->mask & BL_V_BATT) ADD("%s %d %%", _("bat."), s->batt);
    else if (s->mask & BL_V_VOLT) { bl_fmt_num(v, sizeof v, s->volt, 2); ADD("%s V", v); }
#undef ADD
}

void bl_dev_sub(const bl_dev_t *d, char *out, size_t n)
{
    if (d->has_sen && d->sen.mask) {
        bl_sensor_line(&d->sen, out, n);
        return;
    }
    const char *parts[4];
    int np = 0;
    const char *co = d->ad.nmfg ? bl_company_name(d->ad.mfg[0].company) : NULL;
    if (co && strcmp(co, bl_dev_name(d))) parts[np++] = co;
    const char *ap[2];
    if (d->ad.nmfg && d->ad.mfg[0].company == 0x004C && bl_apple_types(&d->ad.mfg[0], ap, 1) > 0) parts[np++] = _(ap[0]);
    else if (d->label && !d->ad.name[0] && !d->alias[0]) { /* already the name */ }
    else if (d->label) parts[np++] = _(d->label);
    else if (d->cls != BL_CLS_UNKNOWN) parts[np++] = _(bl_class_name(d->cls));
    parts[np++] = _(bl_addr_kind_name(bl_addr_kind(d->addr, d->addr_type)));
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < np && k < n; i++) k += snprintf(out + k, n - k, "%s%s", i ? " · " : "", parts[i]);
}

static bool passes(const bl_dev_t *d, int filter)
{
    if (BL.hide_gone && !bl_alive(d) && !d->fav) return false;
    if (BL.min_rssi > -100 && (d->rssi_avg < BL.min_rssi || !bl_alive(d)) && !d->fav) return false;
    switch (filter) {
    case BL_FILT_NAMED:  return d->ad.name[0] || d->alias[0];
    case BL_FILT_FAV:    return d->fav;
    case BL_FILT_CONN:   return (d->kinds & ((1u << AOS_BLE_ADV_IND) | (1u << AOS_BLE_ADV_DIRECT_IND))) != 0;
    case BL_FILT_SENSOR: return d->has_sen;
    case BL_FILT_BEACON: return d->has_bc;
    case BL_FILT_APPLE:  return d->ad.nmfg && d->ad.mfg[0].company == 0x004C;
    default:             return true;
    }
}

static int s_sort;
static int cmp_dev(const void *pa, const void *pb)
{
    const bl_dev_t *a = &BL.dev[*(const int *)pa], *b = &BL.dev[*(const int *)pb];
    /* favourites first, then the living, then the order asked */
    if (a->fav != b->fav) return a->fav ? -1 : 1;
    bool la = bl_alive(a), lb = bl_alive(b);
    if (la != lb) return la ? -1 : 1;
    if (s_sort == BL_SORT_NAME) {
        bool na = a->ad.name[0] || a->alias[0], nb = b->ad.name[0] || b->alias[0];
        if (na != nb) return na ? -1 : 1;
        int c = strcasecmp(bl_dev_name(a), bl_dev_name(b));
        if (c) return c;
    } else if (s_sort == BL_SORT_RECENT) {
        if (a->first_ms != b->first_ms) return (int32_t)(b->first_ms - a->first_ms) > 0 ? 1 : -1;
    }
    float ra = la ? a->rssi_avg : -200, rb = lb ? b->rssi_avg : -200;
    if (ra != rb) return ra > rb ? -1 : 1;
    return memcmp(a->addr, b->addr, 6);
}

int bl_sorted(int *out, int max, int filter, int sort)
{
    int n = 0;
    for (int i = 0; i < BL.ndev && n < max; i++)
        if (passes(&BL.dev[i], filter)) out[n++] = i;
    s_sort = sort;
    qsort(out, n, sizeof out[0], cmp_dev);
    return n;
}

/* -------------------------------------------------------------------------- */
/* nombres.txt                                                                 */
/* -------------------------------------------------------------------------- */

static uint32_t s_names_mtime;
static uint32_t s_names_check;

static void names_path(char *out, size_t n)
{
    snprintf(out, n, "%s/ble/nombres.txt", card_root());
}

static uint32_t file_mtime(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (uint32_t)st.st_mtime ^ (uint32_t)st.st_size : 0;
}

static void names_load(void)
{
    char path[160];
    names_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    s_names_mtime = file_mtime(path);
    if (!f) return;
    for (int i = 0; i < BL.ndev; i++) {
        BL.dev[i].fav = false;
        BL.dev[i].alias[0] = 0;
    }
    char line[128];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1++ = 0;
        char *p2 = strchr(p1, '|');
        if (p2) *p2++ = 0;
        uint8_t a[6];
        if (!bl_parse_addr(line, a)) continue;
        int i = bl_find(a);
        if (i < 0) {
            if (BL.ndev >= BL_DEV_MAX) continue;
            /* a favourite not heard yet this time: a record waiting for it */
            aos_ble_adv_t fake = { .t_ms = 0 };
            memcpy(fake.addr, a, 6);
            bl_dev_t *d = dev_new(&fake);
            if (!d) continue;
            d->last_ms = now_ms() - BL_GONE_MS - 1;
            d->first_ms = d->last_ms;
            i = (int)(d - BL.dev);
        }
        BL.dev[i].fav = strchr(p1, '*') != NULL;
        if (p2) snprintf(BL.dev[i].alias, sizeof BL.dev[i].alias, "%.27s", p2);
    }
    fclose(f);
    BL.gen++;
}

void bl_names_save(void)
{
    char path[160], dir[160];
    snprintf(dir, sizeof dir, "%s/ble", card_root());
    mkdir(dir, 0777);
    names_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("# BLE de P4OS: favoritos y nombres. dirección|*|nombre\n", f);
    for (int i = 0; i < BL.ndev; i++) {
        const bl_dev_t *d = &BL.dev[i];
        if (!d->fav && !d->alias[0]) continue;
        char a[20], l[80];
        bl_fmt_addr(d->addr, a, sizeof a);
        snprintf(l, sizeof l, "%s|%s|%s\n", a, d->fav ? "*" : "", d->alias);
        fputs(l, f);
    }
    fclose(f);
    s_names_mtime = file_mtime(path);
}

void bl_names_poll(void)
{
    uint32_t t = now_ms();
    if (s_names_check && t - s_names_check < 2000) return;
    s_names_check = t;
    char path[160];
    names_path(path, sizeof path);
    uint32_t m = file_mtime(path);
    if (m != s_names_mtime) names_load();
}

/* -------------------------------------------------------------------------- */
/* CSV and MQTT                                                                */
/* -------------------------------------------------------------------------- */

bool bl_mqtt_ready(void)
{
    return aos_mqtt_state() == BL_MQTT_CONNECTED;
}

static void sensor_json(const bl_dev_t *d, char *j, size_t n)
{
    const bl_sensor_t *s = &d->sen;
    char a[20];
    bl_fmt_addr(d->addr, a, sizeof a);
    size_t k = snprintf(j, n, "{\"address\":\"%s\",\"name\":\"", a);
    /* the name, with what would break the JSON left out */
    const char *nm = d->alias[0] ? d->alias : d->ad.name;
    for (const char *p = nm; *p && k < n - 8; p++)
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) j[k++] = *p;
    k += snprintf(j + k, n - k, "\",\"format\":\"%s\",\"rssi\":%d", s->format ? s->format : "", d->rssi);
#define F(bit, key, v, fmt) if ((s->mask & (bit)) && k < n) k += snprintf(j + k, n - k, ",\"" key "\":" fmt, v)
    F(BL_V_TEMP, "temperature", (double)s->temp, "%.2f");
    F(BL_V_HUM, "humidity", (double)s->hum, "%.1f");
    F(BL_V_PRESS, "pressure", (double)s->press, "%.1f");
    F(BL_V_BATT, "battery", s->batt, "%d");
    F(BL_V_VOLT, "voltage", (double)s->volt, "%.3f");
    F(BL_V_CO2, "co2", (double)s->co2, "%.0f");
    F(BL_V_PM25, "pm25", (double)s->pm25, "%.0f");
    F(BL_V_LUX, "illuminance", (double)s->lux, "%.0f");
    F(BL_V_MOIST, "moisture", (double)s->moist, "%.0f");
    F(BL_V_WEIGHT, "weight", (double)s->weight, "%.2f");
    F(BL_V_OPEN, "open", s->open, "%d");
    F(BL_V_MOTION, "motion", s->motion, "%d");
    F(BL_V_HR, "heart_rate", s->hr, "%d");
#undef F
    if (k < n) snprintf(j + k, n - k, "}");
}

void bl_log_tick(void)
{
    static uint32_t last_min;
    if (!BL.log_csv && !BL.mqtt) return;
    uint32_t t = now_ms();
    uint32_t min = t / 60000;
    if (min == last_min) return;
    last_min = min;
    bool mq = BL.mqtt && bl_mqtt_ready();
    FILE *f = NULL;
    char stamp[32] = "";
    if (BL.log_csv) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char dir[160], path[200];
        snprintf(dir, sizeof dir, "%s/ble", card_root());
        mkdir(dir, 0777);
        if (aos_hal_time_is_valid()) {
            snprintf(path, sizeof path, "%s/sensores-%04d-%02d-%02d.csv", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else {
            /* no clock yet: the minutes since the board started */
            snprintf(path, sizeof path, "%s/sensores-sin-hora.csv", dir);
            snprintf(stamp, sizeof stamp, "+%u min", (unsigned)min);
        }
        struct stat st;
        bool fresh = stat(path, &st) != 0;
        f = fopen(path, "a");
        if (f && fresh) fputs("hora,direccion,nombre,formato,rssi,temperatura,humedad,presion,bateria,voltaje\n", f);
    }
    for (int i = 0; i < BL.ndev; i++) {
        const bl_dev_t *d = &BL.dev[i];
        if (!d->has_sen || d->sen.encrypted || t - d->sen_ms > 120000) continue;
        if (f) {
            char a[20], line[256], tv[16] = "", hv[16] = "", pv[16] = "", bv[8] = "", vv[16] = "";
            bl_fmt_addr(d->addr, a, sizeof a);
            const bl_sensor_t *s = &d->sen;
            if (s->mask & BL_V_TEMP) snprintf(tv, sizeof tv, "%.2f", (double)s->temp);
            if (s->mask & BL_V_HUM) snprintf(hv, sizeof hv, "%.1f", (double)s->hum);
            if (s->mask & BL_V_PRESS) snprintf(pv, sizeof pv, "%.1f", (double)s->press);
            if (s->mask & BL_V_BATT) snprintf(bv, sizeof bv, "%d", s->batt);
            if (s->mask & BL_V_VOLT) snprintf(vv, sizeof vv, "%.3f", (double)s->volt);
            char nm[32];
            snprintf(nm, sizeof nm, "%s", d->alias[0] ? d->alias : d->ad.name);
            for (char *p = nm; *p; p++) if (*p == ',' || *p == '"') *p = ' ';
            snprintf(line, sizeof line, "%s,%s,%s,%s,%d,%s,%s,%s,%s,%s\n", stamp, a, nm, s->format ? s->format : "",
                     d->rssi, tv, hv, pv, bv, vv);
            fputs(line, f);
        }
        if (mq) {
            char topic[96], j[400], a[16];
            snprintf(a, sizeof a, "%02x%02x%02x%02x%02x%02x", d->addr[0], d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5]);
            snprintf(topic, sizeof topic, "%s/ble/%s", aos_hal_device_name(), a);
            sensor_json(d, j, sizeof j);
            aos_mqtt_publish(topic, j, 0, false);
        }
    }
    if (f) fclose(f);
}

/* the first load of the names, once the table exists */
void bl_names_first(void);
void bl_names_first(void)
{
    names_load();
}
