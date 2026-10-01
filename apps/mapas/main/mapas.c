/*
 * P4OS (from AmoledOS) - MAPAS: a street map on the whole screen.
 *
 * The data are OpenFreeMap's vector tiles (OpenMapTiles schema, from
 * OpenStreetMap), drawn on the board in a dark style: no API key, no quota,
 * and the same tiles online, in the card's cache and in the offline packs a
 * zone is downloaded into.
 *
 *   one finger ....... move the map; a flick keeps it going
 *   two fingers ...... zoom about the point between them
 *   double tap ....... a level closer, there
 *   + / - ............ a level in or out
 *   the search bar ... search (the offline zones first, Photon online)
 *   menu ............. zones, offline maps, settings
 *   long press ....... the coordinates of that point, and a pin there
 *
 * How it is built, and why (mapas.h has the buffers, mp_frame.c the frame):
 *
 *   - The worker (core 0, prio 3) renders the map into buffers bigger than
 *     the screen, of two qualities: FULL, the screen's resolution, shown
 *     while the map is still, and HALF, half of it and a bigger piece of the
 *     world, shown enlarged while the map moves faster than a FULL render
 *     keeps up with. Which one it renders next is decided from what a FULL
 *     render has been costing and how fast the map is moving (want_render).
 *     A FULL render that the view has already left is abandoned halfway.
 *   - LVGL's side never waits for a render: every frame it cuts the screen
 *     out of the best buffer there is (moved, or scaled while a pinch or a
 *     zoom animation is on), draws the buttons on top and blits it. A pan
 *     costs a copy.
 *   - Tiles come from RAM, the offline packs, the card's cache or the
 *     network, in that order (mp_store.c). The requests are made from LVGL's
 *     side (the HAL's HTTP wants that); the worker decodes the answers and
 *     writes them to the cache.
 *
 * It builds two ways from the same source: tools/build_apps.sh mapas for the
 * board, and compiled into the simulator (AOS_SIM_BUILTIN), where there is
 * no blit and the frame goes through an LVGL canvas.
 */
#include "mapas.h"

#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_gesture.h"
#include "aos_fonts.h"

#include "mp_mem.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_STACK (24 * 1024)
#define EDGE        16                  /* re-render when the screen is this close to a buffer's edge */

/* Decoded tiles. A dense z14 tile of a city centre decodes to ~0.5 MB and
 * the screen meets up to 30 at once; the buffers take ~10 MB. */
#define RAM_BUDGET_MAX  (10u * 1024u * 1024u)
#define RAM_BUDGET_MIN  (2u * 1024u * 1024u)
#define HTTP_MAX    (1100 * 1024)       /* a tile's ceiling; the densest z14 seen was 0.5 MB */

/* One download at a time unless the preference map_inflight says more.
 * Measured on the watch (2026-09-26): a 132 KB tile alone came in at
 * 333 KB/s; two TLS downloads at once crawled at 13 KB/s, one was cut after
 * 20 s, and the WiFi logged bcn_timeout - two 16 KB TCP windows filled the
 * receive buffers faster than TLS emptied them. The P4 talks to its radio
 * (an ESP32-C6) over SDIO with a bigger receive window (MEMORY.md), and
 * whether two at once is safe there is still to be measured on the board. */
#define INFLIGHT_DEF 1
#define INFLIGHT_MAX 3
#define TILEJSON    "https://tiles.openfreemap.org/planet"

static app_t *s_app;

/* ---------------------------------------------------------------------------
 * Places on the card
 * ------------------------------------------------------------------------- */

static void zones_path(char *out, size_t n)
{
    const char *d = mp_maps_dir();
    snprintf(out, n, "%s/zones.txt", d ? d : aos_hal_path_data());
}

/* zones.txt: one per line, tab-separated: name, latitude and longitude in
 * millionths of a degree, zoom in tenths. Lines starting with # are notes.
 * AmoledOS's portal page writes it; "Save this view" appends to it. */
void mp_zones_load(app_t *a)
{
    a->nzones = 0;
    char path[128];
    zones_path(path, sizeof path);
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    char *buf = (char *)malloc(8192);
    int n = buf ? (int)fread(buf, 1, 8191, fp) : 0;
    fclose(fp);
    if (!buf) return;
    buf[n] = 0;
    char *line = buf;
    while (*line && a->nzones < MAX_ZONES) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *f[4] = { line, NULL, NULL, NULL };
        for (int i = 1; i < 4; i++) {
            char *t = f[i - 1] ? strchr(f[i - 1], '\t') : NULL;
            if (t) {
                *t = 0;
                f[i] = t + 1;
            }
        }
        if (line[0] != '#' && f[1] && f[2]) {
            zone_t *z = &a->zones[a->nzones];
            snprintf(z->name, sizeof z->name, "%.31s", f[0]);
            char *cr = strchr(z->name, '\r');
            if (cr) *cr = 0;
            long la = strtol(f[1], NULL, 10), lo = strtol(f[2], NULL, 10);
            long zz = f[3] ? strtol(f[3], NULL, 10) : 150;
            if (z->name[0] && la >= -85000000 && la <= 85000000 && lo >= -180000000 && lo <= 180000000) {
                mp_lonlat_to_world((float)lo / 1e6f, (float)la / 1e6f, &z->cx, &z->cy);
                z->z = zz >= 20 && zz <= 185 ? (float)zz / 10.0f : 15.0f;
                a->nzones++;
            }
        }
        if (!end) break;
        line = end + 1;
    }
    free(buf);
}

bool mp_zone_append(app_t *a, const char *name)
{
    char path[128], line[96];
    zones_path(path, sizeof path);
    float lon, lat;
    mp_world_to_lonlat(a->view.cx, a->view.cy, &lon, &lat);
    FILE *fp = fopen(path, "ab");
    if (!fp) return false;
    int n = snprintf(line, sizeof line, "%s\t%ld\t%ld\t%d\n", name, (long)(lat * 1e6f),
                     (long)(lon * 1e6f), (int)(a->view.z * 10 + 0.5f));
    bool ok = fwrite(line, 1, (size_t)n, fp) == (size_t)n;
    ok = fclose(fp) == 0 && ok;
    return ok;
}

/* goto.txt: latitude, longitude (millionths) and zoom (tenths), tab-separated,
 * written by AmoledOS's /mapas page ("show it on the watch"; a P4OS portal
 * page would write the same). Read once and deleted. */
static void goto_check(app_t *a)
{
    const char *d = mp_maps_dir();
    if (!d) return;
    char path[128], buf[64];
    snprintf(path, sizeof path, "%s/goto.txt", d);
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    int n = (int)fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    remove(path);
    if (n <= 0) return;
    buf[n] = 0;
    char *p = buf, *e;
    long la = strtol(p, &e, 10);
    if (e == p) return;
    p = e;
    long lo = strtol(p, &e, 10);
    if (e == p) return;
    p = e;
    long zz = strtol(p, &e, 10);
    if (e == p) zz = 150;
    if (la < -85000000 || la > 85000000 || lo < -180000000 || lo > 180000000) return;
    mp_lonlat_to_world((float)lo / 1e6f, (float)la / 1e6f, &a->goto_cx, &a->goto_cy);
    a->goto_z = zz >= 20 && zz <= 185 ? (float)zz / 10.0f : 15.0f;
    a->goto_seq++;
    /* the page writes it after uploading a zone: the packs again, so the new
     * one is drawn without opening the menu */
    a->want_rescan = true;
    aos_hal_log("mapas", "goto %ld, %ld z%ld from the portal", la, lo, zz);
}

/* ---------------------------------------------------------------------------
 * The worker
 * ------------------------------------------------------------------------- */

static back_t *front_of(app_t *a, int q)
{
    int f = a->pair[q].front;
    return f < 0 ? NULL : &a->pair[q].b[f];
}

/* Whether b holds the whole screen for view v, 'slack' screen pixels to
 * spare all round, and at a zoom within dzmax of its own. */
static bool covers(const app_t *a, const back_t *b, const mp_view_t *v, float slack, float dzmax)
{
    if (!b || !b->px || b->geo != a->geo || b->w <= 0) return false;
    float dz = v->z - b->v.z;
    if (fabsf(dz) > dzmax) return false;
    float inv = b->res * exp2f(-dz);                /* buffer px per screen px */
    float s = mp_px_per_unit(b->v.z) * b->res;      /* buffer px per world unit */
    float bx = b->w * 0.5f + (float)(int32_t)(v->cx - b->v.cx) * s;
    float by = b->h * 0.5f + (float)(int32_t)(v->cy - b->v.cy) * s;
    float hw = (a->sw * 0.5f + slack) * inv, hh = (a->sh * 0.5f + slack) * inv;
    return bx - hw >= -0.01f && by - hh >= -0.01f && bx + hw <= b->w + 0.01f && by + hh <= b->h + 0.01f;
}

/* drawn before tiles it lacked arrived: worth drawing again */
static bool stale(const app_t *a, const back_t *b)
{
    return b && b->tgen != a->tgen && (b->missing || b->stand_in);
}

/* Which buffer the worker should draw next, or -1.
 *
 * Still: a FULL render of exactly this view, unless the one there is already
 * that. Moving: nothing while some buffer still has room around the screen;
 * then FULL if a FULL render would be finished before the map outruns its
 * margin at this speed (a slow pan stays sharp), else HALF. The first render
 * of all is HALF, the quickest way to something on the screen. */
static int want_render(app_t *a, const mp_view_t *v, bool moving)
{
    back_t *F = front_of(a, Q_FULL), *H = front_of(a, Q_HALF);
    if (F && F->geo != a->geo) F = NULL;
    if (H && H->geo != a->geo) H = NULL;
    if (!F && !H) return Q_HALF;
    if (!moving) {
        if (!covers(a, F, v, 0, 0.001f) || stale(a, F)) return Q_FULL;
        return -1;
    }
    if (F && covers(a, F, v, EDGE, 0.55f) && !stale(a, F)) return -1;
    float tf = a->t_full_us ? (float)a->t_full_us / 1e6f : 0.6f;
#ifdef AOS_SIM
    /* MAPAS_TFULL=ms: pretend a FULL render costs that, as on the board, to
     * see the HALF buffers the Mac is too fast to need */
    static int s_tfull = -1;
    if (s_tfull < 0) s_tfull = getenv("MAPAS_TFULL") ? atoi(getenv("MAPAS_TFULL")) : 0;
    if (s_tfull > 0) tf = (float)s_tfull / 1000.0f;
#endif
    bool slow = !a->zooming && a->speed * tf < FM * 0.5f;
    if (slow) return Q_FULL;
    if (H && covers(a, H, v, EDGE, 0.55f) && !stale(a, H)) return -1;
    return Q_HALF;
}

typedef struct {
    app_t  *a;
    back_t *b;
} abort_ctx_t;

/* A FULL render stops when the view has gone where it will not cover, or
 * while a zoom is under way (the next one will be at another zoom anyway).
 * A HALF one always finishes: it is what is shown next. */
static bool render_abort(void *arg)
{
    abort_ctx_t *c = (abort_ctx_t *)arg;
    app_t *a = c->a;
    if (aos_hal_worker_should_stop() || c->b->geo != a->geo) return true;
    if (c->b->res < 1.0f) return false;
    mp_view_t v = a->view;
    if (!covers(a, c->b, &v, 0, 0.55f)) return true;
    return a->zooming && fabsf(v.z - c->b->v.z) > 0.05f;
}

static void log_render(app_t *a, int q, const back_t *b, const mp_render_stats_t *st)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    /* every FULL one (they are few), a HALF one a second at most */
    if (q == Q_HALF && now - a->t_log < 1000 && fabsf(b->v.z - a->z_log) < 0.5f) return;
    a->z_log = b->v.z;
    a->t_log = now;
    uint32_t rb, hp, hc;
    int rt;
    mp_store_stats(&rb, &rt, &hp, &hc);
    /* the dearest classes */
    int top[3] = { -1, -1, -1 };
    for (int c = 0; c < MC_GEOM_END; c++) {
        for (int k = 0; k < 3; k++) {
            if (top[k] < 0 || st->us_cls[c] > st->us_cls[top[k]]) {
                for (int m = 2; m > k; m--) top[m] = top[m - 1];
                top[k] = c;
                break;
            }
        }
    }
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    aos_hal_log("mapas", "%s %dx%d z%.2f: %u ms = tiles %u + geometry %u + labels %u (glyphs %u); "
                "%d tiles, %d stand-ins, %d missing, %d labels, points %u -> %u; dearest %d:%u %d:%u %d:%u "
                "| ram %d tiles %u KB, pack %u, cache %u | internal %u, psram %u KB",
                q == Q_FULL ? "full" : "half", b->w, b->h, (double)b->v.z,
                (unsigned)(st->us_total / 1000), (unsigned)(st->us_tiles / 1000),
                (unsigned)(st->us_geom / 1000), (unsigned)(st->us_labels / 1000),
                (unsigned)(st->us_ldraw / 1000), st->shown, st->stand_in, st->missing, st->labels,
                (unsigned)st->pts_in, (unsigned)st->pts_out,
                top[0], (unsigned)(st->us_cls[top[0]] / 1000), top[1], (unsigned)(st->us_cls[top[1]] / 1000),
                top[2], (unsigned)(st->us_cls[top[2]] / 1000),
                rt, (unsigned)(rb / 1024), (unsigned)hp, (unsigned)hc, (unsigned)fi, (unsigned)(fp / 1024));
}

static void render_one(app_t *a, int q, const mp_view_t *v)
{
    pair_t *p = &a->pair[q];
    int bi = p->front < 0 ? 0 : 1 - p->front;
    back_t *b = &p->b[bi];
    while (a->composing == b && !aos_hal_worker_should_stop()) aos_hal_worker_sleep(1);
    int sw = a->sw, sh = a->sh;
    b->geo = a->geo;
    b->res = q == Q_FULL ? 1.0f : 0.5f;
    b->w = q == Q_FULL ? sw + 2 * FM : sw / 2 + 2 * LM;
    b->h = q == Q_FULL ? sh + 2 * FM : sh / 2 + 2 * LM;
    b->v = *v;
    b->tgen = a->tgen;

    int16_t rr[8][4];
    abort_ctx_t ac = { a, b };
    mp_render_opts_t o = {
        .res = b->res,
        .fonts = a->fonts[q],
        .reserve = (const int16_t (*)[4])rr,
        .nreserve = mp_frame_reserve(a, b, rr, 8),
        .abort = render_abort,
        .abort_arg = &ac,
    };
    mp_fb_t fb = { b->px, b->w, b->h, 0, 0, b->w, b->h };
    mp_render_stats_t st;
    bool done = mp_render(&fb, v, &o, &st);
    if (!done) {
        aos_hal_log("mapas", "%s render left after %u ms: the view moved on",
                    q == Q_FULL ? "full" : "half", (unsigned)(st.us_total / 1000));
        return;
    }
    b->missing = st.missing;
    b->stand_in = st.stand_in;
    __sync_synchronize();
    p->front = bi;
    a->front_gen++;

    volatile uint32_t *t = q == Q_FULL ? &a->t_full_us : &a->t_half_us;
    *t = *t ? (*t * 3 + st.us_total) / 4 : st.us_total;
    a->b_render_us[q] += st.us_total;
    a->b_renders[q]++;

    int n = st.missing < MP_MAX_MISSING ? st.missing : MP_MAX_MISSING;
    for (int i = 0; i < n; i++) a->want[i] = st.miss[i];
    a->nwant = n;
    a->missing = st.missing;
    log_render(a, q, b, &st);
}

/* MAPAS_RBENCH=1 (simulator, development): the same view rendered into
 * buffers of the watch's size, the HALF and the FULL ones, eight times
 * each, for what a render costs per pixel. */
static void render_bench(app_t *a)
{
    static const struct { const char *name; int w, h; float res; } B[] = {
        { "watch 560x640", 560, 640, 1.0f },
        { "half 616x896", AOS_PANEL_W / 2 + 2 * LM, AOS_PANEL_H / 2 + 2 * LM, 0.5f },
        { "full 912x1472", AOS_PANEL_W + 2 * FM, AOS_PANEL_H + 2 * FM, 1.0f },
    };
    mp_view_t v = a->view;
    back_t *b = &a->pair[Q_FULL].b[1];
    for (size_t k = 0; k < sizeof B / sizeof B[0]; k++) {
        uint32_t sum = 0, geo = 0, lab = 0;
        int labels = 0;
        for (int i = 0; i < 9; i++) {
            mp_render_opts_t o = { .res = B[k].res, .fonts = a->fonts[B[k].res < 1 ? Q_HALF : Q_FULL] };
            mp_fb_t fb = { b->px, B[k].w, B[k].h, 0, 0, B[k].w, B[k].h };
            mp_render_stats_t st;
            mp_render(&fb, &v, &o, &st);
            if (i == 0) continue;           /* the first one loads the tiles */
            sum += st.us_total;
            geo += st.us_geom;
            lab += st.us_labels;
            labels = st.labels;
        }
        float mpx = (float)B[k].w * B[k].h / 1e6f;
        aos_hal_log("mapas", "rbench z%.2f %s: %u us (geometry %u, labels %u, %d labels), %u us per Mpx",
                    (double)v.z, B[k].name, (unsigned)(sum / 8), (unsigned)(geo / 8), (unsigned)(lab / 8),
                    labels, (unsigned)(sum / 8 / mpx));
    }
}

static void worker(void *arg)
{
    app_t *a = (app_t *)arg;
    mp_store_init(a->ram_budget);
    mp_store_scan_packs();
    a->npacks = mp_store_packs(a->packs, MP_MAX_PACKS);
    a->packs_ready = true;
    goto_check(a);
    uint32_t goto_t = (uint32_t)aos_hal_uptime_ms();
    const char *rb = getenv("MAPAS_RBENCH");
    bool rbench = rb && rb[0] == '1';

    while (!aos_hal_worker_should_stop()) {
        uint32_t tn = (uint32_t)aos_hal_uptime_ms();
        if (tn - goto_t > 1500) {
            goto_t = tn;
            goto_check(a);
        }
        if (a->want_rescan) {
            a->want_rescan = false;
            mp_store_scan_packs();
            a->npacks = mp_store_packs(a->packs, MP_MAX_PACKS);
            a->packs_ready = true;
            a->tgen++;
        }
        if (a->want_clear) {
            a->want_clear = false;
            a->cleared = mp_store_clear_cache();
        }
        if (a->want_search) {
            /* searching goes up before want_search comes down, so LVGL's
             * side never sees both down while the hits are being written */
            a->searching = true;
            a->want_search = false;
            a->search_cancel = false;
            char key[sizeof a->skey];
            memcpy(key, a->skey, sizeof key);
            key[sizeof key - 1] = 0;
            uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
            int n = mp_search_files(key, a->whits, MAX_OFFLINE, &a->search_cancel);
            a->nwhits = n;
            aos_hal_log("mapas", "search \"%s\": %d offline in %u ms", key, n,
                        (unsigned)((uint32_t)aos_hal_uptime_ms() - t0));
            a->search_seq++;
            __sync_synchronize();
            a->searching = false;
        }

        /* the answers from the network */
        for (int i = 0; i < NQ; i++) {
            net_t *q = &a->q[i];
            if (q->state != Q_READY) continue;
            const char *body = q->empty ? NULL : aos_hal_http_body(q->id);
            int len = q->empty ? 0 : aos_hal_http_len(q->id);
            if (q->empty || body) {
                mp_store_put(q->k.z, q->k.x, q->k.y, (uint8_t *)body, len, true);
                a->tgen++;
            }
            q->state = Q_DONE;
        }

        mp_view_t v = a->view;
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        bool moving = a->touching || a->zooming || now - a->moved_ms < STILL_MS;
        int q = want_render(a, &v, moving);
        if (q < 0) {
            aos_hal_worker_sleep(8);
            continue;
        }
        render_one(a, q, &v);
        if (rbench && q == Q_FULL && a->goto_seen && fabsf(v.z - a->goto_z) < 0.01f) {
            rbench = false;
            render_bench(a);
        }
        aos_hal_worker_sleep(1);
    }
    mp_store_deinit();
}

/* ---------------------------------------------------------------------------
 * The network (LVGL's side)
 * ------------------------------------------------------------------------- */

static bool key_eq(const mp_key_t *a, const mp_key_t *b)
{
    return a->z == b->z && a->x == b->x && a->y == b->y;
}

/* "tiles":["https://.../{z}/{x}/{y}.pbf"] out of the TileJSON */
static bool tilejson_parse(app_t *a, const char *js)
{
    const char *p = strstr(js, "\"tiles\"");
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e || e - p >= (long)sizeof a->tpl || e - p < 20) return false;
    if (strncmp(p, "https://", 8) != 0 || !strstr(p, "{z}")) return false;
    memcpy(a->tpl, p, (size_t)(e - p));
    a->tpl[e - p] = 0;
    return true;
}

static bool tile_url(const app_t *a, const mp_key_t *k, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = a->tpl; *p && o + 12 < n; p++) {
        if (p[0] == '{' && p[2] == '}' && (p[1] == 'z' || p[1] == 'x' || p[1] == 'y')) {
            unsigned v = p[1] == 'z' ? k->z : p[1] == 'x' ? (unsigned)k->x : (unsigned)k->y;
            o += (size_t)snprintf(out + o, n - o, "%u", v);
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
    return o + 12 < n;
}

static bool failed_recently(app_t *a, const mp_key_t *k, uint32_t now)
{
    for (int i = 0; i < a->nfailed; i++)
        if (key_eq(&a->failed[i].k, k) && (int32_t)(a->failed[i].until - now) > 0) return true;
    for (int i = 0; i < 8; i++)
        if (key_eq(&a->recent[i].k, k) && now - a->recent[i].t < 3000) return true;
    return false;
}

static void failed_add(app_t *a, const mp_key_t *k, uint32_t until)
{
    int i = a->nfailed < MAX_FAILED ? a->nfailed++ : (int)(until % MAX_FAILED);
    a->failed[i].k = *k;
    a->failed[i].until = until;
}

int mp_inflight(const app_t *a)
{
    int n = 0;
    for (int i = 0; i < NQ; i++) n += a->q[i].state != Q_FREE;
    return n;
}

static void net_pump(app_t *a)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();

    /* give back what the worker has used */
    for (int i = 0; i < NQ; i++) {
        net_t *q = &a->q[i];
        if (q->state != Q_DONE) continue;
        if (q->id > 0) aos_hal_http_release(q->id);
        a->recent[a->recent_i].k = q->k;
        a->recent[a->recent_i].t = now;
        a->recent_i = (a->recent_i + 1) % 8;
        q->id = 0;
        q->state = Q_FREE;
        a->overlay_dirty = true;
    }
    /* what came in */
    for (int i = 0; i < NQ; i++) {
        net_t *q = &a->q[i];
        if (q->state != Q_FLIGHT) continue;
        aos_http_state_t hs = aos_hal_http_state(q->id);
        if (hs == AOS_HTTP_BUSY) continue;
        int code = aos_hal_http_status(q->id);
        int len = aos_hal_http_len(q->id);
        /* A download cut short still says 200 (HTTP/1.0 ends at the socket's
         * close): only a tile that parses to its end is kept. */
        bool whole = hs == AOS_HTTP_DONE && len < HTTP_MAX - 1 &&
                     (len == 0 || mp_mvt_complete((const uint8_t *)aos_hal_http_body(q->id), len));
        if (whole) {
            uint32_t ms = now - q->t0;
            aos_hal_log("mapas", "tile %d/%u/%u: %d KB in %u ms (%u KB/s), %d in flight",
                        q->k.z, (unsigned)q->k.x, (unsigned)q->k.y, len / 1024, (unsigned)ms,
                        (unsigned)(ms ? (uint32_t)len / ms : 0), mp_inflight(a));
            q->empty = code == 204 || len == 0;
            a->downloaded += (uint32_t)len;
            a->last_err = 0;
            q->state = Q_READY;
        } else if (hs == AOS_HTTP_FAILED && (code == 404 || code == 204)) {
            q->empty = true;
            q->state = Q_READY;
        } else {
            bool big = hs == AOS_HTTP_DONE && len >= HTTP_MAX - 1;
            aos_hal_log("mapas", "tile %d/%u/%u: %s %d, %d bytes", q->k.z, (unsigned)q->k.x,
                        (unsigned)q->k.y, big ? "too big" : hs == AOS_HTTP_DONE ? "cut short" : "failed",
                        code, len);
            a->last_err = hs == AOS_HTTP_DONE ? -100 : code;
            failed_add(a, &q->k, now + (big ? 600000 : code == AOS_HTTP_ERR_SIN_HORA ? 3000 : 6000));
            aos_hal_http_release(q->id);
            q->id = 0;
            q->state = Q_FREE;
            a->overlay_dirty = true;
        }
    }

    /* Awake while something is in flight, and a few seconds after, for the
     * next tile: with modem sleep every round trip cost the watch 200-300 ms.
     * On the P4 the radio does not nap on USB power anyway (MEMORY.md), but
     * on a battery it does. */
    bool busy = mp_inflight(a) > 0 || a->tpl_id > 0;
    if (busy) a->busy_ms = now;
    bool want_ll = busy || now - a->busy_ms < 4000;
    if (want_ll != a->low_latency) {
        a->low_latency = want_ll;
        aos_hal_net_low_latency(want_ll);
    }

    if (!a->online || aos_hal_net_state() != AOS_NET_CONNECTED) return;

    /* the template, once (and kept in a preference for next time) */
    if (!a->tpl[0]) {
        if (a->tpl_id > 0) {
            aos_http_state_t hs = aos_hal_http_state(a->tpl_id);
            if (hs == AOS_HTTP_BUSY) return;
            if (hs == AOS_HTTP_DONE && tilejson_parse(a, aos_hal_http_body(a->tpl_id))) {
                aos_hal_pref_set_str("map_tpl", a->tpl);
                aos_hal_log("mapas", "tiles: %s", a->tpl);
            } else {
                a->last_err = aos_hal_http_status(a->tpl_id);
                a->tpl_retry = now + (a->last_err == AOS_HTTP_ERR_SIN_HORA ? 3000 : 15000);
            }
            aos_hal_http_release(a->tpl_id);
            a->tpl_id = 0;
            a->overlay_dirty = true;
        } else if ((int32_t)(now - a->tpl_retry) >= 0) {
            a->tpl_id = aos_hal_http_get(TILEJSON, 64 * 1024);
            if (a->tpl_id <= 0) {
                a->tpl_id = 0;
                a->tpl_retry = now + 2000;
            }
        }
        if (!a->tpl[0]) return;
    }

    /* ask for what the last render lacked, nearest first */
    int n = a->nwant;
    for (int w = 0; w < n && mp_inflight(a) < a->max_inflight; w++) {
        mp_key_t k = a->want[w];
        bool queued = false;
        for (int i = 0; i < NQ; i++)
            if (a->q[i].state != Q_FREE && key_eq(&a->q[i].k, &k)) queued = true;
        if (queued || failed_recently(a, &k, now)) continue;
        int slot = -1;
        for (int i = 0; i < NQ; i++)
            if (a->q[i].state == Q_FREE) { slot = i; break; }
        if (slot < 0) break;
        char url[256];
        if (!tile_url(a, &k, url, sizeof url)) continue;
        int id = aos_hal_http_get(url, HTTP_MAX);
        if (id <= 0) break;             /* no slot in the HAL right now */
        a->q[slot].id = id;
        a->q[slot].k = k;
        a->q[slot].empty = false;
        a->q[slot].t0 = now;
        a->q[slot].state = Q_FLIGHT;
        a->overlay_dirty = true;
    }
}

/* ---------------------------------------------------------------------------
 * The view
 * ------------------------------------------------------------------------- */

static void view_moved(app_t *a)
{
    a->moved_ms = (uint32_t)aos_hal_uptime_ms();
    a->view_seq++;
}

static uint32_t clamp_y(int64_t y)
{
    if (y < 0) return 0;
    if (y > 0xFFFFFFFFll) return 0xFFFFFFFFu;
    return (uint32_t)y;
}

static float clampf(float v, float lim)
{
    return v > lim ? lim : v < -lim ? -lim : v;
}

void mp_view_pan(app_t *a, float dx, float dy)
{
    float s = mp_px_per_unit(a->view.z);
    float ux = clampf(-dx / s, 2.0e9f), uy = clampf(-dy / s, 2.0e9f);
    a->view.cx += (uint32_t)(int32_t)ux;
    a->view.cy = clamp_y((int64_t)a->view.cy + (int32_t)uy);
    view_moved(a);
}

/* zoom to z keeping the world point under screen (ax, ay) where it is */
static void view_zoom_at(app_t *a, float z, float ax, float ay)
{
    if (z < MP_ZMIN) z = MP_ZMIN;
    if (z > MP_ZMAX) z = MP_ZMAX;
    float s0 = mp_px_per_unit(a->view.z), s1 = mp_px_per_unit(z);
    float ox = ax - a->sw * 0.5f, oy = ay - a->sh * 0.5f;
    float ux = clampf(ox / s0 - ox / s1, 2.0e9f), uy = clampf(oy / s0 - oy / s1, 2.0e9f);
    a->view.cx += (uint32_t)(int32_t)ux;
    a->view.cy = clamp_y((int64_t)a->view.cy + (int32_t)uy);
    a->view.z = z;
    view_moved(a);
}

static void zoom_to(app_t *a, float z, float ax, float ay)
{
    if (z < MP_ZMIN) z = MP_ZMIN;
    if (z > MP_ZMAX) z = MP_ZMAX;
    a->zt = z;
    a->ax = ax;
    a->ay = ay;
    a->animating = true;
    a->zooming = true;
    a->vx = a->vy = 0;
}

void mp_go_to(app_t *a, uint32_t cx, uint32_t cy, float z)
{
    a->view.cx = cx;
    a->view.cy = cy;
    a->view.z = z < MP_ZMIN ? MP_ZMIN : z > MP_ZMAX ? MP_ZMAX : z;
    a->animating = false;
    a->zooming = a->pinching;
    a->vx = a->vy = 0;
    view_moved(a);
}

static void view_save(app_t *a)
{
    aos_hal_pref_set_i32("map_cx", (int32_t)a->view.cx);
    aos_hal_pref_set_i32("map_cy", (int32_t)a->view.cy);
    aos_hal_pref_set_i32("map_z", (int32_t)(a->view.z * 100));
}

static void view_load(app_t *a)
{
    int32_t cx, cy, z;
    if (aos_hal_pref_get_i32("map_cx", &cx) && aos_hal_pref_get_i32("map_cy", &cy) &&
        aos_hal_pref_get_i32("map_z", &z)) {
        mp_go_to(a, (uint32_t)cx, (uint32_t)cy, (float)z / 100.0f);
        return;
    }
    /* first time: where Clima is, else the Obelisco */
    int32_t la, lo;
    float lat = -34.6037f, lon = -58.3816f, zz = 12.5f;
    if (aos_hal_pref_get_i32("clima_lat", &la) && aos_hal_pref_get_i32("clima_lon", &lo)) {
        lat = (float)la / 1e4f;
        lon = (float)lo / 1e4f;
        zz = 13.0f;
    }
    uint32_t x, y;
    mp_lonlat_to_world(lon, lat, &x, &y);
    mp_go_to(a, x, y, zz);
}

/* How fast the map moves on the screen, for the worker's choice of buffer:
 * the centre's travel since the last sample, in screen pixels per second. */
static void speed_sample(app_t *a, uint32_t now)
{
    uint32_t dt = now - a->sp_ms;
    if (dt < 30) return;
    float s = mp_px_per_unit(a->view.z);
    float dx = (float)(int32_t)(a->view.cx - a->sp_cx) * s, dy = (float)(int32_t)(a->view.cy - a->sp_cy) * s;
    float v = sqrtf(dx * dx + dy * dy) * 1000.0f / (float)dt;
    if (dt > 400) v = 0;
    a->speed = a->speed * 0.5f + v * 0.5f;
    a->sp_ms = now;
    a->sp_cx = a->view.cx;
    a->sp_cy = a->view.cy;
}

/* ---------------------------------------------------------------------------
 * The bench: <card>/maps/bench.txt
 *
 * Taps injected from outside reach LVGL, not the gesture recogniser (so it
 * was on the watch), so a file on the card drives a scripted pan and zoom and the log times it. The
 * app deletes the file when done. "q=text" in it: the search to time (else
 * "serrano"); "r": the centre of Buenos Aires at z15, 15.5 and 16, rendered
 * six times each, for steady numbers per part of the render.
 * ------------------------------------------------------------------------- */

static void bench_done(app_t *a)
{
    char path[128];
    snprintf(path, sizeof path, "%s/bench.txt", mp_maps_dir() ? mp_maps_dir() : ".");
    remove(path);
    a->bench = false;
}

static void bench_static_step(app_t *a)
{
    static const float Z[] = { 15.0f, 15.5f, 16.0f };
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (a->bs_zoom >= 3) return;
    if (a->bs_count == 0 && now - a->bs_t > 100 && !a->bench_zoomed) {
        uint32_t x, y;
        mp_lonlat_to_world(-58.3816f, -34.6037f, &x, &y);
        mp_go_to(a, x, y, Z[a->bs_zoom]);
        a->bench_zoomed = true;
        a->bs_t = now;
        return;
    }
    if (a->bs_count == 0 && (a->missing || mp_inflight(a) || now - a->bs_t < 2500)) return;
    if (now - a->bs_t < 1800) return;
    a->bs_t = now;
    if (a->bs_count++ < 6) {
        a->tgen++;                      /* the worker renders the same view again */
        back_t *F = front_of(a, Q_FULL);
        if (F) F->missing = 1;          /* ...by making it stale */
        return;
    }
    a->bs_count = 0;
    a->bench_zoomed = false;
    if (++a->bs_zoom >= 3) {
        bench_done(a);
        aos_hal_log("mapas", "bench static: done");
    }
}

/* Wait for the data, then pan, zoom in, pan, zoom out and a slow continuous
 * zoom like a pinch, logging each phase. */
static void bench_step(app_t *a)
{
    static const char *const NAME[] = { "wait", "pan", "zoom in", "pan z+2", "zoom out", "pinch",
                                        "search", "end" };
    uint32_t now = (uint32_t)aos_hal_uptime_ms(), el = now - a->bench_t;
    float dt = TICK_MS / 1000.0f, cx = a->sw * 0.5f, cy = a->sh * 0.5f;
    bool next = false;
    switch (a->bench_phase) {
    case 0: next = (el > 2000 && a->missing == 0 && !mp_inflight(a)) || el > 30000; break;
    case 1: a->touching = true; mp_view_pan(a, -420 * dt, 0); next = el > 3000; break;
    case 2: a->touching = false; if (!a->bench_zoomed) zoom_to(a, a->view.z + 2, cx, cy); a->bench_zoomed = true; next = el > 4000; break;
    case 3: a->touching = true; mp_view_pan(a, -300 * dt, -200 * dt); next = el > 3000; break;
    case 4: a->touching = false; if (!a->bench_zoomed) zoom_to(a, a->view.z - 3, cx, cy); a->bench_zoomed = true; next = el > 4000; break;
    case 5: a->zooming = true; view_zoom_at(a, a->view.z + 0.8f * dt, cx, cy); next = el > 2500; break;
    case 6:
        a->zooming = false;
        if (!a->bench_zoomed) {
            mp_show_search(a);
            if (a->ta) lv_textarea_set_text(a->ta, a->bench_q);
            if (a->kb) lv_obj_send_event(a->kb, LV_EVENT_READY, NULL);
            a->bench_zoomed = true;
        }
        next = (a->search_seq != 0 && !a->searching && a->photon_id == 0 && (a->photon_done || !a->online)) ||
               el > 20000;
        if (next) {
            aos_hal_log("mapas", "bench search \"%s\": %d hits, online %s", a->query, a->nhits,
                        a->photon_done ? "answered" : "no");
            mp_show_map(a);
        }
        break;
    default: return;
    }
    if (!next) return;
    if (a->bench_phase > 0) {
        uint32_t f = a->b_frames ? a->b_frames : 1;
        uint32_t rf = a->b_renders[Q_FULL] ? a->b_renders[Q_FULL] : 1, rh = a->b_renders[Q_HALF] ? a->b_renders[Q_HALF] : 1;
        aos_hal_log("mapas", "bench %s: %u frames in %u ms (%u fps), compose %u us + present %u us, %u flips, "
                    "%u blits, %u through LVGL, %u again after LVGL; renders full %u x %u ms, half %u x %u ms; "
                    "z%.2f, %d missing",
                    NAME[a->bench_phase], (unsigned)a->b_frames, (unsigned)el,
                    (unsigned)(a->b_frames * 1000 / (el ? el : 1)), (unsigned)(a->b_compose / f),
                    (unsigned)(a->b_present / f), (unsigned)a->b_flips, (unsigned)a->b_blits, (unsigned)a->b_lvgl,
                    (unsigned)a->b_retouch,
                    (unsigned)a->b_renders[Q_FULL], (unsigned)(a->b_render_us[Q_FULL] / rf / 1000),
                    (unsigned)a->b_renders[Q_HALF], (unsigned)(a->b_render_us[Q_HALF] / rh / 1000),
                    (double)a->view.z, a->missing);
    }
    a->bench_phase++;
    a->bench_zoomed = false;
    a->bench_t = now;
    a->b_frames = a->b_compose = a->b_present = a->b_blits = a->b_lvgl = a->b_flips = a->b_retouch = 0;
    for (int q = 0; q < Q_N; q++) a->b_renders[q] = a->b_render_us[q] = 0;
    if (a->bench_phase == 7) bench_done(a);
}

static void bench_check(app_t *a)
{
    if (!mp_maps_dir()) return;
    char path[128];
    snprintf(path, sizeof path, "%s/bench.txt", mp_maps_dir());
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    char b[48] = "";
    size_t r = fread(b, 1, sizeof b - 1, fp);
    b[r] = 0;
    fclose(fp);
    char *nl = strpbrk(b, "\r\n");
    if (nl) *nl = 0;
    snprintf(a->bench_q, sizeof a->bench_q, "%.39s", !strncmp(b, "q=", 2) && b[2] ? b + 2 : "serrano");
    a->bench_static = b[0] == 'r';
    a->bench = true;
    a->bench_t = (uint32_t)aos_hal_uptime_ms();
    /* always the same place: the centre of Buenos Aires at z13 */
    uint32_t bx, by;
    mp_lonlat_to_world(-58.3816f, -34.6037f, &bx, &by);
    mp_go_to(a, bx, by, 13.0f);
    aos_hal_log("mapas", "bench.txt found: timing a scripted pan and zoom");
}

/* ---------------------------------------------------------------------------
 * The frame timer
 * ------------------------------------------------------------------------- */

static void tick(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    float dt = (float)(now - a->last_tick) / 1000.0f;
    if (dt <= 0 || dt > 0.1f) dt = TICK_MS / 1000.0f;
    a->last_tick = now;

    net_pump(a);
    mp_search_pump(a);
    if (a->goto_seq != a->goto_seen) {
        a->goto_seen = a->goto_seq;
        mp_go_to(a, a->goto_cx, a->goto_cy, a->goto_z);
        if (!a->viewing) mp_show_map(a);
    }
    if (a->bench) {
        if (a->bench_static) bench_static_step(a);
        else bench_step(a);
    }
    if (!a->viewing || a->hidden) return;

    /* What is over the map, read here in LVGL's task (mp_frame_push goes by
     * it). Anything but a toast: the zoom and the flick wait under it. When
     * it comes, a frame through LVGL if its canvas is behind the screen (so
     * that what LVGL draws under it is the map as it is); when it goes, a
     * whole frame to the panel. */
    uint32_t ov = aos_ui_overlay();
    if (ov != a->over_bits) {
        if (!ov || (!a->over_bits && !a->frame_synced)) a->overlay_dirty = true;
        aos_hal_log("mapas", ov ? "overlay 0x%02x over the map: frames through LVGL%s"
                                : "overlay gone (was 0x%02x): frames to the panel%s",
                    (unsigned)(ov ? ov : a->over_bits),
                    ov & ~(uint32_t)AOS_UI_OVER_TOAST ? ", the map waits" : "");
        a->over_bits = ov;
    }
    bool wait = (ov & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;

    if (wait) {
        /* the zoom and the flick go on when it has gone */
    } else if (a->animating) {
        float d = a->zt - a->view.z;
        if (fabsf(d) < 0.01f) {
            view_zoom_at(a, a->zt, a->ax, a->ay);
            a->animating = false;
            a->zooming = a->pinching;
        } else {
            /* the same ease per second whatever the frame rate */
            view_zoom_at(a, a->view.z + d * (1.0f - expf(-dt * 20.0f)), a->ax, a->ay);
        }
    } else if (!a->touching && (fabsf(a->vx) > 20 || fabsf(a->vy) > 20)) {
        /* the flick: the speed it was let go with, fading (0.92 a 16 ms
         * frame on the watch, the same per second here) */
        mp_view_pan(a, a->vx * dt, a->vy * dt);
        float k = expf(-dt * 5.2f);
        a->vx *= k;
        a->vy *= k;
    }
    speed_sample(a, now);

    /* a few frames after opening, while the runtime's curtain is still over
     * the app: nothing blitted under it */
    if (now - a->open_ms < 350) return;
    if (a->view_seq != a->shown_seq || a->front_gen != a->shown_gen || a->overlay_dirty) {
        a->shown_seq = a->view_seq;
        a->shown_gen = a->front_gen;
        a->overlay_dirty = false;
        mp_frame_push(a);
    } else if (mp_frame_sync(a, now)) {
        a->overlay_dirty = true;            /* LVGL painted over it: the next tick */
    }
}

/* ---------------------------------------------------------------------------
 * Touch
 * ------------------------------------------------------------------------- */

static bool buttons(app_t *a, float x, float y)
{
    const lay_t *L = &a->L;
    if (mp_hit_disc(&L->zin, x, y)) {
        zoom_to(a, floorf((a->animating ? a->zt : a->view.z) + 1.0f + 0.01f), a->sw * 0.5f, a->sh * 0.5f);
        return true;
    }
    if (mp_hit_disc(&L->zout, x, y)) {
        zoom_to(a, ceilf((a->animating ? a->zt : a->view.z) - 1.0f - 0.01f), a->sw * 0.5f, a->sh * 0.5f);
        return true;
    }
    if (mp_hit_disc(&L->menu, x, y)) {
        mp_show_list(a);
        return true;
    }
    if (mp_hit_pill(a, x, y)) {
        /* the cross at the end of the pill, with a pin up, takes the pin off */
        if (a->pin_on && x > L->px1 - (L->py1 - L->py0)) {
            a->pin_on = false;
            a->overlay_dirty = true;
        } else {
            mp_show_search(a);
        }
        return true;
    }
    return false;
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    app_t *a = (app_t *)user;
    if (!a->viewing) return;
    switch (ev->type) {
    case AOS_GESTURE_DRAG_BEGIN:
        a->touching = true;
        a->animating = false;
        a->zooming = false;
        a->vx = a->vy = 0;
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        a->touching = true;
        a->animating = false;
        a->pinching = true;
        a->zooming = true;
        a->vx = a->vy = 0;
        break;
    case AOS_GESTURE_DRAG:
        mp_view_pan(a, ev->dx, ev->dy);
        break;
    case AOS_GESTURE_DRAG_END:
        a->touching = false;
        a->vx = ev->vx;
        a->vy = ev->vy;
        break;
    case AOS_GESTURE_PINCH:
        view_zoom_at(a, a->view.z + log2f(ev->scale), ev->x, ev->y);
        mp_view_pan(a, ev->dx, ev->dy);
        break;
    case AOS_GESTURE_PINCH_END:
        a->touching = false;
        a->pinching = false;
        a->zooming = a->animating;
        break;
    case AOS_GESTURE_TAP:
        buttons(a, ev->x, ev->y);
        break;
    case AOS_GESTURE_DOUBLE_TAP:
        /* FAST_TAP: a double tap on a button is its second press */
        if (!buttons(a, ev->x, ev->y)) zoom_to(a, floorf(a->view.z + 1.0f + 0.01f), ev->x, ev->y);
        break;
    case AOS_GESTURE_LONG_PRESS: {
        float s = mp_px_per_unit(a->view.z);
        uint32_t x = a->view.cx + (uint32_t)(int32_t)((ev->x - a->sw * 0.5f) / s);
        uint32_t y = clamp_y((int64_t)a->view.cy + (int32_t)((ev->y - a->sh * 0.5f) / s));
        float lon, lat;
        mp_world_to_lonlat(x, y, &lon, &lat);
        char txt[48];
        snprintf(txt, sizeof txt, "%.5f, %.5f", (double)lat, (double)lon);
        a->pin_on = true;
        a->pin_cx = x;
        a->pin_cy = y;
        snprintf(a->pin_name, sizeof a->pin_name, "%s", txt);
        a->overlay_dirty = true;
        aos_ui_toast(txt, 3000);
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Life cycle
 * ------------------------------------------------------------------------- */

static void mp_destroy(aos_app_t *self, void *inst);

/* The shape of the screen: the map's objects follow it, and the buffers are
 * told (a new geo makes the worker drop what it drew for the old one). */
static void apply_shape(app_t *a)
{
    int sw = AOS_SCREEN_W, sh = AOS_SCREEN_H;
    bool changed = sw != a->sw || sh != a->sh;
    a->sw = sw;
    a->sh = sh;
    if (changed) {
        __sync_synchronize();
        a->geo++;
    }
    mp_layout(a);
    lv_obj_set_size(a->map, sw, sh);
    lv_obj_set_size(a->touch, sw, sh);
    if (a->canvas) {
        lv_canvas_set_buffer(a->canvas, a->frame, sw, sh, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(a->canvas, sw, sh);
    }
    a->overlay_dirty = true;
}

static bool fonts_bake(app_t *a)
{
    /* the labels a touch bigger than the watch's 14/16/20: the same pixel
     * density, but a bigger screen held further away */
    static const lv_font_t *const F[MP_FONTS] = { &aos_inter_16, &aos_inter_20, &aos_inter_24 };
    bool ok = true;
    for (int i = 0; i < MP_FONTS; i++) {
        ok &= mp_font_bake(&a->fonts[Q_FULL][i], F[i]);
        ok &= mp_font_half(&a->fonts[Q_HALF][i], &a->fonts[Q_FULL][i]);
    }
    return ok;
}

static void *mp_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)mp_calloc(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    for (int q = 0; q < Q_N; q++) a->pair[q].front = -1;
    a->cleared = -1;
    a->open_ms = a->last_tick = (uint32_t)aos_hal_uptime_ms();
    s_app = a;
    lv_obj_set_style_bg_color(root, lv_color_hex(MP_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        a->pair[Q_FULL].b[i].px = (uint16_t *)mp_malloc(CAP_FULL * 2);
        a->pair[Q_HALF].b[i].px = (uint16_t *)mp_malloc(CAP_HALF * 2);
        ok &= a->pair[Q_FULL].b[i].px && a->pair[Q_HALF].b[i].px;
    }
    /* the frame: what the DMA2D reads for the panel and the canvas shows */
    a->frame = (uint16_t *)aos_hal_io_alloc(CAP_FRAME * 2);
    ok &= a->frame != NULL;
    if (!ok) {
        mp_destroy(self, a);
        aos_ui_toast(_("Sin memoria"), 2000);
        return NULL;
    }
    for (size_t i = 0; i < CAP_FRAME; i++) a->frame[i] = mp_565(MP_BG);
    /* what is left for decoded tiles: a third of the free PSRAM, capped */
    uint32_t li = 0, left = 0;
    aos_hal_heap_info(&li, &left);
    a->ram_budget = left / 3;
    if (a->ram_budget > RAM_BUDGET_MAX) a->ram_budget = RAM_BUDGET_MAX;
    if (a->ram_budget < RAM_BUDGET_MIN) a->ram_budget = RAM_BUDGET_MIN;

    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    fonts_bake(a);
    aos_hal_log("mapas", "opening: psram %u KB free, buffers %u KB, tiles %u KB; fonts baked in %u ms",
                (unsigned)(fp / 1024), (unsigned)((CAP_FULL * 4 + CAP_HALF * 4 + CAP_FRAME * 2) / 1024),
                (unsigned)(a->ram_budget / 1024), (unsigned)((uint32_t)aos_hal_uptime_ms() - t0));

    int32_t on = 1, blit = 1, inflight = INFLIGHT_DEF;
    aos_hal_pref_get_i32("map_online", &on);
    aos_hal_pref_get_i32("map_blit", &blit);
    aos_hal_pref_get_i32("map_inflight", &inflight);
    a->online = on != 0;
#ifdef AOS_SIM
    /* the simulator has the Mac's network: MAPAS_OFFLINE=1 keeps it off the
     * internet whatever the preference says (the tests use the card's packs) */
    const char *off = getenv("MAPAS_OFFLINE");
    if (off && off[0] == '1') a->online = false;
#endif
    a->use_blit = blit != 0;
    a->max_inflight = inflight < 1 ? 1 : inflight > INFLIGHT_MAX ? INFLIGHT_MAX : (int)inflight;
    aos_hal_pref_get_str("map_tpl", a->tpl, sizeof a->tpl);

    a->map = lv_obj_create(root);
    lv_obj_remove_style_all(a->map);
    lv_obj_set_pos(a->map, 0, 0);
    lv_obj_remove_flag(a->map, LV_OBJ_FLAG_SCROLLABLE);
    /* The frame is also an LVGL canvas, on the board too: whenever LVGL
     * redraws part of the screen for its own reasons (a toast, a banner, a
     * panel closing), what it draws under them is the frame and not black. */
    a->canvas = lv_canvas_create(a->map);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    a->touch = lv_obj_create(a->map);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_pos(a->touch, 0, 0);
    aos_gesture_attach(a->touch, AOS_GESTURE_FLAG_FAST_TAP, gesture_cb, a);
    apply_shape(a);

    view_load(a);
    mp_zones_load(a);
    bench_check(a);
    a->sp_ms = a->open_ms;
    a->sp_cx = a->view.cx;
    a->sp_cy = a->view.cy;

    a->viewing = true;
    a->screen = SCR_MAP;
    a->self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    if (!aos_hal_worker_start_on("mapas", worker, a, WORKER_STACK, 0, 3)) {
        aos_hal_log("mapas", "no worker");
    }
    a->timer = lv_timer_create(tick, TICK_MS, a);

    /* development switches (the board's getenv() is always NULL): MAPAS_Q=text
     * opens the results of that search, MAPAS_PICK=1 then picks the first,
     * MAPAS_SCREEN=list or search opens the menu or the search */
    const char *scr = getenv("MAPAS_SCREEN");
    if (scr && !strcmp(scr, "list")) mp_show_list(a);
    else if (scr && !strcmp(scr, "search")) mp_show_search(a);
    const char *q = getenv("MAPAS_Q");
    if (q && q[0]) {
        mp_show_search(a);
        if (a->ta) lv_textarea_set_text(a->ta, q);
        if (a->kb) lv_obj_send_event(a->kb, LV_EVENT_READY, NULL);
    }
    return a;
}

static void mp_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->search_cancel = true;
    aos_hal_worker_stop();
    for (int i = 0; i < NQ; i++)
        if (a->q[i].state != Q_FREE && a->q[i].id > 0) aos_hal_http_release(a->q[i].id);
    if (a->tpl_id > 0) aos_hal_http_release(a->tpl_id);
    if (a->photon_id > 0) aos_hal_http_release(a->photon_id);
    if (a->low_latency) aos_hal_net_low_latency(false);
    if (a->front_gen) view_save(a);
    for (int q = 0; q < Q_N; q++)
        for (int i = 0; i < MP_FONTS; i++) mp_font_free(&a->fonts[q][i]);
    if (self && self->root) lv_obj_clean(self->root);   /* the canvas uses the frame */
    for (int i = 0; i < 2; i++) {
        mp_free(a->pair[Q_FULL].b[i].px);
        mp_free(a->pair[Q_HALF].b[i].px);
    }
    aos_hal_io_free(a->frame);
    s_app = NULL;
    mp_free(a);
}

static bool mp_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    if (a->screen != SCR_MAP) {
        a->search_cancel = true;
        mp_show_map(a);
        return true;
    }
    return false;
}

static bool mp_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    a->root = root;
    apply_shape(a);
    if (a->screen != SCR_MAP) mp_ui_relayout(a);
    return true;
}

static void mp_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) a->hidden = true;        /* nothing blitted over whatever is in front */
}

static void mp_show(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    a->hidden = false;
    a->overlay_dirty = true;
}

/* A folded map: three panels and a pin. */
static const uint8_t MP_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -22, 2, 24, 64, 4, AIC_C_LIT(0x3E8E5A), 255),
    AIC_RECT(AIC_CENTER, 0, -2, 24, 64, 4, AIC_C_LIT(0x2F6FB0), 255),
    AIC_RECT(AIC_CENTER, 22, 2, 24, 64, 4, AIC_C_LIT(0x3E8E5A), 255),
    AIC_RECT(AIC_CENTER, 0, -14, 26, 26, AIC_CIRCLE, AIC_C_LIT(0xFF5A4E), 255),
    AIC_RECT(AIC_CENTER, 0, -14, 10, 10, AIC_CIRCLE, AIC_C_LIT(0xFFFFFF), 255),
    AIC_END
};

static bool mp_init(aos_app_t *app)
{
    app->desc.id       = "demo.mapas";
    app->desc.name     = "Mapas";
    app->desc.icon     = LV_SYMBOL_GPS;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, MP_ICON, sizeof MP_ICON);
    app->desc.color_a  = 0x1F5C3A;
    app->desc.color_b  = 0x0B2418;
    app->desc.order    = 164;
    /* both orientations: resize() reshapes the buffers in place */
    app->desc.flags    = AOS_APP_FLAG_FULLSCREEN;

    app->create  = mp_create;
    app->destroy = mp_destroy;
    app->back    = mp_back;
    app->resize  = mp_resize;
    app->hide    = mp_hide;
    app->show    = mp_show;
    return true;
}

AOS_APP_ENTRY(mp_init);
