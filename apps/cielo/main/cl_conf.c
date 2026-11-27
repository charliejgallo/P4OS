/*
 * CIELO - the settings, in <data>/cielo.txt: key=value lines that the app
 * writes and the portal page reads (and may write: the app looks at the
 * file's date every few seconds and takes it again when it changed).
 *
 *   place=Buenos Aires<TAB>-34.6037<TAB>-58.3816   the observer
 *   height=25                                       metres
 *   night=0 lines=1 cnames=1 snames=1 mw=1 messier=1 grid=0 ground=1
 *   atmos=1 sats=1 planets=1 alerts=1 alert_min=10 mag=0
 *   sat=25544                                       one line per satellite
 *   tle_url=https://...                             where the elements come from
 *   view=az alt fov                                 degrees
 *
 * The place, when there is none yet, is Clima's (its preferences), as
 * Vuelos does; with neither, Buenos Aires, and the app says so.
 */
#include "cielo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEF_TLE_URL "https://celestrak.org/NORAD/elements/gp.php?GROUP=visual&FORMAT=tle"

const char *cl_conf_path(char *out, size_t n)
{
    snprintf(out, n, "%s/cielo.txt", aos_hal_path_data());
    return out;
}

static long conf_mtime(void)
{
    char p[160];
    struct stat st;
    return stat(cl_conf_path(p, sizeof p), &st) == 0 ? (long)st.st_mtime : -1;
}

void cl_conf_defaults(cl_conf_t *c)
{
    memset(c, 0, sizeof *c);
    c->lines = c->cnames = c->snames = c->mw = c->messier = c->ground = c->atmos = c->sats = c->planets = true;
    c->alerts = true;
    c->alert_min = 10;
    c->height = 25;
    /* the ISS, China's station and the Hubble telescope */
    c->sat_ids[0] = 25544;
    c->sat_ids[1] = 48274;
    c->sat_ids[2] = 20580;
    c->nsat_ids = 3;
    snprintf(c->tle_url, sizeof c->tle_url, "%s", DEF_TLE_URL);
    c->az = 0;
    c->alt = 90;
    c->fov = 190;
    int32_t la, lo;
    if (aos_hal_pref_get_i32("clima_lat", &la) && aos_hal_pref_get_i32("clima_lon", &lo)) {
        c->lat = (float)la / 1e4f;
        c->lon = (float)lo / 1e4f;
        aos_hal_pref_get_str("clima_city", c->place, sizeof c->place);
        if (!c->place[0]) snprintf(c->place, sizeof c->place, "%.2f, %.2f", (double)c->lat, (double)c->lon);
        c->have_place = true;
    } else {
        c->lat = -34.6037f;
        c->lon = -58.3816f;
        snprintf(c->place, sizeof c->place, "Buenos Aires");
    }
}

static void copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && src[i] != '\r' && src[i] != '\n' && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

static bool on(const char *v)
{
    return v[0] == '1' || v[0] == 'y' || v[0] == 's' || v[0] == 't';
}

static void line(cl_conf_t *c, char *ln, bool *sats_seen)
{
    char *eq = strchr(ln, '=');
    if (ln[0] == '#' || !eq) return;
    *eq = 0;
    const char *k = ln, *v = eq + 1;
    if (!strcmp(k, "place")) {
        char *t1 = strchr(v, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2) return;
        float lat = strtof(t1 + 1, NULL), lon = strtof(t2 + 1, NULL);
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return;
        *t1 = 0;
        copy(c->place, sizeof c->place, v);
        c->lat = lat;
        c->lon = lon;
        c->have_place = true;
    } else if (!strcmp(k, "height")) {
        c->height = strtof(v, NULL);
    } else if (!strcmp(k, "night")) {
        c->night = on(v);
    } else if (!strcmp(k, "lines")) {
        c->lines = on(v);
    } else if (!strcmp(k, "cnames")) {
        c->cnames = on(v);
    } else if (!strcmp(k, "snames")) {
        c->snames = on(v);
    } else if (!strcmp(k, "mw")) {
        c->mw = on(v);
    } else if (!strcmp(k, "messier")) {
        c->messier = on(v);
    } else if (!strcmp(k, "grid")) {
        c->grid = on(v);
    } else if (!strcmp(k, "ground")) {
        c->ground = on(v);
    } else if (!strcmp(k, "atmos")) {
        c->atmos = on(v);
    } else if (!strcmp(k, "sats")) {
        c->sats = on(v);
    } else if (!strcmp(k, "planets")) {
        c->planets = on(v);
    } else if (!strcmp(k, "alerts")) {
        c->alerts = on(v);
    } else if (!strcmp(k, "alert_min")) {
        int m = atoi(v);
        c->alert_min = m < 1 ? 1 : m > 120 ? 120 : m;
    } else if (!strcmp(k, "mag")) {
        float m = strtof(v, NULL);
        c->mag_extra = m < -3 ? -3 : m > 2 ? 2 : m;
    } else if (!strcmp(k, "sat")) {
        if (!*sats_seen) {
            c->nsat_ids = 0;
            *sats_seen = true;
        }
        uint32_t id = (uint32_t)strtoul(v, NULL, 10);
        if (id && c->nsat_ids < CL_MAX_SATS) c->sat_ids[c->nsat_ids++] = id;
    } else if (!strcmp(k, "nosats")) {
        c->nsat_ids = 0;
        *sats_seen = true;
    } else if (!strcmp(k, "tle_url")) {
        if (!strncmp(v, "http://", 7) || !strncmp(v, "https://", 8)) copy(c->tle_url, sizeof c->tle_url, v);
    } else if (!strcmp(k, "view")) {
        float az, alt, fov;
        if (sscanf(v, "%f %f %f", &az, &alt, &fov) == 3) {
            c->az = az;
            c->alt = alt;
            c->fov = fov;
        }
    }
}

void cl_conf_load(app_t *a)
{
    char p[160], ln[260];
    FILE *f = fopen(cl_conf_path(p, sizeof p), "r");
    a->conf_mtime = conf_mtime();
    if (!f) return;
    bool sats_seen = false;
    while (fgets(ln, sizeof ln, f)) {
        size_t n = strlen(ln);
        while (n && (ln[n - 1] == '\n' || ln[n - 1] == '\r')) ln[--n] = 0;
        line(&a->conf, ln, &sats_seen);
    }
    fclose(f);
}

void cl_conf_save(app_t *a)
{
    const cl_conf_t *c = &a->conf;
    char p[160], tmp[170];
    cl_conf_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs("# Cielo: written by the app and by its portal page\n", f);
    fprintf(f, "place=%s\t%.4f\t%.4f\n", c->place, (double)c->lat, (double)c->lon);
    fprintf(f, "height=%.0f\n", (double)c->height);
    fprintf(f, "night=%d\nlines=%d\ncnames=%d\nsnames=%d\nmw=%d\nmessier=%d\ngrid=%d\nground=%d\natmos=%d\n", c->night,
            c->lines, c->cnames, c->snames, c->mw, c->messier, c->grid, c->ground, c->atmos);
    fprintf(f, "sats=%d\nplanets=%d\nalerts=%d\nalert_min=%d\nmag=%.1f\n", c->sats, c->planets, c->alerts,
            c->alert_min, (double)c->mag_extra);
    if (!c->nsat_ids) fputs("nosats=1\n", f);
    for (int i = 0; i < c->nsat_ids; i++) fprintf(f, "sat=%u\n", (unsigned)c->sat_ids[i]);
    fprintf(f, "tle_url=%s\n", c->tle_url);
    fprintf(f, "view=%.2f %.2f %.2f\n", (double)c->az, (double)c->alt, (double)c->fov);
    fclose(f);
    remove(p);
    rename(tmp, p);
    a->conf_mtime = conf_mtime();
}

/* the page changed the file: take it again (the view stays the app's) */
void cl_conf_poll(app_t *a)
{
    long m = conf_mtime();
    if (m == a->conf_mtime || m < 0) return;
    cl_conf_t old = a->conf;
    cl_conf_defaults(&a->conf);
    cl_conf_load(a);
    a->conf.az = old.az;
    a->conf.alt = old.alt;
    a->conf.fov = old.fov;
    bool sats_changed = old.nsat_ids != a->conf.nsat_ids ||
                        memcmp(old.sat_ids, a->conf.sat_ids, sizeof old.sat_ids) || strcmp(old.tle_url, a->conf.tle_url);
    bool place_changed = old.lat != a->conf.lat || old.lon != a->conf.lon;
    aos_hal_log("cielo", "settings changed by the page%s%s", sats_changed ? ", satellites" : "",
                place_changed ? ", place" : "");
    if (sats_changed) cl_sat_reload(a);
    if (sats_changed || place_changed) cl_sat_replan(a);
    cl_dirty(a);
}
