/*
 * P4OS - Home Assistant client service. The contract is in aos_ha.h; this is
 * how it keeps it.
 *
 * The thread's life: wait for a token and the network, open
 * ws://<host>/api/websocket, authenticate, then load in this order
 *
 *     get_config                              the house's name
 *     config/area_registry/list               areas, by id
 *     config/device_registry/list             which area each device is in
 *     config/entity_registry/list_for_display hidden entities, categories,
 *                                             an entity's own area
 *     get_states                              everything, once
 *     subscribe_events state_changed          and from then on, the changes
 *
 * and loop: incoming events update the table, queued calls go out, a ping
 * every 25 s tells a dead connection from a quiet house. Anything wrong
 * drops the connection and it starts over after a growing pause, except a
 * refused token, which waits until the token changes.
 *
 * An entity is shown if its domain is one the app knows how to draw, it is
 * not hidden and it is not a config/diagnostic entity (the "signal strength"
 * and "firmware" clutter every integration adds). Its area is its own or,
 * failing that, its device's - the same rule HA's dashboards use.
 *
 * list_for_display is the compact registry listing (HA 2023.3 on); a big
 * installation's full config/entity_registry/list runs to megabytes. If it
 * is refused the areas are simply left out.
 *
 * Everything the thread writes into the table is written under the mutex;
 * cJSON only lives inside the thread.
 */
#include "aos_ha.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "cJSON.h"
#include "aos_notif_internal.h"      /* HA as a provider of notifications */
#include <time.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Q_LEN        16
#define PING_EVERY_MS 25000
#define DEAD_AFTER_MS 60000

const char *const aos_ha_hvac_names[AOS_HA_HVAC_COUNT] = { "off", "heat", "cool", "heat_cool", "auto", "dry", "fan_only" };

static const char *const DOMAIN_NAME[HA_DOMAIN_COUNT] = {
    "light", "switch", "input_boolean", "fan", "cover", "climate", "lock",
    "media_player", "scene", "script", "button", "sensor", "binary_sensor",
};

typedef struct { char domain[24], service[32], id[64], data[192]; } call_t;

static struct {
    void *mx;
    bool started;
    aos_ha_entity_t *ent;
    int n;
    char area_id[AOS_HA_MAX_AREAS][40];
    char area_name[AOS_HA_MAX_AREAS][32];
    int na;
    volatile uint32_t version;
    volatile aos_ha_state_t state;
    char err[112];
    char location[48];
    char url[128];
    char token[600];
    volatile bool kick;             /* reconnect with the new configuration */
    call_t q[Q_LEN];
    int qh, qt;
    /* thread only */
    aos_ws_t *ws;
    int next_id;
    int sub_id, sub_notif, sub_p4os;
    /* one history at a time: the sheet that asked for it */
    struct {
        char id[64];
        int hours;
        volatile int state;         /* AOS_HA_HIST_* */
        int req_id;
        int n;
        float v[AOS_HA_HIST_MAX];
        uint32_t t[AOS_HA_HIST_MAX];    /* seconds since t0 */
        time_t t0, t1;
        float lo, hi;
    } hist;
    uint32_t last_rx, last_ping;
} H AOS_BSS_PSRAM;                  /* 11 KB (the history); no ISR or DMA touches it */

static void bump(void) { H.version++; }

static void set_state(aos_ha_state_t s, const char *why)
{
    aos_hal_mutex_lock(H.mx);
    H.state = s;
    if (why) snprintf(H.err, sizeof H.err, "%s", why);
    bump();
    aos_hal_mutex_unlock(H.mx);
}

static int domain_of(const char *eid)
{
    size_t l = strcspn(eid, ".");
    for (int d = 0; d < HA_DOMAIN_COUNT; d++)
        if (strlen(DOMAIN_NAME[d]) == l && !strncmp(eid, DOMAIN_NAME[d], l)) return d;
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Configuration                                                               */
/* -------------------------------------------------------------------------- */

static void cfg_load(void)
{
    if (!aos_hal_pref_get_str("ha_url", H.url, sizeof H.url)) H.url[0] = 0;
    if (!aos_hal_pref_get_str("ha_token", H.token, sizeof H.token)) H.token[0] = 0;
}

bool aos_ha_configured(void) { return H.url[0] && H.token[0]; }
const char *aos_ha_url(void) { return H.url; }
bool aos_ha_has_token(void) { return H.token[0] != 0; }

bool aos_ha_set_url(const char *url)
{
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || strlen(url) >= sizeof H.url) return false;
    char u[128];
    snprintf(u, sizeof u, "%s", url);
    size_t l = strlen(u);
    while (l && u[l - 1] == '/') u[--l] = 0;       /* "http://ha:8123/" is the same place */
    snprintf(H.url, sizeof H.url, "%s", u);
    aos_hal_pref_set_str("ha_url", H.url);
    H.kick = true;
    return true;
}

void aos_ha_set_token(const char *token)
{
    snprintf(H.token, sizeof H.token, "%s", token ? token : "");
    aos_hal_pref_set_str("ha_token", H.token);
    H.kick = true;
}

void aos_ha_reconnect(void) { H.kick = true; }

static void trim(char *s)
{
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
    size_t i = 0;
    while (isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, l - i + 1);
}

static void favs_set_all(const char *list);

#define TOKEN_KEPT "(guardado en P4OS)"

bool aos_ha_import_file(void)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return false;
    char path[160];
    snprintf(path, sizeof path, "%s/ha.txt", root);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char *lines[32];
    int nl = 0;
    char buf[800];
    bool got = false, token_moved = false;
    while (nl < 32 && fgets(buf, sizeof buf, f)) {
        char *eq = strchr(buf, '=');
        char key[16] = "", val[700] = "";
        if (eq && buf[0] != '#') {
            snprintf(key, sizeof key, "%.*s", (int)(eq - buf), buf);
            snprintf(val, sizeof val, "%s", eq + 1);
            trim(key);
            trim(val);
        }
        if (!strcmp(key, "url") && val[0] && strcmp(val, H.url)) {
            got |= aos_ha_set_url(val);
        } else if (!strcmp(key, "token") && val[0] && strcmp(val, TOKEN_KEPT)) {
            aos_ha_set_token(val);
            token_moved = got = true;
            snprintf(buf, sizeof buf, "token=" TOKEN_KEPT "\n");
        } else if (!strcmp(key, "favs") && val[0] && token_moved) {
            favs_set_all(val);
        }
        lines[nl++] = strdup(buf);
    }
    fclose(f);
    /* the token does not stay on the card in the clear */
    if (token_moved && (f = fopen(path, "w"))) {
        for (int i = 0; i < nl; i++) fputs(lines[i], f);
        fclose(f);
        aos_hal_log("ha", "token moved from ha.txt into the preferences");
    }
    for (int i = 0; i < nl; i++) free(lines[i]);
    return got;
}

/* -------------------------------------------------------------------------- */
/* Favourites                                                                  */
/* -------------------------------------------------------------------------- */

static char s_favs[2048];
static bool s_favs_loaded;

static void favs_load(void)
{
    if (s_favs_loaded) return;
    if (!aos_hal_pref_get_str("ha_favs", s_favs, sizeof s_favs)) s_favs[0] = 0;
    s_favs_loaded = true;
}

static void favs_set_all(const char *list)
{
    snprintf(s_favs, sizeof s_favs, "%s", list);
    s_favs_loaded = true;
    aos_hal_pref_set_str("ha_favs", s_favs);
    if (H.mx) {
        aos_hal_mutex_lock(H.mx);
        bump();
        aos_hal_mutex_unlock(H.mx);
    }
}

bool aos_ha_is_fav(const char *id)
{
    favs_load();
    size_t l = strlen(id);
    for (const char *p = s_favs; *p; ) {
        size_t k = strcspn(p, ",");
        if (k == l && !strncmp(p, id, l)) return true;
        p += k + (p[k] == ',');
    }
    return false;
}

void aos_ha_set_fav(const char *id, bool on)
{
    favs_load();
    if (on == aos_ha_is_fav(id)) return;
    if (on) {
        if (strlen(s_favs) + strlen(id) + 2 >= sizeof s_favs) return;
        if (s_favs[0]) strcat(s_favs, ",");
        strcat(s_favs, id);
    } else {
        char out[sizeof s_favs] = "";
        size_t l = strlen(id);
        for (const char *p = s_favs; *p; ) {
            size_t k = strcspn(p, ",");
            if (!(k == l && !strncmp(p, id, l))) {
                if (out[0]) strcat(out, ",");
                strncat(out, p, k);
            }
            p += k + (p[k] == ',');
        }
        snprintf(s_favs, sizeof s_favs, "%s", out);
    }
    aos_hal_pref_set_str("ha_favs", s_favs);
    aos_hal_mutex_lock(H.mx);
    bump();
    aos_hal_mutex_unlock(H.mx);
}

int aos_ha_favs(char ids[][64], int max)
{
    favs_load();
    int n = 0;
    for (const char *p = s_favs; *p && n < max; ) {
        size_t k = strcspn(p, ",");
        snprintf(ids[n++], 64, "%.*s", (int)k, p);
        p += k + (p[k] == ',');
    }
    return n;
}

/* -------------------------------------------------------------------------- */
/* The table                                                                   */
/* -------------------------------------------------------------------------- */

void aos_ha_lock(void) { aos_hal_mutex_lock(H.mx); }
void aos_ha_unlock(void) { aos_hal_mutex_unlock(H.mx); }
int aos_ha_count(void) { return H.n; }
const aos_ha_entity_t *aos_ha_at(int i) { return i >= 0 && i < H.n ? &H.ent[i] : NULL; }
int aos_ha_area_count(void) { return H.na; }
const char *aos_ha_area_name(int i) { return i >= 0 && i < H.na ? H.area_name[i] : ""; }
aos_ha_state_t aos_ha_state(void) { return H.state; }
const char *aos_ha_error(void) { return H.err; }
const char *aos_ha_location(void) { return H.location[0] ? H.location : "Home Assistant"; }
uint32_t aos_ha_version(void) { return H.version; }

int aos_ha_find(const char *id)
{
    for (int i = 0; i < H.n; i++)
        if (!strcmp(H.ent[i].id, id)) return i;
    return -1;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

/* A state object from HA into an entity (the entity's id and area are set). */
static void apply_state(aos_ha_entity_t *e, const cJSON *st)
{
    const char *s = jstr(st, "state");
    snprintf(e->state, sizeof e->state, "%s", s ? s : "unknown");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(st, "attributes");
    const char *fn = jstr(a, "friendly_name");
    snprintf(e->name, sizeof e->name, "%s", fn ? fn : strchr(e->id, '.') + 1);
    const char *u = jstr(a, "unit_of_measurement");
    snprintf(e->unit, sizeof e->unit, "%s", u ? u : "");
    const char *dc = jstr(a, "device_class");
    snprintf(e->device_class, sizeof e->device_class, "%s", dc ? dc : "");
    const char *ic = jstr(a, "icon");
    snprintf(e->icon, sizeof e->icon, "%s", ic ? ic : "");
    e->brightness = -1;
    e->position = -1;
    switch (e->domain) {
    case HA_LIGHT: {
        bool dim = false;
        const cJSON *m, *modes = cJSON_GetObjectItemCaseSensitive(a, "supported_color_modes");
        cJSON_ArrayForEach(m, modes)
            if (cJSON_IsString(m) && strcmp(m->valuestring, "onoff")) dim = true;
        if (dim) e->brightness = (int16_t)jnum(a, "brightness", 0);
        break;
    }
    case HA_COVER: e->position = (int16_t)jnum(a, "current_position", -1); break;
    case HA_FAN: e->position = (int16_t)jnum(a, "percentage", -1); break;
    case HA_CLIMATE: {
        e->current_temp = (float)jnum(a, "current_temperature", NAN);
        e->target_temp = (float)jnum(a, "temperature", NAN);
        e->temp_min = (float)jnum(a, "min_temp", 7);
        e->temp_max = (float)jnum(a, "max_temp", 35);
        e->temp_step = (float)jnum(a, "target_temp_step", 0.5);
        const char *ac = jstr(a, "hvac_action");
        snprintf(e->hvac_action, sizeof e->hvac_action, "%s", ac ? ac : "");
        e->hvac_modes = 0;
        const cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(a, "hvac_modes"))
            for (int k = 0; k < AOS_HA_HVAC_COUNT; k++)
                if (cJSON_IsString(m) && !strcmp(m->valuestring, aos_ha_hvac_names[k])) e->hvac_modes |= 1u << k;
        break;
    }
    default: break;
    }
    e->pending = false;
    e->changed_ms = (uint32_t)aos_hal_uptime_ms();
}

/* -------------------------------------------------------------------------- */
/* Talking to HA                                                               */
/* -------------------------------------------------------------------------- */

static bool send_json(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    bool ok = s && aos_hal_ws_send_text(H.ws, s, -1) == 0;
    free(s);
    return ok;
}

/* Sends {"id":N,"type":type, ...extra} and waits for the result with that id.
 * Returns the whole parsed message (cJSON_Delete it) or NULL. */
static void on_message(const char *msg, int n);

static cJSON *request(const char *type, cJSON *extra, int timeout_ms)
{
    cJSON *o = extra ? extra : cJSON_CreateObject();
    int id = ++H.next_id;
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "type", type);
    bool sent = send_json(o);
    cJSON_Delete(o);
    if (!sent) return NULL;
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    while ((uint32_t)aos_hal_uptime_ms() - t0 < (uint32_t)timeout_ms) {
        const char *msg;
        int n = aos_hal_ws_recv_text(H.ws, &msg, 500);
        if (n < 0) return NULL;
        if (!n) continue;
        H.last_rx = (uint32_t)aos_hal_uptime_ms();
        cJSON *m = cJSON_ParseWithLength(msg, (size_t)n);
        if (!m) continue;
        if ((int)jnum(m, "id", -1) == id) return m;
        cJSON_Delete(m);
        /* not the answer: an event of an earlier subscription, which must not
         * be lost (the notifications' "current" list arrives right here) */
        on_message(msg, n);
    }
    return NULL;
}

static bool result_ok(const cJSON *m) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "success")); }

typedef struct { char key[64]; char val[40]; int8_t flags; } kv_t;   /* flags: 1 = leave out */

static int kv_cmp(const void *a, const void *b) { return strcmp(((const kv_t *)a)->key, ((const kv_t *)b)->key); }

static const kv_t *kv_find(const kv_t *v, int n, const char *key)
{
    kv_t k;
    snprintf(k.key, sizeof k.key, "%s", key);
    return v ? bsearch(&k, v, n, sizeof *v, kv_cmp) : NULL;
}

static int area_index(const char *area_id)
{
    if (!area_id) return -1;
    for (int i = 0; i < H.na; i++)
        if (!strcmp(H.area_id[i], area_id)) return i;
    return -1;
}

static bool load_everything(void)
{
    set_state(AOS_HA_LOADING, "");
    H.sub_id = H.sub_notif = H.sub_p4os = -1;     /* nothing of the last connection answers any more */
    if (H.hist.state == AOS_HA_HIST_WAITING) H.hist.state = AOS_HA_HIST_QUEUED;
    cJSON *m = request("get_config", NULL, 10000);
    if (m && result_ok(m)) {
        const char *ln = jstr(cJSON_GetObjectItemCaseSensitive(m, "result"), "location_name");
        aos_hal_mutex_lock(H.mx);
        snprintf(H.location, sizeof H.location, "%s", ln ? ln : "");
        aos_hal_mutex_unlock(H.mx);
    }
    cJSON_Delete(m);

    /* areas */
    char aid[AOS_HA_MAX_AREAS][40], aname[AOS_HA_MAX_AREAS][32];
    int na = 0;
    m = request("config/area_registry/list", NULL, 10000);
    if (m && result_ok(m)) {
        const cJSON *a;
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(m, "result")) {
            const char *id = jstr(a, "area_id"), *nm = jstr(a, "name");
            if (!id || !nm || na >= AOS_HA_MAX_AREAS) continue;
            snprintf(aid[na], sizeof aid[na], "%s", id);
            snprintf(aname[na], sizeof aname[na], "%s", nm);
            na++;
        }
    }
    cJSON_Delete(m);

    /* devices -> area */
    kv_t *dev = NULL;
    int nd = 0;
    m = request("config/device_registry/list", NULL, 20000);
    if (m && result_ok(m)) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "result"), *d;
        dev = calloc(cJSON_GetArraySize(r) + 1, sizeof *dev);
        cJSON_ArrayForEach(d, r) {
            const char *id = jstr(d, "id"), *ar = jstr(d, "area_id");
            if (!dev || !id || !ar) continue;
            snprintf(dev[nd].key, sizeof dev[nd].key, "%s", id);
            snprintf(dev[nd].val, sizeof dev[nd].val, "%s", ar);
            nd++;
        }
        if (dev) qsort(dev, nd, sizeof *dev, kv_cmp);
    }
    cJSON_Delete(m);

    /* entities -> area, hidden, category */
    kv_t *reg = NULL;
    int nr = 0;
    m = request("config/entity_registry/list_for_display", NULL, 20000);
    if (m && result_ok(m)) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "result"), "entities"), *e;
        reg = calloc(cJSON_GetArraySize(r) + 1, sizeof *reg);
        cJSON_ArrayForEach(e, r) {
            const char *id = jstr(e, "ei");
            if (!reg || !id || domain_of(id) < 0) continue;
            kv_t *k = &reg[nr++];
            snprintf(k->key, sizeof k->key, "%s", id);
            const char *ar = jstr(e, "ai");
            if (!ar) {
                const kv_t *d = kv_find(dev, nd, jstr(e, "di") ? jstr(e, "di") : "");
                ar = d ? d->val : NULL;
            }
            snprintf(k->val, sizeof k->val, "%s", ar ? ar : "");
            k->flags = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "hb")) ||
                       cJSON_GetObjectItemCaseSensitive(e, "ec") != NULL;
        }
        qsort(reg, nr, sizeof *reg, kv_cmp);
    } else {
        aos_hal_log("ha", "no entity registry listing: showing everything, without areas");
    }
    cJSON_Delete(m);
    free(dev);

    /* states */
    m = request("get_states", NULL, 30000);
    if (!m || !result_ok(m)) {
        cJSON_Delete(m);
        free(reg);
        return false;
    }
    aos_hal_mutex_lock(H.mx);
    memcpy(H.area_id, aid, sizeof aid);
    memcpy(H.area_name, aname, sizeof aname);
    H.na = na;
    H.n = 0;
    const cJSON *st;
    int skipped = 0;
    cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(m, "result")) {
        const char *id = jstr(st, "entity_id");
        int d = id ? domain_of(id) : -1;
        if (d < 0 || strlen(id) >= sizeof H.ent[0].id) continue;
        const kv_t *k = kv_find(reg, nr, id);
        if (k && k->flags) { skipped++; continue; }
        if (H.n >= AOS_HA_MAX_ENTITIES) break;
        aos_ha_entity_t *e = &H.ent[H.n++];
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "%s", id);
        e->domain = (uint8_t)d;
        e->area = (int8_t)(k && k->val[0] ? area_index(k->val) : -1);
        apply_state(e, st);
    }
    bump();
    aos_hal_mutex_unlock(H.mx);
    aos_hal_log("ha", "%d entities in %d areas (%d left out)", H.n, na, skipped);
    cJSON_Delete(m);
    free(reg);

    cJSON *sub = cJSON_CreateObject();
    cJSON_AddStringToObject(sub, "event_type", "state_changed");
    H.sub_id = H.next_id + 1;
    m = request("subscribe_events", sub, 10000);
    bool ok = m && result_ok(m);
    cJSON_Delete(m);
    if (!ok) return false;

    /* HA's own notifications (automations create them), and an event of ours
     * for "tell the bench" from any automation: event p4os_notify with
     * title and message. Neither is essential: an old HA without the first
     * just does not send them. */
    H.sub_notif = H.next_id + 1;
    m = request("persistent_notification/subscribe", NULL, 10000);
    if (!m || !result_ok(m)) H.sub_notif = -1;
    cJSON_Delete(m);
    sub = cJSON_CreateObject();
    cJSON_AddStringToObject(sub, "event_type", "p4os_notify");
    H.sub_p4os = H.next_id + 1;
    m = request("subscribe_events", sub, 10000);
    if (!m || !result_ok(m)) H.sub_p4os = -1;
    cJSON_Delete(m);
    return true;
}

/* ---- notifications ---- */

static uint32_t uid_of(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h | 0x40000000u;         /* never 0, and apart from the phone's small numbers */
}

/* HA's persistent notifications by the uid they have here, so one dismissed
 * on the P4 is dismissed in HA too (and so on every other screen). */
#define PN_KEEP 32
static struct { uint32_t uid; char nid[80]; } s_pn[PN_KEEP];
static int s_pn_next;
static void *s_pn_mx;

static void pn_remember(uint32_t uid, const char *nid)
{
    aos_hal_mutex_lock(s_pn_mx);
    int slot = -1;
    for (int i = 0; i < PN_KEEP; i++) if (s_pn[i].uid == uid) slot = i;
    if (slot < 0) { slot = s_pn_next; s_pn_next = (s_pn_next + 1) % PN_KEEP; }
    s_pn[slot].uid = uid;
    snprintf(s_pn[slot].nid, sizeof s_pn[slot].nid, "%s", nid);
    aos_hal_mutex_unlock(s_pn_mx);
}

static void pn_dismissed(uint32_t uid)
{
    char nid[80] = "";
    aos_hal_mutex_lock(s_pn_mx);
    for (int i = 0; i < PN_KEEP; i++)
        if (s_pn[i].uid == uid) { snprintf(nid, sizeof nid, "%s", s_pn[i].nid); s_pn[i].uid = 0; }
    aos_hal_mutex_unlock(s_pn_mx);
    if (!nid[0]) return;            /* not one of HA's persistent ones (a p4os_notify event) */
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "notification_id", nid);
    char *js = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    if (js && !aos_ha_call("persistent_notification", "dismiss", "", js))
        aos_hal_log("ha", "could not dismiss %s in HA (not connected)", nid);
    free(js);
}

static void notify(const char *key, const char *title, const char *msg, bool pre_existing)
{
    aos_notif_t n = { 0 };
    n.uid = uid_of(key);
    n.category = AOS_NOTIF_OTHER;
    snprintf(n.app, sizeof n.app, "Home Assistant");
    snprintf(n.title, sizeof n.title, "%s", title && title[0] ? title : aos_ha_location());
    /* HA's messages are Markdown: the two marks that show most, gone */
    char clean[sizeof n.message];
    size_t k = 0;
    for (const char *p = msg ? msg : ""; *p && k + 1 < sizeof clean; p++)
        if (*p != '*' && *p != '`') clean[k++] = *p;
    clean[k] = 0;
    snprintf(n.message, sizeof n.message, "%s", clean);
    n.when = time(NULL);
    n.pre_existing = pre_existing;
    aos_notif_push(&n);
}

static void on_persistent(const cJSON *ev)
{
    const char *kind = jstr(ev, "type");
    const cJSON *all = cJSON_GetObjectItemCaseSensitive(ev, "notifications"), *it;
    if (!kind) return;
    cJSON_ArrayForEach(it, all) {
        const char *nid = jstr(it, "notification_id");
        if (!nid) nid = it->string ? it->string : "";
        char key[96];
        snprintf(key, sizeof key, "pn:%s", nid);
        if (!strcmp(kind, "removed")) aos_notif_push_removed(uid_of(key));
        else {
            pn_remember(uid_of(key), nid);
            notify(key, jstr(it, "title"), jstr(it, "message"), !strcmp(kind, "current"));
        }
    }
}

/* ---- history ---- */

static void history_send(void)
{
    char start[40];
    time_t now = time(NULL), from = now - (time_t)H.hist.hours * 3600;
    struct tm g;
    gmtime_r(&from, &g);
    strftime(start, sizeof start, "%Y-%m-%dT%H:%M:%S+00:00", &g);
    cJSON *o = cJSON_CreateObject();
    int id = ++H.next_id;
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "type", "history/history_during_period");
    cJSON_AddStringToObject(o, "start_time", start);
    cJSON *ids = cJSON_AddArrayToObject(o, "entity_ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(H.hist.id));
    cJSON_AddTrueToObject(o, "minimal_response");
    cJSON_AddTrueToObject(o, "no_attributes");
    cJSON_AddFalseToObject(o, "significant_changes_only");
    aos_hal_mutex_lock(H.mx);
    H.hist.req_id = id;
    H.hist.state = AOS_HA_HIST_WAITING;
    H.hist.t0 = from;
    H.hist.t1 = now;
    aos_hal_mutex_unlock(H.mx);
    if (!send_json(o)) H.hist.state = AOS_HA_HIST_FAILED;
    cJSON_Delete(o);
}

/* The answer: {"<entity>": [{"s": "23.4", "lu": 1727...}, ...]}, a point per
 * change. Numbers only, averaged into at most AOS_HA_HIST_MAX buckets. */
static void history_parse(const cJSON *result)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(result, H.hist.id), *p;
    static float sum[AOS_HA_HIST_MAX];
    static uint16_t cnt[AOS_HA_HIST_MAX];
    memset(sum, 0, sizeof sum);
    memset(cnt, 0, sizeof cnt);
    double span = (double)(H.hist.t1 - H.hist.t0);
    if (span < 60) span = 60;
    float last = NAN;
    int first_bucket = -1;
    cJSON_ArrayForEach(p, list) {
        const char *sv = jstr(p, "s");
        double lu = jnum(p, "lu", jnum(p, "lc", NAN));
        if (!sv || isnan(lu)) continue;
        char *end;
        double v = strtod(sv, &end);
        if (end == sv || *end) continue;               /* unavailable, unknown */
        int b = (int)((lu - (double)H.hist.t0) / span * AOS_HA_HIST_MAX);
        if (b < 0) b = 0;                              /* the state at the start of the period */
        if (b >= AOS_HA_HIST_MAX) b = AOS_HA_HIST_MAX - 1;
        sum[b] += (float)v;
        cnt[b]++;
        if (first_bucket < 0) first_bucket = b;
        last = (float)v;
    }
    aos_hal_mutex_lock(H.mx);
    H.hist.n = 0;
    H.hist.lo = INFINITY;
    H.hist.hi = -INFINITY;
    /* a sensor that changes rarely leaves empty buckets: hold the last value */
    float hold = NAN;
    for (int b = 0; b < AOS_HA_HIST_MAX; b++) {
        if (cnt[b]) hold = sum[b] / cnt[b];
        if (isnan(hold)) continue;
        H.hist.v[H.hist.n] = hold;
        H.hist.t[H.hist.n] = (uint32_t)(span * b / AOS_HA_HIST_MAX);
        if (hold < H.hist.lo) H.hist.lo = hold;
        if (hold > H.hist.hi) H.hist.hi = hold;
        H.hist.n++;
    }
    (void)last;
    H.hist.state = H.hist.n ? AOS_HA_HIST_READY : AOS_HA_HIST_EMPTY;
    bump();
    aos_hal_mutex_unlock(H.mx);
}

bool aos_ha_history_request(const char *entity_id, int hours)
{
    if (H.state != AOS_HA_READY || !entity_id || !aos_hal_time_is_valid()) return false;
    aos_hal_mutex_lock(H.mx);
    snprintf(H.hist.id, sizeof H.hist.id, "%s", entity_id);
    H.hist.hours = hours < 1 ? 1 : hours > 24 * 14 ? 24 * 14 : hours;
    H.hist.n = 0;
    H.hist.state = AOS_HA_HIST_QUEUED;
    bump();
    aos_hal_mutex_unlock(H.mx);
    return true;
}

int aos_ha_history(const char *entity_id, float *v, uint32_t *t, int max, float *lo, float *hi, uint32_t *span_s)
{
    aos_hal_mutex_lock(H.mx);
    int st = strcmp(H.hist.id, entity_id ? entity_id : "") ? AOS_HA_HIST_NONE : H.hist.state;
    int n = 0;
    if (st == AOS_HA_HIST_READY) {
        /* thin to what the caller can draw */
        n = H.hist.n < max ? H.hist.n : max;
        for (int i = 0; i < n; i++) {
            int k = (int)((int64_t)i * H.hist.n / n);
            if (v) v[i] = H.hist.v[k];
            if (t) t[i] = H.hist.t[k];
        }
        if (lo) *lo = H.hist.lo;
        if (hi) *hi = H.hist.hi;
        if (span_s) *span_s = (uint32_t)(H.hist.t1 - H.hist.t0);
    }
    aos_hal_mutex_unlock(H.mx);
    return st == AOS_HA_HIST_READY ? n : -st;
}

static void on_message(const char *msg, int n)
{
    cJSON *m = cJSON_ParseWithLength(msg, (size_t)n);
    if (!m) return;
    const char *type = jstr(m, "type");
    if (type && !strcmp(type, "event") && (int)jnum(m, "id", -1) == H.sub_id) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "event"), "data");
        const char *id = jstr(data, "entity_id");
        const cJSON *ns = cJSON_GetObjectItemCaseSensitive(data, "new_state");
        if (id) {
            aos_hal_mutex_lock(H.mx);
            int i = aos_ha_find(id);
            if (i >= 0) {
                if (cJSON_IsObject(ns)) apply_state(&H.ent[i], ns);
                else snprintf(H.ent[i].state, sizeof H.ent[i].state, "unavailable");
                bump();
            }
            aos_hal_mutex_unlock(H.mx);
        }
    } else if (type && !strcmp(type, "event") && H.sub_notif > 0 && (int)jnum(m, "id", -1) == H.sub_notif) {
        on_persistent(cJSON_GetObjectItemCaseSensitive(m, "event"));
    } else if (type && !strcmp(type, "event") && H.sub_p4os > 0 && (int)jnum(m, "id", -1) == H.sub_p4os) {
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(m, "event");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(ev, "data");
        char key[48];
        snprintf(key, sizeof key, "ev:%lu", (unsigned long)aos_hal_uptime_ms());
        notify(key, jstr(data, "title"), jstr(data, "message"), false);
    } else if (type && !strcmp(type, "result") && (int)jnum(m, "id", -1) == H.hist.req_id && H.hist.state == AOS_HA_HIST_WAITING) {
        if (result_ok(m)) history_parse(cJSON_GetObjectItemCaseSensitive(m, "result"));
        else { aos_hal_mutex_lock(H.mx); H.hist.state = AOS_HA_HIST_FAILED; bump(); aos_hal_mutex_unlock(H.mx); }
    } else if (type && !strcmp(type, "result") && !result_ok(m)) {
        const char *em = jstr(cJSON_GetObjectItemCaseSensitive(m, "error"), "message");
        aos_hal_log("ha", "call %d refused: %s", (int)jnum(m, "id", -1), em ? em : "?");
    }
    cJSON_Delete(m);
}

static void send_queued(void)
{
    for (;;) {
        call_t c;
        aos_hal_mutex_lock(H.mx);
        bool any = H.qh != H.qt;
        if (any) { c = H.q[H.qt]; H.qt = (H.qt + 1) % Q_LEN; }
        aos_hal_mutex_unlock(H.mx);
        if (!any) return;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", ++H.next_id);
        cJSON_AddStringToObject(o, "type", "call_service");
        cJSON_AddStringToObject(o, "domain", c.domain);
        cJSON_AddStringToObject(o, "service", c.service);
        if (c.id[0]) {                  /* a call without an entity (dismissing a notification) has no target */
            cJSON *tg = cJSON_AddObjectToObject(o, "target");
            cJSON_AddStringToObject(tg, "entity_id", c.id);
        }
        cJSON *sd = c.data[0] ? cJSON_Parse(c.data) : NULL;
        if (cJSON_IsObject(sd)) cJSON_AddItemToObject(o, "service_data", sd);
        else cJSON_Delete(sd);
        send_json(o);
        cJSON_Delete(o);
    }
}

/* ws://host:port/api/websocket from http://host:port, and wss:// from
 * https:// (Nabu Casa, or a reverse proxy with a real certificate) */
static bool ws_url(char *out, size_t n)
{
    if (!strncmp(H.url, "https://", 8)) {
        snprintf(out, n, "wss://%.120s/api/websocket", H.url + 8);
    } else if (!strncmp(H.url, "http://", 7)) {
        snprintf(out, n, "ws://%.120s/api/websocket", H.url + 7);
    } else {
        set_state(AOS_HA_ERROR, _("La dirección tiene que empezar con http:// o https://"));
        return false;
    }
    return true;
}

static void service_thread(void *arg)
{
    (void)arg;
    int backoff = 0;
    uint32_t last_import = 0;
    char token_refused[sizeof H.token] = "";
    aos_ha_import_file();
    for (;;) {
        H.kick = false;
        if (!aos_ha_configured()) {
            set_state(AOS_HA_UNCONFIGURED, "");
            if ((uint32_t)aos_hal_uptime_ms() - last_import > 5000) {
                last_import = (uint32_t)aos_hal_uptime_ms();
                aos_ha_import_file();
            }
            aos_hal_sleep_ms(500);
            continue;
        }
        if (!strcmp(H.token, token_refused)) { aos_hal_sleep_ms(500); continue; }
        if (aos_hal_net_state() != AOS_NET_CONNECTED) {
            set_state(AOS_HA_WAITING_NET, "");
            aos_hal_sleep_ms(1000);
            continue;
        }
        char url[160];
        if (!ws_url(url, sizeof url)) { while (!H.kick) aos_hal_sleep_ms(500); continue; }
        set_state(AOS_HA_CONNECTING, "");
        H.ws = aos_hal_ws_open(url, NULL, 8000);
        H.next_id = 0;
        bool ready = false;
        if (!H.ws) {
            char e[112];
            const char *why = aos_hal_ws_last_error();
            /* the transport's reasons are log lines, in English: the usual
             * ones said to the user */
            if (strstr(why, "not signed by a trusted")) snprintf(e, sizeof e, "%s", _("El certificado no es de una autoridad confiable"));
            else if (strstr(why, "is not for")) snprintf(e, sizeof e, "%s", _("El certificado es de otra dirección"));
            else if (strstr(why, "expired")) snprintf(e, sizeof e, "%s", _("El certificado venció"));
            else if (strstr(why, "clock") || strstr(why, "valid time")) snprintf(e, sizeof e, "%s", _("Falta la hora para verificar el certificado"));
            else if (strstr(why, "cannot resolve")) snprintf(e, sizeof e, _("No encuentro %.80s en la red"), H.url);
            else if (strstr(why, "refused the WebSocket")) snprintf(e, sizeof e, "%s", _("Eso no parece Home Assistant"));
            else if (why[0] && !strstr(why, "cannot connect")) snprintf(e, sizeof e, "%.110s", why);
            else snprintf(e, sizeof e, _("No contesta %.96s"), H.url);
            set_state(AOS_HA_ERROR, e);
        } else {
            /* auth_required, auth, auth_ok */
            const char *msg;
            int n = aos_hal_ws_recv_text(H.ws, &msg, 8000);
            cJSON *m = n > 0 ? cJSON_ParseWithLength(msg, (size_t)n) : NULL;
            bool asked = m && jstr(m, "type") && !strcmp(jstr(m, "type"), "auth_required");
            cJSON_Delete(m);
            if (asked) {
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "type", "auth");
                cJSON_AddStringToObject(a, "access_token", H.token);
                send_json(a);
                cJSON_Delete(a);
                n = aos_hal_ws_recv_text(H.ws, &msg, 8000);
                m = n > 0 ? cJSON_ParseWithLength(msg, (size_t)n) : NULL;
                const char *t = m ? jstr(m, "type") : NULL;
                if (t && !strcmp(t, "auth_ok")) {
                    ready = load_everything();
                    if (!ready) set_state(AOS_HA_ERROR, _("Se cortó mientras cargaba los estados"));
                } else if (t && !strcmp(t, "auth_invalid")) {
                    snprintf(token_refused, sizeof token_refused, "%s", H.token);
                    set_state(AOS_HA_AUTH_FAILED, _("Home Assistant rechazó el token"));
                } else {
                    set_state(AOS_HA_ERROR, _("No terminó la autenticación"));
                }
                cJSON_Delete(m);
            } else {
                set_state(AOS_HA_ERROR, _("Eso no parece Home Assistant"));
            }
        }
        if (ready) {
            backoff = 0;
            set_state(AOS_HA_READY, "");
            H.last_rx = H.last_ping = (uint32_t)aos_hal_uptime_ms();
            while (!H.kick && aos_hal_ws_is_open(H.ws) && aos_hal_net_state() == AOS_NET_CONNECTED) {
                const char *msg;
                int n = aos_hal_ws_recv_text(H.ws, &msg, 100);
                uint32_t now = (uint32_t)aos_hal_uptime_ms();
                if (n > 0) { H.last_rx = now; on_message(msg, n); }
                if (n < 0) break;
                send_queued();
                if (H.hist.state == AOS_HA_HIST_QUEUED) history_send();
                if (now - H.last_ping > PING_EVERY_MS) {
                    H.last_ping = now;
                    char p[40];
                    snprintf(p, sizeof p, "{\"id\":%d,\"type\":\"ping\"}", ++H.next_id);
                    aos_hal_ws_send_text(H.ws, p, -1);
                }
                if (now - H.last_rx > DEAD_AFTER_MS) break;
            }
            if (!H.kick) set_state(AOS_HA_ERROR, _("Se perdió la conexión"));
        }
        aos_hal_ws_close(H.ws);
        H.ws = NULL;
        if (H.kick) { token_refused[0] = 0; continue; }
        static const int PAUSE_S[] = { 2, 5, 10, 20, 30 };
        int s = PAUSE_S[backoff < 4 ? backoff++ : 4];
        for (int i = 0; i < s * 10 && !H.kick; i++) aos_hal_sleep_ms(100);
    }
}

void aos_ha_start(void)
{
    if (H.started) return;
    H.started = true;
    H.mx = aos_hal_mutex_create();
    s_pn_mx = aos_hal_mutex_create();
    aos_notif_set_dismiss_hook(pn_dismissed);
    H.ent = calloc(AOS_HA_MAX_ENTITIES, sizeof *H.ent);
    cfg_load();
    favs_load();
    if (!H.ent || !aos_hal_thread_start("ha", service_thread, NULL, 12288, 4)   /* a TLS handshake verifies the chain here: ~4.4 KB */) {
        H.state = AOS_HA_ERROR;
        snprintf(H.err, sizeof H.err, "%s", _("No se pudo arrancar el servicio"));
    }
}

/* -------------------------------------------------------------------------- */
/* Calls                                                                       */
/* -------------------------------------------------------------------------- */

/* The state a call will most likely leave, shown at once. */
static void optimistic(aos_ha_entity_t *e, const char *service)
{
    bool on = !strcmp(e->state, "on");
    const char *ns = NULL;
    if (!strcmp(service, "toggle")) ns = on ? "off" : "on";
    else if (!strcmp(service, "turn_on") && e->domain <= HA_FAN) ns = "on";
    else if (!strcmp(service, "turn_off") && e->domain <= HA_FAN) ns = "off";
    else if (!strcmp(service, "open_cover")) ns = "opening";
    else if (!strcmp(service, "close_cover")) ns = "closing";
    else if (!strcmp(service, "lock")) ns = "locking";
    else if (!strcmp(service, "unlock")) ns = "unlocking";
    if (ns && e->domain != HA_CLIMATE) snprintf(e->state, sizeof e->state, "%s", ns);
    if (e->domain == HA_LIGHT && e->brightness >= 0 && ns) e->brightness = !strcmp(ns, "on") ? (e->brightness ? e->brightness : 255) : 0;
    e->pending = true;
    e->changed_ms = (uint32_t)aos_hal_uptime_ms();
}

bool aos_ha_call(const char *domain, const char *service, const char *entity_id, const char *data_json)
{
    if (H.state != AOS_HA_READY || !domain || !service || !entity_id) return false;
    aos_hal_mutex_lock(H.mx);
    int next = (H.qh + 1) % Q_LEN;
    bool ok = next != H.qt;
    if (ok) {
        call_t *c = &H.q[H.qh];
        snprintf(c->domain, sizeof c->domain, "%s", domain);
        snprintf(c->service, sizeof c->service, "%s", service);
        snprintf(c->id, sizeof c->id, "%s", entity_id);
        snprintf(c->data, sizeof c->data, "%s", data_json ? data_json : "");
        H.qh = next;
        int i = aos_ha_find(entity_id);
        if (i >= 0) optimistic(&H.ent[i], service);
        bump();
    }
    aos_hal_mutex_unlock(H.mx);
    return ok;
}

bool aos_ha_tap(const char *entity_id)
{
    aos_hal_mutex_lock(H.mx);
    int i = aos_ha_find(entity_id);
    aos_ha_entity_t e = i >= 0 ? H.ent[i] : (aos_ha_entity_t){ 0 };
    aos_hal_mutex_unlock(H.mx);
    if (i < 0 || !strcmp(e.state, "unavailable")) return false;
    const char *dom = DOMAIN_NAME[e.domain], *svc = NULL;
    switch (e.domain) {
    case HA_LIGHT: case HA_SWITCH: case HA_INPUT_BOOLEAN: case HA_FAN: svc = "toggle"; break;
    case HA_COVER:
        svc = !strcmp(e.state, "opening") || !strcmp(e.state, "closing") ? "stop_cover"
            : !strcmp(e.state, "closed") ? "open_cover" : "close_cover";
        break;
    case HA_LOCK: svc = !strcmp(e.state, "locked") ? "unlock" : "lock"; break;
    case HA_MEDIA: svc = "media_play_pause"; break;
    case HA_SCENE: svc = "turn_on"; break;
    case HA_SCRIPT: svc = !strcmp(e.state, "on") ? "turn_off" : "turn_on"; break;
    case HA_BUTTON: svc = "press"; break;
    case HA_CLIMATE: svc = !strcmp(e.state, "off") ? "turn_on" : "turn_off"; break;
    default: return false;
    }
    return aos_ha_call(dom, svc, entity_id, NULL);
}
