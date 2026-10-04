/*
 * P4OS - /api/radio: the nine keys of the Radio app and what plays (from
 * AmoledOS' aos_radio_api.c). The page is the Radio app's own, on the card
 * (apps/radio/web/radio.js, docs/PORTAL-PAGES.md).
 *
 *   GET  /api/radio                     the nine keys, the switches, what plays
 *   POST /api/radio {"do": "set", "k": N, "name": ..., "url": ...}   a station on key N (0..8)
 *                   {"do": "clear", "k": N}                          empty key N
 *                   {"do": "swap", "a": N, "b": M}                   exchange two keys (and their logos)
 *                   {"do": "play", "k": N}                           play key N on the board
 *                   {"do": "test", "name": ..., "url": ...}          play an address on no key
 *                   {"do": "pause"|"resume"|"stop"|"next"|"prev"}
 *                   {"do": "vol", "v": 0..100}
 *                   {"do": "art", "on": true|false}                  look covers up, or not
 *
 * The keys are preferences rad0..rad8 = "name \x1F url"; the Radio app reads
 * them and rebuilds its keys when rad_gen moves. What is bigger lives on the
 * card and only the page and the app read it: the list of stations
 * (/radio/library.json) and each key's logo (/radio/logoN.jpg, a 160 px JPEG
 * the browser drew). Clearing or swapping keys takes their logos along.
 */
#include "aos_portal_radio.h"
#include "aos_hal.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define KEYS 9
#define SEP  '\x1f'

static void send_cjson(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void reply(aos_httpd_req_t *r, const char *error)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", !error);
    if (error) cJSON_AddStringToObject(o, "error", error);
    send_cjson(r, error ? 400 : 200, o);
}

/* ---- the keys ---- */

static bool key_load(int i, aos_radio_station_t *st)
{
    char key[8], raw[sizeof st->name + sizeof st->url + 4];
    memset(st, 0, sizeof *st);
    snprintf(key, sizeof key, "rad%d", i);
    if (!aos_hal_pref_get_str(key, raw, sizeof raw) || !raw[0]) return false;
    char *sep = strchr(raw, SEP);
    if (!sep) return false;
    *sep = '\0';
    memcpy(st->name, raw, strnlen(raw, sizeof st->name - 1));          /* st is zeroed: ends in 0 */
    memcpy(st->url, sep + 1, strnlen(sep + 1, sizeof st->url - 1));
    return st->url[0] != '\0';
}

static void key_store(int i, const aos_radio_station_t *st)
{
    char key[8], raw[sizeof st->name + sizeof st->url + 4];
    snprintf(key, sizeof key, "rad%d", i);
    if (st && st->url[0]) snprintf(raw, sizeof raw, "%s%c%s", st->name, SEP, st->url);
    else raw[0] = '\0';
    aos_hal_pref_set_str(key, raw);
}

static void bump_gen(void)
{
    int32_t g = 0;
    aos_hal_pref_get_i32("rad_gen", &g);
    aos_hal_pref_set_i32("rad_gen", g + 1);
}

static bool logo_path(int i, char *out, size_t len)
{
    const char *root = aos_hal_path_sd_root();
    return root && snprintf(out, len, "%s/radio/logo%d.jpg", root, i) < (int)len;
}

static bool has_logo(int i)
{
    char p[96];
    struct stat st;
    return logo_path(i, p, sizeof p) && stat(p, &st) == 0 && st.st_size > 0;
}

/* Printable, and never the separator. */
static bool text_ok(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) return false;
    return true;
}

static const char *check_station(const char *name, const char *url)
{
    if (!name || !name[0] || strlen(name) >= sizeof(((aos_radio_station_t *)0)->name) || !text_ok(name))
        return "el nombre tiene que tener entre 1 y 47 bytes";
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)))
        return "la dirección tiene que empezar con http:// o https://";
    if (strlen(url) >= sizeof(((aos_radio_station_t *)0)->url) || strlen(url) < 11 || !text_ok(url) || strchr(url, ' '))
        return "la dirección no es válida";
    return NULL;
}

static void play_keys(int k)
{
    aos_radio_station_t *list = calloc(KEYS, sizeof *list);
    if (!list) return;
    for (int i = 0; i < KEYS; i++) key_load(i, &list[i]);
    if (aos_hal_radio_play(list, KEYS, k)) aos_hal_pref_set_i32("rad_last", k);
    free(list);
}

/* ---- GET ---- */

static const char *state_name(aos_radio_state_t s)
{
    switch (s) {
    case AOS_RADIO_CONNECTING: return "connecting";
    case AOS_RADIO_BUFFERING:  return "buffering";
    case AOS_RADIO_PLAYING:    return "playing";
    case AOS_RADIO_RETRYING:   return "retrying";
    case AOS_RADIO_FAILED:     return "failed";
    default:                   return "off";
    }
}

static void api_get(aos_httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *keys = cJSON_AddArrayToObject(o, "keys");
    for (int i = 0; i < KEYS; i++) {
        aos_radio_station_t st;
        key_load(i, &st);
        cJSON *k = cJSON_CreateObject();
        cJSON_AddStringToObject(k, "name", st.name);
        cJSON_AddStringToObject(k, "url", st.url);
        cJSON_AddBoolToObject(k, "logo", st.url[0] && has_logo(i));
        cJSON_AddItemToArray(keys, k);
    }
    int32_t gen = 0, art = 1, last = 0;
    aos_hal_pref_get_i32("rad_gen", &gen);
    aos_hal_pref_get_i32("rad_art", &art);
    aos_hal_pref_get_i32("rad_last", &last);
    cJSON_AddNumberToObject(o, "gen", gen);
    cJSON_AddBoolToObject(o, "art", art != 0);
    cJSON_AddNumberToObject(o, "last", last);
    cJSON_AddNumberToObject(o, "volume", aos_hal_volume_get());
    cJSON_AddStringToObject(o, "device", aos_hal_device_name());

    aos_player_info_t *pi = calloc(1, sizeof *pi);
    aos_radio_status_t *rs = calloc(1, sizeof *rs);
    if (pi && aos_hal_player_info(pi)) {
        cJSON *p = cJSON_AddObjectToObject(o, "player");
        cJSON_AddStringToObject(p, "state", pi->state == AOS_PLAYER_PLAYING ? "playing"
                                          : pi->state == AOS_PLAYER_PAUSED ? "paused" : "stopped");
        cJSON_AddBoolToObject(p, "live", pi->live);
        cJSON_AddStringToObject(p, "title", pi->title);
        cJSON_AddStringToObject(p, "artist", pi->artist);
    }
    if (rs && aos_hal_radio_status(rs)) {
        cJSON *d = cJSON_AddObjectToObject(o, "radio");
        cJSON_AddStringToObject(d, "state", state_name(rs->state));
        cJSON_AddNumberToObject(d, "index", rs->index);
        cJSON_AddStringToObject(d, "station", rs->station);
        cJSON_AddStringToObject(d, "url", rs->url);
        cJSON_AddStringToObject(d, "title", rs->title);
        cJSON_AddStringToObject(d, "host", rs->host);
        cJSON_AddStringToObject(d, "icy_name", rs->icy_name);
        cJSON_AddStringToObject(d, "icy_genre", rs->icy_genre);
        cJSON_AddStringToObject(d, "error", rs->error);
        cJSON_AddStringToObject(d, "codec", rs->codec);
        cJSON_AddBoolToObject(d, "hls", rs->hls);
        cJSON_AddBoolToObject(d, "tls", rs->tls);
        cJSON_AddNumberToObject(d, "kbps", rs->kbps);
        cJSON_AddNumberToObject(d, "rate", rs->sample_rate);
        cJSON_AddNumberToObject(d, "buffer_ms", rs->buffer_ms);
        cJSON_AddNumberToObject(d, "reconnects", rs->reconnects);
        cJSON_AddNumberToObject(d, "listening_s", rs->listening_s);
    }
    free(pi);
    free(rs);
    send_cjson(r, 200, o);
}

/* ---- POST ---- */

static int jint(cJSON *b, const char *k, int def)
{
    cJSON *v = cJSON_GetObjectItem(b, k);
    return cJSON_IsNumber(v) ? v->valueint : cJSON_IsString(v) ? atoi(v->valuestring) : def;
}

static const char *jstr(cJSON *b, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(b, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void api_post(aos_httpd_req_t *r)
{
    char *raw = aos_httpd_body_all(r, 2048);
    cJSON *b = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    const char *what = b ? jstr(b, "do") : NULL;
    if (!what) {
        cJSON_Delete(b);
        reply(r, "falta \"do\"");
        return;
    }
    int k = jint(b, "k", -1);
    bool key_ok = k >= 0 && k < KEYS;
    const char *err = NULL;

    if (!strcmp(what, "set") || !strcmp(what, "test")) {
        const char *name = jstr(b, "name"), *url = jstr(b, "url");
        err = check_station(name, url);
        if (!err) {
            aos_radio_station_t st;
            memset(&st, 0, sizeof st);
            snprintf(st.name, sizeof st.name, "%s", name);
            snprintf(st.url, sizeof st.url, "%s", url);
            if (what[0] == 't') {
                aos_hal_radio_play(&st, 1, 0);
            } else if (!key_ok) {
                err = "no existe esa tecla";
            } else {
                key_store(k, &st);
                bump_gen();
                aos_hal_log("radio", "key %d: %s -> %s", k + 1, st.name, st.url);
            }
        }
    } else if (!strcmp(what, "clear")) {
        if (!key_ok) {
            err = "no existe esa tecla";
        } else {
            key_store(k, NULL);
            char p[96];
            if (logo_path(k, p, sizeof p)) remove(p);
            bump_gen();
        }
    } else if (!strcmp(what, "swap")) {
        int a = jint(b, "a", -1), c = jint(b, "b", -1);
        if (a < 0 || a >= KEYS || c < 0 || c >= KEYS || a == c) {
            err = "teclas inválidas";
        } else {
            aos_radio_station_t sa, sb;
            key_load(a, &sa);
            key_load(c, &sb);
            key_store(a, &sb);
            key_store(c, &sa);
            char pa[96], pb[96], pt[100];
            if (logo_path(a, pa, sizeof pa) && logo_path(c, pb, sizeof pb)) {
                snprintf(pt, sizeof pt, "%s.tmp", pa);
                remove(pt);
                rename(pa, pt);         /* any of the three may be missing: fine */
                rename(pb, pa);
                rename(pt, pb);
            }
            bump_gen();
        }
    } else if (!strcmp(what, "play")) {
        aos_radio_station_t st;
        if (!key_ok || !key_load(k, &st)) err = "esa tecla está vacía";
        else play_keys(k);
    } else if (!strcmp(what, "vol")) {
        int v = jint(b, "v", -1);
        if (v < 0) err = "falta \"v\"";
        else aos_hal_volume_set(v > 100 ? 100 : v);
    } else if (!strcmp(what, "art")) {
        aos_hal_pref_set_i32("rad_art", cJSON_IsTrue(cJSON_GetObjectItem(b, "on")) ? 1 : 0);
    } else if (!strcmp(what, "pause")) {
        aos_hal_player_pause();
    } else if (!strcmp(what, "resume")) {
        aos_hal_player_resume();
    } else if (!strcmp(what, "stop")) {
        aos_hal_player_stop();
    } else if (!strcmp(what, "next")) {
        aos_hal_player_next();
    } else if (!strcmp(what, "prev")) {
        aos_hal_player_prev();
    } else {
        err = "acción desconocida";
    }
    cJSON_Delete(b);
    reply(r, err);
}

bool aos_portal_radio(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strcmp(p, "radio")) return false;
    if (!strcmp(m, "GET")) api_get(r);
    else if (!strcmp(m, "POST")) api_post(r);
    else return false;
    return true;
}
