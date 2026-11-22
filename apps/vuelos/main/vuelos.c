/*
 * P4OS - VUELOS: a flight radar. A place on the map and the aircraft around
 * it, live.
 *
 *   one finger ....... move the map; a flick keeps it going
 *   two fingers ...... zoom about the point between them
 *   double tap ....... a level closer, there
 *   tap an aircraft .. its card: flight, registration, type, altitude,
 *                      speeds, squawk, distance, and the route if known
 *   long press ....... the place goes there
 *   + / - ............ a level in or out
 *   the house ........ back to the place
 *   the list ......... the aircraft by distance or altitude
 *   menu ............. the settings
 *
 * The map is Mapas' (vl_render.c and the files around it, copied): the
 * worker (core 0) renders OpenFreeMap's vector tiles into buffers bigger
 * than the screen, in a muted style, and every frame is cut out of the best
 * of them on LVGL's side, with the aircraft drawn on top (vl_frame.c). An
 * aircraft moves between two readings by its speed and track, so frames go
 * up while any is on the screen, as often as its movement shows.
 *
 * The aircraft come from one of four sources (vl_feed.c): airplanes.live,
 * adsb.lol, the OpenSky Network, or a receiver of one's own on the network
 * (readsb or dump1090's aircraft.json). The settings are a file on the card
 * that the portal page writes too (vl_conf.c).
 */
#include "vuelos.h"

#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_gesture.h"
#include "aos_fonts.h"

#include "vl_mem.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_STACK (24 * 1024)
#define EDGE        16
#define RAM_BUDGET_MAX  (8u * 1024u * 1024u)
#define RAM_BUDGET_MIN  (2u * 1024u * 1024u)
/* A tile's ceiling: the densest z14 seen was 0.5 MB. The HAL sets the
 * whole of it aside in one piece before it asks, and with this app's
 * buffers the board seldom has a megabyte in one piece: net_pump() asks
 * with half and with a quarter when it does not (most tiles are under
 * 200 KB), and a tile that does not fit its ceiling is "too big". */
#define HTTP_MAX    (1100 * 1024)
#define HTTP_MAX_LEAST (256 * 1024)
#define TILEJSON    "https://tiles.openfreemap.org/planet"
#define PICK_R      56                  /* px around an aircraft that pick it */

/* ---------------------------------------------------------------------------
 * The worker: the map's renders (Mapas' own, without the search and zones)
 * ------------------------------------------------------------------------- */

static back_t *front_of(app_t *a, int q)
{
    int f = a->pair[q].front;
    return f < 0 ? NULL : &a->pair[q].b[f];
}

static bool covers(const app_t *a, const back_t *b, const vl_view_t *v, float slack, float dzmax)
{
    if (!b || !b->px || b->geo != a->geo || b->w <= 0) return false;
    float dz = v->z - b->v.z;
    if (fabsf(dz) > dzmax) return false;
    float inv = b->res * exp2f(-dz);
    float s = vl_px_per_unit(b->v.z) * b->res;
    float bx = b->w * 0.5f + (float)(int32_t)(v->cx - b->v.cx) * s;
    float by = b->h * 0.5f + (float)(int32_t)(v->cy - b->v.cy) * s;
    float hw = (a->sw * 0.5f + slack) * inv, hh = (a->sh * 0.5f + slack) * inv;
    return bx - hw >= -0.01f && by - hh >= -0.01f && bx + hw <= b->w + 0.01f && by + hh <= b->h + 0.01f;
}

static bool stale(const app_t *a, const back_t *b)
{
    return b && b->tgen != a->tgen && (b->missing || b->stand_in);
}

static int want_render(app_t *a, const vl_view_t *v, bool moving)
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
    bool slow = !a->zooming && a->speed * tf < FM * 0.5f;
    if (slow) return Q_FULL;
    if (H && covers(a, H, v, EDGE, 0.55f) && !stale(a, H)) return -1;
    return Q_HALF;
}

typedef struct {
    app_t  *a;
    back_t *b;
} abort_ctx_t;

static bool render_abort(void *arg)
{
    abort_ctx_t *c = (abort_ctx_t *)arg;
    app_t *a = c->a;
    if (aos_hal_worker_should_stop() || c->b->geo != a->geo) return true;
    if (c->b->res < 1.0f) return false;
    vl_view_t v = a->view;
    if (!covers(a, c->b, &v, 0, 0.55f)) return true;
    return a->zooming && fabsf(v.z - c->b->v.z) > 0.05f;
}

static void log_render(app_t *a, int q, const back_t *b, const vl_render_stats_t *st)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (q == Q_HALF && now - a->t_log < 1000 && fabsf(b->v.z - a->z_log) < 0.5f) return;
    a->z_log = b->v.z;
    a->t_log = now;
    aos_hal_log("vuelos", "%s %dx%d z%.2f: %u ms (tiles %u, geometry %u, labels %u); %d tiles, %d stand-ins, %d missing",
                q == Q_FULL ? "full" : "half", b->w, b->h, (double)b->v.z, (unsigned)(st->us_total / 1000),
                (unsigned)(st->us_tiles / 1000), (unsigned)(st->us_geom / 1000), (unsigned)(st->us_labels / 1000),
                st->shown, st->stand_in, st->missing);
}

static void render_one(app_t *a, int q, const vl_view_t *v)
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
    vl_render_opts_t o = {
        .res = b->res,
        .fonts = a->fonts[q],
        .reserve = (const int16_t (*)[4])rr,
        .nreserve = vl_frame_reserve(a, b, rr, 8),
        .abort = render_abort,
        .abort_arg = &ac,
    };
    vl_fb_t fb = { b->px, b->w, b->h, 0, 0, b->w, b->h };
    vl_render_stats_t st;
    if (!vl_render(&fb, v, &o, &st)) return;
    b->missing = st.missing;
    b->stand_in = st.stand_in;
    __sync_synchronize();
    p->front = bi;
    a->front_gen++;
    volatile uint32_t *t = q == Q_FULL ? &a->t_full_us : &a->t_half_us;
    *t = *t ? (*t * 3 + st.us_total) / 4 : st.us_total;
    int n = st.missing < VL_MAX_MISSING ? st.missing : VL_MAX_MISSING;
    for (int i = 0; i < n; i++) a->want[i] = st.miss[i];
    a->nwant = n;
    a->missing = st.missing;
    log_render(a, q, b, &st);
}

static void worker(void *arg)
{
    app_t *a = (app_t *)arg;
    vl_store_init(a->ram_budget);
    vl_store_scan_packs();      /* Mapas' offline zones, read here as they are */
    while (!aos_hal_worker_should_stop()) {
        for (int i = 0; i < NQ; i++) {
            net_t *q = &a->q[i];
            if (q->state != Q_READY) continue;
            const char *body = q->empty ? NULL : aos_hal_http_body(q->id);
            int len = q->empty ? 0 : aos_hal_http_len(q->id);
            if (q->empty || body) {
                vl_store_put(q->k.z, q->k.x, q->k.y, (uint8_t *)body, len, true);
                a->tgen++;
            }
            q->state = Q_DONE;
        }
        vl_view_t v = a->view;
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        bool moving = a->touching || a->zooming || now - a->moved_ms < STILL_MS;
        int q = want_render(a, &v, moving);
        if (q < 0) {
            aos_hal_worker_sleep(8);
            continue;
        }
        render_one(a, q, &v);
        aos_hal_worker_sleep(1);
    }
    vl_store_deinit();
}

/* ---------------------------------------------------------------------------
 * The tiles' network (LVGL's side), as in Mapas
 * ------------------------------------------------------------------------- */

static bool key_eq(const vl_key_t *a, const vl_key_t *b)
{
    return a->z == b->z && a->x == b->x && a->y == b->y;
}

static bool tilejson_parse(app_t *a, const char *js)
{
    const char *p = js ? strstr(js, "\"tiles\"") : NULL;
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

static bool tile_url(const app_t *a, const vl_key_t *k, char *out, size_t n)
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

static bool failed_recently(app_t *a, const vl_key_t *k, uint32_t now)
{
    for (int i = 0; i < a->nfailed; i++)
        if (key_eq(&a->failed[i].k, k) && (int32_t)(a->failed[i].until - now) > 0) return true;
    for (int i = 0; i < 8; i++)
        if (key_eq(&a->recent[i].k, k) && now - a->recent[i].t < 3000) return true;
    return false;
}

static void failed_add(app_t *a, const vl_key_t *k, uint32_t until)
{
    int i = a->nfailed < MAX_FAILED ? a->nfailed++ : (int)(until % MAX_FAILED);
    a->failed[i].k = *k;
    a->failed[i].until = until;
}

int vl_inflight(const app_t *a)
{
    int n = 0;
    for (int i = 0; i < NQ; i++) n += a->q[i].state != Q_FREE;
    return n;
}

static void net_pump(app_t *a)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
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
    for (int i = 0; i < NQ; i++) {
        net_t *q = &a->q[i];
        if (q->state != Q_FLIGHT) continue;
        aos_http_state_t hs = aos_hal_http_state(q->id);
        if (hs == AOS_HTTP_BUSY) continue;
        int code = aos_hal_http_status(q->id);
        int len = aos_hal_http_len(q->id);
        bool whole = hs == AOS_HTTP_DONE && len < q->max - 1 &&
                     (len == 0 || vl_mvt_complete((const uint8_t *)aos_hal_http_body(q->id), len));
        if (whole) {
            q->empty = code == 204 || len == 0;
            a->last_err = 0;
            q->state = Q_READY;
        } else if (hs == AOS_HTTP_FAILED && (code == 404 || code == 204)) {
            q->empty = true;
            q->state = Q_READY;
        } else {
            /* too big for a ceiling that was cut for want of memory: again
             * soon, when there may be more */
            bool big = hs == AOS_HTTP_DONE && len >= q->max - 1;
            aos_hal_log("vuelos", "tile %d/%u/%u: %s %d", q->k.z, (unsigned)q->k.x, (unsigned)q->k.y,
                        big ? "too big" : hs == AOS_HTTP_DONE ? "cut short" : "failed", big ? q->max / 1024 : code);
            a->last_err = hs == AOS_HTTP_DONE ? -100 : code;
            failed_add(a, &q->k, now + (big ? (q->max < HTTP_MAX ? 60000 : 600000)
                                            : code == AOS_HTTP_ERR_SIN_HORA ? 3000 : 6000));
            aos_hal_http_release(q->id);
            q->id = 0;
            q->state = Q_FREE;
            a->overlay_dirty = true;
        }
    }

    bool busy = vl_inflight(a) > 0 || a->tpl_id > 0 || a->feed_id > 0;
    if (busy) a->busy_ms = now;
    bool want_ll = busy || now - a->busy_ms < 4000;
    if (want_ll != a->low_latency) {
        a->low_latency = want_ll;
        aos_hal_net_low_latency(want_ll);
    }

    if (!a->online || aos_hal_net_state() != AOS_NET_CONNECTED) return;

    if (!a->tpl[0]) {
        if (a->tpl_id > 0) {
            aos_http_state_t hs = aos_hal_http_state(a->tpl_id);
            if (hs == AOS_HTTP_BUSY) return;
            if (hs == AOS_HTTP_DONE && tilejson_parse(a, aos_hal_http_body(a->tpl_id))) {
                aos_hal_pref_set_str("vl_tpl", a->tpl);
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

    /* One tile at a time, as Mapas measured; the HAL has three slots in all,
     * and the aircraft and the routes want theirs. */
    int n = a->nwant;
    for (int w = 0; w < n && vl_inflight(a) < 1; w++) {
        vl_key_t k = a->want[w];
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
        int max = HTTP_MAX, id;
        while ((id = aos_hal_http_get(url, max)) == -3 && max / 2 >= HTTP_MAX_LEAST) max /= 2;
        if (id <= 0) {
            if (id == -3 && !a->tile_nomem) {
                uint32_t psram = 0;
                aos_hal_heap_info(NULL, &psram);
                aos_hal_log("vuelos", "tile %d/%u/%u: no memory to ask for it, %u KB of PSRAM free", k.z,
                            (unsigned)k.x, (unsigned)k.y, (unsigned)(psram / 1024));
            }
            a->tile_nomem = id == -3;
            break;
        }
        a->tile_nomem = false;
        a->q[slot].id = id;
        a->q[slot].k = k;
        a->q[slot].max = max;
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

void vl_to_screen(const app_t *a, uint32_t x, uint32_t y, float *sx, float *sy)
{
    float s = vl_px_per_unit(a->view.z);
    *sx = a->sw * 0.5f + (float)(int32_t)(x - a->view.cx) * s;
    *sy = a->sh * 0.5f + (float)(int32_t)(y - a->view.cy) * s;
}

void vl_view_pan(app_t *a, float dx, float dy)
{
    float s = vl_px_per_unit(a->view.z);
    float ux = clampf(-dx / s, 2.0e9f), uy = clampf(-dy / s, 2.0e9f);
    a->view.cx += (uint32_t)(int32_t)ux;
    a->view.cy = clamp_y((int64_t)a->view.cy + (int32_t)uy);
    view_moved(a);
}

static void view_zoom_at(app_t *a, float z, float ax, float ay)
{
    if (z < VL_ZMIN) z = VL_ZMIN;
    if (z > VL_ZMAX) z = VL_ZMAX;
    float s0 = vl_px_per_unit(a->view.z), s1 = vl_px_per_unit(z);
    float ox = ax - a->sw * 0.5f, oy = ay - a->sh * 0.5f;
    float ux = clampf(ox / s0 - ox / s1, 2.0e9f), uy = clampf(oy / s0 - oy / s1, 2.0e9f);
    a->view.cx += (uint32_t)(int32_t)ux;
    a->view.cy = clamp_y((int64_t)a->view.cy + (int32_t)uy);
    a->view.z = z;
    view_moved(a);
}

void vl_zoom_to(app_t *a, float z, float ax, float ay)
{
    if (z < VL_ZMIN) z = VL_ZMIN;
    if (z > VL_ZMAX) z = VL_ZMAX;
    a->zt = z;
    a->ax = ax;
    a->ay = ay;
    a->animating = true;
    a->zooming = true;
    a->vx = a->vy = 0;
}

void vl_go_to(app_t *a, uint32_t cx, uint32_t cy, float z)
{
    a->view.cx = cx;
    a->view.cy = cy;
    a->view.z = z < VL_ZMIN ? VL_ZMIN : z > VL_ZMAX ? VL_ZMAX : z;
    a->animating = false;
    a->zooming = a->pinching;
    a->vx = a->vy = 0;
    view_moved(a);
}

/* the zoom at which the radius asked for fits the shorter side */
static float zoom_for_radius(const app_t *a)
{
    float side = (float)(a->sw < a->sh ? a->sw : a->sh) * 0.5f - 30.0f;
    float m = (float)a->conf.radius_km * 1000.0f * 0.75f;
    float c = cosf(a->conf.lat * 3.14159265f / 180.0f);
    float z = log2f(40075016.7f * c * side / (256.0f * m));
    return z < 6 ? 6 : z > 13 ? 13 : z;
}

void vl_place_apply(app_t *a)
{
    vl_lonlat_to_world(a->conf.lon, a->conf.lat, &a->place_x, &a->place_y);
    a->overlay_dirty = true;
}

static void go_home(app_t *a)
{
    a->follow = false;
    vl_go_to(a, a->place_x, a->place_y, zoom_for_radius(a));
}

static void view_save(app_t *a)
{
    aos_hal_pref_set_i32("vl_cx", (int32_t)a->view.cx);
    aos_hal_pref_set_i32("vl_cy", (int32_t)a->view.cy);
    aos_hal_pref_set_i32("vl_z", (int32_t)(a->view.z * 100));
    aos_hal_pref_set_i32("vl_px", (int32_t)a->place_x);
    aos_hal_pref_set_i32("vl_py", (int32_t)a->place_y);
}

/* Where it was left, if it was left around the same place; else the place. */
static void view_load(app_t *a)
{
    int32_t cx, cy, z, px, py;
    if (aos_hal_pref_get_i32("vl_cx", &cx) && aos_hal_pref_get_i32("vl_cy", &cy) &&
        aos_hal_pref_get_i32("vl_z", &z) && aos_hal_pref_get_i32("vl_px", &px) &&
        aos_hal_pref_get_i32("vl_py", &py) && (uint32_t)px == a->place_x && (uint32_t)py == a->place_y) {
        vl_go_to(a, (uint32_t)cx, (uint32_t)cy, (float)z / 100.0f);
        return;
    }
    go_home(a);
}

static void speed_sample(app_t *a, uint32_t now)
{
    uint32_t dt = now - a->sp_ms;
    if (dt < 30) return;
    float s = vl_px_per_unit(a->view.z);
    float dx = (float)(int32_t)(a->view.cx - a->sp_cx) * s, dy = (float)(int32_t)(a->view.cy - a->sp_cy) * s;
    float v = sqrtf(dx * dx + dy * dy) * 1000.0f / (float)dt;
    if (dt > 400) v = 0;
    a->speed = a->speed * 0.5f + v * 0.5f;
    a->sp_ms = now;
    a->sp_cx = a->view.cx;
    a->sp_cy = a->view.cy;
}

/* ---------------------------------------------------------------------------
 * The gamepad: the cross moves the map, L and R zoom, A picks the aircraft
 * nearest the centre (and the next one at every press), B lets it go,
 * START opens the list. Read every frame, from the timer.
 * ------------------------------------------------------------------------- */

static int nearest_to(app_t *a, float x, float y, float rmax, uint32_t after_hex)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    int best = -1;
    float bd = rmax * rmax;
    float after_d = -1;
    if (after_hex) {
        int j = vl_ac_find(a, after_hex);
        if (j >= 0) {
            uint32_t wx, wy;
            float sx, sy;
            vl_ac_pos(a, &a->ac[j], now, &wx, &wy);
            vl_to_screen(a, wx, wy, &sx, &sy);
            after_d = (sx - x) * (sx - x) + (sy - y) * (sy - y);
        }
    }
    for (int i = 0; i < a->nac; i++) {
        vl_ac_t *ac = &a->ac[i];
        if (!ac->used || vl_ac_stale(ac, now)) continue;
        uint32_t wx, wy;
        float sx, sy;
        vl_ac_pos(a, ac, now, &wx, &wy);
        vl_to_screen(a, wx, wy, &sx, &sy);
        float d = (sx - x) * (sx - x) + (sy - y) * (sy - y);
        if (after_d >= 0 && (d < after_d || (d == after_d && ac->hex <= after_hex))) continue;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void pad_step(app_t *a, float dt)
{
    aos_pad_update(&a->pad, lv_tick_get());
    if (!a->pad.connected) return;
    aos_pad_t *p = &a->pad;
    if (a->screen != SCR_MAP) {
        if (p->pressed & (AOS_PAD_B | AOS_PAD_START)) vl_show_map(a);
        return;
    }
    float sp = 700.0f * dt;
    float dx = 0, dy = 0;
    if (p->x || p->y) {
        dx = -(float)p->x / 32767.0f * sp;
        dy = -(float)p->y / 32767.0f * sp;
    } else {
        if (p->held & AOS_PAD_LEFT) dx += sp;
        if (p->held & AOS_PAD_RIGHT) dx -= sp;
        if (p->held & AOS_PAD_UP) dy += sp;
        if (p->held & AOS_PAD_DOWN) dy -= sp;
    }
    if (dx || dy) {
        a->follow = false;
        a->touching = true;
        vl_view_pan(a, dx, dy);
    } else if (p->released & AOS_PAD_DIRS) {
        a->touching = false;
    }
    float cx = a->sw * 0.5f, cy = a->sh * 0.5f;
    if (p->pressed & AOS_PAD_R) vl_zoom_to(a, floorf((a->animating ? a->zt : a->view.z) + 1.01f), cx, cy);
    if (p->pressed & AOS_PAD_L) vl_zoom_to(a, ceilf((a->animating ? a->zt : a->view.z) - 1.01f), cx, cy);
    if (p->pressed & AOS_PAD_A) {
        int i = nearest_to(a, cx, cy, 1e5f, a->sel >= 0 ? a->sel_hex : 0);
        if (i < 0) i = nearest_to(a, cx, cy, 1e5f, 0);
        vl_select(a, i);
    }
    if (p->pressed & AOS_PAD_B) vl_select(a, -1);
    if (p->pressed & AOS_PAD_START) vl_show_list(a);
}

/* ---------------------------------------------------------------------------
 * The frame timer
 * ------------------------------------------------------------------------- */

/* Whether the aircraft moved enough on the screen for another frame: half a
 * pixel at the speed of the fastest one, 12 frames a second at most. */
static bool aircraft_due(app_t *a, uint32_t now)
{
    uint32_t since = now - a->ac_push_ms;
    if (since < 80) return false;
    float v = a->ac_speed_px;
    if (v <= 0.01f) return since > 1000;    /* the card's "seen ... ago" */
    float wait = 500.0f / v;                /* ms for half a pixel */
    return since >= (wait < 80 ? 80 : wait > 1000 ? 1000 : wait);
}

static void tick(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    float dt = (float)(now - a->last_tick) / 1000.0f;
    if (dt <= 0 || dt > 0.1f) dt = TICK_MS / 1000.0f;
    a->last_tick = now;

    vl_conf_poll(a, now);
    if (!a->hidden) {
        net_pump(a);
        vl_feed_pump(a, now);
        pad_step(a, dt);
    }
    vl_ui_tick(a, now);
    if (!a->viewing || a->hidden) return;

    uint32_t ov = aos_ui_overlay();
    if (ov != a->over_bits) {
        if (!ov || (!a->over_bits && !a->frame_synced)) a->overlay_dirty = true;
        a->over_bits = ov;
    }
    bool wait = (ov & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;

    if (wait) {
    } else if (a->animating) {
        float d = a->zt - a->view.z;
        if (fabsf(d) < 0.01f) {
            view_zoom_at(a, a->zt, a->ax, a->ay);
            a->animating = false;
            a->zooming = a->pinching;
        } else {
            view_zoom_at(a, a->view.z + d * (1.0f - expf(-dt * 20.0f)), a->ax, a->ay);
        }
    } else if (!a->touching && (fabsf(a->vx) > 20 || fabsf(a->vy) > 20)) {
        vl_view_pan(a, a->vx * dt, a->vy * dt);
        float k = expf(-dt * 5.2f);
        a->vx *= k;
        a->vy *= k;
    }
    /* following the picked aircraft: the map goes with it */
    if (!wait && a->follow && a->sel >= 0 && !a->touching) {
        uint32_t x, y;
        vl_ac_pos(a, &a->ac[a->sel], now, &x, &y);
        if (x != a->view.cx || y != a->view.cy) {
            int32_t ddx = (int32_t)(x - a->view.cx), ddy = (int32_t)(y - a->view.cy);
            float k = 1.0f - expf(-dt * 6.0f);
            a->view.cx += (uint32_t)(int32_t)((float)ddx * k);
            a->view.cy += (uint32_t)(int32_t)((float)ddy * k);
            view_moved(a);
        }
    }
    speed_sample(a, now);

    if (now - a->open_ms < 350) return;
    if (a->view_seq != a->shown_seq || a->front_gen != a->shown_gen || a->overlay_dirty ||
        aircraft_due(a, now)) {
        a->shown_seq = a->view_seq;
        a->shown_gen = a->front_gen;
        a->overlay_dirty = false;
        a->ac_push_ms = now;
        vl_frame_push(a);
    } else if (vl_frame_sync(a, now)) {
        a->overlay_dirty = true;
    }
}

/* ---------------------------------------------------------------------------
 * Touch
 * ------------------------------------------------------------------------- */

static bool buttons(app_t *a, float x, float y)
{
    const lay_t *L = &a->L;
    if (a->sel >= 0 && vl_hit_card(a, x, y)) {
        if (vl_hit_disc(&L->card_close, x, y)) {
            vl_select(a, -1);
        } else if (vl_hit_disc(&L->card_follow, x, y)) {
            a->follow = !a->follow;
            a->overlay_dirty = true;
        }
        return true;
    }
    if (vl_hit_disc(&L->zin, x, y)) {
        vl_zoom_to(a, floorf((a->animating ? a->zt : a->view.z) + 1.0f + 0.01f), a->sw * 0.5f, a->sh * 0.5f);
        return true;
    }
    if (vl_hit_disc(&L->zout, x, y)) {
        vl_zoom_to(a, ceilf((a->animating ? a->zt : a->view.z) - 1.0f - 0.01f), a->sw * 0.5f, a->sh * 0.5f);
        return true;
    }
    if (vl_hit_disc(&L->home, x, y)) {
        go_home(a);
        return true;
    }
    if (vl_hit_disc(&L->menu, x, y)) {
        vl_show_settings(a);
        return true;
    }
    if (vl_hit_disc(&L->list, x, y) ||
        (x >= L->px0 - 8 && x < L->px1 + 8 && y >= L->py0 - 8 && y < L->py1 + 8)) {
        vl_show_list(a);
        return true;
    }
    return false;
}

static void tap(app_t *a, float x, float y)
{
    if (buttons(a, x, y)) return;
    int i = nearest_to(a, x, y, PICK_R, 0);
    vl_select(a, i);
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
        a->follow = false;
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        a->touching = true;
        a->animating = false;
        a->pinching = true;
        a->zooming = true;
        a->vx = a->vy = 0;
        break;
    case AOS_GESTURE_DRAG:
        vl_view_pan(a, ev->dx, ev->dy);
        break;
    case AOS_GESTURE_DRAG_END:
        a->touching = false;
        a->vx = ev->vx;
        a->vy = ev->vy;
        break;
    case AOS_GESTURE_PINCH:
        view_zoom_at(a, a->view.z + log2f(ev->scale), a->follow ? a->sw * 0.5f : ev->x,
                     a->follow ? a->sh * 0.5f : ev->y);
        if (!a->follow) vl_view_pan(a, ev->dx, ev->dy);
        break;
    case AOS_GESTURE_PINCH_END:
        a->touching = false;
        a->pinching = false;
        a->zooming = a->animating;
        break;
    case AOS_GESTURE_TAP:
        tap(a, ev->x, ev->y);
        break;
    case AOS_GESTURE_DOUBLE_TAP:
        if (!buttons(a, ev->x, ev->y)) vl_zoom_to(a, floorf(a->view.z + 1.0f + 0.01f), ev->x, ev->y);
        break;
    case AOS_GESTURE_LONG_PRESS: {
        if (a->sel >= 0 && vl_hit_card(a, ev->x, ev->y)) break;
        float s = vl_px_per_unit(a->view.z);
        uint32_t x = a->view.cx + (uint32_t)(int32_t)((ev->x - a->sw * 0.5f) / s);
        uint32_t y = clamp_y((int64_t)a->view.cy + (int32_t)((ev->y - a->sh * 0.5f) / s));
        float lon, lat;
        vl_world_to_lonlat(x, y, &lon, &lat);
        a->conf.lat = lat;
        a->conf.lon = lon;
        a->conf.have_place = true;
        snprintf(a->conf.name, sizeof a->conf.name, "%.4f, %.4f", (double)lat, (double)lon);
        vl_conf_save(a);
        vl_place_apply(a);
        vl_feed_restart(a);
        char txt[80];
        snprintf(txt, sizeof txt, "%s: %s", _("El lugar pasa a"), a->conf.name);
        aos_ui_toast(txt, 2500);
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Life cycle
 * ------------------------------------------------------------------------- */

static void vl_destroy(aos_app_t *self, void *inst);

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
    vl_layout(a);
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
    static const lv_font_t *const F[VL_FONTS] = { &aos_inter_16, &aos_inter_20, &aos_inter_24 };
    bool ok = true;
    for (int i = 0; i < VL_FONTS; i++) {
        ok &= vl_font_bake(&a->fonts[Q_FULL][i], F[i]);
        ok &= vl_font_half(&a->fonts[Q_HALF][i], &a->fonts[Q_FULL][i]);
    }
    ok &= vl_font_bake(&a->fbig, &aos_inter_32);
    return ok;
}

static void *vl_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)vl_calloc(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    for (int q = 0; q < Q_N; q++) a->pair[q].front = -1;
    a->open_ms = a->last_tick = (uint32_t)aos_hal_uptime_ms();
    a->sel = -1;
    lv_obj_set_style_bg_color(root, lv_color_hex(VL_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    bool ok = vl_feed_init(a);
    for (int i = 0; i < 2; i++) {
        a->pair[Q_FULL].b[i].px = (uint16_t *)vl_malloc(CAP_FULL * 2);
        a->pair[Q_HALF].b[i].px = (uint16_t *)vl_malloc(CAP_HALF * 2);
        ok &= a->pair[Q_FULL].b[i].px && a->pair[Q_HALF].b[i].px;
    }
    a->frame = (uint16_t *)aos_hal_io_alloc(CAP_FRAME * 2);
    ok &= a->frame != NULL;
    if (!ok) {
        vl_destroy(self, a);
        aos_ui_toast(_("Sin memoria"), 2000);
        return NULL;
    }
    for (size_t i = 0; i < CAP_FRAME; i++) a->frame[i] = vl_565(VL_BG);
    uint32_t li = 0, left = 0;
    aos_hal_heap_info(&li, &left);
    a->ram_budget = left / 3;
    if (a->ram_budget > RAM_BUDGET_MAX) a->ram_budget = RAM_BUDGET_MAX;
    if (a->ram_budget < RAM_BUDGET_MIN) a->ram_budget = RAM_BUDGET_MIN;
    fonts_bake(a);

    int32_t blit = 1;
    aos_hal_pref_get_i32("map_blit", &blit);
    a->use_blit = blit != 0;
    a->online = true;
    aos_hal_pref_get_str("vl_tpl", a->tpl, sizeof a->tpl);

    vl_conf_defaults(&a->conf);
    vl_conf_load(a);
    vl_place_apply(a);
    vl_air_load(a);

    a->map = lv_obj_create(root);
    lv_obj_remove_style_all(a->map);
    lv_obj_set_pos(a->map, 0, 0);
    lv_obj_remove_flag(a->map, LV_OBJ_FLAG_SCROLLABLE);
    a->canvas = lv_canvas_create(a->map);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    a->touch = lv_obj_create(a->map);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_pos(a->touch, 0, 0);
    aos_gesture_attach(a->touch, AOS_GESTURE_FLAG_FAST_TAP, gesture_cb, a);
    apply_shape(a);

    view_load(a);
    a->sp_ms = a->open_ms;
    a->sp_cx = a->view.cx;
    a->sp_cy = a->view.cy;
    a->viewing = true;
    a->screen = SCR_MAP;
    a->self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    if (!aos_hal_worker_start_on("vuelos", worker, a, WORKER_STACK, 0, 3)) aos_hal_log("vuelos", "no worker");
    a->timer = lv_timer_create(tick, TICK_MS, a);
    aos_hal_log("vuelos", "open: %s, %d km around %.4f, %.4f; %d airports", vl_src_name(a->conf.src),
                a->conf.radius_km, (double)a->conf.lat, (double)a->conf.lon, a->nap);

    /* development switches for the simulator (the board's getenv() is NULL):
     * VUELOS_SCREEN=list|settings opens that panel, VUELOS_PICK=1 picks the
     * aircraft nearest the place once the first reading is in */
    const char *scr = getenv("VUELOS_SCREEN");
    if (scr && !strcmp(scr, "list")) vl_show_list(a);
    else if (scr && !strcmp(scr, "settings")) vl_show_settings(a);
    return a;
}

static void vl_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    aos_hal_worker_stop();
    for (int i = 0; i < NQ; i++)
        if (a->q[i].state != Q_FREE && a->q[i].id > 0) aos_hal_http_release(a->q[i].id);
    if (a->tpl_id > 0) aos_hal_http_release(a->tpl_id);
    if (a->low_latency) aos_hal_net_low_latency(false);
    if (a->front_gen) view_save(a);
    vl_feed_free(a);
    vl_air_free(a);
    for (int q = 0; q < Q_N; q++)
        for (int i = 0; i < VL_FONTS; i++) vl_font_free(&a->fonts[q][i]);
    vl_font_free(&a->fbig);
    if (self && self->root) lv_obj_clean(self->root);
    for (int i = 0; i < 2; i++) {
        vl_free(a->pair[Q_FULL].b[i].px);
        vl_free(a->pair[Q_HALF].b[i].px);
    }
    aos_hal_io_free(a->frame);
    vl_free(a);
}

static bool vl_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    if (a->screen != SCR_MAP) {
        vl_show_map(a);
        return true;
    }
    if (a->sel >= 0) {
        vl_select(a, -1);
        return true;
    }
    return false;
}

static bool vl_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    a->root = root;
    apply_shape(a);
    if (a->screen != SCR_MAP) vl_ui_relayout(a);
    return true;
}

static void vl_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) a->hidden = true;    /* nothing blitted, nothing asked of the sources */
}

static void vl_show(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    a->hidden = false;
    a->overlay_dirty = true;
}

/* A radar: a dark disc, two rings, and an aircraft crossing it. */
static const uint8_t VL_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 0, 92, 92, AIC_CIRCLE, AIC_C_LIT(0x0E2233), 255),
    AIC_RING(92, 3, AIC_C_LIT(0x3E7CA8), 255),
    AIC_RING(58, 3, AIC_C_LIT(0x2F5E80), 255),
    AIC_RECT(AIC_CENTER, 0, 0, 8, 8, AIC_CIRCLE, AIC_C_LIT(0x7FC8FF), 255),
    AIC_RECT(AIC_CENTER, 0, 0, 11, 60, 6, AIC_C_LIT(0xFFC94A), 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, 4, -4, 54, 12, 6, AIC_C_LIT(0xFFC94A), 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, -17, 17, 24, 8, 4, AIC_C_LIT(0xFFC94A), 255),
    AIC_ROT(450),
    AIC_END
};

static bool vl_init(aos_app_t *app)
{
    app->desc.id       = "aos.vuelos";
    app->desc.name     = "Vuelos";
    app->desc.icon     = LV_SYMBOL_GPS;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, VL_ICON, sizeof VL_ICON);
    app->desc.color_a  = 0x16405E;
    app->desc.color_b  = 0x081826;
    app->desc.order    = 165;
    app->desc.flags    = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP_AWAKE;

    app->create  = vl_create;
    app->destroy = vl_destroy;
    app->back    = vl_back;
    app->resize  = vl_resize;
    app->hide    = vl_hide;
    app->show    = vl_show;
    return true;
}

AOS_APP_ENTRY(vl_init);
