/*
 * VUELOS - the settings, in <data>/vuelos.txt: one key=value a line, lines
 * starting with # are notes. The app writes it when something is changed on
 * the board, and the portal page (web/vuelos.js) writes the same file; the
 * app looks at its date every two seconds and takes it again if it changed,
 * as Clima does with its place.
 *
 *   place=<name><TAB><lat><TAB><lon>      degrees, with a point
 *   src=airplanes | adsblol | opensky | local
 *   radius=<km>                           around the place, 10 to 400
 *   url=<http://.../data/aircraft.json>   the receiver of one's own
 *   lol_key=, lol_day=<requests a day>    adsb.lol
 *   os_id=, os_secret=                    OpenSky's API client, optional
 *   alert_over=0|1, over_km=, over_ft=    an aircraft passing overhead
 *   alert_emerg=0|1, alert_mil=0|1, sound=0|1
 *   trail=<minutes>, units=aviation|metric, routes=0|1, labels=0|1
 *
 * The keys are the person's own, typed in the portal; they live on the
 * card and nowhere else.
 */
#include "vuelos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const SRC_KEY[SRC_N] = { "airplanes", "adsblol", "opensky", "local" };

const char *vl_src_name(int src)
{
    static const char *const N[SRC_N] = { "airplanes.live", "adsb.lol", "OpenSky Network", "receptor" };
    return src >= 0 && src < SRC_N ? N[src] : "?";
}

static void conf_path(char *out, size_t n)
{
    snprintf(out, n, "%s/vuelos.txt", aos_hal_path_data());
}

static long conf_mtime(void)
{
    char path[160];
    conf_path(path, sizeof path);
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_mtime : -1;
}

void vl_conf_defaults(vl_conf_t *c)
{
    memset(c, 0, sizeof *c);
    c->src = SRC_AIRPLANES;
    c->radius_km = 93;              /* 50 NM */
    c->lol_day = 500;
    c->alert_over = true;
    c->over_km = 3.0f;
    c->over_ft = 12000;
    c->alert_emerg = true;
    c->alert_mil = false;
    c->alert_sound = true;
    c->trail_min = 3;
    c->units = UNITS_AVIATION;
    c->routes = true;
    c->labels = true;
    /* the first place: Clima's city, else the centre of Buenos Aires, as
     * Mapas starts */
    int32_t la, lo;
    if (aos_hal_pref_get_i32("clima_lat", &la) && aos_hal_pref_get_i32("clima_lon", &lo)) {
        c->lat = (float)la / 1e4f;
        c->lon = (float)lo / 1e4f;
        aos_hal_pref_get_str("clima_city", c->name, sizeof c->name);
    } else {
        c->lat = -34.6037f;
        c->lon = -58.3816f;
        snprintf(c->name, sizeof c->name, "Buenos Aires");
    }
}

static void copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && src[i] != '\r' && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

static bool on(const char *v)
{
    return v[0] == '1' || v[0] == 'y' || v[0] == 's' || v[0] == 't';
}

static void conf_line(vl_conf_t *c, char *line)
{
    char *eq = strchr(line, '=');
    if (line[0] == '#' || !eq) return;
    *eq = 0;
    const char *k = line, *v = eq + 1;
    if (!strcmp(k, "place")) {
        char *t1 = strchr(v, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2) return;
        float lat = strtof(t1 + 1, NULL), lon = strtof(t2 + 1, NULL);
        if (lat < -85 || lat > 85 || lon < -180 || lon > 180) return;
        *t1 = 0;
        copy(c->name, sizeof c->name, v);
        c->lat = lat;
        c->lon = lon;
        c->have_place = true;
    } else if (!strcmp(k, "src")) {
        for (int i = 0; i < SRC_N; i++)
            if (!strcmp(v, SRC_KEY[i])) c->src = i;
    } else if (!strcmp(k, "radius")) {
        int r = atoi(v);
        c->radius_km = r < 10 ? 10 : r > 400 ? 400 : r;
    } else if (!strcmp(k, "url")) {
        copy(c->url, sizeof c->url, v);
    } else if (!strcmp(k, "lol_key")) {
        copy(c->lol_key, sizeof c->lol_key, v);
    } else if (!strcmp(k, "lol_day")) {
        int d = atoi(v);
        c->lol_day = d < 50 ? 50 : d > 400000 ? 400000 : d;
    } else if (!strcmp(k, "os_id")) {
        copy(c->os_id, sizeof c->os_id, v);
    } else if (!strcmp(k, "os_secret")) {
        copy(c->os_secret, sizeof c->os_secret, v);
    } else if (!strcmp(k, "alert_over")) {
        c->alert_over = on(v);
    } else if (!strcmp(k, "over_km")) {
        float f = strtof(v, NULL);
        c->over_km = f < 0.5f ? 0.5f : f > 20 ? 20 : f;
    } else if (!strcmp(k, "over_ft")) {
        int f = atoi(v);
        c->over_ft = f < 1000 ? 1000 : f > 60000 ? 60000 : f;
    } else if (!strcmp(k, "alert_emerg")) {
        c->alert_emerg = on(v);
    } else if (!strcmp(k, "alert_mil")) {
        c->alert_mil = on(v);
    } else if (!strcmp(k, "sound")) {
        c->alert_sound = on(v);
    } else if (!strcmp(k, "trail")) {
        int m = atoi(v);
        c->trail_min = m < 0 ? 0 : m > 15 ? 15 : m;
    } else if (!strcmp(k, "units")) {
        c->units = !strcmp(v, "metric") ? UNITS_METRIC : UNITS_AVIATION;
    } else if (!strcmp(k, "routes")) {
        c->routes = on(v);
    } else if (!strcmp(k, "labels")) {
        c->labels = on(v);
    }
}

bool vl_conf_load(app_t *a)
{
    long mt = conf_mtime();
    a->conf_mtime = mt;
    if (mt < 0) return false;
    char path[160];
    conf_path(path, sizeof path);
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    char *buf = (char *)malloc(4096);
    int n = buf ? (int)fread(buf, 1, 4095, fp) : 0;
    fclose(fp);
    if (!buf) return false;
    buf[n] = 0;
    vl_conf_t c = a->conf;
    char *line = buf;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        conf_line(&c, line);
        if (!end) break;
        line = end + 1;
    }
    free(buf);
    a->conf = c;
    return true;
}

bool vl_conf_save(app_t *a)
{
    const vl_conf_t *c = &a->conf;
    char path[160], tmp[168];
    conf_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return false;
    char *b = (char *)malloc(2048);
    if (!b) {
        fclose(fp);
        return false;
    }
    int n = snprintf(b, 2048,
        "# Vuelos: lo escriben la app y su pagina del portal\n"
        "place=%s\t%.5f\t%.5f\n"
        "src=%s\nradius=%d\nurl=%s\nlol_key=%s\nlol_day=%d\nos_id=%s\nos_secret=%s\n"
        "alert_over=%d\nover_km=%.1f\nover_ft=%d\nalert_emerg=%d\nalert_mil=%d\nsound=%d\n"
        "trail=%d\nunits=%s\nroutes=%d\nlabels=%d\n",
        c->name, (double)c->lat, (double)c->lon, SRC_KEY[c->src], c->radius_km, c->url, c->lol_key,
        c->lol_day, c->os_id, c->os_secret, c->alert_over, (double)c->over_km, c->over_ft, c->alert_emerg,
        c->alert_mil, c->alert_sound, c->trail_min, c->units == UNITS_METRIC ? "metric" : "aviation",
        c->routes, c->labels);
    bool ok = n > 0 && n < 2048 && fwrite(b, 1, (size_t)n, fp) == (size_t)n;
    free(b);
    ok = fclose(fp) == 0 && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    a->conf_mtime = conf_mtime();
    return ok;
}

/* The page changed the file: the new settings, and what follows from them. */
void vl_conf_poll(app_t *a, uint32_t now)
{
    if (now - a->conf_check_ms < 2000) return;
    a->conf_check_ms = now;
    long mt = conf_mtime();
    if (mt == a->conf_mtime) return;
    vl_conf_t old = a->conf;
    if (!vl_conf_load(a)) return;
    aos_hal_log("vuelos", "vuelos.txt changed: %s, %d km", vl_src_name(a->conf.src), a->conf.radius_km);
    bool moved = old.lat != a->conf.lat || old.lon != a->conf.lon;
    if (moved) {
        vl_place_apply(a);
        vl_go_to(a, a->place_x, a->place_y, a->view.z);
    }
    if (moved || old.src != a->conf.src || old.radius_km != a->conf.radius_km || strcmp(old.url, a->conf.url) ||
        strcmp(old.lol_key, a->conf.lol_key) || strcmp(old.os_id, a->conf.os_id) ||
        strcmp(old.os_secret, a->conf.os_secret))
        vl_feed_restart(a);
    a->overlay_dirty = true;
}
