/*
 * VUELOS - the aircraft: where they come from, how a reading becomes tracks,
 * and how they move between readings.
 *
 * The sources, and what each allows (README.md has the terms):
 *
 *   airplanes.live  https://api.airplanes.live/v2/point/<lat>/<lon>/<nm>,
 *                   250 NM at most, one request a second at most, for
 *                   non-commercial use. Asked every 5 s.
 *   adsb.lol        https://api.adsb.lol/v2/lat/<lat>/lon/<lon>/dist/<nm>,
 *                   500 requests a day per address without a key, more with
 *                   one (x-api-key). The interval spreads the day's requests
 *                   evenly: 173 s with none.
 *   OpenSky         https://opensky-network.org/api/states/all?lamin=...,
 *                   a box; 400 credits a day anonymous, 4000 with an API
 *                   client (OAuth2 client credentials), 1 to 4 credits a
 *                   request by the box's size. The interval spreads them too.
 *   receiver        readsb's or dump1090's aircraft.json at the address the
 *                   person typed, on their own network: every 2 s.
 *
 * The first two answer in the same shape (readsb's, "ac": [...]), and the
 * receiver's is readsb's own ("aircraft": [...]); OpenSky's is rows of
 * eighteen values. All four become the same vl_ac_t.
 *
 * Between readings an aircraft goes on by its ground speed and track from
 * the time of its fix. When the next reading comes, the difference between
 * where it was being drawn and where the new fix puts it is kept and faded
 * out over a second: it glides to the new track, it never jumps.
 *
 * Origin and destination come from adsbdb (api.adsbdb.com), by callsign,
 * one request when an aircraft is picked, kept in RAM for the session.
 */
#include "vuelos.h"

#include "aos_ui.h"
#include "aos_i18n.h"
#include "aos_text_safe.h"
#include "vl_mem.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI_F        3.14159265f
/* The ceiling of a reading. The HAL sets the whole ceiling aside, in one
 * piece, before it asks; with the app's own buffers the board had 1.7 MB of
 * PSRAM left and never one piece of the 1.5 MB this used to be, so no
 * request ever left and nothing said so (the board, 2026-11-22). An aircraft
 * is about a kilobyte in readsb's JSON, and only the nearest VL_MAX_AC are
 * kept: the ceiling goes by the radius, and vl_feed_pump() asks for less when
 * even that is not there. A reading that does not fit is cut and its last
 * aircraft are lost, which feed_parse() takes. */
#define FEED_MAX_NEAR   (256 * 1024)    /* up to 60 NM */
#define FEED_MAX_MID    (512 * 1024)    /* up to 130 NM */
#define FEED_MAX_FAR    (768 * 1024)    /* 250 NM around a hub is ~600 aircraft */
#define FEED_MAX_LEAST  (48 * 1024)
#define MAX_RECS    700
#define KT_MS       0.514444f
#define NM_KM       1.852f
#define OS_TOKEN_URL "/auth/realms/opensky-network/protocol/openid-connect/token"

/* ---------------------------------------------------------------------------
 * Hosts. VUELOS_FAKE=http://127.0.0.1:port (simulator, tests) sends every
 * service to the fake server in tools/fake_adsb.py instead; on the board
 * getenv() is always NULL.
 * ------------------------------------------------------------------------- */

static const char *host(const char *real)
{
    const char *f = getenv("VUELOS_FAKE");
    return f && f[0] ? f : real;
}

/* airplanes.live answers 403 to some networks, whatever is asked (the
 * author's, 2026-11-22: a page that says to write to them). It is the
 * default and the one that allows a reading every 5 s, so it stays the
 * default, and when it refuses before giving a single reading adsb.lol
 * stands in for it, with adsb.lol's own pace. Choosing a source again
 * (vl_feed_restart) tries the chosen one anew. */
int vl_src_now(const app_t *a)
{
    return a->src_alt && a->conf.src == SRC_AIRPLANES ? SRC_ADSBLOL : a->conf.src;
}

/* ---------------------------------------------------------------------------
 * A small JSON scanner: enough to walk an object's keys and an array's
 * values without building anything.
 * ------------------------------------------------------------------------- */

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return p;
}

/* past the string that starts at p (on its quote) */
static const char *str_skip(const char *p)
{
    p++;
    while (*p && *p != '"') p += p[0] == '\\' && p[1] ? 2 : 1;
    return *p ? p + 1 : p;
}

static const char *val_skip(const char *p)
{
    p = ws(p);
    if (*p == '"') return str_skip(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = str_skip(p);
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* the string at p into out, without escapes (none of what is read here
 * has any that matter); false if p is not a string */
static bool str_get(const char *p, char *out, size_t n)
{
    p = ws(p);
    if (*p != '"') {
        if (n) out[0] = 0;
        return false;
    }
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        if (p[0] == '\\' && p[1]) p++;
        if (o + 1 < n) out[o++] = *p;
        p++;
    }
    out[o] = 0;
    return true;
}

static bool num_get(const char *p, float *out)
{
    p = ws(p);
    if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) return false;
    char *e;
    float v = strtof(p, &e);
    if (e == p) return false;
    *out = v;
    return true;
}

/* Walks the members of the object at p: cb(key, value, user) for each.
 * Returns past the object. */
typedef void (*member_fn)(const char *key, size_t klen, const char *val, void *user);

static const char *obj_walk(const char *p, member_fn cb, void *user)
{
    p = ws(p);
    if (*p != '{') return val_skip(p);
    p++;
    for (;;) {
        p = ws(p);
        if (*p == '}') return p + 1;
        if (*p != '"') return val_skip(p);
        const char *k = p + 1;
        const char *ke = str_skip(p);
        size_t kl = (size_t)(ke - 1 - k);
        p = ws(ke);
        if (*p != ':') return p;
        p = ws(p + 1);
        cb(k, kl, p, user);
        p = ws(val_skip(p));
        if (*p == ',') p++;
        else if (*p == '}') return p + 1;
        else return p;
    }
}

/* the value of the member "key" of the object at p, NULL if none */
static const char *obj_find(const char *p, const char *key)
{
    p = ws(p);
    if (*p != '{') return NULL;
    p++;
    size_t kn = strlen(key);
    for (;;) {
        p = ws(p);
        if (*p != '"') return NULL;
        const char *k = p + 1;
        const char *ke = str_skip(p);
        p = ws(ke);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if ((size_t)(ke - 1 - k) == kn && !strncmp(k, key, kn)) return p;
        p = ws(val_skip(p));
        if (*p != ',') return NULL;
        p++;
    }
}

static bool key_is(const char *k, size_t kl, const char *name)
{
    return strlen(name) == kl && !strncmp(k, name, kl);
}

/* ---------------------------------------------------------------------------
 * One aircraft of a reading
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t hex;
    char     flight[10], reg[12], type[6], cat[3];
    bool     ground, mil;
    bool     has_alt, has_gs, has_vr, has_trk, has_sq, has_pos;
    int32_t  alt_ft, vr_fpm;
    float    gs_kt, trk, lat, lon;
    float    age_s;             /* of the position */
    uint16_t sq;
    float    dist_km, brg;
} rec_t;

static uint32_t hex_of(const char *s)
{
    uint32_t v = 0, flag = 0;
    if (*s == '~') {
        flag = 1u << 24;
        s++;
    }
    for (int i = 0; i < 6 && *s; i++, s++) {
        char c = *s;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint32_t)d;
    }
    return v | flag;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '_')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static void v2_member(const char *k, size_t kl, const char *v, void *user)
{
    rec_t *r = (rec_t *)user;
    char s[16];
    float f;
    if (key_is(k, kl, "hex")) {
        if (str_get(v, s, sizeof s)) r->hex = hex_of(s);
    } else if (key_is(k, kl, "flight")) {
        str_get(v, r->flight, sizeof r->flight);
        trim(r->flight);
    } else if (key_is(k, kl, "r")) {
        str_get(v, r->reg, sizeof r->reg);
    } else if (key_is(k, kl, "t")) {
        str_get(v, r->type, sizeof r->type);
    } else if (key_is(k, kl, "category")) {
        str_get(v, r->cat, sizeof r->cat);
    } else if (key_is(k, kl, "alt_baro") || key_is(k, kl, "altitude")) {
        if (str_get(v, s, sizeof s)) {
            if (!strcmp(s, "ground")) r->ground = true;
        } else if (num_get(v, &f)) {
            r->alt_ft = (int32_t)f;
            r->has_alt = true;
        }
    } else if (key_is(k, kl, "alt_geom")) {
        if (!r->has_alt && num_get(v, &f)) {
            r->alt_ft = (int32_t)f;
            r->has_alt = true;
        }
    } else if (key_is(k, kl, "gs") || key_is(k, kl, "speed")) {
        if (num_get(v, &f)) {
            r->gs_kt = f;
            r->has_gs = true;
        }
    } else if (key_is(k, kl, "track")) {
        if (num_get(v, &f)) {
            r->trk = f;
            r->has_trk = true;
        }
    } else if (key_is(k, kl, "true_heading") || key_is(k, kl, "mag_heading")) {
        if (!r->has_trk && num_get(v, &f)) {
            r->trk = f;
            r->has_trk = true;
        }
    } else if (key_is(k, kl, "baro_rate") || key_is(k, kl, "vert_rate")) {
        if (num_get(v, &f)) {
            r->vr_fpm = (int32_t)f;
            r->has_vr = true;
        }
    } else if (key_is(k, kl, "geom_rate")) {
        if (!r->has_vr && num_get(v, &f)) {
            r->vr_fpm = (int32_t)f;
            r->has_vr = true;
        }
    } else if (key_is(k, kl, "squawk")) {
        if (str_get(v, s, sizeof s) && strlen(s) == 4) {
            r->sq = (uint16_t)atoi(s);
            r->has_sq = true;
        }
    } else if (key_is(k, kl, "lat")) {
        if (num_get(v, &f)) {
            r->lat = f;
            r->has_pos = true;
        }
    } else if (key_is(k, kl, "lon")) {
        if (num_get(v, &f)) r->lon = f;
    } else if (key_is(k, kl, "seen_pos")) {
        if (num_get(v, &f)) r->age_s = f;
    } else if (key_is(k, kl, "dbFlags")) {
        if (num_get(v, &f)) r->mil = ((int)f & 1) != 0;
    }
}

/* OpenSky's row: [icao24, callsign, country, t_pos, t_contact, lon, lat,
 * baro_alt m, on_ground, velocity m/s, track, vrate m/s, sensors, geo_alt m,
 * squawk, spi, source, category] */
static const char *os_row(const char *p, rec_t *r, float now_s)
{
    p = ws(p);
    if (*p != '[') return val_skip(p);
    p++;
    float f, tpos = 0;
    bool has_lon = false, has_lat = false, gnd = false;
    char s[16];
    for (int i = 0;; i++) {
        p = ws(p);
        if (*p == ']') {
            p++;
            break;
        }
        switch (i) {
        case 0: if (str_get(p, s, sizeof s)) r->hex = hex_of(s); break;
        case 1: str_get(p, r->flight, sizeof r->flight); trim(r->flight); break;
        case 3: if (num_get(p, &f)) tpos = f; break;
        case 5: if (num_get(p, &f)) { r->lon = f; has_lon = true; } break;
        case 6: if (num_get(p, &f)) { r->lat = f; has_lat = true; } break;
        case 7: if (num_get(p, &f)) { r->alt_ft = (int32_t)(f / 0.3048f); r->has_alt = true; } break;
        case 8: gnd = !strncmp(ws(p), "true", 4); break;
        case 9: if (num_get(p, &f)) { r->gs_kt = f / KT_MS; r->has_gs = true; } break;
        case 10: if (num_get(p, &f)) { r->trk = f; r->has_trk = true; } break;
        case 11: if (num_get(p, &f)) { r->vr_fpm = (int32_t)(f / 0.3048f * 60.0f); r->has_vr = true; } break;
        case 13: if (!r->has_alt && num_get(p, &f)) { r->alt_ft = (int32_t)(f / 0.3048f); r->has_alt = true; } break;
        case 14: if (str_get(p, s, sizeof s) && strlen(s) == 4) { r->sq = (uint16_t)atoi(s); r->has_sq = true; } break;
        case 17:
            /* 0 none, 1 no category; 2 light .. 7 high performance, 8 rotorcraft,
             * 9 glider, 10 lighter than air, 11 parachutist, 12 ultralight,
             * 14 UAV, 15 space, 16-17 surface vehicles */
            if (num_get(p, &f)) {
                static const char *const C[] = { "", "", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "B1",
                                                 "B2", "B3", "B4", "", "B6", "B7", "C1", "C2", "C3", "C3" };
                int c = (int)f;
                if (c >= 0 && c < 20) snprintf(r->cat, sizeof r->cat, "%s", C[c]);
            }
            break;
        default: break;
        }
        p = ws(val_skip(p));
        if (*p == ',') p++;
    }
    r->ground = gnd;
    if (gnd) r->has_alt = false;
    r->has_pos = has_lon && has_lat;
    r->age_s = tpos > 0 && now_s > tpos ? now_s - tpos : 0;
    return p;
}

/* ---------------------------------------------------------------------------
 * What an aircraft is drawn as: its emitter category, else its type
 * ------------------------------------------------------------------------- */

static bool type_in(const char *t, const char *const *list)
{
    for (; *list; list++)
        if (!strcmp(t, *list)) return true;
    return false;
}

static uint8_t kind_of(const rec_t *r)
{
    static const char *const HELI[] = {
        "R22", "R44", "R66", "EC20", "EC30", "EC35", "EC45", "EC55", "EC75", "AS50", "AS55", "AS65", "AS32",
        "H125", "H130", "H135", "H145", "H155", "H160", "H175", "B06", "B407", "B412", "B429", "B505", "A109",
        "A119", "A139", "A169", "A189", "S76", "S92", "H60", "UH1", "B212", "MI8", "MI17", "NH90", "EH10",
        "CH47", "K32", "BK17", "GAZL", "ALO3", NULL };
    static const char *const HEAVY[] = {
        "A332", "A333", "A338", "A339", "A342", "A343", "A345", "A346", "A359", "A35K", "A388", "A3ST", "B744",
        "B748", "B752", "B753", "B762", "B763", "B764", "B772", "B773", "B77L", "B77W", "B778", "B779", "B788",
        "B789", "B78X", "MD11", "DC10", "IL96", "A124", "C5M", "C17", "A400", "KC2", "K35R", "B742", "A306",
        "A30B", "BLCF", "A337", NULL };
    static const char *const PROP[] = {
        "AT43", "AT45", "AT72", "AT75", "AT76", "DH8A", "DH8B", "DH8C", "DH8D", "SF34", "JS41", "D328", "F50",
        "SW4", "B190", "E120", "C130", "C30J", "AN26", "L410", "DHC6", "CRJ1", "CRJ2", "CRJ7", "CRJ9", "CRJX",
        "E135", "E145", "E170", "E75L", "E75S", "E190", "E195", "E290", "E295", "BCS1", "BCS3", "RJ85",
        "RJ1H", "B461", "B462", "B463", "F70", "F100", "AJ27", "C919", "SU95", "PC12", "PC24", "C208", "BE20",
        "B350", "C25A", "C25B", "C25C", "C510", "C525", "C550", "C560", "C56X", "C680", "C68A", "C750", "E50P",
        "E55P", "LJ35", "LJ45", "LJ60", "FA7X", "FA8X", "F2TH", "F900", "GLF4", "GLF5", "GLF6", "GLEX", "CL30",
        "CL35", "CL60", "H25B", "PRM1", "HDJT", "SF50", "TBM7", "TBM8", "TBM9", NULL };
    static const char *const BALLOON[] = { "BALL", "SHIP", NULL };
    static const char *const GLIDER[] = { "GLID", "ASK21", "DG1T", "DISC", "DUO", "LS8", "ASW2", NULL };
    const char *c = r->cat;
    if (!strcmp(c, "C1") || !strcmp(c, "C2") || !strcmp(c, "C3")) return K_GROUND;
    if (!strcmp(c, "A7") || type_in(r->type, HELI)) return K_HELI;
    if (!strcmp(c, "B2") || type_in(r->type, BALLOON)) return K_BALLOON;
    if (!strcmp(c, "B1") || type_in(r->type, GLIDER)) return K_GLIDER;
    if (!strcmp(c, "B6")) return K_DRONE;
    if (!strcmp(c, "A5") || type_in(r->type, HEAVY)) return K_HEAVY;
    if (type_in(r->type, PROP)) return K_PROP;
    if (!strcmp(c, "A1") || !strcmp(c, "B4")) return K_LIGHT;
    if (!strcmp(c, "A2")) return K_PROP;
    if (!c[0] && r->type[0] && (r->type[0] == 'C' || r->type[0] == 'P' || r->type[0] == 'S') &&
        r->type[1] >= '0' && r->type[1] <= '9')
        return K_LIGHT;             /* C172, P28A, SR22 and the like */
    if (!c[0] && !r->type[0] && r->has_alt && r->alt_ft < 5000 && r->has_gs && r->gs_kt < 140) return K_LIGHT;
    return K_JET;
}

/* ---------------------------------------------------------------------------
 * Geometry
 * ------------------------------------------------------------------------- */

static void dist_brg(float lat0, float lon0, float lat, float lon, float *km, float *brg)
{
    float c = cosf((lat0 + lat) * 0.5f * PI_F / 180.0f);
    float dx = (lon - lon0) * 111.32f * c, dy = (lat - lat0) * 110.57f;
    if (dx > 20000 || dx < -20000) dx = 0;
    *km = sqrtf(dx * dx + dy * dy);
    float b = atan2f(dx, dy) * 180.0f / PI_F;
    *brg = b < 0 ? b + 360.0f : b;
}

/* metres per world unit at a latitude */
static float m_per_unit(float lat)
{
    return 40075016.7f * cosf(lat * PI_F / 180.0f) / 4294967296.0f;
}

/* How far past its fix an aircraft is carried: to the next reading and a
 * little more, never more than four minutes. */
static uint32_t carry_ms(const app_t *a)
{
    uint32_t c = (uint32_t)vl_feed_interval_s(a) * 1500u + 15000u;
    return c > 240000u ? 240000u : c;
}

bool vl_ac_stale(const vl_ac_t *ac, uint32_t now)
{
    return !ac->used || (int32_t)(now - ac->fix_ms) > 300000;
}

void vl_ac_pos(const app_t *a, const vl_ac_t *ac, uint32_t now, uint32_t *x, uint32_t *y)
{
    int32_t dt = (int32_t)(now - ac->fix_ms);
    if (dt < 0) dt = 0;
    uint32_t cap = carry_ms(a);
    if ((uint32_t)dt > cap) dt = (int32_t)cap;
    float t = (float)dt / 1000.0f;
    float fx = ac->vx * t, fy = ac->vy * t;
    int32_t de = (int32_t)(now - ac->e_ms);
    if (de >= 0 && de < 3000) {
        float k = expf(-(float)de / 450.0f);
        fx += ac->ex * k;
        fy += ac->ey * k;
    }
    *x = ac->fix_x + (uint32_t)(int32_t)fx;
    *y = ac->fix_y + (uint32_t)(int32_t)fy;
}

int vl_ac_find(const app_t *a, uint32_t hex)
{
    for (int i = 0; i < a->nac; i++)
        if (a->ac[i].used && a->ac[i].hex == hex) return i;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Units
 * ------------------------------------------------------------------------- */

/* 12345 -> "12 345", with a narrow no-break space as the system writes */
static void thousands(int v, char *out, size_t n)
{
    char t[16];
    int neg = v < 0;
    unsigned u = (unsigned)(neg ? -v : v);
    snprintf(t, sizeof t, "%u", u);
    size_t l = strlen(t), o = 0;
    if (neg && o + 1 < n) out[o++] = '-';
    for (size_t i = 0; i < l && o + 4 < n; i++) {
        out[o++] = t[i];
        size_t left = l - 1 - i;
        if (left && left % 3 == 0) {
            out[o++] = (char)0xC2;      /* U+00A0, the no-break space */
            out[o++] = (char)0xA0;
        }
    }
    out[o] = 0;
}

void vl_fmt_alt(const app_t *a, const vl_ac_t *ac, char *out, size_t n)
{
    if (ac->ground) {
        snprintf(out, n, "%s", _("en tierra"));
        return;
    }
    if (!ac->has_alt) {
        snprintf(out, n, "-");
        return;
    }
    char t[20];
    if (a->conf.units == UNITS_METRIC) {
        thousands((int)roundf((float)ac->alt_ft * 0.3048f), t, sizeof t);
        snprintf(out, n, "%s m", t);
    } else {
        thousands((int)ac->alt_ft, t, sizeof t);
        snprintf(out, n, "%s ft", t);
    }
}

void vl_fmt_speed(const app_t *a, const vl_ac_t *ac, char *out, size_t n)
{
    if (!ac->has_gs) {
        snprintf(out, n, "-");
        return;
    }
    if (a->conf.units == UNITS_METRIC) snprintf(out, n, "%d km/h", (int)roundf(ac->gs_kt * NM_KM));
    else snprintf(out, n, "%d kt", (int)roundf(ac->gs_kt));
}

void vl_fmt_vrate(const app_t *a, const vl_ac_t *ac, char *out, size_t n)
{
    if (!ac->has_vr || ac->ground) {
        snprintf(out, n, "-");
        return;
    }
    char t[20];
    if (a->conf.units == UNITS_METRIC) {
        float ms = (float)ac->vr_fpm * 0.3048f / 60.0f;
        snprintf(out, n, "%s%.1f m/s", ms > 0.05f ? "+" : "", (double)ms);
    } else {
        int v = (int)ac->vr_fpm;
        v = v / 64 * 64;            /* the transponder's step */
        thousands(v, t, sizeof t);
        snprintf(out, n, "%s%s ft/min", v > 0 ? "+" : "", t);
    }
}

void vl_fmt_dist(const app_t *a, float km, char *out, size_t n)
{
    if (a->conf.units == UNITS_METRIC) {
        if (km < 10) snprintf(out, n, "%.1f km", (double)km);
        else snprintf(out, n, "%d km", (int)roundf(km));
    } else {
        float nm = km / NM_KM;
        if (nm < 10) snprintf(out, n, "%.1f NM", (double)nm);
        else snprintf(out, n, "%d NM", (int)roundf(nm));
    }
}

/* ---------------------------------------------------------------------------
 * Requests: how often, and what is asked
 * ------------------------------------------------------------------------- */

static int os_credits(const app_t *a)
{
    float dlat = (float)a->conf.radius_km / 110.57f * 2.0f;
    float c = cosf(a->conf.lat * PI_F / 180.0f);
    float dlon = (float)a->conf.radius_km / (111.32f * (c < 0.1f ? 0.1f : c)) * 2.0f;
    float area = dlat * dlon;
    return area <= 25 ? 1 : area <= 100 ? 2 : area <= 400 ? 3 : 4;
}

static int day_budget(const app_t *a)
{
    if (vl_src_now(a) == SRC_ADSBLOL) return a->conf.lol_day;
    if (vl_src_now(a) == SRC_OPENSKY) return a->conf.os_id[0] && a->conf.os_secret[0] ? 4000 : 400;
    return 0;
}

int vl_feed_interval_s(const app_t *a)
{
    switch (vl_src_now(a)) {
    case SRC_AIRPLANES: return 5;
    case SRC_LOCAL: return 2;
    case SRC_ADSBLOL: {
        int s = (86400 + a->conf.lol_day - 1) / a->conf.lol_day;
        return s < 5 ? 5 : s;
    }
    case SRC_OPENSKY: {
        int b = day_budget(a);
        int s = (86400 * os_credits(a) + b - 1) / b;
        int min = a->conf.os_id[0] ? 5 : 10;
        return s < min ? min : s;
    }
    default: return 10;
    }
}

/* the requests of today, for the sources that count them by the day (UTC) */
static int32_t today(void)
{
    if (!aos_hal_time_is_valid()) return -1;
    return (int32_t)(time(NULL) / 86400);
}

static void day_load(app_t *a)
{
    int32_t d = -1, u = 0;
    aos_hal_pref_get_i32("vl_dayno", &d);
    aos_hal_pref_get_i32("vl_dayused", &u);
    int32_t t = today();
    a->day_no = t;
    a->day_used = t >= 0 && d == t ? u : 0;
}

static void day_count(app_t *a)
{
    if (!day_budget(a)) return;
    int32_t t = today();
    if (t != a->day_no) {
        a->day_no = t;
        a->day_used = 0;
    }
    a->day_used++;
    if (t >= 0) {
        aos_hal_pref_set_i32("vl_dayno", t);
        aos_hal_pref_set_i32("vl_dayused", a->day_used);
    }
}

static void url_enc(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out[o++] = (char)c;
        else
            o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

/* OpenSky's token for an API client: a POST of the client credentials */
static bool os_token_pump(app_t *a, uint32_t now)
{
    if (!a->conf.os_id[0] || !a->conf.os_secret[0]) return true;
    if (a->os_token[0] && (int32_t)(a->os_token_until - now) > 60000) return true;
    if (a->os_token_id > 0) {
        aos_http_state_t hs = aos_hal_http_state(a->os_token_id);
        if (hs == AOS_HTTP_BUSY) return false;
        bool ok = false;
        if (hs == AOS_HTTP_DONE && aos_hal_http_status(a->os_token_id) == 200) {
            const char *body = aos_hal_http_body(a->os_token_id);
            const char *t = body ? strstr(body, "\"access_token\"") : NULL;
            if (t) {
                t = strchr(t + 14, ':');
                if (t && str_get(t + 1, a->os_token, sizeof a->os_token) && a->os_token[0]) {
                    float exp = 1800;
                    const char *e = strstr(body, "\"expires_in\"");
                    if (e && (e = strchr(e, ':'))) num_get(e + 1, &exp);
                    a->os_token_until = now + (uint32_t)(exp * 1000.0f);
                    ok = true;
                }
            }
        }
        if (!ok) {
            a->feed_err = hs == AOS_HTTP_DONE ? aos_hal_http_status(a->os_token_id) : aos_hal_http_status(a->os_token_id);
            aos_hal_log("vuelos", "opensky token: failed (%d)", a->feed_err);
            a->os_token[0] = 0;
            a->feed_next = now + 30000;
        } else {
            aos_hal_log("vuelos", "opensky token: valid for %u s", (unsigned)((a->os_token_until - now) / 1000));
        }
        aos_hal_http_release(a->os_token_id);
        a->os_token_id = 0;
        return ok;
    }
    char id[200], sec[200], body[460], url[200];
    url_enc(a->conf.os_id, id, sizeof id);
    url_enc(a->conf.os_secret, sec, sizeof sec);
    snprintf(body, sizeof body, "grant_type=client_credentials&client_id=%s&client_secret=%s", id, sec);
    snprintf(url, sizeof url, "%s%s", host("https://auth.opensky-network.org"), OS_TOKEN_URL);
    a->os_token_id = aos_hal_http_request("POST", url, NULL, body, "application/x-www-form-urlencoded", 16 * 1024);
    if (a->os_token_id <= 0) {
        a->os_token_id = 0;
        a->feed_next = now + 3000;
    }
    return false;
}

static int feed_max(const app_t *a)
{
    float nm = (float)a->conf.radius_km / NM_KM;
    return nm <= 60 ? FEED_MAX_NEAR : nm <= 130 ? FEED_MAX_MID : FEED_MAX_FAR;
}

/* The request's id, or what aos_hal_http_request() said instead: -2 with
 * every slot taken, -3 without the memory for 'max'. */
static int feed_ask(app_t *a, int max)
{
    const vl_conf_t *c = &a->conf;
    char url[240];
    int nm = (int)ceilf((float)c->radius_km / NM_KM);
    if (nm > 250) nm = 250;
    switch (vl_src_now(a)) {
    case SRC_AIRPLANES:
        snprintf(url, sizeof url, "%s/v2/point/%.4f/%.4f/%d", host("https://api.airplanes.live"), (double)c->lat,
                 (double)c->lon, nm);
        return aos_hal_http_get(url, max);
    case SRC_ADSBLOL: {
        snprintf(url, sizeof url, "%s/v2/lat/%.4f/lon/%.4f/dist/%d", host("https://api.adsb.lol"), (double)c->lat,
                 (double)c->lon, nm);
        if (!c->lol_key[0]) return aos_hal_http_get(url, max);
        char hdr[120];
        snprintf(hdr, sizeof hdr, "x-api-key: %.80s\r\n", c->lol_key);
        return aos_hal_http_request("GET", url, hdr, NULL, NULL, max);
    }
    case SRC_OPENSKY: {
        float dlat = (float)c->radius_km / 110.57f;
        float cl = cosf(c->lat * PI_F / 180.0f);
        float dlon = (float)c->radius_km / (111.32f * (cl < 0.1f ? 0.1f : cl));
        snprintf(url, sizeof url, "%s/api/states/all?lamin=%.3f&lomin=%.3f&lamax=%.3f&lomax=%.3f&extended=1",
                 host("https://opensky-network.org"), (double)(c->lat - dlat), (double)(c->lon - dlon),
                 (double)(c->lat + dlat), (double)(c->lon + dlon));
        if (!a->os_token[0]) return aos_hal_http_get(url, max);
        size_t hn = strlen(a->os_token) + 40;
        char *hdr = (char *)malloc(hn);
        if (!hdr) return -1;
        snprintf(hdr, hn, "Authorization: Bearer %s\r\n", a->os_token);
        int id = aos_hal_http_request("GET", url, hdr, NULL, NULL, max);
        free(hdr);
        return id;
    }
    case SRC_LOCAL:
        if (!c->url[0]) return -1;
        return aos_hal_http_get(c->url, max);
    default: return -1;
    }
}

/* ---------------------------------------------------------------------------
 * A reading becomes tracks
 * ------------------------------------------------------------------------- */

static int rec_cmp(const void *x, const void *y)
{
    const rec_t *a = (const rec_t *)x, *b = (const rec_t *)y;
    return a->dist_km < b->dist_km ? -1 : a->dist_km > b->dist_km;
}

static void trail_add(app_t *a, vl_ac_t *ac, uint32_t now)
{
    if (ac->ntrail) {
        const trail_pt_t *l = &ac->trail[(ac->trail_i + VL_TRAIL - 1) % VL_TRAIL];
        float m = m_per_unit(ac->lat);
        float dx = (float)(int32_t)(ac->fix_x - l->x) * m, dy = (float)(int32_t)(ac->fix_y - l->y) * m;
        if (dx * dx + dy * dy < 150.0f * 150.0f && now - l->t < 30000) return;
    }
    (void)a;
    trail_pt_t *p = &ac->trail[ac->trail_i];
    p->x = ac->fix_x;
    p->y = ac->fix_y;
    p->alt = ac->ground ? 0 : ac->has_alt ? ac->alt_ft : INT32_MIN;
    p->t = ac->fix_ms;
    ac->trail_i = (ac->trail_i + 1) % VL_TRAIL;
    if (ac->ntrail < VL_TRAIL) ac->ntrail++;
}

static void merge(app_t *a, const rec_t *r, uint32_t now)
{
    int i = vl_ac_find(a, r->hex);
    vl_ac_t *ac;
    bool fresh = i < 0;
    if (fresh) {
        for (i = 0; i < a->nac && a->ac[i].used; i++) {}
        if (i >= VL_MAX_AC) return;
        if (i == a->nac) a->nac++;
        ac = &a->ac[i];
        memset(ac, 0, sizeof *ac);
        ac->used = true;
        ac->hex = r->hex;
    } else {
        ac = &a->ac[i];
    }
    uint32_t fix_ms = now - (uint32_t)(r->age_s * 1000.0f);
    /* an older fix than the one held (a source that repeats itself) only
     * refreshes the rest */
    bool newer = fresh || (int32_t)(fix_ms - ac->fix_ms) > 200 || r->lat != ac->lat || r->lon != ac->lon;

    uint32_t dx_now = 0, dy_now = 0;
    if (!fresh) vl_ac_pos(a, ac, now, &dx_now, &dy_now);

    if (r->flight[0]) snprintf(ac->flight, sizeof ac->flight, "%s", r->flight);
    if (r->reg[0]) snprintf(ac->reg, sizeof ac->reg, "%s", r->reg);
    if (r->type[0]) snprintf(ac->type, sizeof ac->type, "%s", r->type);
    if (r->cat[0]) snprintf(ac->cat, sizeof ac->cat, "%s", r->cat);
    ac->ground = r->ground;
    ac->mil = r->mil;
    ac->has_alt = r->has_alt;
    ac->alt_ft = r->alt_ft;
    ac->has_vr = r->has_vr;
    ac->vr_fpm = r->vr_fpm;
    ac->has_gs = r->has_gs;
    ac->gs_kt = r->gs_kt;
    if (r->has_trk) {
        ac->has_trk = true;
        ac->trk = r->trk;
    }
    ac->has_sq = r->has_sq;
    ac->sq = r->sq;
    ac->dist_km = r->dist_km;
    ac->brg = r->brg;
    ac->seen_ms = now;
    rec_t kr = *r;
    snprintf(kr.cat, sizeof kr.cat, "%s", ac->cat);
    snprintf(kr.type, sizeof kr.type, "%s", ac->type);
    ac->kind = kind_of(&kr);

    if (newer) {
        ac->lat = r->lat;
        ac->lon = r->lon;
        vl_lonlat_to_world(r->lon, r->lat, &ac->fix_x, &ac->fix_y);
        ac->fix_ms = fix_ms;
        float m = m_per_unit(r->lat);
        float v = ac->has_gs && !(ac->ground && ac->gs_kt < 2) ? ac->gs_kt * KT_MS / m : 0;
        float t = ac->trk * PI_F / 180.0f;
        ac->vx = v * sinf(t);
        ac->vy = -v * cosf(t);
        ac->ex = ac->ey = 0;
        if (!fresh) {
            uint32_t px, py;
            ac->e_ms = 0;
            vl_ac_pos(a, ac, now, &px, &py);
            float ex = (float)(int32_t)(dx_now - px), ey = (float)(int32_t)(dy_now - py);
            /* farther than 3 km: a new track, not a correction */
            if (sqrtf(ex * ex + ey * ey) * m < 3000.0f) {
                ac->ex = ex;
                ac->ey = ey;
                ac->e_ms = now;
            }
        }
        trail_add(a, ac, now);
    }
}

/* the alerts of this reading: one toast, the most serious first */
static void alerts(app_t *a)
{
    const vl_conf_t *c = &a->conf;
    int best = -1, what = 0;
    for (int i = 0; i < a->nac; i++) {
        vl_ac_t *ac = &a->ac[i];
        if (!ac->used || ac->seen_ms != a->feed_ok_ms) continue;
        bool over = !ac->ground && ac->has_alt && ac->alt_ft <= c->over_ft && ac->dist_km <= c->over_km;
        if (!over && ac->dist_km > c->over_km * 2) ac->alerted &= (uint8_t)~ALERT_OVER;
        int w = 0;
        if (c->alert_emerg && ac->has_sq && (ac->sq == 7500 || ac->sq == 7600 || ac->sq == 7700) &&
            !(ac->alerted & ALERT_EMERG))
            w = ALERT_EMERG;
        else if (c->alert_over && over && !(ac->alerted & ALERT_OVER))
            w = ALERT_OVER;
        else if (c->alert_mil && ac->mil && !(ac->alerted & ALERT_MIL))
            w = ALERT_MIL;
        if (w && (best < 0 || w < what || (w == what && ac->dist_km < a->ac[best].dist_km))) {
            best = i;
            what = w;
        }
    }
    if (best < 0) return;
    vl_ac_t *ac = &a->ac[best];
    ac->alerted |= (uint8_t)what;
    char name[24], alt[24], txt[120];
    snprintf(name, sizeof name, "%s", ac->flight[0] ? ac->flight : ac->reg[0] ? ac->reg : "?");
    vl_fmt_alt(a, ac, alt, sizeof alt);
    if (what == ALERT_EMERG) {
        const char *why = ac->sq == 7700 ? _("emergencia") : ac->sq == 7600 ? _("falla de radio") : _("interferencia ilícita");
        snprintf(txt, sizeof txt, "%s %s: squawk %04u, %s", _("Atención"), name, (unsigned)ac->sq, why);
    } else if (what == ALERT_OVER) {
        snprintf(txt, sizeof txt, "%s: %s, %s", _("Pasa encima"), name, alt);
    } else {
        snprintf(txt, sizeof txt, "%s: %s, %s", _("Militar"), name, alt);
    }
    aos_ui_toast(txt, 4000);
    if (c->alert_sound) aos_hal_beep(what == ALERT_EMERG ? 1400 : 1000, what == ALERT_EMERG ? 300 : 120);
    aos_hal_log("vuelos", "alert: %s", txt);
}

static bool feed_parse(app_t *a, const char *body, uint32_t now)
{
    rec_t *recs = (rec_t *)vl_malloc(sizeof(rec_t) * MAX_RECS);
    if (!recs) return false;
    int n = 0;
    bool shape = false;
    const char *arr = NULL;
    bool os = a->feed_src == SRC_OPENSKY;
    float now_s = 0;
    if (os) {
        const char *t = obj_find(body, "time");
        if (t) num_get(t, &now_s);
        arr = obj_find(body, "states");
        shape = arr != NULL;
        if (arr && !strncmp(arr, "null", 4)) arr = NULL;
    } else {
        arr = obj_find(body, "ac");
        if (!arr) arr = obj_find(body, "aircraft");
        shape = arr != NULL;
    }
    if (arr && *arr == '[') {
        const char *p = arr + 1;
        while (n < MAX_RECS) {
            p = ws(p);
            if (*p != '{' && *p != '[') break;
            rec_t *r = &recs[n];
            memset(r, 0, sizeof *r);
            p = os ? os_row(p, r, now_s) : obj_walk(p, v2_member, r);
            p = ws(p);
            if (*p == ',') p++;
            if (!r->hex || !r->has_pos || r->age_s > 60) continue;
            if (r->lat < -85 || r->lat > 85 || r->lon < -180 || r->lon > 180) continue;
            dist_brg(a->conf.lat, a->conf.lon, r->lat, r->lon, &r->dist_km, &r->brg);
            if (r->dist_km > a->conf.radius_km * 1.05f) continue;
            n++;
        }
    }
    if (shape) {
        qsort(recs, (size_t)n, sizeof(rec_t), rec_cmp);
        a->feed_ok_ms = now;
        int m = n < VL_MAX_AC ? n : VL_MAX_AC;
        for (int i = 0; i < m; i++) merge(a, &recs[i], now);
        a->feed_count = n;
    }
    vl_free(recs);
    return shape;
}

/* the ones the last readings no longer have */
static void expire(app_t *a, uint32_t now)
{
    uint32_t keep = (uint32_t)vl_feed_interval_s(a) * 2500u + 20000u;
    if (keep < 60000) keep = 60000;
    if (keep > 400000) keep = 400000;
    for (int i = 0; i < a->nac; i++) {
        vl_ac_t *ac = &a->ac[i];
        if (!ac->used || now - ac->seen_ms < keep) continue;
        ac->used = false;
        if (a->sel == i) vl_select(a, -1);
    }
    while (a->nac && !a->ac[a->nac - 1].used) a->nac--;
}

/* ---------------------------------------------------------------------------
 * The portal page's table: the aircraft of the last reading as JSON, on the
 * live channel, only while the page is looking (docs/PORTAL-PAGES.md)
 * ------------------------------------------------------------------------- */

static size_t js_str(char *o, size_t n, const char *s)
{
    size_t k = 0;
    if (k + 1 < n) o[k++] = '"';
    for (; *s && k + 3 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') o[k++] = '\\';
        if (c < 0x20) c = ' ';
        o[k++] = (char)c;
    }
    if (k + 1 < n) o[k++] = '"';
    o[k] = 0;
    return k;
}

static void live_put(app_t *a)
{
    if (aos_hal_live_idle_ms("aos.vuelos") > 15000) return;
    size_t cap = AOS_LIVE_BLOB_MAX;
    char *b = (char *)vl_malloc(cap);
    if (!b) return;
    size_t o = 0;
    char f[24], r[24], t[16], nm[56];
    js_str(nm, sizeof nm, a->conf.name);
    o += (size_t)snprintf(b + o, cap - o, "{\"src\":\"%s\",\"place\":%s,\"units\":%d,\"n\":%d,\"ac\":[",
                          vl_src_name(vl_src_now(a)), nm, a->conf.units, a->feed_count);
    bool first = true;
    for (int i = 0; i < a->nac && o + 400 < cap; i++) {
        const vl_ac_t *ac = &a->ac[i];
        if (!ac->used || ac->seen_ms != a->feed_ok_ms) continue;
        js_str(f, sizeof f, ac->flight);
        js_str(r, sizeof r, ac->reg);
        js_str(t, sizeof t, ac->type);
        o += (size_t)snprintf(b + o, cap - o,
                              "%s{\"hex\":\"%06X\",\"f\":%s,\"r\":%s,\"t\":%s,\"g\":%d,\"alt\":%ld,\"gs\":%d,"
                              "\"vr\":%ld,\"trk\":%d,\"sq\":\"%s%04u\",\"d\":%.1f,\"brg\":%d,\"mil\":%d}",
                              first ? "" : ",", (unsigned)(ac->hex & 0xFFFFFF), f, r, t, ac->ground,
                              ac->has_alt ? (long)ac->alt_ft : -999999L, ac->has_gs ? (int)roundf(ac->gs_kt) : -1,
                              ac->has_vr ? (long)ac->vr_fpm : 0L, ac->has_trk ? (int)roundf(ac->trk) : -1,
                              ac->has_sq ? "" : "-", ac->has_sq ? (unsigned)ac->sq : 0u, (double)ac->dist_km,
                              (int)roundf(ac->brg), ac->mil);
        first = false;
    }
    o += (size_t)snprintf(b + o, cap - o, "]}");
    if (o < cap) aos_hal_live_put("aos.vuelos", "ac", "application/json", b, o);
    vl_free(b);
}

/* ---------------------------------------------------------------------------
 * Routes: adsbdb, by callsign
 * ------------------------------------------------------------------------- */

static void route_airport(const char *obj, char *code, size_t cn, char *city, size_t ctn)
{
    const char *v;
    code[0] = city[0] = 0;
    if ((v = obj_find(obj, "iata_code"))) str_get(v, code, cn);
    if (!code[0] && (v = obj_find(obj, "icao_code"))) str_get(v, code, cn);
    if ((v = obj_find(obj, "municipality"))) str_get(v, city, ctn);
}

const vl_route_t *vl_route_of(app_t *a, const vl_ac_t *ac)
{
    if (!ac->flight[0]) return NULL;
    for (int i = 0; i < VL_ROUTES; i++)
        if (a->routes[i].flight[0] && !strcmp(a->routes[i].flight, ac->flight)) return &a->routes[i];
    return NULL;
}

static void route_pump(app_t *a)
{
    if (a->route_id > 0) {
        aos_http_state_t hs = aos_hal_http_state(a->route_id);
        if (hs == AOS_HTTP_BUSY) return;
        vl_route_t *r = &a->routes[a->route_slot];
        r->state = 2;
        if (hs == AOS_HTTP_DONE && aos_hal_http_status(a->route_id) == 200) {
            const char *b = aos_hal_http_body(a->route_id);
            const char *resp = b ? obj_find(b, "response") : NULL;
            const char *fr = resp ? obj_find(resp, "flightroute") : NULL;
            const char *o = fr ? obj_find(fr, "origin") : NULL;
            const char *d = fr ? obj_find(fr, "destination") : NULL;
            if (o && d) {
                char city[64];
                route_airport(o, r->from, sizeof r->from, city, sizeof city);
                aos_text_safe(r->from_city, sizeof r->from_city, city);
                route_airport(d, r->to, sizeof r->to, city, sizeof city);
                aos_text_safe(r->to_city, sizeof r->to_city, city);
                r->state = r->from[0] && r->to[0] ? 1 : 2;
            }
        }
        aos_hal_log("vuelos", "route %s: %s", r->flight, r->state == 1 ? "known" : "unknown");
        aos_hal_http_release(a->route_id);
        a->route_id = 0;
        a->overlay_dirty = true;
        return;
    }
    if (!a->conf.routes || a->sel < 0 || a->conf.src == SRC_LOCAL) return;
    const vl_ac_t *ac = &a->ac[a->sel];
    if (!ac->flight[0] || vl_route_of(a, ac) || a->ac[a->sel].ground) return;
    if (aos_hal_net_state() != AOS_NET_CONNECTED) return;
    char cs[24], url[160];
    url_enc(ac->flight, cs, sizeof cs);
    snprintf(url, sizeof url, "%s/v0/callsign/%s", host("https://api.adsbdb.com"), cs);
    int id = aos_hal_http_get(url, 16 * 1024);
    if (id <= 0) return;
    a->route_id = id;
    a->route_slot = a->route_i;
    a->route_i = (a->route_i + 1) % VL_ROUTES;
    vl_route_t *r = &a->routes[a->route_slot];
    memset(r, 0, sizeof *r);
    snprintf(r->flight, sizeof r->flight, "%s", ac->flight);
}

/* ---------------------------------------------------------------------------
 * The pump, from the frame timer
 * ------------------------------------------------------------------------- */

bool vl_feed_init(app_t *a)
{
    a->ac = (vl_ac_t *)vl_calloc(sizeof(vl_ac_t) * VL_MAX_AC);
    a->sel = -1;
    day_load(a);
    return a->ac != NULL;
}

void vl_feed_free(app_t *a)
{
    if (a->feed_id > 0) aos_hal_http_release(a->feed_id);
    if (a->route_id > 0) aos_hal_http_release(a->route_id);
    if (a->os_token_id > 0) aos_hal_http_release(a->os_token_id);
    a->feed_id = a->route_id = a->os_token_id = 0;
    aos_hal_live_clear("aos.vuelos");
    vl_free(a->ac);
    a->ac = NULL;
}

void vl_feed_restart(app_t *a)
{
    if (a->feed_id > 0) aos_hal_http_release(a->feed_id);
    if (a->os_token_id > 0) aos_hal_http_release(a->os_token_id);
    a->feed_id = a->os_token_id = 0;
    a->os_token[0] = 0;
    for (int i = 0; i < a->nac; i++) a->ac[i].used = false;
    a->nac = 0;
    a->sel = -1;
    a->feed_count = 0;
    a->feed_ok_ms = 0;
    a->feed_err = 0;
    a->src_alt = false;
    a->feed_next = (uint32_t)aos_hal_uptime_ms();
    a->overlay_dirty = true;
}

void vl_select(app_t *a, int i)
{
    if (i >= 0 && (i >= a->nac || !a->ac[i].used)) i = -1;
    if (i != a->sel) a->follow = false;
    a->sel = i;
    a->sel_hex = i >= 0 ? a->ac[i].hex : 0;
    a->overlay_dirty = true;
}

void vl_feed_pump(app_t *a, uint32_t now)
{
    if (!a->ac) return;
    route_pump(a);
    expire(a, now);

    /* VUELOS_PICK=<callsign> (simulator): picks that flight when it shows up */
    const char *pick = getenv("VUELOS_PICK");
    if (pick && pick[0] && a->sel < 0 && a->feed_ok_ms) {
        for (int i = 0; i < a->nac; i++)
            if (a->ac[i].used && !strcmp(a->ac[i].flight, pick)) {
                vl_select(a, i);
                break;
            }
    }

    if (a->feed_id > 0) {
        aos_http_state_t hs = aos_hal_http_state(a->feed_id);
        if (hs == AOS_HTTP_BUSY) {
            if (now - a->feed_t0 > 30000) {
                aos_hal_log("vuelos", "%s: no answer in 30 s", vl_src_name(a->feed_src));
                aos_hal_http_release(a->feed_id);
                a->feed_id = 0;
                a->feed_err = AOS_HTTP_ERR_RECV;
                a->feed_next = now + 5000;
                a->overlay_dirty = true;
            }
            return;
        }
        int code = aos_hal_http_status(a->feed_id);
        int len = aos_hal_http_len(a->feed_id);
        int iv = vl_feed_interval_s(a);
        if (hs == AOS_HTTP_DONE && code == 200) {
            uint32_t t0 = (uint32_t)aos_hal_uptime_us();
            bool ok = feed_parse(a, aos_hal_http_body(a->feed_id), now);
            uint32_t us = (uint32_t)aos_hal_uptime_us() - t0;
            a->feed_parse_err = !ok;
            a->feed_err = ok ? 0 : -200;
            a->feed_bytes = (uint32_t)len;
            aos_hal_log("vuelos", "%s: %d KB in %u ms, %d aircraft, parsed in %u ms%s", vl_src_name(a->feed_src),
                        len / 1024, (unsigned)(now - a->feed_t0), a->feed_count, (unsigned)(us / 1000),
                        ok ? "" : " (not the expected shape)");
            if (ok) {
                alerts(a);
                live_put(a);
            }
            a->feed_next = a->feed_t0 + (uint32_t)iv * 1000u;
        } else {
            a->feed_err = hs == AOS_HTTP_DONE ? code : code;
            aos_hal_log("vuelos", "%s: failed, %d", vl_src_name(a->feed_src), a->feed_err);
            /* too many requests: ten minutes off; anything else, a while */
            uint32_t back = code == 429 ? 600000u : code == 401 || code == 403 ? 120000u
                          : code == AOS_HTTP_ERR_SIN_HORA ? 3000u : (uint32_t)iv * 1000u + 10000u;
            if (code == 401) a->os_token[0] = 0;
            if ((code == 401 || code == 403) && a->feed_src == SRC_AIRPLANES && !a->feed_ok_ms && !a->src_alt) {
                aos_hal_log("vuelos", "airplanes.live refuses this network: adsb.lol in its place");
                a->src_alt = true;
                a->feed_err = 0;
                back = 500;
            }
            a->feed_next = now + back;
        }
        aos_hal_http_release(a->feed_id);
        a->feed_id = 0;
        a->overlay_dirty = true;
        return;
    }

    if ((int32_t)(now - a->feed_next) < 0) return;
    if (aos_hal_net_state() != AOS_NET_CONNECTED) {
        a->feed_next = now + 1000;
        return;
    }
    if (a->conf.src == SRC_LOCAL && !a->conf.url[0]) return;
    if (a->conf.src == SRC_OPENSKY && !os_token_pump(a, now)) return;
    int budget = day_budget(a);
    if (budget) {
        int32_t t = today();
        if (t != a->day_no) {
            a->day_no = t;
            a->day_used = 0;
        }
        if (a->day_used >= budget) {
            a->feed_err = 429;
            a->feed_next = now + 60000;
            return;
        }
    }
    /* less when the whole ceiling is not there in one piece */
    int max = feed_max(a), id;
    while ((id = feed_ask(a, max)) == -3 && max / 2 >= FEED_MAX_LEAST) max /= 2;
    if (id <= 0) {
        /* -2 is the three slots taken, which passes; the rest is said, once */
        int err = id == -3 ? AOS_HTTP_ERR_MEM : id == -2 ? 0 : AOS_HTTP_ERR_CONNECT;
        if (err && err != a->feed_err) {
            uint32_t psram = 0;
            aos_hal_heap_info(NULL, &psram);
            aos_hal_log("vuelos", "%s: the request did not start (%d), %u KB of PSRAM free", vl_src_name(vl_src_now(a)),
                        id, (unsigned)(psram / 1024));
            a->feed_err = err;
            a->overlay_dirty = true;
        }
        a->feed_next = now + (err ? 5000 : 2000);
        return;
    }
    day_count(a);
    a->feed_id = id;
    a->feed_src = vl_src_now(a);
    a->feed_t0 = now;
}
