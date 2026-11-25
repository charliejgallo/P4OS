/*
 * P4OS - what a Tuya device is and what its datapoints mean (aos_zigbee.h),
 * from the card's zigbee/tuya.txt (tools/zigbee/gen_tuya.py, from
 * Zigbee2MQTT's definitions); on the board and in the simulator alike.
 *
 * A device is looked up once, by its Zigbee model and manufacturer name
 * ("TS0601", "_TZE200_aoclfnxz"), reading the file through (some 190 KB):
 * its vendor, model and description, and its set of datapoints as the
 * file has it ("1:temperature:n:10,u°C 2:humidity:n:1,u% ..."), kept in
 * PSRAM. A datapoint's text is made from that line each time. Without the
 * file (no card, or the pack not installed) nothing is kept and the next
 * call looks again.
 */
#include "aos_zigbee.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#define tz_malloc(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define tz_calloc(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define tz_malloc(n) malloc(n)
#define tz_calloc(n) calloc(1, (n))
#endif

#define CACHE 40
#define LINE  1536

typedef struct {
    char  key[68];                      /* model '/' manufacturer */
    bool  found;
    char  vendor[24], model[40], desc[64];
    char *set;                          /* the datapoints, NULL: none */
} entry_t;

static entry_t        *s_cache;         /* PSRAM, CACHE of them */
static int             s_n, s_next;
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

/* the most common names in Spanish; the rest as the file has them */
static const struct { const char *en, *es; } NAMES[] = {
    { "state", N_("estado") },
    { "battery", N_("batería") },
    { "child_lock", N_("bloqueo de teclas") },
    { "action", N_("acción") },
    { "position", N_("posición") },
    { "local_temperature", N_("temperatura") },
    { "current_heating_setpoint", N_("consigna") },
    { "temperature", N_("temperatura") },
    { "illuminance", N_("luz") },
    { "illuminance_lux", N_("luz") },
    { "local_temperature_calibration", N_("corrección") },
    { "temperature_calibration", N_("corrección") },
    { "energy", N_("energía") },
    { "power", N_("potencia") },
    { "power_on_behavior", N_("al volver la luz") },
    { "humidity", N_("humedad") },
    { "backlight_mode", N_("luz de fondo") },
    { "voltage", N_("tensión") },
    { "system_mode", N_("modo") },
    { "presence", N_("presencia") },
    { "occupancy", N_("presencia") },
    { "frost_protection", N_("antiheladas") },
    { "current", N_("corriente") },
    { "running_state", N_("funcionando") },
    { "motor_direction", N_("sentido del motor") },
    { "battery_state", N_("estado de la pila") },
    { "window_detection", N_("ventana abierta") },
    { "countdown", N_("cuenta regresiva") },
    { "co2", "CO2" },
    { "water_leak", N_("agua") },
    { "contact", N_("contacto") },
    { "smoke", N_("humo") },
    { "tamper", N_("tapa abierta") },
    { "motion", N_("movimiento") },
    { "alarm", N_("alarma") },
    { "brightness", N_("brillo") },
    { "soil_moisture", N_("humedad del suelo") },
    { "pm25", "PM2.5" },
    { "voc", "VOC" },
    { "formaldehyd", N_("formaldehído") },
    { "distance", N_("distancia") },
    { "sensitivity", N_("sensibilidad") },
    { "mode", N_("modo") },
    { "preset", N_("modo") },
    { "valve_position", N_("válvula") },
    { "eco_temperature", N_("temperatura eco") },
    { "comfort_temperature", N_("temperatura de confort") },
    { "power_factor", N_("factor de potencia") },
    { "ac_frequency", N_("frecuencia") },
    { "produced_energy", N_("energía entregada") },
    { "temperature_unit", N_("unidad") },
};

static void nice_name(const char *raw, char *out, size_t n)
{
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (!strcmp(raw, NAMES[i].en)) {
            snprintf(out, n, "%s", aos_tr(NAMES[i].es));
            return;
        }
    /* state_l2: the second output */
    int k;
    if (sscanf(raw, "state_l%d", &k) == 1) {
        snprintf(out, n, _("salida %d"), k);
        return;
    }
    snprintf(out, n, "%s", raw);
    for (char *p = out; *p; p++)
        if (*p == '_') *p = ' ';
}

static void copy_field(char *dst, size_t n, const char *from, const char *to)
{
    size_t l = to ? (size_t)(to - from) : strlen(from);
    if (l >= n) l = n - 1;
    memcpy(dst, from, l);
    dst[l] = '\0';
}

/* the entry of a device, looked up in the file if it is not kept yet; NULL
 * without the file. With s_mx held. */
static entry_t *lookup(const aos_zigbee_dev_t *d)
{
    if (!d->manuf[0]) return NULL;
    char key[68];
    snprintf(key, sizeof key, "%s/%s", d->model, d->manuf);
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_cache[i].key, key)) return &s_cache[i];
    const char *root = aos_hal_path_sd_root();
    if (!root) return NULL;
    char path[96];
    snprintf(path, sizeof path, "%s/zigbee/tuya.txt", root);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    /* zeroed: an entry used for the first time has no key and no set to free
     * (not zeroed, the first lookup freed garbage: the portal aborted, on
     * the board, 2026-11-25) */
    if (!s_cache) s_cache = tz_calloc(sizeof(entry_t) * CACHE);
    char *line = tz_malloc(LINE);
    if (!s_cache || !line) {
        free(line);
        fclose(f);
        return NULL;
    }
    entry_t *e = &s_cache[s_n < CACHE ? s_n++ : (s_next++ % CACHE)];
    if (e->key[0]) free(e->set);
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", key);
    /* "M\tmodel\tmanufacturer\tset\tvendor|model|description"; the S lines
     * come first: the set's is found on a second pass from the start */
    long set = -1;
    size_t ml = strlen(d->model), fl = strlen(d->manuf);
    while (fgets(line, LINE, f)) {
        if (line[0] != 'M' || line[1] != '\t') continue;
        char *p = line + 2;
        if (strncmp(p, d->model, ml) || p[ml] != '\t' || strncmp(p + ml + 1, d->manuf, fl) || p[ml + 1 + fl] != '\t')
            continue;
        p += ml + 1 + fl + 1;
        line[strcspn(line, "\r\n")] = '\0';
        set = *p == '-' ? -1 : strtol(p, NULL, 10);
        char *who = strchr(p, '\t');
        if (who) {
            who++;
            char *b1 = strchr(who, '|'), *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
            copy_field(e->vendor, sizeof e->vendor, who, b1);
            if (b1) copy_field(e->model, sizeof e->model, b1 + 1, b2);
            if (b2) copy_field(e->desc, sizeof e->desc, b2 + 1, NULL);
        }
        e->found = true;
        break;
    }
    if (set >= 0) {
        rewind(f);
        while (fgets(line, LINE, f) && line[0] != 'M') {
            if (line[0] != 'S' || strtol(line + 2, NULL, 10) != set) continue;
            line[strcspn(line, "\r\n")] = '\0';
            char *t = strchr(line + 2, '\t');
            if (t) {
                size_t l = strlen(t + 1);
                e->set = tz_malloc(l + 1);
                if (e->set) memcpy(e->set, t + 1, l + 1);
            }
            break;
        }
    }
    free(line);
    fclose(f);
    return e;
}

bool aos_zigbee_ident(const aos_zigbee_dev_t *d, char *vendor, size_t vn, char *model, size_t mn, char *desc, size_t dn)
{
    pthread_mutex_lock(&s_mx);
    entry_t *e = lookup(d);
    bool ok = e && e->found;
    if (ok) {
        if (vn) snprintf(vendor, vn, "%s", e->vendor);
        if (mn) snprintf(model, mn, "%s", e->model);
        if (dn) snprintf(desc, dn, "%s", e->desc);
    }
    pthread_mutex_unlock(&s_mx);
    return ok;
}

/* "12:name:kind:arg" of dp in the set, copied out; false if not there */
static bool dp_entry(const char *set, uint8_t dp, char *out, size_t n)
{
    for (const char *p = set; p && *p;) {
        const char *end = strchr(p, ' ');
        if (strtol(p, NULL, 10) == dp) {
            copy_field(out, n, p, end);
            return true;
        }
        p = end ? end + 1 : NULL;
    }
    return false;
}

bool aos_zigbee_dp_text(const aos_zigbee_dev_t *d, const aos_zigbee_dp_t *q, char *name, size_t nn, char *val, size_t vn,
                        bool *is_bool)
{
    char ent[512];
    pthread_mutex_lock(&s_mx);
    entry_t *e = lookup(d);
    bool ok = e && e->set && dp_entry(e->set, q->dp, ent, sizeof ent);
    pthread_mutex_unlock(&s_mx);
    if (is_bool) *is_bool = q->type == 1;
    if (!ok) return false;
    /* dp : name : kind [: arg] */
    char *nm = strchr(ent, ':');
    if (!nm) return false;
    nm++;
    char *kd = strchr(nm, ':');
    if (!kd) return false;
    *kd++ = '\0';
    char *arg = strchr(kd, ':');
    if (arg) *arg++ = '\0';
    nice_name(nm, name, nn);
    bool numeric = q->type == 1 || q->type == 2 || q->type == 4 || q->type == 5;
    if (kd[0] == 'b' && numeric) {
        bool on = q->v != 0;
        if (arg && arg[0] == 'i') on = !on;
        snprintf(val, vn, "%s", on ? _("sí") : _("no"));
        if (is_bool) *is_bool = q->type == 1;
    } else if (kd[0] == 'n' && numeric) {
        /* "10,u°C": a divisor; "x10": a multiplier */
        const char *unit = arg ? strstr(arg, ",u") : NULL;
        double v = q->v;
        if (arg && arg[0] == 'x') v *= atof(arg + 1);
        else if (arg && atof(arg) > 1) v /= atof(arg);
        int dec = arg && arg[0] != 'x' ? (atof(arg) >= 1000 ? 3 : atof(arg) >= 100 ? 2 : atof(arg) >= 10 ? 1 : 0) : 0;
        snprintf(val, vn, "%.*f%s%s", dec, v, unit ? " " : "", unit ? unit + 2 : "");
    } else if (kd[0] == 'e' && numeric && arg) {
        /* "0=off,1=on,2=previous" */
        char want[16];
        snprintf(want, sizeof want, "%ld=", (long)q->v);
        const char *p = arg;
        val[0] = '\0';
        while (p && *p) {
            const char *end = strchr(p, ',');
            if (!strncmp(p, want, strlen(want))) {
                copy_field(val, vn, p + strlen(want), end);
                break;
            }
            p = end ? end + 1 : NULL;
        }
        if (!val[0]) snprintf(val, vn, "%ld", (long)q->v);
    } else if (numeric) {
        snprintf(val, vn, "%ld", (long)q->v);
    } else {
        int l = q->len < (int)sizeof q->raw ? q->len : (int)sizeof q->raw, k = 0;
        bool text = q->type == 3;
        for (int m = 0; m < l && text; m++) text = q->raw[m] >= 32 && q->raw[m] < 127;
        if (text) {
            k = snprintf(val, vn, "%.*s", l, (const char *)q->raw);
        } else {
            for (int m = 0; m < l && k + 3 < (int)vn; m++) k += snprintf(val + k, vn - (size_t)k, "%02x", q->raw[m]);
        }
        if (q->len > l && k + 4 < (int)vn) snprintf(val + k, vn - (size_t)k, "...");
    }
    return true;
}
