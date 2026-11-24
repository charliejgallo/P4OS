/*
 * PLANEO - the game (see vu_game.h)
 */
#include "vu_game.h"
#include "vu_text.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define COUNTDOWN 3.0f

/* each world's three hours: rings, race, packages */
static const uint8_t HOURS[VW_BIOMES][VG_KINDS] = {
    { VW_NOON, VW_MORNING, VW_EVENING },     /* islands */
    { VW_EVENING, VW_NOON, VW_MORNING },     /* canyons */
    { VW_MORNING, VW_NOON, VW_EVENING },     /* snow */
    { VW_EVENING, VW_NIGHT, VW_NOON },       /* volcano */
    { VW_NOON, VW_EVENING, VW_NIGHT },       /* city */
};

static const uint8_t NEEDS[VW_BIOMES] = { 0, 3, 7, 12, 18 };

void vg_mission_world(int mission, int *biome, int *hour)
{
    int b = (mission / VG_KINDS) % VW_BIOMES, k = mission % VG_KINDS;
    *biome = b;
    *hour = HOURS[b][k];
}

int vg_mission_kind(int mission)
{
    return mission % VG_KINDS;
}

int vg_world_needs(int biome)
{
    return NEEDS[biome % VW_BIOMES];
}

void vg_fmt_time(char *b, int n, float t)
{
    if (t < 0) t = 0;
    int ds = (int)(t * 10.0f);
    snprintf(b, (size_t)n, "%d:%02d.%d", ds / 600, (ds / 10) % 60, ds % 10);
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static uint32_t s_rng;

static float rnd(float a, float b)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return a + (b - a) * (float)(s_rng & 0xFFFFFF) / 16777216.0f;
}

static inline float wrapd(float d)
{
    if (d > VW_SIZE * 0.5f) d -= VW_SIZE;
    if (d < -VW_SIZE * 0.5f) d += VW_SIZE;
    return d;
}

/* the highest ground along a segment, and round its end */
static float ground_max(const vw_world_t *w, float x0, float y0, float x1, float y1, float spread)
{
    float m = 0;
    for (int i = 0; i <= 12; i++) {
        float t = (float)i / 12.0f;
        float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
        float h = vw_height(w, vw_wrap(x), vw_wrap(y));
        if (h > m) m = h;
    }
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 0.785f;
        float h = vw_height(w, vw_wrap(x1 + cosf(a) * spread), vw_wrap(y1 + sinf(a) * spread));
        if (h > m) m = h;
    }
    return m;
}

static void set_pt(vg_pt_t *p, float x, float y, float z, float dx, float dy, float dz)
{
    p->p[0] = vw_wrap(x);
    p->p[1] = vw_wrap(y);
    p->p[2] = z;
    float l = sqrtf(dx * dx + dy * dy + dz * dz);
    if (l < 1e-3f) l = 1;
    p->n[0] = dx / l;
    p->n[1] = dy / l;
    p->n[2] = dz / l;
}

/* A course of rings: each step tries a few headings and keeps the one over
 * the lowest ground (valleys, rivers, streets), a little at random */
static float make_course(vg_game_t *g, const vw_world_t *w, int n, float x, float y, float z, float yaw)
{
    float len = 0;
    float px = x, py = y, pz = z;
    float hd = yaw;
    for (int i = 0; i < n; i++) {
        float best = 1e9f, bh = hd, bd = 200;
        for (int c = 0; c < 5; c++) {
            float h = hd + (float)(c - 2) * 0.42f + rnd(-0.15f, 0.15f);
            float d = rnd(180.0f, 250.0f);
            float gm = ground_max(w, px, py, px + cosf(h) * d, py + sinf(h) * d, 25.0f);
            float score = gm + rnd(0, 30.0f) + (c == 2 ? -10.0f : 0);
            if (score < best) {
                best = score;
                bh = h;
                bd = d;
            }
        }
        if (i == 0) {
            bh = yaw;
            bd = 220.0f;
        }
        float nx = px + cosf(bh) * bd, ny = py + sinf(bh) * bd;
        float gm = ground_max(w, px, py, nx, ny, 25.0f);
        float nz = gm + rnd(24.0f, 42.0f);
        /* the first ring at about the height the plane starts at */
        if (i == 0 && nz < z - 25.0f) nz = z - 25.0f;
        if (nz > 500.0f) nz = 500.0f;
        if (w->water >= 0 && nz < w->water + 18.0f) nz = w->water + 18.0f;
        set_pt(&g->pt[i], nx, ny, nz, nx - px, ny - py, (nz - pz) * 0.5f);
        float seg = sqrtf((nx - px) * (nx - px) + (ny - py) * (ny - py) + (nz - pz) * (nz - pz));
        g->seglen[i] = seg;
        len += seg;
        px = nx;
        py = ny;
        pz = nz;
        hd = bh;
    }
    g->npt = n;
    return len;
}

/* Packages: three to take and three places to take them, on clear flat
 * ground, each a few hundred metres from the last */
static float make_deliveries(vg_game_t *g, const vw_world_t *w, float x, float y)
{
    float len = 0, px = x, py = y;
    int n = 0;
    for (int k = 0; k < 6; k++) {
        float bx = px, by = py, best = 1e9f;
        for (int c = 0; c < 24; c++) {
            float a = rnd(0, 6.2831f), d = rnd(280.0f, 460.0f);
            float cx = px + cosf(a) * d, cy = py + sinf(a) * d;
            float h = vw_height(w, vw_wrap(cx), vw_wrap(cy));
            float hx = vw_height(w, vw_wrap(cx + 12), vw_wrap(cy)), hy = vw_height(w, vw_wrap(cx), vw_wrap(cy + 12));
            float slope = fabsf(hx - h) + fabsf(hy - h);
            bool wet = vw_is_water(w, vw_wrap(cx), vw_wrap(cy));
            float score = slope * 4.0f + (wet ? 500.0f : 0) + h * 0.15f + rnd(0, 20.0f);
            if (score < best) {
                best = score;
                bx = cx;
                by = cy;
            }
        }
        float h = vw_height(w, vw_wrap(bx), vw_wrap(by));
        set_pt(&g->pt[n], bx, by, h, bx - px, by - py, 0);
        float seg = sqrtf((bx - px) * (bx - px) + (by - py) * (by - py));
        g->seglen[n] = seg;
        len += seg;
        n++;
        px = bx;
        py = by;
    }
    g->npt = n;
    return len;
}

/* --------------------------------------------------------------------------
 * The rival: a Catmull-Rom curve through the start and the rings
 * -------------------------------------------------------------------------- */

static void course_pt(const vg_game_t *g, int i, float *o)
{
    /* -1 is where the race starts, beside the player */
    if (i < 0) {
        memcpy(o, g->rival_start, sizeof(float) * 3);
        return;
    }
    if (i >= g->npt) {
        /* past the end: straight on */
        const vg_pt_t *l = &g->pt[g->npt - 1];
        float k = (float)(i - g->npt + 1) * 200.0f;
        for (int a = 0; a < 3; a++) o[a] = l->p[a] + l->n[a] * k;
        return;
    }
    memcpy(o, g->pt[i].p, sizeof(float) * 3);
}

/* the points around segment i made continuous across the world's wrap */
static void seg_points(const vg_game_t *g, int i, float P[4][3])
{
    float ref[3];
    course_pt(g, i - 1, ref);
    for (int k = 0; k < 4; k++) {
        float q[3];
        course_pt(g, i - 2 + k, q);
        P[k][0] = ref[0] + wrapd(q[0] - ref[0]);
        P[k][1] = ref[1] + wrapd(q[1] - ref[1]);
        P[k][2] = q[2];
    }
}

static void catmull(float P[4][3], float t, float *o, float *d)
{
    float t2 = t * t, t3 = t2 * t;
    for (int a = 0; a < 3; a++) {
        float p0 = P[0][a], p1 = P[1][a], p2 = P[2][a], p3 = P[3][a];
        o[a] = 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
        d[a] = 0.5f * ((-p0 + p2) + 2 * (2 * p0 - 5 * p1 + 4 * p2 - p3) * t + 3 * (-p0 + 3 * p1 - 3 * p2 + p3) * t2);
    }
}

static void rival_at(vg_game_t *g, float s)
{
    int i = 0;
    while (i < g->npt - 1 && s > g->seglen[i]) {
        s -= g->seglen[i];
        i++;
    }
    float t = g->seglen[i] > 0 ? s / g->seglen[i] : 0;
    if (t > 1.5f) t = 1.5f;
    float P[4][3], o[3], d[3], o2[3], d2[3];
    seg_points(g, i, P);
    catmull(P, t, o, d);
    catmull(P, t + 0.08f, o2, d2);
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1e-3f) l = 1;
    float f[3] = { d[0] / l, d[1] / l, d[2] / l };
    /* the bank from how fast the heading turns */
    float h0 = atan2f(d[1], d[0]), h1 = atan2f(d2[1], d2[0]);
    float dh = h1 - h0;
    while (dh > 3.14159f) dh -= 6.28318f;
    while (dh < -3.14159f) dh += 6.28318f;
    float bank = -dh * 4.0f;
    if (bank > 1.0f) bank = 1.0f;
    if (bank < -1.0f) bank = -1.0f;
    float fh = sqrtf(f[0] * f[0] + f[1] * f[1]);
    float r0[3] = { f[1] / fh, -f[0] / fh, 0 };
    float u0[3] = { r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0] };
    float cb = cosf(bank), sb = sinf(bank);
    for (int a = 0; a < 3; a++) {
        g->rival_f[a] = f[a];
        g->rival_r[a] = r0[a] * cb - u0[a] * sb;
        g->rival_u[a] = u0[a] * cb + r0[a] * sb;
    }
    g->rival_p[0] = vw_wrap(o[0]);
    g->rival_p[1] = vw_wrap(o[1]);
    g->rival_p[2] = o[2];
}

bool vg_rival(const vg_game_t *g)
{
    return g->kind == VG_RACE;
}

/* --------------------------------------------------------------------------
 * A flight
 * -------------------------------------------------------------------------- */

static void place_plane(vf_plane_t *pl, const float p[3], float yaw, float v)
{
    float c = cosf(yaw), s = sinf(yaw);
    memset(pl->f, 0, sizeof pl->f);
    memset(pl->r, 0, sizeof pl->r);
    memset(pl->u, 0, sizeof pl->u);
    pl->f[0] = c;
    pl->f[1] = s;
    pl->r[0] = s;
    pl->r[1] = -c;
    pl->u[2] = 1;
    memcpy(pl->p, p, sizeof pl->p);
    pl->v = v;
    pl->crashed = false;
    pl->crash_t = 0;
    pl->bank = pl->pitch = 0;
    pl->heading = yaw;
    pl->stall = 0;
    memcpy(pl->cam_d, pl->f, sizeof pl->cam_d);
    pl->cam_ok = false;
    pl->cam_roll = 0;
}

void vg_start(vg_game_t *g, const vw_world_t *w, int mission, vf_plane_t *pl)
{
    memset(g, 0, sizeof *g);
    g->mission = mission;
    g->banner = -1;
    g->biome = w->biome;
    g->hour = w->hour;
    vf_reset(pl, w);
    memcpy(g->safe_p, w->start, sizeof g->safe_p);
    g->safe_yaw = w->start_yaw;
    if (mission < 0) {
        g->kind = VG_FREE;
        g->state = VG_ST_FLY;
        return;
    }
    g->kind = vg_mission_kind(mission);
    s_rng = w->seed * 7919u + (uint32_t)mission * 104729u + 17u;
    float len;
    switch (g->kind) {
    case VG_RACE:
        len = make_course(g, w, 12, w->start[0], w->start[1], w->start[2], w->start_yaw);
        g->rival_v = 60.0f + 3.0f * (float)g->biome;
        g->rival_len = len;
        for (int i = 0; i < g->npt - 1; i++) {
            /* the curve is a little longer than the chords */
            g->seglen[i] *= 1.04f;
        }
        g->rival_done_t = len * 1.04f / g->rival_v;
        g->par[0] = 1e9f;
        g->par[1] = g->rival_done_t;
        g->par[2] = g->rival_done_t - 6.0f;
        /* the rival starts beside the player, to the right */
        g->rival_start[0] = vw_wrap(w->start[0] + sinf(w->start_yaw) * 16.0f);
        g->rival_start[1] = vw_wrap(w->start[1] - cosf(w->start_yaw) * 16.0f);
        g->rival_start[2] = w->start[2];
        rival_at(g, 0);
        break;
    case VG_DELIVER:
        len = make_deliveries(g, w, w->start[0], w->start[1]);
        g->par[0] = 1e9f;
        g->par[1] = len / 42.0f + 40.0f;
        g->par[2] = len / 52.0f + 22.0f;
        break;
    default:
        len = make_course(g, w, 10, w->start[0], w->start[1], w->start[2], w->start_yaw);
        g->par[0] = 1e9f;
        g->par[1] = len / 55.0f + 8.0f;
        g->par[2] = len / 66.0f + 3.0f;
        break;
    }
    g->state = VG_ST_COUNT;
    g->t = -COUNTDOWN;
    aos_hal_log("vuelo", "mission %d: kind %d, %d points, %.0f m, par %.0f / %.0f s", mission, g->kind, g->npt,
                (double)len, (double)g->par[1], (double)g->par[2]);
}

static void banner(vg_game_t *g, int word)
{
    g->banner = word;
    g->banner_t = 0;
}

static void finish(vg_game_t *g)
{
    g->state = VG_ST_DONE;
    g->final_t = g->t + g->penalty;
    g->events |= VG_EV_FINISH;
    if (g->kind == VG_RACE) {
        g->won = g->final_t < g->rival_done_t;
        g->stars = 1 + (g->won ? 1 : 0) + (g->final_t < g->par[2] ? 1 : 0);
        banner(g, g->won ? VT_W_WIN : VT_W_LOST);
    } else {
        g->won = true;
        g->stars = 1 + (g->final_t <= g->par[1] ? 1 : 0) + (g->final_t <= g->par[2] ? 1 : 0);
        banner(g, VT_W_FINISH);
    }
}

void vg_step(vg_game_t *g, const vw_world_t *w, vf_plane_t *pl, const float p0[3], float dt)
{
    g->banner_t += dt;
    if (g->state == VG_ST_COUNT) {
        int before = (int)ceilf(-g->t);
        g->t += dt;
        int after = (int)ceilf(-g->t);
        if (after != before && after > 0) g->events |= VG_EV_COUNT;
        if (g->t >= 0) {
            g->t = 0;
            g->state = VG_ST_FLY;
            g->events |= VG_EV_GO;
            banner(g, VT_W_GO);
        }
        return;
    }
    if (g->state == VG_ST_FLY && g->kind != VG_FREE) g->t += dt;
    if (g->kind == VG_RACE) {
        float s = g->rival_v * g->t;
        rival_at(g, s);
    }
    /* a crash: five seconds, and back to the last thing passed */
    if (pl->crashed) {
        if (pl->crash_t < 0.05f) {
            g->events |= VG_EV_CRASH;
            banner(g, VT_W_CRASH);
        }
        if (pl->crash_t > 1.6f) {
            float p[3];
            float yaw = g->safe_yaw;
            if (g->kind == VG_FREE) {
                p[0] = pl->p[0];
                p[1] = pl->p[1];
                yaw = pl->heading;
            } else {
                p[0] = g->safe_p[0];
                p[1] = g->safe_p[1];
            }
            float top = 0;
            for (int i = 0; i < 12; i++) {
                float a = (float)i * 0.5236f;
                for (int r = 0; r <= 160; r += 40) {
                    float h = vw_height(w, vw_wrap(p[0] + cosf(a) * (float)r), vw_wrap(p[1] + sinf(a) * (float)r));
                    if (h > top) top = h;
                }
            }
            p[2] = top + 45.0f;
            if (g->kind != VG_FREE && g->state == VG_ST_FLY) g->penalty += VG_CRASH_S;
            int view = pl->view;
            float thr = pl->in_thr;
            place_plane(pl, p, yaw, 50.0f);
            pl->view = view;
            pl->in_thr = pl->thr = thr;
        }
        return;
    }
    if (g->state != VG_ST_FLY || g->next >= g->npt) return;
    const vg_pt_t *c = &g->pt[g->next];
    if (g->kind == VG_DELIVER) {
        float dx = wrapd(pl->p[0] - c->p[0]), dy = wrapd(pl->p[1] - c->p[1]);
        if (dx * dx + dy * dy < VG_BEAM_R * VG_BEAM_R && pl->p[2] - c->p[2] < VG_BEAM_LOW) {
            g->carrying = !g->carrying;
            g->events |= g->carrying ? VG_EV_PICK : VG_EV_DROP;
            banner(g, g->carrying ? VT_W_PICK : VT_W_DROP);
            memcpy(g->safe_p, c->p, sizeof g->safe_p);
            g->safe_yaw = pl->heading;
            g->next++;
            if (g->next >= g->npt) finish(g);
        }
        return;
    }
    /* through the ring's plane, inside it */
    float a0[3] = { wrapd(p0[0] - c->p[0]), wrapd(p0[1] - c->p[1]), p0[2] - c->p[2] };
    float d[3] = { wrapd(pl->p[0] - p0[0]), wrapd(pl->p[1] - p0[1]), pl->p[2] - p0[2] };
    float s0 = a0[0] * c->n[0] + a0[1] * c->n[1] + a0[2] * c->n[2];
    float s1 = s0 + d[0] * c->n[0] + d[1] * c->n[1] + d[2] * c->n[2];
    if (s0 < 0 && s1 >= 0) {
        float k = s0 / (s0 - s1);
        float x[3] = { a0[0] + d[0] * k, a0[1] + d[1] * k, a0[2] + d[2] * k };
        float r2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
        if (r2 < VG_RING_R * VG_RING_R * 1.5f) {
            g->events |= VG_EV_RING;
            memcpy(g->safe_p, c->p, sizeof g->safe_p);
            g->safe_yaw = atan2f(c->n[1], c->n[0]);
            if (g->next + 1 < g->npt) {
                const vg_pt_t *nx = &g->pt[g->next + 1];
                g->safe_yaw = atan2f(wrapd(nx->p[1] - c->p[1]), wrapd(nx->p[0] - c->p[0]));
            }
            g->next++;
            if (g->next >= g->npt) finish(g);
        }
    }
}

/* --------------------------------------------------------------------------
 * Records
 * -------------------------------------------------------------------------- */

void vg_load(vg_save_t *s)
{
    memset(s, 0, sizeof *s);
    for (int m = 0; m < VG_MISSIONS; m++) {
        char k[16];
        snprintf(k, sizeof k, "pl_m%d", m);
        int32_t v;
        if (aos_hal_pref_get_i32(k, &v) && v > 0) {
            s->stars[m] = (uint8_t)((uint32_t)v >> 28);
            s->best_ms[m] = (uint32_t)v & 0x0FFFFFFF;
        }
    }
}

void vg_store(const vg_save_t *s, int m)
{
    if (m < 0 || m >= VG_MISSIONS) return;
    char k[16];
    snprintf(k, sizeof k, "pl_m%d", m);
    uint32_t v = ((uint32_t)s->stars[m] << 28) | (s->best_ms[m] & 0x0FFFFFFF);
    aos_hal_pref_set_i32(k, (int32_t)v);
}

int vg_total_stars(const vg_save_t *s)
{
    int n = 0;
    for (int m = 0; m < VG_MISSIONS; m++) n += s->stars[m];
    return n;
}
